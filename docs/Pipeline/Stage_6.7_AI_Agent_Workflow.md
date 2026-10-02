# Stage 6.7 — AI Agent Workflow & Analysis Protocol

## Протокол работы внешнего AI Agent с Vector Debugger

### Цель

Определить единый и воспроизводимый протокол работы внешнего AI Agent с:

```text
Task Library
Knowledge Base
Profiles
MCP Server
Agent API
```

AI Agent должен уметь самостоятельно выполнять исследование и анализ ROM Vector-06C, используя существующий MCP API.

На этом этапе **не реализуется сам AI Agent**.

---

# 1. Архитектура

Целевая схема:

```text
                 ┌─────────────────────┐
                 │     AI Agent        │
                 │ Claude / Local LLM  │
                 └──────────┬──────────┘
                            │
                 ┌──────────▼──────────┐
                 │      Profile        │
                 └──────────┬──────────┘
                            │
                    Tasks + Knowledge
                            │
                 ┌──────────▼──────────┐
                 │    Analysis Loop    │
                 └──────────┬──────────┘
                            │
                         MCP
                            │
                 ┌──────────▼──────────┐
                 │    Vector Debugger  │
                 └─────────────────────┘
```

AI Agent находится **вне** `vector-debugger`.

`vector-debugger` предоставляет:

```text
MCP
Agent API
Knowledge Base
Task Library
Profiles
```

но не содержит LLM runtime.

---

# 2. Роли компонентов

## AI Agent

Отвечает за:

* понимание задачи;
* выбор Profile;
* выбор Tasks;
* чтение Knowledge;
* планирование анализа;
* вызовы MCP;
* интерпретацию результатов;
* формирование гипотез;
* проверку гипотез;
* формирование итогового отчёта.

---

## Profile

Определяет тип исследования.

Например:

```text
rom_audit
reverse_engineering
bug_hunting
```

Profile не выполняет анализ самостоятельно.

Он определяет:

```text
какие Tasks использовать;
какие Knowledge документы необходимы;
какой результат требуется получить.
```

---

## Task

Определяет методику выполнения конкретной операции.

Например:

```text
generate_map
stack_safety
find_bugs
analyze_vram
analyze_io
```

Task должен отвечать:

```text
Что анализировать?
Какие данные нужны?
Какие MCP tools использовать?
Как интерпретировать результат?
Какие проверки выполнить?
Какой результат считать завершённым?
```

---

## Knowledge Base

Содержит технические знания о Vector-06C.

Knowledge Base не содержит алгоритм выполнения конкретной задачи.

---

## MCP

Предоставляет AI Agent доступ к Debugger.

MCP является транспортным/протокольным интерфейсом.

В MCP не должна находиться методология анализа.

---

# 3. Основной workflow

Каждый анализ должен выполняться по следующей схеме:

```text
1. определить задачу
2. выбрать Profile
3. загрузить Profile
4. определить Tasks
5. загрузить необходимые Knowledge
6. сформировать Analysis Plan
7. проверить состояние Debugger
8. загрузить ROM при необходимости
9. выполнить MCP operations
10. анализировать результаты
11. сформировать гипотезы
12. проверить гипотезы дополнительными MCP operations
13. оценить доказательства
14. сформировать результат
15. указать ограничения и неизвестные
```

---

# 4. Analysis Plan

Перед сложным анализом Agent должен сформировать внутренний план.

Минимальная структура:

```text
Goal
Profile
Tasks
Knowledge
Required MCP operations
Expected evidence
Validation steps
Output
```

Пример:

```text
Goal:
Find suspicious memory accesses.

Profile:
bug_hunting

Tasks:
find_bugs
stack_safety

Knowledge:
cpu.md
memory.md
verification.md

MCP:
debug_load_rom
debug_get_cpu_state
debug_get_memory_map
debug_get_execution_trace
debug_get_instruction_history

Validation:
reproduce suspicious address
inspect surrounding instructions
check whether access is valid for the current memory map

Output:
list of findings with evidence
```

Analysis Plan не является новым API и не обязан сохраняться в проекте.

---

# 5. Не выполнять MCP operations вслепую

Перед использованием MCP Agent должен понимать:

```text
какую информацию он получает;
зачем она нужна;
какую гипотезу она проверяет.
```

Не следует выполнять большое количество случайных запросов только для получения данных.

Каждый существенный MCP вызов должен иметь аналитическую цель.

---

# 6. Состояние Debugger

Перед state-dependent операциями Agent должен определить состояние эмулятора.

Использовать:

```text
debug_get_debug_state
```

или соответствующие существующие инструменты.

Не предполагать:

```text
ROM loaded
CPU paused
CPU running
breakpoint active
```

если это не подтверждено текущим состоянием.

---

# 7. Run / Pause / Step

Следовать существующей семантике DebugBackend.

Команды:

```text
run
pause
step
reset
```

являются запросами на изменение состояния.

После операции Agent при необходимости должен проверить фактическое состояние через:

```text
debug_get_debug_state
```

Нельзя считать:

```text
run() accepted
```

эквивалентом:

```text
CPU is already running
```

Аналогично для `pause()`.

---

# 8. Работа с ROM

Если анализ требует ROM:

```text
debug_load_rom
```

Agent должен проверить результат загрузки.

После загрузки рекомендуется получить:

```text
CPU state
memory map
symbols
```

в зависимости от выбранного Task.

Не следует автоматически загружать ROM, если он уже загружен и текущая задача не требует замены ROM.

---

# 9. Evidence-first analysis

AI Agent не должен делать техническое утверждение только на основании предположения.

Для каждого существенного вывода желательно иметь:

```text
Observation
Evidence
Interpretation
Conclusion
```

Например:

```text
Observation:
instruction writes to address C000h.

Evidence:
execution trace + memory map.

Interpretation:
C000h belongs to screen plane.

Conclusion:
instruction modifies VRAM.
```

---

# 10. Факты, выводы и гипотезы

Agent должен различать три уровня:

### Fact

Непосредственно подтверждённый факт.

```text
PC = 8123h
```

### Inference

Вывод из нескольких подтверждённых фактов.

```text
8123h lies inside a known code region.
```

### Hypothesis

Предположение, требующее дополнительной проверки.

```text
This routine may be responsible for sprite rendering.
```

Нельзя выдавать Hypothesis за Fact.

---

# 11. Использование Knowledge Base

Knowledge Base должна использоваться как технический reference.

При конфликте:

```text
Knowledge
vs
observed emulator behavior
```

Agent должен явно сообщать об этом.

При наличии:

```text
CONFLICT
```

в `verification.md` нельзя самостоятельно выбирать сторону без дополнительных доказательств.

---

# 12. Emulator Behavior vs Original Hardware

При формировании вывода Agent обязан учитывать источник информации.

Если результат подтверждает только:

```text
current emulator behavior
```

нельзя формулировать его как:

```text
original Vector-06C hardware definitely does X
```

Корректная формулировка:

```text
The current emulator implements X.
```

или:

```text
According to the verified emulator behavior, X occurs.
```

Если аппаратное поведение подтверждено независимо — это должно быть указано отдельно.

---

# 13. Проверка гипотез

После обнаружения подозрительного поведения Agent не должен немедленно объявлять его ошибкой.

Использовать цикл:

```text
Observation
    ↓
Hypothesis
    ↓
Additional evidence
    ↓
Test
    ↓
Confirmed / Rejected / Uncertain
```

Например:

```text
Instruction writes unexpected byte
        ↓
Hypothesis: wrong memory mapping
        ↓
inspect memory map
        ↓
inspect surrounding instructions
        ↓
inspect execution trace
        ↓
conclusion
```

---

# 14. Не делать вывод из одного MCP результата

Один результат может быть недостаточен для сложного вывода.

Например:

```text
address = C000h
```

сам по себе не доказывает:

```text
bug
```

Нужно учитывать:

```text
CPU state
memory map
instruction context
program flow
known hardware behavior
```

если это требуется выбранной задачей.

---

# 15. Iterative Analysis Loop

Основной цикл анализа:

```text
┌──────────────────────┐
│ Current hypothesis   │
└──────────┬───────────┘
           ↓
┌──────────────────────┐
│ Select MCP operation │
└──────────┬───────────┘
           ↓
┌──────────────────────┐
│ Observe result       │
└──────────┬───────────┘
           ↓
┌──────────────────────┐
│ Update hypothesis    │
└──────────┬───────────┘
           │
           ├── confirmed ──→ conclusion
           │
           ├── rejected ───→ new hypothesis
           │
           └── insufficient
                    ↓
             additional test
```

Количество итераций не должно быть искусственно ограничено Task'ом, если MCP API и ресурсы позволяют продолжить исследование.

---

# 16. Работа с большими данными

Agent должен использовать существующие лимиты Agent API:

```text
MAX_DISASSEMBLY_COUNT
MAX_SYMBOLS_LIMIT
MAX_CALL_GRAPH_LIMIT
MAX_STACK_LIMIT
MAX_TRACE_ENTRIES
MAX_HISTORY_ENTRIES
```

Не запрашивать без необходимости максимальный объём данных.

Предпочтительно:

```text
small targeted query
```

вместо:

```text
entire dataset
```

если задача этого не требует.

---

# 17. Локализация анализа

При исследовании большого ROM Agent должен сначала локализовать интересующий участок.

Например:

```text
ROM
 ↓
memory map
 ↓
symbols/functions
 ↓
interesting function
 ↓
disassembly
 ↓
trace
 ↓
specific instruction
```

Не следует начинать с полного анализа всех 64 KB, если задача касается конкретной функции.

---

# 18. Работа с символами

Если ROM имеет `.map`/symbols:

```text
symbols
functions
xrefs
call graph
```

должны использоваться для повышения точности анализа.

Предпочтительно формулировать вывод:

```text
function DRAW_FIRE at 8A20h
```

вместо:

```text
routine at 8A20h
```

если символ действительно подтверждён.

---

# 19. Работа без символов

Если символов нет:

Agent может использовать:

```text
disassembly
execution trace
instruction history
memory accesses
I/O accesses
call graph
```

Но не должен придумывать имена функций как установленные факты.

Допустимо использовать временные обозначения:

```text
subroutine_8123
candidate_renderer
unknown_io_handler
```

при условии, что это явно аналитическое обозначение.

---

# 20. Bug Finding

При поиске ошибки результат должен содержать:

```text
Location
Observed behavior
Expected behavior
Evidence
Reasoning
Confidence
Suggested next verification
```

Например:

```text
Address: 8234h

Observed:
...

Expected:
...

Evidence:
...

Reasoning:
...

Confidence:
high

Next verification:
...
```

Agent не должен заявлять «bug» только потому, что код выглядит необычно.

---

# 21. Неизвестный результат

Если доказательств недостаточно:

```text
UNKNOWN
```

или:

```text
UNCONFIRMED
```

является допустимым результатом.

Лучше сообщить:

```text
insufficient evidence
```

чем сделать необоснованный вывод.

---

# 22. Final Report

Итоговый отчёт должен иметь стандартную структуру:

```text
# Analysis

## Goal

## ROM

## Profile

## Tasks

## Findings

### Finding 1

Location:
Observation:
Evidence:
Conclusion:
Confidence:

### Finding 2
...

## Verified Facts

## Inferences

## Hypotheses

## Unknowns

## Limitations

## Recommended Next Steps
```

Для простых задач допускается сокращённый формат.

---

# 23. Confidence

Не вводить сложную математическую систему confidence.

Достаточно:

```text
high
medium
low
```

Правило:

```text
high
```

только при наличии достаточных независимых подтверждений.

```text
medium
```

если вывод хорошо обоснован, но есть ограничения.

```text
low
```

если вывод основан на неполных данных или требует дополнительной проверки.

---

# 24. Ошибки AI Agent

Agent должен уметь обнаруживать собственные ошибки анализа.

Если новое наблюдение противоречит предыдущему выводу:

```text
не скрывать противоречие;
пересмотреть гипотезу;
повторить необходимые проверки.
```

Не разрешается просто игнорировать противоречащие результаты MCP.

---

# 25. Запрещённые предположения

Agent не должен автоматически предполагать:

```text
CPU clock
undocumented opcode behavior
hardware timing
I/O semantics
memory behavior
video behavior
sound behavior
```

если это не подтверждено Knowledge Base, MCP observation или другим явно указанным источником.

---

# 26. Работа с конфликтами Knowledge Base

Если Knowledge Base содержит:

```text
CONFLICT
```

Agent должен включить это в reasoning.

Например:

```text
Known conflict:
CALL timing differs between sources.

Therefore:
timing-based conclusion cannot be considered hardware-proven.
```

Нельзя молча выбирать одну версию.

---

# 27. Reproducibility

Существенный результат анализа должен быть воспроизводим.

Для каждого Finding желательно сохранить:

```text
ROM
address
relevant MCP operations
important parameters
observed result
```

Это позволит другому Agent повторить проверку.

Не требуется сохранять абсолютно каждый MCP вызов.

---

# 28. MCP остаётся stateless protocol layer

Не добавлять в MCP:

```text
analysis state
hypothesis engine
task engine
AI memory
reasoning engine
```

MCP только предоставляет операции Debugger.

Состояние анализа находится у внешнего AI Agent.

---

# 29. Task Library остаётся декларативной

Task не должен превращаться в программный сценарий.

Не создавать:

```text
Task Runner
Task Engine
Workflow Engine
```

Task остаётся Markdown-инструкцией.

---

# 30. Profile остаётся декларативным

Profile только объединяет:

```text
Tasks
Knowledge
goal/output expectations
```

Не добавлять в Profile исполняемую логику.

---

# 31. Поддержка разных AI

Протокол должен быть независимым от конкретной модели.

Он должен одинаково подходить для:

```text
Claude
GPT
локальной LLM
другого AI Agent
```

Нельзя использовать API конкретного AI-провайдера внутри Knowledge/Tasks/Profiles.

---

# 32. Работа с локальной LLM

Knowledge Base и Tasks должны быть пригодны для локальной LLM с ограниченным context window.

Поэтому Agent должен:

```text
не загружать всю KB целиком;
выбирать только необходимые документы;
читать только относящиеся к задаче Tasks;
использовать targeted MCP queries.
```

---

# 33. Минимальный Agent Prompt Contract

В дальнейшем внешний Agent должен получать базовую инструкцию примерно следующего содержания:

```text
You are analyzing a Vector-06C ROM.

Use the provided Profile, Tasks and Knowledge Base.

Use MCP tools to obtain evidence.

Do not invent hardware facts.

Distinguish:
- verified facts;
- emulator behavior;
- inference;
- hypothesis;
- unknown.

When sources conflict, report the conflict.

Do not treat an unverified claim as established fact.

Validate important hypotheses with additional MCP operations.

Produce an evidence-based final report.
```

Это пока **концептуальный контракт**, а не готовая system prompt для конкретной LLM.

---

# 34. Не добавлять Agent Runtime

На Stage 6.7 запрещено создавать:

```text
agent executable
LLM integration
OpenAI client
Claude client
Ollama integration
local model runner
agent memory database
RAG
embeddings
vector database
```

Всё это относится к будущему этапу.

---

# 35. Что должно быть реализовано в репозитории

Stage 6.7 не требует значительных изменений C++.

Допускается добавить документацию:

```text
debugger/docs/Pipeline/Stage_6.7_AI_Agent_Workflow.md
```

и при необходимости:

```text
debugger/agent/AI_AGENT_WORKFLOW.md
```

или аналогичный документ.

При необходимости можно уточнить существующие:

```text
tasks/*
profiles/*
```

но не менять MCP/Agent API ради Stage 6.7.

---

# 36. Validator

Расширить validator только если это необходимо для проверки нового workflow.

Он может проверять:

```text
Profile → Tasks
Profile → Knowledge
Task → Knowledge
```

и отсутствие битых ссылок.

Не требуется проверять качество reasoning AI Agent автоматически.

---

# 37. Тестирование

Проверить:

```text
validate_agent_knowledge.py
```

и все существующие C++ tests.

Ожидается:

```text
0 errors
0 warnings
```

и отсутствие регрессий.

---

# 38. Проверка изменений

Обязательно:

```bash
git diff -- src/
```

Результат должен быть пустым.

Также не должны изменяться:

```text
Agent API
MCP implementation
DebugBackend
DebugAdapter
Board
```

если для этого нет отдельной необходимости.

---

# 39. Критерий завершения Stage 6.7

Stage считается завершённым, если:

* определён единый AI Agent workflow;
* определены роли Profile / Task / Knowledge / MCP;
* определён Analysis Plan;
* определён evidence-first подход;
* разделены Fact / Inference / Hypothesis;
* определена обработка CONFLICT и UNVERIFIED;
* определено различие Hardware / Emulator Behavior;
* определён iterative analysis loop;
* определён стандарт итогового отчёта;
* определён принцип reproducibility;
* определена работа с большими данными;
* определена работа с ROM с символами и без символов;
* определена обработка недостаточных доказательств;
* определён минимальный Agent Prompt Contract;
* workflow не зависит от конкретной LLM;
* MCP остаётся protocol layer;
* Task Library остаётся декларативной;
* Profile остаётся декларативным;
* AI runtime не добавлен;
* RAG/embeddings не добавлены;
* Agent API не изменён;
* MCP API не изменён;
* `git diff -- src/` пуст;
* все существующие тесты проходят.

После выполнения **не добавлять AI Agent runtime и новую функциональность Debugger**.

Stage 6.7 является спецификацией взаимодействия внешнего AI Agent с существующей инфраструктурой Vector Debugger.
