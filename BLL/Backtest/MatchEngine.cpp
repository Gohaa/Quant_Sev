#include "Backtest/MatchEngine.hpp"

#include <cmath>

namespace quant_sev::bll {

MatchEngine::MatchEngine(double initial_capital, int volume, double contract_multiplier, double fee_rate,
                         double fee_per_lot, double tick_size, double slippage_ticks, double margin_rate)
    : equity_(initial_capital),
      volume_(volume > 0 ? volume : 1),
      contract_multiplier_(contract_multiplier > 0 ? contract_multiplier : 10),
      fee_rate_(fee_rate >= 0 ? fee_rate : 0),
      fee_per_lot_(fee_per_lot >= 0 ? fee_per_lot : 0),
      tick_size_(tick_size > 0 ? tick_size : 0),
      slippage_ticks_(slippage_ticks >= 0 ? slippage_ticks : 0),
      margin_rate_(margin_rate > 0 ? margin_rate : 0) {}

double MatchEngine::apply_slippage(double price, bool is_buy) const {
    if (slippage_ticks_ <= 0 || tick_size_ <= 0) {
        return price;
    }
    const double slip = slippage_ticks_ * tick_size_;
    return is_buy ? price + slip : price - slip;
}

double MatchEngine::calc_fee(double price) const {
    double fee = 0;
    if (fee_rate_ > 0) {
        fee += std::abs(price) * volume_ * contract_multiplier_ * fee_rate_;
    }
    if (fee_per_lot_ > 0) {
        fee += fee_per_lot_ * volume_;
    }
    return fee;
}

double MatchEngine::calc_margin(double price) const {
    if (margin_rate_ <= 0) {
        return 0;
    }
    return std::abs(price) * volume_ * contract_multiplier_ * margin_rate_;
}

double MatchEngine::calc_pnl(double entry, double exit, int direction) const {
    return (exit - entry) * direction * volume_ * contract_multiplier_;
}

double MatchEngine::mark_to_market(double price) const {
    if (position_ == 0) {
        return equity_;
    }
    return equity_ + calc_pnl(entry_price_, price, position_);
}

void MatchEngine::record_slippage(double raw_price, double fill_price) {
    if (slippage_ticks_ <= 0 || tick_size_ <= 0) {
        return;
    }
    slippage_cost_ += std::abs(fill_price - raw_price) * volume_ * contract_multiplier_;
}

void MatchEngine::update_peak_margin() {
    if (position_ == 0) {
        return;
    }
    peak_margin_ = std::max(peak_margin_, calc_margin(entry_price_));
}

bool MatchEngine::can_open(double price) const {
    if (margin_rate_ <= 0) {
        return true;
    }
    const double required = calc_margin(price) + calc_fee(price);
    return required <= equity_;
}

void MatchEngine::close_position(int bar_index, double price, const std::string& reason) {
    if (position_ == 0) {
        return;
    }

    const bool is_buy = position_ < 0;
    const double fill = apply_slippage(price, is_buy);
    record_slippage(price, fill);
    const double open_fee = calc_fee(entry_price_);
    const double close_fee = calc_fee(fill);

    BacktestTrade trade;
    trade.side = entry_side_;
    trade.open_bar = entry_bar_;
    trade.close_bar = bar_index;
    trade.open_price = entry_price_;
    trade.close_price = fill;
    trade.bars_held = bar_index - entry_bar_;
    trade.fee = open_fee + close_fee;
    trade.slippage_cost = entry_slippage_ + std::abs(fill - price) * volume_ * contract_multiplier_;
    trade.pnl = calc_pnl(entry_price_, fill, position_) - trade.fee;
    equity_ += trade.pnl;
    trades_.push_back(trade);
    position_ = 0;
    entry_price_ = 0;
    entry_bar_ = 0;
    entry_side_.clear();
    entry_slippage_ = 0;
    (void)reason;
}

void MatchEngine::on_signal(const StrategyBarSignal& signal, const BarRecord& bar) {
    equity_curve_.push_back(equity_);

    if (signal.action == "close_long" || signal.action == "close_short") {
        close_position(signal.bar_index, signal.price, signal.label);
        return;
    }

    if (signal.action == "buy") {
        if (position_ < 0) {
            close_position(signal.bar_index, signal.price, "reverse");
        }
        if (position_ == 0) {
            const double fill = apply_slippage(signal.price, true);
            record_slippage(signal.price, fill);
            entry_slippage_ = std::abs(fill - signal.price) * volume_ * contract_multiplier_;
            if (!can_open(fill)) {
                margin_rejects_ += 1;
                return;
            }
            const double fee = calc_fee(fill);
            position_ = 1;
            entry_price_ = fill;
            entry_bar_ = signal.bar_index;
            entry_side_ = "long";
            equity_ -= fee;
            update_peak_margin();
        }
        return;
    }

    if (signal.action == "sell") {
        if (position_ > 0) {
            close_position(signal.bar_index, signal.price, "reverse");
        }
        if (position_ == 0) {
            const double fill = apply_slippage(signal.price, false);
            record_slippage(signal.price, fill);
            entry_slippage_ = std::abs(fill - signal.price) * volume_ * contract_multiplier_;
            if (!can_open(fill)) {
                margin_rejects_ += 1;
                return;
            }
            const double fee = calc_fee(fill);
            position_ = -1;
            entry_price_ = fill;
            entry_bar_ = signal.bar_index;
            entry_side_ = "short";
            equity_ -= fee;
            update_peak_margin();
        }
    }
    (void)bar;
}

void MatchEngine::finalize(int last_bar_index, double last_price) {
    if (position_ != 0) {
        close_position(last_bar_index, last_price, "finalize");
    }
    equity_curve_.push_back(equity_);
}

}  // namespace quant_sev::bll
