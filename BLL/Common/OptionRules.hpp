#pragma once

#include <optional>
#include <string>
#include <unordered_map>

namespace quant_sev::bll {

struct ContractSpec {
    std::string symbol;
    std::string name;
    std::string exchange;
    double multiplier{10};
    double tick{1};
};

class OptionRules {
public:
    bool load(const std::string& rules_json_path);

    std::optional<ContractSpec> lookup(const std::string& product) const;

private:
    std::unordered_map<std::string, ContractSpec> by_symbol_;
};

}  // namespace quant_sev::bll
