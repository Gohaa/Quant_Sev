#pragma once

#include <chrono>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace quant_sev::core {

struct StrategyDefinition {
    std::string id;
    std::string name;
    std::string period;
    std::string logic{"dual_thrust"};
    std::string default_instrument_id;
    int default_volume{1};
    int min_interval_sec{60};
    int daily_limit{20};
    std::string description;
};

struct StrategyLegRuntime {
    std::string instrument_id;
    int volume{1};
    nlohmann::json params{nlohmann::json::object()};
};

struct StrategyRuntime {
    std::string state{"stopped"};
    std::string user_id;
    std::string instrument_id;
    int volume{1};
    int min_interval_sec{60};
    int daily_limit{20};
    int orders_today{0};
    std::string trading_day;
    std::chrono::steady_clock::time_point last_order_at{};
    bool has_last_order{false};
    std::vector<StrategyLegRuntime> legs;
};

struct CtaOrderView {
    std::string order_ref;
    std::string order_sys_id;
    std::string user_id;
    std::string instrument_id;
    std::string direction;
    std::string offset;
    std::string price_type;
    double price{0};
    int volume{0};
    int volume_traded{0};
    std::string source;
    std::string status;
    std::string message;
    std::string created_at;
};

struct CtaPositionView {
    std::string instrument_id;
    int long_volume{0};
    int short_volume{0};
    int long_today{0};
    int long_yd{0};
    int short_today{0};
    int short_yd{0};
    double avg_long_price{0};
    double avg_short_price{0};
};

struct TradingSignal {
    std::string strategy_id;
    std::string user_id;
    std::string instrument_id;
    std::string direction;
    std::string offset;
    std::string price_type;
    double price{0};
    int volume{0};
};

}  // namespace quant_sev::core
