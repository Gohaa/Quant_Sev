#include "Strategy/StrategyRuntime.hpp"

#include <algorithm>
#include <sstream>

namespace quant_sev::bll {

namespace {

int json_int(const nlohmann::json& doc, const char* key, int fallback) {
    if (!doc.contains(key)) {
        return fallback;
    }
    if (doc[key].is_number_integer()) {
        return doc[key].get<int>();
    }
    if (doc[key].is_number()) {
        return static_cast<int>(doc[key].get<double>());
    }
    return fallback;
}

}  // namespace

void register_c_strategy_DualThrustStrategy();
void register_c_strategy_MaCrossStrategy();
void register_c_strategy_MacdStrategy();
void register_c_strategy_DoubleEmaStrategy();
void register_c_strategy_BollStrategy();

void StrategyRegistry::ensure_builtin_modules() const {
    if (builtins_loaded_) {
        return;
    }
    builtins_loaded_ = true;
    register_c_strategy_DualThrustStrategy();
    register_c_strategy_MaCrossStrategy();
    register_c_strategy_MacdStrategy();
    register_c_strategy_DoubleEmaStrategy();
    register_c_strategy_BollStrategy();
}

std::string StrategyContext::indicator_cache_key(const std::string& name,
                                                 const std::vector<double>& options) {
    std::ostringstream oss;
    oss << name;
    for (double option : options) {
        oss << '|' << option;
    }
    return oss.str();
}

IndicatorSeriesMap& StrategyContext::ensure_indicator(const std::string& name,
                                                      const std::vector<double>& options) {
    if (!indicator_cache_ || !bars) {
        static IndicatorSeriesMap empty;
        return empty;
    }
    const std::string key = indicator_cache_key(name, options);
    auto it = indicator_cache_->find(key);
    if (it == indicator_cache_->end()) {
        it = indicator_cache_->emplace(key, compute_indicator_on_bars(*bars, name, options)).first;
    }
    return it->second;
}

std::optional<double> StrategyContext::indicator_at(const std::string& output_key, int index) const {
    if (!indicator_cache_) {
        return std::nullopt;
    }
    for (const auto& [_, series] : *indicator_cache_) {
        const auto value = indicator_value_at(series, output_key, index);
        if (value) {
            return value;
        }
    }
    return std::nullopt;
}

StrategyRegistry& StrategyRegistry::instance() {
    static StrategyRegistry registry;
    return registry;
}

void StrategyRegistry::register_strategy(const std::string& logic_id, StrategyFactory factory) {
    factories_[logic_id] = std::move(factory);
}

std::unique_ptr<CStrategyBase> StrategyRegistry::create(const std::string& logic_id) const {
    ensure_builtin_modules();
    const auto it = factories_.find(logic_id);
    if (it == factories_.end()) {
        return nullptr;
    }
    return it->second();
}

bool StrategyRegistry::has(const std::string& logic_id) const {
    ensure_builtin_modules();
    return factories_.count(logic_id) > 0;
}

std::vector<std::string> StrategyRegistry::list_logics() const {
    ensure_builtin_modules();
    std::vector<std::string> out;
    out.reserve(factories_.size());
    for (const auto& [logic_id, _] : factories_) {
        out.push_back(logic_id);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<StrategyInputSpec> StrategyRegistry::input_specs(const std::string& logic_id) const {
    ensure_builtin_modules();
    const auto instance = create(logic_id);
    if (!instance) {
        return {};
    }
    return instance->input_specs();
}

nlohmann::json StrategyRegistry::input_specs_json(const std::string& logic_id) const {
    nlohmann::json inputs = nlohmann::json::array();
    for (const auto& spec : input_specs(logic_id)) {
        inputs.push_back({{"key", spec.key},
                          {"var_name", spec.var_name},
                          {"label", spec.label},
                          {"type", spec.type == StrategyInputType::Int ? "int" : "double"},
                          {"default", spec.default_value},
                          {"min", spec.min_value},
                          {"max", spec.max_value},
                          {"optimize",
                           {{"start", spec.optimize_start},
                            {"step", spec.optimize_step},
                            {"stop", spec.optimize_stop}}}});
    }
    return inputs;
}

StrategySession::StrategySession(std::string logic_id) : logic_id_(std::move(logic_id)) {
    instance_ = StrategyRegistry::instance().create(logic_id_);
}

void StrategySession::reset(const nlohmann::json& params, const std::vector<BarRecord>& bars,
                            const std::string& period) {
    if (!instance_) {
        return;
    }
    if (initialized_) {
        StrategyContext ctx = make_context(bars.empty() ? 0 : static_cast<int>(bars.size()) - 1, bars, period, nullptr);
        instance_->OnDeinit(ctx, StrategyDeinitReason::Parameters);
        indicator_cache_.clear();
        initialized_ = false;
    }
    params_ = params;
    period_ = period;
    instance_->apply_inputs(params_);
    StrategyContext ctx = make_context(bars.empty() ? 0 : static_cast<int>(bars.size()) - 1, bars, period, nullptr);
    ctx.params = params_;
    instance_->OnInit(ctx);
    initialized_ = true;
}

void StrategySession::deinit(StrategyDeinitReason reason) {
    if (!instance_ || !initialized_) {
        return;
    }
    StrategyContext ctx;
    ctx.indicator_cache_ = &indicator_cache_;
    ctx.params = params_;
    ctx.period = period_;
    instance_->OnDeinit(ctx, reason);
    initialized_ = false;
    indicator_cache_.clear();
}

StrategyContext StrategySession::make_context(int index, const std::vector<BarRecord>& bars,
                                              const std::string& period, const std::vector<TickRecord>* ticks) {
    StrategyContext ctx;
    ctx.bars = &bars;
    ctx.bar_ticks = ticks;
    ctx.tick = nullptr;
    ctx.bar_index = index;
    ctx.period = period;
    ctx.params = params_;
    ctx.indicator_cache_ = &indicator_cache_;
    return ctx;
}

std::optional<StrategyBarSignal> StrategySession::on_bar(int index, const std::vector<BarRecord>& bars,
                                                         const std::string& period,
                                                         const std::vector<TickRecord>* ticks) {
    if (!instance_ || !initialized_) {
        return std::nullopt;
    }

    StrategyContext ctx = make_context(index, bars, period, ticks);
    std::optional<StrategyBarSignal> signal;

    if (ticks && !ticks->empty()) {
        for (const auto& tick : *ticks) {
            ctx.tick = &tick;
            instance_->OnTick(ctx, signal);
            if (signal) {
                signal->bar_index = index;
                return signal;
            }
        }
        return std::nullopt;
    }

    instance_->OnBar(ctx, signal);
    if (signal) {
        signal->bar_index = index;
    }
    return signal;
}

nlohmann::json StrategySession::chart_indicators(const std::vector<BarRecord>& bars,
                                                 const std::string& period) const {
    if (!instance_) {
        return nlohmann::json::object();
    }
    return instance_->chart_indicators(bars, period);
}

nlohmann::json strategy_params_from_backtest(const nlohmann::json& source) {
    nlohmann::json params = source;
    if (!params.contains("days")) {
        params["days"] = json_int(source, "days", 4);
    }
    if (!params.contains("ma_fast")) {
        params["ma_fast"] = json_int(source, "ma_fast", 5);
    }
    if (!params.contains("ma_slow")) {
        params["ma_slow"] = json_int(source, "ma_slow", 20);
    }
    if (!params.contains("macd_short")) {
        params["macd_short"] = json_int(source, "macd_short", 12);
    }
    if (!params.contains("macd_long")) {
        params["macd_long"] = json_int(source, "macd_long", 26);
    }
    if (!params.contains("macd_signal")) {
        params["macd_signal"] = json_int(source, "macd_signal", 9);
    }
    if (!params.contains("boll_period")) {
        params["boll_period"] = json_int(source, "boll_period", 20);
    }
    if (!params.contains("boll_stddev")) {
        params["boll_stddev"] = source.value("boll_stddev", 2.0);
    }
    if (!params.contains("k1")) {
        params["k1"] = source.value("k1", 0.5);
    }
    if (!params.contains("k2")) {
        params["k2"] = source.value("k2", 0.5);
    }
    params["intra_bar"] = source.value("intra_bar", false);
    return params;
}

}  // namespace quant_sev::bll
