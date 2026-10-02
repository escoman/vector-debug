# Stage 3.8 — Disassembly View

## 1. DisassemblyWindow class (`debugger/gui/disassembly_window.h`, `disassembly_window.cpp`)

New files following the pattern of `breakpoints_window.h/cpp` and `memory_inspector_window.h`.

```cpp
class DisassemblyWindow {
public:
    void render(DebugBackend &backend);
    bool isVisible() const;
    void setVisible(bool v);
    void requestRefresh() { needsRefresh_ = true; }

    // Callback: Go to Memory Inspector (same pattern as StackViewWindow)
    std::function<void(uint16_t address)> onGoToMemoryInspector;

private:
    bool visible_ = true;
    bool followPc_ = true;
    bool needsRefresh_ = true;
    uint16_t viewAddress_ = 0;      // top of view
    uint16_t lastPc_ = 0xFFFF + 1;  // detect PC changes
    char addressInput_[8] = "";
    bool scrollToPc_ = false;       // request scroll on next frame

    void renderToolbar(DebugBackend &backend);
    void renderDisassemblyList(DebugBackend &backend);
    bool parseAddress(const char *input, uint16_t &address) const;
};
```

## 2. Disassembly rendering algorithm

**Read function**: use `backend.readMemory(addr)` — goes through `DebugMemoryAccess::peek()`, respects banking, no `Memory::buffer()`.

**Follow PC = ON** (default):
- `topAddr = pc - 60` (go back 60 bytes — enough for ~20 max-length instructions)
- Decode forward from `topAddr` using `disassemble()` + `opcode_info::get_length()`
- Skip lines until address < pc - 60 (discard partially decoded region)
- Continue decoding until ~40 lines collected or address wraps past `0xFFFF`
- After rendering, `ImGui::SetScrollHereY()` on the PC line

**Follow PC = OFF**:
- Start decoding from `viewAddress_`
- Collect ~40 lines or until address wraps
- No auto-scroll

**PC change detection**: each frame, if `followPc_` and `cpu.pc != lastPc_`, set `scrollToPc_ = true` and update `viewAddress_`.

**Address boundary**: all address arithmetic uses `uint16_t` wrapping. Stop decoding when the next address wraps past `0xFFFF` (i.e., `nextAddr < addr`).

## 3. Line format

```
→  ●  8003  C3 02 80   JMP 8002H
```

Columns: `PC_marker | BP_marker | Address | Bytes | Instruction`

- PC marker: `→` (yellow text) when address == PC
- BP marker: `●` (red text) when `backend.hasBreakpoint(addr)`
- Address: `%04X`
- Bytes: hex bytes from `DisassembledInstruction.bytes[]`, space-padded to 9 chars (max 3 bytes)
- Instruction: `DisassembledInstruction.text`

Highlight the PC line with a subtle background color.

## 4. Toolbar

```
Address: [____] [Go] [PC] [x] Follow PC    [Step]
```

- **Go**: parse hex, set `viewAddress_`, disable `followPc_`
- **PC**: set `viewAddress_ = pc`, enable scroll, keep `followPc_` state
- **Follow PC**: checkbox toggle
- **Step**: `backend.stepInstruction()`, then `requestRefresh()` on self + memory inspector + stack view

## 5. Context menu (right-click on instruction line)

Using `ImGui::BeginPopupContextItem()`:
- If no BP at line address: "Set Breakpoint" → `backend.addBreakpoint(addr)`
- If BP exists: "Remove Breakpoint" → `backend.removeBreakpoint(addr)`
- Separator
- "Go to Memory Inspector" → call `onGoToMemoryInspector(addr)` callback

Double-click on line → same "Go to Memory Inspector" action.

## 6. Integration in `gui.h` / `gui.cpp`

**gui.h**:
- `#include "disassembly_window.h"`
- Member: `DisassemblyWindow disassemblyView_;`

**gui.cpp** `render()`:
- Setup callback: `disassemblyView_.onGoToMemoryInspector = [this](uint16_t addr) { memoryInspector_.navigateTo(addr); };`
- Render: `disassemblyView_.render(backend);`

**gui.cpp** `renderControls()`:
- Add "Disassembly" toggle button alongside existing window toggles

**gui.cpp** after Step/Run/Pause/Reset buttons:
- Add `disassemblyView_.requestRefresh();` alongside existing `memoryInspector_.requestRefresh()` calls

## 7. CMakeLists.txt

Add `${DBG_GUI_DIR}/disassembly_window.cpp` to `GUI_SOURCES`.

## 8. Backend tests (`debugger/tests/test_backend.cpp`)

6 new tests:

| Test | Description |
|------|-------------|
| `test_disasm_nop` | `00` → NOP, length 1 |
| `test_disasm_mvi` | `3E 55` → MVI A,55H, length 2 |
| `test_disasm_jmp` | `C3 02 80` → JMP 8002H, length 3 |
| `test_disasm_sequential` | Decode at 8000→8002→8003, verify PC advancement |
| `test_disasm_boundary` | Decode at FFFF: returns 1-byte instruction, no crash |
| `test_disasm_banking` | Write bytes, switch bank, verify disassembler sees new virtual content |

All tests use `disassemble()` with a `DisasmReadFn` backed by `DebugMemoryAccess::peek()` or a simple buffer.

## 9. Board smoke test (`debugger/tests/test_board_smoke.cpp`)

New section after existing BP tests:
- Verify test program bytes (already in memory): `3E 55 / 3C / C3 02 00`
- Disassemble at 0000: verify `MVI A, 55`, length 2
- Disassemble at 0002: verify `INR A`, length 1
- Disassemble at 0003: verify `JMP 0002`, length 3
- Set BP at 0002, run, verify PC=0002, stopReason=Breakpoint
- Step, verify PC=0003, A=0x56
- Disassemble at PC (0003): verify `JMP 0002`

## 10. Files changed

| File | Change |
|------|--------|
| `debugger/gui/disassembly_window.h` | New — DisassemblyWindow class |
| `debugger/gui/disassembly_window.cpp` | New — render implementation |
| `debugger/gui/gui.h` | +include, +DisassemblyWindow member |
| `debugger/gui/gui.cpp` | +toggle button, +render call, +callback, +refresh calls |
| `debugger/CMakeLists.txt` | +disassembly_window.cpp in GUI_SOURCES |
| `debugger/tests/test_backend.cpp` | +6 disassembly tests |
| `debugger/tests/test_board_smoke.cpp` | +disassembly smoke section |

## 11. Key constraints

- `src/` not modified
- No `Memory::buffer()` — use `backend.readMemory()` (DebugMemoryAccess::peek)
- No duplicate opcode tables — use existing `disassemble()` + `opcode_info`
- No direct CPU/Board access from GUI — all through DebugBackend
- No local breakpoint list — use DebugBackend BP API as single source of truth
- No caching (simple re-disassembly each frame is fast enough for ~40 lines)
