#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "Indicator/IndicatorCompute.hpp"
#include "Storage/StorageTypes.hpp"
#include "Strategy/StrategyTypes.hpp"

namespace quant_sev::bll {

enum class StrategyInputType {
    Int = 0,
    Double = 1
};

/** MQL5 input 等价物：策略在 CTA 中声明，供回测/优化 UI 自动发现 */
struct StrategyInputSpec {
    std::string key;
    std::string var_name;
    std::string label;
    StrategyInputType type{StrategyInputType::Int};
    double default_value{0};
    double optimize_start{0};
    double optimize_step{1};
    double optimize_stop{0};
    double min_value{0};
    double max_value{0};
};

#define QUANT_SEV_INPUT_INT(key, var, label, def, ostart, ostep, ostop, vmin, vmax)                       \
    StrategyInputSpec {                                                                                  \
        key, var, label, StrategyInputType::Int, static_cast<double>(def), static_cast<double>(ostart),  \
            static_cast<double>(ostep), static_cast<double>(ostop), static_cast<double>(vmin),            \
            static_cast<double>(vmax)                                                                    \
    }

#define QUANT_SEV_INPUT_DOUBLE(key, var, label, def, ostart, ostep, ostop, vmin, vmax)                   \
    StrategyInputSpec {                                                                                  \
        key, var, label, StrategyInputType::Double, def, ostart, ostep, ostop, vmin, vmax                  \
    }

enum class StrategyDeinitReason {
    Remove = 0,
    Reload = 1,
    ChartClose = 2,
    Parameters = 3,
    Account = 4,
    Template = 5,
    InitFailed = 6,
    Close = 7
};

/** 策略回调上下文：K 线、指标缓存、参数 */
struct StrategyContext {
    const std::vector<BarRecord>* bars{nullptr};
    const std::vector<TickRecord>* bar_ticks{nullptr};
    const TickRecord* tick{nullptr};
    int bar_index{0};
    std::string period;
    nlohmann::json params;

    IndicatorSeriesMap& ensure_indicator(const std::string& name, const std::vector<double>& options);
    std::optional<double> indicator_at(const std::string& output_key, int index) const;

private:
    friend class StrategySession;
    friend struct StrategyChartContext;
    std::unordered_map<std::string, IndicatorSeriesMap>* indicator_cache_{nullptr};

    static std::string indicator_cache_key(const std::string& name, const std::vector<double>& options);
};

/** 供 chart_indicators 构建指标序列 */
struct StrategyChartContext {
    std::unordered_map<std::string, IndicatorSeriesMap> cache;
    StrategyContext ctx;

    StrategyChartContext(const std::vector<BarRecord>& bars, const std::string& period,
                         const nlohmann::json& params) {
        ctx.bars = &bars;
        ctx.period = period;
        ctx.params = params;
        ctx.indicator_cache_ = &cache;
    }
};

/** C_Strategy 基类：OnInit / OnBar / OnTick / … */
class CStrategyBase {
public:
    virtual ~CStrategyBase() = default;

    virtual void apply_inputs(const nlohmann::json& params) { params_ = params; }
    /** 返回策略 input 列表（类似 MQL5 的 input 变量） */
    virtual std::vector<StrategyInputSpec> input_specs() const { return {}; }
    virtual void OnInit(StrategyContext& ctx) = 0;
    virtual void OnDeinit(StrategyContext& ctx, StrategyDeinitReason reason) { (void)ctx; (void)reason; }
    virtual void OnBar(StrategyContext& ctx, std::optional<StrategyBarSignal>& signal) = 0;
    virtual void OnTick(StrategyContext& ctx, std::optional<StrategyBarSignal>& signal) { (void)ctx; (void)signal; }
    virtual void OnTrade(StrategyContext& ctx) { (void)ctx; }
    virtual void OnTimer(StrategyContext& ctx) { (void)ctx; }
    virtual nlohmann::json chart_indicators(const std::vector<BarRecord>& bars, const std::string& period) {
        (void)bars;
        (void)period;
        return nlohmann::json::object();
    }

protected:
    nlohmann::json params_;
};

using StrategyFactory = std::function<std::unique_ptr<CStrategyBase>()>;

class StrategyRegistry {
public:
    static StrategyRegistry& instance();

    void register_strategy(const std::string& logic_id, StrategyFactory factory);
    std::unique_ptr<CStrategyBase> create(const std::string& logic_id) const;
    bool has(const std::string& logic_id) const;
    std::vector<std::string> list_logics() const;
    std::vector<StrategyInputSpec> input_specs(const std::string& logic_id) const;
    nlohmann::json input_specs_json(const std::string& logic_id) const;

private:
    void ensure_builtin_modules() const;

    mutable bool builtins_loaded_{false};
    std::unordered_map<std::string, StrategyFactory> factories_;
};

/** 按需创建策略实例并驱动生命周期 */
class StrategySession {
public:
    explicit StrategySession(std::string logic_id);

    bool valid() const { return instance_ != nullptr; }
    const std::string& logic_id() const { return logic_id_; }

    void reset(const nlohmann::json& params, const std::vector<BarRecord>& bars, const std::string& period);
    void deinit(StrategyDeinitReason reason = StrategyDeinitReason::Remove);

    std::optional<StrategyBarSignal> on_bar(int index, const std::vector<BarRecord>& bars, const std::string& period,
                                            const std::vector<TickRecord>* ticks = nullptr);
    nlohmann::json chart_indicators(const std::vector<BarRecord>& bars, const std::string& period) const;

private:
    StrategyContext make_context(int index, const std::vector<BarRecord>& bars, const std::string& period,
                                 const std::vector<TickRecord>* ticks);

    std::string logic_id_;
    std::unique_ptr<CStrategyBase> instance_;
    nlohmann::json params_;
    std::string period_;
    bool initialized_{false};
    std::unordered_map<std::string, IndicatorSeriesMap> indicator_cache_;
};

nlohmann::json strategy_params_from_backtest(const nlohmann::json& source);

#define QUANT_SEV_REGISTER_C_STRATEGY(logic_id, ClassName)                                           \
    void register_c_strategy_##ClassName() {                                                         \
        quant_sev::bll::StrategyRegistry::instance().register_strategy(                               \
            logic_id, []() -> std::unique_ptr<quant_sev::bll::CStrategyBase> {                       \
                return std::make_unique<ClassName>();                                                \
            });                                                                                      \
    }

}  // namespace quant_sev::bll
