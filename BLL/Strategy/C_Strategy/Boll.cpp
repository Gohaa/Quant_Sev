#include "Strategy/StrategyRuntime.hpp"

namespace quant_sev::bll {
namespace {

class BollStrategy final : public CStrategyBase {
public:
    //--- input
    int InpPeriod = 20;
    double InpStdDev = 2.0;

    void apply_inputs(const nlohmann::json& params) override {
        params_ = params;
        InpPeriod = params.value("boll_period", 20);
        InpStdDev = params.value("boll_stddev", 2.0);
    }

    std::vector<StrategyInputSpec> input_specs() const override {
        return {
            QUANT_SEV_INPUT_INT("boll_period", "InpPeriod", "布林带周期", 20, 15, 5, 25, 5, 200),
            QUANT_SEV_INPUT_DOUBLE("boll_stddev", "InpStdDev", "标准差倍数", 2.0, 1.5, 0.5, 2.5, 0.5, 5.0),
        };
    }

    void OnInit(StrategyContext& ctx) override {
        apply_inputs(ctx.params);
        position_ = 0;
        ctx.ensure_indicator("bbands", {static_cast<double>(InpPeriod), InpStdDev});
    }

    void OnDeinit(StrategyContext& ctx, StrategyDeinitReason reason) override {
        (void)ctx;
        (void)reason;
        position_ = 0;
    }

    void OnBar(StrategyContext& ctx, std::optional<StrategyBarSignal>& signal) override {
        if (!ctx.bars) {
            return;
        }
        const int index = ctx.bar_index;
        auto& series = ctx.ensure_indicator("bbands", {static_cast<double>(InpPeriod), InpStdDev});
        const auto lower = indicator_value_at(series, "bbands_lower", index);
        const auto upper = indicator_value_at(series, "bbands_upper", index);
        if (!lower || !upper) {
            return;
        }

        const auto& bar = (*ctx.bars)[static_cast<std::size_t>(index)];
        StrategyBarSignal out;
        out.bar_index = index;
        out.price = bar.close;

        if (position_ <= 0 && bar.low <= *lower) {
            if (position_ < 0) {
                out.action = "close_short";
                out.label = "BOLL 平空";
                position_ = 0;
                signal = out;
                return;
            }
            out.action = "buy";
            out.label = "BOLL 开多";
            position_ = 1;
            signal = out;
            return;
        }

        if (position_ >= 0 && bar.high >= *upper) {
            if (position_ > 0) {
                out.action = "close_long";
                out.label = "BOLL 平多";
                position_ = 0;
                signal = out;
                return;
            }
            out.action = "sell";
            out.label = "BOLL 开空";
            position_ = -1;
            signal = out;
        }
    }

    nlohmann::json chart_indicators(const std::vector<BarRecord>& bars, const std::string& period) override {
        (void)period;
        StrategyChartContext chart_ctx(bars, period, params_);
        const auto& series = chart_ctx.ctx.ensure_indicator("bbands", {static_cast<double>(InpPeriod), InpStdDev});
        nlohmann::json chart = nlohmann::json::object();
        for (const auto& [key, values] : series) {
            chart[key] = nlohmann::json::array();
            for (const auto& v : values) {
                chart[key].push_back(v ? nlohmann::json(*v) : nlohmann::json());
            }
        }
        return chart;
    }

private:
    int position_{0};
};

}  // namespace

QUANT_SEV_REGISTER_C_STRATEGY("boll", BollStrategy);

}  // namespace quant_sev::bll
