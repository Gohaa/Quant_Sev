#pragma once

#include <string>
#include <vector>

namespace quant_sev::bll {

struct IndicatorQuery {
    std::string instrument_id;
    std::string period{"m1"};
    std::string name;
    std::vector<double> options;
    int limit{500};
};

}  // namespace quant_sev::bll
