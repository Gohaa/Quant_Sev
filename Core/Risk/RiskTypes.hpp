#pragma once

#include <string>
#include <unordered_map>

namespace quant_sev::core {

enum class RiskCheckMode {
    Normal,
    EmergencyClose,
};

struct InstrumentQuoteSnapshot {
    double last_price{0};
    double upper_limit{0};
    double lower_limit{0};
};

struct RiskConfig {
    int order_rate_limit_per_minute{20};
    int min_order_interval_sec{1};
    int order_cancel_limit_per_minute{0};
    int duplicate_order_limit_per_minute{0};
    double order_rate_per_sec{0};
    int order_burst{30};
    int max_rtt_ms{0};
    int latency_pause_sec{60};
    double max_daily_loss{0};
    bool block_limit_touch{false};
    std::unordered_map<std::string, int> max_position;
    bool halt_all_orders{false};
    bool halt_all_strategies{false};
    double max_risk_degree{0.85};
    double max_product_concentration{0.6};
    double initial_capital{1000000.0};
    double contract_multiplier{10.0};
    double margin_rate{0.1};
    int session_reconnect_interval_ms{0};
    int max_net_position{0};
    bool use_ctp_account{true};
};

struct RiskCheckResult {
    bool ok{false};
    std::string code;
    std::string message;
};

}  // namespace quant_sev::core
