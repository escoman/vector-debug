# Stage 6.15 (Iteration 2) — Диагностика debug_create_function Timeout

## Статус: ЗАВЕРШЕНО

## Root Cause

**`debug_create_function` НЕ timeout.** Команда успешно выполняется через command queue.

Проблема: `createFunction` создавал символ в **SymbolDatabase**, но **НЕ создавал RDB object**.

Agent использовал `debug_create_function` и затем проверял `debug_get_rdb_info`:
- `object_count = 0` → Agent интерпретировал как "операция не удалась"
- Фактически символ был создан в SymbolDatabase (виден через `debug_get_symbols`)

## Affected Path

```
MCP → debug_create_function
    → AgentApi::createFunction()
    → DebugBackend::requestCreateFunction()
    → submitAndWait() → commandQueue → emulation thread
    → executeCommand(): symbols_.addSymbol() → SymbolDatabase ✓
    → RDB: ничего не создано ✗
    → Agent проверяет get_rdb_info → object_count=0 → "timeout" (misdiagnosis)
```

## Why addRdbObject Works

`debug_add_rdb_object` → `AgentApi::addRdbObject()` → `rdbController().addObject()` — напрямую в RDB.
Agent проверяет `get_rdb_info` → `object_count > 0` → OK.

## Why createFunction "Timed Out"

`debug_create_function` → command queue → `symbols_.addSymbol()` — только SymbolDatabase.
RDB не изменяется → `dirty=false`, `object_count=0`.
Agent ожидает RDB object, не находит его, интерпретирует как failure.

## Fix

**Файл**: `debugger/src/backend.cpp`, `executeCommand()`

`CreateFunction` и `AddLabel` теперь также создают RDB objects:

```cpp
case CommandType::CreateFunction: {
    bool ok = symbols_.addSymbol(cmd.address, cmd.name, SymbolType::Function);
    if (ok) {
        RdbObject rdbObj;
        rdbObj.address = cmd.address;
        rdbObj.type    = RdbObjectType::Function;
        rdbObj.name    = cmd.name;
        rdbObj.hasSize = false;
        rdb_->addObject(rdbObj);
    }
    break;
}
```

Аналогично для `AddLabel` с `RdbObjectType::Label`.

## Verification

### Real MCP Wire Test

```
debug_add_rdb_object(0x0100, "test_func") → success
debug_create_function(0x0200)             → success
debug_get_rdb_info                        → object_count: 2, dirty: true
debug_list_rdb_objects                    → 2 objects (test_func + sub_0200)
debug_get_symbols                         → 1 symbol (sub_0200)
```

### Regression Test

`test_create_function_creates_rdb_object` (#48) в `test_agent_integration.cpp`:
1. createFunction → RDB has 1 object (type=function)
2. addLabel → RDB has 2 objects
3. save → dirty=false, file exists
4. reload → both objects preserved

### All Tests

```
test_agent_api:          105/105 ✓
test_agent_commands:      15/15  ✓
test_agent_contract:      49/49  ✓
test_agent_integration:   48/48  ✓
test_backend:            120/120 ✓
test_board_smoke:          2/2   ✓
test_call_graph_model:    22/22  ✓
test_config_manager:       8/8   ✓
test_gui_smoke:            3/3   ✓
test_live_map:            21/21  ✓
test_map_import:           8/8   ✓
test_map_integration:      6/6   ✓
test_map_loader:          20/20  ✓
test_mcp_protocol:        44/44  ✓
test_rdb_controller:      39/39  ✓
test_reset_cpu_state:     14/14  ✓
test_rom_load_address:    28/28  ✓
test_rom_loading:          9/9   ✓
test_symbol_database:     22/22  ✓
test_vram_mapping:        17/17  ✓
test_workspace:           15/15  ✓
```

## Таблица операций (после фикса)

| Этап              | addRdbObject           | createFunction              |
| ----------------- | ---------------------- | --------------------------- |
| MCP handler       | api_.addRdbObject()    | api_.createFunction()       |
| Agent API         | rdbController().add()  | requestCreateFunction()     |
| Backend request   | DIRECT                 | submitAndWait()             |
| command queue     | no                     | yes                         |
| emulation thread  | no                     | yes                         |
| command execution | n/a                    | executeCommand()            |
| SymbolDatabase    | no                     | addSymbol() ✓               |
| RDB operation     | rdb.addObject() ✓      | rdb.addObject() ✓ (FIXED)   |
| completion        | direct return          | promise.set_value()         |
| response          | success                | success                     |

## Files Modified

- `debugger/src/backend.cpp` — CreateFunction + AddLabel also create RDB objects
- `debugger/agent/tests/test_agent_integration.cpp` — regression test #48
- `src/` NOT modified
