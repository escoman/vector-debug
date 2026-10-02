# Stage 5.3.3.1 — Command Queue и строгая изоляция Emulation Thread: Fixup

## 1. Цель

Довести Stage 5.3.3 до полного соответствия архитектурному инварианту:

> **Все операции, изменяющие состояние CPU/Board/Memory/IO/VRAM, выполняются только в Emulation Thread.**

Исправить оставшиеся проблемы в реализации `c7ba9b8`, не меняя архитектуру Agent API и не добавляя MCP/LLM.

---

# 2. Главный инвариант

В production-коде:

```text
GUI Thread
Agent Thread
Other Threads
      │
      ▼
 Command Queue
      │
      ▼
Emulation Thread
      │
      ▼
 Board / CPU / Memory / IO / TV
```

Ни один внешний поток не должен напрямую вызывать:

* `Board::...`, изменяющий состояние;
* `target_->stepInstruction()`;
* `target_->reset()`;
* `target_->executeFrame()`;
* `executeCommand()`;
* `executeTraceInternal()`;
* другие state-changing операции DebugTarget.

Исключение допускается только для thread-safe atomic сигналов управления:

* request pause/break;
* request quit.

---

# 3. Убрать production direct-execution fallback

## 3.1. Проблема

Сейчас `submitAndWait()` имеет путь:

```cpp
if (!emulationLoopRunning_) {
    executeCommand(*cmd);
}
```

А `requestExecuteTrace()` имеет аналогичный прямой вызов:

```cpp
executeTraceInternal(...);
```

Это нарушает строгую изоляцию.

## 3.2. Требование

Убрать все production-вызовы:

```cpp
executeCommand(...)
executeTraceInternal(...)
stepInstruction(...)
target_->...
```

из вызывающего потока.

`submitAndWait()` должен всегда помещать command в очередь.

## 3.3. Жизненный цикл

Если Emulation Thread ещё не запущен, необходимо определить единый способ его запуска.

Предпочтительный вариант:

```text
DebugBackend construction
        ↓
start emulation loop
        ↓
Emulation Thread ждёт Command Queue
        ↓
commands выполняются
```

При этом обычный debugger/GUI/Agent API не должен знать, запущен ли loop.

Если для unit-тестов необходим синхронный executor, он должен быть отдельным тестовым механизмом и не использоваться production API.

---

# 4. Step

`requestStep()` должен всегда работать через Command Queue.

Запрещено:

```cpp
if (!emulationLoopRunning_)
    stepInstruction();
```

Нужна схема:

```text
requestStep()
    ↓
Command::Step
    ↓
Queue
    ↓
Emulation Thread
    ↓
executeStepInternal()
    ↓
target_->stepInstruction()
    ↓
promise result
```

`executeStepInternal()` должен быть private и вызываться только из Emulation Thread.

---

# 5. Run и Breakpoint

`requestRun()` не должен выполнять:

```cpp
checkBreakpoint()
```

в вызывающем потоке.

Breakpoint проверяется только внутри Emulation Thread непосредственно перед выполнением очередной инструкции.

Схема:

```text
requestRun()
    ↓
Command::Run
    ↓
Queue
    ↓
Emulation Thread
    ↓
checkBreakpoint()
    ↓
execute frame / instruction
```

Особенно важно не создавать состояние:

```text
Agent Thread → checkBreakpoint() → Board
```

---

# 6. Pause

## 6.1. Определить семантику

`Pause` должен быть **emergency interrupt**, а не обычной queued operation.

Это позволяет остановить выполнение текущего длинного кадра/trace без ожидания обработки очереди.

Использовать:

```cpp
std::atomic<bool> pauseRequestedAtomic_;
```

Запрещено использовать обычный `bool pauseRequested_` одновременно с atomic-версией.

Удалить `pauseRequested_`, если он больше не нужен.

## 6.2. requestPause()

`requestPause()`:

```cpp
pauseRequestedAtomic_.store(true, std::memory_order_release);
breakRequested_.store(true, std::memory_order_release);
```

Не должен обращаться к Board/CPU.

## 6.3. Emulation Thread

Emulation Thread периодически проверяет:

```cpp
pauseRequestedAtomic_.load(std::memory_order_acquire)
```

и только он изменяет:

```text
running_
state_
debugger attach/detach
```

После обработки:

```cpp
pauseRequestedAtomic_.store(false, ...)
```

---

# 7. Quit

Quit должен иметь однозначную семантику.

## 7.1. requestQuit()

`requestQuit()`:

1. создаёт `Command::Quit`;
2. помещает его в очередь;
3. устанавливает emergency atomic signal, если это необходимо для выхода из текущего execution loop.

Не должен напрямую изменять Board.

## 7.2. Emulation Thread

Emulation Thread:

```text
Quit command
    ↓
stop execution
    ↓
set running = false
    ↓
detach debugger
    ↓
fulfill Quit promise
    ↓
drain remaining queue
    ↓
fulfill all remaining promises
    ↓
exit loop
```

Каждый command, оставшийся в очереди, обязан получить результат:

```text
Cancelled
```

или другой явно определённый terminal status.

**Ни один promise не должен остаться неисполненным.**

---

# 8. Command lifecycle и timeout

Текущий механизм:

```cpp
cancelled = true
```

недостаточен, если command уже начал выполняться.

Необходимо ввести явное состояние command:

```text
Queued
   ↓
Executing
   ↓
Completed
```

или эквивалентный механизм.

## 8.1. Правило timeout

Если command ещё `Queued`:

```text
timeout
  ↓
cancel
  ↓
command гарантированно НЕ выполняется
```

Если command уже `Executing`:

```text
command уже выполняется
  ↓
caller не должен считать, что operation отменена
```

Предпочтительно не возвращать timeout для уже выполняющегося короткого state-changing command.

## 8.2. Запрещено

Нельзя получить:

```text
Agent:
    requestMemoryWrite()
    ↓
    timeout
    ↓
    считает operation отменённой

Emulation Thread:
    позже выполняет MemoryWrite
```

Это недопустимо.

---

# 9. ExecuteTrace

`requestExecuteTrace()` должен использовать promise, принадлежащий **конкретному Command**.

Запрещено наличие глобального:

```cpp
pendingTracePromise_
```

или аналогичного single-slot механизма.

Структура должна концептуально выглядеть так:

```cpp
struct Command {
    CommandType type;
    ...
    std::promise<CommandResult> promise;
    std::promise<TraceResult> tracePromise;
};
```

либо через единый typed result.

## 9.1. Выполнение

Только:

```text
Agent Thread
    ↓
requestExecuteTrace()
    ↓
Command Queue
    ↓
Emulation Thread
    ↓
executeTraceInternal()
    ↓
TraceResult
    ↓
promise
```

## 9.2. Concurrent Trace

Два одновременно поступивших trace-запроса не должны:

* перезаписывать promise друг друга;
* терять результат;
* обращаться к CPU одновременно.

Допустимо:

```text
Trace A → Queue
Trace B → Queue

A executes
B waits

A result
B executes
```

или возвращать `Busy`, если это явно реализовано.

Предпочтительно последовательное выполнение через queue.

---

# 10. Trace cancellation

`executeTraceInternal()` должен регулярно проверять:

```cpp
breakRequested_
pauseRequestedAtomic_
quitRequested_
```

При запросе Pause:

```text
Trace → остановлен → Paused
```

При Quit:

```text
Trace → остановлен → shutdown
```

Trace не должен продолжать выполнять инструкции после подтверждённого quit/pause.

---

# 11. AgentApi::traceFunction()

Удалить polling:

```cpp
while (!backend_.isPaused()) {
    std::this_thread::sleep_for(...);
}
```

Запрещены:

* `sleep_for`;
* polling `isPaused()`;
* искусственные задержки;
* busy wait.

`traceFunction()` должен ждать результат:

```text
requestExecuteTrace(params)
        ↓
future.get()
```

или использовать другой уже существующий механизм completion.

Agent API не должен самостоятельно синхронизироваться с Emulation Thread через polling.

---

# 12. Unknown PC

Сохранить исправление `hasPc`.

Обязательное различие:

```text
PC = 0000
```

и

```text
PC неизвестен
```

не должны представляться одинаково.

Использовать:

```cpp
bool hasPc;
```

или эквивалент:

```cpp
std::optional<uint16_t> pc;
```

При отсутствии PC:

```text
hasPc = false
```

Запрещено автоматически подставлять:

```cpp
pc = 0;
```

для неизвестного значения.

---

# 13. SymbolDatabase

Не расширять Stage 5.3.3.1.

Текущее правило сохранить:

```text
Agent
  ↓
DebugBackend command
  ↓
Emulation Thread
  ↓
SymbolDatabase mutation
```

Если `SymbolDatabase&` остаётся публичным для GUI, это должно быть явно документировано как архитектурное исключение для существующего GUI-кода.

Agent API не должен напрямую изменять SymbolDatabase.

---

# 14. Обязательные интеграционные тесты

## 14.1. Реальный threaded Step

Тест должен доказать именно thread isolation.

Зафиксировать:

```cpp
emulationThreadId
callerThreadId
```

и убедиться:

```text
callerThreadId != emulationThreadId
```

При выполнении CPU step внутри target зафиксировать:

```cpp
actualExecutionThreadId
```

Проверить:

```text
actualExecutionThreadId == emulationThreadId
```

Тест, который только проверяет изменение PC/history, недостаточен.

---

# 15. Реальный threaded ExecuteTrace

Тест должен:

1. создать реальный `DebugBackend`;
2. запустить Emulation Thread;
3. вызвать `AgentApi::traceFunction()` из другого thread;
4. выполнить реальный trace;
5. проверить thread IDs;
6. получить `TraceResult`;
7. проверить PC sequence.

Нельзя считать тестом threaded integration вариант, где trace выполняется через direct fallback.

---

# 16. Concurrent commands

Создать реальный `DebugBackend`.

Из нескольких threads одновременно отправить:

```text
Step
MemoryWrite
RegisterWrite
Breakpoint
ExecuteTrace
```

Проверить:

* нет crash;
* нет deadlock;
* нет потерянных promises;
* нет повреждения результатов;
* CPU/Board изменяется только Emulation Thread.

MockBackend недостаточен — должен быть хотя бы один реальный threaded integration test.

---

# 17. Quit с pending commands

Нужен настоящий сценарий:

```text
Thread A:
    отправляет command
    ждёт future

Thread B:
    requestQuit()
```

Команда должна получить terminal result:

```text
Completed
```

или

```text
Cancelled
```

но **никогда не остаться pending**.

Особенно важно проверить несколько команд в очереди:

```text
Command A
Command B
Command C
Quit
Command D
Command E
```

После Quit:

```text
A/B/C → Completed или корректно прерваны
Quit → Completed
D/E → Cancelled
```

Ни один future не должен зависнуть.

---

# 18. Timeout test

Создать ситуацию:

```text
Command queued
↓
caller timeout
↓
command cancelled
↓
Emulation Thread начинает обработку
```

Проверить, что отменённая queued-команда **не выполняется**.

Отдельно проверить ситуацию:

```text
Command already Executing
↓
caller waits
```

и убедиться, что состояние command не переходит обратно в Cancelled.

---

# 19. IO attribution

Текущий тест недостаточен, если он только проверяет, что trace завершился.

Создать реальный код с инструкцией:

```text
IN
```

или

```text
OUT
```

и проверить:

```text
TraceResult
    ↓
IO event
    ↓
correct sequence number
    ↓
correct PC
    ↓
correct port
```

Должен проверяться именно IO event, а не только факт успешного trace.

---

# 20. Memory / VRAM attribution

Для Memory:

```text
instruction
    ↓
memory read/write
    ↓
TraceResult
    ↓
correct sequence
    ↓
correct PC
    ↓
correct address
```

Для VRAM:

```text
instruction
    ↓
write C000/E000/... 
    ↓
VRAM event
    ↓
correct sequence
    ↓
correct PC
```

Нельзя просто проверять глобальную history после завершения trace.

---

# 21. Source-code audit

После исправлений выполнить поиск по production-коду debugger.

Проверить отсутствие прямых вызовов state-changing target methods из:

* Agent Thread;
* GUI Thread;
* request API;
* helper threads.

Особое внимание:

```text
target_->stepInstruction
target_->reset
target_->executeFrame
executeCommand
executeTraceInternal
checkBreakpoint
```

Каждый вызов должен находиться либо:

* внутри Emulation Thread execution path;
* либо в специально обозначенном тестовом executor, не используемом production.

---

# 22. Thread-safety audit

Проверить все shared state:

```text
running_
state_
breakRequested_
pauseRequested_
quitRequested_
traceBusy_
command queue
```

Для каждого определить владельца:

### Emulation Thread ownership

```text
Board
CPU
Memory
IO
TV
running state
debugger state
```

### Atomic cross-thread signals

```text
breakRequested_
pauseRequestedAtomic_
quitRequested_
```

### Mutex-protected data

```text
CommandQueue
other shared snapshots/state
```

Не должно остаться data race на обычных `bool`, enum или integer, одновременно читаемых и записываемых разными threads.

---

# 23. Тестирование

Обязательно:

### ENABLE_AI_AGENT=ON

Запустить:

```text
test_backend
test_agent_api
test_agent_commands
test_agent_mock
test_agent_integration
```

Все новые тесты должны проходить.

### ENABLE_AI_AGENT=OFF

Проверить полный regression suite.

Допустимы только уже известные:

```text
2 pre-existing timing failures
```

Новые ошибки недопустимы.

---

# 24. Запрещённые изменения

В рамках Stage 5.3.3.1 НЕ делать:

* MCP;
* LLM;
* prompt system;
* AI planning;
* новые Agent tools;
* новые GUI windows;
* изменение GUI architecture;
* изменение Vector emulator core без необходимости;
* рефакторинг несвязанных компонентов;
* изменение существующего debugger API без необходимости.

Цель — только завершение command queue/thread isolation.

---

# 25. Acceptance Criteria

Stage 5.3.3.1 считается выполненным только если:

* [ ] нет production direct-execution fallback;
* [ ] Step всегда проходит через Emulation Thread;
* [ ] Run всегда проходит через Command Queue;
* [ ] breakpoint check выполняется только Emulation Thread;
* [ ] Pause не имеет data race;
* [ ] Quit имеет однозначную семантику;
* [ ] все pending promises гарантированно завершаются;
* [ ] timeout не приводит к позднему неожиданному выполнению queued command;
* [ ] ExecuteTrace использует per-command promise;
* [ ] concurrent traces безопасны;
* [ ] trace cancellation работает;
* [ ] `traceFunction()` не использует polling/sleep;
* [ ] Unknown PC корректно отличается от PC=0000;
* [ ] есть реальный threaded Step test;
* [ ] есть реальный threaded ExecuteTrace test;
* [ ] есть реальный concurrent command test;
* [ ] есть настоящий IO attribution test;
* [ ] есть Memory attribution test;
* [ ] есть VRAM attribution test;
* [ ] есть pending-command + Quit test;
* [ ] есть timeout/cancellation test;
* [ ] production source audit пройден;
* [ ] thread-safety audit пройден;
* [ ] `ENABLE_AI_AGENT=ON` regression пройден;
* [ ] `ENABLE_AI_AGENT=OFF` regression пройден;
* [ ] MCP/LLM не добавлялись.

## Итоговая архитектура

```text
                 ┌─────────────────┐
                 │    GUI Thread   │
                 └────────┬────────┘
                          │
                 ┌────────▼────────┐
                 │   Agent Thread  │
                 └────────┬────────┘
                          │
                          ▼
                 ┌─────────────────┐
                 │  Command Queue  │
                 │ mutex + CV      │
                 └────────┬────────┘
                          │
                          ▼
              ┌────────────────────────┐
              │    Emulation Thread    │
              │                        │
              │ executeCommand()       │
              │ executeTraceInternal() │
              │ executeStepInternal()  │
              │ checkBreakpoint()      │
              └───────────┬────────────┘
                          │
                          ▼
                 ┌─────────────────┐
                 │ IDebugTarget    │
                 │ DebugAdapter    │
                 │ Board / CPU     │
                 └─────────────────┘

Atomic signals:
    Pause ───────────────► Emulation Thread
    Break ───────────────► Emulation Thread
    Quit ────────────────► Emulation Thread
```

**Главный критерий:** внешний поток может только **сформировать запрос**. Выполнение запроса над эмулируемой машиной всегда принадлежит Emulation Thread.
