# Stage 6.13 — RDB Links API и документация Agent

## 1. Agent API — 3 новых метода

Файлы: `debugger/agent/agent_api.h`, `debugger/agent/agent_api.cpp`

```cpp
AgentApiResult<void> addRdbLink(uint16_t source, uint16_t target);
AgentApiResult<void> removeRdbLink(uint16_t source, uint16_t target);
AgentApiResult<std::vector<uint16_t>> getRdbLinks(uint16_t source);
```

- `addRdbLink`: проверить source object, добавить target в links (идемпотентно), dirty
- `removeRdbLink`: найти source, удалить target из links (NotFound если нет), dirty
- `getRdbLinks`: вернуть sorted vector адресов (NotFound если source нет)
- Target может не существовать как RDB object (unresolved)
- Без автосохранения, без перестройки Call Graph

## 2. MCP tools — 3 новых инструмента

Файлы: `debugger/mcp/mcp_tools.h`, `debugger/mcp/mcp_tools.cpp`, `debugger/mcp/mcp_json.h/cpp`

- `debug_add_rdb_link` — args: `{source, target}`
- `debug_remove_rdb_link` — args: `{source, target}`
- `debug_get_rdb_links` — args: `{source}`

Тонкий adapter → AgentApi. JSON Schema, isError для ошибок.

## 3. Тесты Agent API

Файл: `debugger/agent/tests/test_agent_api.cpp`

Покрыть сценарии:
- add link (existing target, unresolved target, duplicate, multiple, self-link, cyclic)
- remove link (existing, missing, one of multiple)
- get links (empty, one, multiple, stable ordering)
- persistence (add → save → reload → verify)
- object interaction (remove source/target → verify links)

## 4. Тесты MCP wire-level

Файл: `debugger/mcp/tests/test_mcp_protocol.cpp`

- valid add/get/remove
- invalid source (no object)
- duplicate link
- unresolved target
- missing link removal
- persistence через save_rdb
- корректный JSON response и isError

Обновить счётчик tools (текущее + 3).

## 5. Проверка removeRdbObject семантики

Проверить текущее поведение `removeObject()`:
- исходящие links удаляются с объектом
- входящие links из других объектов НЕ удаляются

Добавить тест, фиксирующий поведение. Не менять реализацию без необходимости.

## 6. Документация Agent

Обновить файлы:
- `.qoder/skills/vector06c-debugger/SKILL.md`
- `.qoder/agents/vector06c-analyst.md`
- `debugger/agent/AI_AGENT_WORKFLOW.md`
- `debugger/agent/knowledge/rdb_format.md`

Добавить:
- Правило: Agent НЕ редактирует .rdb напрямую, только через MCP
- Workflow: Analysis → Evidence → RDB objects → RDB links → Save → Build Call Graph → Review
- Evidence-first: Fact / Inference / Hypothesis
- Links семантика: directed, unresolved target OK, no duplicates
- `rdb.graph` — визуальные данные, links — семантические (в .rdb)

## 7. GUI callback (прикрепить к изменениям пользователя)

Убедиться что `callGraphWindow_.markOutdated()` и `callGraphWindow_.onRomLoaded()` корректно работают с новыми links. Call Graph перестраивается по кнопке Build, не автоматически.

## 8. Build + тесты

- Собрать, проверить компиляцию
- Запустить все тесты: test_agent_api, test_mcp_protocol, test_rdb_controller, test_backend, test_map_import
- Проверить что `src/` не изменён

## 9. Коммит

Формат по конвенции. Не коммитить без явного разрешения пользователя.
