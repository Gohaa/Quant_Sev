#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "Storage/StorageTypes.hpp"

namespace quant_sev::bll {

/** 与 Bar 序列等长的指标输出（无前值处为 nullopt） */
using IndicatorSeriesMap = std::unordered_map<std::string, std::vector<std::optional<double>>>;

/** 在内存 Bar 序列上调用 Tulip 指标（与 IndicatorEngine 同源） */
IndicatorSeriesMap compute_indicator_on_bars(const std::vector<BarRecord>& bars, const std::string& name,
                                             const std::vector<double>& options);

std::optional<double> indicator_value_at(const IndicatorSeriesMap& series, const std::string& output_key,
                                         int bar_index);

}  // namespace quant_sev::bll
