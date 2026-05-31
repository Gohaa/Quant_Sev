#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "Common/ContractRules.hpp"

namespace quant_sev::bll {

class RolloverEngine {
public:
    bool initialize(const std::filesystem::path& project_root);

    std::optional<std::string> next_contract(const std::string& instrument_id) const;
    nlohmann::json suggest(const nlohmann::json& symbol_list, const nlohmann::json& quote_board) const;
    nlohmann::json apply(const std::filesystem::path& project_root, const nlohmann::json& payload) const;
    nlohmann::json record_daily_snapshot(const std::filesystem::path& project_root, const std::string& trading_day,
                                         const nlohmann::json& symbol_list,
                                         const nlohmann::json& quote_board) const;
    nlohmann::json daily_view(const std::filesystem::path& project_root) const;

private:
    static double quote_metric(const nlohmann::json& quote_row);
    static std::optional<nlohmann::json> find_quote(const nlohmann::json& quote_board,
                                                    const std::string& instrument_id);
    static int count_consecutive_leading_days(const nlohmann::json& store, int symbol_id);

    ContractRules rules_;
    int consecutive_days_{2};
    std::filesystem::path project_root_;
};

}  // namespace quant_sev::bll
