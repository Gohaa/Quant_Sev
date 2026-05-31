#include "Strategy/StrategyRuntime.hpp"

#include <algorithm>

namespace quant_sev::bll {
namespace {

std::optional<std::pair<double, double>> dual_thrust_bands(const std::vector<BarRecord>& bars, int index, int lookback,
                                                           double k1, double k2) {
    if (index <= 0 || index >= static_cast<int>(bars.size())) {
        return std::nullopt;
    }
    if (lookback <= 0) {
        lookback = 1;
    }
    if (index < lookback) {
        return std::nullopt;
    }
    const int start = index - lookback;
    double range_high = bars[static_cast<std::size_t>(start)].high;
    double range_low = bars[static_cast<std::size_t>(start)].low;
    for (int i = start + 1; i < index; ++i) {
        range_high = std::max(range_high, bars[static_cast<std::size_t>(i)].high);
        range_low = std::min(range_low, bars[static_cast<std::size_t>(i)].low);
    }
    const auto& bar = bars[static_cast<std::size_t>(index)];
    const double range = range_high - range_low;
    return {{bar.open + k1 * range, bar.open - k2 * range}};
}

class DualThrustStrategy final : public CStrategyBase {
public:
    //--- input（当前周期 K 线上的回看根数；周期由外部 Bar 引擎聚合）
    int InpDays = 4;
    double InpK1 = 0.5;
    double InpK2 = 0.5;

    void apply_inputs(const nlohmann::json& params) override {
        params_ = params;
        InpDays = params.value("days", 4);
        InpK1 = params.value("k1", 0.5);
        InpK2 = params.value("k2", 0.5);
    }

    std::vector<StrategyInputSpec> input_specs() const override {
        return {
            QUANT_SEV_INPUT_INT("days", "InpDays", "回看 K 线根数", 4, 3, 1, 5, 1, 500),
            QUANT_SEV_INPUT_DOUBLE("k1", "InpK1", "K1", 0.5, 0.3, 0.2, 0.7, 0.01, 10.0),
            QUANT_SEV_INPUT_DOUBLE("k2", "InpK2", "K2", 0.5, 0.3, 0.2, 0.7, 0.01, 10.0),
        };
    }

    void OnInit(StrategyContext& ctx) override {
        apply_inputs(ctx.params);
        position_ = 0;
    }

    void OnDeinit(StrategyContext& ctx, StrategyDeinitReason reason) override {
        (void)ctx;
        (void)reason;
        position_ = 0;
    }

    void OnTrade(StrategyContext& ctx) override { (void)ctx; }

    void OnTimer(StrategyContext& ctx) override { (void)ctx; }

    void OnBar(StrategyContext& ctx, std::optional<StrategyBarSignal>& signal) override {
        if (!ctx.bars) {
            return;
        }
        const int index = ctx.bar_index;
        const auto lines = dual_thrust_bands(*ctx.bars, index, InpDays, InpK1, InpK2);
        if (!lines) {
            return;
        }
        emit_at_bar(*ctx.bars, index, lines->first, lines->second, signal);
    }

    void OnTick(StrategyContext& ctx, std::optional<StrategyBarSignal>& signal) override {
        (void)ctx;
        (void)signal;
    }

    nlohmann::json chart_indicators(const std::vector<BarRecord>& bars, const std::string& period) override {
        (void)period;
        nlohmann::json upper = nlohmann::json::array();
        nlohmann::json lower = nlohmann::json::array();
        for (int i = 0; i < static_cast<int>(bars.size()); ++i) {
            const auto bands = dual_thrust_bands(bars, i, InpDays, InpK1, InpK2);
            upper.push_back(bands ? nlohmann::json(bands->first) : nlohmann::json());
            lower.push_back(bands ? nlohmann::json(bands->second) : nlohmann::json());
        }
        return {{"dual_upper", upper}, {"dual_lower", lower}};
    }

private:
    int position_{0};

    void emit_at_bar(const std::vector<BarRecord>& bars, int index, double buy_line, double sell_line,
                     std::optional<StrategyBarSignal>& signal) {
        const auto& bar = bars[static_cast<std::size_t>(index)];
        StrategyBarSignal out;
        out.bar_index = index;
        out.price = bar.close;

        if (position_ <= 0 && bar.high >= buy_line) {
            if (position_ < 0) {
                out.action = "close_short";
                out.label = "DualThrust 平空";
                position_ = 0;
                signal = out;
                return;
            }
            out.action = "buy";
            out.label = "DualThrust 开多";
            position_ = 1;
            signal = out;
            return;
        }
        if (position_ >= 0 && bar.low <= sell_line) {
            if (position_ > 0) {
                out.action = "close_long";
                out.label = "DualThrust 平多";
                position_ = 0;
                signal = out;
                return;
            }
            out.action = "sell";
            out.label = "DualThrust 开空";
            position_ = -1;
            signal = out;
        }
    }
};

}  // namespace

QUANT_SEV_REGISTER_C_STRATEGY("dual_thrust", DualThrustStrategy);

}  // namespace quant_sev::bll
