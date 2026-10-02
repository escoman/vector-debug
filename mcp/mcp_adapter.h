#pragma once

// ---------------------------------------------------------------------------
// McpServer — Stage 6.4
//
// Thin MCP adapter over AgentApi.
// Registers 38 debug_* tools and runs stdio transport.
//
// MCP → AgentApi only. Never touches Board, Memory, DebugAdapter directly.
// ---------------------------------------------------------------------------

#include "mcp_server.h"   // cpp-mcp mcp::server
#include "mcp_tool.h"     // cpp-mcp tool_builder
#include "agent_api.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

class McpServer
{
public:
    explicit McpServer(AgentApi &api);
    ~McpServer();

    // Register all 38 debug_* tools.
    void registerAllTools();

    // Run stdio transport (blocks until stdin closes).
    void runStdio();

    // Access the underlying cpp-mcp server (for testing).
    mcp::server &server() { return *server_; }
    const mcp::server &server() const { return *server_; }

    // Get list of registered tool names (for testing).
    std::vector<std::string> registeredToolNames() const;

    // Direct tool invocation (for testing — bypasses JSON-RPC transport).
    // Calls the registered handler for 'toolName' with given params.
    // Throws mcp::mcp_exception if tool not found.
    mcp::json callTool(const std::string &toolName, const mcp::json &params = mcp::json::object());

private:
    AgentApi &api_;
    std::unique_ptr<mcp::server> server_;

    // Local tool handler map for direct invocation (testing).
    std::map<std::string, mcp::tool_handler> handlers_;

    // -- Tool registration helpers ------------------------------------------

    void registerExecutionTools();
    void registerCpuTools();
    void registerMemoryTools();
    void registerIoTools();
    void registerKeyboardTools();       // virtual keyboard injection
    void registerBreakpointTools();
    void registerDisassemblyTools();
    void registerStackTools();
    void registerSymbolTools();
    void registerMemoryMapTools();
    void registerIoTraceTools();
    void registerDebugStateTools();
    void registerRomTools();
    void registerAnnotationTools();
    void registerRdbTools();
    void registerRuntimeAnalysisTools();  // Stage 6.20
    void registerBatchAnalysisTools();    // Stage 6.26
    void registerRasterTools();           // Stage 6.27: beam / raster

    // Register a tool with both cpp-mcp server and local handler map.
    void registerTool(const mcp::tool &tool, mcp::tool_handler handler);

    // -- MCP tool result helpers --------------------------------------------

    // Convert AgentApiResult<void> to MCP CallToolResult. Throws on failure.
    mcp::json requireVoidResult(const AgentApiResult<void> &result);

    // Build MCP CallToolResult (success): {content: [...], isError: false}
    static mcp::json textContent(const mcp::json &data);

    // Build MCP CallToolResult (error): {content: [...], isError: true}
    static mcp::json errorContent(const std::string &errorCode, const std::string &message);
};
