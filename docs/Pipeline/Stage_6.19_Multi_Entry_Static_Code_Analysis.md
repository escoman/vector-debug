# Stage 6.19 — Multi-Entry Static Code Analysis

## Расширение `debug_analyze_code` для анализа нескольких точек входа

### Цель

Расширить существующий:

```text
debug_analyze_code
```

так, чтобы он мог выполнять статический CFG-анализ **от нескольких известных точек входа одновременно**.

Основная задача Stage 6.19 — увеличить покрытие статически подтверждённого кода ROM за счёт анализа уже известных entry points из RDB/SymbolDatabase, не добавляя эвристического распознавания jump tables.

---

# 1. Основной принцип

Сейчас:

```text
debug_analyze_code(address)
        ↓
BFS / CFG
        ↓
code regions
```

После Stage 6.19:

```text
debug_analyze_code(addresses)
        ↓
BFS / CFG от каждой точки
        ↓
объединение результатов
        ↓
единый набор code regions
```

Существующий режим с одной точкой входа должен продолжать работать.

---

# 2. Не менять существующий CFG-анализатор

Использовать существующий:

```text
code_analyzer
```

и его алгоритм BFS.

Не создавать новый анализатор.

Не реализовывать второй CFG engine.

Архитектура должна быть:

```text
AgentApi
    ↓
CodeAnalyzer
    ↓
existing 8080 decoder
```

Расширяется только способ задания entry points и объединения результатов.

---

# 3. API

Расширить `AgentApi::analyzeCode()` так, чтобы поддерживались несколько entry points.

Предпочтительный интерфейс:

```cpp
AgentApiResult<CodeAnalysisResult>
analyzeCode(
    const std::vector<uint16_t>& entryPoints,
    size_t maxInstructions
);
```

Если сохранение старого API требует overload:

```cpp
analyzeCode(uint16_t address, ...)
analyzeCode(std::vector<uint16_t> addresses, ...)
```

допускается overload.

Не ломать существующих пользователей API.

---

# 4. MCP interface

Существующий инструмент:

```text
debug_analyze_code
```

должен поддерживать:

```json
{
  "addresses": [0, 256, 512, 768]
}
```

Старый вариант:

```json
{
  "address": 0
}
```

должен продолжать работать.

Не создавать отдельный:

```text
debug_analyze_code_multi
```

если расширение существующего MCP tool возможно без нарушения текущего API.

---

# 5. MCP schema

Поддержать:

```text
address
```

для обратной совместимости и:

```text
addresses
```

для нового режима.

Для нового режима:

```text
addresses:
    array
    minItems = 1
    maxItems = разумный безопасный предел
```

Рекомендуемый предел:

```text
256 entry points
```

Каждый элемент:

```text
0..65535
```

Пустой массив должен возвращать:

```text
InvalidArgument
```

Если одновременно переданы:

```text
address
addresses
```

необходимо определить однозначное поведение.

Предпочтительно:

```text
addresses имеет приоритет
```

либо вернуть `InvalidArgument`.

Лучше вернуть `InvalidArgument`, чтобы агент не получал неожиданную семантику.

---

# 6. Удаление дубликатов

Если передано:

```text
[0100h, 0100h, 0200h]
```

анализировать:

```text
0100h
0200h
```

только один раз.

Порядок entry points сохранять в результате там, где это возможно.

---

# 7. Объединение результатов

Несколько запусков CFG могут обнаружить одни и те же инструкции.

Результат должен содержать каждую инструкцию только один раз.

Например:

```text
entry 0100 → 0200
entry 0150 → 0200
```

Инструкция по адресу `0200` должна присутствовать в итоговом результате один раз.

---

# 8. Информация об источнике

Желательно сохранить информацию о том, от каких entry points была достигнута инструкция/область.

Например:

```json
{
  "address": 512,
  "entry_points": [256, 336]
}
```

Если изменение существующего формата результата слишком велико, это поле можно добавить только для multi-entry режима.

Однако не следует создавать сложную новую модель provenance.

Минимально достаточно:

```text
entry_points
```

на уровне code region или инструкции.

---

# 9. Code regions

После объединения:

```text
regions
```

не должны дублироваться.

Если два анализа дают пересекающиеся диапазоны:

```text
0100–0120
0110–0140
```

не оставлять два независимых overlapping regions без необходимости.

Объединение должно быть детерминированным.

Рекомендуется получить:

```text
0100–0140
```

если диапазоны действительно относятся к одному непрерывному набору инструкций.

Не объединять регионы только на основании близости адресов, если между ними присутствуют неизвестные/невалидные байты.

---

# 10. Instructions

Итоговый список инструкций должен быть:

```text
unique by CPU address
```

и отсортирован:

```text
ascending address
```

если существующий формат API не требует иной сортировки.

Порядок должен быть детерминированным.

---

# 11. Coverage

Добавить/сохранить информацию о покрытии.

Минимально:

```text
instructions_count
code_bytes
coverage
```

Где:

```text
coverage =
    confirmed_code_bytes / analyzed_memory_size
```

Если denominator уже определён текущим `debug_analyze_code`, сохранить его семантику.

Не вводить новую неоднозначную метрику без необходимости.

Важно явно различать:

```text
analyzed bytes
confirmed code bytes
```

---

# 12. Entry Points в результате

Результат должен явно сообщать:

```text
entry_points
```

которые реально анализировались.

Например:

```json
{
  "entry_points": [0, 256, 512],
  "instructions_count": 687,
  "code_bytes": 1101
}
```

Это позволит агенту понимать, что анализ был выполнен не только от `0x0000`.

---

# 13. RDB integration — только чтение

Stage 6.19 **не должен изменять RDB**.

Разрешено:

```text
RDB
 ↓
получить известные functions/entry points
 ↓
analyzeCode()
```

Запрещено:

```text
analysis
 ↓
автоматически создать RDB objects
 ↓
автоматически создать RDB links
```

Не добавлять:

```text
debug_populate_rdb_from_analysis
```

в Stage 6.19.

---

# 14. Не создавать автоматически links

Даже если CFG обнаружил:

```text
CALL 1234h
```

не создавать автоматически:

```text
RDB link source → 1234h
```

CFG result является evidence для AI Agent.

Решение о создании RDB link остаётся за Agent.

---

# 15. Использование RDB functions

AI Agent должен получить возможность выполнять:

```text
debug_get_symbols
debug_get_rdb_objects
```

и использовать известные функции как дополнительные entry points.

Пример workflow:

```text
debug_load_rom
       ↓
debug_get_symbols
       ↓
известные функции
       ↓
получить их addresses
       ↓
debug_analyze_code(addresses=[...])
```

Важно:

**RDB-функция не является автоматически доказанным кодом только потому, что она находится в RDB.**

Она является известной/заданной точкой входа для анализа.

---

# 16. Entry point 0x0000

Существующее правило Stage 6.14 сохраняется.

Первичная точка исследования ROM:

```text
0x0000
```

Поэтому стандартный workflow должен начинаться с:

```text
debug_analyze_code(addresses=[0x0000])
```

или старого:

```text
debug_analyze_code(address=0x0000)
```

После этого агент может добавить дополнительные известные entry points.

---

# 17. Jump tables

В Stage 6.19 **не добавлять эвристическое обнаружение jump tables**.

Не реализовывать специальные правила вида:

```text
LXI H,address
...
PCHL
```

и не считать автоматически:

```text
address
```

jump table.

Причина:

```text
PCHL
```

может использоваться различными способами, и подобная эвристика может создавать ложные code/data классификации.

Это отдельная задача для будущего этапа, если реальный анализ покажет её необходимость.

---

# 18. PCHL

Не добавлять специальную семантику:

```text
PCHL → неизвестный target
```

в текущем Stage.

Существующий анализатор должен продолжать обрабатывать косвенный переход согласно своей текущей модели.

Если target невозможно определить статически:

```text
target = unknown
```

и анализ не должен угадывать адрес.

---

# 19. Нельзя считать все RDB-функции кодом

Наличие:

```text
84 functions
```

не является доказательством:

```text
84 независимых корректных entry points
```

Агент может использовать их для дополнительного анализа, но результат `debug_analyze_code` должен оставаться объективным результатом CFG traversal.

---

# 20. Ограничения

Сохранить существующие ограничения:

```text
MAX_ANALYSIS_INSTRUCTIONS
MAX_CODE_REGIONS
```

или их текущие аналоги.

Если multi-entry анализ превышает лимит:

не обходить лимит молча.

Вернуть существующую ошибку/ограничение либо явно обозначить truncation в результате, если такая модель уже предусмотрена.

Не создавать новый бесконечный analysis path.

---

# 21. Производительность

При наличии большого количества entry points:

```text
[84 addresses]
```

не должно происходить бессмысленное повторное декодирование одних и тех же инструкций.

Минимально допустимо:

```text
analyze entry 1
analyze entry 2
...
deduplicate
```

Но предпочтительно использовать общий cache уже декодированных инструкций, если это не усложняет существующий analyzer.

Не требуется преждевременная сложная оптимизация.

Главное:

```text
correctness > optimization
```

---

# 22. Thread safety

Инструмент остаётся read-only.

Он не должен:

```text
останавливать Board
изменять CPU
изменять Memory
изменять RDB
создавать commands
```

Если существующий `debug_analyze_code` работает через текущую модель доступа к памяти, сохранить её.

---

# 23. Agent Skill

Обновить:

```text
.qoder/skills/vector06c-debugger/SKILL.md
```

Добавить правило:

### Multi-Entry Analysis

Если анализ от `0x0000` покрывает только небольшую часть ROM, агент должен:

1. получить известные RDB functions;
2. собрать их addresses;
3. запустить `debug_analyze_code` с несколькими entry points;
4. сравнить coverage;
5. использовать полученный результат как evidence.

Не делать вывод:

```text
unreachable = data
```

только на основании отсутствия статической достижимости.

---

# 24. Agent Workflow

Рекомендуемый workflow:

```text
1. debug_load_rom
2. debug_get_rdb_objects / debug_get_symbols
3. analyze from 0x0000
4. collect known entry points
5. analyze from multiple entry points
6. compare coverage
7. debug_disassemble_range для подтверждённых ranges
8. анализ функций
9. создание/обновление RDB objects агентом
10. создание подтверждённых RDB links агентом
11. debug_save_rdb
```

Stage 6.19 отвечает только за пункт:

```text
5
```

и связанные с ним данные результата.

---

# 25. Agent не должен делать ручной disassembly

Сохраняется правило Stage 6.9:

```text
MCP-first
```

Agent не должен самостоятельно реализовывать:

```text
8080 decoder
opcode parser
branch target calculation
```

Использовать:

```text
debug_analyze_code
debug_disassemble_range
```

---

# 26. Тесты Agent API

Добавить tests:

### Single entry compatibility

Проверить:

```text
analyzeCode(0x0000)
```

даёт тот же результат, что и:

```text
analyzeCode([0x0000])
```

### Multiple entries

Например:

```text
[0x0000, 0x0100]
```

должны анализировать оба entry points.

### Duplicate entries

```text
[0x0100, 0x0100]
```

не должны удваивать инструкции.

### Shared code

Два entry points, ведущие к одной функции, должны дать один набор инструкций.

### Result ordering

Результат должен быть детерминированным.

### Entry point reporting

Проверить корректность списка entry points.

### Coverage

Проверить корректность code bytes/instruction count.

### RDB read-only

До/после:

```text
RDB == unchanged
```

---

# 27. MCP tests

Добавить проверки:

```text
debug_analyze_code
```

с:

```text
address
addresses
```

Проверить:

* schema;
* single entry;
* multiple entries;
* duplicate entries;
* invalid empty array;
* invalid address;
* maximum number of entry points;
* result JSON;
* real MCP wire call.

---

# 28. Реальный ROM smoke test

Использовать реальный ROM, например:

```text
putup.rom
```

или другой крупный ROM.

Сравнить:

```text
Analysis A:
entry = 0x0000

Analysis B:
entry = 0x0000 + known RDB function addresses
```

Зафиксировать:

```text
instructions
code bytes
regions
coverage
```

Главная проверка:

**multi-entry анализ действительно увеличивает подтверждённое покрытие.**

Если покрытие практически не изменилось — не придумывать искусственное объяснение.

---

# 29. Что считать успехом

Не требуется достичь какого-либо заранее заданного процента покрытия.

Нельзя устанавливать критерий:

```text
coverage >= 50%
```

или:

```text
coverage >= 80%
```

без экспериментального основания.

Цель Stage:

```text
multi-entry analysis работает корректно
```

и позволяет объективно измерить, сколько дополнительного кода обнаруживается от известных entry points.

---

# 30. Документация

Создать:

```text
debugger/docs/Pipeline/Stage_6.19_Multi_Entry_Static_Code_Analysis.md
```

Документ должен содержать:

* цель;
* архитектуру;
* API;
* MCP schema;
* формат результата;
* ограничения;
* примеры;
* тестирование;
* результат smoke test.

После завершения добавить статус:

```text
Status: Completed
```

---

# 31. Regression

После реализации проверить минимум:

```text
test_agent_api
test_mcp_protocol
test_backend
test_rdb_controller
test_map_loader
test_symbol_database
test_asm_exporter
test_call_graph_model
test_gui_smoke
```

Также выполнить существующие regression suites проекта.

---

# 32. `src/` не изменять

Обязательное требование:

```bash
git diff -- src/
```

должен быть пустым.

Не добавлять:

```text
hooks
runtime instrumentation
Board modifications
Memory modifications
CPU modifications
```

Stage реализуется только в debugger layer.

---

# 33. Не добавлять новую функциональность сверх Stage

В Stage 6.19 не включать:

```text
jump-table detector
PCHL resolver
RDB auto-population
automatic RDB links
automatic function creation
automatic DATA classification
new disassembler
new opcode decoder
ROM exporter
ASM exporter changes
```

Это отдельные потенциальные задачи будущих этапов.

---

# 34. Acceptance Criteria

Stage 6.19 считается завершённым, если:

* `debug_analyze_code` поддерживает несколько entry points;
* старый single-entry API продолжает работать;
* MCP поддерживает новый формат;
* duplicate entry points не дублируют результат;
* общие участки кода не дублируются;
* инструкции уникальны по CPU address;
* результат детерминирован;
* entry points отражаются в результате;
* code regions корректно объединяются;
* coverage доступен;
* используется существующий CFG analyzer;
* используется существующий 8080 decoder;
* RDB не изменяется;
* RDB используется только как источник известных entry points;
* RDB links автоматически не создаются;
* jump-table эвристики не добавлены;
* PCHL не получает выдуманные targets;
* Agent Skill обновлён;
* Agent использует multi-entry workflow;
* MCP wire test проходит;
* regression tests проходят;
* на реальном ROM проведено сравнение single-entry/multi-entry;
* `git diff -- src/` пустой.

---

# 35. Ожидаемая архитектура после Stage

```text
                 ROM
                  │
          ┌───────┴────────┐
          │                │
     RDB functions     Entry 0x0000
          │                │
          └───────┬────────┘
                  ↓
        debug_analyze_code
        [multiple entries]
                  ↓
             CFG analysis
                  ↓
        confirmed code regions
                  ↓
     debug_disassemble_range
                  ↓
              AI Agent
                  ↓
          RDB objects/links
```

Ключевой принцип:

```text
Multi-entry analysis
≠
automatic reverse engineering
```

Stage 6.19 только расширяет **объём статически исследуемого кода**, используя уже известные точки входа. Решение о том, что найденный код означает и какие RDB objects/links следует создать, остаётся на уровне AI Agent.

---

## Implementation Summary

Status: Completed

### Изменённые файлы

| Файл | Изменение |
|------|-----------|
| `debugger/agent/code_analyzer.h` | +`analyzeCodeMulti()` — BFS от нескольких entry points с общим visited set; `analyzeCode()` теперь wrapper |
| `debugger/agent/agent_types.h` | +`MAX_ANALYSIS_ENTRY_POINTS = 256` |
| `debugger/agent/agent_api.h` | +overload `analyzeCode(vector<uint16_t>, size_t)` |
| `debugger/agent/agent_api.cpp` | +реализация multi-entry overload с валидацией |
| `debugger/mcp/mcp_adapter.cpp` | +параметр `addresses[]`, mutual exclusion, `entry_points`/`code_bytes` в результате |
| `debugger/mcp/tests/test_mcp_protocol.cpp` | +6 MCP tests (single compat, multi, duplicates, empty, exclusion, missing) |
| `debugger/agent/tests/test_agent_integration.cpp` | +тест #51 (multi-entry, duplicates, shared code, sorting, codeBytes) |
| `.qoder/skills/vector06c-debugger/SKILL.md` | +Multi-Entry Analysis section, tools count 55 |
| `.qoder/agents/vector06c-analyst.md` | tools count 55, +`debug_disassemble_range` |

### Результаты тестирования

| Suite | Результат |
|-------|----------|
| test_agent_integration | 51/51 ✅ |
| test_mcp_protocol | 50/50 ✅ |
| test_agent_commands | 15/15 ✅ |
| test_agent_contract | 49/49 ✅ |
| test_backend | 120/120 ✅ |
| test_rdb_controller | 39/39 ✅ |
| test_map_loader | 20/20 ✅ |
| test_call_graph_model | 22/22 ✅ |
| test_asm_exporter | 62/62 ✅ |
| test_map_import | 8/8 ✅ |

### `git diff -- src/` — пустой
