#pragma once

#include <optional>
#include <string>
#include <vector>

#include "Account/AccountRecord.hpp"
#include "Config/Config.hpp"

namespace quant_sev::core {

class AccountService {
public:
    explicit AccountService(Config& config);

    std::vector<AccountRecord> list_accounts() const;
    std::optional<AccountRecord> find_by_user_id(const std::string& user_id) const;
    std::optional<AccountRecord> resolve(const nlohmann::json& payload) const;
    nlohmann::json saved_accounts_public() const;

private:
    Config& config_;
};

}  // namespace quant_sev::core
