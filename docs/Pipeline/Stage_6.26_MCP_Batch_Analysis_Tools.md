# ТЗ — Развитие MCP-сервера для общего анализа Vector-06C ROM

*Происхождение: составлено по итогам исследования `PUTUP.ROM`
(`/home/alexey/Projects/vector-games/roms/redesign/putup/AFTER.md`, §7 R1.3).*

## 1. Цель

Подготовить `v06c-mcp` как универсальный серверный слой для автоматизированного анализа ROM Vector-06C.

Основная задача этапа — устранить ограничения MCP, выявленные при полном исследовании `PUTUP.ROM`, и предоставить пакетные операции там, где последовательные MCP round-trip становятся узким местом.

MCP-сервер не должен содержать семантическую логику анализа ROM.

Архитектура:

```text
Python analysis scripts / AI Agent
              ↓
           MCP API
              ↓
        DebugBackend
              ↓
        IDebugTarget
              ↓
        DebugAdapter
              ↓
         Vector Board
```

---

# 2. Главный принцип

Новые MCP-инструменты должны:

* предоставлять данные;
* выполнять детерминированные операции;
* агрегировать уже существующие данные;
* уменьшать количество round-trip;
* не принимать решения о семантике ROM.

MCP не должен самостоятельно определять:

```text
func_*
data_*
music_*
string
variable
code/data classification
семантику функций
назначение таблиц
Fact / Inference / Hypothesis
```

Это остаётся за Python-анализатором и AI-агентом.

---

# 3. Совместимость

Не ломать существующие MCP-инструменты.

Особенно сохранить:

```text
debug_analyze_code
debug_disassemble_range
debug_read_memory_range
debug_get_memory_access_map
debug_get_memory_access_log
debug_create_memory_snapshot
debug_compare_memory_snapshots
debug_get_rdb_info
debug_find_rdb_object
...
```

Новые инструменты добавляются как расширение существующего API.

Старые инструменты не удалять только ради нового batch API.

---

# 4. Capability discovery

## 4.1. `tools/list`

Сервер обязан корректно предоставлять полный список реально зарегистрированных MCP-инструментов.

Количество инструментов не должно быть захардкожено в клиентском коде.

Клиент должен определять возможности через:

```text
initialize
tools/list
```

## 4.2. Версия возможностей

Если потребуется различать версии API, добавить машиночитаемый capability/version механизм.

Не заставлять Python-код угадывать возможности по версии бинаря.

---

# 5. Batch disassembly

## 5.1. Новый инструмент

Предусмотреть:

```text
debug_disassemble_image
```

Назначение: получить дизассемблирование ROM-образа крупными диапазонами без сотен отдельных MCP-вызовов.

Параметры должны позволять указать:

```text
start
length
```

либо эквивалентный диапазон ROM.

Дополнительно предусмотреть разумный лимит результата.

## 5.2. Требования

Результат должен быть эквивалентен последовательности:

```text
debug_disassemble_range(...)
debug_disassemble_range(...)
...
```

для тех же диапазонов.

Не менять существующий формат инструкции.

Каждая инструкция должна сохранять:

```text
address
bytes
mnemonic
operands
size
branch_target
branch_type
```

## 5.3. Важное ограничение

Инструмент не должен самостоятельно классифицировать байты как:

```text
code
data
table
string
```

Это остаётся задачей клиента.

---

# 6. Coverage report

## 6.1. Новый инструмент

Предусмотреть:

```text
debug_coverage_report
```

Он должен агрегировать существующий анализ кода и предоставить:

```text
code ranges
uncovered ranges
branch targets
targets without corresponding analyzed code
coverage statistics
```

## 6.2. JCC

Особенно важно учитывать обнаруженную проблему:

```text
analyze_code
    ↓
не все цели JCC автоматически становятся entry points
```

Инструмент должен явно показывать:

```text
branch target exists
but target is not covered
```

а не скрывать проблему.

## 6.3. Fixpoint

MCP-инструмент может предоставлять:

```text
coverage report
```

но не должен автоматически изменять RDB.

Python-код должен иметь возможность самостоятельно выполнить:

```text
report
→ add Label
→ analyze
→ report
→ ...
```

---

# 7. Полный memory diff

## 7.1. Новый инструмент

Предусмотреть:

```text
debug_diff_memory
```

Назначение: надёжно сравнивать два состояния памяти.

Например:

```text
snapshot A
snapshot B
range 0000–7FFF
```

## 7.2. Критическое требование

Новый инструмент не должен повторять ограничение текущего:

```text
debug_compare_memory_snapshots
```

Если два состояния содержат плотное изменение большого диапазона, результат должен содержать все изменившиеся диапазоны/байты в пределах установленного лимита.

## 7.3. Семантика

Результат должен позволять получить:

```text
address
old_value
new_value
```

и агрегированные contiguous ranges.

Например:

```text
6000–601F:
old = ...
new = ...
```

## 7.4. Backward compatibility

`debug_compare_memory_snapshots` не менять без необходимости.

`debug_diff_memory` должен стать новым инструментом для случаев, где требуется гарантированно полный diff.

---

# 8. Поиск байтовых последовательностей

## 8.1. Новый инструмент

Предусмотреть:

```text
debug_find_bytecode_sequence
```

Параметры:

```text
range_start
range_end
pattern
optional mask
```

Результат:

```text
matching addresses
```

## 8.2. Назначение

Инструмент предназначен для поиска:

```text
портовых сигнатур
машинных шаблонов
таблиц
структур
известных последовательностей
```

Он не должен интерпретировать найденную последовательность.

Например:

```text
OUT 0Bh
```

может быть найдено как байтовый шаблон, но MCP не должен писать:

```text
это VI53
```

---

# 9. Поиск immediate values

Предусмотреть:

```text
debug_find_immediate_in_range
```

для поиска заданного значения в операндах инструкций.

Важно: интерпретацию инструкции должен выполнять существующий Debugger disassembler.

Python не должен реализовывать собственный opcode decoder.

---

# 10. VRAM access

Предусмотреть:

```text
debug_get_vram_bytes
```

для получения фактического содержимого VRAM по адресу/диапазону.

Минимальные параметры:

```text
address
length
```

Инструмент должен работать через существующий debugger target API.

Не добавлять новую зависимость GUI.

---

# 11. Screen pixel probe

При необходимости предусмотреть:

```text
debug_get_screen_pixel
```

Параметры:

```text
x
y
```

Результат должен позволять сопоставить:

```text
координаты
→ VRAM
→ pixel/plane data
```

без необходимости Python-коду воспроизводить Vector video mapping.

---

# 12. Batch runtime profiling

Предусмотреть инструмент пакетного наблюдения за breakpoint:

```text
debug_profile_breakpoint
```

или эквивалентный API.

Параметры:

```text
address
hit_count
```

Результат для каждого срабатывания:

```text
sequence
PC
AF
BC
DE
HL
SP
...
```

Цель: заменить последовательность:

```text
set breakpoint
run
wait
pause
read CPU
run
wait
pause
read CPU
...
```

одним логическим MCP-вызовом.

## Важно

Инструмент не должен сам делать вывод:

```text
A = parameter
E = parameter
```

Он только собирает наблюдения.

---

# 13. Batch RDB operations

Если Python-прототип покажет значительную стоимость большого числа:

```text
add object
add link
update object
```

предусмотреть batch API.

Например:

```text
debug_add_rdb_objects
debug_add_rdb_links
```

с массивом операций.

## Требования

Batch-операция должна иметь транзакционную семантику:

```text
all valid → apply
ошибка → предсказуемый результат
```

Нельзя получить половину применённых объектов без явного документированного поведения.

---

# 14. Ограничения RDB

MCP должен явно соблюдать существующую модель:

```text
один RDB object на address
```

Нельзя обходить это ограничение созданием скрытых дублей.

Для элементов таблиц, локальных меток и логических подструктур использовать существующий механизм:

```text
label
comment
properties
links
```

либо иной уже предусмотренный RDB механизм.

---

# 15. `analyze_code`

Существующий API:

```text
start_address
addresses[]
```

оставить обратно совместимым.

Если одновременно переданы:

```text
start_address
addresses
```

→ `InvalidArgument`.

Новый batch API не должен заставлять клиента знать об этом ограничении.

Например, внутренний helper может принимать:

```text
vector<uint16_t> entry_points
```

и сам корректно выполнять необходимые операции.

---

# 16. Лимиты

Каждый новый batch-инструмент должен иметь явные ограничения:

```text
max range
max result bytes
max matches
max instructions
max entries
```

При превышении лимита MCP должен возвращать:

```text
isError = true
```

и структурированный `ErrorCode`.

Нельзя молча обрезать результат.

---

# 17. Determinism

Для одинакового состояния:

```text
ROM
RAM
CPU
RDB
parameters
```

MCP должен возвращать одинаковый результат.

Не использовать:

```text
random
unordered output
timestamp-dependent ordering
```

если это влияет на содержимое результата.

Коллекции должны иметь стабильный порядок.

---

# 18. Runtime state

Не добавлять инструменты, которые неявно делают:

```text
reset
load ROM
pause
run
clear RDB
clear breakpoints
```

если это не является их непосредственным назначением.

Batch-анализ не должен неожиданно менять состояние эмулятора.

---

# 19. Ошибки

Использовать существующую модель:

```text
AgentApiResult<T>
ErrorCode
```

Не придумывать отдельную систему ошибок для MCP.

MCP только сериализует результат.

---

# 20. Тестирование

Для каждого нового инструмента:

### Unit

Проверить:

```text
valid input
empty input
invalid range
maximum range
limit overflow
deterministic ordering
```

### Integration

Проверить на реальном:

```text
putup.rom
testay.rom
```

если соответствующие ROM доступны.

### Equivalence

Для batch-инструмента обязательно сравнить:

```text
batch result
```

с результатом последовательности существующих MCP/API-вызовов.

Например:

```text
debug_disassemble_image
==
N × debug_disassemble_range
```

```text
debug_diff_memory
==
local byte-for-byte comparison
```

---

# 21. Python reference requirement

До оптимизации MCP любой новый batch-инструмент должен иметь эталонную реализацию на Python.

Схема:

```text
Python reference
       ↓
измерение
       ↓
MCP implementation
       ↓
сравнение результатов
```

Нельзя сначала реализовать сложный C++ batch-инструмент, а затем подгонять Python под его поведение.

---

# 22. Что НЕ делать

На этом этапе не добавлять:

```text
AI inference
semantic classifier
automatic function naming
automatic data classification
automatic RDB reconstruction
automatic jump-table inference
automatic music recognition
automatic hardware identification
```

Также не добавлять:

```text
RAG
embeddings
vector database
LLM runtime
```

---

# 23. Проверка source isolation

После всех изменений:

```bash
git diff -- src/
git diff -- psp/
```

должны быть пустыми.

MCP должен работать только через существующие:

```text
IDebugBackend
IDebugTarget
DebugAdapter
```

Не добавлять новые hooks в `src/`.

---

# 24. Критерий завершения

Этап считается завершённым, когда:

* `tools/list` отражает реальный набор MCP-инструментов;
* capability discovery работает;
* batch disassembly работает;
* coverage report работает;
* полный memory diff работает;
* byte-sequence search работает;
* immediate search работает;
* VRAM range read работает;
* breakpoint profiling работает либо документирован как следующий этап;
* все новые лимиты формализованы;
* batch results эквивалентны существующим операциям;
* существующий MCP API не сломан;
* тесты проходят;
* `v06c-mcp` собирается;
* нет изменений в `src/`;
* нет изменений в `psp/`.

Главное: **не переносить все перечисленные функции в MCP сразу**. Реализовывать только те batch-операции, для которых Python-прототип покажет существенное количество round-trip или существенное время.
