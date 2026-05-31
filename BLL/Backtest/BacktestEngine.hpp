#pragma once

#include <mutex>
#include <nlohmann/json.hpp>
#include <unordered_map>

#include "Backtest/BacktestProgress.hpp"
#include "Backtest/BacktestTypes.hpp"
#include "Backtest/MatchEngine.hpp"
#include "Common/OptionRules.hpp"
#include "Storage/StorageEngine.hpp"

namespace quant_sev::bll {

class BacktestEngine {
public:
    explicit BacktestEngine(StorageEngine& storage);

    bool initialize();
    nlohmann::json run(const BacktestRequest& request);
    nlohmann::json progress() const;
    bool is_running() const;
    nlohmann::json last_result() const;
    nlohmann::json contract_spec(const std::string& instrument_id) const;
    nlohmann::json list_strategies() const;
    nlohmann::json strategy_inputs(const std::string& strategy_id) const;

private:
    void set_progress(bool running, const std::string& phase, int current, int total, const std::string& message,
                      const std::string& instrument_id = "");
    void apply_contract_spec(BacktestRequest& request) const;
    static BacktestRequest request_for_strategy(const BacktestRequest& base, const std::string& strategy,
                                                const nlohmann::json& strategy_row);
    static std::string period_from_ui(const std::string& ui_period);
    static nlohmann::json build_summary(const BacktestRequest& request, const MatchEngine& match, int bars,
                                        int ticks_processed = 0);
    static nlohmann::json build_comparison(const nlohmann::json& bar_result, const nlohmann::json& tick_result);
    static std::string period_bar_key(const BarRecord& bar, const std::string& period);
    static std::string period_bucket_key_from_tick(const TickRecord& tick, const std::string& period);
    static std::unordered_map<std::string, std::vector<TickRecord>> index_ticks_by_period(
        const std::vector<TickRecord>& ticks, const std::string& period);

    nlohmann::json run_bar_mode(const BacktestRequest& request);
    nlohmann::json run_tick_mode(const BacktestRequest& request);
    nlohmann::json run_compare_mode(const BacktestRequest& request);
    nlohmann::json run_multi_mode(const BacktestRequest& request);
    nlohmann::json run_multi_symbol_mode(const BacktestRequest& request);
    nlohmann::json run_optimize_mode(const BacktestRequest& request);
    nlohmann::json run_optimize_single(const BacktestRequest& request);
    nlohmann::json run_multi_symbol_optimize_mode(const BacktestRequest& request);
    nlohmann::json run_strategy_on_bars(const BacktestRequest& request, const std::string& period,
                                        const std::vector<BarRecord>& bars, int ticks_processed,
                                        const std::unordered_map<std::string, std::vector<TickRecord>>* ticks_index,
                                        const std::string& match_style);

    StorageEngine& storage_;
    OptionRules option_rules_;
    nlohmann::json strategy_catalog_{nlohmann::json::array()};
    mutable std::mutex mutex_;
    BacktestProgress progress_;
    nlohmann::json last_result_;
};

}  // namespace quant_sev::bll
