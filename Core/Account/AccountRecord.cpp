#include "Account/AccountRecord.hpp"

namespace quant_sev::core {

AccountRecord AccountRecord::from_json(const nlohmann::json& item) {
    AccountRecord record;
    record.name = item.value("name", "");
    record.user_id = item.value("user_id", "");
    record.broker_id = item.value("broker_id", "");
    record.md_front = item.value("md_front", "");
    record.td_front = item.value("td_front", item.value("trader_front", ""));
    record.password = item.value("password", "");
    record.app_id = item.value("app_id", "");
    record.auth_code = item.value("auth_code", "");
    record.account_type = item.value("account_type", "sim");
    record.enabled = item.value("enabled", true);
    return record;
}

nlohmann::json AccountRecord::to_json(bool include_password) const {
    nlohmann::json item = {{"name", name},
                           {"user_id", user_id},
                           {"broker_id", broker_id},
                           {"md_front", md_front},
                           {"td_front", td_front},
                           {"trader_front", td_front},
                           {"app_id", app_id},
                           {"auth_code", auth_code},
                           {"account_type", account_type},
                           {"enabled", enabled}};
    if (include_password && !password.empty()) {
        item["password"] = password;
    }
    return item;
}

}  // namespace quant_sev::core
