#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "Account/AccountRecord.hpp"
#include "Common/ConnectResult.hpp"
#include "Config/Config.hpp"
#include "CTA/CtaTypes.hpp"
#include "Trade/OrderTypes.hpp"

namespace quant_sev::core {

class CtaEngine {
public:
    using OrderExecutor = std::function<OrderResult(const AccountRecord&, const OrderRequest&)>;

    bool initialize(Config& config);
    void set_order_executor(OrderExecutor executor);
    void set_strategy_gate(std::function<bool()> gate);

    nlohmann::json list_strategies() const;
    ConnectResult start_strategy(const nlohmann::json& payload);
    ConnectResult stop_strategy(const nlohmann::json& payload);
    ConnectResult stop_all_strategies();
    nlohmann::json emergency_flatten(const AccountRecord& account, const OrderExecutor& executor);
    ConnectResult submit_signal(const AccountRecord& account, const TradingSignal& signal);

    nlohmann::json orders_view(const nlohmann::json& query) const;
    nlohmann::json positions_view(const nlohmann::json& query) const;
    nlohmann::json account_view(const nlohmann::json& query) const;

    void record_order(const AccountRecord& account, const OrderRequest& request, const OrderResult& result,
                      const std::string& source);
    void apply_order_update(const nlohmann::json& update);
    void apply_trade_update(const nlohmann::json& update);
    void replace_positions_from_snapshot(const std::string& user_id, const nlohmann::json& positions);
    std::optional<CtaPositionView> position_for(const std::string& user_id,
                                                const std::string& instrument_id) const;

private:
    const StrategyDefinition* find_definition(const std::string& strategy_id) const;
    StrategyRuntime* find_runtime(const std::string& strategy_id);
    const StrategyRuntime* find_runtime(const std::string& strategy_id) const;
    ConnectResult validate_signal(const StrategyDefinition& definition, StrategyRuntime& runtime,
                                  const TradingSignal& signal);
    void apply_position_delta(const std::string& user_id, const OrderRequest& request, int filled_volume);
    static std::string now_text();
    static std::string trading_day_key();
    void reset_daily_counters_if_needed(StrategyRuntime& runtime);

    mutable std::mutex mutex_;
    Config* config_{nullptr};
    OrderExecutor order_executor_;
    std::function<bool()> strategy_gate_;
    std::vector<StrategyDefinition> definitions_;
    std::unordered_map<std::string, nlohmann::json> strategy_configs_;
    std::unordered_map<std::string, StrategyRuntime> runtimes_;
    std::vector<CtaOrderView> orders_;
    std::unordered_set<std::string> seen_trade_ids_;
    std::unordered_map<std::string, std::unordered_map<std::string, CtaPositionView>> positions_by_user_;
};

}  // namespace quant_sev::core
