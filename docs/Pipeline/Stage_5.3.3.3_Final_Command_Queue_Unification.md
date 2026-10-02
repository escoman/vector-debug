# Stage 5.3.3.3 — Финальная унификация Command Queue

## Цель

Устранить последние архитектурные проблемы Stage 5.3.3.2:

1. убрать глобальный `nextPausePromise_`;
2. обеспечить единый путь выполнения команд через CAS `Queued → Executing`;
3. сделать ожидание завершения `Run` корректным при параллельных запросах;
4. гарантировать корректное завершение `Quit`;
5. добавить настоящий тест конкурентного CAS.

Изменения выполняются только в `debugger/`. Основной эмулятор в `src/` не изменять.

---

## 1. Убрать глобальный `nextPausePromise_`

Полностью удалить из `DebugBackend`:

* `nextPauseMutex_`;
* `nextPausePromise_`;
* весь код их создания, хранения и очистки.

Нельзя использовать один глобальный promise для ожидания результата любого следующего `Run`.

### Требование

Promise, связанный с ожиданием остановки после `Run`, должен принадлежать **конкретной команде Run**.

Например, расширить `Command`:

```cpp
std::shared_ptr<std::promise<void>> pausePromise;
```

или использовать эквивалентную конструкцию, при которой promise однозначно принадлежит конкретному `Run`.

Разрешается иметь несколько одновременно существующих `Run`-команд без перезаписи promise другой командой.

---

## 2. Переделать `requestRunFuture()`

`requestRunFuture()` должен работать только через Command Queue.

Запрещено:

* выполнять `Run` непосредственно из вызывающего потока;
* создавать глобальный promise;
* использовать `std::async(std::launch::deferred)` как обход Command Queue;
* использовать polling или `sleep_for`.

### Требуемая семантика

Вызов:

```cpp
auto future = backend.requestRunFuture();
```

должен:

1. создать конкретную команду `Run`;
2. создать для неё future/promise ожидания остановки;
3. поставить команду в Command Queue;
4. вернуть future, связанный именно с этой командой;
5. future должен завершиться при переходе эмулятора из `Running` в `Paused`.

То есть:

```text
Run command #1 → pauseFuture #1
Run command #2 → pauseFuture #2
```

не должны влиять друг на друга.

Если `Run` остановлен breakpoint'ом — соответствующий future завершается.

Если `Run` остановлен `requestPause()` — соответствующий future завершается.

Если выполнение завершено из-за `Quit` — соответствующий future также должен быть корректно завершён.

---

## 3. Единый путь обработки команд

Сейчас существуют два разных механизма:

```text
processOneCommand()
executeFramesTarget_() → tryDequeue() → executeCommand()
```

Это устранить.

### Требование

Любая команда, извлечённая из `CommandQueue`, должна проходить одну общую функцию обработки.

Логика должна быть принципиально такой:

```text
dequeue
  ↓
CAS Queued → Executing
  ↓
если CAS не прошёл
  → команда уже Cancelled
  → promise завершить как Cancelled
  ↓
проверка Quit/cancellation
  ↓
executeCommand()
  ↓
Executing → Completed
  ↓
promise завершить
```

`executeFramesTarget_()` не должен самостоятельно вызывать `executeCommand()` для извлечённых команд.

---

## 4. CAS должен выполняться всегда

Единственная допустимая последовательность изменения состояния:

```text
Queued
 ├──→ Cancelled
 └──→ Executing → Completed
```

Переход:

```text
Queued → Cancelled
```

выполняется вызывающим потоком при timeout/cancellation.

Переход:

```text
Queued → Executing
```

выполняется только emulation thread.

Использовать `compare_exchange_strong()`.

Нельзя делать:

```cpp
if (cmd->state == Queued)
    cmd->state = Executing;
```

поскольку это race.

---

## 5. Устранить второй путь выполнения в executeFramesTarget_()

В `executeFramesTarget_()` удалить логику вида:

```cpp
while (auto cmd = commandQueue_.tryDequeue()) {
    ...
    executeCommand(*cmd);
}
```

Вместо этого использовать тот же механизм обработки команд, что и `processOneCommand()`.

Особенно важно:

* `Step`;
* `Reset`;
* `Load`;
* `MemoryWrite`;
* `RegisterWrite`;
* `ExecuteTrace`;
* `Run`;
* `Quit`.

не должны иметь отдельной альтернативной обработки, обходящей CAS.

---

## 6. Команда Quit

`Quit` также должна проходить через общий механизм команд.

После получения `Quit`:

1. команда переводится `Queued → Executing`;
2. устанавливается `quitRequested_`;
3. прекращается выполнение CPU;
4. `running_` переводится в `false`;
5. состояние становится `Paused`;
6. завершается promise команды;
7. оставшиеся команды в очереди переводятся в `Cancelled`;
8. их promises также завершаются;
9. emulation thread корректно выходит.

Ни один promise не должен оставаться навсегда в состоянии ожидания.

Особенно проверить случай:

```text
Run
Step
ExecuteTrace
Quit
```

и другие комбинации нескольких ожидающих команд.

---

## 7. Несколько параллельных Run

Добавить тест, запускающий несколько запросов `requestRunFuture()` из разных потоков.

Проверить:

* оба запроса проходят через очередь;
* promise одного `Run` не заменяет promise другого;
* нет deadlock;
* оба future завершаются корректно;
* CPU продолжает выполняться только в emulation thread.

Если архитектура сознательно запрещает несколько одновременно ожидающих `Run`, это должно быть явно зафиксировано в API и проверено тестом. Предпочтительный вариант — поддержать несколько запросов корректно.

---

## 8. Настоящий тест CAS race

Текущий `test_command_state_cas` недостаточен: он проверяет уже заранее отменённую команду, но не реальную конкуренцию.

Создать тест, в котором **два потока одновременно пытаются изменить одну и ту же команду**:

```text
Thread A:
    Queued → Cancelled

Thread B:
    Queued → Executing
```

Использовать `std::barrier`, `std::latch` или `condition_variable` для синхронизации старта.

Требование:

* ровно один CAS должен успешно изменить состояние `Queued`;
* второй CAS обязан завершиться неудачей;
* итоговое состояние должно быть либо `Cancelled`, либо `Executing`;
* состояние не должно повреждаться;
* невозможно получить одновременно успешные оба перехода.

Не использовать `sleep_for()` для создания race.

---

## 9. Проверить thread isolation

Сохранить и расширить существующие тесты:

* `Step`;
* `ExecuteTrace`;
* `Run`;
* `Reset`;
* `MemoryWrite`;
* `RegisterWrite`.

Для каждой операции проверить:

```text
caller thread != emulation thread
CPU/Board operation == emulation thread
```

GUI/Agent thread не должен напрямую выполнять CPU или Board mutation.

---

## 10. AgentApi::traceFunction()

После изменения `requestRunFuture()` сохранить текущую модель:

```cpp
auto pauseFuture = backend_.requestRunFuture();
pauseFuture.wait();
```

Polling через:

```cpp
yield();
sleep_for();
```

не использовать.

После завершения `Run`:

1. проверить `isPaused()`;
2. выполнить `requestExecuteTrace()`;
3. дождаться результата через future.

`traceFunction()` не должен напрямую вызывать CPU/Board API.

---

## 11. Thread-safe state

Проверить все обращения к:

* `state_`;
* `running_`;
* `quitRequested_`;
* `breakRequested_`;
* `traceBusy_`.

Для обычного состояния использовать существующий `stateMutex_`.

Для atomic-переменных не вводить дополнительную несогласованную синхронизацию.

Не должно остаться мест, где одна часть кода пишет `state_` под одним mutex, а другая читает без него или под другим mutex.

---

## 12. Регрессионные тесты

После реализации обязательно выполнить полный набор:

```text
test_backend
test_agent_api
test_agent_commands
test_agent_mock
test_agent_integration
test_symbol_database
test_vram_mapping
test_workspace
test_live_map
test_board_smoke
test_gui_smoke
```

Требования:

* новые тесты проходят;
* старые тесты не деградируют;
* отсутствуют deadlock;
* отсутствуют зависшие futures;
* отсутствуют обращения к Board/CPU из Agent/GUI thread.

Если есть pre-existing failure, явно указать его отдельно и не считать результатом Stage 5.3.3.3.

---

## 13. Что запрещено

Не делать:

* изменений в `src/`;
* нового глобального promise;
* второго command-processing path;
* прямого выполнения CPU из Agent/GUI thread;
* polling;
* `sleep_for()` в production-коде;
* обхода Command Queue;
* прямого изменения `running_` из `requestRun()`, `requestPause()` или `requestReset()`;
* дублирования логики `executeCommand()` в `executeFramesTarget_()`.

---

## 14. Критерий готовности

Stage 5.3.3.3 считается завершённым только если:

```text
GUI/Agent
    ↓
IDebugBackend
    ↓
Command Queue
    ↓
Emulation Thread
    ↓
Board/CPU
```

является единственным production-путём изменения состояния эмулятора.

Для каждой команды гарантируется:

```text
Queued
   ↓
 ┌─────────────┐
 │             │
Cancelled    Executing
               ↓
            Completed
```

И дополнительно:

* `Run` имеет собственный pause future;
* несколько `Run` не конфликтуют;
* все команды проходят один CAS-based processing path;
* `Quit` не оставляет зависших promises;
* CAS race проверяется реальным многопоточным тестом;
* Agent `traceFunction()` работает без polling;
* полный regression suite проходит.
