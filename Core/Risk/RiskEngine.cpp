#include "Risk/RiskEngine.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

#include "Logger/Logger.hpp"

namespace quant_sev::core {

namespace {

bool is_open_offset(const std::string& offset) {
    return offset == "open" || offset.empty();
}

nlohmann::json positions_array(const nlohmann::json& positions_view) {
    if (positions_view.contains("positions") && positions_view["positions"].is_array()) {
        return positions_view["positions"];
    }
    return nlohmann::json::array();
}

constexpr double kLimitTouchBand = 0.001;

}  // namespace

bool RiskEngine::load(Config& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = RiskConfig{};
    const auto doc = config.read_json_file("Risk.json");
    config_.order_rate_limit_per_minute = doc.value("order_rate_limit", doc.value("order_rate_limit_per_minute", 20));
    config_.min_order_interval_sec = doc.value("min_order_interval_sec", 1);
    config_.order_cancel_limit_per_minute =
        doc.value("order_cancel_limit", doc.value("order_cancel_limit_per_minute", 0));
    config_.duplicate_order_limit_per_minute =
        doc.value("duplicate_order_limit", doc.value("duplicate_order_limit_per_minute", 0));
    config_.order_rate_per_sec = doc.value("order_rate_per_sec", 0.0);
    config_.order_burst = doc.value("order_burst", 30);
    config_.max_rtt_ms = doc.value("max_rtt_ms", 0);
    config_.latency_pause_sec = doc.value("latency_pause_sec", 60);
    config_.max_daily_loss = doc.value("max_daily_loss", 0.0);
    config_.block_limit_touch = doc.value("block_limit_touch", false);
    config_.max_risk_degree = doc.value("max_risk_degree", 0.85);
    config_.max_product_concentration = doc.value("max_product_concentration", 0.6);
    config_.initial_capital = doc.value("initial_capital", 1000000.0);
    config_.contract_multiplier = doc.value("contract_multiplier", doc.value("contract_multiplier_default", 10.0));
    config_.margin_rate = doc.value("margin_rate", doc.value("margin_rate_default", 0.1));
    config_.session_reconnect_interval_ms =
        doc.value("session_reconnect_interval_ms", doc.value("session_reconnect_ms", 0));
    config_.max_net_position = doc.value("max_net_position", 0);
    config_.use_ctp_account = doc.value("use_ctp_account", true);
    if (doc.contains("emergency") && doc["emergency"].is_object()) {
        config_.halt_all_orders = doc["emergency"].value("halt_all_orders", false);
        config_.halt_all_strategies = doc["emergency"].value("halt_all_strategies", false);
    }
    if (doc.contains("max_position") && doc["max_position"].is_object()) {
        for (const auto& [key, value] : doc["max_position"].items()) {
            if (value.is_number_integer()) {
                config_.max_position[key] = value.get<int>();
            }
        }
    }
    Logger::instance().info("Risk 配置已加载: rate=" + std::to_string(config_.order_rate_limit_per_minute) +
                            "/min risk_deg=" + std::to_string(config_.max_risk_degree));
    return true;
}

bool RiskEngine::apply_config_patch(const nlohmann::json& patch, Config& config) {
    auto doc = config.read_json_file("Risk.json");
    if (patch.contains("order_rate_limit")) {
        doc["order_rate_limit"] = patch["order_rate_limit"];
    }
    if (patch.contains("order_rate_limit_per_minute")) {
        doc["order_rate_limit"] = patch["order_rate_limit_per_minute"];
    }
    if (patch.contains("min_order_interval_sec")) {
        doc["min_order_interval_sec"] = patch["min_order_interval_sec"];
    }
    if (patch.contains("order_cancel_limit") || patch.contains("order_cancel_limit_per_minute")) {
        doc["order_cancel_limit"] = patch.value("order_cancel_limit", patch.value("order_cancel_limit_per_minute", 0));
    }
    if (patch.contains("duplicate_order_limit") || patch.contains("duplicate_order_limit_per_minute")) {
        doc["duplicate_order_limit"] =
            patch.value("duplicate_order_limit", patch.value("duplicate_order_limit_per_minute", 0));
    }
    if (patch.contains("order_rate_per_sec")) {
        doc["order_rate_per_sec"] = patch["order_rate_per_sec"];
    }
    if (patch.contains("order_burst")) {
        doc["order_burst"] = patch["order_burst"];
    }
    if (patch.contains("max_rtt_ms")) {
        doc["max_rtt_ms"] = patch["max_rtt_ms"];
    }
    if (patch.contains("latency_pause_sec")) {
        doc["latency_pause_sec"] = patch["latency_pause_sec"];
    }
    if (patch.contains("max_daily_loss")) {
        doc["max_daily_loss"] = patch["max_daily_loss"];
    }
    if (patch.contains("block_limit_touch")) {
        doc["block_limit_touch"] = patch["block_limit_touch"];
    }
    if (patch.contains("max_risk_degree")) {
        doc["max_risk_degree"] = patch["max_risk_degree"];
    }
    if (patch.contains("max_product_concentration")) {
        doc["max_product_concentration"] = patch["max_product_concentration"];
    }
    if (patch.contains("initial_capital")) {
        doc["initial_capital"] = patch["initial_capital"];
    }
    if (patch.contains("max_net_position")) {
        doc["max_net_position"] = patch["max_net_position"];
    }
    if (patch.contains("max_position") && patch["max_position"].is_object()) {
        if (!doc.contains("max_position") || !doc["max_position"].is_object()) {
            doc["max_position"] = nlohmann::json::object();
        }
        for (const auto& [key, value] : patch["max_position"].items()) {
            doc["max_position"][key] = value;
        }
    }
    if (patch.contains("emergency") && patch["emergency"].is_object()) {
        if (!doc.contains("emergency") || !doc["emergency"].is_object()) {
            doc["emergency"] = nlohmann::json::object();
        }
        for (const auto& [key, value] : patch["emergency"].items()) {
            doc["emergency"][key] = value;
        }
    }
    if (patch.contains("margin_rate")) {
        doc["margin_rate"] = patch["margin_rate"];
    }
    if (patch.contains("session_reconnect_interval_ms")) {
        doc["session_reconnect_interval_ms"] = patch["session_reconnect_interval_ms"];
    }
    if (patch.contains("session_reconnect_ms")) {
        doc["session_reconnect_interval_ms"] = patch["session_reconnect_ms"];
    }
    if (patch.contains("use_ctp_account")) {
        doc["use_ctp_account"] = patch["use_ctp_account"];
    }
    if (!config.write_json_file("Risk.json", doc)) {
        return false;
    }
    return load(config);
}

std::string RiskEngine::product_key(const std::string& instrument_id) {
    std::string key;
    key.reserve(instrument_id.size());
    for (char ch : instrument_id) {
        key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    while (!key.empty() && std::isdigit(static_cast<unsigned char>(key.back()))) {
        key.pop_back();
    }
    return key;
}

std::string RiskEngine::order_fingerprint(const OrderRequest& request) {
    std::ostringstream oss;
    oss << request.instrument_id << '|' << request.direction << '|' << request.offset << '|' << request.price << '|'
        << request.volume;
    return oss.str();
}

int RiskEngine::max_position_for(const std::string& instrument_id) const {
    const std::string product = product_key(instrument_id);
    const auto product_it = config_.max_position.find(product);
    if (product_it != config_.max_position.end()) {
        return product_it->second;
    }
    const auto default_it = config_.max_position.find("default");
    if (default_it != config_.max_position.end()) {
        return default_it->second;
    }
    return 0;
}

void RiskEngine::prune_action_times(const std::string& user_id, std::chrono::steady_clock::time_point now) const {
    const auto window_start = now - std::chrono::minutes(1);
    auto& times = action_times_by_user_[user_id];
    while (!times.empty() && times.front() < window_start) {
        times.pop_front();
    }
}

int RiskEngine::actions_in_last_minute(const std::string& user_id) const {
    if (user_id.empty()) {
        return 0;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto it = action_times_by_user_.find(user_id);
    if (it == action_times_by_user_.end()) {
        return 0;
    }
    const auto window_start = now - std::chrono::minutes(1);
    int count = 0;
    for (const auto& ts : it->second) {
        if (ts >= window_start) {
            count += 1;
        }
    }
    return count;
}

int RiskEngine::duplicates_in_last_minute(const std::string& user_id, const std::string& fingerprint) const {
    if (user_id.empty() || fingerprint.empty()) {
        return 0;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto window_start = now - std::chrono::minutes(1);
    const auto it = duplicate_events_by_user_.find(user_id);
    if (it == duplicate_events_by_user_.end()) {
        return 0;
    }
    int count = 0;
    for (const auto& [fp, ts] : it->second) {
        if (fp == fingerprint && ts >= window_start) {
            count += 1;
        }
    }
    return count;
}

int RiskEngine::orders_in_last_minute(const std::string& user_id) const {
    if (user_id.empty()) {
        return 0;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto window_start = now - std::chrono::minutes(1);
    const auto it = order_times_by_user_.find(user_id);
    if (it == order_times_by_user_.end()) {
        return 0;
    }
    int count = 0;
    for (const auto& ts : it->second) {
        if (ts >= window_start) {
            count += 1;
        }
    }
    return count;
}

bool RiskEngine::is_latency_paused(std::chrono::steady_clock::time_point now) const {
    return latency_paused_until_.time_since_epoch().count() > 0 && now < latency_paused_until_;
}

bool RiskEngine::consume_token_bucket(const std::string& user_id, std::chrono::steady_clock::time_point now) const {
    if (config_.order_rate_per_sec <= 0) {
        return true;
    }
    const int burst = config_.order_burst > 0 ? config_.order_burst : static_cast<int>(config_.order_rate_per_sec);
    auto& tokens = token_bucket_tokens_by_user_[user_id];
    auto& last_refill = token_bucket_last_refill_by_user_[user_id];
    if (last_refill.time_since_epoch().count() == 0) {
        tokens = static_cast<double>(burst);
        last_refill = now;
    } else {
        const auto elapsed = std::chrono::duration<double>(now - last_refill).count();
        tokens = std::min(static_cast<double>(burst), tokens + elapsed * config_.order_rate_per_sec);
        last_refill = now;
    }
    if (tokens < 1.0) {
        return false;
    }
    tokens -= 1.0;
    return true;
}

RiskCheckResult RiskEngine::check_limit_touch(const OrderRequest& request,
                                              const InstrumentQuoteSnapshot* quote) const {
    RiskCheckResult result;
    if (!config_.block_limit_touch || quote == nullptr) {
        result.ok = true;
        result.message = "ok";
        return result;
    }
    if (!is_open_offset(request.offset)) {
        result.ok = true;
        result.message = "ok";
        return result;
    }
    double px = request.price > 0 ? request.price : quote->last_price;
    if (px <= 0 || quote->upper_limit <= 0 || quote->lower_limit <= 0) {
        result.ok = true;
        result.message = "ok";
        return result;
    }
    if (px >= quote->upper_limit * (1.0 - kLimitTouchBand) || px <= quote->lower_limit * (1.0 + kLimitTouchBand)) {
        result.code = "limit_touch";
        result.message = "涨跌停附近禁止报单: 价格 " + std::to_string(px);
        return result;
    }
    result.ok = true;
    result.message = "ok";
    return result;
}

RiskCheckResult RiskEngine::check_daily_loss(const TradingAccountSnapshot* ctp_account) const {
    RiskCheckResult result;
    if (config_.max_daily_loss <= 0 || ctp_account == nullptr) {
        result.ok = true;
        result.message = "ok";
        return result;
    }
    const double daily_pnl = ctp_account->close_profit + ctp_account->position_profit;
    if (daily_pnl < -config_.max_daily_loss) {
        result.code = "daily_loss";
        result.message = "日亏损超限: " + std::to_string(static_cast<int>(daily_pnl)) + " < -" +
                         std::to_string(static_cast<int>(config_.max_daily_loss));
        return result;
    }
    result.ok = true;
    result.message = "ok";
    return result;
}

RiskCheckResult RiskEngine::check_concentration(const OrderRequest& request,
                                                const nlohmann::json& positions_view) const {
    RiskCheckResult result;
    if (!is_open_offset(request.offset)) {
        result.ok = true;
        result.message = "ok";
        return result;
    }
    if (config_.max_product_concentration <= 0 || config_.max_product_concentration >= 1.0) {
        result.ok = true;
        result.message = "ok";
        return result;
    }

    const std::string product = product_key(request.instrument_id);
    int total_lots = 0;
    int product_lots = 0;
    for (const auto& row : positions_array(positions_view)) {
        const int lots = row.value("long", 0) + row.value("short", 0);
        total_lots += lots;
        const std::string inst = row.value("instrument_id", "");
        if (product_key(inst) == product) {
            product_lots += lots;
        }
    }
    product_lots += request.volume;
    total_lots += request.volume;
    if (total_lots <= 0) {
        result.ok = true;
        result.message = "ok";
        return result;
    }

    const double ratio = static_cast<double>(product_lots) / static_cast<double>(total_lots);
    if (ratio > config_.max_product_concentration) {
        result.code = "concentration";
        result.message = "品种集中度超限: " + product + " " + std::to_string(static_cast<int>(ratio * 100)) +
                         "% > " + std::to_string(static_cast<int>(config_.max_product_concentration * 100)) + "%";
        return result;
    }

    result.ok = true;
    result.message = "ok";
    return result;
}

RiskCheckResult RiskEngine::check_capital_usage(const OrderRequest& request,
                                                const nlohmann::json& positions_view,
                                                const TradingAccountSnapshot* ctp_account) const {
    RiskCheckResult result;

    int net_lots = 0;
    for (const auto& row : positions_array(positions_view)) {
        const int long_vol = row.value("long", 0);
        const int short_vol = row.value("short", 0);
        net_lots += std::abs(long_vol - short_vol);
    }
    if (is_open_offset(request.offset)) {
        net_lots += request.volume;
    }
    if (config_.max_net_position > 0 && net_lots > config_.max_net_position) {
        result.code = "net_position";
        result.message = "净持仓超限: " + std::to_string(net_lots) + " > " +
                         std::to_string(config_.max_net_position);
        return result;
    }

    if (config_.use_ctp_account && ctp_account != nullptr && ctp_account->balance > 0) {
        const double equity = ctp_account->balance;
        const double margin_used = ctp_account->curr_margin + ctp_account->frozen_margin;
        double additional_margin = 0;
        if (is_open_offset(request.offset)) {
            additional_margin = estimate_order_margin(request);
            if (additional_margin > 0 && ctp_account->available + 1e-6 < additional_margin) {
                result.code = "available";
                result.message = "可用资金不足: 可用 " + std::to_string(static_cast<int>(ctp_account->available)) +
                                 " < 预估保证金 " + std::to_string(static_cast<int>(additional_margin));
                return result;
            }
        }

        if (config_.max_risk_degree > 0) {
            const double projected_margin = margin_used + additional_margin;
            const double risk_degree = projected_margin / equity;
            if (risk_degree > config_.max_risk_degree) {
                result.code = "capital";
                result.message = "CTP 风险度超限: " + std::to_string(static_cast<int>(risk_degree * 100)) + "% > " +
                                 std::to_string(static_cast<int>(config_.max_risk_degree * 100)) + "%";
                return result;
            }
        }

        result.ok = true;
        result.message = "ok";
        return result;
    }

    if (config_.initial_capital <= 0 || config_.max_risk_degree <= 0) {
        result.ok = true;
        result.message = "ok";
        return result;
    }

    double notional = 0;
    for (const auto& row : positions_array(positions_view)) {
        const int long_vol = row.value("long", 0);
        const int short_vol = row.value("short", 0);
        const double avg_long = row.value("avg_long_price", 0.0);
        const double avg_short = row.value("avg_short_price", 0.0);
        notional += long_vol * avg_long + short_vol * avg_short;
    }

    if (is_open_offset(request.offset)) {
        const double px = request.price > 0 ? request.price : 0;
        if (px > 0) {
            notional += request.volume * px;
        }
    }

    const double margin_used = notional * config_.contract_multiplier * config_.margin_rate;
    const double risk_degree = margin_used / config_.initial_capital;
    if (risk_degree > config_.max_risk_degree) {
        result.code = "capital";
        result.message = "风险度超限: " + std::to_string(static_cast<int>(risk_degree * 100)) + "% > " +
                         std::to_string(static_cast<int>(config_.max_risk_degree * 100)) + "%";
        return result;
    }

    result.ok = true;
    result.message = "ok";
    return result;
}

double RiskEngine::estimate_order_margin(const OrderRequest& request) const {
    if (!is_open_offset(request.offset) || request.volume <= 0) {
        return 0;
    }
    const double px = request.price > 0 ? request.price : 0;
    if (px <= 0) {
        return 0;
    }
    return request.volume * px * config_.contract_multiplier * config_.margin_rate;
}

RiskCheckResult RiskEngine::check_new_order(const AccountRecord& account, const OrderRequest& request,
                                            const CtaPositionView* position,
                                            const nlohmann::json& positions_view,
                                            const TradingAccountSnapshot* ctp_account, RiskCheckMode mode,
                                            const InstrumentQuoteSnapshot* quote) const {
    RiskCheckResult result;
    if (account.user_id.empty()) {
        result.code = "validate";
        result.message = "user_id 为空";
        return result;
    }
    if (request.instrument_id.empty()) {
        result.code = "validate";
        result.message = "instrument_id 为空";
        return result;
    }
    if (request.volume <= 0) {
        result.code = "validate";
        result.message = "volume 必须大于 0";
        return result;
    }

    if (config_.halt_all_orders && is_open_offset(request.offset)) {
        result.code = "halt";
        result.message = "应急停单已开启，拒绝开仓报单";
        return result;
    }

    const bool skip_throttle = mode == RiskCheckMode::EmergencyClose;
    const auto now = std::chrono::steady_clock::now();
    if (!skip_throttle) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (is_latency_paused(now)) {
            result.code = "latency";
            result.message = "RTT 暂停中，拒绝报单";
            return result;
        }

        if (config_.min_order_interval_sec > 0) {
            const auto last_it = last_order_by_user_.find(account.user_id);
            if (last_it != last_order_by_user_.end()) {
                const auto elapsed = now - last_it->second;
                if (elapsed < std::chrono::seconds(config_.min_order_interval_sec)) {
                    result.code = "interval";
                    result.message = "未满足最小报单间隔 " + std::to_string(config_.min_order_interval_sec) + " 秒";
                    return result;
                }
            }
        }

        if (config_.order_rate_per_sec > 0 && !consume_token_bucket(account.user_id, now)) {
            result.code = "token_bucket";
            result.message = "令牌桶频控超限（" + std::to_string(static_cast<int>(config_.order_rate_per_sec)) +
                             " 笔/秒）";
            return result;
        }

        if (config_.order_rate_limit_per_minute > 0) {
            auto& times = order_times_by_user_[account.user_id];
            const auto window_start = now - std::chrono::minutes(1);
            while (!times.empty() && times.front() < window_start) {
                times.pop_front();
            }
            if (static_cast<int>(times.size()) >= config_.order_rate_limit_per_minute) {
                result.code = "rate";
                result.message = "报单频率超限（" + std::to_string(config_.order_rate_limit_per_minute) + " 笔/分钟）";
                return result;
            }
        }

        if (config_.order_cancel_limit_per_minute > 0) {
            prune_action_times(account.user_id, now);
            if (actions_in_last_minute(account.user_id) >= config_.order_cancel_limit_per_minute) {
                result.code = "order_cancel";
                result.message = "报撤单频率超限（" + std::to_string(config_.order_cancel_limit_per_minute) +
                                 " 笔/分钟）";
                return result;
            }
        }

        if (config_.duplicate_order_limit_per_minute > 0) {
            const std::string fingerprint = order_fingerprint(request);
            if (duplicates_in_last_minute(account.user_id, fingerprint) >= config_.duplicate_order_limit_per_minute) {
                result.code = "duplicate";
                result.message = "重复报单超限";
                return result;
            }
        }
    }

    if (is_open_offset(request.offset)) {
        const auto daily_loss = check_daily_loss(ctp_account);
        if (!daily_loss.ok) {
            return daily_loss;
        }

        const auto limit_touch = check_limit_touch(request, quote);
        if (!limit_touch.ok) {
            return limit_touch;
        }

        const int limit = max_position_for(request.instrument_id);
        if (limit > 0 && position != nullptr) {
            const bool is_buy = request.direction != "sell";
            const int current = is_buy ? position->long_volume : position->short_volume;
            if (current + request.volume > limit) {
                result.code = "position";
                result.message = "持仓超限: 当前 " + std::to_string(current) + " + 报单 " +
                                 std::to_string(request.volume) + " > 限额 " + std::to_string(limit);
                return result;
            }
        }

        const auto concentration = check_concentration(request, positions_view);
        if (!concentration.ok) {
            return concentration;
        }

        const auto capital = check_capital_usage(request, positions_view, ctp_account);
        if (!capital.ok) {
            return capital;
        }
    }

    result.ok = true;
    result.message = "ok";
    return result;
}

void RiskEngine::on_order_accepted(const std::string& user_id, const OrderRequest* request) {
    if (user_id.empty()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    last_order_by_user_[user_id] = now;
    if (config_.order_rate_limit_per_minute > 0) {
        order_times_by_user_[user_id].push_back(now);
    }
    if (config_.order_cancel_limit_per_minute > 0) {
        action_times_by_user_[user_id].push_back(now);
    }
    if (request != nullptr && config_.duplicate_order_limit_per_minute > 0) {
        auto& events = duplicate_events_by_user_[user_id];
        events.push_back({order_fingerprint(*request), now});
        const auto window_start = now - std::chrono::minutes(1);
        while (!events.empty() && events.front().second < window_start) {
            events.pop_front();
        }
    }
}

void RiskEngine::on_order_cancel(const std::string& user_id) {
    if (user_id.empty() || config_.order_cancel_limit_per_minute <= 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    action_times_by_user_[user_id].push_back(now);
}

void RiskEngine::record_order_rtt(int rtt_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_rtt_ms_ = rtt_ms;
    if (config_.max_rtt_ms > 0 && rtt_ms > config_.max_rtt_ms && config_.latency_pause_sec > 0) {
        latency_paused_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(config_.latency_pause_sec);
        Logger::instance().warn("Risk RTT 超限 " + std::to_string(rtt_ms) + "ms，暂停报单 " +
                                std::to_string(config_.latency_pause_sec) + " 秒");
    }
}

void RiskEngine::clear_latency_pause() {
    std::lock_guard<std::mutex> lock(mutex_);
    latency_paused_until_ = {};
    Logger::instance().info("Risk RTT 暂停已清除");
}

void RiskEngine::record_rejection(const std::string& code) {
    if (code.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    reject_counts_[code] += 1;
}

void RiskEngine::set_halt_all_orders(bool halt) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_.halt_all_orders = halt;
    Logger::instance().warn(std::string("Risk 应急停单: ") + (halt ? "开启" : "关闭"));
}

void RiskEngine::set_halt_all_strategies(bool halt) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_.halt_all_strategies = halt;
    Logger::instance().warn(std::string("Risk 策略暂停: ") + (halt ? "开启" : "关闭"));
}

bool RiskEngine::halt_all_strategies() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_.halt_all_strategies;
}

int RiskEngine::session_reconnect_interval_ms() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_.session_reconnect_interval_ms;
}

nlohmann::json RiskEngine::status_view() const {
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json max_position = nlohmann::json::object();
    for (const auto& [key, value] : config_.max_position) {
        max_position[key] = value;
    }
    nlohmann::json reject_counts = nlohmann::json::object();
    for (const auto& [key, value] : reject_counts_) {
        reject_counts[key] = value;
    }
    const auto now = std::chrono::steady_clock::now();
    return {{"order_rate_limit_per_minute", config_.order_rate_limit_per_minute},
            {"min_order_interval_sec", config_.min_order_interval_sec},
            {"order_cancel_limit_per_minute", config_.order_cancel_limit_per_minute},
            {"duplicate_order_limit_per_minute", config_.duplicate_order_limit_per_minute},
            {"order_rate_per_sec", config_.order_rate_per_sec},
            {"order_burst", config_.order_burst},
            {"max_rtt_ms", config_.max_rtt_ms},
            {"latency_pause_sec", config_.latency_pause_sec},
            {"max_daily_loss", config_.max_daily_loss},
            {"block_limit_touch", config_.block_limit_touch},
            {"halt_all_orders", config_.halt_all_orders},
            {"halt_all_strategies", config_.halt_all_strategies},
            {"max_risk_degree", config_.max_risk_degree},
            {"max_product_concentration", config_.max_product_concentration},
            {"initial_capital", config_.initial_capital},
            {"contract_multiplier", config_.contract_multiplier},
            {"margin_rate", config_.margin_rate},
            {"use_ctp_account", config_.use_ctp_account},
            {"session_reconnect_interval_ms", config_.session_reconnect_interval_ms},
            {"max_net_position", config_.max_net_position},
            {"max_position", max_position},
            {"reject_counts", reject_counts},
            {"last_rtt_ms", last_rtt_ms_},
            {"latency_paused", is_latency_paused(now)}};
}

nlohmann::json RiskEngine::runtime_view(const std::string& user_id, const nlohmann::json& positions_view,
                                        const TradingAccountSnapshot* ctp_account) const {
    std::lock_guard<std::mutex> lock(mutex_);

    nlohmann::json concentration = nlohmann::json::array();
    std::unordered_map<std::string, int> product_lots;
    int total_lots = 0;
    int net_lots = 0;
    double notional = 0;

    for (const auto& row : positions_array(positions_view)) {
        const int long_vol = row.value("long", 0);
        const int short_vol = row.value("short", 0);
        const int lots = long_vol + short_vol;
        total_lots += lots;
        net_lots += std::abs(long_vol - short_vol);
        const double avg_long = row.value("avg_long_price", 0.0);
        const double avg_short = row.value("avg_short_price", 0.0);
        notional += long_vol * avg_long + short_vol * avg_short;

        const std::string product = product_key(row.value("instrument_id", ""));
        if (!product.empty()) {
            product_lots[product] += lots;
        }
    }

    for (const auto& [product, lots] : product_lots) {
        const double ratio = total_lots > 0 ? static_cast<double>(lots) / static_cast<double>(total_lots) : 0;
        concentration.push_back({{"product", product}, {"lots", lots}, {"ratio", ratio}});
    }

    const double estimated_margin = notional * config_.contract_multiplier * config_.margin_rate;
    const double estimated_risk_degree =
        config_.initial_capital > 0 ? estimated_margin / config_.initial_capital : 0;

    const auto now = std::chrono::steady_clock::now();
    double token_tokens = 0;
    const auto bucket_it = token_bucket_tokens_by_user_.find(user_id);
    if (bucket_it != token_bucket_tokens_by_user_.end()) {
        token_tokens = bucket_it->second;
    }

    nlohmann::json body = {{"user_id", user_id},
                           {"orders_last_minute", orders_in_last_minute(user_id)},
                           {"actions_last_minute", actions_in_last_minute(user_id)},
                           {"token_bucket_tokens", token_tokens},
                           {"total_lots", total_lots},
                           {"net_lots", net_lots},
                           {"estimated_notional", notional},
                           {"estimated_margin", estimated_margin},
                           {"estimated_risk_degree", estimated_risk_degree},
                           {"concentration", concentration},
                           {"last_rtt_ms", last_rtt_ms_},
                           {"latency_paused", is_latency_paused(now)}};

    if (config_.use_ctp_account && ctp_account != nullptr && ctp_account->balance > 0) {
        const double margin_used = ctp_account->curr_margin + ctp_account->frozen_margin;
        const double risk_degree = margin_used / ctp_account->balance;
        const double daily_pnl = ctp_account->close_profit + ctp_account->position_profit;
        body["capital_source"] = "ctp";
        body["risk_degree"] = risk_degree;
        body["daily_pnl"] = daily_pnl;
        body["ctp_balance"] = ctp_account->balance;
        body["ctp_available"] = ctp_account->available;
        body["ctp_margin"] = margin_used;
        body["ctp_curr_margin"] = ctp_account->curr_margin;
        body["ctp_position_profit"] = ctp_account->position_profit;
        body["ctp_close_profit"] = ctp_account->close_profit;
    } else {
        body["capital_source"] = "estimate";
        body["risk_degree"] = estimated_risk_degree;
    }

    return body;
}

}  // namespace quant_sev::core
