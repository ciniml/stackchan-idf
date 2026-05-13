#pragma once

#include <cstdint>

namespace stackchan::avatar {

// 16-bit RGB565 palette. Colours match m5stack-avatar-rs defaults.
struct Palette {
    std::uint16_t primary;
    std::uint16_t background;
    std::uint16_t secondary;
    std::uint16_t balloon_foreground;
    std::uint16_t balloon_background;
};

inline constexpr Palette kDefaultPalette{
    .primary = 0xFFFFu,            // white
    .background = 0x0000u,         // black
    .secondary = 0xFFE0u,          // yellow
    .balloon_foreground = 0xFFFFu, // white
    .balloon_background = 0x0000u, // black
};

} // namespace stackchan::avatar
