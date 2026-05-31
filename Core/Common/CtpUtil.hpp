#pragma once

#include <cstddef>
#include <cstring>
#include <string>

namespace quant_sev::core {

template <std::size_t N>
inline void copy_to_field(char (&dest)[N], const std::string& src) {
    std::memset(dest, 0, N);
    if (!src.empty()) {
        std::strncpy(dest, src.c_str(), N - 1);
    }
}

inline std::string trim_cstr(const char* value) {
    if (value == nullptr) {
        return {};
    }
    return std::string(value);
}

}  // namespace quant_sev::core
