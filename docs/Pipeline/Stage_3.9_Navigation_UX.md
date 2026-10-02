# Stage 3.9 — Navigation & Cross-Reference UX

## 1. Central Navigation API in `gui.h`

Add public methods to `DebuggerGui`:

```cpp
void gotoMemory(uint16_t address);
void gotoDisassembly(uint16_t address);
void gotoStack(uint16_t address);
```

Each method:
1. Opens the target window (`setVisible(true)`)
2. Calls `gotoAddress(address)` on it
3. Does NOT change Follow PC/SP state (manual navigation never toggles Follow)

Replace all existing per-window `onGoToMemoryInspector` callbacks with the central API. Windows get new callbacks pointing to the Gui navigation methods instead.

## 2. MemoryInspectorWindow changes

**Header** (`memory_inspector_window.h`):
- Rename `navigateTo()` to `gotoAddress()` (same semantics, clearer name)
- Add `bool pendingScroll_ = false;`
- Add `uint16_t address() const { return address_; }` getter (for tests)
- Add `std::function<void(uint16_t)> onGoToDisassembly;` callback

**Implementation** (`memory_inspector_window.cpp`):
- `gotoAddress()`: set `address_`, `selectedAddress_`, update `addressInput_`, set `pendingScroll_ = true`, `needsRefresh_ = true`, `visible_ = true`
- In `renderMemoryView()`: after clipper loop, if `pendingScroll_`, calculate target line for `selectedAddress_`, call `ImGui::SetScrollY()`, clear flag
- In existing right-click context menu: add "Go to Disassembly" item calling `onGoToDisassembly(selectedAddress_)`

## 3. DisassemblyWindow changes

**Header** (`disassembly_window.h`):
- Add `void gotoAddress(uint16_t address);`
- Add `bool pendingScroll_ = false;`
- Add `uint16_t address() const { return viewAddress_; }` getter
- Keep existing `onGoToMemoryInspector` callback

**Implementation** (`disassembly_window.cpp`):
- `gotoAddress()`: set `viewAddress_ = address`, `followPc_ = false`, `pendingScroll_ = true`, `needsRefresh_ = true`, `visible_ = true`, update `addressInput_`
- In `renderDisassemblyList()`: when rendering the line matching `viewAddress_`, if `pendingScroll_`, call `ImGui::SetScrollHereY(0.3f)`, clear flag

## 4. StackViewWindow changes

**Header** (`stack_view_window.h`):
- Add `void gotoAddress(uint16_t address);`
- Add `bool followSP_ = true;` (real toggle)
- Add `uint16_t viewCenter_ = 0;` (center when not following SP)
- Add `bool pendingScroll_ = false;`
- Add `uint16_t address() const` getter
- Add `bool followSP() const` / `void setFollowSP(bool)`
- Add `std::function<void(uint16_t)> onGoToDisassembly;`

**Implementation** (`stack_view_window.cpp`):
- `gotoAddress()`: set `viewCenter_`, `followSP_ = false`, `pendingScroll_ = true`, `needsRefresh_ = true`, `visible_ = true`
- `refreshSnapshot()`: if `followSP_`, center on `cpu.sp`; else center on `viewCenter_`
- Replace "Follow SP" button with `ImGui::Checkbox("Follow SP", &followSP_)`
- In `renderStackView()`: if `pendingScroll_`, scroll to target line, clear flag
- Add "Go to Disassembly" to context menu (for address row and word value)

## 5. BreakpointsWindow changes

**Header** (`breakpoints_window.h`):
- Add `std::function<void(uint16_t)> onGoToDisassembly;`
- Add `std::function<void(uint16_t)> onGoToMemoryInspector;`

**Implementation** (`breakpoints_window.cpp`):
- Add right-click context menu on each BP row: "Go to Disassembly", "Go to Memory Inspector"

## 6. CPU Registers context menus in `gui.cpp`

In `renderCpuPanel()`, add `ImGui::BeginPopupContextItem()` on PC and SP rows:

- **PC**: "Go to Disassembly" → `gotoDisassembly(s.pc)`, "Go to Memory Inspector" → `gotoMemory(s.pc)`
- **SP**: "Go to Stack" → `gotoStack(s.sp)`, "Go to Memory Inspector" → `gotoMemory(s.sp)`

## 7. gui.cpp callback wiring

Replace all per-window callbacks with central API:
```cpp
stackView_.onGoToMemoryInspector = [this](uint16_t a) { gotoMemory(a); };
stackView_.onGoToDisassembly   = [this](uint16_t a) { gotoDisassembly(a); };
memoryInspector_.onGoToDisassembly = [this](uint16_t a) { gotoDisassembly(a); };
disassemblyView_.onGoToMemoryInspector = [this](uint16_t a) { gotoMemory(a); };
breakpointsWindow_.onGoToDisassembly = [this](uint16_t a) { gotoDisassembly(a); };
breakpointsWindow_.onGoToMemoryInspector = [this](uint16_t a) { gotoMemory(a); };
```

## 8. Tests — 10 new in `test_backend.cpp`

| # | Test | Checks |
|---|------|--------|
| 1 | `test_nav_memory` | `gotoAddress(0x1234)` → `address() == 0x1234` |
| 2 | `test_nav_disassembly` | `gotoAddress(0x2345)` → `address() == 0x2345` |
| 3 | `test_nav_stack` | `gotoAddress(0x7F00)` → `address() == 0x7F00` |
| 4 | `test_nav_stack_word` | LE word at address computed correctly |
| 5 | `test_nav_breakpoint` | BP address matches navigation target |
| 6 | `test_nav_follow_pc_on` | Follow PC on: address tracks PC changes |
| 7 | `test_nav_follow_pc_off` | Follow PC off: address stays after PC change |
| 8 | `test_nav_follow_sp_on` | Follow SP on: address tracks SP changes |
| 9 | `test_nav_follow_sp_off` | Follow SP off: address stays after SP change |
| 10 | `test_nav_boundaries` | gotoAddress(0x0000) and gotoAddress(0xFFFF) on all windows |

## 9. Board smoke test

New section in `test_board_smoke.cpp`:
- PC → Disassembly/Memory navigation
- SP → Stack navigation
- BP → Disassembly/Memory
- Stack Word → Disassembly
- Follow PC ON/OFF
- Follow SP ON/OFF

## 10. Files changed

| File | Change |
|------|--------|
| `debugger/gui/gui.h` | +3 goto methods |
| `debugger/gui/gui.cpp` | +goto impls, replace callbacks, register context menus |
| `debugger/gui/memory_inspector_window.h` | +gotoAddress, +pendingScroll_, +address(), +onGoToDisassembly |
| `debugger/gui/memory_inspector_window.cpp` | +scroll logic, +context menu item |
| `debugger/gui/disassembly_window.h` | +gotoAddress, +pendingScroll_, +address() |
| `debugger/gui/disassembly_window.cpp` | +gotoAddress, +scroll for goto |
| `debugger/gui/stack_view_window.h` | +gotoAddress, +followSP_, +viewCenter_, +pendingScroll_, +onGoToDisassembly |
| `debugger/gui/stack_view_window.cpp` | +Follow SP checkbox, +gotoAddress, +scroll, +context menus |
| `debugger/gui/breakpoints_window.h` | +2 callbacks |
| `debugger/gui/breakpoints_window.cpp` | +context menu per BP |
| `debugger/tests/test_backend.cpp` | +10 navigation tests |
| `debugger/tests/test_board_smoke.cpp` | +navigation smoke section |

## 11. Constraints

- `src/` not modified
- No `Memory::buffer()`
- No new banking/breakpoint/disassembler logic
- Windows don't reference each other — callbacks only
- Manual navigation never auto-toggles Follow
- All addresses `uint16_t` virtual
- Auto-open closed target windows
