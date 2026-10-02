Пишем дебаггер в debugger. Вот новое ТЗ. Его сохранить в папку debugger/docs/Pipeline/.  И можно приступать к выполнению.

# Stage 6.6 — Knowledge Base Integration & Verification

## Проверка, очистка и интеграция Vector-06C Knowledge Base

### Цель

Привести Knowledge Base Vector-06C в состояние, при котором её содержимое можно безопасно использовать внешним AI Agent при анализе ROM.

Основной принцип:

```text
verification.md
      ↓
verified Knowledge Base
      ↓
Tasks
      ↓
AI Agent
      ↓
MCP
```

На этом этапе **не создавать AI Agent**, не добавлять MCP tools и не расширять Agent API.

---

# 1. Исходная структура

Использовать существующую структуру:

```text
debugger/agent/
├── knowledge/
│   └── vector06c/
│       ├── architecture.md
│       ├── cpu.md
│       ├── memory.md
│       ├── video.md
│       ├── io.md
│       ├── keyboard.md
│       ├── sound.md
│       ├── rom_format.md
│       └── verification.md
│
├── tasks/
│   ├── generate_map/
│   ├── stack_safety/
│   ├── find_bugs/
│   ├── analyze_vram/
│   └── analyze_io/
│
└── profiles/
    ├── rom_audit.md
    ├── reverse_engineering.md
    └── bug_hunting.md
```

`verification.md` является **метадокументом проверки достоверности**, а не заменой тематических документов.

---

# 2. Проверить все Knowledge Base документы

Проверить:

```text
architecture.md
cpu.md
memory.md
video.md
io.md
keyboard.md
sound.md
rom_format.md
```

Каждое существенное утверждение должно быть сопоставлено с:

```text
verification.md
```

Проверять не только наличие самого факта, но и его статус:

```text
VERIFIED_BY_CODE
VERIFIED_BY_PRIMARY_SOURCE
VERIFIED
PARTIALLY_VERIFIED
CONFLICT
UNVERIFIED
INCORRECT
```

---

# 3. Не считать `status: verified` абсолютной истиной

В Knowledge Base нельзя создавать впечатление, что любой факт со статусом:

```yaml
status: verified
```

доказан оригинальным железом.

Необходимо различать как минимум:

```text
verified by emulator/code
verified by primary documentation
verified by independent measurement
partially verified
conflicting sources
unknown
```

Если факт подтверждён только поведением эмулятора, это должно быть явно понятно из документа.

Например:

```yaml
status: verified
source: emulator
```

означает:

> факт подтверждён текущей реализацией эмулятора.

Это не означает автоматического подтверждения оригинальной аппаратурой.

---

# 4. Разделять Hardware и Emulator Behavior

В Knowledge Base необходимо явно различать:

```text
Original Hardware
```

и:

```text
Emulator Behavior
```

Особенно для:

```text
CPU timing
wait states
video timing
memory mapping
I/O
undocumented behavior
ROM/boot behavior
```

Нельзя выдавать поведение конкретного эмулятора за доказанное поведение оригинального Vector-06C.

Если источник факта — VSDL или EMU80, это должно быть обозначено.

---

# 5. Обязательно исправить известные ошибки

В процессе проверки учитывать результаты `verification.md`.

В частности:

### Palette

Не утверждать, что:

```text
Port B напрямую задаёт индекс цвета пикселя.
```

Корректно разделить:

```text
pixel data → palette index
Port B → border / video mode related control
```

согласно подтверждённым данным.

---

### Screen RAM

Не описывать 32 KB экранной памяти как дополнительную память сверх 64 KB.

Корректная модель:

```text
64 KB address space
└── upper 32 KB
    └── screen-related memory
```

если именно это подтверждено соответствующим источником.

---

### Wait states

Если таблица timing/wait states взята из EMU80, нельзя описывать её как непосредственно подтверждённую характеристику оригинального Vector-06C без соответствующей оговорки.

---

### 512×256

Сохранять различие между:

```text
логическим представлением плоскостей
```

и:

```text
физическим расположением/нумерацией плоскостей
```

если источники используют различную терминологию.

---

### TIMSoft / EMU80 / VSDL

При конфликте источников не выбирать вариант «на глаз».

Конфликт должен оставаться явно обозначенным.

Известные конфликты:

```text
TIMSoft:
Port 02h = mathematical color code

VSDL + EMU80:
другая интерпретация
```

и timing:

```text
CALL
XTHL
```

Если нет аппаратного подтверждения, не объявлять один вариант окончательно истинным.

---

# 6. Обработка конфликтов

Для каждого `CONFLICT` использовать формат:

```text
Claim
Source A
Source B
Current emulator behavior
What is established
What is unknown
Required verification
```

Например:

```text
Status: CONFLICT

Claim:
CALL instruction timing

Source A:
...

Source B:
...

Emulator behavior:
...

Established:
...

Unknown:
...

Required verification:
cycle-exact hardware measurement
```

Запрещается самостоятельно устранять конфликт выбором наиболее «правдоподобной» версии.

---

# 7. Обработка UNVERIFIED

Для неподтверждённых сведений использовать явный статус:

```text
UNVERIFIED
```

Такие сведения могут находиться в Knowledge Base только при условии, что они явно обозначены как неподтверждённые.

AI Agent не должен воспринимать их как установленные технические факты.

Если утверждение не нужно для работы Knowledge Base и не имеет ценности как открытый вопрос — его можно удалить.

---

# 8. Обработка INCORRECT

Все сведения, отмеченные в `verification.md` как:

```text
INCORRECT
```

не должны оставаться в тематических KB-документах как факты.

Допускается оставить информацию только в историческом/аудиторском контексте:

```text
Previously claimed:
...

Verified correction:
...
```

если это необходимо для понимания истории проверки.

---

# 9. Metadata

Все тематические KB-документы должны иметь единообразный metadata block.

Например:

```yaml
---
platform: Vector-06C
topic: memory
status: verified
source: emulator
---
```

Допустимые значения `status`:

```text
verified
partially_verified
conflict
unverified
```

Допустимые значения `source` должны однозначно указывать происхождение информации, например:

```text
emulator
primary_source
measurement
community
multiple
```

Не вводить сложную систему scoring/confidence без необходимости.

---

# 10. Не дублировать verification report

`verification.md` содержит результаты аудита и доказательства.

Тематические документы должны содержать сами технические сведения.

Не требуется переносить всю таблицу доказательств в каждый KB-файл.

Правильная схема:

```text
cpu.md
    ↓
техническое утверждение

verification.md
    ↓
почему утверждение считается достоверным
```

---

# 11. Связать Tasks с Knowledge Base

Проверить существующие Tasks:

```text
generate_map
stack_safety
find_bugs
analyze_vram
analyze_io
```

Каждый Task должен явно указывать необходимые Knowledge Base документы.

Например:

```yaml
knowledge:
  - vector06c/memory.md
  - vector06c/cpu.md
  - vector06c/verification.md
```

Не указывать Knowledge Base, которая фактически не используется задачей.

---

# 12. Правило разделения Task и Knowledge

Не переносить технические факты из Knowledge Base в Tasks без необходимости.

### Knowledge Base

Отвечает:

```text
Как работает Vector-06C?
```

### Task

Отвечает:

```text
Как выполнить анализ?
```

Например:

```text
memory.md
    → описание memory map

find_bugs/TASK.md
    → алгоритм поиска подозрительных обращений к памяти
```

Методология и факты должны оставаться разделёнными.

---

# 13. Проверить Profiles

Проверить:

```text
rom_audit.md
reverse_engineering.md
bug_hunting.md
```

Profiles должны ссылаться только на существующие:

```text
Tasks
Knowledge
```

Не должно быть ссылок на:

```text
несуществующие файлы
старые имена документов
несуществующие MCP tools
```

---

# 14. Усилить Validator

Расширить:

```text
debugger/agent/tests/validate_agent_knowledge.py
```

Validator должен проверять:

### Files

* все обязательные KB-файлы существуют;
* существует `verification.md`;
* все Tasks существуют;
* все Profiles существуют.

### Metadata

* присутствует metadata;
* `platform` корректен;
* `topic` корректен;
* `status` входит в допустимый список;
* `source` корректен.

### References

Проверять:

```text
Task → Knowledge
Profile → Task
Profile → Knowledge
```

Все ссылки должны указывать на существующие документы.

### Status consistency

Не должно быть ситуации, когда документ или Task утверждает факт как установленный, если соответствующий источник в `verification.md` помечен:

```text
CONFLICT
```

или:

```text
UNVERIFIED
```

без соответствующего предупреждения.

---

# 15. Validator не должен проверять истинность фактов автоматически

Validator не является системой фактологической проверки.

Он проверяет:

```text
структуру
metadata
ссылки
статусы
consistency
```

Но не пытается самостоятельно определить, действительно ли:

```text
Port X = Y
```

или:

```text
instruction timing = N
```

Фактологическая проверка выполняется человеком/исследованием и фиксируется в:

```text
verification.md
```

---

# 16. Проверить Tasks на использование существующих MCP tools

Для каждого Task проверить:

```text
required tools
```

с существующим MCP API.

На этом этапе:

* не добавлять новые MCP tools;
* не менять Agent API;
* не менять MCP protocol;
* не создавать Task Runner.

Если Task требует несуществующий инструмент — исправить Task так, чтобы использовать существующий API, либо явно отметить невозможность операции.

---

# 17. Не добавлять RAG

На этом этапе не создавать:

```text
embeddings
vector database
RAG pipeline
semantic search
LLM runtime
```

Knowledge Base остаётся обычным набором Markdown-документов.

В дальнейшем внешний AI Agent сможет сам выбирать необходимые документы.

---

# 18. Проверка качества

После изменений выполнить:

```bash
python3 debugger/agent/tests/validate_agent_knowledge.py
```

Ожидаемый результат:

```text
0 errors
0 warnings
```

Также запустить все существующие тесты проекта.

Ожидается отсутствие регрессий.

---

# 19. Проверка изменений

Проверить:

```bash
git status
git diff
git diff -- src/
```

Критическое требование:

```text
git diff -- src/
```

должен быть пустым.

Также не должны изменяться:

```text
debugger/agent/agent_api.*
debugger/mcp/*
```

если такие изменения не являются абсолютно необходимыми для исправления документации.

Предпочтительно изменять только:

```text
debugger/agent/knowledge/
debugger/agent/tasks/
debugger/agent/profiles/
debugger/agent/tests/validate_agent_knowledge.py
```

---

# 20. Запрещённые изменения

В Stage 6.6 НЕ делать:

```text
новые MCP tools
новые Agent API методы
Task Runner
AI runtime
LLM integration
RAG
embeddings
vector database
изменения src/
изменения Board
изменения DebugAdapter
изменения DebugBackend
```

Это исключительно этап подготовки и верификации Knowledge Base.

---

# 21. Финальная структура

После Stage 6.6 ожидается:

```text
debugger/agent/
├── knowledge/
│   └── vector06c/
│       ├── architecture.md
│       ├── cpu.md
│       ├── memory.md
│       ├── video.md
│       ├── io.md
│       ├── keyboard.md
│       ├── sound.md
│       ├── rom_format.md
│       └── verification.md
│
├── tasks/
│   ├── generate_map/
│   ├── stack_safety/
│   ├── find_bugs/
│   ├── analyze_vram/
│   └── analyze_io/
│
└── profiles/
    ├── rom_audit.md
    ├── reverse_engineering.md
    └── bug_hunting.md
```

---

# 22. Критерий завершения Stage 6.6

Stage считается завершённым только если одновременно:

* все 8 тематических KB проверены относительно `verification.md`;
* известные ошибки исправлены;
* Hardware и Emulator Behavior различаются;
* `CONFLICT` явно обозначены;
* `UNVERIFIED` не выдаются за факты;
* `INCORRECT` удалены или явно исправлены;
* metadata унифицирован;
* Tasks имеют корректные ссылки на Knowledge;
* Profiles имеют корректные ссылки на Tasks/Knowledge;
* validator проверяет структуру и consistency;
* validator показывает `0 errors`;
* все существующие тесты проходят;
* `git diff -- src/` пустой;
* Agent API не изменён;
* MCP не изменён;
* новых инструментов не добавлено;
* RAG/embeddings/AI runtime не добавлены.

После выполнения **не добавлять новую функциональность**.

Stage 6.6 является финальной подготовкой Knowledge Base перед проектированием внешнего AI Age