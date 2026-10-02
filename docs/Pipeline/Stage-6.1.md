# ТЗ — Stage 6.1: Завершение Agent API

## 1. Цель этапа

Завершить и стабилизировать Agent API проекта `vector-debugger`.

После выполнения этапа внешний клиент/AI-агент должен иметь возможность управлять отладкой ROM Вектора-06Ц исключительно через Agent API, не имея доступа к:

* ImGui;
* SDL;
* `Board`;
* `Memory`;
* `i8080`;
* `TV`;
* другим внутренним классам эмулятора.

Agent API должен быть самостоятельным интерфейсом над существующим `DebugBackend` / `IDebugTarget`.

**MCP, HTTP, WebSocket и другие внешние протоколы на этом этапе НЕ реализовывать.**

---

# 2. Архитектура

Сохранить архитектуру:

```text
GUI
  │
  ▼
IDebugBackend
  │
  ▼
DebugBackend
  │
  ▼
IDebugTarget
  │
  ▼
DebugAdapter
  │
  ▼
Board
```

Agent API располагается на уровне `DebugBackend` и не должен обращаться непосредственно к `Board`.

Предпочтительная схема:

```text
AI Agent / future MCP
        │
        ▼
    Agent API
        │
        ▼
   DebugBackend
        │
        ▼
   IDebugTarget
        │
        ▼
   DebugAdapter
        │
        ▼
      Board
```

GUI и Agent API используют один и тот же backend.

Не создавать вторую независимую реализацию отладочных операций.

---

# 3. Главный принцип API

API должен быть ориентирован не на внутреннюю структуру программы, а на действия отладчика.

Не экспортировать внутренние классы Vector Debugger.

Например, агенту не должен возвращаться объект `Board`.

Вместо этого возвращаются простые структуры данных:

* числа;
* строки;
* массивы;
* структуры состояния;
* результаты операций;
* ошибки.

API должен быть пригоден для последующего преобразования в JSON/MCP без изменения его семантики.

---

# 4. Управление ROM

Реализовать:

```text
load_rom(path)
```

Требования:

* использовать существующий механизм загрузки ROM;
* определять origin по расширению ROM согласно существующему `rom_load_address`;
* после загрузки устанавливать PC согласно существующему поведению `LOADROM`;
* корректно выполнять reset/инициализацию;
* возвращать результат операции и информацию о загруженном ROM.

Результат должен содержать как минимум:

```text
success
path
origin
pc
```

Не дублировать алгоритм определения origin внутри Agent API.

Использовать существующий код.

---

# 5. Управление выполнением

Реализовать следующие команды:

```text
run()
pause()
step()
reset()
```

Также предусмотреть:

```text
is_running()
```

### run

Запускает выполнение CPU.

### pause

Запрашивает остановку выполнения.

### step

Выполняет ровно одну инструкцию CPU.

Важно:

* операции над `Board` выполняются только в emulation thread;
* Agent API не должен напрямую вызывать методы `Board`;
* использовать существующую command queue / thread synchronization.

### reset

Выполняет существующий reset debugger-а.

Не создавать отдельную реализацию reset.

---

# 6. Состояние CPU

Реализовать:

```text
get_cpu_state()
```

Результат должен содержать:

```text
PC
SP

A
F

B
C
D
E
H
L

IFF
```

При необходимости добавить:

```text
HALT
```

если это состояние уже корректно доступно через существующий backend/target.

Не изменять CPU для получения этой информации.

---

# 7. Память

Реализовать:

```text
read_memory(address, length)
write_memory(address, data)
```

Требования:

* адреса 16-bit;
* корректная обработка диапазона;
* чтение не должно создавать debugger-side побочных эффектов;
* использовать существующий `DebugMemoryAccess` / adapter;
* запись должна выполняться через emulation thread.

Результат чтения должен быть простым массивом байтов.

Например:

```text
read_memory(0xC000, 16)
```

возвращает:

```text
address = 0xC000
data = [ ...16 bytes... ]
```

Не возвращать внутренний указатель на память.

---

# 8. Регистры

Реализовать:

```text
get_registers()
set_register(name, value)
```

Если текущий backend уже имеет операции изменения регистров, использовать их.

Минимальный набор:

```text
A
F
B
C
D
E
H
L
PC
SP
IFF
```

Изменение регистров выполняется только через command queue.

---

# 9. Breakpoints

Реализовать полный жизненный цикл breakpoint:

```text
set_breakpoint(address)
remove_breakpoint(address)
list_breakpoints()
clear_breakpoints()
```

Дополнительно сохранить существующую поддержку:

```text
enable/disable
```

если она уже реализована в backend.

Особое требование:

### Синхронизация

Agent API не должен иметь отдельного списка breakpoints, не синхронизированного с Board.

Использовать существующий механизм:

```text
DebugBackend
    ↓
DebugAdapter::syncBreakpoints()
    ↓
Board
```

При удалении breakpoint он должен реально удаляться из Board.

Уже исправленный механизм `syncedBreakpoints_` сохранить.

Добавить regression tests:

1. set → breakpoint срабатывает;
2. remove → бывший адрес больше не останавливает CPU;
3. повторный set/remove;
4. два breakpoint → удаление одного не удаляет второй;
5. clear all;
6. disable/enable.

---

# 10. Disassembly

Реализовать:

```text
disassemble(address, count)
```

Результат для каждой инструкции должен содержать минимум:

```text
address
bytes
mnemonic
```

Желательно:

```text
next_address
```

Пример логической структуры:

```text
address: 0x8123
bytes:   ...
mnemonic: MOV A,M
```

Использовать существующий disassembler/opcode information.

Не реализовывать второй disassembler специально для Agent API.

---

# 11. Instruction History

Реализовать:

```text
get_instruction_history(count)
```

Возвращать последние выполненные инструкции.

Минимально:

```text
address
opcode/bytes
disassembly
```

Если существующая история уже содержит дополнительные поля — использовать их.

История должна быть пригодна для анализа AI-агентом.

---

# 12. Execution Trace

Реализовать:

```text
get_execution_trace(...)
```

Использовать существующий механизм Execution Trace.

API должен позволять получить ограниченное количество записей.

Минимально:

```text
address
instruction
```

Не возвращать бесконечные массивы.

Предусмотреть параметр `limit`.

---

# 13. Stack

Реализовать:

```text
get_stack(...)
```

Минимально:

```text
address
value
```

и, если уже поддерживается:

```text
symbol
```

Количество элементов должно ограничиваться параметром `limit`.

---

# 14. I/O

Реализовать:

```text
read_io(port)
```

Если текущий backend уже предоставляет запись:

```text
write_io(port, value)
```

также экспортировать её через Agent API.

Не реализовывать новую модель I/O.

Использовать существующий `IDebugTarget` / adapter.

---

# 15. Symbols / Functions

Экспортировать существующую информацию:

```text
get_symbols(...)
get_function(address)
```

Минимальная информация:

```text
name
address
type
```

Для функции желательно:

```text
start
end
name
```

Не создавать новый symbol database.

Использовать существующий `SymbolDatabase`.

---

# 16. Xrefs

Реализовать:

```text
get_xrefs(address)
```

Возвращать существующие cross-references.

Минимально:

```text
from
to
type
```

Не выполнять отдельный полный анализ ROM при каждом запросе, если существующий Symbol/Xref механизм уже предоставляет эту информацию.

---

# 17. Call Graph

Реализовать:

```text
get_call_graph(...)
```

Использовать существующий Call Graph.

Не создавать отдельную реализацию графа для Agent API.

Результат ограничивать параметрами:

```text
address
depth
limit
```

если такая модель совместима с существующей реализацией.

---

# 18. Memory Map

Экспортировать существующий Memory Map:

```text
get_memory_map()
```

Агенту нужен не GUI-пиксельный framebuffer, а структурированные блоки памяти.

Минимальная информация для блока:

```text
start
end
classification
read_activity
write_activity
```

Classification может быть:

```text
Unknown
Code
Data
```

Текущая логика Live Activity должна использоваться без изменений.

Не переносить GUI-логику цветов в Agent API.

---

# 19. VRAM

Экспортировать существующий VRAM mapping:

```text
get_vram_info()
```

Результат должен описывать реальные области VRAM Вектора-06Ц.

Минимально:

```text
video_mode
planes
address
size
```

Использовать существующий `vram_mapping`.

Не реализовывать новый алгоритм определения VRAM.

---

# 20. Screen

Добавить agent-friendly получение состояния экрана:

```text
get_screen_info()
```

На этом этапе НЕ требуется передавать изображение в виде PNG/JPEG.

Достаточно предоставить структурированную информацию:

```text
width
height
mode
planes
```

Если текущий backend уже способен предоставить snapshot экрана — разрешается добавить:

```text
get_screen_snapshot()
```

но это не должно становиться обязательной частью Stage 6.1.

---

# 21. Общий Debug State

Добавить высокоуровневую команду:

```text
get_debug_state()
```

Она должна позволять агенту одним запросом получить краткое состояние debugger-а.

Минимально:

```text
rom
cpu
running
breakpoints
```

При наличии:

```text
current_instruction
function
```

Цель — агенту не нужно делать 5–10 отдельных запросов для обычного анализа текущего состояния.

---

# 22. Ошибки

Все Agent API операции должны иметь единообразный результат.

Предусмотреть:

```text
success
error_code
error_message
```

Для успешной операции `error_code` должен быть пустым/нулевым.

Не использовать исключения как основной протокол взаимодействия Agent API.

Минимальные категории ошибок:

```text
InvalidArgument
InvalidAddress
InvalidRange
NoRomLoaded
NotPaused
NotRunning
OperationFailed
Timeout
Unsupported
```

Использовать только реально необходимые категории.

Не создавать искусственно большое количество типов ошибок.

---

# 23. Потоковая модель

Критически важно:

### GUI thread

может:

* читать immutable snapshots;
* отправлять commands;
* вызывать Agent API.

### Emulation thread

единственный имеет право изменять:

* Board;
* CPU;
* Memory;
* I/O;
* execution state.

Agent API НЕ должен напрямую обращаться к Board.

Например запрещено:

```cpp
agent -> board_->single_step()
```

Правильно:

```text
Agent API
    ↓
requestStep()
    ↓
command queue
    ↓
emulation thread
    ↓
Board::single_step()
```

То же относится к:

* reset;
* run;
* pause;
* memory write;
* register write;
* breakpoint operations.

---

# 24. Чтение состояния

Для чтения состояния использовать существующий механизм snapshots/backend.

Не блокировать GUI или Agent API надолго ожиданием CPU.

Для потенциально долгих операций предусмотреть timeout.

Не вводить mutex вокруг всего `Board`.

---

# 25. JSON-ready структура

Хотя JSON/MCP на этом этапе не реализуется, структуры Agent API должны быть легко сериализуемыми.

Избегать:

```cpp
std::function
raw pointers
Board&
Memory&
ImGui types
SDL types
```

в публичных Agent API структурах.

Предпочтительны:

```cpp
uint16_t
uint32_t
bool
float
std::string
std::vector<T>
enum class
plain structs
```

---

# 26. GUI не должен зависеть от Agent API

GUI продолжает работать через:

```text
IDebugBackend
```

Agent API не должен заставлять GUI использовать новый слой.

Если GUI и Agent API требуют одну и ту же операцию, backend должен переиспользоваться.

Не создавать:

```text
GUI implementation
Agent implementation
```

одной и той же функции.

---

# 27. MockBackend

Расширить существующий `MockBackend` настолько, чтобы Agent API можно было тестировать без реального Vector Board.

Mock должен позволять проверить:

* run;
* pause;
* step;
* reset;
* memory read/write;
* registers;
* breakpoints;
* disassembly;
* history;
* stack;
* I/O;
* symbols;
* xrefs;
* trace.

Не нужно симулировать полноценный CPU.

Mock проверяет контракт API и передачу команд.

---

# 28. Тесты

Добавить отдельный набор тестов:

```text
test_agent_api
```

или расширить существующий, если это уже соответствует архитектуре проекта.

Проверить как минимум:

### ROM

* load ROM;
* origin;
* PC после загрузки;
* reset.

### Execution

* run;
* pause;
* step;
* reset.

### CPU

* чтение регистров;
* изменение регистров.

### Memory

* read;
* write;
* boundary addresses;
* invalid range.

### Breakpoints

* set;
* hit;
* remove;
* clear;
* enable/disable;
* synchronization with Board.

### Disassembly

* address;
* instruction;
* bytes.

### Analysis

* history;
* trace;
* stack;
* symbols;
* functions;
* xrefs;
* call graph;
* memory map;
* VRAM.

### Error handling

Проверить корректные ошибки для:

* ROM не загружен;
* неправильного адреса;
* неправильного диапазона;
* неизвестного символа;
* неподдерживаемой операции.

---

# 29. Integration test

Обязательно иметь хотя бы один тест, проходящий полный путь:

```text
Agent API
   ↓
DebugBackend
   ↓
IDebugTarget
   ↓
DebugAdapter
   ↓
Board
```

Пример:

```text
load ROM
↓
set breakpoint
↓
run
↓
breakpoint hit
↓
get_cpu_state
↓
read_memory
↓
step
↓
get_instruction_history
↓
remove breakpoint
↓
continue
```

Это должен быть тест реального Board, а не MockBackend.

---

# 30. Нельзя делать в этом этапе

Не делать:

* MCP server;
* HTTP API;
* WebSocket;
* GUI для Agent API;
* поддержку Claude/Codex/Qwen напрямую;
* собственный LLM client;
* AI prompt system;
* AI memory;
* автоматический reverse engineering;
* новые алгоритмы анализа ROM;
* новый disassembler;
* новый symbol database;
* новую модель VRAM;
* изменения публичного API `Board`, если существующего API достаточно.

Особенно важно:

**не изменять `src/` без необходимости.**

Предпочтительно все изменения ограничить:

```text
debugger/
```

Если для конкретной функции действительно требуется изменение `src/`, сначала доказать отсутствие необходимого существующего API.

---

# 31. Критерий завершения Stage 6.1

Stage считается завершённым, когда:

1. Agent API имеет законченный стабильный интерфейс.
2. Все основные debugger operations доступны агенту.
3. Agent API не зависит от GUI.
4. Agent API не зависит от конкретного `Board`.
5. Все изменения Board выполняются через emulation thread.
6. Breakpoint synchronization работает корректно.
7. Есть Mock tests.
8. Есть integration test с реальным Board.
9. Нет дублирования существующей debugger-логики.
10. Все существующие тесты продолжают проходить.
11. Новые тесты проходят.
12. API не содержит ImGui/SDL/Vector-specific types в публичном интерфейсе.
13. API пригоден для последующей сериализации в JSON/MCP.

---

# 32. Ожидаемый результат

После Stage 6.1 должно быть возможно написать следующий слой:

```text
MCP Tool
    ↓
Agent API call
    ↓
DebugBackend
    ↓
Vector Debugger
```

не изменяя саму отладочную логику.

То есть следующим этапом разработчик должен иметь возможность реализовать MCP server практически как тонкий адаптер:

```text
MCP request
    ↓
parse arguments
    ↓
Agent API
    ↓
serialize result
    ↓
MCP response
```

**Главная цель Stage 6.1 — сделать именно такой Agent API.**
