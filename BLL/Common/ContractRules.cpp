#include "Common/ContractRules.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>

#include <nlohmann/json.hpp>

namespace quant_sev::bll {

namespace {

std::string upper_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
}

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool starts_with_case(const std::string& haystack, const std::string& prefix, bool upper) {
    if (prefix.size() > haystack.size()) {
        return false;
    }
    const auto left = upper ? upper_copy(haystack) : lower_copy(haystack);
    const auto right = upper ? upper_copy(prefix) : lower_copy(prefix);
    return left.compare(0, right.size(), right) == 0;
}

}  // namespace

bool ContractRules::load(const std::string& rules_json_path) {
    exchanges_.clear();
    std::ifstream in(rules_json_path);
    if (!in) {
        return false;
    }
    nlohmann::json doc;
    in >> doc;
    if (!doc.contains("futureContracts") || !doc["futureContracts"].contains("exchanges")) {
        return false;
    }
    for (const auto& item : doc["futureContracts"]["exchanges"]) {
        ExchangeRule rule;
        rule.exchange = item.value("exchange", "");
        rule.upper_case = item.value("symbolCase", "lower") == "upper";
        rule.year_digits = item.value("yearSuffixDigits", 2);
        rule.month_digits = item.value("deliveryMonthDigits", 2);
        if (item.contains("symbols") && item["symbols"].is_array()) {
            for (const auto& sym : item["symbols"]) {
                rule.symbols.push_back(sym.get<std::string>());
            }
        }
        std::sort(rule.symbols.begin(), rule.symbols.end(),
                  [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
        exchanges_.push_back(std::move(rule));
    }
    return true;
}

std::optional<ParsedInstrument> ContractRules::parse(const std::string& instrument_id) const {
    if (instrument_id.empty()) {
        return std::nullopt;
    }
    for (const auto& ex : exchanges_) {
        for (const auto& sym : ex.symbols) {
            if (!starts_with_case(instrument_id, sym, ex.upper_case)) {
                continue;
            }
            const std::size_t prefix_len = sym.size();
            if (instrument_id.size() < prefix_len + static_cast<std::size_t>(ex.year_digits + ex.month_digits)) {
                continue;
            }
            const std::string rest = instrument_id.substr(prefix_len);
            if (rest.size() != static_cast<std::size_t>(ex.year_digits + ex.month_digits)) {
                continue;
            }
            ParsedInstrument parsed;
            parsed.instrument_id = instrument_id;
            parsed.exchange = ex.exchange;
            parsed.product = ex.upper_case ? upper_copy(sym) : lower_copy(sym);
            parsed.year_suffix = rest.substr(0, static_cast<std::size_t>(ex.year_digits));
            parsed.delivery_month = rest.substr(static_cast<std::size_t>(ex.year_digits));
            parsed.month_slot = parsed.delivery_month;
            return parsed;
        }
    }
    return std::nullopt;
}

}  // namespace quant_sev::bll
