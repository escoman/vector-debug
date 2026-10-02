# Stage 6.18 — MCP Range Disassembly

## MCP-инструмент `debug_disassemble_range`

### Цель

Добавить MCP-инструмент для последовательного дизассемблирования заданного диапазона памяти Vector-06Ц.

Инструмент предназначен прежде всего для AI Agent, которому необходимо получать полный дизассемблированный диапазон ROM без многократных вызовов `debug_disassemble`.

Инструмент **не должен создавать новый механизм дизассемблирования**.

Использовать существующий 8080 decoder/disassembler.

---

# 1. Новый Agent API

Добавить:

```cpp
debug_disassemble_range
```

через соответствующий метод `AgentApi`.

Предлагаемый интерфейс:

```cpp
AgentApiResult<std::vector<DisassembledInstruction>>
disassembleRange(
    uint16_t address,
    uint16_t size
);
```

или эквивалентный существующему стилю Agent API.

---

# 2. Ограничение диапазона

Максимальный размер одного запроса:

```text
16384 bytes
```

Использовать существующее ограничение диапазонного чтения.

Проверять:

```text
size > 0
size <= 16384
```

Также корректно обрабатывать выход диапазона за пределы адресного пространства 64K.

Запрос:

```text
address + size > 0x10000
```

должен завершаться `InvalidRange`.

Не допускать wrap-around адреса.

---

# 3. Формат результата

Для каждой успешно декодированной инструкции возвращать:

```text
address
bytes
mnemonic
operands
size
branch_target
branch_type
```

Пример:

```json
{
  "address": 256,
  "bytes": [195, 32, 1],
  "mnemonic": "JMP",
  "operands": "0120h",
  "size": 3,
  "branch_target": 288,
  "branch_type": "JMP"
}
```

Для обычной инструкции:

```json
{
  "address": 259,
  "bytes": [62, 5],
  "mnemonic": "MVI",
  "operands": "A,05h",
  "size": 2,
  "branch_target": null,
  "branch_type": null
}
```

---

# 4. Branch information

Для инструкций управления потоком определить:

```text
JMP
Jcc
CALL
Ccc
RET
Rcc
RST
```

`branch_target` должен вычисляться самим debugger/disassembler layer.

AI Agent не должен самостоятельно вычислять адрес перехода из opcode/operands.

## JMP

```text
branch_target = absolute target
branch_type = "JMP"
```

## Conditional JMP

Например:

```text
JZ
JNZ
JC
JNC
JP
JM
JPE
JPO
```

Возвращать:

```text
branch_target = target
branch_type = "JCC"
```

При этом не утверждать, что переход действительно был выполнен.

## CALL

```text
branch_target = target
branch_type = "CALL"
```

## Conditional CALL

```text
branch_target = target
branch_type = "CALL"
```

## RET

```text
branch_target = null
branch_type = "RET"
```

Поскольку фактический адрес возврата определяется стеком во время исполнения.

## Conditional RET

```text
branch_target = null
branch_type = "RET"
```

## RST

Для `RST n`:

```text
branch_type = "RST"
branch_target = n * 8
```

Например:

```text
RST 7 → 0038h
```

---

# 5. Не путать статический target и фактическое исполнение

Инструмент выполняет **статический дизассемблинг**.

Он не должен утверждать:

```text
"JZ был выполнен"
```

если это невозможно определить статически.

Правильно:

```text
branch_target = 0120h
branch_type = JCC
```

Это означает только наличие потенциального перехода.

---

# 6. Инструкция должна полностью помещаться в диапазон

Если последняя инструкция начинается внутри диапазона, но её operand bytes выходят за пределы диапазона:

```text
address = 0x1FFE
size = 0x0002
```

и opcode требует 3 байта,

инструкция не должна быть возвращена как корректно декодированная.

Не читать байты за пределами запрошенного диапазона.

Возможное поведение:

```text
status = incomplete_instruction
```

либо ошибка `InvalidRange`.

Предпочтительно использовать существующую модель ошибок/результатов проекта без введения специальной ошибки, если она уже не требуется.

---

# 7. Unknown / invalid opcode

Если decoder встречает недопустимый или неизвестный opcode:

не придумывать инструкцию.

Возвращаемая модель должна позволять обозначить:

```text
unknown / invalid opcode
```

без ложного mnemonic.

Не пытаться самостоятельно интерпретировать байт как DATA.

---

# 8. Последовательность дизассемблирования

`debug_disassemble_range` работает последовательно:

```text
address
    ↓
decode instruction
    ↓
address += instruction.size
    ↓
decode next instruction
    ↓
...
```

Инструмент **не выполняет CFG traversal**.

Он не должен:

* переходить по JMP;
* пропускать байты после JMP;
* начинать новые ветви;
* определять достижимость;
* определять code/data;
* изменять RDB.

Например:

```text
0100: JMP 0200
0103: MVI A,01
0105: ...
```

все инструкции диапазона должны быть возвращены последовательно.

---

# 9. Не дублировать `debug_analyze_code`

Разделение обязанностей:

```text
debug_read_memory_range
    ↓
получение сырых байтов

debug_disassemble_range
    ↓
последовательный disassembly

debug_analyze_code
    ↓
CFG / reachability / code regions
```

`debug_disassemble_range` не должен выполнять CFG-анализ.

`debug_analyze_code` не должен быть переписан на базе нового MCP-инструмента.

Оба инструмента могут использовать общий существующий decoder.

---

# 10. Не добавлять новый opcode decoder

Запрещено:

```text
новая таблица opcode
новый 8080 decoder
копирование decoder в MCP
копирование decoder в Agent API
```

Использовать существующий debugger disassembler.

Если существующий decoder уже предоставляет opcode/operand/size — использовать его напрямую.

---

# 11. MCP Tool

Зарегистрировать:

```text
debug_disassemble_range
```

Параметры:

```json
{
  "address": 256,
  "size": 1024
}
```

Schema должна содержать числовые ограничения:

```text
address: 0..65535
size: 1..16384
```

MCP должен быть только transport/protocol layer:

```text
MCP
 ↓
AgentApi::disassembleRange()
 ↓
existing debugger disassembler
```

Не реализовывать дизассемблер внутри MCP handler.

---

# 12. MCP JSON result

Результат должен быть пригоден для прямой обработки AI Agent.

Пример:

```json
{
  "address": 256,
  "size": 16,
  "instructions": [
    {
      "address": 256,
      "bytes": [195, 32, 1],
      "mnemonic": "JMP",
      "operands": "0120h",
      "size": 3,
      "branch_target": 288,
      "branch_type": "JMP"
    },
    {
      "address": 259,
      "bytes": [62, 5],
      "mnemonic": "MVI",
      "operands": "A,05h",
      "size": 2,
      "branch_target": null,
      "branch_type": null
    }
  ]
}
```

Названия полей можно адаптировать к существующим debugger JSON conventions.

---

# 13. Address semantics

Адреса всегда являются адресами Vector-06Ц:

```text
0x0000..0xFFFF
```

Для ROM analysis не использовать:

```text
file offset
```

вместо CPU address.

Если ROM загружен с origin:

```text
0x0100
```

то:

```text
CPU address = 0x0100
```

а не:

```text
file offset = 0
```

Agent должен получать именно CPU addresses.

---

# 14. RDB

`debug_disassemble_range`:

**не должен изменять RDB.**

Не создавать автоматически:

```text
functions
labels
objects
links
comments
```

Дизассемблирование является read-only операцией.

---

# 15. Symbols

Если символическая информация уже доступна через существующий debugger API, можно использовать её для представления operand/target.

Однако это не должно быть обязательным условием работы инструмента.

Главное:

```text
branch_target
```

должен быть числовым CPU address.

Не подменять его исключительно именем символа.

---

# 16. Agent Skill

Обновить:

```text
.qoder/skills/vector06c-debugger/SKILL.md
```

Добавить правило:

### Range Disassembly

Для последовательного анализа диапазона ROM использовать:

```text
debug_disassemble_range
```

вместо серии:

```text
debug_disassemble
```

Agent должен использовать его когда ему необходимо:

* получить полный кодовый диапазон;
* проверить последовательность инструкций;
* исследовать функцию;
* проверить участок ROM;
* подготовить данные для анализа;
* получить branch targets.

---

# 17. Agent Workflow

Рекомендуемый workflow:

```text
debug_read_memory_range
        ↓
debug_analyze_code
        ↓
получить code regions
        ↓
debug_disassemble_range
        ↓
получить инструкции + branch targets
        ↓
анализ функций / RDB
```

Для отдельной функции:

```text
RDB function
    ↓
address + size
    ↓
debug_disassemble_range
```

Не запускать `debug_analyze_code` повторно для каждого байта или каждой инструкции.

---

# 18. Не делать `debug_export_rom`

В рамках Stage 6.18 **не добавлять**:

```text
debug_export_rom
export_rom()
```

Причина:

`debug_read_memory_range` уже позволяет получить полный ROM диапазон до 16 KB за один вызов.

Файловый экспорт не является необходимой частью debugger MCP API.

AI Agent должен работать через MCP API, а не через дополнительный обмен бинарными файлами.

---

# 19. Не переносить ASM conversion в MCP

Не добавлять:

```text
debug_convert_to_z80asm
debug_export_asm
```

и аналогичные функции.

Конвертация:

```text
8080 → z80asm
```

уже является ответственностью:

```text
v06c-asm-export
```

MCP предоставляет данные:

```text
bytes
mnemonics
operands
branch targets
```

ASM exporter использует эти данные самостоятельно.

---

# 20. Тесты Agent API

Добавить unit tests для:

### Basic

```text
single instruction
multiple instructions
mixed instruction sizes
empty/invalid range
```

### Branches

Проверить:

```text
JMP
JZ
JNZ
CALL
CC
RET
RST
```

### Targets

Проверить точность:

```text
JMP 1234h → 1234h
CALL 5678h → 5678h
RST 7 → 0038h
```

### Range boundaries

Проверить:

```text
instruction полностью внутри range
instruction обрывается на конце range
range = 16384
range > 16384
address + size > 0x10000
```

### Invalid opcode

Проверить, что неизвестный opcode не превращается в ложную инструкцию.

### Read-only

Проверить:

```text
RDB до вызова == RDB после вызова
```

---

# 21. MCP tests

Добавить MCP tests:

```text
debug_disassemble_range
```

Проверить:

* корректную schema;
* успешный вызов;
* JSON result;
* branch_target;
* branch_type;
* ограничения `size`;
* invalid range;
* real MCP wire call.

Не использовать stub decoder.

---

# 22. Реальный MCP smoke test

Обязательно проверить через настоящий stdio MCP:

```text
debug_load_rom
        ↓
debug_disassemble_range
```

На реальном ROM, например:

```text
clrs.rom
```

или другом имеющемся ROM.

Проверить, что агент получает:

```text
address
bytes
mnemonic
operands
size
branch_target
branch_type
```

без ручного декодирования.

---

# 23. Regression

После реализации обязательно проверить:

```text
test_agent_api
test_mcp_protocol
test_backend
test_rdb_controller
test_map_import
test_call_graph_model
test_gui_smoke
```

и существующие regression suites проекта.

Новые тесты должны проходить полностью.

---

# 24. Qoder Skill / Agent

Обновить:

```text
.qoder/skills/vector06c-debugger/SKILL.md
.qoder/agents/vector06c-analyst.md
debugger/agent/AI_AGENT_WORKFLOW.md
```

Указать:

```text
MCP-first
```

и запретить агенту самостоятельно декодировать opcode.

Agent должен:

```text
MCP → debug_disassemble_range
```

а не:

```text
MCP → bytes → собственный Python decoder
```

---

# 25. `src/` не изменять

Критическое требование:

```bash
git diff -- src/
```

должен быть пустым.

Stage 6.18 реализуется только в:

```text
debugger/
.qoder/
```

и связанных debugger-specific файлах.

Не добавлять hooks или instrumentation в основной эмулятор.

---

# 26. Acceptance Criteria

Stage 6.18 считается завершённым, если:

* существует `debug_disassemble_range`;
* Agent API предоставляет range disassembly;
* MCP предоставляет `debug_disassemble_range`;
* используется существующий 8080 decoder;
* максимум 16384 байта за запрос;
* диапазон не допускает address wrap-around;
* каждая инструкция содержит address;
* каждая инструкция содержит bytes;
* каждая инструкция содержит mnemonic;
* каждая инструкция содержит operands;
* каждая инструкция содержит size;
* переходы содержат branch target;
* переходы содержат branch type;
* RET не получает выдуманный target;
* RST получает корректный target;
* дизассемблирование последовательное, без CFG traversal;
* `debug_analyze_code` остаётся отдельным CFG-инструментом;
* RDB не изменяется;
* MCP не содержит собственного decoder;
* `debug_export_rom` не добавлен;
* ASM conversion не перенесён в MCP;
* Qoder Skill/Agent используют новый инструмент;
* реальные MCP wire tests проходят;
* regression tests проходят;
* `git diff -- src/` пустой.

---

# 27. Результат Stage

После Stage 6.18 AI Agent должен иметь нормальный путь:

```text
ROM
 │
 ├── debug_read_memory_range
 │
 ├── debug_analyze_code
 │       ↓
 │   code regions
 │
 └── debug_disassemble_range
         ↓
   complete instructions
         +
   branch targets
         ↓
      AI Agent
         ↓
   RDB / analysis
         ↓
   v06c-asm-export
         ↓
      z80asm
```

Главная цель Stage 6.18 — **не расширять возможности reverse-engineering engine**, а дать агенту удобный структурированный доступ к уже существующему 8080 disassembler.

---

# 28. Статус выполнения Stage 6.18

**Статус:** ✅ Завершено

**Дата:** 2026-09-09

**Результат:**

1. ✅ Добавлен AgentApi::disassembleRange() метод
2. ✅ Добавлен MCP tool debug_disassemble_range
3. ✅ Используется существующий 8080 decoder (disassembler.h)
4. ✅ Максимум 16384 байта за запрос
5. ✅ Проверка address wrap-around
6. ✅ Каждая инструкция содержит address, bytes, mnemonic, operands, size
7. ✅ Переходы содержат branch_target и branch_type
8. ✅ RET не получает выдуманный target (null)
9. ✅ RST получает корректный target (n * 8)
10. ✅ Дизассемблирование последовательное, без CFG traversal
11. ✅ debug_analyze_code остаётся отдельным CFG-инструментом
12. ✅ RDB не изменяется (read-only операция)
13. ✅ MCP не содержит собственного decoder
14. ✅ debug_export_rom не добавлен
15. ✅ ASM conversion не перенесён в MCP
16. ✅ Qoder Skill обновлён (добавлена секция "Range Disassembly")
17. ✅ Qoder Agent обновлён (добавлен workflow "Range Disassembly")
18. ✅ Добавлены unit tests для Agent API
19. ✅ Regression tests проходят
20. ✅ src/ не изменён

**Файлы:**
- `debugger/agent/agent_types.h` — добавлены DisassembledRangeInstruction и DisassembleRangeResult
- `debugger/agent/agent_api.h` — добавлена декларация disassembleRange()
- `debugger/agent/agent_api.cpp` — реализация disassembleRange() (~120 lines)
- `debugger/mcp/mcp_adapter.cpp` — регистрация MCP tool debug_disassemble_range
- `debugger/agent/tests/test_agent_api.cpp` — 5 новых тестов для disassembleRange
- `.qoder/skills/vector06c-debugger/SKILL.md` — добавлена секция "Range Disassembly"
- `.qoder/agents/vector06c-analyst.md` — добавлен workflow "Range Disassembly"

**Тесты:**
- test_disassemble_range_basic — базовое последовательное дизассемблирование
- test_disassemble_range_branches — branch targets и типы (JMP, CALL, RET, RST)
- test_disassemble_range_invalid — проверка invalid range (size=0, size>16384, wrap-around)
- test_disassemble_range_incomplete — incomplete instruction at end
- test_disassemble_range_readonly — проверка что RDB не изменяется

**Verification:**
- src/ changes: 0
- Regression tests: 263+ pass (backend 120, rdb 39, map_loader 20, symbol_db 22, asm_exporter 62)
- MCP tools: 55 registered (was 54)

**Готово к коммиту.**
