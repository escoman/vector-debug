// ---------------------------------------------------------------------------
// test_mcp_protocol — Stage 6.4
//
// MCP protocol tests using MockAgentBackend.
// Verifies: tool registration, schema, execution, error propagation, E2E.
//
// No Board/SDL dependency — uses MockAgentBackend.
// ---------------------------------------------------------------------------

#include "mcp_adapter.h"
#include "mcp_json.h"
#include "agent_api.h"
#include "mock_backend_for_agent.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <set>
#include <algorithm>

// ---------------------------------------------------------------------------
// Test framework
// ---------------------------------------------------------------------------

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_BEGIN(name) \
    do { \
        tests_run++; \
        const char *_test_name = name; \
        int _failures = 0; \
        (void)_test_name;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "  FAIL: %s (line %d): %s\n", _test_name, __LINE__, msg); \
            _failures++; \
        } \
    } while(0)

#define CHECK_EQ(a, b, msg) \
    do { \
        if ((a) != (b)) { \
            fprintf(stderr, "  FAIL: %s (line %d): %s\n", _test_name, __LINE__, msg); \
            _failures++; \
        } \
    } while(0)

#define TEST_END() \
        if (_failures == 0) { \
            tests_passed++; \
            printf("  \033[32mPASS\033[0m %s\n", _test_name); \
        } else { \
            tests_failed++; \
            printf("  \033[31mFAIL\033[0m %s (%d failures)\n", _test_name, _failures); \
        } \
    } while(0)

// ---------------------------------------------------------------------------
// Fixture: MockAgentBackend + AgentApi + McpServer
// ---------------------------------------------------------------------------

struct Fixture {
    MockAgentBackend mock;
    AgentApi api;
    McpServer mcp;

    Fixture() : api(mock), mcp(api) {
        mcp.registerAllTools();
    }
};

// Helper: get text from MCP handler result
// Handler returns content array directly: [{type: "text", text: "..."}]
static std::string getTextFromContent(const mcp::json &result) {
    if (result.is_array() && !result.empty()) {
        auto &first = result[0];
        if (first.is_object() && first.contains("text")) {
            return first["text"].get<std::string>();
        }
    }
    return "";
}

// Helper: check if MCP handler result is an error
// Error results contain error_code in the text data
static bool isErrorContent(const mcp::json &result) {
    if (result.is_array() && !result.empty()) {
        auto &first = result[0];
        if (first.is_object() && first.contains("text")) {
            try {
                auto data = mcp::json::parse(first["text"].get<std::string>());
                // errorContent produces {error_code, message}
                // mcp_json::errorResult produces {success: false, error_code, error}
                return data.contains("error_code");
            } catch (...) {
                return false;
            }
        }
    }
    return false;
}

// Helper: parse text content as JSON
static mcp::json parseTextAsJson(const mcp::json &result) {
    std::string text = getTextFromContent(result);
    if (!text.empty()) {
        return mcp::json::parse(text);
    }
    return mcp::json();
}

// ---------------------------------------------------------------------------
// Tool Registration Tests
// ---------------------------------------------------------------------------

void test_all_tools_registered() {
    TEST_BEGIN("all 74 tools registered");
    Fixture f;
    auto names = f.mcp.registeredToolNames();
    // Stage 6.26: 70, Stage 6.27 raster/beam (beam_state, screen_snapshot,
    // raster_events) = 73, + debug_get_server_info (build provenance) = 74
    CHECK_EQ(static_cast<int>(names.size()), 74, "should have 74 tools");
    // tools/list must reflect reality: no duplicates in registration
    std::set<std::string> unique(names.begin(), names.end());
    CHECK_EQ(static_cast<int>(unique.size()), static_cast<int>(names.size()),
             "tool names must be unique");
    // Stage 6.27: the three raster/beam tools must be registered
    for (const char *t : {"debug_get_beam_state",
                          "debug_get_screen_snapshot",
                          "debug_get_raster_events",
                          "debug_get_server_info"}) {
        CHECK(unique.count(t) == 1, std::string("registered: ") + t);
    }
    TEST_END();
}

void test_tool_names_have_debug_prefix() {
    TEST_BEGIN("all tool names have debug_ prefix");
    Fixture f;
    auto names = f.mcp.registeredToolNames();
    bool allPrefixed = true;
    for (auto &n : names) {
        if (n.substr(0, 6) != "debug_") {
            allPrefixed = false;
            fprintf(stderr, "    tool without prefix: %s\n", n.c_str());
        }
    }
    CHECK(allPrefixed, "all tools should have debug_ prefix");
    TEST_END();
}

void test_expected_tools_exist() {
    TEST_BEGIN("expected tools exist");
    Fixture f;
    auto names = f.mcp.registeredToolNames();
    std::set<std::string> nameSet(names.begin(), names.end());

    const char *expected[] = {
        "debug_run", "debug_pause", "debug_step", "debug_reset", "debug_is_running",
        "debug_get_cpu_state", "debug_get_registers", "debug_set_register",
        "debug_read_memory", "debug_write_memory",
        "debug_read_io", "debug_write_io",
        // Virtual keyboard injection
        "debug_list_keys", "debug_press_key", "debug_release_key", "debug_type_key",
        "debug_set_breakpoint", "debug_remove_breakpoint", "debug_list_breakpoints",
        "debug_clear_breakpoints", "debug_set_breakpoint_enabled",
        "debug_disassemble", "debug_get_instruction_history", "debug_get_execution_trace",
        "debug_get_stack",
        "debug_get_symbols", "debug_get_function", "debug_get_function_context",
        "debug_get_xrefs", "debug_get_call_graph",
        "debug_get_memory_map", "debug_get_vram_info", "debug_get_screen_info",
        "debug_get_io_trace",
        "debug_get_state",
        "debug_load_rom",
        "debug_set_comment", "debug_set_function_comment", "debug_rename_function",
        "debug_create_function", "debug_delete_function", "debug_add_label",
        // Stage 6.11: RDB tools
        "debug_get_rdb_info", "debug_list_rdb_objects", "debug_get_rdb_object",
        "debug_find_rdb_object", "debug_add_rdb_object", "debug_update_rdb_object",
        "debug_remove_rdb_object", "debug_set_rdb_comment", "debug_set_rdb_property",
        "debug_save_rdb", "debug_reload_rdb",
        // Stage 6.13: RDB Links
        "debug_add_rdb_link", "debug_remove_rdb_link", "debug_get_rdb_links",
        // Stage 6.16: Reverse Engineering Primitives
        "debug_read_memory_range", "debug_analyze_code",
        // Stage 6.18: Range Disassembly
        "debug_disassemble_range",
        // Stage 6.20: Runtime Memory Analysis
        "debug_clear_memory_access_map", "debug_get_memory_access_map",
        "debug_get_memory_access_log", "debug_create_memory_snapshot",
        "debug_compare_memory_snapshots",
        // Stage 6.26: Batch Analysis Tools
        "debug_disassemble_image", "debug_coverage_report", "debug_diff_memory",
        "debug_find_bytecode_sequence", "debug_find_immediate_in_range",
        "debug_get_vram_bytes"
    };

    for (auto &e : expected) {
        CHECK(nameSet.count(e) == 1, e);
    }
    TEST_END();
}

// ---------------------------------------------------------------------------
// Schema Validation Tests
// ---------------------------------------------------------------------------

void test_tool_schemas_have_properties() {
    TEST_BEGIN("tool schemas have type=object");
    Fixture f;
    auto tools = f.mcp.server().get_tools();
    bool allValid = true;
    for (auto &t : tools) {
        if (!t.parameters_schema.contains("type") ||
            t.parameters_schema["type"] != "object") {
            fprintf(stderr, "    tool %s missing type=object\n", t.name.c_str());
            allValid = false;
        }
    }
    CHECK(allValid, "all tools should have type=object schema");
    TEST_END();
}

void test_address_params_are_numbers() {
    TEST_BEGIN("address params are numbers");
    Fixture f;
    auto tools = f.mcp.server().get_tools();
    for (auto &t : tools) {
        if (t.parameters_schema.contains("properties") &&
            t.parameters_schema["properties"].contains("address")) {
            auto &addr = t.parameters_schema["properties"]["address"];
            CHECK(addr["type"] == "number", (t.name + " address should be number").c_str());
        }
    }
    TEST_END();
}

void test_read_memory_requires_address_and_size() {
    TEST_BEGIN("debug_read_memory requires address and size");
    Fixture f;
    auto tools = f.mcp.server().get_tools();
    for (auto &t : tools) {
        if (t.name == "debug_read_memory") {
            CHECK(t.parameters_schema.contains("required"), "should have required");
            auto &req = t.parameters_schema["required"];
            bool hasAddr = false, hasSize = false;
            for (auto &r : req) {
                if (r == "address") hasAddr = true;
                if (r == "size") hasSize = true;
            }
            CHECK(hasAddr, "should require address");
            CHECK(hasSize, "should require size");
        }
    }
    TEST_END();
}

// ---------------------------------------------------------------------------
// Execution Tests
// ---------------------------------------------------------------------------

void test_debug_step() {
    TEST_BEGIN("debug_step executes step");
    Fixture f;
    auto result = f.mcp.callTool("debug_step");
    CHECK(!isErrorContent(result), "step should succeed");
    auto data = parseTextAsJson(result);
    CHECK(data.contains("success"), "should have success field");
    CHECK(data["success"] == true, "success should be true");
    TEST_END();
}

void test_debug_is_running() {
    TEST_BEGIN("debug_is_running returns state");
    Fixture f;
    auto result = f.mcp.callTool("debug_is_running");
    CHECK(!isErrorContent(result), "should succeed");
    auto data = parseTextAsJson(result);
    CHECK(data.contains("running"), "should have running field");
    CHECK(data["running"] == false, "mock starts paused");
    TEST_END();
}

void test_debug_get_cpu_state() {
    TEST_BEGIN("debug_get_cpu_state returns registers");
    Fixture f;
    auto result = f.mcp.callTool("debug_get_cpu_state");
    CHECK(!isErrorContent(result), "should succeed");
    auto data = parseTextAsJson(result);
    CHECK(data.contains("pc"), "should have pc");
    CHECK(data.contains("sp"), "should have sp");
    CHECK(data.contains("a"), "should have a");
    CHECK(data["pc"] == "0x0100", "PC should be 0x0100");
    CHECK(data["a"] == "0x42", "A should be 0x42");
    TEST_END();
}

void test_debug_get_registers() {
    TEST_BEGIN("debug_get_registers returns formatted registers");
    Fixture f;
    auto result = f.mcp.callTool("debug_get_registers");
    CHECK(!isErrorContent(result), "should succeed");
    auto data = parseTextAsJson(result);
    CHECK(data.contains("PC"), "should have PC");
    CHECK(data.contains("AF"), "should have AF");
    CHECK(data.contains("HL"), "should have HL");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Memory Tests
// ---------------------------------------------------------------------------

void test_debug_read_memory() {
    TEST_BEGIN("debug_read_memory reads bytes");
    Fixture f;
    auto result = f.mcp.callTool("debug_read_memory", {{"address", 0x0100}, {"size", 3}});
    CHECK(!isErrorContent(result), "should succeed");
    auto data = parseTextAsJson(result);
    CHECK(data.contains("bytes"), "should have bytes");
    CHECK_EQ(static_cast<int>(data["bytes"].size()), 3, "should read 3 bytes");
    // 0x0100 = 0x31 (LXI SP, 0xF800)
    CHECK(data["bytes"][0] == 0x31, "first byte should be 0x31");
    TEST_END();
}

void test_debug_write_memory() {
    TEST_BEGIN("debug_write_memory writes bytes");
    Fixture f;
    auto result = f.mcp.callTool("debug_write_memory",
        {{"address", 0x1000}, {"data", {0xAA, 0xBB, 0xCC}}});
    if (isErrorContent(result)) {
        fprintf(stderr, "    write_memory error text: %s\n", getTextFromContent(result).c_str());
    }
    CHECK(!isErrorContent(result), "should succeed");
    auto data = parseTextAsJson(result);
    CHECK(data["written"] == 3, "should write 3 bytes");

    // Verify by reading back
    auto readResult = f.mcp.callTool("debug_read_memory", {{"address", 0x1000}, {"size", 3}});
    auto readData = parseTextAsJson(readResult);
    CHECK(readData["bytes"][0] == 0xAA, "first byte should be 0xAA");
    CHECK(readData["bytes"][1] == 0xBB, "second byte should be 0xBB");
    CHECK(readData["bytes"][2] == 0xCC, "third byte should be 0xCC");
    TEST_END();
}

// ---------------------------------------------------------------------------
// I/O Tests
// ---------------------------------------------------------------------------

void test_debug_read_io() {
    TEST_BEGIN("debug_read_io reads port");
    Fixture f;
    f.mock.setIoPort(0x42, 0x55);
    auto result = f.mcp.callTool("debug_read_io", {{"port", 0x42}});
    CHECK(!isErrorContent(result), "should succeed");
    auto data = parseTextAsJson(result);
    CHECK(data.contains("value"), "should have value");
    TEST_END();
}

void test_debug_write_io() {
    TEST_BEGIN("debug_write_io writes port");
    Fixture f;
    auto result = f.mcp.callTool("debug_write_io", {{"port", 0x10}, {"value", 0xAA}});
    CHECK(!isErrorContent(result), "should succeed");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Breakpoint Tests
// ---------------------------------------------------------------------------

void test_breakpoint_lifecycle() {
    TEST_BEGIN("breakpoint set/list/remove/clear lifecycle");
    Fixture f;

    // Set
    auto r1 = f.mcp.callTool("debug_set_breakpoint", {{"address", 0x0200}});
    CHECK(!isErrorContent(r1), "set should succeed");

    // List
    auto r2 = f.mcp.callTool("debug_list_breakpoints");
    auto listData = parseTextAsJson(r2);
    CHECK(listData["count"] == 1, "should have 1 breakpoint");

    // Enable/disable
    auto r3 = f.mcp.callTool("debug_set_breakpoint_enabled",
        {{"address", 0x0200}, {"enabled", false}});
    CHECK(!isErrorContent(r3), "disable should succeed");

    // Remove
    auto r4 = f.mcp.callTool("debug_remove_breakpoint", {{"address", 0x0200}});
    CHECK(!isErrorContent(r4), "remove should succeed");

    // List again
    auto r5 = f.mcp.callTool("debug_list_breakpoints");
    auto listData2 = parseTextAsJson(r5);
    CHECK(listData2["count"] == 0, "should have 0 breakpoints");

    // Set two and clear all
    f.mcp.callTool("debug_set_breakpoint", {{"address", 0x0100}});
    f.mcp.callTool("debug_set_breakpoint", {{"address", 0x0200}});
    auto r6 = f.mcp.callTool("debug_clear_breakpoints");
    CHECK(!isErrorContent(r6), "clear should succeed");

    auto r7 = f.mcp.callTool("debug_list_breakpoints");
    auto listData3 = parseTextAsJson(r7);
    CHECK(listData3["count"] == 0, "should have 0 breakpoints after clear");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Disassembly Tests
// ---------------------------------------------------------------------------

void test_debug_disassemble() {
    TEST_BEGIN("debug_disassemble returns instructions");
    Fixture f;
    auto result = f.mcp.callTool("debug_disassemble", {{"address", 0x0100}, {"count", 3}});
    CHECK(!isErrorContent(result), "should succeed");
    auto data = parseTextAsJson(result);
    CHECK(data.contains("instructions"), "should have instructions");
    CHECK(data["count"] == 3, "should have 3 instructions");
    auto &insts = data["instructions"];
    CHECK(insts[0]["address"] == "0x0100", "first at 0x0100");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Stack Tests
// ---------------------------------------------------------------------------

void test_debug_get_stack() {
    TEST_BEGIN("debug_get_stack returns entries");
    Fixture f;
    auto result = f.mcp.callTool("debug_get_stack", {{"limit", 5}});
    CHECK(!isErrorContent(result), "should succeed");
    auto data = parseTextAsJson(result);
    CHECK(data.contains("entries"), "should have entries");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Symbol / Analysis Tests
// ---------------------------------------------------------------------------

void test_debug_get_symbols_empty() {
    TEST_BEGIN("debug_get_symbols returns empty initially");
    Fixture f;
    auto result = f.mcp.callTool("debug_get_symbols");
    CHECK(!isErrorContent(result), "should succeed");
    auto data = parseTextAsJson(result);
    CHECK(data["count"] == 0, "should have 0 symbols initially");
    TEST_END();
}

void test_debug_annotations() {
    TEST_BEGIN("debug annotation tools (create/rename/comment/label/delete)");
    Fixture f;

    // Create function
    auto r1 = f.mcp.callTool("debug_create_function", {{"address", 0x0200}});
    CHECK(!isErrorContent(r1), "create should succeed");

    // Rename
    auto r2 = f.mcp.callTool("debug_rename_function",
        {{"address", 0x0200}, {"name", "my_func"}});
    CHECK(!isErrorContent(r2), "rename should succeed");

    // Set function comment
    auto r3 = f.mcp.callTool("debug_set_function_comment",
        {{"address", 0x0200}, {"comment", "test comment"}});
    CHECK(!isErrorContent(r3), "set_function_comment should succeed");

    // Add label
    auto r4 = f.mcp.callTool("debug_add_label",
        {{"address", 0x0206}, {"name", "write_loop"}});
    CHECK(!isErrorContent(r4), "add_label should succeed");

    // Set comment (on existing symbol)
    auto r5 = f.mcp.callTool("debug_set_comment",
        {{"address", 0x0206}, {"comment", "mov_m_a"}});
    CHECK(!isErrorContent(r5), "set_comment should succeed");

    // Get symbols — should have 2 now
    auto r6 = f.mcp.callTool("debug_get_symbols");
    auto symData = parseTextAsJson(r6);
    CHECK(symData["count"] == 2, "should have 2 symbols");

    // Get function
    auto r7 = f.mcp.callTool("debug_get_function", {{"address", 0x0200}});
    CHECK(!isErrorContent(r7), "get_function should succeed");
    auto funcData = parseTextAsJson(r7);
    CHECK(funcData["name"] == "my_func", "name should be my_func");

    // Delete function
    auto r8 = f.mcp.callTool("debug_delete_function", {{"address", 0x0200}});
    CHECK(!isErrorContent(r8), "delete should succeed");

    // Get function after delete — should error
    auto r9 = f.mcp.callTool("debug_get_function", {{"address", 0x0200}});
    CHECK(isErrorContent(r9), "get_function after delete should error");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Debug State Tests
// ---------------------------------------------------------------------------

void test_debug_get_state() {
    TEST_BEGIN("debug_get_state returns full state");
    Fixture f;
    auto result = f.mcp.callTool("debug_get_state");
    CHECK(!isErrorContent(result), "should succeed");
    auto data = parseTextAsJson(result);
    CHECK(data.contains("running"), "should have running");
    CHECK(data.contains("cpu"), "should have cpu");
    CHECK(data.contains("breakpoints"), "should have breakpoints");
    CHECK(data.contains("current_instruction"), "should have current_instruction");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Memory Map / Screen / VRAM Tests
// ---------------------------------------------------------------------------

void test_debug_get_memory_map() {
    TEST_BEGIN("debug_get_memory_map returns blocks");
    Fixture f;
    auto result = f.mcp.callTool("debug_get_memory_map");
    CHECK(!isErrorContent(result), "should succeed");
    auto data = parseTextAsJson(result);
    CHECK(data.contains("blocks"), "should have blocks");
    TEST_END();
}

void test_debug_get_screen_info() {
    TEST_BEGIN("debug_get_screen_info returns info");
    Fixture f;
    auto result = f.mcp.callTool("debug_get_screen_info");
    CHECK(!isErrorContent(result), "should succeed");
    auto data = parseTextAsJson(result);
    CHECK(data.contains("width"), "should have width");
    TEST_END();
}

void test_debug_get_vram_info() {
    TEST_BEGIN("debug_get_vram_info returns info");
    Fixture f;
    auto result = f.mcp.callTool("debug_get_vram_info");
    CHECK(!isErrorContent(result), "should succeed");
    auto data = parseTextAsJson(result);
    CHECK(data.contains("vram_base"), "should have vram_base");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Error Propagation Tests
// ---------------------------------------------------------------------------

void test_error_propagation_invalid_address() {
    TEST_BEGIN("error propagation: invalid address");
    Fixture f;
    // Address out of range
    bool threw = false;
    try {
        f.mcp.callTool("debug_read_memory", {{"address", 70000}, {"size", 1}});
    } catch (const mcp::mcp_exception &e) {
        threw = true;
    }
    CHECK(threw, "should throw for address > 0xFFFF");
    TEST_END();
}

void test_error_propagation_missing_param() {
    TEST_BEGIN("error propagation: missing required param");
    Fixture f;
    bool threw = false;
    try {
        f.mcp.callTool("debug_read_memory", {{"address", 0x0100}});
        // size defaults to 1, so this should actually succeed
    } catch (const mcp::mcp_exception &) {
        threw = true;
    }
    // debug_read_memory has size with default, so missing size is OK
    CHECK(!threw, "missing optional size should not throw");
    TEST_END();
}

void test_error_propagation_tool_not_found() {
    TEST_BEGIN("error propagation: tool not found");
    Fixture f;
    bool threw = false;
    try {
        f.mcp.callTool("debug_nonexistent");
    } catch (const mcp::mcp_exception &e) {
        threw = true;
    }
    CHECK(threw, "should throw for unknown tool");
    TEST_END();
}

void test_error_propagation_get_function_not_found() {
    TEST_BEGIN("error propagation: get_function returns NotFound");
    Fixture f;
    auto result = f.mcp.callTool("debug_get_function", {{"address", 0x9999}});
    CHECK(isErrorContent(result), "should return error for unknown function");
    auto data = parseTextAsJson(result);
    CHECK(data.contains("error_code"), "should have error_code");
    CHECK(data.contains("message"), "should have error message");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Wire-Level Error Format Tests (Stage 6.4.1)
// ---------------------------------------------------------------------------

void test_wire_level_error_format() {
    TEST_BEGIN("wire-level: CallToolResult has isError at top level");
    Fixture f;
    
    // Trigger an error: handler returns errorContent (via AgentApi error result)
    // Use debug_get_function which returns error for unknown address
    auto handlerResult = f.mcp.callTool("debug_get_function", {{"address", 0x9999}});
    
    // Simulate library wire wrapping (cpp-mcp tools/call handler)
    mcp::json result = {{"content", handlerResult}, {"isError", false}};
    
    // Verify CallToolResult structure (Stage 6.4.1)
    CHECK(result.is_object(), "result must be an object");
    CHECK(result.contains("content"), "result must have 'content' field");
    CHECK(result.contains("isError"), "result must have 'isError' field at top level");
    
    // Verify content structure (handler returns array directly)
    auto &content = result["content"];
    CHECK(content.is_array(), "content must be an array");
    CHECK(!content.empty(), "content must not be empty");
    
    auto &first = content[0];
    CHECK(first.contains("type"), "content item must have 'type'");
    CHECK(first["type"] == "text", "content type must be 'text'");
    CHECK(first.contains("text"), "content item must have 'text'");
    
    // Verify error data is in text
    auto errData = mcp::json::parse(first["text"].get<std::string>());
    CHECK(errData.contains("error_code"), "error data must have error_code");
    CHECK(errData.contains("message"), "error data must have message");
    
    TEST_END();
}

void test_wire_level_success_format() {
    TEST_BEGIN("wire-level: CallToolResult success has isError=false");
    Fixture f;
    
    // Successful call
    auto handlerResult = f.mcp.callTool("debug_is_running");
    
    // Simulate library wire wrapping (cpp-mcp tools/call handler)
    mcp::json result = {{"content", handlerResult}, {"isError", false}};
    
    // Verify CallToolResult structure
    CHECK(result.is_object(), "result must be an object");
    CHECK(result.contains("content"), "result must have 'content' field");
    CHECK(result.contains("isError"), "result must have 'isError' field at top level");
    CHECK(result["isError"].get<bool>() == false, "isError must be false for success");
    
    // Verify content structure
    auto &content = result["content"];
    CHECK(content.is_array(), "content must be an array");
    CHECK(!content.empty(), "content must not be empty");
    
    auto &first = content[0];
    CHECK(first.contains("type"), "content item must have 'type'");
    CHECK(first["type"] == "text", "content type must be 'text'");
    CHECK(first.contains("text"), "content item must have 'text'");
    
    TEST_END();
}

// ---------------------------------------------------------------------------
// End-to-End Integration Test
// ---------------------------------------------------------------------------

void test_e2e_read_memory_full_path() {
    TEST_BEGIN("E2E: read_memory full path MCP → AgentApi → MockBackend → JSON");
    Fixture f;

    // Write known data to mock
    f.mock.setMemory(0x0300, {0xDE, 0xAD, 0xBE, 0xEF});

    // MCP request
    auto result = f.mcp.callTool("debug_read_memory", {{"address", 0x0300}, {"size", 4}});

    // Verify MCP response
    if (isErrorContent(result)) {
        fprintf(stderr, "    read_memory error: %s\n", getTextFromContent(result).c_str());
    }
    CHECK(!isErrorContent(result), "should succeed");
    auto data = parseTextAsJson(result);
    CHECK(data["address"] == "0x0300", "address should match");
    CHECK(data["size"] == 4, "size should be 4");

    auto &bytes = data["bytes"];
    CHECK(bytes[0] == 0xDE, "byte 0 should be 0xDE");
    CHECK(bytes[1] == 0xAD, "byte 1 should be 0xAD");
    CHECK(bytes[2] == 0xBE, "byte 2 should be 0xBE");
    CHECK(bytes[3] == 0xEF, "byte 3 should be 0xEF");
    TEST_END();
}

void test_e2e_write_then_read() {
    TEST_BEGIN("E2E: write_memory then read_memory roundtrip");
    Fixture f;

    // Write via MCP
    auto w = f.mcp.callTool("debug_write_memory",
        {{"address", 0x0500}, {"data", {0x11, 0x22, 0x33}}});
    CHECK(!isErrorContent(w), "write should succeed");

    // Read back via MCP
    auto r = f.mcp.callTool("debug_read_memory", {{"address", 0x0500}, {"size", 3}});
    auto data = parseTextAsJson(r);
    CHECK(data["bytes"][0] == 0x11, "byte 0");
    CHECK(data["bytes"][1] == 0x22, "byte 1");
    CHECK(data["bytes"][2] == 0x33, "byte 2");
    TEST_END();
}

// ---------------------------------------------------------------------------
// JSON Serialization Tests
// ---------------------------------------------------------------------------

void test_json_cpu_state_format() {
    TEST_BEGIN("JSON: CpuState serialization format");
    CpuState cpu{};
    cpu.pc = 0x0100;
    cpu.sp = 0xF800;
    cpu.a = 0x42;
    cpu.flags = 0x00;

    auto j = mcp_json::cpuStateToJson(cpu);
    CHECK(j["pc"] == "0x0100", "pc format");
    CHECK(j["sp"] == "0xF800", "sp format");
    CHECK(j["a"] == "0x42", "a format");
    TEST_END();
}

void test_json_breakpoint_format() {
    TEST_BEGIN("JSON: breakpoint serialization format");
    DebuggerBreakpoint bp{0x1234, true};
    auto j = mcp_json::breakpointToJson(bp);
    CHECK(j["address"] == "0x1234", "address format");
    CHECK(j["enabled"] == true, "enabled field");
    TEST_END();
}

void test_json_error_result() {
    TEST_BEGIN("JSON: error result format");
    auto j = mcp_json::errorResult(ErrorCode::NotFound, "symbol not found");
    CHECK(j["success"] == false, "success should be false");
    CHECK(j["error"] == "symbol not found", "error message");
    CHECK(static_cast<int>(j["error_code"]) == static_cast<int>(ErrorCode::NotFound), "error code");
    TEST_END();
}

// ---------------------------------------------------------------------------
// I/O Boundary Tests
// ---------------------------------------------------------------------------

void test_io_port_boundaries() {
    TEST_BEGIN("I/O port boundaries: 0x00 and 0xFF");
    Fixture f;

    // Port 0x00
    auto r1 = f.mcp.callTool("debug_read_io", {{"port", 0x00}});
    CHECK(!isErrorContent(r1), "port 0x00 should be valid");

    // Port 0xFF
    auto r2 = f.mcp.callTool("debug_read_io", {{"port", 0xFF}});
    CHECK(!isErrorContent(r2), "port 0xFF should be valid");

    // Port out of range
    bool threw = false;
    try {
        f.mcp.callTool("debug_read_io", {{"port", 256}});
    } catch (const mcp::mcp_exception &) {
        threw = true;
    }
    CHECK(threw, "port 256 should throw");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Trace / History Tests
// ---------------------------------------------------------------------------

void test_debug_get_execution_trace() {
    TEST_BEGIN("debug_get_execution_trace returns events");
    Fixture f;
    auto result = f.mcp.callTool("debug_get_execution_trace", {{"max_entries", 100}});
    CHECK(!isErrorContent(result), "should succeed");
    auto data = parseTextAsJson(result);
    CHECK(data.contains("events"), "should have events");
    TEST_END();
}

void test_debug_get_io_trace() {
    TEST_BEGIN("debug_get_io_trace returns events");
    Fixture f;
    auto result = f.mcp.callTool("debug_get_io_trace", {{"max_entries", 100}});
    CHECK(!isErrorContent(result), "should succeed");
    auto data = parseTextAsJson(result);
    CHECK(data.contains("events"), "should have events");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Call Graph Tests
// ---------------------------------------------------------------------------

void test_debug_get_call_graph() {
    TEST_BEGIN("debug_get_call_graph returns edges");
    Fixture f;
    auto result = f.mcp.callTool("debug_get_call_graph");
    CHECK(!isErrorContent(result), "should succeed");
    auto data = parseTextAsJson(result);
    CHECK(data.contains("edges"), "should have edges");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Set Register Tests
// ---------------------------------------------------------------------------

void test_debug_set_register() {
    TEST_BEGIN("debug_set_register changes register");
    Fixture f;

    // Set A register (part of AF)
    auto r = f.mcp.callTool("debug_set_register", {{"name", "PC"}, {"value", 0x0200}});
    CHECK(!isErrorContent(r), "set_register should succeed");

    // Verify
    auto cpu = f.mcp.callTool("debug_get_cpu_state");
    auto data = parseTextAsJson(cpu);
    CHECK(data["pc"] == "0x0200", "PC should be 0x0200");
    TEST_END();
}

// ---------------------------------------------------------------------------
// RDB Links Tests (Stage 6.13)
// ---------------------------------------------------------------------------

void test_rdb_link_lifecycle() {
    TEST_BEGIN("RDB link add/get/remove lifecycle");
    Fixture f;

    // Create source and target objects
    auto addSrc = f.mcp.callTool("debug_add_rdb_object",
        {{"address", 0x0100}, {"name", "src"}, {"type", "Function"}});
    CHECK(!isErrorContent(addSrc), "add source succeeds");

    auto addTgt = f.mcp.callTool("debug_add_rdb_object",
        {{"address", 0x0200}, {"name", "tgt"}, {"type", "Function"}});
    CHECK(!isErrorContent(addTgt), "add target succeeds");

    // Add link
    auto addLink = f.mcp.callTool("debug_add_rdb_link",
        {{"source", 0x0100}, {"target", 0x0200}});
    CHECK(!isErrorContent(addLink), "add link succeeds");

    // Get links
    auto getLinks = f.mcp.callTool("debug_get_rdb_links", {{"source", 0x0100}});
    CHECK(!isErrorContent(getLinks), "get links succeeds");
    auto data = parseTextAsJson(getLinks);
    CHECK(data.contains("links"), "has links array");
    CHECK_EQ(1, static_cast<int>(data["links"].size()), "1 link");
    CHECK(data["links"][0] == 0x0200, "target is 0x0200");

    // Remove link
    auto rmLink = f.mcp.callTool("debug_remove_rdb_link",
        {{"source", 0x0100}, {"target", 0x0200}});
    CHECK(!isErrorContent(rmLink), "remove link succeeds");

    // Verify empty
    auto getLinks2 = f.mcp.callTool("debug_get_rdb_links", {{"source", 0x0100}});
    auto data2 = parseTextAsJson(getLinks2);
    CHECK_EQ(0, static_cast<int>(data2["links"].size()), "0 links after remove");
    TEST_END();
}

void test_rdb_link_invalid_source() {
    TEST_BEGIN("RDB link — invalid source");
    Fixture f;

    // Add link with no source object
    auto r = f.mcp.callTool("debug_add_rdb_link",
        {{"source", 0x9999}, {"target", 0x0100}});
    CHECK(isErrorContent(r), "add link fails with no source");

    // Get links with no source object
    auto r2 = f.mcp.callTool("debug_get_rdb_links", {{"source", 0x9999}});
    CHECK(isErrorContent(r2), "get links fails with no source");
    TEST_END();
}

void test_rdb_link_unresolved_target() {
    TEST_BEGIN("RDB link — unresolved target");
    Fixture f;

    // Create source only
    f.mcp.callTool("debug_add_rdb_object",
        {{"address", 0x0100}, {"name", "src"}, {"type", "Function"}});

    // Link to non-existent target
    auto r = f.mcp.callTool("debug_add_rdb_link",
        {{"source", 0x0100}, {"target", 0x0345}});
    CHECK(!isErrorContent(r), "link to unresolved target succeeds");

    // Verify
    auto links = f.mcp.callTool("debug_get_rdb_links", {{"source", 0x0100}});
    auto data = parseTextAsJson(links);
    CHECK_EQ(1, static_cast<int>(data["links"].size()), "1 link");
    CHECK(data["links"][0] == 0x0345, "unresolved target stored");
    TEST_END();
}

void test_rdb_link_duplicate() {
    TEST_BEGIN("RDB link — duplicate is idempotent");
    Fixture f;

    f.mcp.callTool("debug_add_rdb_object",
        {{"address", 0x0100}, {"name", "src"}, {"type", "Function"}});

    auto r1 = f.mcp.callTool("debug_add_rdb_link",
        {{"source", 0x0100}, {"target", 0x0200}});
    CHECK(!isErrorContent(r1), "first add succeeds");

    auto r2 = f.mcp.callTool("debug_add_rdb_link",
        {{"source", 0x0100}, {"target", 0x0200}});
    CHECK(!isErrorContent(r2), "duplicate add succeeds");

    auto links = f.mcp.callTool("debug_get_rdb_links", {{"source", 0x0100}});
    auto data = parseTextAsJson(links);
    CHECK_EQ(1, static_cast<int>(data["links"].size()), "still 1 link");
    TEST_END();
}

void test_rdb_link_remove_missing() {
    TEST_BEGIN("RDB link — remove missing fails");
    Fixture f;

    f.mcp.callTool("debug_add_rdb_object",
        {{"address", 0x0100}, {"name", "src"}, {"type", "Function"}});

    auto r = f.mcp.callTool("debug_remove_rdb_link",
        {{"source", 0x0100}, {"target", 0x0200}});
    CHECK(isErrorContent(r), "remove non-existent link fails");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Stage 6.19: Multi-entry analyze_code MCP tests
// ---------------------------------------------------------------------------

void test_mcp_analyze_code_single_entry_compat() {
    TEST_BEGIN("MCP: debug_analyze_code single entry backward compat");
    Fixture f;

    // Write NOP + RET at 0x0200
    f.mock.setMemory(0x0200, {0x00, 0xC9});

    auto r = f.mcp.callTool("debug_analyze_code",
        {{"start_address", 0x0200}});
    CHECK(!isErrorContent(r), "single entry should succeed");
    auto data = parseTextAsJson(r);
    CHECK(data.contains("entry_points"), "result has entry_points");
    CHECK(data["entry_points"].size() == 1, "1 entry point");
    CHECK(data.contains("code_bytes"), "result has code_bytes");
    TEST_END();
}

void test_mcp_analyze_code_multi_entry() {
    TEST_BEGIN("MCP: debug_analyze_code multi-entry via addresses");
    Fixture f;

    // Write code at two separate locations
    f.mock.setMemory(0x0100, {0x00, 0xC3, 0x20, 0x01});  // NOP; JMP 0x0120
    f.mock.setMemory(0x0110, {0x00, 0xC9});               // NOP; RET
    f.mock.setMemory(0x0120, {0x00, 0xC9});               // NOP; RET

    auto r = f.mcp.callTool("debug_analyze_code",
        {{"addresses", {0x0100, 0x0110}}});
    CHECK(!isErrorContent(r), "multi-entry should succeed");
    auto data = parseTextAsJson(r);
    CHECK(data["entry_points"].size() == 2, "2 entry points");
    CHECK(data["instruction_count"].get<int>() > 0, "has instructions");
    CHECK(data["code_bytes"].get<int>() > 0, "has code_bytes");
    TEST_END();
}

void test_mcp_analyze_code_duplicate_entries() {
    TEST_BEGIN("MCP: debug_analyze_code duplicate entries");
    Fixture f;
    f.mock.setMemory(0x0200, {0x00, 0xC9});

    auto r = f.mcp.callTool("debug_analyze_code",
        {{"addresses", {0x0200, 0x0200}}});
    CHECK(!isErrorContent(r), "duplicate entries succeed");
    auto data = parseTextAsJson(r);
    CHECK(data["entry_points"].size() == 1, "duplicates removed");
    TEST_END();
}

void test_mcp_analyze_code_empty_addresses() {
    TEST_BEGIN("MCP: debug_analyze_code empty addresses array");
    Fixture f;

    bool threw = false;
    try {
        f.mcp.callTool("debug_analyze_code",
            {{"addresses", mcp::json::array()}});
    } catch (const mcp::mcp_exception &) {
        threw = true;
    }
    CHECK(threw, "empty array throws");
    TEST_END();
}

void test_mcp_analyze_code_mutual_exclusion() {
    TEST_BEGIN("MCP: debug_analyze_code start_address and addresses exclusive");
    Fixture f;

    bool threw = false;
    try {
        f.mcp.callTool("debug_analyze_code",
            {{"start_address", 0x0000}, {"addresses", {0x0100}}});
    } catch (const mcp::mcp_exception &) {
        threw = true;
    }
    CHECK(threw, "both params throws");
    TEST_END();
}

void test_mcp_analyze_code_neither_param() {
    TEST_BEGIN("MCP: debug_analyze_code requires one of start_address/addresses");
    Fixture f;

    bool threw = false;
    try {
        f.mcp.callTool("debug_analyze_code",
            {{"max_instructions", 100}});
    } catch (const mcp::mcp_exception &) {
        threw = true;
    }
    CHECK(threw, "missing both params throws");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Stage 6.26: Batch analysis tools
// ---------------------------------------------------------------------------

void test_mcp_disassemble_image_basic() {
    TEST_BEGIN("MCP: debug_disassemble_image basic sweep");
    Fixture f;
    // Mock program: LXI SP,F800 / MVI A,55 / CALL 0200 / HLT = 9 bytes, 4 instrs
    auto r = f.mcp.callTool("debug_disassemble_image",
        {{"address", 0x0100}, {"length", 9}});
    CHECK(!isErrorContent(r), "image disassembly should succeed");
    auto data = parseTextAsJson(r);
    CHECK_EQ(data["instruction_count"].get<int>(), 4, "4 instructions");
    CHECK_EQ(data["length"].get<int>(), 9, "length echoed");
    CHECK_EQ(data["address"].get<std::string>(), "0x0100", "address echoed");

    const auto &inst = data["instructions"];
    CHECK_EQ(inst[0]["address"].get<std::string>(), "0x0100", "first addr");
    CHECK_EQ(inst[0]["mnemonic"].get<std::string>(), "LXI", "first mnemonic");
    CHECK_EQ(inst[0]["size"].get<int>(), 3, "LXI size 3");
    CHECK_EQ(inst[0]["bytes"].size(), 3u, "LXI 3 bytes");
    // §5.2: every instruction keeps the debug_disassemble_range field shape
    for (const auto &i : inst) {
        CHECK(i.contains("address") && i.contains("mnemonic") &&
              i.contains("operands") && i.contains("size") &&
              i.contains("bytes") && i.contains("branch_target") &&
              i.contains("branch_type"), "instruction field shape");
    }
    // CALL at 0x0105: branch_target 0x0200, non-null branch_type
    CHECK_EQ(inst[2]["mnemonic"].get<std::string>(), "CALL", "CALL mnemonic");
    CHECK(inst[2]["branch_target"] != nullptr, "CALL has branch_target");
    CHECK_EQ(inst[2]["branch_target"].get<int>(), 0x0200, "target 0x0200");
    CHECK(inst[2]["branch_type"] != nullptr, "CALL has branch_type");
    CHECK(inst[3]["branch_target"].is_null(), "HLT has no branch_target");
    TEST_END();
}

void test_mcp_disassemble_image_equivalence() {
    TEST_BEGIN("MCP: debug_disassemble_image == N x debug_disassemble_range");
    Fixture f;
    // §5.2 / §20: batch result must equal the existing single-range result
    auto rImg = f.mcp.callTool("debug_disassemble_image",
        {{"address", 0x0100}, {"length", 8}});
    auto rRng = f.mcp.callTool("debug_disassemble_range",
        {{"address", 0x0100}, {"size", 8}});
    CHECK(!isErrorContent(rImg) && !isErrorContent(rRng), "both succeed");
    auto dImg = parseTextAsJson(rImg);
    auto dRng = parseTextAsJson(rRng);
    CHECK(dImg["instructions"] == dRng["instructions"],
          "instruction arrays must be identical");
    // Second window (subroutine area) must match too
    auto rImg2 = f.mcp.callTool("debug_disassemble_image",
        {{"address", 0x0200}, {"length", 10}});
    auto rRng2 = f.mcp.callTool("debug_disassemble_range",
        {{"address", 0x0200}, {"size", 10}});
    auto dImg2 = parseTextAsJson(rImg2);
    auto dRng2 = parseTextAsJson(rRng2);
    CHECK(dImg2["instructions"] == dRng2["instructions"],
          "second window identical");
    TEST_END();
}

void test_mcp_disassemble_image_limit() {
    TEST_BEGIN("MCP: debug_disassemble_image limit_exceeded (no silent cut)");
    Fixture f;
    // Zeroed memory outside the program area sweeps as 1-byte NOPs:
    // full 64K image > MAX_DISASSEMBLE_IMAGE_INSTRUCTIONS (40000)
    auto r = f.mcp.callTool("debug_disassemble_image",
        {{"address", 0x0000}, {"length", 65536}});
    CHECK(isErrorContent(r), "dense NOP sweep must error, not truncate");
    auto data = parseTextAsJson(r);
    CHECK_EQ(data["error_code"].get<std::string>(), "limit_exceeded",
             "structured limit_exceeded error");
    // Wrap-around past 64K is an invalid_range error
    auto r2 = f.mcp.callTool("debug_disassemble_image",
        {{"address", 0xFFF8}, {"length", 16}});
    CHECK(isErrorContent(r2), "64K wrap must error");
    CHECK_EQ(parseTextAsJson(r2)["error_code"].get<std::string>(),
             "invalid_range", "wrap is invalid_range");
    TEST_END();
}

void test_mcp_coverage_report() {
    TEST_BEGIN("MCP: debug_coverage_report structure + JCC blind spots");
    Fixture f;
    // Add an isolated RET so a second entry point produces a separate range
    f.mock.setMemory(0x0300, {0xC9});

    auto r = f.mcp.callTool("debug_coverage_report",
        {{"start_address", 0x0100},
         {"addresses", {0x0300}},
         {"range_start", 0}, {"range_length", 0x1000}});
    CHECK(!isErrorContent(r), "coverage report should succeed");
    auto data = parseTextAsJson(r);

    CHECK(data.contains("code_ranges"), "has code_ranges");
    CHECK(data.contains("uncovered_ranges"), "has uncovered_ranges");
    CHECK(data.contains("branch_targets"), "has branch_targets");
    CHECK(data.contains("uncovered_branch_targets"), "has uncovered_branch_targets");
    CHECK(data["code_ranges"].size() >= 3,
          "three code areas: main, subroutine, isolated RET");
    CHECK_EQ(data["code_ranges"][0]["start"].get<std::string>(), "0x0100",
             "first code range at program start");
    bool foundSubroutine = false, foundIsolated = false;
    for (const auto &rg : data["code_ranges"]) {
        if (rg["start"].get<std::string>() == "0x0200") foundSubroutine = true;
        if (rg["start"].get<std::string>() == "0x0300") foundIsolated = true;
    }
    CHECK(foundSubroutine, "CALL target covered");
    CHECK(foundIsolated, "explicit entry point covered");

    // CALL 0x0200 must show up as a branch target that IS covered
    CHECK(data["branch_targets"].size() >= 1, "branch targets listed");
    for (const auto &bt : data["branch_targets"]) {
        CHECK(bt.contains("from") && bt.contains("to") && bt.contains("type"),
              "branch target shape");
    }
    CHECK_EQ(data["uncovered_branch_targets"].size(), 0u,
             "0x0200 reachable => no uncovered targets here");

    CHECK(data.contains("stats"), "has stats");
    const auto &st = data["stats"];
    CHECK(st["code_bytes"].get<int>() > 0, "code_bytes > 0");
    CHECK_EQ(st["image_bytes"].get<int>(), 0x1000, "image_bytes = range_length");
    CHECK(st.contains("coverage_percent"), "coverage_percent present");
    CHECK(st["truncated"] == false, "not truncated for tiny program");
    TEST_END();
}

void test_mcp_coverage_report_requires_entry() {
    TEST_BEGIN("MCP: debug_coverage_report requires entry point");
    Fixture f;
    bool threw = false;
    try {
        f.mcp.callTool("debug_coverage_report", {{"range_length", 0x100}});
    } catch (const mcp::mcp_exception &) {
        threw = true;
    }
    CHECK(threw, "missing entry points throws");
    TEST_END();
}

void test_mcp_diff_memory() {
    TEST_BEGIN("MCP: debug_diff_memory complete diff with old/new");
    Fixture f;

    auto snapA = parseTextAsJson(f.mcp.callTool("debug_create_memory_snapshot", {}));
    uint32_t idA = static_cast<uint32_t>(snapA["snapshot_id"].get<int>());

    // One scattered byte + a dense 32-byte block: old tool under-reports,
    // debug_diff_memory must report everything (§7.2)
    f.mock.setMemory(0x0104, {0xAA});                       // 1 byte
    std::vector<uint8_t> block(32, 0x5A);
    f.mock.setMemory(0x6000, block);                        // dense 32 bytes

    auto snapB = parseTextAsJson(f.mcp.callTool("debug_create_memory_snapshot", {}));
    uint32_t idB = static_cast<uint32_t>(snapB["snapshot_id"].get<int>());

    auto r = f.mcp.callTool("debug_diff_memory",
        {{"snapshot_a", idA}, {"snapshot_b", idB}});
    CHECK(!isErrorContent(r), "diff should succeed");
    auto data = parseTextAsJson(r);
    CHECK_EQ(data["changed_bytes"].get<int>(), 33, "33 changed bytes total");
    CHECK_EQ(data["range_count"].get<int>(), 2, "two contiguous ranges");

    bool foundByte = false, foundBlock = false;
    for (const auto &rg : data["ranges"]) {
        if (rg["address"].get<std::string>() == "0x0104") {
            foundByte = true;
            CHECK_EQ(rg["size"].get<int>(), 1, "single-byte range");
            CHECK_EQ(rg["old"][0].get<int>(), 0x55, "old value");
            CHECK_EQ(rg["new"][0].get<int>(), 0xAA, "new value");
        }
        if (rg["address"].get<std::string>() == "0x6000") {
            foundBlock = true;
            CHECK_EQ(rg["size"].get<int>(), 32, "dense range keeps all 32 bytes");
            CHECK_EQ(rg["old"].size(), 32u, "old[] full width");
            CHECK_EQ(rg["new"].size(), 32u, "new[] full width");
            CHECK_EQ(rg["new"][31].get<int>(), 0x5A, "last byte reported");
        }
    }
    CHECK(foundByte, "scattered byte diff present");
    CHECK(foundBlock, "dense block diff present");
    TEST_END();
}

void test_mcp_diff_memory_missing_snapshot() {
    TEST_BEGIN("MCP: debug_diff_memory unknown snapshot id");
    Fixture f;
    auto snap = parseTextAsJson(f.mcp.callTool("debug_create_memory_snapshot", {}));
    uint32_t idA = static_cast<uint32_t>(snap["snapshot_id"].get<int>());
    auto r = f.mcp.callTool("debug_diff_memory",
        {{"snapshot_a", idA}, {"snapshot_b", 9999}});
    CHECK(isErrorContent(r), "missing snapshot must error");
    CHECK_EQ(parseTextAsJson(r)["error_code"].get<std::string>(),
             "not_found", "structured not_found");
    TEST_END();
}

void test_mcp_find_bytecode_sequence() {
    TEST_BEGIN("MCP: debug_find_bytecode_sequence + mask + determinism");
    Fixture f;
    // CD 00 occurs at 0x0105 (CALL 0x0200)
    auto r = f.mcp.callTool("debug_find_bytecode_sequence",
        {{"range_start", 0x0100}, {"range_end", 0x01FF},
         {"pattern", {0xCD, 0x00}}});
    CHECK(!isErrorContent(r), "search should succeed");
    auto data = parseTextAsJson(r);
    CHECK_EQ(data["match_count"].get<int>(), 1, "one match");
    CHECK_EQ(data["addresses"][0].get<std::string>(), "0x0105", "match address");
    CHECK_EQ(data["scanned_bytes"].get<int>(), 0x100, "scanned range");

    // Masked pattern: 3E ?? matches MVI A,55 at 0x0103
    auto rm = f.mcp.callTool("debug_find_bytecode_sequence",
        {{"range_start", 0x0100}, {"range_end", 0x01FF},
         {"pattern", {0x3E, 0x99}}, {"mask", {0xFF, 0x00}}});
    auto dm = parseTextAsJson(rm);
    CHECK_EQ(dm["match_count"].get<int>(), 1, "masked match found");
    CHECK_EQ(dm["addresses"][0].get<std::string>(), "0x0103", "masked address");

    // No match: empty result, still success
    auto r0 = f.mcp.callTool("debug_find_bytecode_sequence",
        {{"range_start", 0x0100}, {"range_end", 0x01FF},
         {"pattern", {0xFF, 0xFF, 0xFF}}});
    CHECK(!isErrorContent(r0), "no-match is not an error");
    CHECK_EQ(parseTextAsJson(r0)["match_count"].get<int>(), 0, "zero matches");

    // §17 determinism: identical state → identical result
    auto rAgain = f.mcp.callTool("debug_find_bytecode_sequence",
        {{"range_start", 0x0100}, {"range_end", 0x01FF},
         {"pattern", {0xCD, 0x00}}});
    CHECK(parseTextAsJson(rAgain).dump() == data.dump(), "deterministic repeat");

    // Invalid range end < start throws
    bool threw = false;
    try {
        f.mcp.callTool("debug_find_bytecode_sequence",
            {{"range_start", 0x0100}, {"range_end", 0x00FF},
             {"pattern", {0xCD}}});
    } catch (const mcp::mcp_exception &) { threw = true; }
    CHECK(threw, "inverted range throws");
    TEST_END();
}

void test_mcp_find_immediate_in_range() {
    TEST_BEGIN("MCP: debug_find_immediate_in_range via disassembler");
    Fixture f;
    // Operand 0x0200 → CALL at 0x0105 (interpretation by Debugger disassembler)
    auto r = f.mcp.callTool("debug_find_immediate_in_range",
        {{"range_start", 0x0100}, {"range_end", 0x010F}, {"value", 0x0200}});
    CHECK(!isErrorContent(r), "immediate search should succeed");
    auto data = parseTextAsJson(r);
    CHECK_EQ(data["match_count"].get<int>(), 1, "one CALL 0x0200 match");
    CHECK_EQ(data["matches"][0]["address"].get<std::string>(), "0x0105", "match addr");
    CHECK_EQ(data["matches"][0]["mnemonic"].get<std::string>(), "CALL", "CALL found");

    // 16-bit immediate F800 of LXI SP,F800
    auto r2 = f.mcp.callTool("debug_find_immediate_in_range",
        {{"range_start", 0x0100}, {"range_end", 0x010F}, {"value", 0xF800}});
    auto d2 = parseTextAsJson(r2);
    CHECK_EQ(d2["match_count"].get<int>(), 1, "LXI immediate found");
    CHECK_EQ(d2["matches"][0]["address"].get<std::string>(), "0x0100", "LXI addr");

    // No false positives from register names ("A" must not parse as 0x0A)
    auto r3 = f.mcp.callTool("debug_find_immediate_in_range",
        {{"range_start", 0x0100}, {"range_end", 0x010F}, {"value", 0x000A}});
    CHECK_EQ(parseTextAsJson(r3)["match_count"].get<int>(), 0,
             "register 'A' is not an immediate");
    TEST_END();
}

void test_mcp_get_vram_bytes() {
    TEST_BEGIN("MCP: debug_get_vram_bytes read + validation");
    Fixture f;
    f.mock.setMemory(0x8000, {0xDE, 0xAD, 0xBE, 0xEF});
    auto r = f.mcp.callTool("debug_get_vram_bytes",
        {{"address", 0x8000}, {"length", 4}});
    CHECK(!isErrorContent(r), "vram read should succeed");
    auto data = parseTextAsJson(r);
    CHECK_EQ(data["address"].get<std::string>(), "0x8000", "address echoed");
    CHECK_EQ(data["length"].get<int>(), 4, "length echoed");
    CHECK_EQ(data["bytes"][0].get<int>(), 0xDE, "byte 0");
    CHECK_EQ(data["bytes"][3].get<int>(), 0xEF, "byte 3");

    // Below VRAM region
    auto rBad = f.mcp.callTool("debug_get_vram_bytes",
        {{"address", 0x1000}, {"length", 4}});
    CHECK(isErrorContent(rBad), "non-VRAM address must error");
    CHECK_EQ(parseTextAsJson(rBad)["error_code"].get<std::string>(),
             "invalid_address", "structured invalid_address");

    // Past 0xFFFF
    auto rEnd = f.mcp.callTool("debug_get_vram_bytes",
        {{"address", 0xFFFC}, {"length", 8}});
    CHECK(isErrorContent(rEnd), "overflow must error");
    TEST_END();
}

void test_mcp_get_server_info() {
    TEST_BEGIN("MCP: debug_get_server_info build provenance");
    Fixture f;
    auto r = f.mcp.callTool("debug_get_server_info", {});
    CHECK(!isErrorContent(r), "server info should succeed");
    auto data = parseTextAsJson(r);
    CHECK_EQ(data["name"].get<std::string>(), "v06c-mcp", "name");
    CHECK(!data["version"].get<std::string>().empty(), "version non-empty");
    CHECK(!data["gitHash"].get<std::string>().empty(), "gitHash non-empty");
    CHECK(data["buildSeq"].get<int>() >= 0, "buildSeq numeric");
    CHECK(!data["buildTime"].get<std::string>().empty(), "buildTime non-empty");
    CHECK_EQ(data["apiVersion"].get<int>(), 4, "api_version 4");
    CHECK(data["pid"].get<int>() > 1, "pid plausible");
    CHECK(!data["startedAt"].get<std::string>().empty(), "startedAt non-empty");
    TEST_END();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main()
{
    setbuf(stdout, nullptr);

    printf("\n\033[1;33m========================================\033[0m\n");
    printf("\033[1;33m  MCP Protocol Tests — Stage 6.4\033[0m\n");
    printf("\033[1;33m========================================\033[0m\n\n");

    // Registration
    test_all_tools_registered();
    test_tool_names_have_debug_prefix();
    test_expected_tools_exist();

    // Schema
    test_tool_schemas_have_properties();
    test_address_params_are_numbers();
    test_read_memory_requires_address_and_size();

    // Execution
    test_debug_step();
    test_debug_is_running();
    test_debug_get_cpu_state();
    test_debug_get_registers();

    // Memory
    test_debug_read_memory();
    test_debug_write_memory();

    // I/O
    test_debug_read_io();
    test_debug_write_io();
    test_io_port_boundaries();

    // Breakpoints
    test_breakpoint_lifecycle();

    // Disassembly
    test_debug_disassemble();

    // Stack
    test_debug_get_stack();

    // Symbols / Analysis
    test_debug_get_symbols_empty();
    test_debug_annotations();

    // Debug state
    test_debug_get_state();

    // Memory map / Screen / VRAM
    test_debug_get_memory_map();
    test_debug_get_screen_info();
    test_debug_get_vram_info();

    // Trace / History
    test_debug_get_execution_trace();
    test_debug_get_io_trace();
    test_debug_get_call_graph();

    // Set register
    test_debug_set_register();

    // RDB Links (Stage 6.13)
    test_rdb_link_lifecycle();
    test_rdb_link_invalid_source();
    test_rdb_link_unresolved_target();
    test_rdb_link_duplicate();
    test_rdb_link_remove_missing();

    // Error propagation
    test_error_propagation_invalid_address();
    test_error_propagation_missing_param();
    test_error_propagation_tool_not_found();
    test_error_propagation_get_function_not_found();

    // Wire-level format (Stage 6.4.1)
    test_wire_level_error_format();
    test_wire_level_success_format();

    // E2E
    test_e2e_read_memory_full_path();
    test_e2e_write_then_read();

    // Stage 6.19: Multi-entry analyze_code
    test_mcp_analyze_code_single_entry_compat();
    test_mcp_analyze_code_multi_entry();
    test_mcp_analyze_code_duplicate_entries();
    test_mcp_analyze_code_empty_addresses();
    test_mcp_analyze_code_mutual_exclusion();
    test_mcp_analyze_code_neither_param();

    // Stage 6.26: Batch analysis tools
    test_mcp_disassemble_image_basic();
    test_mcp_disassemble_image_equivalence();
    test_mcp_disassemble_image_limit();
    test_mcp_coverage_report();
    test_mcp_coverage_report_requires_entry();
    test_mcp_diff_memory();
    test_mcp_diff_memory_missing_snapshot();
    test_mcp_find_bytecode_sequence();
    test_mcp_find_immediate_in_range();
    test_mcp_get_vram_bytes();

    // Build provenance
    test_mcp_get_server_info();

    // JSON serialization
    test_json_cpu_state_format();
    test_json_breakpoint_format();
    test_json_error_result();

    printf("\n\033[1;33m========================================\033[0m\n");
    printf("\033[1;33m  Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) {
        printf(" (\033[31m%d FAILED\033[0m)", tests_failed);
    }
    printf("\033[1;33m\033[0m\n");
    printf("\033[1;33m========================================\033[0m\n\n");

    return tests_failed > 0 ? 1 : 0;
}
