#pragma once

#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "Account/AccountService.hpp"
#include "Common/ConnectResult.hpp"
#include "Config/Config.hpp"
#include "Quote/QuoteEngine.hpp"

namespace quant_sev::core {

class SymbolService {
public:
    SymbolService(Config& config, AccountService& accounts, QuoteEngine& quote);

    nlohmann::json list_symbols() const;
    nlohmann::json subscribed_symbols() const;
    ConnectResult load_symbols(const nlohmann::json& payload);
    ConnectResult unsubscribe_symbols(const nlohmann::json& payload);

private:
    std::vector<std::string> resolve_instruments(const nlohmann::json& payload) const;
    std::optional<std::string> resolve_md_front(const nlohmann::json& payload) const;

    Config& config_;
    AccountService& accounts_;
    QuoteEngine& quote_;
    mutable std::mutex mutex_;
    std::set<std::string> subscribed_;
};

}  // namespace quant_sev::core
