# v06c-mcp — MCP Server for Vector-06C Debugger

## What is v06c-mcp?

`v06c-mcp` is a [Model Context Protocol](https://modelcontextprotocol.io/) (MCP) server that provides AI Agent access to the Vector-06C debugger through the existing Agent API.

It is a **thin protocol adapter** — all debugger logic lives in the Agent API layer. MCP only handles JSON-RPC transport and parameter serialization.

```
AI Agent → MCP Client → v06c-mcp → AgentApi → DebugBackend → Vector-06C
```

## Build

```bash
cd debugger/build
cmake .. -DENABLE_AI_AGENT=ON
make v06c-mcp
```

## Run (stdio transport)

```bash
./v06c-mcp
```

The server reads JSON-RPC messages from stdin and writes responses to stdout. Diagnostic messages go to stderr.

### MCP Client Configuration

Example Claude Desktop configuration (`claude_desktop_config.json`):

```json
{
  "mcpServers": {
    "vector-debugger": {
      "command": "/path/to/v06c-mcp"
    }
  }
}
```

## MCP Tools (73 tools)

All tools have the `debug_` prefix. Each is a thin wrapper over an AgentApi method.

> The authoritative tool list is what the server returns from `tools/list` — clients must
> not hardcode the count. Machine-readable capability marker: `v06c.api_version` in the
> `initialize` result (2 = Stage 6.26 batch analysis tools, 3 = Stage 6.27 raster/beam
> debugging tools). The grouped tables below were
> written for Stage 6.4 and do not list every tool added in Stages 6.11–6.25; the
> Stage 6.26 batch tools and Stage 6.27 raster/beam tools are documented at the end of
> this section.

### Execution (5)
| Tool | Description |
|---|---|
| `debug_run` | Start or resume CPU execution |
| `debug_pause` | Pause CPU execution |
| `debug_step` | Execute one CPU instruction while paused |
| `debug_reset` | Reset the CPU to initial state |
| `debug_is_running` | Check if the CPU is currently running |

### CPU (3)
| Tool | Description |
|---|---|
| `debug_get_cpu_state` | Get CPU register state |
| `debug_get_registers` | Get formatted register summary (AF, BC, DE, HL) |
| `debug_set_register` | Set a register value by name |

### Memory (2)
| Tool | Description |
|---|---|
| `debug_read_memory` | Read bytes from memory |
| `debug_write_memory` | Write bytes to memory |

### I/O (2)
| Tool | Description |
|---|---|
| `debug_read_io` | Read an I/O port value |
| `debug_write_io` | Write a value to an I/O port |

### Breakpoints (5)
| Tool | Description |
|---|---|
| `debug_set_breakpoint` | Set a breakpoint |
| `debug_remove_breakpoint` | Remove a breakpoint |
| `debug_list_breakpoints` | List all breakpoints |
| `debug_clear_breakpoints` | Remove all breakpoints |
| `debug_set_breakpoint_enabled` | Enable/disable a breakpoint |

### Disassembly / Trace (3)
| Tool | Description |
|---|---|
| `debug_disassemble` | Disassemble instructions at address |
| `debug_get_instruction_history` | Get last N executed instructions |
| `debug_get_execution_trace` | Get execution trace events |

### Stack (1)
| Tool | Description |
|---|---|
| `debug_get_stack` | Get current stack contents |

### Symbols / Analysis (5)
| Tool | Description |
|---|---|
| `debug_get_symbols` | List known symbols |
| `debug_get_function` | Get function info at address |
| `debug_get_function_context` | Full function context (disasm, xrefs, trace) |
| `debug_get_xrefs` | Get cross-references to address |
| `debug_get_call_graph` | Get call graph edges |

### Memory Map / Video (3)
| Tool | Description |
|---|---|
| `debug_get_memory_map` | Get 256-block memory map |
| `debug_get_vram_info` | Get VRAM layout info |
| `debug_get_screen_info` | Get screen mode parameters |

### I/O Trace (1)
| Tool | Description |
|---|---|
| `debug_get_io_trace` | Get I/O port access trace |

### Debug State (1)
| Tool | Description |
|---|---|
| `debug_get_state` | Full debugger state snapshot |

### ROM (1)
| Tool | Description |
|---|---|
| `debug_load_rom` | Load a ROM file |

### Annotations (6)
| Tool | Description |
|---|---|
| `debug_set_comment` | Set comment at address |
| `debug_set_function_comment` | Set function comment |
| `debug_rename_function` | Rename a function |
| `debug_create_function` | Create a function |
| `debug_delete_function` | Delete a function |
| `debug_add_label` | Add a label at address |

### Batch Analysis — Stage 6.26 (6)
| Tool | Description |
|---|---|
| `debug_disassemble_image` | Batch linear disassembly of a large ROM-image range (start + length ≤ 64K); result equals N × `debug_disassemble_range`; no code/data classification |
| `debug_coverage_report` | Aggregated coverage: code ranges, uncovered ranges, branch targets, branch targets without analyzed code; does NOT modify RDB |
| `debug_diff_memory` | Complete byte-for-byte snapshot diff with old/new values per contiguous range; limit overflow is a `limit_exceeded` error, never a silent cut |
| `debug_find_bytecode_sequence` | Byte pattern search (optional per-byte mask) in a range; returns addresses only, no interpretation |
| `debug_find_immediate_in_range` | Find instructions whose numeric operand equals a value (interpretation by the Debugger disassembler) |
| `debug_get_vram_bytes` | Raw VRAM bytes from the memory-mapped region 0x8000–0xFFFF via the existing target API |

Limits (§16): `MAX_DISASSEMBLE_IMAGE_INSTRUCTIONS=40000`, `MAX_SEARCH_MATCHES=1000`
(hard cap 10000), `MAX_BYTECODE_PATTERN_LENGTH=32`, `MAX_DIFF_CHANGED_RANGES=4096`,
`MAX_DIFF_CHANGED_BYTES=32768`, `MAX_VRAM_READ_RANGE=32768` — see `AgentLimits`.

### Raster / Beam — Stage 6.27 (3)
| Tool | Description |
|---|---|
| `debug_get_beam_state` | Read-only snapshot of the video beam/raster position (`frame`, `raster_line`, `v_cycle_in_frame`/`_in_line`, `rpixel`), the visible-area coords, the CPU `pc`/`opcode` beside the beam, and the palette entry the video path is currently shifting out (`palette_index`/`palette_value`) plus the separate hardware `border_index`. Timing constants (`line_v_cycles=768`, `frame_lines=312`, `frame_v_cycles=239616`) come from the emulator's video model, never computed by MCP. |
| `debug_get_screen_snapshot` | The real TV framebuffer, encoded as a PNG returned as MCP **image content** plus a JSON metadata text item (`width`, `height`, `frame`, `source:"tv"`, `format:"RGB"`, `complete_frame`). This is the composed output — NOT a reconstruction from VRAM — so it reveals mid-frame raster/palette effects. |
| `debug_get_raster_events` | Ring of `OUT` instructions, each correlated with the exact beam position (`frame`, `v_cycle`, `raster_line`, `v_cycle_in_line`) and `pc`/`port`/`value` at the moment it executed (recorded on the emulation thread). Optional filters: `frame`, `v_cycle_start`/`v_cycle_end`, `port`, `pc`, `max_results` (default 1000, cap 50000). |

All three are read-only: they never pause, step, reset or re-render the
emulation. Every value originates in `DebugAdapter` (the only Vector-aware
layer); the Agent API and MCP add no video timing of their own.

## Raster / Racing-the-Beam Debugging

> **Inspection of VRAM alone is insufficient for raster effects.** A ROM that
> races the beam rewrites the palette (ports `0x0C`–`0x0F`) or VRAM *while a
> frame is being drawn*; the value a given pixel ultimately shows depends on
> WHEN, relative to the beam, those writes happen. A static VRAM read can look
> unchanged while the screen visibly animates.

Use this chain instead:

```
debug_get_beam_state  →  where is the beam right now? what palette is it shifting out?
debug_get_raster_events →  which OUT landed on which raster_line / v_cycle?
debug_get_screen_snapshot →  what did the composed frame actually look like?
```

Typical investigation:
1. `debug_run`, then `debug_get_raster_events` filtered on `port: 0x0C` to find
   the palette writes and their `v_cycle`/`raster_line` — this tells you the
   scanline each colour change lands on.
2. `debug_get_screen_snapshot` to capture the actual pixels (decode the PNG) and
   compare against `debug_get_vram_bytes` — a divergence is the signature of a
   racing-the-beam effect.
3. `debug_get_beam_state` to anchor a breakpoint/step to a beam position.

## Architecture

```
debugger/mcp/
    mcp_adapter.h/cpp    — McpServer class (tool registration, callTool)
    mcp_json.h/cpp       — JSON serialization for Agent API types
    mcp_main.cpp         — v06c-mcp entry point (headless with real Board)
    tests/
        test_mcp_protocol.cpp — 60 tests (Stage 6.4.1 … 6.27)
    README.md

debugger/thirdparty/cpp-mcp/ — cpp-mcp library (MIT, hkr04/cpp-mcp)
```

### Dependencies

```
v06c-mcp
    → debugger_mcp (MCP adapter)
    → debugger_agent (AgentApi)
    → debugger_adapter (DebugAdapter + Board)
    → debugger_core (types, DebugBackend)
    → cpp-mcp (MCP protocol)
    → SDL2 (for headless Board)
    → pthread
```

**Stage 6.4.1**: Uses real `DebugAdapter`/`Board` instead of `NoBoardTarget`. Headless mode achieved via `Options.novideo=true` and `Options.nosound=true` — no SDL window or audio device is created, but full Vector-06C hardware emulation is available.

**Does NOT depend on**: OpenGL, ImGui.

## Limitations

- **stdio transport only** — no HTTP/SSE/WebSocket
- **No MCP Resources** — not implemented in this stage
- **No MCP Prompts** — not implemented in this stage
- **Single client** — one MCP server per debugger session
- **Headless mode** — no video/sound (but real Board emulation)

## MCP Protocol Compliance (Stage 6.4.1)

### CallToolResult Format

All tool responses follow the MCP `CallToolResult` format:

```json
{
  "content": [{"type": "text", "text": "..."}],
  "isError": true/false
}
```

`isError` is at the **top level** of the result object, not inside content items.

### Numeric Constraints

Tool schemas include numeric constraints where applicable:
- `address`: `minimum: 0, maximum: 65535` (uint16_t)
- `port`: `minimum: 0, maximum: 255` (uint8_t)
- `size`: `minimum: 1` (where applicable)
- `value`: `minimum: 0, maximum: 255` or `65535` (depending on context)

## Tests

```bash
make test_mcp_protocol
./test_mcp_protocol
```

60 tests covering (Stage 6.4.1 … 6.27):
- Tool registration (all 73 tools, unique names — tools/list reflects reality)
- Schema validation (types, required params, numeric constraints)
- Tool execution (via MockAgentBackend)
- Error propagation (AgentApiResult → MCP error)
- **Wire-level format** (CallToolResult with isError at top level)
- End-to-end (MCP → AgentApi → Mock → JSON)
- JSON serialization format
- I/O port boundaries

### Integration (real ROMs, real server)

```bash
python3 debugger/tests/integration/test_stage626_integration.py   # path to v06c-mcp auto-detected in build/
```

Spawns `v06c-mcp` over stdio in a temporary work dir and checks the Stage 6.26 batch
tools against real ROM images (`putup.rom`, `TESTAY.ROM`; override paths via
`V06C_PUTUP_ROM` / `V06C_TESTAY_ROM`). Missing ROMs or server binary → `SKIP`, exit 0.
Includes the §20 equivalence check `debug_disassemble_image == N × debug_disassemble_range`
and the §16 `limit_exceeded` no-silent-cut check.

```bash
python3 debugger/tests/integration/test_raster_debugging.py   # path to v06c-mcp auto-detected in build/
```

Stage 6.27 raster/beam integration: boots the default ROM, then exercises
`debug_get_beam_state` (frame/raster/v_cycle invariants), `debug_get_screen_snapshot`
(decodes the returned PNG and validates IHDR dimensions + zlib IDAT) and
`debug_get_raster_events` (monotonic `v_cycle`, port filter). The racing-the-beam
scenario is gated on `V06C_FIRE3_ROM`. Missing ROMs or server binary → `SKIP`, exit 0.

## Regression

All existing tests must pass:

```bash
./test_agent_api          # 115 tests (incl. Stage 6.27 beam/screen/raster)
./test_agent_commands     # 15 tests
./test_agent_contract     # 49 tests
./test_agent_integration  # 51 tests
./test_mcp_protocol       # 60 tests (Stage 6.4.1 … 6.27)
```

All `test_*` binaries in the build directory must pass.
