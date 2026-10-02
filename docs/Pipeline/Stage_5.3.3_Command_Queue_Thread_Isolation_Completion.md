# Stage 5.3.3 — Завершение Command Queue и строгая изоляция Emulation Thread

## 1. Цель

Устранить оставшиеся проблемы Stage 5.3.2 и добиться строгого архитектурного инварианта:

> **Только Emulation Thread имеет право непосредственно выполнять CPU/Board state-changing operations.**

Agent Thread и GUI Thread только формируют команды и получают результаты.

Функциональность AI Agent, MCP и LLM в этом этапе не расширять.

---

# 2. Главный инвариант

Production-код должен иметь архитектуру:

```text
GUI Thread ──────┐
                 │
Agent Thread ────┼──→ Command Queue ──→ Emulation Thread ──→ Board
                 │
Other Thread ────┘
```

Только Emulation Thread может непосредственно вызывать:

```text
CPU execution
Board execution
Memory write
Register write
Reset
ROM loading
Breakpoint mutation
SymbolDatabase mutation
Trace execution
```

Исключений в production API быть не должно.

---

# 3. Удалить `pendingTracePromise_`

Удалить глобальное состояние вида:

```cpp
pendingTracePromise_
```

Trace promise/result не должен храниться отдельно от команды.

Каждая команда `ExecuteTrace` должна самостоятельно владеть:

```text
параметрами trace
promise/result
```

Например:

```cpp
struct Command {
    CommandType type;
    ...
    std::shared_ptr<std::promise<TraceExecutionResult>> tracePromise;
};
```

или эквивалентная архитектура.

При:

```text
Agent A → Trace A
Agent B → Trace B
```

должно быть:

```text
Command A → Result A
Command B → Result B
```

Никаких глобальных promise для trace.

---

# 4. Command Queue

Сохранить настоящую FIFO queue из Stage 5.3.2.

Запрещено возвращать single-slot состояние:

```text
pendingCommand
pendingTrace
pendingPromise
```

для передачи команд между потоками.

Каждая команда должна существовать независимо от вызывающего stack frame.

---

# 5. Command Result

Каждая state-changing command должна иметь собственный результат.

Минимально:

```cpp
struct CommandResult {
    bool success;
    CommandStatus status;
    std::string error;
};
```

Для trace допускается:

```cpp
struct TraceCommandResult {
    CommandResult command;
    TraceExecutionResult trace;
};
```

или эквивалент.

---

# 6. `requestRun()`

`requestRun()` не должен обращаться к CPU/Board из вызывающего потока.

Запрещено:

```cpp
requestRun()
    ↓
checkBreakpoint()
    ↓
target_->getCpuState()
```

Правильно:

```text
Agent/GUI
    ↓
Run Command
    ↓
Queue
    ↓
Emulation Thread
    ↓
checkBreakpoint()
    ↓
start execution
```

Все решения о запуске должны приниматься Emulation Thread.

---

# 7. `requestStep()`

Разделить публичный request и внутреннее выполнение.

Публичный API:

```cpp
requestStep()
```

только создаёт Command.

Внутренний:

```cpp
executeStepInternal()
```

исполняется только Emulation Thread.

Удалить production fallback:

```cpp
if (!emulationLoopRunning_)
    stepInstruction();
```

если он позволяет Agent/GUI thread непосредственно выполнять CPU.

Для unit tests создать отдельный тестовый/internal механизм либо запускать настоящий Emulation Thread.

---

# 8. `requestPause()`

Pause должен иметь чёткую архитектуру.

Допускается atomic emergency flag:

```cpp
breakRequested_
```

для минимальной latency.

Но `requestPause()` не должен непосредственно изменять debugger execution state из Agent Thread.

Рекомендуемая схема:

```text
requestPause()
    ↓
Pause Command / break request
    ↓
Emulation Thread
    ↓
state = Paused
```

Если atomic break flag сохраняется как fast-path, документировать его как механизм **запроса остановки**, а не как изменение состояния.

---

# 9. `requestQuit()`

Quit должен корректно завершать Emulation Thread.

Необходимо обеспечить:

```text
Queued command
        ↓
Quit
        ↓
Emulation Thread stops
        ↓
all waiting callers receive result
```

Ни один `future` не должен оставаться навсегда в состоянии ожидания.

---

# 10. Data race elimination

Проверить все поля, которые читаются и записываются разными потоками.

В частности:

```text
pauseRequested_
quitRequested_
running_
state_
traceBusy_
```

Для каждого определить ownership.

Варианты:

```text
atomic
```

или:

```text
mutex-protected
```

или:

```text
Emulation Thread only
```

Нельзя оставлять обычный `bool`, который записывается одним потоком и читается другим без synchronization.

Особое внимание:

```cpp
pauseRequested_
quitRequested_
```

из текущего `backend.cpp`.

---

# 11. `processOneCommand()`

После извлечения команды из очереди она должна получить гарантированный результат.

Нельзя:

```cpp
auto cmd = dequeue();

if (quitRequested_)
    return;
```

если это оставляет:

```text
cmd.promise
```

без результата.

Для каждой извлечённой команды должен существовать один из путей:

```text
Completed
Failed
Timeout
Cancelled
```

---

# 12. Timeout

Текущая схема:

```text
enqueue
wait 5 sec
timeout
```

недостаточна.

Если caller получил:

```text
Timeout
```

команда не должна потом неожиданно изменить состояние Board.

Необходимо реализовать один из вариантов:

### Вариант A — cancellation

Команда имеет:

```cpp
cancelled
```

и Emulation Thread проверяет cancellation до выполнения.

### Вариант B — command ownership + guaranteed completion

Caller после timeout не считает операцию окончательно отменённой, а command гарантированно завершается позже и его результат сохраняется/доставляется.

Для state-changing commands предпочтителен вариант A.

---

# 13. Trace timeout

Особое внимание `ExecuteTrace`.

Если trace имеет ограничение:

```text maxInstructions
timeout
```

после достижения ограничения Emulation Thread должен:

```text
остановить trace
сформировать TraceResult
установить ExitReason::Timeout
вернуть результат
```

а не продолжить выполнять функцию после того, как Agent получил timeout.

---

# 14. `executeTraceInternal()`

Функция должна существовать только как internal Emulation Thread operation.

Она не должна быть доступна Agent через synchronous direct-call API.

Допустим:

```cpp
requestExecuteTrace(...)
```

и внутри Backend:

```cpp
executeTraceInternal(...)
```

Но запрещено:

```cpp
Agent → executeTraceInternal()
```

или:

```cpp
Agent → executeTrace()
```

который напрямую запускает CPU.

---

# 15. Concurrent Trace

Два запроса:

```text
Agent A → traceFunction(A)
Agent B → traceFunction(B)
```

не должны одновременно изменять CPU state.

Допустимы два поведения:

### Serialization

```text
Trace A
   ↓
Trace B
```

### Busy

```text
Trace A → executing
Trace B → Busy
```

Но недопустимо:

```text
Trace A + Trace B → simultaneous CPU execution
```

---

# 16. Real threaded trace test

Текущий тест:

```text
requestExecuteTrace()
→ direct execution without emulation thread
```

не считать достаточным.

Создать настоящий тест:

```text
Test Thread / Agent Thread
          ↓
requestExecuteTrace()
          ↓
Command Queue
          ↓
Emulation Thread
          ↓
executeTraceInternal()
          ↓
real Board
```

Тест должен проверять, что CPU действительно исполняется в Emulation Thread.

---

# 17. Как доказать поток исполнения

Добавить диагностический механизм только для тестов, например:

```cpp
std::thread::id executionThreadId;
```

либо callback/test hook.

Проверить:

```text
Agent thread ID != Emulation thread ID
CPU execution thread ID == Emulation thread ID
```

Не добавлять production logging ради этого.

---

# 18. Real concurrent command test

Создать минимум два Agent threads:

```text
Thread A → requestAddBreakpoint(0x0200)
Thread B → requestAddBreakpoint(0x0300)
```

После завершения:

```text
0x0200 exists
0x0300 exists
```

и каждый caller получает собственный корректный `CommandResult`.

Повторить для двух разных command types, например:

```text
breakpoint + annotation
```

---

# 19. Real concurrent trace policy test

Проверить:

```text
Thread A → traceFunction(0x0200)
Thread B → traceFunction(0x0300)
```

На одном Board.

Ожидаемый результат должен быть детерминированным:

```text
serialized
```

или:

```text
second → Busy
```

Зафиксировать выбранную policy в тесте.

---

# 20. Commands while Running

Проверить реальный Emulation Thread.

Во время:

```text
Running
```

должны корректно работать:

```text
Pause
Breakpoint mutation
Quit
```

и остальные допустимые commands.

Не допускается зависание из-за:

```cpp
if (running_)
    return;
```

или отсутствия polling Command Queue.

---

# 21. Pause semantics

Проверить сценарий:

```text
Run
 ↓
CPU executes
 ↓
Agent → requestPause()
 ↓
Emulation Thread observes request
 ↓
state = Paused
 ↓
future completes
```

Проверить, что после completion:

```text
running == false
```

и CPU больше не исполняется.

---

# 22. Quit semantics

Проверить:

```text
Run
 ↓
Agent → requestQuit()
 ↓
Emulation Thread exits
 ↓
future(s) complete
 ↓
thread joins
```

Не должно быть:

* deadlock;
* dangling promise;
* command lost;
* thread продолжается после shutdown.

---

# 23. IO attribution test

Текущий тест с названием `io_attribution` фактически не проверяет attribution.

Создать реальный ROM:

```text
function:
    OUT port
    RET
```

И trace должен содержать:

```text
PC
port
direction
value
```

Проверить, что IO event находится внутри исследуемого trace.

Добавить IO до и после функции и убедиться, что они не попадают в результат.

---

# 24. Memory attribution

Сохранить существующий подход:

```text
InstructionEvent
    ↓
sequence
    ↓
PC
```

Проверить:

```text
memory write before function
memory write inside function
memory write after function
```

В `TraceResult` должен остаться только middle event.

---

# 25. VRAM attribution

Аналогично:

```text
VRAM write before
VRAM write inside
VRAM write after
```

Только write внутри trace должен попасть в:

```text
TraceResult.vramWrites
```

---

# 26. Unknown PC

Исправить attribution:

Запрещено:

```cpp
uint16_t pc = 0;
```

как значение Unknown.

Использовать:

```text
optional<uint16_t>
```

или:

```text
hasPc
```

или другой однозначный механизм.

Обязательно добавить тест:

```text
event without matching InstructionEvent
    ↓
hasPc == false
```

и отдельно:

```text
real event at PC=0x0000
    ↓
hasPc == true
PC == 0x0000
```

Таким образом `0000` и Unknown различаются.

---

# 27. SymbolDatabase

Не расширять этот этап.

Допустимо временно оставить:

```cpp
const SymbolDatabase&
```

для read-only операций Agent.

Но запрещено возвращать mutable reference для Agent mutation.

Все изменения SymbolDatabase — только через Command Queue.

---

# 28. Test architecture

Mock tests сохранить.

Но они не должны считаться доказательством thread isolation.

Разделить тесты:

```text
Unit:
AgentApi → MockBackend

Integration:
AgentApi → DebugBackend → DebugAdapter → Board

Threaded Integration:
AgentApi → DebugBackend → CommandQueue → EmulationThread → Board
```

---

# 29. Mandatory tests

Добавить минимум:

1. command A/B results are not mixed;
2. concurrent breakpoint commands on real Backend;
3. concurrent annotation commands on real Backend;
4. CPU executes only on Emulation Thread;
5. step executes only on Emulation Thread;
6. trace executes only on Emulation Thread;
7. commands while Running;
8. Pause while Running;
9. Quit while Running;
10. trace timeout/cancellation;
11. concurrent trace policy;
12. real threaded trace;
13. IO attribution;
14. Memory attribution;
15. VRAM attribution;
16. Unknown PC;
17. PC=0000 is not Unknown;
18. no pending promise after Quit;
19. no command lost during shutdown;
20. OFF build isolation.

---

# 30. Проверка исходного кода

После реализации обязательно выполнить поиск по production-коду и проверить все места:

```text
stepInstruction(
executeTraceInternal(
target_->getCpuState(
target_->stepInstruction(
target_->write
symbols_.add
symbols_.rename
symbols_.setComment
```

Каждый вызов должен находиться либо:

```text
Emulation Thread
```

либо в явно обозначенном test-only коде.

Agent Thread не должен иметь прямого execution path к CPU.

---

# 31. Regression

### ENABLE_AI_AGENT=OFF

Все существующие debugger tests должны проходить.

Наличие Agent не должно влиять на:

* DebugBackend;
* GUI;
* emulator;
* debugger startup.

### ENABLE_AI_AGENT=ON

Все существующие tests + все Agent tests должны проходить.

Если имеются старые flaky/timing tests, необходимо явно указать их и показать, что они воспроизводятся одинаково до и после Stage 5.3.3.

Нельзя автоматически объявлять failure «pre-existing» без подтверждения.

---

# 32. Запрещено

В этом этапе не делать:

* MCP;
* LLM;
* Ollama;
* Qwen;
* OpenAI;
* Claude;
* HTTP AI API;
* prompts;
* RAG;
* vector database;
* новые Agent capabilities;
* GUI changes.

---

# 33. Критерий ACCEPT

Stage 5.3.3 принимается только если:

### Command Queue

* отсутствует `pendingGenericCommand_`;
* отсутствует `pendingTracePromise_`;
* каждая команда является самостоятельным объектом;
* команды не теряются;
* результаты не смешиваются.

### Thread ownership

```text
Agent Thread → commands only
GUI Thread   → commands/snapshots only
Emulation Thread → Board/CPU
```

### Trace

`traceFunction()` проходит через:

```text
Command Queue
→ Emulation Thread
→ executeTraceInternal()
```

### Synchronization

Нет data race на:

```text
state
pause
quit
trace
command queue
```

### Timeout

Timeout не приводит к последующему неожиданному изменению Board.

### Integration

Есть реальные threaded tests через:

```text
AgentApi
→ DebugBackend
→ DebugAdapter
→ Board
```

### Attribution

Memory / IO / VRAM события корректно относятся к trace interval.

### Unknown

Unknown PC не превращается в `0x0000`.

### Regression

OFF и ON builds проходят согласно указанным требованиям.

---

# 34. Итоговая архитектура

После Stage 5.3.3 должно быть:

```text
                 ┌─────────────┐
GUI Thread ─────►│             │
                 │ DebugBackend│
Agent Thread ───►│             │
                 └──────┬──────┘
                        │
                   Command Queue
                        │
                        ▼
                ┌─────────────────┐
                │ Emulation Thread│
                │                 │
                │ executeCommand  │
                │ executeStep     │
                │ executeTrace    │
                │ Board           │
                │ CPU             │
                └─────────────────┘
```

Ключевой инвариант:

> **Никакой Agent/GUI API не должен иметь пути, позволяющего выполнить инструкцию CPU или изменить состояние Board непосредственно из вызывающего потока.**

После выполнения этого этапа можно считать **инструментальный слой Agent API архитектурно готовым для MCP**, и только после отдельного финального ревью переходить к MCP.
