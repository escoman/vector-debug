# ТЗ: Портирование сборки vector-debug на Windows (MinGW/MSYS2)

**Статус:** черновик
**Цель:** получить нативную Windows-сборку (`v06c-debugger.exe`, `v06c-mcp.exe`, `v06c-asm-export.exe`) с ускорением итераций портирования за счёт локальной Windows-машины вместо цикла «push → CI → читать лог».

---

## 1. Проблема и подход

Сейчас проект собирается только под Linux. Windows-джоба в CI
(`.github/workflows/release.yml`) оставлена с `continue-on-error: true` —
она служит наблюдателем: по логу видно, какая стена падает следующей. Каждый
цикл «изменили CMake → пуш → CI (5–15 мин) → разбор ошибки» линеен и медленен.

**Подход:** перенести итерацию поиска и починки ошибок CMake на локальную
Windows-машину, а CI оставить в роли финальной приёмочной проверки. Рабочий цикл:

```
локальная машина: найти стену → починить → проверить за секунды
        ↓ (коммит каждой стены отдельно, Conventional Commits)
CI: прогнать Windows-джобу → убедиться, что окружение раннера совпадает
        ↓ (когда джоба зелёная)
убрать continue-on-error → Windows-артефакт в релизе
```

**Критическое условие:** локальное окружение должно **точно воспроизводить
Windows-джобу CI** (msystem, компилятор, генератор CMake, набор пакетов).
Иначе починки на локальной машине не сойдутся с раннером, и выигрыш
превратится в новые рассинхроны.

---

## 2. Эталон окружения (берётся строго из release.yml)

| Параметр | Значение | Источник |
|---|---|---|
| Runner | `windows-latest` + `msys2/setup-msys2@v2` | release.yml, step «Set up MSYS2» |
| msystem | **MINGW64** (не UCRT64, не CLANG64) | `msystem: MINGW64` |
| Компилятор | GCC из `mingw-w64-x86_64-toolchain` | пакет из списка install |
| CMake | `mingw-w64-x86_64-cmake` (не системный Windows-CMake!) | пакет из списка install |
| Генератор | `-G "MinGW Makefiles"` | step «Build (Windows / MinGW)» |
| Shell | `msys2 {0}` (bash внутри MSYS2) | step «Build (Windows / MinGW)» |
| Флаг сборки | `-DENABLE_AI_AGENT=ON` | step «Build (Windows / MinGW)» |
| Цели | `v06c-debugger v06c-mcp v06c-asm-export -j$(nproc)` | step «Build (Windows / MinGW)» |

### Пакеты для установки (имена — точные, включая регистр!)

```bash
pacman -S --noconfirm --needed \
  mingw-w64-x86_64-toolchain \
  mingw-w64-x86_64-cmake \
  mingw-w64-x86_64-boost \
  mingw-w64-x86_64-SDL2 \
  mingw-w64-x86_64-nlohmann-json \
  mingw-w64-x86_64-binutils \
  make zip
```

> ⚠️ **Регистр имён MSYS2 критичен.** `mingw-w64-x86_64-sdl2` (со строчной
> `s`) — такого имени не существует, pacman сообщит `target not found` и
> **отменит всю транзакцию**, из-за чего остальные пакеты списка не
> установятся вообще, и ошибка будет выглядеть как «проблема с другим
> пакетом». Проверять каждое имя через `pacman -Ss`.
>
> ⚠️ `make` и `zip` из списка — пакеты MSYS (не `mingw-w64-x86_64-`),
> ставить в том же msystem-окружении.

### Что НЕ нужно ставить отдельно

- `nlohmann/json` — вендорится заголовком `thirdparty/nlohmann/json.hpp`;
  пакет CI страхует cpp-mcp, но для сборки достаточно вендорного.
- `cpp-mcp`, `imgui`, `v06c-emu` — git-подмодули (`git submodule update --init --recursive`).
- OpenGL: на MinGW `libGL`/`opengl32` из комплекта mingw-w64 (`mingw-w64-x86_64-opengl` при необходимости).

---

## 3. Настройка локальной Windows-машины

1. Установить [MSYS2](https://www.msys2.org/) (64-bit).
2. Обновить ядро: `pacman -Syu` (повторить, пока не перестанет спрашивать подтверждение).
3. Установить пакеты из раздела 2 из ярлыка **MSYS2 MinGW x64** (это MINGW64 — тот же msystem, что на CI).
4. Установить `git` (можно `pacman -S git` в том же окружении) и
   клонировать репозиторий с подмодулями:
   ```bash
   git clone --recursive https://github.com/escoman/vector-debug.git
   # или, если уже клонировано без подмодулей:
   git submodule update --init --recursive
   ```
5. Конфигурация и сборка — командой, **идентичной CI-джобе**:
   ```bash
   cmake -S . -B build -G "MinGW Makefiles" -DENABLE_AI_AGENT=ON
   cmake --build build --target v06c-debugger v06c-mcp v06c-asm-export -j"$(nproc)"
   ```

**Критерий настроенной машины:** конфигурация CMake доходит до этапа сборки
(падает не на `cmake -S`, а на `cmake --build` или позже) — иначе окружение
ещё не то, и чинить CMakeLists преждевременно.

### Альтернативы, если «чистой» Windows-машины нет

- **Windows-виртуалка** (VirtualBox / KVM+VHDX / VMware) с MSYS2 — тот же
  выигрыш по скорости итераций, чуть медленнее I/O. Предпочтительнее
  кросс-компиляции: окружение идентично раннеру.
- **Кросс-компиляция с Linux** (`x86_64-w64-mingw32-gcc` из apt) — годится
  только для поиска/починки ошибок CMake; окружение отличается от CI ещё
  сильнее, GUI-проверку не заменяет. Не рекомендуется как основной путь.

---

## 4. Карта стен (в порядке ожидаемого столкновения)

По состоянию на текущий `CMakeLists.txt`. Стена 0 уже преодолена — зафиксировано для истории.

| # | Стена | Симптом | Файл/строки | Подход к починке |
|---|---|---|---|---|
| 0 | ~~`cmake_minimum_required(2.8.12)`~~ | CMake 4.x на windows-latest: «Compatibility with CMake < 3.5 has been removed» на первой строке | CMakeLists.txt:1 | **Уже исправлено**: поднято до `3.16`. Policy-сдвиг проверен reconfigure. |
| 1 | `-pthread` в `CMAKE_CXX_FLAGS` | gcc примет флаг, но линковка потоков на MinGW требует `winpthread` | CMakeLists.txt:5 | Гадать не надо: прогон локально покажет реальное поведение. Вероятный фикс — условный флаг/библиотека для Windows или переход на `Threads::Threads` (`find_package(Threads)` — штатный кроссплатформенный путь). |
| 2 | `target_link_libraries(... pthread)` в ~8 целях | `undefined reference to __imp_pthread_create` и подобное при линковке | CMakeLists.txt (213, 238, 290, 312, 416, 441, 474, 505, …) | Заменить `pthread` на `Threads::Threads` (или `winpthread` условно). Лучше делать вместе со стеной 1 — одним коммитом «pthread → Threads::Threads». |
| 3 | `objcopy -O elf64-x86-64` для `boots.bin` | MinGW-objcopy пишет ELF-объект, а линкуется PE/COFF → ошибка формата на линковке | CMakeLists.txt:512–537 | Условие по `WIN32`: формат `pe-x86-64`; проверить, что имена символов `_binary_boots_bin_start/_end/_size` совпадают с тем, что генерирует `objcopy -I binary -O pe-x86-64`, и что `-no-pie` (если используется) корректен для MinGW. Альтернатива — генерация C-массива из бинарника (кросс-платформенно, но меняет подход). |
| 4 | `find_package(SDL2 REQUIRED)` через `CMAKE_MODULE_PATH=${ROOT_DIR}/cmake` | FindSDL2-модуль смотрит Linux-пути, на MSYS2 может не найти SDL2 | CMakeLists.txt:115–116 | На MSYS2 SDL2 ставит `sdl2-config` и `.pc`-файл: вариант — использовать `find_package(SDL2 CONFIG)` / pkg-config, или передать `SDL2_DIR`. Проверять, что модуль в `cmake/` вообще подключается корректно на Windows. |
| 5 | `find_package(Boost REQUIRED COMPONENTS ...)` | MSYS2-Boost обычно находится, но `program_options/filesystem/thread` могут потребовать `Boost_USE_STATIC_LIBS=ON/OFF` манёвра | CMakeLists.txt:45 | Локализовать ошибкой configure; при необходимости — `set(Boost_USE_STATIC_LIBS OFF)` и явный путь. |
| 6 | `find_package(OpenGL REQUIRED)` | На MinGW OpenGL — это `libopengl32.a` из mingw-w64; нужен пакет `mingw-w64-x86_64-opengl` или наличие в составе toolchain | CMakeLists.txt:449 | Добавить пакет / проверить, что `OpenGL::GL` резолвится. Рендеринг ImGui — OpenGL2 backend, на Windows требует реального GL-контекста от SDL (проверка на железе). |
| 7 | `.desktop` / `install-desktop` цели | Linux-only, ошибок сборки не дают | CMakeLists.txt | Обернуть в `if(NOT WIN32)` — косметика, низкий приоритет. |
| 8 | Упаковка DLL в `package_release.sh` | Список DLL захардкожен (`libSDL2-2.0-0.dll` и др.), поиск через `command -v` может не находить в MINGW64 | package_release.sh:63–68 | Проверить локально: после сборки прогнать `bash scripts/package_release.sh windows <ver>` и убедиться, что бандл самодостаточен (запуск с чистого `PATH` / на машине без MSYS2). Допиливать по факту выявленных пропусков (например, `libimgui` нет — он статический, но GL/SDL-зависимости могут тянуть ещё). |

Порядок — ожидаемый, не гарантированный: локальная итерация может вскрывать
стены в другом порядке. Стены 1–3 — обязательные; 4–6 — вероятные; 7–8 — по факту.

---

## 5. Правила процесса

1. **Одна стена — один коммит.** Message в стиле Conventional Commits:
   `fix(build): <стена> for MinGW/MSYS2`. Это позволяет CI-логам и бисекции
   оставаться читаемыми.
2. **CI — только приёмка.** Пуш в main / pull request с прогоном Windows-джобы —
   после того, как локальная сборка зелёная. Не использовать CI для поиска ошибок.
3. **Локальный прогон идентичен CI.** Команды из раздела 3, шаг 5 — те же
   строки, что в `release.yml`. Любое отличие («у меня привычнее») обесценивает
   проверку.
4. **Тесты на Windows — отдельный шаг.** Методика тестов в проекте —
   отдельные бинарники `test_*` (без CTest). На Windows-машине прогонять как
   минимум `test_backend`, `test_board_smoke`, `test_gui_smoke`,
   `test_agent_api`; `test_agent_mock` — известен падением и на Linux,
   падение на Windows не считать блокером портирования.
5. **README.md обновить по завершении:** снять предупреждающую врезку
   («Windows-сборка пока не портирована», строка ~145) и добавить раздел
   «Установка зависимостей (Windows / MSYS2») с пакетами из раздела 2.

---

## 6. Изменения в CI после успешного портирования

В `.github/workflows/release.yml`:

1. Удалить `allow_failure: true` и `continue-on-error: ${{ !!matrix.allow_failure }}`
   из matrix/джобы — Windows-джоба становится обязательной.
2. Update-логика пакетов (раздел 2) уже в джобе — сверить, что список
   совпадает с локальным эталоном (регистр `SDL2`!).
3. Шаг `Package` уже вызывает `package_release.sh windows` — убедиться, что
   артефакт `vector-debug-<ver>-windows-x64.zip` формируется и содержит DLL.
4. Артефакт-джоба `publish` подхватит zip автоматически (`pattern: "v06c-*"`).

---

## 7. Критерии приёмки

- [ ] Локальная Windows-машина (MINGW64): `cmake -S . -B build -G "MinGW Makefiles" -DENABLE_AI_AGENT=ON` проходит без ошибок.
- [ ] `cmake --build build` собирает все три цели: `v06c-debugger.exe`, `v06c-mcp.exe`, `v06c-asm-export.exe`.
- [ ] `v06c-debugger.exe` запускается: открывается окно, рендерит Vector Screen (OpenGL-путь жив на реальном железе), грузит ROM.
- [ ] `v06c-mcp.exe` отвечает на `tools/list` по stdio (JSON-RPC 2.0).
- [ ] Прогнаны тесты (раздел 5.4) — все, кроме заведомо падающего `test_agent_mock`, зелёные.
- [ ] `scripts/package_release.sh windows` собирает zip; распакованный бандл запускается на машине **без** установленного MSYS2.
- [ ] Windows-джоба CI зелёна дважды подряд → `continue-on-error` удалён, релиз публикуется с двумя платформами.
- [ ] README обновлён (раздел 5.5), CHANGELOG — запись о добавлении Windows-релиза.

---

## 8. Риски

| Риск | Митигация |
|---|---|
| Расхождение локального MSYS2 и образов раннера (версии gcc/cmake со временем накатываются) | Синхронизировать `release.yml` с локальным окружением; перед финальным прогоном — `pacman -Syu` локально |
| OpenGL на Windows раннера (`windows-latest`) — software rendering ( Mesa llvmpipe ) в CI может не давать GUI-тестам проходить | Для CI — не требовать GUI-тесты в Windows-джобе (в текущей джобе тесты и не запускаются, только сборка+пакировка); проверка GUI — локально на железе |
| `objcopy -I binary -O pe-x86-64` генерирует символы с иными именами/секциями, чем ожидает код | Заранее проверить `objdump -t boots.bin.o`; при проблемах — запасной путь: генерация C-массива через `xxd -i`-подобный скрипт |
| Подмодуль `vendor/v06c-emu` может содержать Linux-specific код (boost::filesystem пути, сигналы) | Ревью `#ifdef V06C_DEBUGGER`-guard'ов на платформенную специфичность; при обнаружении — правки только в форке по регламенту README (ветка `4write`) |
| Дублирование списков пакетов (локальный эталон vs `release.yml`) может разойтись со временем | Сверять списки при каждом изменении toolchain; держать список из раздела 2 как единственный источник истины |
| Бесплатные Windows-раннеры медленнее и в очереди | Итерации — локально; CI — только приёмка, 2–3 прогона на стену максимум |

---

## 9. Что НЕ входит в ТЗ

- Миграция на MSVC / Visual Studio (путь — строго MinGW/MSYS2, как в CI).
- CMake-структурирование «правильных» `Threads::Threads`-refactoring'ов вне
  минимально необходимых для Windows.
- Изменение функциональности эмулятора/GUI — только сборочная система.
- Публикация Windows-релиза до выполнения критериев раздела 7.
