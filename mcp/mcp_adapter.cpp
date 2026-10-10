// ---------------------------------------------------------------------------
// mcp_adapter.cpp — Stage 6.4
//
// McpServer implementation: registers all debug_* tools (tools/list is
// authoritative) as thin wrappers
// over AgentApi methods. Each handler: parse params → call AgentApi →
// serialize result to MCP content.
//
// MCP → AgentApi only. Never touches Board, Memory, DebugAdapter directly.
// ---------------------------------------------------------------------------

#include "mcp_adapter.h"
#include "mcp_json.h"
#include "vdb_version.h"

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <sstream>
#include <unistd.h>   // getpid()

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

McpServer::McpServer(AgentApi &api)
    : api_(api)
{
    mcp::server::configuration conf;
    conf.name = "v06c-mcp";
    conf.version = VDB_VERSION;
    server_ = std::make_unique<mcp::server>(conf);
    server_->set_server_info("v06c-mcp", VDB_VERSION);
    // Stage 6.26 §4: capability discovery.  tools/list always reflects the
    // actually registered tools (server_->get_tools()); the machine-readable
    // v06c.api_version lets clients detect batch-analysis capability without
    // guessing from the binary version.  Bump api_version when new tools or
    // result fields are added: 2 = Stage 6.26 batch analysis tools,
    // 3 = Stage 6.27 raster/beam debugging tools,
    // 4 = debug_get_server_info build-provenance tool.
    server_->set_capabilities({
        {"tools", {{"listChanged", false}}},
        {"v06c",  {{"api_version", 4}}}
    });
}

McpServer::~McpServer() = default;

// ---------------------------------------------------------------------------
// MCP CallToolResult helpers (Stage 6.4.1)
//
// MCP protocol requires CallToolResult format:
// {
//   "content": [{"type": "text", "text": "..."}],
//   "isError": true/false
// }
// ---------------------------------------------------------------------------

mcp::json McpServer::textContent(const mcp::json &data) {
    return mcp::json::array({
        {{"type", "text"}, {"text", data.dump(2)}}
    });
}

mcp::json McpServer::errorContent(const std::string &errorCode, const std::string &message) {
    mcp::json errData = {
        {"error_code", errorCode},
        {"message",    message}
    };
    return mcp::json::array({
        {{"type", "text"}, {"text", errData.dump(2)}}
    });
}

mcp::json McpServer::requireVoidResult(const AgentApiResult<void> &result) {
    if (!result.success) {
        throw mcp::mcp_exception(mcp::error_code::internal_error, result.error_message);
    }
    return textContent(mcp_json::successVoidResult());
}

// ---------------------------------------------------------------------------
// Register all tools
// ---------------------------------------------------------------------------

void McpServer::registerAllTools() {
    registerExecutionTools();
    registerCpuTools();
    registerMemoryTools();
    registerIoTools();
    registerKeyboardTools();
    registerBreakpointTools();
    registerDisassemblyTools();
    registerStackTools();
    registerSymbolTools();
    registerMemoryMapTools();
    registerIoTraceTools();
    registerDebugStateTools();
    registerRomTools();
    registerAnnotationTools();
    registerRdbTools();
    registerRuntimeAnalysisTools();  // Stage 6.20
    registerBatchAnalysisTools();    // Stage 6.26
    registerRasterTools();           // Stage 6.27: beam / raster
    registerServerTools();           // build provenance / version
}

void McpServer::runStdio() {
    server_->start_stdio();
}

std::vector<std::string> McpServer::registeredToolNames() const {
    std::vector<std::string> names;
    for (auto &t : server_->get_tools()) {
        names.push_back(t.name);
    }
    return names;
}

void McpServer::registerTool(const mcp::tool &tool, mcp::tool_handler handler) {
    handlers_[tool.name] = handler;
    server_->register_tool(tool, handler);
}

mcp::json McpServer::callTool(const std::string &toolName, const mcp::json &params) {
    auto it = handlers_.find(toolName);
    if (it == handlers_.end()) {
        throw mcp::mcp_exception(mcp::error_code::method_not_found,
            "tool not found: " + toolName);
    }
    return it->second(params, "test_session");
}

// ---------------------------------------------------------------------------
// Schema enhancement helpers (Stage 6.4.1)
//
// cpp-mcp tool_builder doesn't support numeric constraints directly.
// We manually add minimum/maximum to parameter schemas where appropriate.
// ---------------------------------------------------------------------------

static void addNumericConstraint(mcp::tool &tool, const std::string &paramName,
                                  std::optional<int> minimum = std::nullopt,
                                  std::optional<int> maximum = std::nullopt) {
    if (!tool.parameters_schema.contains("properties")) return;
    auto &props = tool.parameters_schema["properties"];
    if (!props.contains(paramName)) return;
    
    auto &paramSchema = props[paramName];
    if (minimum.has_value()) {
        paramSchema["minimum"] = minimum.value();
    }
    if (maximum.has_value()) {
        paramSchema["maximum"] = maximum.value();
    }
}

// ---------------------------------------------------------------------------
// Param extraction helpers
// ---------------------------------------------------------------------------

static uint16_t getAddress(const mcp::json &params) {
    if (!params.contains("address")) {
        throw mcp::mcp_exception(mcp::error_code::invalid_params, "missing required parameter: address");
    }
    int addr = params["address"].get<int>();
    if (addr < 0 || addr > 0xFFFF) {
        throw mcp::mcp_exception(mcp::error_code::invalid_params, "address out of range 0x0000..0xFFFF");
    }
    return static_cast<uint16_t>(addr);
}

static uint16_t getAddress(const mcp::json &params, const char *key) {
    if (!params.contains(key)) {
        throw mcp::mcp_exception(mcp::error_code::invalid_params,
            std::string("missing required parameter: ") + key);
    }
    int addr = params[key].get<int>();
    if (addr < 0 || addr > 0xFFFF) {
        throw mcp::mcp_exception(mcp::error_code::invalid_params,
            std::string(key) + " out of range 0x0000..0xFFFF");
    }
    return static_cast<uint16_t>(addr);
}

static size_t getCount(const mcp::json &params, const char *name, size_t defaultVal) {
    if (!params.contains(name)) return defaultVal;
    int v = params[name].get<int>();
    if (v < 0) {
        throw mcp::mcp_exception(mcp::error_code::invalid_params,
            std::string(name) + " must be >= 0");
    }
    return static_cast<size_t>(v);
}

static uint8_t getUint8(const mcp::json &params, const char *name) {
    if (!params.contains(name)) {
        throw mcp::mcp_exception(mcp::error_code::invalid_params,
            std::string("missing required parameter: ") + name);
    }
    int v = params[name].get<int>();
    if (v < 0 || v > 0xFF) {
        throw mcp::mcp_exception(mcp::error_code::invalid_params,
            std::string(name) + " out of range 0x00..0xFF");
    }
    return static_cast<uint8_t>(v);
}

// Required string parameter for the keyboard tools (the key name).
static std::string getKeyParam(const mcp::json &params) {
    if (!params.contains("key")) {
        throw mcp::mcp_exception(mcp::error_code::invalid_params,
            std::string("missing required parameter: key"));
    }
    if (!params["key"].is_string()) {
        throw mcp::mcp_exception(mcp::error_code::invalid_params,
            std::string("key must be a string"));
    }
    return params["key"].get<std::string>();
}

// ---------------------------------------------------------------------------
// Execution tools (5)
// ---------------------------------------------------------------------------

void McpServer::registerExecutionTools() {
    // debug_run
    {
        auto tool = mcp::tool_builder("debug_run")
            .with_description("Start or resume CPU execution.")
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            auto r = api_.run();
            if (!r.success) return errorContent("run_failed", r.error_message);
            return textContent(mcp_json::successVoidResult());
        });
    }

    // debug_pause
    {
        auto tool = mcp::tool_builder("debug_pause")
            .with_description("Pause CPU execution.")
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            auto r = api_.pause();
            if (!r.success) return errorContent("pause_failed", r.error_message);
            return textContent(mcp_json::successVoidResult());
        });
    }

    // debug_step
    {
        auto tool = mcp::tool_builder("debug_step")
            .with_description("Execute exactly one CPU instruction while paused.")
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            auto r = api_.step();
            if (!r.success) return errorContent("step_failed", r.error_message);
            return textContent(mcp_json::successVoidResult());
        });
    }

    // debug_reset
    {
        auto tool = mcp::tool_builder("debug_reset")
            .with_description("Reset the CPU to initial state.")
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            auto r = api_.reset();
            if (!r.success) return errorContent("reset_failed", r.error_message);
            return textContent(mcp_json::successVoidResult());
        });
    }

    // debug_is_running
    {
        auto tool = mcp::tool_builder("debug_is_running")
            .with_description("Check if the CPU is currently running.")
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            bool running = api_.isRunning();
            return textContent({{"running", running}});
        });
    }
}

// ---------------------------------------------------------------------------
// CPU tools (3)
// ---------------------------------------------------------------------------

void McpServer::registerCpuTools() {
    // debug_get_cpu_state
    {
        auto tool = mcp::tool_builder("debug_get_cpu_state")
            .with_description("Get the current CPU register state. "
                              "'cycles' is the duration in T-states of the last "
                              "executed instruction, not a cumulative counter.")
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            auto r = api_.getCpuState();
            if (!r.success) return errorContent("get_cpu_state_failed", r.error_message);
            return textContent(mcp_json::cpuStateToJson(r.value));
        });
    }

    // debug_get_registers (formatted view of getCpuState)
    {
        auto tool = mcp::tool_builder("debug_get_registers")
            .with_description("Get all CPU registers as a formatted summary.")
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            auto r = api_.getCpuState();
            if (!r.success) return errorContent("get_registers_failed", r.error_message);
            mcp::json regSummary = {
                {"PC",  mcp_json::hex16(r.value.pc)},
                {"SP",  mcp_json::hex16(r.value.sp)},
                {"A",   mcp_json::hex8(r.value.a)},
                {"F",   mcp_json::hex8(r.value.flags)},
                {"B",   mcp_json::hex8(r.value.b)},
                {"C",   mcp_json::hex8(r.value.c)},
                {"D",   mcp_json::hex8(r.value.d)},
                {"E",   mcp_json::hex8(r.value.e)},
                {"H",   mcp_json::hex8(r.value.h)},
                {"L",   mcp_json::hex8(r.value.l)},
                {"AF",  mcp_json::hex16((static_cast<uint16_t>(r.value.a) << 8) | r.value.flags)},
                {"BC",  mcp_json::hex16((static_cast<uint16_t>(r.value.b) << 8) | r.value.c)},
                {"DE",  mcp_json::hex16((static_cast<uint16_t>(r.value.d) << 8) | r.value.e)},
                {"HL",  mcp_json::hex16((static_cast<uint16_t>(r.value.h) << 8) | r.value.l)}
            };
            return textContent(regSummary);
        });
    }

    // debug_set_register
    {
        auto tool = mcp::tool_builder("debug_set_register")
            .with_description("Set a CPU register value by name.")
            .with_string_param("name", "Register name (A, B, C, D, E, H, L, AF, BC, DE, HL, SP, PC)")
            .with_number_param("value", "16-bit value (0..65535)")
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            if (!params.contains("name")) {
                throw mcp::mcp_exception(mcp::error_code::invalid_params, "missing required parameter: name");
            }
            std::string name = params["name"].get<std::string>();
            int val = params["value"].get<int>();
            if (val < 0 || val > 0xFFFF) {
                throw mcp::mcp_exception(mcp::error_code::invalid_params, "value out of range 0..65535");
            }
            auto r = api_.setRegister(name, static_cast<uint16_t>(val));
            if (!r.success) return errorContent("set_register_failed", r.error_message);
            return textContent(mcp_json::successVoidResult());
        });
    }
}

// ---------------------------------------------------------------------------
// Memory tools (2)
// ---------------------------------------------------------------------------

void McpServer::registerMemoryTools() {
    // debug_read_memory
    {
        auto tool = mcp::tool_builder("debug_read_memory")
            .with_description("Read bytes from Vector-06C memory.")
            .with_number_param("address", "Start address (0..65535)")
            .with_number_param("size", "Number of bytes to read (>= 1)")
            .build();
        // Stage 6.4.1: Add numeric constraints to schema
        addNumericConstraint(tool, "address", 0, 65535);
        addNumericConstraint(tool, "size", 1, std::nullopt);
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            uint16_t addr = getAddress(params);
            size_t size = getCount(params, "size", 1);
            if (size == 0) {
                throw mcp::mcp_exception(mcp::error_code::invalid_params, "size must be >= 1");
            }
            auto r = api_.readMemory(addr, size);
            if (!r.success) return errorContent("read_memory_failed", r.error_message);
            mcp::json bytesArr = mcp::json::array();
            for (auto b : r.value) bytesArr.push_back(b);
            return textContent({
                {"address", mcp_json::hex16(addr)},
                {"size",    static_cast<int>(r.value.size())},
                {"bytes",   bytesArr}
            });
        });
    }

    // debug_write_memory
    {
        auto tool = mcp::tool_builder("debug_write_memory")
            .with_description("Write bytes to Vector-06C memory.")
            .with_number_param("address", "Start address (0..65535)")
            .with_array_param("data", "Array of byte values (0..255)", "integer")
            .build();
        // Stage 6.4.1: Add numeric constraints to schema
        addNumericConstraint(tool, "address", 0, 65535);
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            uint16_t addr = getAddress(params);
            if (!params.contains("data")) {
                throw mcp::mcp_exception(mcp::error_code::invalid_params, "missing required parameter: data");
            }
            std::vector<uint8_t> data;
            for (auto &v : params["data"]) {
                int byte = v.get<int>();
                if (byte < 0 || byte > 0xFF) {
                    throw mcp::mcp_exception(mcp::error_code::invalid_params, "byte value out of range 0..255");
                }
                data.push_back(static_cast<uint8_t>(byte));
            }
            auto r = api_.writeMemory(addr, data);
            if (!r.success) return errorContent("write_memory_failed", r.error_message);
            return textContent({
                {"address", mcp_json::hex16(addr)},
                {"written", static_cast<int>(data.size())}
            });
        });
    }

    // debug_read_memory_range (Stage 6.16)
    {
        auto tool = mcp::tool_builder("debug_read_memory_range")
            .with_description("Read a contiguous range of memory bytes in a single call. "
                              "Use for bulk ROM data access instead of repeated debug_read_memory.")
            .with_number_param("address", "Start address (0..65535)")
            .with_number_param("length", "Number of bytes to read (1..16384)")
            .build();
        addNumericConstraint(tool, "address", 0, 65535);
        addNumericConstraint(tool, "length", 1, static_cast<int>(AgentLimits::MAX_MEMORY_READ_RANGE));
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            uint16_t addr = getAddress(params);
            size_t length = getCount(params, "length", 1);
            if (length == 0) {
                throw mcp::mcp_exception(mcp::error_code::invalid_params, "length must be >= 1");
            }
            if (length > AgentLimits::MAX_MEMORY_READ_RANGE) {
                throw mcp::mcp_exception(mcp::error_code::invalid_params,
                    "length exceeds maximum (" + std::to_string(AgentLimits::MAX_MEMORY_READ_RANGE) + ")");
            }
            uint32_t endAddr = static_cast<uint32_t>(addr) + length;
            if (endAddr > 0x10000) {
                return errorContent("invalid_range",
                    "address + length exceeds 64K address space");
            }
            auto r = api_.readMemory(addr, length);
            if (!r.success) return errorContent("read_memory_range_failed", r.error_message);
            mcp::json dataArr = mcp::json::array();
            for (auto b : r.value) dataArr.push_back(b);
            return textContent({
                {"address", mcp_json::hex16(addr)},
                {"length",  static_cast<int>(r.value.size())},
                {"data",    dataArr}
            });
        });
    }
}

// ---------------------------------------------------------------------------
// I/O tools (2)
// ---------------------------------------------------------------------------

void McpServer::registerIoTools() {
    // debug_read_io
    {
        auto tool = mcp::tool_builder("debug_read_io")
            .with_description("Read a value from an I/O port.")
            .with_number_param("port", "I/O port number (0..255)")
            .build();
        // Stage 6.4.1: Add numeric constraints to schema
        addNumericConstraint(tool, "port", 0, 255);
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            uint8_t port = getUint8(params, "port");
            auto r = api_.readIo(port);
            if (!r.success) return errorContent("read_io_failed", r.error_message);
            return textContent({
                {"port",  mcp_json::hex8(port)},
                {"value", mcp_json::hex8(r.value)}
            });
        });
    }

    // debug_write_io
    {
        auto tool = mcp::tool_builder("debug_write_io")
            .with_description("Write a value to an I/O port.")
            .with_number_param("port", "I/O port number (0..255)")
            .with_number_param("value", "Byte value to write (0..255)")
            .build();
        // Stage 6.4.1: Add numeric constraints to schema
        addNumericConstraint(tool, "port", 0, 255);
        addNumericConstraint(tool, "value", 0, 255);
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            uint8_t port = getUint8(params, "port");
            uint8_t value = getUint8(params, "value");
            auto r = api_.writeIo(port, value);
            if (!r.success) return errorContent("write_io_failed", r.error_message);
            return textContent({
                {"port",  mcp_json::hex8(port)},
                {"value", mcp_json::hex8(value)}
            });
        });
    }
}

// ---------------------------------------------------------------------------
// Virtual keyboard tools (4)
//
// Inject key events into the emulated 8x8 keyboard matrix.  Keys are named
// (case-insensitive); debug_list_keys returns the authoritative table.
// The ROM only reads the matrix while the CPU runs, so typeKey holds the key
// in real time; pressKey/releaseKey are the raw primitives for combinations.
// ---------------------------------------------------------------------------

void McpServer::registerKeyboardTools() {
    // debug_list_keys
    {
        auto tool = mcp::tool_builder("debug_list_keys")
            .with_description("List all virtual-keyboard key names accepted by the keyboard tools.")
            .build();
        registerTool(tool, [this](const mcp::json &, const std::string &) -> mcp::json {
            auto r = api_.listKeys();
            if (!r.success) return errorContent("list_keys_failed", r.error_message);
            mcp::json arr = mcp::json::array();
            for (const auto &k : r.value) {
                arr.push_back({
                    {"name",        k.name},
                    {"scancode",    k.scancode},
                    {"description", k.description},
                    {"modifier",    k.modifier}
                });
            }
            return textContent({{"count", static_cast<int>(r.value.size())},
                                {"keys", arr}});
        });
    }

    // debug_press_key
    {
        auto tool = mcp::tool_builder("debug_press_key")
            .with_description("Press (and hold) a keyboard key. Pair with debug_release_key. Use debug_type_key for a normal tap.")
            .with_string_param("key", "Key name (see debug_list_keys), e.g. A, 1, SPACE, ENTER, F1, SS, US, RUS")
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            auto r = api_.pressKey(getKeyParam(params));
            if (!r.success) return errorContent("press_key_failed", r.error_message);
            return textContent(mcp_json::successVoidResult());
        });
    }

    // debug_release_key
    {
        auto tool = mcp::tool_builder("debug_release_key")
            .with_description("Release a previously pressed keyboard key.")
            .with_string_param("key", "Key name (see debug_list_keys)")
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            auto r = api_.releaseKey(getKeyParam(params));
            if (!r.success) return errorContent("release_key_failed", r.error_message);
            return textContent(mcp_json::successVoidResult());
        });
    }

    // debug_type_key
    {
        auto tool = mcp::tool_builder("debug_type_key")
            .with_description("Tap a key: press, hold ~120ms (real time), release. Requires the CPU to be running (debug_run) or the ROM will not sample it; on pause it latches until resume.")
            .with_string_param("key", "Key name (see debug_list_keys)")
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            auto r = api_.typeKey(getKeyParam(params));
            if (!r.success) return errorContent("type_key_failed", r.error_message);
            return textContent(mcp_json::successVoidResult());
        });
    }
}

// ---------------------------------------------------------------------------
// Breakpoint tools (5)
// ---------------------------------------------------------------------------

void McpServer::registerBreakpointTools() {
    // debug_set_breakpoint
    {
        auto tool = mcp::tool_builder("debug_set_breakpoint")
            .with_description("Set a breakpoint at an address.")
            .with_number_param("address", "Breakpoint address (0..65535)")
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            uint16_t addr = getAddress(params);
            auto r = api_.setBreakpoint(addr);
            if (!r.success) return errorContent("set_breakpoint_failed", r.error_message);
            return textContent({{"breakpoint", mcp_json::hex16(addr)}});
        });
    }

    // debug_remove_breakpoint
    {
        auto tool = mcp::tool_builder("debug_remove_breakpoint")
            .with_description("Remove a breakpoint from an address.")
            .with_number_param("address", "Breakpoint address (0..65535)")
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            uint16_t addr = getAddress(params);
            auto r = api_.clearBreakpoint(addr);
            if (!r.success) return errorContent("remove_breakpoint_failed", r.error_message);
            return textContent(mcp_json::successVoidResult());
        });
    }

    // debug_list_breakpoints
    {
        auto tool = mcp::tool_builder("debug_list_breakpoints")
            .with_description("List all breakpoints.")
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            auto r = api_.listBreakpoints();
            if (!r.success) return errorContent("list_breakpoints_failed", r.error_message);
            return textContent({
                {"count",      static_cast<int>(r.value.size())},
                {"breakpoints", mcp_json::breakpointsToJson(r.value)}
            });
        });
    }

    // debug_clear_breakpoints
    {
        auto tool = mcp::tool_builder("debug_clear_breakpoints")
            .with_description("Remove all breakpoints.")
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            auto r = api_.clearAllBreakpoints();
            if (!r.success) return errorContent("clear_breakpoints_failed", r.error_message);
            return textContent(mcp_json::successVoidResult());
        });
    }

    // debug_set_breakpoint_enabled
    {
        auto tool = mcp::tool_builder("debug_set_breakpoint_enabled")
            .with_description("Enable or disable a breakpoint.")
            .with_number_param("address", "Breakpoint address (0..65535)")
            .with_boolean_param("enabled", "true to enable, false to disable")
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            uint16_t addr = getAddress(params);
            if (!params.contains("enabled")) {
                throw mcp::mcp_exception(mcp::error_code::invalid_params, "missing required parameter: enabled");
            }
            bool enabled = params["enabled"].get<bool>();
            auto r = api_.setBreakpointEnabled(addr, enabled);
            if (!r.success) return errorContent("set_breakpoint_enabled_failed", r.error_message);
            return textContent(mcp_json::successVoidResult());
        });
    }
}

// ---------------------------------------------------------------------------
// Disassembly / Trace tools (3)
// ---------------------------------------------------------------------------

void McpServer::registerDisassemblyTools() {
    // debug_disassemble
    {
        auto tool = mcp::tool_builder("debug_disassemble")
            .with_description("Disassemble instructions starting at an address.")
            .with_number_param("address", "Start address (0..65535)")
            .with_number_param("count", "Number of instructions (default 10)", false)
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            uint16_t addr = getAddress(params);
            size_t count = getCount(params, "count", 10);
            auto r = api_.disassemble(addr, count);
            if (!r.success) return errorContent("disassemble_failed", r.error_message);
            return textContent({
                {"address",      mcp_json::hex16(addr)},
                {"count",        static_cast<int>(r.value.size())},
                {"instructions", mcp_json::disassembledInstructionsToJson(r.value)}
            });
        });
    }

    // debug_get_instruction_history
    {
        auto tool = mcp::tool_builder("debug_get_instruction_history")
            .with_description("Get the last N executed instructions.")
            .with_number_param("count", "Number of entries (default 100)", false)
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            size_t count = getCount(params, "count", 100);
            auto r = api_.getInstructionHistory(count);
            if (!r.success) return errorContent("get_instruction_history_failed", r.error_message);
            return textContent({
                {"count",   static_cast<int>(r.value.size())},
                {"entries", mcp_json::historyEntriesToJson(r.value)}
            });
        });
    }

    // debug_get_execution_trace
    {
        auto tool = mcp::tool_builder("debug_get_execution_trace")
            .with_description("Get execution trace events.")
            .with_number_param("max_entries", "Maximum entries (default 1000)", false)
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            size_t maxEntries = getCount(params, "max_entries", 1000);
            auto r = api_.getExecutionTrace(maxEntries);
            if (!r.success) return errorContent("get_execution_trace_failed", r.error_message);
            return textContent({
                {"count",  static_cast<int>(r.value.size())},
                {"events", mcp_json::instructionEventsToJson(r.value)}
            });
        });
    }

    // debug_analyze_code (Stage 6.16, 6.19 — multi-entry)
    {
        auto tool = mcp::tool_builder("debug_analyze_code")
            .with_description("Analyze reachable 8080 machine code using control-flow analysis. "
                              "Follows JMP/CALL/Jcc/RST targets. Does NOT modify RDB. "
                              "Use 'start_address' for single entry point or 'addresses' for multi-entry analysis.")
            .with_number_param("start_address", "Entry point address (0..65535). Mutually exclusive with 'addresses'", false)
            .with_array_param("addresses", "Array of entry point addresses (0..65535). Mutually exclusive with 'start_address'", "integer", false)
            .with_number_param("max_instructions", "Maximum instructions to analyze (default 1000)", false)
            .build();
        addNumericConstraint(tool, "start_address", 0, 65535);
        addNumericConstraint(tool, "max_instructions", 1, static_cast<int>(AgentLimits::MAX_CODE_ANALYSIS_INSTRUCTIONS));
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            bool hasStartAddr = params.contains("start_address");
            bool hasAddresses = params.contains("addresses");

            if (hasStartAddr && hasAddresses) {
                throw mcp::mcp_exception(mcp::error_code::invalid_params,
                    "'start_address' and 'addresses' are mutually exclusive");
            }
            if (!hasStartAddr && !hasAddresses) {
                throw mcp::mcp_exception(mcp::error_code::invalid_params,
                    "either 'start_address' or 'addresses' is required");
            }

            size_t maxInstr = getCount(params, "max_instructions", 1000);
            AgentApiResult<CodeAnalysisResult> r;

            if (hasAddresses) {
                const auto &arr = params["addresses"];
                if (!arr.is_array()) {
                    throw mcp::mcp_exception(mcp::error_code::invalid_params,
                        "'addresses' must be an array");
                }
                if (arr.empty()) {
                    throw mcp::mcp_exception(mcp::error_code::invalid_params,
                        "'addresses' must not be empty");
                }
                if (arr.size() > AgentLimits::MAX_ANALYSIS_ENTRY_POINTS) {
                    throw mcp::mcp_exception(mcp::error_code::invalid_params,
                        "'addresses' count exceeds limit (" +
                        std::to_string(AgentLimits::MAX_ANALYSIS_ENTRY_POINTS) + ")");
                }
                std::vector<uint16_t> entryPoints;
                for (const auto &v : arr) {
                    int addr = v.get<int>();
                    if (addr < 0 || addr > 65535) {
                        throw mcp::mcp_exception(mcp::error_code::invalid_params,
                            "each address must be 0..65535");
                    }
                    entryPoints.push_back(static_cast<uint16_t>(addr));
                }
                r = api_.analyzeCode(entryPoints, maxInstr);
            } else {
                uint16_t startAddr = getAddress(params, "start_address");
                r = api_.analyzeCode(startAddr, maxInstr);
            }

            if (!r.success) return errorContent("analyze_code_failed", r.error_message);

            const auto &a = r.value;

            mcp::json instrs = mcp::json::array();
            for (const auto &inst : a.instructions) {
                instrs.push_back({
                    {"address",   mcp_json::hex16(inst.address)},
                    {"opcode",    mcp_json::hex8(inst.opcode)},
                    {"mnemonic",  inst.mnemonic},
                    {"operands",  inst.operands},
                    {"text",      inst.text},
                    {"size",      inst.size}
                });
            }

            mcp::json refs = mcp::json::array();
            for (const auto &ref : a.references) {
                refs.push_back({
                    {"from", mcp_json::hex16(ref.from)},
                    {"to",   mcp_json::hex16(ref.to)},
                    {"type", ref.type}
                });
            }

            mcp::json ranges = mcp::json::array();
            for (const auto &rng : a.ranges) {
                ranges.push_back({
                    {"start", mcp_json::hex16(rng.start)},
                    {"end",   mcp_json::hex16(rng.end)}
                });
            }

            mcp::json conflicts = mcp::json::array();
            for (const auto &c : a.conflicts) {
                conflicts.push_back({
                    {"type",    "instruction_boundary_conflict"},
                    {"address", mcp_json::hex16(c.address)},
                    {"description", c.description}
                });
            }

            mcp::json eps = mcp::json::array();
            for (auto ep : a.entryPoints) {
                eps.push_back(mcp_json::hex16(ep));
            }

            return textContent({
                {"entry_point",     mcp_json::hex16(a.entryPoint)},
                {"entry_points",    eps},
                {"instruction_count", static_cast<int>(a.instructionCount)},
                {"code_bytes",      static_cast<int>(a.codeBytes)},
                {"truncated",       a.truncated},
                {"instructions",    instrs},
                {"references",      refs},
                {"ranges",          ranges},
                {"conflicts",       conflicts}
            });
        });
    }

    // debug_disassemble_range (Stage 6.18)
    {
        auto tool = mcp::tool_builder("debug_disassemble_range")
            .with_description("Sequentially disassemble a range of memory. "
                              "Returns instructions with branch targets and types. "
                              "Does NOT perform CFG traversal or modify RDB.")
            .with_number_param("address", "Start address (0..65535)")
            .with_number_param("size", "Number of bytes to disassemble (1..16384)")
            .build();
        addNumericConstraint(tool, "address", 0, 65535);
        addNumericConstraint(tool, "size", 1, static_cast<int>(AgentLimits::MAX_MEMORY_READ_RANGE));
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            uint16_t addr = getAddress(params);
            uint16_t size = getAddress(params, "size");
            
            if (size == 0) {
                throw mcp::mcp_exception(mcp::error_code::invalid_params, "size must be >= 1");
            }
            if (size > AgentLimits::MAX_MEMORY_READ_RANGE) {
                throw mcp::mcp_exception(mcp::error_code::invalid_params,
                    "size exceeds maximum (" + std::to_string(AgentLimits::MAX_MEMORY_READ_RANGE) + ")");
            }
            uint32_t endAddr = static_cast<uint32_t>(addr) + size;
            if (endAddr > 0x10000) {
                return errorContent("invalid_range",
                    "address + size exceeds 64K address space");
            }
            
            auto r = api_.disassembleRange(addr, size);
            if (!r.success) return errorContent("disassemble_range_failed", r.error_message);
            
            const auto &result = r.value;
            
            mcp::json instrs = mcp::json::array();
            for (const auto &inst : result.instructions) {
                mcp::json instrJson;
                instrJson["address"] = mcp_json::hex16(inst.address);
                instrJson["mnemonic"] = inst.mnemonic;
                instrJson["operands"] = inst.operands;
                instrJson["size"] = inst.size;
                
                // Add bytes array
                mcp::json bytesArr = mcp::json::array();
                for (auto b : inst.bytes) bytesArr.push_back(b);
                instrJson["bytes"] = bytesArr;
                
                // Add branch info
                if (inst.branch_target.has_value()) {
                    instrJson["branch_target"] = inst.branch_target.value();
                } else {
                    instrJson["branch_target"] = nullptr;
                }
                
                if (inst.branch_type.empty()) {
                    instrJson["branch_type"] = nullptr;
                } else {
                    instrJson["branch_type"] = inst.branch_type;
                }
                
                instrs.push_back(instrJson);
            }
            
            return textContent({
                {"address",              mcp_json::hex16(result.startAddress)},
                {"size",                 result.size},
                {"instruction_count",   static_cast<int>(result.instructions.size())},
                {"incomplete_instruction", result.incomplete_instruction},
                {"instructions",         instrs}
            });
        });
    }
}

// ---------------------------------------------------------------------------
// Stack tools (1)
// ---------------------------------------------------------------------------

void McpServer::registerStackTools() {
    // debug_get_stack
    {
        auto tool = mcp::tool_builder("debug_get_stack")
            .with_description("Get the current stack contents.")
            .with_number_param("limit", "Maximum entries (default 32)", false)
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            size_t limit = getCount(params, "limit", 32);
            auto r = api_.getStack(limit);
            if (!r.success) return errorContent("get_stack_failed", r.error_message);
            return textContent({
                {"count",   static_cast<int>(r.value.size())},
                {"entries", mcp_json::stackEntriesToJson(r.value)}
            });
        });
    }
}

// ---------------------------------------------------------------------------
// Symbol / Analysis tools (5)
// ---------------------------------------------------------------------------

void McpServer::registerSymbolTools() {
    // debug_get_symbols
    {
        auto tool = mcp::tool_builder("debug_get_symbols")
            .with_description("List known symbols (functions and labels).")
            .with_number_param("limit", "Maximum symbols (0 = all)", false)
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            size_t limit = getCount(params, "limit", 0);
            auto r = api_.getSymbols(limit);
            if (!r.success) return errorContent("get_symbols_failed", r.error_message);
            return textContent({
                {"count",   static_cast<int>(r.value.size())},
                {"symbols", mcp_json::symbolInfosToJson(r.value)}
            });
        });
    }

    // debug_get_function
    {
        auto tool = mcp::tool_builder("debug_get_function")
            .with_description("Get symbol information for a function at an address.")
            .with_number_param("address", "Function start address (0..65535)")
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            uint16_t addr = getAddress(params);
            auto r = api_.getFunction(addr);
            if (!r.success) return errorContent("get_function_failed", r.error_message);
            return textContent(mcp_json::symbolInfoToJson(r.value));
        });
    }

    // debug_get_function_context
    {
        auto tool = mcp::tool_builder("debug_get_function_context")
            .with_description("Get full context for a function (disassembly, xrefs, trace data).")
            .with_number_param("address", "Function start address (0..65535)")
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            uint16_t addr = getAddress(params);
            auto r = api_.getFunctionContext(addr);
            if (!r.success) return errorContent("get_function_context_failed", r.error_message);
            return textContent(mcp_json::functionContextToJson(r.value));
        });
    }

    // debug_get_xrefs
    {
        auto tool = mcp::tool_builder("debug_get_xrefs")
            .with_description("Get cross-references to an address.")
            .with_number_param("address", "Target address (0..65535)")
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            uint16_t addr = getAddress(params);
            auto r = api_.getXrefs(addr);
            if (!r.success) return errorContent("get_xrefs_failed", r.error_message);
            return textContent({
                {"count", static_cast<int>(r.value.size())},
                {"xrefs", mcp_json::xrefResultsToJson(r.value)}
            });
        });
    }

    // debug_get_call_graph
    {
        auto tool = mcp::tool_builder("debug_get_call_graph")
            .with_description("Get the call graph edges.")
            .with_number_param("address", "Optional: filter by function address", false)
            .with_number_param("limit", "Maximum edges (0 = all)", false)
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            std::optional<uint16_t> addr;
            if (params.contains("address")) {
                addr = getAddress(params);
            }
            size_t limit = getCount(params, "limit", 0);
            auto r = api_.getCallGraph(addr, limit);
            if (!r.success) return errorContent("get_call_graph_failed", r.error_message);
            return textContent({
                {"count", static_cast<int>(r.value.size())},
                {"edges", mcp_json::callGraphEdgesToJson(r.value)}
            });
        });
    }
}

// ---------------------------------------------------------------------------
// Memory Map / Video tools (3)
// ---------------------------------------------------------------------------

void McpServer::registerMemoryMapTools() {
    // debug_get_memory_map
    {
        auto tool = mcp::tool_builder("debug_get_memory_map")
            .with_description("Get the 256-block memory map. "
                              "execute_activity = instructions started in the block "
                              "(cumulative); read_activity/write_activity are 0/1 flags.")
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            auto r = api_.getMemoryMap();
            if (!r.success) return errorContent("get_memory_map_failed", r.error_message);
            return textContent({
                {"count",  static_cast<int>(r.value.size())},
                {"blocks", mcp_json::memoryMapBlocksToJson(r.value)}
            });
        });
    }

    // debug_get_vram_info
    {
        auto tool = mcp::tool_builder("debug_get_vram_info")
            .with_description("Get VRAM layout information.")
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            auto r = api_.getVramInfo();
            if (!r.success) return errorContent("get_vram_info_failed", r.error_message);
            return textContent(mcp_json::vramInfoToJson(r.value));
        });
    }

    // debug_get_screen_info
    {
        auto tool = mcp::tool_builder("debug_get_screen_info")
            .with_description("Get current screen mode parameters.")
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            auto r = api_.getScreenInfo();
            if (!r.success) return errorContent("get_screen_info_failed", r.error_message);
            return textContent(mcp_json::screenInfoToJson(r.value));
        });
    }
}

// ---------------------------------------------------------------------------
// I/O Trace tools (1)
// ---------------------------------------------------------------------------

void McpServer::registerIoTraceTools() {
    // debug_get_io_trace
    {
        auto tool = mcp::tool_builder("debug_get_io_trace")
            .with_description("Get I/O port access trace events.")
            .with_number_param("max_entries", "Maximum entries (default 1000)", false)
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            size_t maxEntries = getCount(params, "max_entries", 1000);
            auto r = api_.getIoTrace(maxEntries);
            if (!r.success) return errorContent("get_io_trace_failed", r.error_message);
            return textContent({
                {"count",  static_cast<int>(r.value.size())},
                {"events", mcp_json::ioAccessEventsToJson(r.value)}
            });
        });
    }
}

// ---------------------------------------------------------------------------
// Debug State tools (1)
// ---------------------------------------------------------------------------

void McpServer::registerDebugStateTools() {
    // debug_get_state
    {
        auto tool = mcp::tool_builder("debug_get_state")
            .with_description("Get full debugger state snapshot. "
                              "'cycles' inside cpu_state is the T-state count of "
                              "the last executed instruction, not a total.")
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            auto r = api_.getDebugState();
            if (!r.success) return errorContent("get_state_failed", r.error_message);
            return textContent(mcp_json::debugStateToJson(r.value));
        });
    }
}

// ---------------------------------------------------------------------------
// ROM tools (1)
// ---------------------------------------------------------------------------

void McpServer::registerRomTools() {
    // debug_load_rom
    {
        auto tool = mcp::tool_builder("debug_load_rom")
            .with_description("Load a ROM file into memory.")
            .with_string_param("path", "Path to the ROM file")
            .with_number_param("org", "Origin address (default 0)", false)
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            if (!params.contains("path")) {
                throw mcp::mcp_exception(mcp::error_code::invalid_params, "missing required parameter: path");
            }
            std::string path = params["path"].get<std::string>();
            uint32_t org = 0;
            if (params.contains("org")) {
                int orgVal = params["org"].get<int>();
                if (orgVal < 0 || orgVal > 0xFFFF) {
                    throw mcp::mcp_exception(mcp::error_code::invalid_params, "org out of range 0..65535");
                }
                org = static_cast<uint32_t>(orgVal);
            }
            auto r = api_.loadRom(path, org);
            if (!r.success) return errorContent("load_rom_failed", r.error_message);
            return textContent(mcp_json::loadRomResultToJson(r.value));
        });
    }
}

// ---------------------------------------------------------------------------
// Annotation tools (6)
// ---------------------------------------------------------------------------

void McpServer::registerAnnotationTools() {
    // debug_set_comment
    {
        auto tool = mcp::tool_builder("debug_set_comment")
            .with_description("Set a comment at an address.")
            .with_number_param("address", "Address (0..65535)")
            .with_string_param("comment", "Comment text")
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            uint16_t addr = getAddress(params);
            if (!params.contains("comment")) {
                throw mcp::mcp_exception(mcp::error_code::invalid_params, "missing required parameter: comment");
            }
            std::string comment = params["comment"].get<std::string>();
            auto r = api_.setComment(addr, comment);
            if (!r.success) return errorContent("set_comment_failed", r.error_message);
            return textContent(mcp_json::successVoidResult());
        });
    }

    // debug_set_function_comment
    {
        auto tool = mcp::tool_builder("debug_set_function_comment")
            .with_description("Set a comment for a function.")
            .with_number_param("address", "Function address (0..65535)")
            .with_string_param("comment", "Comment text")
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            uint16_t addr = getAddress(params);
            if (!params.contains("comment")) {
                throw mcp::mcp_exception(mcp::error_code::invalid_params, "missing required parameter: comment");
            }
            std::string comment = params["comment"].get<std::string>();
            auto r = api_.setFunctionComment(addr, comment);
            if (!r.success) return errorContent("set_function_comment_failed", r.error_message);
            return textContent(mcp_json::successVoidResult());
        });
    }

    // debug_rename_function
    {
        auto tool = mcp::tool_builder("debug_rename_function")
            .with_description("Rename a function symbol (created via debug_create_function). "
                              "For RDB objects use debug_update_rdb_object instead.")
            .with_number_param("address", "Function address (0..65535)")
            .with_string_param("name", "New function name")
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            uint16_t addr = getAddress(params);
            if (!params.contains("name")) {
                throw mcp::mcp_exception(mcp::error_code::invalid_params, "missing required parameter: name");
            }
            std::string name = params["name"].get<std::string>();
            auto r = api_.renameFunction(addr, name);
            if (!r.success) return errorContent("rename_function_failed", r.error_message);
            return textContent(mcp_json::successVoidResult());
        });
    }

    // debug_create_function
    {
        auto tool = mcp::tool_builder("debug_create_function")
            .with_description("Create a new function at an address.")
            .with_number_param("address", "Function start address (0..65535)")
            .with_number_param("size", "Function size in bytes (0 = auto)", false)
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            uint16_t addr = getAddress(params);
            size_t size = getCount(params, "size", 0);
            auto r = api_.createFunction(addr, static_cast<uint16_t>(size));
            if (!r.success) return errorContent("create_function_failed", r.error_message);
            return textContent(mcp_json::successVoidResult());
        });
    }

    // debug_delete_function
    {
        auto tool = mcp::tool_builder("debug_delete_function")
            .with_description("Delete a function at an address.")
            .with_number_param("address", "Function address (0..65535)")
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            uint16_t addr = getAddress(params);
            auto r = api_.deleteFunction(addr);
            if (!r.success) return errorContent("delete_function_failed", r.error_message);
            return textContent(mcp_json::successVoidResult());
        });
    }

    // debug_add_label
    {
        auto tool = mcp::tool_builder("debug_add_label")
            .with_description("Add a label at an address.")
            .with_number_param("address", "Label address (0..65535)")
            .with_string_param("name", "Label name")
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            uint16_t addr = getAddress(params);
            if (!params.contains("name")) {
                throw mcp::mcp_exception(mcp::error_code::invalid_params, "missing required parameter: name");
            }
            std::string name = params["name"].get<std::string>();
            auto r = api_.addLabel(addr, name);
            if (!r.success) return errorContent("add_label_failed", r.error_message);
            return textContent(mcp_json::successVoidResult());
        });
    }
}

// ---------------------------------------------------------------------------
// RDB tools (Stage 6.11 — 11 tools)
// ---------------------------------------------------------------------------

void McpServer::registerRdbTools() {
    // debug_get_rdb_info
    {
        auto tool = mcp::tool_builder("debug_get_rdb_info")
            .with_description("Get ROM Database (RDB) info: path, platform, dirty state, object count, ROM identity.")
            .build();
        registerTool(tool, [this](const mcp::json &, const std::string &) -> mcp::json {
            auto r = api_.getRdbInfo();
            if (!r.success) return errorContent("get_rdb_info_failed", r.error_message);
            return textContent(mcp_json::rdbInfoToJson(r.value));
        });
    }

    // debug_list_rdb_objects
    {
        auto tool = mcp::tool_builder("debug_list_rdb_objects")
            .with_description("List all RDB objects sorted by address.")
            .with_number_param("limit", "Maximum objects (0 = all)", false)
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            size_t limit = getCount(params, "limit", 0);
            auto r = api_.listRdbObjects(limit);
            if (!r.success) return errorContent("list_rdb_objects_failed", r.error_message);
            return textContent({
                {"count",   static_cast<int>(r.value.size())},
                {"objects", mcp_json::rdbObjectsToJson(r.value)}
            });
        });
    }

    // debug_get_rdb_object
    {
        auto tool = mcp::tool_builder("debug_get_rdb_object")
            .with_description("Get a single RDB object by address.")
            .with_number_param("address", "Object address (0..65535)")
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            uint16_t addr = getAddress(params);
            auto r = api_.getRdbObject(addr);
            if (!r.success) return errorContent("get_rdb_object_failed", r.error_message);
            return textContent(mcp_json::rdbObjectToJson(r.value));
        });
    }

    // debug_find_rdb_object
    {
        auto tool = mcp::tool_builder("debug_find_rdb_object")
            .with_description("Find an RDB object by name (case-sensitive).")
            .with_string_param("name", "Object name to search for")
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            if (!params.contains("name")) {
                throw mcp::mcp_exception(mcp::error_code::invalid_params, "missing required parameter: name");
            }
            std::string name = params["name"].get<std::string>();
            auto r = api_.findRdbObject(name);
            if (!r.success) return errorContent("find_rdb_object_failed", r.error_message);
            return textContent(mcp_json::rdbObjectToJson(r.value));
        });
    }

    // debug_add_rdb_object
    {
        auto tool = mcp::tool_builder("debug_add_rdb_object")
            .with_description("Add a new object to the ROM Database.")
            .with_number_param("address", "Object address (0..65535)")
            .with_string_param("name", "Object name")
            .with_string_param("type", "Object type: Function, Variable, Data, Table, String, Code, Label, Unknown", false)
            .with_number_param("size", "Object size in bytes (0 = unknown)", false)
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            uint16_t addr = getAddress(params);
            if (!params.contains("name")) {
                throw mcp::mcp_exception(mcp::error_code::invalid_params, "missing required parameter: name");
            }
            std::string name = params["name"].get<std::string>();
            std::string type = "Label";
            if (params.contains("type")) {
                type = params["type"].get<std::string>();
            }
            uint32_t size = 0;
            if (params.contains("size")) {
                size = params["size"].get<int>();
            }
            auto r = api_.addRdbObject(addr, name, type, size);
            if (!r.success) return errorContent("add_rdb_object_failed", r.error_message);
            return textContent(mcp_json::successVoidResult());
        });
    }

    // debug_update_rdb_object
    {
        auto tool = mcp::tool_builder("debug_update_rdb_object")
            .with_description("Update an existing RDB object's name, type, and size. "
                              "Use this to rename RDB objects (not debug_rename_function, which is for function symbols only).")
            .with_number_param("address", "Object address (0..65535)")
            .with_string_param("name", "New name")
            .with_string_param("type", "New type")
            .with_number_param("size", "New size (0 = unknown)")
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            uint16_t addr = getAddress(params);
            if (!params.contains("name")) {
                throw mcp::mcp_exception(mcp::error_code::invalid_params, "missing required parameter: name");
            }
            if (!params.contains("type")) {
                throw mcp::mcp_exception(mcp::error_code::invalid_params, "missing required parameter: type");
            }
            std::string name = params["name"].get<std::string>();
            std::string type = params["type"].get<std::string>();
            uint32_t size = 0;
            if (params.contains("size")) {
                size = params["size"].get<int>();
            }
            bool hasSize = (size > 0);
            auto r = api_.updateRdbObject(addr, name, type, size, hasSize);
            if (!r.success) return errorContent("update_rdb_object_failed", r.error_message);
            return textContent(mcp_json::successVoidResult());
        });
    }

    // debug_remove_rdb_object
    {
        auto tool = mcp::tool_builder("debug_remove_rdb_object")
            .with_description("Remove an RDB object at an address.")
            .with_number_param("address", "Object address (0..65535)")
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            uint16_t addr = getAddress(params);
            auto r = api_.removeRdbObject(addr);
            if (!r.success) return errorContent("remove_rdb_object_failed", r.error_message);
            return textContent(mcp_json::successVoidResult());
        });
    }

    // debug_set_rdb_comment
    {
        auto tool = mcp::tool_builder("debug_set_rdb_comment")
            .with_description("Set a comment on an RDB object.")
            .with_number_param("address", "Object address (0..65535)")
            .with_string_param("comment", "Comment text")
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            uint16_t addr = getAddress(params);
            if (!params.contains("comment")) {
                throw mcp::mcp_exception(mcp::error_code::invalid_params, "missing required parameter: comment");
            }
            std::string comment = params["comment"].get<std::string>();
            auto r = api_.setRdbComment(addr, comment);
            if (!r.success) return errorContent("set_rdb_comment_failed", r.error_message);
            return textContent(mcp_json::successVoidResult());
        });
    }

    // debug_set_rdb_property
    {
        auto tool = mcp::tool_builder("debug_set_rdb_property")
            .with_description("Set a property on an RDB object.")
            .with_number_param("address", "Object address (0..65535)")
            .with_string_param("property", "Property name")
            .with_string_param("value", "Property value (stored as string)")
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            uint16_t addr = getAddress(params);
            if (!params.contains("property")) {
                throw mcp::mcp_exception(mcp::error_code::invalid_params, "missing required parameter: property");
            }
            if (!params.contains("value")) {
                throw mcp::mcp_exception(mcp::error_code::invalid_params, "missing required parameter: value");
            }
            std::string prop = params["property"].get<std::string>();
            std::string val  = params["value"].get<std::string>();
            auto r = api_.setRdbProperty(addr, prop, val);
            if (!r.success) return errorContent("set_rdb_property_failed", r.error_message);
            return textContent(mcp_json::successVoidResult());
        });
    }

    // debug_save_rdb
    {
        auto tool = mcp::tool_builder("debug_save_rdb")
            .with_description("Save the ROM Database to disk (.rdb file).")
            .build();
        registerTool(tool, [this](const mcp::json &, const std::string &) -> mcp::json {
            auto r = api_.saveRdb();
            if (!r.success) return errorContent("save_rdb_failed", r.error_message);
            return textContent(mcp_json::successVoidResult());
        });
    }

    // debug_reload_rdb
    {
        auto tool = mcp::tool_builder("debug_reload_rdb")
            .with_description("Reload the ROM Database from disk (discard unsaved changes).")
            .build();
        registerTool(tool, [this](const mcp::json &, const std::string &) -> mcp::json {
            auto r = api_.reloadRdb();
            if (!r.success) return errorContent("reload_rdb_failed", r.error_message);
            return textContent(mcp_json::successVoidResult());
        });
    }

    // debug_add_rdb_link (Stage 6.13)
    {
        auto tool = mcp::tool_builder("debug_add_rdb_link")
            .with_description("Add a directed link from source RDB object to target address.")
            .with_number_param("source", "Source object address (0..65535)")
            .with_number_param("target", "Target address (0..65535)")
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            uint16_t source = getAddress(params, "source");
            uint16_t target = getAddress(params, "target");
            auto r = api_.addRdbLink(source, target);
            if (!r.success) return errorContent("add_rdb_link_failed", r.error_message);
            return textContent(mcp_json::successVoidResult());
        });
    }

    // debug_remove_rdb_link (Stage 6.13)
    {
        auto tool = mcp::tool_builder("debug_remove_rdb_link")
            .with_description("Remove a directed link from source RDB object to target address.")
            .with_number_param("source", "Source object address (0..65535)")
            .with_number_param("target", "Target address (0..65535)")
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            uint16_t source = getAddress(params, "source");
            uint16_t target = getAddress(params, "target");
            auto r = api_.removeRdbLink(source, target);
            if (!r.success) return errorContent("remove_rdb_link_failed", r.error_message);
            return textContent(mcp_json::successVoidResult());
        });
    }

    // debug_get_rdb_links (Stage 6.13)
    {
        auto tool = mcp::tool_builder("debug_get_rdb_links")
            .with_description("Get links from a source RDB object (list of target addresses).")
            .with_number_param("source", "Source object address (0..65535)")
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            uint16_t source = getAddress(params, "source");
            auto r = api_.getRdbLinks(source);
            if (!r.success) return errorContent("get_rdb_links_failed", r.error_message);
            return textContent(mcp_json::rdbLinksToJson(source, r.value));
        });
    }
}

// ---------------------------------------------------------------------------
// Runtime Memory Analysis tools (Stage 6.20) — 5 tools
// ---------------------------------------------------------------------------

void McpServer::registerRuntimeAnalysisTools() {
    // debug_clear_memory_access_map
    {
        auto tool = mcp::tool_builder("debug_clear_memory_access_map")
            .with_description("Clear the runtime memory access map and access log. "
                              "Does not reset CPU, Board, or unload ROM.")
            .build();
        registerTool(tool, [this](const mcp::json &, const std::string &) -> mcp::json {
            auto r = api_.clearMemoryAccessMap();
            if (!r.success) return errorContent("clear_memory_access_map_failed", r.error_message);
            return textContent(mcp_json::successVoidResult());
        });
    }

    // debug_get_memory_access_map
    {
        auto tool = mcp::tool_builder("debug_get_memory_access_map")
            .with_description("Get the runtime memory access map (256 blocks of 256 bytes). "
                              "Only active blocks (with read/write/fetch activity) are returned.")
            .build();
        registerTool(tool, [this](const mcp::json &, const std::string &) -> mcp::json {
            auto r = api_.getMemoryAccessMap();
            if (!r.success) return errorContent("get_memory_access_map_failed", r.error_message);
            // Stage 6.22 §3: "active_blocks" used to be the size of the whole
            // 256-entry map while the payload was filtered by activity, so the
            // two disagreed by orders of magnitude.  Count with the very same
            // predicate the payload uses.
            int active = 0;
            for (const auto &b : r.value)
                if (mcp_json::runtimeBlockActive(b)) ++active;
            return textContent({
                {"total_blocks", static_cast<int>(r.value.size())},
                {"active_blocks", active},
                {"blocks", mcp_json::runtimeAccessBlocksToJson(r.value)}
            });
        });
    }

    // debug_get_memory_access_log
    {
        auto tool = mcp::tool_builder("debug_get_memory_access_log")
            .with_description("Get the bounded runtime memory access log. "
                              "Each entry has address, type (read/write/fetch), PC, and value.")
            .with_number_param("max_entries", "Maximum entries to return (default 1000)", false)
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            size_t maxEntries = getCount(params, "max_entries", 1000);
            auto r = api_.getMemoryAccessLog(maxEntries);
            if (!r.success) return errorContent("get_memory_access_log_failed", r.error_message);
            return textContent({
                {"count", static_cast<int>(r.value.size())},
                {"entries", mcp_json::runtimeAccessLogEntriesToJson(r.value)}
            });
        });
    }

    // debug_create_memory_snapshot
    {
        auto tool = mcp::tool_builder("debug_create_memory_snapshot")
            .with_description("Create a snapshot of the current memory state. "
                              "Returns a snapshot_id for later comparison.")
            .with_number_param("address", "Start address (default 0)", false)
            .with_number_param("size", "Number of bytes (default 65536 = full 64K)", false)
            .build();
        addNumericConstraint(tool, "address", 0, 65535);
        addNumericConstraint(tool, "size", 1, 65536);
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            uint16_t addr = 0;
            size_t size = 65536;
            if (params.contains("address")) {
                addr = static_cast<uint16_t>(params["address"].get<int>());
            }
            if (params.contains("size")) {
                size = static_cast<size_t>(params["size"].get<int>());
            }
            auto r = api_.createMemorySnapshot(addr, size);
            if (!r.success) return errorContent("create_memory_snapshot_failed", r.error_message);
            return textContent({
                {"snapshot_id", static_cast<int>(r.value)},
                {"address", mcp_json::hex16(addr)},
                {"size", static_cast<int>(size)}
            });
        });
    }

    // debug_compare_memory_snapshots
    {
        auto tool = mcp::tool_builder("debug_compare_memory_snapshots")
            .with_description("Compare two memory snapshots. Returns changed ranges as merged contiguous blocks.")
            .with_number_param("snapshot_a", "First snapshot ID")
            .with_number_param("snapshot_b", "Second snapshot ID")
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            if (!params.contains("snapshot_a") || !params.contains("snapshot_b")) {
                throw mcp::mcp_exception(mcp::error_code::invalid_params,
                    "missing required parameters: snapshot_a, snapshot_b");
            }
            uint32_t idA = static_cast<uint32_t>(params["snapshot_a"].get<int>());
            uint32_t idB = static_cast<uint32_t>(params["snapshot_b"].get<int>());
            auto r = api_.compareMemorySnapshots(idA, idB);
            if (!r.success) return errorContent("compare_memory_snapshots_failed", r.error_message);
            auto diffJson = mcp_json::memorySnapshotDiffToJson(r.value);
            diffJson["snapshot_a"] = static_cast<int>(idA);
            diffJson["snapshot_b"] = static_cast<int>(idB);
            return textContent(diffJson);
        });
    }
}

// ---------------------------------------------------------------------------
// Batch analysis tools (Stage 6.26) — 6 tools
//
// MCP-сервер только предоставляет данные и агрегирует существующие операции.
// Классификация code/data/tables/strings, именование и семантика остаются за
// клиентом (Python-анализатор / AI-агент).
// ---------------------------------------------------------------------------

// Structured ErrorCode from the shared Agent API model (§19).
static const char *agentErrorCodeName(ErrorCode code) {
    switch (code) {
    case ErrorCode::None:            return "none";
    case ErrorCode::InvalidArgument: return "invalid_argument";
    case ErrorCode::InvalidAddress:  return "invalid_address";
    case ErrorCode::InvalidRange:    return "invalid_range";
    case ErrorCode::NoRomLoaded:     return "no_rom_loaded";
    case ErrorCode::NotPaused:       return "not_paused";
    case ErrorCode::NotRunning:      return "not_running";
    case ErrorCode::OperationFailed: return "operation_failed";
    case ErrorCode::Timeout:         return "timeout";
    case ErrorCode::Unsupported:     return "unsupported";
    case ErrorCode::NotFound:        return "not_found";
    case ErrorCode::LimitExceeded:   return "limit_exceeded";
    }
    return "operation_failed";
}

// Instruction JSON identical in shape to debug_disassemble_range (§5.2).
static mcp::json rangeInstructionJson(const DisassembledRangeInstruction &inst) {
    mcp::json instrJson;
    instrJson["address"]  = mcp_json::hex16(inst.address);
    instrJson["mnemonic"] = inst.mnemonic;
    instrJson["operands"] = inst.operands;
    instrJson["size"]     = inst.size;

    mcp::json bytesArr = mcp::json::array();
    for (auto b : inst.bytes) bytesArr.push_back(b);
    instrJson["bytes"] = bytesArr;

    if (inst.branch_target.has_value()) {
        instrJson["branch_target"] = inst.branch_target.value();
    } else {
        instrJson["branch_target"] = nullptr;
    }

    if (inst.branch_type.empty()) {
        instrJson["branch_type"] = nullptr;
    } else {
        instrJson["branch_type"] = inst.branch_type;
    }
    return instrJson;
}

// Parse a JSON array of byte-sized ints into a byte vector.
static std::vector<uint8_t> getByteArray(const mcp::json &params, const char *name,
                                          bool required, size_t maxLen) {
    if (!params.contains(name)) {
        if (required) {
            throw mcp::mcp_exception(mcp::error_code::invalid_params,
                std::string("missing required parameter: ") + name);
        }
        return {};
    }
    if (!params[name].is_array()) {
        throw mcp::mcp_exception(mcp::error_code::invalid_params,
            std::string(name) + " must be an array");
    }
    const auto &arr = params[name];
    if (arr.size() > maxLen) {
        throw mcp::mcp_exception(mcp::error_code::invalid_params,
            std::string(name) + " exceeds maximum length (" +
            std::to_string(maxLen) + ")");
    }
    std::vector<uint8_t> out;
    out.reserve(arr.size());
    for (const auto &v : arr) {
        int b = v.get<int>();
        if (b < 0 || b > 0xFF) {
            throw mcp::mcp_exception(mcp::error_code::invalid_params,
                std::string(name) + " values must be 0..255");
        }
        out.push_back(static_cast<uint8_t>(b));
    }
    return out;
}

// Inclusive 16-bit range pair [range_start, range_end].
static void getRangeEndpoints(const mcp::json &params, uint16_t &start, uint16_t &end) {
    start = getAddress(params, "range_start");
    end   = getAddress(params, "range_end");
    if (end < start) {
        throw mcp::mcp_exception(mcp::error_code::invalid_params,
            "range_end must be >= range_start");
    }
}

void McpServer::registerBatchAnalysisTools() {
    // debug_disassemble_image (Stage 6.26 §5)
    {
        auto tool = mcp::tool_builder("debug_disassemble_image")
            .with_description("Batch linear disassembly of a large ROM-image range in one call. "
                              "Result equals debug_disassemble_range over the same range. "
                              "Does NOT classify code/data and does NOT modify RDB.")
            .with_number_param("address", "Start address (0..65535)")
            .with_number_param("length", "Number of bytes (1..65536)")
            .build();
        addNumericConstraint(tool, "address", 0, 65535);
        addNumericConstraint(tool, "length", 1, static_cast<int>(AgentLimits::MAX_DISASSEMBLE_IMAGE_LENGTH));
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            uint16_t addr = getAddress(params);
            uint32_t length = static_cast<uint32_t>(getCount(params, "length", 0));
            if (length == 0) {
                throw mcp::mcp_exception(mcp::error_code::invalid_params, "length must be >= 1");
            }

            auto r = api_.disassembleImage(addr, length);
            if (!r.success) return errorContent(agentErrorCodeName(r.error_code), r.error_message);

            const auto &result = r.value;
            mcp::json instrs = mcp::json::array();
            for (const auto &inst : result.instructions) {
                instrs.push_back(rangeInstructionJson(inst));
            }
            return textContent({
                {"address",                mcp_json::hex16(addr)},
                {"length",                 static_cast<int>(length)},
                {"instruction_count",      static_cast<int>(result.instructions.size())},
                {"incomplete_instruction", result.incomplete_instruction},
                {"max_instructions_limit", static_cast<int>(AgentLimits::MAX_DISASSEMBLE_IMAGE_INSTRUCTIONS)},
                {"instructions",           instrs}
            });
        });
    }

    // debug_coverage_report (Stage 6.26 §6)
    {
        auto tool = mcp::tool_builder("debug_coverage_report")
            .with_description("Aggregate code-coverage report: analyzed code ranges, uncovered gaps, "
                              "branch targets, and branch targets WITHOUT analyzed code (JCC blind spots). "
                              "Runs control-flow analysis from entry points; does NOT modify RDB. "
                              "'start_address' and 'addresses' may be combined.")
            .with_number_param("start_address", "Single entry point (optional)", false)
            .with_array_param("addresses", "Extra entry points (optional)", "integer", false)
            .with_number_param("range_start", "Image range start (default 0)", false)
            .with_number_param("range_length", "Image range length (default 65536)", false)
            .with_number_param("max_instructions", "Analysis budget (default 10000)", false)
            .build();
        addNumericConstraint(tool, "start_address", 0, 65535);
        addNumericConstraint(tool, "range_start", 0, 65535);
        addNumericConstraint(tool, "range_length", 1, 65536);
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            // §15: the batch API hides analyzeCode's param exclusivity —
            // start_address and addresses are simply merged into one vector.
            std::vector<uint16_t> entryPoints;
            if (params.contains("start_address")) {
                entryPoints.push_back(getAddress(params, "start_address"));
            }
            if (params.contains("addresses")) {
                if (!params["addresses"].is_array()) {
                    throw mcp::mcp_exception(mcp::error_code::invalid_params,
                        "'addresses' must be an array");
                }
                for (const auto &v : params["addresses"]) {
                    int addr = v.get<int>();
                    if (addr < 0 || addr > 65535) {
                        throw mcp::mcp_exception(mcp::error_code::invalid_params,
                            "each address must be 0..65535");
                    }
                    entryPoints.push_back(static_cast<uint16_t>(addr));
                }
            }
            if (entryPoints.empty()) {
                throw mcp::mcp_exception(mcp::error_code::invalid_params,
                    "at least one of 'start_address' / 'addresses' is required");
            }

            uint16_t rangeStart = params.contains("range_start")
                ? getAddress(params, "range_start") : 0;
            uint32_t rangeLength = static_cast<uint32_t>(
                getCount(params, "range_length", 65536));
            size_t maxInstr = getCount(params, "max_instructions",
                                        AgentLimits::MAX_CODE_ANALYSIS_INSTRUCTIONS);

            auto r = api_.coverageReport(entryPoints, rangeStart, rangeLength, maxInstr);
            if (!r.success) return errorContent(agentErrorCodeName(r.error_code), r.error_message);

            const auto &rep = r.value;
            auto rangesJson = [](const std::vector<CoverageRange> &ranges) {
                mcp::json out = mcp::json::array();
                for (const auto &rg : ranges) {
                    out.push_back({{"start", mcp_json::hex16(rg.start)},
                                   {"end",   mcp_json::hex16(rg.end)},
                                   {"size",  static_cast<int>(rg.end) - rg.start + 1}});
                }
                return out;
            };

            mcp::json targets = mcp::json::array();
            for (const auto &bt : rep.branchTargets) {
                targets.push_back({{"from", mcp_json::hex16(bt.from)},
                                   {"to",   mcp_json::hex16(bt.to)},
                                   {"type", bt.type}});
            }
            mcp::json uncoveredTargets = mcp::json::array();
            for (auto a : rep.uncoveredBranchTargets) {
                uncoveredTargets.push_back(mcp_json::hex16(a));
            }

            double percent = rep.imageBytes > 0
                ? (100.0 * static_cast<double>(rep.codeBytes) /
                   static_cast<double>(rep.imageBytes))
                : 0.0;

            mcp::json imageObj = {{"start", mcp_json::hex16(rep.imageStart)},
                                  {"end",   mcp_json::hex16(rep.imageEnd)}};
            mcp::json statsObj = {
                {"instruction_count", static_cast<int>(rep.instructionCount)},
                {"code_bytes",        static_cast<int>(rep.codeBytes)},
                {"image_bytes",       static_cast<int>(rep.imageBytes)},
                {"coverage_percent",  static_cast<int>(percent * 100 + 0.5) / 100.0},
                {"truncated",         rep.truncated}
            };

            return textContent({
                {"image",                     imageObj},
                {"code_ranges",               rangesJson(rep.codeRanges)},
                {"uncovered_ranges",          rangesJson(rep.uncoveredRanges)},
                {"branch_targets",            targets},
                {"uncovered_branch_targets",  uncoveredTargets},
                {"stats",                     statsObj}
            });
        });
    }

    // debug_diff_memory (Stage 6.26 §7)
    {
        auto tool = mcp::tool_builder("debug_diff_memory")
            .with_description("Complete byte-for-byte diff of two memory snapshots over a range, "
                              "with old/new values per contiguous range. Unlike "
                              "debug_compare_memory_snapshots, a dense diff that exceeds the "
                              "limits is a limit_exceeded error — never a silent under-report.")
            .with_number_param("snapshot_a", "First snapshot ID")
            .with_number_param("snapshot_b", "Second snapshot ID")
            .with_number_param("address", "Range start (default: covered by both snapshots)", false)
            .with_number_param("length", "Range length (default: common covered range)", false)
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            if (!params.contains("snapshot_a") || !params.contains("snapshot_b")) {
                throw mcp::mcp_exception(mcp::error_code::invalid_params,
                    "missing required parameters: snapshot_a, snapshot_b");
            }
            uint32_t idA = static_cast<uint32_t>(params["snapshot_a"].get<int>());
            uint32_t idB = static_cast<uint32_t>(params["snapshot_b"].get<int>());

            uint16_t start = params.contains("address")
                ? getAddress(params, "address") : 0;
            uint32_t length = params.contains("length")
                ? static_cast<uint32_t>(params["length"].get<int>()) : 0;
            if (length == 0) {
                // Default: full range of snapshot A clipped to snapshot B.
                auto snapA = api_.getMemorySnapshot(idA);
                if (!snapA.success) {
                    return errorContent(agentErrorCodeName(snapA.error_code), snapA.error_message);
                }
                start = snapA.value.start_address;
                length = static_cast<uint32_t>(snapA.value.data.size());
            }

            auto r = api_.diffMemorySnapshots(idA, idB, start, length);
            if (!r.success) return errorContent(agentErrorCodeName(r.error_code), r.error_message);

            const auto &diff = r.value;
            mcp::json ranges = mcp::json::array();
            for (const auto &rg : diff.ranges) {
                mcp::json oldArr = mcp::json::array(), newArr = mcp::json::array();
                for (auto b : rg.oldBytes) oldArr.push_back(b);
                for (auto b : rg.newBytes) newArr.push_back(b);
                ranges.push_back({
                    {"address", mcp_json::hex16(rg.address)},
                    {"size",    static_cast<int>(rg.oldBytes.size())},
                    {"old",     oldArr},
                    {"new",     newArr}
                });
            }
            return textContent({
                {"start",           mcp_json::hex16(diff.start)},
                {"length",          static_cast<int>(diff.length)},
                {"changed_bytes",   static_cast<int>(diff.changedBytes)},
                {"range_count",     static_cast<int>(diff.ranges.size())},
                {"snapshot_a",      static_cast<int>(idA)},
                {"snapshot_b",      static_cast<int>(idB)},
                {"ranges",          ranges}
            });
        });
    }

    // debug_find_bytecode_sequence (Stage 6.26 §8)
    {
        auto tool = mcp::tool_builder("debug_find_bytecode_sequence")
            .with_description("Find a byte pattern (optionally masked) in a memory range. "
                              "Returns matching addresses only — no interpretation of what the "
                              "bytes mean. Overlapping matches are reported. Range is inclusive.")
            .with_number_param("range_start", "First address to scan (0..65535)")
            .with_number_param("range_end", "Last address to scan, inclusive (0..65535)")
            .with_array_param("pattern", "Byte values to find (0..255 each, max 32)", "integer", true)
            .with_array_param("mask", "Optional per-byte mask: 0 = wildcard, else must match", "integer", false)
            .with_number_param("max_matches", "Maximum matches (default 1000, hard cap 10000)", false)
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            uint16_t start, end;
            getRangeEndpoints(params, start, end);
            auto pattern = getByteArray(params, "pattern", true,
                                        AgentLimits::MAX_BYTECODE_PATTERN_LENGTH);
            auto mask    = getByteArray(params, "mask", false,
                                        AgentLimits::MAX_BYTECODE_PATTERN_LENGTH);
            size_t maxMatches = getCount(params, "max_matches", AgentLimits::MAX_SEARCH_MATCHES);

            auto r = api_.findBytecodeSequence(start, end, pattern, mask, maxMatches);
            if (!r.success) return errorContent(agentErrorCodeName(r.error_code), r.error_message);

            mcp::json addresses = mcp::json::array();
            for (auto a : r.value.addresses) addresses.push_back(mcp_json::hex16(a));
            return textContent({
                {"range_start",   mcp_json::hex16(r.value.rangeStart)},
                {"range_end",     mcp_json::hex16(r.value.rangeEnd)},
                {"scanned_bytes", static_cast<int>(r.value.scannedBytes)},
                {"match_count",   static_cast<int>(r.value.addresses.size())},
                {"addresses",     addresses}
            });
        });
    }

    // debug_find_immediate_in_range (Stage 6.26 §9)
    {
        auto tool = mcp::tool_builder("debug_find_immediate_in_range")
            .with_description("Find instructions whose numeric operand (immediate or address) "
                              "equals the given value. Instruction interpretation is done by the "
                              "Debugger disassembler (linear sweep). Range is inclusive.")
            .with_number_param("range_start", "First address to scan (0..65535)")
            .with_number_param("range_end", "Last address to scan, inclusive (0..65535)")
            .with_number_param("value", "Operand value to match (0..65535)")
            .with_number_param("max_matches", "Maximum matches (default 1000, hard cap 10000)", false)
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            uint16_t start, end;
            getRangeEndpoints(params, start, end);
            uint16_t value = getAddress(params, "value");
            size_t maxMatches = getCount(params, "max_matches", AgentLimits::MAX_SEARCH_MATCHES);

            auto r = api_.findImmediateInRange(start, end, value, maxMatches);
            if (!r.success) return errorContent(agentErrorCodeName(r.error_code), r.error_message);

            mcp::json matches = mcp::json::array();
            for (const auto &m : r.value.matches) {
                mcp::json bytesArr = mcp::json::array();
                for (auto b : m.bytes) bytesArr.push_back(b);
                std::string text = m.operands.empty() ? m.mnemonic
                                                      : m.mnemonic + " " + m.operands;
                matches.push_back({
                    {"address",  mcp_json::hex16(m.address)},
                    {"mnemonic", m.mnemonic},
                    {"operands", m.operands},
                    {"text",     text},
                    {"bytes",    bytesArr}
                });
            }
            return textContent({
                {"range_start", mcp_json::hex16(r.value.rangeStart)},
                {"range_end",   mcp_json::hex16(r.value.rangeEnd)},
                {"value",       mcp_json::hex16(r.value.value)},
                {"match_count", static_cast<int>(r.value.matches.size())},
                {"matches",     matches}
            });
        });
    }

    // debug_get_vram_bytes (Stage 6.26 §10)
    {
        auto tool = mcp::tool_builder("debug_get_vram_bytes")
            .with_description("Read raw VRAM bytes from the memory-mapped VRAM region "
                              "(0x8000..0xFFFF, four 8K bit-planes). Contents only — no "
                              "decoding of pixel semantics.")
            .with_number_param("address", "VRAM address (0x8000..0xFFFF)")
            .with_number_param("length", "Number of bytes (1..32768)")
            .build();
        addNumericConstraint(tool, "address", 0x8000, 0xFFFF);
        addNumericConstraint(tool, "length", 1, static_cast<int>(AgentLimits::MAX_VRAM_READ_RANGE));
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            uint16_t addr = getAddress(params);
            uint32_t length = static_cast<uint32_t>(getCount(params, "length", 0));
            if (length == 0) {
                throw mcp::mcp_exception(mcp::error_code::invalid_params, "length must be >= 1");
            }

            auto r = api_.getVramBytes(addr, length);
            if (!r.success) return errorContent(agentErrorCodeName(r.error_code), r.error_message);

            mcp::json bytesArr = mcp::json::array();
            for (auto b : r.value) bytesArr.push_back(b);
            return textContent({
                {"address", mcp_json::hex16(addr)},
                {"length",  static_cast<int>(r.value.size())},
                {"bytes",   bytesArr}
            });
        });
    }
}

// ---------------------------------------------------------------------------
// Raster / beam debugging tools (Stage 6.27)
//
// Thin adapters: every value is produced by DebugAdapter and returned via the
// Agent API. MCP performs no video-timing or rendering computation itself.
// Read-only: calling these never pauses, steps, resets or re-renders.
// ---------------------------------------------------------------------------

void McpServer::registerRasterTools() {
    // debug_get_beam_state — current beam/raster position + video timing +
    // palette under the beam.
    {
        auto tool = mcp::tool_builder("debug_get_beam_state")
            .with_description(
                "Read-only snapshot of the video beam/raster position (frame, "
                "raster_line, v_cycle), the CPU instruction executing beside the "
                "beam, and the palette entry the video path is currently shifting "
                "out. Essential for debugging racing-the-beam / raster effects. "
                "Does NOT pause or re-render the emulation.")
            .build();
        registerTool(tool, [this](const mcp::json &, const std::string &) -> mcp::json {
            auto r = api_.getBeamState();
            if (!r.success) return errorContent(agentErrorCodeName(r.error_code), r.error_message);
            return textContent(mcp_json::beamStateToJson(r.value));
        });
    }

    // debug_get_screen_snapshot — the real TV framebuffer as a PNG image.
    {
        auto tool = mcp::tool_builder("debug_get_screen_snapshot")
            .with_description(
                "Capture the currently-displayed frame from the emulator's real "
                "TV framebuffer and return it as a PNG image plus JSON metadata "
                "(frame id, dimensions, source). This is the composed output — "
                "NOT a reconstruction from VRAM — so it reveals mid-frame "
                "raster/palette effects that a VRAM read alone cannot. Does NOT "
                "pause or re-render the emulation.")
            .build();
        registerTool(tool, [this](const mcp::json &, const std::string &) -> mcp::json {
            auto r = api_.getScreenSnapshot();
            if (!r.success) return errorContent(agentErrorCodeName(r.error_code), r.error_message);

            // Build a CallToolResult content array by hand:
            //   [ image/png (base64, best-effort), text metadata ]
            mcp::json meta = mcp_json::screenSnapshotToJson(r.value);
            mcp::json content = mcp::json::array();
            if (!r.value.pngBase64.empty()) {
                content.push_back({
                    {"type", "image"},
                    {"data", r.value.pngBase64},
                    {"mimeType", "image/png"}
                });
            }
            content.push_back({{"type", "text"}, {"text", meta.dump(2)}});
            return content;
        });
    }

    // debug_get_raster_events — OUT instructions correlated with beam position.
    {
        auto tool = mcp::tool_builder("debug_get_raster_events")
            .with_description(
                "Return recorded OUT instructions with the exact beam position "
                "(frame, raster_line, v_cycle) and PC at the moment each executed. "
                "This is the accurate mid-frame path for racing-the-beam analysis: "
                "match a palette write (port 0x0C-0x0F) to the line/pixel it lands "
                "on. Filters are optional: frame, v_cycle range, port, pc, "
                "max_results. Read-only.")
            .with_number_param("frame", "Filter by frame number (0 = any)", false)
            .with_number_param("v_cycle_start", "Filter v_cycle >= this (default 0)", false)
            .with_number_param("v_cycle_end", "Filter v_cycle <= this (default 4294967295)", false)
            .with_number_param("port", "Filter by I/O port (-1/absent = any)", false)
            .with_number_param("pc", "Filter by PC (absent = any)", false)
            .with_number_param("max_results", "Max events (default 1000, cap 50000)", false)
            .build();
        registerTool(tool, [this](const mcp::json &params, const std::string &) -> mcp::json {
            auto optInt = [&params](const char *name, int def) -> int {
                if (!params.contains(name) || params[name].is_null()) return def;
                return params[name].get<int>();
            };
            uint64_t frame      = static_cast<uint64_t>(optInt("frame", 0));
            uint32_t vStart     = static_cast<uint32_t>(optInt("v_cycle_start", 0));
            uint32_t vEnd       = static_cast<uint32_t>(
                                      optInt("v_cycle_end", -1)); // -1 -> 0xFFFFFFFF
            int      port       = optInt("port", -1);
            uint16_t pc         = static_cast<uint16_t>(optInt("pc", 0xFFFF));
            size_t   maxResults = static_cast<size_t>(
                                      optInt("max_results",
                                             static_cast<int>(AgentLimits::RASTER_EVENTS_DEFAULT_LIMIT)));

            auto r = api_.getRasterEvents(frame, vStart, vEnd, port, pc, maxResults);
            if (!r.success) return errorContent(agentErrorCodeName(r.error_code), r.error_message);

            mcp::json events = mcp::json::array();
            for (const auto &e : r.value) {
                events.push_back({
                    {"frame",           e.frame},
                    {"v_cycle",         e.vCycle},
                    {"raster_line",     e.rasterLine},
                    {"v_cycle_in_line", e.vCycleInLine},
                    {"pc",              mcp_json::hex16(e.pc)},
                    {"port",            e.port},
                    {"value",           e.value}
                });
            }
            return textContent({
                {"count",  static_cast<int>(events.size())},
                {"events", events}
            });
        });
    }
}

// ---------------------------------------------------------------------------
// Server tools (1) — build provenance
//
// debug_get_server_info answers "which binary is this session actually
// talking to?": an IDE stdio session keeps running the process it spawned,
// even after the on-disk executable has been rebuilt (the old inode stays
// alive as "(deleted)"). Comparing buildSeq/gitHash against the current
// build/ directory exposes that stale-server state in one call.
// ---------------------------------------------------------------------------

void McpServer::registerServerTools() {
    // debug_get_server_info
    {
        auto tool = mcp::tool_builder("debug_get_server_info")
            .with_description("Return MCP server build provenance (version, git hash, build sequence, start time, pid) to verify which binary the session is talking to.")
            .build();
        registerTool(tool, [this](const mcp::json &, const std::string &) -> mcp::json {
            // ISO-8601 UTC formatter for the time_t stamps.
            auto isoUtc = [](std::time_t t) -> std::string {
                char buf[32];
                std::tm tm{};
                gmtime_r(&t, &tm);
                std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
                return buf;
            };
            std::time_t now = std::time(nullptr);
            return textContent({
                {"name",       "v06c-mcp"},
                {"version",    VDB_VERSION},
                {"gitHash",    VDB_GIT_HASH},
                {"gitState",   VDB_GIT_DIRTY},
                {"buildSeq",   VDB_BUILD_SEQ},
                {"buildTime",  VDB_BUILD_TIME},
                {"apiVersion", 4},
                {"pid",        static_cast<int>(::getpid())},
                {"startedAt",  isoUtc(bootTime_)},
                {"uptimeSec",  static_cast<int>(now - bootTime_)}
            });
        });
    }
}


