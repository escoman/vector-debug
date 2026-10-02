## Stage 3.6 — CPU Registers Inspector & Editing

### 1. Minimal src/ additions

Add one new accessor to expose `ei_pending`:

**`src/i8080.h`** — add declaration:
```cpp
int i8080_ei_pending(void);
```

**`src/i8080.cpp`** — add implementation:
```cpp
int i8080_ei_pending(void) { return EI_PENDING; }
```

`last_pc` will be tracked in DebugBackend (no src/ change needed).
`cpu_cycles` already accessible via `i8080_cycles()`.

### 2. CpuState expansion (`debugger/src/backend.h`)

Add fields:
```cpp
struct CpuState {
    // ... existing fields ...
    uint32_t cycles;       // from i8080_cycles()
    bool ei_pending;       // from i8080_ei_pending()
    uint16_t last_pc;      // tracked by backend
};
```

### 3. RegisterId enum + writeRegister API (`debugger/src/backend.h`)

```cpp
enum class RegisterId { AF, BC, DE, HL, SP, PC };

bool writeRegister(RegisterId id, uint16_t value);
```

Pattern identical to `writeMemory()`: posts `PendingCommand::RegisterWrite`, waits on `resultCv_`.

### 4. Backend implementation (`debugger/src/backend.cpp`)

**New PendingCommand**: `RegisterWrite`

**New state**:
```cpp
RegisterId writeRegId_;
uint16_t   writeRegValue_;
bool       writeRegResult_;
```

**`writeRegister()`**: Post command + wait (same pattern as `writeMemory()`).

**`processRegisterWrite()`**:
1. Check `state_ == Paused` under `stateMutex_` — reject if not.
2. Execute register change:
   - `AF`: `i8080_setreg_a(value >> 8)`, `i8080_setreg_f(value & 0xFF)`
   - `BC`: `i8080_setreg_b(value >> 8)`, `i8080_setreg_c(value & 0xFF)`
   - `DE`: `i8080_setreg_d(value >> 8)`, `i8080_setreg_e(value & 0xFF)`
   - `HL`: `i8080_setreg_h(value >> 8)`, `i8080_setreg_l(value & 0xFF)`
   - `SP`: `i8080_setreg_sp(value)`
   - `PC`: `i8080_jump(value)`
3. Set `writeRegResult_ = true`.

**`getCpuState()`**: Expand with `cycles`, `ei_pending`, `last_pc`.

**`last_pc` tracking**: Save `pc` before each `stepInstruction()` into `lastPc_` member.

**`processOneCommand()`**: Add `RegisterWrite` case (unlock mutex, process, signal).

**`poll_debugger`**: Reject `RegisterWrite` while Running (same as MemoryWrite).

### 5. GUI — CPU Panel rewrite (`debugger/gui/gui.h`, `gui.cpp`)

**`gui.h`**: Add CPU panel editing state:
```cpp
// CPU register editing (Stage 3.6)
bool editingRegister_ = false;
RegisterId editingRegId_;
char editRegBuffer_[8] = "";
bool writeRegFailed_ = false;
```

Change `renderCpuPanel` signature to take `DebugBackend&` (currently takes `const CpuState&`).

**`gui.cpp` — `renderCpuPanel(DebugBackend &backend)`**:

Layout:
```
CPU
PC   1234      [editable]
AF   1234      [editable]  A=12 F=34
BC   5678      [editable]  B=56 C=78
DE   9ABC      [editable]  D=9A E=BC
HL   DEF0      [editable]  H=DE L=F0
SP   8000      [editable]
─────────────
S Z AC P CY   (individual flag bits)
1 0  1  1  0
─────────────
IFF  1
EI pending: 0
Cycles: 12345
Last PC: 1230
```

**Edit UX** (same as Memory Inspector):
- Double-click on register value -> `editingRegister_ = true`, store RegisterId + pre-fill hex buffer
- Edit input appears below register list (like Memory Inspector toolbar edit)
- Enter -> `backend.writeRegister(id, value)` -> refresh
- Esc -> cancel
- Error message if write failed

**Flags display**: Extract from F register byte:
```cpp
uint8_t f = cpu.flags;
bool S  = (f >> 7) & 1;
bool Z  = (f >> 6) & 1;
bool AC = (f >> 4) & 1;
bool P  = (f >> 2) & 1;
bool CY = f & 1;
```

### 6. Integration — refresh after register write

After successful register write:
- Cancel edit mode
- CPU panel auto-updates next frame (reads fresh `getCpuState()`)
- If PC changed: "Current Instruction" panel updates automatically (uses `cpu.pc`)
- If SP changed: Stack View refreshes on next render cycle

### 7. Tests (`debugger/tests/test_backend.cpp`)

Add 7 tests:

1. **test_cpu_regs_read** — verify all registers readable after reset
2. **test_cpu_regs_write** — write each register (AF/BC/DE/HL/SP/PC), verify via `getCpuState()`
3. **test_cpu_regs_8_16_consistency** — write BC=0x1234, verify B=0x12, C=0x34
4. **test_cpu_regs_pc_execution** — write PC, put instruction at new PC, step, verify executed
5. **test_cpu_regs_sp** — write SP, verify new value
6. **test_cpu_regs_running_rejection** — verify writeRegister returns false when Running (test state check logic directly)
7. **test_cpu_regs_reset** — modify registers, reset, verify post-reset state

For tests without emulation thread: use `processRegisterWrite()` directly (set write state under mutex, call method).

### 8. Real Board smoke test (`debugger/tests/test_board_smoke.cpp`)

Add section:
- Write PC, SP, BC, DE, HL via `Memory::write` + `i8080_setreg_*()` (simulating what emulation thread does)
- Read back via `getCpuState()`
- Verify PC -> Step executes from new address

### 9. Files changed

| File | Change |
|------|--------|
| `src/i8080.h` | +`i8080_ei_pending()` declaration |
| `src/i8080.cpp` | +`i8080_ei_pending()` implementation |
| `debugger/src/backend.h` | +RegisterId enum, `writeRegister()`, expanded CpuState, RegisterWrite command state |
| `debugger/src/backend.cpp` | +writeRegister, processRegisterWrite, processOneCommand RegisterWrite case, lastPc_ tracking |
| `debugger/gui/gui.h` | +register editing state, modified renderCpuPanel signature |
| `debugger/gui/gui.cpp` | Rewritten renderCpuPanel with full display + editing |
| `debugger/tests/test_backend.cpp` | +7 register tests |
| `debugger/tests/test_board_smoke.cpp` | +register write/read section |
