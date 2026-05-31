#include "Common/OptionRules.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>

#include <nlohmann/json.hpp>

namespace quant_sev::bll {

namespace {

std::string normalize_key(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

}  // namespace

bool OptionRules::load(const std::string& rules_json_path) {
    by_symbol_.clear();
    std::ifstream in(rules_json_path);
    if (!in) {
        return false;
    }

    nlohmann::json doc;
    in >> doc;
    if (!doc.contains("symbols") || !doc["symbols"].is_array()) {
        return false;
    }

    for (const auto& item : doc["symbols"]) {
        ContractSpec spec;
        spec.symbol = item.value("symbol", "");
        spec.name = item.value("name", "");
        spec.exchange = item.value("exchange", "");
        spec.multiplier = item.value("multiplier", 10.0);
        spec.tick = item.value("tick", 1.0);
        if (spec.symbol.empty()) {
            continue;
        }
        by_symbol_[normalize_key(spec.symbol)] = spec;
    }
    return !by_symbol_.empty();
}

std::optional<ContractSpec> OptionRules::lookup(const std::string& product) const {
    if (product.empty()) {
        return std::nullopt;
    }
    const auto it = by_symbol_.find(normalize_key(product));
    if (it == by_symbol_.end()) {
        return std::nullopt;
    }
    return it->second;
}

}  // namespace quant_sev::bll
