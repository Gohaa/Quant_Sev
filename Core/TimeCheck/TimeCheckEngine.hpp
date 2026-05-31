#pragma once

#include "Common/ConnectResult.hpp"
#include "Config/Config.hpp"
#include "TimeCheck/TimeCheckTypes.hpp"

namespace quant_sev::core {

class TimeCheckEngine {
public:
    bool load(Config& config);
    ConnectResult check(bool manual_order = false) const;
    TimeCheckStatus status() const;

private:
    TimeCheckConfig config_;
};

}  // namespace quant_sev::core
