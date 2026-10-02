# Stage 6.22 — Учёт fetch/sequence в свободном прогоне и условные переходы в анализаторе

**Каноническая копия для разработчика:**
`/home/alexey/Projects/vector-debugger/debugger/docs/Pipeline/Stage_6.22_Conditional_Flow_And_Runtime_Memory_Accounting.md`
**Репозиторий:** `/home/alexey/Projects/vector-debugger`
**Файлы:** `src/i8080.cpp`, `src/hal.cpp`, `src/memory.{h,cpp}`, `src/board.cpp`,
`src/vio.h`, `debugger/src/backend.cpp`, `debugger/src/debug_adapter.cpp`,
`debugger/agent/agent_api.cpp`, `debugger/agent/agent_types.h`,
`debugger/mcp/mcp_json.cpp`, `debugger/mcp/mcp_adapter.cpp`,
`debugger/agent/code_analyzer.h`, `debugger/src/disassembler.cpp`,
`debugger/tests/*`
**Цель:** поля, которые MCP выдаёт про обращения ЦП к памяти и портам, должны
соответствовать реальному выполнению в **свободном прогоне** (`debug_run`), а не
только в пошаговом режиме; клавиатурный ввод должен быть доступен агенту.

Счёт: 15.09.2026. Все строки проверены по исходникам (только чтение), все
наблюдения — по `TESTAY.ROM` (Stage 1, `roms/redesign/testay/reports/Stage1.md` §6).

**Итог исполнения (15.09.2026, правки в рабочем дереве, не закоммичены):** п.1–5
закрыты, п.7 закрыт частично (см. исправленный критерий в §11). Замеры, таблицы
и то, где лежат тесты — **§11**. Пункт 6 закрыт раньше (Stage 6.21).

**Статус на момент переноса в конвейер:** п.6 (клавиатурный API) уже закрыт —
Stage 6.21 `debugger/docs/Pipeline/Stage_6.21_MCP_Keyboard_Injection.md`,
commit `4e859ed`. Пункт оставлен в документе как источник критериев приёмки,
требующих проверки на TESTAY.

---

## 0. Фикстуры и как запустить приёмку

```
FIX=/home/alexey/Projects/vector-games/roms/redesign/testay
ROM=$FIX/src/TESTAY.ROM     3712 байта, origin 0x0100,
                            SHA-256 8170faa43034a3ed8ca573be3a854184cdd1751457e74befad83e5289f4d3db5
RDB=$FIX/src/TESTAY.rdb     65 объектов / 51 связь (нужна для п.7)
```

Приёмка п.7 проверяется без ручных seed'ов: `debug_analyze_code(addresses=
[0x0000, 0x0100], max_instructions=10000)` и сверка с числами в §7. Приёмка
п.1–п.3 — прогон `debug_run` и `debug_get_memory_access_map` /
`debug_get_memory_access_log` / `debug_get_io_trace`.

Юнит-тестов `ctest` в этом проекте **нет** — каждый тест самостоятельный
executable со своим `main()`. Запускать так:

```
cd debugger/build && cmake .. && make -j8
./test_runtime_accounting   # §1–§4, включая границу JSON mcp_json   (6/6)
./test_backend              # регрессия шагового пути                (121/121)
./test_agent_api            # §4: getMemoryMap/execute_activity
./test_code_analyzer        # §7: классификатор условных переходов   (48/48)
```

---

## 1. P0 — выборка инструкций (fetch) не учитывается в свободном прогоне

### Что наблюдаено

`debug_run` → `debug_get_memory_access_map` (TESTAY, ~2 мин работы):

```jsonc
{ "block": "0x0400", "read": true, "read_count": 140251848,
  "write": true, "write_count": 302824, "fetch": false, "fetch_count": 0 }
```

`debug_get_memory_access_log` (50000 записей): `read` 49403, `write` 597,
**`fetch` 0**. При этом у «чтений» выполняется признак выборки:
`address == pc + 1`, напр. `read 0x04A4 @ pc 0x04A5`.

### Корень

Механизм есть, но он заводится только на пути шага:

```
src/backend.cpp:482   fetchRemaining_ = r.length;      // только в stepInstructionDetailed()
src/backend.cpp:741   bool isFetch = fetchRemaining_ > 0;   // onMemoryRead
                      // :743 Fetch++, :747 иначе Read++
```

В свободном прогоне `fetchRemaining_` всегда 0, и ядро не различает типы:

```
src/board.cpp:190      instr_time += i8080_instruction(&last_opcode);
src/i8080.cpp:2005     *report_opcode = RD_BYTE(PC++);   // тот же путь, что и чтение данных
src/hal.cpp:23         i8080_hal_memory_read_byte(addr) { return memory->read(addr, false); }
src/memory.cpp:58      if (onread) onread(addr, phys, stackrq, value);   // тега M1 нет
```

Сериализация `Fetch` в JSON исправна (`debugger/mcp/mcp_json.cpp:477`) — тип просто
никогда не выставляется.

### Что сделать

Нужен источник признака «это чтение по адресу выборки» для **свободного** прогона.
Два варианта, выбрать один; оба маленькие.

**Вариант A (предпочтительный, честный) — тег M1 в ядре.**
1. `Memory::read(uint16_t addr, bool stackrq, bool fetch = false)`, в `onread`
   добавить параметр `fetch`.
2. В `i8080_instruction()` (`src/i8080.cpp:2005`) читать опкод через fetch-вариант
   (`i8080_hal_memory_fetch_byte`, `src/hal.cpp`).
3. `onMemoryRead` в `debug_adapter`/`backend` пробрасывает флаг и помечает
   `MemoryAccessType::Fetch`, не полагаясь на `fetchRemaining_`.

**Вариант B (если ядро трогать нельзя) — окно выборки в бэкенде.**
1. В `Board` добавить хук, вызываемый **до** `i8080_instruction()`
   (`src/board.cpp:190`): `oninstrbegin(uint16_t pc)`.
2. В бэкенде по этому хуку декодировать длину инструкции уже имеющимся
   дизассемблером и выставлять `fetchRemaining_ = length` — т. е. переиспользовать
   ровно ту логику, что сейчас стоит в `stepInstructionDetailed()`
   (`src/backend.cpp:482`).

Независимо от варианта: **определение поля зафиксировать в документации** —
`fetch` = чтение байтов самой инструкции (опкод + операнды), `read` = чтение
данных по адресу из операндов. Иначе `read_count` снова начнут смешивать с
активностью данных.

### Критерий приёмки

```
debug_load_rom(TESTAY.ROM) → debug_run 10 s → debug_get_memory_access_map
```
* у блоков 0x0100–0x0600 `fetch_count > 0` и имеет порядок «миллионы», а
  `read_count` у тех же блоков падает на **2–3 порядка** относительно текущего
  значения (140 млн → единицы-сотни тысяч);
* в `debug_get_memory_access_log` присутствуют записи `"type": "fetch"`;
* для любой записи `fetch` выполняется `address == pc` или `address == pc + k`
  в пределах длины этой инструкции;
* шаг через `debug_step` даёт те же значения, что и свободный прогон на том же
  участке (регрессия текущего поведения не допускается).

---

## 2. P1 — `sequence` в трассе портов не растёт при свободном прогоне

**Наблюдено:** во всех событиях `debug_get_io_trace` (2 окна, 2000 и 3000
событий) `"sequence": 0`.

**Корень:** `instructionSequence_++` стоит только в пути шага —
`src/backend.cpp:531`. В свободном прогоне счётчик не двигается, поэтому все
события получают одно число и **потеряна привязка I/O к инструкции**.

**Сделать:** инкрементировать `instructionSequence_` в том же месте, что и учёт
fetch из п.1 (хук `oninstrbegin` / путь варианта A). Стоимость — одна строка.

**Критерий приёмки:** у подряд идущих событий `OUT 0x15`/`OUT 0x14` значения
`sequence` строго возрастают; между двумя соседними записями регистра AY
разница `sequence` стабильна (для TESTAY — 2 writes AY на кадр, т. е. порядок
можно использовать как метроном).

---

## 3. P1 — `active_blocks` не совпадает с полезной нагрузкой ответа

**Наблюдено:** `debug_get_memory_access_map` возвращает
`"active_blocks": 256` при 16 реально перечисленных блоках.

**Корень:** `debugger/mcp/mcp_adapter.cpp:1434` считает
`static_cast<int>(r.value.size())` — все 256 блоков карты, тогда как фильтр по
активности (`b.read || b.write || b.fetch`) применяется позже, в
`debugger/mcp/mcp_json.cpp:464`. Счётчик и payload считаются по разным правилам.

**Сделать:** считать `active_blocks` по тому же критерию, что и выдаваемый
список; отдельно добавить `total_blocks: 256`, чтобы не потерять информацию.

**Критерий приёмки:** `active_blocks == len(blocks)` для любого ROM в любом
состоянии; `total_blocks == 256`.

---

## 4. P2 — мёртвое поле `execute_activity`

**Наблюдено:** `execute_activity: 0` у всех блоков, всегда.

**Корень:** поле объявлено (`debugger/agent/agent_types.h:406`) и сериализуется
(`debugger/mcp/mcp_json.cpp:155`), но **нигде не присваивается**.

**Сделать** (одно из двух, по вкусу поддерживающего код): либо заполнять
(например — число исполненных инструкций, попавших в блок, либо флаг
«блок содержит код»), либо **удалить** поле из структуры и из JSON. Держать
константный ноль нельзя: потребитель читает его как измерение.

**Критерий приёмки:** поле удалено ИЛИ для TESTAY в блоках 0x0100–0x0600
ненулевое, монотонно растущее с временем работы значение.

---

## 5. P2 — `cycles` в `debug_get_state` недокументирован

Это **не дефект**, но поле вводит в заблуждение.

**Наблюдено:** `cycles: 10`, затем `cycles: 5` — выглядит как «счётчик стоит на
месте».

**Корень:** возвращается `i8080_cycles()`, который по определению —
«number of cycles taken by the last instr» (`src/i8080.h:42`,
`debug_adapter.cpp:306`), а не суммарные циклы.

**Сделать:** в описание MCP-инструмента и в JSON-поле (или переименованием
`last_instruction_cycles`) внести формулировку «циклы последней выполненной
инструкции». Дополнительно: если нужен общий счётчик — `Board` уже копит
`instr_time`; пробросить его как `total_cycles`, не ломая совместимость.

**Критерий приёмки:** описание инструмента не допускает прочтения поля как
накопительного; тест `tests/test_*state*` фиксирует, что `cycles` == длительность
последней инструкции.

---

## 6. P1 — клавиатурный ввод реализован, но не выставлен в AgentApi/MCP

**Наблюдено:** на Stage 1 нельзя проверить реакцию TESTAY на нажатие клавиши
(ветка `0x04B2`), потому что `IN 0x01` даёт 0xEF во всех 130 наблюдениях, и
моделировать нажатие нечем.

**Корень — это незакрытый слой exposed API, а не отсутствующая фича:**

```
debugger/src/backend.cpp:272   DebugBackend::pressKey(int scancode)
debugger/src/debug_adapter.cpp:484  DebugAdapter::pressKey → keyboard.apply_key(scancode, false)
debugger/src/debug_adapter.cpp:489  DebugAdapter::releaseKey → keyboard.apply_key(scancode, true)
src/keyboard.h:73-82           LSHIFT→ss, LCTRL→us, LALT/LGUI/F6→rus (с инверсией уровня)
src/vio.h:84-88                IN порт 0x01, старшая полубайт:
                               bit4 = tape sample, bit5 = ~ss, bit6 = ~us, bit7 = ~rus
```

Т. е. весь путь до регистра порта уже работает, наружу не выведено ни в
`debugger/agent/agent_api.cpp`, ни в набор `debug_*` инструментов.

**Сделать:** три инструмента (минимально достаточный набор):

| Инструмент | Параметры | Семантика |
|---|---|---|
| `debug_press_key` | `scancode` (int, значения SDL2) | удержание нажатой клавиши |
| `debug_release_key` | `scancode` | снятие |
| `debug_get_keyboard_state` | — | `{ss, us, rus, port01_readback}` — чтобы видеть эффект до трассы |

Требования к API:
* аргумент — **SDL2 scancode** (`SDL_SCANCODE_*`), в описании инструмента обязана
  быть таблица клавиш, значимых для Vector: `F6=63`, `LCTRL=224`, `LSHIFT=225`,
  `LALT=226`, `LGUI=227` (модификаторы РУС/СС/УС) — иначе агент будет гадать;
* допустимо принять и символьное имя (`"F6"`), но не вместо, а в дополнение к числу;
* нажатие работает и в паузе, и в свободном прогоне; состояние матрицы
  сохраняется до `debug_release_key` (не автотик).

**Критерий приёмки (проверяемый на TESTAY):**

```
debug_load_rom(TESTAY.ROM) → debug_run → debug_pause
debug_press_key(63)            ; F6 = РУС
IN 0x01 (через трассу/чтение порта) == 0x6F      ; бит7 = 0
→ в трассе появляется ветка 0x04B2 (MVI A,07 / OUT 15 / ... / OUT 14)
```

Сейчас при отпущенной клавишине `IN 0x01` = 0xEF, и эта ветка не достижима —
см. Stage1.md §2.3 и §4.

---

## 7. P0 — анализатор кода не идёт по условным переходам (несмотря на описание)

**Наблюдено (TESTAY.ROM):** `debug_analyze_code(addresses=[0x0000,0x0100],
max_instructions=10000)` → 643 инструкции, 29 `references`, **все типа `JMP` или
`CALL`**. Условных (`JCC`/`CALLCC`) — ни одного, хотя в списке инструкций они
есть: `0x01CF CZ 0158`, `0x020B JC 023C`, `0x0210 JC 024D`, `0x0218 JZ 026B`,
`0x0431 JZ 0437`, `0x04AF JC 04BB`, `0x01B5 JNZ 033A`, `0x0571 JNZ 056B` —
215 целей по всем условным переходам ROM.

Цена: не дособрано **244 байта кода** — блоки 0x0158–0x01A1 (74),
0x023C–0x02C2 (135), 0x032E–0x0331, 0x0437–0x0441, 0x04BB–0x04CC, 0x053D–0x053E.
Все они — обычная достижимая из ROM подпрограмма и обработчики; недоступность
только из-за того, что вход в них условный. Именно эти интервалы фигурируют
как «потерянные 170 байт» в `TZ-exporter.md` §2.3.

**Корень — мёртвая маска в классификаторе**, `debugger/agent/code_analyzer.h:98-110`:

```cpp
uint8_t lo = opcode & 0x0F;
// Jcc: C2,JNZ  CA,JZ  D2,JNC  DA,JC  E2,JPO  EA,JPE  F2,JP  FA,JM
if ((opcode & 0xC7) == 0xC0 && lo != 0x09 && lo != 0x01 && ...) {
    if (lo == 0x02 || lo == 0x0A)
        return ControlFlowType::ConditionalJmp;   // недостижимо
}
if ((opcode & 0xC7) == 0xC0 && (lo == 0x04 || lo == 0x0C))
    return ControlFlowType::ConditionalCall;      // недостижимо
```

Условие `(opcode & 0xC7) == 0xC0` требует нулевых биты 0–2 (`opcode & 0x07 == 0`),
тогда как у всего семейства Jcc/Ccc биты 0–2 равны `010`/`100`/`110` (условие
закодировано в битах 3–5, а бит1 всегда 1). Проверка покрывает только opcodes
`C0,C8,D0,D8,E0,E8,F0,F8` — т. е. **только Rcc**, у которых `lo` равен 0x00/0x08.
Поэтому `ConditionalRet` работает, а `ConditionalJmp`/`ConditionalCall`
не возвращаются **никогда**; переход попадает в `default:` → `Sequential`,
BFS идёт только на fall-through.

Второе место: `debugger/src/disassembler.cpp:241-246` поле `hasTarget`/
`target` выставляет для `F_JMP`/`F_CALL` корректно (условные тоже), так что
цель есть — её только не читают. Подтверждение: в `debug_disassemble_range`
у `JC 0165` поле `branch_target: null`, а у `CALL 0105` — `261`. Значит
`hasTarget` теряется при конвертации в MCP-структуру (agent/`code_analyzer.h:195`
берёт `di.target`, но не `di.hasTarget`) — проверить по пути `toAgentInstruction`.

**Сделать:**
1. Починить маску: базовый тест `(opcode & 0xC0) == 0xC0`, далее разбирать
   `lo = opcode & 0x0F`: `2/A` → ConditionalJmp, `4/C` → ConditionalCall,
   `0/8` → ConditionalRet, `7/F` → Restart; не забыть исключить `0xC3/C9/CD/E9/76`,
   уже обработанные выше.
2. BFS (`code_analyzer.h:225-258`) уже пушит цель для этих типов — правки
   п.1 достаточно; reference'ы `JCC`/`CALLCC` (`:209`, `:215`) начнут появляться.
3. Перенести классификацию в общий заголовок/`.cpp`, чтобы `debug_analyze_code`
   и экспортёр (`export/asm_exporter.cpp:217 analyzeCodeMulti`) использовали
   один и тот же код — сейчас экспортёр наследует ту же ошибку.
4. Тест: ROM с `JC` в несмежный блок — цель должна попасть и в `ranges`,
   и в `references` с `type: "JCC"`.

**Критерий приёмки на TESTAY.ROM** (прогон `debug_analyze_code` только от
`[0x0000, 0x0100]`, без ручных seeds):

> ⚠ **Ниже записан критерий, который измерения 15.09 опровергли.** Число
> «≥ 871 инструкций от двух сидов» недостижимо ни до, ни после фикса: из
> `[0x0000, 0x0100]` статически достижимо 94 инструкции в окне ROM. Исправленный
> критерий и полные таблицы — в **§11**, приёмку сверять по нему.

```
instruction_count ≥ 871        (сейчас 643; 871 — сколько даёт полный набор seeds
                                из RDB: 32 функции + 2 Label)
code_bytes        ≥ 1325       (сейчас 932); из них 1069 — в окне ROM 0x0100..0x0F7F,
                                остальное — NOP-sled 0x0000..0x00FF
references        содержат JCC/CALLCC (сейчас 0 шт.)
ranges            покрывают 0x0158, 0x023C–0x02C2, 0x032E, 0x0437, 0x04BB
```

**Связка с `TZ-exporter.md` §2.3.** Экспортёр сеет анализ адресами объектов
`Function`/`Label`/`Code` из RDB (`export/asm_exporter.cpp:205-213`, там же:
«Seeding all function addresses guarantees coverage»). Измерено на Stage 2:
полный набор функций в RDB возвращает 155 из 170 потерянных байт, а остаток
(15 байт — цели `JZ` **внутри** функций, 0x032E и 0x0437) закрылся только двумя
вручную заведёнными `Label`. То есть seeds — рабочая временная затычка, но она
требует ручного труда ровно там, где анализатор слеп. После п.7 эти `Label`
становятся избыточными (оставить можно — вреда нет).
Пункты §2.3 (отчёт о покрытии, `gap_%04X`, ненулевой exit code) при этом
**остаются** — они про обнаружение потери, а не про её причины.

---

## 8. Не дефекты — не выдывать повторно

Проверено по исходникам; в этом ТЗ сознательно **не** входят:

| Поле | Почему всё правильно |
|---|---|
| `classification: "unknown"` у всех блоков в `debug_get_memory_access_map` | классификация берётся из регионов `SymbolDatabase` (`debugger/agent/agent_api.cpp:1029-1045`); на Stage 1 RDB пуста. Заполнится после Stage 2 — это ожидаемое поведение |
| `cycles` как значение (не как название) | см. п.5: число верное, претензия только к документации |

Отдельно: расхождение документации по биту 4 порта C (`keyboard.md` — «Tape
input: bit 4», `verification.md` §8.1 — «бит0 = магн») **разрешилось по коду
эмулятора**: `src/vio.h:84-88` — бит 4 = сэмпл ленты, бит 5/6/7 =
инверсные СС/УС/РУС, младший полубайт = `PC & 0x0F`. Статус: *Emulator
Behavior*, для оригинального железа не верифицировано. Правка — в базу знаний
(`agent/knowledge/vector06c/verification.md` §8.1), не в код; к п.1–6 отношения
не имеет.

---

## 9. Вне рамок этого ТЗ

`src/opcode_info.h` (ALU уже починено, длины верны),

`v06c-asm-export` (компоновка, клиппинг) — см. `TZ-exporter.md`,
пункты 2.1–2.2. Покрытие кода (§2.3) после п.7 закрывается семенами из RDB.
Формат `.rdb`, GUI, автокомментарии `debug_analyze_code`,
реальная аппаратная верификация таймингов AY/развёртки.

## 10. Завязка на проект-потребителя

Пока не закрыты п.1, п.6 и п.7, анализ ROM вынужденно обходится косвенными
признаками: код от данных отличают проверкой `address == pc + 1` в
`debug_get_memory_access_log` вместо `fetch_count`, а клавиатурные ветки вообще
не проверяемы трассой. После п.1 Stage-отчёты можно строить на карте памяти
напрямую, после п.6 — закрывать сценарий «выход/реакция по клавиатуре».

---

## 11. Итог исполнения и замеры (15.09.2026)

Правки внесены в `/home/alexey/Projects/vector-debugger`, **без коммита**
(ревью делает другой агент). Скрипты патчей и замеров —
`roms/redesign/testay/.rt/` (`fix7_runtime.py`, `fix8_cmake.py`,
`fix9_json_boundary.py`, `measure_analyze.cpp`, `run_measure.sh`).

### 11.1 Пункты 1–5

Выбран **вариант B** из п.1 (хук в ядре, а не тег M1): `RD_BYTE` в
`src/i8080.cpp` используется и для опкода, и для операндов, и для чтения
данных, поэтому на уровне ядра отличить выборку от данных нельзя без длины
инструкции — а длина как раз и известна в бэкенде.

| п. | Что сделано | Где проверено | Результат |
|---|---|---|---|
| 1 | `std::function<void(int pc)> Board::oninstrbegin` (`src/board.{h,cpp}`), вызов **до** `i8080_instruction()`; проброс через `IDebugTarget::setInstructionBeginCallback` → `DebugAdapter`/`NoBoardTarget` → `DebugBackend::onInstructionBegin()`, где выставляется `fetchRemaining_` и `fetchBasePc_` | `test_runtime_accounting`: *free run counts instruction fetches separately from data reads* | `fetch_count` 0 → **110**; `read_count` в блоке кода 110 → **0** (оперы больше не «чтения данных») |
| 1 | journal `pc` = адрес инструкции-инициатора, а не текущего PC | *log entries carry the accessing instruction* | 11 записей `fetch` на цикл, каждая в `pc..pc+2`; чтение данных `LDA` записано на `0x0105` |
| 1 | регрессия «шаг = прогон» запрещена | *debug_step and the free run produce the same access map* | **0** расходящихся блоков из 256 |
| 2 | `instructionSequence_++` в том же хуке (флаг `steppingInProgress_` не даёт считать дважды) | *instruction sequence grows in the free run* | 0 после 1-й инструкции, **9 после 10**; шаг: 3 после 3 |
| 3 | `mcp_json::runtimeBlockActive()` — единственный предикат; `active_blocks` считается по нему, добавлен `total_blocks` | *active block count matches the serialised block list* | `active_blocks == len(blocks)`; fetch-only блок 0x0100 попал в ответ (раньше был не виден) |
| 4 | `execute_activity` = сумма `activitySnapshot().executeCount` по окну блока | `test_agent_api`: *getMemoryMap sums execute_activity per block*; `test_runtime_accounting`: *per-address execute counters grow* | блок1 = **3**, блок2 = **6**, пустые = 0; в свободном прогоне счётчики растут |
| 5 | `cycles` переформулирован в описаниях `debug_get_state` / `debug_get_cpu_state` как «длительность последней инструкции» | текст инструментов `mcp_adapter.cpp` | — |

**Попутный дефект, найденный при п.4:** суммирование по блоку писалось как
`for (uint16_t a = block.start; a <= block.end; ++a)`; у последнего блока
`end == 0xFFFF`, счётчик переполняется и цикл не завершается —
`debug_get_memory_map` **вешал процесс**. Исправлено на `int`; тест
`getMemoryMap sums execute_activity per block` служит сторожем (он бы висел).

`test_runtime_accounting` на `4e859ed` (до правок) даёт **0/6** с ровно теми
симптомами, что описаны в п.1–4: `fetch_count expected 110, got 0`,
`read_count expected 0, got 110`, sequence застыл на 0, все `executeCount` 0.

### 11.2 Пункт 7 — замеры анализатора

Четыре замера (`run_measure.sh`). `sled` = сиды `[0x0000, 0x0100]` (условие
«без ручных seeds»), `rdb` = 34 адреса объектов `Function|Label|Code` из
`TESTAY.rdb` — то, что сеет экспортёр. `old` = `code_analyzer.h` из `4e859ed`
(состояние до п.7; позже этот коммит стал HEAD, поэтому «до» закреплено номером),
`new` = рабочее дерево. Бюджет `max_instructions = 10000` не исчерпан ни в одном
прогоне.

| метрика | sled old | sled new | rdb old | rdb new |
|---|---:|---:|---:|---:|
| instruction_count | 350 | 350 | 615 | 615 |
| → в окне ROM | 94 | 94 | 615 | 615 |
| code_bytes | 434 | 434 | 1069 | 1069 |
| → в окне ROM | 178 | 178 | 1069 | 1069 |
| ranges | 5 | 5 | 4 | 4 |
| references | 12 | **15** | 52 | **83** |
| · CALL | 9 | 9 | 39 | 39 |
| · CALLCC | 0 | 0 | 0 | **1** |
| · JCC | 0 | **3** | 0 | **30** |
| · JMP | 3 | 3 | 13 | 13 |
| · RST | 0 | 0 | 0 | 0 |
| conflicts | 0 | 0 | 0 | 0 |
| truncated | no | no | no | no |

Эффект фикса маски на TESTAY — **только список ссылок**: +3 в sled, +31 в rdb.
Покрытие не меняется ни в одной конфигурации (615 инструкций, 1069 байт кода,
4 range — и до, и после), потому что все блоки, куда ведут условные переходы, уже
засеяны объектами RDB: `0x0158`, `0x023C`, `0x032E`, `0x0437`, `0x04BB`, `0x053D`
— это адреса `Function`/`Label`. Дособирание цели `Jcc` end-to-end на этом ROM
наблюдаемо быть не может; его сторожит `test_code_analyzer`.

Все 8 блоков из перечня ТЗ лежат в `ranges` в обеих версиях. Единственный
непокрытый участок ROM — `0x0470–0x0471` (2 байта между `var_046f_init` и
`data_a_channel_state`): пробел атрибуции RDB, а не дефект анализа; экспортёр
помечает их `gap_0470`, побайтовая точность образа от этого не страдает.

**Erratum.** Первая версия раздела снята дефектным харнессом: `run_measure.sh`
передавал сиды десятичной строкой (`str(int('0x0100', 16))` → `"256"`), а
`measure_analyze.cpp` читал `argv` по основанию 16. Каждый адрес подменялся
(`0x0100 → 0x0256`, `0x0105 → 0x0261`, …), анализатор стартовал из середины
инструкций и из несуществующих объектов. Оттуда взялись «1757 → 1887 инструкций
в окне ROM», «24 из 34 сидов — переменные в ОЗУ, бюджет исчерпан», «4 блока
недостижимы, нужен анализ `JP (HL)` → новый пункт ТЗ» и «`conflicts` 0 → 2»: на
корректных сидах не подтверждается ни одно. Правки харнесса — `.rt/run_measure.sh`
(основание 16, «до» закреплено за `4e859ed`) и `.rt/measure_analyze.cpp` (печать
конфликта с адресом, хозяином и ведущим переходом). На код ошибка не влияла:
экспортёр сеет анализатор из C++ напрямую, поэтому приёмка §3 и числа
`export.json` были верны всё это время.

### 11.3 Пункт 7 — исправленный критерий приёмки

Замечание к исходному: «≥ 871 инструкция» не является ни нижней, ни верхней
границей — это число получено на **old** коде и в другой конфигурации прогона
(через MCP). Корректные инварианты:

```
1. references содержат JCC и CALLCC (на 4e859ed — ноль)         ✅ 30 и 1 (rdb), 3 (sled)
2. code_bytes в окне ROM = 1069 и совпадает с export.json       ✅ (столько же на old — см. 11.2)
3. цели Jcc уходят в очередь BFS (страж — test_code_analyzer)   ✅ 48/48, red на 4e859ed
4. conflicts = 0 на полной RDB-сетке сидов                      ✅
5. 0x0470–0x0471 (2 байта) — пробел атрибуции RDB, не анализ    → Stage 3
```

### 11.4 Что осталось непроверенным

* Приёмка п.1–3 **живым MCP** (`debug_run` + `debug_get_memory_access_map`) —
  текущий запущенный `v06c-mcp` собран до правок (процесс с 16:43, бинарник
  пересобран в 19:11), перезапуск обрывает сессию пользователя. Юнит-тесты
  закрывают `DebugBackend` + `mcp_json`; не закрыт только последний ярлык
  `DebugAdapter::setInstructionBeginCallback → board.oninstrbegin`, а он
  буквальный (2 строки) и совпадает с `setMemoryCallbacks` того же файла.
* `test_agent_api` (108/111) и `test_agent_mock` (SIGSEGV) падают **и на HEAD**
  — проверено в отдельном worktree. `test_agent_api` на HEAD вообще падает
  раньше (rc=134, `std::bad_optional_access` в тесте `removeRdbLink`). Три
  фейла — устаревшие ожидания VRAM (коммит `fa31edb` вернул 4 плоскости, тесты
  ждут 2/1) и `disassembleRange` за границей 16384. К этому ТЗ не относятся.
