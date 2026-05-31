#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "Storage/StorageEngine.hpp"
#include "Storage/StorageTypes.hpp"
#include "Strategy/StrategyRuntime.hpp"

namespace quant_sev::bll {

class StrategyEngine {
public:
    using RunningStrategiesProvider = std::function<nlohmann::json()>;
    using SignalHandler = std::function<void(const nlohmann::json& payload)>;

    explicit StrategyEngine(StorageEngine& storage);

    void set_running_strategies_provider(RunningStrategiesProvider provider);
    void set_signal_handler(SignalHandler handler);
    void on_bar_closed(const std::string& instrument_id, const BarRecord& bar, const std::string& period);

private:
    struct RuntimeSlot {
        std::deque<BarRecord> bars;
        std::unique_ptr<StrategySession> session;
        std::string logic;
        nlohmann::json row;
    };

    void evaluate_strategy(RuntimeSlot& slot, const nlohmann::json& row, const std::string& instrument_id,
                           const BarRecord& bar, const std::string& period);

    StorageEngine& storage_;
    RunningStrategiesProvider running_provider_;
    SignalHandler signal_handler_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, RuntimeSlot> slots_;
};

}  // namespace quant_sev::bll
