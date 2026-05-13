#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include <M5GFX.h>

#include "avatar/draw_context.hpp"
#include "avatar/expression.hpp"
#include "avatar/palette.hpp"

namespace stackchan::avatar {

class Avatar {
public:
    explicit Avatar(M5GFX& display);
    ~Avatar();

    Avatar(const Avatar&) = delete;
    Avatar& operator=(const Avatar&) = delete;
    Avatar(Avatar&&) noexcept;
    Avatar& operator=(Avatar&&) noexcept;

    bool begin();

    void set_expression(Expression expression) noexcept;
    void set_mouth_open(float ratio) noexcept;
    void set_gaze(float horizontal, float vertical) noexcept;
    void set_palette(const Palette& palette) noexcept;
    void set_balloon_text(std::string_view text);
    void clear_balloon() noexcept;

    // Drives animators with the current time in milliseconds and renders one frame.
    void tick(std::uint32_t now_ms);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace stackchan::avatar
