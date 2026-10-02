# Stage 6.8 — Qoder/Qwen-Lite ROM Analysis Integration & Accuracy

## Цель

Завершить интеграцию внешнего AI-агента на базе **Qoder + Qwen-Lite** с Vector-06C Debugger и обеспечить воспроизводимый, доказательный и максимально точный анализ ROM через существующий MCP/Agent API.

На этом этапе **не создавать собственного AI runtime**, RAG, embeddings, vector database или отдельного Python-агента.

Целевая схема:

```text
Qoder IDE
    ↓
Qwen-Lite
    ↓ MCP / stdio
v06c-mcp
    ↓
Agent API
    ↓
DebugBackend
    ↓
DebugAdapter
    ↓
Vector-06C Board
```

Документация и знания:

```text
Qwen-Lite
    ↓
Vector-06C Skill
    ↓
AI_AGENT_WORKFLOW.md
    ↓
Profiles / Tasks
    ↓
Knowledge Base
    ↓
MCP evidence
```

---

# 1. Qoder должен уметь анализировать ROM непосредственно из чата

Должен поддерживаться сценарий:

```text
Пользователь:
Проанализируй clrs.rom. Что он делает?
```

Qwen-Lite должен самостоятельно:

1. определить ROM;
2. выбрать подходящий профиль анализа;
3. выбрать необходимые Tasks;
4. прочитать соответствующие документы Knowledge Base;
5. загрузить ROM через MCP при необходимости;
6. получить фактические данные от Debugger;
7. провести анализ;
8. проверить важные гипотезы;
9. сформировать итоговый отчёт.

Не требуется запускать отдельный пользовательский скрипт.

---

# 2. Создать Qoder Skill

Добавить project-level Skill:

```text
.qoder/
└── skills/
    └── vector06c-debugger/
        └── SKILL.md
```

Skill должен быть частью репозитория и обеспечивать воспроизводимость настройки.

Skill должен описывать:

* назначение Vector-06C Debugger;
* расположение Knowledge Base;
* расположение Profiles;
* расположение Tasks;
* расположение `AI_AGENT_WORKFLOW.md`;
* порядок анализа ROM;
* правила использования MCP;
* требования к доказательности;
* правила интерпретации дизассемблирования;
* правила проверки гипотез;
* формат итогового отчёта.

---

# 3. Skill не должен дублировать Knowledge Base

Не копировать содержимое:

```text
architecture.md
cpu.md
memory.md
video.md
io.md
keyboard.md
sound.md
rom_format.md
verification.md
```

в `SKILL.md`.

Skill должен указывать Qwen, где находятся эти документы и когда их необходимо читать.

Центральным источником знаний остаётся:

```text
debugger/agent/knowledge/vector06c/
```

---

# 4. Использовать существующий Workflow

Skill обязан ссылаться на:

```text
debugger/agent/AI_AGENT_WORKFLOW.md
```

и заставлять модель соблюдать его последовательность:

```text
Profile
  ↓
Tasks
  ↓
Knowledge
  ↓
Analysis Plan
  ↓
MCP operations
  ↓
Evidence
  ↓
Hypotheses
  ↓
Validation
  ↓
Final Report
```

Не создавать второй конкурирующий workflow.

---

# 5. Обязательное разделение Fact / Inference / Hypothesis

При анализе ROM модель должна явно различать:

### Fact

Непосредственно наблюдаемое через MCP или исходный код.

Например:

```text
PC=013F
OUT 0Ch выполняется
пишется значение 07h
```

### Inference

Вывод из нескольких фактов.

Например:

```text
Программа изменяет палитру во время выполнения.
```

### Hypothesis

Предположение, требующее проверки.

Например:

```text
Вероятно, программа синхронизирует изменение палитры
с разверткой экрана.
```

Гипотезу нельзя выдавать за установленный факт.

---

# 6. Обязательное разделение Hardware / Emulator

Каждое существенное утверждение должно классифицироваться как:

```text
Original Hardware Fact
```

или:

```text
Emulator Behavior
```

или:

```text
Unknown
```

Особенно это относится к:

* timing;
* wait states;
* видеорежимам;
* палитре;
* портам;
* прерываниям;
* undocumented instructions;
* особенностям памяти.

Нельзя выдавать поведение VSDL/EMU80 за подтверждённое поведение реального Vector-06C.

Использовать:

```text
debugger/agent/knowledge/vector06c/verification.md
```

как основной файл аудита известных противоречий и неопределённостей.

---

# 7. Повысить точность дизассемблирования

Анализатор не должен считать результат дизассемблера автоматически доказательством смысла программы.

Необходимо различать:

```text
Instruction decoding
```

и:

```text
Program semantics
```

Для каждой важной инструкции учитывать:

* opcode;
* адрес;
* длину;
* операнды;
* изменение регистров;
* изменение памяти;
* изменение I/O;
* изменение PC;
* влияние на стек;
* возможный переход управления.

---

# 8. Не делать выводы только по соседним байтам

Особенно запрещается логика вида:

```text
после кода находятся байты
→ значит это таблица данных
```

Перед утверждением:

```text
это таблица
```

необходимо установить хотя бы одно из:

* код явно вычисляет адрес таблицы;
* выполняется чтение из этого диапазона;
* диапазон является операндом инструкции;
* адрес достигается через известный control/data flow;
* наблюдается фактическое обращение через MCP.

Если этого нет:

```text
Unknown / possible data
```

а не утверждение о назначении.

---

# 9. Проверка control flow

При анализе ROM необходимо строить фактическую цепочку выполнения там, где это возможно:

```text
entry
 ↓
instruction
 ↓
branch / call
 ↓
target
 ↓
return
```

Особое внимание:

```text
JMP
CALL
RET
RST
conditional JMP
conditional CALL
conditional RET
```

Нельзя считать область данных кодом только потому, что она успешно декодируется как инструкции.

---

# 10. Проверка спорных участков через MCP

Если вывод модели зависит от спорного участка ROM, она должна выполнить дополнительные операции MCP.

Например:

```text
disassemble
↓
найден OUT 0Ch
↓
проверить execution trace
↓
проверить I/O trace
↓
проверить фактические значения
```

или:

```text
найден CALL
↓
проверить destination
↓
проверить stack
↓
проверить execution history
```

Принцип:

```text
Suspicious claim
    ↓
Additional evidence
    ↓
Validated / Rejected / Unknown
```

---

# 11. Не считать timing доказанным без соответствующего evidence

Особенно осторожно обращаться с:

```text
CALL timing
XTHL timing
instruction cycles
wait states
CPU frequency
frame timing
VBlank timing
```

Если значение известно только из:

```text
EMU80
VSDL
TIMSoft
```

необходимо указывать источник и статус согласно `verification.md`.

Если аппаратная проверка отсутствует:

```text
Hardware status: UNVERIFIED
```

---

# 12. Проверка видеологических утверждений

При анализе программ, работающих с видео, необходимо отдельно проверять:

```text
VRAM address
plane
palette register
border
512/256 mode
screen timing
```

Для Vector-06C использовать актуальную карту:

```text
0x8000 — plane 0
0xA000 — plane 1
0xC000 — plane 2
0xE000 — plane 3
```

Не утверждать, что:

```text
Port B = palette index
```

если фактическая реализация показывает другое.

Палитра и border должны анализироваться отдельно.

---

# 13. Пример: анализ clrs.rom

Для ROM типа:

```text
clrs.rom
```

модель должна проверить, а не просто предположить:

```text
entry point
RST7 vector
interrupt handler
OUT 0Ch
OUT 02h
VRAM access
stack
timing loops
RET
data/code boundaries
```

Если обнаружены изменения палитры, необходимо установить:

1. какие значения записываются;
2. в какой порт;
3. сколько раз;
4. в каком порядке;
5. где находится код;
6. вызывается ли он из interrupt handler;
7. действительно ли выполняются эти записи;
8. есть ли связь с экранным timing;
9. можно ли доказать эффект на реальном hardware или только в эмуляторе.

Только после этого разрешается формулировать вывод о назначении программы.

---

# 14. Не использовать комментарии дизассемблера как источник истины

Например, если дизассемблер показывает:

```text
DCX B
```

нельзя интерпретировать это как:

```text
B--
```

поскольку инструкция изменяет пару:

```text
BC
```

Комментарии должны соответствовать реальной семантике Intel 8080.

Для каждой критической инструкции модель должна проверять:

```text
opcode → instruction → operands → affected registers/memory
```

---

# 15. Обязательная проверка перед финальным выводом

Перед тем как сформулировать ключевой вывод, модель должна задать себе:

```text
Какие факты я реально наблюдал?
Какие выводы следуют из этих фактов?
Какие утверждения пока являются гипотезами?
Есть ли альтернативное объяснение?
Можно ли проверить его через MCP?
```

Если доказательств недостаточно:

```text
Unknown
```

предпочтительнее, чем предположение.

---

# 16. Использовать существующие MCP tools

Не добавлять новые MCP tools только ради Stage 6.8.

Использовать существующие операции:

```text
debug_load_rom
debug_get_debug_state
debug_disassemble
debug_get_cpu_state
debug_read_memory
debug_get_memory_map
debug_get_screen_info
debug_get_vram_info
debug_get_execution_trace
debug_get_io_trace
debug_get_instruction_history
debug_get_symbols
debug_get_functions
debug_get_xrefs
debug_get_call_graph
debug_get_stack
debug_get_logs
```

и остальные существующие `debug_*` tools по необходимости.

---

# 17. Не дублировать Agent API в Qoder Skill

Skill не должен реализовывать собственные:

```text
debugger commands
Board access
Memory access
CPU access
```

Он только определяет:

```text
когда
что
зачем
и как проверять результат
```

Фактические операции выполняются через MCP.

---

# 18. Профили анализа

Skill должен уметь выбирать существующий профиль:

```text
rom_audit
reverse_engineering
bug_hunting
```

Пример:

### "Что делает ROM?"

Использовать:

```text
reverse_engineering
```

### "Проверь ROM на ошибки"

Использовать:

```text
bug_hunting
```

### "Проведи полный аудит ROM"

Использовать:

```text
rom_audit
```

Не создавать новые профили без необходимости.

---

# 19. Выбор Tasks

Модель должна выбирать только необходимые задачи.

Например:

```text
reverse_engineering
    ↓
stack_safety
analyze_vram
analyze_io
```

если они действительно относятся к программе.

Не запускать все Tasks автоматически без причины.

---

# 20. Формат итогового отчёта

Каждый анализ ROM должен завершаться структурой:

```text
Goal
ROM
Profile
Tasks

Findings

Verified Facts

Inferences

Hypotheses

Unknowns

Evidence

Limitations

Recommended Next Steps
```

Для существенных утверждений желательно указывать:

```text
Evidence:
- address
- instruction
- MCP result
- trace
- memory value
- I/O event
```

---

# 21. Уровень уверенности

Для существенных выводов использовать:

```text
Confidence: High
Confidence: Medium
Confidence: Low
```

Правило:

### High

Непосредственно подтверждается MCP/evidence или надёжным первичным источником.

### Medium

Логически следует из нескольких фактов, но прямого подтверждения нет.

### Low

Гипотеза или интерпретация с недостаточным количеством evidence.

---

# 22. Проверка Skill

Создать тестовый сценарий:

```text
Проанализируй clrs.rom. Что он делает?
```

Ожидается, что Qwen-Lite:

1. обнаружит ROM;
2. выберет подходящий профиль;
3. прочитает необходимые KB;
4. использует MCP;
5. выполнит дизассемблирование;
6. проверит execution/I/O при необходимости;
7. отличит факт от гипотезы;
8. не назовёт область после RET таблицей без evidence;
9. корректно интерпретирует инструкции 8080;
10. выдаст итоговый отчёт.

---

# 23. Regression ROM tests

Проверить минимум:

```text
clrs.rom
```

и ещё несколько ROM с разными характеристиками:

```text
обычный игровой ROM
ROM с активным I/O
ROM с VRAM manipulation
ROM с interrupt handler
ROM с MAP-файлом
```

Для каждого проверить:

```text
MCP calls
analysis quality
fact/inference separation
hardware/emulator separation
```

---

# 24. Не изменять существующую архитектуру Debugger

Stage 6.8 не должен менять:

```text
Agent API
MCP protocol
DebugBackend
DebugAdapter
IDebugBackend
IDebugTarget
```

если для этого нет абсолютно необходимой причины.

Основная работа выполняется в:

```text
.qoder/
debugger/agent/
```

и документации.

---

# 25. Не изменять src/

Обязательно:

```bash
git diff -- src/
```

должен быть пустым.

Stage 6.8 не требует изменений Vector emulator core.

---

# 26. Не добавлять AI runtime

Запрещено добавлять:

```text
LLM runtime
Python AI agent
RAG
embeddings
vector database
local model inference
```

Qwen-Lite работает внутри Qoder.

Debugger предоставляет только:

```text
Knowledge
Tasks
Profiles
Workflow
MCP evidence
```

---

# 27. Критерии завершения Stage 6.8

Stage считается завершённым, если:

* создан project-level Vector-06C Skill для Qoder;
* Skill использует существующий `AI_AGENT_WORKFLOW.md`;
* Skill использует существующие Profiles/Tasks/Knowledge;
* Qoder/Qwen-Lite способен анализировать ROM непосредственно из чата;
* MCP используется для получения фактических данных;
* анализ не зависит только от статического дизассемблирования;
* Fact / Inference / Hypothesis разделяются;
* Hardware / Emulator разделяются;
* спорные утверждения проверяются дополнительным evidence;
* control flow анализируется корректно;
* 8080 instructions интерпретируются корректно;
* данные не объявляются кодом без evidence;
* гипотезы не выдаются за факты;
* clrs.rom успешно проходит контрольный анализ;
* несколько дополнительных ROM проходят regression analysis;
* Agent API не изменён без необходимости;
* MCP не изменён без необходимости;
* `src/` не изменён;
* существующие тесты проходят.

После выполнения **не добавлять новую функциональность**.

Цель Stage 6.8 — сделать существующую инфраструктуру удобной и надёжной для реального AI-анализа ROM через Qoder/Qwen-Lite.
