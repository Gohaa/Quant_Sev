#include "OrderBook/OrderBookEngine.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>

namespace quant_sev::bll {

namespace {

int json_int(const nlohmann::json& doc, const char* key) {
    if (!doc.contains(key)) {
        return 0;
    }
    const auto& value = doc[key];
    if (value.is_number_integer()) {
        return value.get<int>();
    }
    if (value.is_number()) {
        return static_cast<int>(value.get<double>());
    }
    return 0;
}

double json_double(const nlohmann::json& doc, const char* key) {
    if (!doc.contains(key)) {
        return 0;
    }
    const auto& value = doc[key];
    if (value.is_number()) {
        return value.get<double>();
    }
    return 0;
}

void read_side_level(const nlohmann::json& tick, const char* price_key, const char* volume_key,
                     OrderBookLevel& level) {
    level.price = json_double(tick, price_key);
    level.volume = json_int(tick, volume_key);
}

}  // namespace

OrderBookSnapshot OrderBookEngine::build_from_tick(const nlohmann::json& tick) {
    OrderBookSnapshot book;
    book.instrument_id = tick.value("instrument_id", "");
    book.last_price = json_double(tick, "last_price");
    book.update_time = tick.value("update_time", "");
    book.update_millisec = json_int(tick, "update_millisec");

    static constexpr std::array<const char*, 5> kBidPrice = {
        "bid_price1", "bid_price2", "bid_price3", "bid_price4", "bid_price5"};
    static constexpr std::array<const char*, 5> kBidVolume = {
        "bid_volume1", "bid_volume2", "bid_volume3", "bid_volume4", "bid_volume5"};
    static constexpr std::array<const char*, 5> kAskPrice = {
        "ask_price1", "ask_price2", "ask_price3", "ask_price4", "ask_price5"};
    static constexpr std::array<const char*, 5> kAskVolume = {
        "ask_volume1", "ask_volume2", "ask_volume3", "ask_volume4", "ask_volume5"};

    for (std::size_t i = 0; i < 5; ++i) {
        read_side_level(tick, kBidPrice[i], kBidVolume[i], book.bids[i]);
        read_side_level(tick, kAskPrice[i], kAskVolume[i], book.asks[i]);
    }

    const double bid1 = book.bids[0].price;
    const double ask1 = book.asks[0].price;
    if (bid1 > 0 && ask1 > 0) {
        book.spread = ask1 - bid1;
        book.mid_price = (bid1 + ask1) * 0.5;
    } else if (book.last_price > 0) {
        book.mid_price = book.last_price;
    }

    for (const auto& level : book.bids) {
        book.bid_depth += level.volume;
    }
    for (const auto& level : book.asks) {
        book.ask_depth += level.volume;
    }

    const int total_depth = book.bid_depth + book.ask_depth;
    if (total_depth > 0) {
        book.imbalance = static_cast<double>(book.bid_depth - book.ask_depth) / static_cast<double>(total_depth);
    }

    return book;
}

nlohmann::json OrderBookEngine::snapshot_to_json(const OrderBookSnapshot& book, int depth) {
    depth = std::clamp(depth, 1, 5);
    nlohmann::json bids = nlohmann::json::array();
    nlohmann::json asks = nlohmann::json::array();
    for (int i = 0; i < depth; ++i) {
        bids.push_back({{"price", book.bids[static_cast<std::size_t>(i)].price},
                        {"volume", book.bids[static_cast<std::size_t>(i)].volume}});
        asks.push_back({{"price", book.asks[static_cast<std::size_t>(i)].price},
                        {"volume", book.asks[static_cast<std::size_t>(i)].volume}});
    }

    return {{"instrument_id", book.instrument_id},
            {"last_price", book.last_price},
            {"spread", book.spread},
            {"mid_price", book.mid_price},
            {"bid_depth", book.bid_depth},
            {"ask_depth", book.ask_depth},
            {"imbalance", book.imbalance},
            {"update_time", book.update_time},
            {"update_millisec", book.update_millisec},
            {"bids", bids},
            {"asks", asks}};
}

void OrderBookEngine::on_tick(const nlohmann::json& tick) {
    const std::string instrument_id = tick.value("instrument_id", "");
    if (instrument_id.empty()) {
        return;
    }
    const auto book = build_from_tick(tick);
    std::lock_guard<std::mutex> lock(mutex_);
    books_[instrument_id] = book;
}

nlohmann::json OrderBookEngine::snapshot(const std::string& instrument_id, int depth) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = books_.find(instrument_id);
    if (it == books_.end()) {
        const auto same_id = [&](const std::pair<const std::string, OrderBookSnapshot>& row) {
            if (row.first.size() != instrument_id.size()) {
                return false;
            }
            for (std::size_t i = 0; i < instrument_id.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(row.first[i])) !=
                    std::tolower(static_cast<unsigned char>(instrument_id[i]))) {
                    return false;
                }
            }
            return true;
        };
        it = std::find_if(books_.begin(), books_.end(), same_id);
    }
    if (it == books_.end()) {
        return {{"error", "instrument not found"}, {"instrument_id", instrument_id}};
    }
    return snapshot_to_json(it->second, depth);
}

nlohmann::json OrderBookEngine::board(int depth) const {
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json rows = nlohmann::json::array();
    for (const auto& [instrument_id, book] : books_) {
        (void)instrument_id;
        rows.push_back(snapshot_to_json(book, depth));
    }
    return {{"count", rows.size()}, {"books", rows}};
}

nlohmann::json OrderBookEngine::list_instruments() const {
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json items = nlohmann::json::array();
    for (const auto& [instrument_id, _] : books_) {
        items.push_back(instrument_id);
    }
    return {{"instruments", items}, {"count", items.size()}};
}

}  // namespace quant_sev::bll
