#pragma once

#include <string>

#include "Storage/StorageTypes.hpp"

namespace quant_sev::bll {

void merge_bar_record(BarRecord& target, const BarRecord& incoming, bool is_first);

std::string m15_bucket_key(const std::string& date, const std::string& time);

std::string h1_label_for_m15(const std::string& m15_time);

bool m15_closes_h1(const std::string& m15_time);

std::vector<BarRecord> aggregate_bars(const std::vector<BarRecord>& m1_bars, const std::string& period);

}  // namespace quant_sev::bll
