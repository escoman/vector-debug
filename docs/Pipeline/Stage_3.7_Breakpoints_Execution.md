## Stage 3.7 — Breakpoints & Execution Control

### 1. Breakpoint model (`debugger/src/backend.h`)

Replace existing `std::map<int, uint16_t> breakpoints_` with a proper model:

```cpp
struct DebuggerBreakpoint {
    uint16_t address;
    bool     enabled;
};
```

Add `StopReason` enum:

```cpp
enum class StopReason {
    None,
    Breakpoint,
    UserPause,
    Step,
    Reset
};
```

New storage: `std::map<int, DebuggerBreakpoint> breakpoints_` (keeps int id for internal use).

New member: `StopReason stopReason_ = StopReason::None;`

### 2. Expanded Breakpoint API (`debugger/src/backend.h`)

```cpp
// Replace existing breakpoint API:
int  addBreakpoint(uint16_t address);       // returns id, or -1 if duplicate
bool removeBreakpoint(uint16_t address);    // remove by address
bool setBreakpointEnabled(uint16_t address, bool enabled);
bool hasBreakpoint(uint16_t address) const;
std::vector<DebuggerBreakpoint> getBreakpoints() const;  // thread-safe snapshot
void clearBreakpoints();
StopReason getStopReason() const;
```

Keep `removeBreakpoint(int id)` for backward compat with tests.

### 3. Backend implementation (`debugger/src/backend.cpp`)

**`addBreakpoint()`**: Check for duplicate address first. If exists, return -1. Otherwise assign new id.

**`removeBreakpoint(address)`**: Find by address, erase. Return false if not found.

**`checkBreakpoint()`**: Update to check `enabled` flag.

**`getBreakpoints()`**: Take snapshot under `commandMutex_`.

**`getStopReason()`**: Return `stopReason_`.

**Set StopReason in appropriate places:**
- `run()` loop, breakpoint hit: `stopReason_ = StopReason::Breakpoint`
- `run()` loop, pause: `stopReason_ = StopReason::UserPause`
- `stepInstruction()` called: `stopReason_ = StopReason::Step`
- `reset()`: `stopReason_ = StopReason::Reset`
- `processOneCommand()` Board path, breakpoint: `stopReason_ = StopReason::Breakpoint`
- `processOneCommand()` no-Board path, breakpoint: `stopReason_ = StopReason::Breakpoint`

**"Run after breakpoint" step-over:**

In `requestRun()`, before setting state to Running:
1. Check if current PC has an enabled breakpoint
2. If yes: set a flag `skipBreakpoint_` = true
3. In the run loop, when `skipBreakpoint_` is true and PC matches a breakpoint, execute one instruction first, then clear the flag

For the no-Board path (in `processOneCommand()` execution loop):
```cpp
if (checkBreakpoint()) {
    if (skipBreakpoint_) {
        skipBreakpoint_ = false;
        stepInstruction();  // step past the breakpoint
        continue;
    }
    stopReason_ = StopReason::Breakpoint;
    state_ = DebuggerState::Paused;
    break;
}
```

For the Board path: sync breakpoints to Board, but handle skip in `poll_debugger`:
- Before syncing, if PC is at a breakpoint, do `stepInstruction()` first, then sync and run
- Or: set `skipBreakpoint_` flag, and in `poll_debugger`, if flag is set and PC at breakpoint, step and clear flag

**Board breakpoint sync:**

Before entering the Board execution loop in `processOneCommand()`:
1. Clear Board's breakpoints: `board_->remove_breakpoint(0/1, addr, kind)` for each — or simpler: just add all enabled debugger breakpoints to Board
2. For each enabled `DebuggerBreakpoint`: `board_->insert_breakpoint(0, bp.address, 1)`
3. Set `board_->debugging = 1` (already done via `debugger_attached()`)

When Board stops (debugger_interrupt set in poll_debugger or execute_frame returns):
- Check if it was a breakpoint stop → set `stopReason_`

**Reset preserves breakpoints:**
`reset()` already doesn't touch `breakpoints_`. Just add `stopReason_ = StopReason::Reset`.

### 4. Breakpoints Window (`debugger/gui/breakpoints_window.h`, `breakpoints_window.cpp`)

New files following the pattern of `stack_view_window.h/cpp`:

```cpp
class BreakpointsWindow {
public:
    void render(DebugBackend &backend);
    bool isVisible() const;
    void setVisible(bool v);
private:
    bool visible_ = true;
    char addressInput_[8] = "";
    int selectedBpIndex_ = -1;
};
```

Layout:
```
Breakpoints
  Address: [____] [Add]
  ┌──────────────────────────────────┐
  │ [x]  8000  JMP 8000             │
  │ [x]  8123  CALL 4000            │
  │ [ ]  9000  MVI A,55             │
  └──────────────────────────────────┘
  [Remove] [Clear All]
```

- Checkbox toggles `enabled` via `backend.setBreakpointEnabled()`
- Add: parse hex input, call `backend.addBreakpoint()`
- Remove: call `backend.removeBreakpoint(address)` for selected
- Clear All: call `backend.clearBreakpoints()`
- Disassembly column uses `Disassembler` with `backend.readMemory()`

### 5. Memory Inspector breakpoint markers (`debugger/gui/memory_inspector_window.cpp`)

In `renderMemoryView()`, for each line:
- Check if any address in the line has a breakpoint via `backend.hasBreakpoint(addr)`
- If yes, show a marker (e.g., red dot or `B` prefix) before the address

Add right-click context menu:
- `ImGui::BeginPopupContextItem()` on the memory view area
- "Set Breakpoint" / "Remove Breakpoint" for `selectedAddress_`
- Calls `backend.addBreakpoint()` / `backend.removeBreakpoint()`

### 6. Status bar — Stop Reason (`debugger/gui/gui.cpp`)

In `renderStatusBar()`, show stop reason:
```cpp
const char *reasonStr = "";
switch (backend.getStopReason()) {
    case StopReason::Breakpoint: reasonStr = " (Breakpoint at XXXX)"; break;
    case StopReason::UserPause:  reasonStr = " (User Pause)"; break;
    case StopReason::Step:       reasonStr = " (Step)"; break;
    case StopReason::Reset:      reasonStr = " (Reset)"; break;
    default: break;
}
```

### 7. Controls — Breakpoints toggle (`debugger/gui/gui.cpp`)

Add "Breakpoints" button in `renderControls()` alongside Memory Inspector and Stack View toggles.

### 8. Integration in gui.h

Add `BreakpointsWindow breakpointsWindow_` member.

### 9. Tests (`debugger/tests/test_backend.cpp`)

Add 10 tests:

1. **test_bp_add** — add breakpoint, verify `hasBreakpoint()`
2. **test_bp_duplicate** — add same address twice, verify only one exists
3. **test_bp_remove** — add then remove, verify gone
4. **test_bp_enable_disable** — toggle enabled, verify via snapshot
5. **test_bp_multiple** — add 3 breakpoints, verify snapshot
6. **test_bp_clear** — add several, clear all, verify empty
7. **test_bp_break_before_execution** — place instruction at addr, set BP, run(), verify PC at BP addr, instruction NOT executed, stopReason == Breakpoint
8. **test_bp_continue_after** — after hitting BP, run again, verify instruction at BP was executed (step-over semantics)
9. **test_bp_step_over** — PC at BP, step, verify exactly one instruction executed
10. **test_bp_reset_preserves** — add BP, reset, verify BP still exists

### 10. Real Board smoke test (`debugger/tests/test_board_smoke.cpp`)

Add section:
- Write test program: `0000: MVI A,55 / 0002: INR A / 0003: JMP 0002`
- Add breakpoint at 0002
- Step to 0002 (or run)
- Verify: PC=0002, A=55 (INR A not yet executed)
- Step: PC=0003, A=56

### 11. Files changed

| File | Change |
|------|--------|
| `debugger/src/backend.h` | +DebuggerBreakpoint struct, StopReason enum, expanded BP API, skipBreakpoint_ |
| `debugger/src/backend.cpp` | BP API impl, stopReason tracking, skip logic, Board sync |
| `debugger/gui/breakpoints_window.h` | New file — BreakpointsWindow class |
| `debugger/gui/breakpoints_window.cpp` | New file — render breakpoint list + controls |
| `debugger/gui/gui.h` | +BreakpointsWindow member |
| `debugger/gui/gui.cpp` | Status bar stop reason, Breakpoints toggle, render call |
| `debugger/gui/memory_inspector_window.h` | +breakpoint marker support |
| `debugger/gui/memory_inspector_window.cpp` | BP markers, right-click context menu |
| `debugger/tests/test_backend.cpp` | +10 breakpoint tests |
| `debugger/tests/test_board_smoke.cpp` | +BP smoke test section |

### 12. Key constraints

- `src/` not modified
- All BP operations through DebugBackend
- Thread safety via commandMutex_ for snapshot API
- Board sync uses existing `insert_breakpoint()` / `remove_breakpoint()` — no src/ changes
- Disabled breakpoints excluded from Board sync and checkBreakpoint()
