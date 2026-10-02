# Stage 6.14 — ROM Mapping: Entry Point, RDB Links и обязательное сохранение

## Цель

Уточнить workflow AI Agent при первичном исследовании ROM. Три проблемы:
1. Agent начинает с `_main`, а не с `0x0000`
2. Agent не сохраняет `.rdb` без команды
3. Agent не создаёт links без команды

## Изменения — только документация

`src/` не менять. MCP/Agent API не менять (Stage 6.13 дал нужные инструменты).

## 1. SKILL.md

Файл: `.qoder/skills/vector06c-debugger/SKILL.md`

Добавить раздел `## ROM Mapping Entry Point`:
- Initial ROM mapping always starts at 0x0000
- `_main` is not the ROM mapping entry point
- MAP symbols used for identification, not as entry override

Добавить раздел `## Mandatory RDB Completion`:
- ROM mapping incomplete without confirmed RDB links
- Agent must create links after objects
- Agent must save RDB via `debug_save_rdb` without waiting for user command
- Report must include objects count, links count, save status

Обновить workflow — после шага 16 (save objects) добавить:
- 17: Create confirmed RDB links
- 18: Save RDB if links added
- 19: Verify save result
- 20: Final report (со сдвигом нумерации)

## 2. vector06c-analyst.md

Файл: `.qoder/agents/vector06c-analyst.md`

В workflow явно указать:
- ROM entry point = 0x0000 (не `_main`)
- Обязательные финальные шаги: Create RDB links → Save RDB → Verify save
- Не формулировать как optional/if requested/when useful

## 3. AI_AGENT_WORKFLOW.md

Файл: `debugger/agent/AI_AGENT_WORKFLOW.md`

Добавить обязательную последовательность:
```
ROM → 0x0000 → MCP analysis → objects → confirmed links → save RDB → report
```

Явно указать:
- "Creating RDB objects without creating confirmed links is an incomplete ROM mapping."
- "Creating or modifying RDB data without saving it is an incomplete ROM mapping."

Добавить раздел "ROM Mapping Entry Point" с правилом 0x0000.
Обновить итоговый отчёт — добавить objects/links count, save status.

## 4. rdb_format.md

Файл: `debugger/agent/knowledge/rdb_format.md`

Проверить и добавить/уточнить:
- links — часть semantic RDB
- links создаются Agent при подтверждённом control/data flow
- `.rdb.graph` не является заменой links
- после изменения objects/links RDB должен быть сохранён
- Call Graph визуализирует существующие links

## 5. Тесты документации

Проверить наличие в обновлённых файлах:
- Правило `0x0000` как entry point
- Запрет `_main` как entry
- Обязательный шаг: create confirmed RDB links
- Обязательный шаг: debug_save_rdb
- Порядок: 0x0000 → objects → links → save

Сохранить существующие проверки Knowledge Validator.

## 6. Регрессии

Запустить:
- test_agent_api
- test_mcp_protocol
- test_rdb_controller
- test_map_import
- test_gui_smoke
- test_call_graph_model

## 7. Коммит

Формат по конвенции. Не коммитить без разрешения.
