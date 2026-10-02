# Stage 5.3.1 — Исправление и укрепление AI Agent API

## 1. Цель

Исправить архитектурные и функциональные проблемы текущего Stage 5.3, не добавляя пока MCP, LLM или внешние AI-зависимости.

После выполнения:

```text
Agent API
    ↓
IDebugBackend
    ↓
IDebugTarget
    ↓
DebugAdapter
    ↓
Board
    ↓
Vector-06C
```

должна быть потокобезопасной, а результаты анализа должны относиться именно к исследуемой функции, а не ко всей накопленной истории эмулятора.

---

# 2. Жёсткие архитектурные ограничения

### 2.1. AI Agent остаётся изолированным

Весь AI Agent-код находится только в:

```text
debugger/agent/
```

Допускаются изменения интерфейсов существующего Backend только в том случае, если они являются общими debugger API и не содержат AI-специфики.

Запрещено добавлять:

* Ollama;
* Qwen;
* OpenAI;
* Claude;
* MCP SDK;
* HTTP-клиенты;
* Python;
* LLM runtime

в основной Debugger.

`ENABLE_AI_AGENT=OFF` должен полностью исключать Agent из сборки.

---

# 3. Запрет прямого изменения состояния из Agent

`AgentApi` не должен напрямую изменять внутреннее состояние Backend.

Особенно запрещены прямые операции над:

* `SymbolDatabase`;
* breakpoint collection;
* execution state;
* Board;
* CPU;
* Memory;
* I/O;
* VRAM.

Agent может получать данные через публичный `IDebugBackend` API.

Все операции изменения состояния должны проходить через существующий механизм команд Backend.

---

# 4. Breakpoint API

## 4.1. Проблема

Текущие:

```cpp
addBreakpoint()
removeBreakpoint()
setBreakpointEnabled()
```

могут изменять состояние из Agent thread напрямую.

Это необходимо исправить.

## 4.2. Требование

Добавить потокобезопасный command API:

```cpp
requestAddBreakpoint(address)
requestRemoveBreakpoint(address)
requestSetBreakpointEnabled(address, enabled)
```

или эквивалентный механизм.

Команда должна:

1. попасть в Backend command queue;
2. исполниться в debugger/emulation thread согласно существующей архитектуре;
3. вернуть Agent результат выполнения.

Чтение breakpoint state остаётся thread-safe snapshot operation.

---

# 5. Annotation API

## 5.1. Проблема

Agent сейчас получает прямой доступ к `SymbolDatabase`.

Это запрещено.

## 5.2. Требование

Добавить в Backend общие операции:

```cpp
createFunction(...)
renameFunction(...)
deleteFunction(...)
setFunctionComment(...)
addLabel(...)
setComment(...)
```

Но они должны быть реализованы через command protocol.

Agent вызывает только эти операции Backend.

### Важно

Не добавлять в Backend понятия:

```text
AI
Agent
LLM
confidence
hypothesis
```

Backend должен оставаться универсальным debugger API.

---

# 6. Результат annotation operation

Каждая операция должна возвращать достоверный результат:

```cpp
bool success;
std::string error;
```

или эквивалентную структуру.

Нельзя делать:

```cpp
createFunction(...);
return true;
```

если `createFunction()` завершился ошибкой.

Ошибки должны передаваться Agent API без потери информации.

---

# 7. Proposed / Apply архитектура

Сохранить разделение:

```text
AI hypothesis
      ↓
Annotation proposal
      ↓
validation
      ↓
apply
      ↓
Backend command
```

Agent должен иметь возможность сформировать:

```cpp
Annotation
```

без немедленного изменения debugger state.

После этого отдельная операция:

```cpp
applyAnnotation(...)
```

применяет изменение через Backend command API.

---

# 8. Реальный traceFunction()

## 8.1. Проблема

Текущий `traceFunction()` фактически анализирует уже накопленный `instructionHistory`.

Это не dynamic tracing.

## 8.2. Требование

`traceFunction(address)` должен проводить реальный execution experiment.

Минимальный алгоритм:

```text
1. сохранить текущее состояние debugger;
2. установить временный breakpoint на function entry;
3. выполнить run;
4. дождаться входа в функцию;
5. начать сбор trace;
6. выполнить функцию;
7. определить выход из функции;
8. остановить выполнение;
9. удалить временный breakpoint;
10. сформировать TraceResult.
```

При невозможности определить выход:

```text
timeout / max instruction count
```

должен завершать эксперимент безопасно.

---

# 9. Определение выхода из функции

Первичная реализация может использовать:

* `RET`;
* возврат на известный caller PC;
* изменение SP относительно entry;
* maximum instruction count.

Для каждого метода должна быть явно указана степень надёжности.

Например:

```text
ExitReason:
    Ret
    CallerReturn
    Timeout
    Breakpoint
    Halt
    Unknown
```

Не выдавать AI предположение как установленный факт.

---

# 10. Trace должен быть привязан к функции

`TraceResult` должен содержать только события, относящиеся к конкретному execution experiment.

Минимально:

```cpp
struct TraceResult {
    uint16_t entryPC;
    uint16_t exitPC;

    uint32_t instructionCount;
    uint32_t executionCount;

    std::vector<uint16_t> executedPCs;

    std::vector<MemoryAccess> memoryReads;
    std::vector<MemoryAccess> memoryWrites;

    std::vector<IOAccess> ioReads;
    std::vector<IOAccess> ioWrites;

    std::vector<uint16_t> vramWrites;

    std::vector<uint16_t> calledFunctions;

    uint16_t entrySP;
    uint16_t exitSP;

    ExitReason exitReason;
};
```

Конкретная структура может отличаться, если существующие debugger types уже предоставляют эквивалентные данные.

---

# 11. Memory access attribution

## 11.1. Проблема

Накопительные activity counters нельзя использовать как доказательство:

> «эта функция обращалась к адресу X».

Например:

```text
activitySnapshot()
```

показывает накопленную активность всего эмулятора.

## 11.2. Требование

Для dynamic trace необходимо использовать данные, привязанные к конкретным выполненным инструкциям.

Для каждого доступа желательно иметь:

```text
PC
address
access type
value
```

Например:

```cpp
struct MemoryAccess {
    uint16_t pc;
    uint16_t address;
    AccessType type;
    uint8_t value;
};
```

Если существующая instrumentation уже содержит необходимые данные — переиспользовать её.

Не создавать вторую независимую систему Memory instrumentation.

---

# 12. I/O attribution

## 12.1. Проблема

Текущий анализ глобального IO history не позволяет утверждать, что конкретная функция обращалась к конкретному порту.

## 12.2. Требование

IO event должен быть связан с PC инструкции:

```text
PC
port
read/write
value
```

`getFunctionContext()` должен включать только IO события, относящиеся к исследуемой функции.

Если точная привязка отсутствует, поле должно быть:

```text
unknown
```

а не предположение.

---

# 13. VRAM attribution

Аналогично Memory и I/O.

Нельзя использовать:

```text
«в VRAM когда-либо была запись»
```

как доказательство:

```text
«исследуемая функция пишет VRAM».
```

VRAM write event должен иметь хотя бы:

```text
PC
address
value
```

`getFunctionContext()` использует только события данного trace.

---

# 14. Убрать ложную семантику из API

Если информация является:

* heuristic;
* approximate;
* inferred;
* incomplete;

это должно быть отражено в результате.

Например:

```cpp
confidence
```

может использоваться AI Agent как оценка гипотезы.

Но нельзя превращать heuristic result в debugger fact.

---

# 15. getFunctionContext()

`getFunctionContext(address)` должен формировать контекст конкретной функции.

Минимально:

```text
function address
function size
instructions
callers
callees
xrefs
memory reads
memory writes
IO reads
IO writes
VRAM writes
symbols
comments
stack information
```

При этом:

**каждый dynamic факт должен иметь источник — конкретное выполнение/trace.**

Статическая информация может иметь источник:

```text
disassembler
symbol database
xref database
```

---

# 16. Disassembly / границы функции

Текущий linear sweep допускается оставить как первоначальный heuristic analyzer.

Но результат должен быть обозначен как heuristic, если граница функции не подтверждена.

Учитывать как минимум:

```text
RET
CALL
JMP
conditional JMP
RST
HLT
```

Не считать автоматически следующие байты частью функции после безусловного:

```text
JMP
```

если control-flow analysis однозначно показывает недостижимость.

Полноценный CFG в рамках этого этапа не требуется.

---

# 17. Stack analysis

Для trace собирать:

```text
CALL
RET
PUSH
POP
RST
SP
```

и формировать:

```text
entrySP
minimumSP
maximumSP
exitSP
callDepth
```

AI должен получить возможность отличить:

```text
balanced
unbalanced
unknown
```

Но debugger не должен утверждать `balanced`, если выполнение закончилось по timeout.

---

# 18. Threading

Гарантировать:

```text
GUI thread
Agent thread
Emulation thread
```

не имеют одновременного доступа к mutable Board state.

### Agent thread

Может:

* отправить command;
* дождаться результата;
* получить snapshot;
* анализировать snapshot локально.

### Emulation thread

Единственный поток, который непосредственно работает с:

* Board;
* CPU;
* Memory;
* I/O;
* execution state.

### GUI

Получает immutable snapshots.

---

# 19. Command result

Каждая state-changing Agent operation должна иметь понятный lifecycle:

```text
submitted
executing
completed
failed
timeout
```

Не использовать `sleep()` для синхронизации.

Не использовать polling внутренних структур Board.

---

# 20. Agent Log

Существующий Agent Log сохранить.

Для каждой операции желательно записывать:

```text
timestamp
operation
arguments
result
duration
error
```

Не записывать секреты, API keys и другие credentials.

---

# 21. MockBackend

Расширить MockBackend так, чтобы тестировать:

* breakpoint commands;
* annotation commands;
* trace;
* errors;
* timeout;
* concurrent Agent operations.

Mock должен позволять проверить, что Agent API действительно использует Backend API, а не внутренние структуры.

---

# 22. Интеграционный тест

Оставить реальный сценарий:

```text
Agent
 ↓
DebugBackend
 ↓
DebugAdapter
 ↓
Board
 ↓
Vector-06C
```

Создать ROM-тест, в котором функция:

```text
CALL subroutine
    ↓
memory write
    ↓
I/O write
    ↓
VRAM write
    ↓
RET
```

Тест должен проверить, что `TraceResult` содержит именно события этой функции.

Особенно важно:

```text
событие до CALL
событие внутри функции
событие после RET
```

должны различаться.

---

# 23. Regression tests

Обязательно проверить:

### ENABLE_AI_AGENT=OFF

```text
170/170
```

существующих тестов должны продолжать проходить.

Agent targets и Agent tests не должны собираться.

### ENABLE_AI_AGENT=ON

Все существующие тесты + Agent tests должны проходить.

Количество тестов не фиксировать жёстко в ТЗ: важно отсутствие регрессий и наличие всех новых проверок.

---

# 24. Новые обязательные тесты

Добавить тесты:

1. breakpoint mutation через command protocol;
2. breakpoint concurrent access;
3. annotation mutation через command protocol;
4. annotation error propagation;
5. failed `applyAnnotation()`;
6. dynamic trace entry;
7. dynamic trace exit;
8. trace timeout;
9. memory access attribution;
10. IO access attribution;
11. VRAM access attribution;
12. stack tracking;
13. unrelated events excluded from trace;
14. `getFunctionContext()` uses only relevant events;
15. Agent OFF build isolation.

---

# 25. Запрещено

В этом этапе **не делать**:

* MCP;
* Ollama;
* Qwen;
* OpenAI API;
* Claude API;
* LLM inference;
* prompt management;
* vector database;
* RAG;
* автоматическое управление GUI;
* обучение AI;
* AI-specific изменения в `src/`.

---

# 26. Критерий приёмки

Stage 5.3.1 принимается только если:

### Архитектура

```text
Agent
  ↓
IDebugBackend
```

и Agent не имеет доступа к `Board`, `Memory`, `i8080`, `AY`, `TV`, `Soundnik`, SDL или ImGui.

### Потоки

Все изменения состояния выполняются через Backend command mechanism.

### Trace

`traceFunction()` действительно запускает выполнение и получает новый trace.

### Attribution

Memory / I/O / VRAM события однозначно относятся к конкретным инструкциям и конкретному execution experiment.

### Надёжность

Heuristic / unknown данные не выдаются как подтверждённые факты.

### Tests

Все существующие тесты проходят без AI.

Все новые тесты проходят с AI Agent.

### Изоляция

При:

```text
ENABLE_AI_AGENT=OFF
```

основной debugger собирается и работает полностью независимо от Agent.

---

## Результат этапа

После Stage 5.3.1 должен существовать надёжный инструментальный слой:

```text
AI Agent
    │
    ├── observe
    ├── experiment
    ├── trace
    ├── analyze
    └── annotate
          │
          ▼
    IDebugBackend
```

который можно безопасно подключить к будущему MCP/LLM, не изменяя архитектуру самого эмулятора.
