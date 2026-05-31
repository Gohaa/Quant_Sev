#pragma once

#include <string>

namespace quant_sev::bll {

struct BacktestProgress {
    bool running{false};
    std::string phase;
    std::string instrument_id;
    int current{0};
    int total{0};
    int percent{0};
    std::string message;
};

}  // namespace quant_sev::bll
