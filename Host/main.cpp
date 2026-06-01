#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#include <httplib.h>

#include "Config/Config.hpp"
#include "Gateway/Gateway.hpp"
#include "Logger/Logger.hpp"
#include "WebSocketServer/TickWsServer.hpp"

namespace fs = std::filesystem;

namespace {

std::atomic<bool> g_running{true};

bool has_app_config(const fs::path& root) {
    return fs::exists(root / "config" / "app.json");
}

fs::path detect_root_dir(int argc, char** argv) {
#ifdef QUANT_SEV_ROOT
    const fs::path compile_root(QUANT_SEV_ROOT);
    if (has_app_config(compile_root)) {
        return fs::absolute(compile_root);
    }
#endif
    if (argc > 1) {
        const fs::path arg_root(argv[1]);
        if (has_app_config(arg_root)) {
            return fs::absolute(arg_root);
        }
    }
    if (argc > 0 && argv[0] != nullptr) {
        fs::path probe = fs::absolute(fs::path(argv[0])).parent_path();
        for (int depth = 0; depth < 6 && !probe.empty(); ++depth) {
            if (has_app_config(probe)) {
                return fs::absolute(probe);
            }
            probe = probe.parent_path();
        }
    }
    const fs::path cwd = fs::current_path();
    if (has_app_config(cwd)) {
        return cwd;
    }
    return cwd;
}

std::string json_response(const quant_sev::core::ApiResponse& response) {
    return response.body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

void write_api_response(httplib::Response& res, quant_sev::core::Gateway& gateway,
                        const std::string& method, const std::string& path, const std::string& body) {
    try {
        const auto api = gateway.handle(method, path, body);
        res.status = api.status;
        res.set_content(json_response(api), "application/json; charset=utf-8");
    } catch (const std::exception& ex) {
        quant_sev::core::Logger::instance().error(std::string("HTTP 处理异常: ") + ex.what());
        res.status = 500;
        res.set_content(R"({"ok":false,"error":"服务器内部错误"})", "application/json; charset=utf-8");
    } catch (...) {
        quant_sev::core::Logger::instance().error("HTTP 处理未知异常");
        res.status = 500;
        res.set_content(R"({"ok":false,"error":"服务器内部错误"})", "application/json; charset=utf-8");
    }
}

void handle_signal(int) { g_running = false; }

}  // namespace

int main(int argc, char** argv) {
    using quant_sev::core::Config;
    using quant_sev::core::Gateway;
    using quant_sev::core::Logger;
    using quant_sev::host::TickWsServer;

    std::signal(SIGINT, handle_signal);
#ifndef _WIN32
    std::signal(SIGTERM, handle_signal);
#endif

    const fs::path root_dir = detect_root_dir(argc, argv);
    if (!has_app_config(root_dir)) {
        std::cerr << "未找到 config/app.json。请在本机项目根目录运行，或执行:\n"
                  << "  quant_sev_host.exe <项目根目录>\n"
                  << "当前工作目录: " << fs::current_path().string() << std::endl;
        return 1;
    }
    Logger::instance().info("Quant_Sev Host 启动，根目录: " + root_dir.string());

    Config config(root_dir);
    if (!config.load()) {
        Logger::instance().error("Config 加载失败");
        return 1;
    }

    Logger::instance().set_persist_dir(config.config_path("logs"));
    Logger::instance().info("日志持久化目录: " + config.config_path("logs").string());

    Gateway gateway(config);
    TickWsServer tick_ws(config.app().host.ws_port);
    gateway.set_tick_listener([&tick_ws](const nlohmann::json& tick) { tick_ws.broadcast_tick(tick); });
    gateway.set_order_listener([&tick_ws](const nlohmann::json& order) { tick_ws.broadcast_event("order", order); });
    gateway.set_trade_listener([&tick_ws](const nlohmann::json& trade) { tick_ws.broadcast_event("trade", trade); });
    tick_ws.start();

    if (!gateway.initialize()) {
        Logger::instance().error("Gateway 初始化失败");
        tick_ws.stop();
        return 1;
    }

    const auto web_root = config.web_root();
    if (!fs::exists(web_root)) {
        Logger::instance().error("Web 根目录不存在: " + web_root.string());
        tick_ws.stop();
        return 1;
    }

    httplib::Server server;
    server.set_read_timeout(120, 0);
    server.set_write_timeout(120, 0);
    server.Get(R"(/api/.*)", [&](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json query = nlohmann::json::object();
        for (const auto& [key, value] : req.params) {
            query[key] = value;
        }
        write_api_response(res, gateway, "GET", req.path, query.dump());
    });

    server.Post(R"(/api/.*)", [&](const httplib::Request& req, httplib::Response& res) {
        write_api_response(res, gateway, "POST", req.path, req.body);
    });

    server.set_mount_point("/", web_root.string());

    const int port = config.app().host.http_port;
    Logger::instance().info("HTTP 服务监听 http://127.0.0.1:" + std::to_string(port));
    Logger::instance().info("静态资源: " + web_root.string());

    std::thread http_thread([&]() {
        if (!server.listen("127.0.0.1", port)) {
            Logger::instance().error("HTTP 监听失败，端口可能被占用: " + std::to_string(port));
            g_running = false;
        }
    });

    while (g_running) {
        gateway.poll_reconnect();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    server.stop();
    if (http_thread.joinable()) {
        http_thread.join();
    }
    tick_ws.stop();
    Logger::instance().info("Quant_Sev Host 已退出");
    return 0;
}
