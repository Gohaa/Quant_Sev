#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "Account/AccountService.hpp"
#include "Config/Config.hpp"
#include "CTA/CtaEngine.hpp"
#include "Quote/QuoteEngine.hpp"
#include "Risk/RiskEngine.hpp"
#include "Symbol/SymbolService.hpp"
#include "TimeCheck/TimeCheckEngine.hpp"
#include "Trade/TradeEngine.hpp"

namespace quant_sev::bll {
class BacktestEngine;
class BarEngine;
class IndicatorEngine;
class OrderBookEngine;
class RolloverEngine;
class StorageEngine;
class StrategyEngine;
}  // namespace quant_sev::bll

namespace quant_sev::core {

struct GatewayStatus {
    bool md_ok{false};
    bool td_ok{false};
    std::string version;
};

struct ApiResponse {
    int status{200};
    nlohmann::json body;
};

class Gateway {
public:
    using TickListener = std::function<void(const nlohmann::json&)>;
    using OrderListener = std::function<void(const nlohmann::json&)>;
    using TradeListener = std::function<void(const nlohmann::json&)>;

    explicit Gateway(Config& config);
    ~Gateway();

    bool initialize();
    void poll_reconnect();
    void set_tick_listener(TickListener listener);
    void set_order_listener(OrderListener listener);
    void set_trade_listener(TradeListener listener);
    GatewayStatus status() const;
    nlohmann::json reconnect_status() const;
    ApiResponse handle(const std::string& method, const std::string& path, const std::string& body);

private:
    struct ReconnectState {
        std::optional<AccountRecord> md_account;
        std::optional<AccountRecord> td_account;
        bool md_watch{false};
        bool td_watch{false};
        std::chrono::steady_clock::time_point md_next_attempt{};
        std::chrono::steady_clock::time_point td_next_attempt{};
        int md_attempts{0};
        int td_attempts{0};
        bool md_in_progress{false};
        bool td_in_progress{false};
        std::string md_last_error;
        std::string td_last_error;
    };

    ApiResponse handle_get(const std::string& path, const std::string& query);
    ApiResponse handle_post(const std::string& path, const std::string& body);
    std::optional<AccountRecord> resolve_account(const nlohmann::json& payload) const;
    ApiResponse connect_result_to_response(const ConnectResult& result) const;
    OrderResult execute_order(const AccountRecord& account, const OrderRequest& order, bool manual_order,
                              RiskCheckMode risk_mode = RiskCheckMode::Normal);
    OrderResult execute_emergency_close(const AccountRecord& account, const OrderRequest& order);
    nlohmann::json md_quote_board() const;
    void on_market_tick(const nlohmann::json& tick);
    void register_md_session(const AccountRecord& account);
    void unregister_md_session();
    void register_td_session(const AccountRecord& account);
    void unregister_td_session();
    void on_md_disconnected(const std::string& md_front, int reason);
    void on_td_disconnected(const std::string& user_id, int reason);
    void schedule_md_reconnect();
    void schedule_td_reconnect();
    void try_reconnect_md();
    void try_reconnect_td();
    void refresh_trading_account_cache(const AccountRecord& account);
    nlohmann::json rollover_quote_map() const;
    void update_rollover_tick_state(const nlohmann::json& tick);
    void maybe_record_rollover_snapshot(const std::string& trading_day, const char* reason);
    std::optional<InstrumentQuoteSnapshot> quote_snapshot_for(const std::string& instrument_id) const;
    void track_order_send_time(const std::string& user_id, const std::string& order_ref);
    void on_order_update_rtt(const nlohmann::json& update);

    Config& config_;
    GatewayStatus status_;
    AccountService accounts_;
    QuoteEngine quote_;
    TradeEngine trade_;
    CtaEngine cta_;
    RiskEngine risk_;
    TimeCheckEngine time_check_;
    SymbolService symbols_;
    TickListener tick_listener_;
    OrderListener order_listener_;
    TradeListener trade_listener_;
    std::unique_ptr<bll::StorageEngine> storage_;
    std::unique_ptr<bll::BarEngine> bar_;
    std::unique_ptr<bll::OrderBookEngine> orderbook_;
    std::unique_ptr<bll::RolloverEngine> rollover_;
    std::unique_ptr<bll::IndicatorEngine> indicator_;
    std::unique_ptr<bll::StrategyEngine> strategy_;
    std::unique_ptr<bll::BacktestEngine> backtest_;
    mutable std::mutex reconnect_mutex_;
    ReconnectState reconnect_;
    mutable std::mutex rollover_mutex_;
    nlohmann::json rollover_quote_cache_{nlohmann::json::object()};
    std::string rollover_last_trading_day_;
    std::string rollover_last_auto_snapshot_day_;
    bool rollover_last_in_session_{false};
    mutable std::mutex order_rtt_mutex_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> pending_order_send_;
};

}  // namespace quant_sev::core
