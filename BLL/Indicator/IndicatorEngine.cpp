#include "Indicator/IndicatorEngine.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_map>
#include <vector>

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

IndicatorEngine::IndicatorEngine(StorageEngine& storage) : storage_(storage) {}

std::string IndicatorEngine::normalize_name(const std::string& name) {
    const std::string lowered = to_lower(name);
    static const std::unordered_map<std::string, std::string> kAliases = {
        {"ma", "sma"},
    };
    const auto it = kAliases.find(lowered);
    return it != kAliases.end() ? it->second : lowered;
}

std::string IndicatorEngine::type_label(int type) {
    switch (type) {
        case TI_TYPE_OVERLAY:
            return "overlay";
        case TI_TYPE_INDICATOR:
            return "indicator";
        case TI_TYPE_MATH:
            return "math";
        case TI_TYPE_SIMPLE:
            return "simple";
        case TI_TYPE_COMPARATIVE:
            return "comparative";
        default:
            return "unknown";
    }
}

nlohmann::json IndicatorEngine::catalog() const {
    nlohmann::json items = nlohmann::json::array();
    for (const ti_indicator_info* info = ti_indicators; info != nullptr && info->name != nullptr; ++info) {
        nlohmann::json inputs = nlohmann::json::array();
        nlohmann::json option_names = nlohmann::json::array();
        nlohmann::json outputs = nlohmann::json::array();
        for (int i = 0; i < info->inputs; ++i) {
            inputs.push_back(info->input_names[i]);
        }
        for (int i = 0; i < info->options; ++i) {
            option_names.push_back(info->option_names[i]);
        }
        for (int i = 0; i < info->outputs; ++i) {
            outputs.push_back(info->output_names[i]);
        }
        items.push_back({{"name", info->name},
                         {"full_name", info->full_name},
                         {"type", type_label(info->type)},
                         {"inputs", inputs},
                         {"options", option_names},
                         {"outputs", outputs}});
    }
    return {{"version", TI_VERSION}, {"count", ti_indicator_count()}, {"indicators", items}};
}

nlohmann::json IndicatorEngine::compute(const IndicatorQuery& query) const {
    if (query.instrument_id.empty()) {
        return {{"error", "instrument_id required"}};
    }
    if (query.name.empty()) {
        return {{"error", "name required"}};
    }

    const std::string indicator_name = normalize_name(query.name);
    const ti_indicator_info* info = ti_find_indicator(indicator_name.c_str());
    if (info == nullptr) {
        return {{"error", "unknown indicator: " + query.name}};
    }
    if (static_cast<int>(query.options.size()) < info->options) {
        return {{"error", "indicator requires " + std::to_string(info->options) + " option(s)"}};
    }

    BarQuery bar_query;
    bar_query.instrument_id = query.instrument_id;
    bar_query.period = query.period;
    bar_query.limit = query.limit > 0 ? query.limit : 500;
    const auto bars = storage_.query_bars(bar_query);
    if (bars.empty()) {
        return {{"error", "no bars for " + query.instrument_id + " period=" + query.period}};
    }

    const int size = static_cast<int>(bars.size());
    std::vector<TI_REAL> open(static_cast<std::size_t>(size));
    std::vector<TI_REAL> high(static_cast<std::size_t>(size));
    std::vector<TI_REAL> low(static_cast<std::size_t>(size));
    std::vector<TI_REAL> close(static_cast<std::size_t>(size));
    std::vector<TI_REAL> volume(static_cast<std::size_t>(size));
    nlohmann::json times = nlohmann::json::array();

    for (int i = 0; i < size; ++i) {
        const auto& bar = bars[static_cast<std::size_t>(i)];
        open[static_cast<std::size_t>(i)] = bar.open;
        high[static_cast<std::size_t>(i)] = bar.high;
        low[static_cast<std::size_t>(i)] = bar.low;
        close[static_cast<std::size_t>(i)] = bar.close;
        volume[static_cast<std::size_t>(i)] = static_cast<TI_REAL>(bar.volume);
        times.push_back(bar.date + " " + bar.time);
    }

    TI_REAL options[TI_MAXINDPARAMS]{};
    for (int i = 0; i < info->options; ++i) {
        options[i] = query.options[static_cast<std::size_t>(i)];
    }

    const int start = info->start(options);
    if (start < 0 || start >= size) {
        return {{"error", "invalid indicator start offset"}};
    }

    TI_REAL const* inputs[TI_MAXINDPARAMS]{};
    bool used_real = false;
    bool used_real2 = false;
    for (int i = 0; i < info->inputs; ++i) {
        const char* mapped = map_input_series(info->input_names[i], used_real, used_real2);
        if (mapped == nullptr) {
            return {{"error", std::string("unsupported input: ") + info->input_names[i]}};
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

    const int rc = info->indicator(size, inputs, options, outputs);
    if (rc != TI_OKAY) {
        return {{"error", "indicator compute failed"}, {"code", rc}};
    }

    nlohmann::json output_json = nlohmann::json::object();
    for (int i = 0; i < info->outputs; ++i) {
        nlohmann::json series = nlohmann::json::array();
        for (int j = 0; j < size; ++j) {
            if (j < start) {
                series.push_back(nullptr);
                continue;
            }
            const TI_REAL value = output_buffers[static_cast<std::size_t>(i)][static_cast<std::size_t>(j - start)];
            if (std::isnan(value)) {
                series.push_back(nullptr);
            } else {
                series.push_back(value);
            }
        }
        output_json[info->output_names[i]] = series;
    }

    nlohmann::json options_json = nlohmann::json::array();
    for (int i = 0; i < info->options; ++i) {
        options_json.push_back(options[i]);
    }

    return {{"instrument_id", query.instrument_id},
            {"period", query.period},
            {"indicator", info->name},
            {"full_name", info->full_name},
            {"start", start},
            {"bars", size},
            {"options", options_json},
            {"times", times},
            {"outputs", output_json}};
}

}  // namespace quant_sev::bll
