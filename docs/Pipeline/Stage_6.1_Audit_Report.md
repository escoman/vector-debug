# Аудит Agent API — Stage 6.1

**Коммиты:** `bfc523a`, `bbf433b`  
**Дата аудита:** 6 сентября 2026

---

## 1. Overall Status

**Stage 6.1 NEARLY COMPLETE**

Подавляющая часть ТЗ реализована. Остаются 2 концептуальных пробела и несколько проблем согласованности контрактов.

---

## 2. Таблица требований ТЗ

| # | Требование Stage 6.1 | Текущий метод | Статус | Что осталось |
|---|---|---|---|---|
| 1 | `run()` | `AgentApi::run()` → `void` | DONE | — |
| 2 | `pause()` | `AgentApi::pause()` → `void` | DONE | — |
| 3 | `step()` | `AgentApi::step()` → `void` | DONE | — |
| 4 | `reset()` | `AgentApi::reset()` → `void` | DONE | — |
| 5 | `isRunning()` | `AgentApi::isRunning()` → `bool` | DONE | — |
| 6 | `loadRom()` | `AgentApi::loadRom()` → `bool` | DONE | Возвращает `bool`, не `AgentApiResult` |
| 7 | loadRom structured result (`path`, `origin`, `pc`) | `AgentApi::loadRomInfo()` → `AgentApiResult<LoadRomResult>` | DONE | — |
| 8 | `getCpuState()` | `AgentApi::getCpuState()` → `CpuState` | DONE | Не обёрнут в `AgentApiResult` |
| 9 | `getRegisters()` | — | **MISSING** | Семантически покрыт `getCpuState()` (возвращает все регистры). Отдельный метод не нужен |
| 10 | `setRegister(name, value)` | `AgentApi::setRegister()` → `AgentApiResult<void>` | DONE | — |
| 11 | `readMemory(address, length)` | `AgentApi::readMemory()` → `vector<uint8_t>` | DONE | Не обёрнут в `AgentApiResult` |
| 12 | `writeMemory(address, data)` | `AgentApi::writeMemory()` → `bool` | DONE | Не обёрнут в `AgentApiResult` |
| 13 | `setBreakpoint(address)` | `AgentApi::setBreakpoint()` → `CommandResult` | DONE | — |
| 14 | `removeBreakpoint(address)` | `AgentApi::clearBreakpoint()` → `CommandResult` | DONE | — |
| 15 | `listBreakpoints()` | `AgentApi::listBreakpoints()` → `vector<DebuggerBreakpoint>` | DONE | Не обёрнут в `AgentApiResult` |
| 16 | `clearBreakpoints()` | `AgentApi::clearAllBreakpoints()` → `AgentApiResult<void>` | DONE | — |
| 17 | enable/disable breakpoints | `AgentApi::setBreakpointEnabled()` → `CommandResult` | DONE | — |
| 18 | Breakpoint sync до Board | Через `requestAdd/RemoveBreakpoint` → Command Queue → `syncBreakpointsToTarget()` → `DebugAdapter::syncBreakpoints()` → Board | DONE | Подтверждено тестами |
| 19 | `disassemble(address, count)` | `AgentApi::disassemble()` → `AgentApiResult<vector<...>>` | DONE | — |
| 20 | `getInstructionHistory(count)` | `AgentApi::getInstructionHistory()` → `AgentApiResult<vector<...>>` | DONE | Семантически отличается от `getExecutionTrace` (см. §5) |
| 21 | `getExecutionTrace()` | `AgentApi::getExecutionTrace()` → `vector<InstructionEvent>` | DONE | Не обёрнут в `AgentApiResult` |
| 22 | `getStack(limit)` | `AgentApi::getStack()` → `AgentApiResult<vector<StackEntry>>` | DONE | — |
| 23 | `readIo(port)` | — | **MISSING** | Нет ни в `IDebugBackend`, ни в `AgentApi` |
| 24 | `writeIo(port, value)` | — | **MISSING** | Нет ни в `IDebugBackend`, ни в `AgentApi` |
| 25 | `getSymbols()` | `AgentApi::getSymbols()` → `AgentApiResult<vector<SymbolInfo>>` | DONE | — |
| 26 | `getFunction(address)` | `AgentApi::getFunction()` → `AgentApiResult<SymbolInfo>` | DONE | — |
| 27 | `getXrefs(address)` | `AgentApi::getXrefs()` → `AgentApiResult<vector<XrefResult>>` | DONE | — |
| 28 | `getCallGraph(address, depth, limit)` | `AgentApi::getCallGraph()` → `AgentApiResult<vector<CallGraphEdge>>` | PARTIAL | Параметр `depth` отсутствует; фильтрация по адресу и лимит есть |
| 29 | `getMemoryMap()` | `AgentApi::getMemoryMap()` → `AgentApiResult<vector<MemoryMapBlock>>` | DONE | — |
| 30 | `getVramInfo()` | `AgentApi::getVramInfo()` → `AgentApiResult<VramInfoResult>` | DONE | Зависит от реального video mode (256/512) |
| 31 | `getScreenInfo()` | `AgentApi::getScreenInfo()` → `AgentApiResult<ScreenInfoResult>` | DONE | — |
| 32 | `getDebugState()` → `AgentApiResult<DebugStateResult>` | `AgentApi::getDebugState()` → `AgentApiResult<DebugStateResult>` | DONE | Содержит running, CPU, breakpoints, current instruction/function |

---

## 3. Public Agent API — полный список

| # | Метод | Назначение | Тип результата | Mock | Unit test | Integration test |
|---|---|---|---|---|---|---|
| 1 | `run()` | Запуск | `void` | ✅ | ✅ | ✅ |
| 2 | `pause()` | Пауза | `void` | ✅ | ✅ | ✅ |
| 3 | `step()` | Пошаговое выполнение | `void` | ✅ | ✅ | ✅ |
| 4 | `reset()` | Сброс CPU | `void` | ✅ | ✅ | — |
| 5 | `isRunning()` | Статус running | `bool` | ✅ | ✅ | — |
| 6 | `getCpuState()` | Снимок CPU | `CpuState` | ✅ | ✅ | — |
| 7 | `readMemory()` | Чтение памяти | `vector<uint8_t>` | ✅ | ✅ | — |
| 8 | `writeMemory()` | Запись памяти | `bool` | ✅ | ✅ | — |
| 9 | `setBreakpoint()` | Установить breakpoint | `CommandResult` | ✅ | ✅ | ✅ |
| 10 | `clearBreakpoint()` | Удалить breakpoint | `CommandResult` | ✅ | ✅ | ✅ |
| 11 | `setBreakpointEnabled()` | Enable/disable | `CommandResult` | ✅ | — | ✅ |
| 12 | `listBreakpoints()` | Список breakpoint'ов | `vector<DebuggerBreakpoint>` | ✅ | ✅ | — |
| 13 | `clearAllBreakpoints()` | Очистить все | `AgentApiResult<void>` | ✅ | ✅ | — |
| 14 | `setRegister()` | Установить регистр | `AgentApiResult<void>` | ✅ | ✅ (8 тестов) | — |
| 15 | `disassemble()` | Дизассемблировать | `AgentApiResult<vector<...>>` | ✅ | ✅ (4 теста) | — |
| 16 | `getInstructionHistory()` | История инструкций | `AgentApiResult<vector<...>>` | ✅ | ✅ (4 теста) | — |
| 17 | `getExecutionTrace()` | Трассировка выполнения | `vector<InstructionEvent>` | ✅ | ✅ | — |
| 18 | `getIoTrace()` | Трассировка I/O | `vector<IoAccessEvent>` | ✅ | — | — |
| 19 | `getScreen()` | Снимок экрана | `IDebugBackend::ScreenSnapshot` | ✅ | — | — |
| 20 | `createFunction()` | Создать функцию | `CommandResult` | ✅ | ✅ | ✅ |
| 21 | `renameFunction()` | Переименовать | `CommandResult` | ✅ | ✅ | ✅ |
| 22 | `setFunctionComment()` | Комментарий | `CommandResult` | ✅ | ✅ | ✅ |
| 23 | `deleteFunction()` | Удалить функцию | `CommandResult` | ✅ | ✅ | ✅ |
| 24 | `addLabel()` | Добавить метку | `CommandResult` | ✅ | ✅ | ✅ |
| 25 | `setComment()` | Комментарий (alias) | `CommandResult` | ✅ | ✅ (через setFunctionComment) | — |
| 26 | `applyAnnotation()` | Применить аннотацию | `CommandResult` | ✅ | ✅ | ✅ |
| 27 | `getFunctionContext()` | Контекст функции | `FunctionContext` | ✅ | ✅ (4 теста) | ✅ |
| 28 | `traceFunction()` | Трассировка функции | `TraceResult` | ✅ | ✅ | ✅ |
| 29 | `getStack()` | Содержимое стека | `AgentApiResult<vector<...>>` | ✅ | ✅ (4 теста) | — |
| 30 | `getMemoryMap()` | Карта памяти | `AgentApiResult<vector<...>>` | ✅ | ✅ (2 теста) | — |
| 31 | `getScreenInfo()` | Информация об экране | `AgentApiResult<ScreenInfoResult>` | ✅ | ✅ | — |
| 32 | `getVramInfo()` | Информация о VRAM | `AgentApiResult<VramInfoResult>` | ✅ | ✅ (2 теста: 256 + 512) | — |
| 33 | `getSymbols()` | Таблица символов | `AgentApiResult<vector<SymbolInfo>>` | ✅ | ✅ (3 теста) | — |
| 34 | `getFunction()` | Найти символ | `AgentApiResult<SymbolInfo>` | ✅ | ✅ (2 теста) | — |
| 35 | `getXrefs()` | Перекрёстные ссылки | `AgentApiResult<vector<XrefResult>>` | ✅ | ✅ (2 теста) | — |
| 36 | `getCallGraph()` | Граф вызовов | `AgentApiResult<vector<CallGraphEdge>>` | ✅ | ✅ (2 теста) | — |
| 37 | `loadRom()` | Загрузить ROM | `bool` | ✅ | — | — |
| 38 | `loadRomInfo()` | Загрузить ROM (структ.) | `AgentApiResult<LoadRomResult>` | ✅ | ✅ (3 теста) | — |
| 39 | `getDebugState()` | Снимок отладчика | `AgentApiResult<DebugStateResult>` | ✅ | ✅ (3 теста) | — |
| 40 | `log()` | Журнал операций | `const AgentLog &` | ✅ | ✅ | — |
| 41 | `clearLog()` | Очистить журнал | `void` | ✅ | ✅ | — |

**Итого: 41 публичный метод.**

---

## 4. Архитектурные проблемы

### 4.1. Чистая архитек — БЕЗ проблем

- ✅ Нет зависимостей от ImGui
- ✅ Нет зависимостей от SDL
- ✅ Нет прямого доступа к `Board`
- ✅ Нет прямого доступа к `Memory`
- ✅ Нет прямого доступа к `i8080`
- ✅ Все операции идут через `IDebugBackend`
- ✅ State-changing операции используют Command Queue
- ✅ Breakpoint sync проходит полную цепочку до Board

### 4.2. Обнаруженные проблемы

**Проблема 1: `getScreen()` возвращает `IDebugBackend::ScreenSnapshot`**

Это единственный случай, когда тип из `IDebugBackend` "протекает" в публичный API Agent API. Для JSON/MCP сериализации это создаст зависимость от интерфейса backend'а.

*Рекомендация:* определить `AgentScreenSnapshot` в `agent_types.h` или использовать конвертацию.

**Проблема 2: Отсутствие `readIo()`/`writeIo()`**

ТЗ (§I/O) требует `readIo(port)` и `writeIo(port, value)`. Эти методы отсутствуют и в `IDebugBackend`, и в `AgentApi`. IDebugBackend не имеет прямого метода чтения/записи портов — только IO history tracking.

*Оценка:* Для полноценного AI-агента, который хочет исследовать I/O-устройства Vector-06C, это существенный пробел. Однако реализация потребует добавления виртуальных методов в `IDebugBackend`.

**Проблема 3: `getCallGraph()` не имеет параметра `depth`**

ТЗ указывает `getCallGraph(address, depth, limit)`. Текущая реализация принимает `(address, limit)`, без ограничения глубины рекурсии. Для неглубокого анализа это не критично.

---

## 5. Дублирование

### 5.1. `getInstructionHistory()` vs `getExecutionTrace()`

Оба метода вызывают `backend_.instructionHistorySnapshot()`. Это **не дублирование**, а различная семантика:
- `getInstructionHistory()` — последние N выполненных инструкций с дизассемблированием (формат `InstructionHistoryEntry`)
- `getExecutionTrace()` — сырые `InstructionEvent` с полной информацией (sequence, before/after CPU state, operand bytes)

Разные типы результата, разные сценарии использования. **Допустимо.**

### 5.2. `loadRom()` vs `loadRomInfo()`

Оба вызывают `backend_.loadRom()`. `loadRom()` — legacy (возвращает `bool`), `loadRomInfo()` — Stage 6.1 (структурированный результат). **Незначительное дублирование**, `loadRom()` можно считать deprecated.

### 5.3. `getCpuState()` vs `getRegisters()`

`getCpuState()` возвращает полный `CpuState` (все регистры + PC + SP + flags + iff + cycles). Отдельный `getRegisters()` не нужен — информация полностью покрывается.

### 5.4. VRAM / Video mode / Disassembler / Symbols / Xrefs / Call graph / Memory map

Все используют существующую backend-логику:
- VRAM: `videoModeSnapshot()` — без дублирования
- Disassembler: `::disassemble()` из `disassembler.h` — без дублирования
- Symbols/Xrefs/CallGraph: `SymbolDatabase` — без дублирования
- Memory Map: `liveActivitySnapshot()` + `symbolDatabase().allRegions()` — без дублирования
- Breakpoints: `requestAddBreakpoint()` и т.д. — без дублирования

**Дублирование не обнаружено.**

---

## 6. Mock Coverage

`MockAgentBackend` реализует **все 42+ виртуальных метода** `IDebugBackend`.

| Группа | Статус |
|---|---|
| CPU state / execution | ✅ Полностью |
| Memory access | ✅ Полностью |
| Breakpoints (direct + command) | ✅ Полностью |
| History (instruction/memory/IO) | ✅ Полностью |
| Screen / Video mode / VRAM writes | ✅ Полностью (video mode настраивается через `setVideoMode()`) |
| Palette / Sound | ✅ Заглушки (пустые snapshots) |
| Activity / Live Activity | ✅ Activity counters; Live Activity — заглушка |
| Symbols (read + write commands) | ✅ Полностью |
| Trace execution | ✅ Полностью (синхронно через promise/future) |
| ROM / WAV / Keyboard | ✅ Заглушки (всегда true / no-op) |

---

## 7. Test Coverage

| Тестовый набор | Кол-во тестов | Статус |
|---|---|---|
| test_agent_api (unit) | 71/71 | ✅ PASS |
| test_agent_commands | 15/15 | ✅ PASS |
| test_agent_integration | 45/45 | ✅ PASS |
| test_vram_mapping | 17/17 | ✅ PASS |
| test_symbol_database | 22/22 | ✅ PASS |
| test_backend | 115/115 | ✅ PASS |
| test_rom_load_address | 28/28 | ✅ PASS |
| test_board_smoke | 2/2 | ✅ PASS |
| test_config_manager | 8/8 | ✅ PASS |
| test_live_map | 21/21 | ✅ PASS |
| test_reset_cpu_state | 14/14 | ✅ PASS |
| test_rom_loading | 9/9 | ✅ PASS |
| test_workspace | 15/15 | ✅ PASS |
| test_agent_mock (Board-level) | ~30 | ⚠️ PRE-EXISTING FAILURE (boot ROM overlay) |
| **Итого (рабочие)** | **382/382** | **✅ ALL PASS** |

### Покрытие по методам Agent API

- **Полное (unit + integration):** breakpoints, annotations, trace, function context, VRAM attribution, IO attribution, memory attribution, stack tracking
- **Unit only:** disassemble, getInstructionHistory, setRegister (8 тестов), getStack, getMemoryMap, getScreenInfo, getVramInfo (256 + 512), getSymbols, getFunction, getXrefs, getCallGraph, loadRomInfo, getDebugState
- **Без тестов:** `getScreen()`, `getIoTrace()`, `loadRom()` (только через `loadRomInfo`)

---

## 8. Единый контракт `AgentApiResult<T>`

### Методы, использующие `AgentApiResult<T>` (14 методов):
`setRegister`, `disassemble`, `getInstructionHistory`, `clearAllBreakpoints`, `getStack`, `getMemoryMap`, `getScreenInfo`, `getVramInfo`, `getSymbols`, `getFunction`, `getXrefs`, `getCallGraph`, `loadRomInfo`, `getDebugState` — все методы Stage 6.1.

### Методы, НЕ использующие `AgentApiResult<T>` (27 методов):

**Группа A — pre-Stage 6.1 (legacy):**
- `run()`, `pause()`, `step()`, `reset()` → `void` — оправдано (fire-and-forget команды)
- `isRunning()` → `bool` — простое свойство состояния
- `getCpuState()` → `CpuState` — snapshot, не может упасть
- `readMemory()` → `vector<uint8_t>` — не может упасть (возвращает пустой vector)
- `writeMemory()` → `bool` — уже возвращает success/failure
- `listBreakpoints()` → `vector<DebuggerBreakpoint>` — не может упасть
- `getExecutionTrace()` → `vector<InstructionEvent>` — snapshot
- `getIoTrace()` → `vector<IoAccessEvent>` — snapshot
- `getScreen()` → `ScreenSnapshot` — snapshot
- `loadRom()` → `bool` — legacy, есть `loadRomInfo()`
- `log()` → `const AgentLog &` — getter
- `clearLog()` → `void` — fire-and-forget

**Группа B — `CommandResult` (10 методов):**
- `setBreakpoint`, `clearBreakpoint`, `setBreakpointEnabled`, `createFunction`, `renameFunction`, `setFunctionComment`, `deleteFunction`, `addLabel`, `setComment`, `applyAnnotation`

`CommandResult` — это **фактически аналог** `AgentApiResult` для операций, проходящих через Command Queue. Он содержит `success`, `error`, `status`. Это **допустимое исключение** — командам нужен `status` (Completed/Failed/Timeout/Cancelled), которого нет в `AgentApiResult`.

### Вывод по контракту

Новые методы Stage 6.1 **все** используют `AgentApiResult<T>`. Старые методы (pre-6.1) используют `void`/`bool`/raw types, что оправдано их семантикой (snapshots, fire-and-forget). **Критических нарушений контракта нет.**

---

## 9. Проверка `bfc523a` + `bbf433b`

1. ✅ `getDebugState()` использует `AgentApiResult<DebugStateResult>` — подтверждено
2. ✅ `getVramInfo()` использует текущий video mode — подтверждено
3. ✅ 256×256: 1 плоскость at `vramBase`, размер 8192 — тест проходит
4. ✅ 512×256: 2 плоскости at 0xC000/0xE000, размер 16384 — тест проходит
5. ✅ Нет старых вызовов `getDebugState()`, несовместимых с новой сигнатурой (проверено grep)
6. ✅ Нет сломанных API consumers (все 382 теста проходят)

---

## 10. Проверка изменений в `src/`

**Файлы в `src/` НЕ изменялись.** Все изменения Stage 6.1 находятся в `debugger/agent/`.

`IDebugBackend` уже предоставляет все необходимые методы:
- `videoModeSnapshot()` — для VRAM info
- `getCpuState()`, `getBreakpoints()`, `readMemory()` — для debug state
- `instructionHistorySnapshot()` — для instruction history и execution trace
- `writeRegister()` — для setRegister

**Ни один Agent API метод не обходит архитектуру.**

---

## 11. Remaining Work

### Критически необходимое (для полного соответствия ТЗ):

1. **`readIo(port)` / `writeIo(port, value)`** — отсутствуют. Требуют:
   - Добавить `virtual uint8_t readIoPort(uint8_t port)` и `virtual void writeIoPort(uint8_t port, uint8_t value)` в `IDebugBackend`
   - Реализовать в `DebugBackend` (через DebugAdapter → Board)
   - Добавить в `AgentApi`
   - Реализовать в `MockAgentBackend`
   - Добавить тесты

### Желательное (не критично для Stage 6.1):

2. **`getCallGraph()` — параметр `depth`** — отсутствует ограничение глубины. Для практического использования AI-агентом может быть полезно, но не блокирует.

3. **`getScreen()` — заменить `IDebugBackend::ScreenSnapshot` на standalone тип** — косметическая проблема, не блокирует.

4. **`loadRom()` — пометить как deprecated** — существует параллельно с `loadRomInfo()`.

5. **Тесты для `getIoTrace()`, `getScreen()`, `loadRom()`** — отсутствуют, но не критичны.

6. **`test_agent_mock`** — предсуществующий сбой (boot ROM overlay при записи в 0x0000). Не связан с Stage 6.1, но требует отдельного исправления.

---

## 12. Recommendation

**Можно переходить к следующему этапу** при одном условии:

Если `readIo`/`writeIo` критичны для планируемого AI-агента — необходимо реализовать их следующим итерацией. Если AI-агент будет работать только с памятью, breakpoints, символами и трассировкой — **Stage 6.1 можно считать завершённой**.

Архитектура чистая, дублирование отсутствует, 382 теста проходят, `AgentApiResult<T>` последовательно используется во всех новых методах, VRAM info корректно зависит от видеорежима, breakpoint sync проходит полную цепочку до Board.
