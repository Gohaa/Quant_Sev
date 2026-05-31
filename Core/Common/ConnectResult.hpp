#pragma once

#include <string>

namespace quant_sev::core {

struct ConnectResult {
    bool ok{false};
    std::string message;
};

}  // namespace quant_sev::core
