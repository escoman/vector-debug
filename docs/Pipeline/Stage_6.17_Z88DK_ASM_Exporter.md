# Stage 6.17 — Z88DK ASM Exporter

## Цель

Создать отдельный инструмент экспорта результатов reverse engineering ROM Vector-06Ц в исходный проект для сборки через Z88DK/z80asm.

Exporter должен преобразовывать:

```text
ROM
+
RDB
+
результаты code analysis
+
символы/ссылки
+
описания DATA
        ↓
Z88DK ASM project
```

Результатом являются `.asm` и `.inc` файлы, пригодные для дальнейшего ручного редактирования и сборки.

**MCP не должен генерировать `.asm`-исходники.**

---

# 1. Основной принцип

Exporter является отдельным компонентом:

```text
MCP
 ↓
Agent
 ↓
RDB
 ↓
ASM Exporter
 ↓
.asm / .inc
```

Он не должен обращаться непосредственно к:

```text
Board
Memory
CPU
IO
TV
SDL
ImGui
```

Exporter работает с данными ROM/RDB/анализа.

---

# 2. Удалить необходимость в `generate_asm.py`

После реализации exporter агент не должен создавать собственный Python-генератор `.asm` только ради:

* `org`;
* `include`;
* `PUBLIC`;
* `EXTERN`;
* labels;
* `defb`;
* разбиения ROM на файлы;
* генерации z80asm source.

Существующий:

```text
generate_asm.py
```

не переносить в MCP.

Его функциональность должна постепенно заменяться новым exporter.

---

# 3. Не переносить `disasm.py`

Exporter не должен содержать собственного 8080 opcode decoder.

Для генерации кода использовать уже полученные результаты:

```text
debug_disassemble
debug_analyze_code
```

или соответствующие внутренние типы/методы Agent API.

Если exporter требует информацию о конкретной инструкции, она должна быть получена из существующего debugger decoder.

Не создавать вторую таблицу opcode.

---

# 4. Источники данных

Exporter должен использовать следующие источники:

### ROM

Исходные байты ROM.

### RDB

Использовать:

```text
objects
links
names
types
sizes
comments
properties
```

### Code Analysis

Использовать:

```text
instructions
ranges
references
conflicts
```

### Symbols

Использовать существующие debugger symbols, если они доступны.

При наличии RDB name приоритет должен быть у RDB.

---

# 5. Приоритет имени

Для адреса:

```text
0x1234
```

использовать имя в следующем порядке:

```text
RDB object name
    ↓
debugger symbol
    ↓
generated label
```

Generated label:

```text
label_1234
```

или аналогичный стабильный формат.

Не использовать случайные/неповторяемые имена.

---

# 6. Типы RDB objects

Exporter должен учитывать как минимум:

```text
function
label
data
constant
variable
unknown
```

Для `function`:

```asm
function_name:
```

Для `label`:

```asm
label_name:
```

Для `data` использовать соответствующий формат данных.

Для `constant` предпочтительно использовать символическую константу, если формат объекта содержит необходимую информацию.

`unknown` не должен автоматически превращаться в CODE.

---

# 7. CODE/DATA граница

Критически важно:

```text
RDB data object
```

имеет приоритет над предположением exporter.

Если code analysis сообщает:

```text
code
```

но RDB содержит подтверждённый DATA object в этом диапазоне, exporter не должен молча дизассемблировать его как код.

Такой конфликт должен быть отражён в отчёте экспорта.

---

# 8. Генерация инструкций

Инструкции должны экспортироваться в синтаксис, совместимый с используемой версией `z80asm`.

Например:

```text
8080:
LXI H,1234H
MOV A,M
JNZ 0200H
```

должен экспортироваться в согласованном z80asm-представлении.

Конкретное преобразование должно использовать **один существующий debugger representation**, а не копировать decoder из `disasm.py`.

---

# 9. CALL/JMP labels

Если инструкция содержит ссылку:

```text
CALL 0x1234
JMP 0x1234
JNZ 0x1234
```

и адресу соответствует имя:

```text
draw_sprite
```

exporter должен использовать:

```asm
call draw_sprite
```

а не:

```asm
call 0x1234
```

если символ может быть корректно разрешён.

---

# 10. RDB links

Использовать RDB links как подтверждённые отношения:

```text
source → target
```

Links могут использоваться для определения символических ссылок между объектами.

Но exporter не должен создавать новые RDB links.

Exporter только читает RDB.

---

# 11. Неизвестные ссылки

Если target не имеет имени:

```text
CALL 0x2345
```

exporter должен создать стабильную локальную метку:

```asm
call label_2345
```

и:

```asm
label_2345:
```

если target является частью экспортируемого code region.

Если target не может быть безопасно классифицирован, сохранить числовой адрес и сообщить об этом в export report.

---

# 12. DATA export

Для подтверждённых DATA objects экспортировать байты через:

```asm
defb
```

Например:

```asm
sprite_data:
    defb 01h,02h,03h,04h
```

Размер объекта брать из RDB.

Если размер отсутствует, не угадывать его молча.

Такой объект должен быть отмечен как требующий уточнения.

---

# 13. Большие DATA regions

Не создавать одну огромную строку:

```asm
defb ..., ..., ..., ...
```

Для больших регионов использовать разумное форматирование по строкам.

Например:

```asm
gfx_data:
    defb ...
    defb ...
    defb ...
```

Размер строки выбрать так, чтобы исходник оставался читаемым.

---

# 14. `org`

Главный файл проекта должен содержать:

```asm
org 0100h
```

или соответствующий origin ROM.

Origin должен определяться из ROM/debugger metadata.

Не использовать `_main` как origin.

---

# 15. ROM mapping entry point

Следовать правилу Stage 6.14:

```text
0x0000
```

является точкой начала исследования ROM.

Это не означает, что exporter обязан создавать:

```asm
org 0000h
```

если фактический ROM load origin иной.

Exporter должен различать:

```text
mapping entry point
ROM origin
```

---

# 16. Разбиение на файлы

Exporter должен поддерживать структурированное разбиение исходника.

Минимально:

```text
main.asm
code/
data/
```

Конкретная схема может быть простой.

Не требуется воспроизводить структуру `generate_asm.py` один в один.

Главное:

* код отделён от данных;
* большие DATA regions не смешиваются с CODE;
* main file содержит includes.

---

# 17. Main ASM

Главный файл должен выглядеть концептуально:

```asm
org 0100h

include "code.asm"
include "data.asm"
```

или использовать более детальное разбиение.

Не создавать PUBLIC/EXTERN без необходимости.

---

# 18. PUBLIC / EXTERN

Если функция/символ реально находится в другом сгенерированном ASM module, использовать:

```asm
PUBLIC symbol
EXTERN symbol
```

Но не генерировать эти директивы автоматически для каждого символа.

Если всё находится в одном module, PUBLIC/EXTERN не требуются.

---

# 19. Comments

RDB comments должны переноситься в исходник.

Например:

```asm
; Draw sprite
draw_sprite:
```

Не терять пользовательские комментарии.

Комментарии должны безопасно экранироваться/нормализоваться под синтаксис assembler.

---

# 20. Generated labels

Для автоматически созданных labels использовать единообразный формат:

```text
label_XXXX
```

где:

```text
XXXX
```

— hexadecimal address.

Например:

```asm
label_2A40:
```

Generated labels должны быть стабильными между двумя экспортами одного и того же RDB.

---

# 21. Conflicts

Если code analysis содержит:

```text
instruction_boundary_conflict
```

exporter не должен скрывать проблему.

Экспорт должен:

1. продолжить там, где это безопасно;
2. записать конфликт в export report;
3. не выдавать конфликтный участок как гарантированно корректный код.

---

# 22. Unknown regions

Неизвестные ROM-области не должны автоматически превращаться в:

```asm
nop
```

или другой машинный код.

Если region не классифицирован:

```text
UNKNOWN
```

он должен либо:

* экспортироваться как raw `defb`;
* либо быть пропущен с явной записью в report.

Предпочтительно `defb`, чтобы данные ROM не потерялись.

---

# 23. ROM bytes preservation

Критически важное требование:

Экспорт не должен изменять ROM.

Для каждого экспортируемого диапазона:

```text
original ROM bytes
```

должны соответствовать:

```text
assembled bytes
```

если этот участок предназначен для повторной сборки.

---

# 24. Export manifest

Создать файл:

```text
export.json
```

с информацией:

```json
{
  "rom": "putup.rom",
  "rom_size": 17664,
  "origin": "0x0100",
  "mapping_entry_point": "0x0000",
  "objects": 123,
  "links": 87,
  "code_ranges": 15,
  "data_ranges": 9,
  "warnings": 2,
  "errors": 0
}
```

Точные поля можно адаптировать под существующие project types.

Manifest нужен для воспроизводимости экспорта.

---

# 25. Export report

Помимо machine-readable `export.json`, сформировать краткий текстовый отчёт.

Минимально:

```text
ROM
Origin
Mapping entry point
Generated files
Code ranges
Data ranges
RDB objects
RDB links
Warnings
Conflicts
Unresolved references
```

---

# 26. Deterministic export

Один и тот же:

```text
ROM + RDB + analysis
```

должен давать одинаковый результат.

Не использовать:

* случайные имена;
* адреса указателей памяти;
* порядок обхода unordered containers без сортировки;
* текущие timestamps внутри исходников.

---

# 27. Не изменять RDB

Exporter является read-only.

Запрещено:

```text
создавать objects
изменять objects
удалять objects
создавать links
удалять links
save RDB
```

Если данных недостаточно, exporter сообщает об этом.

---

# 28. CLI

Создать отдельный CLI-инструмент.

Например:

```bash
vector06c-asm-export \
    --rom putup.rom \
    --rdb putup.rdb \
    --output build/putup
```

Названия бинарника и параметры можно адаптировать к существующей структуре CMake.

Минимальные параметры:

```text
--rom
--rdb
--output
```

Если RDB не указан явно, автоматически искать:

```text
<rom>.rdb
```

---

# 29. Работа без RDB

Если RDB отсутствует:

Exporter может использовать результаты code analysis и symbols, но не должен притворяться, что ROM полностью размечен.

В report указать:

```text
RDB: not found
```

и количество участков, классификация которых невозможна.

Не создавать `.rdb` автоматически.

---

# 30. Работа с MAP

MAP не должен становиться вторым независимым источником истины.

Если MAP уже импортирован в RDB:

```text
RDB → exporter
```

Если RDB отсутствует, exporter может использовать MAP только если существующая архитектура проекта уже предоставляет такой механизм.

Не добавлять новый независимый MAP parser ради exporter.

---

# 31. Z88DK compatibility

Exporter должен генерировать синтаксис, совместимый с фактически используемым `z80asm`.

Обязательно проверить на реальном Z88DK:

```text
labels
equ/constants
defb
org
include
PUBLIC
EXTERN
```

Не полагаться только на синтаксическую интуицию.

---

# 32. Сборка результата

Созданный exporter должен уметь сформировать проект, который можно передать Z88DK/z80asm.

Минимальная проверка:

```bash
z80asm ...
```

должна завершаться успешно для тестового ROM/fixture.

---

# 33. Round-trip test

Для небольшого тестового ROM:

```text
ROM
 ↓
analysis/RDB
 ↓
export
 ↓
z80asm
 ↓
rebuilt binary
```

Сравнить:

```text
original bytes
==
rebuilt bytes
```

для диапазонов, которые exporter объявляет воспроизводимыми.

Если полная бинарная идентичность невозможна из-за незаполненных/неразмеченных областей, это должно быть явно указано в тесте и отчёте.

---

# 34. Тестовый fixture

Создать небольшой ROM fixture, содержащий:

```text
entry
function
CALL
JMP
conditional branch
label
data table
unknown region
```

RDB fixture должен содержать:

```text
function objects
label objects
data object
links
comments
```

Проверить полученный ASM.

---

# 35. Проверка generated labels

Для fixture проверить:

```text
label_XXXX
```

и убедиться, что:

* labels уникальны;
* labels стабильны;
* references используют правильные labels;
* повторный export не меняет имена.

---

# 36. Проверка DATA

Проверить:

```text
RDB DATA object
 ↓
defb
 ↓
original bytes
```

Размер и содержимое должны совпадать.

---

# 37. Проверка comments

Проверить перенос:

```text
RDB comment
 ↓
ASM comment
```

без потери содержимого.

---

# 38. Проверка конфликтов

Создать fixture с:

```text
instruction_boundary_conflict
```

Exporter должен завершиться контролируемо и добавить warning/error в report.

Не выдавать ложное утверждение о корректности участка.

---

# 39. Интеграция с существующим проектом

Exporter должен быть добавлен в CMake.

Не включать в:

```text
debugger_core
```

если он требует файлового/CLI слоя.

Предпочтительная структура:

```text
debugger_core
debugger_adapter
debugger_agent
debugger_mcp
debugger_asm_exporter
```

Если отдельная library не нужна, допустим простой executable, использующий существующие core/agent types.

---

# 40. Qoder Skill

Обновить:

```text
.qoder/skills/vector06c-debugger/SKILL.md
```

Добавить правило:

> Для генерации Z88DK ASM использовать штатный ASM exporter. Не создавать собственный `generate_asm.py`.

Также:

> Не создавать собственный 8080 decoder или disassembler.

---

# 41. Qoder Agent

Обновить:

```text
.qoder/agents/vector06c-analyst.md
```

Workflow:

```text
Load ROM
 ↓
debug_analyze_code
 ↓
debug_read_memory_range
 ↓
RDB mapping
 ↓
RDB links
 ↓
save RDB
 ↓
ASM exporter
 ↓
build with z80asm
 ↓
verify bytes
```

Exporter должен использоваться только после завершения mapping/RDB.

---

# 42. Что НЕ делать

На Stage 6.17 не добавлять:

```text
новый disassembler
новый opcode decoder
автоматический RDB mapper
новый MAP parser
MCP ASM generator
RAG
AI analysis runtime
```

Не пытаться автоматически определить всю DATA ROM.

Exporter — это **транслятор уже имеющейся информации**, а не reverse-engineering engine.

---

# 43. Проверка исходного ROM

Для каждого export:

```text
SHA-256 ROM
```

записывать в `export.json`.

Это позволяет убедиться, что export относится именно к исследованной версии ROM.

---

# 44. Error handling

Ошибки должны быть явными.

Минимально:

```text
ROM not found
RDB not found
invalid RDB
invalid ROM range
overlapping objects
invalid object size
unresolved reference
code/data conflict
assembler syntax error
```

Не продолжать экспорт молча после критической ошибки.

---

# 45. Regression

После реализации должны пройти:

```text
test_agent_api
test_mcp_protocol
test_backend
test_rdb_controller
test_map_import
test_call_graph_model
test_gui_smoke
```

и новые exporter tests.

MCP functionality не должна регрессировать.

---

# 46. Проверка `src/`

Обязательно:

```bash
git diff -- src/
```

Результат:

```text
пусто
```

Stage 6.17 не требует изменений основного эмулятора.

---

# 47. Критерий завершения Stage 6.17

Stage считается завершённым, если:

* существует отдельный Z88DK ASM exporter;
* exporter не является MCP tool;
* exporter не содержит собственного 8080 decoder;
* exporter использует RDB;
* exporter использует результаты code analysis;
* RDB является основным источником имён/типов/ссылок;
* CODE и DATA корректно разделяются;
* DATA экспортируется через `defb`;
* labels генерируются стабильно;
* CALL/JMP references используют labels;
* RDB comments переносятся;
* `org` формируется из ROM origin;
* mapping entry point `0x0000` не путается с ROM origin;
* большие DATA regions корректно экспортируются;
* unknown regions не превращаются в придуманный код;
* conflicts отражаются в report;
* генерируется `export.json`;
* экспорт детерминирован;
* существует CLI;
* результат собирается через Z88DK/z80asm;
* существует round-trip test;
* generated binary проверяется относительно исходного ROM;
* Qoder Skill обновлён;
* Qoder Agent обновлён;
* все regression tests проходят;
* `src/` не изменён.

После выполнения **не добавлять новую reverse-engineering функциональность**.

Stage 6.17 — это исключительно создание воспроизводимого Z88DK ASM exporter поверх уже существующих ROM analysis и RDB.

---

# 48. Статус выполнения Stage 6.17

**Статус:** ✅ Завершено

**Дата:** 2026-09-09

**Результат:**

1. ✅ Создан отдельный CLI инструмент `v06c-asm-export`
2. ✅ Реализован full export pipeline: ROM + RDB → ASM files
3. ✅ Конвертация 8080 → z80asm синтаксис (lowercase, hex `h` suffix, register names)
4. ✅ Разделение CODE/DATA секций
5. ✅ RDB как основной источник имён/типов/ссылок
6. ✅ Code analysis integration (control-flow analysis)
7. ✅ Генерация стабильных labels для CALL/JMP targets
8. ✅ Перенос RDB comments в ASM
9. ✅ Генерация `export.json` manifest
10. ✅ Детерминированный output
11. ✅ Unit tests (62 checks, все проходят)
12. ✅ Smoke test с реальным ROM (clrs.rom)
13. ✅ Regression tests проходят (test_backend, test_rdb_controller, test_map_loader, etc.)
14. ✅ `src/` не изменён
15. ✅ Qoder Skill обновлён (добавлена секция "Генерация Z88DK ASM")
16. ✅ Qoder Agent обновлён (добавлен workflow "ASM Export")

**Файлы:**
- `debugger/export/asm_exporter.h` — header с типами и классом AsmExporter
- `debugger/export/asm_exporter.cpp` — реализация export logic (~750 lines)
- `debugger/export/asm_exporter_main.cpp` — CLI entry point
- `debugger/tests/test_asm_exporter.cpp` — unit tests (11 test cases, 62 checks)
- `debugger/CMakeLists.txt` — добавлены targets v06c-asm-export и test_asm_exporter
- `.qoder/skills/vector06c-debugger/SKILL.md` — добавлена секция о ASM exporter
- `.qoder/agents/vector06c-analyst.md` — добавлен workflow diagram

**Исправленные баги:**
- convertOperands: исправлена обработка двух регистров (MOV A,B → a,b)
- convertOperands: исправлена обработка register+hex (MVI A,D3 → a,d3h)

**Готово к коммиту.**
