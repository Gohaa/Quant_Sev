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

inline std::string sanitize_utf8(std::string text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
            ++i;
            continue;
        }
        auto append_multibyte = [&](std::size_t len) {
            if (i + len <= text.size()) {
                out.append(text, i, len);
                i += len;
                return true;
            }
            return false;
        };
        if ((c & 0xE0) == 0xC0 && append_multibyte(2)) {
            continue;
        }
        if ((c & 0xF0) == 0xE0 && append_multibyte(3)) {
            continue;
        }
        if ((c & 0xF8) == 0xF0 && append_multibyte(4)) {
            continue;
        }
        if (c >= 0x80 && i + 1 < text.size()) {
            ++i;
            ++i;
            out += '?';
            continue;
        }
        out.push_back('?');
        ++i;
    }
    return out;
}

inline const char* ctp_order_error_hint(int error_id) {
    switch (error_id) {
        case 50:
            return "平今仓位不足";
        case 51:
            return "平昨仓位不足";
        case 148:
            return "无效的 ExchangeID";
        default:
            return nullptr;
    }
}

}  // namespace quant_sev::core
