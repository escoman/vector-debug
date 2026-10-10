cmake_minimum_required(VERSION 3.16)
project(vector-debugger)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -g -pthread")

# Debugger-only additions in the shared emulator sources (../src) are wrapped in
# #ifdef V06C_DEBUGGER. Define it here so the debugger build sees peek(),
# RawPaletteByte(), etc. The plain v06x emulator build does NOT define it.
add_definitions(-DV06C_DEBUGGER=1)

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

set(V06C_EMU_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/v06c-emu" CACHE PATH
    "Path to Vector-06C emulator sources (git submodule)")
set(ROOT_DIR ${V06C_EMU_DIR})
set(SRC_DIR  ${ROOT_DIR}/src)
set(DBG_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(DBG_TST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests)
set(DBG_GUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/gui)
set(DBG_RDB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/rdb)
set(AGENT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/agent)
set(IMGUI_DIR   ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/imgui)

# ---------------------------------------------------------------------------
# Include paths — we need headers from the main project (memory.h, i8080.h)
# and from the debugger module (backend.h, opcode_info.h).
# ---------------------------------------------------------------------------

include_directories(${SRC_DIR})
include_directories(${DBG_SRC_DIR})
include_directories(${AGENT_DIR})
include_directories(${Boost_INCLUDE_DIRS})
include_directories(${ROOT_DIR}/fast-filters/sources)  # coredsp for resampler
include_directories(${ROOT_DIR}/coreutil/sources)       # coreutil for coredsp
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/thirdparty)  # stb_image.h, nlohmann/json
include_directories(${DBG_RDB_DIR})                          # RDB module (Stage 6.11)

# ---------------------------------------------------------------------------
# Find Boost (required for Board sources)
# ---------------------------------------------------------------------------

find_package(Boost REQUIRED COMPONENTS program_options system thread chrono filesystem)

# ---------------------------------------------------------------------------
# Sources
#
# We compile the CPU core (i8080.cpp) and memory model (memory.cpp) from the
# main project.  We deliberately DO NOT link hal.cpp — the test binary
# provides its own minimal HAL implementation inside test_backend.cpp.
# ---------------------------------------------------------------------------

set(CORE_SOURCES
    ${SRC_DIR}/i8080.cpp
    ${SRC_DIR}/memory.cpp
)

# Board sources for full emulator integration (Stage 3.2)
# NOTE: hal.cpp is deliberately excluded — each target provides its own HAL.
# NOTE: icon.cpp uses a debugger-specific stub (no-op; icon comes from .desktop file)
set(BOARD_SOURCES
    ${SRC_DIR}/board.cpp
    ${SRC_DIR}/cadence.cpp
    ${SRC_DIR}/tv.cpp
    ${SRC_DIR}/sound.cpp
    ${SRC_DIR}/filler.cpp
    ${SRC_DIR}/options.cpp
    ${SRC_DIR}/util.cpp
    ${SRC_DIR}/resampler.cpp
    ${DBG_SRC_DIR}/icon.cpp
)

set(DBG_SOURCES
    ${DBG_SRC_DIR}/backend.cpp
    ${DBG_SRC_DIR}/disassembler.cpp
    ${DBG_SRC_DIR}/debug_memory.cpp
    ${DBG_SRC_DIR}/symbol_database.cpp
    ${DBG_SRC_DIR}/project_file.cpp
    ${DBG_SRC_DIR}/vram_mapping.cpp
    ${DBG_SRC_DIR}/no_board_target.cpp
    ${DBG_SRC_DIR}/map_loader.cpp
    ${DBG_SRC_DIR}/map_import.cpp
    ${DBG_SRC_DIR}/sha256.cpp
    ${DBG_SRC_DIR}/audio_midi.cpp
)

# RDB Module (Stage 6.11) — ROM Database Controller
# No dependency on ImGui, SDL, OpenGL, Board, MCP, or Agent.
set(RDB_SOURCES
    ${DBG_RDB_DIR}/rdb_types.cpp
    ${DBG_RDB_DIR}/rdb_controller.cpp
)

# Debug Adapter — bridge between Debugger Core and Vector Emulator (Stage 3.13/3.13a)
# Contains HAL function definitions and IDebugTarget implementation.
set(ADAPTER_SOURCES
    ${DBG_SRC_DIR}/debug_adapter.cpp
)

set(TEST_SOURCES
    ${DBG_TST_DIR}/test_backend.cpp
)

set(BOARD_TEST_SOURCES
    ${DBG_TST_DIR}/test_board_smoke.cpp
    ${DBG_TST_DIR}/dummy_resources.cpp
)

# ---------------------------------------------------------------------------
# Find SDL2 (required for Board sources)
# ---------------------------------------------------------------------------

set(CMAKE_MODULE_PATH ${ROOT_DIR}/cmake)
find_package(SDL2 REQUIRED)

# ---------------------------------------------------------------------------
# Test executable
# ---------------------------------------------------------------------------

add_executable(test_backend
    ${TEST_SOURCES}
    ${DBG_SOURCES}
    ${RDB_SOURCES}
    ${CORE_SOURCES}
)

target_link_libraries(test_backend
    ${Boost_LIBRARIES}
)

# ---------------------------------------------------------------------------
# RDB Controller test executable (Stage 6.11)
# ---------------------------------------------------------------------------

add_executable(test_rdb_controller
    ${DBG_TST_DIR}/test_rdb_controller.cpp
    ${RDB_SOURCES}
)

target_include_directories(test_rdb_controller PRIVATE
    ${DBG_RDB_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
)

# ---------------------------------------------------------------------------
# Symbol Database test executable (Stage 4.1)
# ---------------------------------------------------------------------------

add_executable(test_symbol_database
    ${DBG_TST_DIR}/test_symbol_database.cpp
    ${DBG_SRC_DIR}/symbol_database.cpp
)

# ---------------------------------------------------------------------------
# VRAM Mapping test executable (Stage 4 Enhanced)
# ---------------------------------------------------------------------------

add_executable(test_vram_mapping
    ${DBG_TST_DIR}/test_vram_mapping.cpp
    ${DBG_SRC_DIR}/vram_mapping.cpp
)

# ---------------------------------------------------------------------------
# MAP Loader test executable (Stage 6.2)
# ---------------------------------------------------------------------------

add_executable(test_map_loader
    ${DBG_TST_DIR}/test_map_loader.cpp
    ${DBG_SRC_DIR}/map_loader.cpp
    ${DBG_SRC_DIR}/symbol_database.cpp
)

# ---------------------------------------------------------------------------
# MAP Import test executable (Stage 6.11)
# ---------------------------------------------------------------------------

add_executable(test_map_import
    ${DBG_TST_DIR}/test_map_import.cpp
    ${DBG_SRC_DIR}/map_import.cpp
    ${DBG_SRC_DIR}/map_loader.cpp
    ${DBG_SRC_DIR}/symbol_database.cpp
    ${RDB_SOURCES}
)

target_include_directories(test_map_import PRIVATE
    ${DBG_SRC_DIR}
    ${DBG_RDB_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
)

# ---------------------------------------------------------------------------
# MAP Loader integration test (Stage 6.2)
# Uses real DebugBackend + NoBoardTarget.
# ---------------------------------------------------------------------------

add_executable(test_map_integration
    ${DBG_TST_DIR}/test_map_integration.cpp
    ${DBG_SOURCES}
    ${RDB_SOURCES}
    ${CORE_SOURCES}
    ${SRC_DIR}/options.cpp
)

target_include_directories(test_map_integration PRIVATE
    ${DBG_SRC_DIR}
    ${SRC_DIR}
)

target_link_libraries(test_map_integration
    ${Boost_LIBRARIES}
    pthread
)

# ---------------------------------------------------------------------------
# Runtime memory accounting tests (Stage 6.22 §1–§4)
# ---------------------------------------------------------------------------

add_executable(test_runtime_accounting
    ${DBG_TST_DIR}/test_runtime_accounting.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mcp/mcp_json.cpp
    ${DBG_SOURCES}
    ${RDB_SOURCES}
    ${CORE_SOURCES}
    ${SRC_DIR}/options.cpp
)

target_include_directories(test_runtime_accounting PRIVATE
    ${DBG_SRC_DIR}
    ${SRC_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/mcp
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/cpp-mcp/common
)

target_link_libraries(test_runtime_accounting
    ${Boost_LIBRARIES}
    pthread
)

# ---------------------------------------------------------------------------
# Board integration test executable (Stage 3.2)
# ---------------------------------------------------------------------------

add_executable(test_board_smoke
    ${BOARD_TEST_SOURCES}
    ${DBG_SOURCES}
    ${RDB_SOURCES}
    ${BOARD_SOURCES}
    ${SRC_DIR}/hal.cpp
    ${CORE_SOURCES}
)

target_link_libraries(test_board_smoke
    ${SDL2_LIBRARY}
    ${Boost_LIBRARIES}
)

# ---------------------------------------------------------------------------
# ROM load address unit test (Stage: Pipeline — correct ROM loading)
# ---------------------------------------------------------------------------

add_executable(test_rom_load_address
    ${DBG_TST_DIR}/test_rom_load_address.cpp
)

target_include_directories(test_rom_load_address PRIVATE
    ${DBG_SRC_DIR}
)

# ---------------------------------------------------------------------------
# ROM load + reset integration test (Stage: Pipeline — correct ROM loading)
# ---------------------------------------------------------------------------

add_executable(test_rom_loading
    ${DBG_TST_DIR}/test_rom_loading.cpp
    ${DBG_SOURCES}
    ${RDB_SOURCES}
    ${CORE_SOURCES}
    ${SRC_DIR}/options.cpp
)

target_include_directories(test_rom_loading PRIVATE
    ${DBG_SRC_DIR}
    ${SRC_DIR}
)

target_link_libraries(test_rom_loading
    ${Boost_LIBRARIES}
    pthread
)

# ---------------------------------------------------------------------------
# Reset CPU state regression test (Stage: Pipeline — BLK+СБР exact behavior)
# ---------------------------------------------------------------------------

add_executable(test_reset_cpu_state
    ${DBG_TST_DIR}/test_reset_cpu_state.cpp
    ${DBG_SOURCES}
    ${RDB_SOURCES}
    ${CORE_SOURCES}
    ${SRC_DIR}/options.cpp
)

target_include_directories(test_reset_cpu_state PRIVATE
    ${DBG_SRC_DIR}
    ${SRC_DIR}
)

target_link_libraries(test_reset_cpu_state
    ${Boost_LIBRARIES}
    pthread
)

# ---------------------------------------------------------------------------
# Workspace Manager test executable (Stage 5.1)
# ---------------------------------------------------------------------------

add_executable(test_workspace
    ${DBG_TST_DIR}/test_workspace.cpp
    ${DBG_GUI_DIR}/workspace_manager.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
)

target_include_directories(test_workspace PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${DBG_GUI_DIR}
)

target_link_libraries(test_workspace
    ${Boost_LIBRARIES}
)

# ---------------------------------------------------------------------------
# Config Manager test executable
# ---------------------------------------------------------------------------

add_executable(test_config_manager
    ${DBG_TST_DIR}/test_config_manager.cpp
    ${DBG_GUI_DIR}/config_manager.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
)

target_include_directories(test_config_manager PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${DBG_GUI_DIR}
)

target_link_libraries(test_config_manager
    ${Boost_LIBRARIES}
)

# ---------------------------------------------------------------------------
# GUI Smoke Test (Stage 5.1)
# ---------------------------------------------------------------------------
# Standalone test that spawns v06c-debugger and checks it doesn't crash.
# No project libraries needed — just POSIX fork/exec/wait.

add_executable(test_gui_smoke
    ${DBG_TST_DIR}/test_gui_smoke.cpp
)

# Ensure v06c-debugger is built before running this test
add_dependencies(test_gui_smoke v06c-debugger)

# ---------------------------------------------------------------------------
# Dear ImGui sources
# ---------------------------------------------------------------------------

set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl2.cpp
)

# ---------------------------------------------------------------------------
# debugger_core — static library (Stage 3.13a)
#
# Contains Debugger Core (backend, disassembler, etc.) + NoBoardTarget.
# Does NOT depend on Board, ImGui, SDL2, or OpenGL.
# Does NOT contain debug_adapter.cpp (which has HAL + Board dependencies).
# ---------------------------------------------------------------------------

add_library(debugger_core STATIC
    ${DBG_SOURCES}
    ${RDB_SOURCES}
)

target_include_directories(debugger_core PUBLIC
    ${DBG_SRC_DIR}
    ${SRC_DIR}
    ${DBG_RDB_DIR}
    ${AGENT_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
    ${Boost_INCLUDE_DIRS}
    ${ROOT_DIR}/fast-filters/sources
    ${ROOT_DIR}/coreutil/sources
)

target_link_libraries(debugger_core
    ${Boost_LIBRARIES}
    pthread
)

# ---------------------------------------------------------------------------
# debugger_adapter — static library (Stage 3.13a)
#
# Contains DebugAdapter (IDebugTarget implementation + HAL functions).
# Links with Board sources and CORE_SOURCES for full emulator integration.
# ---------------------------------------------------------------------------

add_library(debugger_adapter STATIC
    ${ADAPTER_SOURCES}
    ${BOARD_SOURCES}
    ${CORE_SOURCES}
)

target_include_directories(debugger_adapter PUBLIC
    ${DBG_SRC_DIR}
    ${SRC_DIR}
)

target_link_libraries(debugger_adapter
    debugger_core
    ${SDL2_LIBRARY}
    ${Boost_LIBRARIES}
    pthread
)

# ---------------------------------------------------------------------------
# GUI executable
# ---------------------------------------------------------------------------

# Find OpenGL
find_package(OpenGL REQUIRED)

# ---------------------------------------------------------------------------
# Live Memory Map test executable (Stage 5.2)
# ---------------------------------------------------------------------------

add_executable(test_live_map
    ${DBG_TST_DIR}/test_live_map.cpp
    ${DBG_GUI_DIR}/memory_map_window.cpp
    ${DBG_SRC_DIR}/symbol_database.cpp
    ${IMGUI_SOURCES}
)

target_include_directories(test_live_map PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${DBG_GUI_DIR}
    ${DBG_SRC_DIR}
    ${SRC_DIR}
)

target_link_libraries(test_live_map
    ${SDL2_LIBRARY}
    ${OPENGL_LIBRARIES}
    ${Boost_LIBRARIES}
    pthread
)

# ---------------------------------------------------------------------------
# Memory Access filter test executable (Stage 6.25)
#
# Pure-function tests over `applyMemoryAccessFilter()`.  The window .cpp is
# linked to satisfy the free function's TU; ImGui and disassembler sources
# are pulled in only for link-time symbol resolution — no test calls into
# render().
# ---------------------------------------------------------------------------

add_executable(test_memory_access_filter
    ${DBG_TST_DIR}/test_memory_access_filter.cpp
    ${DBG_GUI_DIR}/memory_access_window.cpp
    ${DBG_SRC_DIR}/disassembler.cpp
    ${IMGUI_SOURCES}
)

target_include_directories(test_memory_access_filter PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${DBG_GUI_DIR}
    ${DBG_SRC_DIR}
    ${SRC_DIR}
)

target_link_libraries(test_memory_access_filter
    ${SDL2_LIBRARY}
    ${OPENGL_LIBRARIES}
    ${Boost_LIBRARIES}
    pthread
)

# ---------------------------------------------------------------------------
# Binary resources (boot ROM, window icon)
# ---------------------------------------------------------------------------

find_program(OBJCOPY objcopy)

# Detect output format for objcopy based on platform
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(DBG_OBJCOPY_FMT "-O" "elf64-x86-64")
else()
    set(DBG_OBJCOPY_FMT "-O" "elf32-i386")
endif()

# Copy boots.bin to build directory and convert to object file
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/boots.bin
    COMMAND ${CMAKE_COMMAND} -E copy ${ROOT_DIR}/boot/boots.bin ${CMAKE_CURRENT_BINARY_DIR}/boots.bin
    DEPENDS ${ROOT_DIR}/boot/boots.bin
)

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/boots.bin.o
    COMMAND ${OBJCOPY} -I binary ${DBG_OBJCOPY_FMT} boots.bin boots.bin.o
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/boots.bin
)

set(BINARY_RESOURCES
    ${CMAKE_CURRENT_BINARY_DIR}/boots.bin.o
)

# ---------------------------------------------------------------------------
# Workspace presets
#
# The GUI reads/writes window layouts in <cwd>/workspaces/*.ini, relative to
# where v06c-debugger runs (the build dir). The tuned Default layout is kept as
# a tracked source asset under the top-level workspaces/ dir and seeded into the
# build dir at configure time — ONLY when absent, so the user's live tweaks in
# build/workspaces/ survive reconfigures (build/ itself stays gitignored).
# To promote an edited layout back into the repo, copy build/workspaces/*.ini to
# the top-level workspaces/ and commit.
# ---------------------------------------------------------------------------
file(GLOB WORKSPACE_PRESET_SEEDS ${CMAKE_CURRENT_SOURCE_DIR}/workspaces/*.ini)
if(WORKSPACE_PRESET_SEEDS)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/workspaces)
    foreach(_ws ${WORKSPACE_PRESET_SEEDS})
        get_filename_component(_wsname ${_ws} NAME)
        if(NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}/workspaces/${_wsname})
            file(COPY ${_ws} DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/workspaces)
        endif()
    endforeach()
endif()

# ---------------------------------------------------------------------------
# Sound Window test executable
#
# Part A: pure SoundWindow::analyzeSnapshot / channel-order tests (ImGui
# sources only needed to link sound_window.cpp). Part B: real DebugAdapter
# — PC0/i8253/AY port write monitoring through the io.onwrite hook.
# ---------------------------------------------------------------------------

add_executable(test_sound_window
    ${DBG_TST_DIR}/test_sound_window.cpp
    ${DBG_GUI_DIR}/sound_window.cpp
    ${DBG_GUI_DIR}/rom_file_dialog.cpp   # SAVE MID dialog lives in the Sound window
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${BINARY_RESOURCES}
)

target_include_directories(test_sound_window PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${DBG_GUI_DIR}
    ${DBG_SRC_DIR}
    ${SRC_DIR}
)

target_link_libraries(test_sound_window
    debugger_adapter
    debugger_core
    ${SDL2_LIBRARY}
    ${Boost_LIBRARIES}
    pthread
)

# Binary resources produce absolute symbols — disable PIE
set_target_properties(test_sound_window PROPERTIES
    LINK_FLAGS "-no-pie"
    POSITION_INDEPENDENT_CODE OFF
)

# ---------------------------------------------------------------------------
# Audio grab -> MIDI export test (Stage 6.28)
#
# Part A: pure audiomidi tests (VI53 byte-stream parser + SMF writer), no
# board needed. Part B: DebugAdapter grab buffer through io.onwrite with
# injected port writes. Part C: guarded golden comparison of PUTUP/RISEOUT
# attract-mode melodies vs score-derived reference .mid files.
# ---------------------------------------------------------------------------

add_executable(test_audio_midi
    ${DBG_TST_DIR}/test_audio_midi.cpp
    ${BINARY_RESOURCES}
)

target_include_directories(test_audio_midi PRIVATE
    ${DBG_SRC_DIR}
    ${SRC_DIR}
)

target_link_libraries(test_audio_midi
    debugger_adapter
    debugger_core
    ${SDL2_LIBRARY}
    ${Boost_LIBRARIES}
    pthread
)

# Binary resources produce absolute symbols — disable PIE
set_target_properties(test_audio_midi PROPERTIES
    LINK_FLAGS "-no-pie"
    POSITION_INDEPENDENT_CODE OFF
)

# ---------------------------------------------------------------------------
# AI Agent Module (Stage 5.3) — optional, off by default
# ---------------------------------------------------------------------------

option(ENABLE_AI_AGENT "Build AI Agent module for dynamic ROM analysis" OFF)

if(ENABLE_AI_AGENT)

    add_library(debugger_agent STATIC
        ${AGENT_DIR}/agent_api.cpp
        ${AGENT_DIR}/agent_log.cpp
        ${AGENT_DIR}/png_encoder.cpp
    )

    target_include_directories(debugger_agent PUBLIC
        ${AGENT_DIR}
        ${DBG_SRC_DIR}
        ${SRC_DIR}
    )

    target_link_libraries(debugger_agent
        debugger_core
        pthread
    )

    # Agent API tests (use mock backend — no Board/SDL dependency)
    add_executable(test_agent_api
        ${AGENT_DIR}/tests/test_agent_api.cpp
    )

    target_include_directories(test_agent_api PRIVATE
        ${AGENT_DIR}
        ${DBG_SRC_DIR}
        ${SRC_DIR}
    )

    target_link_libraries(test_agent_api
        debugger_agent
        debugger_core
        ${Boost_LIBRARIES}
        pthread
    )

    # Agent command protocol tests (Stage 5.3.1 — 15 mandatory tests)
    add_executable(test_agent_commands
        ${AGENT_DIR}/tests/test_agent_commands.cpp
    )

    target_include_directories(test_agent_commands PRIVATE
        ${AGENT_DIR}
        ${DBG_SRC_DIR}
        ${SRC_DIR}
    )

    target_link_libraries(test_agent_commands
        debugger_agent
        debugger_core
        ${Boost_LIBRARIES}
        pthread
    )

    # Mock Agent scenario test (uses real DebugBackend + DebugAdapter)
    add_executable(test_agent_mock
        ${AGENT_DIR}/tests/test_agent_mock.cpp
        ${AGENT_DIR}/agent_api.cpp
        ${AGENT_DIR}/agent_log.cpp
        ${AGENT_DIR}/png_encoder.cpp
        ${BINARY_RESOURCES}
    )

    target_include_directories(test_agent_mock PRIVATE
        ${AGENT_DIR}
        ${DBG_SRC_DIR}
        ${SRC_DIR}
    )

    target_link_libraries(test_agent_mock
        debugger_adapter
        debugger_core
        ${SDL2_LIBRARY}
        ${Boost_LIBRARIES}
        pthread
    )

    # Binary resources produce absolute symbols — disable PIE
    set_target_properties(test_agent_mock PROPERTIES
        LINK_FLAGS "-no-pie"
        POSITION_INDEPENDENT_CODE OFF
    )

    # Agent integration tests (Stage 5.3.2 — 20 mandatory tests)
    # Uses real DebugBackend + NoBoardTarget — needs CORE_SOURCES + options.
    add_executable(test_agent_integration
        ${AGENT_DIR}/tests/test_agent_integration.cpp
        ${DBG_SOURCES}
        ${RDB_SOURCES}
        ${CORE_SOURCES}
        ${SRC_DIR}/options.cpp
    )

    target_include_directories(test_agent_integration PRIVATE
        ${AGENT_DIR}
        ${DBG_SRC_DIR}
        ${SRC_DIR}
    )

    target_link_libraries(test_agent_integration
        debugger_agent
        ${Boost_LIBRARIES}
        pthread
    )

    # Agent API contract tests (Stage 6.3 — verify public API contract)
    add_executable(test_agent_contract
        ${AGENT_DIR}/tests/test_agent_contract.cpp
    )

    target_include_directories(test_agent_contract PRIVATE
        ${AGENT_DIR}
        ${DBG_SRC_DIR}
        ${SRC_DIR}
    )

    target_link_libraries(test_agent_contract
        debugger_agent
        debugger_core
        ${Boost_LIBRARIES}
        pthread
    )

    # Agent Runtime Memory Analysis tests (Stage 6.20)
    add_executable(test_agent_runtime_memory
        ${AGENT_DIR}/tests/test_agent_runtime_memory.cpp
    )

    target_include_directories(test_agent_runtime_memory PRIVATE
        ${AGENT_DIR}
        ${DBG_SRC_DIR}
        ${SRC_DIR}
    )

    target_link_libraries(test_agent_runtime_memory
        debugger_agent
        debugger_core
        ${Boost_LIBRARIES}
        pthread
    )

    # ROM Load Breakpoint Reset tests (Stage 6.20.1)
    # Uses real DebugBackend + NoBoardTarget — needs CORE_SOURCES + options.
    add_executable(test_agent_rom_load_breakpoints
        ${AGENT_DIR}/tests/test_agent_rom_load_breakpoints.cpp
        ${DBG_SOURCES}
        ${RDB_SOURCES}
        ${CORE_SOURCES}
        ${SRC_DIR}/options.cpp
    )

    target_include_directories(test_agent_rom_load_breakpoints PRIVATE
        ${AGENT_DIR}
        ${DBG_SRC_DIR}
        ${SRC_DIR}
    )

    target_link_libraries(test_agent_rom_load_breakpoints
        debugger_agent
        ${Boost_LIBRARIES}
        pthread
    )

    # RDB Save Symbol Sync tests (Stage 6.20.2)
    # Verifies rename/comment propagate to RDB for debug_save_rdb.
add_executable(test_agent_rdb_save_sync
${AGENT_DIR}/tests/test_agent_rdb_save_sync.cpp
        ${DBG_SOURCES}
        ${RDB_SOURCES}
        ${CORE_SOURCES}
        ${SRC_DIR}/options.cpp
    )

target_include_directories(test_agent_rdb_save_sync PRIVATE
        ${AGENT_DIR}
        ${DBG_SRC_DIR}
        ${SRC_DIR}
    )

target_link_libraries(test_agent_rdb_save_sync
        debugger_agent
        ${Boost_LIBRARIES}
        pthread
    )

    # -----------------------------------------------------------------------
    # MCP Adapter Module (Stage 6.4)
    # -----------------------------------------------------------------------

    set(MCP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/mcp)
    set(CPP_MCP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/cpp-mcp)

    # cpp-mcp library (from submodule sources)
    add_library(cpp_mcp STATIC
        ${CPP_MCP_DIR}/src/mcp_message.cpp
        ${CPP_MCP_DIR}/src/mcp_tool.cpp
        ${CPP_MCP_DIR}/src/mcp_server.cpp
        ${CPP_MCP_DIR}/src/mcp_resource.cpp
        ${CPP_MCP_DIR}/src/mcp_stdio_client.cpp
        ${CPP_MCP_DIR}/src/mcp_sse_client.cpp
    )

    target_include_directories(cpp_mcp PUBLIC
        ${CPP_MCP_DIR}/include
        ${CPP_MCP_DIR}/common
    )

    target_compile_definitions(cpp_mcp PUBLIC
        MCP_MAX_SESSIONS=10
        MCP_SESSION_TIMEOUT=30
    )

    target_link_libraries(cpp_mcp PUBLIC pthread)

    # debugger_mcp — MCP adapter library
    add_library(debugger_mcp STATIC
        ${MCP_DIR}/mcp_adapter.cpp
        ${MCP_DIR}/mcp_json.cpp
    )

    target_include_directories(debugger_mcp PUBLIC
        ${MCP_DIR}
        ${CPP_MCP_DIR}/include
        ${CPP_MCP_DIR}/common
        ${AGENT_DIR}
        ${DBG_SRC_DIR}
        ${SRC_DIR}
    )

    target_link_libraries(debugger_mcp
        debugger_agent
        debugger_core
        cpp_mcp
        pthread
    )

    # v06c-mcp — MCP executable (headless, stdio transport)
    # Stage 6.4.1: Uses real DebugAdapter/Board instead of NoBoardTarget.

    # ---- Build provenance stamp (mcp/vdb_version.h) ------------------------
    # A build sequence number incremented on every configure, persisted in
    # the build dir, plus git hash/dirty state and a UTC configure timestamp.
    # `make` (scripts/make_release.sh) re-configures on every run, so the
    # sequence grows with each rebuild; debug_get_server_info reports it so
    # clients can tell a stale running server apart from a fresh binary.
    set(VDB_SEQ_FILE "${CMAKE_BINARY_DIR}/.build_seq")
    if(EXISTS ${VDB_SEQ_FILE})
        file(READ ${VDB_SEQ_FILE} VDB_BUILD_SEQ)
        string(STRIP "${VDB_BUILD_SEQ}" VDB_BUILD_SEQ)
    else()
        set(VDB_BUILD_SEQ 0)
    endif()
    math(EXPR VDB_BUILD_SEQ "${VDB_BUILD_SEQ} + 1")
    file(WRITE ${VDB_SEQ_FILE} "${VDB_BUILD_SEQ}\n")

    find_package(Git QUIET)
    set(VDB_GIT_HASH "unknown")
    set(VDB_GIT_DIRTY "unknown")
    if(Git_FOUND)
        execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE VDB_GIT_HASH
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET RESULT_VARIABLE VDB_GIT_RES)
        if(NOT VDB_GIT_RES EQUAL 0)
            set(VDB_GIT_HASH "unknown")
        endif()
        execute_process(COMMAND ${GIT_EXECUTABLE} status --porcelain
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE VDB_GIT_STATUS
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(VDB_GIT_STATUS STREQUAL "")
            set(VDB_GIT_DIRTY "clean")
        else()
            set(VDB_GIT_DIRTY "dirty")
        endif()
    endif()
    string(TIMESTAMP VDB_BUILD_TIME "%Y-%m-%dT%H:%M:%SZ" UTC)
    message(STATUS "v06c-mcp build stamp: seq=${VDB_BUILD_SEQ} "
                   "git=${VDB_GIT_HASH} (${VDB_GIT_DIRTY}) time=${VDB_BUILD_TIME}")

    add_executable(v06c-mcp
        ${MCP_DIR}/mcp_main.cpp
        ${BINARY_RESOURCES}
    )

    # Stamp the MCP TU pair: mcp_main.cpp (executable banner) and
    # mcp_adapter.cpp (debugger_mcp lib — the debug_get_server_info handler).
    # Other consumers of debugger_mcp (unit tests, GUI) tolerate both stamped
    # and default values from vdb_version.h.
    target_compile_definitions(v06c-mcp PRIVATE
        VDB_GIT_HASH="${VDB_GIT_HASH}"
        VDB_GIT_DIRTY="${VDB_GIT_DIRTY}"
        VDB_BUILD_TIME="${VDB_BUILD_TIME}"
        VDB_BUILD_SEQ=${VDB_BUILD_SEQ}
    )
    target_compile_definitions(debugger_mcp PRIVATE
        VDB_GIT_HASH="${VDB_GIT_HASH}"
        VDB_GIT_DIRTY="${VDB_GIT_DIRTY}"
        VDB_BUILD_TIME="${VDB_BUILD_TIME}"
        VDB_BUILD_SEQ=${VDB_BUILD_SEQ}
    )

    target_include_directories(v06c-mcp PRIVATE
        ${MCP_DIR}
        ${CPP_MCP_DIR}/include
        ${CPP_MCP_DIR}/common
        ${AGENT_DIR}
        ${DBG_SRC_DIR}
        ${SRC_DIR}
    )

    target_link_libraries(v06c-mcp
        debugger_mcp
        debugger_agent
        debugger_adapter
        debugger_core
        cpp_mcp
        ${SDL2_LIBRARY}
        ${Boost_LIBRARIES}
        pthread
    )

    # Binary resources produce absolute symbols (_binary_*_size) which cannot
    # be used with position-independent code on x86_64. Disable PIE for this target.
    set_target_properties(v06c-mcp PROPERTIES
        LINK_FLAGS "-no-pie"
        POSITION_INDEPENDENT_CODE OFF
    )

    # MCP protocol tests (Stage 6.4)
    add_executable(test_mcp_protocol
        ${MCP_DIR}/tests/test_mcp_protocol.cpp
    )

    target_include_directories(test_mcp_protocol PRIVATE
        ${MCP_DIR}
        ${CPP_MCP_DIR}/include
        ${CPP_MCP_DIR}/common
        ${AGENT_DIR}
        ${AGENT_DIR}/tests
        ${DBG_SRC_DIR}
        ${SRC_DIR}
    )

    target_link_libraries(test_mcp_protocol
        debugger_mcp
        debugger_agent
        debugger_core
        cpp_mcp
        ${Boost_LIBRARIES}
        pthread
    )
endif()

# ---------------------------------------------------------------------------
# ASM Exporter (Stage 6.17) — Z88DK ASM export CLI tool
# ---------------------------------------------------------------------------

set(EXPORT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/export)

add_executable(v06c-asm-export
    ${EXPORT_DIR}/asm_exporter.cpp
    ${EXPORT_DIR}/asm_exporter_main.cpp
)

target_include_directories(v06c-asm-export PRIVATE
    ${EXPORT_DIR}
    ${AGENT_DIR}
    ${DBG_SRC_DIR}
    ${SRC_DIR}
    ${DBG_RDB_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
)

target_link_libraries(v06c-asm-export
    debugger_core
    ${Boost_LIBRARIES}
    pthread
)

# ASM Exporter tests (Stage 6.17)
add_executable(test_asm_exporter
    ${DBG_TST_DIR}/test_asm_exporter.cpp
    ${EXPORT_DIR}/asm_exporter.cpp
)

target_include_directories(test_asm_exporter PRIVATE
    ${EXPORT_DIR}
    ${AGENT_DIR}
    ${DBG_SRC_DIR}
    ${SRC_DIR}
    ${DBG_RDB_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
)

target_link_libraries(test_asm_exporter
    debugger_core
    ${Boost_LIBRARIES}
    pthread
)

# ---------------------------------------------------------------------------
# Code-analyzer tests (Stage 6.22 §7.4)
#
# classifyControlFlow() / analyzeCodeMulti() are inline in
# agent/code_analyzer.h; the target only needs debugger_core for disassemble().
# Guards the conditional-family mask: before Stage 6.22 the classifier matched
# only Rcc, so Jcc/Ccc/RST targets were never reached by the BFS.
# ---------------------------------------------------------------------------

add_executable(test_code_analyzer
    ${AGENT_DIR}/tests/test_code_analyzer.cpp
)

target_include_directories(test_code_analyzer PRIVATE
    ${AGENT_DIR}
    ${DBG_SRC_DIR}
    ${SRC_DIR}
)

target_link_libraries(test_code_analyzer
    debugger_core
    ${Boost_LIBRARIES}
    pthread
)

include_directories(${SDL2_INCLUDE_DIR})
include_directories(${IMGUI_DIR})
include_directories(${IMGUI_DIR}/backends)
include_directories(${DBG_GUI_DIR})

set(GUI_SOURCES
    ${DBG_GUI_DIR}/main.cpp
    ${DBG_GUI_DIR}/gui.cpp
    ${DBG_GUI_DIR}/workspace_manager.cpp
    ${DBG_GUI_DIR}/config_manager.cpp
    ${DBG_GUI_DIR}/memory_inspector_window.cpp
    ${DBG_GUI_DIR}/stack_view_window.cpp
    ${DBG_GUI_DIR}/breakpoints_window.cpp
    ${DBG_GUI_DIR}/disassembly_window.cpp
    ${DBG_GUI_DIR}/execution_trace_window.cpp
    ${DBG_GUI_DIR}/io_inspector_window.cpp
    ${DBG_GUI_DIR}/vector_screen_window.cpp
    ${DBG_GUI_DIR}/memory_map_window.cpp
    ${DBG_GUI_DIR}/memory_access_window.cpp
    ${DBG_GUI_DIR}/functions_window.cpp
    ${DBG_GUI_DIR}/xrefs_window.cpp
    ${DBG_GUI_DIR}/search_window.cpp
    ${DBG_GUI_DIR}/keyboard_window.cpp
    ${DBG_GUI_DIR}/rom_file_dialog.cpp
    ${DBG_GUI_DIR}/sound_window.cpp
    ${DBG_GUI_DIR}/plane_screen_window.cpp
    ${DBG_GUI_DIR}/rom_database_window.cpp
)

# ---------------------------------------------------------------------------
# GUI executable
# ---------------------------------------------------------------------------

# Find OpenGL
find_package(OpenGL REQUIRED)

add_executable(v06c-debugger
    ${GUI_SOURCES}
    ${IMGUI_SOURCES}
    ${BINARY_RESOURCES}
)

target_link_libraries(v06c-debugger
    debugger_adapter
    debugger_core
    ${SDL2_LIBRARY}
    ${OPENGL_LIBRARIES}
    ${Boost_LIBRARIES}
    pthread
)

# Binary resources produce absolute symbols (_binary_*_size) which cannot
# be used with position-independent code on x86_64. Disable PIE for this target.
set_target_properties(v06c-debugger PROPERTIES
    LINK_FLAGS "-no-pie"
    POSITION_INDEPENDENT_CODE OFF
)

# ---------------------------------------------------------------------------
# Desktop integration — generate .desktop file with absolute paths
# ---------------------------------------------------------------------------

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/res/v06c-debugger.desktop.in
    ${CMAKE_CURRENT_BINARY_DIR}/v06c-debugger.desktop
    @ONLY
)

# make install-desktop — copy .desktop to ~/.local/share/applications/
add_custom_target(install-desktop
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_CURRENT_BINARY_DIR}/v06c-debugger.desktop
        $ENV{HOME}/.local/share/applications/v06c-debugger.desktop
    COMMENT "Installing v06c-debugger.desktop to ~/.local/share/applications/"
)
