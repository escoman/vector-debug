# Stage 3.10 -- Execution Trace / History

## 1. Extend InstructionEvent with operand bytes

**File: `debugger/src/events.h`**

Add `uint8_t operandBytes[2] = {0, 0};` to `InstructionEvent`. This captures the full instruction bytes at execution time (correct for self-modifying code).

```cpp
struct InstructionEvent {
    uint64_t sequence;
    uint16_t pcBefore;
    uint16_t pcAfter;
    uint8_t opcode;
    uint8_t length;
    int cycles;
    uint8_t operandBytes[2] = {0, 0};  // NEW: bytes at PC+1 and PC+2
    CpuState before;
    CpuState after;
};
```

**File: `debugger/src/backend.cpp`** (stepInstruction)

Store the already-captured operand bytes into InstructionEvent:
```cpp
ie.operandBytes[0] = operandBytes[0];
ie.operandBytes[1] = operandBytes[1];
```

No other changes to `debugger/src/`. The existing `instructionHistorySnapshot()` API is used as-is.

## 2. ExecutionTraceWindow class

**New files: `debugger/gui/execution_trace_window.h`, `execution_trace_window.cpp`**

```cpp
class ExecutionTraceWindow {
public:
    ExecutionTraceWindow() {}
    void render(DebugBackend &backend);
    bool isVisible() const { return visible_; }
    void setVisible(bool v) { visible_ = v; }
    void requestRefresh() { needsRefresh_ = true; }

    // Navigation callbacks (Stage 3.9 pattern)
    std::function<void(uint16_t)> onGoToDisassembly;
    std::function<void(uint16_t)> onGoToMemoryInspector;

private:
    bool visible_ = true;
    bool followExecution_ = true;
    bool pauseCapture_ = false;
    bool needsRefresh_ = true;
    int maxEntries_ = 1000;         // display limit (user-configurable 100-10000)
    char searchBuffer_[64] = "";

    // Cached snapshot (refreshed when needsRefresh_)
    std::vector<InstructionEvent> cachedEntries_;
    uint64_t lastSnapshotSeq_ = 0;  // detect new data
};
```

**render() logic:**
1. If `!needsRefresh_` and no new data, use `cachedEntries_`
2. If `!pauseCapture_`: call `backend.instructionHistorySnapshot()`, cache result, update `lastSnapshotSeq_`
3. Render toolbar: `[x] Follow Execution` `[x] Pause Capture` `[Clear]` `Max entries: [1000]` `Search: [...]`
4. Render table: `Seq | PC | Bytes | Instruction | Cycles`
5. For each entry (last `maxEntries_` from snapshot):
   - **Seq**: `ie.sequence`
   - **PC**: `%04X` of `ie.pcBefore`
   - **Bytes**: opcode + operandBytes (reconstructed from event, NOT re-read from memory)
   - **Instruction**: use `disassemble()` with a readFn that returns stored bytes for the entry's address, or falls back to `backend.readMemory()` for display
   - **Cycles**: `ie.cycles`
6. If search text non-empty, filter rows by `Instruction` column containing search text
7. If `followExecution_`, auto-scroll to last row via `ImGui::SetScrollHereY(1.0f)`
8. Right-click context menu per row: "Go to Disassembly", "Go to Memory Inspector"
9. Double-click row: "Go to Disassembly"

**Disassembly for trace entries**: Build a `DisasmReadFn` that returns bytes from the InstructionEvent's stored opcode+operandBytes (correct even for self-modifying code). For the trace table, we construct the disassembly string from the stored data, not from current memory.

**Clear**: calls `backend.clearHistory()` -- clears all instrumentation history. Does NOT reset CPU or Board.

## 3. Integration in gui.h / gui.cpp

**gui.h**: Add `#include "execution_trace_window.h"` and member `ExecutionTraceWindow executionTrace_;`

**gui.cpp render()**:
- Wire callbacks: `executionTrace_.onGoToDisassembly = [this](uint16_t a) { gotoDisassembly(a); };`
- Wire callbacks: `executionTrace_.onGoToMemoryInspector = [this](uint16_t a) { gotoMemory(a); };`
- Render: `executionTrace_.render(backend);`

**gui.cpp renderControls()**:
- Add "Execution Trace" toggle button
- After Step/Run/Pause/Reset: `executionTrace_.requestRefresh();`

## 4. CMakeLists.txt

Add `${DBG_GUI_DIR}/execution_trace_window.cpp` to `GUI_SOURCES`.

## 5. Tests -- 8 new in test_backend.cpp

| # | Test | Checks |
|---|------|--------|
| 1 | `test_trace_single` | Step MVI A,55 -> trace.size()==1, PC==0, opcode==0x3E, length==2 |
| 2 | `test_trace_sequence` | Step 3 instructions -> trace[0..2].pc matches, sequence monotonic |
| 3 | `test_trace_jmp` | JMP 0010 then NOP -> trace shows 0000, 0010 (not 0003) |
| 4 | `test_trace_breakpoint` | BP at 0002, Run -> trace=[0000], PC=0002; Step -> trace=[0000,0002], PC=0003 |
| 5 | `test_trace_overflow` | RingBuffer capacity test: push >capacity, verify size==capacity, last entries preserved |
| 6 | `test_trace_clear` | Step several, clearHistory(), trace empty, CPU not reset |
| 7 | `test_trace_snapshot_isolation` | Get snapshot1, step more, snapshot1 unchanged |
| 8 | `test_trace_operand_bytes` | Step MVI A,55 -> ie.operandBytes[0]==0x55; Step JMP -> operandBytes has target |

## 6. Board smoke test

New section in `test_board_smoke.cpp` after Navigation:
- Step MVI A,55 -> trace has PC=0000, opcode=3E, length=2
- Step INR A -> trace has PC=0002, opcode=3C, length=1
- BP at 0002, reset+Run -> PC=0002, last trace PC=0000
- Step -> last trace PC=0002

## 7. Design decisions

1. **Trace buffer size**: Use existing ring buffer (capacity 10000). GUI displays last N entries (default 1000, configurable 100-10000 via input field).
2. **Thread safety**: Use existing `instructionHistorySnapshot()` which returns a copy under mutex. GUI caches the snapshot, only refreshes when `needsRefresh_` is set.
3. **Reset semantics**: Clear trace on Reset (simple approach). `clearHistory()` is called in `DebugBackend::reset()`.
4. **Fields from InstructionEvent**: sequence, pcBefore, opcode, length, cycles, operandBytes (new).
5. **Memory::buffer()**: NOT used. Bytes come from InstructionEvent.

## 8. Files changed

| File | Change |
|------|--------|
| `debugger/src/events.h` | +operandBytes[2] in InstructionEvent |
| `debugger/src/backend.cpp` | Store operandBytes in InstructionEvent |
| `debugger/gui/execution_trace_window.h` | New -- ExecutionTraceWindow class |
| `debugger/gui/execution_trace_window.cpp` | New -- render implementation |
| `debugger/gui/gui.h` | +include, +ExecutionTraceWindow member |
| `debugger/gui/gui.cpp` | +toggle button, +render call, +callbacks, +refresh |
| `debugger/CMakeLists.txt` | +execution_trace_window.cpp in GUI_SOURCES |
| `debugger/tests/test_backend.cpp` | +8 trace tests |
| `debugger/tests/test_board_smoke.cpp` | +trace smoke section |

## 9. Constraints

- `src/i8080.*`, `src/memory.*`, `src/board.*`, `src/io.*` NOT modified
- No `Memory::buffer()`
- No new parallel instruction event system
- Windows don't reference each other -- callbacks only
- Existing `instructionHistorySnapshot()` API used as-is
- `debugger/src/` changes limited to events.h (+field) and backend.cpp (+2 lines to populate it)
