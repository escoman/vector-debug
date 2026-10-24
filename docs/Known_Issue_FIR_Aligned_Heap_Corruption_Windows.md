# Known Issue — повреждение кучи (STATUS_HEAP_CORRUPTION) в `coredsp::FIR` на Windows

- **Статус:** Open (решено чинить на Linux-сборке/upstream сначала; submodule
  `vendor/v06c-emu` в этой ветке намеренно **не** патчится)
- **Обнаружено:** прогон тестовой сюиты под MinGW/MSYS2 (Windows-порт)
- **Влияние:** 6 тестовых бинарей падают/флакают на Windows, хотя все ассерты проходят:
  `test_board_smoke`, `test_sound_window`, `test_audio_midi`, `test_agent_mock`
  (детерминированный крах) и `test_mcp_protocol` (flaky-крах на выходе процесса;
  61/61 ассерты проходят стабильно). Плюс `test_gui_smoke`: сам smoke-механизм
  (spawn → pipe → WM_CLOSE) работает, но дочерний `v06c-debugger.exe` падает с
  `STATUS_HEAP_CORRUPTION` в деструкторе `FIR` на выключении, поэтому «shutdown
  complete» никогда не печатается (3/3 FAIL по той же причине).
- **Приоритет:** высокий для Windows-CI (красные тесты), но корень — во внешнем submodule.

## Это дефект НЕ в этом репозитории

Дефект находится в vendored-подмодуле `vendor/v06c-emu` (git submodule,
`escoman/vector06sdl`, ветка `4write`). Код самого векс-отладчика (`src/`, `gui/`,
`agent/`, `rdb/`, `export/`, `tests/`) корректен — он лишь строит `Board`, который
владеет цепочкой `Soundnik → Resampler → coredsp::FIR<N, __vector>`.

## Корень (подтверждено кодом)

Фирменный ресемплер выделяет выровненный SIMD-буфер placement-new'ом, но освобождает
его **штатным** `delete[]` — аллокаторы/деструкторы не согласованы.

`fast-filters/sources/coredsp/filter.h`:

```cpp
std::unique_ptr<real_type[], coreutil::aligned_delete<real_type>> c_;  // ✓ верный deleter
std::unique_ptr<real_type[]> x_aligned;                               // ✗ deleter по умолчанию
```

`fast-filters/sources/coredsp/filter/fir.tcc` (конструктор):

```cpp
c_.reset(new (coreutil::aligned(coreutil::simd_alignment)) real_type[nn]());        // выровненно
x_aligned.reset(new (coreutil::aligned(coreutil::simd_alignment)) real_type[nn]()); // выровненно
```

Оба члена выделяются **выровненным** placement-`new[]`, но `c_` освобождается через
`coreutil::aligned_delete` (→ `alfree`), а `x_aligned` — через default-`delete[]`.

`coreutil/sources/coreutil/memory.tcc` — реализация аллокатора:

| Платформа | `almalloc` | `alfree` (для `aligned_delete`) | default `operator delete[]` |
|---|---|---|---|
| Windows (`_WIN32`) | `::_aligned_malloc` | `::_aligned_free` | CRT `free()` |
| POSIX | `::posix_memalign` | `::free` | CRT `free()` |

- **Windows:** память из `_aligned_malloc` **обязана** освобождаться `_aligned_free`.
  Передача её в `free()` (через default-`delete[]`) — неопределённое поведение:
  портит кучу → `STATUS_HEAP_CORRUPTION` (`0xC0000374`) при разрушении `FIR`.
- **POSIX:** `posix_memalign` возвращает блок, освобождаемый обычным `free()`, поэтому
  та же ошибка «случайно» безопасна — отсюда зелёный Linux-CI и красный Windows.

Детонатор — деструктор `Soundnik`/`Resampler` (разрушение локального/членного `Board`
со звуком). Поэтому крах случается либо в середине прогона (разрушение `Board` между
тестами), либо на выходе процесса.

## Как воспроизвести (Windows / MinGW)

```powershell
# после сборки тестовых целей
build\test_agent_mock.exe    ; $LASTEXITCODE   # -> -1073740940 (0xC0000374), 0 строк вывода
build\test_board_smoke.exe   ; $LASTEXITCODE   # -> 0xC0000374 после длинной вереницы «ok»
build\test_sound_window.exe  ; $LASTEXITCODE   # -> 0xC0000374 в середине
build\test_audio_midi.exe    ; $LASTEXITCODE   # -> 0xC0000374 в середине
# test_mcp_protocol.exe: 61/61 PASS, но иногда 0xC0000374/abort(3) на выходе (flaky)
# test_gui_smoke.exe: спавнит v06c-debugger.exe; закрытие окна (WM_CLOSE→SDL_QUIT)
#   приводит к падению потомка с 0xC0000374 в деструкторе FIR -> 0/3 FAIL
```

## Предлагаемое исправление (upstream / сначала на Linux)

Однострочник в submodule — привести deleter `x_aligned` в соответствие с alloc, по
образу уже корректного `c_`:

```cpp
// vendor/v06c-emu/fast-filters/sources/coredsp/filter.h:32
- std::unique_ptr<real_type[]> x_aligned;
+ std::unique_ptr<real_type[], coreutil::aligned_delete<real_type>> x_aligned;
```

Это platform-agnostic правка (на Linux `aligned_delete`→`free`, что сейчас и так
работает; на Windows даёт нужный `_aligned_free`). Прогнать тесты на Linux-сборке,
убедиться в отсутствии регрессий и отправить патч в `vector06sdl` (ветка `4write`);
затем обновить submodule-гитлинк здесь. Правка **не** выполняется в этой Windows-ветке
по согласованному решению.

## Временное состояние Windows-CI

Пять перечисленных целей остаются красными/flaky как известный Windows-only дефект
vendored-ресемплера. Их портирование/сборка корректны (`SDL_MAIN_HANDLED`,
`SDL2_LIB_ONLY`); блокирует исключительно рантайм-крах в submodule.
`test_gui_smoke` портирован на `CreateProcess`/`WM_CLOSE` и механически корректен,
но так же упирается в этот дефект (крах потомка на выключении).
