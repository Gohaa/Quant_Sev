#include "Quote/QuoteEngine.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "Common/CtpUtil.hpp"
#include "Logger/Logger.hpp"

#ifdef QUANT_SEV_HAS_CTP
#include "ThostFtdcMdApi.h"
#endif

namespace quant_sev::core {

namespace fs = std::filesystem;

#ifdef QUANT_SEV_HAS_CTP

namespace {

class MdSession;

class MdSpiImpl : public CThostFtdcMdSpi {
public:
    explicit MdSpiImpl(MdSession* session) : session_(session) {}

    void OnFrontConnected() override;
    void OnFrontDisconnected(int nReason) override;
    void OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin, CThostFtdcRspInfoField* pRspInfo,
                        int nRequestID, bool bIsLast) override;
    void OnRspSubMarketData(CThostFtdcSpecificInstrumentField* pSpecificInstrument,
                            CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;
    void OnRspError(CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;
    void OnRtnDepthMarketData(CThostFtdcDepthMarketDataField* pDepthMarketData) override;

private:
    MdSession* session_;
};

struct MdSession {
    std::string front;
    AccountRecord account;
    CThostFtdcMdApi* api{nullptr};
    MdSpiImpl* spi{nullptr};
    std::atomic<bool> front_connected{false};
    std::atomic<bool> logged_in{false};
    std::atomic<int> request_id{1};
    std::mutex mutex;
    std::condition_variable cv;
    std::string last_error;
    std::unordered_map<std::string, nlohmann::json> ticks;
    std::vector<std::string> subscribed;
    QuoteEngine::TickCallback tick_callback;
    std::function<void(const std::string&, int)> disconnect_callback;
};

void resubscribe_instruments(MdSession* session) {
    if (session == nullptr || session->api == nullptr || !session->logged_in || session->subscribed.empty()) {
        return;
    }
    std::vector<char*> ptrs;
    ptrs.reserve(session->subscribed.size());
    for (const auto& id : session->subscribed) {
        ptrs.push_back(const_cast<char*>(id.c_str()));
    }
    session->api->SubscribeMarketData(ptrs.data(), static_cast<int>(ptrs.size()));
    Logger::instance().ctp("Md 重新订阅 " + std::to_string(ptrs.size()) + " 个合约");
}

void MdSpiImpl::OnFrontConnected() {
    if (session_ == nullptr || session_->api == nullptr) {
        return;
    }
    session_->front_connected = true;
    Logger::instance().ctp("Md 前置已连接: " + session_->front);

    CThostFtdcReqUserLoginField req{};
    copy_to_field(req.BrokerID, session_->account.broker_id);
    copy_to_field(req.UserID, session_->account.user_id);
    copy_to_field(req.Password, session_->account.password);
    session_->api->ReqUserLogin(&req, session_->request_id.fetch_add(1));
}

void MdSpiImpl::OnFrontDisconnected(int nReason) {
    if (session_ == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(session_->mutex);
    session_->front_connected = false;
    session_->logged_in = false;
    Logger::instance().warn("Md 前置断开: " + session_->front + " reason=0x" +
                            std::to_string(static_cast<unsigned>(nReason)));
    session_->cv.notify_all();
    if (session_->disconnect_callback) {
        session_->disconnect_callback(session_->front, nReason);
    }
}

void MdSpiImpl::OnRspUserLogin(CThostFtdcRspUserLoginField* /*pRspUserLogin*/,
                               CThostFtdcRspInfoField* pRspInfo, int /*nRequestID*/, bool bIsLast) {
    if (session_ == nullptr || !bIsLast) {
        return;
    }
    std::lock_guard<std::mutex> lock(session_->mutex);
    if (pRspInfo != nullptr && pRspInfo->ErrorID != 0) {
        session_->last_error = "Md 登录失败(" + std::to_string(pRspInfo->ErrorID) + "): " +
                               trim_cstr(pRspInfo->ErrorMsg);
        Logger::instance().error(session_->last_error);
    } else {
        session_->logged_in = true;
        session_->last_error.clear();
        Logger::instance().ctp("DialogRsp: Md OnRspUserLogin OK user=" + session_->account.user_id + " @ " +
                               session_->front);
        resubscribe_instruments(session_);
    }
    session_->cv.notify_all();
}

void MdSpiImpl::OnRspSubMarketData(CThostFtdcSpecificInstrumentField* pSpecificInstrument,
                                   CThostFtdcRspInfoField* pRspInfo, int /*nRequestID*/, bool bIsLast) {
    if (session_ == nullptr || !bIsLast) {
        return;
    }
    const std::string instrument =
        pSpecificInstrument != nullptr ? trim_cstr(pSpecificInstrument->InstrumentID) : std::string{};
    if (pRspInfo != nullptr && pRspInfo->ErrorID != 0) {
        Logger::instance().warn("Md 订阅失败 " + instrument + ": " + trim_cstr(pRspInfo->ErrorMsg));
    } else if (!instrument.empty()) {
        Logger::instance().ctp("DialogRsp: Md OnRspSubMarketData OK " + instrument);
    }
}

void MdSpiImpl::OnRspError(CThostFtdcRspInfoField* pRspInfo, int /*nRequestID*/, bool bIsLast) {
    if (session_ == nullptr || !bIsLast || pRspInfo == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(session_->mutex);
    session_->last_error = "Md 错误(" + std::to_string(pRspInfo->ErrorID) + "): " +
                           trim_cstr(pRspInfo->ErrorMsg);
    Logger::instance().error(session_->last_error);
    session_->cv.notify_all();
}

void MdSpiImpl::OnRtnDepthMarketData(CThostFtdcDepthMarketDataField* tick) {
    if (session_ == nullptr || tick == nullptr) {
        return;
    }
    nlohmann::json payload = {
        {"instrument_id", trim_cstr(tick->InstrumentID)},
        {"last_price", tick->LastPrice},
        {"volume", tick->Volume},
        {"open_interest", tick->OpenInterest},
        {"bid_price1", tick->BidPrice1},
        {"bid_volume1", tick->BidVolume1},
        {"ask_price1", tick->AskPrice1},
        {"ask_volume1", tick->AskVolume1},
        {"bid_price2", tick->BidPrice2},
        {"bid_volume2", tick->BidVolume2},
        {"ask_price2", tick->AskPrice2},
        {"ask_volume2", tick->AskVolume2},
        {"bid_price3", tick->BidPrice3},
        {"bid_volume3", tick->BidVolume3},
        {"ask_price3", tick->AskPrice3},
        {"ask_volume3", tick->AskVolume3},
        {"bid_price4", tick->BidPrice4},
        {"bid_volume4", tick->BidVolume4},
        {"ask_price4", tick->AskPrice4},
        {"ask_volume4", tick->AskVolume4},
        {"bid_price5", tick->BidPrice5},
        {"bid_volume5", tick->BidVolume5},
        {"ask_price5", tick->AskPrice5},
        {"ask_volume5", tick->AskVolume5},
        {"update_time", trim_cstr(tick->UpdateTime)},
        {"update_millisec", tick->UpdateMillisec},
        {"trading_day", trim_cstr(tick->TradingDay)},
        {"open_price", tick->OpenPrice},
        {"pre_close", tick->PreClosePrice},
        {"pre_settlement", tick->PreSettlementPrice},
        {"upper_limit", tick->UpperLimitPrice},
        {"lower_limit", tick->LowerLimitPrice}};

    const std::string key = payload["instrument_id"].get<std::string>();
    QuoteEngine::TickCallback callback;
    {
        std::lock_guard<std::mutex> lock(session_->mutex);
        session_->ticks[key] = payload;
        callback = session_->tick_callback;
    }
    if (callback) {
        callback(payload);
    }
}

void destroy_session(MdSession& session) {
    if (session.api != nullptr) {
        session.api->RegisterSpi(nullptr);
        session.api->Release();
        session.api = nullptr;
    }
    delete session.spi;
    session.spi = nullptr;
    session.front_connected = false;
    session.logged_in = false;
}

}  // namespace

#endif

struct QuoteEngine::Impl {
    mutable std::mutex mutex;
    QuoteEngine::TickCallback tick_callback;
    std::function<void(const std::string&, int)> disconnect_callback;
    fs::path project_root{"."};

#ifdef QUANT_SEV_HAS_CTP
    std::unordered_map<std::string, std::unique_ptr<MdSession>> sessions;
#else
    bool stub_warned{false};
#endif
};

QuoteEngine::QuoteEngine() : impl_(std::make_unique<Impl>()) {}
QuoteEngine::~QuoteEngine() = default;

void QuoteEngine::set_project_root(const fs::path& root) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->project_root = root;
}

void QuoteEngine::set_tick_callback(TickCallback callback) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->tick_callback = std::move(callback);
#ifdef QUANT_SEV_HAS_CTP
    for (auto& [_, session] : impl_->sessions) {
        session->tick_callback = impl_->tick_callback;
    }
#endif
}

void QuoteEngine::set_disconnect_callback(std::function<void(const std::string&, int)> callback) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->disconnect_callback = std::move(callback);
#ifdef QUANT_SEV_HAS_CTP
    for (auto& [_, session] : impl_->sessions) {
        session->disconnect_callback = impl_->disconnect_callback;
    }
#endif
}

ConnectResult QuoteEngine::connect(const AccountRecord& account) {
    if (account.md_front.empty()) {
        return {false, "md_front 为空"};
    }
    if (account.user_id.empty() || account.broker_id.empty()) {
        return {false, "broker_id / user_id 不完整"};
    }

#ifndef QUANT_SEV_HAS_CTP
    if (!impl_->stub_warned) {
        Logger::instance().warn("CTP 未链接，行情连接不可用（CMake 开启 QUANT_SEV_ENABLE_CTP）");
        impl_->stub_warned = true;
    }
    return {false, "CTP 未启用：请配置 CTP SDK 库并开启 QUANT_SEV_ENABLE_CTP 重新编译"};
#else
    std::unique_lock<std::mutex> lock(impl_->mutex);
    auto& slot = impl_->sessions[account.md_front];
    if (slot && slot->logged_in) {
        return {true, "行情已连接: " + account.md_front};
    }

    if (!slot) {
        slot = std::make_unique<MdSession>();
        slot->front = account.md_front;
    }
    slot->account = account;
    slot->tick_callback = impl_->tick_callback;
    slot->disconnect_callback = impl_->disconnect_callback;

    if (slot->api == nullptr) {
        const fs::path flow_dir =
            impl_->project_root / "data" / "flow" / ("md_" + std::to_string(std::hash<std::string>{}(account.md_front)));
        fs::create_directories(flow_dir);
        slot->spi = new MdSpiImpl(slot.get());
        slot->api = CThostFtdcMdApi::CreateFtdcMdApi(flow_dir.string().c_str(), false, false, true);
        slot->api->RegisterSpi(slot->spi);
        char front[256]{};
        copy_to_field(front, account.md_front);
        slot->api->RegisterFront(front);
        slot->api->Init();
        Logger::instance().info("Md Init: " + account.md_front);
    }

    lock.unlock();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    {
        std::unique_lock<std::mutex> wait_lock(slot->mutex);
        slot->cv.wait_until(wait_lock, deadline, [&]() { return slot->logged_in.load() || !slot->last_error.empty(); });
    }

    if (slot->logged_in) {
        return {true, "Md 登录成功"};
    }
    if (!slot->last_error.empty()) {
        return {false, slot->last_error};
    }
    return {false, "Md 登录超时"};
#endif
}

ConnectResult QuoteEngine::disconnect(const std::string& md_front) {
    if (md_front.empty()) {
        return {false, "md_front 为空"};
    }
#ifndef QUANT_SEV_HAS_CTP
    return {false, "CTP 未启用"};
#else
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->sessions.find(md_front);
    if (it == impl_->sessions.end()) {
        return {true, "行情未连接"};
    }
    destroy_session(*it->second);
    impl_->sessions.erase(it);
    Logger::instance().info("Md 已断开: " + md_front);
    return {true, "Md 已断开"};
#endif
}

bool QuoteEngine::is_ready() const {
#ifndef QUANT_SEV_HAS_CTP
    return false;
#else
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const auto& [_, session] : impl_->sessions) {
        if (session->logged_in.load()) {
            return true;
        }
    }
    return false;
#endif
}

bool QuoteEngine::is_front_ready(const std::string& md_front) const {
#ifndef QUANT_SEV_HAS_CTP
    (void)md_front;
    return false;
#else
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->sessions.find(md_front);
    return it != impl_->sessions.end() && it->second->logged_in.load();
#endif
}

bool QuoteEngine::is_user_md_ready(const std::string& user_id) const {
#ifndef QUANT_SEV_HAS_CTP
    (void)user_id;
    return false;
#else
    if (user_id.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const auto& [_, session] : impl_->sessions) {
        if (session->logged_in.load() && session->account.user_id == user_id) {
            return true;
        }
    }
    return false;
#endif
}

nlohmann::json QuoteEngine::md_sessions_status() const {
#ifndef QUANT_SEV_HAS_CTP
    return {{"fronts", nlohmann::json::array()}, {"users", nlohmann::json::array()}};
#else
    nlohmann::json fronts = nlohmann::json::array();
    nlohmann::json users = nlohmann::json::array();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const auto& [front, session] : impl_->sessions) {
        if (!session->logged_in.load()) {
            continue;
        }
        fronts.push_back(front);
        if (!session->account.user_id.empty()) {
            users.push_back(session->account.user_id);
        }
    }
    return {{"fronts", fronts}, {"users", users}};
#endif
}

nlohmann::json QuoteEngine::quote_board() const {
    nlohmann::json board = nlohmann::json::array();
#ifndef QUANT_SEV_HAS_CTP
    return board;
#else
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const auto& [front, session] : impl_->sessions) {
        std::lock_guard<std::mutex> tick_lock(session->mutex);
        for (const auto& [instrument, tick] : session->ticks) {
            nlohmann::json row = tick;
            row["md_front"] = front;
            board.push_back(row);
        }
    }
    return board;
#endif
}

ConnectResult QuoteEngine::subscribe_instruments(const std::string& md_front,
                                                 const std::vector<std::string>& instruments) {
    if (md_front.empty() || instruments.empty()) {
        return {false, "md_front 或合约列表为空"};
    }
#ifndef QUANT_SEV_HAS_CTP
    return {false, "CTP 未启用"};
#else
    std::unique_lock<std::mutex> lock(impl_->mutex);
    const auto it = impl_->sessions.find(md_front);
    if (it == impl_->sessions.end() || !it->second->logged_in) {
        return {false, "行情未登录: " + md_front};
    }

    auto& session = *it->second;
    std::vector<std::string> pending;
    for (const auto& id : instruments) {
        if (id.empty()) {
            continue;
        }
        if (std::find(session.subscribed.begin(), session.subscribed.end(), id) == session.subscribed.end()) {
            session.subscribed.push_back(id);
            pending.push_back(id);
        }
    }
    if (pending.empty()) {
        return {true, "合约已在订阅列表"};
    }

    constexpr int kBatchSize = 50;
    int total = 0;
    for (std::size_t offset = 0; offset < pending.size(); offset += kBatchSize) {
        const auto batch_end = std::min(offset + static_cast<std::size_t>(kBatchSize), pending.size());
        std::vector<char*> ptrs;
        ptrs.reserve(batch_end - offset);
        for (std::size_t i = offset; i < batch_end; ++i) {
            ptrs.push_back(const_cast<char*>(pending[i].c_str()));
        }
        const int rc = session.api->SubscribeMarketData(ptrs.data(), static_cast<int>(ptrs.size()));
        if (rc != 0) {
            return {false, "SubscribeMarketData 返回 " + std::to_string(rc) + "（已提交 " +
                            std::to_string(total) + " 个）"};
        }
        total += static_cast<int>(ptrs.size());
    }
    return {true, "已提交订阅 " + std::to_string(total) + " 个合约"};
#endif
}

ConnectResult QuoteEngine::unsubscribe_instruments(const std::string& md_front,
                                                   const std::vector<std::string>& instruments) {
    if (md_front.empty() || instruments.empty()) {
        return {false, "md_front 或合约列表为空"};
    }
#ifndef QUANT_SEV_HAS_CTP
    return {false, "CTP 未启用"};
#else
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->sessions.find(md_front);
    if (it == impl_->sessions.end() || !it->second->logged_in) {
        return {false, "行情未登录: " + md_front};
    }

    auto& session = *it->second;
    std::vector<char*> ptrs;
    std::vector<std::string> pending;
    for (const auto& id : instruments) {
        if (id.empty()) {
            continue;
        }
        pending.push_back(id);
        ptrs.push_back(const_cast<char*>(id.c_str()));
    }
    if (pending.empty()) {
        return {false, "无有效合约"};
    }

    session.api->UnSubscribeMarketData(ptrs.data(), static_cast<int>(ptrs.size()));
    for (const auto& id : pending) {
        session.subscribed.erase(std::remove(session.subscribed.begin(), session.subscribed.end(), id),
                                 session.subscribed.end());
        session.ticks.erase(id);
    }
    return {true, "已退订 " + std::to_string(pending.size()) + " 个合约"};
#endif
}

std::vector<std::string> QuoteEngine::subscribed_instruments(const std::string& md_front) const {
#ifndef QUANT_SEV_HAS_CTP
    (void)md_front;
    return {};
#else
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->sessions.find(md_front);
    if (it == impl_->sessions.end()) {
        return {};
    }
    std::lock_guard<std::mutex> session_lock(it->second->mutex);
    return it->second->subscribed;
#endif
}

}  // namespace quant_sev::core
