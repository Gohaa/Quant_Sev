#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace quant_sev::host {

class TickWsServer {
public:
    explicit TickWsServer(int port = 8081);
    ~TickWsServer();

    TickWsServer(const TickWsServer&) = delete;
    TickWsServer& operator=(const TickWsServer&) = delete;

    void start();
    void stop();
    void broadcast_tick(const nlohmann::json& tick);
    void broadcast_event(const std::string& type, const nlohmann::json& data);

private:
    void run_loop();
    bool perform_handshake(int client_fd, const std::string& request);
    void remove_client(int client_fd);

    int port_;
    int listen_fd_{-1};
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex clients_mutex_;
    std::vector<int> clients_;
};

}  // namespace quant_sev::host
