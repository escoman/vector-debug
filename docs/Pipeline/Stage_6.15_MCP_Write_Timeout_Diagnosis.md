# Stage 6.15 — Диагностика Timeout RDB Write Operations через MCP

## Статус: ЗАВЕРШЕНО

## Root Cause

**`mcp_main.cpp` не запускал emulation thread (`runUntilPause()`).**

Без emulation thread command queue никогда не обрабатывается. Все операции, идущие через `submitAndWait()` (annotations, breakpoints, run/step), ожидали результат в `future.wait_for(5s)` и получали timeout, т.к. никто не вызывал `commandQueue_.waitAndDequeue()` + `processCommand()`.

## Affected Path

```
MCP → AgentApi::setComment/createFunction/addLabel
    → backend_.requestSetComment/requestCreateFunction/requestAddLabel
    → submitAndWait()
    → commandQueue_.enqueue() + future.wait_for(5s)
    → TIMEOUT (emulation thread не запущен, queue не обрабатывается)
```

## Why Reads Work

RDB read operations (`getRdbInfo`, `listRdbObjects`, `getRdbObject`, `findRdbObject`)
и RDB write operations (`addRdbObject`, `setRdbComment`, `saveRdb`, `addRdbLink`)
обращаются к `backend_.rdbController()` напрямую — без command queue.

Annotation operations (`debug_set_comment`, `debug_create_function`, `debug_add_label`,
`debug_set_function_comment`) идут через `backend_.request*()` → command queue → emulation thread.

## Why Writes Timeout

Annotation write operations используют `submitAndWait()`:
```cpp
commandQueue_.enqueue(std::move(cmd));
auto status = future.wait_for(std::chrono::seconds(5));
// → timeout, т.к. emulation thread не запущен
```

`testSynchronous_ = false` в production, поэтому synchronous fallback не работает.

## Why saveRdb Appeared Successful

`saveRdb()` → `backend_.rdbController().save()` — прямая операция.
Если Agent использовал annotation tools вместо RDB tools, RDB оставался пустым.
`save()` на не-dirty RDB возвращает `true` (нечего сохранять) → `success=true`.

## Fix

**Файл**: `debugger/mcp/mcp_main.cpp`

Добавлен запуск emulation thread перед `mcp.runStdio()`:
```cpp
std::thread emulationThread([&backend]() {
    backend.runUntilPause();
});
```

И корректное завершение:
```cpp
backend.requestQuit();
emulationThread.join();
```

## Regression Test

**Файл**: `debugger/agent/tests/test_agent_integration.cpp`

Тест `test_mcp_command_queue_and_rdb_workflow` (#47) проверяет:
1. Annotation operations через command queue с emulation thread
2. RDB operations напрямую
3. Полный workflow: addRdbObject → dirty → save → file exists → reload → data preserved

## Acceptance Criteria

Все операции работают без timeout:
- read operations → OK
- write operations (annotations) → OK
- RDB operations → OK
- save → OK
- reload → OK

## Regression Results

```
test_agent_api:          105/105 ✓
test_agent_commands:      15/15  ✓
test_agent_contract:      49/49  ✓
test_agent_integration:   47/47  ✓
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

## Таблица операций

| Operation          | MCP              | Agent API          | Queue     | Emulation thread | Result  |
|--------------------|------------------|--------------------|-----------|------------------|---------|
| getRdbInfo         | direct           | rdbController()    | no        | no               | OK      |
| listRdbObjects     | direct           | rdbController()    | no        | no               | OK      |
| addRdbObject       | direct           | rdbController()    | no        | no               | OK      |
| setRdbComment      | direct           | rdbController()    | no        | no               | OK      |
| addRdbLink         | direct           | rdbController()    | no        | no               | OK      |
| saveRdb            | direct           | rdbController()    | no        | no               | OK      |
| setComment         | submitAndWait    | requestSetComment  | yes       | yes              | OK*     |
| createFunction     | submitAndWait    | requestCreateFunc  | yes       | yes              | OK*     |
| addLabel           | submitAndWait    | requestAddLabel    | yes       | yes              | OK*     |

*OK после фикса (запуск emulation thread)
