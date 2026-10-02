# Stage 6.4 — MCP Adapter

## Цель

Добавить MCP-сервер (`v06c-mcp`) — тонкий протокольный адаптер над Agent API.

```
AI Agent → MCP Client → v06c-mcp → AgentApi → DebugBackend → DebugAdapter → Board
```

---

## Архитектура

### Каталог

```
debugger/
    mcp/
        mcp_server.h         — McpServer class (регистрация tools, создание сервера)
        mcp_server.cpp       — реализация
        mcp_json.h           — JSON-сериализация Agent API типов
        mcp_json.cpp         — реализация
        mcp_main.cpp         — entry point для v06c-mcp
        README.md            — документация
    thirdparty/
        cpp-mcp/             — submodule (hkr04/cpp-mcp, MIT)
```

### Зависимости

```
v06c-mcp
    → debugger_agent (AgentApi)
    → debugger_core (types)
    → cpp-mcp (MCP protocol)
    → nlohmann/json (через cpp-mcp/common/json.hpp)
    → pthread
```

НЕ зависит от: SDL, OpenGL, ImGui, Board sources.

### Ограничения

- `src/` не изменяется
- MCP обращается ТОЛЬКО к `AgentApi`
- GUI не изменяется
- Нет MCP Resources/Prompts
- Нет HTTP/SSE — только stdio transport

---

## MCP Tools (38 tools)

Все tools имеют префикс `debug_`. Каждая — тонкая обёртка над методом AgentApi.

### Execution (5)
| Tool | AgentApi method |
|---|---|
| `debug_run` | `run()` |
| `debug_pause` | `pause()` |
| `debug_step` | `step()` |
| `debug_reset` | `reset()` |
| `debug_is_running` | `isRunning()` |

### CPU (3)
| Tool | AgentApi method |
|---|---|
| `debug_get_cpu_state` | `getCpuState()` |
| `debug_get_registers` | `getCpuState()` (formatted) |
| `debug_set_register` | `setRegister(name, value)` |

### Memory (2)
| Tool | AgentApi method |
|---|---|
| `debug_read_memory` | `readMemory(address, size)` |
| `debug_write_memory` | `writeMemory(address, data)` |

### I/O (2)
| Tool | AgentApi method |
|---|---|
| `debug_read_io` | `readIo(port)` |
| `debug_write_io` | `writeIo(port, value)` |

### Breakpoints (5)
| Tool | AgentApi method |
|---|---|
| `debug_set_breakpoint` | `setBreakpoint(address)` |
| `debug_remove_breakpoint` | `clearBreakpoint(address)` |
| `debug_list_breakpoints` | `listBreakpoints()` |
| `debug_clear_breakpoints` | `clearAllBreakpoints()` |
| `debug_set_breakpoint_enabled` | `setBreakpointEnabled(address, enabled)` |

### Disassembly / Trace (3)
| Tool | AgentApi method |
|---|---|
| `debug_disassemble` | `disassemble(address, count)` |
| `debug_get_instruction_history` | `getInstructionHistory(count)` |
| `debug_get_execution_trace` | `getExecutionTrace(maxEntries)` |

### Stack (1)
| Tool | AgentApi method |
|---|---|
| `debug_get_stack` | `getStack(limit)` |

### Symbols / Analysis (5)
| Tool | AgentApi method |
|---|---|
| `debug_get_symbols` | `getSymbols(limit)` |
| `debug_get_function` | `getFunction(address)` |
| `debug_get_function_context` | `getFunctionContext(address)` |
| `debug_get_xrefs` | `getXrefs(address)` |
| `debug_get_call_graph` | `getCallGraph(address, limit)` |

### Memory Map / Video (3)
| Tool | AgentApi method |
|---|---|
| `debug_get_memory_map` | `getMemoryMap()` |
| `debug_get_vram_info` | `getVramInfo()` |
| `debug_get_screen_info` | `getScreenInfo()` |

### I/O Trace (1)
| Tool | AgentApi method |
|---|---|
| `debug_get_io_trace` | `getIoTrace(maxEntries)` |

### Debug State (1)
| Tool | AgentApi method |
|---|---|
| `debug_get_state` | `getDebugState()` |

### ROM (1)
| Tool | AgentApi method |
|---|---|
| `debug_load_rom` | `loadRom(path, org)` |

### Annotations (6)
| Tool | AgentApi method |
|---|---|
| `debug_set_comment` | `setComment(address, comment)` |
| `debug_set_function_comment` | `setFunctionComment(address, comment)` |
| `debug_rename_function` | `renameFunction(address, name)` |
| `debug_create_function` | `createFunction(address, size)` |
| `debug_delete_function` | `deleteFunction(address)` |
| `debug_add_label` | `addLabel(address, name)` |

---

## Error Handling

```
AgentApiResult<T> failure
    → MCP error response с error_code + error_message
    → isError: true в MCP content
```

Никогда не превращать ошибку в успешный ответ.

---

## JSON Serialization

Все Agent API типы сериализуются в JSON через `mcp_json.h`:
- `CpuState` → `{"pc": "0x0100", "sp": "0xF800", "a": "0x42", ...}`
- Адреса — строки hex ("0x0100") для читаемости
- Байты — массив чисел
- Breakpoints, symbols — массивы объектов

---

## v06c-mcp Executable

```cpp
int main() {
    // 1. Создать emulator (headless)
    Options.novideo = true;
    Options.nosound = true;
    DebugAdapter adapter;
    adapter.init();
    adapter.bindHal();
    DebugBackend backend(adapter);
    
    // 2. Создать AgentApi
    AgentApi api(backend);
    
    // 3. Создать MCP server
    McpServer mcp(api);
    mcp.registerAllTools();
    
    // 4. Запустить stdio transport
    mcp.runStdio();
}
```

---

## Tests

### test_mcp_protocol.cpp
- Tool registration (все 38 tools зарегистрированы)
- Schema validation (required params, types)
- Tool execution через mock backend
- Error propagation (AgentApiResult failure → MCP error)
- End-to-end: read_memory → mock → JSON result

---

## Порядок реализации

### Фаза 1: Основа
1. `mcp_json.h/cpp` — JSON-сериализация всех Agent API типов
2. `mcp_server.h/cpp` — McpServer (регистрация tools, обёртка над cpp-mcp)

### Фаза 2: Tool handlers
3. Регистрация всех 38 tools с handlers
4. Каждый handler: parse params → call AgentApi → serialize result

### Фаза 3: Executable
5. `mcp_main.cpp` — entry point для v06c-mcp

### Фаза 4: CMake + Tests
6. CMakeLists.txt — debugger_mcp library + v06c-mcp executable + test_mcp_protocol
7. Tests — protocol, registration, schema, execution, error propagation, E2E

### Фаза 5: Documentation + Regression
8. `mcp/README.md`
9. Прогнать все существующие тесты

---

## Изменённые файлы

| Файл | Характер |
|---|---|
| `debugger/mcp/mcp_server.h` | **Новый** |
| `debugger/mcp/mcp_server.cpp` | **Новый** |
| `debugger/mcp/mcp_json.h` | **Новый** |
| `debugger/mcp/mcp_json.cpp` | **Новый** |
| `debugger/mcp/mcp_main.cpp` | **Новый** |
| `debugger/mcp/README.md` | **Новый** |
| `debugger/mcp/tests/test_mcp_protocol.cpp` | **Новый** |
| `debugger/thirdparty/cpp-mcp/` | **Submodule** |
| `debugger/CMakeLists.txt` | Изменить — добавить MCP targets |
| `.gitmodules` | Изменить — добавить cpp-mcp |

**Не изменяются**: `src/`, `debugger/src/`, `debugger/gui/`, `debugger/agent/`

---

## Критерий завершения

1. ✅ `v06c-mcp` собирается и запускается
2. ✅ stdio transport работает
3. ✅ 38 tools зарегистрированы с корректными schemas
4. ✅ MCP → AgentApi только (нет прямых обращений к Board/Memory)
5. ✅ AgentApiResult errors корректно передаются
6. ✅ AgentLimits соблюдаются
7. ✅ `debugger_core` не получает MCP-зависимостей
8. ✅ MCP tests проходят
9. ✅ Все существующие tests проходят
10. ✅ `src/` не изменён
