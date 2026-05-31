#pragma once

#include <optional>
#include <string>
#include <vector>

namespace quant_sev::bll {

struct ParsedInstrument {
    std::string instrument_id;
    std::string exchange;
    std::string product;
    std::string year_suffix;
    std::string delivery_month;
    std::string month_slot;
};

class ContractRules {
public:
    bool load(const std::string& rules_json_path);

    std::optional<ParsedInstrument> parse(const std::string& instrument_id) const;

private:
    struct ExchangeRule {
        std::string exchange;
        bool upper_case{false};
        int year_digits{2};
        int month_digits{2};
        std::vector<std::string> symbols;
    };

    std::vector<ExchangeRule> exchanges_;
};

}  // namespace quant_sev::bll
