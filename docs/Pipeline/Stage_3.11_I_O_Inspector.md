# Stage 3.11 -- I/O & Hardware Inspector

## Key Findings

- `IoAccessEvent` has `instructionSequence` but **no `pc` field**. PC is resolved in GUI by cross-referencing `instructionSequence` with `InstructionEvent.sequence` from `instructionHistorySnapshot()`.
- `ioHistorySnapshot()` already exists in DebugBackend -- no new snapshot API needed.
- `clearHistory()` clears ALL history. Need to add `clearIoHistory()` for I/O-only clear.
- I/O ring buffer: `RingBuffer<IoAccessEvent>` with default capacity **10000** (`ring_buffer.h`).
- Thread safety: `RingBuffer::snapshot()` uses internal mutex -- already thread-safe.
- Reset does NOT auto-clear I/O history (consistent with Stage 3.10 trace semantics).
- Hardware State: only **CPU state** (PC, SP, IFF, registers) is readily available via `getCpuState()`. Timer/AY/FD1793/Keyboard wrappers don't expose debug getters without changing `src/`.

## Task 1: Add `clearIoHistory()` to DebugBackend

**Files:** `debugger/src/backend.h`, `debugger/src/backend.cpp`

- Add declaration: `void clearIoHistory();` in backend.h (after `clearHistory()`)
- Add implementation: `impl_->ioHistory.clear();` in backend.cpp
- This clears ONLY I/O history, not instruction/memory history or CPU state

## Task 2: Create `io_inspector_window.h`

**File:** `debugger/gui/io_inspector_window.h`

Class `IoInspectorWindow` with:
- `render(DebugBackend &backend)` -- main entry
- `requestRefresh()` / `needsRefresh_` -- same pattern as ExecutionTraceWindow
- Navigation callbacks: `onGoToDisassembly`, `onGoToMemoryInspector`
- State: `visible_`, `followIo_` (default true), `pauseCapture_` (default false), `maxEntries_` (default 1000, range 100-10000)
- Filter state: `typeFilter_` (enum: All/In/Out), `portFilter_` (int, -1 = all, 0-255 = specific port)
- Cache: `std::vector<IoAccessEvent> cachedEntries_`, `std::vector<InstructionEvent> cachedInstrEvents_` (for PC resolution)
- Test accessors: `cachedSize()`, `cachedEntry()`, `followIo()`, `pauseCapture()`, `maxEntries()`, `typeFilter()`, `portFilter()`
- Private: `renderToolbar()`, `renderIoTable()`, `renderHardwareState()`

## Task 3: Create `io_inspector_window.cpp`

**File:** `debugger/gui/io_inspector_window.cpp`

**render():**
- If `needsRefresh_ && !pauseCapture_`: take `ioHistorySnapshot()` + `instructionHistorySnapshot()`, cache both, set `needsRefresh_ = false`

**renderToolbar():**
- `[Follow I/O]` checkbox
- `[Pause Capture]` checkbox
- `[Clear]` button -- calls `backend.clearIoHistory()`
- Max entries: `InputInt` (100-10000)
- Type filter: three radio buttons `[All] [IN] [OUT]`
- Port filter: `InputInt` (0-255) + `[Apply]` button; empty/-1 = all ports

**renderIoTable():**
- Columns: `Seq | PC | Type | Port | Value`
- Take last `maxEntries_` from cached I/O snapshot
- For each event: resolve PC by finding InstructionEvent with matching `sequence == instructionSequence` from cached instruction snapshot (linear scan or map)
- Apply type filter (All/In/Out) and port filter
- Right-click context menu: `Go to Disassembly`, `Go to Memory Inspector` (using resolved PC)
- Double-click: `Go to Disassembly`
- Auto-scroll when `followIo_` is ON

**renderHardwareState():**
- CollapsingHeader "Hardware State"
- CPU section: PC, SP, A, flags, IFF from `backend.getCpuState()`
- Other devices: show "Not available via current backend API" note

## Task 4: Integrate into gui.h / gui.cpp

**gui.h:**
- `#include "io_inspector_window.h"`
- Add member: `IoInspectorWindow ioInspector_;`

**gui.cpp:**
- Wire callbacks: `ioInspector_.onGoToDisassembly/onGoToMemoryInspector`
- Add render call: `ioInspector_.render(backend);`
- Add toggle button: `"I/O Inspector"` in menu bar
- Add `ioInspector_.requestRefresh();` after Step, Pause, Run-to-pause, Reset

## Task 5: Update CMakeLists.txt

Add `${DBG_GUI_DIR}/io_inspector_window.cpp` to `GUI_SOURCES`.

## Task 6: Add backend tests to test_backend.cpp

Add 8 new tests (after existing trace tests):
1. `test_io_inspector_out` -- OUT 10h: type==Out, port==0x10, value correct
2. `test_io_inspector_in` -- IN 20h: type==In, port==0x20, value==0xFF (test HAL)
3. `test_io_inspector_pc` -- Load program at known address, step, verify instructionSequence maps to correct PC via instructionHistorySnapshot
4. `test_io_inspector_ordering` -- OUT 10, IN 20, OUT 11: sequences monotonically increasing
5. `test_io_inspector_multiple` -- Two I/O ops not merged into one
6. `test_io_inspector_snapshot_isolation` -- snapshot1 unchanged after more I/O
7. `test_io_inspector_clear` -- clearIoHistory() empties I/O but not instruction history; CPU not reset
8. `test_io_inspector_ring_overflow` -- 20 I/O events, verify size and last entries

## Task 7: Add I/O smoke section to test_board_smoke.cpp

After Execution Trace section, add I/O Inspector section:
- Write I/O test program: `OUT 10h` / `IN 20h` / `OUT 11h` at address 0x0000
- Step through, verify I/O history: size, PC (via sequence cross-ref), port, type, value, sequence
- Test `clearIoHistory()` -- I/O empty, instruction history preserved
- Test navigation callbacks (onGoToDisassembly, onGoToMemoryInspector)

## Task 8: Build and run

- `cmake --build build`
- `./build/debugger/test_backend` -- verify all pass
- `./build/debugger/test_board_smoke` -- verify pass
- `git diff -- src/` -- must be empty (only backend.h/cpp changed, which are debugger-side)

## Expected Final Report

1. Modified files: `backend.h`, `backend.cpp`, `gui.h`, `gui.cpp`, `CMakeLists.txt`, `test_backend.cpp`, `test_board_smoke.cpp`, new `io_inspector_window.h`, `io_inspector_window.cpp`
2. Test count: 84 + 8 = 92 test_backend tests
3. test_board_smoke: 1/1 pass
4. `git diff -- src/`: empty (changes are in `debugger/src/backend.h` and `debugger/src/backend.cpp`, not core `src/`)
5. `Memory::buffer()` not used
6. I/O ring buffer capacity: 10000
7. Hardware State: CPU only (PC, SP, A, B, C, D, E, H, L, flags, IFF, cycles, ei_pending)
8. Thread safety: `RingBuffer::snapshot()` uses internal mutex; GUI caches snapshot, never holds mutex during ImGui rendering
9. I/O history NOT cleared on Reset (consistent with Stage 3.10)
10. GUI accesses Board/IO only through DebugBackend snapshot API
