#pragma once

#include <functional>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "Storage/StorageEngine.hpp"

namespace quant_sev::bll {

class BarEngine {
public:
    explicit BarEngine(StorageEngine& storage);

    void on_tick(const nlohmann::json& tick);
    using BarClosedCallback =
        std::function<void(const std::string& instrument_id, const BarRecord& bar, const std::string& period)>;
    void set_bar_closed_callback(BarClosedCallback callback);

private:
    struct ActiveBar {
        std::string bucket;
        BarRecord record;
        long long last_cumulative_volume{0};
        bool has_volume{false};
    };

    struct InstrumentState {
        ActiveBar m1;
        ActiveBar m15;
        ActiveBar h1;
        ActiveBar d1;
    };

    std::string make_m1_bucket(const nlohmann::json& tick) const;
    std::string format_trading_date(const std::string& trading_day) const;
    void on_m1_closed(const std::string& instrument_id, const ParsedInstrument& parsed, const BarRecord& bar);
    void flush_m15(const std::string& instrument_id, const ParsedInstrument& parsed, InstrumentState& state);
    void flush_h1(const std::string& instrument_id, const ParsedInstrument& parsed, InstrumentState& state);
    void flush_d1(const std::string& instrument_id, const ParsedInstrument& parsed, InstrumentState& state);
    void emit_bar_closed(const std::string& instrument_id, const BarRecord& bar, const std::string& period);

    StorageEngine& storage_;
    BarClosedCallback bar_closed_;
    std::unordered_map<std::string, InstrumentState> active_;
};

}  // namespace quant_sev::bll
