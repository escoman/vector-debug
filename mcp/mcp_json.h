#pragma once

// ---------------------------------------------------------------------------
// mcp_json — Stage 6.4
//
// JSON serialization helpers for Agent API types.
// Converts CpuState, breakpoints, disassembly, symbols, traces, etc.
// to mcp::json (nlohmann::ordered_json).
//
// No dependency on Board, Memory, CPU, IO, or emulator internals.
// Only depends on agent_types.h and cpp-mcp json.hpp.
// ---------------------------------------------------------------------------

#include "json.hpp"
#include "agent_types.h"
#include "debugger_types.h"
#include "events.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mcp_json {

using json = nlohmann::ordered_json;

// -- Address formatting ------------------------------------------------------

// Format uint16_t as "0xHHHH" hex string.
inline std::string hex16(uint16_t v) {
    char buf[8];
    snprintf(buf, sizeof(buf), "0x%04X", v);
    return buf;
}

// Format uint8_t as "0xHH" hex string.
inline std::string hex8(uint8_t v) {
    char buf[6];
    snprintf(buf, sizeof(buf), "0x%02X", v);
    return buf;
}

// -- CpuState ----------------------------------------------------------------

json cpuStateToJson(const CpuState &cpu);

// -- DebuggerBreakpoint ------------------------------------------------------

json breakpointToJson(const DebuggerBreakpoint &bp);
json breakpointsToJson(const std::vector<DebuggerBreakpoint> &bps);

// -- DisassembledInstructionResult -------------------------------------------

json disassembledInstructionToJson(const DisassembledInstructionResult &inst);
json disassembledInstructionsToJson(const std::vector<DisassembledInstructionResult> &insts);

// -- InstructionHistoryEntry -------------------------------------------------

json historyEntryToJson(const InstructionHistoryEntry &entry);
json historyEntriesToJson(const std::vector<InstructionHistoryEntry> &entries);

// -- InstructionEvent (execution trace) --------------------------------------

json instructionEventToJson(const InstructionEvent &ev);
json instructionEventsToJson(const std::vector<InstructionEvent> &events);

// -- IoAccessEvent -----------------------------------------------------------

json ioAccessEventToJson(const IoAccessEvent &ev);
json ioAccessEventsToJson(const std::vector<IoAccessEvent> &events);

// -- StackEntry --------------------------------------------------------------

json stackEntryToJson(const StackEntry &entry);
json stackEntriesToJson(const std::vector<StackEntry> &entries);

// -- MemoryMapBlock ----------------------------------------------------------

json memoryMapBlockToJson(const MemoryMapBlock &block);
json memoryMapBlocksToJson(const std::vector<MemoryMapBlock> &blocks);

// -- ScreenInfoResult --------------------------------------------------------

json screenInfoToJson(const ScreenInfoResult &info);

// -- VramInfoResult ----------------------------------------------------------

json vramInfoToJson(const VramInfoResult &info);

// -- SymbolInfo --------------------------------------------------------------

json symbolInfoToJson(const SymbolInfo &sym);
json symbolInfosToJson(const std::vector<SymbolInfo> &syms);

// -- XrefResult --------------------------------------------------------------

json xrefResultToJson(const XrefResult &xref);
json xrefResultsToJson(const std::vector<XrefResult> &xrefs);

// -- CallGraphEdge -----------------------------------------------------------

json callGraphEdgeToJson(const CallGraphEdge &edge);
json callGraphEdgesToJson(const std::vector<CallGraphEdge> &edges);

// -- LoadRomResult -----------------------------------------------------------

json loadRomResultToJson(const LoadRomResult &result);

// -- DebugStateResult --------------------------------------------------------

json debugStateToJson(const DebugStateResult &state);

// -- AgentScreenSnapshot -----------------------------------------------------

json screenSnapshotToJson(const AgentScreenSnapshot &snap);

// -- Beam / raster state (Stage 6.27) ----------------------------------------

json beamStateToJson(const AgentBeamState &beam);

// -- FunctionContext ---------------------------------------------------------

json functionContextToJson(const FunctionContext &ctx);

// -- TraceResult -------------------------------------------------------------

json traceResultToJson(const TraceResult &result);

// -- RDB types (Stage 6.11) --------------------------------------------------

json rdbInfoToJson(const RdbInfoResult &info);
json rdbObjectToJson(const RdbObjectResult &obj);
json rdbObjectsToJson(const std::vector<RdbObjectResult> &objects);
json rdbLinksToJson(uint16_t source, const std::vector<uint16_t> &links);

// -- Runtime Memory Analysis types (Stage 6.20) ------------------------------

bool runtimeBlockActive(const RuntimeAccessBlock &block);   // Stage 6.22 §3
json runtimeAccessBlockToJson(const RuntimeAccessBlock &block);
json runtimeAccessBlocksToJson(const std::vector<RuntimeAccessBlock> &blocks);
json runtimeAccessLogEntryToJson(const RuntimeAccessLogEntry &entry);
json runtimeAccessLogEntriesToJson(const std::vector<RuntimeAccessLogEntry> &entries);
json memorySnapshotDiffToJson(const MemorySnapshotDiff &diff);

// -- Error response ----------------------------------------------------------

// Build MCP error content from AgentApiResult failure.
json errorResult(ErrorCode code, const std::string &message);

// Build MCP success content wrapping a value.
json successResult(const json &value);

// Build MCP success content for void operations.
json successVoidResult();

} // namespace mcp_json
