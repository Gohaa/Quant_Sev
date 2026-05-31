#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace quant_sev::core {

struct HostConfig {
    int http_port{8080};
    int ws_port{8081};
    std::filesystem::path web_root{"Web"};
    std::filesystem::path config_dir{"config"};
};

struct AppConfig {
    HostConfig host;
    std::string gateway_version{"0.1.0"};
};

class Config {
public:
    explicit Config(std::filesystem::path root_dir);

    bool load();
    const AppConfig& app() const { return app_; }
    std::filesystem::path root_dir() const { return root_dir_; }
    std::filesystem::path config_path(const std::string& filename) const;
    std::filesystem::path web_root() const;
    nlohmann::json read_json_file(const std::string& filename) const;
    bool write_json_file(const std::string& filename, const nlohmann::json& data);

private:
    std::filesystem::path root_dir_;
    AppConfig app_;
};

}  // namespace quant_sev::core
