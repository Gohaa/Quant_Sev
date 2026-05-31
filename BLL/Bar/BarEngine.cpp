#include "Bar/BarEngine.hpp"

#include <algorithm>

#include "Bar/BarPeriod.hpp"

namespace quant_sev::bll {

BarEngine::BarEngine(StorageEngine& storage) : storage_(storage) {}

void BarEngine::set_bar_closed_callback(BarClosedCallback callback) {
    bar_closed_ = std::move(callback);
}

void BarEngine::emit_bar_closed(const std::string& instrument_id, const BarRecord& bar,
                                const std::string& period) {
    if (bar_closed_) {
        bar_closed_(instrument_id, bar, period);
    }
}

std::string BarEngine::format_trading_date(const std::string& trading_day) const {
    if (trading_day.size() == 8) {
        return trading_day.substr(0, 4) + "/" + trading_day.substr(4, 2) + "/" + trading_day.substr(6, 2);
    }
    return trading_day;
}

std::string BarEngine::make_m1_bucket(const nlohmann::json& tick) const {
    const std::string trading_day = tick.value("trading_day", "");
    const std::string update_time = tick.value("update_time", "00:00:00");
    std::string minute = update_time;
    if (minute.size() >= 5) {
        minute = minute.substr(0, 5) + ":00";
    }
    return trading_day + ' ' + minute;
}

void BarEngine::flush_m15(const std::string& instrument_id, const ParsedInstrument& parsed,
                          InstrumentState& state) {
    if (state.m15.bucket.empty()) {
        return;
    }
    storage_.append_bar(parsed, "m15", state.m15.record);
    emit_bar_closed(instrument_id, state.m15.record, "m15");

    const std::string h1_label = h1_label_for_m15(state.m15.record.time);
    const bool first_h1 = state.h1.bucket.empty() || state.h1.bucket != h1_label;
    merge_bar_record(state.h1.record, state.m15.record, first_h1);
    state.h1.bucket = h1_label;
    state.h1.record.time = h1_label;

    if (m15_closes_h1(state.m15.record.time)) {
        flush_h1(instrument_id, parsed, state);
    }

    state.m15 = {};
}

void BarEngine::flush_h1(const std::string& instrument_id, const ParsedInstrument& parsed,
                         InstrumentState& state) {
    if (state.h1.bucket.empty()) {
        return;
    }
    storage_.append_bar(parsed, "h1", state.h1.record);
    emit_bar_closed(instrument_id, state.h1.record, "h1");
    state.h1 = {};
    (void)instrument_id;
}

void BarEngine::flush_d1(const std::string& instrument_id, const ParsedInstrument& parsed,
                         InstrumentState& state) {
    if (state.d1.bucket.empty()) {
        return;
    }
    storage_.append_bar(parsed, "d1", state.d1.record);
    emit_bar_closed(instrument_id, state.d1.record, "d1");
    state.d1 = {};
    (void)instrument_id;
}

void BarEngine::on_m1_closed(const std::string& instrument_id, const ParsedInstrument& parsed,
                             const BarRecord& bar) {
    storage_.append_bar(parsed, "m1", bar);
    emit_bar_closed(instrument_id, bar, "m1");

    auto& state = active_[instrument_id];

    const std::string m15_bucket = m15_bucket_key(bar.date, bar.time);
    const bool first_m15 = state.m15.bucket.empty() || state.m15.bucket != m15_bucket;
    if (first_m15 && !state.m15.bucket.empty()) {
        flush_m15(instrument_id, parsed, state);
    }
    merge_bar_record(state.m15.record, bar, state.m15.bucket.empty() || first_m15);
    state.m15.bucket = m15_bucket;
    if (first_m15) {
        state.m15.record.time = m15_bucket.substr(m15_bucket.find(' ') + 1);
    }

    const std::string d1_bucket = bar.date;
    const bool first_d1 = state.d1.bucket.empty() || state.d1.bucket != d1_bucket;
    if (first_d1 && !state.d1.bucket.empty()) {
        flush_d1(instrument_id, parsed, state);
    }
    merge_bar_record(state.d1.record, bar, state.d1.bucket.empty() || first_d1);
    state.d1.bucket = d1_bucket;
    state.d1.record.time = "00:00:00";
}

void BarEngine::on_tick(const nlohmann::json& tick) {
    const std::string instrument_id = tick.value("instrument_id", "");
    if (instrument_id.empty()) {
        return;
    }

    storage_.write_tick(tick);

    const std::string bucket = make_m1_bucket(tick);
    const double price = tick.value("last_price", 0.0);
    const long long cumulative_volume = tick.value("volume", 0);
    const double turnover = tick.value("turnover", 0.0);
    const long long open_interest = tick.value("open_interest", 0);

    auto& slot = active_[instrument_id].m1;
    if (slot.bucket.empty()) {
        slot.bucket = bucket;
        slot.record.date = format_trading_date(tick.value("trading_day", ""));
        slot.record.time = bucket.substr(bucket.find(' ') + 1);
        slot.record.open = price;
        slot.record.high = price;
        slot.record.low = price;
        slot.record.close = price;
        slot.record.volume = 0;
        slot.record.turnover = turnover;
        slot.record.open_interest = open_interest;
        slot.last_cumulative_volume = cumulative_volume;
        slot.has_volume = true;
        return;
    }

    if (slot.bucket != bucket) {
        const auto parsed = storage_.parse_instrument(instrument_id);
        if (parsed) {
            on_m1_closed(instrument_id, *parsed, slot.record);
        }

        slot.bucket = bucket;
        slot.record = BarRecord{};
        slot.record.date = format_trading_date(tick.value("trading_day", ""));
        slot.record.time = bucket.substr(bucket.find(' ') + 1);
        slot.record.open = price;
        slot.record.high = price;
        slot.record.low = price;
        slot.record.close = price;
        slot.record.volume = 0;
        slot.record.turnover = turnover;
        slot.record.open_interest = open_interest;
        slot.last_cumulative_volume = cumulative_volume;
        slot.has_volume = true;
        return;
    }

    slot.record.high = std::max(slot.record.high, price);
    slot.record.low = std::min(slot.record.low, price);
    slot.record.close = price;
    slot.record.turnover = turnover;
    slot.record.open_interest = open_interest;
    if (slot.has_volume && cumulative_volume >= slot.last_cumulative_volume) {
        slot.record.volume += cumulative_volume - slot.last_cumulative_volume;
    }
    slot.last_cumulative_volume = cumulative_volume;
}

}  // namespace quant_sev::bll
