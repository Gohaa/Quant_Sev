#include "TimeCheck/TimeCheckEngine.hpp"

#include <chrono>
#include <iomanip>
#include <sstream>

namespace quant_sev::core {

namespace {

int parse_hms_to_seconds(const std::string& time) {
    if (time.size() < 5) {
        return -1;
    }
    try {
        const int hour = std::stoi(time.substr(0, 2));
        const int minute = std::stoi(time.substr(3, 2));
        const int second = time.size() >= 8 ? std::stoi(time.substr(6, 2)) : 0;
        if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
            return -1;
        }
        return hour * 3600 + minute * 60 + second;
    } catch (...) {
        return -1;
    }
}

std::string now_hms_text() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm{};
#ifdef _WIN32
    localtime_s(&local_tm, &tt);
#else
    localtime_r(&tt, &local_tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&local_tm, "%H:%M:%S");
    return oss.str();
}

int now_seconds_of_day() {
    return parse_hms_to_seconds(now_hms_text());
}

}  // namespace

bool TimeCheckEngine::load(Config& config) {
    config_ = TimeCheckConfig{};
    const auto doc = config.read_json_file("Risk.json");
    config_.allow_manual_outside_session = doc.value("allow_manual_outside_session", false);
    if (!doc.contains("trading_sessions") || !doc["trading_sessions"].is_array()) {
        return true;
    }
    for (const auto& item : doc["trading_sessions"]) {
        TradingSession session;
        session.start = item.value("start", "");
        session.end = item.value("end", "");
        session.label = item.value("label", "session");
        if (session.start.empty() || session.end.empty()) {
            continue;
        }
        config_.trading_sessions.push_back(std::move(session));
    }
    return true;
}

TimeCheckStatus TimeCheckEngine::status() const {
    TimeCheckStatus result;
    result.current_time = now_hms_text();
    if (config_.trading_sessions.empty()) {
        result.in_session = true;
        result.phase = "always_open";
        result.message = "未配置交易时段，默认允许";
        return result;
    }

    const int now_sec = now_seconds_of_day();
    if (now_sec < 0) {
        result.message = "无法解析本地时间";
        return result;
    }

    for (const auto& session : config_.trading_sessions) {
        const int start = parse_hms_to_seconds(session.start);
        const int end = parse_hms_to_seconds(session.end);
        if (start < 0 || end < 0) {
            continue;
        }
        if (now_sec >= start && now_sec <= end) {
            result.in_session = true;
            result.phase = session.label.empty() ? "trading" : session.label;
            result.message = "当前处于交易时段 " + session.start + "-" + session.end;
            return result;
        }
    }

    result.phase = "closed";
    result.message = "当前非交易时段";
    return result;
}

ConnectResult TimeCheckEngine::check(bool manual_order) const {
    const auto current = status();
    if (current.in_session) {
        return {true, current.message};
    }
    if (manual_order && config_.allow_manual_outside_session) {
        return {true, "人工报单允许非交易时段（配置）"};
    }
    return {false, current.message.empty() ? "当前非交易时段" : current.message};
}

}  // namespace quant_sev::core
