#include "Trade/TradeEngine.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Common/ContractRules.hpp"
#include "Common/CtpUtil.hpp"
#include "Common/JsonQuery.hpp"
#include "Logger/Logger.hpp"
#include "Trade/OrderTypes.hpp"

#ifdef QUANT_SEV_HAS_CTP
#include "ThostFtdcTraderApi.h"
#endif

namespace quant_sev::core {

namespace fs = std::filesystem;

#ifdef QUANT_SEV_HAS_CTP

namespace {

class TdSession;

class TdSpiImpl : public CThostFtdcTraderSpi {
public:
    explicit TdSpiImpl(TdSession* session) : session_(session) {}

    void OnFrontConnected() override;
    void OnFrontDisconnected(int nReason) override;
    void OnRspAuthenticate(CThostFtdcRspAuthenticateField* /*pRspAuthenticateField*/,
                            CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;
    void OnRspUserLogin(CThostFtdcRspUserLoginField* /*pRspUserLogin*/, CThostFtdcRspInfoField* pRspInfo,
                        int nRequestID, bool bIsLast) override;
    void OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField* /*pSettlementInfoConfirm*/,
                                    CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;
    void OnRspOrderInsert(CThostFtdcInputOrderField* pInputOrder, CThostFtdcRspInfoField* pRspInfo,
                          int nRequestID, bool bIsLast) override;
    void OnRspOrderAction(CThostFtdcInputOrderActionField* pInputOrderAction, CThostFtdcRspInfoField* pRspInfo,
                          int nRequestID, bool bIsLast) override;
    void OnRtnOrder(CThostFtdcOrderField* pOrder) override;
    void OnRtnTrade(CThostFtdcTradeField* pTrade) override;
    void OnRspQryOrder(CThostFtdcOrderField* pOrder, CThostFtdcRspInfoField* pRspInfo, int nRequestID,
                       bool bIsLast) override;
    void OnRspQryTrade(CThostFtdcTradeField* pTrade, CThostFtdcRspInfoField* pRspInfo, int nRequestID,
                       bool bIsLast) override;
    void OnRspQryTradingAccount(CThostFtdcTradingAccountField* pTradingAccount, CThostFtdcRspInfoField* pRspInfo,
                                int nRequestID, bool bIsLast) override;
    void OnRspQryInvestorPosition(CThostFtdcInvestorPositionField* pInvestorPosition, CThostFtdcRspInfoField* pRspInfo,
                                  int nRequestID, bool bIsLast) override;
    void OnRspError(CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;

private:
    void request_login();
    TdSession* session_;
};

struct HistoryQueryState {
    int request_id{0};
    bool ready{false};
    bool is_orders{false};
    std::string error;
    std::vector<OrderUpdateRecord> orders;
    std::vector<TradeUpdateRecord> trades;
};

struct AccountQueryState {
    int request_id{0};
    bool ready{false};
    std::string error;
    TradingAccountSnapshot account;
    bool has_account{false};
};

struct PositionQueryState {
    int request_id{0};
    bool ready{false};
    std::string error;
    std::unordered_map<std::string, InvestorPositionSnapshot> by_instrument;
};

struct TdSession {
    AccountRecord account;
    CThostFtdcTraderApi* api{nullptr};
    TdSpiImpl* spi{nullptr};
    std::atomic<bool> front_connected{false};
    std::atomic<bool> logged_in{false};
    std::atomic<int> request_id{1};
    std::mutex mutex;
    std::condition_variable cv;
    std::string last_error;
    bool settlement_confirmed{false};
    int order_ref_seq{1};
    int front_id{0};
    int session_id{0};
    int pending_order_request_id{0};
    OrderResult pending_order_result;
    bool order_response_ready{false};
    int pending_action_request_id{0};
    CancelResult pending_action_result;
    bool action_response_ready{false};
    std::vector<OrderUpdateRecord> order_updates;
    std::vector<TradeUpdateRecord> trade_updates;
    HistoryQueryState pending_history;
    AccountQueryState pending_account;
    PositionQueryState pending_positions;
    TradingAccountSnapshot trading_account_cache;
    bool trading_account_cached{false};
    std::vector<InvestorPositionSnapshot> position_cache;
    bool position_cached{false};
    std::function<void(const nlohmann::json&)> order_notify;
    std::function<void(const nlohmann::json&)> trade_notify;
    std::function<void(const std::string&, int)> disconnect_callback;
};

void TdSpiImpl::request_login() {
    CThostFtdcReqUserLoginField req{};
    copy_to_field(req.BrokerID, session_->account.broker_id);
    copy_to_field(req.UserID, session_->account.user_id);
    copy_to_field(req.Password, session_->account.password);
    session_->api->ReqUserLogin(&req, session_->request_id.fetch_add(1));
}

void TdSpiImpl::OnFrontConnected() {
    if (session_ == nullptr || session_->api == nullptr) {
        return;
    }
    session_->front_connected = true;
    Logger::instance().ctp("Td 前置已连接: " + session_->account.user_id);

    CThostFtdcReqAuthenticateField req{};
    copy_to_field(req.BrokerID, session_->account.broker_id);
    copy_to_field(req.UserID, session_->account.user_id);
    copy_to_field(req.AppID, session_->account.app_id);
    copy_to_field(req.AuthCode, session_->account.auth_code);
    session_->api->ReqAuthenticate(&req, session_->request_id.fetch_add(1));
}

void TdSpiImpl::OnFrontDisconnected(int nReason) {
    if (session_ == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(session_->mutex);
    session_->front_connected = false;
    session_->logged_in = false;
    session_->settlement_confirmed = false;
    Logger::instance().warn("Td 前置断开: " + session_->account.user_id + " reason=0x" +
                            std::to_string(static_cast<unsigned>(nReason)));
    session_->cv.notify_all();
    if (session_->disconnect_callback) {
        session_->disconnect_callback(session_->account.user_id, nReason);
    }
}

void TdSpiImpl::OnRspAuthenticate(CThostFtdcRspAuthenticateField* /*pRspAuthenticateField*/,
                                   CThostFtdcRspInfoField* pRspInfo, int /*nRequestID*/, bool bIsLast) {
    if (session_ == nullptr || !bIsLast) {
        return;
    }
    if (pRspInfo != nullptr && pRspInfo->ErrorID != 0) {
        std::lock_guard<std::mutex> lock(session_->mutex);
        session_->last_error = "Td 认证失败(" + std::to_string(pRspInfo->ErrorID) + "): " +
                               trim_cstr(pRspInfo->ErrorMsg);
        Logger::instance().error(session_->last_error);
        session_->cv.notify_all();
        return;
    }
    Logger::instance().ctp("DialogRsp: Td OnRspAuthenticate OK user=" + session_->account.user_id);
    request_login();
}

void TdSpiImpl::OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin,
                               CThostFtdcRspInfoField* pRspInfo, int /*nRequestID*/, bool bIsLast) {
    if (session_ == nullptr || !bIsLast) {
        return;
    }
    if (pRspInfo != nullptr && pRspInfo->ErrorID != 0) {
        std::lock_guard<std::mutex> lock(session_->mutex);
        session_->last_error = "Td 登录失败(" + std::to_string(pRspInfo->ErrorID) + "): " +
                               trim_cstr(pRspInfo->ErrorMsg);
        Logger::instance().error(session_->last_error);
        Logger::instance().error(session_->last_error);
        session_->cv.notify_all();
        return;
    }
    if (pRspUserLogin != nullptr) {
        session_->front_id = pRspUserLogin->FrontID;
        session_->session_id = pRspUserLogin->SessionID;
        Logger::instance().ctp("DialogRsp: Td OnRspUserLogin OK user=" + session_->account.user_id +
                               " FrontID=" + std::to_string(pRspUserLogin->FrontID) +
                               " SessionID=" + std::to_string(pRspUserLogin->SessionID) +
                               " TradingDay=" + trim_cstr(pRspUserLogin->TradingDay));
    }

    CThostFtdcSettlementInfoConfirmField confirm{};
    copy_to_field(confirm.BrokerID, session_->account.broker_id);
    copy_to_field(confirm.InvestorID, session_->account.user_id);
    session_->api->ReqSettlementInfoConfirm(&confirm, session_->request_id.fetch_add(1));
}

void TdSpiImpl::OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField* /*pSettlementInfoConfirm*/,
                                           CThostFtdcRspInfoField* pRspInfo, int /*nRequestID*/, bool bIsLast) {
    if (session_ == nullptr || !bIsLast) {
        return;
    }
    std::lock_guard<std::mutex> lock(session_->mutex);
    if (pRspInfo != nullptr && pRspInfo->ErrorID != 0) {
        session_->last_error = "Td 结算确认失败(" + std::to_string(pRspInfo->ErrorID) + "): " +
                               trim_cstr(pRspInfo->ErrorMsg);
        Logger::instance().error(session_->last_error);
    } else {
        session_->logged_in = true;
        session_->settlement_confirmed = true;
        session_->last_error.clear();
        Logger::instance().ctp("DialogRsp: Td OnRspSettlementInfoConfirm OK user=" + session_->account.user_id);
    }
    session_->cv.notify_all();
}

void TdSpiImpl::OnRspOrderInsert(CThostFtdcInputOrderField* pInputOrder, CThostFtdcRspInfoField* pRspInfo,
                                 int nRequestID, bool bIsLast) {
    if (session_ == nullptr) {
        return;
    }
    if (nRequestID != session_->pending_order_request_id) {
        return;
    }
    const bool has_error = pRspInfo != nullptr && pRspInfo->ErrorID != 0;
    if (!has_error && !bIsLast) {
        return;
    }
    std::lock_guard<std::mutex> lock(session_->mutex);
    session_->order_response_ready = true;
    if (pInputOrder != nullptr) {
        session_->pending_order_result.order_ref = trim_cstr(pInputOrder->OrderRef);
    }
    if (has_error) {
        session_->pending_order_result.ok = false;
        session_->pending_order_result.error_id = pRspInfo->ErrorID;
        const char* hint = ctp_order_error_hint(pRspInfo->ErrorID);
        if (hint != nullptr) {
            session_->pending_order_result.message =
                "报单拒绝(" + std::to_string(pRspInfo->ErrorID) + "): " + hint;
        } else {
            session_->pending_order_result.message =
                "报单拒绝(" + std::to_string(pRspInfo->ErrorID) + "): " +
                sanitize_utf8(trim_cstr(pRspInfo->ErrorMsg));
        }
        Logger::instance().ctp("DialogRsp: Td OnRspOrderInsert FAIL OrderRef=" +
                               session_->pending_order_result.order_ref + " ErrorID=" +
                               std::to_string(pRspInfo->ErrorID) + " " +
                               sanitize_utf8(trim_cstr(pRspInfo->ErrorMsg)));
    } else if (!session_->pending_order_result.ok) {
        session_->pending_order_result.ok = true;
        session_->pending_order_result.message = "报单已提交";
        const std::string inst =
            pInputOrder != nullptr ? trim_cstr(pInputOrder->InstrumentID) : std::string{};
        Logger::instance().ctp("DialogRsp: Td OnRspOrderInsert OK OrderRef=" +
                               session_->pending_order_result.order_ref +
                               (inst.empty() ? "" : " InstrumentID=" + inst));
    }
    session_->cv.notify_all();
}

std::string map_order_status(char status) {
    switch (status) {
        case THOST_FTDC_OST_AllTraded:
            return "all_traded";
        case THOST_FTDC_OST_PartTradedQueueing:
        case THOST_FTDC_OST_PartTradedNotQueueing:
            return "part_traded";
        case THOST_FTDC_OST_NoTradeQueueing:
            return "no_trade";
        case THOST_FTDC_OST_NoTradeNotQueueing:
            return "no_trade";
        case THOST_FTDC_OST_Canceled:
            return "canceled";
        case THOST_FTDC_OST_NotTouched:
            return "not_touched";
        case THOST_FTDC_OST_Touched:
            return "touched";
        default:
            return "unknown";
    }
}

std::string map_direction_text(char direction) {
    return direction == THOST_FTDC_D_Sell ? "sell" : "buy";
}

std::string map_offset_text(char offset) {
    if (offset == THOST_FTDC_OF_Close) {
        return "close";
    }
    if (offset == THOST_FTDC_OF_CloseToday) {
        return "close_today";
    }
    return "open";
}

std::string normalize_yyyymmdd(std::string value) {
    std::string digits;
    digits.reserve(8);
    for (char ch : value) {
        if (ch >= '0' && ch <= '9') {
            digits.push_back(ch);
        }
    }
    if (digits.size() >= 8) {
        return digits.substr(0, 8);
    }
    return digits;
}

bool date_in_range(const std::string& yyyymmdd, const std::string& from, const std::string& to) {
    if (yyyymmdd.empty()) {
        return true;
    }
    if (!from.empty() && yyyymmdd < from) {
        return false;
    }
    if (!to.empty() && yyyymmdd > to) {
        return false;
    }
    return true;
}

void merge_order_record(std::vector<OrderUpdateRecord>& target, const OrderUpdateRecord& record) {
    for (auto& existing : target) {
        const bool same_sys = !record.order_sys_id.empty() && existing.order_sys_id == record.order_sys_id;
        const bool same_ref = !record.order_ref.empty() && existing.order_ref == record.order_ref &&
                              existing.instrument_id == record.instrument_id;
        if (same_sys || same_ref) {
            existing = record;
            return;
        }
    }
    target.push_back(record);
    if (target.size() > 2000) {
        target.erase(target.begin(), target.begin() + static_cast<std::ptrdiff_t>(target.size() - 2000));
    }
}

void merge_trade_record(std::vector<TradeUpdateRecord>& target, const TradeUpdateRecord& record) {
    for (auto& existing : target) {
        if (!record.trade_id.empty() && existing.trade_id == record.trade_id) {
            existing = record;
            return;
        }
    }
    target.push_back(record);
    if (target.size() > 2000) {
        target.erase(target.begin(), target.begin() + static_cast<std::ptrdiff_t>(target.size() - 2000));
    }
}

nlohmann::json order_update_to_json(const OrderUpdateRecord& record) {
    return {{"user_id", record.user_id},
            {"instrument_id", record.instrument_id},
            {"order_ref", record.order_ref},
            {"order_sys_id", record.order_sys_id},
            {"direction", record.direction},
            {"offset", record.offset},
            {"status", record.status},
            {"limit_price", record.limit_price},
            {"volume_total", record.volume_total},
            {"volume_traded", record.volume_traded},
            {"volume_left", record.volume_left},
            {"insert_date", record.insert_date},
            {"insert_time", record.insert_time},
            {"update_time", record.update_time},
            {"status_msg", sanitize_utf8(record.status_msg)}};
}

nlohmann::json trade_update_to_json(const TradeUpdateRecord& record) {
    const double turnover = record.price * static_cast<double>(record.volume);
    return {{"user_id", record.user_id},
            {"trade_id", record.trade_id},
            {"order_ref", record.order_ref},
            {"order_sys_id", record.order_sys_id},
            {"instrument_id", record.instrument_id},
            {"exchange_id", record.exchange_id},
            {"direction", record.direction},
            {"offset", record.offset},
            {"price", record.price},
            {"volume", record.volume},
            {"turnover", turnover},
            {"trade_date", record.trade_date},
            {"trade_time", record.trade_time}};
}

nlohmann::json trading_account_to_json(const TradingAccountSnapshot& account) {
    return {{"user_id", account.user_id},
            {"broker_id", account.broker_id},
            {"account_id", account.account_id},
            {"trading_day", account.trading_day},
            {"currency_id", account.currency_id},
            {"balance", account.balance},
            {"available", account.available},
            {"curr_margin", account.curr_margin},
            {"frozen_margin", account.frozen_margin},
            {"frozen_cash", account.frozen_cash},
            {"frozen_commission", account.frozen_commission},
            {"commission", account.commission},
            {"close_profit", account.close_profit},
            {"position_profit", account.position_profit},
            {"withdraw_quota", account.withdraw_quota},
            {"deposit", account.deposit},
            {"withdraw", account.withdraw},
            {"pre_balance", account.pre_balance},
            {"exchange_margin", account.exchange_margin}};
}

TradingAccountSnapshot build_trading_account(CThostFtdcTradingAccountField* field, const std::string& user_id) {
    TradingAccountSnapshot account;
    if (field == nullptr) {
        return account;
    }
    account.user_id = user_id;
    account.broker_id = trim_cstr(field->BrokerID);
    account.account_id = trim_cstr(field->AccountID);
    account.trading_day = trim_cstr(field->TradingDay);
    account.currency_id = trim_cstr(field->CurrencyID);
    account.balance = field->Balance;
    account.available = field->Available;
    account.curr_margin = field->CurrMargin;
    account.frozen_margin = field->FrozenMargin;
    account.frozen_cash = field->FrozenCash;
    account.frozen_commission = field->FrozenCommission;
    account.commission = field->Commission;
    account.close_profit = field->CloseProfit;
    account.position_profit = field->PositionProfit;
    account.withdraw_quota = field->WithdrawQuota;
    account.deposit = field->Deposit;
    account.withdraw = field->Withdraw;
    account.pre_balance = field->PreBalance;
    account.exchange_margin = field->ExchangeMargin;
    return account;
}

TradeUpdateRecord build_trade_update(CThostFtdcTradeField* pTrade, const std::string& user_id) {
    TradeUpdateRecord record;
    record.user_id = user_id;
    record.trade_id = trim_cstr(pTrade->TradeID);
    record.order_ref = trim_cstr(pTrade->OrderRef);
    record.order_sys_id = trim_cstr(pTrade->OrderSysID);
    record.instrument_id = trim_cstr(pTrade->InstrumentID);
    record.exchange_id = trim_cstr(pTrade->ExchangeID);
    record.direction = map_direction_text(pTrade->Direction);
    record.offset = map_offset_text(pTrade->OffsetFlag);
    record.price = pTrade->Price;
    record.volume = pTrade->Volume;
    record.trade_date = trim_cstr(pTrade->TradeDate);
    record.trade_time = trim_cstr(pTrade->TradeTime);
    return record;
}

OrderUpdateRecord build_order_update(CThostFtdcOrderField* pOrder, const std::string& user_id) {
    OrderUpdateRecord record;
    record.user_id = user_id;
    record.instrument_id = trim_cstr(pOrder->InstrumentID);
    record.order_ref = trim_cstr(pOrder->OrderRef);
    record.order_sys_id = trim_cstr(pOrder->OrderSysID);
    record.direction = map_direction_text(pOrder->Direction);
    record.offset = map_offset_text(pOrder->CombOffsetFlag[0]);
    record.status = map_order_status(pOrder->OrderStatus);
    record.limit_price = pOrder->LimitPrice;
    record.volume_total = pOrder->VolumeTotalOriginal;
    record.volume_traded = pOrder->VolumeTraded;
    record.volume_left = pOrder->VolumeTotal;
    record.insert_date = trim_cstr(pOrder->InsertDate);
    if (record.insert_date.empty()) {
        record.insert_date = trim_cstr(pOrder->TradingDay);
    }
    record.insert_time = trim_cstr(pOrder->InsertTime);
    record.update_time = trim_cstr(pOrder->UpdateTime);
    record.status_msg = sanitize_utf8(trim_cstr(pOrder->StatusMsg));
    return record;
}

void TdSpiImpl::OnRtnOrder(CThostFtdcOrderField* pOrder) {
    if (session_ == nullptr || pOrder == nullptr) {
        return;
    }
    const OrderUpdateRecord record = build_order_update(pOrder, session_->account.user_id);
    const nlohmann::json payload = order_update_to_json(record);
    std::function<void(const nlohmann::json&)> notify;
    {
        std::lock_guard<std::mutex> lock(session_->mutex);
        if (session_->pending_order_request_id != 0 && !session_->order_response_ready &&
            record.order_ref == session_->pending_order_result.order_ref) {
            session_->order_response_ready = true;
            session_->pending_order_result.ok = true;
            session_->pending_order_result.message = "报单已提交";
            session_->cv.notify_all();
        }
        session_->order_updates.push_back(record);
        if (session_->order_updates.size() > 500) {
            session_->order_updates.erase(
                session_->order_updates.begin(),
                session_->order_updates.begin() +
                    static_cast<std::ptrdiff_t>(session_->order_updates.size() - 500));
        }
        notify = session_->order_notify;
    }
    Logger::instance().ctp("DialogRsp: Td OnRtnOrder InstrumentID=" + record.instrument_id + " OrderRef=" +
                           record.order_ref + " OrderSysID=" + record.order_sys_id + " Status=" +
                           record.status + " Vol=" + std::to_string(record.volume_traded) + "/" +
                           std::to_string(record.volume_total) + " Price=" +
                           std::to_string(record.limit_price) +
                           (record.status_msg.empty() ? "" : " StatusMsg=" + record.status_msg));
    if (notify) {
        try {
            notify(payload);
        } catch (const std::exception& ex) {
            Logger::instance().error(std::string("order_notify 异常: ") + ex.what());
        } catch (...) {
            Logger::instance().error("order_notify 未知异常");
        }
    }
}

void TdSpiImpl::OnRtnTrade(CThostFtdcTradeField* pTrade) {
    if (session_ == nullptr || pTrade == nullptr) {
        return;
    }
    const TradeUpdateRecord record = build_trade_update(pTrade, session_->account.user_id);
    const nlohmann::json payload = trade_update_to_json(record);
    std::function<void(const nlohmann::json&)> notify;
    {
        std::lock_guard<std::mutex> lock(session_->mutex);
        session_->trade_updates.push_back(record);
        if (session_->trade_updates.size() > 500) {
            session_->trade_updates.erase(
                session_->trade_updates.begin(),
                session_->trade_updates.begin() +
                    static_cast<std::ptrdiff_t>(session_->trade_updates.size() - 500));
        }
        notify = session_->trade_notify;
    }
    Logger::instance().ctp("DialogRsp: Td OnRtnTrade InstrumentID=" + record.instrument_id + " TradeID=" +
                           record.trade_id + " OrderRef=" + record.order_ref + " Direction=" +
                           record.direction + " Offset=" + record.offset + " Vol=" +
                           std::to_string(record.volume) + " Price=" + std::to_string(record.price));
    if (notify) {
        try {
            notify(payload);
        } catch (const std::exception& ex) {
            Logger::instance().error(std::string("trade_notify 异常: ") + ex.what());
        } catch (...) {
            Logger::instance().error("trade_notify 未知异常");
        }
    }
}

void TdSpiImpl::OnRspQryOrder(CThostFtdcOrderField* pOrder, CThostFtdcRspInfoField* pRspInfo, int nRequestID,
                             bool bIsLast) {
    if (session_ == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(session_->mutex);
    if (nRequestID != session_->pending_history.request_id || !session_->pending_history.is_orders) {
        return;
    }
    if (pRspInfo != nullptr && pRspInfo->ErrorID != 0) {
        session_->pending_history.error =
            "查询委托失败(" + std::to_string(pRspInfo->ErrorID) + "): " + trim_cstr(pRspInfo->ErrorMsg);
    }
    if (pOrder != nullptr) {
        session_->pending_history.orders.push_back(build_order_update(pOrder, session_->account.user_id));
    }
    if (bIsLast) {
        session_->pending_history.ready = true;
        session_->cv.notify_all();
    }
}

void TdSpiImpl::OnRspQryTrade(CThostFtdcTradeField* pTrade, CThostFtdcRspInfoField* pRspInfo, int nRequestID,
                              bool bIsLast) {
    if (session_ == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(session_->mutex);
    if (nRequestID != session_->pending_history.request_id || session_->pending_history.is_orders) {
        return;
    }
    if (pRspInfo != nullptr && pRspInfo->ErrorID != 0) {
        session_->pending_history.error =
            "查询成交失败(" + std::to_string(pRspInfo->ErrorID) + "): " + trim_cstr(pRspInfo->ErrorMsg);
    }
    if (pTrade != nullptr) {
        session_->pending_history.trades.push_back(build_trade_update(pTrade, session_->account.user_id));
    }
    if (bIsLast) {
        session_->pending_history.ready = true;
        session_->cv.notify_all();
    }
}

void TdSpiImpl::OnRspQryTradingAccount(CThostFtdcTradingAccountField* pTradingAccount,
                                       CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) {
    if (session_ == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(session_->mutex);
    if (nRequestID != session_->pending_account.request_id) {
        return;
    }
    if (pRspInfo != nullptr && pRspInfo->ErrorID != 0) {
        session_->pending_account.error =
            "查询资金失败(" + std::to_string(pRspInfo->ErrorID) + "): " + trim_cstr(pRspInfo->ErrorMsg);
    }
    if (pTradingAccount != nullptr) {
        session_->pending_account.account =
            build_trading_account(pTradingAccount, session_->account.user_id);
        session_->pending_account.has_account = true;
        session_->trading_account_cache = session_->pending_account.account;
        session_->trading_account_cached = true;
    }
    if (bIsLast) {
        if (session_->pending_account.has_account) {
            const auto& acc = session_->pending_account.account;
            Logger::instance().ctp("QueryRsp: Td OnRspQryTradingAccount user=" + session_->account.user_id +
                                   " Balance=" + std::to_string(acc.balance) + " Available=" +
                                   std::to_string(acc.available) + " Margin=" + std::to_string(acc.curr_margin));
        } else {
            Logger::instance().ctp("QueryRsp: Td OnRspQryTradingAccount user=" + session_->account.user_id);
        }
        session_->pending_account.ready = true;
        session_->cv.notify_all();
    }
}

void merge_investor_position_row(std::unordered_map<std::string, InvestorPositionSnapshot>& book,
                                 CThostFtdcInvestorPositionField* field) {
    if (field == nullptr) {
        return;
    }
    const int volume = field->Position;
    if (volume <= 0) {
        return;
    }
    const std::string instrument_id = trim_cstr(field->InstrumentID);
    if (instrument_id.empty()) {
        return;
    }
    auto& snap = book[instrument_id];
    snap.instrument_id = instrument_id;
    const std::string exchange_id = trim_cstr(field->ExchangeID);
    if (!exchange_id.empty()) {
        snap.exchange_id = exchange_id;
    }
    const double avg_price = volume > 0 ? field->PositionCost / static_cast<double>(volume) : 0.0;
    snap.position_profit += field->PositionProfit;
    const bool is_today = field->PositionDate == THOST_FTDC_PSD_Today;
    if (field->PosiDirection == THOST_FTDC_PD_Long) {
        if (is_today) {
            snap.long_today += volume;
        } else {
            snap.long_yd += volume;
        }
        snap.long_volume = snap.long_today + snap.long_yd;
        if (avg_price > 0) {
            snap.avg_long_price = avg_price;
        }
        snap.long_profit += field->PositionProfit;
    } else if (field->PosiDirection == THOST_FTDC_PD_Short) {
        if (is_today) {
            snap.short_today += volume;
        } else {
            snap.short_yd += volume;
        }
        snap.short_volume = snap.short_today + snap.short_yd;
        if (avg_price > 0) {
            snap.avg_short_price = avg_price;
        }
        snap.short_profit += field->PositionProfit;
    } else {
        if (is_today) {
            snap.long_today += volume;
        } else {
            snap.long_yd += volume;
        }
        snap.long_volume = snap.long_today + snap.long_yd;
        if (avg_price > 0) {
            snap.avg_long_price = avg_price;
        }
        snap.long_profit += field->PositionProfit;
    }
}

void TdSpiImpl::OnRspQryInvestorPosition(CThostFtdcInvestorPositionField* pInvestorPosition,
                                         CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) {
    if (session_ == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(session_->mutex);
    if (nRequestID != session_->pending_positions.request_id) {
        return;
    }
    if (pRspInfo != nullptr && pRspInfo->ErrorID != 0) {
        session_->pending_positions.error =
            "查询持仓失败(" + std::to_string(pRspInfo->ErrorID) + "): " + trim_cstr(pRspInfo->ErrorMsg);
    }
    if (pInvestorPosition != nullptr) {
        merge_investor_position_row(session_->pending_positions.by_instrument, pInvestorPosition);
    }
    if (bIsLast) {
        Logger::instance().ctp("QueryRsp: Td OnRspQryInvestorPosition legs=" +
                               std::to_string(session_->pending_positions.by_instrument.size()) + " user=" +
                               session_->account.user_id);
        session_->pending_positions.ready = true;
        session_->cv.notify_all();
    }
}

void TdSpiImpl::OnRspOrderAction(CThostFtdcInputOrderActionField* pInputOrderAction,
                                 CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) {
    if (session_ == nullptr || !bIsLast) {
        return;
    }
    if (nRequestID != session_->pending_action_request_id) {
        return;
    }
    std::lock_guard<std::mutex> lock(session_->mutex);
    session_->action_response_ready = true;
    if (pInputOrderAction != nullptr) {
        session_->pending_action_result.order_ref = trim_cstr(pInputOrderAction->OrderRef);
    }
    if (pRspInfo != nullptr && pRspInfo->ErrorID != 0) {
        session_->pending_action_result.ok = false;
        session_->pending_action_result.error_id = pRspInfo->ErrorID;
        session_->pending_action_result.message =
            "撤单拒绝(" + std::to_string(pRspInfo->ErrorID) + "): " + trim_cstr(pRspInfo->ErrorMsg);
        Logger::instance().ctp("DialogRsp: Td OnRspOrderAction FAIL OrderRef=" +
                               session_->pending_action_result.order_ref + " ErrorID=" +
                               std::to_string(pRspInfo->ErrorID) + " " + trim_cstr(pRspInfo->ErrorMsg));
    } else {
        session_->pending_action_result.ok = true;
        session_->pending_action_result.message = "撤单已提交";
        Logger::instance().ctp("DialogRsp: Td OnRspOrderAction OK OrderRef=" +
                               session_->pending_action_result.order_ref);
    }
    session_->cv.notify_all();
}

void TdSpiImpl::OnRspError(CThostFtdcRspInfoField* pRspInfo, int /*nRequestID*/, bool bIsLast) {
    if (session_ == nullptr || !bIsLast || pRspInfo == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(session_->mutex);
    session_->last_error = "Td 错误(" + std::to_string(pRspInfo->ErrorID) + "): " +
                           trim_cstr(pRspInfo->ErrorMsg);
    Logger::instance().error(session_->last_error);
    if (session_->pending_order_request_id != 0 && !session_->order_response_ready) {
        session_->order_response_ready = true;
        session_->pending_order_result.ok = false;
        session_->pending_order_result.error_id = pRspInfo->ErrorID;
        session_->pending_order_result.message = session_->last_error;
    }
    if (session_->pending_action_request_id != 0 && !session_->action_response_ready) {
        session_->action_response_ready = true;
        session_->pending_action_result.ok = false;
        session_->pending_action_result.error_id = pRspInfo->ErrorID;
        session_->pending_action_result.message = session_->last_error;
    }
    session_->cv.notify_all();
}

void destroy_session(TdSession& session) {
    if (session.api != nullptr) {
        session.api->RegisterSpi(nullptr);
        session.api->Release();
        session.api = nullptr;
    }
    delete session.spi;
    session.spi = nullptr;
    session.front_connected = false;
    session.logged_in = false;
    session.settlement_confirmed = false;
}

}  // namespace

#endif

#ifdef QUANT_SEV_HAS_CTP
namespace {

char map_direction(const std::string& direction) {
    if (direction == "sell") {
        return THOST_FTDC_D_Sell;
    }
    return THOST_FTDC_D_Buy;
}

char map_offset_flag(const std::string& offset) {
    if (offset == "close") {
        return THOST_FTDC_OF_Close;
    }
    if (offset == "close_today" || offset == "close-today") {
        return THOST_FTDC_OF_CloseToday;
    }
    return THOST_FTDC_OF_Open;
}

char map_price_type(const std::string& price_type) {
    if (price_type == "market") {
        return THOST_FTDC_OPT_AnyPrice;
    }
    if (price_type == "opponent") {
        return THOST_FTDC_OPT_BestPrice;
    }
    return THOST_FTDC_OPT_LimitPrice;
}

}  // namespace
#endif

#ifdef QUANT_SEV_HAS_CTP
namespace {

std::string td_session_key(const AccountRecord& account) {
    return account.user_id + "\x1f" + account.td_front;
}

}  // namespace
#endif

struct TradeEngine::Impl {
    mutable std::mutex mutex;
    fs::path project_root{"."};
    mutable bll::ContractRules contract_rules;
    mutable bool contract_rules_loaded{false};
    OrderListener order_listener;
    TradeListener trade_listener;
    std::function<void(const std::string&, int)> disconnect_callback;

    void ensure_contract_rules_loaded() const {
        if (contract_rules_loaded) {
            return;
        }
        const auto rules_path = project_root / "config" / "Contract_Rules.json";
        contract_rules.load(rules_path.string());
        contract_rules_loaded = true;
    }

    std::optional<bll::ParsedInstrument> parse_instrument(const std::string& instrument_id) const {
        ensure_contract_rules_loaded();
        return contract_rules.parse(instrument_id);
    }
#ifdef QUANT_SEV_HAS_CTP
    std::unordered_map<std::string, std::unique_ptr<TdSession>> sessions;

    void bind_order_notify(TdSession& session) {
        session.order_notify = [this](const nlohmann::json& payload) {
            OrderListener listener;
            {
                std::lock_guard<std::mutex> lock(mutex);
                listener = order_listener;
            }
            if (listener) {
                try {
                    listener(payload);
                } catch (const std::exception& ex) {
                    Logger::instance().error(std::string("order_listener 异常: ") + ex.what());
                } catch (...) {
                    Logger::instance().error("order_listener 未知异常");
                }
            }
        };
    }

    void bind_trade_notify(TdSession& session) {
        session.trade_notify = [this](const nlohmann::json& payload) {
            TradeListener listener;
            {
                std::lock_guard<std::mutex> lock(mutex);
                listener = trade_listener;
            }
            if (listener) {
                try {
                    listener(payload);
                } catch (const std::exception& ex) {
                    Logger::instance().error(std::string("trade_listener 异常: ") + ex.what());
                } catch (...) {
                    Logger::instance().error("trade_listener 未知异常");
                }
            }
        };
    }
#else
    bool stub_warned{false};
#endif
};

TradeEngine::TradeEngine() : impl_(std::make_unique<Impl>()) {}
TradeEngine::~TradeEngine() = default;

void TradeEngine::set_project_root(const fs::path& root) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->project_root = root;
}

ConnectResult TradeEngine::connect(const AccountRecord& account) {
    if (account.td_front.empty()) {
        return {false, "td_front / trader_front 为空"};
    }
    if (account.user_id.empty() || account.broker_id.empty()) {
        return {false, "broker_id / user_id 不完整"};
    }
    if (account.app_id.empty() || account.auth_code.empty()) {
        return {false, "app_id / auth_code 不完整"};
    }

#ifndef QUANT_SEV_HAS_CTP
    if (!impl_->stub_warned) {
        Logger::instance().warn("CTP 未链接，交易连接不可用（CMake 开启 QUANT_SEV_ENABLE_CTP）");
        impl_->stub_warned = true;
    }
    return {false, "CTP 未启用：请配置 CTP SDK 库并开启 QUANT_SEV_ENABLE_CTP 重新编译"};
#else
    std::unique_lock<std::mutex> lock(impl_->mutex);
    const std::string key = td_session_key(account);
    auto& slot = impl_->sessions[key];
    if (slot && slot->logged_in) {
        return {true, "交易已连接: " + account.user_id + " @ " + account.td_front};
    }

    if (!slot) {
        slot = std::make_unique<TdSession>();
        impl_->bind_order_notify(*slot);
        impl_->bind_trade_notify(*slot);
    }
    slot->account = account;
    slot->disconnect_callback = impl_->disconnect_callback;

    if (slot->api == nullptr) {
        const fs::path flow_dir =
            impl_->project_root / "data" / "flow" /
            ("td_" + account.user_id + "_" + std::to_string(std::hash<std::string>{}(account.td_front)));
        fs::create_directories(flow_dir);
        slot->spi = new TdSpiImpl(slot.get());
        slot->api = CThostFtdcTraderApi::CreateFtdcTraderApi(flow_dir.string().c_str(), true);
        slot->api->RegisterSpi(slot->spi);
        slot->api->SubscribePublicTopic(THOST_TERT_QUICK);
        slot->api->SubscribePrivateTopic(THOST_TERT_QUICK);
        char front[256]{};
        copy_to_field(front, account.td_front);
        slot->api->RegisterFront(front);
        slot->api->Init();
        Logger::instance().info("Td Init: " + account.user_id + " @ " + account.td_front);
    }

    lock.unlock();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    {
        std::unique_lock<std::mutex> wait_lock(slot->mutex);
        slot->cv.wait_until(wait_lock, deadline, [&]() { return slot->logged_in.load() || !slot->last_error.empty(); });
    }

    if (slot->logged_in) {
        return {true, "Td 登录成功"};
    }
    if (!slot->last_error.empty()) {
        return {false, slot->last_error};
    }
    return {false, "Td 登录超时"};
#endif
}

ConnectResult TradeEngine::disconnect(const AccountRecord& account) {
    if (account.user_id.empty() || account.td_front.empty()) {
        return {false, "user_id / td_front 为空"};
    }
#ifndef QUANT_SEV_HAS_CTP
    return {false, "CTP 未启用"};
#else
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const std::string key = td_session_key(account);
    auto it = impl_->sessions.find(key);
    if (it == impl_->sessions.end()) {
        return {true, "交易未连接"};
    }
    destroy_session(*it->second);
    impl_->sessions.erase(it);
    Logger::instance().info("Td 已断开: " + account.user_id + " @ " + account.td_front);
    return {true, "Td 已断开"};
#endif
}

bool TradeEngine::is_ready() const {
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

bool TradeEngine::is_user_ready(const std::string& user_id) const {
#ifndef QUANT_SEV_HAS_CTP
    (void)user_id;
    return false;
#else
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const auto& [_, session] : impl_->sessions) {
        if (session->account.user_id == user_id && session->logged_in.load()) {
            return true;
        }
    }
    return false;
#endif
}

bool TradeEngine::is_account_ready(const AccountRecord& account) const {
#ifndef QUANT_SEV_HAS_CTP
    (void)account;
    return false;
#else
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->sessions.find(td_session_key(account));
    return it != impl_->sessions.end() && it->second->logged_in.load();
#endif
}

nlohmann::json TradeEngine::td_sessions_status() const {
#ifndef QUANT_SEV_HAS_CTP
    return {{"users", nlohmann::json::array()}, {"sessions", nlohmann::json::array()}};
#else
    nlohmann::json users = nlohmann::json::array();
    nlohmann::json sessions = nlohmann::json::array();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const auto& [_, session] : impl_->sessions) {
        if (!session->logged_in.load()) {
            continue;
        }
        users.push_back(session->account.user_id);
        sessions.push_back({{"user_id", session->account.user_id}, {"td_front", session->account.td_front}});
    }
    return {{"users", users}, {"sessions", sessions}};
#endif
}

OrderResult TradeEngine::insert_order(const AccountRecord& account, const OrderRequest& request) {
    OrderResult result;
    if (request.instrument_id.empty()) {
        result.message = "instrument_id 为空";
        return result;
    }
    if (request.volume <= 0) {
        result.message = "volume 必须大于 0";
        return result;
    }

#ifndef QUANT_SEV_HAS_CTP
    result.message = "CTP 未启用";
    return result;
#else
    if (!is_account_ready(account)) {
        const auto connected = connect(account);
        if (!connected.ok) {
            result.message = connected.message;
            return result;
        }
    }

    std::unique_lock<std::mutex> lock(impl_->mutex);
    const auto it = impl_->sessions.find(td_session_key(account));
    if (it == impl_->sessions.end() || !it->second->logged_in || it->second->api == nullptr) {
        result.message = "交易未登录: " + account.user_id;
        return result;
    }

    auto& session = *it->second;
    const int request_id = session.request_id.fetch_add(1);
    const int order_ref_num = session.order_ref_seq++;
    std::ostringstream order_ref_stream;
    order_ref_stream << order_ref_num;
    const std::string order_ref = order_ref_stream.str();

    session.pending_order_request_id = request_id;
    session.pending_order_result = OrderResult{};
    session.pending_order_result.order_ref = order_ref;
    session.order_response_ready = false;

    std::string ctp_instrument_id = request.instrument_id;
    std::string exchange_id;
    if (const auto parsed = impl_->parse_instrument(request.instrument_id)) {
        exchange_id = parsed->exchange;
        ctp_instrument_id = parsed->product + parsed->year_suffix + parsed->delivery_month;
    }

    CThostFtdcInputOrderField req{};
    copy_to_field(req.BrokerID, account.broker_id);
    copy_to_field(req.InvestorID, account.user_id);
    copy_to_field(req.UserID, account.user_id);
    copy_to_field(req.InstrumentID, ctp_instrument_id);
    if (!exchange_id.empty()) {
        copy_to_field(req.ExchangeID, exchange_id);
    }
    copy_to_field(req.OrderRef, order_ref);
    req.OrderPriceType = map_price_type(request.price_type);
    req.Direction = map_direction(request.direction);
    req.CombOffsetFlag[0] = map_offset_flag(request.offset);
    req.CombHedgeFlag[0] = THOST_FTDC_HF_Speculation;
    req.LimitPrice = request.price;
    req.VolumeTotalOriginal = request.volume;
    req.TimeCondition = THOST_FTDC_TC_GFD;
    req.VolumeCondition = THOST_FTDC_VC_AV;
    req.ContingentCondition = THOST_FTDC_CC_Immediately;
    req.ForceCloseReason = THOST_FTDC_FCC_NotForceClose;
    req.IsAutoSuspend = 0;
    req.UserForceClose = 0;

    const int rc = session.api->ReqOrderInsert(&req, request_id);
    if (rc != 0) {
        session.pending_order_request_id = 0;
        result.message = "ReqOrderInsert 返回 " + std::to_string(rc);
        return result;
    }

    lock.unlock();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    {
        std::unique_lock<std::mutex> wait_lock(session.mutex);
        session.cv.wait_until(wait_lock, deadline, [&]() { return session.order_response_ready; });
    }

    std::lock_guard<std::mutex> result_lock(impl_->mutex);
    result = session.pending_order_result;
    session.pending_order_request_id = 0;
    if (!session.order_response_ready) {
        result.ok = false;
        result.message = "报单响应超时";
        result.order_ref = order_ref;
    }
    return result;
#endif
}

void TradeEngine::set_order_listener(OrderListener listener) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->order_listener = std::move(listener);
#ifdef QUANT_SEV_HAS_CTP
    for (auto& [_, session] : impl_->sessions) {
        impl_->bind_order_notify(*session);
    }
#endif
}

void TradeEngine::set_trade_listener(TradeListener listener) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->trade_listener = std::move(listener);
#ifdef QUANT_SEV_HAS_CTP
    for (auto& [_, session] : impl_->sessions) {
        impl_->bind_trade_notify(*session);
    }
#endif
}

void TradeEngine::set_disconnect_callback(std::function<void(const std::string&, int)> callback) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->disconnect_callback = std::move(callback);
#ifdef QUANT_SEV_HAS_CTP
    for (auto& [_, session] : impl_->sessions) {
        session->disconnect_callback = impl_->disconnect_callback;
    }
#endif
}

nlohmann::json TradeEngine::order_updates(const nlohmann::json& query) const {
    const std::string user_id = query.value("user_id", "");
    const int limit = json_int_param(query, "limit", 100);
    nlohmann::json rows = nlohmann::json::array();

#ifndef QUANT_SEV_HAS_CTP
    return {{"orders", rows}};
#else
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const auto& [_, session] : impl_->sessions) {
        if (!user_id.empty() && session->account.user_id != user_id) {
            continue;
        }
        for (auto it = session->order_updates.rbegin(); it != session->order_updates.rend(); ++it) {
            rows.push_back(order_update_to_json(*it));
            if (static_cast<int>(rows.size()) >= limit) {
                return {{"orders", rows}};
            }
        }
    }
    return {{"orders", rows}};
#endif
}

nlohmann::json TradeEngine::trade_updates(const nlohmann::json& query) const {
    const std::string user_id = query.value("user_id", "");
    const int limit = json_int_param(query, "limit", 100);
    nlohmann::json rows = nlohmann::json::array();

#ifndef QUANT_SEV_HAS_CTP
    return {{"trades", rows}};
#else
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const auto& [_, session] : impl_->sessions) {
        if (!user_id.empty() && session->account.user_id != user_id) {
            continue;
        }
        for (auto it = session->trade_updates.rbegin(); it != session->trade_updates.rend(); ++it) {
            rows.push_back(trade_update_to_json(*it));
            if (static_cast<int>(rows.size()) >= limit) {
                return {{"trades", rows}};
            }
        }
    }
    return {{"trades", rows}};
#endif
}

#ifdef QUANT_SEV_HAS_CTP
namespace {

bool wait_history_query(TdSession& session, int request_id) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    std::unique_lock<std::mutex> lock(session.mutex);
    return session.cv.wait_until(lock, deadline, [&]() {
        return session.pending_history.request_id == request_id && session.pending_history.ready;
    });
}

bool wait_account_query(TdSession& session, int request_id) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    std::unique_lock<std::mutex> lock(session.mutex);
    return session.cv.wait_until(lock, deadline, [&]() {
        return session.pending_account.request_id == request_id && session.pending_account.ready;
    });
}

bool wait_position_query(TdSession& session, int request_id) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    std::unique_lock<std::mutex> lock(session.mutex);
    return session.cv.wait_until(lock, deadline, [&]() {
        return session.pending_positions.request_id == request_id && session.pending_positions.ready;
    });
}

nlohmann::json investor_positions_to_json(const std::vector<InvestorPositionSnapshot>& positions) {
    nlohmann::json rows = nlohmann::json::array();
    int total_long = 0;
    int total_short = 0;
    std::unordered_set<std::string> contract_ids;
    for (const auto& pos : positions) {
        if (pos.long_volume == 0 && pos.short_volume == 0) {
            continue;
        }
        contract_ids.insert(pos.instrument_id);
        total_long += pos.long_volume;
        total_short += pos.short_volume;
        if (pos.long_volume > 0) {
            rows.push_back({{"instrument_id", pos.instrument_id},
                            {"exchange_id", pos.exchange_id},
                            {"direction", "long"},
                            {"volume", pos.long_volume},
                            {"long_today", pos.long_today},
                            {"long_yd", pos.long_yd},
                            {"open_price", pos.avg_long_price},
                            {"position_profit", pos.long_profit}});
        }
        if (pos.short_volume > 0) {
            rows.push_back({{"instrument_id", pos.instrument_id},
                            {"exchange_id", pos.exchange_id},
                            {"direction", "short"},
                            {"volume", pos.short_volume},
                            {"short_today", pos.short_today},
                            {"short_yd", pos.short_yd},
                            {"open_price", pos.avg_short_price},
                            {"position_profit", pos.short_profit}});
        }
    }
    return {{"positions", rows},
            {"summary",
             {{"total_contracts", static_cast<int>(contract_ids.size())},
              {"total_long", total_long},
              {"total_short", total_short}}}};
}

}  // namespace
#endif

nlohmann::json TradeEngine::query_investor_positions(const AccountRecord* account, const nlohmann::json& query) {
    const std::string user_id = query.value("user_id", account != nullptr ? account->user_id : "");
    const bool refresh = json_bool_param(query, "refresh", false);

#ifndef QUANT_SEV_HAS_CTP
    (void)refresh;
    (void)account;
    return {{"source", "cache"}, {"cached", false}, {"positions", nlohmann::json::array()},
            {"summary", {{"total_contracts", 0}, {"total_long", 0}, {"total_short", 0}}}};
#else
    if (refresh) {
        if (account == nullptr || user_id.empty()) {
            return {{"error", "refresh 需要有效 user_id 与已连接账户"}};
        }
        if (!is_account_ready(*account)) {
            const auto connected = connect(*account);
            if (!connected.ok) {
                return {{"error", connected.message}};
            }
        }

        std::unique_lock<std::mutex> lock(impl_->mutex);
        const auto it = impl_->sessions.find(td_session_key(*account));
        if (it == impl_->sessions.end() || !it->second->logged_in || it->second->api == nullptr) {
            return {{"error", "交易未登录: " + account->user_id}};
        }

        auto& session = *it->second;
        const int request_id = session.request_id.fetch_add(1);
        {
            std::lock_guard<std::mutex> session_lock(session.mutex);
            session.pending_positions = PositionQueryState{};
            session.pending_positions.request_id = request_id;
        }

        CThostFtdcQryInvestorPositionField req{};
        copy_to_field(req.BrokerID, account->broker_id);
        copy_to_field(req.InvestorID, account->user_id);
        const int rc = session.api->ReqQryInvestorPosition(&req, request_id);
        if (rc != 0) {
            return {{"error", "ReqQryInvestorPosition 返回 " + std::to_string(rc)}};
        }

        lock.unlock();
        if (!wait_position_query(session, request_id)) {
            return {{"error", "CTP 持仓查询超时"}};
        }

        std::lock_guard<std::mutex> session_lock(session.mutex);
        if (!session.pending_positions.error.empty()) {
            return {{"error", session.pending_positions.error}};
        }
        session.position_cache.clear();
        for (const auto& [_, snap] : session.pending_positions.by_instrument) {
            if (snap.long_volume > 0 || snap.short_volume > 0) {
                session.position_cache.push_back(snap);
            }
        }
        session.position_cached = true;
        session.pending_positions = PositionQueryState{};
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const auto& [_, session] : impl_->sessions) {
        if (!user_id.empty() && session->account.user_id != user_id) {
            continue;
        }
        if (!session->position_cached) {
            continue;
        }
        auto body = investor_positions_to_json(session->position_cache);
        body["source"] = refresh ? "ctp" : "cache";
        body["cached"] = true;
        return body;
    }

    return {{"source", "cache"},
            {"cached", false},
            {"positions", nlohmann::json::array()},
            {"summary", {{"total_contracts", 0}, {"total_long", 0}, {"total_short", 0}}}};
#endif
}

nlohmann::json TradeEngine::query_history(const AccountRecord* account, const nlohmann::json& query) {
    const std::string user_id = query.value("user_id", account != nullptr ? account->user_id : "");
    const std::string type = query.value("type", "trades");
    const std::string instrument_id = query.value("instrument_id", "");
    const bool refresh = json_bool_param(query, "refresh", false);
    const int limit = json_int_param(query, "limit", 500);

#ifndef QUANT_SEV_HAS_CTP
    (void)refresh;
    (void)account;
    (void)user_id;
    (void)instrument_id;
    (void)limit;
    return {{"type", type},
            {"source", "cache"},
            {"rows", nlohmann::json::array()},
            {"count", 0}};
#else
    const std::string date_from = normalize_yyyymmdd(query.value("date_from", ""));
    const std::string date_to = normalize_yyyymmdd(query.value("date_to", ""));

    nlohmann::json rows = nlohmann::json::array();
    std::string source = "cache";
    std::string ctp_message;
    if (refresh) {
        if (account == nullptr || user_id.empty()) {
            return {{"error", "refresh 需要有效 user_id 与已连接账户"}};
        }
        if (!is_account_ready(*account)) {
            const auto connected = connect(*account);
            if (!connected.ok) {
                return {{"error", connected.message}};
            }
        }

        std::unique_lock<std::mutex> lock(impl_->mutex);
        const auto it = impl_->sessions.find(td_session_key(*account));
        if (it == impl_->sessions.end() || !it->second->logged_in || it->second->api == nullptr) {
            return {{"error", "交易未登录: " + account->user_id}};
        }

        auto& session = *it->second;
        const int request_id = session.request_id.fetch_add(1);
        {
            std::lock_guard<std::mutex> session_lock(session.mutex);
            session.pending_history = HistoryQueryState{};
            session.pending_history.request_id = request_id;
            session.pending_history.is_orders = type == "orders";
        }

        if (type == "orders") {
            CThostFtdcQryOrderField req{};
            copy_to_field(req.BrokerID, account->broker_id);
            copy_to_field(req.InvestorID, account->user_id);
            if (!instrument_id.empty()) {
                copy_to_field(req.InstrumentID, instrument_id);
            }
            const int rc = session.api->ReqQryOrder(&req, request_id);
            if (rc != 0) {
                return {{"error", "ReqQryOrder 返回 " + std::to_string(rc)}};
            }
        } else {
            CThostFtdcQryTradeField req{};
            copy_to_field(req.BrokerID, account->broker_id);
            copy_to_field(req.InvestorID, account->user_id);
            if (!instrument_id.empty()) {
                copy_to_field(req.InstrumentID, instrument_id);
            }
            const int rc = session.api->ReqQryTrade(&req, request_id);
            if (rc != 0) {
                return {{"error", "ReqQryTrade 返回 " + std::to_string(rc)}};
            }
        }

        lock.unlock();
        if (!wait_history_query(session, request_id)) {
            return {{"error", "CTP 历史查询超时"}};
        }

        std::lock_guard<std::mutex> session_lock(session.mutex);
        if (!session.pending_history.error.empty()) {
            ctp_message = session.pending_history.error;
        }
        if (type == "orders") {
            for (const auto& record : session.pending_history.orders) {
                merge_order_record(session.order_updates, record);
            }
        } else {
            for (const auto& record : session.pending_history.trades) {
                merge_trade_record(session.trade_updates, record);
            }
        }
        source = "ctp";
        session.pending_history = HistoryQueryState{};
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const auto& [_, session] : impl_->sessions) {
        if (!user_id.empty() && session->account.user_id != user_id) {
            continue;
        }

        if (type == "orders") {
            for (auto it = session->order_updates.rbegin(); it != session->order_updates.rend(); ++it) {
                const std::string row_date = normalize_yyyymmdd(it->insert_date);
                if (!date_in_range(row_date, date_from, date_to)) {
                    continue;
                }
                if (!instrument_id.empty() && it->instrument_id != instrument_id) {
                    continue;
                }
                const std::string display_date = row_date.size() == 8
                                                     ? row_date.substr(0, 4) + "-" + row_date.substr(4, 2) + "-" +
                                                           row_date.substr(6, 2)
                                                     : row_date;
                rows.push_back({{"date", display_date},
                                {"time", it->insert_time},
                                {"instrument_id", it->instrument_id},
                                {"direction", it->direction},
                                {"offset", it->offset},
                                {"price", it->limit_price},
                                {"volume", it->volume_total},
                                {"volume_traded", it->volume_traded},
                                {"status", it->status},
                                {"order_ref", it->order_ref},
                                {"order_sys_id", it->order_sys_id}});
                if (static_cast<int>(rows.size()) >= limit) {
                    break;
                }
            }
        } else {
            for (auto it = session->trade_updates.rbegin(); it != session->trade_updates.rend(); ++it) {
                const std::string row_date = normalize_yyyymmdd(it->trade_date);
                if (!date_in_range(row_date, date_from, date_to)) {
                    continue;
                }
                if (!instrument_id.empty() && it->instrument_id != instrument_id) {
                    continue;
                }
                const std::string display_date = row_date.size() == 8
                                                     ? row_date.substr(0, 4) + "-" + row_date.substr(4, 2) + "-" +
                                                           row_date.substr(6, 2)
                                                     : row_date;
                rows.push_back({{"date", display_date},
                                {"time", it->trade_time},
                                {"instrument_id", it->instrument_id},
                                {"direction", it->direction},
                                {"offset", it->offset},
                                {"price", it->price},
                                {"volume", it->volume},
                                {"turnover", it->price * static_cast<double>(it->volume)},
                                {"status", "filled"},
                                {"trade_id", it->trade_id},
                                {"order_ref", it->order_ref}});
                if (static_cast<int>(rows.size()) >= limit) {
                    break;
                }
            }
        }
    }

    nlohmann::json body = {{"type", type}, {"source", source}, {"rows", rows}, {"count", rows.size()}};
    if (!ctp_message.empty()) {
        body["ctp_message"] = ctp_message;
    }
    return body;
#endif
}

nlohmann::json TradeEngine::query_trading_account(const AccountRecord* account, const nlohmann::json& query) {
    const std::string user_id = query.value("user_id", account != nullptr ? account->user_id : "");
    const bool refresh = json_bool_param(query, "refresh", false);

#ifndef QUANT_SEV_HAS_CTP
    (void)refresh;
    (void)account;
    return {{"source", "cache"}, {"account", nlohmann::json::object()}, {"cached", false}};
#else
    if (refresh) {
        if (account == nullptr || user_id.empty()) {
            return {{"error", "refresh 需要有效 user_id 与已连接账户"}};
        }
        if (!is_account_ready(*account)) {
            const auto connected = connect(*account);
            if (!connected.ok) {
                return {{"error", connected.message}};
            }
        }

        std::unique_lock<std::mutex> lock(impl_->mutex);
        const auto it = impl_->sessions.find(td_session_key(*account));
        if (it == impl_->sessions.end() || !it->second->logged_in || it->second->api == nullptr) {
            return {{"error", "交易未登录: " + account->user_id}};
        }

        auto& session = *it->second;
        const int request_id = session.request_id.fetch_add(1);
        {
            std::lock_guard<std::mutex> session_lock(session.mutex);
            session.pending_account = AccountQueryState{};
            session.pending_account.request_id = request_id;
        }

        CThostFtdcQryTradingAccountField req{};
        copy_to_field(req.BrokerID, account->broker_id);
        copy_to_field(req.InvestorID, account->user_id);
        copy_to_field(req.CurrencyID, "CNY");
        const int rc = session.api->ReqQryTradingAccount(&req, request_id);
        if (rc != 0) {
            return {{"error", "ReqQryTradingAccount 返回 " + std::to_string(rc)}};
        }

        lock.unlock();
        if (!wait_account_query(session, request_id)) {
            return {{"error", "CTP 资金查询超时"}};
        }

        std::lock_guard<std::mutex> session_lock(session.mutex);
        if (!session.pending_account.error.empty()) {
            return {{"error", session.pending_account.error}};
        }
        if (session.pending_account.has_account) {
            session.trading_account_cache = session.pending_account.account;
            session.trading_account_cached = true;
        }
        session.pending_account = AccountQueryState{};
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (account != nullptr) {
        const auto it = impl_->sessions.find(td_session_key(*account));
        if (it != impl_->sessions.end() && it->second->trading_account_cached) {
            return {{"source", refresh ? "ctp" : "cache"},
                    {"cached", true},
                    {"account", trading_account_to_json(it->second->trading_account_cache)}};
        }
    }
    for (const auto& [_, session] : impl_->sessions) {
        if (!user_id.empty() && session->account.user_id != user_id) {
            continue;
        }
        if (!session->trading_account_cached) {
            continue;
        }
        return {{"source", refresh ? "ctp" : "cache"},
                {"cached", true},
                {"account", trading_account_to_json(session->trading_account_cache)}};
    }

    return {{"source", "cache"}, {"cached", false}, {"account", nlohmann::json::object()}};
#endif
}

std::optional<TradingAccountSnapshot> TradeEngine::cached_trading_account(const std::string& user_id) const {
#ifndef QUANT_SEV_HAS_CTP
    (void)user_id;
    return std::nullopt;
#else
    if (user_id.empty()) {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const auto& [_, session] : impl_->sessions) {
        if (session->account.user_id != user_id) {
            continue;
        }
        if (!session->trading_account_cached) {
            continue;
        }
        return session->trading_account_cache;
    }
    return std::nullopt;
#endif
}

CancelResult TradeEngine::cancel_order(const AccountRecord& account, const CancelOrderRequest& request) {
    CancelResult result;
    if (request.order_ref.empty() && request.order_sys_id.empty()) {
        result.message = "order_ref 或 order_sys_id 至少填一项";
        return result;
    }
    if (request.order_ref.empty() && request.instrument_id.empty()) {
        result.message = "按 order_sys_id 撤单时 instrument_id 必填";
        return result;
    }

#ifndef QUANT_SEV_HAS_CTP
    result.message = "CTP 未启用";
    return result;
#else
    if (!is_account_ready(account)) {
        const auto connected = connect(account);
        if (!connected.ok) {
            result.message = connected.message;
            return result;
        }
    }

    std::unique_lock<std::mutex> lock(impl_->mutex);
    const auto it = impl_->sessions.find(td_session_key(account));
    if (it == impl_->sessions.end() || !it->second->logged_in || it->second->api == nullptr) {
        result.message = "交易未登录: " + account.user_id;
        return result;
    }

    auto& session = *it->second;
    const int request_id = session.request_id.fetch_add(1);
    const std::string order_ref = request.order_ref;

    session.pending_action_request_id = request_id;
    session.pending_action_result = CancelResult{};
    session.pending_action_result.order_ref = order_ref;
    session.action_response_ready = false;

    CThostFtdcInputOrderActionField action{};
    copy_to_field(action.BrokerID, account.broker_id);
    copy_to_field(action.InvestorID, account.user_id);
    copy_to_field(action.UserID, account.user_id);
    action.FrontID = session.front_id;
    action.SessionID = session.session_id;
    action.ActionFlag = THOST_FTDC_AF_Delete;
    if (!request.order_sys_id.empty()) {
        copy_to_field(action.OrderSysID, request.order_sys_id);
    }
    if (!order_ref.empty()) {
        copy_to_field(action.OrderRef, order_ref);
    }
    if (!request.instrument_id.empty()) {
        copy_to_field(action.InstrumentID, request.instrument_id);
    }
    if (!request.exchange_id.empty()) {
        copy_to_field(action.ExchangeID, request.exchange_id);
    }

    const int rc = session.api->ReqOrderAction(&action, request_id);
    if (rc != 0) {
        session.pending_action_request_id = 0;
        result.message = "ReqOrderAction 返回 " + std::to_string(rc);
        return result;
    }

    lock.unlock();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    {
        std::unique_lock<std::mutex> wait_lock(session.mutex);
        session.cv.wait_until(wait_lock, deadline, [&]() { return session.action_response_ready; });
    }

    std::lock_guard<std::mutex> result_lock(impl_->mutex);
    result = session.pending_action_result;
    session.pending_action_request_id = 0;
    if (!session.action_response_ready) {
        result.ok = false;
        result.message = "撤单响应超时";
        result.order_ref = order_ref;
    }
    return result;
#endif
}

}  // namespace quant_sev::core
