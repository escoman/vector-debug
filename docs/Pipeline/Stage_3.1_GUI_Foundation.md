# Stage 3.1 — GUI Foundation (Dear ImGui + SDL2)

## Архитектура

```
GUI thread (main)              Emulation thread
  SDL2 window                    DebugBackend (owner)
  Dear ImGui                     Memory
  reads snapshots                executes instructions
  sends commands  ──queue──>     processes commands
  reads results   <──result──    produces results
```

GUI не обращается к i8080/Memory/Board напрямую — только через DebugBackend.

## Шаг 1. Зависимости

Установить dev-пакеты:
```
sudo apt install libsdl2-dev libgl-dev
```

Скачать Dear ImGui (v1.91.x) в `debugger/thirdparty/imgui/`:
- Ядро: `imgui.cpp`, `imgui_draw.cpp`, `imgui_tables.cpp`, `imgui_widgets.cpp`, `imgui_demo.cpp`
- Backends: `imgui_impl_sdl2.cpp`, `imgui_impl_opengl2.cpp`
- Заголовки: все соответствующие `.h`

Используем OpenGL 2 backend — максимально совместимый, не требует шейдеров. Эмулятор использует OpenGL 3 для своих шейдеров, но ImGui backend независим.

## Шаг 2. Минимальные изменения в DebugBackend

**Файл:** `debugger/src/backend.h`, `debugger/src/backend.cpp`

Добавить thread-safe механизм управления из GUI:

```cpp
// Новые публичные методы:
void requestStep();          // запрос: выполнить 1 инструкцию
void requestRun();           // запрос: запустить выполнение
void requestPause();         // запрос: остановиться
void waitForCompletion();    // ждать завершения step

// Новые приватные поля:
std::mutex commandMutex_;
std::condition_variable commandCv_;
std::condition_variable resultCv_;
enum class PendingCommand { None, Step, Run, Pause };
PendingCommand pendingCommand_ = PendingCommand::None;
bool stepCompleted_ = false;
```

Логика:
- `requestStep()` — устанавливает `pendingCommand_ = Step`, ждёт `stepCompleted_` через `resultCv_`
- `requestRun()` — устанавливает `pendingCommand_ = Run`, сигнализирует `commandCv_`
- `requestPause()` — устанавливает `pendingCommand_ = Pause`, сигнализирует `commandCv_`
- `waitForCompletion()` — ждёт завершения текущей операции

Эмуляционный поток вызывает `processCommands()` между инструкциями.

## Шаг 3. Модуль GUI

**Файлы:** `debugger/gui/gui.h`, `debugger/gui/gui.cpp`

```cpp
class DebuggerGui {
public:
    bool initialize(int width, int height);
    void shutdown();
    void beginFrame();
    void render(DebugBackend &backend);
    void endFrame(SDL_Window *window);
    bool shouldQuit() const;
private:
    SDL_Window *window_ = nullptr;
    SDL_GLContext glContext_ = nullptr;
    bool quit_ = false;
    void renderCpuPanel(const CpuState &state);
    void renderCurrentInstruction(uint16_t pc, DebugBackend &backend);
    void renderInstructionHistory(DebugBackend &backend);
    void renderStatusBar(DebugBackend &backend);
    void renderControls(DebugBackend &backend);
};
```

Layout главного окна (ImGui):
- Menu bar: (пока пустой)
- Left panel: CPU registers + Current Instruction + Controls (Step/Run/Pause)
- Right panel: Instruction History (ImGui::BeginChild с прокруткой)
- Bottom: Status bar (state, PC, instruction count)

## Шаг 4. Точка входа

**Файл:** `debugger/gui/main.cpp`

```
main():
  1. SDL_Init, создать окно, GL context
  2. gui.initialize()
  3. Создать Memory, DebugBackend
  4. Запустить emulation thread
  5. Main loop:
     a. SDL_PollEvent → обработка quit/resize
     b. gui.beginFrame()
     c. gui.render(backend) — отрисовка панелей
     d. gui.endFrame(window) — SDL_GL_SwapWindow
  6. gui.shutdown(), SDL_Quit
```

Emulation thread:
```
threadFunc():
  loop:
    wait for command (commandCv_)
    if Step: backend.stepInstruction(), signal resultCv_
    if Run: while (!pauseRequested && !breakpoint) stepInstruction()
    if Quit: break
```

## Шаг 5. CMake интеграция

**Файл:** `debugger/CMakeLists.txt`

Добавить:
- `find_package(SDL2 REQUIRED)` + `find_package(OpenGL REQUIRED)`
- Include paths для ImGui sources
- Новый target `debugger_gui` с sources:
  - `gui/gui.cpp`, `gui/main.cpp`
  - ImGui sources (11 файлов)
  - `src/backend.cpp`, `src/disassembler.cpp`
  - `../src/i8080.cpp`, `../src/memory.cpp`
- Link: `${SDL2_LIBRARY}`, `${OPENGL_LIBRARIES}`, pthread

## Шаг 6. Панели GUI

### CPU Panel
```
PC  0000
AF  0000
BC  0000
DE  0000
HL  0000
SP  0000
IFF 0
```
Данные из `backend.getCpuState()`. Всё в hex.

### Current Instruction
Использовать `disassemble()` из `disassembler.h` с `DisasmReadFn` через `Memory::peek()`:
```
PC: 0000
0000: NOP
```

### Instruction History
`backend.instructionHistorySnapshot()` → ImGui::ListBox или BeginChild.
Для каждой инструкции: `PC  DISASSEMBLY`.
Последняя инструкция выделяется цветом.

### Controls
Кнопки `[Step]` `[Run]` `[Pause]`.
- Step: `backend.requestStep()` + `backend.waitForCompletion()`
- Run: `backend.requestRun()`
- Pause: `backend.requestPause()`

### Status Bar
```
Status: PAUSED  PC: 0000  Instructions: 0
```

## Шаг 7. Тестирование

1. Сборка в Linux: `cd debugger/build && cmake .. && make`
2. Запуск: `./debugger_gui`
3. Проверка: окно открывается, панели отображаются
4. Step: выполняет 1 инструкцию, регистры обновляются
5. Run: выполнение продолжается
6. Pause: выполнение останавливается
7. History: показывает выполненные инструкции
8. Существующие 21/21 тестов test_backend продолжают проходить

## Что НЕ делается на этом этапе

- Memory Map / Memory Dump
- Breakpoint management UI (только отображение факта остановки)
- I/O Trace
- Vector Screen
- Watchpoints
- Редактирование памяти/регистров
- GDB UI
- Windows cross-build (только Linux)

## Файлы

**Новые:**
- `debugger/thirdparty/imgui/` — ~15 файлов Dear ImGui
- `debugger/gui/gui.h`
- `debugger/gui/gui.cpp`
- `debugger/gui/main.cpp`

**Изменённые:**
- `debugger/CMakeLists.txt` — добавить target debugger_gui
- `debugger/src/backend.h` — добавить thread-safe command API
- `debugger/src/backend.cpp` — реализовать command processing

**Неизменные:**
- `src/*` (ядро эмулятора)
- `debugger/src/events.h`, `disassembler.h`, `ring_buffer.h`, `opcode_info.h`
- `debugger/tests/test_backend.cpp`

## Известные ограничения

- OpenGL 2 renderer (не 3) — проще, но достаточно
- Нет Windows cross-build на этом этапе
- Run без ограничения скорости — CPU на 100% в одном ядре
- Breakpoint UI только показывает факт остановки, без управления
