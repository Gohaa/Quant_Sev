#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace quant_sev::host::ws {

std::string base64_encode(const unsigned char* data, std::size_t len);
std::string sha1_base64(const std::string& input);
std::string make_accept_key(const std::string& client_key);
std::vector<unsigned char> encode_text_frame(const std::string& payload);

}  // namespace quant_sev::host::ws
