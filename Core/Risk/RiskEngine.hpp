#pragma once

#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "Account/AccountRecord.hpp"
#include "Config/Config.hpp"
#include "CTA/CtaTypes.hpp"
#include "Risk/RiskTypes.hpp"
#include "Trade/OrderTypes.hpp"

namespace quant_sev::core {

class RiskEngine {
public:
    bool load(Config& config);
    bool apply_config_patch(const nlohmann::json& patch, Config& config);
    RiskCheckResult check_new_order(const AccountRecord& account, const OrderRequest& request,
                                    const CtaPositionView* position,
                                    const nlohmann::json& positions_view,
                                    const TradingAccountSnapshot* ctp_account = nullptr,
                                    RiskCheckMode mode = RiskCheckMode::Normal,
                                    const InstrumentQuoteSnapshot* quote = nullptr) const;
    void on_order_accepted(const std::string& user_id, const OrderRequest* request = nullptr);
    void on_order_cancel(const std::string& user_id);
    void record_order_rtt(int rtt_ms);
    void clear_latency_pause();
    void record_rejection(const std::string& code);
    void set_halt_all_orders(bool halt);
    void set_halt_all_strategies(bool halt);
    bool halt_all_strategies() const;
    int session_reconnect_interval_ms() const;
    nlohmann::json status_view() const;
    nlohmann::json runtime_view(const std::string& user_id, const nlohmann::json& positions_view,
                                const TradingAccountSnapshot* ctp_account = nullptr) const;

private:
    int max_position_for(const std::string& instrument_id) const;
    int actions_in_last_minute(const std::string& user_id) const;
    int duplicates_in_last_minute(const std::string& user_id, const std::string& fingerprint) const;
    int orders_in_last_minute(const std::string& user_id) const;
    bool is_latency_paused(std::chrono::steady_clock::time_point now) const;
    bool consume_token_bucket(const std::string& user_id, std::chrono::steady_clock::time_point now) const;
    void prune_action_times(const std::string& user_id, std::chrono::steady_clock::time_point now) const;
    static std::string order_fingerprint(const OrderRequest& request);
    RiskCheckResult check_concentration(const OrderRequest& request,
                                        const nlohmann::json& positions_view) const;
    RiskCheckResult check_capital_usage(const OrderRequest& request,
                                        const nlohmann::json& positions_view,
                                        const TradingAccountSnapshot* ctp_account) const;
    RiskCheckResult check_limit_touch(const OrderRequest& request,
                                      const InstrumentQuoteSnapshot* quote) const;
    RiskCheckResult check_daily_loss(const TradingAccountSnapshot* ctp_account) const;
    double estimate_order_margin(const OrderRequest& request) const;
    static std::string product_key(const std::string& instrument_id);

    mutable std::mutex mutex_;
    RiskConfig config_;
    mutable std::unordered_map<std::string, std::deque<std::chrono::steady_clock::time_point>> order_times_by_user_;
    mutable std::unordered_map<std::string, std::deque<std::chrono::steady_clock::time_point>> action_times_by_user_;
    mutable std::unordered_map<std::string, std::deque<std::pair<std::string, std::chrono::steady_clock::time_point>>>
        duplicate_events_by_user_;
    mutable std::unordered_map<std::string, std::chrono::steady_clock::time_point> last_order_by_user_;
    mutable std::unordered_map<std::string, double> token_bucket_tokens_by_user_;
    mutable std::unordered_map<std::string, std::chrono::steady_clock::time_point> token_bucket_last_refill_by_user_;
    mutable std::unordered_map<std::string, int> reject_counts_;
    mutable std::chrono::steady_clock::time_point latency_paused_until_{};
    mutable int last_rtt_ms_{0};
};

}  // namespace quant_sev::core
