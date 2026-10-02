# Stage 3.13: Полное отделение GUI от ядра Debugger

## Обзор

Разделить debugger на два независимых слоя: GUI (Dear ImGui/SDL2) и Debugger Core (DebugBackend + Adapter). GUI не должен напрямую обращаться к Board, Memory, CPU, IO или любому компоненту эмулятора.

## Текущее состояние нарушений

Найдены следующие прямые зависимости GUI от эмулятора:

| Файл | Нарушение |
|------|-----------|
| `gui/board_wrapper.h/cpp` | Содержит Board, Memory, IO, TV, Soundnik и т.д. Находится в `gui/` |
| `gui/gui.h` | `class Memory;` forward declaration, `render(DebugBackend&, Memory&)` |
| `gui/gui.cpp` | `DebugMemoryAccess::peek(memory, addr)` в `renderCurrentInstruction()` |
| `gui/main.cpp` | HAL bindings, `g_memory/g_io/g_board`, `BoardWrapper` напрямую |
| `src/backend.cpp:983` | `requestPause()` вызывает `board_->debugger_break()` из GUI thread |

---

## Phase 1: Перемещение BoardWrapper в Adapter layer

### 1.1 Переместить файлы

```
gui/board_wrapper.h  ->  debugger/src/debug_adapter.h
gui/board_wrapper.cpp -> debugger/src/debug_adapter.cpp
```

Переименовать класс `BoardWrapper` -> `DebugAdapter`.

### 1.2 Обновить include-ы

- `debugger/src/debug_adapter.h` — заголовки эмулятора (разрешено, это Adapter)
- `debugger/gui/main.cpp` — `#include "debug_adapter.h"` вместо `"board_wrapper.h"`
- `debugger/CMakeLists.txt` — обновить пути

### 1.3 Обновить CMakeLists.txt

Заменить `${DBG_GUI_DIR}/board_wrapper.cpp` на `${DBG_SRC_DIR}/debug_adapter.cpp` в `GUI_SOURCES`.

---

## Phase 2: Удаление Memory из GUI

### 2.1 Добавить `readMemoryByte()` в DebugBackend

В `debugger/src/backend.h` добавить публичный метод:
```cpp
uint8_t readMemoryByte(uint16_t address);
```
Он уже существует (`readMemory`), но нужен простой byte-level peek для disassembler.

### 2.2 Убрать `Memory&` из `gui.h` / `gui.cpp`

- Удалить `class Memory;` forward declaration из `gui.h`
- Изменить `render(DebugBackend &backend, Memory &memory)` -> `render(DebugBackend &backend)`
- Изменить `renderCurrentInstruction(pc, backend, memory)` -> `renderCurrentInstruction(pc, backend)`
- В `renderCurrentInstruction()` заменить `DebugMemoryAccess::peek(memory, addr)` на `backend.readMemoryByte(addr)`

### 2.3 Убрать `Memory&` из `main.cpp`

- `gui.render(backend, wrapper.memory)` -> `gui.render(backend)`

---

## Phase 3: Fix requestPause() — убрать вызов Board из GUI thread

### 3.1 Проблема

`requestPause()` в `backend.cpp:983` вызывает `board_->debugger_break()` напрямую из GUI thread. Это нарушение правила "Board access only from emulation thread".

### 3.2 Решение

Добавить `std::atomic<bool> breakRequested_{false}` в `DebugBackend`.

`requestPause()`:
```cpp
void DebugBackend::requestPause() {
    running_.store(false, std::memory_order_release);
    breakRequested_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        pauseRequested_ = true;
    }
    commandCv_.notify_one();
}
```

`poll_debugger` callback (в emulation thread) проверяет `breakRequested_`:
```cpp
if (breakRequested_.load(std::memory_order_acquire)) {
    breakRequested_.store(false);
    board_->debugger_break();
    return;
}
```

`executeFramesBoard_()` также проверяет `breakRequested_` между frame-ами.

---

## Phase 4: Разделение CMake targets

### 4.1 Создать `debugger_core` static library

```cmake
add_library(debugger_core STATIC
    ${DBG_SOURCES}
    ${DBG_SRC_DIR}/debug_adapter.cpp
)
target_include_directories(debugger_core PUBLIC ${DBG_SRC_DIR} ${SRC_DIR})
target_link_libraries(debugger_core ${Boost_LIBRARIES} pthread)
```

`debugger_core` НЕ зависит от ImGui, SDL2, OpenGL.

### 4.2 Обновить `v06c-debugger` target

```cmake
add_executable(v06c-debugger
    ${GUI_SOURCES}
    ${IMGUI_SOURCES}
    ${BOARD_SOURCES}
    ${CORE_SOURCES}
    ...
)
target_link_libraries(v06c-debugger
    debugger_core
    ${SDL2_LIBRARY}
    ${OPENGL_LIBRARIES}
    ${Boost_LIBRARIES}
    pthread
)
```

### 4.3 Обновить тесты

`test_backend` уже линкуется с `DBG_SOURCES` + `CORE_SOURCES`. Может линковаться с `debugger_core` вместо этого. Но `test_backend` использует `DEBUGGER_NO_BOARD`, поэтому `debug_adapter.cpp` не нужен. Решение: `debugger_core` условно компилируется, или тесты линкуются напрямую с нужными файлами (как сейчас).

Альтернатива: разделить `debugger_core` на `debugger_core_base` (без adapter) и `debugger_core` (с adapter). Но это усложнение. Пока оставляем тесты как есть — они линкуются напрямую.

---

## Phase 5: HAL bindings — переместить из main.cpp

### 5.1 Проблема

`main.cpp` содержит HAL bindings (`i8080_hal_*`) и globals (`g_memory`, `g_io`, `g_board`). Это не нарушение GUI rules (main.cpp — это bootstrap, не GUI rendering), но для чистоты архитектуры стоит переместить в Adapter.

### 5.2 Решение

Переместить HAL bindings в `debug_adapter.cpp`:
- `i8080_hal_bind()` -> `DebugAdapter::bindHal()`
- HAL callback functions -> static functions в `debug_adapter.cpp`
- Globals `g_memory/g_io/g_board` -> static в `debug_adapter.cpp`

`main.cpp` вызывает только `adapter.bindHal()` и не знает о HAL деталях.

---

## Phase 6: MockDebugBackend

### 6.1 Создать `debugger/tests/mock_backend.h`

Класс `MockDebugBackend`, реализующий тот же публичный интерфейс `DebugBackend`, но возвращающий тестовые данные:
- Фиксированный `CpuState`
- Тестовые `MemorySnapshot`
- Тестовые breakpoints, symbols, etc.

### 6.2 Использовать для GUI-тестов

Mock позволяет запускать GUI без реального эмулятора.

---

## Phase 7: Финальная верификация

### 7.1 Поиск прямых зависимостей

```bash
grep -rn "Board\|Memory\b\|i8080\|TV\b\|Soundnik\|FD1793\|I8253\|AY\b\|PixelFiller\|HAL\|BoardWrapper" debugger/gui/*.cpp debugger/gui/*.h
```

Цель: 0 прямых обращений (кроме `#include "backend.h"` который является Debugger Core API).

### 7.2 Проверка src/

```bash
git diff -- src/
```

Должен быть пустым (эмулятор не изменён).

### 7.3 Все тесты проходят

```
test_backend:       115/115
test_board_smoke:   1/1
test_symbol_database: 22/22
test_vram_mapping:  17/17
```

---

## Порядок выполнения

1. Phase 1 — Перемещение BoardWrapper (15 мин)
2. Phase 2 — Удаление Memory из GUI (10 мин)
3. Phase 3 — Fix requestPause() (15 мин)
4. Phase 4 — CMake split (15 мин)
5. Phase 5 — HAL bindings (10 мин)
6. Phase 6 — MockBackend (20 мин)
7. Phase 7 — Верификация (10 мин)

## Файлы для изменения

| Файл | Изменение |
|------|-----------|
| `debugger/gui/board_wrapper.h` | Переместить в `debugger/src/debug_adapter.h` |
| `debugger/gui/board_wrapper.cpp` | Переместить в `debugger/src/debug_adapter.cpp` |
| `debugger/src/backend.h` | Добавить `breakRequested_`, `readMemoryByte()` |
| `debugger/src/backend.cpp` | Fix `requestPause()`, `poll_debugger` |
| `debugger/gui/gui.h` | Убрать `Memory&`, forward decl |
| `debugger/gui/gui.cpp` | Убрать `DebugMemoryAccess::peek(memory)` |
| `debugger/gui/main.cpp` | Упростить, убрать HAL globals |
| `debugger/CMakeLists.txt` | Split targets, обновить пути |
| `debugger/tests/mock_backend.h` | Новый файл |

## Критерий завершения

```
GUI -> DebugBackend -> DebugAdapter -> Board
```

- GUI не знает о Board/Memory/CPU/IO
- Core не знает об ImGui/SDL/OpenGL
- `debugger_core` собирается без GUI зависимостей
- Все тесты проходят
- `src/` не изменён
