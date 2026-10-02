# DEBUGGER_ARCHITECTURE.md

## Анализ существующей архитектуры vector06sdl

### 1. CPU — Intel 8080 (i8080.cpp / i8080.h)

**Реализация:** Полная модель КР580ВМ80А в namespace `i8080cpu`.

**Состояние CPU** — глобальная статическая структура:

```cpp
struct i8080 {
    flag_reg f;          // S, Z, AC, P, CY
    reg_pair af, bc, de, hl;
    reg_pair sp, pc;
    uns16 iff;           // interrupt flip-flop
    uns8 ei_pending;
    uns8 cpu_cycles;
    uns16 last_pc;
};
static struct i8080 cpu;
```

**Ключевые функции:**

| Функция | Назначение |
|---------|-----------|
| `i8080_instruction(int *report_opcode)` | Fetch opcode по PC, increment PC, execute. Возвращает cycles. |
| `i8080_execute(int opcode)` | Execute одного opcode. Возвращает v_cycles (векторные такты). |
| `i8080_pc()` | Текущий PC |
| `i8080_regs_a/b/c/d/e/h/l/f/sp()` | Чтение регистров |
| `i8080_setreg_*()` | Запись регистров |
| `i8080_jump(addr)` | Установка PC |
| `i8080_iff()` | Состояние interrupt flip-flop |
| `i8080_cycles()` | Cycles последней инструкции |
| `i8080_init()` | Сброс CPU |

**Доступ к памяти из CPU:** Через макросы `RD_BYTE`, `RD_WORD`, `RD_STACK`, `WR_BYTE`, `WR_WORD`, `WR_STACK`, которые вызывают функции HAL.

**Важно:** CPU не имеет hooks для instruction before/after. Execution — это `i8080_instruction()` → `i8080_execute()`. Перехват возможен только на уровне HAL или обёртки вокруг `i8080_instruction()`.

---

### 2. Execution Loop (board.cpp)

**Главный цикл кадра** — `Board::execute_frame(bool update_screen)`:

```
execute_frame()
├── hooks.frame(frame_no)
├── poll_debugger()
├── if (debugger_interrupt) return
├── for (; !filler.brk && !debugger_interrupt; )
│   ├── check_interrupt()
│   ├── if (debugging && check_breakpoint())
│   │   ├── debugger_interrupt = true
│   │   └── onbreakpoint()
│   └── single_step(update_screen)
```

**`single_step()`** — выполняет одну инструкцию:

```
single_step()
├── i8080_instruction(&last_opcode)  // ← единственная точка выполнения CPU
├── filler.fill(...)                  // видеогенерация
├── interrupt logic
├── tape_player.advance()
└── soundnik.soundSteps()
```

**Точки интеграции для debugger:**
- `poll_debugger` — callback, вызывается в начале каждого кадра
- `onbreakpoint` — callback при срабатывании breakpoint
- `debugger_interrupt` — флаг остановки выполнения
- `debugging` — флаг включения режима отладки

---

### 3. Модель памяти (memory.h / memory.cpp)

**Структура:**

```cpp
class Memory {
    uint8_t bytes[TOTAL_MEMORY];  // 64 KB + 256 KB = 320 KB
    bool mode_stack, mode_map;
    uint32_t page_map, page_stack;
    std::vector<uint8_t> bootbytes;

public:
    // Callbacks для tracking
    std::function<void(uint32_t virt, uint32_t phys, bool stack, uint8_t value)> onwrite;
    std::function<void(uint32_t virt, uint32_t phys, bool stack, uint8_t value)> onread;

    uint8_t read(uint32_t addr, bool stackrq);
    void write(uint32_t addr, uint8_t w8, bool stackrq);
    uint8_t * buffer();  // прямой доступ к массиву bytes
};
```

**Адресация:** Вектор-06Ц имеет банковскую структуру памяти. `bigram_select()` определяет физический адрес в зависимости от режима (stack/map). `tobank()` преобразует линейный адрес в банковый.

**Boot ROM:** Если `bootbytes` не пуст, адреса ниже его размера читаются из boot ROM, а не из RAM.

**Callbacks:** `onread` и `onwrite` вызываются при каждом обращении к памяти. Это **готовая точка интеграции** для memory tracking и watchpoints.

**Прямой доступ:** `buffer()` возвращает указатель на массив — можно читать 64KB без вызова `read()` (но минуя bank switching).

---

### 4. HAL — Hardware Abstraction Layer (hal.cpp / i8080_hal.h)

**Назначение:** Связывает CPU с Memory и IO.

```cpp
void i8080_hal_bind(Memory &_mem, IO &_io, Board &_board);

int i8080_hal_memory_read_byte(int addr);      // → memory->read(addr, false)
void i8080_hal_memory_write_byte(int addr, int value);
int i8080_hal_memory_read_word(int addr, bool stack);
void i8080_hal_memory_write_word(int addr, int word, bool stack);
int i8080_hal_io_input(int port);              // → io->input(port)
void i8080_hal_io_output(int port, int value); // → io->output(port, value)
void i8080_hal_iff(int on);                    // → board->interrupt(on)
```

**Точки интеграции:** HAL — идеальное место для instrumentation. Можно добавить hooks прямо здесь, но это будет влиять на производительность в NORMAL mode. Альтернатива — использовать callbacks Memory/IO.

---

### 5. I/O (vio.h)

**Класс IO** обслуживает:
- PIA (КР580ВВ79А) — порты 0x00-0x03
- PPI2 — порты 0x04-0x07
- Timer (КР580ВИ53) — порты 0x08-0x0b
- Palette — порты 0x0c-0x0f
- Quaz (контроллер памяти) — порт 0x10
- AY-3-8912 — порты 0x14-0x15
- FD1793 (FDC) — порты 0x18-0x1c

**Callbacks:**

```cpp
std::function<int(uint32_t port, uint8_t value)> onread;   // IN
std::function<void(uint32_t port, uint8_t value)> onwrite;  // OUT
```

Вызываются в `input()` и `output()`. Готовая точка для I/O tracking и I/O breakpoints.

**Важно:** `onread` возвращает `int` — если возвращает не -1, значение подменяется. Это используется для debug hook'ов.

---

### 6. Video Subsystem (tv.h / tv.cpp / filler.h)

**TV** — SDL2 window + renderer + textures (или OpenGL).

**Framebuffer:** `uint32_t * bmp` — массив пикселей экрана. Размер: `screen_width * screen_height` (576×288 по умолчанию).

**PixelFiller** — генератор видеосигнала. Работает в тактовом режиме, преобразуя циклы CPU в пиксели. Читает память через `Memory &` и настройки из `IO &` (палитра, режимы, скролл).

**Доступ к экрану:** `TV::pixels()` возвращает `uint32_t*` — текущий framebuffer. Можно использовать для отображения в debugger GUI.

**Двойная буферизация:** TV использует 2 текстуры (`NTEXTURES = 2`), переключаясь между ними.

---

### 7. Существующий Debugger API (board.h / board.cpp)

**Управление:**

| Метод | Назначение |
|-------|-----------|
| `debugger_attached()` | Подключение debugger, `debugging = 1`, останов |
| `debugger_detached()` | Отключение, `debugging = 0`, продолжение |
| `debugger_break()` | Установка `debugger_interrupt = 1` (останов) |
| `debugger_continue()` | Сброс `debugger_interrupt = 0` (продолжение) |
| `single_step(update_screen)` | Выполнение одной инструкции |

**Breakpoints:**

| Метод | Назначение |
|-------|-----------|
| `insert_breakpoint(type, addr, kind)` | Установка breakpoint |
| `remove_breakpoint(type, addr, kind)` | Удаление breakpoint |
| `check_breakpoint()` | Проверка: PC в списке breakpoints? |
| `check_watchpoint(addr, value, how)` | Проверка watchpoint |

**Типы breakpoint (GDB-совместимые):**
- 0/1 — software/hardware breakpoint (по адресу)
- 2 — write watchpoint
- 3 — read watchpoint
- 4 — access watchpoint

**Чтение/запись состояния:**

| Метод | Назначение |
|-------|-----------|
| `read_registers()` | Строка с регистрами (GDB format) |
| `write_registers(uint8_t*)` | Запись регистров |
| `read_memory(start, count)` | Чтение памяти (hex string) |
| `write_memory_byte(addr, value)` | Запись байта |

**Hooks:**

```cpp
std::function<void(void)> poll_debugger;     // вызов в начале кадра
std::function<void(void)> onbreakpoint;      // при breakpoint
std::function<void(void)> onframetimer;      // таймер кадров

struct {
    std::function<void(int)> frame;          // начало кадра
    std::function<void(int)> jump;           // jump
} hooks;
```

---

### 8. GDB Remote Protocol Server (server.h / server.cpp)

**Реализация:** Полноценный GDB RSP сервер на boost::asio, порт 4000.

**Поддерживаемые команды:**

| Команда | Действие |
|---------|----------|
| `g` | Чтение регистров |
| `G` | Запись регистров |
| `m` | Чтение памяти |
| `M` | Запись памяти |
| `Z` | Установка breakpoint/watchpoint |
| `z` | Удаление breakpoint/watchpoint |
| `s` | Single step |
| `c` | Continue |
| `?` | Причина остановки |
| `H` | Thread |
| `q` | Query |
| `D` | Detach |
| `^C` | Interrupt |

**Архитектура:** `Session` обрабатывает подключение. При breakpoint отправляет `T05` через async_write.

---

### 9. Scripting Infrastructure (scriptnik.h / scriptnik.cpp)

**Движок:** ChaiScript (интерпретируемый язык).

**Доступный API для скриптов:**

```
loadwav(filename)
keydown(scancode) / keyup(scancode)
insert_breakpoint(type, addr, kind)
debugger_attached() / debugger_detached()
debugger_break() / debugger_continue()
read_register(name) / set_register(name, value)
read_memory(addr, stackrq) / write_memory(addr, w8, stackrq)
```

**Callbacks:** `onframe(frame)`, `onwavfinished(dummy)`, `onbreakpoint()`.

---

### 10. Threading Model (emulator.h / emulator.cpp)

**Два потока:**
1. **Emulator thread** — выполняет `threadfunc()`, крутит `execute_frame()` по событиям из `ui_to_engine_queue`
2. **UI thread** — `run_event_loop()`, обрабатывает SDL events, рендерит

**Коммуникация:**
- `ui_to_engine_queue` (boost::sync_queue) — EXECUTE_FRAME, KEYDOWN, KEYUP, QUIT
- `engine_to_ui_queue` (boost::sync_priority_queue) — RENDER

**Для debugger:** При `debugger_interrupt` цикл выполнения в `execute_frame()` прерывается. GUI может безопасно читать состояние.

---

## Точки интеграции для Debugger

### Существующие (использовать как есть):

1. **`Board::debugger_interrupt`** — останов/продолжение
2. **`Board::debugging`** — режим отладки
3. **`Board::check_breakpoint()`** — проверка breakpoint по PC
4. **`Board::single_step()`** — одна инструкция
5. **`Board::onbreakpoint`** — callback при остановке
6. **`Board::poll_debugger`** — polling в начале кадра
7. **`Memory::onread/onwrite`** — tracking обращений к памяти
8. **`IO::onread/onwrite`** — tracking обращений к I/O
9. **`TV::pixels()`** — доступ к framebuffer
10. **CPU register accessors** — полный набор get/set

### Новые (необходимо добавить):

1. **Instruction hook** — обёртка вокруг `i8080_instruction()` для tracking:
   - PC before/after
   - opcode, operands
   - memory accesses
   - I/O accesses

2. **O(1) breakpoint lookup** — заменить `std::vector<Breakpoint>` + линейный поиск на `std::array<bool, 65536>` или `std::bitset<65536>`

3. **Memory access history** — ring buffer записей `{PC, addr, value, R/W}`

4. **I/O history** — ring buffer записей `{PC, port, value, IN/OUT}`

5. **Instruction history** — ring buffer записей `{PC, opcode, mnemonic}`

6. **Disassembler** — использовать существующую таблицу opcode из `i8080_execute()` (extract mnemonic/length)

7. **Source mapping (z88dk)** — загрузчик .map/.cdb/.sym

---

## Необходимые изменения в существующем коде

### Минимальные (не ломают совместимость):

1. **board.cpp: `execute_frame()`** — добавить вызов instruction hook перед/после `single_step()`
2. **board.cpp: `check_breakpoint()`** — заменить linear search на O(1) lookup
3. **i8080.cpp** — добавить таблицу opcode info (mnemonic, length, cycles) для disassembler
4. **hal.cpp** — опционально добавить hooks в read/write для memory tracking

### Не требуют изменений:

- CPU core (i8080.cpp) — не нужно модифицировать для базового debugger
- Memory model — callbacks уже есть
- TV/Video — framebuffer доступен через `pixels()`
- GDB server — можно использовать параллельно или заменить

---

## Новые интерфейсы

### DebugBackend (предварительно)

```cpp
class DebugBackend {
    // CPU State
    CPUState getCPUState();
    void setCPURegister(reg, value);

    // Execution
    void run();
    void pause();
    void stop();
    void reset();
    StepResult stepInto();

    // Breakpoints
    void addBreakpoint(BreakpointType, address);
    void removeBreakpoint(id);
    void addWatchpoint(addr, length, type);

    // Memory
    uint8_t readMemory(addr);
    void writeMemory(addr, value);
    const uint8_t* getMemoryPtr();  // прямой доступ к 64KB

    // Memory Map
    MemoryAccess getAccessState(addr);  // для coloring
    AccessStats getAccessStats(addr);

    // History
    InstructionHistory getInstructionHistory(depth);
    MemoryAccessHistory getMemoryHistory(depth);
    IOAccessHistory getIOHistory(depth);

    // Video
    const uint32_t* getScreenPixels();

    // Disassembler
    DisasmLine disassemble(addr);

    // Source
    SourceLocation addressToSource(addr);
    std::vector<addr> sourceToAddresses(file, line);

    // Symbols
    Symbol findSymbol(name);
    Symbol symbolAt(addr);

    // Stack
    StackFrame getStack();
};
```

---

## Последовательность реализации

### Phase 1: Backend Core

1. `DebugBackend` — обёртка над Board/CPU/Memory
2. Instruction hook — обёртка вокруг `single_step()` с tracking
3. O(1) breakpoints — `std::array<bool, 65536>`
4. Memory/IO tracking — использование существующих callbacks
5. Instruction/Memory/IO history — ring buffers

### Phase 2: Disassembler & Symbols

6. 8080 disassembler — таблица opcode → mnemonic
7. z88dk debug info loader — .map, .cdb, .sym парсеры
8. Source mapping — address ↔ source line

### Phase 3: GUI (Dear ImGui + SDL2)

9. Main window layout — dockable windows
10. CPU/Registers window
11. Disassembler / Source view
12. Memory Dump
13. Memory Map (64KB live map)
14. Vector-06C Screen
15. Stack Viewer
16. Ports / I/O History
17. Breakpoints / Watchpoints panel
18. Debugger Console

### Phase 4: Integration & Polish

19. ROM loading + auto-detect debug files
20. Threading (GUI + emulator separate threads)
21. Performance optimization
22. Test program
23. Acceptance tests
