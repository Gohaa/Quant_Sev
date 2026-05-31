#include "Strategy/StrategyRuntime.hpp"

namespace quant_sev::bll {
namespace {

class MacdStrategy final : public CStrategyBase {
public:
    //--- input
    int InpShort = 12;
    int InpLong = 26;
    int InpSignal = 9;

    void apply_inputs(const nlohmann::json& params) override {
        params_ = params;
        InpShort = params.value("macd_short", 12);
        InpLong = params.value("macd_long", 26);
        InpSignal = params.value("macd_signal", 9);
        if (InpLong <= InpShort) {
            InpLong = InpShort + 1;
        }
    }

    std::vector<StrategyInputSpec> input_specs() const override {
        return {
            QUANT_SEV_INPUT_INT("macd_short", "InpShort", "MACD 短周期", 12, 10, 2, 14, 2, 50),
            QUANT_SEV_INPUT_INT("macd_long", "InpLong", "MACD 长周期", 26, 22, 4, 30, 3, 100),
            QUANT_SEV_INPUT_INT("macd_signal", "InpSignal", "MACD 信号线", 9, 7, 2, 11, 2, 30),
        };
    }

    void OnInit(StrategyContext& ctx) override {
        apply_inputs(ctx.params);
        position_ = 0;
        prev_macd_above_.reset();
        ctx.ensure_indicator("macd", {static_cast<double>(InpShort), static_cast<double>(InpLong),
                                      static_cast<double>(InpSignal)});
    }

    void OnDeinit(StrategyContext& ctx, StrategyDeinitReason reason) override {
        (void)ctx;
        (void)reason;
        position_ = 0;
        prev_macd_above_.reset();
    }

    void OnBar(StrategyContext& ctx, std::optional<StrategyBarSignal>& signal) override {
        if (!ctx.bars) {
            return;
        }
        const int index = ctx.bar_index;
        if (index + 1 < InpLong) {
            return;
        }

        auto& series = ctx.ensure_indicator("macd", {static_cast<double>(InpShort), static_cast<double>(InpLong),
                                                   static_cast<double>(InpSignal)});
        const auto macd = indicator_value_at(series, "macd", index);
        const auto sig = indicator_value_at(series, "macd_signal", index);
        if (!macd || !sig) {
            return;
        }

        const bool macd_above = *macd > *sig;
        if (!prev_macd_above_) {
            prev_macd_above_ = macd_above;
            return;
        }

        const bool crossed_up = !*prev_macd_above_ && macd_above;
        const bool crossed_down = *prev_macd_above_ && !macd_above;
        prev_macd_above_ = macd_above;

        const auto& bar = (*ctx.bars)[static_cast<std::size_t>(index)];
        StrategyBarSignal out;
        out.bar_index = index;
        out.price = bar.close;

        if (crossed_up) {
            if (position_ < 0) {
                out.action = "close_short";
                out.label = "MACD 平空";
                position_ = 0;
                signal = out;
                return;
            }
            if (position_ == 0) {
                out.action = "buy";
                out.label = "MACD 开多";
                position_ = 1;
                signal = out;
            }
            return;
        }

        if (crossed_down) {
            if (position_ > 0) {
                out.action = "close_long";
                out.label = "MACD 平多";
                position_ = 0;
                signal = out;
                return;
            }
            if (position_ == 0) {
                out.action = "sell";
                out.label = "MACD 开空";
                position_ = -1;
                signal = out;
            }
        }
    }

    nlohmann::json chart_indicators(const std::vector<BarRecord>& bars, const std::string& period) override {
        (void)period;
        StrategyChartContext chart_ctx(bars, period, params_);
        const auto& series = chart_ctx.ctx.ensure_indicator("macd", {static_cast<double>(InpShort),
                                                                     static_cast<double>(InpLong),
                                                                     static_cast<double>(InpSignal)});
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
    std::optional<bool> prev_macd_above_;
};

}  // namespace

QUANT_SEV_REGISTER_C_STRATEGY("macd", MacdStrategy);

}  // namespace quant_sev::bll
