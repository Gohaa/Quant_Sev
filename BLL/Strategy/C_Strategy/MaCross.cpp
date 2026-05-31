#include "Strategy/StrategyRuntime.hpp"

namespace quant_sev::bll {
namespace {

class MaCrossStrategy final : public CStrategyBase {
public:
    //--- input
    int InpFast = 5;
    int InpSlow = 20;

    void apply_inputs(const nlohmann::json& params) override {
        params_ = params;
        InpFast = params.value("ma_fast", params.value("fast", 5));
        InpSlow = params.value("ma_slow", params.value("slow", 20));
        if (InpSlow <= InpFast) {
            InpSlow = InpFast + 1;
        }
    }

    std::vector<StrategyInputSpec> input_specs() const override {
        return {
            QUANT_SEV_INPUT_INT("ma_fast", "InpFast", "快周期", 5, 5, 5, 15, 2, 200),
            QUANT_SEV_INPUT_INT("ma_slow", "InpSlow", "慢周期", 20, 20, 10, 40, 3, 500),
        };
    }

    void OnInit(StrategyContext& ctx) override {
        apply_inputs(ctx.params);
        position_ = 0;
        prev_fast_above_.reset();
        ctx.ensure_indicator("sma", {static_cast<double>(InpFast)});
        ctx.ensure_indicator("sma", {static_cast<double>(InpSlow)});
    }

    void OnDeinit(StrategyContext& ctx, StrategyDeinitReason reason) override {
        (void)ctx;
        (void)reason;
        position_ = 0;
        prev_fast_above_.reset();
    }

    void OnBar(StrategyContext& ctx, std::optional<StrategyBarSignal>& signal) override {
        if (!ctx.bars) {
            return;
        }
        const int index = ctx.bar_index;
        if (index + 1 < InpSlow) {
            return;
        }

        auto& fast_series = ctx.ensure_indicator("sma", {static_cast<double>(InpFast)});
        auto& slow_series = ctx.ensure_indicator("sma", {static_cast<double>(InpSlow)});
        const auto fast_sma = indicator_value_at(fast_series, "sma", index);
        const auto slow_sma = indicator_value_at(slow_series, "sma", index);
        if (!fast_sma || !slow_sma) {
            return;
        }

        const bool fast_above = *fast_sma > *slow_sma;
        if (!prev_fast_above_) {
            prev_fast_above_ = fast_above;
            return;
        }

        const bool crossed_up = !*prev_fast_above_ && fast_above;
        const bool crossed_down = *prev_fast_above_ && !fast_above;
        prev_fast_above_ = fast_above;

        const auto& bar = (*ctx.bars)[static_cast<std::size_t>(index)];
        StrategyBarSignal out;
        out.bar_index = index;
        out.price = bar.close;

        if (crossed_up) {
            if (position_ < 0) {
                out.action = "close_short";
                out.label = "MA Cross 平空";
                position_ = 0;
                signal = out;
                return;
            }
            if (position_ == 0) {
                out.action = "buy";
                out.label = "MA Cross 开多";
                position_ = 1;
                signal = out;
            }
            return;
        }

        if (crossed_down) {
            if (position_ > 0) {
                out.action = "close_long";
                out.label = "MA Cross 平多";
                position_ = 0;
                signal = out;
                return;
            }
            if (position_ == 0) {
                out.action = "sell";
                out.label = "MA Cross 开空";
                position_ = -1;
                signal = out;
            }
        }
    }

    nlohmann::json chart_indicators(const std::vector<BarRecord>& bars, const std::string& period) override {
        (void)period;
        StrategyChartContext chart_ctx(bars, period, params_);
        auto& fast_series = chart_ctx.ctx.ensure_indicator("sma", {static_cast<double>(InpFast)});
        auto& slow_series = chart_ctx.ctx.ensure_indicator("sma", {static_cast<double>(InpSlow)});

        nlohmann::json chart = nlohmann::json::object();
        if (fast_series.count("sma")) {
            chart["ma_fast"] = nlohmann::json::array();
            for (const auto& v : fast_series.at("sma")) {
                chart["ma_fast"].push_back(v ? nlohmann::json(*v) : nlohmann::json());
            }
        }
        if (slow_series.count("sma")) {
            chart["ma_slow"] = nlohmann::json::array();
            for (const auto& v : slow_series.at("sma")) {
                chart["ma_slow"].push_back(v ? nlohmann::json(*v) : nlohmann::json());
            }
        }
        return chart;
    }

private:
    int position_{0};
    std::optional<bool> prev_fast_above_;
};

}  // namespace

QUANT_SEV_REGISTER_C_STRATEGY("ma_cross", MaCrossStrategy);

}  // namespace quant_sev::bll
