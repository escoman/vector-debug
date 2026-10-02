# Stage 6.3 — Agent API Contract & Consistency

## 1. Цель

Привести публичный Agent API к единому предсказуемому контракту. Не добавлять новую функциональность. Не менять `src/`.

---

## 2. Полная таблица публичного Agent API

### 2.1. Execution Control

| # | Метод | Текущая сигнатура | Новая сигнатура | Обоснование | ErrorCode | GUI | Тесты |
|---|---|---|---|---|---|---|---|
| 1 | `run()` | `void` | `AgentApiResult<void>` | Публичный API, может сообщить об ошибке (нет ROM) | `NoRomLoaded`, `OperationFailed` | — | ✓ (test_agent_api) |
| 2 | `pause()` | `void` | `AgentApiResult<void>` | Может сообщить: уже остановлен, нет backend | `NotRunning`, `OperationFailed` | — | ✓ |
| 3 | `step()` | `void` | `AgentApiResult<void>` | Может сообщить: не paused, нет ROM | `NotPaused`, `NoRomLoaded`, `OperationFailed` | — | ✓ |
| 4 | `reset()` | `void` | `AgentApiResult<void>` | Может сообщить: нет ROM | `NoRomLoaded`, `OperationFailed` | — | ✓ |
| 5 | `isRunning()` | `bool` | `bool` (без изменений) | Read-only snapshot, не может «ошибиться» | — | — | ✓ |

**Влияние на внутренние интерфейсы**: `run/pause/step/reset` вызывают `backend_.requestRun()` и т.д. — это void-методы IDebugBackend. AgentApi будет делать предварительную валидацию (isPaused, getCpuState) и пост-проверку. CommandResult не участвует — эти команды идут через void-request, не через command queue с результатом.

### 2.2. CPU State

| # | Метод | Текущая сигнатура | Новая сигнатура | Обоснование | ErrorCode | GUI | Тесты |
|---|---|---|---|---|---|---|---|
| 6 | `getCpuState()` | `CpuState` | `AgentApiResult<CpuState>` | Публичный API, единообразие | `NoRomLoaded` | — | ✓ |

### 2.3. Memory Access

| # | Метод | Текущая сигнатура | Новая сигнатура | Обоснование | ErrorCode | GUI | Тесты |
|---|---|---|---|---|---|---|---|
| 7 | `readMemory()` | `std::vector<uint8_t>` | `AgentApiResult<std::vector<uint8_t>>` | Нет ошибки при address+size overflow | `InvalidRange`, `NoRomLoaded` | — | ✓ |
| 8 | `writeMemory()` | `bool` | `AgentApiResult<void>` | Единообразие + error message | `InvalidRange`, `NoRomLoaded`, `OperationFailed` | — | ✓ |

### 2.4. I/O Ports

| # | Метод | Текущая сигнатура | Новая сигнатура | Обоснование | ErrorCode | GUI | Тесты |
|---|---|---|---|---|---|---|---|
| 9 | `readIo()` | `AgentApiResult<uint8_t>` | без изменений | Уже в контракте | — | — | ✓ |
| 10 | `writeIo()` | `CommandResult` | `AgentApiResult<void>` | Публичный API не должен возвращать CommandResult | `OperationFailed` | — | ✓ |

### 2.5. Breakpoints

| # | Метод | Текущая сигнатура | Новая сигнатура | Обоснование | ErrorCode | GUI | Тесты |
|---|---|---|---|---|---|---|---|
| 11 | `setBreakpoint()` | `CommandResult` | `AgentApiResult<void>` | Публичный API | `InvalidAddress`, `OperationFailed` | — | ✓ |
| 12 | `clearBreakpoint()` | `CommandResult` | `AgentApiResult<void>` | Публичный API | `NotFound`, `OperationFailed` | — | ✓ |
| 13 | `setBreakpointEnabled()` | `CommandResult` | `AgentApiResult<void>` | Публичный API | `NotFound`, `OperationFailed` | — | нет — добавить |
| 14 | `listBreakpoints()` | `std::vector<...>` | `AgentApiResult<std::vector<...>>` | Единообразие | — | — | ✓ |
| 15 | `clearAllBreakpoints()` | `AgentApiResult<void>` | без изменений | Уже в контракте | — | — | ✓ |

### 2.6. Registers

| # | Метод | Текущая сигнатура | Новая сигнатура | Обоснование | ErrorCode | GUI | Тесты |
|---|---|---|---|---|---|---|---|
| 16 | `setRegister()` | `AgentApiResult<void>` | без изменений | Уже в контракте | — | — | ✓ |

### 2.7. Disassembly

| # | Метод | Текущая сигнатура | Новая сигнатура | Обоснование | ErrorCode | GUI | Тесты |
|---|---|---|---|---|---|---|---|
| 17 | `disassemble()` | `AgentApiResult<vector<...>>` | без изменений | Уже в контракте | — | — | ✓ |

### 2.8. Instruction History

| # | Метод | Текущая сигнатура | Новая сигнатура | Обоснование | ErrorCode | GUI | Тесты |
|---|---|---|---|---|---|---|---|
| 18 | `getInstructionHistory()` | `AgentApiResult<vector<...>>` | без изменений | Уже в контракте | — | — | ✓ |

### 2.9. Trace / I/O History

| # | Метод | Текущая сигнатура | Новая сигнатура | Обоснование | ErrorCode | GUI | Тесты |
|---|---|---|---|---|---|---|---|
| 19 | `getExecutionTrace()` | `std::vector<InstructionEvent>` | `AgentApiResult<std::vector<InstructionEvent>>` | Единообразие, maxEntries=0 → error | `InvalidArgument` | — | ✓ |
| 20 | `getIoTrace()` | `std::vector<IoAccessEvent>` | `AgentApiResult<std::vector<IoAccessEvent>>` | Единообразие | `InvalidArgument` | — | ✓ |

### 2.10. Screen

| # | Метод | Текущая сигнатура | Новая сигнатура | Обоснование | ErrorCode | GUI | Тесты |
|---|---|---|---|---|---|---|---|
| 21 | `getScreen()` | `IDebugBackend::ScreenSnapshot` | `AgentApiResult<AgentScreenSnapshot>` | MCP readiness: убрать зависимость от IDebugBackend type | — | — | нет |

`AgentScreenSnapshot` — value type в `agent_types.h`:
```cpp
struct AgentScreenSnapshot {
    std::vector<uint32_t> pixels;  // ARGB8888
    int width = 0;
    int height = 0;
};
```

### 2.11. Annotations

| # | Метод | Текущая сигнатура | Новая сигнатура | Обоснование | ErrorCode | GUI | Тесты |
|---|---|---|---|---|---|---|---|
| 22 | `createFunction()` | `CommandResult` | `AgentApiResult<void>` | Публичный API | `InvalidAddress`, `OperationFailed` | — | ✓ |
| 23 | `renameFunction()` | `CommandResult` | `AgentApiResult<void>` | Публичный API | `NotFound`, `OperationFailed` | — | ✓ |
| 24 | `setFunctionComment()` | `CommandResult` | `AgentApiResult<void>` | Публичный API | `NotFound`, `OperationFailed` | — | ✓ |
| 25 | `deleteFunction()` | `CommandResult` | `AgentApiResult<void>` | Публичный API | `NotFound`, `OperationFailed` | — | ✓ |
| 26 | `addLabel()` | `CommandResult` | `AgentApiResult<void>` | Публичный API | `InvalidAddress`, `OperationFailed` | — | ✓ |
| 27 | `setComment()` | `CommandResult` | `AgentApiResult<void>` | Публичный API | `NotFound`, `OperationFailed` | — | нет (делегат) |
| 28 | `applyAnnotation()` | `CommandResult` | `AgentApiResult<void>` | Публичный API | `InvalidArgument`, `OperationFailed` | — | ✓ |

### 2.12. High-level Analysis

| # | Метод | Текущая сигнатура | Новая сигнатура | Обоснование | ErrorCode | GUI | Тесты |
|---|---|---|---|---|---|---|---|
| 29 | `getFunctionContext()` | `FunctionContext` | `AgentApiResult<FunctionContext>` | Единообразие | `NoRomLoaded` | — | ✓ |
| 30 | `traceFunction()` | `TraceResult` | `AgentApiResult<TraceResult>` | Единообразие, может сообщить timeout | `NoRomLoaded`, `Timeout`, `OperationFailed` | — | ✓ |

### 2.13. Stack

| # | Метод | Текущая сигнатура | Новая сигнатура | Обоснование | ErrorCode | GUI | Тесты |
|---|---|---|---|---|---|---|---|
| 31 | `getStack()` | `AgentApiResult<vector<...>>` | без изменений (исправить overflow) | Уже в контракте, но нужен overflow fix | `InvalidArgument` | — | ✓ |

### 2.14. Memory Map

| # | Метод | Текущая сигнатура | Новая сигнатура | Обоснование | ErrorCode | GUI | Тесты |
|---|---|---|---|---|---|---|---|
| 32 | `getMemoryMap()` | `AgentApiResult<vector<...>>` | без изменений | Уже в контракте | — | — | ✓ |

### 2.15. Screen/VRAM Info

| # | Метод | Текущая сигнатура | Новая сигнатура | Обоснование | ErrorCode | GUI | Тесты |
|---|---|---|---|---|---|---|---|
| 33 | `getScreenInfo()` | `AgentApiResult<ScreenInfoResult>` | без изменений | Уже в контракте | — | — | ✓ |
| 34 | `getVramInfo()` | `AgentApiResult<VramInfoResult>` | без изменений | Уже в контракте | — | — | ✓ |

### 2.16. Symbols

| # | Метод | Текущая сигнатура | Новая сигнатура | Обоснование | ErrorCode | GUI | Тесты |
|---|---|---|---|---|---|---|---|
| 35 | `getSymbols()` | `AgentApiResult<vector<...>>` | без изменений | Уже в контракте | — | — | ✓ |
| 36 | `getFunction()` | `AgentApiResult<SymbolInfo>` | без изменений (заменить InvalidAddress → NotFound) | ErrorCode некорректен | `NotFound` | — | ✓ |

### 2.17. Xrefs

| # | Метод | Текущая сигнатура | Новая сигнатура | Обоснование | ErrorCode | GUI | Тесты |
|---|---|---|---|---|---|---|---|
| 37 | `getXrefs()` | `AgentApiResult<vector<...>>` | без изменений | Уже в контракте | — | — | ✓ |

### 2.18. Call Graph

| # | Метод | Текущая сигнатура | Новая сигнатура | Обоснование | ErrorCode | GUI | Тесты |
|---|---|---|---|---|---|---|---|
| 38 | `getCallGraph()` | `AgentApiResult<vector<...>> getCallGraph(uint16_t address=0, size_t limit=0)` | `AgentApiResult<vector<...>> getCallGraph(std::optional<uint16_t> address, size_t limit=0)` | `$0000` — валидный адрес, нельзя использовать как «не задано» | — | — | ✓ |

### 2.19. ROM

| # | Метод | Текущая сигнатура | Новая сигнатура | Обоснование | ErrorCode | GUI | Тесты |
|---|---|---|---|---|---|---|---|
| 39 | `loadRom()` | `bool` | **Удалить** | Не используется нигде (ни GUI, ни тесты). `loadRomInfo()` покрывает всё. | — | — | нет |
| 40 | `loadRomInfo()` | `AgentApiResult<LoadRomResult>` | `AgentApiResult<LoadRomResult> loadRom(path, org)` (переименовать в `loadRom`) | Устранить дублирование; `loadRom` — единственный публичный метод загрузки | `InvalidArgument`, `OperationFailed` | — | ✓ (как loadRomInfo) |

### 2.20. Debug State

| # | Метод | Текущая сигнатура | Новая сигнатура | Обоснование | ErrorCode | GUI | Тесты |
|---|---|---|---|---|---|---|---|
| 41 | `getDebugState()` | `AgentApiResult<DebugStateResult>` | без изменений | Уже в контракте | — | — | ✓ |

### 2.21. Agent Log

| # | Метод | Текущая сигнатура | Новая сигнатура | Обоснование | ErrorCode | GUI | Тесты |
|---|---|---|---|---|---|---|---|
| 42 | `log()` | `const AgentLog&` | без изменений | Не операция, accessor | — | — | — |
| 43 | `clearLog()` | `void` | без изменений | Не операция, management | — | — | — |

---

## 3. Сводка изменений

### 3.1. Методы, требующие изменения сигнатуры (18 методов)

| # | Метод | Изменение |
|---|---|---|
| 1 | `run()` | `void` → `AgentApiResult<void>` |
| 2 | `pause()` | `void` → `AgentApiResult<void>` |
| 3 | `step()` | `void` → `AgentApiResult<void>` |
| 4 | `reset()` | `void` → `AgentApiResult<void>` |
| 5 | `getCpuState()` | `CpuState` → `AgentApiResult<CpuState>` |
| 6 | `readMemory()` | `vector<uint8_t>` → `AgentApiResult<vector<uint8_t>>` |
| 7 | `writeMemory()` | `bool` → `AgentApiResult<void>` |
| 8 | `writeIo()` | `CommandResult` → `AgentApiResult<void>` |
| 9 | `setBreakpoint()` | `CommandResult` → `AgentApiResult<void>` |
| 10 | `clearBreakpoint()` | `CommandResult` → `AgentApiResult<void>` |
| 11 | `setBreakpointEnabled()` | `CommandResult` → `AgentApiResult<void>` |
| 12 | `listBreakpoints()` | `vector<...>` → `AgentApiResult<vector<...>>` |
| 13 | `getExecutionTrace()` | `vector<...>` → `AgentApiResult<vector<...>>` |
| 14 | `getIoTrace()` | `vector<...>` → `AgentApiResult<vector<...>>` |
| 15 | `getScreen()` | `ScreenSnapshot` → `AgentApiResult<AgentScreenSnapshot>` |
| 16 | `createFunction()` | `CommandResult` → `AgentApiResult<void>` |
| 17 | `renameFunction()` | `CommandResult` → `AgentApiResult<void>` |
| 18 | `setFunctionComment()` | `CommandResult` → `AgentApiResult<void>` |
| 19 | `deleteFunction()` | `CommandResult` → `AgentApiResult<void>` |
| 20 | `addLabel()` | `CommandResult` → `AgentApiResult<void>` |
| 21 | `setComment()` | `CommandResult` → `AgentApiResult<void>` |
| 22 | `applyAnnotation()` | `CommandResult` → `AgentApiResult<void>` |
| 23 | `getFunctionContext()` | `FunctionContext` → `AgentApiResult<FunctionContext>` |
| 24 | `traceFunction()` | `TraceResult` → `AgentApiResult<TraceResult>` |
| 25 | `loadRom()` | **Удалить** (dead API) |
| 26 | `loadRomInfo()` | Переименовать в `loadRom()` |
| 27 | `getCallGraph()` | `uint16_t address=0` → `std::optional<uint16_t> address` |

### 3.2. Методы без изменений (16 методов)

`isRunning()`, `readIo()`, `clearAllBreakpoints()`, `setRegister()`, `disassemble()`, `getInstructionHistory()`, `getStack()`, `getMemoryMap()`, `getScreenInfo()`, `getVramInfo()`, `getSymbols()`, `getFunction()`, `getXrefs()`, `getDebugState()`, `log()`, `clearLog()`

### 3.3. Методы с косметическими изменениями (1 метод)

`getFunction()` — замена `ErrorCode::InvalidAddress` → `ErrorCode::NotFound`

---

## 4. ErrorCode Matrix

### 4.1. Новый ErrorCode

```cpp
NotFound   // после Unsupported — объект не найден по корректному ключу
```

Обоснование: `getFunction()` и `clearBreakpoint()` нуждаются в различении «неверный адрес» и «адрес верен, но объекта нет». Существующие коды не покрывают эту семантику.

Аналогичные случаи в API:
- `getFunction(addr)` — символ не найден по корректному адресу → `NotFound`
- `clearBreakpoint(addr)` — breakpoint не найден → `NotFound`
- `renameFunction(addr, name)` — символ не найден → `NotFound`
- `deleteFunction(addr)` — символ не найден → `NotFound`
- `setFunctionComment(addr, ...)` — символ не найден → `NotFound`
- `setComment(addr, ...)` — символ не найден → `NotFound`

### 4.2. Полная таблица ErrorCode → условия

| ErrorCode | Условия возникновения | Методы |
|---|---|---|
| `None` | Операция успешна | Все |
| `InvalidArgument` | Недопустимый аргумент (count=0, неизвестный регистр, некорректный тип annotation) | `disassemble`, `getInstructionHistory`, `getStack`, `setRegister`, `applyAnnotation`, `getExecutionTrace`, `getIoTrace` |
| `InvalidAddress` | Адрес за пределами 0x0000-0xFFFF (не применимо к uint16_t, но для address+size overflow) | `readMemory`, `writeMemory`, `setBreakpoint`, `addLabel` |
| `InvalidRange` | `address + size > 0xFFFF + 1` (wrap-around) | `readMemory`, `writeMemory` |
| `NoRomLoaded` | ROM не загружен, операция требует загруженный ROM | `run`, `step`, `reset`, `getCpuState`, `readMemory`, `writeMemory`, `getFunctionContext`, `traceFunction` |
| `NotPaused` | Эмуляция running, операция требует paused | `step` |
| `NotRunning` | Эмуляция paused, операция требует running | `pause` (опционально — может быть no-op) |
| `OperationFailed` | Операция не удалась по внутренней причине | `run`, `pause`, `step`, `reset`, `writeMemory`, `writeIo`, `setBreakpoint`, `clearBreakpoint`, `setBreakpointEnabled`, все annotations, `traceFunction` |
| `Timeout` | Превышен таймаут | `traceFunction` |
| `NotFound` | Объект не найден по корректному ключу | `getFunction`, `clearBreakpoint`, `renameFunction`, `deleteFunction`, `setFunctionComment`, `setComment` |
| `Unsupported` | Операция не поддерживается | Зарезервирован |

---

## 5. Семантика `0` для count/limit/length

| Метод | Параметр | Семантика `0` | Обоснование |
|---|---|---|---|
| `disassemble(count)` | count | **Error** (`InvalidArgument`) | «Дизассемблируй 0 инструкций» — логически бессмысленно |
| `getInstructionHistory(count)` | count | **Error** (`InvalidArgument`) | Аналогично |
| `getStack(limit)` | limit | **Error** (`InvalidArgument`) | «Покажи 0 элементов стека» — бессмысленно |
| `getExecutionTrace(maxEntries)` | maxEntries | **Error** (`InvalidArgument`) | «Покажи 0 записей» — бессмысленно |
| `getIoTrace(maxEntries)` | maxEntries | **Error** (`InvalidArgument`) | Аналогично |
| `getSymbols(limit)` | limit | **0 = all** (unlimited, max 10000) | «Покажи все символы» — логично |
| `getCallGraph(limit)` | limit | **0 = all** (unlimited, max 10000) | «Покажи весь граф» — логично |

**Модель**: два логически различных случая:
- `count` — «сколько запросить» → 0 = error (бессмысленно)
- `limit` — «максимальное количество» → 0 = unlimited (фильтр сверху)

---

## 6. Решение для `$0000` в `getCallGraph`

**Проблема**: текущий `getCallGraph(uint16_t address = 0)` использует `address == 0` как «весь граф». `$0000` — валидный адрес Vector-06C.

**Решение**:
```cpp
AgentApiResult<std::vector<CallGraphEdge>>
getCallGraph(std::optional<uint16_t> address = std::nullopt, size_t limit = 0);
```

- `std::nullopt` (или вызов без аргументов) → весь граф проекта
- `std::optional<uint16_t>(0x0000)` → граф конкретной функции at $0000
- `std::optional<uint16_t>(addr)` → граф конкретной функции at addr

**Обратная совместимость**: существующий вызов `api.getCallGraph()` продолжит работать (nullopt по умолчанию). Вызов `api.getCallGraph(0x0200)` потребует явного `std::optional<uint16_t>(0x0200)` или `api.getCallGraph(0x0200)` — implicit conversion from `uint16_t` to `std::optional<uint16_t>` работает.

---

## 7. Защита от address overflow

### 7.1. `getStack()` — текущий баг

```cpp
uint16_t addr = sp + static_cast<uint16_t>(i * 2);  // wrap-around!
```

**Исправление**:
```cpp
uint32_t fullAddr = static_cast<uint32_t>(sp) + static_cast<uint32_t>(i) * 2;
if (fullAddr > 0xFFFF) break;  // stop at memory boundary
uint16_t addr = static_cast<uint16_t>(fullAddr);
```

### 7.2. `readMemory()` — добавить проверку

```cpp
uint32_t endAddr = static_cast<uint32_t>(address) + size;
if (endAddr > 0x10000) {
    return AgentApiResult<...>::fail(ErrorCode::InvalidRange, "address + size exceeds 0xFFFF");
}
```

### 7.3. `disassemble()` — уже есть overflow guard

```cpp
uint16_t nextPc = pc + di.length;
if (nextPc <= pc) break;  // overflow guard — уже есть
```

Без изменений.

---

## 8. ROM Loading: решение

**Факт**: `loadRom()` (bool) не используется нигде — ни в GUI, ни в тестах. `loadRomInfo()` используется в 3 тестах.

**Решение**:
1. Удалить `bool loadRom(const std::string &path, uint32_t org = 0)` из публичного API
2. Переименовать `loadRomInfo()` → `loadRom()`:
   ```cpp
   AgentApiResult<LoadRomResult> loadRom(const std::string &path, uint32_t org = 0);
   ```
3. Внутренний `backend_.loadRom()` не изменяется.

**Влияние**: только 3 теста в `test_agent_api.cpp` нужно обновить (`loadRomInfo` → `loadRom`).

---

## 9. Влияние на существующие тесты

### 9.1. Тесты, требующие обновления

| Файл | Методы | Изменение |
|---|---|---|
| `test_agent_api.cpp` | `run/pause/step/reset` | Проверять `.success` вместо void |
| `test_agent_api.cpp` | `getCpuState()` | Использовать `.value.pc` вместо `.pc` |
| `test_agent_api.cpp` | `readMemory()` | Использовать `.value` вместо прямого vector |
| `test_agent_api.cpp` | `writeMemory()` | Проверять `.success` вместо `bool` |
| `test_agent_api.cpp` | `loadRomInfo()` | Переименовать в `loadRom()` |
| `test_agent_commands.cpp` | `setBreakpoint/clearBreakpoint` | Проверять `.success` вместо `CommandResult.success` |
| `test_agent_commands.cpp` | `createFunction/renameFunction/...` | Аналогично |
| `test_agent_commands.cpp` | `applyAnnotation` | Аналогично |
| `test_agent_commands.cpp` | `traceFunction` | Использовать `.value` вместо прямого TraceResult |
| `test_agent_integration.cpp` | `writeIo` | Проверять `.success` вместо `CommandResult.success` |
| `test_agent_api.cpp` | `getCallGraph(0x0200)` | → `getCallGraph(std::optional<uint16_t>(0x0200))` |

### 9.2. GUI

**Не затрагивается.** GUI не использует `AgentApi`.

---

## 10. Порядок выполнения

### Фаза 1: Подготовка типов
1. Добавить `ErrorCode::NotFound` в `agent_types.h`
2. Добавить `AgentScreenSnapshot` в `agent_types.h`
3. Определить safe max constants

### Фаза 2: Изменение сигнатур + реализация
Изменить `agent_api.h` и `agent_api.cpp`:
1. Execution: `run/pause/step/reset` → `AgentApiResult<void>` + валидация
2. CPU: `getCpuState()` → `AgentApiResult<CpuState>`
3. Memory: `readMemory/writeMemory` → `AgentApiResult` + overflow checks
4. I/O: `writeIo` → `AgentApiResult<void>`
5. Breakpoints: `setBreakpoint/clearBreakpoint/setBreakpointEnabled/listBreakpoints` → `AgentApiResult`
6. Annotations: все 7 методов → `AgentApiResult<void>`
7. Traces: `getExecutionTrace/getIoTrace` → `AgentApiResult`
8. High-level: `getFunctionContext/traceFunction` → `AgentApiResult`
9. Screen: `getScreen()` → `AgentApiResult<AgentScreenSnapshot>`
10. ROM: удалить `loadRom(bool)`, переименовать `loadRomInfo` → `loadRom`
11. Call Graph: `std::optional<uint16_t>` вместо `address=0`
12. `getFunction()`: `ErrorCode::InvalidAddress` → `ErrorCode::NotFound`
13. `getStack()`: overflow fix

### Фаза 3: Обновление существующих тестов
Обновить `test_agent_api.cpp`, `test_agent_commands.cpp`, `test_agent_integration.cpp` под новые сигнатуры.

### Фаза 4: Новые Contract Tests
Создать `test_agent_contract.cpp` — ~50 тестов публичного контракта.

### Фаза 5: CMakeLists.txt + Regression
Добавить `test_agent_contract`, прогнать полный test suite.

---

## 11. Изменённые файлы

| Файл | Характер изменений |
|---|---|
| `debugger/agent/agent_types.h` | Добавить `ErrorCode::NotFound`, `AgentScreenSnapshot`, max constants |
| `debugger/agent/agent_api.h` | Изменить ~27 сигнатур, удалить `loadRom(bool)` |
| `debugger/agent/agent_api.cpp` | Реализация новых сигнатур, overflow fixes, валидация |
| `debugger/agent/tests/test_agent_api.cpp` | Обновить под новые сигнатуры |
| `debugger/agent/tests/test_agent_commands.cpp` | Обновить под новые сигнатуры |
| `debugger/agent/tests/test_agent_integration.cpp` | Обновить под новые сигнатуры |
| `debugger/agent/tests/test_agent_contract.cpp` | **Новый** — contract tests |
| `debugger/CMakeLists.txt` | Добавить `test_agent_contract` |

**Не изменяются**: `src/`, `debugger/src/`, `debugger/gui/`, `debugger/tests/mock_backend.h`.

---

## 12. Критерий завершения

1. ✅ Все 43 публичных метода AgentApi соответствуют единому result contract
2. ✅ ErrorCode используется последовательно (включая `NotFound`)
3. ✅ `getCallGraph` использует `std::optional<uint16_t>` — `$0000` однозначен
4. ✅ Нет address wrap-around в `getStack`, `readMemory`
5. ✅ Семантика `0` определена для каждого count/limit
6. ✅ `loadRom` — единственный метод загрузки ROM
7. ✅ Agent API не зависит от GUI (подтверждено: GUI не использует AgentApi)
8. ✅ Public result types не содержат IDebugBackend/Board/SDL/ImGui types
9. ✅ Все существующие тесты проходят
10. ✅ ~50 новых contract tests проходят
11. ✅ `src/` не изменён
12. ✅ MCP не реализован
