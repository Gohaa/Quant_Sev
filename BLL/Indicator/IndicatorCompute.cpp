#include "Indicator/IndicatorCompute.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <unordered_map>

extern "C" {
#include "indicators.h"
}

namespace quant_sev::bll {

namespace {

std::string to_lower(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

std::string normalize_name(const std::string& name) {
    const std::string lowered = to_lower(name);
    static const std::unordered_map<std::string, std::string> kAliases = {
        {"ma", "sma"},
        {"bb", "bbands"},
        {"boll", "bbands"},
    };
    const auto it = kAliases.find(lowered);
    return it != kAliases.end() ? it->second : lowered;
}

const char* map_input_series(const std::string& input_name, bool& used_real, bool& used_real2) {
    if (input_name == "open") {
        return "open";
    }
    if (input_name == "high") {
        return "high";
    }
    if (input_name == "low") {
        return "low";
    }
    if (input_name == "close") {
        return "close";
    }
    if (input_name == "volume") {
        return "volume";
    }
    if (input_name == "real") {
        if (!used_real) {
            used_real = true;
            return "close";
        }
        used_real2 = true;
        return "open";
    }
    return nullptr;
}

}  // namespace

IndicatorSeriesMap compute_indicator_on_bars(const std::vector<BarRecord>& bars, const std::string& name,
                                               const std::vector<double>& options) {
    IndicatorSeriesMap result;
    if (bars.empty() || name.empty()) {
        return result;
    }

    const std::string indicator_name = normalize_name(name);
    const ti_indicator_info* info = ti_find_indicator(indicator_name.c_str());
    if (info == nullptr) {
        return result;
    }
    if (static_cast<int>(options.size()) < info->options) {
        return result;
    }

    const int size = static_cast<int>(bars.size());
    std::vector<TI_REAL> open(static_cast<std::size_t>(size));
    std::vector<TI_REAL> high(static_cast<std::size_t>(size));
    std::vector<TI_REAL> low(static_cast<std::size_t>(size));
    std::vector<TI_REAL> close(static_cast<std::size_t>(size));
    std::vector<TI_REAL> volume(static_cast<std::size_t>(size));

    for (int i = 0; i < size; ++i) {
        const auto& bar = bars[static_cast<std::size_t>(i)];
        open[static_cast<std::size_t>(i)] = bar.open;
        high[static_cast<std::size_t>(i)] = bar.high;
        low[static_cast<std::size_t>(i)] = bar.low;
        close[static_cast<std::size_t>(i)] = bar.close;
        volume[static_cast<std::size_t>(i)] = static_cast<TI_REAL>(bar.volume);
    }

    TI_REAL ti_options[TI_MAXINDPARAMS]{};
    for (int i = 0; i < info->options; ++i) {
        ti_options[i] = options[static_cast<std::size_t>(i)];
    }

    const int start = info->start(ti_options);
    if (start < 0 || start >= size) {
        return result;
    }

    TI_REAL const* inputs[TI_MAXINDPARAMS]{};
    bool used_real = false;
    bool used_real2 = false;
    for (int i = 0; i < info->inputs; ++i) {
        const char* mapped = map_input_series(info->input_names[i], used_real, used_real2);
        if (mapped == nullptr) {
            return result;
        }
        if (std::string(mapped) == "open") {
            inputs[i] = open.data();
        } else if (std::string(mapped) == "high") {
            inputs[i] = high.data();
        } else if (std::string(mapped) == "low") {
            inputs[i] = low.data();
        } else if (std::string(mapped) == "close") {
            inputs[i] = close.data();
        } else {
            inputs[i] = volume.data();
        }
    }

    std::vector<std::vector<TI_REAL>> output_buffers(static_cast<std::size_t>(info->outputs));
    TI_REAL* outputs[TI_MAXINDPARAMS]{};
    for (int i = 0; i < info->outputs; ++i) {
        output_buffers[static_cast<std::size_t>(i)].assign(static_cast<std::size_t>(size - start), 0.0);
        outputs[i] = output_buffers[static_cast<std::size_t>(i)].data();
    }

    if (info->indicator(size, inputs, ti_options, outputs) != TI_OKAY) {
        return result;
    }

    for (int i = 0; i < info->outputs; ++i) {
        std::vector<std::optional<double>> series(static_cast<std::size_t>(size));
        for (int j = 0; j < size; ++j) {
            if (j < start) {
                series[static_cast<std::size_t>(j)] = std::nullopt;
                continue;
            }
            const TI_REAL value =
                output_buffers[static_cast<std::size_t>(i)][static_cast<std::size_t>(j - start)];
            if (std::isnan(value)) {
                series[static_cast<std::size_t>(j)] = std::nullopt;
            } else {
                series[static_cast<std::size_t>(j)] = value;
            }
        }
        result[info->output_names[i]] = std::move(series);
    }
    return result;
}

std::optional<double> indicator_value_at(const IndicatorSeriesMap& series, const std::string& output_key,
                                         int bar_index) {
    const auto it = series.find(output_key);
    if (it == series.end() || bar_index < 0 || bar_index >= static_cast<int>(it->second.size())) {
        return std::nullopt;
    }
    return it->second[static_cast<std::size_t>(bar_index)];
}

}  // namespace quant_sev::bll
