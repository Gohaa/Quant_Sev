#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "Account/AccountRecord.hpp"
#include "Common/ConnectResult.hpp"
#include "Trade/OrderTypes.hpp"

namespace quant_sev::core {

class TradeEngine {
public:
    using OrderListener = std::function<void(const nlohmann::json&)>;
    using TradeListener = std::function<void(const nlohmann::json&)>;

    TradeEngine();
    ~TradeEngine();

    TradeEngine(const TradeEngine&) = delete;
    TradeEngine& operator=(const TradeEngine&) = delete;

    ConnectResult connect(const AccountRecord& account);
    ConnectResult disconnect(const AccountRecord& account);
    OrderResult insert_order(const AccountRecord& account, const OrderRequest& request);
    CancelResult cancel_order(const AccountRecord& account, const CancelOrderRequest& request);
    nlohmann::json order_updates(const nlohmann::json& query) const;
    nlohmann::json trade_updates(const nlohmann::json& query) const;
    nlohmann::json query_history(const AccountRecord* account, const nlohmann::json& query);
    nlohmann::json query_trading_account(const AccountRecord* account, const nlohmann::json& query);
    nlohmann::json query_investor_positions(const AccountRecord* account, const nlohmann::json& query);
    std::optional<TradingAccountSnapshot> cached_trading_account(const std::string& user_id) const;
    void set_order_listener(OrderListener listener);
    void set_trade_listener(TradeListener listener);
    void set_disconnect_callback(std::function<void(const std::string& user_id, int reason)> callback);
    bool is_ready() const;
    bool is_user_ready(const std::string& user_id) const;
    bool is_account_ready(const AccountRecord& account) const;
    nlohmann::json td_sessions_status() const;
    void set_project_root(const std::filesystem::path& root);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace quant_sev::core
