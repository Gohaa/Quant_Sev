#include "Backtest/TickBarSynthesizer.hpp"

#include <algorithm>

namespace quant_sev::bll {

void TickBarSynthesizer::set_bar_closed_callback(BarClosedCallback callback) {
    bar_closed_ = std::move(callback);
}

std::string TickBarSynthesizer::format_trading_date(const std::string& trading_day) {
    if (trading_day.size() == 8) {
        return trading_day.substr(0, 4) + "/" + trading_day.substr(4, 2) + "/" + trading_day.substr(6, 2);
    }
    return trading_day;
}

std::string TickBarSynthesizer::make_m1_bucket(const TickRecord& tick) {
    std::string minute = tick.update_time;
    if (minute.size() >= 5) {
        minute = minute.substr(0, 5) + ":00";
    }
    return tick.trading_day + ' ' + minute;
}

void TickBarSynthesizer::on_tick(const TickRecord& tick) {
    if (tick.trading_day.empty()) {
        return;
    }

    const std::string bucket = make_m1_bucket(tick);
    const double price = tick.last_price;
    const long long cumulative_volume = tick.volume;
    const double turnover = tick.turnover;
    const long long open_interest = tick.open_interest;

    if (m1_.bucket.empty()) {
        m1_.bucket = bucket;
        m1_.record.date = format_trading_date(tick.trading_day);
        m1_.record.time = bucket.substr(bucket.find(' ') + 1);
        m1_.record.open = price;
        m1_.record.high = price;
        m1_.record.low = price;
        m1_.record.close = price;
        m1_.record.volume = 0;
        m1_.record.turnover = turnover;
        m1_.record.open_interest = open_interest;
        m1_.last_cumulative_volume = cumulative_volume;
        m1_.has_volume = true;
        return;
    }

    if (m1_.bucket != bucket) {
        if (bar_closed_) {
            bar_closed_(m1_.record);
        }

        m1_.bucket = bucket;
        m1_.record = BarRecord{};
        m1_.record.date = format_trading_date(tick.trading_day);
        m1_.record.time = bucket.substr(bucket.find(' ') + 1);
        m1_.record.open = price;
        m1_.record.high = price;
        m1_.record.low = price;
        m1_.record.close = price;
        m1_.record.volume = 0;
        m1_.record.turnover = turnover;
        m1_.record.open_interest = open_interest;
        m1_.last_cumulative_volume = cumulative_volume;
        m1_.has_volume = true;
        return;
    }

    m1_.record.high = std::max(m1_.record.high, price);
    m1_.record.low = std::min(m1_.record.low, price);
    m1_.record.close = price;
    m1_.record.turnover = turnover;
    m1_.record.open_interest = open_interest;
    if (m1_.has_volume && cumulative_volume >= m1_.last_cumulative_volume) {
        m1_.record.volume += cumulative_volume - m1_.last_cumulative_volume;
    }
    m1_.last_cumulative_volume = cumulative_volume;
}

void TickBarSynthesizer::flush() {
    if (m1_.bucket.empty()) {
        return;
    }
    if (bar_closed_) {
        bar_closed_(m1_.record);
    }
    m1_ = {};
}

}  // namespace quant_sev::bll
