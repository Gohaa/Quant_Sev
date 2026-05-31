#pragma once

#include <vector>

#include "Backtest/BacktestTypes.hpp"
#include "Storage/StorageTypes.hpp"
#include "Strategy/StrategyTypes.hpp"

namespace quant_sev::bll {

class MatchEngine {
public:
    MatchEngine(double initial_capital, int volume, double contract_multiplier, double fee_rate, double fee_per_lot,
                double tick_size, double slippage_ticks, double margin_rate);

    void on_signal(const StrategyBarSignal& signal, const BarRecord& bar);
    void finalize(int last_bar_index, double last_price);

    double equity() const { return equity_; }
    /** 按 mark price 计入浮动盈亏的权益（用于逐 Bar 权益曲线） */
    double mark_to_market(double price) const;
    double peak_margin() const { return peak_margin_; }
    int margin_rejects() const { return margin_rejects_; }
    const std::vector<BacktestTrade>& trades() const { return trades_; }
    const std::vector<double>& equity_curve() const { return equity_curve_; }
    double total_slippage_cost() const { return slippage_cost_; }

private:
    void record_slippage(double raw_price, double fill_price);
    void close_position(int bar_index, double price, const std::string& reason);
    bool can_open(double price) const;
    double apply_slippage(double price, bool is_buy) const;
    double calc_pnl(double entry, double exit, int direction) const;
    double calc_fee(double price) const;
    double calc_margin(double price) const;
    void update_peak_margin();

    double equity_;
    int volume_;
    double contract_multiplier_;
    double fee_rate_;
    double fee_per_lot_;
    double tick_size_;
    double slippage_ticks_;
    double margin_rate_;
    double peak_margin_{0};
    int margin_rejects_{0};
    int position_{0};
    double entry_price_{0};
    int entry_bar_{0};
    std::string entry_side_;
    std::vector<BacktestTrade> trades_;
    std::vector<double> equity_curve_;
    double slippage_cost_{0};
    double entry_slippage_{0};
};

}  // namespace quant_sev::bll
