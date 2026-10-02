# Stage 5.3.2 — Command Queue и строгая изоляция Emulation Thread

## 1. Цель

Исправить архитектурные проблемы Stage 5.3.1:

* заменить single-slot `pendingGenericCommand_` на настоящую очередь команд;
* гарантировать выполнение всех операций изменения состояния только в Emulation Thread;
* перенести `executeTrace()` в Emulation Thread;
* исключить прямое выполнение CPU/Board из Agent Thread;
* обеспечить корректную работу нескольких одновременных Agent requests;
* добавить реальные multithread integration tests.

Функциональность AI Agent не расширять.

---

# 2. Главный инвариант

Должен соблюдаться строгий принцип:

```text
GUI Thread ──────┐
                 │
Agent Thread ────┼──→ Command Queue ──→ Emulation Thread ──→ Board
                 │
Other Thread ────┘
```

**Только Emulation Thread имеет право непосредственно изменять состояние Board/CPU/Memory/IO.**

Agent Thread никогда не должен непосредственно выполнять:

* CPU instruction;
* Board execution;
* Memory write;
* Register write;
* reset;
* breakpoint mutation;
* SymbolDatabase mutation;
* trace execution.

---

# 3. Настоящая Command Queue

## 3.1. Удалить single-slot механизм

Удалить:

```cpp
pendingGenericCommand_
```

и любую архитектуру вида:

```cpp
GenericCommand *pending...
```

одна команда не должна иметь возможность перезаписать другую.

---

## 3.2. Реализовать очередь

Допускается:

```cpp
std::queue<Command>
```

с:

```cpp
std::mutex
std::condition_variable
```

либо эквивалентная потокобезопасная очередь.

Каждая команда должна существовать независимо от вызывающего stack frame.

Нельзя помещать в очередь указатель на локальный объект вызывающего потока.

---

# 4. Command Future / Result

Каждая state-changing command должна иметь собственный результат.

Рекомендуемый механизм:

```cpp
std::future<CommandResult>
```

или эквивалент.

Схема:

```text
Agent Thread
    │
    ├── create command
    ├── enqueue(command)
    └── wait future
             │
             ▼
       Emulation Thread
             │
             ├── execute command
             └── set result
             │
             ▼
       Agent Thread
```

Не использовать:

* `sleep()`;
* polling;
* busy wait;
* ожидание изменения случайного состояния;
* указатели на локальные command objects.

---

# 5. Command lifecycle

Каждая команда должна иметь состояние:

```text
Queued
Executing
Completed
Failed
Timeout
```

Минимально допустимо вернуть:

```cpp
CommandResult {
    bool success;
    CommandStatus status;
    std::string error;
}
```

Ошибки не должны теряться.

---

# 6. Все state-changing Backend API

Следующие операции обязаны использовать Command Queue:

```text
requestRun()
requestPause()
stepInstruction()
requestReset()
requestLoadRom()
requestWriteMemory()
requestWriteRegister()

requestAddBreakpoint()
requestRemoveBreakpoint()
requestSetBreakpointEnabled()

requestCreateFunction()
requestRenameSymbol()
requestSetComment()
requestDeleteSymbol()

requestExecuteTrace()
```

Названия могут отличаться, но принцип обязателен.

---

# 7. Step

Текущий:

```cpp
stepInstruction()
```

не должен непосредственно исполнять CPU, если вызывается из Agent/GUI thread.

Необходимо разделить:

```text
requestStep()
```

и внутреннюю операцию:

```text
executeStep()
```

где:

```text
requestStep()
    ↓
Command Queue
    ↓
Emulation Thread
    ↓
executeStep()
```

Внутренний `executeStep()` не должен быть доступен Agent API.

---

# 8. Run / Pause

`requestRun()` должен только отправлять команду.

Он не должен выполнять CPU и не должен напрямую обращаться к Board.

Особенно запрещено выполнение из Agent Thread:

```cpp
target_->getCpuState()
target_->stepInstruction()
target_->run()
```

для изменения execution state.

Проверка breakpoint, CPU state и изменение `running_` должны выполняться в Emulation Thread.

---

# 9. Breakpoints

Все изменения breakpoint state:

```text
add
remove
enable
disable
```

выполняются только Emulation Thread.

Agent:

```text
requestAddBreakpoint()
```

получает `future<CommandResult>` и ждёт завершения.

Чтение списка breakpoint допускается через thread-safe snapshot.

---

# 10. Symbol / Annotation

Все изменения:

```text
createFunction
renameFunction
deleteFunction
setFunctionComment
addLabel
setComment
```

должны выполняться внутри Emulation Thread.

Agent не должен получать mutable `SymbolDatabase`.

Существующий `symbolDatabase()` можно временно оставить для чтения, если удаление API потребует большого отдельного рефакторинга, но:

**mutable reference не должна использоваться Agent для записи.**

---

# 11. `executeTrace()` — обязательное исправление

## 11.1. Текущая проблема

Нельзя делать:

```text
Agent Thread
    ↓
executeTrace()
    ↓
CPU
```

## 11.2. Требуемая архитектура

Добавить:

```cpp
requestExecuteTrace(...)
```

который создаёт command:

```text
Agent Thread
    ↓
Command Queue
    ↓
Emulation Thread
    ↓
executeTraceInternal(...)
    ↓
TraceResult
    ↓
future
    ↓
Agent Thread
```

---

# 12. `executeTraceInternal()`

Внутренняя функция выполняется исключительно в Emulation Thread.

Она может:

* читать CPU state;
* выполнять инструкции;
* читать Memory;
* получать IO events;
* получать VRAM events;
* отслеживать SP;
* собирать executed PCs.

Она не должна быть доступна напрямую через `IDebugBackend` как обычная synchronous operation.

---

# 13. Trace execution

`traceFunction(address)` должен использовать:

```text
requestAddBreakpoint()
requestRun()
wait
requestExecuteTrace()
requestRemoveBreakpoint()
```

Но необходимо учитывать, что после достижения entry breakpoint эмуляция остановлена.

`requestExecuteTrace()` должен самостоятельно выполнить исследуемый execution interval в Emulation Thread.

---

# 14. Trace Result

`TraceResult` должен возвращаться через command result/future.

Нельзя возвращать указатель или ссылку на временный объект.

Результат должен быть value-owned:

```cpp
TraceResult
```

или `shared_ptr<const TraceResult>`.

---

# 15. Несколько одновременных Agent requests

Очередь должна корректно обрабатывать:

```text
Agent A → command A
Agent B → command B
Agent C → command C
```

Результат каждой команды должен вернуться именно своему caller.

Нельзя:

```text
A получает результат B
B получает результат A
```

или терять одну из команд.

---

# 16. Commands во время Running

Это критически важно.

Команды должны корректно обрабатываться, когда эмулятор находится в состоянии:

```text
Running
```

Нельзя иметь архитектуру:

```cpp
if (running_)
    return;
```

которая блокирует обработку всех generic commands.

Необходимо определить безопасную модель:

### Допускается два класса команд

```text
Immediate-safe
Execution-state
```

или единая очередь.

Но в любом случае:

```text
Pause
Breakpoint
Trace
Quit
```

должны иметь определённое поведение во время Running.

---

# 17. Приоритет Pause / Quit

Во время непрерывного выполнения CPU очередь должна гарантировать, что:

```text
Pause
Quit
```

не будут ждать окончания бесконечного execution loop.

Polling command queue должно происходить регулярно внутри emulation loop.

Не требуется polling после каждой инструкции, если это слишком дорого.

Допускается проверять очередь через фиксированный interval:

```text
N instructions
или
N cycles
```

но latency должна быть ограниченной и предсказуемой.

---

# 18. `requestRun()` и breakpoint

Проверка:

```text
current PC == breakpoint
```

должна выполняться в Emulation Thread.

Agent Thread не должен читать CPU PC для принятия решения:

> «можно ли запускать».

Если breakpoint уже находится на текущем PC, поведение должно быть определено явно и покрыто тестом.

---

# 19. Thread-safe snapshots

Следующие операции должны оставаться безопасными для Agent/GUI:

```text
getCpuState()
getMemorySnapshot()
getBreakpoints()
getInstructionHistory()
getIoHistory()
getVramActivity()
getSymbols()
```

Они должны возвращать snapshot/value.

Запрещены mutable references на состояние, изменяемое Emulation Thread.

---

# 20. Memory / IO / VRAM

Существующую instrumentation не переписывать.

Для dynamic trace продолжить использовать:

```text
InstructionEvent
MemoryEvent
IO event
VRAM event
sequence → PC
```

как реализовано в Stage 5.3.1.

Главное требование:

```text
trace interval
      ↓
event sequence range
      ↓
only relevant events
```

---

# 21. Unknown PC

Никогда не использовать:

```cpp
pc = 0
```

как обозначение неизвестного PC.

Если PC не удалось определить:

```text
hasPc = false
```

или эквивалентный механизм.

`0x0000` должен означать именно реальный PC 0000.

---

# 22. Command lifetime

Запрещено:

```cpp
GenericCommand cmd;
queue.push(&cmd);
```

если `cmd` находится на stack вызывающего потока.

Command должен владеть всеми необходимыми данными:

```text
address
value
parameters
promise
```

до момента завершения исполнения.

---

# 23. Cancellation / timeout

Для операций:

```text
trace
run
pause
```

должен существовать безопасный timeout.

При timeout:

```text
CommandStatus::Timeout
```

и Emulation Thread не должен продолжать выполнение неконтролируемой операции после того, как Agent считает её завершённой.

Особенно важно для `executeTrace()`.

---

# 24. MockBackend

MockBackend сохранить.

Он должен имитировать:

```text
Command Queue
Future
CommandResult
```

достаточно реалистично.

Но Mock не должен быть единственным доказательством потокобезопасности.

---

# 25. Обязательный Real Integration Test

Добавить тест с реальным:

```text
AgentApi
    ↓
DebugBackend
    ↓
DebugAdapter
    ↓
Board
```

и отдельным Emulation Thread.

Минимальный сценарий:

```text
1. Load ROM
2. Start Emulation Thread
3. Agent thread вызывает requestRun()
4. Emulation thread выполняет CPU
5. Agent thread вызывает requestPause()
6. Получается CommandResult
7. CPU state проверяется
```

---

# 26. Обязательный concurrent integration test

Два Agent threads одновременно:

```text
Thread A:
    requestAddBreakpoint(0x0200)

Thread B:
    requestAddBreakpoint(0x0300)
```

После завершения:

```text
breakpoint 0200 существует
breakpoint 0300 существует
```

Ни одна команда не потеряна.

---

# 27. Обязательный trace integration test

Реальный ROM:

```text
main
    CALL function
    ...
    
function:
    memory write
    IO write
    VRAM write
    RET
```

Тест:

```text
Agent
 ↓
traceFunction(function)
 ↓
real execution
 ↓
TraceResult
```

Проверить:

* entry PC;
* exit PC;
* executed PCs;
* memory write;
* IO write;
* VRAM write;
* SP;
* exit reason;
* отсутствие событий до entry;
* отсутствие событий после exit.

---

# 28. Обязательный concurrent trace test

Два Agent threads не должны одновременно запускать два `executeTrace()` на одном Board.

Backend должен либо:

```text
serialize
```

либо вернуть корректную:

```text
Busy
```

но никогда:

```text
undefined behavior
```

---

# 29. Regression

Проверить:

### OFF

```text
ENABLE_AI_AGENT=OFF
```

Основной debugger полностью собирается и работает.

Agent library/tests отсутствуют.

### ON

```text
ENABLE_AI_AGENT=ON
```

Все существующие тесты + все Agent tests проходят.

---

# 30. Обязательные тесты

Добавить минимум:

1. command queue preserves order;
2. concurrent commands are not lost;
3. each command receives its own result;
4. command executes on Emulation Thread;
5. Agent Thread never executes CPU directly;
6. commands work while Running;
7. Pause while Running;
8. Quit while Running;
9. breakpoint mutation through queue;
10. annotation mutation through queue;
11. trace execution through queue;
12. trace timeout;
13. concurrent trace requests;
14. real DebugBackend threaded integration;
15. real breakpoint integration;
16. real trace integration;
17. memory attribution;
18. IO attribution;
19. VRAM attribution;
20. unknown PC remains unknown.

---

# 31. Проверка архитектуры

После реализации необходимо проверить grep/code review:

Agent-код не должен содержать прямых вызовов:

```text
Board
Memory
i8080
AY
TV
Soundnik
SDL
ImGui
```

для выполнения операций.

Особое внимание:

```text
executeTrace
stepInstruction
requestRun
requestPause
breakpoints
annotations
```

---

# 32. Запрещено в Stage 5.3.2

Не добавлять:

* MCP;
* LLM;
* Ollama;
* Qwen;
* OpenAI;
* Claude;
* AI prompts;
* RAG;
* vector database;
* новые AI функции;
* GUI функции.

Цель этапа — исключительно исправление concurrency и command architecture.

---

# 33. Критерий ACCEPT

Stage 5.3.2 принимается только если одновременно выполнены все условия:

### A. Command Queue

Нет single-slot `pendingGenericCommand_`.

Команды не теряются при concurrent submission.

### B. Thread ownership

Только Emulation Thread непосредственно изменяет Board/CPU state.

### C. Trace

`traceFunction()` выполняет реальный код через Emulation Thread.

### D. Attribution

Memory / IO / VRAM события относятся к конкретному trace interval.

### E. Synchronization

Нет `sleep`, busy-wait или polling внутренних mutable structures.

### F. Integration

Есть реальные threaded tests через:

```text
AgentApi → DebugBackend → DebugAdapter → Board
```

### G. Regression

```text
OFF → все существующие тесты проходят
ON  → все существующие + Agent tests проходят
```

### H. Isolation

Основной debugger не зависит от AI Agent.

---

# 34. Итоговая архитектура после Stage 5.3.2

```text
                 ┌──────────────┐
                 │     GUI      │
                 └──────┬───────┘
                        │
                 ┌──────▼───────┐
                 │ IDebugBackend │
                 └──────┬───────┘
                        │
                 ┌──────▼───────┐
                 │ Command Queue│
                 └──────┬───────┘
                        │
                 ┌──────▼──────────┐
                 │ Emulation Thread│
                 │                 │
                 │ DebugBackend    │
                 │ DebugAdapter    │
                 │ Board           │
                 │ CPU             │
                 └─────────────────┘

                 ┌──────────────┐
                 │  AI Agent    │
                 └──────┬───────┘
                        │
                        └──→ IDebugBackend
```

**Ключевой инвариант:**

> GUI и AI могут просить Emulation Thread что-либо сделать, но никогда не делают это сами.

После выполнения этого этапа можно переходить к следующему этапу — MCP/внешний Agent interface.
