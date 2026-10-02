// Vector-06C Debugger — GUI entry point
//
// Composition root: creates DebugAdapter (emulator) + DebugBackend (core),
// an emulation thread, and runs the Dear ImGui main loop.
//
// Stage 3.13a: HAL functions moved to debug_adapter.cpp.
// main.cpp no longer contains Memory, IO, Board, or i8080_hal_* code.

#include "gui.h"
#include "backend.h"
#include "debug_adapter.h"

#include <cstdio>
#include <thread>

// ---------------------------------------------------------------------------
// Global backend pointer — used by HAL functions in debug_adapter.cpp
// for I/O instrumentation (onIoInput/onIoOutput callbacks).
// ---------------------------------------------------------------------------

DebugBackend *g_adapter_backend = nullptr;

// ---------------------------------------------------------------------------
// Emulation thread
// ---------------------------------------------------------------------------

static void emulationThreadFunc(DebugBackend &backend)
{
    backend.runUntilPause();
}

// Resolve a launch argument into a plain local filesystem path. A desktop
// "Open with..." hand-off passes either a raw path ("%f") or a "file://" URI
// ("%u"/"%U"), the latter percent-encoded (spaces etc.). Normalize both.
static std::string resolveLaunchPath(std::string p)
{
    const std::string scheme = "file://";
    if (p.rfind(scheme, 0) == 0)          // starts with "file://"
        p = p.substr(scheme.size());
    std::string out;
    out.reserve(p.size());
    for (size_t i = 0; i < p.size(); ++i) {
        auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        if (p[i] == '%' && i + 2 < p.size()) {
            int hi = hex(p[i + 1]), lo = hex(p[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        out.push_back(p[i]);
    }
    return out;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char *argv[])
{
    printf("Vector-06C Debugger — starting up\n");

    // Configure for debugger mode: OpenGL mode allocates direct pixel buffer
    // that we can read via screenSnapshot() for the Vector Screen window.
    Options.opengl = true;

    // --- Create full emulator (Board + all dependencies) ---
    DebugAdapter adapter;
    adapter.init();
    adapter.bindHal();   // sets static pointers used by HAL functions

    // --- Create DebugBackend and attach adapter as IDebugTarget ---
    DebugBackend backend(adapter);
    g_adapter_backend = &backend;

    // --- Load ROM from command line / "Open with..." hand-off if provided ---
    std::string cliRomPath;   // non-empty only after a successful load
    if (argc > 1) {
        std::string rom_path = resolveLaunchPath(argv[1]);
        uint32_t org = 0;

        // If user explicitly provides an origin address, use it.
        // Otherwise pass 0 and let the adapter auto-detect from extension.
        if (argc > 2) {
            org = std::strtoul(argv[2], nullptr, 0);
        }

        if (!backend.loadRom(rom_path, org)) {
            std::fprintf(stderr, "Failed to load ROM: %s\n", rom_path.c_str());
        } else {
            cliRomPath = std::move(rom_path);
        }
    }

    // --- Start emulation thread ---
    std::thread emuThread(emulationThreadFunc, std::ref(backend));

    // --- Init GUI ---
    DebuggerGui gui;
    if (!gui.initialize(1024, 768)) {
        std::fprintf(stderr, "Failed to initialize GUI\n");
        backend.requestQuit();
        emuThread.join();
        return 1;
    }

    // A ROM loaded before the GUI existed (command line / "Open with...")
    // must still show its filename in the toolbar and be added to Recent
    // ROMs, exactly like the "Open ROM..." menu item does.
    if (!cliRomPath.empty()) {
        gui.adoptCommandLineRom(cliRomPath);
    }

    // --- Main loop ---
    while (!gui.shouldQuit()) {
        gui.beginFrame(backend);
        gui.render(backend);  // DebugBackend& implicitly converts to IDebugBackend&
        gui.endFrame();

        // Workspace init deferred to between frames (Stage 5.1 fix):
        // writeBuiltinIfMissing creates a temp ImGui context which would
        // corrupt the main frame state if called during render.
        gui.applyPendingWorkspace();
    }

    // --- Shutdown ---
    backend.requestQuit();
    emuThread.join();
    gui.shutdown();
    adapter.shutdown();

    g_adapter_backend = nullptr;

    printf("Vector-06C Debugger — shutdown complete\n");

    return 0;
}
