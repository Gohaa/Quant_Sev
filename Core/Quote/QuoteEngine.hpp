#pragma once

#include <functional>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "Account/AccountRecord.hpp"
#include "Common/ConnectResult.hpp"

namespace quant_sev::core {

class QuoteEngine {
public:
    using TickCallback = std::function<void(const nlohmann::json&)>;

    QuoteEngine();
    ~QuoteEngine();

    QuoteEngine(const QuoteEngine&) = delete;
    QuoteEngine& operator=(const QuoteEngine&) = delete;

    ConnectResult connect(const AccountRecord& account);
    ConnectResult disconnect(const std::string& md_front);
    bool is_ready() const;
    bool is_front_ready(const std::string& md_front) const;
    bool is_user_md_ready(const std::string& user_id) const;
    nlohmann::json md_sessions_status() const;
    nlohmann::json quote_board() const;
    void set_tick_callback(TickCallback callback);
    void set_disconnect_callback(std::function<void(const std::string& md_front, int reason)> callback);
    void set_project_root(const std::filesystem::path& root);

    ConnectResult subscribe_instruments(const std::string& md_front,
                                        const std::vector<std::string>& instruments);
    ConnectResult unsubscribe_instruments(const std::string& md_front,
                                          const std::vector<std::string>& instruments);
    std::vector<std::string> subscribed_instruments(const std::string& md_front) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace quant_sev::core
