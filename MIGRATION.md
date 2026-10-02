# Миграция debugger/ → отдельный репозиторий (план A)

> Документ-передача для агента, запущенного в workspace
> `/home/alexey/Projects/vector-debug`. Все решения уже обсуждены с
> пользователем — выполняй, не пересогласовывая подход. Историю коммитов
> переносить НЕ НУЖНО — обычный copy без git-магии. Отмечай прогресс и
> коммить после каждого логического этапа; push в GitHub — только когда все
> тесты зелёные. Ответы пользователю — на русском.
>
> ВАЖНО: это редакция v2 — перенос БЕЗ истории (простой copy). Если по
> предыдущей редакции уже была выполнена миграция истории через
> filter-branch/filter-repo — откатить ветку main в пустое состояние
> (`git symbolic-ref refs/heads/main` + удаление импортированных коммитов,
> напр. `git update-ref -d refs/heads/main; git checkout main`) и начать заново
> по этому документу. Сам файл MIGRATION.md при откате сохранить.

## Контекст

- **Источник**: `/home/alexey/Projects/vector-debugger` — клон монолита
  `escoman/vector06sdl` (переименованный форк `svofski/vector06sdl`).
  Папка `debugger/` в нём содержит отладчик; рабочая ветка **`4write`**
  (HEAD = `5cea733`) — именно в ней лежат guard'ы `V06C_DEBUGGER` в `src/`.
  В `origin/master` guard'ов НЕТ.
- **Приёмник**: ЭТОТ репозиторий `escoman/vector-debug`
  (`/home/alexey/Projects/vector-debug`), пока пустой (ветка `main` без коммитов).
- **Цель**: скопировать содержимое `debugger/` в корень этого репозитория;
  эмулятор (`src/`, `cmake/`, `boot/`, `fast-filters/`, `coreutil/`, `testroms/`)
  подключить как submodule `vendor/v06c-emu` на ветку `4write`.
- Старый репозиторий НЕ трогать (папку `debugger/` из него не удалять,
  remote не менять) — пока пользователь не проверит новый репо.

Принятые решения (не пересматривать):
1. **Перенос без истории** — copy working tree (кроме build-артефактов),
   первый коммит в новом репо: `Initial import of debugger from vector06sdl monorepo`.
2. Submodule URL: `https://github.com/escoman/vector06sdl`, ветка `4write`
   (единственная с guard'ами). Если guard'ы переедут в master/upstream —
   ветку переключим отдельно.
3. Путь submodule эмулятора: `vendor/v06c-emu`.
4. `ROOT_DIR` в CMake превращается в cache-переменную `V06C_EMU_DIR` —
   ОДНА точка правки, все остальные `${ROOT_DIR}/...` пути продолжают работать.

## Шаг 0. Проверки

```bash
git remote -v      # origin = https://github.com/escoman/vector-debug
git status         # чистый, main без коммитов
```

## Шаг 1. Копирование debugger/ в корень

```bash
rsync -a --exclude 'build/' /home/alexey/Projects/vector-debugger/debugger/ \
    /home/alexey/Projects/vector-debug/
```

Скопируется: `src/ tests/ gui/ rdb/ agent/ mcp/ export/ res/ docs/ thirdparty/
CMakeLists.txt README.md .gitignore`. Из `debugger/build/` нужен только
`make_release.sh` — его переложим отдельно (шаг 5.4). Папку `build/` с
бинарниками/объектами не копируем.

ВНИМАНИЕ: в новом репе корнем станет бывшая `debugger/`, а внутри появится
`src/` — это исходники ЯДРА дебаггера (backend, disassembler...), не путать
с `src/` эмулятора (он теперь в `vendor/v06c-emu/src/`).

Первый коммит (после шагов 2–3, когда пути внутри файлов уже починены, —
см. порядок ниже).

## Шаг 2. cpp-mcp как submodule

`.gitmodules` скопировался со старым путём `debugger/thirdparty/cpp-mcp`.
Переписать на:

```ini
[submodule "thirdparty/cpp-mcp"]
    path = thirdparty/cpp-mcp
    url = https://github.com/hkr04/cpp-mcp.git
```

Каталог `thirdparty/cpp-mcp/` с кодом уже скопирован как обычные файлы.
Превратить его в submodule:

```bash
rm -rf thirdparty/cpp-mcp          # содержимое вернётся при submodule update --init
git submodule add https://github.com/hkr04/cpp-mcp.git thirdparty/cpp-mcp
git submodule update --init thirdparty/cpp-mcp
# опционально: закрепить тот же коммит, что в старом репо:
# git -C /home/alexey/Projects/vector-debugger submodule status debugger/thirdparty/cpp-mcp
# → hash 13a8bef5..., его и checkout в thirdparty/cpp-mcp
```

## Шаг 3. Submodule эмулятора

```bash
git submodule add -b 4write https://github.com/escoman/vector06sdl vendor/v06c-emu
ls vendor/v06c-emu/src vendor/v06c-emu/cmake vendor/v06c-emu/boot/boots.bin \
   vendor/v06c-emu/fast-filters/sources vendor/v06c-emu/coreutil/sources \
   vendor/v06c-emu/testroms/clrs.rom   # всё должно существовать
grep -rl V06C_DEBUGGER vendor/v06c-emu/src/ | head   # guard'ы на месте (>=5 файлов)
```

Нюансы:
- `.gitmodules` форка (внутри vendor/) ссылается на `debugger/thirdparty/cpp-mcp` —
  безвредно, не трогать.
- Внутри `vendor/v06c-emu` никогда не коммитить на detached HEAD:
  сначала `git switch 4write`.

Коммит: `Initial import of debugger from vector06sdl monorepo`
(шаги 1–3 вместе) — или разбить на import / cpp-mcp / emu-submodule, на вкус агента.

## Шаг 4. Починка путей

### 4.1 `CMakeLists.txt` (теперь в корне репозитория)

Заменить блок Paths (строки ~16-17):

```cmake
# было:
set(ROOT_DIR    ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(SRC_DIR     ${ROOT_DIR}/src)
# станет:
set(V06C_EMU_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/v06c-emu" CACHE PATH
    "Path to Vector-06C emulator sources (git submodule)")
set(ROOT_DIR ${V06C_EMU_DIR})
set(SRC_DIR  ${ROOT_DIR}/src)
```

Больше в CMakeLists менять ничего не нужно: все остальные `${ROOT_DIR}/...`
(`cmake/`, `boot/boots.bin`, `fast-filters`, `coreutil`, `src/`) разрешатся
в submodule. Комментарий про `#ifdef V06C_DEBUGGER` (строки 7-10) оставить.

### 4.2 `tests/test_gui_smoke.cpp`

ROM-фикстура `../../testroms/clrs.rom` больше не существует: testroms теперь
в submodule. Относительно build-каталога путь становится
`../vendor/v06c-emu/testroms/clrs.rom` (аккуратно поправить ВСЕ
fallback-кандидаты вокруг строки ~264, сверяясь с тем, как строится `buildDir`).

### 4.3 `README.md`

- `./v06c-debugger ../../testroms/clrs.rom` → `../vendor/v06c-emu/testroms/clrs.rom`
  (и аналогично для `v06c-mcp`);
- добавить раздел: клонирование `git clone --recursive`,
  обновление submodule (pull в `vendor/v06c-emu` → тесты → commit-bump),
  правки `src/` — только в форке эмулятора под `#ifdef V06C_DEBUGGER`.

### 4.4 `make_release.sh`

```bash
mkdir -p scripts
cp /home/alexey/Projects/vector-debugger/debugger/build/make_release.sh scripts/
```

Адаптировать: запуск из `build/` (пути `..` остаются валидны), порядок
`cmake -DENABLE_AI_AGENT=ON ..` → `make clean` (ИМЕННО ПОСЛЕ cmake) →
`make v06c-debugger v06c-mcp -j$(nproc)`. Больше хитростей с gitignore не
нужно — скрипт лежит в `scripts/`, а `build/` игнорируется.

### 4.5 `.gitignore`

Проверить скопированный `.gitignore`: должен игнорировать `/build/`
(в старом репо он был рассчитан на `debugger/build` — привести к новому
корню). Заодно добавить `/.qoder/` если пользователь не хочет светить IDE-конфиг.

## Шаг 5. Конфиг IDE

Создать `.qoder/mcp.json`:

```json
{
  "servers": {
    "vector-debugger": {
      "command": "/home/alexey/Projects/vector-debug/build/v06c-mcp"
    }
  }
}
```

Скопировать из старого workspace `.qoder/skills/` и `.qoder/agents/`
(skill `vector06c-debugger`, агент `vector06c-analyst`).
Опционально (улучшение, не обязательное сейчас): CMake-цель `install-local`
с копированием бинарников в `$HOME/.local/bin` и путь в mcp.json на неё.

## Шаг 6. Сборка и тесты (ОБЯЗАТЕЛЬНО перед push)

```bash
mkdir -p build && cd build
cmake -DENABLE_AI_AGENT=ON ..
cmake --build . --target v06c-debugger v06c-mcp -j$(nproc)
```

Прогнать все test-таргеты (собираются по имени):

```bash
cmake --build . --target test_backend test_board_smoke test_rdb_controller \
  test_symbol_database test_vram_mapping test_map_loader test_map_import \
  test_map_integration test_runtime_accounting test_rom_loading \
  test_reset_cpu_state test_asm_exporter test_code_analyzer \
  test_call_graph_model test_workspace test_config_manager test_live_map \
  test_memory_access_filter test_sound_window test_rom_load_address \
  test_mcp_protocol test_agent_api test_agent_commands test_agent_mock \
  test_agent_integration test_agent_contract test_agent_runtime_memory \
  test_agent_rom_load_breakpoints test_agent_rdb_save_sync -j$(nproc)
for t in test_*; do ./$t || echo "FAIL: $t"; done
./test_gui_smoke   # последний: требует v06c-debugger + testroms из submodule
```

GUI-тесты требуют X11-сессию пользователя. Ориентир: чистая сборка ~1м50с.
Известные грабли: `-no-pie` для целей с `boots.bin.o` (уже в CMake);
`ENABLE_AI_AGENT=ON` обязателен для `v06c-mcp`.

## Шаг 7. Commit, push и финальная сверка

```bash
cd /home/alexey/Projects/vector-debug
git add -A && git commit          # если ещё не набрано по шагам
git push -u origin main
git status              # чист? ahead/behind?
git submodule status    # pin 4write-коммита форка; cpp-mcp на нужном hash
```

После push пользователь проверяет новый workspace в IDE (MCP поднимается,
GUI стартует, ROM грузится). ТОЛЬКО после его подтверждения обсуждается
следующий этап: удаление `debugger/` из старого репозитория.

## Регламент на будущее (зафиксировать в README)

- Правки в `src/` (новые hooks под `V06C_DEBUGGER`) — только через
  `vendor/v06c-emu` с `git switch 4write` и push в форк, затем bump здесь.
- Обновление эмулятора: pull в submodule → тесты → commit-bump.
- `git submodule update` сам не обновляется до upstream — это всегда осознанный коммит.

## Полезные факты (экономия времени агента)

- Вне `debugger/CMakeLists.txt` никаких ссылок на `../src`, `../boot`,
  `../cmake` в коде сборки нет — один корень всех путей.
- `boots.bin` вшивается в бинарь через objcopy → `v06c-mcp` самодостаточен
  в рантайме; ROM'ы подаются абсолютными путями.
- Папка `boost/` в корне старого репо дебаггеру НЕ нужна (там только
  `find_package(Boost)` из системы).
- Коммит guard'ов в src: `08d108c` (ветка 4write). Guard-места: `memory.h/.cpp`,
  `vio.h`, `ay.h`, `filler.h`, `fd1793.h`, `util.h` — 12 штук.
