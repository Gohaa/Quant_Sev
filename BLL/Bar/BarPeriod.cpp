#include "Bar/BarPeriod.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

namespace quant_sev::bll {

namespace {

bool parse_hms(const std::string& time, int& hour, int& minute, int& second) {
    if (time.size() < 5) {
        return false;
    }
    try {
        hour = std::stoi(time.substr(0, 2));
        minute = std::stoi(time.substr(3, 2));
        second = time.size() >= 8 ? std::stoi(time.substr(6, 2)) : 0;
        return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59 && second >= 0 && second <= 59;
    } catch (...) {
        return false;
    }
}

std::string format_hms(int hour, int minute, int second) {
    char buf[9];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", hour, minute, second);
    return buf;
}

int time_to_minutes(const std::string& time) {
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (!parse_hms(time, hour, minute, second)) {
        return -1;
    }
    return hour * 60 + minute;
}

bool minutes_in_range(int value, int start, int end_inclusive) {
    return value >= start && value <= end_inclusive;
}

}  // namespace

void merge_bar_record(BarRecord& target, const BarRecord& incoming, bool is_first) {
    if (is_first) {
        target = incoming;
        return;
    }
    target.high = std::max(target.high, incoming.high);
    target.low = std::min(target.low, incoming.low);
    target.close = incoming.close;
    target.volume += incoming.volume;
    target.turnover = incoming.turnover;
    target.open_interest = incoming.open_interest;
}

std::string m15_bucket_key(const std::string& date, const std::string& time) {
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (!parse_hms(time, hour, minute, second)) {
        return date + ' ' + time;
    }
    minute = (minute / 15) * 15;
    return date + ' ' + format_hms(hour, minute, 0);
}

std::string h1_label_for_m15(const std::string& m15_time) {
    const int minutes = time_to_minutes(m15_time);
    if (minutes < 0) {
        return m15_time;
    }

    struct Slot {
        int start;
        int end;
        int label_hour;
        int label_minute;
    };

    static const std::array<Slot, 10> kSlots = {{
        {21 * 60, 21 * 60 + 45, 21, 0},
        {22 * 60, 22 * 60 + 45, 22, 0},
        {23 * 60, 23 * 60 + 45, 23, 0},
        {0, 45, 0, 0},
        {1 * 60, 1 * 60 + 45, 1, 0},
        {2 * 60, 2 * 60 + 45, 2, 0},
        {9 * 60, 9 * 60 + 45, 9, 0},
        {10 * 60, 11 * 60, 10, 0},
        {11 * 60 + 15, 14 * 60, 11, 15},
        {14 * 60 + 15, 14 * 60 + 45, 14, 15},
    }};

    for (const auto& slot : kSlots) {
        if (minutes_in_range(minutes, slot.start, slot.end)) {
            return format_hms(slot.label_hour, slot.label_minute, 0);
        }
    }

    const int hour = minutes / 60;
    return format_hms(hour, 0, 0);
}

bool m15_closes_h1(const std::string& m15_time) {
    static const std::array<const char*, 10> kCloseTimes = {
        "21:45:00", "22:45:00", "23:45:00", "00:45:00", "01:45:00", "02:45:00",
        "09:45:00", "11:00:00", "14:00:00", "14:45:00",
    };
    for (const auto* close_time : kCloseTimes) {
        if (m15_time == close_time) {
            return true;
        }
    }
    return false;
}

std::vector<BarRecord> aggregate_bars(const std::vector<BarRecord>& m1_bars, const std::string& period) {
    if (period == "m1" || m1_bars.empty()) {
        return m1_bars;
    }

    struct ActiveBar {
        std::string bucket;
        BarRecord record;
    };

    auto flush = [](std::vector<BarRecord>& out, ActiveBar& slot) {
        if (slot.bucket.empty()) {
            return;
        }
        out.push_back(slot.record);
        slot = {};
    };

    if (period == "d1") {
        std::vector<BarRecord> out;
        ActiveBar slot;
        for (const auto& bar : m1_bars) {
            const bool first = slot.bucket.empty() || slot.bucket != bar.date;
            if (!slot.bucket.empty() && first) {
                flush(out, slot);
            }
            merge_bar_record(slot.record, bar, slot.bucket.empty() || first);
            slot.bucket = bar.date;
            slot.record.time = "00:00:00";
        }
        flush(out, slot);
        return out;
    }

    std::vector<BarRecord> m15_bars;
    {
        ActiveBar slot;
        for (const auto& bar : m1_bars) {
            const std::string bucket = m15_bucket_key(bar.date, bar.time);
            const bool first = slot.bucket.empty() || slot.bucket != bucket;
            if (!slot.bucket.empty() && first) {
                flush(m15_bars, slot);
            }
            merge_bar_record(slot.record, bar, slot.bucket.empty() || first);
            slot.bucket = bucket;
            if (first) {
                slot.record.time = bucket.substr(bucket.find(' ') + 1);
            }
        }
        flush(m15_bars, slot);
    }

    if (period == "m15") {
        return m15_bars;
    }

    if (period == "h1") {
        std::vector<BarRecord> out;
        ActiveBar slot;
        for (const auto& m15 : m15_bars) {
            const std::string label = h1_label_for_m15(m15.time);
            const bool first = slot.bucket.empty() || slot.bucket != label;
            if (!slot.bucket.empty() && first) {
                flush(out, slot);
            }
            merge_bar_record(slot.record, m15, slot.bucket.empty() || first);
            slot.bucket = label;
            slot.record.time = label;
            if (m15_closes_h1(m15.time)) {
                flush(out, slot);
            }
        }
        flush(out, slot);
        return out;
    }

    return m15_bars;
}

}  // namespace quant_sev::bll
