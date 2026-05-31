#pragma once

#include <array>
#include <mutex>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace quant_sev::bll {

struct OrderBookLevel {
    double price{0};
    int volume{0};
};

struct OrderBookSnapshot {
    std::string instrument_id;
    std::array<OrderBookLevel, 5> bids{};
    std::array<OrderBookLevel, 5> asks{};
    double last_price{0};
    double spread{0};
    double mid_price{0};
    int bid_depth{0};
    int ask_depth{0};
    double imbalance{0};
    std::string update_time;
    int update_millisec{0};
};

class OrderBookEngine {
public:
    void on_tick(const nlohmann::json& tick);
    nlohmann::json snapshot(const std::string& instrument_id, int depth = 5) const;
    nlohmann::json board(int depth = 5) const;
    nlohmann::json list_instruments() const;

private:
    static OrderBookSnapshot build_from_tick(const nlohmann::json& tick);
    static nlohmann::json snapshot_to_json(const OrderBookSnapshot& book, int depth);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, OrderBookSnapshot> books_;
};

}  // namespace quant_sev::bll
