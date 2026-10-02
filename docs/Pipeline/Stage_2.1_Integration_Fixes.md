# Этап 2.1 — Integration & Correctness Fixes

## 1. Memory::peek() — корректный доступ к виртуальной памяти

**Файл:** `src/memory.h`, `src/memory.cpp` (основной эмулятор)

Добавить метод `peek(uint16_t virt)`:
- Выполняет `bigram_select()` + `tobank()` — тот же bank translation, что и реальный CPU
- Читает из `bytes[]` или `bootbytes[]` (как и `read()`)
- **НЕ** вызывает `onread` callback
- **НЕ** изменяет состояние

```cpp
// memory.h
uint8_t peek(uint16_t virt) const;

// memory.cpp
uint8_t Memory::peek(uint16_t virt) const {
    uint32_t bigaddr = const_cast<Memory*>(this)->bigram_select(virt & 0xffff, false);
    if (this->bootbytes.size() && bigaddr < this->bootbytes.size()) {
        return this->bootbytes[bigaddr];
    }
    uint32_t phys = this->tobank(bigaddr);
    return this->bytes[phys];
}
```

## 2. Разделение Fetch / Read / Write

**Файл:** `debugger/src/events.h`

Добавить `Fetch` в `MemoryAccessType`:
```cpp
enum class MemoryAccessType { Fetch, Read, Write };
```

**Файл:** `debugger/src/backend.h`, `debugger/src/backend.cpp`

Стратегия: **fetch window** — до выполнения инструкции бэкенд знает PC и длину. Первые N обращений к памяти классифицируются как Fetch, остальные — Read. Это работает потому что 8080 всегда сначала читает opcode+operands последовательно от PC, и только потом делает data reads.

```cpp
// backend.h — private fields
int fetchRemaining_ = 0;

// backend.cpp — stepInstruction()
fetchRemaining_ = r.length;  // до i8080_instruction()

// backend.cpp — onMemoryRead()
if (fetchRemaining_ > 0) {
    ev.type = MemoryAccessType::Fetch;
    fetchRemaining_--;
} else {
    ev.type = MemoryAccessType::Read;
}
```

Замечание: `Memory::read()` и `i8080_hal_memory_read_byte()` не изменяются. CPU core (`i8080.cpp`) не изменяется.

## 3. Thread Safety — Snapshot API

**Файл:** `debugger/src/backend.h`, `debugger/src/backend.cpp`

Заменить возврат `const &` на snapshot-методы:
```cpp
std::vector<InstructionEvent> instructionHistorySnapshot() const;
std::vector<MemoryAccessEvent> memoryHistorySnapshot() const;
std::vector<IoAccessEvent> ioHistorySnapshot() const;
std::vector<MemoryStats> memoryStatsSnapshot() const;  // 65536 entries
```

Каждый snapshot копирует данные под mutex-ом ring buffer-а. `memoryStatsSnapshot()` копирует массив `impl_->memStats[65536]`.

Удалить старые методы: `instructionHistoryAt()`, `memoryHistoryAt()`, `ioHistoryAt()`, `memoryStatsAt()`.

**RingBuffer:** добавить метод `snapshot()`:
```cpp
std::vector<T> snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<T> result;
    result.reserve(count_);
    size_t start = (count_ < buf_.size()) ? 0 : head_;
    for (size_t i = 0; i < count_; ++i)
        result.push_back(buf_[(start + i) % buf_.size()]);
    return result;
}
```

## 4. Enable/Disable Instrumentation

**Файл:** `debugger/src/backend.h`, `debugger/src/backend.cpp`

```cpp
void setInstrumentationEnabled(bool enabled);
bool isInstrumentationEnabled() const;
```

Когда `enabled_ == false`:
- Memory callbacks остаются установленными (чтобы не ломать цепочку), но пропускают push в ring buffer и обновление stats
- `onInstruction` callback не вызывается
- `onTrace` callback не вызывается
- Sequence counter не инкрементируется

Это позволяет измерить overhead: при `enabled_ == false` остаётся только overhead от самих callback-ов (function call + branch).

## 5. Disassembler — проверка всех 256 opcode

**Файл:** `debugger/tests/test_backend.cpp`

Добавить тест `test_disassembler_all_opcodes()`:
- Массив из 256 ожидаемых мнемоник (по стандарту 8080)
- Для каждого opcode: поместить в буфер, вызвать `disassemble()`, проверить mnemonic
- Для инструкций с операндами: проверить формат operands (регистры, immediate, etc.)
- Особое внимание: `0x76 = HLT`, `0x77 = MOV M,A`, `0xDB = IN port`

## 6. Smoke-test с реальным Memory

**Файл:** `debugger/tests/test_backend.cpp`

Новый тест `test_smoke_real_memory()`:
- Использует `Memory::read()`/`write()` (с bank translation через `tobank()`)
- Загружает ROM через `Memory::init_from_vector()`
- Test HAL вызывает `memory->read(addr, false)` / `memory->write(addr, val, false)`
- Проверяет: instruction events, Fetch/Read/Write классификация, I/O events, sequence numbering, PC/opcode корректность

```cpp
// Test HAL для smoke-test (использует реальные Memory::read/write)
int smoke_hal_read_byte(int addr) {
    return test_memory->read(addr, false);  // с bank translation + callbacks
}
void smoke_hal_write_byte(int addr, int value) {
    test_memory->write(addr, value, false);  // с bank translation + callbacks
}
```

## 7. Performance measurement

**Файл:** `debugger/tests/test_backend.cpp`

Обновить тест `test_performance()`:
- 1 000 000 инструкций (не 10 000)
- Измерить с instrumentation ON и OFF
- Вывести overhead в процентах и ns/instr

```cpp
const int N = 1000000;
// Phase 1: OFF
dbg->setInstrumentationEnabled(false);
auto t0 = now(); for (int i=0; i<N; ++i) dbg->stepInstruction(); auto t1 = now();
// Phase 2: ON
dbg->setInstrumentationEnabled(true);
auto t2 = now(); for (int i=0; i<N; ++i) dbg->stepInstruction(); auto t3 = now();
double off_ms = ..., on_ms = ...;
double overhead_pct = (on_ms - off_ms) / off_ms * 100;
```

## 8. Обновление существующих тестов

**Файл:** `debugger/tests/test_backend.cpp`

- Заменить `instructionHistoryAt(i)` на использование snapshot
- Заменить `memoryHistoryAt(i)` на snapshot
- Заменить `ioHistoryAt(i)` на snapshot
- Заменить `memoryStatsAt(addr)` на snapshot
- Обновить проверки Fetch/Read (раньше все были Read, теперь opcode fetch = Fetch)
- Убрать `buffer()` для CPU-visible адресов в проверках (заменить на `peek()`)

## Сводка изменений по файлам

| Файл | Изменение |
|------|-----------|
| `src/memory.h` | + `peek()` метод |
| `src/memory.cpp` | + `peek()` реализация |
| `debugger/src/events.h` | + `Fetch` в enum |
| `debugger/src/ring_buffer.h` | + `snapshot()` метод |
| `debugger/src/backend.h` | + snapshot API, + enable/disable, + fetch tracking |
| `debugger/src/backend.cpp` | + snapshot реализация, + fetch window, + enable/disable |
| `debugger/tests/test_backend.cpp` | Обновление тестов, disassembler 256, smoke-test, performance |
| `debugger/CMakeLists.txt` | Без изменений (если не появятся новые .cpp файлы) |

**Файлы, которые НЕ изменяются:** `i8080.cpp`, `i8080_hal.h`, `hal.cpp`, `board.cpp`, `server.cpp`, `vio.h`

## Порядок реализации

1. `memory.h`/`memory.cpp` — добавить `peek()`
2. `events.h` — добавить `Fetch`
3. `ring_buffer.h` — добавить `snapshot()`
4. `backend.h`/`backend.cpp` — snapshot API, fetch window, enable/disable
5. `test_backend.cpp` — обновить существующие тесты + новые тесты
6. Сборка, запуск, исправление ошибок
7. Отчёт
