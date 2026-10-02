# Stage 6.20.1 — ROM Load Lifecycle: Breakpoint Reset

## Сброс debugger state при загрузке нового ROM

### Цель

Исправить lifecycle загрузки ROM в debugger.

При успешной загрузке нового ROM breakpoint'ы, относящиеся к предыдущему ROM, не должны сохраняться.

Сейчас `debug_load_rom()` загружает новый ROM, но существующий breakpoint state `DebugBackend` не очищается. В результате breakpoint'ы от предыдущего ROM могут остаться в debugger state и впоследствии синхронизироваться с новым `Board`.

Целевое поведение:

```text
ROM A
  ↓
breakpoints A
  ↓
debug_load_rom(ROM B)
  ↓
successful load
  ↓
debugger state reset
  ↓
ROM B
  ↓
no breakpoints
```

---

# 1. Основное правило

При **успешной загрузке нового ROM** необходимо удалить все breakpoint'ы текущего debugger session.

После:

```text
debug_load_rom(ROM B)
```

результат:

```text
breakpoints = []
```

Breakpoint'ы ROM A не должны переноситься на ROM B.

---

# 2. Ошибка загрузки ROM

Если загрузка ROM завершилась ошибкой:

```text
file not found
invalid ROM
I/O error
invalid argument
```

существующее состояние debugger **не изменять**.

Например:

```text
ROM A
breakpoints = {1234, 5678}

debug_load_rom(invalid.rom)
        ↓
error

ROM A
breakpoints = {1234, 5678}
```

То есть очистка breakpoint'ов производится только после подтверждённого успешного `loadRom()`.

---

# 3. DebugBackend

Основное изменение выполнить в:

```text
debugger/src/backend.h
debugger/src/backend.cpp
```

или в соответствующем текущем DebugBackend/AgentApi lifecycle-коде.

После успешной загрузки нового ROM очистить:

```cpp
breakpoints_
```

или соответствующее текущее хранилище breakpoint'ов.

Не создавать новый контейнер breakpoint'ов без необходимости.

Использовать существующую модель хранения.

---

# 4. DebugAdapter

Необходимо синхронизировать состояние adapter с новым ROM.

Существующий механизм:

```cpp
syncBreakpoints()
```

или его фактический текущий аналог должен после загрузки нового ROM гарантировать:

```text
DebugBackend breakpoints = empty
DebugAdapter syncedBreakpoints = empty
Board breakpoints = empty
```

Особое внимание уделить:

```cpp
syncedBreakpoints_
```

Он также не должен содержать адреса от предыдущего ROM.

Если текущая архитектура позволяет гарантировать это через существующий `syncBreakpoints()`, не добавлять второй механизм.

---

# 5. Порядок операций

Правильный порядок:

```text
1. Validate/load ROM
2. Confirm successful ROM load
3. Reset debugger state belonging to previous ROM
4. Clear breakpoints
5. Clear adapter's synchronized breakpoint state
6. Reset/invalidate other ROM-dependent debugger state
7. Return success
```

При этом нельзя очищать breakpoint'ы **до** успешной загрузки ROM.

---

# 6. Не затрагивать Board напрямую из GUI

GUI по-прежнему не должен выполнять:

```cpp
board->clearBreakpoints()
```

или аналогичные операции.

Lifecycle остаётся:

```text
GUI
 ↓
IDebugBackend
 ↓
DebugBackend
 ↓
IDebugTarget
 ↓
DebugAdapter
 ↓
Board
```

MCP:

```text
MCP
 ↓
AgentApi
 ↓
DebugBackend
 ↓
IDebugTarget
 ↓
DebugAdapter
 ↓
Board
```

---

# 7. MCP API

Не добавлять новый MCP tool.

Существующий:

```text
debug_load_rom
```

должен автоматически обеспечивать новое поведение.

Контракт:

### До загрузки

```text
ROM A loaded
breakpoints = [0x0100, 0x0200]
```

### После успешной загрузки ROM B

```text
ROM B loaded
breakpoints = []
```

### После ошибки загрузки ROM B

```text
ROM A still loaded
breakpoints = [0x0100, 0x0200]
```

---

# 8. Agent API

Не добавлять новый метод.

Изменить только внутреннее поведение существующего:

```cpp
loadRom(...)
```

Контракт Agent API должен остаться прежним.

---

# 9. Другие ROM-dependent состояния

При реализации проверить существующий lifecycle `loadRom()`.

Если уже существуют механизмы очистки/инвалидации:

```text
instruction history
execution trace
I/O trace
memory access map
memory access log
snapshots
symbols
functions
RDB
call graph
analysis state
```

не дублировать их очистку.

Stage 6.20.1 отвечает прежде всего за breakpoint lifecycle.

Если Stage 6.20 уже реализован к моменту выполнения этого этапа, breakpoint reset должен быть согласован с его общим правилом:

```text
new ROM = new runtime/debug session
```

---

# 10. RDB

Не удалять:

```text
ROM.rdb
```

и не очищать RDB при загрузке ROM.

RDB является persistent database конкретного ROM.

При загрузке нового ROM:

```text
ROM A → ROM B
```

должен использоваться RDB, соответствующий ROM B.

Breakpoint state и RDB — разные сущности.

---

# 11. MAP / Symbols

Не изменять существующую логику:

```text
ROM
 ↓
RDB
 ↓
MAP supplement
```

Очистка breakpoint'ов не должна удалять:

```text
RDB objects
symbols
functions
MAP data
```

---

# 12. Tests

Добавить regression tests.

## Test 1 — successful ROM load clears breakpoints

Сценарий:

```text
load ROM A
add breakpoint 0x0100
add breakpoint 0x0200

load ROM B

get breakpoints
```

Ожидается:

```text
[]
```

---

## Test 2 — failed ROM load preserves breakpoints

Сценарий:

```text
load ROM A
add breakpoint 0x0100

load invalid/nonexistent ROM

get breakpoints
```

Ожидается:

```text
[0x0100]
```

---

## Test 3 — Board receives no stale breakpoints

Проверить не только API state, но и фактическое состояние target/Board.

После:

```text
load ROM A
breakpoint 0x0100

load ROM B
```

на Board не должно остаться:

```text
0x0100
```

---

## Test 4 — new breakpoint works after ROM load

Проверить:

```text
load ROM A
breakpoint 0x0100

load ROM B

breakpoint 0x0200
run
```

Breakpoint `0x0200` должен нормально работать.

Это важно для проверки, что очистка старого state не ломает последующую синхронизацию.

---

## Test 5 — multiple breakpoints

Проверить:

```text
load ROM A
add 10 breakpoints

load ROM B
```

Результат:

```text
0 breakpoints
```

---

## Test 6 — repeated ROM loads

Проверить:

```text
load ROM A
add breakpoint

load ROM B
add breakpoint

load ROM A
```

После каждой успешной загрузки breakpoint list должен начинаться пустым.

---

# 13. MCP wire test

Добавить/обновить MCP integration test:

```text
debug_load_rom
debug_add_breakpoint
debug_get_breakpoints
debug_load_rom
debug_get_breakpoints
```

Ожидается:

```json
{
  "breakpoints": []
}
```

После второй успешной загрузки.

Отдельно проверить ошибочный `debug_load_rom`.

---

# 14. Не менять публичный контракт

Не менять:

```text
debug_load_rom
AgentApi::loadRom()
IDebugBackend
IDebugTarget
```

только ради добавления отдельного reset API.

Не создавать:

```text
debug_clear_breakpoints_on_load
debug_reset_debugger_state
```

или аналогичные MCP-команды.

Это внутреннее lifecycle-поведение загрузки ROM.

---

# 15. Потоки

Сохранить существующую thread-safety модель.

GUI/MCP не должны напрямую очищать breakpoint'ы на Board.

Если `loadRom()` выполняется через command queue/emulation thread, очистка target/Board breakpoint state должна происходить в том же безопасном контексте.

Не вводить дополнительные потоки.

---

# 16. `src/` — запрещено изменять

Обязательно:

```bash
git diff -- src/
```

должен быть пустым.

Stage 6.20.1 выполняется только внутри:

```text
debugger/
```

---

# 17. Проверка существующей реализации

Перед изменением кода обязательно проследить фактическую цепочку:

```text
debug_load_rom
    ↓
MCP adapter
    ↓
AgentApi::loadRom
    ↓
DebugBackend::loadRom
    ↓
DebugAdapter::loadRom
    ↓
Board::reset(LOADROM)
```

и определить фактические точки хранения:

```text
breakpoints_
syncedBreakpoints_
Board breakpoint state
```

Не делать предположений о названиях методов/полей.

Использовать существующие механизмы синхронизации там, где это возможно.

---

# 18. Regression

После изменений обязательно выполнить:

```text
backend tests
Agent API tests
MCP protocol tests
RDB tests
MAP tests
GUI smoke tests
```

А также существующий полный набор regression tests проекта.

Проверить:

```text
Run
Pause
Step
Breakpoint
Run → Breakpoint
Step after breakpoint
Reset
ROM loading
```

---

# 19. Проверка через реальный MCP

Не ограничиваться unit tests.

Проверить реальную цепочку:

```text
MCP
 ↓
AgentApi
 ↓
DebugBackend
 ↓
DebugAdapter
 ↓
Board
```

Минимальный сценарий:

```text
debug_load_rom(ROM A)
debug_add_breakpoint(ADDRESS)
debug_get_breakpoints()

debug_load_rom(ROM B)
debug_get_breakpoints()
```

Ожидается:

```text
после ROM A:
ADDRESS

после ROM B:
пусто
```

---

# 20. Критерий завершения

Stage 6.20.1 считается завершённым, если:

* успешная загрузка нового ROM очищает breakpoint list;
* breakpoint state предыдущего ROM не переносится;
* `DebugAdapter` не сохраняет stale synchronized breakpoints;
* Board не содержит stale breakpoints;
* неуспешная загрузка ROM не изменяет существующие breakpoint'ы;
* после загрузки нового ROM можно нормально добавить новые breakpoint'ы;
* `debug_load_rom` не получил новый MCP API;
* Agent API не получил новый публичный метод;
* GUI не получает доступ к Board;
* поточная модель не изменена;
* RDB/MAP/symbols не удаляются;
* `git diff -- src/` пустой;
* unit/integration/MCP regression tests проходят;
* реальная MCP-цепочка подтверждает очистку breakpoint'ов.

После выполнения **не добавлять новую функциональность**.

Это точечный lifecycle fix для `debug_load_rom`.
