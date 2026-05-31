#include "Strategy/StrategyEngine.hpp"

#include "Logger/Logger.hpp"

namespace quant_sev::bll {

StrategyEngine::StrategyEngine(StorageEngine& storage) : storage_(storage) {}

void StrategyEngine::set_running_strategies_provider(RunningStrategiesProvider provider) {
    running_provider_ = std::move(provider);
}

void StrategyEngine::set_signal_handler(SignalHandler handler) {
    signal_handler_ = std::move(handler);
}

void StrategyEngine::evaluate_strategy(RuntimeSlot& slot, const nlohmann::json& row,
                                       const std::string& instrument_id, const BarRecord& bar,
                                       const std::string& period) {
    if (slot.bars.size() < 2 || !signal_handler_) {
        return;
    }

    const std::string logic = row.value("logic", "dual_thrust");
    if (!StrategyRegistry::instance().has(logic)) {
        quant_sev::core::Logger::instance().warn("未知策略 logic: " + logic);
        return;
    }

    if (!slot.session || slot.logic != logic) {
        if (slot.session) {
            slot.session->deinit(StrategyDeinitReason::Reload);
        }
        slot.logic = logic;
        slot.row = row;
        slot.session = std::make_unique<StrategySession>(logic);
        if (!slot.session->valid()) {
            quant_sev::core::Logger::instance().warn("策略载入失败: " + logic);
            slot.session.reset();
            return;
        }
        const std::vector<BarRecord> history(slot.bars.begin(), slot.bars.end());
        slot.session->reset(row, history, period);
    }

    const std::vector<BarRecord> history(slot.bars.begin(), slot.bars.end());
    const int index = static_cast<int>(history.size()) - 1;
    const auto signal = slot.session->on_bar(index, history, period, nullptr);
    if (!signal) {
        (void)bar;
        return;
    }

    nlohmann::json payload = {{"strategy_id", row.value("id", "")},
                              {"user_id", row.value("user_id", "")},
                              {"instrument_id", instrument_id},
                              {"action", signal->action},
                              {"label", signal->label},
                              {"price", signal->price},
                              {"volume", row.value("volume", row.value("default_volume", 1))},
                              {"bar_index", signal->bar_index}};
    signal_handler_(payload);
    quant_sev::core::Logger::instance().info("Strategy on_bar 信号: " + signal->label);
    (void)bar;
}

void StrategyEngine::on_bar_closed(const std::string& instrument_id, const BarRecord& bar,
                                   const std::string& period) {
    if (!running_provider_) {
        return;
    }
    const auto doc = running_provider_();
    if (!doc.contains("strategies") || !doc["strategies"].is_array()) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& row : doc["strategies"]) {
        if (row.value("state", "") != "running") {
            continue;
        }
        const std::string strategy_period = row.value("period", "m1");
        if (strategy_period != period) {
            continue;
        }
        const std::string strategy_id = row.value("id", "");
        if (strategy_id.empty()) {
            continue;
        }

        std::vector<nlohmann::json> leg_rows;
        if (row.contains("legs") && row["legs"].is_array() && !row["legs"].empty()) {
            for (const auto& leg : row["legs"]) {
                if (leg.value("instrument_id", "") != instrument_id) {
                    continue;
                }
                nlohmann::json leg_row = row;
                leg_row["instrument_id"] = instrument_id;
                leg_row["volume"] = leg.value("volume", row.value("volume", row.value("default_volume", 1)));
                if (leg.contains("params") && leg["params"].is_object()) {
                    for (auto it = leg["params"].begin(); it != leg["params"].end(); ++it) {
                        leg_row[it.key()] = it.value();
                    }
                }
                leg_rows.push_back(std::move(leg_row));
            }
        } else if (row.value("instrument_id", "") == instrument_id) {
            leg_rows.push_back(row);
        }
        if (leg_rows.empty()) {
            continue;
        }

        for (const auto& leg_row : leg_rows) {
            auto& slot = slots_[strategy_id + "|" + instrument_id];
            if (slot.bars.empty()) {
                BarQuery query;
                query.instrument_id = instrument_id;
                query.period = period;
                query.limit = 2000;
                const auto history = storage_.query_bars(query);
                for (const auto& item : history) {
                    slot.bars.push_back(item);
                }
            }
            slot.bars.push_back(bar);
            while (slot.bars.size() > 5000) {
                slot.bars.pop_front();
            }

            evaluate_strategy(slot, leg_row, instrument_id, bar, period);
        }
    }
}

}  // namespace quant_sev::bll
