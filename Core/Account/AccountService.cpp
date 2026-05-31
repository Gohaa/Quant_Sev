#include "Account/AccountService.hpp"

namespace quant_sev::core {

AccountService::AccountService(Config& config) : config_(config) {}

std::vector<AccountRecord> AccountService::list_accounts() const {
    std::vector<AccountRecord> result;
    const auto doc = config_.read_json_file("Account.json");
    if (!doc.contains("accounts") || !doc["accounts"].is_array()) {
        return result;
    }
    for (const auto& item : doc["accounts"]) {
        result.push_back(AccountRecord::from_json(item));
    }
    return result;
}

std::optional<AccountRecord> AccountService::find_by_user_id(const std::string& user_id) const {
    if (user_id.empty()) {
        return std::nullopt;
    }
    std::optional<AccountRecord> match;
    for (const auto& account : list_accounts()) {
        if (account.user_id != user_id) {
            continue;
        }
        if (!match) {
            match = account;
            continue;
        }
        if (account.enabled && !match->enabled) {
            match = account;
            continue;
        }
        if (account.enabled == match->enabled) {
            match = account;
        }
    }
    return match;
}

nlohmann::json AccountService::saved_accounts_public() const {
    nlohmann::json out = nlohmann::json::object();
    out["accounts"] = nlohmann::json::array();
    for (const auto& account : list_accounts()) {
        out["accounts"].push_back(account.to_json(false));
    }
    return out;
}

}  // namespace quant_sev::core
