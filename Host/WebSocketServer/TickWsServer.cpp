#include "WebSocketServer/TickWsServer.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>

#include "Logger/Logger.hpp"
#include "WebSocketServer/WsUtil.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
inline int close_socket(socket_t fd) { return closesocket(fd); }
inline int last_socket_error() { return WSAGetLastError(); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
inline int close_socket(socket_t fd) { return close(fd); }
inline int last_socket_error() { return errno; }
#endif

namespace quant_sev::host {

namespace {

bool socket_send_all(socket_t fd, const unsigned char* data, std::size_t len) {
    std::size_t sent = 0;
    while (sent < len) {
#ifdef _WIN32
        const int rc = send(fd, reinterpret_cast<const char*>(data + sent),
                            static_cast<int>(len - sent), 0);
#else
        const ssize_t rc = send(fd, data + sent, len - sent, 0);
#endif
        if (rc <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(rc);
    }
    return true;
}

std::string recv_request(socket_t fd) {
    std::string request;
    char buffer[1024];
    while (request.find("\r\n\r\n") == std::string::npos) {
        const int rc = recv(fd, buffer, sizeof(buffer), 0);
        if (rc <= 0) {
            break;
        }
        request.append(buffer, buffer + rc);
        if (request.size() > 8192) {
            break;
        }
    }
    return request;
}

std::string header_value(const std::string& request, const std::string& key) {
    const std::string needle = key + ":";
    const auto pos = request.find(needle);
    if (pos == std::string::npos) {
        return {};
    }
    auto start = pos + needle.size();
    while (start < request.size() && (request[start] == ' ' || request[start] == '\t')) {
        ++start;
    }
    const auto end = request.find("\r\n", start);
    if (end == std::string::npos) {
        return {};
    }
    return request.substr(start, end - start);
}

}  // namespace

TickWsServer::TickWsServer(int port) : port_(port) {}

TickWsServer::~TickWsServer() { stop(); }

void TickWsServer::start() {
    if (running_.exchange(true)) {
        return;
    }
    thread_ = std::thread([this]() { run_loop(); });
}

void TickWsServer::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (listen_fd_ != kInvalidSocket) {
        close_socket(listen_fd_);
        listen_fd_ = kInvalidSocket;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (const auto fd : clients_) {
        close_socket(fd);
    }
    clients_.clear();
}

void TickWsServer::broadcast_event(const std::string& type, const nlohmann::json& data) {
    nlohmann::json envelope = {{"type", type}, {"data", data}};
    const std::string payload = envelope.dump();
    const auto frame = ws::encode_text_frame(payload);

    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (auto it = clients_.begin(); it != clients_.end();) {
        if (!socket_send_all(*it, frame.data(), frame.size())) {
            close_socket(*it);
            it = clients_.erase(it);
        } else {
            ++it;
        }
    }
}

void TickWsServer::broadcast_tick(const nlohmann::json& tick) {
    broadcast_event("tick", tick);
}

bool TickWsServer::perform_handshake(int client_fd, const std::string& request) {
    if (request.find("GET /ws/tick") == std::string::npos) {
        const char* response = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
        send(client_fd, response, static_cast<int>(strlen(response)), 0);
        return false;
    }
    const auto key = header_value(request, "Sec-WebSocket-Key");
    if (key.empty()) {
        return false;
    }
    const auto accept = ws::make_accept_key(key);
    std::ostringstream oss;
    oss << "HTTP/1.1 101 Switching Protocols\r\n"
        << "Upgrade: websocket\r\n"
        << "Connection: Upgrade\r\n"
        << "Sec-WebSocket-Accept: " << accept << "\r\n\r\n";
    const auto response = oss.str();
    return send(client_fd, response.c_str(), static_cast<int>(response.size()), 0) > 0;
}

void TickWsServer::run_loop() {
#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        quant_sev::core::Logger::instance().error("WSAStartup 失败");
        running_ = false;
        return;
    }
#endif

    listen_fd_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_fd_ == kInvalidSocket) {
        quant_sev::core::Logger::instance().error("Tick WS socket 创建失败");
        running_ = false;
        return;
    }

    int yes = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<u_short>(port_));
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        quant_sev::core::Logger::instance().error("Tick WS bind 失败，端口 " + std::to_string(port_));
        close_socket(listen_fd_);
        listen_fd_ = kInvalidSocket;
        running_ = false;
        return;
    }
    if (listen(listen_fd_, SOMAXCONN) != 0) {
        quant_sev::core::Logger::instance().error("Tick WS listen 失败");
        close_socket(listen_fd_);
        listen_fd_ = kInvalidSocket;
        running_ = false;
        return;
    }

    quant_sev::core::Logger::instance().info("Tick WebSocket 监听 ws://127.0.0.1:" + std::to_string(port_) +
                                             "/ws/tick");

    while (running_) {
        fd_set readfds;
        FD_ZERO(&readfds);
#ifdef _WIN32
        FD_SET(listen_fd_, &readfds);
        socket_t max_fd = listen_fd_;
#else
        FD_SET(listen_fd_, &readfds);
        int max_fd = listen_fd_;
#endif
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            for (const auto fd : clients_) {
                FD_SET(fd, &readfds);
                if (fd > max_fd) {
                    max_fd = fd;
                }
            }
        }

        timeval timeout{0, 200000};
        const int ready = select(static_cast<int>(max_fd + 1), &readfds, nullptr, nullptr, &timeout);
        if (ready <= 0) {
            continue;
        }

        if (FD_ISSET(listen_fd_, &readfds)) {
            const socket_t client = accept(listen_fd_, nullptr, nullptr);
            if (client != kInvalidSocket) {
                const auto request = recv_request(client);
                if (perform_handshake(client, request)) {
                    std::lock_guard<std::mutex> lock(clients_mutex_);
                    clients_.push_back(client);
                } else {
                    close_socket(client);
                }
            }
        }

        std::vector<int> stale;
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            for (const auto fd : clients_) {
                if (FD_ISSET(fd, &readfds)) {
                    char buf[256];
                    const int rc = recv(fd, buf, sizeof(buf), 0);
                    if (rc <= 0) {
                        stale.push_back(fd);
                    }
                }
            }
        }
        for (const auto fd : stale) {
            remove_client(fd);
        }
    }

    if (listen_fd_ != kInvalidSocket) {
        close_socket(listen_fd_);
        listen_fd_ = kInvalidSocket;
    }
#ifdef _WIN32
    WSACleanup();
#endif
}

void TickWsServer::remove_client(int client_fd) {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    clients_.erase(std::remove(clients_.begin(), clients_.end(), client_fd), clients_.end());
    close_socket(client_fd);
}

}  // namespace quant_sev::host
