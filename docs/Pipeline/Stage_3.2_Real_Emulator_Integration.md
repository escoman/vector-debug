# Stage 3.2 — Real Emulator Integration + ROM Loading

## Архитектурный принцип

```
GUI
 │
 ▼
DebugBackend (facade)
 │
 ▼
Board
 ├── CPU (i8080)
 ├── Memory
 ├── I/O
 ├── Video (TV + PixelFiller)
 ├── Sound (Soundnik)
 └── Interrupts
```

DebugBackend становится facade над Board. Не дублировать логику Board.

## Шаг 1. Модификация DebugBackend для работы с Board

**Файлы:** `debugger/src/backend.h`, `debugger/src/backend.cpp`

### 1.1 Добавить Board reference (опциональный)

```cpp
class DebugBackend {
private:
    Memory &memory_;
    Board *board_ = nullptr;  // optional, for real emulator mode
    
public:
    // Existing constructor (for tests)
    explicit DebugBackend(Memory &memory);
    
    // New: attach Board for real emulator mode
    void attachBoard(Board *board);
};
```

### 1.2 Модификация stepInstruction()

Если Board подключён:
- Сохранить состояние CPU
- Установить fetch window
- Вызвать `Board::single_step(update_screen=false)`
- Записать события (InstructionEvent, MemoryAccessEvent)
- Вернуть StepResult

Если Board не подключён (tests):
- Использовать текущую логику (i8080_instruction)

### 1.3 Модификация runUntilPause()

Если Board подключён:
- Установить `Board::poll_debugger` callback для проверки команд
- Цикл: вызывать `Board::execute_frame(update_screen=false)`
- Board проверяет `debugger_interrupt` на каждой итерации
- При pause/breakpoint/quit: выйти из цикла

Если Board не подключён (tests):
- Использовать текущую логику (stepInstruction loop)

### 1.4 Интеграция breakpoints

Использовать Board's breakpoint mechanism:
- `addBreakpoint()` → `Board::insert_breakpoint(0, addr, 1)`
- `removeBreakpoint()` → `Board::remove_breakpoint(0, addr, 1)`
- `checkBreakpoint()` → `Board::check_breakpoint()`

Синхронизировать список breakpoints между DebugBackend и Board.

### 1.5 Добавление loadRom()

```cpp
bool DebugBackend::loadRom(const std::string &path, uint32_t org = 0) {
    // Загрузить файл
    std::vector<uint8_t> rom_data = util::load_binfile(path);
    if (rom_data.empty()) return false;
    
    // Загрузить в Memory
    memory_.init_from_vector(rom_data, org);
    
    // Reset Board в режим LOADROM
    if (board_) {
        Options.pc = org;  // Установить PC для reset
        board_->reset(Board::ResetMode::LOADROM);
    }
    
    // Очистить историю
    clearHistory();
    
    return true;
}
```

## Шаг 2. Создание Board wrapper для GUI

**Файл:** `debugger/gui/board_wrapper.h`, `debugger/gui/board_wrapper.cpp`

Создать wrapper, содержащий все зависимости Board:

```cpp
class BoardWrapper {
public:
    Memory memory;
    FD1793 fdc;
    Wav wav;
    WavPlayer tape_player;
    Keyboard keyboard;
    I8253 timer;
    TimerWrapper tw;
    AY ay;
    AYWrapper aw;
    Soundnik soundnik;
    IO io;
    TV tv;
    PixelFiller filler;
    Board board;
    
    BoardWrapper();
    void init();
    void shutdown();
};
```

Инициализация:
- `filler.init()`
- `soundnik.init(nullptr)`
- `tv.init()`
- `board.init()`
- `fdc.init()`
- `board.reset(Board::ResetMode::BLKVVOD)` (с bootrom)

## Шаг 3. Модификация GUI main.cpp

**Файл:** `debugger/gui/main.cpp`

```cpp
int main() {
    // 1. Создать BoardWrapper
    BoardWrapper wrapper;
    wrapper.init();
    
    // 2. Создать DebugBackend
    DebugBackend backend(wrapper.memory);
    backend.attachBoard(&wrapper.board);
    
    // 3. Установить callbacks
    wrapper.board.poll_debugger = [&backend]() {
        // Проверить pending commands
        // Обработать pause/breakpoint
    };
    
    // 4. Запустить emulation thread
    std::thread emuThread([&backend]() {
        backend.runUntilPause();
    });
    
    // 5. GUI loop
    while (!gui.shouldQuit()) {
        gui.beginFrame();
        gui.render(backend);
        gui.endFrame();
    }
    
    // 6. Shutdown
    backend.requestQuit();
    emuThread.join();
    wrapper.shutdown();
}
```

## Шаг 4. Добавление ROM loading в GUI

**Файл:** `debugger/gui/gui.cpp`

Добавить кнопку "Load ROM" в Controls:
- Открыть диалог (использовать SDL2 nativefiledialog или простой текстовый ввод)
- Вызвать `backend.loadRom(path)`
- Обновить отображение имени ROM

Альтернатива: передать ROM через командную строку:
```cpp
int main(int argc, char *argv[]) {
    if (argc > 1) {
        backend.loadRom(argv[1]);
    }
}
```

## Шаг 5. Обновление CMakeLists.txt

**Файл:** `debugger/CMakeLists.txt`

Добавить источники из основного проекта:
- `../src/board.cpp`
- `../src/io.cpp`
- `../src/tv.cpp`
- `../src/sound.cpp`
- `../src/filler.cpp`
- `../src/8253.cpp`
- `../src/ay.cpp`
- `../src/fd1793.cpp`
- `../src/wav.cpp`
- `../src/keyboard.cpp`
- `../src/options.cpp`
- `../src/util.cpp`

Добавить GUI sources:
- `gui/board_wrapper.cpp`

Добавить include paths:
- `../boost` (для boost headers)

Link libraries:
- SDL2, OpenGL (уже есть)
- pthread (уже есть)

## Шаг 6. Минимальные изменения в Board

**Файл:** `src/board.h`, `src/board.cpp`

Добавить debug hook для instrumentation (опционально):
```cpp
class Board {
public:
    std::function<void(void)> on_before_instruction;
    std::function<void(void)> on_after_instruction;
};
```

В `single_step()`:
```cpp
void Board::single_step(bool update_screen) {
    if (on_before_instruction) on_before_instruction();
    
    // ... existing code ...
    this->instr_time += i8080_instruction(&this->last_opcode);
    // ... existing code ...
    
    if (on_after_instruction) on_after_instruction();
}
```

Это позволит DebugBackend устанавливать fetch window и записывать события.

## Шаг 7. Тестирование

### 7.1 Unit tests
- 21/21 test_backend tests должны проходить
- Tests используют DebugBackend без Board

### 7.2 Integration tests
- Запустить реальный ROM (например, `testroms/bord2.rom`)
- Проверить:
  - CPU выполняется
  - Память записывается
  - I/O events появляются
  - History заполняется
  - PC изменяется
  - Breakpoint останавливает выполнение

### 7.3 Video/Sound/Interrupts
- Запустить ROM, использующий interrupts (например, `testroms/vst.rom`)
- Убедиться, что interrupts работают
- Проверить, что video/sound не ломаются

## Файлы

**Новые:**
- `debugger/gui/board_wrapper.h`
- `debugger/gui/board_wrapper.cpp`

**Изменённые:**
- `debugger/src/backend.h` — добавить Board reference, loadRom()
- `debugger/src/backend.cpp` — использовать Board для execution
- `debugger/gui/main.cpp` — создать BoardWrapper, attach Board
- `debugger/gui/gui.cpp` — добавить Load ROM кнопку
- `debugger/CMakeLists.txt` — добавить источники Board
- `src/board.h` — добавить debug hooks (опционально)
- `src/board.cpp` — вызвать hooks в single_step() (опционально)

**Неизменные:**
- `debugger/tests/test_backend.cpp`
- `debugger/src/events.h`, `disassembler.h`, `ring_buffer.h`, `opcode_info.h`

## Известные ограничения

- Нет frame pacing на этом этапе (допустимо)
- Нет Vector Screen UI
- Нет Memory Dump UI
- Нет Breakpoint management UI (только загрузка)
- ROM loading через командную строку или простой диалог
- Windows cross-build не проверялся

## Критерии успеха

1. 21/21 test_backend tests проходят
2. Реальный ROM загружается и выполняется
3. Step выполняет ровно одну инструкцию
4. Run запускает полный Board с video/sound/interrupts
5. Pause безопасно останавливает выполнение
6. Breakpoint останавливает до инструкции
7. Нет двух конкурирующих emulation loops
8. History заполняется корректно
9. CPU state показывает реальное состояние