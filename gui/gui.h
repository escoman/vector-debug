#pragma once

#include <cstdint>
#include "memory_inspector_window.h"
#include "stack_view_window.h"
#include "breakpoints_window.h"
#include "disassembly_window.h"
#include "execution_trace_window.h"
#include "io_inspector_window.h"
#include "vector_screen_window.h"
#include "memory_map_window.h"
#include "memory_access_window.h"
#include "functions_window.h"
#include "xrefs_window.h"
#include "search_window.h"
#include "keyboard_window.h"
#include "rom_file_dialog.h"
#include "sound_window.h"
#include "plane_screen_window.h"
#include "rom_database_window.h"
#include "workspace_manager.h"
#include "config_manager.h"
#include "idebug_backend.h"
#include "events.h"

// Forward declarations — avoid pulling SDL/ImGui into this header.
struct SDL_Window;
typedef void *SDL_GLContext;

class IDebugBackend;

// ---------------------------------------------------------------------------
// DebuggerGui — minimal Dear ImGui + SDL2 + OpenGL2 GUI layer.
//
// Sits on top of IDebugBackend.  Does NOT access i8080, Memory, Board, etc.
// All emulator data flows through IDebugBackend snapshots/API.
// ---------------------------------------------------------------------------

class DebuggerGui
{
public:
    DebuggerGui() = default;
    ~DebuggerGui();

    // Create SDL window + GL context + init ImGui.
    // Returns false on failure.
    bool initialize(int width, int height);

    // Tear down ImGui + GL context + SDL window.
    void shutdown();

    // Begin a new ImGui frame (processes pending SDL events internally).
    // When keyboard lock is active, forwards key events to the emulator.
    void beginFrame(IDebugBackend &backend);

    // Render all debugger panels.  Called between beginFrame/endFrame.
    void render(IDebugBackend &backend);

    // Finalize the frame and swap buffers.
    void endFrame();

    // True after the user requested window close.
    bool shouldQuit() const;

    // Apply pending workspace initialization (call between frames).
    void applyPendingWorkspace();

    // Adopt a ROM that was loaded before the GUI existed (command line or
    // "Open with..." launch): show its filename in the toolbar and add it to
    // Recent ROMs, matching what the "Open ROM..." menu item does.
    void adoptCommandLineRom(const std::string &path);
    
    // Central Navigation API (Stage 3.9)
    void gotoMemory(uint16_t address);
    void gotoDisassembly(uint16_t address);
    void gotoStack(uint16_t address);

private:
    SDL_Window   *window_    = nullptr;
    SDL_GLContext glContext_  = nullptr;
    bool          quit_       = false;

    // Panel renderers
    void renderToolbar(IDebugBackend &backend);
    void renderCpuPanel(IDebugBackend &backend);
    void renderCurrentInstruction(uint16_t pc, IDebugBackend &backend);
    void renderInstructionHistory(IDebugBackend &backend);
    void renderStatusBar(IDebugBackend &backend);
    void renderControls(IDebugBackend &backend);
    //! Ask every view to re-read its state after a machine reset
    //! (F11/F12 hotkeys or a virtual ВВОД/СБР click).
    void refreshAfterReset();
    void layoutCascade();
    void layoutTile();
    void applyCascade();
    
    // Memory Inspector window (Stage 3.3)
    MemoryInspectorWindow memoryInspector_;
    
    // Stack View window (Stage 3.4)
    StackViewWindow stackView_;
    
    // Breakpoints window (Stage 3.7)
    BreakpointsWindow breakpointsWindow_;
    
    // Disassembly view (Stage 3.8)
    DisassemblyWindow disassemblyView_;
    
    // Execution Trace window (Stage 3.10)
    ExecutionTraceWindow executionTrace_;
    
    // I/O Inspector window (Stage 3.11)
    IoInspectorWindow ioInspector_;
    
    // Vector Screen window (Stage 4.3)
    VectorScreenWindow vectorScreen_;
    
    // Memory Map window (Stage 4.4)
    MemoryMapWindow memoryMap_;
    
    // Memory Access window (Stage 6.25)
    MemoryAccessWindow memoryAccess_;
    
    // Functions window (Stage 4.5)
    FunctionsWindow functionsWindow_;
    
    // ROM Database window (Stage 6.11)
    RomDatabaseWindow romDatabaseWindow_;
    
    // Xrefs window (Stage 4.7)
    XrefsWindow xrefsWindow_;
    
    // Search window (Stage 4.8)
    SearchWindow searchWindow_;
    
    // Keyboard window (Stage 5.3)
    KeyboardWindow keyboardWindow_;
    
    // ROM File Dialog (custom, non-blocking)
    RomFileDialog romFileDialog_;
    
    // Sound window
    SoundWindow soundWindow_;
    
    // VRAM Plane Screen window
    PlaneScreenWindow planeScreen_;
    
    // Instruction History cache (Stage 3.12 — avoid per-frame snapshot)
    std::vector<InstructionEvent> cachedHistEntries_;
    bool histNeedsRefresh_ = true;

    // Open ROM dialog state
    bool showOpenRomDialog_ = false;
    char romAddrBuffer_[16] = "0x0100";
    bool romAutoDetect_ = true;
    char romErrorBuffer_[256] = "";
    std::string currentRomName_ = "BOOT";  // filename only, shown in toolbar

    // Open WAV dialog state
    bool showOpenWavDialog_ = false;
    char wavErrorBuffer_[256] = "";

    // "Install .desktop" menu action: result message + destination writer.
    // Copies the CMake-generated v06c-debugger.desktop (which sits next to
    // the executable) into ~/.local/share/applications/ so a file manager's
    // "Open with..." can hand ROM files to the debugger. The message is
    // surfaced through the same modal pattern as the ROM/WAV error buffers.
    char desktopMsgBuffer_[512] = "";
    bool desktopMsgError_ = false;
    void installDesktopFile();

    // Recent ROMs — managed by ConfigManager
    void loadRomFile(const std::string &path, IDebugBackend &backend);
    void loadWavFile(const std::string &path, IDebugBackend &backend);

    // CPU register editing state (Stage 3.6)
    bool editingRegister_ = false;
    IDebugBackend::RegisterId editingRegId_ = IDebugBackend::RegisterId::AF;
    char editRegBuffer_[8] = "";
    bool writeRegFailed_ = false;

    // Dockable panel visibility (Stage 5.0)
    bool showCpuRegisters_      = true;
    bool showInstructionHistory_ = true;

    // Workspace Manager (Stage 5.1)
    WorkspaceManager workspaceManager_;

    // Config Manager — app-level settings (Recent ROMs, last ROM dir, etc.)
    ConfigManager configManager_;
    bool showSaveAsDialog_ = false;
    char saveAsNameBuffer_[256] = "";
    bool showDeleteConfirm_ = false;

    // Cascade / Tile layout state
    struct CascadePos { float x, y; };
    std::map<std::string, CascadePos> cascadePos_;
    bool cascadeRequested_ = false;
    unsigned int mainDockId_ = 0;
};
