#pragma once
#include <array>
#include <vector>
#include <cstdint>
#include <string_view>

namespace edmo {
constexpr uint32_t width = 192, height = 32;
constexpr uint32_t liveWidth = 1280, liveHeight = 960;
using LivePixels = std::vector<uint32_t>;
using Pixels = std::array<uint32_t, width * height>;

// Five columns of seven pixels per glyph. Straight alpha, little-endian RGBA/BGRA.
inline Pixels testTexture(bool bgra) noexcept {
    constexpr std::string_view alphabet = "EDMOVR NLI";
    constexpr uint8_t glyph[][5] = {{127, 73, 73, 73, 65},
                                    {127, 65, 65, 34, 28},
                                    {127, 2, 12, 2, 127},
                                    {62, 65, 65, 65, 62},
                                    {31, 32, 64, 32, 31},
                                    {127, 9, 25, 41, 70},
                                    {},
                                    {127, 2, 4, 8, 127},
                                    {127, 64, 64, 64, 64},
                                    {0, 65, 127, 65, 0}};
    constexpr std::string_view label = "EDMO VR ONLINE";
    Pixels pixels{};
    for (uint32_t y = 4; y < height - 4; ++y)
        for (uint32_t x = 5; x < width - 5; ++x)
            pixels[y * width + x] = 0x780e0e0e;
    const uint32_t ink = bgra ? 0xff65efe3 : 0xffe3ef65;
    uint32_t start = (width - (label.size() * 12 - 2)) / 2;
    for (char letter : label) {
        const auto& columns = glyph[alphabet.find(letter)];
        for (uint32_t x = 0; x < 5; ++x)
            for (uint32_t y = 0; y < 7; ++y)
                if (columns[x] & (1u << y)) {
                    const auto i = (9 + 2 * y) * width + start + 2 * x;
                    pixels[i] = pixels[i + 1] = pixels[i + width] = pixels[i + width + 1] = ink;
                }
        start += 12;
    }
    return pixels;
}
} // namespace edmo
