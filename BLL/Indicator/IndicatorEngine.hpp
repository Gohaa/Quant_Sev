#pragma once

#include <nlohmann/json.hpp>

#include "Indicator/IndicatorTypes.hpp"
#include "Storage/StorageEngine.hpp"

namespace quant_sev::bll {

class IndicatorEngine {
public:
    explicit IndicatorEngine(StorageEngine& storage);

    nlohmann::json catalog() const;
    nlohmann::json compute(const IndicatorQuery& query) const;

private:
    static std::string normalize_name(const std::string& name);
    static std::string type_label(int type);

    StorageEngine& storage_;
};

}  // namespace quant_sev::bll
