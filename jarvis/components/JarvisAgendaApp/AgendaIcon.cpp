/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Launcher icon for the Jarvis agenda, drawn at compile time (112 x 112 ARGB).
 */
#include <array>
#include <cstddef>
#include <cstdint>

#include "lvgl.h"

namespace {

constexpr int ICON_SIZE = 112;
constexpr size_t ICON_BYTES = ICON_SIZE * ICON_SIZE * 4;

constexpr bool insideRoundedRectangle(int x, int y, int left, int top, int right, int bottom, int radius)
{
    if (x < left || x > right || y < top || y > bottom) {
        return false;
    }
    const int cx = x < left + radius ? left + radius : (x > right - radius ? right - radius : x);
    const int cy = y < top + radius ? top + radius : (y > bottom - radius ? bottom - radius : y);
    const int dx = x - cx;
    const int dy = y - cy;
    return dx * dx + dy * dy <= radius * radius;
}

constexpr std::array<uint8_t, ICON_BYTES> makeAgendaIcon()
{
    std::array<uint8_t, ICON_BYTES> pixels{};
    for (int y = 0; y < ICON_SIZE; ++y) {
        for (int x = 0; x < ICON_SIZE; ++x) {
            int r = 0, g = 0, b = 0, a = 0;
            if (insideRoundedRectangle(x, y, 2, 2, 109, 109, 27)) {
                // Jarvis cyan to deep blue
                r = 20 + (x * 30) / 111;
                g = 190 - (y * 110) / 111;
                b = 230 - (x * 40) / 111;
                a = 255;
            }
            // shadow of the page
            if (insideRoundedRectangle(x - 3, y - 3, 22, 26, 89, 92, 10)) {
                r = (r * 50 + 10 * 50) / 100;
                g = (g * 50 + 40 * 50) / 100;
                b = (b * 50 + 70 * 50) / 100;
            }
            // the page
            if (insideRoundedRectangle(x, y, 22, 26, 89, 92, 10)) {
                r = 250; g = 252; b = 255; a = 255;
                if (y <= 44) {  // header band
                    r = 255; g = 179; b = 71;
                }
            }
            // binder rings
            if (insideRoundedRectangle(x, y, 36, 17, 43, 34, 3) ||
                    insideRoundedRectangle(x, y, 68, 17, 75, 34, 3)) {
                r = 40; g = 60; b = 80; a = 255;
            }
            // 4 x 3 grid of days
            for (int row = 0; row < 3; ++row) {
                for (int col = 0; col < 4; ++col) {
                    const int left = 31 + col * 13;
                    const int top = 52 + row * 12;
                    if (insideRoundedRectangle(x, y, left, top, left + 8, top + 7, 2)) {
                        const bool today = row == 1 && col == 2;
                        r = today ? 49 : 120;
                        g = today ? 225 : 140;
                        b = today ? 255 : 160;
                        a = 255;
                    }
                }
            }
            const size_t o = static_cast<size_t>(y * ICON_SIZE + x) * 4;
            pixels[o + 0] = static_cast<uint8_t>(b);
            pixels[o + 1] = static_cast<uint8_t>(g);
            pixels[o + 2] = static_cast<uint8_t>(r);
            pixels[o + 3] = static_cast<uint8_t>(a);
        }
    }
    return pixels;
}

alignas(4) constexpr auto AGENDA_ICON_PIXELS = makeAgendaIcon();

} // namespace

extern const lv_image_dsc_t img_app_jarvis_agenda = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_ARGB8888,
        .flags = 0,
        .w = ICON_SIZE,
        .h = ICON_SIZE,
        .stride = ICON_SIZE * 4,
        .reserved_2 = 0,
    },
    .data_size = AGENDA_ICON_PIXELS.size(),
    .data = AGENDA_ICON_PIXELS.data(),
    .reserved = nullptr,
    .reserved_2 = nullptr,
};
