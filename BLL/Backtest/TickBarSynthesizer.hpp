#pragma once

#include <functional>
#include <string>
#include <vector>

#include "Storage/StorageTypes.hpp"

namespace quant_sev::bll {

class TickBarSynthesizer {
public:
    using BarClosedCallback = std::function<void(const BarRecord& bar)>;

    void set_bar_closed_callback(BarClosedCallback callback);
    void on_tick(const TickRecord& tick);
    void flush();

private:
    struct ActiveBar {
        std::string bucket;
        BarRecord record;
        long long last_cumulative_volume{0};
        bool has_volume{false};
    };

    static std::string format_trading_date(const std::string& trading_day);
    static std::string make_m1_bucket(const TickRecord& tick);

    BarClosedCallback bar_closed_;
    ActiveBar m1_;
};

}  // namespace quant_sev::bll
