// Dear ImGui
#include "imgui.h"
#include "about_window.h"

// SDL2 + OpenGL (texture upload + ImTextureID convention match the screen windows)
#include "SDL.h"
#include "SDL_opengl.h"

// Build provenance / semantic version (single source of truth with the MCP server).
#include "vdb_version.h"

#include <cstdio>
#include <cstdint>

// stb_image — PNG decoder, implementation emitted in exactly this TU.
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// ---------------------------------------------------------------------------
// Layout constants
// ---------------------------------------------------------------------------
namespace {
constexpr float kLogoDisplay  = 128.0f;   // logo drawn at 128 px square
constexpr float kGap          = 24.0f;    // gap between logo and text block
constexpr float kCardPad      = 32.0f;    // inner padding of the splash/about cards

// Fixed card sizes let us center deterministically (an AlwaysAutoResize window
// has no reliable size until the frame after it is laid out, which makes
// per-frame centering jitter). Content is fixed, so a static box fits.
constexpr ImVec2 kSplashCardSize{560.0f, 250.0f};
constexpr ImVec2 kAboutCardSize {560.0f, 350.0f};

// Top-left corner that centers a `size` box over the main viewport.
inline ImVec2 centeredOnViewport(ImVec2 size)
{
    ImGuiViewport *vp = ImGui::GetMainViewport();
    return ImVec2(vp->Pos.x + (vp->Size.x - size.x) * 0.5f,
                  vp->Pos.y + (vp->Size.y - size.y) * 0.5f);
}
}

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

AboutWindow::~AboutWindow()
{
    // The GL context may already be gone at destruction; ImGui_ImplOpenGL2 is
    // shut down before DebuggerGui is torn down, but the texture handle becomes
    // dangling anyway. Only delete while a context is current.
    if (logoTexId_ && SDL_GL_GetCurrentContext()) {
        destroyLogoTexture();
    }
}

// ---------------------------------------------------------------------------
// Mode activation
// ---------------------------------------------------------------------------

void AboutWindow::showSplash(unsigned dwellMs)
{
    splashActive_    = true;
    aboutVisible_    = false;
    splashDwellMs_   = dwellMs;
    splashStartTick_ = SDL_GetTicks();
}

void AboutWindow::showAbout()
{
    aboutVisible_ = true;
}

// ---------------------------------------------------------------------------
// Logo path resolution — mirrors resolveMonoFont(): prefer the copy seeded
// next to the executable (build dir / release bundle), fall back to the source
// tree so a run straight from the repo without the copy still shows the logo.
// ---------------------------------------------------------------------------

std::string AboutWindow::resolveLogoPath()
{
    std::string nextToExe;
    if (const char *base = SDL_GetBasePath()) {
        nextToExe = std::string(base) + "logo16.png";
        SDL_free(const_cast<char *>(base));
    }

    const char *candidates[] = {
        nextToExe.empty() ? nullptr : nextToExe.c_str(),
        "logo16.png",                    // relative to CWD (== exe dir at startup)
        "res/logo16.png",                // running from the source tree root
    };

    for (const char *p : candidates) {
        if (p && *p) {
            if (FILE *f = fopen(p, "rb")) { fclose(f); return p; }
        }
    }
    return std::string();
}

// ---------------------------------------------------------------------------
// Texture upload
// ---------------------------------------------------------------------------

bool AboutWindow::ensureLogoTexture()
{
    if (logoTexId_) return true;
    if (logoTried_) return false;
    logoTried_ = true;

    const std::string path = resolveLogoPath();
    if (path.empty()) return false;

    int comp = 0;
    // Force 4 channels (RGBA); stb converts the 8-bit paletted PNG for us.
    unsigned char *px = stbi_load(path.c_str(), &logoW_, &logoH_, &comp, 4);
    if (!px || logoW_ <= 0 || logoH_ <= 0) {
        if (px) stbi_image_free(px);
        return false;
    }

    glGenTextures(1, &logoTexId_);
    glBindTexture(GL_TEXTURE_2D, logoTexId_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // stb returns top-down RGBA; ImGui's OpenGL2 backend maps uv (0,0) to the
    // top-left, matching the screen windows — so no vertical flip is needed.
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, logoW_, logoH_, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, px);

    stbi_image_free(px);
    return true;
}

void AboutWindow::destroyLogoTexture()
{
    if (logoTexId_) {
        glDeleteTextures(1, &logoTexId_);
        logoTexId_ = 0;
    }
}

// ---------------------------------------------------------------------------
// Shared content block: logo (left) + application data (right).
// ---------------------------------------------------------------------------

void AboutWindow::drawContent()
{
    const ImU32 accent  = IM_COL32(120, 200, 255, 255);

    // --- Logo ---
    ImGui::BeginGroup();
    if (ensureLogoTexture()) {
        ImGui::Image(static_cast<ImTextureID>(static_cast<uint64_t>(logoTexId_)),
                     ImVec2(kLogoDisplay, kLogoDisplay));
    } else {
        // Placeholder square when the PNG is unavailable, so the layout and
        // the About dialog still look intentional rather than empty.
        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(p, ImVec2(p.x + kLogoDisplay, p.y + kLogoDisplay),
                          IM_COL32(40, 46, 60, 255));
        dl->AddRect(p, ImVec2(p.x + kLogoDisplay, p.y + kLogoDisplay), accent);
        ImGui::Dummy(ImVec2(kLogoDisplay, kLogoDisplay));
    }
    ImGui::EndGroup();
    ImGui::SameLine(0.0f, kGap);
    // --- Application data ---
    ImGui::BeginGroup();
    const float textW = 360.0f;

    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + textW);

    ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "Debugger for Vector-06C");
#ifdef _WIN32
    ImGui::TextColored(ImVec4(0.75f, 0.78f, 0.85f, 1.0f), "Version for Windows");
#else
    ImGui::TextColored(ImVec4(0.75f, 0.78f, 0.85f, 1.0f), "Version for Linux");
#endif
    ImGui::TextColored(ImVec4(0.75f, 0.78f, 0.85f, 1.0f), "%s", VDB_VERSION);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "Developer: Sarmin Alexey");

    // Clickable URL: accent-colored text, hand cursor and SDL_OpenURL on click.
    auto renderLink = [](const char *url) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.47f, 0.78f, 1.0f, 1.0f));
        ImGui::TextUnformatted(url);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                SDL_OpenURL(url);
            }
        }
    };
    renderLink("https://sarmin.ru");

    ImGui::Spacing();
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "Emulator engine by svofski:");
    renderLink("https://github.com/svofski/vector06sdl");

    ImGui::PopTextWrapPos();
    ImGui::EndGroup();

    (void)accent;
}

// ---------------------------------------------------------------------------
// Per-frame render (drawn last ⇒ top-most)
// ---------------------------------------------------------------------------

void AboutWindow::render()
{
    // --- Splash overlay ---
    if (splashActive_) {
        const uint32_t now = SDL_GetTicks();
        const bool expired = (now - splashStartTick_) >= splashDwellMs_;
        const bool dismissed = expired ||
                               ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
                               ImGui::IsKeyPressed(ImGuiKey_Escape);

        if (dismissed) {
            splashActive_ = false;
        } else {
            ImGuiViewport *vp = ImGui::GetMainViewport();

            // Dim the entire viewport behind the card.
            ImGui::GetBackgroundDrawList()->AddRectFilled(
                vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                IM_COL32(0, 0, 0, 170));

            ImGuiWindowFlags cardFlags =
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings;

            ImGui::SetNextWindowPos(centeredOnViewport(kSplashCardSize), ImGuiCond_Always);
            ImGui::SetNextWindowSize(kSplashCardSize, ImGuiCond_Always);
            ImGui::SetNextWindowFocus();
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kCardPad, kCardPad));
            ImGui::Begin("##SplashCard", nullptr, cardFlags);
            drawContent();
            ImGui::End();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }
    }

    // --- About window (File ▸ About) ---
    if (aboutVisible_) {
        ImGui::SetNextWindowPos(centeredOnViewport(kAboutCardSize), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(kAboutCardSize, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowFocus();

        ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDocking |
            ImGuiWindowFlags_NoCollapse;

        // Opaque black background (the default dark theme's WindowBg has a
        // slight alpha, which reads as translucency over the panels behind it).
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kCardPad, kCardPad));
        if (ImGui::Begin("About vector-debugger", &aboutVisible_, flags)) {
            drawContent();
            ImGui::Spacing();
            ImGui::Spacing();
            // Center the Close button under the content.
            float btnW = ImGui::CalcTextSize("Close").x + ImGui::GetStyle().FramePadding.x * 2.0f;
            ImGui::SetCursorPosX((ImGui::GetWindowSize().x - btnW) * 0.5f);
            if (ImGui::Button("Close")) {
                aboutVisible_ = false;
            }
        }
        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }
}
