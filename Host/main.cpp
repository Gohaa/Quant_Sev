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

fs::path detect_root_dir(int argc, char** argv) {
#ifdef QUANT_SEV_ROOT
    const fs::path compile_root(QUANT_SEV_ROOT);
    if (fs::exists(compile_root / "config" / "app.json")) {
        return fs::absolute(compile_root);
    }
#endif
    if (argc > 1) {
        const fs::path arg_root(argv[1]);
        if (fs::exists(arg_root / "config" / "app.json")) {
            return fs::absolute(arg_root);
        }
    }
    const fs::path cwd = fs::current_path();
    if (fs::exists(cwd / "config" / "app.json")) {
        return cwd;
    }
    return cwd;
}

std::string json_response(const quant_sev::core::ApiResponse& response) {
    return response.body.dump();
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
    Logger::instance().info("Quant_Sev Host 启动，根目录: " + root_dir.string());

    Config config(root_dir);
    if (!config.load()) {
        Logger::instance().error("Config 加载失败");
        return 1;
    }

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
    server.Get(R"(/api/.*)", [&](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json query = nlohmann::json::object();
        for (const auto& [key, value] : req.params) {
            query[key] = value;
        }
        const auto api = gateway.handle("GET", req.path, query.dump());
        res.status = api.status;
        res.set_content(json_response(api), "application/json; charset=utf-8");
    });

    server.Post(R"(/api/.*)", [&](const httplib::Request& req, httplib::Response& res) {
        const auto api = gateway.handle("POST", req.path, req.body);
        res.status = api.status;
        res.set_content(json_response(api), "application/json; charset=utf-8");
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
