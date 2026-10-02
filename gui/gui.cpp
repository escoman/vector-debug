#include "gui.h"

// Dear ImGui core + backends
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl2.h"

// SDL2
#include "SDL.h"
#include "SDL_opengl.h"

// Debugger backend
#include "backend.h"
#include "events.h"
#include "disassembler.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <string>
#include <algorithm>
#include <chrono>
#include <thread>
#include <sys/stat.h>   // mkdir() for the "Install .desktop" action

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

DebuggerGui::~DebuggerGui()
{
    shutdown();
}

bool DebuggerGui::initialize(int width, int height)
{
    // SDL init (video + events)
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    // GL 2.1 context
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);

    window_ = SDL_CreateWindow(
        "Vector-06C Debugger",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        width, height,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window_) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return false;
    }

    // Maximize after creation — ImGui viewport init may override the
    // SDL_WINDOW_MAXIMIZED create flag, so we maximize explicitly.
    SDL_MaximizeWindow(window_);

    glContext_ = SDL_GL_CreateContext(window_);
    if (!glContext_) {
        std::fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window_);
        SDL_Quit();
        return false;
    }

    SDL_GL_MakeCurrent(window_, glContext_);
    SDL_GL_SetSwapInterval(1);  // vsync

    // ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // Style
    ImGui::StyleColorsDark();

    // Load monospaced font with Unicode support for button icons
    io.Fonts->AddFontFromFileTTF(
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 15.0f);

    // Small monospaced font for long keyboard legends (ВВОД, БЛК, etc.)
    KeyboardWindow::sSmallFont = io.Fonts->AddFontFromFileTTF(
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 11.0f);

    // Backend init
    ImGui_ImplSDL2_InitForOpenGL(window_, glContext_);
    ImGui_ImplOpenGL2_Init();

    // Workspace Manager (Stage 5.1) — directory setup only;
    // visibility refs and actual loading happen after windows are created
    //
    // Allow override via environment variable so that GUI smoke tests
    // don't clobber the user's real workspace presets.
    std::string wsDir = "workspaces";
    if (const char *env = std::getenv("V06C_WORKSPACE_DIR")) {
        wsDir = env;
    }
    workspaceManager_.initialize(wsDir);

    // ConfigManager — app-level settings (config.ini next to executable)
    // Uses current directory (where the executable lives), not workspaces/.
    std::string configDir = ".";
    if (const char *env = std::getenv("V06C_CONFIG_DIR")) {
        configDir = env;
    }
    configManager_.initialize(configDir);

    return true;
}

void DebuggerGui::shutdown()
{
    if (!window_) return;

    // Save config (Recent ROMs, etc.) on exit
    configManager_.shutdown();

    // Do NOT auto-save workspace on exit — only explicit Save action.

    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_GL_DeleteContext(glContext_);
    SDL_DestroyWindow(window_);
    SDL_Quit();

    glContext_ = nullptr;
    window_    = nullptr;
}

// ---------------------------------------------------------------------------
// Frame management
// ---------------------------------------------------------------------------

void DebuggerGui::beginFrame(IDebugBackend &backend)
{
    const bool kbdLocked = keyboardWindow_.isLocked();

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        // When keyboard lock is active, forward key events to the emulator
        // and block them from ImGui — the user's keyboard is "captured" by
        // the emulated Vector-06C.
        if (kbdLocked && (event.type == SDL_KEYDOWN ||
                          event.type == SDL_KEYUP ||
                          event.type == SDL_TEXTINPUT)) {
            keyboardWindow_.handleSdlEvent(event, backend);
            continue;  // do NOT pass to ImGui
        }

        ImGui_ImplSDL2_ProcessEvent(&event);
        if (event.type == SDL_QUIT) {
            quit_ = true;
        }
        if (event.type == SDL_WINDOWEVENT &&
            event.window.event == SDL_WINDOWEVENT_CLOSE &&
            event.window.windowID == SDL_GetWindowID(window_)) {
            quit_ = true;
        }
    }

    ImGui_ImplOpenGL2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
}

void DebuggerGui::endFrame()
{
    ImGui::Render();
    glClearColor(0.10f, 0.10f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(window_);
}

bool DebuggerGui::shouldQuit() const
{
    return quit_;
}

void DebuggerGui::applyPendingWorkspace()
{
    workspaceManager_.applyPendingWorkspace();
    workspaceManager_.processDeferredOps();
}

// ---------------------------------------------------------------------------
// Central Navigation API (Stage 3.9)
// ---------------------------------------------------------------------------

void DebuggerGui::gotoMemory(uint16_t address)
{
    memoryInspector_.setVisible(true);
    memoryInspector_.gotoAddress(address);
}

void DebuggerGui::gotoDisassembly(uint16_t address)
{
    disassemblyView_.setVisible(true);
    disassemblyView_.gotoAddress(address);
}

void DebuggerGui::gotoStack(uint16_t address)
{
    stackView_.setVisible(true);
    stackView_.gotoAddress(address);
}

// ---------------------------------------------------------------------------
// Post-reset refresh — every view re-reads the machine state
// ---------------------------------------------------------------------------

void DebuggerGui::refreshAfterReset()
{
    memoryInspector_.requestRefresh();
    stackView_.requestRefresh();
    disassemblyView_.requestRefresh();
    executionTrace_.requestRefresh();
    ioInspector_.requestRefresh();
    vectorScreen_.requestRefresh();
    functionsWindow_.requestRefresh();
    romDatabaseWindow_.requestRefresh();
    xrefsWindow_.requestRefresh();
    histNeedsRefresh_ = true;
}

// ---------------------------------------------------------------------------
// Main render — assembles all panels
// ---------------------------------------------------------------------------

void DebuggerGui::render(IDebugBackend &backend)
{
    ImGuiViewport *viewport = ImGui::GetMainViewport();

    // --- Hotkeys (debug control) ---
    if (!keyboardWindow_.isLocked()) {
        bool hotkeyPressed = false;

        if (ImGui::IsKeyPressed(ImGuiKey_F3)) {
            backend.requestPause();
            hotkeyPressed = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F4)) {
            backend.requestStep();
            hotkeyPressed = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F5)) {
            backend.requestRun();
            hotkeyPressed = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F11)) {
            backend.requestReset();
            currentRomName_ = "BOOT";
            hotkeyPressed = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F12)) {
            backend.requestRestart();
            currentRomName_ = "BOOT";
            hotkeyPressed = true;
        }

        if (hotkeyPressed) {
            refreshAfterReset();
        }
    }

    // --- Main menu bar (rendered first, on top of everything) ---
    renderToolbar(backend);

    // --- Calculate work area (between menu bar and status bar) ---
    // viewport->WorkPos already starts below the main menu bar.
    float statusBarHeight = ImGui::GetFrameHeightWithSpacing();
    ImVec2 workPos = viewport->WorkPos;
    ImVec2 workSize = viewport->WorkSize;
    workSize.y -= statusBarHeight;

    // --- DockSpace (Stage 5.0/5.1) ---
    // Fills the work area between menu bar and status bar.
    ImGui::SetNextWindowPos(workPos);
    ImGui::SetNextWindowSize(workSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags hostFlags = 0;
    hostFlags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##DockSpaceHost", nullptr, hostFlags);
    mainDockId_ = ImGui::GetID("MainDock");
    ImGui::DockSpace(mainDockId_, ImVec2(0, 0),
        ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::End();
    ImGui::PopStyleVar();

    // Get CPU state for this frame
    CpuState cpu = backend.getCpuState();

    // --- CPU Registers window (dockable) ---
    if (showCpuRegisters_) {
        ImGui::SetNextWindowSize(ImVec2(280, 0), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("CPU Registers", &showCpuRegisters_)) {
            renderCpuPanel(backend);
            ImGui::Separator();
            renderCurrentInstruction(cpu.pc, backend);
        }
        ImGui::End();
    }

    // --- Instruction History window (dockable) ---
    if (showInstructionHistory_) {
        ImGui::SetNextWindowSize(ImVec2(400, 300), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Instruction History", &showInstructionHistory_)) {
            renderInstructionHistory(backend);
        }
        ImGui::End();
    }

    // --- Open ROM dialog (custom, non-blocking) ---
    if (showOpenRomDialog_) {
        showOpenRomDialog_ = false;
        std::string lastDir = configManager_.get("LastRomDirectory");
        romFileDialog_.onFileSelected = [this, &backend](const std::string &path) {
            // Save the directory of the selected ROM
            size_t lastSlash = path.rfind('/');
            if (lastSlash != std::string::npos) {
                configManager_.set("LastRomDirectory",
                    path.substr(0, lastSlash));
            }
            loadRomFile(path, backend);
        };
        romFileDialog_.show(lastDir);
    }

    // --- Open WAV dialog (custom, non-blocking) ---
    if (showOpenWavDialog_) {
        showOpenWavDialog_ = false;
        std::string lastDir = configManager_.get("LastRomDirectory");
        romFileDialog_.onFileSelected = [this, &backend](const std::string &path) {
            // Save the directory of the selected WAV
            size_t lastSlash = path.rfind('/');
            if (lastSlash != std::string::npos) {
                configManager_.set("LastRomDirectory",
                    path.substr(0, lastSlash));
            }
            loadWavFile(path, backend);
        };
        romFileDialog_.show(lastDir, "Open WAV File", {".wav"});
    }
    
    // Render the ROM file dialog if open
    romFileDialog_.render();
    if (romErrorBuffer_[0]) {
        ImGui::OpenPopup("ROM Error");
    }
    if (ImGui::BeginPopupModal("ROM Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", romErrorBuffer_);
        if (ImGui::Button("OK")) {
            romErrorBuffer_[0] = '\0';
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (wavErrorBuffer_[0]) {
        ImGui::OpenPopup("WAV Error");
    }
    if (ImGui::BeginPopupModal("WAV Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", wavErrorBuffer_);
        if (ImGui::Button("OK")) {
            wavErrorBuffer_[0] = '\0';
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // "Install .desktop" result (success or error), same modal pattern.
    if (desktopMsgBuffer_[0]) {
        ImGui::OpenPopup("Install .desktop");
    }
    if (ImGui::BeginPopupModal("Install .desktop", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(desktopMsgError_ ? ImVec4(1.0f, 0.3f, 0.3f, 1.0f)
                                            : ImVec4(0.3f, 1.0f, 0.4f, 1.0f),
                           "%s", desktopMsgBuffer_);
        if (ImGui::Button("OK")) {
            desktopMsgBuffer_[0] = '\0';
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Setup cross-window navigation callbacks (Stage 3.9)
    stackView_.onGoToMemoryInspector = [this](uint16_t a) { gotoMemory(a); };
    stackView_.onGoToDisassembly = [this](uint16_t a) { gotoDisassembly(a); };
    
    memoryInspector_.onGoToDisassembly = [this](uint16_t a) { gotoDisassembly(a); };
    
    disassemblyView_.onGoToMemoryInspector = [this](uint16_t a) { gotoMemory(a); };
    
    breakpointsWindow_.onGoToDisassembly = [this](uint16_t a) { gotoDisassembly(a); };
    breakpointsWindow_.onGoToMemoryInspector = [this](uint16_t a) { gotoMemory(a); };
    
    executionTrace_.onGoToDisassembly = [this](uint16_t a) { gotoDisassembly(a); };
    executionTrace_.onGoToMemoryInspector = [this](uint16_t a) { gotoMemory(a); };
    
    ioInspector_.onGoToDisassembly = [this](uint16_t a) { gotoDisassembly(a); };
    ioInspector_.onGoToMemoryInspector = [this](uint16_t a) { gotoMemory(a); };
    
    memoryMap_.onGoToDisassembly = [this](uint16_t a) { gotoDisassembly(a); };
    memoryMap_.onGoToMemoryInspector = [this](uint16_t a) { gotoMemory(a); };

    // Stage 6.25: Memory Access window — same navigation callbacks plus a
    // cross-window query to seed its range from the currently hovered /
    // selected block in the Memory Map.
    memoryAccess_.onGoToDisassembly = [this](uint16_t a) { gotoDisassembly(a); };
    memoryAccess_.onGoToMemoryInspector = [this](uint16_t a) { gotoMemory(a); };
    memoryAccess_.getSelectedMapBlock = [this](uint16_t &out) -> bool {
        return memoryMap_.getSelectedBlockAddress(out);
    };

    // Stage 6.25: Memory Map context menu item "To Memory Access" pushes
    // the block's range into the Memory Access window and focuses it.
    memoryMap_.onGoToMemoryAccess = [this](uint16_t a) {
        memoryAccess_.focusOnBlock(a);
    };
    
    functionsWindow_.onGoToDisassembly = [this](uint16_t a) { gotoDisassembly(a); };
    functionsWindow_.onGoToMemoryInspector = [this](uint16_t a) { gotoMemory(a); };
    
    romDatabaseWindow_.onGoToDisassembly = [this](uint16_t a) { gotoDisassembly(a); };
    romDatabaseWindow_.onGoToMemoryInspector = [this](uint16_t a) { gotoMemory(a); };
    
    xrefsWindow_.onGoToDisassembly = [this](uint16_t a) { gotoDisassembly(a); };
    
    searchWindow_.onGoToDisassembly = [this](uint16_t a) { gotoDisassembly(a); };
    searchWindow_.onGoToMemoryInspector = [this](uint16_t a) { gotoMemory(a); };
    
    // Stage 4 Enhanced: Vector Screen navigation callbacks
    vectorScreen_.onGoToMemoryInspector = [this](uint16_t a) { gotoMemory(a); };
    vectorScreen_.onGoToDisassembly = [this](uint16_t a) { gotoDisassembly(a); };

    // --- Apply cascade layout BEFORE rendering windows ---
    if (cascadeRequested_) {
        applyCascade();
    }

    // Render all debugger windows (dockable)
    memoryInspector_.render(backend);
    stackView_.render(backend);
    breakpointsWindow_.render(backend);
    disassemblyView_.render(backend);
    executionTrace_.render(backend);
    ioInspector_.render(backend);
    vectorScreen_.render(backend);
    memoryMap_.render(backend);
    memoryAccess_.render(backend);
    functionsWindow_.render(backend);
    romDatabaseWindow_.render(backend);
    xrefsWindow_.render(backend);
    searchWindow_.render(backend);
    keyboardWindow_.render(backend);
    // A virtual ВВОД/СБР key resets the machine — same follow-up as the
    // F11/F12 hotkeys.
    if (keyboardWindow_.consumeResetPerformed()) {
        currentRomName_ = "BOOT";
        refreshAfterReset();
    }
    soundWindow_.render(backend);
    planeScreen_.render(backend);

    // Register visibility refs on first frame (triggers workspace loading)
    static bool visRegsRegistered = false;
    if (!visRegsRegistered) {
        workspaceManager_.setWindowVisibilityRefs({
            {"CPU Registers", &showCpuRegisters_},
            {"Instruction History", &showInstructionHistory_},
            {"Vector Screen", &vectorScreen_.getVisibleRef()},
            {"Memory Inspector", &memoryInspector_.getVisibleRef()},
            {"Memory Map", &memoryMap_.getVisibleRef()},
            {"Memory Access", &memoryAccess_.getVisibleRef()},
            {"Disassembly", &disassemblyView_.getVisibleRef()},
            {"Stack View", &stackView_.getVisibleRef()},
            {"Breakpoints", &breakpointsWindow_.getVisibleRef()},
            {"Execution Trace", &executionTrace_.getVisibleRef()},
            {"I/O & Hardware Inspector", &ioInspector_.getVisibleRef()},
            {"MAP-file Info", &functionsWindow_.getVisibleRef()},
            {"ROM Database", &romDatabaseWindow_.getVisibleRef()},
            {"Cross References", &xrefsWindow_.getVisibleRef()},
            {"Search", &searchWindow_.getVisibleRef()},
            {"Keyboard", &keyboardWindow_.getVisibleRef()},
            {"Sound", &soundWindow_.getVisibleRef()},
            {"VRAM Planes", &planeScreen_.getVisibleRef()},
        });
        visRegsRegistered = true;
    }

    // Autosave workspace if layout changed
    workspaceManager_.autosave();

    // Autosave config (Recent ROMs, etc.)
    configManager_.autosave();

    // --- Save As dialog ---
    if (showSaveAsDialog_) {
        ImGui::OpenPopup("Save Workspace As");
        showSaveAsDialog_ = false;
    }
    if (ImGui::BeginPopupModal("Save Workspace As", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Workspace name:");
        ImGui::InputText("##name", saveAsNameBuffer_, sizeof(saveAsNameBuffer_));
        if (ImGui::Button("Save") || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
            if (saveAsNameBuffer_[0] != '\0') {
                workspaceManager_.saveWorkspaceAs(saveAsNameBuffer_);
                saveAsNameBuffer_[0] = '\0';
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            saveAsNameBuffer_[0] = '\0';
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // --- Delete confirmation ---
    if (showDeleteConfirm_) {
        ImGui::OpenPopup("Delete Workspace?");
        showDeleteConfirm_ = false;
    }
    if (ImGui::BeginPopupModal("Delete Workspace?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete workspace '%s'?", workspaceManager_.currentWorkspaceName().c_str());
        if (workspaceManager_.isBuiltIn(workspaceManager_.currentWorkspaceName())) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                               "Cannot delete built-in workspace.");
            if (ImGui::Button("OK")) { ImGui::CloseCurrentPopup(); }
        } else {
            if (ImGui::Button("Delete")) {
                workspaceManager_.deleteWorkspace(workspaceManager_.currentWorkspaceName());
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) { ImGui::CloseCurrentPopup(); }
        }
        ImGui::EndPopup();
    }

    // --- Status bar (fixed overlay at bottom) ---
    {
        ImGui::SetNextWindowPos(ImVec2(workPos.x, workPos.y + workSize.y));
        ImGui::SetNextWindowSize(ImVec2(workSize.x, statusBarHeight));
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNavFocus |
            ImGuiWindowFlags_NoDocking;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("##StatusBar", nullptr, flags);
        ImGui::PopStyleVar(2);
        renderStatusBar(backend);
        ImGui::End();
    }
}

// ---------------------------------------------------------------------------
// CPU Panel
// ---------------------------------------------------------------------------

void DebuggerGui::renderCpuPanel(IDebugBackend &backend)
{
    CpuState s = backend.getCpuState();
    
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "CPU");
    ImGui::Spacing();
    
    // Register editing mode (Stage 3.6)
    if (editingRegister_) {
        const char *regNames[] = { "AF", "BC", "DE", "HL", "SP", "PC" };
        int idx = static_cast<int>(editingRegId_);
        ImGui::Text("Edit %s:", regNames[idx]);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        bool enterPressed = ImGui::InputText("##editreg", editRegBuffer_, sizeof(editRegBuffer_),
            ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue |
            ImGuiInputTextFlags_AutoSelectAll);
        ImGui::SameLine();
        if (ImGui::Button("Go") || enterPressed) {
            unsigned int value = 0;
            if (sscanf(editRegBuffer_, "%x", &value) == 1 && value <= 0xFFFF) {
                bool ok = backend.writeRegister(editingRegId_, static_cast<uint16_t>(value));
                if (ok) {
                    editingRegister_ = false;
                    writeRegFailed_ = false;
                } else {
                    writeRegFailed_ = true;
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            editingRegister_ = false;
            writeRegFailed_ = false;
        }
        if (writeRegFailed_) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Write failed: CPU must be Paused");
        }
        ImGui::Separator();
        return;  // Skip normal display while editing
    }
    
    writeRegFailed_ = false;
    
    // Helper lambda: render editable register row (Stage 3.6)
    auto renderRegRow = [&](const char *name, uint16_t value, IDebugBackend::RegisterId regId) {
        ImGui::Text("%s   %04X", name, value);
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            editingRegister_ = true;
            editingRegId_ = regId;
            snprintf(editRegBuffer_, sizeof(editRegBuffer_), "%04X", value);
        }
    };
    
    // 16-bit register pairs
    uint16_t af = (static_cast<uint16_t>(s.a) << 8) | s.flags;
    uint16_t bc = (static_cast<uint16_t>(s.b) << 8) | s.c;
    uint16_t de = (static_cast<uint16_t>(s.d) << 8) | s.e;
    uint16_t hl = (static_cast<uint16_t>(s.h) << 8) | s.l;
    
    renderRegRow("PC", s.pc, IDebugBackend::RegisterId::PC);
    
    // Stage 3.9: PC context menu for navigation
    if (ImGui::BeginPopupContextItem("pc_ctx")) {
        if (ImGui::MenuItem("Go to Disassembly")) {
            gotoDisassembly(s.pc);
        }
        if (ImGui::MenuItem("Go to Memory Inspector")) {
            gotoMemory(s.pc);
        }
        ImGui::EndPopup();
    }
    
    renderRegRow("AF", af,  IDebugBackend::RegisterId::AF);
    renderRegRow("BC", bc,  IDebugBackend::RegisterId::BC);
    renderRegRow("DE", de,  IDebugBackend::RegisterId::DE);
    renderRegRow("HL", hl,  IDebugBackend::RegisterId::HL);
    renderRegRow("SP", s.sp, IDebugBackend::RegisterId::SP);
    
    // Stage 3.9: SP context menu for navigation
    if (ImGui::BeginPopupContextItem("sp_ctx")) {
        if (ImGui::MenuItem("Go to Stack")) {
            gotoStack(s.sp);
        }
        if (ImGui::MenuItem("Go to Memory Inspector")) {
            gotoMemory(s.sp);
        }
        ImGui::EndPopup();
    }
    
    // 8-bit components (read-only)
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
        "A=%02X  B=%02X C=%02X  D=%02X E=%02X  H=%02X L=%02X",
        s.a, s.b, s.c, s.d, s.e, s.h, s.l);
    
    // Flags
    ImGui::Spacing();
    uint8_t f = s.flags;
    bool flagS  = (f >> 7) & 1;
    bool flagZ  = (f >> 6) & 1;
    bool flagAC = (f >> 4) & 1;
    bool flagP  = (f >> 2) & 1;
    bool flagCY = f & 1;
    
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Flags");
    ImGui::Text("S  Z  AC P  CY");
    ImGui::Text("%d  %d   %d  %d   %d",
        flagS ? 1 : 0, flagZ ? 1 : 0, flagAC ? 1 : 0,
        flagP ? 1 : 0, flagCY ? 1 : 0);
    
    // Additional CPU state (read-only)
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "State");
    ImGui::Text("IFF:        %d", s.iff ? 1 : 0);
    // Not exposed by the current emulator core.
    ImGui::Text("EI pending: N/A");
    ImGui::Text("Cycles:     %u", s.cycles);
    ImGui::Text("Last PC:    %04X", s.last_pc);
}

// ---------------------------------------------------------------------------
// Current Instruction
// ---------------------------------------------------------------------------

void DebuggerGui::renderCurrentInstruction(uint16_t pc, IDebugBackend &backend)
{
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Current Instruction");
    ImGui::Spacing();

    // Use the disassembler with backend.readMemory() as the read function.
    DisasmReadFn readFn = [&backend](uint16_t addr) -> uint8_t {
        return backend.readMemory(addr);
    };

    DisassembledInstruction instr = disassemble(pc, readFn);
    ImGui::Text("%04X: %s", pc, instr.text.c_str());
}

// ---------------------------------------------------------------------------
// Instruction History
// ---------------------------------------------------------------------------

void DebuggerGui::renderInstructionHistory(IDebugBackend &backend)
{
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Instruction History");
    ImGui::Spacing();

    // Stage 3.12: refresh cache only when needed (avoid per-frame snapshot)
    if (histNeedsRefresh_) {
        cachedHistEntries_ = backend.instructionHistorySnapshot();
        histNeedsRefresh_ = false;
    }

    if (cachedHistEntries_.empty()) {
        ImGui::TextDisabled("(no instructions executed)");
        return;
    }

    // Show last N instructions (scrollable region).
    ImGui::BeginChild("HistoryScroll", ImVec2(0, 0), ImGuiChildFlags_None,
                       ImGuiWindowFlags_None);

    for (size_t i = 0; i < cachedHistEntries_.size(); ++i) {
        const auto &ev = cachedHistEntries_[i];
        bool isLast = (i == cachedHistEntries_.size() - 1);

        if (isLast) {
            // Highlight the most recent instruction.
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.4f, 1.0f));
        }

        ImGui::Text("%04X  %02X  seq=%llu",
                     ev.pcBefore, ev.opcode,
                     (unsigned long long)ev.sequence);

        if (isLast) {
            ImGui::PopStyleColor();
            // Auto-scroll to the last item.
            ImGui::SetScrollHereY(1.0f);
        }
    }

    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Toolbar — menu bar with File, View menus and debug controls (Stage 5.0)
// ---------------------------------------------------------------------------

void DebuggerGui::renderToolbar(IDebugBackend &backend)
{
    if (ImGui::BeginMainMenuBar())
    {
        // File menu
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open ROM...")) {
                showOpenRomDialog_ = true;
                romErrorBuffer_[0] = '\0';
            }
            if (ImGui::MenuItem("Open WAV...")) {
                showOpenWavDialog_ = true;
                wavErrorBuffer_[0] = '\0';
            }
            // Recent ROMs submenu
            const auto &recentRoms = configManager_.getRecentRoms();
            if (!recentRoms.empty()) {
                ImGui::Separator();
                for (size_t i = 0; i < recentRoms.size(); ++i) {
                    // Extract filename from full path
                    std::string displayName = recentRoms[i];
                    size_t lastSlash = displayName.rfind('/');
                    if (lastSlash != std::string::npos) {
                        displayName = displayName.substr(lastSlash + 1);
                    }
                    char label[512];
                    snprintf(label, sizeof(label), "%d. %s", (int)(i + 1),
                             displayName.c_str());
                    if (ImGui::MenuItem(label)) {
                        loadRomFile(recentRoms[i], backend);
                    }
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Install .desktop")) {
                installDesktopFile();
            }
            if (ImGui::MenuItem("Exit")) {
                quit_ = true;
            }
            ImGui::EndMenu();
        }

        // View menu — window visibility toggles + layout
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("CPU Registers", nullptr, &showCpuRegisters_);
            ImGui::MenuItem("Instruction History", nullptr, &showInstructionHistory_);
            ImGui::Separator();
            ImGui::MenuItem("Vector Screen", nullptr, &vectorScreen_.getVisibleRef());
            ImGui::MenuItem("Memory Inspector", nullptr, &memoryInspector_.getVisibleRef());
            ImGui::MenuItem("Memory Map", nullptr, &memoryMap_.getVisibleRef());
            ImGui::MenuItem("Memory Access", nullptr, &memoryAccess_.getVisibleRef());
            ImGui::MenuItem("Disassembly", nullptr, &disassemblyView_.getVisibleRef());
            ImGui::MenuItem("Stack", nullptr, &stackView_.getVisibleRef());
            ImGui::MenuItem("Breakpoints", nullptr, &breakpointsWindow_.getVisibleRef());
            ImGui::MenuItem("Execution Trace", nullptr, &executionTrace_.getVisibleRef());
            ImGui::MenuItem("I/O & Hardware Inspector", nullptr, &ioInspector_.getVisibleRef());
            ImGui::MenuItem("MAP-file Info", nullptr, &functionsWindow_.getVisibleRef());
            ImGui::MenuItem("ROM Database", nullptr, &romDatabaseWindow_.getVisibleRef());
            ImGui::MenuItem("Cross References", nullptr, &xrefsWindow_.getVisibleRef());
            ImGui::MenuItem("Search", nullptr, &searchWindow_.getVisibleRef());
            ImGui::MenuItem("Keyboard", nullptr, &keyboardWindow_.getVisibleRef());
            ImGui::MenuItem("Sound", nullptr, &soundWindow_.getVisibleRef());
            ImGui::MenuItem("VRAM Planes", nullptr, &planeScreen_.getVisibleRef());
            ImGui::Separator();
            if (ImGui::MenuItem("Cascade")) {
                layoutCascade();
            }
            if (ImGui::MenuItem("Tile")) {
                layoutTile();
            }
            ImGui::EndMenu();
        }

        // Workspace menu (Stage 5.1)
        if (ImGui::BeginMenu("Workspace")) {
            auto workspaces = workspaceManager_.listWorkspaces();
            for (const auto &ws : workspaces) {
                bool isCurrent = (ws == workspaceManager_.currentWorkspaceName());
                if (ImGui::MenuItem(ws.c_str(), nullptr, isCurrent)) {
                    workspaceManager_.switchWorkspace(ws);
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Save")) {
                workspaceManager_.saveCurrentWorkspace();
            }
            if (ImGui::MenuItem("Save As...")) {
                snprintf(saveAsNameBuffer_, sizeof(saveAsNameBuffer_), "%s",
                         workspaceManager_.currentWorkspaceName().c_str());
                showSaveAsDialog_ = true;
            }
            if (ImGui::MenuItem("Delete")) {
                showDeleteConfirm_ = true;
            }
            if (ImGui::MenuItem("Reset")) {
                workspaceManager_.resetWorkspace();
            }
            ImGui::EndMenu();
        }

        // Debug controls on the right side
        float romLabelWidth = currentRomName_.empty() ? 0.0f :
            ImGui::CalcTextSize(currentRomName_.c_str()).x + ImGui::GetStyle().ItemSpacing.x;
        float controlsWidth = 480.0f + romLabelWidth;
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - controlsWidth);
        renderControls(backend);

        ImGui::EndMainMenuBar();
    }
}

// ---------------------------------------------------------------------------
// Controls — Run / Pause / Step
// ---------------------------------------------------------------------------

void DebuggerGui::renderControls(IDebugBackend &backend)
{
    // Show currently loaded ROM name
    if (!currentRomName_.empty()) {
        ImGui::TextDisabled("%s", currentRomName_.c_str());
        ImGui::SameLine();
    }

    DebuggerState state = backend.getState();
    bool paused = (state == DebuggerState::Paused);
    bool running = (state == DebuggerState::Running);

    // Step: enabled only when paused
    if (!paused) ImGui::BeginDisabled();
    if (ImGui::Button("\xe2\x96\xba Step (F4)")) {  // ► Step (F4)
        backend.requestStep();
        memoryInspector_.requestRefresh();
        stackView_.requestRefresh();
        disassemblyView_.requestRefresh();
        executionTrace_.requestRefresh();
        ioInspector_.requestRefresh();
        vectorScreen_.requestRefresh();
        functionsWindow_.requestRefresh();
        xrefsWindow_.requestRefresh();
        histNeedsRefresh_ = true;
    }
    if (!paused) ImGui::EndDisabled();
    ImGui::SameLine();

    // Run: enabled only when paused
    if (!paused) ImGui::BeginDisabled();
    if (ImGui::Button("\xe2\x96\xb6 Run (F5)")) {  // ▶ Run (F5)
        backend.requestRun();
    }
    if (!paused) ImGui::EndDisabled();
    ImGui::SameLine();

    // Skip: enabled only when paused — run until next instruction
    if (!paused) ImGui::BeginDisabled();
    if (ImGui::Button("\xe2\x8f\xad Skip (F6)")) {  // ⏭ Skip (F6)
        backend.requestSkipInstruction();
        memoryInspector_.requestRefresh();
        stackView_.requestRefresh();
        disassemblyView_.requestRefresh();
        executionTrace_.requestRefresh();
        ioInspector_.requestRefresh();
        vectorScreen_.requestRefresh();
        functionsWindow_.requestRefresh();
        xrefsWindow_.requestRefresh();
        histNeedsRefresh_ = true;
    }
    if (!paused) ImGui::EndDisabled();
    ImGui::SameLine();

    // Pause: enabled only when running
    if (!running) ImGui::BeginDisabled();
    if (ImGui::Button("\xe2\x80\x96 Pause (F3)")) {  // ‖ Pause (F3)
        backend.requestPause();
        memoryInspector_.requestRefresh();
        stackView_.requestRefresh();
        disassemblyView_.requestRefresh();
        executionTrace_.requestRefresh();
        ioInspector_.requestRefresh();
        vectorScreen_.requestRefresh();
        functionsWindow_.requestRefresh();
        xrefsWindow_.requestRefresh();
        histNeedsRefresh_ = true;
    }
    if (!running) ImGui::EndDisabled();
    ImGui::SameLine();

    // Restart (БЛК+ВВОД): always enabled — attaches boot ROM, resets CPU
    if (ImGui::Button("\xe2\x86\xbb Restart (F12)")) {  // ↻ Restart (F12)
        backend.requestRestart();
        currentRomName_ = "BOOT";
        memoryInspector_.requestRefresh();
        stackView_.requestRefresh();
        disassemblyView_.requestRefresh();
        executionTrace_.requestRefresh();
        ioInspector_.requestRefresh();
        vectorScreen_.requestRefresh();
        histNeedsRefresh_ = true;
    }
    ImGui::SameLine();

    // Reset: attach boot ROM, reset CPU to PC=0 (bootloader entry)
    if (ImGui::Button("Reset (F11)")) {
        backend.requestReset();
        currentRomName_ = "BOOT";
        memoryInspector_.requestRefresh();
        stackView_.requestRefresh();
        disassemblyView_.requestRefresh();
        executionTrace_.requestRefresh();
        ioInspector_.requestRefresh();
        vectorScreen_.requestRefresh();
        histNeedsRefresh_ = true;
    }
}

// ---------------------------------------------------------------------------
// Layout — Cascade / Tile (Stage 5.1)
// ---------------------------------------------------------------------------

void DebuggerGui::layoutCascade()
{
    // Collect visible window names (same order as render pass)
    struct WinInfo { const char *name; bool *visible; };
    WinInfo wins[] = {
        {"CPU Registers", &showCpuRegisters_},
        {"Instruction History", &showInstructionHistory_},
        {"Vector Screen", &vectorScreen_.getVisibleRef()},
        {"Memory Inspector", &memoryInspector_.getVisibleRef()},
        {"Memory Map", &memoryMap_.getVisibleRef()},
        {"Memory Access", &memoryAccess_.getVisibleRef()},
        {"Disassembly", &disassemblyView_.getVisibleRef()},
        {"Stack View", &stackView_.getVisibleRef()},
        {"Breakpoints", &breakpointsWindow_.getVisibleRef()},
        {"Execution Trace", &executionTrace_.getVisibleRef()},
        {"I/O & Hardware Inspector", &ioInspector_.getVisibleRef()},
        {"MAP-file Info", &functionsWindow_.getVisibleRef()},
        {"Cross References", &xrefsWindow_.getVisibleRef()},
        {"Search", &searchWindow_.getVisibleRef()},
        {"Keyboard", &keyboardWindow_.getVisibleRef()},
        {"Sound", &soundWindow_.getVisibleRef()},
        {"VRAM Planes", &planeScreen_.getVisibleRef()},
    };

    ImGuiViewport *vp = ImGui::GetMainViewport();
    float baseX = vp->WorkPos.x + 40.0f;
    float baseY = vp->WorkPos.y + 40.0f;
    const int step = 24;
    int offset = 0;

    cascadePos_.clear();
    for (auto &w : wins) {
        if (!w.visible || !*w.visible) continue;
        cascadePos_[w.name] = {baseX + offset, baseY + offset};
        offset += step;
    }
    cascadeRequested_ = true;
}

void DebuggerGui::applyCascade()
{
    struct WinInfo { const char *name; bool *visible; };
    WinInfo wins[] = {
        {"CPU Registers", &showCpuRegisters_},
        {"Instruction History", &showInstructionHistory_},
        {"Vector Screen", &vectorScreen_.getVisibleRef()},
        {"Memory Inspector", &memoryInspector_.getVisibleRef()},
        {"Memory Map", &memoryMap_.getVisibleRef()},
        {"Memory Access", &memoryAccess_.getVisibleRef()},
        {"Disassembly", &disassemblyView_.getVisibleRef()},
        {"Stack View", &stackView_.getVisibleRef()},
        {"Breakpoints", &breakpointsWindow_.getVisibleRef()},
        {"Execution Trace", &executionTrace_.getVisibleRef()},
        {"I/O & Hardware Inspector", &ioInspector_.getVisibleRef()},
        {"MAP-file Info", &functionsWindow_.getVisibleRef()},
        {"Cross References", &xrefsWindow_.getVisibleRef()},
        {"Search", &searchWindow_.getVisibleRef()},
        {"Keyboard", &keyboardWindow_.getVisibleRef()},
        {"Sound", &soundWindow_.getVisibleRef()},
        {"VRAM Planes", &planeScreen_.getVisibleRef()},
    };

    for (auto &w : wins) {
        if (!w.visible || !*w.visible) continue;
        auto it = cascadePos_.find(w.name);
        if (it == cascadePos_.end()) continue;

        // Undock from dockspace
        ImGuiWindow *win = ImGui::FindWindowByName(w.name);
        if (win) {
            ImGui::DockContextQueueUndockWindow(ImGui::GetCurrentContext(), win);
        }
        // Set position and size BEFORE Begin()
        ImGui::SetNextWindowPos(ImVec2(it->second.x, it->second.y), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(600, 450), ImGuiCond_Always);
    }
    cascadePos_.clear();
    cascadeRequested_ = false;
}

void DebuggerGui::layoutTile()
{
    // Collect visible window names
    struct WinInfo { const char *name; bool *visible; };
    std::vector<WinInfo> visible;
    WinInfo all[] = {
        {"CPU Registers", &showCpuRegisters_},
        {"Instruction History", &showInstructionHistory_},
        {"Vector Screen", &vectorScreen_.getVisibleRef()},
        {"Memory Inspector", &memoryInspector_.getVisibleRef()},
        {"Memory Map", &memoryMap_.getVisibleRef()},
        {"Memory Access", &memoryAccess_.getVisibleRef()},
        {"Disassembly", &disassemblyView_.getVisibleRef()},
        {"Stack View", &stackView_.getVisibleRef()},
        {"Breakpoints", &breakpointsWindow_.getVisibleRef()},
        {"Execution Trace", &executionTrace_.getVisibleRef()},
        {"I/O & Hardware Inspector", &ioInspector_.getVisibleRef()},
        {"MAP-file Info", &functionsWindow_.getVisibleRef()},
        {"Cross References", &xrefsWindow_.getVisibleRef()},
        {"Search", &searchWindow_.getVisibleRef()},
        {"Keyboard", &keyboardWindow_.getVisibleRef()},
        {"Sound", &soundWindow_.getVisibleRef()},
        {"VRAM Planes", &planeScreen_.getVisibleRef()},
    };
    for (auto &w : all) {
        if (w.visible && *w.visible) visible.push_back(w);
    }
    if (visible.empty()) return;

    // Use the saved main dockspace ID (set during render)
    ImGuiID dsId = mainDockId_;
    if (dsId == 0) return;

    int n = (int)visible.size();
    if (n == 1) {
        ImGui::DockBuilderDockWindow(visible[0].name, dsId);
        return;
    }

    // Split dockspace: left column (1/3) and right area (2/3)
    ImGuiID leftId, rightId;
    ImGui::DockBuilderSplitNode(dsId, ImGuiDir_Left, 0.33f, &leftId, &rightId);

    // Split right area into top-right and bottom-right
    ImGuiID topRightId, bottomRightId;
    ImGui::DockBuilderSplitNode(rightId, ImGuiDir_Up, 0.5f, &topRightId, &bottomRightId);

    // Distribute windows evenly across the three regions
    int perRegion = (n + 2) / 3;
    for (int i = 0; i < n; i++) {
        ImGuiID target;
        if (i < perRegion) target = leftId;
        else if (i < 2 * perRegion) target = topRightId;
        else target = bottomRightId;
        ImGui::DockBuilderDockWindow(visible[i].name, target);
    }

    ImGui::DockBuilderFinish(dsId);
}

// ---------------------------------------------------------------------------
// Status bar
// ---------------------------------------------------------------------------

void DebuggerGui::renderStatusBar(IDebugBackend &backend)
{
    DebuggerState state = backend.getState();
    CpuState cpu = backend.getCpuState();
    uint64_t seq = backend.instructionSequence();

    const char *stateStr = "UNKNOWN";
    switch (state) {
        case DebuggerState::Running: stateStr = "RUNNING"; break;
        case DebuggerState::Paused:  stateStr = "PAUSED";  break;
        case DebuggerState::Stopped: stateStr = "STOPPED"; break;
    }

    // Stop reason (Stage 3.7)
    const char *reasonStr = "";
    switch (backend.getStopReason()) {
        case StopReason::Breakpoint:
            // Show address of the breakpoint
            {
                static char reasonBuf[64];
                snprintf(reasonBuf, sizeof(reasonBuf), " (Breakpoint at %04X)", cpu.pc);
                reasonStr = reasonBuf;
            }
            break;
        case StopReason::UserPause:  reasonStr = " (User Pause)"; break;
        case StopReason::Step:       reasonStr = " (Step)"; break;
        case StopReason::Reset:      reasonStr = " (Reset)"; break;
        default: break;
    }

    // Stage 4.10: Show current function name if PC is inside a function
    std::string funcName = backend.symbolDatabase().displayName(cpu.pc);
    if (funcName.empty()) {
        ImGui::Text("Status: %s%s   PC: %04X   Instructions: %llu",
                    stateStr, reasonStr, cpu.pc, (unsigned long long)seq);
    } else {
        ImGui::Text("Status: %s%s   PC: %04X (%s)   Instructions: %llu",
                    stateStr, reasonStr, cpu.pc, funcName.c_str(), (unsigned long long)seq);
    }

    // Vector Screen hover coordinates (Stage 5.1)
    if (vectorScreen_.isHoveringScreen()) {
        ImGui::SameLine();
        ImGui::Text("Screen: 512x256  X: %d  Y: %d",
                    vectorScreen_.hoverScreenX(), vectorScreen_.hoverScreenY());
    } else if (vectorScreen_.isHoveringBorder()) {
        ImGui::SameLine();
        ImGui::Text("Screen: 512x256  (border)");
    }
}

void DebuggerGui::loadRomFile(const std::string &path, IDebugBackend &backend)
{
    // Make a local copy: 'path' may be a reference into configManager_'s
    // recentRoms_ vector (when called from the Recent ROMs menu), and
    // addRecentRom() below can reallocate that vector, invalidating it.
    std::string romPath = path;

    uint32_t org = 0;
    backend.requestPause();
    if (backend.loadRom(romPath, org)) {
        // Extract filename from path
        size_t lastSlash = romPath.rfind('/');
        currentRomName_ = (lastSlash != std::string::npos)
            ? romPath.substr(lastSlash + 1) : romPath;
        // Add to recent ROMs list (ConfigManager handles persistence)
        configManager_.addRecentRom(romPath);
        memoryInspector_.requestRefresh();
        disassemblyView_.requestRefresh();
        stackView_.requestRefresh();
        vectorScreen_.requestRefresh();
        functionsWindow_.requestRefresh();
        romDatabaseWindow_.requestRefresh();
        histNeedsRefresh_ = true;
    } else {
        snprintf(romErrorBuffer_, sizeof(romErrorBuffer_),
                 "Failed to load: %s", romPath.c_str());
    }
}

void DebuggerGui::adoptCommandLineRom(const std::string &path)
{
    // The ROM was already loaded into the backend by main() before the GUI
    // existed; here we only do the GUI-side bookkeeping so a command-line /
    // "Open with..." launch looks identical to the "Open ROM..." menu: show
    // the filename in the toolbar and record it in the Recent ROMs list.
    size_t lastSlash = path.rfind('/');
    currentRomName_ = (lastSlash != std::string::npos)
        ? path.substr(lastSlash + 1) : path;
    configManager_.addRecentRom(path);
}

void DebuggerGui::installDesktopFile()
{
    desktopMsgBuffer_[0] = '\0';
    desktopMsgError_ = false;

    // Destination: ~/.local/share/applications/v06c-debugger.desktop
    const char *home = getenv("HOME");
    if (!home || !*home) {
        snprintf(desktopMsgBuffer_, sizeof(desktopMsgBuffer_),
                 "Cannot resolve $HOME - the .desktop was not installed.");
        desktopMsgError_ = true;
        return;
    }
    std::string dir  = std::string(home) + "/.local/share/applications";
    std::string dest = dir + "/v06c-debugger.desktop";

    // Source: the file CMake generates next to the executable (SDL_GetBasePath
    // returns the exe directory with a trailing slash). It already carries the
    // correct absolute Exec/Icon paths for this build, so we copy it verbatim
    // and keep a single source of truth (res/v06c-debugger.desktop.in).
    std::string src;
    if (char *base = SDL_GetBasePath()) {
        src = std::string(base) + "v06c-debugger.desktop";
        SDL_free(base);
    }

    std::string content;
    bool readOk = false;
    if (!src.empty()) {
        if (FILE *f = fopen(src.c_str(), "rb")) {
            char buf[4096];
            size_t n;
            content.clear();
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
                content.append(buf, n);
            fclose(f);
            readOk = true;
        }
    }
    if (!readOk) {
        snprintf(desktopMsgBuffer_, sizeof(desktopMsgBuffer_),
                 "Source not found next to the executable:\n%s", src.c_str());
        desktopMsgError_ = true;
        return;
    }

    // Create ~/.local/share/applications (mkdir -p semantics; EEXIST ignored).
    for (size_t i = 1; i < dir.size(); ++i) {
        if (dir[i] == '/')
            mkdir(dir.substr(0, i).c_str(), 0755);
    }
    mkdir(dir.c_str(), 0755);

    // Write (create or replace) the destination file.
    if (FILE *g = fopen(dest.c_str(), "wb")) {
        fwrite(content.data(), 1, content.size(), g);
        fclose(g);
        snprintf(desktopMsgBuffer_, sizeof(desktopMsgBuffer_),
                 "Installed desktop entry:\n%s", dest.c_str());
    } else {
        snprintf(desktopMsgBuffer_, sizeof(desktopMsgBuffer_),
                 "Failed to write:\n%s", dest.c_str());
        desktopMsgError_ = true;
    }
}

void DebuggerGui::loadWavFile(const std::string &path, IDebugBackend &backend)
{
    if (backend.loadWav(path)) {
        // Extract filename from path
        size_t lastSlash = path.rfind('/');
        currentRomName_ = (lastSlash != std::string::npos)
            ? path.substr(lastSlash + 1) : path;

        // Reset with boot ROM and start emulation
        // Press F1 to switch bootloader to tape mode
        backend.requestReset();
        backend.requestRun();
        backend.pressKey(SDL_SCANCODE_F1);
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        backend.releaseKey(SDL_SCANCODE_F1);
    } else {
        snprintf(wavErrorBuffer_, sizeof(wavErrorBuffer_),
                 "Failed to load: %s", path.c_str());
    }
}
