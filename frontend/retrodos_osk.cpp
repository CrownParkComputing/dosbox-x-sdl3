/*
 * retro-dosbox — on-screen keyboard.
 *
 * A handheld has no keyboard, and DOS games are full of titles that cannot be
 * started, configured or quit without one. This is not a text-entry widget:
 * DOS reads the keyboard at the SCANCODE level, so every key here sends a
 * scancode down and up through retrodos_host_send_key, exactly as a real key
 * would arrive.
 *
 * Modifiers are STICKY rather than held. You cannot press Ctrl and C at once
 * with one finger, so Ctrl latches, applies to the next key, and releases
 * itself -- which is what makes Ctrl+C, Alt+X and the Ctrl+Alt+Del of a
 * hundred install programs reachable at all.
 */
#include "retrodos_osk.h"

#include "imgui.h"
#include "retrodos_host.h"

#include <SDL3/SDL.h>

namespace retrodos {
namespace {

/* Share of screen height the keyboard may take. It spans the full width, so
 * this only decides how tall the keys are -- enough for a comfortable target
 * while still leaving most of the DOS picture visible above it. */
const float kHeightShare = 0.42f;

struct Key {
    const char *label;
    int         scancode;
    float       width;   /* in units of one standard key */
};

/* Compact layout: a full 104-key board is unusable at thumb size, so this is
 * the set DOS software actually needs -- letters, digits, the punctuation DOS
 * paths use, function keys, and the editing/arrow cluster. */
const Key kRow0[] = {
    {"Esc", SDL_SCANCODE_ESCAPE, 1.4f},
    {"F1", SDL_SCANCODE_F1, 1}, {"F2", SDL_SCANCODE_F2, 1},
    {"F3", SDL_SCANCODE_F3, 1}, {"F4", SDL_SCANCODE_F4, 1},
    {"F5", SDL_SCANCODE_F5, 1}, {"F6", SDL_SCANCODE_F6, 1},
    {"F7", SDL_SCANCODE_F7, 1}, {"F8", SDL_SCANCODE_F8, 1},
    {"F9", SDL_SCANCODE_F9, 1}, {"F10", SDL_SCANCODE_F10, 1},
    {"F11", SDL_SCANCODE_F11, 1}, {"F12", SDL_SCANCODE_F12, 1},
};
const Key kRow1[] = {
    {"1", SDL_SCANCODE_1, 1}, {"2", SDL_SCANCODE_2, 1}, {"3", SDL_SCANCODE_3, 1},
    {"4", SDL_SCANCODE_4, 1}, {"5", SDL_SCANCODE_5, 1}, {"6", SDL_SCANCODE_6, 1},
    {"7", SDL_SCANCODE_7, 1}, {"8", SDL_SCANCODE_8, 1}, {"9", SDL_SCANCODE_9, 1},
    {"0", SDL_SCANCODE_0, 1}, {"-", SDL_SCANCODE_MINUS, 1},
    {"=", SDL_SCANCODE_EQUALS, 1}, {"Bksp", SDL_SCANCODE_BACKSPACE, 1.6f},
};
const Key kRow2[] = {
    {"Tab", SDL_SCANCODE_TAB, 1.4f},
    {"Q", SDL_SCANCODE_Q, 1}, {"W", SDL_SCANCODE_W, 1}, {"E", SDL_SCANCODE_E, 1},
    {"R", SDL_SCANCODE_R, 1}, {"T", SDL_SCANCODE_T, 1}, {"Y", SDL_SCANCODE_Y, 1},
    {"U", SDL_SCANCODE_U, 1}, {"I", SDL_SCANCODE_I, 1}, {"O", SDL_SCANCODE_O, 1},
    {"P", SDL_SCANCODE_P, 1}, {"[", SDL_SCANCODE_LEFTBRACKET, 1},
    {"]", SDL_SCANCODE_RIGHTBRACKET, 1},
};
const Key kRow3[] = {
    {"Caps", SDL_SCANCODE_CAPSLOCK, 1.6f},
    {"A", SDL_SCANCODE_A, 1}, {"S", SDL_SCANCODE_S, 1}, {"D", SDL_SCANCODE_D, 1},
    {"F", SDL_SCANCODE_F, 1}, {"G", SDL_SCANCODE_G, 1}, {"H", SDL_SCANCODE_H, 1},
    {"J", SDL_SCANCODE_J, 1}, {"K", SDL_SCANCODE_K, 1}, {"L", SDL_SCANCODE_L, 1},
    {";", SDL_SCANCODE_SEMICOLON, 1}, {"'", SDL_SCANCODE_APOSTROPHE, 1},
    {"Enter", SDL_SCANCODE_RETURN, 1.8f},
};
const Key kRow4[] = {
    {"Shift", SDL_SCANCODE_LSHIFT, 2.0f},
    {"Z", SDL_SCANCODE_Z, 1}, {"X", SDL_SCANCODE_X, 1}, {"C", SDL_SCANCODE_C, 1},
    {"V", SDL_SCANCODE_V, 1}, {"B", SDL_SCANCODE_B, 1}, {"N", SDL_SCANCODE_N, 1},
    {"M", SDL_SCANCODE_M, 1}, {",", SDL_SCANCODE_COMMA, 1},
    {".", SDL_SCANCODE_PERIOD, 1}, {"/", SDL_SCANCODE_SLASH, 1},
    {"\\", SDL_SCANCODE_BACKSLASH, 1},
};
const Key kRow5[] = {
    {"Ctrl", SDL_SCANCODE_LCTRL, 1.6f},
    {"Alt", SDL_SCANCODE_LALT, 1.4f},
    {"Space", SDL_SCANCODE_SPACE, 5.0f},
    {"<", SDL_SCANCODE_LEFT, 1}, {"^", SDL_SCANCODE_UP, 1},
    {"v", SDL_SCANCODE_DOWN, 1}, {">", SDL_SCANCODE_RIGHT, 1},
};

bool g_shift = false, g_ctrl = false, g_alt = false;

bool is_modifier(int sc)
{
    return sc == SDL_SCANCODE_LSHIFT || sc == SDL_SCANCODE_LCTRL ||
           sc == SDL_SCANCODE_LALT;
}

void tap(int scancode)
{
    /* Modifiers first, so the guest sees them held when the key arrives. */
    if (g_ctrl)  retrodos_host_send_key(SDL_SCANCODE_LCTRL,  true);
    if (g_alt)   retrodos_host_send_key(SDL_SCANCODE_LALT,   true);
    if (g_shift) retrodos_host_send_key(SDL_SCANCODE_LSHIFT, true);

    retrodos_host_send_key(scancode, true);
    retrodos_host_send_key(scancode, false);

    if (g_shift) { retrodos_host_send_key(SDL_SCANCODE_LSHIFT, false); g_shift = false; }
    if (g_alt)   { retrodos_host_send_key(SDL_SCANCODE_LALT,   false); g_alt   = false; }
    if (g_ctrl)  { retrodos_host_send_key(SDL_SCANCODE_LCTRL,  false); g_ctrl  = false; }
}

void draw_row(const Key *keys, int count, float unit_w, float unit_h, float gap)
{
    for (int i = 0; i < count; ++i) {
        if (i) ImGui::SameLine(0.0f, gap);
        const Key &k = keys[i];

        bool latched = false;
        if      (k.scancode == SDL_SCANCODE_LSHIFT) latched = g_shift;
        else if (k.scancode == SDL_SCANCODE_LCTRL)  latched = g_ctrl;
        else if (k.scancode == SDL_SCANCODE_LALT)   latched = g_alt;

        if (latched)
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));

        ImGui::PushID(k.scancode * 131 + i);
        ImGui::Button(k.label, ImVec2(unit_w * k.width, unit_h));

        /* Fires on PRESS, not on release over the same key.
         *
         * A real key acts the moment it goes down, and a thumb that lands on a
         * key and slides slightly off before lifting still means to have
         * pressed it -- with release-based activation that input is simply
         * lost, which on a touchscreen happens constantly. */
        if (ImGui::IsItemActivated()) {
            if (is_modifier(k.scancode)) {
                /* Sticky: latch for the NEXT key rather than requiring two
                 * fingers, which a handheld cannot offer. */
                if      (k.scancode == SDL_SCANCODE_LSHIFT) g_shift = !g_shift;
                else if (k.scancode == SDL_SCANCODE_LCTRL)  g_ctrl  = !g_ctrl;
                else                                        g_alt   = !g_alt;
            } else {
                tap(k.scancode);
            }
        }
        ImGui::PopID();

        if (latched) ImGui::PopStyleColor();
    }
}

} /* namespace */

void osk_draw(float screen_w, float screen_h)
{
    /* Keys are RECTANGLES, not squares.
     *
     * Sizing everything from one unit forces a choice between a keyboard that
     * spans the screen and one that leaves the game visible: the layout is
     * ~14.5 wide by 6 tall, so a full-width board on 16:9 is about 70% of the
     * height, and shrinking it to fit a height budget leaves it under half the
     * screen wide -- a cramped island with a lot of empty space beside it.
     *
     * Real keyboards are not square-keyed either. Taking WIDTH from the screen
     * width and HEIGHT from a separate budget gives a board that spans the
     * display, stays about two fifths of its height, and has comfortably large
     * targets. */
    const float gap  = ImGui::GetStyle().ItemSpacing.x * 0.4f;
    const float pad  = ImGui::GetStyle().WindowPadding.y;

    const float unit_w = (screen_w * 0.985f - gap * 14.0f - pad * 2.0f) / 14.5f;
    const float unit_h = (screen_h * kHeightShare - gap * 5.0f - pad * 2.0f) / 6.0f;

    const float kb_w = unit_w * 14.5f + gap * 14.0f + pad * 2.0f;
    const float kb_h = unit_h * 6.0f  + gap * 5.0f  + pad * 2.0f;

    ImGui::SetNextWindowPos(ImVec2(screen_w * 0.5f, screen_h - pad), ImGuiCond_Always,
                            ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(kb_w, kb_h));
    ImGui::SetNextWindowBgAlpha(0.88f);   /* keep the game readable behind it */
    ImGui::Begin("##osk", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoNav);

    draw_row(kRow0, SDL_arraysize(kRow0), unit_w, unit_h, gap);
    draw_row(kRow1, SDL_arraysize(kRow1), unit_w, unit_h, gap);
    draw_row(kRow2, SDL_arraysize(kRow2), unit_w, unit_h, gap);
    draw_row(kRow3, SDL_arraysize(kRow3), unit_w, unit_h, gap);
    draw_row(kRow4, SDL_arraysize(kRow4), unit_w, unit_h, gap);
    draw_row(kRow5, SDL_arraysize(kRow5), unit_w, unit_h, gap);

    ImGui::End();
}

void osk_send_ctrl_alt_del(void)
{
    /* Enough install programs and DOS shells rely on this that it deserves a
     * dedicated button rather than three sticky taps. */
    retrodos_host_send_key(SDL_SCANCODE_LCTRL, true);
    retrodos_host_send_key(SDL_SCANCODE_LALT,  true);
    retrodos_host_send_key(SDL_SCANCODE_DELETE, true);
    retrodos_host_send_key(SDL_SCANCODE_DELETE, false);
    retrodos_host_send_key(SDL_SCANCODE_LALT,  false);
    retrodos_host_send_key(SDL_SCANCODE_LCTRL, false);
}

} /* namespace retrodos */
