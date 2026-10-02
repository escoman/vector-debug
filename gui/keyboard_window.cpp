#include "keyboard_window.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "SDL.h"

#include <cstdio>
#include <cstring>
#include <cmath>

// SDL scancode constants (from SDL2/SDL_scancode.h)
enum {
    SC_A = 4,  SC_B = 5,  SC_C = 6,  SC_D = 7,
    SC_E = 8,  SC_F = 9,  SC_G = 10, SC_H = 11,
    SC_I = 12, SC_J = 13, SC_K = 14, SC_L = 15,
    SC_M = 16, SC_N = 17, SC_O = 18, SC_P = 19,
    SC_Q = 20, SC_R = 21, SC_S = 22, SC_T = 23,
    SC_U = 24, SC_V = 25, SC_W = 26, SC_X = 27,
    SC_Y = 28, SC_Z = 29,
    SC_1 = 30, SC_2 = 31, SC_3 = 32, SC_4 = 33,
    SC_5 = 34, SC_6 = 35, SC_7 = 36, SC_8 = 37,
    SC_9 = 38, SC_0 = 39,
    SC_RETURN    = 40,
    SC_ESCAPE    = 41,
    SC_BACKSPACE = 42,
    SC_TAB       = 43,
    SC_SPACE     = 44,
    SC_MINUS     = 45,
    SC_EQUALS    = 46,
    SC_LBRACKET  = 47,
    SC_RBRACKET  = 48,
    SC_BACKSLASH = 49,
    SC_SEMICOLON = 51,
    SC_APOSTROPHE= 52,
    SC_GRAVE     = 53,
    SC_COMMA     = 54,
    SC_PERIOD    = 55,
    SC_SLASH     = 56,
    SC_F1 = 58, SC_F2 = 59, SC_F3 = 60, SC_F4 = 61, SC_F5 = 62,
    SC_F6 = 63, SC_F7 = 64, SC_F8 = 65,
    SC_F11 = 68, SC_F12 = 69,
    SC_UP    = 82,
    SC_LEFT  = 80,
    SC_RIGHT = 79,
    SC_DOWN  = 81,
    SC_LSHIFT = 225,
    SC_LCTRL  = 224,
    SC_LALT   = 226,
    // БЛК is a system key: on a real Vector it is wired to the reset circuit
    // together with ВВОД/СБР and has no keyboard-matrix line of its own. The
    // virtual key only latches host-side, so it never reaches the emulator.
    SC_BLK    = -100,
};

// ---------------------------------------------------------------------------
// Key layout — matches the PSP port's vkbd layout
// ---------------------------------------------------------------------------

const std::vector<KeyboardWindow::KeyDef> &KeyboardWindow::getKeyLayout()
{
    static const std::vector<KeyDef> layout = {
        // Row 0: ; 1 2 3 4 5 6 7 8 9 0 - /
        //   RU: ; 1 2 3 4 5 6 7 8 9 0 - /
        //   EN: + ! @ # $ % ^ & * ( ) _ ?
        {0.0f, 0, 1, 1, SC_SEMICOLON, "+",  ";",  KC_ALPHA},
        {1.0f, 0, 1, 1, SC_1,           "!",  "1",  KC_ALPHA},
        {2.0f, 0, 1, 1, SC_2,           "@",  "2",  KC_ALPHA},
        {3.0f, 0, 1, 1, SC_3,           "#",  "3",  KC_ALPHA},
        {4.0f, 0, 1, 1, SC_4,           "$",  "4",  KC_ALPHA},
        {5.0f, 0, 1, 1, SC_5,           "%",  "5",  KC_ALPHA},
        {6.0f, 0, 1, 1, SC_6,           "^",  "6",  KC_ALPHA},
        {7.0f, 0, 1, 1, SC_7,           "&",  "7",  KC_ALPHA},
        {8.0f, 0, 1, 1, SC_8,           "*",  "8",  KC_ALPHA},
        {9.0f, 0, 1, 1, SC_9,           "(",  "9",  KC_ALPHA},
        {10.0f,0, 1, 1, SC_0,           ")",  "0",  KC_ALPHA},
        {11.0f,0, 1, 1, SC_EQUALS,      "_",  "-",  KC_ALPHA},
        {12.0f,0, 1, 1, SC_SLASH,       "?",  "/",  KC_ALPHA},

        // Row 1 (indented 0.5u): J C U K E N G [ ] Z H '
        //   RU: Й Ц У К Е Н Г Ш Щ З Х :
        {0.5f, 1, 1, 1, SC_J,           "J",  "\xd0\xb9", KC_ALPHA},  // й
        {1.5f, 1, 1, 1, SC_C,           "C",  "\xd1\x86", KC_ALPHA},  // ц
        {2.5f, 1, 1, 1, SC_U,           "U",  "\xd1\x83", KC_ALPHA},  // у
        {3.5f, 1, 1, 1, SC_K,           "K",  "\xd0\xba", KC_ALPHA},  // к
        {4.5f, 1, 1, 1, SC_E,           "E",  "\xd0\xb5", KC_ALPHA},  // е
        {5.5f, 1, 1, 1, SC_N,           "N",  "\xd0\xbd", KC_ALPHA},  // н
        {6.5f, 1, 1, 1, SC_G,           "G",  "\xd0\xb3", KC_ALPHA},  // г
        {7.5f, 1, 1, 1, SC_LBRACKET,    "[",  "\xd1\x88", KC_ALPHA},  // ш
        {8.5f, 1, 1, 1, SC_RBRACKET,    "]",  "\xd1\x89", KC_ALPHA},  // щ
        {9.5f, 1, 1, 1, SC_Z,           "Z",  "\xd0\xb7", KC_ALPHA},  // з
        {10.5f,1, 1, 1, SC_H,           "H",  "\xd1\x85", KC_ALPHA},  // х
        {11.5f,1, 1, 1, SC_APOSTROPHE,  "'",  ":",      KC_ALPHA},

        // Row 2: US F Y W A P R O L D V \ .
        //   RU: УС Ф Ы В А П Р О Л Д Ж Э .
        {0.0f, 2, 1, 1, SC_LCTRL,       "", "\xd0\xa3\xd0\xa1", KC_GREEN},  // УС
        {1.0f, 2, 1, 1, SC_F,           "F",  "\xd1\x84", KC_ALPHA},  // ф
        {2.0f, 2, 1, 1, SC_Y,           "Y",  "\xd1\x8b", KC_ALPHA},  // ы
        {3.0f, 2, 1, 1, SC_W,           "W",  "\xd0\xb2", KC_ALPHA},  // в
        {4.0f, 2, 1, 1, SC_A,           "A",  "\xd0\xb0", KC_ALPHA},  // а
        {5.0f, 2, 1, 1, SC_P,           "P",  "\xd0\xbf", KC_ALPHA},  // п
        {6.0f, 2, 1, 1, SC_R,           "R",  "\xd1\x80", KC_ALPHA},  // р
        {7.0f, 2, 1, 1, SC_O,           "O",  "\xd0\xbe", KC_ALPHA},  // о
        {8.0f, 2, 1, 1, SC_L,           "L",  "\xd0\xbb", KC_ALPHA},  // л
        {9.0f, 2, 1, 1, SC_D,           "D",  "\xd0\xb4", KC_ALPHA},  // д
        {10.0f,2, 1, 1, SC_V,           "V",  "\xd0\xb6", KC_ALPHA},  // ж
        {11.0f,2, 1, 1, SC_BACKSLASH,   "\\", "\xd1\x8d", KC_BROWN},  // э
        {12.0f,2, 1, 1, SC_PERIOD,      ".",  ">",      KC_ALPHA},

        // Row 3: SS Q ^ S M I T X B - , VK
        //   RU: СС Й ^ С М И Т Ь Б - , ВК
        {0.0f, 3, 1, 1, SC_LSHIFT,      "", "\xd0\xa1\xd0\xa1", KC_BROWN},  // СС
        {1.0f, 3, 1, 1, SC_Q,           "Q",  "\xd0\xb9", KC_ALPHA},  // й
        {2.0f, 3, 1, 1, SC_GRAVE,       "^",  "^",      KC_ALPHA},
        {3.0f, 3, 1, 1, SC_S,           "S",  "\xd1\x81", KC_ALPHA},  // с
        {4.0f, 3, 1, 1, SC_M,           "M",  "\xd0\xbc", KC_ALPHA},  // м
        {5.0f, 3, 1, 1, SC_I,           "I",  "\xd0\xb8", KC_ALPHA},  // и
        {6.0f, 3, 1, 1, SC_T,           "T",  "\xd1\x82", KC_ALPHA},  // т
        {7.0f, 3, 1, 1, SC_X,           "X",  "\xd1\x8c", KC_ALPHA},  // ь
        {8.0f, 3, 1, 1, SC_B,           "B",  "\xd0\xb1", KC_ALPHA},  // б
        {9.0f, 3, 1, 1, SC_MINUS,       "-",  "@",      KC_ALPHA},
        {10.0f,3, 1, 1, SC_COMMA,       ",",  "<",      KC_ALPHA},
        {11.0f,3, 1, 1, SC_RETURN,      "", "\xd0\x92\xd0\x9a", KC_GREEN},  // ВК

        // Row 4: RUS TAB PS ZB
        //   RU: РУС ТАБ ПС ЗБ
        {0.0f, 4, 1.5f, 1, SC_F6,       "RUS","\xd0\xa0\xd0\xa3\xd0\xa1",KC_FN},  // РУС
        {1.5f, 4, 1.5f, 1, SC_TAB,      "","\xd0\xa2\xd0\x90\xd0\x91",KC_FN},  // ТАБ
        {3.0f, 4, 5.0f, 1, SC_SPACE,    "",   nullptr, KC_GREEN},
        {8.0f, 4, 1.5f, 1, SC_LALT,     "", "\xd0\x9f\xd0\xa1", KC_GREEN},  // ПС
        {9.5f, 4, 1.5f, 1, SC_BACKSPACE,"", "\xd0\x97\xd0\x91", KC_ALPHA},  // ЗБ

        // --- Numpad (3 columns, offset 14.5u) ---
        // Single centered label (label_ru = nullptr)

        // Row 0: ВВОД БЛК СБР
        //   Three system keys — harmless on their own. The Vector resets only
        //   on БЛК+ВВОД (attach boot ROM) or БЛК+СБР (detach boot ROM), so БЛК
        //   latches like СС/УС and ВВОД/СБР fire the request while it is held.
        {14.5f, 0, 1, 1, SC_F11,       "\xd0\x92\xd0\x92\xd0\x9e\xd0\x94", nullptr, KC_BROWN},  // ВВОД
        {15.5f, 0, 1, 1, SC_BLK,       "\xd0\x91\xd0\x9b\xd0\x9a", nullptr, KC_BROWN},  // БЛК
        {16.5f, 0, 1, 1, SC_F12,       "\xd0\xa1\xd0\x91\xd0\xa0", nullptr, KC_BROWN},  // СБР

        // Row 1: F1 F2 F3
        {14.5f, 1, 1, 1, SC_F1,        "F1",  nullptr, KC_FN},
        {15.5f, 1, 1, 1, SC_F2,        "F2",  nullptr, KC_FN},
        {16.5f, 1, 1, 1, SC_F3,        "F3",  nullptr, KC_FN},

        // Row 2: F4 F5 AR2
        {14.5f, 2, 1, 1, SC_F4,        "F4",  nullptr, KC_FN},
        {15.5f, 2, 1, 1, SC_F5,        "F5",  nullptr, KC_FN},
        {16.5f, 2, 1, 1, SC_ESCAPE,    "\xd0\x90\xd0\xa0\x32", nullptr, KC_BROWN},  // АР2

        // Row 3: ↖ ↑ СТР
        //   ↖ = matrix col 1 bit 0x01 (the "^\" legend is the arrow glyph),
        //   СТР = matrix col 1 bit 0x02 — both are host scancodes F7/F8.
        {14.5f, 3, 1, 1, SC_F7,        "\xe2\x86\x96", nullptr, KC_ALPHA},  // ↖
        {15.5f, 3, 1, 1, SC_UP,        "\xe2\x96\xb2", nullptr, KC_ALPHA},  // ▲
        {16.5f, 3, 1, 1, SC_F8,        "\xd0\xa1\xd0\xa2\xd0\xa0", nullptr, KC_ALPHA},  // СТР

        // Row 4: ← ↓ →
        {14.5f, 4, 1, 1, SC_LEFT,      "\xe2\x97\x80", nullptr, KC_ALPHA},  // ◀
        {15.5f, 4, 1, 1, SC_DOWN,      "\xe2\x96\xbc", nullptr, KC_ALPHA},  // ▼
        {16.5f, 4, 1, 1, SC_RIGHT,     "\xe2\x96\xb6", nullptr, KC_ALPHA},  // ▶
    };
    return layout;
}

// ---------------------------------------------------------------------------
// Static members
// ---------------------------------------------------------------------------

ImFont *KeyboardWindow::sSmallFont = nullptr;

// ---------------------------------------------------------------------------
// ВВОД / СБР — machine reset, not matrix keys
// ---------------------------------------------------------------------------

bool KeyboardWindow::isResetKey(int scancode)
{
    return scancode == SC_F11 || scancode == SC_F12;
}

bool KeyboardWindow::isBlkLatched() const
{
    return stickyKeys_.count(SC_BLK) > 0;
}

void KeyboardWindow::applyResetKey(int scancode, IDebugBackend &backend)
{
    if (scancode == SC_F11) {
        backend.requestReset();      // БЛК+ВВОД: attach boot ROM, PC=0
    } else if (scancode == SC_F12) {
        backend.requestRestart();    // БЛК+СБР: detach boot ROM, PC=0
    } else {
        return;
    }
    resetPerformed_ = true;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

KeyboardWindow::KeyboardWindow()
{
}

// ---------------------------------------------------------------------------
// SDL event forwarding (physical keyboard → emulator)
// ---------------------------------------------------------------------------

bool KeyboardWindow::handleSdlEvent(const SDL_Event &event, IDebugBackend &backend)
{
    if (!keyboardLock_) return false;

    if (event.type == SDL_KEYDOWN && !event.key.repeat) {
        int sc = event.key.keysym.scancode;
        if (isResetKey(sc)) {
            // Host F11/F12 already stand for the whole БЛК+ВВОД / БЛК+СБР
            // combo — the core does the same in Keyboard::key_down(). The
            // injection path goes straight to apply_key(), which has no
            // matrix entry for them, so map them here instead.
            applyResetKey(sc, backend);
            return true;
        }
        if (activeKeys_.insert(sc).second) {
            backend.pressKey(sc);
        }
        return true;
    }
    if (event.type == SDL_KEYUP) {
        int sc = event.key.keysym.scancode;
        auto it = activeKeys_.find(sc);
        if (it != activeKeys_.end()) {
            activeKeys_.erase(it);
            backend.releaseKey(sc);
        }
        return true;
    }
    return false;
}

void KeyboardWindow::releaseAllKeys(IDebugBackend &backend)
{
    for (int sc : activeKeys_) {
        backend.releaseKey(sc);
    }
    activeKeys_.clear();
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void KeyboardWindow::render(IDebugBackend &backend)
{
    // Release physical keys when window becomes hidden
    static bool wasVisible = visible_;
    if (!visible_ && wasVisible) {
        releaseAllKeys(backend);
        stickyKeys_.erase(SC_BLK);  // never leave БЛК armed out of sight
    }
    wasVisible = visible_;

    if (!visible_) return;

    const auto &keys = getKeyLayout();
    const float pad = 6.0f;
    const float totalW = 17.5f * kKeyW + 2.0f * pad;
    const float totalH = 5.0f * kKeyH + 2.0f * pad;

    ImGui::SetNextWindowSize(ImVec2(totalW + 16, totalH + 40), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Keyboard", &visible_)) {
        ImGui::End();
        return;
    }

    // Lock checkbox — forward physical keyboard to emulator
    ImGui::Checkbox("Lock", &keyboardLock_);
    if (!keyboardLock_ && !activeKeys_.empty()) {
        releaseAllKeys(backend);
    }

    ImDrawList *drawList = ImGui::GetWindowDrawList();
    ImVec2 cursorScreenPos = ImGui::GetCursorScreenPos();
    ImVec2 canvasSize(totalW, totalH);

    // Background
    drawList->AddRectFilled(cursorScreenPos,
        ImVec2(cursorScreenPos.x + canvasSize.x, cursorScreenPos.y + canvasSize.y),
        IM_COL32(40, 40, 48, 255));

    // Invisible interactive area
    ImGui::InvisibleButton("##kbd_canvas", canvasSize);
    bool isItemActive = ImGui::IsItemActive();

    // Get mouse position and determine which key is hovered
    ImVec2 mousePos = ImGui::GetMousePos();
    float localMx = mousePos.x - cursorScreenPos.x - pad;
    float localMy = mousePos.y - cursorScreenPos.y - pad;

    int hoveredScancode = 0;
    for (const auto &k : keys) {
        if (k.scancode == 0) continue;
        float kx = k.col * kKeyW + pad;
        float ky = k.row * kKeyH + pad;
        float kw = k.w * kKeyW - kGap;
        float kh = k.h * kKeyH - kGap;
        if (localMx >= kx && localMx < kx + kw &&
            localMy >= ky && localMy < ky + kh) {
            hoveredScancode = k.scancode;
            break;
        }
    }

    // --- Mouse interaction ---
    bool mouseClicked = ImGui::IsMouseClicked(0) && isItemActive;
    bool mouseReleased = ImGui::IsMouseReleased(0);

    // Release momentary key
    if (mouseReleased && pressedScancode_ != 0) {
        backend.releaseKey(pressedScancode_);
        pressedScancode_ = 0;
    }

    // Press new key
    if (mouseClicked && hoveredScancode != 0) {
        bool isSticky = (hoveredScancode == SC_LSHIFT ||
                         hoveredScancode == SC_LCTRL);
        bool isRuslat = (hoveredScancode == SC_F6);
        bool isBlk = (hoveredScancode == SC_BLK);
        if (isBlk) {
            // БЛК: latches like СС/УС, but is a pure system key — nothing is
            // sent to the matrix, it only arms the ВВОД/СБР resets.
            if (isBlkLatched()) {
                stickyKeys_.erase(SC_BLK);
            } else {
                stickyKeys_.insert(SC_BLK);
            }
        } else if (isSticky) {
            auto it = stickyKeys_.find(hoveredScancode);
            if (it != stickyKeys_.end()) {
                stickyKeys_.erase(it);
                backend.releaseKey(hoveredScancode);
            } else {
                stickyKeys_.insert(hoveredScancode);
                backend.pressKey(hoveredScancode);
            }
        } else if (isRuslat) {
            // РУС/LAT: hold F6 for several frames so the ROM can detect it.
            // The ROM scans keyboard periodically; a single-frame pulse
            // may be missed between scans.
            if (ruslatHoldTimer_ <= 0.0f) {
                backend.pressKey(SC_F6);
                ruslatHoldTimer_ = 0.15f; // ~8 frames at 50 Hz
            }
        } else if (isResetKey(hoveredScancode)) {
            // ВВОД/СБР alone do nothing on a real Vector — they reset the
            // machine only while БЛК is held down.
            if (isBlkLatched()) {
                applyResetKey(hoveredScancode, backend);
            }
        } else {
            // Release previous momentary key
            if (pressedScancode_ != 0) {
                backend.releaseKey(pressedScancode_);
            }
            pressedScancode_ = hoveredScancode;
            backend.pressKey(hoveredScancode);
        }
        selectedScancode_ = hoveredScancode;
        selectionTimer_ = 0.15f;
    }

    // Fade selection timer
    if (selectionTimer_ > 0.0f) {
        selectionTimer_ -= ImGui::GetIO().DeltaTime;
        if (selectionTimer_ < 0.0f) selectionTimer_ = 0.0f;
    }

    // Release РУС/LAT (F6) after hold timer expires
    if (ruslatHoldTimer_ > 0.0f) {
        ruslatHoldTimer_ -= ImGui::GetIO().DeltaTime;
        if (ruslatHoldTimer_ <= 0.0f) {
            ruslatHoldTimer_ = 0.0f;
            backend.releaseKey(SC_F6);
        }
    }

    // --- Draw keys ---
    for (const auto &k : keys) {
        if (k.scancode == 0) continue;

        float kx = k.col * kKeyW + pad;
        float ky = k.row * kKeyH + pad;
        float kw = k.w * kKeyW - kGap;
        float kh = k.h * kKeyH - kGap;

        ImVec2 pMin(cursorScreenPos.x + kx, cursorScreenPos.y + ky);
        ImVec2 pMax(cursorScreenPos.x + kx + kw, cursorScreenPos.y + ky + kh);

        bool isPressed = (k.scancode == pressedScancode_) ||
                         (stickyKeys_.count(k.scancode) > 0) ||
                         (activeKeys_.count(k.scancode) > 0);
        bool isSelected = (k.scancode == selectedScancode_) &&
                          (selectionTimer_ > 0.0f);

        // Key background color
        ImU32 bgColor;
        switch (k.color) {
            case KC_ALPHA: bgColor = IM_COL32(0xBB, 0xB5, 0xA5, 255); break;
            case KC_BROWN: bgColor = IM_COL32(0x60, 0x45, 0x26, 255); break;
            case KC_GREEN: bgColor = IM_COL32(0x7E, 0x7F, 0x65, 255); break;
            case KC_FN:    bgColor = IM_COL32(0x9E, 0x97, 0x76, 255); break;
        }

        if (isPressed) {
            // Lighten the key color (mix with white ~30%)
            switch (k.color) {
                case KC_ALPHA: bgColor = IM_COL32(0xD0, 0xCD, 0xC1, 255); break;
                case KC_BROWN: bgColor = IM_COL32(0x8A, 0x6F, 0x52, 255); break;
                case KC_GREEN: bgColor = IM_COL32(0xA0, 0xA1, 0x8E, 255); break;
                case KC_FN:    bgColor = IM_COL32(0xB8, 0xB2, 0x9C, 255); break;
            }
        }

        drawList->AddRectFilled(pMin, pMax, bgColor, 2.0f);

        // Border
        ImU32 borderColor = isSelected ? IM_COL32(0xFF, 0xFF, 0xFF, 255)
                                       : IM_COL32(0x40, 0x40, 0x40, 255);
        drawList->AddRect(pMin, pMax, borderColor, 2.0f);

        // --- Labels ---
        ImU32 textColor = (k.color == KC_BROWN)
            ? IM_COL32(0x80, 0x80, 0x80, 255)
            : IM_COL32(0x00, 0x00, 0x00, 255);

        bool isNumpad = (k.col >= 14.0f);
        bool hasRu = (k.label_ru != nullptr && k.label_ru[0] != '\0');
        bool hasEn = (k.label[0] != '\0');

        if (isNumpad) {
            // Numpad: single centered label
            if (hasEn) {
                ImVec2 textSize = ImGui::CalcTextSize(k.label);
                // Use small font for long labels on narrow keys
                bool useSmall = (textSize.x > kw - 4.0f) && sSmallFont;
                if (useSmall) {
                    ImGui::PushFont(sSmallFont);
                    textSize = ImGui::CalcTextSize(k.label);
                }
                ImVec2 textPos(pMin.x + (kw - textSize.x) * 0.5f,
                               pMin.y + (kh - textSize.y) * 0.5f);
                drawList->AddText(textPos, textColor, k.label);
                if (useSmall) ImGui::PopFont();
            }
        } else if (hasRu && hasEn) {
            // Diagonal layout: Russian top-left, English bottom-right
            const float mx = 2.0f, my = 1.0f;

            // Russian label — top-left
            {
                bool useSmall = sSmallFont &&
                    (ImGui::CalcTextSize(k.label_ru).x > kw - 2.0f * mx);
                if (useSmall) ImGui::PushFont(sSmallFont);
                drawList->AddText(
                    ImVec2(pMin.x + mx, pMin.y + my),
                    textColor, k.label_ru);
                if (useSmall) ImGui::PopFont();
            }

            // English label — bottom-right
            {
                ImVec2 enSize = ImGui::CalcTextSize(k.label);
                drawList->AddText(
                    ImVec2(pMax.x - mx - enSize.x, pMax.y - my - enSize.y),
                    textColor, k.label);
            }
        } else if (hasEn) {
            // Single label centered (no Russian legend)
            ImVec2 textSize = ImGui::CalcTextSize(k.label);
            ImVec2 textPos(pMin.x + (kw - textSize.x) * 0.5f,
                           pMin.y + (kh - textSize.y) * 0.5f);
            drawList->AddText(textPos, textColor, k.label);
        } else if (hasRu) {
            // Russian-only label centered (modifier keys)
            bool useSmall = sSmallFont &&
                (ImGui::CalcTextSize(k.label_ru).x > kw - 4.0f);
            if (useSmall) ImGui::PushFont(sSmallFont);
            ImVec2 textSize = ImGui::CalcTextSize(k.label_ru);
            ImVec2 textPos(pMin.x + (kw - textSize.x) * 0.5f,
                           pMin.y + (kh - textSize.y) * 0.5f);
            drawList->AddText(textPos, textColor, k.label_ru);
            if (useSmall) ImGui::PopFont();
        }
    }

    // РУС/LAT LED indicator
    {
        float ledX = cursorScreenPos.x + pad + 0.25f * kKeyW;
        float ledY = cursorScreenPos.y + pad + 4.0f * kKeyH + kKeyH * 0.7f;
        float ledR = 4.0f;
        bool ruslatOn = backend.isRuslatMode();
        ImU32 ledColor = !ruslatOn
            ? IM_COL32(0xFF, 0x20, 0x20, 255)   // bright red — ON
            : IM_COL32(0x40, 0x10, 0x10, 255);   // dim — OFF
        drawList->AddCircleFilled(ImVec2(ledX, ledY), ledR, ledColor);
    }

    ImGui::End();
}
