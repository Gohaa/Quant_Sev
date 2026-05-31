#pragma once

#include <string>
#include <vector>

namespace quant_sev::core {

struct TradingSession {
    std::string start;
    std::string end;
    std::string label;
};

struct TimeCheckConfig {
    std::vector<TradingSession> trading_sessions;
    bool allow_manual_outside_session{false};
};

struct TimeCheckStatus {
    bool in_session{false};
    std::string phase{"closed"};
    std::string current_time;
    std::string message;
};

}  // namespace quant_sev::core
