#include "Config/Config.hpp"

#include "Logger/Logger.hpp"

#include <fstream>
#include <sstream>

namespace quant_sev::core {

namespace fs = std::filesystem;

Config::Config(fs::path root_dir) : root_dir_(std::move(root_dir)) {}

bool Config::load() {
    const auto app_path = config_path("app.json");
    if (!fs::exists(app_path)) {
        Logger::instance().warn("config/app.json 不存在，使用默认 Host 配置");
        return true;
    }

    try {
        std::ifstream in(app_path);
        nlohmann::json j;
        in >> j;

        if (j.contains("host")) {
            const auto& host = j["host"];
            if (host.contains("http_port")) {
                app_.host.http_port = host["http_port"].get<int>();
            }
            if (host.contains("ws_port")) {
                app_.host.ws_port = host["ws_port"].get<int>();
            }
            if (host.contains("web_root")) {
                app_.host.web_root = host["web_root"].get<std::string>();
            }
            if (host.contains("config_dir")) {
                app_.host.config_dir = host["config_dir"].get<std::string>();
            }
        }
        if (j.contains("gateway") && j["gateway"].contains("version")) {
            app_.gateway_version = j["gateway"]["version"].get<std::string>();
        }

        std::ostringstream oss;
        oss << "Config 已加载，HTTP " << app_.host.http_port << " WS " << app_.host.ws_port;
        Logger::instance().info(oss.str());
        return true;
    } catch (const std::exception& ex) {
        Logger::instance().error(std::string("解析 app.json 失败: ") + ex.what());
        return false;
    }
}

fs::path Config::config_path(const std::string& filename) const {
    return root_dir_ / app_.host.config_dir / filename;
}

fs::path Config::web_root() const {
    return root_dir_ / app_.host.web_root;
}

nlohmann::json Config::read_json_file(const std::string& filename) const {
    const auto path = config_path(filename);
    if (!fs::exists(path)) {
        return nlohmann::json::object();
    }
    std::ifstream in(path);
    nlohmann::json j;
    in >> j;
    return j;
}

bool Config::write_json_file(const std::string& filename, const nlohmann::json& data) {
    const auto path = config_path(filename);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path);
    if (!out) {
        return false;
    }
    out << data.dump(2);
    return true;
}

}  // namespace quant_sev::core
