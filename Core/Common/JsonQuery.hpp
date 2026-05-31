#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace quant_sev::core {

inline int json_int_param(const nlohmann::json& query, const char* key, int default_value) {
    if (!query.contains(key)) {
        return default_value;
    }
    const auto& value = query.at(key);
    if (value.is_number_integer()) {
        return value.get<int>();
    }
    if (value.is_number_unsigned()) {
        return static_cast<int>(value.get<unsigned>());
    }
    if (value.is_string()) {
        try {
            return std::stoi(value.get<std::string>());
        } catch (...) {
        }
    }
    return default_value;
}

inline bool json_bool_param(const nlohmann::json& query, const char* key, bool default_value) {
    if (!query.contains(key)) {
        return default_value;
    }
    const auto& value = query.at(key);
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_number_integer() || value.is_number_unsigned()) {
        return value.get<int>() != 0;
    }
    if (value.is_string()) {
        const std::string text = value.get<std::string>();
        if (text == "1" || text == "true" || text == "TRUE" || text == "yes" || text == "on") {
            return true;
        }
        if (text == "0" || text == "false" || text == "FALSE" || text == "no" || text == "off") {
            return false;
        }
    }
    return default_value;
}

}  // namespace quant_sev::core
