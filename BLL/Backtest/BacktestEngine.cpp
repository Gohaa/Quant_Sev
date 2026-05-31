#include "Backtest/BacktestEngine.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <unordered_map>

#include "Backtest/MatchEngine.hpp"
#include "Backtest/TickBarSynthesizer.hpp"
#include "Bar/BarPeriod.hpp"
#include "Logger/Logger.hpp"
#include "Strategy/StrategyRuntime.hpp"

namespace quant_sev::bll {

namespace {

std::string format_trading_date(const std::string& trading_day) {
    if (trading_day.size() == 8) {
        return trading_day.substr(0, 4) + "/" + trading_day.substr(4, 2) + "/" + trading_day.substr(6, 2);
    }
    return trading_day;
}

std::string m1_time_floor(const std::string& time) {
    if (time.size() >= 5) {
        return time.substr(0, 5) + ":00";
    }
    return time;
}

double resolved_multiplier(const BacktestRequest& request) {
    return request.contract_multiplier > 0 ? request.contract_multiplier : 10;
}

double resolved_tick_size(const BacktestRequest& request) {
    return request.tick_size > 0 ? request.tick_size : 1;
}

std::string normalize_date_token(std::string value) {
    for (char& ch : value) {
        if (ch == '-') {
            ch = '/';
        }
    }
    return value;
}

std::string bar_datetime_key(const BarRecord& bar) {
    return normalize_date_token(bar.date) + " " + bar.time;
}

std::string range_bound(const std::string& date, bool end_of_day) {
    if (date.empty()) {
        return {};
    }
    return normalize_date_token(date) + (end_of_day ? " 23:59:59" : " 00:00:00");
}

std::vector<BarRecord> filter_bars_by_date(const std::vector<BarRecord>& bars, const std::string& start_date,
                                           const std::string& end_date) {
    if (start_date.empty() && end_date.empty()) {
        return bars;
    }
    const std::string start_key = range_bound(start_date, false);
    const std::string end_key = range_bound(end_date, true);
    std::vector<BarRecord> filtered;
    filtered.reserve(bars.size());
    for (const auto& bar : bars) {
        const std::string key = bar_datetime_key(bar);
        if (!start_key.empty() && key < start_key) {
            continue;
        }
        if (!end_key.empty() && key > end_key) {
            continue;
        }
        filtered.push_back(bar);
    }
    return filtered;
}

std::string bar_time_label(const std::vector<BarRecord>& bars, int index) {
    if (index < 0 || index >= static_cast<int>(bars.size())) {
        return {};
    }
    const auto& bar = bars[static_cast<std::size_t>(index)];
    return bar.date + " " + bar.time;
}

std::vector<int> grid_int_values(const nlohmann::json& grid, const std::string& key,
                                 const std::vector<int>& defaults) {
    if (!grid.contains(key)) {
        return defaults;
    }
    const auto& node = grid[key];
    if (node.is_array() && !node.empty()) {
        std::vector<int> out;
        for (const auto& item : node) {
            if (item.is_number_integer()) {
                out.push_back(item.get<int>());
            } else if (item.is_number()) {
                out.push_back(static_cast<int>(item.get<double>()));
            }
        }
        return out.empty() ? defaults : out;
    }
    if (node.is_object()) {
        if (!node.value("enabled", true)) {
            return defaults;
        }
        const int start = node.value("start", defaults.empty() ? 0 : defaults.front());
        const int step = node.value("step", 1);
        const int stop = node.value("stop", start);
        if (step == 0) {
            return {start};
        }
        std::vector<int> out;
        for (int v = start; step > 0 ? v <= stop : v >= stop; v += step) {
            out.push_back(v);
            if (out.size() > 10000) {
                break;
            }
        }
        return out.empty() ? defaults : out;
    }
    return defaults;
}

std::vector<double> grid_double_values(const nlohmann::json& grid, const std::string& key,
                                       const std::vector<double>& defaults) {
    if (!grid.contains(key)) {
        return defaults;
    }
    const auto& node = grid[key];
    if (node.is_array() && !node.empty()) {
        std::vector<double> out;
        for (const auto& item : node) {
            if (item.is_number()) {
                out.push_back(item.get<double>());
            }
        }
        return out.empty() ? defaults : out;
    }
    if (node.is_object()) {
        if (!node.value("enabled", true)) {
            return defaults;
        }
        const double start = node.value("start", defaults.empty() ? 0.0 : defaults.front());
        const double step = node.value("step", 1.0);
        const double stop = node.value("stop", start);
        if (std::abs(step) < 1e-12) {
            return {start};
        }
        std::vector<double> out;
        for (double v = start; step > 0 ? v <= stop + 1e-9 : v >= stop - 1e-9; v += step) {
            out.push_back(v);
            if (out.size() > 10000) {
                break;
            }
        }
        return out.empty() ? defaults : out;
    }
    return defaults;
}

const std::unordered_map<std::string, std::string>& strategy_display_names() {
    static const std::unordered_map<std::string, std::string> names = {
        {"dual_thrust", "DualThrust 突破"},
        {"ma_cross", "均线金叉/死叉"},
        {"macd", "MACD 金叉/死叉"},
        {"double_ema", "DoubleEMA 交叉"},
        {"boll", "布林带突破"},
    };
    return names;
}

void apply_strategy_param(BacktestRequest& req, const std::string& key, const nlohmann::json& value) {
    req.optimize_combo[key] = value;
    if (key == "days" && value.is_number()) {
        req.days = value.is_number_integer() ? value.get<int>() : static_cast<int>(value.get<double>());
    } else if (key == "k1" && value.is_number()) {
        req.k1 = value.get<double>();
    } else if (key == "k2" && value.is_number()) {
        req.k2 = value.get<double>();
    } else if (key == "ma_fast" && value.is_number()) {
        req.ma_fast = value.is_number_integer() ? value.get<int>() : static_cast<int>(value.get<double>());
    } else if (key == "ma_slow" && value.is_number()) {
        req.ma_slow = value.is_number_integer() ? value.get<int>() : static_cast<int>(value.get<double>());
    } else if (key == "macd_short" && value.is_number()) {
        req.macd_short = value.is_number_integer() ? value.get<int>() : static_cast<int>(value.get<double>());
    } else if (key == "macd_long" && value.is_number()) {
        req.macd_long = value.is_number_integer() ? value.get<int>() : static_cast<int>(value.get<double>());
    } else if (key == "macd_signal" && value.is_number()) {
        req.macd_signal = value.is_number_integer() ? value.get<int>() : static_cast<int>(value.get<double>());
    } else if (key == "boll_period" && value.is_number()) {
        req.boll_period = value.is_number_integer() ? value.get<int>() : static_cast<int>(value.get<double>());
    } else if (key == "boll_stddev" && value.is_number()) {
        req.boll_stddev = value.get<double>();
    }
}

nlohmann::json combo_params_json(const BacktestRequest& req) {
    nlohmann::json params = {{"strategy", req.strategy}};
    const auto specs = StrategyRegistry::instance().input_specs(req.strategy);
    for (const auto& spec : specs) {
        if (!req.optimize_combo.empty() && req.optimize_combo.contains(spec.key)) {
            params[spec.key] = req.optimize_combo[spec.key];
            continue;
        }
        if (spec.key == "days") {
            params[spec.key] = req.days;
        } else if (spec.key == "k1") {
            params[spec.key] = req.k1;
        } else if (spec.key == "k2") {
            params[spec.key] = req.k2;
        } else if (spec.key == "ma_fast") {
            params[spec.key] = req.ma_fast;
        } else if (spec.key == "ma_slow") {
            params[spec.key] = req.ma_slow;
        } else if (spec.key == "macd_short") {
            params[spec.key] = req.macd_short;
        } else if (spec.key == "macd_long") {
            params[spec.key] = req.macd_long;
        } else if (spec.key == "macd_signal") {
            params[spec.key] = req.macd_signal;
        } else if (spec.key == "boll_period") {
            params[spec.key] = req.boll_period;
        } else if (spec.key == "boll_stddev") {
            params[spec.key] = req.boll_stddev;
        }
    }
    return params;
}

std::vector<BacktestRequest> build_optimize_combos(const BacktestRequest& base) {
    const auto& grid = base.optimize_grid;
    std::vector<BacktestRequest> combos;
    const std::string strategy = base.strategy.empty() ? "dual_thrust" : base.strategy;
    const auto specs = StrategyRegistry::instance().input_specs(strategy);
    if (specs.empty()) {
        BacktestRequest req = base;
        req.lite_output = true;
        combos.push_back(req);
        return combos;
    }

    struct Dimension {
        std::string key;
        std::vector<nlohmann::json> values;
    };
    std::vector<Dimension> dimensions;
    for (const auto& spec : specs) {
        std::vector<nlohmann::json> values;
        if (grid.contains(spec.key)) {
            if (spec.type == StrategyInputType::Int) {
                const std::vector<int> defaults = {static_cast<int>(spec.default_value)};
                for (int v : grid_int_values(grid, spec.key, defaults)) {
                    values.push_back(v);
                }
            } else {
                const std::vector<double> defaults = {spec.default_value};
                for (double v : grid_double_values(grid, spec.key, defaults)) {
                    values.push_back(v);
                }
            }
        } else {
            values.push_back(spec.type == StrategyInputType::Int ? static_cast<int>(spec.default_value)
                                                                 : spec.default_value);
        }
        if (!values.empty()) {
            dimensions.push_back({spec.key, values});
        }
    }
    if (dimensions.empty()) {
        BacktestRequest req = base;
        req.lite_output = true;
        combos.push_back(req);
        return combos;
    }

    std::function<void(std::size_t, nlohmann::json)> build_combo;
    build_combo = [&](std::size_t index, nlohmann::json combo) {
        if (index >= dimensions.size()) {
            BacktestRequest req = base;
            req.lite_output = true;
            req.optimize_combo = combo;
            for (const auto& [key, value] : combo.items()) {
                apply_strategy_param(req, key, value);
            }
            combos.push_back(req);
            return;
        }
        for (const auto& value : dimensions[index].values) {
            nlohmann::json next = combo;
            next[dimensions[index].key] = value;
            build_combo(index + 1, next);
        }
    };
    build_combo(0, nlohmann::json::object());
    return combos;
}

nlohmann::json backtest_request_to_params(const BacktestRequest& req) {
    nlohmann::json params = {{"days", req.days},
                             {"k1", req.k1},
                             {"k2", req.k2},
                             {"ma_fast", req.ma_fast},
                             {"ma_slow", req.ma_slow},
                             {"macd_short", req.macd_short},
                             {"macd_long", req.macd_long},
                             {"macd_signal", req.macd_signal},
                             {"boll_period", req.boll_period},
                             {"boll_stddev", req.boll_stddev},
                             {"intra_bar", req.intra_bar}};
    if (!req.optimize_combo.empty()) {
        params.merge_patch(req.optimize_combo);
    }
    return params;
}

}  // namespace

BacktestEngine::BacktestEngine(StorageEngine& storage) : storage_(storage) {}

bool BacktestEngine::initialize() {
    const auto option_path = storage_.config_path("Option_Rules.json");
    if (!option_rules_.load(option_path.string())) {
        quant_sev::core::Logger::instance().warn("Option_Rules.json 加载失败: " + option_path.string());
    }

    const auto strategy_path = storage_.config_path("Strategy_list.json");
    std::ifstream in(strategy_path);
    if (in) {
        nlohmann::json doc;
        in >> doc;
        strategy_catalog_ = doc.value("strategies", nlohmann::json::array());
    }
    return true;
}

void BacktestEngine::set_progress(bool running, const std::string& phase, int current, int total,
                                  const std::string& message, const std::string& instrument_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    progress_.running = running;
    progress_.phase = phase;
    progress_.current = current;
    progress_.total = total;
    progress_.percent = total > 0 ? std::min(100, current * 100 / total) : (running ? 0 : 100);
    progress_.message = message;
    if (!instrument_id.empty()) {
        progress_.instrument_id = instrument_id;
    }
}

nlohmann::json BacktestEngine::progress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {{"running", progress_.running},
            {"phase", progress_.phase},
            {"instrument_id", progress_.instrument_id},
            {"current", progress_.current},
            {"total", progress_.total},
            {"percent", progress_.percent},
            {"message", progress_.message}};
}

bool BacktestEngine::is_running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return progress_.running;
}

void BacktestEngine::apply_contract_spec(BacktestRequest& request) const {
    const auto parsed = storage_.parse_instrument(request.instrument_id);
    if (!parsed) {
        if (request.contract_multiplier <= 0) {
            request.contract_multiplier = 10;
        }
        if (request.tick_size <= 0) {
            request.tick_size = 1;
        }
        return;
    }

    const auto spec = option_rules_.lookup(parsed->product);
    if (spec) {
        if (request.contract_multiplier <= 0) {
            request.contract_multiplier = spec->multiplier;
        }
        if (request.tick_size <= 0) {
            request.tick_size = spec->tick;
        }
    } else {
        if (request.contract_multiplier <= 0) {
            request.contract_multiplier = 10;
        }
        if (request.tick_size <= 0) {
            request.tick_size = 1;
        }
    }
}

BacktestRequest BacktestEngine::request_for_strategy(const BacktestRequest& base, const std::string& strategy,
                                                      const nlohmann::json& strategy_row) {
    BacktestRequest req = base;
    req.strategy = strategy;
    if (!strategy_row.is_null()) {
        if (strategy_row.contains("logic")) {
            req.strategy = strategy_row.value("logic", strategy);
        }
        req.period = strategy_row.value("period", req.period);
        req.days = strategy_row.value("days", req.days);
        req.k1 = strategy_row.value("k1", req.k1);
        req.k2 = strategy_row.value("k2", req.k2);
        req.volume = strategy_row.value("default_volume", req.volume);
        req.ma_fast = strategy_row.value("ma_fast", req.ma_fast);
        req.ma_slow = strategy_row.value("ma_slow", req.ma_slow);
        req.macd_short = strategy_row.value("macd_short", req.macd_short);
        req.macd_long = strategy_row.value("macd_long", req.macd_long);
        req.macd_signal = strategy_row.value("macd_signal", req.macd_signal);
        req.boll_period = strategy_row.value("boll_period", req.boll_period);
        req.boll_stddev = strategy_row.value("boll_stddev", req.boll_stddev);
        if (strategy_row.contains("default_instrument_id") && base.instrument_id.empty()) {
            req.instrument_id = strategy_row.value("default_instrument_id", req.instrument_id);
        }
    }
    return req;
}

nlohmann::json BacktestEngine::contract_spec(const std::string& instrument_id) const {
    nlohmann::json result = {{"instrument_id", instrument_id}, {"found", false}};
    const auto parsed = storage_.parse_instrument(instrument_id);
    if (!parsed) {
        result["error"] = "无法解析合约代码";
        return result;
    }

    result["product"] = parsed->product;
    result["exchange"] = parsed->exchange;
    const auto spec = option_rules_.lookup(parsed->product);
    if (spec) {
        result["found"] = true;
        result["symbol"] = spec->symbol;
        result["name"] = spec->name;
        result["multiplier"] = spec->multiplier;
        result["tick"] = spec->tick;
        result["source"] = "Option_Rules.json";
    } else {
        result["multiplier"] = 10;
        result["tick"] = 1;
        result["source"] = "default";
    }
    return result;
}

nlohmann::json BacktestEngine::list_strategies() const {
    const auto registered = StrategyRegistry::instance().list_logics();
    const auto& names = strategy_display_names();
    nlohmann::json logics = nlohmann::json::array();
    for (const auto& id : registered) {
        const auto it = names.find(id);
        logics.push_back({{"id", id},
                          {"name", it != names.end() ? it->second : id},
                          {"inputs", StrategyRegistry::instance().input_specs_json(id)}});
    }
    return {{"logics", logics}, {"catalog", strategy_catalog_}};
}

nlohmann::json BacktestEngine::strategy_inputs(const std::string& strategy_id) const {
    if (!StrategyRegistry::instance().has(strategy_id)) {
        return {{"error", "unknown strategy: " + strategy_id}};
    }
    const auto& names = strategy_display_names();
    const auto it = names.find(strategy_id);
    return {{"strategy", strategy_id},
            {"name", it != names.end() ? it->second : strategy_id},
            {"inputs", StrategyRegistry::instance().input_specs_json(strategy_id)}};
}

std::string BacktestEngine::period_from_ui(const std::string& ui_period) {
    if (ui_period == "1分钟" || ui_period == "m1") {
        return "m1";
    }
    if (ui_period == "5分钟" || ui_period == "m5") {
        return "m5";
    }
    if (ui_period == "15分钟" || ui_period == "m15") {
        return "m15";
    }
    if (ui_period == "1小时" || ui_period == "h1") {
        return "h1";
    }
    if (ui_period == "日线" || ui_period == "d1") {
        return "d1";
    }
    return ui_period.empty() ? "m15" : ui_period;
}

std::string BacktestEngine::period_bar_key(const BarRecord& bar, const std::string& period) {
    if (period == "d1") {
        return bar.date;
    }
    return bar.date + ' ' + bar.time;
}

std::string BacktestEngine::period_bucket_key_from_tick(const TickRecord& tick, const std::string& period) {
    const std::string date = format_trading_date(tick.trading_day);
    const std::string m1_time = m1_time_floor(tick.update_time);
    if (period == "d1") {
        return date;
    }
    if (period == "m1") {
        return date + ' ' + m1_time;
    }
    const std::string m15_key = m15_bucket_key(date, m1_time);
    if (period == "m15") {
        return m15_key;
    }
    if (period == "h1") {
        const std::string m15_time = m15_key.substr(m15_key.find(' ') + 1);
        return date + ' ' + h1_label_for_m15(m15_time);
    }
    return m15_key;
}

std::unordered_map<std::string, std::vector<TickRecord>> BacktestEngine::index_ticks_by_period(
    const std::vector<TickRecord>& ticks, const std::string& period) {
    std::unordered_map<std::string, std::vector<TickRecord>> index;
    for (const auto& tick : ticks) {
        index[period_bucket_key_from_tick(tick, period)].push_back(tick);
    }
    return index;
}

nlohmann::json BacktestEngine::build_summary(const BacktestRequest& request, const MatchEngine& match, int bars,
                                             int ticks_processed) {
    const auto& trades = match.trades();
    double total_pnl = 0;
    double total_fee = 0;
    int wins = 0;
    for (const auto& trade : trades) {
        total_pnl += trade.pnl;
        total_fee += trade.fee;
        if (trade.pnl > 0) {
            wins += 1;
        }
    }

    double peak = request.initial_capital;
    double max_drawdown = 0;
    for (double point : match.equity_curve()) {
        peak = std::max(peak, point);
        if (peak > 0) {
            max_drawdown = std::max(max_drawdown, (peak - point) / peak);
        }
    }

    const double final_equity = match.equity();
    const double total_return = request.initial_capital > 0
                                    ? (final_equity - request.initial_capital) / request.initial_capital
                                    : 0;

    nlohmann::json summary = {{"initial_capital", request.initial_capital},
                              {"final_equity", final_equity},
                              {"total_return", total_return},
                              {"total_pnl", total_pnl},
                              {"total_fee", total_fee},
                              {"total_slippage_cost", match.total_slippage_cost()},
                              {"trade_count", trades.size()},
                              {"win_rate", trades.empty() ? 0.0
                                                           : static_cast<double>(wins) /
                                                                 static_cast<double>(trades.size())},
                              {"max_drawdown", max_drawdown},
                              {"bars_processed", bars},
                              {"contract_multiplier", resolved_multiplier(request)},
                              {"tick_size", resolved_tick_size(request)},
                              {"slippage_ticks", request.slippage_ticks},
                              {"fee_rate", request.fee_rate},
                              {"fee_per_lot", request.fee_per_lot},
                              {"margin_rate", request.margin_rate},
                              {"peak_margin", match.peak_margin()},
                              {"margin_rejects", match.margin_rejects()}};
    if (ticks_processed > 0) {
        summary["ticks_processed"] = ticks_processed;
    }
    return summary;
}

nlohmann::json BacktestEngine::build_comparison(const nlohmann::json& bar_result, const nlohmann::json& tick_result) {
    const auto& bar_summary = bar_result.value("summary", nlohmann::json::object());
    const auto& tick_summary = tick_result.value("summary", nlohmann::json::object());
    const double bar_return = bar_summary.value("total_return", 0.0);
    const double tick_return = tick_summary.value("total_return", 0.0);
    const int bar_trades = bar_summary.value("trade_count", 0);
    const int tick_trades = tick_summary.value("trade_count", 0);

    return {{"bar_total_return", bar_return},
            {"tick_total_return", tick_return},
            {"return_diff", tick_return - bar_return},
            {"bar_trade_count", bar_trades},
            {"tick_trade_count", tick_trades},
            {"trade_count_diff", tick_trades - bar_trades},
            {"bar_max_drawdown", bar_summary.value("max_drawdown", 0.0)},
            {"tick_max_drawdown", tick_summary.value("max_drawdown", 0.0)},
            {"bar_final_equity", bar_summary.value("final_equity", 0.0)},
            {"tick_final_equity", tick_summary.value("final_equity", 0.0)}};
}

nlohmann::json BacktestEngine::run_strategy_on_bars(
    const BacktestRequest& request, const std::string& period, const std::vector<BarRecord>& bars,
    int ticks_processed, const std::unordered_map<std::string, std::vector<TickRecord>>* ticks_index,
    const std::string& match_style) {
    if (bars.empty()) {
        return {{"error", "no bars after synthesis"}};
    }

    BacktestRequest req = request;
    apply_contract_spec(req);

    if (!StrategyRegistry::instance().has(req.strategy)) {
        return {{"error", "unknown strategy: " + req.strategy}};
    }

    StrategySession session(req.strategy);
    if (!session.valid()) {
        return {{"error", "failed to load strategy: " + req.strategy}};
    }

    const int total_bars = static_cast<int>(bars.size());
    set_progress(true, progress_.phase.empty() ? "bar" : progress_.phase, 0, total_bars,
                 "开始策略回放…", req.instrument_id);

    const nlohmann::json params = backtest_request_to_params(req);
    session.reset(params, bars, period);

    MatchEngine match(req.initial_capital, req.volume, resolved_multiplier(req), req.fee_rate, req.fee_per_lot,
                      resolved_tick_size(req), req.slippage_ticks, req.margin_rate);

    nlohmann::json signals = nlohmann::json::array();
    nlohmann::json bar_series = nlohmann::json::array();
    nlohmann::json equity_curve_bars = nlohmann::json::array();
    const bool use_intrabar = false;

    for (int i = 0; i < static_cast<int>(bars.size()); ++i) {
        if (i % 50 == 0 || i == total_bars - 1) {
            set_progress(true, progress_.phase.empty() ? "bar" : progress_.phase, i + 1, total_bars,
                         "处理 K 线 " + std::to_string(i + 1) + "/" + std::to_string(total_bars),
                         req.instrument_id);
        }
        const auto& bar = bars[static_cast<std::size_t>(i)];
        if (!req.lite_output) {
            bar_series.push_back({{"index", i},
                                  {"date", bar.date},
                                  {"time", bar.time},
                                  {"open", bar.open},
                                  {"high", bar.high},
                                  {"low", bar.low},
                                  {"close", bar.close},
                                  {"volume", bar.volume}});
        }

        std::optional<StrategyBarSignal> signal;
        const std::vector<TickRecord>* bar_ticks = nullptr;
        std::vector<TickRecord> empty_ticks;
        if (use_intrabar) {
            const std::string key = period_bar_key(bar, period);
            const auto it = ticks_index->find(key);
            bar_ticks = it != ticks_index->end() ? &it->second : &empty_ticks;
        }
        signal = session.on_bar(i, bars, period, use_intrabar ? bar_ticks : nullptr);

        if (signal) {
            match.on_signal(*signal, bar);
            if (!req.lite_output) {
                signals.push_back({{"instrument_id", req.instrument_id},
                                   {"bar", signal->bar_index},
                                   {"action", signal->action},
                                   {"label", signal->label},
                                   {"price", signal->price},
                                   {"match_style", match_style},
                                   {"time", bar.date + " " + bar.time}});
            }
        }
        if (!req.lite_output) {
            equity_curve_bars.push_back({{"bar", i}, {"equity", match.mark_to_market(bar.close)}});
        }
    }

    const auto& last_bar = bars.back();
    match.finalize(static_cast<int>(bars.size()) - 1, last_bar.close);

    const auto summary = build_summary(req, match, static_cast<int>(bars.size()), ticks_processed);
    if (req.lite_output) {
        return {{"ok", true},
                {"instrument_id", req.instrument_id},
                {"period", period},
                {"strategy", req.strategy},
                {"params", combo_params_json(req)},
                {"summary", summary}};
    }

    nlohmann::json equity_curve = nlohmann::json::array();
    for (std::size_t i = 0; i < match.equity_curve().size(); ++i) {
        equity_curve.push_back({{"index", i}, {"equity", match.equity_curve()[i]}});
    }

    nlohmann::json closes = nlohmann::json::array();
    for (const auto& trade : match.trades()) {
        closes.push_back({{"instrument_id", req.instrument_id},
                          {"side", trade.side},
                          {"open_bar", trade.open_bar},
                          {"close_bar", trade.close_bar},
                          {"open_time", bar_time_label(bars, trade.open_bar)},
                          {"close_time", bar_time_label(bars, trade.close_bar)},
                          {"bars_held", trade.bars_held},
                          {"open_price", trade.open_price},
                          {"close_price", trade.close_price},
                          {"pnl", trade.pnl},
                          {"fee", trade.fee},
                          {"slippage_cost", trade.slippage_cost},
                          {"match_style", match_style}});
    }

    const std::string mode_label = ticks_processed > 0 ? "tick" : "bar";
    nlohmann::json chart_indicators = session.chart_indicators(bars, period);

    nlohmann::json result = {{"ok", true},
                    {"instrument_id", req.instrument_id},
                    {"period", period},
                    {"mode", mode_label},
                    {"match_style", match_style},
                    {"strategy", req.strategy},
                    {"contract_spec",
                     {{"multiplier", resolved_multiplier(req)},
                      {"tick", resolved_tick_size(req)},
                      {"slippage_ticks", req.slippage_ticks},
                      {"fee_rate", req.fee_rate},
                      {"fee_per_lot", req.fee_per_lot},
                      {"margin_rate", req.margin_rate}}},
                    {"summary", summary},
                    {"equity_curve", equity_curve},
                    {"equity_curve_bars", equity_curve_bars},
                    {"signals", signals},
                    {"closes", closes},
                    {"bars", bar_series},
                    {"chart_indicators", chart_indicators}};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_result_ = result;
    }
    return result;
}

nlohmann::json BacktestEngine::run_bar_mode(const BacktestRequest& request) {
    BacktestRequest req = request;
    apply_contract_spec(req);

    set_progress(true, "bar", 0, 0, "加载 K 线…", req.instrument_id);

    const std::string period = period_from_ui(req.period);
    BarQuery query;
    query.instrument_id = req.instrument_id;
    query.period = period;
    const bool use_date_range = !req.start_date.empty() || !req.end_date.empty();
    query.limit = use_date_range ? 0 : (req.limit > 0 ? req.limit : 50000);
    auto bars = storage_.query_bars(query);
    if (bars.empty() && period != "m15") {
        query.period = "m15";
        bars = storage_.query_bars(query);
    }
    if (bars.empty()) {
        return {{"error", "no bars for " + req.instrument_id + " period=" + period}};
    }
    bars = filter_bars_by_date(bars, req.start_date, req.end_date);
    if (bars.empty()) {
        return {{"error", "date range has no bars for " + req.instrument_id + " (" + req.start_date + " ~ " +
                          req.end_date + ")"}};
    }
    nlohmann::json result = run_strategy_on_bars(req, query.period, bars, 0, nullptr, "bar_close");
    if (result.contains("error") || req.lite_output) {
        return result;
    }
    const auto file_info = storage_.bar_file_info(req.instrument_id, query.period);
    nlohmann::json data_source = {{"requested_period", period},
                                  {"resolved_period", query.period},
                                  {"fallback_m15", query.period != period},
                                  {"start_date", req.start_date},
                                  {"end_date", req.end_date},
                                  {"storage_path", file_info.path},
                                  {"file_bar_count", file_info.bar_count},
                                  {"backtest_bar_count", static_cast<int>(bars.size())}};
    if (file_info.last) {
        const auto& b = *file_info.last;
        data_source["file_last"] = {{"date", b.date}, {"time", b.time}, {"close", b.close}};
    }
    if (!bars.empty()) {
        const auto& b = bars.back();
        data_source["backtest_last"] = {{"date", b.date}, {"time", b.time}, {"close", b.close}};
        data_source["aligned"] = file_info.last && file_info.last->date == b.date &&
                                 file_info.last->time == b.time && file_info.last->close == b.close;
    } else {
        data_source["aligned"] = false;
    }
    result["data_source"] = data_source;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_result_ = result;
    }
    return result;
}

nlohmann::json BacktestEngine::run_tick_mode(const BacktestRequest& request) {
    BacktestRequest req = request;
    apply_contract_spec(req);

    set_progress(true, "tick", 0, 0, "加载 Tick…", req.instrument_id);

    const std::string period = period_from_ui(req.period);
    if (period == "m5") {
        return {{"error", "tick 模式暂不支持 m5，请选 m1/m15/h1/d1"}};
    }

    TickQuery tick_query;
    tick_query.instrument_id = req.instrument_id;
    tick_query.limit = req.tick_limit > 0 ? req.tick_limit : 200000;
    const auto ticks = storage_.query_ticks(tick_query);
    if (ticks.empty()) {
        return {{"error", "no ticks for " + req.instrument_id + " (tick.csv)"}};
    }

    std::vector<BarRecord> m1_bars;
    TickBarSynthesizer synthesizer;
    synthesizer.set_bar_closed_callback([&](const BarRecord& bar) { m1_bars.push_back(bar); });
    for (const auto& tick : ticks) {
        synthesizer.on_tick(tick);
    }
    synthesizer.flush();

    if (m1_bars.empty()) {
        return {{"error", "tick 回放未合成 m1 Bar"}};
    }

    auto bars = aggregate_bars(m1_bars, period);
    if (bars.empty()) {
        bars = m1_bars;
    }

    const int bar_limit = req.limit > 0 ? req.limit : 50000;
    if (static_cast<int>(bars.size()) > bar_limit) {
        bars.erase(bars.begin(), bars.end() - bar_limit);
    }

    const std::string match_style = req.intra_bar ? "intra_bar" : "bar_close";
    if (req.intra_bar && req.strategy == "dual_thrust") {
        const auto ticks_index = index_ticks_by_period(ticks, period);
        return run_strategy_on_bars(req, period, bars, static_cast<int>(ticks.size()), &ticks_index, match_style);
    }
    return run_strategy_on_bars(req, period, bars, static_cast<int>(ticks.size()), nullptr, match_style);
}

nlohmann::json BacktestEngine::run_compare_mode(const BacktestRequest& request) {
    set_progress(true, "compare", 0, 2, "Bar 模式回测…", request.instrument_id);
    BacktestRequest bar_req = request;
    bar_req.mode = "bar";
    const auto bar_result = run_bar_mode(bar_req);
    if (bar_result.contains("error")) {
        return bar_result;
    }

    set_progress(true, "compare", 1, 2, "Tick 模式回测…", request.instrument_id);
    BacktestRequest tick_req = request;
    tick_req.mode = "tick";
    tick_req.intra_bar = true;
    const auto tick_result = run_tick_mode(tick_req);
    if (tick_result.contains("error")) {
        return tick_result;
    }

    nlohmann::json compare_result = {{"ok", true},
                    {"mode", "compare"},
                    {"instrument_id", request.instrument_id},
                    {"period", period_from_ui(request.period)},
                    {"strategy", request.strategy},
                    {"bar", bar_result},
                    {"tick", tick_result},
                    {"comparison", build_comparison(bar_result, tick_result)}};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_result_ = compare_result;
    }
    return compare_result;
}

nlohmann::json BacktestEngine::run_multi_mode(const BacktestRequest& request) {
    std::vector<BacktestRequest> runs;
    BacktestRequest base = request;
    base.mode = "bar";

    if (!request.strategy_ids.empty()) {
        for (const auto& strategy_id : request.strategy_ids) {
            nlohmann::json row = nlohmann::json();
            for (const auto& item : strategy_catalog_) {
                if (item.value("id", "") == strategy_id) {
                    row = item;
                    break;
                }
            }
            if (row.is_null()) {
                return {{"error", "未找到策略: " + strategy_id}};
            }
            runs.push_back(request_for_strategy(base, row.value("logic", "dual_thrust"), row));
        }
    } else {
        const auto& names = request.strategies.empty()
                                ? std::vector<std::string>{request.strategy.empty() ? "dual_thrust" : request.strategy}
                                : request.strategies;
        for (const auto& name : names) {
            runs.push_back(request_for_strategy(base, name, nlohmann::json()));
        }
    }

    nlohmann::json results = nlohmann::json::array();
    nlohmann::json ranking = nlohmann::json::array();
    const int total = static_cast<int>(runs.size());
    for (int i = 0; i < total; ++i) {
        set_progress(true, "multi", i + 1, total, "策略 " + std::to_string(i + 1) + "/" + std::to_string(total),
                     request.instrument_id);
        const auto& req = runs[static_cast<std::size_t>(i)];
        const auto one = run_bar_mode(req);
        if (one.contains("error")) {
            return one;
        }
        results.push_back(one);
        const auto& summary = one.value("summary", nlohmann::json::object());
        ranking.push_back({{"strategy", one.value("strategy", req.strategy)},
                           {"total_return", summary.value("total_return", 0.0)},
                           {"trade_count", summary.value("trade_count", 0)},
                           {"max_drawdown", summary.value("max_drawdown", 0.0)},
                           {"final_equity", summary.value("final_equity", 0.0)}});
    }

    std::sort(ranking.begin(), ranking.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
        return a.value("total_return", 0.0) > b.value("total_return", 0.0);
    });

    nlohmann::json multi_result = {{"ok", true},
                    {"mode", "multi"},
                    {"instrument_id", request.instrument_id},
                    {"period", period_from_ui(request.period)},
                    {"results", results},
                    {"ranking", ranking}};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_result_ = multi_result;
    }
    return multi_result;
}

nlohmann::json BacktestEngine::run_multi_symbol_mode(const BacktestRequest& request) {
    std::vector<std::string> symbols = request.instrument_ids;
    if (symbols.empty() && !request.instrument_id.empty()) {
        symbols.push_back(request.instrument_id);
    }
    if (symbols.size() <= 1) {
        BacktestRequest single = request;
        single.mode = "bar";
        return run_bar_mode(single);
    }

    nlohmann::json results = nlohmann::json::array();
    nlohmann::json ranking = nlohmann::json::array();
    nlohmann::json all_signals = nlohmann::json::array();
    nlohmann::json all_closes = nlohmann::json::array();
    nlohmann::json errors = nlohmann::json::array();
    const int total = static_cast<int>(symbols.size());
    for (int i = 0; i < total; ++i) {
        const auto& symbol = symbols[static_cast<std::size_t>(i)];
        set_progress(true, "multi_symbol", i + 1, total,
                     "合约 " + symbol + " (" + std::to_string(i + 1) + "/" + std::to_string(total) + ")", symbol);
        BacktestRequest req = request;
        req.instrument_id = symbol;
        req.mode = "bar";
        req.lite_output = false;
        const auto one = run_bar_mode(req);
        if (one.contains("error")) {
            errors.push_back({{"instrument_id", symbol}, {"error", one.value("error", "unknown")}});
            ranking.push_back({{"instrument_id", symbol},
                               {"strategy", req.strategy},
                               {"total_return", 0.0},
                               {"trade_count", 0},
                               {"max_drawdown", 0.0},
                               {"win_rate", 0.0},
                               {"final_equity", 0.0},
                               {"error", one.value("error", "unknown")}});
            continue;
        }
        results.push_back(one);
        for (const auto& sig : one.value("signals", nlohmann::json::array())) {
            nlohmann::json row = sig;
            if (!row.contains("instrument_id")) {
                row["instrument_id"] = symbol;
            }
            all_signals.push_back(row);
        }
        for (const auto& close : one.value("closes", nlohmann::json::array())) {
            nlohmann::json row = close;
            if (!row.contains("instrument_id")) {
                row["instrument_id"] = symbol;
            }
            all_closes.push_back(row);
        }
        const auto& summary = one.value("summary", nlohmann::json::object());
        ranking.push_back({{"instrument_id", symbol},
                           {"strategy", one.value("strategy", req.strategy)},
                           {"total_return", summary.value("total_return", 0.0)},
                           {"trade_count", summary.value("trade_count", 0)},
                           {"max_drawdown", summary.value("max_drawdown", 0.0)},
                           {"win_rate", summary.value("win_rate", 0.0)},
                           {"final_equity", summary.value("final_equity", 0.0)}});
    }

    if (results.empty()) {
        return {{"error", "multi_symbol: 所有合约均无可用 K 线"}, {"errors", errors}, {"instrument_ids", symbols}};
    }

    std::sort(ranking.begin(), ranking.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
        return a.value("total_return", 0.0) > b.value("total_return", 0.0);
    });

    nlohmann::json ms_result = {{"ok", true},
                                {"mode", "multi_symbol"},
                                {"instrument_ids", symbols},
                                {"period", period_from_ui(request.period)},
                                {"strategy", request.strategy},
                                {"results", results},
                                {"ranking", ranking},
                                {"all_signals", all_signals},
                                {"all_closes", all_closes},
                                {"errors", errors}};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_result_ = ms_result;
    }
    return ms_result;
}

nlohmann::json BacktestEngine::run_optimize_single(const BacktestRequest& request) {
    const auto combos = build_optimize_combos(request);
    if (combos.empty()) {
        return {{"error", "参数网格为空"}};
    }

    const std::string metric = request.optimize_metric.empty() ? "total_return" : request.optimize_metric;
    nlohmann::json ranking = nlohmann::json::array();
    const int total = static_cast<int>(combos.size());

    set_progress(true, "optimize", 0, total, "准备参数优化…", request.instrument_id);

    for (int i = 0; i < total; ++i) {
        const auto& combo = combos[static_cast<std::size_t>(i)];
        set_progress(true, "optimize", i + 1, total,
                     "优化组合 " + std::to_string(i + 1) + "/" + std::to_string(total), request.instrument_id);
        BacktestRequest req = combo;
        req.mode = "bar";
        const auto one = run_bar_mode(req);
        if (one.contains("error")) {
            nlohmann::json row = combo_params_json(combo);
            row["error"] = one.value("error", "unknown");
            row["total_return"] = 0.0;
            row["max_drawdown"] = 1.0;
            row["win_rate"] = 0.0;
            row["trade_count"] = 0;
            ranking.push_back(row);
            continue;
        }
        const auto& summary = one.value("summary", nlohmann::json::object());
        nlohmann::json row = combo_params_json(combo);
        row["total_return"] = summary.value("total_return", 0.0);
        row["max_drawdown"] = summary.value("max_drawdown", 0.0);
        row["win_rate"] = summary.value("win_rate", 0.0);
        row["trade_count"] = summary.value("trade_count", 0);
        row["final_equity"] = summary.value("final_equity", 0.0);
        ranking.push_back(row);
    }

    const auto metric_key = metric == "min_drawdown" ? "max_drawdown" : metric;
    std::sort(ranking.begin(), ranking.end(), [&](const nlohmann::json& a, const nlohmann::json& b) {
        if (metric == "min_drawdown" || metric == "max_drawdown") {
            return a.value(metric_key, 1.0) < b.value(metric_key, 1.0);
        }
        return a.value(metric_key, 0.0) > b.value(metric_key, 0.0);
    });

    nlohmann::json best = ranking.empty() ? nlohmann::json::object() : ranking.front();
    return {{"ok", true},
            {"mode", "optimize"},
            {"instrument_id", request.instrument_id},
            {"period", period_from_ui(request.period)},
            {"strategy", request.strategy},
            {"optimize_metric", metric},
            {"total_combos", total},
            {"ranking", ranking},
            {"best", best}};
}

nlohmann::json BacktestEngine::run_multi_symbol_optimize_mode(const BacktestRequest& request) {
    const auto symbols = request.instrument_ids.empty()
                             ? std::vector<std::string>{request.instrument_id}
                             : request.instrument_ids;
    nlohmann::json results = nlohmann::json::array();
    nlohmann::json best_summary = nlohmann::json::array();
    nlohmann::json errors = nlohmann::json::array();

    for (std::size_t i = 0; i < symbols.size(); ++i) {
        const auto& symbol = symbols[i];
        set_progress(true, "multi_optimize", static_cast<int>(i) + 1, static_cast<int>(symbols.size()),
                     "优化合约 " + symbol, symbol);
        BacktestRequest sub = request;
        sub.instrument_id = symbol;
        sub.instrument_ids = {symbol};
        const auto one = run_optimize_single(sub);
        if (one.contains("error")) {
            errors.push_back({{"instrument_id", symbol}, {"error", one.value("error", "unknown")}});
            continue;
        }
        nlohmann::json one_result = one;
        one_result["instrument_id"] = symbol;
        results.push_back(one_result);
        nlohmann::json best_row = one_result.value("best", nlohmann::json::object());
        best_row["instrument_id"] = symbol;
        best_summary.push_back(best_row);
    }

    if (results.empty()) {
        return {{"error", "multi_optimize: 所有合约均无有效结果"}, {"errors", errors}, {"instrument_ids", symbols}};
    }

    std::sort(best_summary.begin(), best_summary.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
        return a.value("total_return", 0.0) > b.value("total_return", 0.0);
    });

    nlohmann::json opt_result = {{"ok", true},
                                 {"mode", "multi_optimize"},
                                 {"instrument_ids", symbols},
                                 {"period", period_from_ui(request.period)},
                                 {"strategy", request.strategy},
                                 {"optimize_metric", request.optimize_metric},
                                 {"results", results},
                                 {"best_summary", best_summary},
                                 {"best", best_summary.empty() ? nlohmann::json::object() : best_summary.front()},
                                 {"errors", errors}};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_result_ = opt_result;
    }
    return opt_result;
}

nlohmann::json BacktestEngine::run_optimize_mode(const BacktestRequest& request) {
    if (request.instrument_ids.size() > 1) {
        return run_multi_symbol_optimize_mode(request);
    }
    nlohmann::json opt_result = run_optimize_single(request);
    if (opt_result.contains("error")) {
        return opt_result;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_result_ = opt_result;
    }
    return opt_result;
}

nlohmann::json BacktestEngine::run(const BacktestRequest& request) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (progress_.running) {
            return {{"error", "backtest already running"}};
        }
        progress_.running = true;
    }

    if (request.instrument_id.empty() && request.instrument_ids.empty()) {
        set_progress(false, "", 0, 0, "缺少 instrument_id");
        return {{"error", "instrument_id required"}};
    }

    const std::string mode = request.mode.empty() ? "bar" : request.mode;
    const std::string inst =
        request.instrument_id.empty() ? request.instrument_ids.front() : request.instrument_id;
    set_progress(true, mode, 0, 0, "启动回测…", inst);

    nlohmann::json result;
    int final_total = 0;
    try {
        if (mode == "optimize") {
            result = run_optimize_mode(request);
        } else if (mode == "multi_symbol" ||
                   (!request.instrument_ids.empty() && request.instrument_ids.size() > 1)) {
            result = run_multi_symbol_mode(request);
        } else if (mode == "multi") {
            result = run_multi_mode(request);
        } else if (mode == "compare") {
            result = run_compare_mode(request);
        } else if (mode == "tick") {
            result = run_tick_mode(request);
        } else {
            result = run_bar_mode(request);
        }
    } catch (const std::exception& ex) {
        result = {{"error", ex.what()}};
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        final_total = progress_.total;
    }
    set_progress(false, mode, final_total, final_total,
                 result.contains("error") ? result.value("error", "失败") : "完成", inst);

    if (result.contains("error") && !result.contains("ok")) {
        std::lock_guard<std::mutex> lock(mutex_);
        last_result_ = result;
    }
    return result;
}

nlohmann::json BacktestEngine::last_result() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_result_;
}

}  // namespace quant_sev::bll
