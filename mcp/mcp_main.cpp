// ---------------------------------------------------------------------------
// mcp_main.cpp — Stage 6.4.1
//
// Entry point for v06c-mcp executable.
// Creates a headless emulator with real DebugAdapter/Board (no SDL video/audio),
// AgentApi, McpServer, registers all 38 tools, and runs stdio transport.
//
// Stage 6.4.1: Replaced NoBoardTarget with real DebugAdapter/Board.
// HAL functions are provided by debug_adapter.cpp (same as GUI debugger).
// ---------------------------------------------------------------------------

#include "mcp_adapter.h"
#include "agent_api.h"
#include "backend.h"
#include "debug_adapter.h"
#include "options.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

// ---------------------------------------------------------------------------
// Global backend pointer — used by HAL functions in debug_adapter.cpp
// for I/O instrumentation (onIoInput/onIoOutput callbacks).
// ---------------------------------------------------------------------------

DebugBackend *g_adapter_backend = nullptr;

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char *argv[])
{
    // Headless mode — no video, no sound
    Options.novideo = true;
    Options.nosound = true;

    // Create full emulator (real DebugAdapter with Board, Memory, CPU, IO, etc.)
    // Stage 6.4.1: Uses real DebugAdapter instead of NoBoardTarget.
    DebugAdapter adapter;
    adapter.init();
    adapter.bindHal();   // sets static pointers used by HAL functions

    // Create DebugBackend and attach adapter as IDebugTarget
    DebugBackend backend(adapter);
    g_adapter_backend = &backend;

    // Load ROM from command line if provided
    if (argc > 1) {
        std::string rom_path = argv[1];
        uint32_t org = 0;

        // If user explicitly provides an origin address, use it.
        // Otherwise pass 0 and let the adapter auto-detect from extension.
        if (argc > 2) {
            org = std::strtoul(argv[2], nullptr, 0);
        }

        if (!backend.loadRom(rom_path, org)) {
            std::fprintf(stderr, "Failed to load ROM: %s\n", rom_path.c_str());
        }
    }

    // Create Agent API
    AgentApi api(backend);

    // Create MCP server and register all tools
    McpServer mcp(api);
    mcp.registerAllTools();

    // Log to stderr (stdout is used for MCP protocol)
    fprintf(stderr, "v06c-mcp: %zu tools registered\n", mcp.registeredToolNames().size());

    // Stage 6.15: Start emulation thread.
    // Without it, the command queue is never processed and all operations
    // that go through submitAndWait() (annotations, breakpoints, run/step)
    // would timeout after 5 seconds.
    std::thread emulationThread([&backend]() {
        backend.runUntilPause();
    });
    fprintf(stderr, "v06c-mcp: emulation thread started\n");
    fprintf(stderr, "v06c-mcp: starting stdio transport (real DebugAdapter/Board)\n");

    // Run stdio transport (blocks until stdin closes)
    mcp.runStdio();

    // Cleanup
    fprintf(stderr, "v06c-mcp: shutting down\n");
    backend.requestQuit();
    if (emulationThread.joinable()) {
        emulationThread.join();
    }
    g_adapter_backend = nullptr;
    adapter.shutdown();

    return 0;
}
