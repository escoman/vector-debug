# Stage 3.13: Полное отделение GUI от ядра Debugger

## Текущие нарушения

| Файл | Нарушение |
|------|-----------|
| `gui/board_wrapper.h/cpp` | Содержит Board/Memory/IO/TV/etc в `gui/` |
| `gui/gui.h` | `class Memory;` forward decl, `render(backend, memory)` |
| `gui/gui.cpp:502` | `DebugMemoryAccess::peek(memory, addr)` |
| `gui/main.cpp` | HAL globals `g_memory/g_io/g_board`, `BoardWrapper` |
| `src/backend.cpp:983` | `requestPause()` -> `board_->debugger_break()` из GUI thread |

---

## Phase 1: Перемещение BoardWrapper -> DebugAdapter

Переместить `gui/board_wrapper.{h,cpp}` в `debugger/src/debug_adapter.{h,cpp}`. Переименовать класс `BoardWrapper` -> `DebugAdapter`. Обновить include в `main.cpp` и CMakeLists.txt.

## Phase 2: Удаление Memory из GUI

- Добавить `uint8_t readMemoryByte(uint16_t addr)` в `DebugBackend` (публичный, для disassembler peek)
- Убрать `class Memory;` и `Memory&` из `gui.h`/`gui.cpp`
- `renderCurrentInstruction()` использует `backend.readMemoryByte()` вместо `DebugMemoryAccess::peek(memory)`
- `main.cpp`: `gui.render(backend, wrapper.memory)` -> `gui.render(backend)`

## Phase 3: Fix requestPause()

Добавить `std::atomic<bool> breakRequested_` в `DebugBackend`. `requestPause()` устанавливает флаг, `poll_debugger` callback (в emulation thread) проверяет и вызывает `board_->debugger_break()`. Убрать прямой вызов `board_->debugger_break()` из `requestPause()`.

## Phase 4: Разделение CMake targets

Создать `debugger_core` (static library): `DBG_SOURCES` + `debug_adapter.cpp`. Без ImGui/SDL/OpenGL зависимостей. `v06c-debugger` линкуется с `debugger_core`. Тесты продолжают линковаться напрямую (им не нужен adapter).

## Phase 5: HAL bindings в Adapter

Переместить `i8080_hal_*` functions и globals из `main.cpp` в `debug_adapter.cpp`. `main.cpp` вызывает только `adapter.bindHal()`.

## Phase 6: MockDebugBackend

Создать `debugger/tests/mock_backend.h` — класс с тем же интерфейсом, что `DebugBackend`, возвращающий тестовые данные. Основа для GUI-тестов без эмулятора.

## Phase 7: Верификация

- `grep` по `gui/` на Board/Memory/IO/i8080/TV/HAL/BoardWrapper -> 0 результатов
- `git diff -- src/` -> пусто
- Все тесты проходят: 115/115 + 1/1 + 22/22 + 17/17
- `v06c-debugger` собирается и запускается

---

## Файлы

| Файл | Действие |
|------|----------|
| `gui/board_wrapper.{h,cpp}` | -> `src/debug_adapter.{h,cpp}` |
| `src/backend.h` | +`breakRequested_`, +`readMemoryByte()` |
| `src/backend.cpp` | fix `requestPause()`, `poll_debugger` |
| `gui/gui.h` | -`Memory&`, -forward decl |
| `gui/gui.cpp` | `peek(memory)` -> `backend.readMemoryByte()` |
| `gui/main.cpp` | упростить, HAL в adapter |
| `CMakeLists.txt` | `debugger_core` library |
| `tests/mock_backend.h` | новый файл |
