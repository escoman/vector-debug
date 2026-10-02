# Vector-06C Debugger

Интерактивный отладчик для эмуляции процессора КР580ВМ80А (Intel 8080), используемого в компьютере «Вектор-06Ц».

## Описание

Проект предоставляет:

- **DebugBackend** — библиотека для отладочной эмуляции: пошаговое выполнение, трассировка инструкций, контроль точек останова, дамп памяти.
- **v06c-debugger** — графический интерфейс на базе Dear ImGui + SDL2 с панелью регистров, дизассемблером, историей инструкций, окном Vector Screen и кнопками управления (Step/Run/Pause/Reset).
- **AI Agent API** — программный интерфейс для AI-агентов (LLM), предоставляющий 43+ метода отладки: память, регистры, точки останова, дизассемблер, трассировка, символы. Собирается по флагу `-DV06C_ENABLE_AI_AGENT=ON`.
- **v06c-mcp** — MCP-сервер (Model Context Protocol) для интеграции с AI-агентами через stdio-транспорт. 38 инструментов `debug_*`, тонкая обёртка над Agent API. Собирается вместе с AI Agent.
- **test_backend** — набор автоматических тестов для проверки корректности работы бэкенда.

## Зависимости

Для сборки необходимы:

- **CMake** (версии 2.8.12 или выше) — генерирует Makefile'ы из `CMakeLists.txt`.
- **Компилятор с поддержкой C++17** (GCC 7+, Clang 5+) — исходный код использует `std::optional`, `if constexpr`, structured bindings и другие возможности C++17.
- **Boost** (компоненты: program_options, system, thread, chrono, filesystem) — используется модулем `Board` (основной эмулятор): файловая система для загрузки ROM, потоки для эмуляционного цикла, program_options для разбора аргументов.
- **SDL2** — создание окна, обработка ввода, OpenGL-контекст. Также нужен для `icon_set()` (установка иконки окна из встроенных данных).
- **OpenGL** (libGL) — рендеринг через ImGui backend `imgui_impl_opengl2`.
- **objcopy** (из binutils) — встраивает бинарные ресурсы (`boots.bin`, `icon64.rgba`) в ELF-объект для линковки с исполняемым файлом.

### Установка зависимостей (Ubuntu/Debian)

```bash
sudo apt-get install build-essential cmake libboost-all-dev libsdl2-dev libgl-dev
```

`build-essential` включает GCC, make и binutils (нужный нам `objcopy`).

### Установка зависимостей (Fedora)

```bash
sudo dnf install gcc gcc-c++ cmake make boost-devel SDL2-devel mesa-libGL-devel
```

### Дополнительные зависимости

Проект использует библиотеки из основного репозитория Vector-06C:
- `fast-filters/` — coredsp (для Resampler)
- `coreutil/` — coreutil (для SIMD)

Эти библиотеки поставляются с основным проектом и не требуют отдельной установки.

### Зависимости AI Agent (опционально)

Для сборки AI Agent и MCP-сервера дополнительно требуется:
- **nlohmann/json** — библиотека JSON для C++. Устанавливается через `apt`:
  ```bash
  sudo apt-get install nlohmann-json3-dev
  ```
- **cpp-mcp** — MCP SDK (подмодуль в `thirdparty/cpp-mcp/`, инициализируется автоматически через CMake).

AI Agent и MCP-сервер собираются при включении флага:
```bash
cmake .. -DV06C_ENABLE_AI_AGENT=ON
```

## Dear ImGui — ветка `docking`

Отладчик использует **докинг** (перетаскиваемые и стыкуемые окна), поэтому требуется ветка **`docking`**, а не релиз из master-ветки.

**Почему не подходит релиз (например, v1.91.8 из master):**
- API докинга (`DockSpace`, `DockBuilderAddNode`, `DockBuilderSplitNode`, `DockBuilderDockWindow`, `ImGuiDockNodeFlags_PassthruCentralNode`, `ImGuiConfigFlags_DockingEnable`) отсутствует в релизных тегах master-ветки.
- Эти функции доступны только в ветке `docking`, где докинг является штатным режимом.

**Почему не включена в репозиторий:** Dear ImGui — сторонняя библиотека с собственным циклом releases. Чтобы не дублировать чужой код и иметь возможность обновляться, она подключается как git-клон.

## Сборка

### 1. Клонировать Dear ImGui из ветки `docking` и imgui-node-editor

```bash
cd debugger
mkdir -p thirdparty
git clone --branch docking https://github.com/ocornut/imgui.git thirdparty/imgui
git clone --depth 1 https://github.com/thedmd/imgui-node-editor.git thirdparty/imgui-node-editor
```

> **Примечание:** `--branch docking` клонирует только нужную ветку. Флаг `--depth 1` можно добавить для ускорения, если полная история не нужна.

**imgui-node-editor** — библиотека визуальных node-графов на базе Dear ImGui. Используется в окне Call Graph (Stage 6.12) для отображения графа вызовов из RDB. Поставляется как исходные файлы (copy-paste integration, аналогично Dear ImGui).

### 2. Конфигурация и сборка

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

`cmake ..` находит Boost, SDL2 и OpenGL через `find_package()`, после чего генерирует Makefile'ы. `make` компилирует все цели параллельно.

После сборки в каталоге `build/` появятся:

**Основные цели:**
- `test_backend` — unit-тесты бэкенда (107 тестов)
- `test_symbol_database` — тесты базы символов (22 теста)
- `test_board_smoke` — smoke-тест с реальным Board (1 тест)
- `test_vram_mapping` — тесты маппинга видеопамяти (17 тестов)
- `test_workspace` — тесты менеджера рабочих пространств
- `test_call_graph_model` — тесты модели графа вызовов (19 тестов)
- `test_gui_smoke` — smoke-тест запуска GUI
- `v06c-debugger` — графический отладчик

**AI Agent (при `-DV06C_ENABLE_AI_AGENT=ON`):**
- `test_agent_api` — тесты API агента (77 тестов)
- `test_agent_commands` — тесты команд агента (15 тестов)
- `test_agent_mock` — тесты mock-бэкенда (49 тестов)
- `test_agent_integration` — интеграционные тесты (46 тестов)
- `test_agent_contract` — тесты контракта API (49 тестов)
- `test_mcp_protocol` — тесты MCP-протокола (37 тестов)
- `v06c-mcp` — MCP-сервер для AI-агентов (stdio-транспорт)

## Запуск тестов

```bash
cd debugger/build
./test_backend
```

Ожидается: все тесты пройдены (107/107).

```bash
./test_symbol_database
```

Ожидается: 22/22.

```bash
./test_vram_mapping
```

Ожидается: 17/17.

```bash
./test_workspace
```

Ожидается: все тесты пройдены. Требует Dear ImGui из ветки `docking` (использует DockBuilder API).

## Запуск smoke-теста с реальным Board

```bash
cd debugger/build
./test_board_smoke
```

Ожидается: 1/1 тест пройден.

## Запуск smoke-теста GUI

```bash
cd debugger/build
./test_gui_smoke
```

Запускает `v06c-debugger` и проверяет, что он не падает при старте. Требует собранного `v06c-debugger` (CMake-зависимость проставлена автоматически).

## Запуск отладчика

```bash
cd debugger/build

# Без ROM — загрузится встроенный бут-ПЗУ Вектора
./v06c-debugger

# ROM-файл (загружается с адреса 0x0100, после таблицы векторов прерываний)
./v06c-debugger ../../testroms/clrs.rom

# R0M-файл (загружается с адреса 0x0000, сырой образ памяти)
./v06c-debugger ../../testroms/image.r0m

# С явным указанием адреса загрузки (переопределяет автоопределение)
./v06c-debugger ../../testroms/custom.bin 0x8000
```

**Правила загрузки ROM-файлов:**
- `.rom` — загружаются с адреса `0x0100` (после таблицы векторов прерываний 0x0000–0x00FF)
- `.r0m` — загружаются с адреса `0x0000` (сырой образ памяти)
- Второй аргумент — явное указание адреса загрузки (hex), переопределяет автоопределение по расширению

## Запуск MCP-сервера

MCP-сервер (`v06c-mcp`) работает через stdio-транспорт и предоставляет 38 инструментов `debug_*` для AI-агентов.

```bash
cd debugger/build

# Запуск MCP-сервера (общение через stdin/stdout в формате JSON-RPC 2.0)
./v06c-mcp

# С загрузкой ROM-файла
./v06c-mcp ../../testroms/clrs.rom
```

Пример запроса (JSON-RPC 2.0):
```json
{"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": "debug_get_cpu_state", "arguments": {}}}
```

Подробнее см. [mcp/README.md](mcp/README.md).

## Task Library & Knowledge Base (Stage 6.5)

AI Agent может использовать стандартные методики анализа ROM и проверенную базу знаний о Vector-06C.

### Task Library

Методики выполнения задач (не MCP tools, а инструкции для AI Agent):

| Task ID | Описание |
|---------|----------|
| `generate_map` | Анализ ROM, создание карты функций и меток |
| `stack_safety` | Проверка целостности стека (PUSH/POP, CALL/RET) |
| `find_bugs` | Общий статический/динамический аудит ROM |
| `analyze_vram` | Анализ видеопамяти, палитры, режимов |
| `analyze_io` | Анализ I/O портов и последовательностей |

### Knowledge Base

Проверенные технические сведения о Vector-06C (каждый документ имеет metadata: status, source):

| Документ | Тема | Статус |
|----------|------|--------|
| `architecture.md` | Общая архитектура | verified |
| `cpu.md` | КР580ВМ80А, регистры, флаги | verified |
| `memory.md` | Карта памяти, Bigram paging | verified |
| `video.md` | Видережимы, VRAM, палитра | verified |
| `io.md` | Порты ввода-вывода | verified |
| `keyboard.md` | Клавиатура, матрица 8×8 | verified |
| `sound.md` | AY-3-8912, Covox | verified |
| `rom_format.md` | Форматы ROM (.rom, .r0m, .map) | verified |

### Profiles

Типовые наборы задач и знаний:

| Profile | Описание |
|---------|----------|
| `rom_audit` | Комплексная проверка ROM |
| `reverse_engineering` | Реверс-инжиниринг ROM |
| `bug_hunting` | Поиск ошибок в ROM |

### Validator

```bash
python3 agent/tests/validate_agent_knowledge.py
```

Проверяет: ссылки между Tasks/Knowledge/Profiles, уникальность ID, корректность metadata.

## Структура каталогов

```
debugger/
├── src/            # DebugBackend, дизассемблер, события
├── gui/            # Графический интерфейс (ImGui + SDL2)
├── agent/          # AI Agent API (IDebugBackend, AgentApi, типы)
│   ├── tasks/      # Task Library — методики анализа ROM
│   ├── knowledge/  # Knowledge Base — сведения о Векторе-06Ц
│   └── profiles/   # Profiles — типовые наборы задач и знаний
├── mcp/            # MCP-сервер v06c-mcp (адаптер над Agent API)
├── tests/          # Автоматические тесты
├── thirdparty/     # Сторонние библиотеки (Dear ImGui, imgui-node-editor, cpp-mcp)
└── CMakeLists.txt  # Конфигурация сборки
```
