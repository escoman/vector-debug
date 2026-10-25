# Known Issue — повреждение кучи (STATUS_HEAP_CORRUPTION) в `coredsp::FIR` на Windows

- **Статус:** Fixed upstream — исправлено в submodule `vendor/v06c-emu`
  (commit `54afa7f` на ветке `4write`, запушено в `escoman/vector06sdl`). Гитлинк в
  этом репозитории обновлён на `54afa7f`. Требуется проверка на Windows-CI.
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

## Применённое исправление (upstream)

Однострочник в submodule — deleter `x_aligned` приведён в соответствие с alloc, по
образу уже корректного `c_`:

```cpp
// vendor/v06c-emu/fast-filters/sources/coredsp/filter.h:32
- std::unique_ptr<real_type[]> x_aligned;
+ std::unique_ptr<real_type[], coreutil::aligned_delete<real_type>> x_aligned;
```

Это platform-agnostic правка (на Linux `aligned_delete`→`free`, что и так работало;
на Windows даёт нужный `_aligned_free`). Правка закоммичена в `vector06sdl`
(ветка `4write`, commit `54afa7f`) и запушена в origin. Linux-сборка перепроверена —
регрессий нет (`test_board_smoke`, `test_sound_window`, `test_audio_midi`,
`test_agent_mock`, `test_mcp_protocol` зелёные). Гитлинк submodule в этом
репозитории обновлён на `54afa7f`.

## Состояние Windows-CI (ожидает подтверждения)

Ожидается, что перечисленные цели (`test_board_smoke`, `test_sound_window`,
`test_audio_midi`, `test_agent_mock`, `test_mcp_protocol`, `test_gui_smoke`) станут
зелёными после подтягивания обновлённого submodule-гитлинка. Требуется финальная
проверка прогона в Windows/MinGW-окружении.
