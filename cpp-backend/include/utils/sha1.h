#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace vms {
namespace utils {

class SHA1 {
public:
    static std::vector<uint8_t> digest(const std::vector<uint8_t>& input) {
        uint32_t h0 = 0x67452301;
        uint32_t h1 = 0xEFCDAB89;
        uint32_t h2 = 0x98BADCFE;
        uint32_t h3 = 0x10325476;
        uint32_t h4 = 0xC3D2E1F0;

        std::vector<uint8_t> msg = input;
        uint64_t bit_len = msg.size() * 8;
        msg.push_back(0x80);
        while ((msg.size() * 8) % 512 != 448) msg.push_back(0);
        for (int i = 0; i < 8; i++) msg.push_back(static_cast<uint8_t>((bit_len >> (56 - i * 8)) & 0xFF));

        for (size_t i = 0; i < msg.size(); i += 64) {
            uint32_t w[80];
            for (int j = 0; j < 16; j++) {
                w[j] = (msg[i + j * 4] << 24) | (msg[i + j * 4 + 1] << 16) | (msg[i + j * 4 + 2] << 8) | (msg[i + j * 4 + 3]);
            }
            for (int j = 16; j < 80; j++) w[j] = left_rotate(w[j - 3] ^ w[j - 8] ^ w[j - 14] ^ w[j - 16], 1);

            uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
            for (int j = 0; j < 80; j++) {
                uint32_t f, k;
                if (j < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999; }
                else if (j < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
                else if (j < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
                else { f = b ^ c ^ d; k = 0xCA62C1D6; }

                uint32_t temp = left_rotate(a, 5) + f + e + k + w[j];
                e = d; d = c; c = left_rotate(b, 30); b = a; a = temp;
            }
            h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
        }

        std::vector<uint8_t> res(20);
        auto put = [&](size_t idx, uint32_t val) {
            res[idx] = (val >> 24) & 0xFF; res[idx+1] = (val >> 16) & 0xFF;
            res[idx+2] = (val >> 8) & 0xFF; res[idx+3] = val & 0xFF;
        };
        put(0, h0); put(4, h1); put(8, h2); put(12, h3); put(16, h4);
        return res;
    }

    static std::vector<uint8_t> hmac(const std::vector<uint8_t>& key_in, const std::vector<uint8_t>& msg) {
        std::vector<uint8_t> key = key_in;
        if (key.size() > 64) key = digest(key);
        if (key.size() < 64) key.resize(64, 0);

        std::vector<uint8_t> o_key_pad(64), i_key_pad(64);
        for (int i = 0; i < 64; i++) {
            o_key_pad[i] = key[i] ^ 0x5c;
            i_key_pad[i] = key[i] ^ 0x36;
        }

        std::vector<uint8_t> inner = i_key_pad;
        inner.insert(inner.end(), msg.begin(), msg.end());
        std::vector<uint8_t> inner_hash = digest(inner);

        std::vector<uint8_t> outer = o_key_pad;
        outer.insert(outer.end(), inner_hash.begin(), inner_hash.end());
        return digest(outer);
    }

private:
    static uint32_t left_rotate(uint32_t x, uint32_t n) { return (x << n) | (x >> (32 - n)); }
};

} // namespace utils
} // namespace vms
