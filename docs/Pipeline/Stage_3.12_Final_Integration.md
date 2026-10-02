# Stage 3.12 -- Final Integration / Cleanup

## Audit Results (no issues found)

After thorough code review, the following areas are confirmed correct:
- **Architecture**: GUI -> DebugBackend -> command protocol -> emulation thread -> Board. All commands (Step, Run, Pause, Reset, Quit, MemoryWrite, RegisterWrite) go through the command protocol.
- **Thread safety**: All snapshot APIs use RingBuffer internal mutex; GUI caches snapshots; no mutex held during ImGui rendering.
- **Ring buffers**: Instruction history (10000), I/O history (10000), Memory history (50000) -- all correct overflow/clear behavior.
- **Self-modifying code**: Execution Trace uses stored opcode/operandBytes from InstructionEvent, not current memory. Disassembly uses current memory via DebugMemoryAccess::peek().
- **Navigation**: All cross-window callbacks wired correctly (Stage 3.9 pattern).
- **Breakpoints**: Preserved across reset (verified in test_bp_reset_preserves).
- **No `Memory::buffer()` usage** in debugger code.
- **`src/` not modified** -- all changes in `debugger/`.
- **`onTrace`/`formatTraceLine`**: Still used by test_trace (Stage 1 regression test) -- kept for backward compatibility.

## Task 1: Remove debug printf from board_wrapper.cpp

**File:** `debugger/gui/board_wrapper.cpp`

Remove 7 `printf` statements (lines 37, 57, 70, 84, 89, 93, 102). These are debug output from initial development that shouldn't be in production GUI code. The GUI has proper status bar and window feedback -- these console messages are unnecessary.

## Task 2: Fix per-frame snapshot in renderInstructionHistory

**File:** `debugger/gui/gui.cpp`

`renderInstructionHistory()` (line 394) calls `backend.instructionHistorySnapshot()` every ImGui frame. This copies the entire ring buffer (up to 10000 entries) on every frame -- a performance issue flagged by Section 12 of the spec.

Fix: Add a cached snapshot with `needsRefresh_` pattern, same as ExecutionTraceWindow. Add member variables to DebuggerGui:
- `std::vector<InstructionEvent> cachedHistEntries_`
- `bool histNeedsRefresh_ = true`

Call `instructionHistorySnapshot()` only when `histNeedsRefresh_` is true. Set `histNeedsRefresh_ = true` in the same places where `executionTrace_.requestRefresh()` is called (Step, Pause, Reset).

## Task 3: Add 10 integration tests to test_backend.cpp

**File:** `debugger/tests/test_backend.cpp`

Add these tests (after existing I/O Inspector tests, before main()):

1. `test_integration_reset_step` -- Reset, then Step; verify PC changes
2. `test_integration_bp_run` -- Add BP, Run; verify stop at BP
3. `test_integration_memwrite_disasm` -- Write memory, read it back via readMemory (simulating disassembly refresh)
4. `test_integration_regwrite_navigation` -- Write PC register, verify getCpuState().pc updated
5. `test_integration_trace_disasm` -- Step instructions, verify trace entries have correct PC that maps to disassembly addresses
6. `test_integration_stack_memory` -- Push to stack (modify SP), read stack memory via readMemorySnapshot
7. `test_integration_io_disasm` -- Execute OUT, verify I/O event's instructionSequence maps to correct instruction PC
8. `test_integration_reset_bp_preserved` -- Add BP, Reset, verify BP still present
9. `test_integration_reset_history_preserved` -- Step instructions, Reset, verify instruction history preserved (reset does NOT clear history)
10. `test_integration_quit_while_running` -- requestRun(), then requestQuit(); verify no deadlock (backend reaches Stopped/Paused state)

## Task 4: Clean build and test

- `cmake --build build` for test_backend and test_board_smoke
- Run test_backend: expect 92 + 10 = 102 tests, all pass
- Run test_board_smoke: expect 1/1 pass
- `git diff -- src/` must be empty
- `git diff --check` must be clean

## Expected Final Report

### Changed files
- `debugger/gui/board_wrapper.cpp` -- removed 7 debug printf
- `debugger/gui/gui.h` -- added cachedHistEntries_, histNeedsRefresh_
- `debugger/gui/gui.cpp` -- fixed renderInstructionHistory caching
- `debugger/tests/test_backend.cpp` -- added 10 integration tests

### Tests
- test_backend: 102/102
- test_board_smoke: 1/1

### src/ integrity
- `git diff -- src/`: empty
