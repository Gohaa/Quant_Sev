#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace quant_sev::core {

struct AccountRecord {
    std::string name;
    std::string user_id;
    std::string broker_id;
    std::string md_front;
    std::string td_front;
    std::string password;
    std::string app_id;
    std::string auth_code;
    std::string account_type{"sim"};
    bool enabled{true};

    static AccountRecord from_json(const nlohmann::json& item);
    nlohmann::json to_json(bool include_password = false) const;
};

}  // namespace quant_sev::core
