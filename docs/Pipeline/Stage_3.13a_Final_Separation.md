# Stage 3.13a: Final Debugger Core Separation

## Текущее состояние (после Stage 3.13)

```
gui/main.cpp → DebugAdapter → Board/Memory/IO  (HAL в main.cpp)
gui/*        → DebugBackend& (конкретный класс)
DebugBackend → Board* (прямая зависимость)
DebugBackend → Memory& (прямая зависимость)
```

## Целевое состояние

```
gui/*        → IDebugBackend& (интерфейс)
DebugBackend → IDebugTarget* (интерфейс)
DebugAdapter → IDebugTarget + Board/Memory/IO (реализация)
gui/main.cpp → только composition root (без HAL)
```

---

## Phase 1: IDebugTarget interface

Создать `debugger/src/debug_target.h` — интерфейс между Core и Adapter.

Методы (все, что DebugBackend сейчас берёт от Board/Memory/CPU):

```cpp
class IDebugTarget {
public:
    virtual ~IDebugTarget() = default;
    // Memory
    virtual uint8_t readMemory(uint16_t addr) = 0;
    virtual bool writeMemory(uint16_t addr, uint8_t val) = 0;
    // CPU state
    virtual CpuState getCpuState() = 0;
    virtual void writeCpuRegister(int reg, uint16_t val) = 0;
    // Execution
    virtual void stepInstruction() = 0;
    virtual void executeFrame() = 0;
    virtual void reset(bool loadRom) = 0;
    // Debugger control
    virtual void debuggerBreak() = 0;
    virtual void debuggerContinue() = 0;
    virtual void debuggerAttached() = 0;
    virtual void debuggerDetached() = 0;
    virtual void setPollCallback(std::function<void()> cb) = 0;
    // Breakpoints
    virtual void syncBreakpoint(int id, uint16_t addr) = 0;
    // Screen
    virtual ScreenData screenSnapshot() = 0;
    // ROM
    virtual bool loadRom(const std::string& path, uint32_t org) = 0;
    // Init CPU after ROM load
    virtual void initCpu(uint16_t pc, uint16_t sp) = 0;
};
```

## Phase 2: DebugBackend через IDebugTarget

Изменить `debugger/src/backend.h`:
- `Memory &memory_` → `IDebugTarget *target_` (+ `Memory *rawMemory_` для test-режима)
- `Board *board_` → убрать (всё через target_)
- Конструктор: `DebugBackend(IDebugTarget& target)` + legacy `DebugBackend(Memory&)` для тестов
- `attachBoard()` → `attachTarget(IDebugTarget*)`
- Все `board_->method()` → `target_->method()`
- Все `memory_.method()` → `target_->method()` (кроме callback installation в test-режиме)
- `i8080_*()` → через `target_->writeCpuRegister()` / `target_->getCpuState()`

Для обратной совместимости тестов: legacy-конструктор `DebugBackend(Memory&)` создаёт внутренний `NoBoardTarget` — минимальная реализация `IDebugTarget` без Board (CPU + Memory только).

Файл `debugger/src/no_board_target.h/cpp` — реализация `IDebugTarget` для тестов (заменяет `DEBUGGER_NO_BOARD` логикой).

## Phase 3: DebugAdapter реализует IDebugTarget

Изменить `debugger/src/debug_adapter.h`:
```cpp
class DebugAdapter : public IDebugTarget { ... };
```

Реализовать все виртуальные методы через Board/Memory/CPU:
- `readMemory()` → `memory.read()`
- `stepInstruction()` → `board.single_step(false)`
- `executeFrame()` → `board.execute_frame_with_cadence()`
- `screenSnapshot()` → `board.get_tv()` → pixels
- и т.д.

HAL-функции (`i8080_hal_*`) переместить из `gui/main.cpp` в `debug_adapter.cpp`. `i8080_hal_bind()` определяется в `debug_adapter.cpp` и вызывает `DebugAdapter::setHalPointers()`.

## Phase 4: IDebugBackend interface

Создать `debugger/src/idebug_backend.h` — интерфейс для GUI.

Содержит все методы, которые GUI вызывает (полный список из grep):
- State: `getState()`, `isPaused()`, `getStopReason()`
- Control: `requestRun/Pause/Step/Reset/Quit()`
- CPU: `getCpuState()`
- Memory: `readMemory()`, `readMemorySnapshot()`, `writeMemoryByte()`
- Registers: `writeRegister(RegisterId, uint16_t)`
- Breakpoints: `addBreakpoint()`, `removeBreakpoint()`, `getBreakpoints()`, etc.
- History: `instructionHistorySnapshot()`, `ioHistorySnapshot()`
- Screen: `screenSnapshot()`, `videoModeSnapshot()`, `vramWriteSnapshot()`
- Activity: `activitySnapshot()`, `clearActivityCounters()`
- Symbols: `symbolDatabase()`
- ROM: `loadRom()`
- Misc: `clearHistory()`, `clearIoHistory()`, `stepInstruction()`

`DebugBackend` наследует `IDebugBackend`.

## Phase 5: GUI через IDebugBackend

Заменить `DebugBackend&` → `IDebugBackend&` во всех GUI файлах:
- `gui.h` / `gui.cpp`
- Все `*_window.h` / `*_window.cpp` (12 файлов)

Это механическая замена типа — `DebugBackend` реализует `IDebugBackend`, поэтому polymorphism работает.

GUI-файлы больше не `#include "backend.h"` — только `"idebug_backend.h"`.

## Phase 6: main.cpp — Composition Root

Упростить `gui/main.cpp`:
- Убрать HAL-функции (перемещены в `debug_adapter.cpp`)
- Убрать `i8080_hal.h` include
- Убрать `DebugAdapter::halMemory()/halIo()/halBoard()` вызовы
- Оставить только composition:
```cpp
int main() {
    DebugAdapter adapter;
    adapter.init();
    DebugBackend backend(adapter);  // IDebugTarget
    backend.attachTarget(&adapter);
    DebuggerGui gui;
    gui.initialize(1024, 768);
    while (!gui.shouldQuit()) {
        gui.beginFrame();
        gui.render(backend);  // IDebugBackend&
        gui.endFrame();
    }
    ...
}
```

Результат: `gui/main.cpp` не содержит `Memory`, `IO`, `Board`, `i8080_hal_*`.

## Phase 7: CMake targets

```cmake
# debugger_core — без GUI, без Board
add_library(debugger_core STATIC
    backend.cpp, disassembler.cpp, debug_memory.cpp,
    symbol_database.cpp, project_file.cpp, vram_mapping.cpp,
    no_board_target.cpp  # для тестов
)

# debugger_adapter — Vector-specific
add_library(debugger_adapter STATIC
    debug_adapter.cpp
    + BOARD_SOURCES + CORE_SOURCES
)
target_link_libraries(debugger_adapter debugger_core)

# v06c-debugger — GUI
target_link_libraries(v06c-debugger debugger_adapter debugger_core ...)
```

## Phase 8: Dependency tests

Добавить скрипт/тест `check_dependencies.sh`:
- Проверить `debugger/gui/*` на `#include` запрещённых заголовков
- Проверить `debugger/src/*` на `#include "imgui.h"`, `"SDL.h"`, etc.
- Цель: 0 нарушений

## Phase 9: Верификация

- Все тесты: 115/115 + 1/1 + 22/22 + 17/17
- GUI build: OK
- `git diff -- src/` — пусто
- `git diff --check` — чисто
- grep по gui/: 0 запрещённых зависимостей

---

## Файлы

| Файл | Действие |
|------|----------|
| `src/debug_target.h` | Новый — IDebugTarget interface |
| `src/idebug_backend.h` | Новый — IDebugBackend interface |
| `src/no_board_target.h/cpp` | Новый — IDebugTarget для тестов |
| `src/backend.h` | IDebugTarget* вместо Board*/Memory& |
| `src/backend.cpp` | Все board_-> → target_-> |
| `src/debug_adapter.h/cpp` | : public IDebugTarget, +HAL |
| `gui/gui.h` | IDebugBackend& вместо DebugBackend& |
| `gui/gui.cpp` | то же |
| `gui/*_window.h/cpp` | IDebugBackend& (12 файлов) |
| `gui/main.cpp` | Упростить, убрать HAL |
| `CMakeLists.txt` | debugger_core + debugger_adapter |
| `tests/check_dependencies.sh` | Новый — dependency test |
