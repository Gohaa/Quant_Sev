#include "WebSocketServer/WsUtil.hpp"

#include <cstring>
#include <sstream>

namespace quant_sev::host::ws {

namespace {

constexpr char kBase64Table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

uint32_t left_rotate(uint32_t value, int bits) {
    return (value << bits) | (value >> (32 - bits));
}

std::array<uint32_t, 5> sha1_hash(const std::string& input) {
    uint32_t h0 = 0x67452301;
    uint32_t h1 = 0xEFCDAB89;
    uint32_t h2 = 0x98BADCFE;
    uint32_t h3 = 0x10325476;
    uint32_t h4 = 0xC3D2E1F0;

    std::vector<unsigned char> msg(input.begin(), input.end());
    const uint64_t original_bit_len = static_cast<uint64_t>(msg.size()) * 8ULL;
    msg.push_back(0x80);
    while ((msg.size() % 64) != 56) {
        msg.push_back(0x00);
    }
    for (int i = 7; i >= 0; --i) {
        msg.push_back(static_cast<unsigned char>((original_bit_len >> (i * 8)) & 0xFF));
    }

    for (std::size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t w[80]{};
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(msg[chunk + i * 4]) << 24) |
                   (static_cast<uint32_t>(msg[chunk + i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(msg[chunk + i * 4 + 2]) << 8) |
                   (static_cast<uint32_t>(msg[chunk + i * 4 + 3]));
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = left_rotate(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        uint32_t a = h0;
        uint32_t b = h1;
        uint32_t c = h2;
        uint32_t d = h3;
        uint32_t e = h4;

        for (int i = 0; i < 80; ++i) {
            uint32_t f = 0;
            uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            const uint32_t temp = left_rotate(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = left_rotate(b, 30);
            b = a;
            a = temp;
        }

        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    return {h0, h1, h2, h3, h4};
}

}  // namespace

std::string base64_encode(const unsigned char* data, std::size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (std::size_t i = 0; i < len; i += 3) {
        const unsigned int n = (static_cast<unsigned int>(data[i]) << 16) |
                               ((i + 1 < len ? data[i + 1] : 0) << 8) |
                               (i + 2 < len ? data[i + 2] : 0);
        out.push_back(kBase64Table[(n >> 18) & 63]);
        out.push_back(kBase64Table[(n >> 12) & 63]);
        out.push_back(i + 1 < len ? kBase64Table[(n >> 6) & 63] : '=');
        out.push_back(i + 2 < len ? kBase64Table[n & 63] : '=');
    }
    return out;
}

std::string sha1_base64(const std::string& input) {
    const auto digest = sha1_hash(input);
    unsigned char bytes[20];
    for (int i = 0; i < 5; ++i) {
        bytes[i * 4] = static_cast<unsigned char>((digest[i] >> 24) & 0xFF);
        bytes[i * 4 + 1] = static_cast<unsigned char>((digest[i] >> 16) & 0xFF);
        bytes[i * 4 + 2] = static_cast<unsigned char>((digest[i] >> 8) & 0xFF);
        bytes[i * 4 + 3] = static_cast<unsigned char>(digest[i] & 0xFF);
    }
    return base64_encode(bytes, 20);
}

std::string make_accept_key(const std::string& client_key) {
    return sha1_base64(client_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
}

std::vector<unsigned char> encode_text_frame(const std::string& payload) {
    std::vector<unsigned char> frame;
    frame.push_back(0x81);
    const auto len = payload.size();
    if (len <= 125) {
        frame.push_back(static_cast<unsigned char>(len));
    } else if (len <= 0xFFFF) {
        frame.push_back(126);
        frame.push_back(static_cast<unsigned char>((len >> 8) & 0xFF));
        frame.push_back(static_cast<unsigned char>(len & 0xFF));
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<unsigned char>((len >> (i * 8)) & 0xFF));
        }
    }
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

}  // namespace quant_sev::host::ws
