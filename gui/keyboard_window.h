#pragma once

#include <cstdint>
#include <vector>
#include <set>
#include "idebug_backend.h"

union SDL_Event;
struct ImFont;

// ---------------------------------------------------------------------------
// KeyboardWindow — Virtual Vector-06C keyboard (ImGui/ImDrawList).
//
// Renders the same 5-row layout as the PSP port's vkbd.
// Mouse click sends key press/release through IDebugBackend.
// Modifier keys (SS/US) toggle sticky; other keys are momentary.
//
// "Lock" mode: when enabled, physical keyboard events are forwarded to
// the emulator as if the user clicked the virtual keys.
// ---------------------------------------------------------------------------

class KeyboardWindow
{
public:
    KeyboardWindow();

    void render(IDebugBackend &backend);

    //! Forward an SDL event to the emulator when keyboard lock is active.
    //! Returns true if the event was consumed (sent to emulator).
    bool handleSdlEvent(const SDL_Event &event, IDebugBackend &backend);

    //! Release all physically-held keys (called when lock is disabled or
    //! window is hidden to prevent stuck keys in the emulator).
    void releaseAllKeys(IDebugBackend &backend);

    bool &getVisibleRef() { return visible_; }
    bool isLocked() const { return keyboardLock_; }
    void requestRefresh() {}

    //! True if the user clicked a virtual ВВОД/СБР key (machine reset) since
    //! the last call — the GUI uses it to refresh all views and the ROM label.
    bool consumeResetPerformed() { bool v = resetPerformed_; resetPerformed_ = false; return v; }

private:
    enum KeyColor {
        KC_ALPHA,    // beige — letter/number keys
        KC_BROWN,    // dark brown — УС, Ж, Э, ВК, СС, АР2
        KC_GREEN,    // olive — УС, ЗБ, ПС, space, tab
        KC_FN,       // golden — F1-F5, РУС, ТАБ
    };

    struct KeyDef {
        float col, row;      // grid position (col may be fractional for offsets)
        float w, h;          // grid units
        int scancode;        // SDL_Scancode value (0 = spacer)
        const char *label;   // Latin legend (bottom-right)
        const char *label_ru;// Russian legend (top-left), nullptr = none
        KeyColor color;
    };

    bool visible_ = true;
    bool keyboardLock_ = false;            // "Lock" mode — forward physical keys

    // Key geometry
    static constexpr float kKeyW = 32.0f;
    static constexpr float kKeyH = 28.0f;
    static constexpr float kGap  = 2.0f;

    // Mouse/press state
    int pressedScancode_ = 0;          // momentary key held by mouse
    std::set<int> stickyKeys_;         // toggled modifiers (SS/US)
    int selectedScancode_ = 0;         // last clicked (for visual feedback)
    float selectionTimer_ = 0.0f;
    float ruslatHoldTimer_ = 0.0f;     // hold F6 for several frames so ROM detects it
    bool resetPerformed_ = false;      // ВВОД/СБР clicked → GUI refresh pending

    // ВВОД/СБР: system keys — they reset the machine only while БЛК is latched
    static bool isResetKey(int scancode);
    bool isBlkLatched() const;
    void applyResetKey(int scancode, IDebugBackend &backend);

    // Physical keyboard passthrough state
    std::set<int> activeKeys_;         // scancodes currently held on host keyboard

    // Layout
    static const std::vector<KeyDef> &getKeyLayout();

public:
    // Small font for long labels on narrow keys (set by DebuggerGui::init)
    static ImFont *sSmallFont;

private:
};
