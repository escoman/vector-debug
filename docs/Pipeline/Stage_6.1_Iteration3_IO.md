# ТЗ — Stage 6.1 Iteration 3: I/O Agent API

## Цель

Добавить в Agent API полноценный доступ AI-агента к портам ввода-вывода Vector-06C:

* `readIo(port)`
* `writeIo(port, value)`

Реализация должна строго соблюдать существующую архитектуру debugger и не изменять `src/`.

---

# 1. Архитектура

Использовать существующую цепочку:

```text
AgentApi
    ↓
IDebugBackend
    ↓
DebugBackend
    ↓
Command Queue
    ↓
DebugAdapter
    ↓
Board / IO
```

### Запрещено

* прямой доступ `AgentApi` к `Board`;
* прямой доступ `AgentApi` к `IO`;
* добавление Vector-specific типов в публичные структуры Agent API;
* обход Command Queue для операций, изменяющих состояние;
* изменение файлов основного эмулятора в `src/`.

Все изменения должны находиться в `debugger/`.

---

# 2. IDebugBackend

Добавить в `IDebugBackend`:

```cpp
virtual AgentApiResult<uint8_t> readIoPort(uint8_t port) = 0;
virtual CommandResult writeIoPort(uint8_t port, uint8_t value) = 0;
```

Если существующий контракт проекта использует другую форму результата, сохранить его стиль, но публичный Agent API должен получить структурированный результат.

## Важно

`readIoPort()` не должен обращаться к состоянию `Board` из GUI/Agent API потока.

Для него также должна соблюдаться thread-safety модель debugger.

---

# 3. DebugBackend

Реализовать:

```cpp
readIoPort(uint8_t port)
writeIoPort(uint8_t port, uint8_t value)
```

### `readIoPort`

Операция должна выполняться в emulation thread либо через существующий механизм безопасного snapshot/command execution.

### `writeIoPort`

Обязательно выполнять через Command Queue.

Схема:

```text
AgentApi::writeIo()
        ↓
DebugBackend::requestWriteIoPort()
        ↓
Command Queue
        ↓
emulation thread
        ↓
DebugAdapter
        ↓
Board I/O write
```

Не создавать второй механизм очереди.

---

# 4. DebugAdapter

Добавить Vector-specific реализацию доступа к I/O.

Использовать существующий API эмулятора.

Не изменять `src/`.

Если существующего публичного API Board недостаточно:

1. сначала проверить существующие методы Board/IO;
2. использовать уже имеющиеся debugger hooks/API;
3. только если это невозможно — добавить адаптерный код в `debugger/`.

**Изменение `src/` запрещено.**

Не дублировать реализацию I/O-логики Vector в debugger.

---

# 5. AgentApi

Добавить публичные методы:

```cpp
AgentApiResult<uint8_t> readIo(uint8_t port);

CommandResult writeIo(uint8_t port, uint8_t value);
```

Названия должны быть именно такими:

```text
readIo
writeIo
```

Порты Vector-06C имеют 8-битный адрес:

```cpp
uint8_t port
```

Значение записи:

```cpp
uint8_t value
```

---

# 6. Ошибки

Использовать существующий механизм ошибок.

Не вводить новую систему ошибок.

Минимально должны корректно обрабатываться:

* успешное чтение;
* успешная запись;
* ошибка выполнения команды;
* timeout;
* отменённая команда, если такая возможность уже предусмотрена CommandResult.

Не считать `port == 0` ошибкой: `0x00` является допустимым номером порта.

Не вводить искусственных ограничений диапазона порта — `uint8_t` уже задаёт диапазон `0x00..0xFF`.

---

# 7. MockAgentBackend

Добавить поддержку:

```cpp
readIoPort()
writeIoPort()
```

Mock должен позволять тесту:

1. задать значение порта;
2. прочитать его через AgentApi;
3. записать новое значение;
4. проверить результат записи;
5. проверить, что запись действительно дошла до mock backend.

Не делать mock всегда возвращающим `0` независимо от состояния — он должен моделировать состояние портов.

Например:

```text
port 0x10 = 0x55
readIo(0x10) → 0x55

writeIo(0x10, 0xAA)
readIo(0x10) → 0xAA
```

---

# 8. Реальные Vector I/O

Не придумывать семантику портов.

Agent API предоставляет **сырой доступ к портам**, а не абстракции вида:

```text
writeAyRegister()
setVideoMode()
readKeyboard()
```

Такие специализированные API сейчас не нужны.

Если существующий Board/IO API различает:

* input port;
* output port;
* чтение/запись;
* специальные устройства,

сохранить существующую семантику эмулятора.

Не добавлять собственную таблицу декодирования портов в Agent API.

---

# 9. I/O trace

Не смешивать:

```text
readIo()
writeIo()
```

с существующим:

```text
getIoTrace()
```

`getIoTrace()` должен продолжать отражать реальные I/O операции CPU.

Новый `readIo()` — это debugger/agent inspection operation.

Новый `writeIo()` — это debugger/agent command.

Не создавать искусственную запись в `IoAccessEvent`, если существующая архитектура не предусматривает её для debugger writes.

Особенно важно не испортить существующую семантику CPU I/O trace.

---

# 10. AgentApiResult

Для `readIo()` использовать:

```cpp
AgentApiResult<uint8_t>
```

Для `writeIo()` допустимо использовать существующий:

```cpp
CommandResult
```

поскольку это state-changing command через Command Queue.

Не создавать новый тип результата.

---

# 11. JSON readiness

Публичные типы должны оставаться пригодными для будущего MCP/JSON слоя.

Пример логического результата `readIo`:

```json
{
  "success": true,
  "value": 85,
  "error_code": "None",
  "error_message": ""
}
```

Для `writeIo` сохранить существующий формат `CommandResult`.

Не добавлять JSON serialization в этой итерации.

MCP, HTTP и JSON transport сейчас НЕ реализуются.

---

# 12. Тесты

Добавить unit-тесты Agent API.

Минимальный набор:

### Test 1 — read

```text
mock port 0x10 = 0x55
readIo(0x10)
→ success
→ value == 0x55
```

### Test 2 — write

```text
writeIo(0x10, 0xAA)
→ success
→ mock port 0x10 == 0xAA
```

### Test 3 — zero port

```text
port = 0x00
```

Должен быть допустимым.

### Test 4 — maximum port

```text
port = 0xFF
```

Должен быть допустимым.

### Test 5 — read/write sequence

```text
writeIo(0x20, 0x12)
readIo(0x20) → 0x12

writeIo(0x20, 0x34)
readIo(0x20) → 0x34
```

### Test 6 — command failure

Проверить корректное возвращение ошибки `writeIo()` при отказе backend/command execution.

### Test 7 — existing IO trace

Убедиться, что добавление Agent I/O API не ломает существующий `getIoTrace()`.

Если существующая архитектура позволяет отдельно проверить thread/queue execution — добавить соответствующий integration test.

---

# 13. Integration test

Добавить минимум один integration test для реального debugger backend:

```text
AgentApi
  ↓
DebugBackend
  ↓
Command Queue
  ↓
DebugAdapter
```

Для `writeIo()` обязательно проверить прохождение через Command Queue.

Не тестировать только прямой вызов mock-метода.

---

# 14. Проверка архитектуры

После реализации проверить:

* `AgentApi` не включает Vector-specific headers;
* `AgentApi` не знает о `Board`;
* `IDebugBackend` не содержит ImGui/SDL;
* `writeIo()` проходит через Command Queue;
* CPU/Board state не модифицируется из GUI/Agent API thread;
* нет второго command queue;
* нет дублирования I/O реализации;
* `getIoTrace()` продолжает работать;
* `src/` не изменён.

---

# 15. Регрессия

Запустить полный существующий набор тестов.

Ожидается:

```text
382/382 existing tests PASS
```

плюс новые тесты I/O.

Если `test_agent_mock` по-прежнему имеет известный pre-existing failure, не смешивать его исправление с этой задачей. Зафиксировать его отдельно.

---

# 16. Что НЕ делать

В этой итерации НЕ реализовывать:

* `getCallGraph(depth)`;
* новый `AgentScreenSnapshot`;
* JSON serialization;
* MCP;
* HTTP API;
* специализированные AY/Video/Keyboard API;
* изменение `src/`;
* рефакторинг `AgentApiResult`;
* удаление legacy `loadRom()`;
* исправление `test_agent_mock`, если проблема не связана с I/O.

Задача должна быть минимальной.

---

# 17. Финальный отчёт

После реализации предоставить:

1. список изменённых файлов;
2. описание цепочки `AgentApi → ... → Board`;
3. какие существующие I/O API использованы;
4. подтверждение отсутствия изменений в `src/`;
5. количество новых тестов;
6. результаты полного тестирования;
7. commit hash;
8. отдельно указать, если пришлось отклониться от ТЗ.

## Критерий готовности

Stage 6.1 Iteration 3 считается завершённой только если:

* `readIo()` существует и работает;
* `writeIo()` существует и работает;
* `writeIo()` использует Command Queue;
* есть Mock coverage;
* есть unit-тесты;
* есть integration test;
* существующий `getIoTrace()` не сломан;
* `src/` не изменён;
* архитектура Agent API не нарушена.
