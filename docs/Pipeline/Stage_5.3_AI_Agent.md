## Stage 5.3 — Dynamic Function Discovery & AI Agent

### Структура

```
debugger/agent/
├── agent_types.h          # FunctionContext, TraceResult, Annotation, AgentLogEntry
├── agent_api.h            # AgentApi class (20+ методов)
├── agent_api.cpp          # Primitive ops + high-level analysis
├── agent_log.h/.cpp       # Журнал операций
└── tests/
    ├── mock_backend_for_agent.h   # Mock IDebugBackend
    ├── test_agent_api.cpp         # Unit tests (28+ тестов)
    └── test_agent_mock.cpp        # Full scenario test
```

### Phase 1: Типы + Agent Log
- `agent_types.h` — FunctionContext (instructions, callers, callees, memory/IO/VRAM access, stack behavior), TraceResult, Annotation (с confidence), AgentLogEntry
- `agent_log.h/.cpp` — thread-safe журнал с timestamp, tool, args, result, time

### Phase 2: Примитивные операции (AgentApi)
Делегация в IDebugBackend:
- Execution: run/pause/step/reset → requestRun/requestPause/stepInstruction/requestReset
- CPU: getCpuState → getCpuState()
- Memory: readMemory/writeMemory → readMemorySnapshot/writeMemory
- Breakpoints: set/clear/list → addBreakpoint/removeBreakpoint/getBreakpoints
- Trace: getExecutionTrace → instructionHistorySnapshot (filtered)
- IO/VRAM: ioHistorySnapshot / activitySnapshot
- Annotations: createFunction/renameFunction/setComment → symbolDatabase()

### Phase 3: Высокоуровневые операции
- **getFunctionContext(address)** — дизассемблирует функцию (reuse `disassembler.h`), собирает callers/callees из xrefs, memory/IO/VRAM access из activity snapshot + trace, stack balance из InstructionEvent SP
- **traceFunction(address)** — упрощённый: анализирует записанный trace для диапазона функции

### Phase 4: CMake
```cmake
option(ENABLE_AI_AGENT "Build AI Agent module" OFF)
if(ENABLE_AI_AGENT)
    add_library(debugger_agent STATIC ...)
    add_executable(test_agent_api ...)
    add_executable(test_agent_mock ...)
endif()
```

### Phase 5: Тесты
- 28+ тестов: primitive ops, annotations, high-level, error cases
- Mock Agent scenario: load ROM → run → pause → step → analyze → annotate
- Regression: при OFF все 170 существующих тестов проходят

### Ключевые решения
- Agent API работает через `IDebugBackend&` (не DebugBackend)
- Нет отдельного потока — threading через существующий command protocol
- Confidence хранится в AgentLog, не в SymbolDatabase
- Disassembler reuse из `debugger/src/disassembler.h`
- MCP adapter — следующий этап (не в 5.3)