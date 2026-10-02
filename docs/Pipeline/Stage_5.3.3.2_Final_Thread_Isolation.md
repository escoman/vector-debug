# Stage 5.3.3.2 — Финальный fixup Command Queue и Thread Isolation

## 1. Цель

Устранить оставшиеся архитектурные проблемы после `56bf2b2` и завершить Stage 5.3.3.1.

Цель — получить строгую модель:

> Внешний поток только формирует запрос. Все изменения состояния эмулируемой машины и debugger execution state выполняются Emulation Thread.

Не добавлять MCP, LLM, новые Agent API или GUI-функции.

---

# 2. Убрать polling из AgentApi::traceFunction()

Текущая схема:

```cpp
while (!backend_.isPaused() && waitLimit-- > 0) {
    std::this_thread::yield();
}
```

недопустима.

`yield()` является таким же polling-механизмом, как `sleep_for()`.

## Требование

`traceFunction()` должен ждать непосредственно completion соответствующей операции.

Предпочтительная схема:

```text
Agent Thread
    │
    ├── requestExecuteTrace()
    │
    ▼
Command Queue
    │
    ▼
Emulation Thread
    │
    ├── executeTraceInternal()
    │
    ▼
TraceResult / CommandResult
    │
    ▼
future.get()
    │
    ▼
Agent Thread
```

Не использовать:

* `sleep_for`;
* `yield` в цикле ожидания;
* `isPaused()` polling;
* таймерное ожидание состояния;
* busy wait.

Если trace был остановлен Pause/Break/Breakpoint/Quit, `future` всё равно должен завершиться определённым `TraceResult`/`CommandResult`.

---

# 3. `requestPause()` не должен менять execution state

Сейчас внешний поток изменяет:

```cpp
running_.store(false);
```

Это нарушает ownership модели.

## Требование

`requestPause()` может устанавливать только cross-thread atomic signal:

```cpp
pauseRequestedAtomic_.store(true, std::memory_order_release);
breakRequested_.store(true, std::memory_order_release);
```

Он НЕ должен изменять:

```text
running_
state_
Board
CPU
Memory
```

Emulation Thread после обнаружения pause выполняет:

```text
pause requested
    ↓
running = false
    ↓
state = Paused
    ↓
clear pause request
```

Таким образом:

```text
requestPause()
    = request/signal

Emulation Thread
    = actual state transition
```

---

# 4. `requestReset()` не должен менять execution state

Сейчас `requestReset()` предварительно устанавливает:

```cpp
running_.store(false);
```

Это также удалить.

Правильная схема:

```text
requestReset()
    ↓
enqueue Reset
    ↓
Emulation Thread
    ↓
running = false
    ↓
target_->reset()
    ↓
state = Paused
    ↓
complete promise
```

Если Reset должен прервать текущий execution loop, разрешается установить atomic break/reset signal, но само изменение `running_` выполняется только Emulation Thread.

---

# 5. Command State Machine

Текущего `cancelled` недостаточно для строгой синхронизации.

Ввести явное состояние command:

```cpp
enum class CommandState {
    Queued,
    Executing,
    Cancelled,
    Completed
};
```

или полностью эквивалентную модель.

## 5.1. Обязательные переходы

Разрешены только:

```text
Queued → Executing
Queued → Cancelled
Executing → Completed
```

Для аварийно завершённых операций допускается явно определённый дополнительный terminal state.

Запрещены:

```text
Executing → Cancelled
Completed → Cancelled
Completed → Executing
Cancelled → Executing
```

## 5.2. Атомарность

Переход:

```text
Queued → Executing
```

и отмена:

```text
Queued → Cancelled
```

должны быть взаимно исключающими.

Предпочтительно использовать:

```cpp
compare_exchange
```

на atomic command state.

Пример:

```text
Caller:
    Queued → Cancelled

Emulation Thread:
    Queued → Executing
```

Только один из переходов должен победить.

---

# 6. Timeout semantics

Timeout вызывающего потока не должен создавать ложное состояние:

```text
caller thinks command cancelled
        +
emulation thread executes it later
```

## Если command ещё Queued

При timeout:

```text
Queued → Cancelled
```

После этого Emulation Thread обязан пропустить command без выполнения.

Promise должен получить terminal result.

## Если command уже Executing

Отменять его нельзя:

```text
Executing → Cancelled
```

запрещено.

Caller должен либо:

* дождаться completion;
* либо получить специальный статус `TimeoutExecuting`, если такая семантика необходима.

Но нельзя сообщать:

```text
Cancelled
```

для уже выполняющегося command.

---

# 7. `submitAndWait()`

`submitAndWait()` должен:

1. создать Command;
2. получить future;
3. поместить Command в queue;
4. ждать completion;
5. при timeout попытаться выполнить атомарную отмену;
6. вернуть `Timeout` только если `Queued → Cancelled` действительно состоялся.

Критически важно:

```text
timeout
  ↓
CAS(Queued, Cancelled)
  ↓
успешно → command НЕ выполняется
```

Если CAS неуспешен, значит Emulation Thread уже взял command.

В этом случае нельзя выдавать `Cancelled`.

---

# 8. Единственный механизм защиты `state_`

Сейчас `state_` в разных местах защищается разными mutex.

Это недопустимо.

Выбрать один механизм.

Предпочтительный вариант:

```cpp
stateMutex_
```

Использовать его для:

```text
state_
```

во всех местах, где нужен mutex.

## Требование

Все записи:

```cpp
state_ = ...
```

и соответствующие чтения должны использовать один и тот же synchronization mechanism.

Не должно быть:

```text
commandMutex_ → write state_
stateMutex_   → read state_
```

## `isPaused()`

`isPaused()` не должен читать обычный `state_` без синхронизации.

Варианты:

### Вариант A — mutex

```cpp
std::lock_guard<std::mutex> lock(stateMutex_);
return state_ == DebuggerState::Paused;
```

### Вариант B — atomic state

Сделать `state_` atomic, если это не создаёт лишних архитектурных проблем.

Предпочтительнее не менять модель без необходимости и использовать существующий `stateMutex_`.

---

# 9. `running_`

`running_` остаётся atomic для чтения execution loop, если это необходимо.

Но ownership должен быть следующим:

### Внешние threads

могут:

```text
read running_
```

но не должны самостоятельно выполнять:

```text
Running → Paused
Running → Reset
Running → Quit
```

### Emulation Thread

единственный владелец переходов:

```text
running = true
running = false
```

Исключение — только если конкретный atomic signal является частью documented emergency protocol. В текущей архитектуре это не требуется.

---

# 10. Pause protocol

Полная схема:

```text
Agent/GUI Thread
      │
      │ requestPause()
      ▼
pauseRequestedAtomic = true
breakRequested = true
      │
      ▼
Emulation Thread
      │
      ├── detects request
      │
      ├── stops execution
      │
      ├── running = false
      │
      ├── state = Paused
      │
      └── pauseRequestedAtomic = false
```

Не должно быть:

```text
Agent Thread → running = false
```

---

# 11. Reset protocol

Полная схема:

```text
Agent/GUI Thread
      │
      ▼
Command::Reset
      │
      ▼
Command Queue
      │
      ▼
Emulation Thread
      │
      ├── stop execution
      ├── target_->reset()
      ├── update state
      └── fulfill promise
```

Все операции над `Board` и CPU выполняются внутри Emulation Thread.

---

# 12. Production direct execution audit

Повторно проверить отсутствие production fallback:

```cpp
if (!emulationLoopRunning_) {
    executeCommand(...);
}
```

и:

```cpp
executeTraceInternal(...)
```

из Agent/caller thread.

Разрешённый механизм:

```text
Production:
    always Queue → Emulation Thread

Tests:
    optional test-only synchronous executor
```

`testSynchronous_` не должен случайно использоваться production-кодом.

Желательно сделать его:

* явно test-only;
* недоступным через публичный production API;
* либо пометить/document как testing hook.

---

# 13. Test-only synchronous execution

Синхронный режим допускается только для unit tests, которым не нужен настоящий Emulation Thread.

Он не должен использоваться для доказательства thread isolation.

Поэтому:

```text
testSynchronous_ = true
```

не считается доказательством выполнения на Emulation Thread.

Для таких доказательств обязательны реальные threaded integration tests.

---

# 14. Thread isolation test: Step

Тест должен иметь три идентификатора:

```text
callerThreadId
emulationThreadId
actualCpuExecutionThreadId
```

Проверить:

```text
callerThreadId != emulationThreadId

actualCpuExecutionThreadId == emulationThreadId
```

Нельзя считать тест достаточным только по:

* изменению PC;
* появлению history;
* успешному result.

---

# 15. Thread isolation test: ExecuteTrace

Обязательная схема:

```text
Agent Thread
    ↓
AgentApi::traceFunction()
    ↓
requestExecuteTrace()
    ↓
Command Queue
    ↓
Emulation Thread
    ↓
executeTraceInternal()
```

Тест должен доказать:

```text
trace caller thread != emulation thread
trace CPU execution thread == emulation thread
```

Также проверить корректный `TraceResult`.

---

# 16. IO attribution test

Тест должен реально исполнять:

```text
IN port
```

или:

```text
OUT port
```

и проверять в TraceResult:

```text
PC
port
direction
value
sequence
```

Проверка только успешного завершения trace недостаточна.

---

# 17. VRAM attribution test

Реально выполнить запись в VRAM, например:

```text
C000
```

и проверить:

```text
address
value
PC
sequence
VRAM classification
```

Проверять только глобальную Memory History недостаточно.

---

# 18. Quit with pending commands

Создать реальную многопоточную ситуацию:

```text
Thread A:
    enqueue command A
    wait future A

Thread B:
    enqueue command B
    wait future B

Thread C:
    requestQuit()
```

После завершения:

```text
A → Completed или корректно Cancelled
B → Completed или корректно Cancelled
Quit → Completed
```

Все promises должны завершиться.

Не должно быть зависшего:

```text
future.wait()
```

---

# 19. Pending queue after Quit

Проверить очередь:

```text
A
B
C
Quit
D
E
```

После Quit:

```text
A/B/C → обработаны или корректно завершены
Quit  → Completed
D/E   → Cancelled
```

Ни один command не должен остаться без terminal state.

---

# 20. Timeout test

Обязательный сценарий:

```text
Command
    ↓
Queued
    ↓
caller timeout
    ↓
CAS Queued → Cancelled
```

Проверить, что command не выполняется.

Отдельно проверить race:

```text
Queued
   ├── caller → Cancelled
   └── emulation → Executing
```

Только один переход должен победить.

---

# 21. Trace cancellation

Проверить:

```text
Trace running
    ↓
requestPause()
    ↓
trace stops
    ↓
future completes
```

И отдельно:

```text
Trace running
    ↓
requestQuit()
    ↓
trace stops
    ↓
future completes
    ↓
emulation loop exits
```

Не должно быть зависшего trace future.

---

# 22. Запрет polling в Agent API

После исправлений выполнить поиск:

```text
sleep_for
yield
isPaused()
while (...wait...)
```

в `agent/`.

Допустимы обычные циклы алгоритма обработки данных.

Недопустимы циклы, предназначенные для ожидания состояния Emulation Thread.

---

# 23. Thread-safety audit

Повторно проверить:

```text
state_
running_
breakRequested_
pauseRequestedAtomic_
quitRequested_
traceBusy_
CommandState
CommandQueue
```

Для каждого должна быть явно определена модель:

```text
Emulation Thread ownership
Atomic
Mutex protected
Immutable snapshot
```

Не должно быть одновременно:

```text
plain variable + unsynchronized cross-thread access
```

---

# 24. Source audit

В production-коде найти все:

```text
target_->stepInstruction
target_->reset
target_->executeFrame
executeCommand
executeTraceInternal
checkBreakpoint
```

Для каждого подтвердить:

```text
Emulation Thread
```

или явно обозначенный test-only path.

Особое внимание:

```text
AgentApi
request*
submitAndWait
GUI callbacks
worker threads
```

---

# 25. Regression

Запустить:

### `ENABLE_AI_AGENT=ON`

```text
test_backend
test_agent_api
test_agent_commands
test_agent_mock
test_agent_integration
```

Все новые тесты должны проходить.

Разрешены только два ранее известных timing failures в `test_backend`.

### `ENABLE_AI_AGENT=OFF`

Запустить полный regression suite:

```text
test_backend
test_symbol_database
test_vram_mapping
test_workspace
test_live_map
test_board_smoke
test_gui_smoke
```

Новых failures быть не должно.

---

# 26. Не менять

В Stage 5.3.3.2 запрещено:

* MCP;
* LLM;
* новые Agent tools;
* новые GUI windows;
* изменение Vector core;
* изменение debugger architecture;
* рефакторинг несвязанных компонентов;
* расширение Stage 5.3.3.

Это финальный технический fixup.

---

# 27. Acceptance Criteria

Stage 5.3.3.2 принимается, если:

* [ ] `traceFunction()` не использует polling;
* [ ] нет `yield()`/`sleep_for()` для ожидания Emulation Thread;
* [ ] `requestPause()` не изменяет `running_`;
* [ ] `requestReset()` не изменяет `running_`;
* [ ] `state_` защищается единым synchronization mechanism;
* [ ] `isPaused()` thread-safe;
* [ ] Command имеет атомарный lifecycle;
* [ ] `Queued → Cancelled` и `Queued → Executing` взаимоисключающие;
* [ ] timeout не приводит к позднему выполнению отменённого command;
* [ ] Executing command нельзя объявить Cancelled;
* [ ] production direct execution отсутствует;
* [ ] Step выполняется только Emulation Thread;
* [ ] Run выполняется только Emulation Thread;
* [ ] Reset выполняется только Emulation Thread;
* [ ] ExecuteTrace выполняется только Emulation Thread;
* [ ] breakpoint check выполняется только Emulation Thread;
* [ ] Quit гарантированно завершает все pending promises;
* [ ] Trace cancellation работает;
* [ ] есть реальный threaded Step test;
* [ ] есть реальный threaded ExecuteTrace test;
* [ ] есть concurrent command test;
* [ ] есть timeout race test;
* [ ] есть pending commands + Quit test;
* [ ] есть реальный IO attribution test;
* [ ] есть реальный VRAM attribution test;
* [ ] production source audit пройден;
* [ ] thread-safety audit пройден;
* [ ] `ENABLE_AI_AGENT=ON` regression пройден;
* [ ] `ENABLE_AI_AGENT=OFF` regression пройден.

## Финальный инвариант

```text
                    ┌─────────────────┐
                    │ GUI / Agent /   │
                    │ External Thread │
                    └────────┬────────┘
                             │
                             │ request only
                             ▼
                    ┌─────────────────┐
                    │  Command Queue  │
                    └────────┬────────┘
                             │
                             ▼
                 ┌────────────────────────┐
                 │    Emulation Thread    │
                 │                        │
                 │ executeCommand()       │
                 │ executeStepInternal()  │
                 │ executeTraceInternal() │
                 │ checkBreakpoint()      │
                 │                        │
                 │ Board / CPU / Memory   │
                 └────────────────────────┘

Atomic emergency signals:
    Pause / Break / Quit
             │
             └──────────────► Emulation Thread
```

**Внешний поток никогда не выполняет операцию над эмулируемой машиной. Он только создаёт запрос и получает результат.**
