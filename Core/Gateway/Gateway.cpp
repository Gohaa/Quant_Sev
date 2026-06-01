#include "Gateway/Gateway.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <sstream>
#include <thread>

#include "Common/ContractRules.hpp"
#include "Backtest/BacktestEngine.hpp"
#include "Backtest/BacktestTypes.hpp"
#include "Bar/BarEngine.hpp"
#include "Common/JsonQuery.hpp"
#include "OrderBook/OrderBookEngine.hpp"
#include "Rollover/RolloverEngine.hpp"
#include "CTA/CtaEngine.hpp"
#include "Indicator/IndicatorEngine.hpp"
#include "Logger/Logger.hpp"
#include "Storage/StorageEngine.hpp"
#include "Storage/StorageTypes.hpp"
#include "Indicator/IndicatorTypes.hpp"
#include "CTA/CtaTypes.hpp"
#include "Strategy/StrategyEngine.hpp"
#include "Trade/OrderTypes.hpp"

#ifdef QUANT_SEV_HAS_CTP
#include "ThostFtdcMdApi.h"
#include "ThostFtdcTraderApi.h"
#endif

namespace quant_sev::core {

namespace {

std::string to_upper_ascii(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return text;
}

bool is_valid_web_offset(const std::string& offset) {
    return offset == "open" || offset == "close" || offset.empty();
}

}  // namespace

Gateway::Gateway(Config& config)
    : config_(config),
      accounts_(config),
      symbols_(config, accounts_, quote_),
      storage_(std::make_unique<bll::StorageEngine>()),
      bar_(std::make_unique<bll::BarEngine>(*storage_)),
      orderbook_(std::make_unique<bll::OrderBookEngine>()),
      rollover_(std::make_unique<bll::RolloverEngine>()),
      indicator_(std::make_unique<bll::IndicatorEngine>(*storage_)),
      strategy_(std::make_unique<bll::StrategyEngine>(*storage_)),
      backtest_(std::make_unique<bll::BacktestEngine>(*storage_)) {}

Gateway::~Gateway() = default;

bool Gateway::initialize() {
    status_.version = config_.app().gateway_version;
    status_.md_ok = false;
    status_.td_ok = false;
    quote_.set_project_root(config_.root_dir());
    trade_.set_project_root(config_.root_dir());
    storage_->set_project_root(config_.root_dir());
    if (!storage_->initialize()) {
        Logger::instance().warn("Storage 初始化部分失败，历史查询可能不可用");
    }
    if (!backtest_->initialize()) {
        Logger::instance().warn("Backtest 初始化部分失败，Option_Rules 可能不可用");
    }
    if (!rollover_->initialize(config_.root_dir())) {
        Logger::instance().warn("Rollover 初始化部分失败");
    }
    if (!cta_.initialize(config_)) {
        Logger::instance().warn("CTA 初始化部分失败");
    }
    if (!time_check_.load(config_)) {
        Logger::instance().warn("TimeCheck 加载失败");
    }
    if (!risk_.load(config_)) {
        Logger::instance().warn("Risk 加载失败");
    }
    const auto rules_path = config_.root_dir() / "config" / "Contract_Rules.json";
    if (!contract_rules_.load(rules_path.string())) {
        Logger::instance().warn("ContractRules 加载失败，Web 平仓将默认映射为 close");
    }
    cta_.set_order_executor([this](const AccountRecord& account, const OrderRequest& order) {
        return execute_order(account, order, false);
    });
    cta_.set_strategy_gate([this]() { return risk_.halt_all_strategies(); });
    trade_.set_order_listener([this](const nlohmann::json& update) {
        on_order_update_rtt(update);
        cta_.apply_order_update(update);
        if (order_listener_) {
            order_listener_(update);
        }
    });
    trade_.set_trade_listener([this](const nlohmann::json& update) {
        cta_.apply_trade_update(update);
        if (trade_listener_) {
            trade_listener_(update);
        }
    });
    strategy_->set_running_strategies_provider([this]() { return cta_.list_strategies(); });
    strategy_->set_signal_handler([this](const nlohmann::json& payload) {
        const auto account = resolve_account(payload);
        if (!account) {
            Logger::instance().warn("Strategy 信号无有效账户");
            return;
        }
        core::TradingSignal signal;
        signal.strategy_id = payload.value("strategy_id", "");
        signal.user_id = account->user_id;
        signal.instrument_id = payload.value("instrument_id", "");
        signal.price = payload.value("price", 0.0);
        signal.volume = payload.value("volume", 0);
        const std::string action = payload.value("action", "");
        if (action == "buy" || action == "close_short") {
            signal.direction = "buy";
        } else {
            signal.direction = "sell";
        }
        signal.offset = action.find("close") != std::string::npos ? "close" : "open";
        signal.price_type = "limit";
        cta_.submit_signal(*account, signal);
    });
    bar_->set_bar_closed_callback([this](const std::string& instrument_id, const bll::BarRecord& bar,
                                         const std::string& period) {
        strategy_->on_bar_closed(instrument_id, bar, period);
    });
#ifdef QUANT_SEV_HAS_CTP
    Logger::instance().info(std::string("CTP MdApi: ") + CThostFtdcMdApi::GetApiVersion());
    Logger::instance().info(std::string("CTP TdApi: ") + CThostFtdcTraderApi::GetApiVersion());
#endif
    quote_.set_tick_callback([this](const nlohmann::json& tick) { on_market_tick(tick); });
    quote_.set_disconnect_callback([this](const std::string& md_front, int reason) {
        on_md_disconnected(md_front, reason);
    });
    trade_.set_disconnect_callback([this](const std::string& user_id, int reason) {
        on_td_disconnected(user_id, reason);
    });
    Logger::instance().info("Gateway 初始化完成 (Storage + Bar + Strategy + Backtest)");
    return true;
}

void Gateway::on_market_tick(const nlohmann::json& tick) {
    update_rollover_tick_state(tick);
    orderbook_->on_tick(tick);
    bar_->on_tick(tick);
    if (tick_listener_) {
        tick_listener_(tick);
    }
}

nlohmann::json Gateway::rollover_quote_map() const {
    nlohmann::json map = nlohmann::json::object();
    {
        std::lock_guard<std::mutex> lock(rollover_mutex_);
        for (const auto& [key, value] : rollover_quote_cache_.items()) {
            map[key] = value;
        }
    }
    for (const auto& row : quote_.quote_board()) {
        if (row.contains("instrument_id")) {
            map[row["instrument_id"].get<std::string>()] = row;
        }
    }
    return map;
}

void Gateway::update_rollover_tick_state(const nlohmann::json& tick) {
    const std::string instrument_id = tick.value("instrument_id", "");
    const std::string trading_day = tick.value("trading_day", "");
    if (instrument_id.empty()) {
        return;
    }

    std::string previous_day;
    {
        std::lock_guard<std::mutex> lock(rollover_mutex_);
        previous_day = rollover_last_trading_day_;
        rollover_quote_cache_[instrument_id] = tick;
        if (!trading_day.empty()) {
            rollover_last_trading_day_ = trading_day;
        }
    }

    if (!previous_day.empty() && !trading_day.empty() && trading_day != previous_day) {
        maybe_record_rollover_snapshot(previous_day, "trading_day_change");
    }

    const bool in_session = time_check_.status().in_session;
    if (rollover_last_in_session_ && !in_session && !rollover_last_trading_day_.empty()) {
        maybe_record_rollover_snapshot(rollover_last_trading_day_, "session_end");
    }
    rollover_last_in_session_ = in_session;
}

void Gateway::maybe_record_rollover_snapshot(const std::string& trading_day, const char* reason) {
    if (trading_day.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(rollover_mutex_);
        if (rollover_last_auto_snapshot_day_ == trading_day) {
            return;
        }
        rollover_last_auto_snapshot_day_ = trading_day;
    }

    const auto symbol_doc = symbols_.list_symbols();
    const auto result =
        rollover_->record_daily_snapshot(config_.root_dir(), trading_day, symbol_doc, rollover_quote_map());
    if (result.value("ok", false)) {
        Logger::instance().info(std::string("换月日终快照(") + reason + "): day=" + trading_day + " written=" +
                                std::to_string(result.value("written", 0)));
    } else if (result.contains("error")) {
        Logger::instance().warn(std::string("换月日终快照失败(") + reason +
                                "): " + result["error"].get<std::string>());
    }
}

void Gateway::set_tick_listener(TickListener listener) {
    tick_listener_ = std::move(listener);
}

void Gateway::set_order_listener(OrderListener listener) {
    order_listener_ = std::move(listener);
}

void Gateway::set_trade_listener(TradeListener listener) {
    trade_listener_ = std::move(listener);
}

OrderResult Gateway::execute_order(const AccountRecord& account, const OrderRequest& order, bool manual_order,
                                   RiskCheckMode risk_mode) {
    OrderResult blocked;
    const auto position = cta_.position_for(account.user_id, order.instrument_id);
    OrderRequest trade_order = order;
    trade_order.offset =
        web_offset_to_trade_offset(order.offset, order.instrument_id, order.direction, position);

    if (risk_mode != RiskCheckMode::EmergencyClose) {
        const auto time_result = time_check_.check(manual_order);
        if (!time_result.ok) {
            blocked.message = time_result.message;
            risk_.record_rejection("session");
            return blocked;
        }
    }

    const nlohmann::json positions_view = cta_.positions_view({{"user_id", account.user_id}});
    const auto ctp_account = trade_.cached_trading_account(account.user_id);
    const TradingAccountSnapshot* ctp_ptr = ctp_account ? &*ctp_account : nullptr;
    const auto quote_snap = quote_snapshot_for(trade_order.instrument_id);
    const InstrumentQuoteSnapshot* quote_ptr = quote_snap ? &*quote_snap : nullptr;
    const auto risk_result = risk_.check_new_order(account, trade_order, position ? &*position : nullptr, positions_view,
                                                   ctp_ptr, risk_mode, quote_ptr);
    if (!risk_result.ok) {
        blocked.message = risk_result.message;
        risk_.record_rejection(risk_result.code.empty() ? "limit" : risk_result.code);
        Logger::instance().warn("Risk 拒绝报单: " + blocked.message);
        return blocked;
    }

    const OrderResult result = trade_.insert_order(account, trade_order);
    if (result.ok) {
        risk_.on_order_accepted(account.user_id, &trade_order);
        if (!result.order_ref.empty()) {
            track_order_send_time(account.user_id, result.order_ref);
        }
    }
    return result;
}

std::string Gateway::web_offset_to_trade_offset(const std::string& web_offset,
                                                const std::string& instrument_id,
                                                const std::string& direction,
                                                const std::optional<CtaPositionView>& position) const {
    if (web_offset == "open" || web_offset.empty()) {
        return "open";
    }
    if (web_offset == "close_today" || web_offset == "close-today") {
        return "close_today";
    }
    if (web_offset != "close") {
        return web_offset;
    }
    if (const auto parsed = contract_rules_.parse(instrument_id)) {
        if (parsed->exchange == "SHFE" || parsed->exchange == "INE") {
            const bool closing_long = direction == "sell";
            const int today_vol =
                closing_long ? (position ? position->long_today : 0) : (position ? position->short_today : 0);
            const int yd_vol =
                closing_long ? (position ? position->long_yd : 0) : (position ? position->short_yd : 0);
            if (today_vol > 0 && yd_vol == 0) {
                return "close_today";
            }
            if (yd_vol > 0) {
                return "close";
            }
            if (today_vol > 0) {
                return "close_today";
            }
            return "close";
        }
    }
    return "close";
}

OrderResult Gateway::execute_emergency_close(const AccountRecord& account, const OrderRequest& order) {
    const auto position = cta_.position_for(account.user_id, order.instrument_id);
    const std::string trade_offset =
        web_offset_to_trade_offset(order.offset, order.instrument_id, order.direction, position);
    if (trade_offset != "close" && trade_offset != "close_today") {
        OrderResult blocked;
        blocked.message = "应急通道仅允许平仓";
        return blocked;
    }
    Logger::instance().warn("应急平仓: " + order.instrument_id + " " + order.direction + " vol=" +
                            std::to_string(order.volume));
    return execute_order(account, order, true, RiskCheckMode::EmergencyClose);
}

GatewayStatus Gateway::status() const {
    GatewayStatus current = status_;
    current.md_ok = quote_.is_ready();
    current.td_ok = trade_.is_ready();
    return current;
}

ApiResponse Gateway::handle(const std::string& method, const std::string& path, const std::string& body) {
    std::string normalized = path;
    while (normalized.size() > 1 && normalized.back() == '/') {
        normalized.pop_back();
    }
    if (method == "GET") {
        return handle_get(normalized, body);
    }
    if (method == "POST") {
        return handle_post(normalized, body);
    }
    return {405, {{"error", "method not allowed"}}};
}

nlohmann::json Gateway::md_quote_board() const {
    const auto symbol_doc = symbols_.list_symbols();
    const auto quotes = quote_.quote_board();
    nlohmann::json quote_map = nlohmann::json::object();
    for (const auto& row : quotes) {
        if (row.contains("instrument_id")) {
            const auto id = row["instrument_id"].get<std::string>();
            quote_map[id] = row;
            quote_map[to_upper_ascii(id)] = row;
        }
    }

    nlohmann::json board = nlohmann::json::array();
    if (symbol_doc.contains("symbols") && symbol_doc["symbols"].is_array()) {
        for (const auto& item : symbol_doc["symbols"]) {
            nlohmann::json row = item;
            const auto id = item.value("instrument_id", "");
            if (!id.empty()) {
                if (quote_map.contains(id)) {
                    row["quote"] = quote_map[id];
                } else {
                    const auto upper = to_upper_ascii(id);
                    if (quote_map.contains(upper)) {
                        row["quote"] = quote_map[upper];
                    }
                }
            }
            board.push_back(row);
        }
    }
    for (const auto& row : quotes) {
        const auto id = row.value("instrument_id", "");
        if (id.empty()) {
            continue;
        }
        bool listed = false;
        for (const auto& item : board) {
            if (item.value("instrument_id", "") == id) {
                listed = true;
                break;
            }
        }
        if (!listed) {
            board.push_back({{"instrument_id", id}, {"quote", row}});
        }
    }
    return board;
}

static std::vector<double> json_options_param(const nlohmann::json& query) {
    std::vector<double> options;
    if (!query.contains("options")) {
        return options;
    }
    const auto& value = query.at("options");
    if (value.is_array()) {
        for (const auto& item : value) {
            if (item.is_number()) {
                options.push_back(item.get<double>());
            } else if (item.is_string()) {
                try {
                    options.push_back(std::stod(item.get<std::string>()));
                } catch (...) {
                }
            }
        }
        return options;
    }
    if (value.is_string()) {
        const auto text = value.get<std::string>();
        std::stringstream ss(text);
        std::string token;
        while (std::getline(ss, token, ',')) {
            if (token.empty()) {
                continue;
            }
            try {
                options.push_back(std::stod(token));
            } catch (...) {
            }
        }
    }
    return options;
}

static nlohmann::json bars_to_json(const std::vector<bll::BarRecord>& bars) {
    nlohmann::json out = nlohmann::json::array();
    for (const auto& bar : bars) {
        out.push_back({{"date", bar.date},
                       {"time", bar.time},
                       {"open", bar.open},
                       {"high", bar.high},
                       {"low", bar.low},
                       {"close", bar.close},
                       {"volume", bar.volume},
                       {"turnover", bar.turnover},
                       {"open_interest", bar.open_interest}});
    }
    return out;
}

static nlohmann::json ticks_to_json(const std::vector<bll::TickRecord>& ticks) {
    nlohmann::json out = nlohmann::json::array();
    for (const auto& tick : ticks) {
        out.push_back({{"trading_day", tick.trading_day},
                       {"update_time", tick.update_time},
                       {"update_millisec", tick.update_millisec},
                       {"last_price", tick.last_price},
                       {"volume", tick.volume},
                       {"turnover", tick.turnover},
                       {"open_interest", tick.open_interest}});
    }
    return out;
}

static nlohmann::json build_symbol_index(const nlohmann::json& symbol_doc) {
    nlohmann::json symbol_index = nlohmann::json::object();
    if (symbol_doc.contains("symbols") && symbol_doc["symbols"].is_array()) {
        for (const auto& item : symbol_doc["symbols"]) {
            const std::string key = item.value("exchange", "") + "|" + item.value("product", "") + "|" +
                                    item.value("month_slot", "");
            if (!key.empty()) {
                symbol_index[key] = item;
            }
        }
    }
    return symbol_index;
}

static void enrich_catalog_item(nlohmann::json& item, const nlohmann::json& symbol_index) {
    const std::string key = item.value("exchange", "") + "|" + item.value("product", "") + "|" +
                            item.value("month_slot", "");
    if (symbol_index.contains(key)) {
        const auto& sym = symbol_index[key];
        item["instrument_id"] = sym.value("instrument_id", "");
        item["name"] = sym.value("name", "");
    } else {
        item["instrument_id"] = "";
        item["name"] = "";
    }
}

static nlohmann::json enrich_data_catalog(nlohmann::json catalog, const nlohmann::json& symbol_index) {
    if (catalog.contains("files") && catalog["files"].is_array()) {
        for (auto& item : catalog["files"]) {
            enrich_catalog_item(item, symbol_index);
        }
    }
    if (catalog.contains("tree") && catalog["tree"].is_object()) {
        for (auto it_period = catalog["tree"].begin(); it_period != catalog["tree"].end(); ++it_period) {
            if (!it_period.value().is_object()) {
                continue;
            }
            for (auto it_ex = it_period.value().begin(); it_ex != it_period.value().end(); ++it_ex) {
                if (!it_ex.value().is_object()) {
                    continue;
                }
                for (auto it_prod = it_ex.value().begin(); it_prod != it_ex.value().end(); ++it_prod) {
                    if (!it_prod.value().is_array()) {
                        continue;
                    }
                    for (auto& item : it_prod.value()) {
                        enrich_catalog_item(item, symbol_index);
                    }
                }
            }
        }
    }
    return catalog;
}

ApiResponse Gateway::handle_get(const std::string& path, const std::string& query_body) {
    nlohmann::json query = nlohmann::json::object();
    if (!query_body.empty()) {
        try {
            query = nlohmann::json::parse(query_body);
        } catch (...) {
        }
    }

    const auto st = status();
    if (path == "/api/status") {
        const auto md_sessions = quote_.md_sessions_status();
        const auto td_sessions = trade_.td_sessions_status();
        return {200,
                {{"md_ok", st.md_ok},
                 {"td_ok", st.td_ok},
                 {"version", st.version},
                 {"api_version", "phase5-full"},
                 {"phase", "5"},
                 {"http_port", config_.app().host.http_port},
                 {"ws_port", config_.app().host.ws_port},
                 {"data_browse", true},
                 {"connected_md_fronts", md_sessions.value("fronts", nlohmann::json::array())},
                 {"connected_md_users", md_sessions.value("users", nlohmann::json::array())},
                 {"connected_td_users", td_sessions.value("users", nlohmann::json::array())},
                 {"connected_td_sessions", td_sessions.value("sessions", nlohmann::json::array())},
                 {"reconnect", reconnect_status()},
                 {"ctp_enabled",
#ifdef QUANT_SEV_HAS_CTP
                  true
#else
                  false
#endif
                 }}};
    }
    if (path == "/api/saved_accounts") {
        auto body = accounts_.saved_accounts_public();
        if (body.contains("accounts") && body["accounts"].is_array()) {
            for (auto& acc : body["accounts"]) {
                const std::string md_front = acc.value("md_front", "");
                const std::string user_id = acc.value("user_id", "");
                const bool md_by_front = !md_front.empty() && quote_.is_front_ready(md_front);
                const bool md_by_user = !user_id.empty() && quote_.is_user_md_ready(user_id);
                acc["md_connected"] = md_by_front || md_by_user;
                acc["td_connected"] = trade_.is_account_ready(
                    AccountRecord::from_json(acc));
            }
        }
        return {200, body};
    }
    if (path == "/api/ui_logs") {
        const auto lines = Logger::instance().snapshot();
        return {200, {{"lines", lines}}};
    }
    if (path == "/api/symbols") {
        return {200, symbols_.list_symbols()};
    }
    if (path == "/api/data/browse") {
        const std::string exchange = query.value("exchange", "");
        const std::string product = query.value("product", "");
        const std::string month_slot = query.value("month_slot", "");
        auto result = storage_->browse_data(exchange, product, month_slot);
        if (result.contains("error")) {
            return {400, {{"error", result.value("error", "browse failed")}}};
        }
        if (result.value("level", "") == "file") {
            const auto symbol_index = build_symbol_index(symbols_.list_symbols());
            if (result.contains("items") && result["items"].is_array()) {
                for (auto& item : result["items"]) {
                    enrich_catalog_item(item, symbol_index);
                }
            }
        }
        return {200, result};
    }
    if (path == "/api/data/inventory" || path == "/api/data/tree") {
        auto catalog = storage_->scan_data_catalog();
        const auto symbol_index = build_symbol_index(symbols_.list_symbols());
        catalog = enrich_data_catalog(std::move(catalog), symbol_index);
        if (path == "/api/data/tree") {
            return {200, catalog};
        }
        return {200,
                {{"files", catalog.value("files", nlohmann::json::array())},
                 {"count", catalog.value("count", 0)},
                 {"layout", catalog.value("layout", "")}}};
    }
    if (path == "/api/data/preview") {
        const int limit = json_int_param(query, "limit", 100);
        std::string file_path = query.value("path", "");
        const std::string exchange = query.value("exchange", "");
        const std::string product = query.value("product", "");
        const std::string month_slot = query.value("month_slot", "");
        std::string period = query.value("period", "");
        if (file_path.empty() && !exchange.empty() && !product.empty() && !month_slot.empty() &&
            !period.empty()) {
            file_path = storage_->resolve_storage_file_path(exchange, product, month_slot, period);
        }
        if (file_path.empty()) {
            return {400, {{"error", "path 或 exchange/product/month_slot/period 必填"}}};
        }
        const std::filesystem::path p(file_path);
        const std::string fname = p.filename().string();
        if (fname == "tick.csv") {
            period = "tick";
            const auto ticks = storage_->read_ticks_at_path(file_path, limit);
            if (ticks.empty() && !std::filesystem::exists(p)) {
                return {404, {{"error", "文件不存在"}, {"path", file_path}}};
            }
            return {200,
                    {{"file_type", "tick"},
                     {"period", "tick"},
                     {"path", file_path},
                     {"exchange", exchange},
                     {"product", product},
                     {"month_slot", month_slot},
                     {"instrument_id", query.value("instrument_id", "")},
                     {"ticks", ticks_to_json(ticks)}}};
        }
        if (fname.size() > 4 && fname.substr(fname.size() - 4) == ".csv") {
            if (period.empty()) {
                period = fname.substr(0, fname.size() - 4);
            }
            const auto bars = storage_->read_bars_at_path(file_path, limit);
            if (bars.empty() && !std::filesystem::exists(p)) {
                return {404, {{"error", "文件不存在"}, {"path", file_path}}};
            }
            return {200,
                    {{"file_type", "bar"},
                     {"period", period},
                     {"path", file_path},
                     {"exchange", exchange},
                     {"product", product},
                     {"month_slot", month_slot},
                     {"instrument_id", query.value("instrument_id", "")},
                     {"bars", bars_to_json(bars)}}};
        }
        return {400, {{"error", "unsupported file type"}, {"path", file_path}}};
    }
    if (path == "/api/subscribed_symbols") {
        return {200, symbols_.subscribed_symbols()};
    }
    if (path == "/api/md_quote_board") {
        return {200, {{"quotes", md_quote_board()}}};
    }
    if (path == "/api/orderbook") {
        const std::string instrument_id = query.value("instrument_id", "");
        const int depth = json_int_param(query, "depth", 5);
        if (!instrument_id.empty()) {
            const auto snap = orderbook_->snapshot(instrument_id, depth);
            if (snap.contains("error")) {
                return {404, snap};
            }
            return {200, snap};
        }
        return {200, orderbook_->board(depth)};
    }
    if (path == "/api/rollover/suggest") {
        const auto symbol_doc = symbols_.list_symbols();
        return {200, rollover_->suggest(symbol_doc, rollover_quote_map())};
    }
    if (path == "/api/rollover/daily") {
        return {200, rollover_->daily_view(config_.root_dir())};
    }
    if (path == "/api/rollover/next") {
        const std::string instrument_id = query.value("instrument_id", "");
        if (instrument_id.empty()) {
            return {400, {{"error", "instrument_id required"}}};
        }
        const auto next = rollover_->next_contract(instrument_id);
        if (!next) {
            return {400, {{"error", "无法解析或推算次月合约"}, {"instrument_id", instrument_id}}};
        }
        return {200, {{"instrument_id", instrument_id}, {"next_instrument_id", *next}}};
    }
    if (path == "/api/bars") {
        const std::string instrument_id = query.value("instrument_id", "");
        if (instrument_id.empty()) {
            return {400, {{"error", "instrument_id required"}}};
        }
        const std::string period = query.value("period", "m1");
        static const std::array<const char*, 4> kPeriods = {"m1", "m15", "h1", "d1"};
        const bool valid_period =
            std::any_of(kPeriods.begin(), kPeriods.end(), [&](const char* p) { return period == p; });
        if (!valid_period) {
            return {400, {{"error", "period must be m1|m15|h1|d1"}}};
        }
        bll::BarQuery bar_query;
        bar_query.instrument_id = instrument_id;
        bar_query.period = period;
        bar_query.limit = json_int_param(query, "limit", 500);
        bar_query.prefer_historical = json_bool_param(query, "prefer_historical", true);
        const auto bars = storage_->query_bars(bar_query);
        return {200, {{"instrument_id", instrument_id}, {"period", bar_query.period}, {"bars", bars_to_json(bars)}}};
    }
    if (path == "/api/ticks") {
        const std::string instrument_id = query.value("instrument_id", "");
        if (instrument_id.empty()) {
            return {400, {{"error", "instrument_id required"}}};
        }
        bll::TickQuery tick_query;
        tick_query.instrument_id = instrument_id;
        tick_query.limit = json_int_param(query, "limit", 200);
        if (tick_query.limit <= 0) {
            tick_query.limit = 200;
        }
        const auto ticks = storage_->query_ticks(tick_query);
        return {200, {{"instrument_id", instrument_id}, {"ticks", ticks_to_json(ticks)}}};
    }
    if (path == "/api/diagnostic/bars") {
        const std::string instrument_id = query.value("instrument_id", "");
        if (instrument_id.empty()) {
            return {400, {{"error", "instrument_id required"}}};
        }
        const std::string period = query.value("period", "m1");
        static const std::array<const char*, 4> kDiagPeriods = {"m1", "m15", "h1", "d1"};
        const bool valid_diag_period =
            std::any_of(kDiagPeriods.begin(), kDiagPeriods.end(), [&](const char* p) { return period == p; });
        if (!valid_diag_period) {
            return {400, {{"error", "period must be m1|m15|h1|d1"}}};
        }
        const auto file_info = storage_->bar_file_info(instrument_id, period);
        bll::BarQuery bar_query;
        bar_query.instrument_id = instrument_id;
        bar_query.period = period;
        bar_query.limit = json_int_param(query, "tail", 5);
        if (bar_query.limit <= 0) {
            bar_query.limit = 5;
        }
        const auto api_bars = storage_->query_bars(bar_query);
        nlohmann::json file_last = nullptr;
        if (file_info.last) {
            const auto& bar = *file_info.last;
            file_last = {{"date", bar.date},
                         {"time", bar.time},
                         {"open", bar.open},
                         {"high", bar.high},
                         {"low", bar.low},
                         {"close", bar.close},
                         {"volume", bar.volume}};
        }
        nlohmann::json api_last = api_bars.empty() ? nullptr : bars_to_json(api_bars).back();
        bool tail_match = false;
        if (!api_bars.empty() && file_info.last) {
            const auto& bar = *file_info.last;
            const auto& tail = api_bars.back();
            tail_match = bar.date == tail.date && bar.time == tail.time && bar.close == tail.close;
        } else if (api_bars.empty() && !file_info.last) {
            tail_match = true;
        }
        return {200,
                {{"instrument_id", instrument_id},
                 {"period", period},
                 {"storage_path", file_info.path},
                 {"file_exists", file_info.exists},
                 {"file_bar_count", file_info.bar_count},
                 {"file_last", file_last},
                 {"api_tail", bars_to_json(api_bars)},
                 {"api_last", api_last},
                 {"tail_match", tail_match},
                 {"aligned", file_info.exists && tail_match}}};
    }
    if (path == "/api/diagnostic/backtest-bars") {
        const std::string instrument_id = query.value("instrument_id", "");
        if (instrument_id.empty()) {
            return {400, {{"error", "instrument_id required"}}};
        }
        const std::string period = query.value("period", "m1");
        static const std::array<const char*, 4> kBtPeriods = {"m1", "m15", "h1", "d1"};
        const bool valid_bt_period =
            std::any_of(kBtPeriods.begin(), kBtPeriods.end(), [&](const char* p) { return period == p; });
        if (!valid_bt_period) {
            return {400, {{"error", "period must be m1|m15|h1|d1"}}};
        }
        const int limit = json_int_param(query, "limit", 500);
        const auto file_info = storage_->bar_file_info(instrument_id, period);
        bll::BarQuery bar_query;
        bar_query.instrument_id = instrument_id;
        bar_query.period = period;
        bar_query.limit = limit > 0 ? limit : 500;
        auto backtest_bars = storage_->query_bars(bar_query);
        std::string resolved_period = period;
        bool fallback_m15 = false;
        if (backtest_bars.empty() && period != "m15") {
            bar_query.period = "m15";
            backtest_bars = storage_->query_bars(bar_query);
            if (!backtest_bars.empty()) {
                resolved_period = "m15";
                fallback_m15 = true;
            }
        }
        nlohmann::json file_last = nullptr;
        if (file_info.last) {
            const auto& bar = *file_info.last;
            file_last = {{"date", bar.date}, {"time", bar.time}, {"close", bar.close}};
        }
        nlohmann::json bt_last = nullptr;
        if (!backtest_bars.empty()) {
            const auto& bar = backtest_bars.back();
            bt_last = {{"date", bar.date}, {"time", bar.time}, {"close", bar.close}};
        }
        bool aligned = false;
        if (!backtest_bars.empty() && file_info.last && resolved_period == period) {
            const auto& bar = *file_info.last;
            const auto& tail = backtest_bars.back();
            aligned = bar.date == tail.date && bar.time == tail.time && bar.close == tail.close;
        } else if (backtest_bars.empty() && !file_info.last) {
            aligned = true;
        }
        return {200,
                {{"instrument_id", instrument_id},
                 {"requested_period", period},
                 {"resolved_period", resolved_period},
                 {"fallback_m15", fallback_m15},
                 {"storage_path", file_info.path},
                 {"file_bar_count", file_info.bar_count},
                 {"backtest_bar_count", static_cast<int>(backtest_bars.size())},
                 {"file_last", file_last},
                 {"backtest_last", bt_last},
                 {"aligned", aligned && !fallback_m15}}};
    }
    if (path == "/api/diagnostic/pipeline") {
        const auto strategies_doc = cta_.list_strategies();
        int running = 0;
        int total = 0;
        if (strategies_doc.contains("strategies") && strategies_doc["strategies"].is_array()) {
            for (const auto& row : strategies_doc["strategies"]) {
                total += 1;
                if (row.value("state", "") == "running") {
                    running += 1;
                }
            }
        }
        const auto positions_doc = cta_.positions_view(nlohmann::json::object());
        const auto subscribed = symbols_.subscribed_symbols();
        const int subscribed_count =
            subscribed.contains("instruments") && subscribed["instruments"].is_array()
                ? static_cast<int>(subscribed["instruments"].size())
                : 0;
        const auto risk_view = risk_.status_view();
        const auto time_status = time_check_.status();
        nlohmann::json bar_checks = nlohmann::json::array();
        if (strategies_doc.contains("strategies") && strategies_doc["strategies"].is_array()) {
            for (const auto& row : strategies_doc["strategies"]) {
                if (row.value("state", "") != "running") {
                    continue;
                }
                const std::string instrument_id = row.value("instrument_id", "");
                const std::string period = row.value("period", "m1");
                if (instrument_id.empty()) {
                    continue;
                }
                const auto file_info = storage_->bar_file_info(instrument_id, period);
                bll::BarQuery bar_query;
                bar_query.instrument_id = instrument_id;
                bar_query.period = period;
                bar_query.limit = 1;
                const auto api_bars = storage_->query_bars(bar_query);
                bool tail_match = false;
                if (!api_bars.empty() && file_info.last) {
                    const auto& bar = *file_info.last;
                    const auto& tail = api_bars.back();
                    tail_match = bar.date == tail.date && bar.time == tail.time && bar.close == tail.close;
                } else if (api_bars.empty() && !file_info.last) {
                    tail_match = true;
                }
                nlohmann::json file_last = nullptr;
                if (file_info.last) {
                    const auto& bar = *file_info.last;
                    file_last = {{"date", bar.date}, {"time", bar.time}, {"close", bar.close}};
                }
                bar_checks.push_back({{"strategy_id", row.value("id", "")},
                                      {"instrument_id", instrument_id},
                                      {"period", period},
                                      {"storage_path", file_info.path},
                                      {"file_exists", file_info.exists},
                                      {"file_bar_count", file_info.bar_count},
                                      {"file_last", file_last},
                                      {"aligned", file_info.exists && tail_match}});
            }
        }
        return {200,
                {{"md_ok", st.md_ok},
                 {"td_ok", st.td_ok},
                 {"in_session", time_status.in_session},
                 {"session_phase", time_status.phase},
                 {"subscribed_count", subscribed_count},
                 {"strategies_total", total},
                 {"strategies_running", running},
                 {"strategies", strategies_doc.value("strategies", nlohmann::json::array())},
                 {"bar_checks", bar_checks},
                 {"cta_positions", positions_doc.value("positions", nlohmann::json::array())},
                 {"cta_position_summary", positions_doc.value("summary", nlohmann::json::object())},
                 {"halt_all_orders", risk_view.value("halt_all_orders", false)},
                 {"halt_all_strategies", risk_view.value("halt_all_strategies", false)},
                 {"reconnect", reconnect_status()}}};
    }
    if (path == "/api/strategies") {
        return {200, cta_.list_strategies()};
    }
    if (path == "/api/cta/orders") {
        return {200, cta_.orders_view(query)};
    }
    if (path == "/api/trade/orders") {
        return {200, trade_.order_updates(query)};
    }
    if (path == "/api/trade/trades") {
        return {200, trade_.trade_updates(query)};
    }
    if (path == "/api/trade/history") {
        nlohmann::json payload = query;
        payload["refresh"] = json_bool_param(query, "refresh", false);
        const bool refresh = payload.value("refresh", false);
        std::optional<AccountRecord> account;
        if (!query.value("user_id", "").empty()) {
            account = resolve_account(payload);
        }
        if (refresh && !account) {
            return {400, {{"error", "refresh 需要有效 user_id 与已连接账户"}}};
        }
        const auto result = trade_.query_history(account ? &*account : nullptr, payload);
        if (result.contains("error")) {
            return {400, result};
        }
        return {200, result};
    }
    if (path == "/api/trade/account") {
        nlohmann::json payload = query;
        payload["refresh"] = json_bool_param(query, "refresh", false);
        const bool refresh = payload.value("refresh", false);
        std::optional<AccountRecord> account;
        if (!query.value("user_id", "").empty()) {
            account = resolve_account(payload);
        }
        if (refresh && !account) {
            return {400, {{"error", "refresh 需要有效 user_id 与已连接账户"}}};
        }
        const auto result = trade_.query_trading_account(account ? &*account : nullptr, payload);
        if (result.contains("error")) {
            return {400, result};
        }
        return {200, result};
    }
    if (path == "/api/trade/positions") {
        nlohmann::json payload = query;
        payload["refresh"] = json_bool_param(query, "refresh", false);
        const bool refresh = payload.value("refresh", false);
        std::optional<AccountRecord> account;
        if (!query.value("user_id", "").empty()) {
            account = resolve_account(payload);
        }
        if (refresh && !account) {
            return {400, {{"error", "refresh 需要有效 user_id 与已连接账户"}}};
        }
        const auto result = trade_.query_investor_positions(account ? &*account : nullptr, payload);
        if (result.contains("error")) {
            return {400, result};
        }
        const std::string user_id = query.value("user_id", account ? account->user_id : "");
        if (refresh && !user_id.empty() && result.contains("positions")) {
            cta_.replace_positions_from_snapshot(user_id, result["positions"]);
        }
        return {200, result};
    }
    if (path == "/api/risk/status") {
        const std::string user_id = query.value("user_id", "");
        const auto time_status = time_check_.status();
        nlohmann::json body = {{"risk", risk_.status_view()},
                               {"reconnect", reconnect_status()},
                               {"time_check",
                                {{"in_session", time_status.in_session},
                                 {"phase", time_status.phase},
                                 {"current_time", time_status.current_time},
                                 {"message", time_status.message}}}};
        if (!user_id.empty()) {
            const auto positions = cta_.positions_view({{"user_id", user_id}});
            const auto ctp_account = trade_.cached_trading_account(user_id);
            const TradingAccountSnapshot* ctp_ptr = ctp_account ? &*ctp_account : nullptr;
            body["positions"] = positions;
            body["runtime"] = risk_.runtime_view(user_id, positions, ctp_ptr);
            if (ctp_account) {
                body["ctp_account"] = {{"cached", true},
                                       {"balance", ctp_account->balance},
                                       {"available", ctp_account->available},
                                       {"curr_margin", ctp_account->curr_margin},
                                       {"frozen_margin", ctp_account->frozen_margin},
                                       {"position_profit", ctp_account->position_profit},
                                       {"close_profit", ctp_account->close_profit}};
            } else {
                body["ctp_account"] = {{"cached", false}};
            }
        }
        return {200, body};
    }
    if (path == "/api/cta/positions") {
        return {200, cta_.positions_view(query)};
    }
    if (path == "/api/cta/account") {
        const auto view = cta_.account_view(query);
        if (view.contains("error")) {
            return {400, view};
        }
        return {200, view};
    }
    if (path == "/api/indicators") {
        return {200, indicator_->catalog()};
    }
    if (path == "/api/indicator") {
        const std::string instrument_id = query.value("instrument_id", "");
        if (instrument_id.empty()) {
            return {400, {{"error", "instrument_id required"}}};
        }
        const std::string name = query.value("name", query.value("indicator", ""));
        if (name.empty()) {
            return {400, {{"error", "name required"}}};
        }
        const std::string period = query.value("period", "m1");
        static const std::array<const char*, 4> kPeriods = {"m1", "m15", "h1", "d1"};
        const bool valid_period =
            std::any_of(kPeriods.begin(), kPeriods.end(), [&](const char* p) { return period == p; });
        if (!valid_period) {
            return {400, {{"error", "period must be m1|m15|h1|d1"}}};
        }
        bll::IndicatorQuery ind_query;
        ind_query.instrument_id = instrument_id;
        ind_query.period = period;
        ind_query.name = name;
        ind_query.options = json_options_param(query);
        ind_query.limit = json_int_param(query, "limit", 500);
        const auto result = indicator_->compute(ind_query);
        if (result.contains("error")) {
            return {400, result};
        }
        return {200, result};
    }
    if (path == "/api/backtest/progress") {
        return {200, backtest_->progress()};
    }
    if (path == "/api/backtest/result") {
        return {200, backtest_->last_result()};
    }
    if (path == "/api/backtest/contract_spec") {
        const std::string instrument_id = query.value("instrument_id", query.value("symbol", ""));
        if (instrument_id.empty()) {
            return {400, {{"error", "instrument_id required"}}};
        }
        return {200, backtest_->contract_spec(instrument_id)};
    }
    if (path == "/api/backtest/strategies") {
        return {200, backtest_->list_strategies()};
    }
    if (path == "/api/backtest/strategy_inputs") {
        const std::string strategy = query.value("strategy", query.value("strategy_id", "dual_thrust"));
        return {200, backtest_->strategy_inputs(strategy)};
    }
    return {404, {{"error", "not found"}, {"path", path}}};
}

std::optional<AccountRecord> Gateway::resolve_account(const nlohmann::json& payload) const {
    return accounts_.resolve(payload);
}

ApiResponse Gateway::connect_result_to_response(const ConnectResult& result) const {
    if (result.ok) {
        return {200, {{"ok", true}, {"message", result.message}}};
    }
    return {400, {{"ok", false}, {"error", result.message}}};
}

ApiResponse Gateway::handle_post(const std::string& path, const std::string& body) {
    nlohmann::json payload = nlohmann::json::object();
    if (!body.empty()) {
        try {
            payload = nlohmann::json::parse(body);
        } catch (const std::exception& ex) {
            return {400, {{"error", ex.what()}}};
        }
    }

    if (path == "/api/save_account") {
        try {
            auto accounts_doc = config_.read_json_file("Account.json");
            if (!accounts_doc.contains("accounts") || !accounts_doc["accounts"].is_array()) {
                accounts_doc["accounts"] = nlohmann::json::array();
            }

            const auto record = AccountRecord::from_json(payload);
            auto& accounts = accounts_doc["accounts"];
            bool updated = false;
            for (auto& item : accounts) {
                if (item.value("user_id", "") == record.user_id && !record.user_id.empty()) {
                    item = record.to_json(true);
                    updated = true;
                    break;
                }
            }
            if (!updated) {
                accounts.push_back(record.to_json(true));
            }

            if (!config_.write_json_file("Account.json", accounts_doc)) {
                return {500, {{"error", "write Account.json failed"}}};
            }
            Logger::instance().info("账户配置已保存: " + record.name);
            return {200, {{"ok", true}}};
        } catch (const std::exception& ex) {
            return {400, {{"error", ex.what()}}};
        }
    }

    if (path == "/api/load/md") {
        const auto account = resolve_account(payload);
        if (!account) {
            return {400, {{"error", "未找到账户 user_id"}}};
        }
        Logger::instance().info("Gateway 触发 Md 连接: " + account->user_id);
        const auto md_result = quote_.connect(*account);
        auto response = connect_result_to_response(md_result);
        if (md_result.ok) {
            register_md_session(*account);
            const auto app_doc = config_.read_json_file("app.json");
            const bool auto_sub = !app_doc.contains("symbol") ||
                                  json_bool_param(app_doc["symbol"], "auto_subscribe_all_on_md_connect", true);
            bool do_sub = auto_sub;
            if (payload.contains("subscribe_all")) {
                do_sub = json_bool_param(payload, "subscribe_all", true);
            }
            if (do_sub) {
                nlohmann::json sub_payload = {{"name", account->name},
                                              {"user_id", account->user_id},
                                              {"md_front", account->md_front},
                                              {"subscribe_all", true}};
                const auto sub_result = symbols_.load_symbols(sub_payload);
                if (sub_result.ok) {
                    response.body["symbol_subscribe"] = sub_result.message;
                } else {
                    response.body["symbol_subscribe_error"] = sub_result.message;
                }
            }
            response.body["md_connected"] = quote_.is_front_ready(account->md_front);
            response.body["connected_md_front"] = account->md_front;
        }
        return response;
    }

    if (path == "/api/load/td") {
        const auto account = resolve_account(payload);
        if (!account) {
            return {400, {{"error", "未找到账户 user_id"}}};
        }
        Logger::instance().info("Gateway 触发 Td 连接: " + account->user_id);
        const auto td_result = trade_.connect(*account);
        auto response = connect_result_to_response(td_result);
        if (td_result.ok) {
            register_td_session(*account);
            refresh_trading_account_cache(*account);
            response.body["td_connected"] = trade_.is_account_ready(*account);
            response.body["connected_td_user"] = account->user_id;
        }
        return response;
    }

    if (path == "/api/load/symbol") {
        Logger::instance().info("Gateway 触发 Symbol 订阅");
        return connect_result_to_response(symbols_.load_symbols(payload));
    }

    if (path == "/api/unsubscribe/symbol") {
        return connect_result_to_response(symbols_.unsubscribe_symbols(payload));
    }

    if (path == "/api/disconnect/md") {
        const auto account = resolve_account(payload);
        if (!account) {
            return {400, {{"error", "未找到账户 user_id"}}};
        }
        unregister_md_session();
        return connect_result_to_response(quote_.disconnect(account->md_front));
    }

    if (path == "/api/disconnect/td") {
        const auto account = resolve_account(payload);
        if (!account) {
            return {400, {{"error", "未找到账户 user_id"}}};
        }
        unregister_td_session();
        return connect_result_to_response(trade_.disconnect(*account));
    }

    if (path == "/api/order") {
        const auto account = resolve_account(payload);
        if (!account) {
            return {400, {{"ok", false}, {"error", "未找到账户 user_id"}}};
        }
        OrderRequest order;
        order.user_id = account->user_id;
        order.instrument_id = payload.value("instrument_id", payload.value("contract", ""));
        order.direction = payload.value("direction", "buy");
        order.offset = payload.value("offset", "open");
        order.price_type = payload.value("price_type", payload.value("orderType", "limit"));
        order.price = payload.value("price", 0.0);
        order.volume = json_int_param(payload, "volume", 0);
        if (payload.contains("price") && payload["price"].is_string()) {
            try {
                order.price = std::stod(payload["price"].get<std::string>());
            } catch (...) {
            }
        }
        if (order.instrument_id.empty()) {
            return {400, {{"ok", false}, {"error", "instrument_id 必填"}}};
        }
        if (!is_valid_web_offset(order.offset)) {
            return {400, {{"ok", false}, {"error", "offset 仅支持 open 或 close"}}};
        }
        if (order.volume <= 0) {
            return {400, {{"ok", false}, {"error", "volume 必须大于 0"}}};
        }
        Logger::instance().info("Gateway 人工报单: " + order.instrument_id + " vol=" +
                                std::to_string(order.volume));
        const auto result = execute_order(*account, order, true);
        cta_.record_order(*account, order, result, "manual");
        if (result.ok) {
            return {200,
                    {{"ok", true},
                     {"message", result.message},
                     {"order_ref", result.order_ref},
                     {"instrument_id", order.instrument_id}}};
        }
        return {400,
                {{"ok", false},
                 {"error", result.message},
                 {"error_id", result.error_id},
                 {"order_ref", result.order_ref}}};
    }

    if (path == "/api/cancel_order") {
        const auto account = resolve_account(payload);
        if (!account) {
            return {400, {{"ok", false}, {"error", "未找到账户 user_id"}}};
        }
        CancelOrderRequest cancel;
        cancel.user_id = account->user_id;
        cancel.instrument_id = payload.value("instrument_id", payload.value("contract", ""));
        cancel.order_ref = payload.value("order_ref", "");
        cancel.order_sys_id = payload.value("order_sys_id", "");
        cancel.exchange_id = payload.value("exchange_id", "");
        if (cancel.order_ref.empty() && cancel.order_sys_id.empty()) {
            return {400, {{"ok", false}, {"error", "order_ref 或 order_sys_id 必填"}}};
        }
        if (!cancel.order_ref.empty() && cancel.instrument_id.empty()) {
            return {400, {{"ok", false}, {"error", "按 order_ref 撤单时 instrument_id 必填"}}};
        }
        Logger::instance().info("Gateway 撤单: ref=" + cancel.order_ref + " sys=" + cancel.order_sys_id);
        const auto result = trade_.cancel_order(*account, cancel);
        if (result.ok) {
            risk_.on_order_cancel(account->user_id);
            return {200,
                    {{"ok", true},
                     {"message", result.message},
                     {"order_ref", result.order_ref}}};
        }
        return {400,
                {{"ok", false},
                 {"error", result.message},
                 {"error_id", result.error_id},
                 {"order_ref", result.order_ref}}};
    }

    if (path == "/api/risk/halt") {
        const bool halt = payload.value("halt", payload.value("halt_all_orders", true));
        risk_.set_halt_all_orders(halt);
        bool strategies_halt = risk_.halt_all_strategies();
        if (payload.value("stop_strategies", false)) {
            risk_.set_halt_all_strategies(halt);
            strategies_halt = halt;
            if (halt) {
                cta_.stop_all_strategies();
            }
        }
        risk_.apply_config_patch(
            {{"emergency", {{"halt_all_orders", halt}, {"halt_all_strategies", strategies_halt}}}}, config_);
        return {200,
                {{"ok", true},
                 {"halt_all_orders", halt},
                 {"halt_all_strategies", strategies_halt}}};
    }

    if (path == "/api/risk/emergency") {
        const bool reset = payload.value("reset", false);
        if (reset) {
            risk_.set_halt_all_orders(false);
            risk_.set_halt_all_strategies(false);
            risk_.apply_config_patch({{"emergency", {{"halt_all_orders", false}, {"halt_all_strategies", false}}}},
                                     config_);
            return {200,
                    {{"ok", true},
                     {"reset", true},
                     {"halt_all_orders", false},
                     {"halt_all_strategies", false}}};
        }

        const bool halt = payload.value("halt", true);
        const bool stop_strategies = payload.value("stop_strategies", true);
        const bool flatten = payload.value("flatten", false);

        if (halt) {
            risk_.set_halt_all_orders(true);
        }

        nlohmann::json body = {{"ok", true}};
        if (stop_strategies) {
            risk_.set_halt_all_strategies(true);
            const auto stop_result = cta_.stop_all_strategies();
            body["stop_message"] = stop_result.message;
        }

        risk_.apply_config_patch({{"emergency",
                                   {{"halt_all_orders", halt},
                                    {"halt_all_strategies", risk_.halt_all_strategies()}}}},
                                 config_);

        body["halt_all_orders"] = halt;
        body["halt_all_strategies"] = risk_.halt_all_strategies();

        if (flatten) {
            const auto account = resolve_account(payload);
            if (!account) {
                return {400, {{"ok", false}, {"error", "急平需要有效 user_id 账户"}}};
            }
            body["flatten"] = cta_.emergency_flatten(*account, [this](const AccountRecord& acc, const OrderRequest& ord) {
                return execute_emergency_close(acc, ord);
            });
        }

        Logger::instance().warn("Risk 应急触发: halt=" + std::string(halt ? "1" : "0") + " stop=" +
                                std::string(stop_strategies ? "1" : "0") + " flatten=" +
                                std::string(flatten ? "1" : "0"));
        return {200, body};
    }

    if (path == "/api/risk/config") {
        if (!risk_.apply_config_patch(payload, config_)) {
            return {400, {{"ok", false}, {"error", "Risk.json 写入失败"}}};
        }
        time_check_.load(config_);
        return {200, {{"ok", true}, {"risk", risk_.status_view()}}};
    }

    if (path == "/api/risk/latency/clear") {
        risk_.clear_latency_pause();
        return {200, {{"ok", true}, {"risk", risk_.status_view()}}};
    }

    if (path == "/api/strategy/start") {
        if (risk_.halt_all_strategies()) {
            return {400, {{"ok", false}, {"error", "策略应急暂停中，无法启动"}}};
        }
        return connect_result_to_response(cta_.start_strategy(payload));
    }

    if (path == "/api/strategy/stop") {
        return connect_result_to_response(cta_.stop_strategy(payload));
    }

    if (path == "/api/strategy/signal") {
        const auto account = resolve_account(payload);
        if (!account) {
            return {400, {{"ok", false}, {"error", "未找到账户 user_id"}}};
        }
        TradingSignal signal;
        signal.strategy_id = payload.value("strategy_id", payload.value("id", ""));
        signal.user_id = account->user_id;
        signal.instrument_id = payload.value("instrument_id", payload.value("contract", ""));
        signal.direction = payload.value("direction", "buy");
        signal.offset = payload.value("offset", "open");
        signal.price_type = payload.value("price_type", payload.value("orderType", "limit"));
        signal.price = payload.value("price", 0.0);
        signal.volume = json_int_param(payload, "volume", 0);
        if (payload.contains("price") && payload["price"].is_string()) {
            try {
                signal.price = std::stod(payload["price"].get<std::string>());
            } catch (...) {
            }
        }
        Logger::instance().info("CTA 策略信号: " + signal.strategy_id + " " + signal.instrument_id);
        const auto result = cta_.submit_signal(*account, signal);
        if (result.ok) {
            return {200, {{"ok", true}, {"message", result.message}}};
        }
        return {400, {{"ok", false}, {"error", result.message}}};
    }

    if (path == "/api/data/delete") {
        const std::string file_path = payload.value("path", "");
        const auto result = storage_->delete_data_file(file_path);
        if (!result.ok) {
            return {400, {{"ok", false}, {"error", result.message}, {"path", file_path}}};
        }
        Logger::instance().info("Storage 删除: " + result.path);
        return {200, {{"ok", true}, {"message", result.message}, {"path", result.path}}};
    }

    if (path == "/api/data/import") {
        const std::string exchange = payload.value("exchange", "");
        const std::string product = payload.value("product", "");
        const std::string month_slot = payload.value("month_slot", "");
        const std::string period = payload.value("period", "");
        const std::string csv_content = payload.value("csv_content", "");
        const std::string mode = payload.value("mode", "replace");
        const auto result =
            storage_->import_data_csv(exchange, product, month_slot, period, csv_content, mode);
        if (!result.ok) {
            return {400,
                    {{"ok", false},
                     {"error", result.message},
                     {"exchange", exchange},
                     {"product", product},
                     {"month_slot", month_slot},
                     {"period", period}}};
        }
        Logger::instance().info("Storage 导入: " + result.path + " rows=" + std::to_string(result.row_count));
        return {200,
                {{"ok", true},
                 {"message", result.message},
                 {"path", result.path},
                 {"row_count", result.row_count}}};
    }

    if (path == "/api/rollover/snapshot") {
        std::string trading_day = payload.value("trading_day", "");
        if (trading_day.empty()) {
            std::lock_guard<std::mutex> lock(rollover_mutex_);
            trading_day = rollover_last_trading_day_;
        }
        if (trading_day.empty()) {
            return {400, {{"ok", false}, {"error", "trading_day 未知，请传入或等待行情 Tick"}}};
        }
        const auto symbol_doc = symbols_.list_symbols();
        const auto result =
            rollover_->record_daily_snapshot(config_.root_dir(), trading_day, symbol_doc, rollover_quote_map());
        if (!result.value("ok", false)) {
            return {400, result};
        }
        {
            std::lock_guard<std::mutex> lock(rollover_mutex_);
            rollover_last_auto_snapshot_day_ = trading_day;
        }
        return {200, result};
    }

    if (path == "/api/rollover/apply") {
        const auto result = rollover_->apply(config_.root_dir(), payload);
        if (!result.value("ok", false)) {
            return {400, result};
        }
        const std::string old_id = result.value("old_instrument_id", "");
        const std::string new_id = result.value("new_instrument_id", "");
        nlohmann::json body = result;
        if (payload.value("resubscribe", true) && !old_id.empty() && !new_id.empty()) {
            nlohmann::json unsub = payload;
            unsub["instrument_ids"] = nlohmann::json::array({old_id});
            const auto unsub_result = symbols_.unsubscribe_symbols(unsub);
            body["unsubscribe"] = {{"ok", unsub_result.ok}, {"message", unsub_result.message}};
            nlohmann::json sub = payload;
            sub["instrument_ids"] = nlohmann::json::array({new_id});
            const auto sub_result = symbols_.load_symbols(sub);
            body["subscribe"] = {{"ok", sub_result.ok}, {"message", sub_result.message}};
        }
        return {200, body};
    }

    if (path == "/api/backtest/run" || path == "/api/backtest/optimize") {
        if (backtest_->is_running()) {
            return {409, {{"error", "backtest already running"}}};
        }

        bll::BacktestRequest request;
        request.instrument_id = payload.value("instrument_id", payload.value("symbol", "rb2610"));
        request.period = payload.value("period", "15分钟");
        request.strategy = payload.value("strategy", "dual_thrust");
        if (path == "/api/backtest/optimize" && payload.contains("optimize_strategy")) {
            request.strategy = payload.value("optimize_strategy", request.strategy);
        }
        request.initial_capital = payload.value("initial_capital", payload.value("capital", 1000000.0));
        request.limit = json_int_param(payload, "limit", json_int_param(payload, "max_bars", 50000));
        request.days = json_int_param(payload, "days", 4);
        request.k1 = payload.value("k1", 0.5);
        request.k2 = payload.value("k2", 0.5);
        request.volume = json_int_param(payload, "volume", 1);
        request.contract_multiplier = payload.value("contract_multiplier", payload.value("multiplier", 0.0));
        request.tick_size = payload.value("tick_size", payload.value("tick", 0.0));
        request.slippage_ticks = payload.value("slippage_ticks", payload.value("slippage", 0.0));
        request.fee_rate = payload.value("fee_rate", 0.0001);
        request.fee_per_lot = payload.value("fee_per_lot", payload.value("commission_per_lot", 0.0));
        request.margin_rate = payload.value("margin_rate", 0.1);
        request.start_date = payload.value("start_date", "");
        request.end_date = payload.value("end_date", "");
        request.ma_fast = json_int_param(payload, "ma_fast", 5);
        request.ma_slow = json_int_param(payload, "ma_slow", 20);
        request.macd_short = json_int_param(payload, "macd_short", 12);
        request.macd_long = json_int_param(payload, "macd_long", 26);
        request.macd_signal = json_int_param(payload, "macd_signal", 9);
        request.boll_period = json_int_param(payload, "boll_period", 20);
        request.boll_stddev = payload.value("boll_stddev", 2.0);
        request.optimize_metric = payload.value("optimize_metric", "total_return");
        if (payload.contains("optimize_grid") && payload["optimize_grid"].is_object()) {
            request.optimize_grid = payload["optimize_grid"];
        }
        if (payload.contains("instrument_ids") && payload["instrument_ids"].is_array()) {
            for (const auto& item : payload["instrument_ids"]) {
                if (item.is_string()) {
                    request.instrument_ids.push_back(item.get<std::string>());
                }
            }
        }
        if (request.instrument_ids.empty()) {
            const std::string raw = request.instrument_id;
            std::string token;
            auto flush_token = [&]() {
                while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front()))) {
                    token.erase(token.begin());
                }
                while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back()))) {
                    token.pop_back();
                }
                std::transform(token.begin(), token.end(), token.begin(), [](unsigned char ch) {
                    return static_cast<char>(std::tolower(ch));
                });
                if (!token.empty()) {
                    request.instrument_ids.push_back(token);
                }
                token.clear();
            };
            for (std::size_t i = 0; i < raw.size(); ++i) {
                const unsigned char ch = static_cast<unsigned char>(raw[i]);
                if (ch == ',') {
                    flush_token();
                    continue;
                }
                if (i + 2 < raw.size() && static_cast<unsigned char>(raw[i]) == 0xE3 &&
                    static_cast<unsigned char>(raw[i + 1]) == 0x80 &&
                    static_cast<unsigned char>(raw[i + 2]) == 0x81) {
                    flush_token();
                    i += 2;
                    continue;
                }
                if (ch == ' ' || ch == '\t' || ch == ';') {
                    flush_token();
                    continue;
                }
                token += raw[i];
            }
            flush_token();
        }
        if (request.instrument_ids.size() > 1) {
            request.instrument_id = request.instrument_ids.front();
            if (request.mode == "bar" || request.mode == "multi_symbol") {
                request.mode = "multi_symbol";
            }
        } else if (!request.instrument_ids.empty()) {
            request.instrument_id = request.instrument_ids.front();
        }
        if (payload.contains("strategies") && payload["strategies"].is_array()) {
            for (const auto& item : payload["strategies"]) {
                if (item.is_string()) {
                    request.strategies.push_back(item.get<std::string>());
                }
            }
        }
        if (payload.contains("strategy_ids") && payload["strategy_ids"].is_array()) {
            for (const auto& item : payload["strategy_ids"]) {
                if (item.is_string()) {
                    request.strategy_ids.push_back(item.get<std::string>());
                }
            }
        }
        request.mode = payload.value("mode", path == "/api/backtest/optimize" ? "optimize" : "bar");
        if (path == "/api/backtest/optimize") {
            request.mode = "optimize";
        }
        if (payload.contains("intra_bar")) {
            request.intra_bar = payload.value("intra_bar", true);
        } else {
            request.intra_bar = request.mode == "tick";
        }
        request.tick_limit = json_int_param(payload, "tick_limit", json_int_param(payload, "max_ticks", 200000));

        std::string symbol_log = request.instrument_id;
        if (request.instrument_ids.size() > 1) {
            symbol_log.clear();
            for (std::size_t i = 0; i < request.instrument_ids.size(); ++i) {
                if (i > 0) {
                    symbol_log += ',';
                }
                symbol_log += request.instrument_ids[i];
            }
        }
        Logger::instance().info("Backtest 启动: " + symbol_log + " " + request.period + " mode=" + request.mode +
                                (request.instrument_ids.size() > 1
                                     ? (" multi(" + std::to_string(request.instrument_ids.size()) + ")")
                                     : ""));
        std::thread([this, request]() { backtest_->run(request); }).detach();
        return {202, {{"ok", true}, {"started", true}, {"mode", request.mode}}};
    }

    return {404, {{"error", "not found"}, {"path", path}}};
}

std::optional<InstrumentQuoteSnapshot> Gateway::quote_snapshot_for(const std::string& instrument_id) const {
    if (instrument_id.empty()) {
        return std::nullopt;
    }
    const auto same_instrument = [&](const std::string& left, const std::string& right) {
        if (left.size() != right.size()) {
            return false;
        }
        for (std::size_t i = 0; i < left.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(left[i])) !=
                std::tolower(static_cast<unsigned char>(right[i]))) {
                return false;
            }
        }
        return true;
    };
    for (const auto& row : quote_.quote_board()) {
        if (same_instrument(row.value("instrument_id", std::string{}), instrument_id)) {
            InstrumentQuoteSnapshot snap;
            snap.last_price = row.value("last_price", 0.0);
            snap.upper_limit = row.value("upper_limit", 0.0);
            snap.lower_limit = row.value("lower_limit", 0.0);
            return snap;
        }
    }
    return std::nullopt;
}

void Gateway::track_order_send_time(const std::string& user_id, const std::string& order_ref) {
    if (user_id.empty() || order_ref.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(order_rtt_mutex_);
    pending_order_send_[user_id + ":" + order_ref] = std::chrono::steady_clock::now();
}

void Gateway::on_order_update_rtt(const nlohmann::json& update) {
    const std::string user_id = update.value("user_id", "");
    const std::string order_ref = update.value("order_ref", "");
    if (user_id.empty() || order_ref.empty()) {
        return;
    }
    const std::string key = user_id + ":" + order_ref;
    std::chrono::steady_clock::time_point sent_at;
    {
        std::lock_guard<std::mutex> lock(order_rtt_mutex_);
        const auto it = pending_order_send_.find(key);
        if (it == pending_order_send_.end()) {
            return;
        }
        sent_at = it->second;
        pending_order_send_.erase(it);
    }
    const auto now = std::chrono::steady_clock::now();
    const int rtt_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(now - sent_at).count());
    if (rtt_ms >= 0) {
        risk_.record_order_rtt(rtt_ms);
    }
}

nlohmann::json Gateway::reconnect_status() const {
    const int interval = risk_.session_reconnect_interval_ms();
    std::lock_guard<std::mutex> lock(reconnect_mutex_);
    return {{"enabled", interval > 0},
            {"interval_ms", interval},
            {"md_watch", reconnect_.md_watch},
            {"td_watch", reconnect_.td_watch},
            {"md_attempts", reconnect_.md_attempts},
            {"td_attempts", reconnect_.td_attempts},
            {"md_in_progress", reconnect_.md_in_progress},
            {"td_in_progress", reconnect_.td_in_progress},
            {"md_last_error", reconnect_.md_last_error},
            {"td_last_error", reconnect_.td_last_error}};
}

void Gateway::register_md_session(const AccountRecord& account) {
    std::lock_guard<std::mutex> lock(reconnect_mutex_);
    reconnect_.md_account = account;
    reconnect_.md_watch = true;
    reconnect_.md_attempts = 0;
    reconnect_.md_last_error.clear();
    reconnect_.md_in_progress = false;
}

void Gateway::unregister_md_session() {
    std::lock_guard<std::mutex> lock(reconnect_mutex_);
    reconnect_.md_watch = false;
    reconnect_.md_in_progress = false;
}

void Gateway::register_td_session(const AccountRecord& account) {
    std::lock_guard<std::mutex> lock(reconnect_mutex_);
    reconnect_.td_account = account;
    reconnect_.td_watch = true;
    reconnect_.td_attempts = 0;
    reconnect_.td_last_error.clear();
    reconnect_.td_in_progress = false;
}

void Gateway::unregister_td_session() {
    std::lock_guard<std::mutex> lock(reconnect_mutex_);
    reconnect_.td_watch = false;
    reconnect_.td_in_progress = false;
}

void Gateway::schedule_md_reconnect() {
    const int interval = risk_.session_reconnect_interval_ms();
    const auto now = std::chrono::steady_clock::now();
    if (interval <= 0) {
        reconnect_.md_next_attempt = now + std::chrono::hours(24 * 365);
        return;
    }
    reconnect_.md_next_attempt = now + std::chrono::milliseconds(interval);
}

void Gateway::schedule_td_reconnect() {
    const int interval = risk_.session_reconnect_interval_ms();
    const auto now = std::chrono::steady_clock::now();
    if (interval <= 0) {
        reconnect_.td_next_attempt = now + std::chrono::hours(24 * 365);
        return;
    }
    reconnect_.td_next_attempt = now + std::chrono::milliseconds(interval);
}

void Gateway::on_md_disconnected(const std::string& md_front, int reason) {
    (void)reason;
    std::lock_guard<std::mutex> lock(reconnect_mutex_);
    if (!reconnect_.md_watch || !reconnect_.md_account || reconnect_.md_account->md_front != md_front) {
        return;
    }
    Logger::instance().warn("Gateway 安排 Md 重连: " + md_front);
    schedule_md_reconnect();
}

void Gateway::on_td_disconnected(const std::string& user_id, int reason) {
    (void)reason;
    std::lock_guard<std::mutex> lock(reconnect_mutex_);
    if (!reconnect_.td_watch || !reconnect_.td_account || reconnect_.td_account->user_id != user_id) {
        return;
    }
    Logger::instance().warn("Gateway 安排 Td 重连: " + user_id);
    schedule_td_reconnect();
}

void Gateway::try_reconnect_md() {
    std::optional<AccountRecord> account;
    {
        std::lock_guard<std::mutex> lock(reconnect_mutex_);
        if (!reconnect_.md_watch || !reconnect_.md_account || reconnect_.md_in_progress) {
            return;
        }
        if (quote_.is_front_ready(reconnect_.md_account->md_front)) {
            reconnect_.md_attempts = 0;
            reconnect_.md_last_error.clear();
            return;
        }
        if (std::chrono::steady_clock::now() < reconnect_.md_next_attempt) {
            return;
        }
        account = reconnect_.md_account;
        reconnect_.md_in_progress = true;
    }

    Logger::instance().info("Gateway Md 自动重连: " + account->user_id);
    const auto result = quote_.connect(*account);
    if (result.ok) {
        nlohmann::json sub_payload = {{"user_id", account->user_id}, {"subscribe_all", true}};
        const auto sub_result = symbols_.load_symbols(sub_payload);
        if (!sub_result.ok) {
            Logger::instance().warn("Md 重连后订阅失败: " + sub_result.message);
        }
        std::lock_guard<std::mutex> lock(reconnect_mutex_);
        reconnect_.md_in_progress = false;
        reconnect_.md_attempts = 0;
        reconnect_.md_last_error.clear();
        Logger::instance().info("Gateway Md 重连成功");
        return;
    }

    std::lock_guard<std::mutex> lock(reconnect_mutex_);
    reconnect_.md_in_progress = false;
    reconnect_.md_attempts += 1;
    reconnect_.md_last_error = result.message;
    schedule_md_reconnect();
    Logger::instance().warn("Gateway Md 重连失败(" + std::to_string(reconnect_.md_attempts) + "): " + result.message);
}

void Gateway::try_reconnect_td() {
    std::optional<AccountRecord> account;
    {
        std::lock_guard<std::mutex> lock(reconnect_mutex_);
        if (!reconnect_.td_watch || !reconnect_.td_account || reconnect_.td_in_progress) {
            return;
        }
        if (trade_.is_user_ready(reconnect_.td_account->user_id)) {
            reconnect_.td_attempts = 0;
            reconnect_.td_last_error.clear();
            return;
        }
        if (std::chrono::steady_clock::now() < reconnect_.td_next_attempt) {
            return;
        }
        account = reconnect_.td_account;
        reconnect_.td_in_progress = true;
    }

    Logger::instance().info("Gateway Td 自动重连: " + account->user_id);
    const auto result = trade_.connect(*account);
    if (result.ok) {
        std::lock_guard<std::mutex> lock(reconnect_mutex_);
        reconnect_.td_in_progress = false;
        reconnect_.td_attempts = 0;
        reconnect_.td_last_error.clear();
        Logger::instance().info("Gateway Td 重连成功");
        refresh_trading_account_cache(*account);
        return;
    }

    std::lock_guard<std::mutex> lock(reconnect_mutex_);
    reconnect_.td_in_progress = false;
    reconnect_.td_attempts += 1;
    reconnect_.td_last_error = result.message;
    schedule_td_reconnect();
    Logger::instance().warn("Gateway Td 重连失败(" + std::to_string(reconnect_.td_attempts) + "): " + result.message);
}

void Gateway::refresh_trading_account_cache(const AccountRecord& account) {
    const auto result = trade_.query_trading_account(&account, {{"user_id", account.user_id}, {"refresh", true}});
    if (result.contains("error")) {
        Logger::instance().warn("CTP 资金缓存刷新失败: " + result["error"].get<std::string>());
        return;
    }
    Logger::instance().info("CTP 资金缓存已刷新: " + account.user_id);
}

void Gateway::poll_reconnect() {
    const bool in_session = time_check_.status().in_session;
    if (rollover_last_in_session_ && !in_session && !rollover_last_trading_day_.empty()) {
        maybe_record_rollover_snapshot(rollover_last_trading_day_, "session_end_poll");
    }
    rollover_last_in_session_ = in_session;

    if (risk_.session_reconnect_interval_ms() <= 0) {
        return;
    }
    try_reconnect_md();
    try_reconnect_td();
}

}  // namespace quant_sev::core
