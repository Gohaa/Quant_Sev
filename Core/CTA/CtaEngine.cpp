#include "CTA/CtaEngine.hpp"

#include <algorithm>
#include <array>
#include <iomanip>
#include <sstream>

#include "Common/JsonQuery.hpp"
#include "Logger/Logger.hpp"

namespace quant_sev::core {

namespace {

StrategyDefinition parse_definition(const nlohmann::json& item) {
    StrategyDefinition def;
    def.id = item.value("id", "");
    def.name = item.value("name", def.id);
    def.period = item.value("period", "m1");
    def.logic = item.value("logic", "dual_thrust");
    def.default_instrument_id = item.value("default_instrument_id", item.value("instrument_id", ""));
    def.default_volume = item.value("default_volume", item.value("volume", 1));
    def.min_interval_sec = item.value("min_interval_sec", 60);
    def.daily_limit = item.value("daily_limit", 20);
    def.description = item.value("description", "");
    return def;
}

nlohmann::json definition_to_json(const StrategyDefinition& def, const StrategyRuntime* runtime) {
    nlohmann::json row = {{"id", def.id},
                          {"name", def.name},
                          {"period", def.period},
                          {"logic", def.logic},
                          {"default_instrument_id", def.default_instrument_id},
                          {"default_volume", def.default_volume},
                          {"min_interval_sec", def.min_interval_sec},
                          {"daily_limit", def.daily_limit},
                          {"description", def.description}};
    if (runtime != nullptr) {
        row["state"] = runtime->state;
        row["user_id"] = runtime->user_id;
        row["instrument_id"] = runtime->instrument_id;
        row["volume"] = runtime->volume;
        row["min_interval_sec"] = runtime->min_interval_sec;
        row["daily_limit"] = runtime->daily_limit;
        row["orders_today"] = runtime->orders_today;
        nlohmann::json legs = nlohmann::json::array();
        for (const auto& leg : runtime->legs) {
            legs.push_back({{"instrument_id", leg.instrument_id},
                            {"volume", leg.volume},
                            {"params", leg.params.is_object() ? leg.params : nlohmann::json::object()}});
        }
        row["legs"] = legs;
    } else {
        row["state"] = "stopped";
        row["user_id"] = "";
        row["instrument_id"] = def.default_instrument_id;
        row["volume"] = def.default_volume;
        row["orders_today"] = 0;
        row["legs"] = nlohmann::json::array();
    }
    return row;
}

nlohmann::json default_params_from_config(const nlohmann::json& config_item) {
    static const std::array<const char*, 14> kSkip = {
        "id",           "name",      "period",    "logic",           "description",
        "state",        "user_id",   "instrument_id", "volume",     "default_instrument_id",
        "default_volume", "min_interval_sec", "daily_limit", "legs"};
    nlohmann::json params = nlohmann::json::object();
    if (!config_item.is_object()) {
        return params;
    }
    for (auto it = config_item.begin(); it != config_item.end(); ++it) {
        const std::string key = it.key();
        const bool skip = std::any_of(kSkip.begin(), kSkip.end(), [&](const char* s) { return key == s; });
        if (skip || it.value().is_object() || it.value().is_array()) {
            continue;
        }
        params[key] = it.value();
    }
    return params;
}

void apply_legs_from_payload(StrategyRuntime& runtime, const nlohmann::json& payload,
                             const StrategyDefinition& def, const nlohmann::json& config_item) {
    runtime.legs.clear();
    const nlohmann::json default_params = default_params_from_config(config_item);
    if (payload.contains("legs") && payload["legs"].is_array()) {
        for (const auto& leg : payload["legs"]) {
            StrategyLegRuntime item;
            item.instrument_id = leg.value("instrument_id", "");
            item.volume = leg.value("volume", def.default_volume);
            item.params = default_params;
            if (leg.contains("params") && leg["params"].is_object()) {
                for (auto it = leg["params"].begin(); it != leg["params"].end(); ++it) {
                    item.params[it.key()] = it.value();
                }
            }
            for (auto it = leg.begin(); it != leg.end(); ++it) {
                const std::string key = it.key();
                if (key == "instrument_id" || key == "volume" || key == "params") {
                    continue;
                }
                if (!it.value().is_object() && !it.value().is_array()) {
                    item.params[key] = it.value();
                }
            }
            if (!item.instrument_id.empty() && item.volume > 0) {
                runtime.legs.push_back(std::move(item));
            }
        }
    }
    if (runtime.legs.empty()) {
        StrategyLegRuntime item;
        item.instrument_id = payload.value("instrument_id", def.default_instrument_id);
        item.volume = payload.value("volume", def.default_volume);
        item.params = default_params;
        if (payload.contains("params") && payload["params"].is_object()) {
            for (auto it = payload["params"].begin(); it != payload["params"].end(); ++it) {
                item.params[it.key()] = it.value();
            }
        }
        runtime.legs.push_back(std::move(item));
    }
    runtime.instrument_id = runtime.legs.front().instrument_id;
    runtime.volume = runtime.legs.front().volume;
}

const StrategyLegRuntime* find_leg(const StrategyRuntime& runtime, const std::string& instrument_id) {
    for (const auto& leg : runtime.legs) {
        if (leg.instrument_id == instrument_id) {
            return &leg;
        }
    }
    return nullptr;
}

}  // namespace

bool CtaEngine::initialize(Config& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = &config;
    definitions_.clear();
    strategy_configs_.clear();
    runtimes_.clear();

    const auto doc = config.read_json_file("Strategy_list.json");
    if (!doc.contains("strategies") || !doc["strategies"].is_array()) {
        Logger::instance().warn("Strategy_list.json 无 strategies 数组，CTA 策略列表为空");
        return true;
    }
    for (const auto& item : doc["strategies"]) {
        auto def = parse_definition(item);
        if (def.id.empty()) {
            continue;
        }
        strategy_configs_[def.id] = item;
        definitions_.push_back(std::move(def));
    }
    Logger::instance().info("CTA 已加载策略定义: " + std::to_string(definitions_.size()) + " 个");
    return true;
}

void CtaEngine::set_order_executor(OrderExecutor executor) {
    std::lock_guard<std::mutex> lock(mutex_);
    order_executor_ = std::move(executor);
}

void CtaEngine::set_strategy_gate(std::function<bool()> gate) {
    std::lock_guard<std::mutex> lock(mutex_);
    strategy_gate_ = std::move(gate);
}

const StrategyDefinition* CtaEngine::find_definition(const std::string& strategy_id) const {
    for (const auto& def : definitions_) {
        if (def.id == strategy_id) {
            return &def;
        }
    }
    return nullptr;
}

StrategyRuntime* CtaEngine::find_runtime(const std::string& strategy_id) {
    const auto it = runtimes_.find(strategy_id);
    return it == runtimes_.end() ? nullptr : &it->second;
}

const StrategyRuntime* CtaEngine::find_runtime(const std::string& strategy_id) const {
    const auto it = runtimes_.find(strategy_id);
    return it == runtimes_.end() ? nullptr : &it->second;
}

std::string CtaEngine::now_text() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm{};
#ifdef _WIN32
    localtime_s(&local_tm, &tt);
#else
    localtime_r(&tt, &local_tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&local_tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

std::string CtaEngine::trading_day_key() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm{};
#ifdef _WIN32
    localtime_s(&local_tm, &tt);
#else
    localtime_r(&tt, &local_tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&local_tm, "%Y%m%d");
    return oss.str();
}

void CtaEngine::reset_daily_counters_if_needed(StrategyRuntime& runtime) {
    const std::string today = trading_day_key();
    if (runtime.trading_day != today) {
        runtime.trading_day = today;
        runtime.orders_today = 0;
    }
}

nlohmann::json CtaEngine::list_strategies() const {
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json strategies = nlohmann::json::array();
    for (const auto& def : definitions_) {
        nlohmann::json row = strategy_configs_.count(def.id) > 0 ? strategy_configs_.at(def.id)
                                                                 : nlohmann::json::object();
        row.merge_patch(definition_to_json(def, find_runtime(def.id)));
        strategies.push_back(std::move(row));
    }
    return {{"strategies", strategies}};
}

ConnectResult CtaEngine::start_strategy(const nlohmann::json& payload) {
    const std::string strategy_id = payload.value("strategy_id", payload.value("id", ""));
    if (strategy_id.empty()) {
        return {false, "strategy_id 必填"};
    }
    const std::string user_id = payload.value("user_id", "");
    if (user_id.empty()) {
        return {false, "user_id 必填"};
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const StrategyDefinition* def = find_definition(strategy_id);
    if (def == nullptr) {
        return {false, "未找到策略: " + strategy_id};
    }

    StrategyRuntime runtime;
    runtime.state = "running";
    runtime.user_id = user_id;
    runtime.min_interval_sec = payload.value("min_interval_sec", def->min_interval_sec);
    runtime.daily_limit = payload.value("daily_limit", def->daily_limit);
    runtime.trading_day = trading_day_key();
    runtime.orders_today = 0;
    runtime.has_last_order = false;

    const nlohmann::json config_item =
        strategy_configs_.count(strategy_id) > 0 ? strategy_configs_.at(strategy_id) : nlohmann::json::object();
    apply_legs_from_payload(runtime, payload, *def, config_item);

    if (runtime.legs.empty()) {
        return {false, "至少配置一个有效合约 leg"};
    }
    for (const auto& leg : runtime.legs) {
        if (leg.instrument_id.empty()) {
            return {false, "leg.instrument_id 不能为空"};
        }
        if (leg.volume <= 0) {
            return {false, "leg.volume 必须大于 0"};
        }
    }

    runtimes_[strategy_id] = runtime;
    std::string leg_summary;
    for (std::size_t i = 0; i < runtime.legs.size(); ++i) {
        if (i > 0) {
            leg_summary += ", ";
        }
        leg_summary += runtime.legs[i].instrument_id + "x" + std::to_string(runtime.legs[i].volume);
    }
    Logger::instance().info("CTA 启动策略: " + strategy_id + " user=" + user_id + " legs=[" + leg_summary + "]");
    return {true, "策略已启动: " + def->name};
}

ConnectResult CtaEngine::stop_strategy(const nlohmann::json& payload) {
    const std::string strategy_id = payload.value("strategy_id", payload.value("id", ""));
    if (strategy_id.empty()) {
        return {false, "strategy_id 必填"};
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto* runtime = find_runtime(strategy_id);
    if (runtime == nullptr || runtime->state != "running") {
        return {false, "策略未在运行: " + strategy_id};
    }
    runtime->state = "stopped";
    Logger::instance().info("CTA 停止策略: " + strategy_id);
    return {true, "策略已停止"};
}

ConnectResult CtaEngine::stop_all_strategies() {
    std::lock_guard<std::mutex> lock(mutex_);
    int stopped = 0;
    for (auto& [id, runtime] : runtimes_) {
        (void)id;
        if (runtime.state == "running") {
            runtime.state = "stopped";
            stopped += 1;
        }
    }
    Logger::instance().warn("CTA 应急停止全部策略: " + std::to_string(stopped) + " 个");
    return {true, "已停止 " + std::to_string(stopped) + " 个运行中策略"};
}

nlohmann::json CtaEngine::emergency_flatten(const AccountRecord& account, const OrderExecutor& executor) {
    if (!executor) {
        return {{"ok", false}, {"error", "发单通道未就绪"}};
    }

    nlohmann::json rows = nlohmann::json::array();
    std::vector<std::pair<std::string, int>> long_closes;
    std::vector<std::pair<std::string, int>> short_closes;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto user_it = positions_by_user_.find(account.user_id);
        if (user_it != positions_by_user_.end()) {
            for (const auto& [inst, pos] : user_it->second) {
                if (pos.long_volume > 0) {
                    long_closes.emplace_back(inst, pos.long_volume);
                }
                if (pos.short_volume > 0) {
                    short_closes.emplace_back(inst, pos.short_volume);
                }
            }
        }
    }

    int success_count = 0;
    int fail_count = 0;

    auto submit_close = [&](const std::string& instrument_id, const std::string& direction, int volume) {
        OrderRequest order;
        order.user_id = account.user_id;
        order.instrument_id = instrument_id;
        order.direction = direction;
        order.offset = "close";
        order.price_type = "opponent";
        order.price = 0;
        order.volume = volume;

        const OrderResult result = executor(account, order);
        record_order(account, order, result, "emergency:flatten");
        nlohmann::json row = {{"instrument_id", instrument_id},
                              {"direction", direction},
                              {"volume", volume},
                              {"ok", result.ok},
                              {"message", result.message},
                              {"order_ref", result.order_ref}};
        rows.push_back(row);
        if (result.ok) {
            success_count += 1;
        } else {
            fail_count += 1;
        }
    };

    for (const auto& [inst, vol] : long_closes) {
        submit_close(inst, "sell", vol);
    }
    for (const auto& [inst, vol] : short_closes) {
        submit_close(inst, "buy", vol);
    }

    return {{"ok", fail_count == 0},
            {"submitted", rows},
            {"success_count", success_count},
            {"fail_count", fail_count},
            {"position_count", static_cast<int>(long_closes.size() + short_closes.size())}};
}

ConnectResult CtaEngine::validate_signal(const StrategyDefinition& definition, StrategyRuntime& runtime,
                                         const TradingSignal& signal) {
    if (strategy_gate_ && strategy_gate_()) {
        return {false, "策略已应急暂停，拒绝发单"};
    }
    if (runtime.state != "running") {
        return {false, "策略未运行: " + definition.id};
    }
    if (signal.user_id != runtime.user_id) {
        return {false, "user_id 与策略绑定账户不一致"};
    }
    const StrategyLegRuntime* leg = find_leg(runtime, signal.instrument_id);
    if (leg == nullptr) {
        return {false, "合约不在策略绑定列表: " + signal.instrument_id};
    }
    if (signal.volume <= 0) {
        return {false, "volume 必须大于 0"};
    }
    if (signal.volume > leg->volume) {
        return {false, "报单手数超过该合约策略限制"};
    }

    reset_daily_counters_if_needed(runtime);
    if (runtime.orders_today >= runtime.daily_limit) {
        return {false, "已达策略每日报单上限"};
    }
    if (runtime.has_last_order && runtime.min_interval_sec > 0) {
        const auto elapsed = std::chrono::steady_clock::now() - runtime.last_order_at;
        if (elapsed < std::chrono::seconds(runtime.min_interval_sec)) {
            return {false, "未满足最小报单间隔"};
        }
    }
    return {true, "ok"};
}

ConnectResult CtaEngine::submit_signal(const AccountRecord& account, const TradingSignal& signal) {
    if (!order_executor_) {
        return {false, "CTA 发单通道未就绪"};
    }
    if (signal.strategy_id.empty()) {
        return {false, "strategy_id 必填"};
    }

    StrategyDefinition definition;
    StrategyRuntime runtime_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const StrategyDefinition* def = find_definition(signal.strategy_id);
        if (def == nullptr) {
            return {false, "未找到策略: " + signal.strategy_id};
        }
        StrategyRuntime* runtime = find_runtime(signal.strategy_id);
        if (runtime == nullptr) {
            return {false, "策略未启动: " + signal.strategy_id};
        }
        const auto validated = validate_signal(*def, *runtime, signal);
        if (!validated.ok) {
            return validated;
        }
        definition = *def;
        runtime_copy = *runtime;
    }

    OrderRequest order;
    order.user_id = account.user_id;
    order.instrument_id = signal.instrument_id.empty() ? runtime_copy.instrument_id : signal.instrument_id;
    order.direction = signal.direction.empty() ? "buy" : signal.direction;
    order.offset = signal.offset.empty() ? "open" : signal.offset;
    order.price_type = signal.price_type.empty() ? "limit" : signal.price_type;
    order.price = signal.price;
    order.volume = signal.volume > 0 ? signal.volume : runtime_copy.volume;

    const OrderResult result = order_executor_(account, order);
    record_order(account, order, result, "strategy:" + signal.strategy_id);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        StrategyRuntime* runtime = find_runtime(signal.strategy_id);
        if (runtime != nullptr && result.ok) {
            reset_daily_counters_if_needed(*runtime);
            runtime->orders_today += 1;
            runtime->last_order_at = std::chrono::steady_clock::now();
            runtime->has_last_order = true;
        }
    }

    if (result.ok) {
        return {true, result.message};
    }
    return {false, result.message};
}

void CtaEngine::apply_position_delta(const std::string& user_id, const OrderRequest& request, int filled_volume) {
    if (filled_volume <= 0) {
        return;
    }
    auto& book = positions_by_user_[user_id];
    auto& pos = book[request.instrument_id];
    pos.instrument_id = request.instrument_id;

    const bool is_buy = request.direction == "buy";
    const bool is_open = request.offset == "open" || request.offset.empty();

    if (is_open) {
        if (is_buy) {
            const int total = pos.long_volume + filled_volume;
            pos.avg_long_price =
                total > 0 ? ((pos.avg_long_price * pos.long_volume) + request.price * filled_volume) / total : 0;
            pos.long_volume = total;
        } else {
            const int total = pos.short_volume + filled_volume;
            pos.avg_short_price =
                total > 0 ? ((pos.avg_short_price * pos.short_volume) + request.price * filled_volume) / total : 0;
            pos.short_volume = total;
        }
        return;
    }

    if (is_buy) {
        pos.short_volume = std::max(0, pos.short_volume - filled_volume);
        if (pos.short_volume == 0) {
            pos.avg_short_price = 0;
        }
    } else {
        pos.long_volume = std::max(0, pos.long_volume - filled_volume);
        if (pos.long_volume == 0) {
            pos.avg_long_price = 0;
        }
    }
}

void CtaEngine::record_order(const AccountRecord& account, const OrderRequest& request, const OrderResult& result,
                             const std::string& source) {
    CtaOrderView view;
    view.order_ref = result.order_ref;
    view.user_id = account.user_id;
    view.instrument_id = request.instrument_id;
    view.direction = request.direction;
    view.offset = request.offset;
    view.price_type = request.price_type;
    view.price = request.price;
    view.volume = request.volume;
    view.source = source;
    view.status = result.ok ? "submitted" : "rejected";
    view.message = result.message;
    view.created_at = now_text();

    std::lock_guard<std::mutex> lock(mutex_);
    orders_.push_back(view);
    if (orders_.size() > 500) {
        orders_.erase(orders_.begin(), orders_.begin() + static_cast<std::ptrdiff_t>(orders_.size() - 500));
    }
}

void CtaEngine::apply_trade_update(const nlohmann::json& update) {
    const std::string trade_id = update.value("trade_id", "");
    const std::string user_id = update.value("user_id", "");
    const std::string instrument_id = update.value("instrument_id", "");
    const int volume = update.value("volume", 0);
    if (trade_id.empty() || user_id.empty() || instrument_id.empty() || volume <= 0) {
        return;
    }

    OrderRequest request;
    request.user_id = user_id;
    request.instrument_id = instrument_id;
    request.direction = update.value("direction", "buy");
    request.offset = update.value("offset", "open");
    request.price = update.value("price", 0.0);

    std::lock_guard<std::mutex> lock(mutex_);
    if (seen_trade_ids_.count(trade_id) > 0) {
        return;
    }
    seen_trade_ids_.insert(trade_id);
    if (seen_trade_ids_.size() > 5000) {
        seen_trade_ids_.clear();
    }

    apply_position_delta(user_id, request, volume);

    const std::string order_ref = update.value("order_ref", "");
    if (!order_ref.empty()) {
        for (auto& order : orders_) {
            if (order.order_ref != order_ref) {
                continue;
            }
            if (!user_id.empty() && order.user_id != user_id) {
                continue;
            }
            order.volume_traded += volume;
            if (order.volume > 0 && order.volume_traded >= order.volume) {
                order.status = "filled";
            } else if (order.volume_traded > 0) {
                order.status = "partial";
            }
            break;
        }
    }

    Logger::instance().info("CTA 成交更新持仓: " + instrument_id + " " + request.direction + " " +
                            request.offset + " vol=" + std::to_string(volume));
}

void CtaEngine::replace_positions_from_snapshot(const std::string& user_id, const nlohmann::json& positions) {
    if (user_id.empty() || !positions.is_array()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto& book = positions_by_user_[user_id];
    book.clear();
    for (const auto& row : positions) {
        const std::string instrument_id = row.value("instrument_id", "");
        if (instrument_id.empty()) {
            continue;
        }
        auto& pos = book[instrument_id];
        pos.instrument_id = instrument_id;
        if (row.contains("direction")) {
            const std::string direction = row.value("direction", "");
            const int volume = row.value("volume", 0);
            const double open_price = row.value("open_price", 0.0);
            if (direction == "long") {
                pos.long_volume = volume;
                pos.avg_long_price = open_price;
            } else if (direction == "short") {
                pos.short_volume = volume;
                pos.avg_short_price = open_price;
            }
        } else {
            pos.long_volume = row.value("long", row.value("long_volume", 0));
            pos.short_volume = row.value("short", row.value("short_volume", 0));
            pos.avg_long_price = row.value("avg_long_price", 0.0);
            pos.avg_short_price = row.value("avg_short_price", 0.0);
        }
    }
    for (auto it = book.begin(); it != book.end();) {
        if (it->second.long_volume == 0 && it->second.short_volume == 0) {
            it = book.erase(it);
        } else {
            ++it;
        }
    }
    Logger::instance().info("CTA 持仓快照同步: user=" + user_id + " contracts=" + std::to_string(book.size()));
}

void CtaEngine::apply_order_update(const nlohmann::json& update) {
    const std::string order_ref = update.value("order_ref", "");
    const std::string user_id = update.value("user_id", "");
    const std::string status = update.value("status", "");
    if (order_ref.empty() || status.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& order : orders_) {
        if (order.order_ref != order_ref) {
            continue;
        }
        if (!user_id.empty() && order.user_id != user_id) {
            continue;
        }
        order.status = status;
        const std::string order_sys_id = update.value("order_sys_id", "");
        if (!order_sys_id.empty()) {
            order.order_sys_id = order_sys_id;
        }
        if (update.contains("volume_traded")) {
            order.volume_traded = update.value("volume_traded", 0);
        }
        const std::string status_msg = update.value("status_msg", "");
        if (!status_msg.empty()) {
            order.message = status_msg;
        }
        break;
    }
}

std::optional<CtaPositionView> CtaEngine::position_for(const std::string& user_id,
                                                       const std::string& instrument_id) const {
    if (user_id.empty() || instrument_id.empty()) {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto user_it = positions_by_user_.find(user_id);
    if (user_it == positions_by_user_.end()) {
        return std::nullopt;
    }
    const auto pos_it = user_it->second.find(instrument_id);
    if (pos_it == user_it->second.end()) {
        return std::nullopt;
    }
    return pos_it->second;
}

nlohmann::json CtaEngine::orders_view(const nlohmann::json& query) const {
    const std::string user_id = query.value("user_id", "");
    const int limit = json_int_param(query, "limit", 100);

    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json rows = nlohmann::json::array();
    int count = 0;
    for (auto it = orders_.rbegin(); it != orders_.rend(); ++it) {
        if (!user_id.empty() && it->user_id != user_id) {
            continue;
        }
        rows.push_back({{"order_ref", it->order_ref},
                        {"order_sys_id", it->order_sys_id},
                        {"user_id", it->user_id},
                        {"instrument_id", it->instrument_id},
                        {"direction", it->direction},
                        {"offset", it->offset},
                        {"price_type", it->price_type},
                        {"price", it->price},
                        {"volume", it->volume},
                        {"volume_traded", it->volume_traded},
                        {"source", it->source},
                        {"status", it->status},
                        {"message", it->message},
                        {"created_at", it->created_at}});
        count += 1;
        if (count >= limit) {
            break;
        }
    }
    return {{"orders", rows}};
}

nlohmann::json CtaEngine::positions_view(const nlohmann::json& query) const {
    const std::string user_id = query.value("user_id", "");

    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json rows = nlohmann::json::array();
    int total_long = 0;
    int total_short = 0;

    const auto append_user_positions = [&](const std::string& uid,
                                           const std::unordered_map<std::string, CtaPositionView>& book) {
        for (const auto& [_, pos] : book) {
            if (pos.long_volume == 0 && pos.short_volume == 0) {
                continue;
            }
            total_long += pos.long_volume;
            total_short += pos.short_volume;
            rows.push_back({{"user_id", uid},
                            {"instrument_id", pos.instrument_id},
                            {"long", pos.long_volume},
                            {"short", pos.short_volume},
                            {"avg_long_price", pos.avg_long_price},
                            {"avg_short_price", pos.avg_short_price}});
        }
    };

    if (!user_id.empty()) {
        const auto it = positions_by_user_.find(user_id);
        if (it != positions_by_user_.end()) {
            append_user_positions(user_id, it->second);
        }
    } else {
        for (const auto& [uid, book] : positions_by_user_) {
            append_user_positions(uid, book);
        }
    }

    return {{"positions", rows},
            {"summary", {{"total_contracts", rows.size()}, {"total_long", total_long}, {"total_short", total_short}}}};
}

nlohmann::json CtaEngine::account_view(const nlohmann::json& query) const {
    const std::string user_id = query.value("user_id", "");
    if (user_id.empty()) {
        return {{"error", "user_id required"}};
    }

    std::lock_guard<std::mutex> lock(mutex_);
    int running = 0;
    for (const auto& [id, runtime] : runtimes_) {
        (void)id;
        if (runtime.state == "running" && runtime.user_id == user_id) {
            running += 1;
        }
    }
    int order_count = 0;
    for (const auto& order : orders_) {
        if (order.user_id == user_id) {
            order_count += 1;
        }
    }
    return {{"user_id", user_id}, {"running_strategies", running}, {"order_count", order_count}};
}

}  // namespace quant_sev::core
