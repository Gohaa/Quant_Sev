#include "Symbol/SymbolService.hpp"

#include "Logger/Logger.hpp"

#include <optional>

namespace quant_sev::core {

SymbolService::SymbolService(Config& config, AccountService& accounts, QuoteEngine& quote)
    : config_(config), accounts_(accounts), quote_(quote) {}

nlohmann::json SymbolService::list_symbols() const {
    return config_.read_json_file("Symbol_list.json");
}

nlohmann::json SymbolService::subscribed_symbols() const {
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json out = nlohmann::json::object();
    out["instruments"] = nlohmann::json::array();
    for (const auto& id : subscribed_) {
        out["instruments"].push_back(id);
    }
    return out;
}

std::optional<std::string> SymbolService::resolve_md_front(const nlohmann::json& payload) const {
    if (payload.contains("md_front") && payload["md_front"].is_string()) {
        const auto front = payload["md_front"].get<std::string>();
        if (!front.empty()) {
            return front;
        }
    }
    const auto account = accounts_.resolve(payload);
    if (!account) {
        return std::nullopt;
    }
    return account->md_front;
}

std::vector<std::string> SymbolService::resolve_instruments(const nlohmann::json& payload) const {
    if (payload.value("subscribe_all", false)) {
        std::vector<std::string> all;
        const auto doc = list_symbols();
        if (doc.contains("symbols") && doc["symbols"].is_array()) {
            for (const auto& item : doc["symbols"]) {
                const auto id = item.value("instrument_id", "");
                if (!id.empty()) {
                    all.push_back(id);
                }
            }
        }
        return all;
    }

    std::vector<std::string> ids;
    if (payload.contains("instrument_ids") && payload["instrument_ids"].is_array()) {
        for (const auto& item : payload["instrument_ids"]) {
            if (item.is_string()) {
                const auto id = item.get<std::string>();
                if (!id.empty()) {
                    ids.push_back(id);
                }
            }
        }
    }
    if (ids.empty()) {
        const auto single = payload.value("instrument_id", "");
        if (!single.empty()) {
            ids.push_back(single);
        }
    }
    return ids;
}

ConnectResult SymbolService::load_symbols(const nlohmann::json& payload) {
    const auto md_front = resolve_md_front(payload);
    if (!md_front) {
        return {false, "需要 user_id 或 md_front，且账户存在"};
    }

    const auto instruments = resolve_instruments(payload);
    if (instruments.empty()) {
        return {false, "instrument_id / instrument_ids / subscribe_all 未指定有效合约"};
    }

    const std::string user_id = payload.value("user_id", "");
    const auto account = accounts_.resolve(payload);
    if (account && !quote_.is_front_ready(*md_front)) {
        const auto connected = quote_.connect(*account);
        if (!connected.ok) {
            return connected;
        }
    } else if (!account && !user_id.empty()) {
        return {false, "未找到账户"};
    } else if (!quote_.is_front_ready(*md_front)) {
        return {false, "行情未连接，请先 POST /api/load/md 或提供有效账户"};
    }

    const auto result = quote_.subscribe_instruments(*md_front, instruments);
    if (result.ok) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& id : instruments) {
            subscribed_.insert(id);
        }
        Logger::instance().info("Symbol 订阅 " + std::to_string(instruments.size()) + " 个合约 @ " + *md_front);
    }
    return result;
}

ConnectResult SymbolService::unsubscribe_symbols(const nlohmann::json& payload) {
    const auto md_front = resolve_md_front(payload);
    if (!md_front) {
        return {false, "需要 user_id 或 md_front"};
    }
    const auto instruments = resolve_instruments(payload);
    if (instruments.empty()) {
        return {false, "未指定退订合约"};
    }

    const auto result = quote_.unsubscribe_instruments(*md_front, instruments);
    if (result.ok) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& id : instruments) {
            subscribed_.erase(id);
        }
    }
    return result;
}

}  // namespace quant_sev::core
