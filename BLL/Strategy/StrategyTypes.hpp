#pragma once

#include <optional>
#include <string>

namespace quant_sev::bll {

struct StrategyBarSignal {
    std::string action;
    std::string label;
    double price{0};
    int bar_index{0};
};

}  // namespace quant_sev::bll
