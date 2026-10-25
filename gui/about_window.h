#pragma once

#include <cstdint>
#include <string>

// ---------------------------------------------------------------------------
// AboutWindow — application splash + "About" panel for v06c-debugger.
//
// One class serves two modes that share the same logo + info layout:
//
//   * Splash mode  (showSplash)  — a borderless, top-most overlay drawn on the
//     very first frames after the GUI comes up. It stays on screen while the
//     main window finishes initializing and auto-dismisses after a short dwell
//     (or on any click / key press, whichever comes first). No Close button —
//     it is purely branding shown "while loading".
//
//   * About mode   (showAbout)   — the same content in a normal titled window
//     opened from File ▸ About, with a Close button. Persistent until closed.
//
// render() must be called once per frame at the END of DebuggerGui::render()
// so the overlay is drawn last and therefore sits above every docked panel.
//
// Layout: logo on the left (res/logo16.png, decoded with stb_image and uploaded
// as an OpenGL texture — the same ImTextureID convention the screen windows
// use), application data on the right.
// ---------------------------------------------------------------------------

class AboutWindow {
public:
    AboutWindow() = default;
    ~AboutWindow();

    // Activate the startup splash. dwellMs = minimum time the splash remains
    // visible before it auto-dismisses (a click/keypress ends it sooner).
    void showSplash(unsigned dwellMs = 2200);

    // Open the About panel (File ▸ About). Re-opens / focuses if already up.
    void showAbout();

    // Draw whichever mode is active. Call last in the frame's render pass.
    void render();

private:
    // Shared logo + info block (no Close button here; the caller adds it).
    void drawContent();

    // Lazily decode the logo PNG into an OpenGL texture once a GL context is
    // current. logoTried_ guards against retrying every frame after a failure.
    bool ensureLogoTexture();
    void destroyLogoTexture();

    static std::string resolveLogoPath();

    // Splash state.
    bool     splashActive_    = false;
    uint32_t splashStartTick_ = 0;
    unsigned splashDwellMs_   = 2200;

    // About state.
    bool aboutVisible_ = false;

    // Logo texture (OpenGL). logoTexId_ == 0 means "not loaded / unavailable",
    // in which case a placeholder rectangle is drawn instead.
    unsigned logoTexId_  = 0;   // GLuint
    int      logoW_      = 0;
    int      logoH_      = 0;
    bool     logoTried_  = false;
};
