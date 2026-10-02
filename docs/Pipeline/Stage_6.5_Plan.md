# Stage 6.5 — AI Task Library & Vector-06C Knowledge Base: План реализации

## Контекст

MCP предоставляет **инструменты** (debug_read_memory, debug_disassemble, etc.).
Task Library предоставляет **методику** выполнения задач.
Knowledge Base предоставляет **проверенные технические сведения** о Векторе-06Ц.
Profiles объединяют задачи и знания для типовых сценариев.

**Главный принцип:** Это НЕ код, а version-controlled Markdown-документы + validator.

---

## Архитектура

```
debugger/agent/
├── tasks/
│   ├── generate_map/TASK.md
│   ├── stack_safety/TASK.md
│   ├── find_bugs/TASK.md
│   ├── analyze_vram/TASK.md
│   └── analyze_io/TASK.md
├── knowledge/
│   └── vector06c/
│       ├── architecture.md
│       ├── cpu.md
│       ├── memory.md
│       ├── video.md
│       ├── io.md
│       ├── keyboard.md
│       ├── sound.md
│       └── rom_format.md
├── profiles/
│   ├── rom_audit.md
│   ├── reverse_engineering.md
│   └── bug_hunting.md
└── tests/
    └── validate_agent_knowledge.py
```

---

## Фазы реализации

### Фаза 1: Структура каталогов (15 мин)
Создать `tasks/`, `knowledge/vector06c/`, `profiles/`.

### Фаза 2: Knowledge Base — ядро (4-5 ч)
Создать документы с metadata (platform, topic, status, source):
- `architecture.md` — общая архитектура Вектора-06Ц
- `cpu.md` — КР580ВМ80А, регистры, флаги, прерывания
- `memory.md` — карта памяти 64К, RAM/ROM/VRAM
- `video.md` — видеорежимы, VRAM, палитра
- `io.md` — порты ввода-вывода
- `keyboard.md` — клавиатура, сканкоды
- `sound.md` — AY-3-8912, звук
- `rom_format.md` — форматы ROM (.rom, .r0m, .map)

Каждый документ: verified/partially_verified/unverified + source.

### Фаза 3: Task Library — 5 задач (3-4 ч)
Стандартная структура TASK.md:
- ID, Description, Required Knowledge, Required Tools
- Procedure, Checks, Output, Limitations

Задачи:
1. `generate_map` — анализ ROM, создание MAP-информации
2. `stack_safety` — проверка стека (SP, PUSH, POP, CALL, RET)
3. `find_bugs` — статический/динамический аудит (confirmed/probable/suspicious)
4. `analyze_vram` — анализ видеопамяти
5. `analyze_io` — анализ I/O портов

### Фаза 4: Profiles — 3 профиля (1-2 ч)
- `rom_audit.md` — комплексная проверка ROM
- `reverse_engineering.md` — реверс-инжиниринг
- `bug_hunting.md` — поиск ошибок

### Фаза 5: Validator (2-3 ч)
`validate_agent_knowledge.py`:
- TASK references existing tool
- TASK references existing knowledge
- PROFILE references existing task/knowledge
- IDs unique
- metadata valid
- status допустимые значения

### Фаза 6: Тестирование (1 ч)
- Запустить validator
- Проверить все 226 существующих тестов
- Убедиться, что src/ не изменён

### Фаза 7: Документация и коммит (30 мин)
- Обновить README
- Коммит

---

## Ограничения
- НЕ менять src/
- НЕ добавлять debugger hooks
- НЕ создавать новые MCP tools
- НЕ создавать AI runtime
- НЕ делать RAG/embeddings/vector DB

## Оценка времени: 12-16 ч
