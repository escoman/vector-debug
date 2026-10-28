# Changelog

All notable changes to this project are documented in this file.
Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning: [Semantic Versioning](https://semver.org/).

<!--
The top-most `## [X.Y.Z]` section is used as the GitHub Release notes body
(see the "Build release notes from CHANGELOG" step in .github/workflows/release.yml).
Prepend new versions at the top; keep this first HTML comment free of `## [` lines.
-->

## [Unreleased]

### Added
- `debug_dump_memory` — MCP tool that writes a raw binary dump of a memory range
  (default: the full 64K address space, `0x0000..0xFFFF`) straight to a file and
  returns only `{path, address, length, sha256}`. A whole-memory dump no longer has to
  be assembled from `debug_read_memory_range` chunks, and the bytes never travel
  through the JSON transport, so large reads can no longer be truncated client-side.
  The write is atomic (`<path>.tmp` + rename, the `RdbController::saveAs` pattern) and
  the SHA-256 digest lets a caller verify the file afterwards. Bumps `v06c.api_version`
  to 7.

## [0.6.7] - 2026-10-24

Trustworthy screen snapshots when the debugger stops inside a frame: `debug_get_screen_snapshot`
now reports a mid-frame picture as partial, and concurrent readers can no longer
copy a torn framebuffer.

### Fixed
- `complete_frame` answered `true` for a picture that was half this frame and
  half the previous one. A breakpoint, watchpoint or single step that stops the
  CPU *inside* a frame leaves the TV framebuffer new above the beam and stale
  below it — correct for a slow LCD TV, but not a complete frame. The flag was
  derived from "is the emulator running", which says nothing about the video
  pass. `BeamState` now carries `frameIncomplete`, read straight from the video
  path (`PixelFiller::brk`: set when the raster wraps at the end of a frame,
  cleared only by `filler.reset()` in the `execute_frame` preamble), and
  `complete_frame` means `paused && frame complete`. MCP exposes `frame_incomplete`
  and `raster_line` (the tear point) on both `debug_get_screen_snapshot` metadata
  and `debug_get_beam_state`. An agent analyzing a screenshot no longer has to
  guess which scanline the mix happened on.
- `screenMutex_` guarded nothing: only the reader locked it, while the emulation
  thread wrote the TV buffer continuously, so `debug_get_screen_snapshot` and the
  GUI texture could capture a framebuffer caught mid-write. Both write paths
  (`executeFrame`, `stepInstruction`) now hold the mutex for the call, so a reader
  that owns it copies a buffer nobody is painting. The reader takes it with
  `try_lock` and never blocks the GUI render loop or the MCP transport behind a
  running frame. To keep the on-screen update rate tied to the emulation frame
  rate rather than to who happens to win the `try_lock` race, the emulation thread
  also publishes every completed frame into the `lastScreen_` cache at the frame
  boundary (still under `screenMutex_`), so a reader that loses the race is served
  the newest whole frame instead of one of unknown age — this removed a regression
  where a slow `executeFrame` starved the Vector Screen into rare updates. Lock
  order is always `screenMutex_` → `lastScreenMutex_` on both the reader and the
  writer, so no cycle exists. A paused emulator holds no lock, so the `try_lock`
  success path still reads a mid-frame buffer immediately.
- The GUI screen window read the beam and the texture in separate passes and
  could draw them out of sync; it now reads the beam once per frame and passes it
  down. When the frame is partial the stale region below the beam is dimmed, the
  tear line is drawn, and the status line says `frame incomplete: below beam =
  previous frame`.

### Notes
- `debug_pause` was never affected: `poll_debugger` runs at the top of
  `Board::execute_frame`, so a user pause always lands on a frame boundary. Only
  breakpoints, watchpoints and steps stop mid-frame.

## [0.6.6] - 2026-10-16

Byte-for-byte round-trip integrity for ROMs that contain unassigned i8080
opcodes: `v06c-asm-export` now preserves the byte instead of its `NOP` mnemonic.

### Fixed
- The ASM export silently substituted a byte of the image for each of the seven
  unassigned i8080 opcodes (`08h/10h/18h/20h/28h/30h/38h`). The disassembler
  decodes them as the 1-byte `NOP` and the exporter printed the mnemonic, so
  `28h` reassembled as `00h`. Nothing complained: `nop` is a valid instruction,
  so `toolchain_lint` and `z80asm -m=8080_strict` stayed green and only a
  byte-level diff (`roundtrip_verify`) could see the damage — on BASIC 2.997
  (`basic.r0m`) three bytes at `0x0AA7`, `0x0AE9` and `0x1CB5` were corrupted.
  Such a byte is now emitted as a commented `defb` row, the same way the truly
  undefined opcodes (`CBh/D9h/DDh/EDh/FDh/FFh`) were already carried through
  their `DB` format. A genuine `NOP` (`00h`) still prints as `nop`. Tagging the
  address as an RDB `Data` object did not help and does not change this: inside
  a code range the byte belongs to the instruction (Stage 6.23 §2.4).
  See `docs/Known_Issue_ASM_Exporter_NOP_Opcodes.md`.

## [0.6.5] - 2026-10-12

Reliable state control for AI-agent sessions: `debug_pause` now freezes, and
memory/register/I/O writes on a running machine report why they were refused.

### Fixed
- `debug_pause` blocked until the emulation thread actually reached the Paused
  state instead of returning success the instant the pause was requested. The
  previous fire-and-forget signal let the CPU keep advancing between MCP calls,
  so a "paused" machine was not deterministic and VRAM/snapshot comparison and
  code injection raced. A pause that fails to take effect now surfaces a clear
  `timeout` error rather than a false success.
- `debug_write_memory`, `debug_set_register` and `debug_write_io` on a running
  machine now return the explicit `not_paused` error code with an actionable
  message ("call debug_pause first") instead of the generic `*_failed`. A
  rejected write no longer looks like a data-size limit.

## [1.0.0] - 2026-10-02

First public release of the Vector-06C debugger and its MCP server.

### Added
- Vector-06C (КР580ВМ80А / Intel 8080) emulator with a Dear ImGui docking debugger UI.
- `v06c-mcp` — MCP server for AI agents (stdio transport) with ROM analysis tools.
- CI release pipeline (GitHub Actions): native Linux and Windows builds, artifact
  packaging, and a GitHub Release published on `v*` tags.
- `make release` — local Linux release packaging into `release/`.
- Tuned workspace preset `workspaces/Default.ini` shipped in every release bundle.

### Removed
- GUI "Call Graph" window and the `imgui-node-editor` dependency. The call-graph
  data remains available to AI agents via the MCP tool `debug_get_call_graph`.
