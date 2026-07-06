// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "screens.hpp"

#include <array>
#include <cstddef>

#include "ap_screen.hpp"
#include "atom_status.hpp"
#include "device_ui.hpp"

namespace stackchan::app::screens {

namespace {

// --- Adapters over the existing overlay modules ---------------------------

// SoftAP provisioning QR screen. Draws straight to the panel (M5GFX::qrcode)
// and swallows every tap while AP mode is up (the on-screen 終了 button is
// the dismiss path on touch boards).
class ApScreen final : public Screen {
public:
    explicit ApScreen(M5GFX& display) : display_{display} {}
    bool active() const override { return ap_screen::active(); }
    bool draw(avatar::RichCanvas&) override
    {
        (void)ap_screen::draw(display_);
        return false; // painted the panel directly — don't push the canvas
    }
    bool handle_tap(int x, int y) override { return ap_screen::handle_tap(x, y); }

private:
    M5GFX& display_;
};

// Touch-driven tabbed settings UI (CoreS3 / StopWatch). Consumes taps even
// while inactive: the top-right corner opens it, the top-left corner is the
// one-touch mute zone.
class DeviceUiScreen final : public Screen {
public:
    bool active() const override { return ui::active(); }
    bool draw(avatar::RichCanvas& canvas) override { return ui::draw(canvas); }
    bool handle_tap(int x, int y) override { return ui::handle_tap(x, y); }
    void handle_flick(int dx, int dy) override { ui::handle_flick(dx, dy); }
};

// Button-toggled status overlay (AtomS3R / AtomS3, no LCD touch). All input
// arrives through the BtnA gesture vocabulary polled each tick.
class AtomStatusScreen final : public Screen {
public:
    bool active() const override { return atom_status::active(); }
    bool draw(avatar::RichCanvas& canvas) override { return atom_status::draw(canvas); }
    void poll_input() override { atom_status::poll_button(); }
};

// --- The stack -------------------------------------------------------------

// Storage for the adapters (no heap; exactly one settings-UI flavour is
// constructed). Priority = index order in g_stack.
alignas(ApScreen) unsigned char g_ap_storage[sizeof(ApScreen)];
DeviceUiScreen g_device_ui;
AtomStatusScreen g_atom_status;

std::array<Screen*, 2> g_stack{};
std::size_t g_count = 0;

} // namespace

void init(M5GFX& display, SharedState& state, const stackchan::board::BoardProfile& profile)
{
    g_count = 0;
    g_stack[g_count++] = new (g_ap_storage) ApScreen{display};
    if (profile.button_overlay_ui) {
        atom_status::init(state);
        g_stack[g_count++] = &g_atom_status;
    } else {
        ui::init(state);
        g_stack[g_count++] = &g_device_ui;
    }
}

bool overlay_active()
{
    for (std::size_t i = 0; i < g_count; ++i) {
        if (g_stack[i]->active()) return true;
    }
    return false;
}

bool draw_overlay(avatar::RichCanvas& canvas)
{
    for (std::size_t i = 0; i < g_count; ++i) {
        if (g_stack[i]->active()) {
            return g_stack[i]->draw(canvas);
        }
    }
    return false;
}

void poll_inputs()
{
    for (std::size_t i = 0; i < g_count; ++i) {
        g_stack[i]->poll_input();
    }
}

bool handle_tap(int x, int y)
{
    for (std::size_t i = 0; i < g_count; ++i) {
        if (g_stack[i]->handle_tap(x, y)) return true;
    }
    return false;
}

void handle_flick(int dx, int dy)
{
    for (std::size_t i = 0; i < g_count; ++i) {
        g_stack[i]->handle_flick(dx, dy);
    }
}

} // namespace stackchan::app::screens
