#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace quant_sev::bll {

struct BacktestRequest {
    std::string instrument_id;
    std::vector<std::string> instrument_ids;
    std::string period{"m15"};
    std::string strategy{"dual_thrust"};
    std::vector<std::string> strategies;
    std::vector<std::string> strategy_ids;
    std::string mode{"bar"};
    bool intra_bar{true};
    double initial_capital{1000000};
    int limit{50000};
    int tick_limit{200000};
    int days{4};
    double k1{0.5};
    double k2{0.5};
    int ma_fast{5};
    int ma_slow{20};
    int macd_short{12};
    int macd_long{26};
    int macd_signal{9};
    int boll_period{20};
    double boll_stddev{2.0};
    int volume{1};
    double contract_multiplier{0};
    double tick_size{0};
    double slippage_ticks{0};
    double fee_rate{0.0001};
    double fee_per_lot{0};
    double margin_rate{0.1};
    std::string start_date;
    std::string end_date;
    /** 参数优化：total_return | win_rate | min_drawdown */
    std::string optimize_metric{"total_return"};
    nlohmann::json optimize_grid = nlohmann::json::object();
    /** 参数优化：当前组合值（key -> value） */
    nlohmann::json optimize_combo = nlohmann::json::object();
    /** 轻量输出：仅 summary，用于参数优化网格 */
    bool lite_output{false};
};

struct BacktestTrade {
    std::string side;
    int open_bar{0};
    int close_bar{0};
    double open_price{0};
    double close_price{0};
    double pnl{0};
    double fee{0};
    double slippage_cost{0};
    int bars_held{0};
};

}  // namespace quant_sev::bll
