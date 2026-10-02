# Stage 6.16 — MCP Reverse Engineering Primitives

## Статус: ЗАВЕРШЕНО

## Цель
Два новых MCP-инструмента:
1. `debug_read_memory_range` — массовое чтение памяти (один вызов вместо многих)
2. `debug_analyze_code` — control-flow analysis через существующий 8080 decoder

## Ограничения
- `src/` не изменять ✅ (git diff -- src/ пуст)
- Не создавать второй opcode decoder ✅ (используется существующий `disassemble()`)
- Использовать существующий дизассемблер ✅
- Не изменять RDB автоматически ✅
- Не добавлять .asm генерацию ✅

## Реализация

### A. debug_read_memory_range
- Использует существующий `AgentApi::readMemory(address, size)` — отдельный метод не потребовался
- MCP tool: `address` (0..65535), `length` (1..16384)
- Валидация: address + length ≤ 0x10000
- Лимит: `MAX_MEMORY_READ_RANGE = 16384` (16 KB)
- Возвращает: `{address, length, data[]}`

### B. debug_analyze_code
- `code_analyzer.h` — header-only control-flow analyzer (289 строк)
  - BFS от точки входа
  - Использует существующий `disassemble()` — никакого второго decoder
  - `classifyControlFlow(opcode)` — классификация 8080 opcode
  - visited set, conflict detection, code ranges formation
  - Обработка: JMP, Jcc, CALL, Ccc, RET, RCR, RST, HLT, PCHL
- `AgentApi::analyzeCode(startAddress, maxInstructions)` — метод
- MCP tool: `start_address` (0..65535), `max_instructions` (default 1000)
- Лимит: `MAX_CODE_ANALYSIS_INSTRUCTIONS = 10000`
- Возвращает: `{entry_point, instruction_count, truncated, instructions[], references[], ranges[], conflicts[]}`

### C. Интеграция
- ✅ Обновлён SKILL.md (добавлены правила §29, обновлён workflow)
- ✅ Обновлён vector06c-analyst.md (workflow §30, новые tools в списке)
- ✅ Regression tests: 50/50 integration, 44/44 MCP protocol
- ✅ MCP smoke test: load ROM → analyze_code → read_memory_range → get_rdb_info

## Изменённые файлы
| Файл | Изменение |
|------|-----------|
| `debugger/agent/code_analyzer.h` | **НОВЫЙ** — header-only control-flow analyzer |
| `debugger/agent/agent_types.h` | +2 лимита (MAX_MEMORY_READ_RANGE, MAX_CODE_ANALYSIS_INSTRUCTIONS) |
| `debugger/agent/agent_api.h` | +`analyzeCode()` declaration |
| `debugger/agent/agent_api.cpp` | +`analyzeCode()` implementation |
| `debugger/mcp/mcp_adapter.cpp` | +регистрация 2 MCP tools |
| `debugger/mcp/tests/test_mcp_protocol.cpp` | 52→54 tools count, +expected tools list |
| `debugger/agent/tests/test_agent_integration.cpp` | +тесты #49 (read_memory_range), #50 (analyze_code) |
| `.qoder/skills/vector06c-debugger/SKILL.md` | +правила Stage 6.16, обновлён workflow |
| `.qoder/agents/vector06c-analyst.md` | +workflow с analyze_code, обновлён tools list (54) |

## Результаты тестирования

### Unit/Integration tests
| Suite | Результат |
|-------|-----------|
| test_agent_api | 105/105 ✅ |
| test_agent_commands | 15/15 ✅ |
| test_agent_contract | 49/49 ✅ |
| test_agent_integration | 50/50 ✅ |
| test_mcp_protocol | 44/44 ✅ |
| test_backend | 120/120 ✅ |
| test_rdb_controller | 39/39 ✅ |
| test_map_import | 8/8 ✅ |
| test_call_graph_model | 22/22 ✅ |
| test_gui_smoke | 3/3 ✅ |

### MCP Smoke Test (production binary)
```
load ROM (clrs.rom)                    ✅
debug_analyze_code(0x0000, 500)        ✅ 286 instructions, 1 range, 0 conflicts
debug_read_memory_range(0, 64)         ✅ 64 bytes returned
debug_get_rdb_info                     ✅ loaded, rom_size=119
```

### Production chain verified
```
MCP → AgentApi → DebugBackend → IDebugTarget → DebugAdapter
```

## Критерии завершения (§38)
* ✅ существует `debug_read_memory_range`
* ✅ диапазоны памяти читаются одним MCP-вызовом
* ✅ существует `debug_analyze_code`
* ✅ анализ начинается с заданной точки входа
* ✅ анализ использует существующий 8080 decoder
* ✅ инструкции определяются по control flow
* ✅ JMP/CALL/JCC/RST корректно обрабатываются
* ✅ циклы не приводят к бесконечному анализу (visited set)
* ✅ instruction boundaries отслеживаются
* ✅ conflicts фиксируются
* ✅ code ranges формируются
* ✅ unreachable bytes не объявляются автоматически DATA
* ✅ `debug_analyze_code` не изменяет RDB автоматически
* ✅ MCP не содержит собственного decoder
* ✅ Agent не нуждается в `disasm.py`
* ✅ Agent не нуждается в прямом чтении ROM Python-скриптом
* ✅ Qoder Skill обновлён
* ✅ Qoder Agent обновлён
* ✅ MCP wire tests проходят
* ✅ Agent API tests проходят
* ✅ все regression tests проходят
* ✅ `git diff -- src/` пуст
* ✅ production MCP использует реальный AgentApi/DebugBackend/DebugAdapter
* ✅ никаких stub-реализаций нет
