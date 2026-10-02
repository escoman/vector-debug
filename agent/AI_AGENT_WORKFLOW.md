# AI Agent Workflow Protocol

## Overview

This document defines the standard workflow for an external AI Agent analyzing
Vector-06C ROMs via the Vector Debugger infrastructure.

The AI Agent is **external** to `vector-debugger`. The debugger provides:

- **MCP** — stateless protocol layer exposing Debugger operations
- **Agent API** — structured data access (limits enforced by API)
- **Knowledge Base** — technical reference about Vector-06C hardware
- **Task Library** — declarative analysis methodology
- **Profiles** — research type definitions combining Tasks + Knowledge

The debugger does **not** contain LLM runtime, analysis state, or reasoning engine.

---

## MCP-First Analysis Rule

When analyzing Vector-06C ROMs, the **MCP Debugger is the primary analysis tool**.

The AI Agent must **not** independently:

- read ROM as raw bytes and disassemble it on its own;
- decode instructions by opcode;
- manually build control flow instead of using MCP;
- interpret hex dumps as programs on its own;
- replace MCP results with its own disassembly.

Correct data flow:

```
ROM → MCP Debugger → DebugAdapter → Agent API → disassembly / CPU / memory / I/O / trace → Agent analysis
```

Forbidden data flow:

```
ROM → Agent → self-made disassembler
```

### MCP must be connected before analysis

Before analyzing a ROM, ensure the MCP Debugger is available. If the MCP server is not running, start it using the project-provided method. Do not fall back to self-disassembly just because MCP was not yet started.

If MCP cannot be started, report explicitly:

```
MCP Debugger is not available; reliable ROM analysis cannot be performed.
```

### Disassembly via MCP only

Use `debug_disassemble` to obtain instructions. The MCP result is the authoritative source of disassembly. The Agent analyzes address, opcode/instruction, operands, and control flow as returned by the Debugger — not by re-decoding opcodes independently.

### Priority of MCP over own computation

If the Agent can obtain information via MCP, it must use MCP. Self-computation is allowed **only as a check** on already-obtained MCP data — for correlation and reasoning, not as a replacement for the Debugger. If the Agent’s reasoning contradicts an MCP result, the MCP result takes priority.

### Forbidden fallback

Do not use this fallback:

```
MCP unavailable → take hex dump → self-disassemble ROM → analyze
```

Correct behavior:

```
MCP unavailable → report lack of Debugger evidence → do not present speculative ROM analysis as factual
```

> **The AI Agent is not a Vector-06C disassembler. The Agent is an analyst that uses the Debugger as its source of factual data.**

---

## Component Roles

| Component | Responsibility |
|-----------|---------------|
| AI Agent | Understand task, select Profile/Tasks, read Knowledge, plan analysis, call MCP, interpret results, form hypotheses, produce report |
| Profile | Defines which Tasks to use, which Knowledge is needed, what output is expected |
| Task | Defines methodology for a specific operation: what to analyze, what data is needed, which MCP tools to use, how to interpret results |
| Knowledge Base | Technical reference about Vector-06C (hardware, ports, memory, video, sound) |
| MCP | Stateless transport interface to Debugger operations |

Profile does not execute analysis. Task is not a script. MCP does not contain methodology.

---

## Main Workflow

Every analysis follows this sequence:

```
1.  Define the task
2.  Ensure MCP Debugger is available (if not, report and stop)
3.  Select Profile
4.  Load Profile
5.  Determine Tasks
6.  Load required Knowledge
7.  Form Analysis Plan
8.  Check Debugger state
9.  Load ROM if needed (debug_load_rom)
10. Read RDB (debug_get_rdb_info, debug_list_rdb_objects)
11. Obtain disassembly via MCP (debug_disassemble)
12. Set ROM mapping entry point = 0x0000
13. Execute MCP operations — explore control flow from 0x0000
14. Analyze results
15. Form hypotheses
16. Verify hypotheses with additional MCP operations
17. Evaluate evidence
18. Form result
19. Create/update RDB objects (debug_add_rdb_object, debug_set_rdb_comment)
20. Create confirmed RDB links (debug_add_rdb_link)
21. Save RDB (debug_save_rdb) — mandatory, do not wait for user request
22. Verify save result
23. Note limitations and unknowns
24. Produce final report (with objects count, links count, save status)
```

Steps 2, 9, 10, 11, and 12 are mandatory. Do not replace them with self-analysis of the ROM binary.

---

## ROM Mapping Entry Point

When performing initial ROM mapping, the entry point is:

```
0x0000
```

The Agent must begin control flow exploration from `0x0000`.

`_main` is **not** the ROM mapping entry point. For ROMs built with Z88DK or other compilers, `_main` may reside inside user code and be called by startup code. `_main` is an object discovered during analysis, not the starting point of ROM mapping.

Correct:
```
Mapping entry point: 0x0000
Known symbol: _main = 0x....
```

Incorrect:
```
Entry point: _main
```

If a `.map` file exists, its symbols are used for object identification but do not override the entry point rule.

Typical ROM structure discovered during mapping:
```
0x0000
  ↓
startup code
  ↓
_main
  ↓
user code
```

---

## Mandatory ROM Mapping Completion

A ROM mapping is **incomplete** unless all of the following are done:

1. RDB objects created
2. Confirmed RDB links created (`debug_add_rdb_link`)
3. RDB saved (`debug_save_rdb`) — **mandatory, do not wait for user command**
4. Save result verified

> Creating RDB objects without creating confirmed links is an incomplete ROM mapping.

> Creating or modifying RDB data without saving it is an incomplete ROM mapping.

The final report must include:
```
Mapping entry point: 0x0000
Objects: <count>
Links: <count>
RDB: <path>
RDB save: success / failed
```

---

## Analysis Plan

Before complex analysis, the Agent should form an internal plan.

Minimum structure:

```
Goal:
  What the analysis aims to determine.

Profile:
  Which profile is active.

Tasks:
  Which tasks will be executed.

Knowledge:
  Which KB documents are needed.

MCP Operations:
  Which MCP tools will be called and why.

Expected Evidence:
  What observations would support or refute the goal.

Validation Steps:
  How to verify findings.

Output:
  What the final deliverable looks like.
```

The Analysis Plan is not part of the API and does not need to be persisted.

---

## Evidence-First Analysis

The Agent must not make technical assertions based on assumption alone.

For each significant conclusion:

```
Observation:    What was directly seen (MCP result, memory content, instruction).
Evidence:       Supporting data (trace, memory map, multiple readings).
Interpretation: What the evidence means in context.
Conclusion:     The derived finding.
```

Example:

```
Observation:    Instruction writes to address C000h.
Evidence:       Execution trace + memory map shows C000h is in screen plane.
Interpretation: C000h belongs to bit plane 2 (8000h/A000h/C000h/E000h layout).
Conclusion:     Instruction modifies VRAM.
```

---

## Fact / Inference / Hypothesis

The Agent must distinguish three levels:

### Fact

Directly confirmed observation.

```
PC = 8123h
```

### Inference

Derived from multiple confirmed facts.

```
8123h lies inside a known code region.
```

### Hypothesis

Requires additional verification.

```
This routine may be responsible for sprite rendering.
```

**Never present a Hypothesis as a Fact.**

---

## Confidence

No complex mathematical system. Three levels:

| Level | Meaning |
|-------|---------|
| **high** | Sufficient independent confirmations exist |
| **medium** | Well-reasoned but has limitations |
| **low** | Based on incomplete data or requires further verification |

---

## Iterative Analysis Loop

```
Current hypothesis
    ↓
Select MCP operation
    ↓
Observe result
    ↓
Update hypothesis
    ↓
    ├── confirmed  → conclusion
    ├── rejected   → new hypothesis
    └── insufficient → additional test
```

The number of iterations should not be artificially limited if MCP API and resources allow continuing.

---

## Debugger State Awareness

Before state-dependent operations, the Agent must determine emulator state via:

```
debug_get_state
```

Do **not** assume:
- ROM is loaded
- CPU is paused
- CPU is running
- Breakpoint is active

After `run` / `pause` / `step` / `reset`, verify actual state. "run() accepted" is not equivalent to "CPU is running".

---

## ROM Handling

If analysis requires a ROM:

1. Call `debug_load_rom`
2. Verify load result
3. Read RDB (`debug_get_rdb_info`) to check existing database
4. Obtain CPU state, memory map, symbols as needed

Do **not** reload ROM if it is already loaded and the task does not require replacement.

---

## ROM Database (RDB)

RDB is the **sole persistent store** for ROM analysis results.

### RDB Workflow

```
read RDB (debug_get_rdb_info)
↓
analyze ROM
↓
add/modify objects via RDB API
↓
verify results
↓
save RDB (debug_save_rdb)
```

### RDB Rules

- All analysis findings (functions, labels, comments) go into RDB via MCP tools.
- Do **not** manually generate RDB JSON.
- Do **not** edit `.rdb` as a text file.
- Do **not** generate `.map` files to store analysis results.
- Save RDB after completing a batch of changes, not after every single operation.
- RDB is loaded automatically when a ROM is opened. If `.rdb` exists, it takes priority over `.map`.

### RDB Links

Links represent confirmed semantic relationships between RDB objects.

```
Analysis → Evidence → Create/update RDB objects → Create confirmed RDB links → Save RDB → Build Call Graph → Review
```

Rules:
- Use `debug_add_rdb_link` / `debug_remove_rdb_link` / `debug_get_rdb_links` for link management.
- Target object may not exist (unresolved link is allowed).
- Do not create links based on address proximity alone.
- Links require evidence from disassembly, trace, or confirmed control flow.
- Call Graph is a visualization of RDB links — it does not create them.
- `.rdb.graph` is visual data; `.rdb` contains semantic links.

Evidence levels for links:

```
Fact:       0100 contains CALL 0120.
Inference:  0100 references routine at 0120.
RDB:        debug_add_rdb_link(source=0x0100, target=0x0120)
```

A hypothesis should not automatically become a link. Verify first via MCP.

### Available RDB MCP Tools

| Tool | Purpose |
|------|---------|
| `debug_get_rdb_info` | RDB metadata: path, platform, dirty state, object count, ROM identity |
| `debug_list_rdb_objects` | List all objects sorted by address (with optional limit) |
| `debug_get_rdb_object` | Get single object by address |
| `debug_find_rdb_object` | Find object by name |
| `debug_add_rdb_object` | Add new object (address, name, type, size) |
| `debug_update_rdb_object` | Update existing object |
| `debug_remove_rdb_object` | Remove object |
| `debug_set_rdb_comment` | Set comment on object |
| `debug_set_rdb_property` | Set property on object |
| `debug_save_rdb` | Save RDB to disk |
| `debug_reload_rdb` | Reload from disk (discard unsaved changes) |
| `debug_add_rdb_link` | Add directed link from source to target |
| `debug_remove_rdb_link` | Remove directed link |
| `debug_get_rdb_links` | Get links from source object |

---

## Runtime Memory Analysis (Stage 6.20)

When analyzing ROMs containing packed data, runtime-generated structures, or
dynamic buffers, the Agent can observe **runtime memory behavior**:

1. **Memory Access Map** — 256-block heatmap showing which 256-byte pages are
   read, written, or fetched during execution, with per-page counters.
2. **Memory Access Log** — bounded FIFO of individual memory accesses with
   address, type (Read/Write/Fetch), PC attribution, and value.
3. **Memory Snapshots** — point-in-time captures of memory regions. Two
   snapshots can be compared to produce a list of changed ranges.

### Workflow for Packed Data Analysis

```
1. Clear access map (debug_clear_memory_access_map)
2. Run or step through the unpacking routine
3. Inspect access map (debug_get_memory_access_map) to see which pages were touched
4. Take snapshot A of the data area (debug_create_memory_snapshot)
5. Continue execution
6. Take snapshot B of the same area
7. Compare A vs B (debug_compare_memory_snapshots) to see exactly which bytes changed
```

### Available Runtime Analysis MCP Tools

| Tool | Purpose |
|------|--------|
| `debug_clear_memory_access_map` | Reset all access counters and log entries |
| `debug_get_memory_access_map` | Get 256-block access map (only active blocks returned) |
| `debug_get_memory_access_log` | Get recent access log entries (most recent last) |
| `debug_create_memory_snapshot` | Capture memory region (default: full 64K) |
| `debug_compare_memory_snapshots` | Diff two snapshots → list of changed ranges |

### ROM Isolation

All runtime state (access map, log, snapshots) is **invalidated on ROM load**.
This ensures stale data from a previous ROM does not contaminate analysis of
a new ROM.

---

## Raster / Racing-the-Beam Debugging (Stage 6.27)

**Inspection of VRAM alone is insufficient for raster effects.** Racing-the-beam
ROMs change the palette (`OUT 0x0C–0x0F`) mid-frame, so a static VRAM read cannot
reveal what actually appears on screen. To analyse such effects, query the live
video beam state through the Agent (all three tools are read-only — they never
pause, step, reset or re-render the emulation):

```
1. debug_get_beam_state      — where the beam is right now + palette under it
2. debug_get_raster_events   — recent OUT events correlated to beam position/pc
3. debug_get_screen_snapshot — the composed TV framebuffer as PNG
```

### Available Raster / Beam MCP Tools

| Tool | Purpose |
|------|---------|
| `debug_get_beam_state` | Beam position (`frame`, `raster_line`, `v_cycle_in_frame`/`_in_line`, `rpixel`), visible-area coords, CPU `pc`/`opcode` beside the beam, palette entry being shifted out (`palette_index`/`palette_value`) and hardware `border_index`. Timing constants (`line_v_cycles=768`, `frame_lines=312`, `frame_v_cycles=239616`) come from the emulator video model, never from MCP. |
| `debug_get_raster_events` | Ring of `OUT` instructions, each tagged with exact beam position (`frame`, `v_cycle`, `raster_line`, `v_cycle_in_line`) and `pc`/`port`/`value`, recorded on the emulation thread. Filters: `frame`, `v_cycle_start`/`v_cycle_end`, `port`, `pc`, `max_results` (default 1000, cap 50000). |
| `debug_get_screen_snapshot` | The real composed TV framebuffer returned as MCP **image content** (PNG) plus JSON metadata (`width`, `height`, `frame`, `source:"tv"`, `format:"RGB"`, `complete_frame`). Reveals mid-frame raster/palette effects that a VRAM reconstruction cannot. |

---

## Knowledge Base Usage

Knowledge Base is a technical reference, not an algorithm.

When Knowledge conflicts with observed emulator behavior:
- Report the conflict explicitly
- Do not silently choose one side

When `verification.md` contains `CONFLICT`:
- Include the conflict in reasoning
- Do not treat unverified claims as established facts

When a fact is marked `UNVERIFIED`:
- Do not present it as established

---

## Hardware vs Emulator Behavior

If a result confirms only current emulator behavior:

- **Incorrect**: "Original Vector-06C hardware definitely does X"
- **Correct**: "The current emulator implements X"
- **Correct**: "According to verified emulator behavior, X occurs"

If hardware behavior is independently confirmed, state so explicitly.

---

## Hypothesis Verification

After detecting suspicious behavior, do not immediately declare it a bug.

```
Observation
    ↓
Hypothesis
    ↓
Additional evidence
    ↓
Test
    ↓
Confirmed / Rejected / Uncertain
```

---

## Large Data Handling

Use existing Agent API limits:

```
MAX_DISASSEMBLY_COUNT
MAX_SYMBOLS_LIMIT
MAX_CALL_GRAPH_LIMIT
MAX_STACK_LIMIT
MAX_TRACE_ENTRIES
MAX_HISTORY_ENTRIES
```

Prefer small targeted queries over entire datasets.

Localize analysis before deep-diving:

```
ROM → memory map → symbols/functions → interesting function → disassembly → trace → specific instruction
```

Do not start with full 64 KB analysis if the task concerns a specific function.

---

## Working with Symbols

If ROM has `.map`/symbols: use symbols, functions, xrefs, call graph for precision.

Prefer: `function DRAW_FIRE at 8A20h`
Over: `routine at 8A20h`

If no symbols: use disassembly, trace, history, memory/I/O accesses, call graph.

Do not invent function names as established facts. Use temporary labels:

```
subroutine_8123
candidate_renderer
unknown_io_handler
```

---

## Bug Finding Report

Each bug finding must contain:

```
Location:           Address or range
Observed behavior:  What actually happens
Expected behavior:  What should happen (with justification)
Evidence:           MCP results, traces, memory dumps
Reasoning:          Why this is likely a bug
Confidence:         high / medium / low
Next verification:  Suggested follow-up check
```

Do not declare "bug" just because code looks unusual.

---

## Insufficient Evidence

If evidence is insufficient:

```
UNKNOWN
```

or:

```
UNCONFIRMED
```

is an acceptable result. Better to report "insufficient evidence" than to make an unjustified conclusion.

---

## Final Report Structure

```markdown
# Analysis

## Goal

## ROM

## Mapping Entry Point
0x0000

## Profile

## Tasks

## Objects
<count>

## Links
<count>

## RDB
<path>

## RDB Save
success / failed

## Findings

### Finding 1

Location:
Observation:
Evidence:
Conclusion:
Confidence:

### Finding 2
...

## Verified Facts

## Inferences

## Hypotheses

## Unknowns

## Limitations

## Recommended Next Steps
```

Simple tasks may use a shortened format.

---

## Reproducibility

For each significant finding, preserve:

- ROM used
- Address
- Relevant MCP operations
- Important parameters
- Observed result

This allows another Agent to repeat the verification. Not every MCP call needs to be saved.

---

## Self-Correction

If a new observation contradicts a previous conclusion:

- Do not hide the contradiction
- Revise the hypothesis
- Repeat necessary checks

Never silently ignore contradicting MCP results.

---

## Forbidden Assumptions

The Agent must not automatically assume:

- CPU clock
- Undocumented opcode behavior
- Hardware timing
- I/O semantics
- Memory behavior
- Video behavior
- Sound behavior

Unless confirmed by Knowledge Base, MCP observation, or another explicitly cited source.

---

## Minimal Agent Prompt Contract

```
You are analyzing a Vector-06C ROM.

MCP Debugger is your primary analysis tool.
Do not disassemble ROM independently — use debug_disassemble.
Do not decode opcodes on your own — use MCP results.
If MCP is unavailable, report this and do not perform speculative analysis.

Use the provided Profile, Tasks and Knowledge Base.

Use MCP tools to obtain evidence.

Do not invent hardware facts.

Distinguish:
- verified facts
- emulator behavior
- inference
- hypothesis
- unknown

When sources conflict, report the conflict.
If your reasoning contradicts MCP, MCP takes priority.

Do not treat an unverified claim as established fact.

Validate important hypotheses with additional MCP operations.

Produce an evidence-based final report.
```

This is a conceptual contract, not a ready-made system prompt for a specific LLM.

---

## Constraints

The following must **not** be added to the debugger:

- Analysis state in MCP
- Hypothesis engine
- Task runner / workflow engine
- AI memory / reasoning engine
- Agent executable / LLM integration
- RAG / embeddings / vector database

MCP remains a stateless protocol layer.
Task Library remains declarative Markdown.
Profiles remain declarative.
Analysis state lives in the external AI Agent.
