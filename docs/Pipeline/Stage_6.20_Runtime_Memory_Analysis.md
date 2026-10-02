# Stage 6.20 — Runtime Memory Analysis & ROM/Loader State

## Цель

Расширить инструменты AI-агента для анализа ROM-файлов с упакованными данными,
динамическими буферами и runtime-generated структурами.

## Архитектурные ограничения

- `git diff -- src/` — пуст (не менять эмулятор)
- Вся новая функциональность — в `debugger/`
- MCP → AgentApi → DebugBackend → IDebugTarget → DebugAdapter
- Без новых hooks в `src/`

## Компоненты реализации

### 1. Runtime Memory Access Map (256 блоков × 256 байт)

**Структура** (`agent_types.h`):
```cpp
struct RuntimeAccessBlock {
    uint16_t address;     // start address (block-aligned)
    bool read, write, fetch;
    uint64_t read_count, write_count, fetch_count;
};
```

**Накопление** — в `DebugBackend::onMemoryRead/onMemoryWrite`:
- `fetchRemaining_ > 0` → fetch, иначе read (уже есть)
- Блок = `addr >> 8`
- Атомарные счётчики или mutex (onMemoryRead/Write вызываются из emulation thread)

**Ring buffer log** — `RuntimeAccessLogEntry`:
```cpp
struct RuntimeAccessLogEntry {
    uint16_t address;
    enum Type { Read, Write, Fetch } type;
    uint16_t pc;
    uint8_t value;
};
```

Ёмкость: `AgentLimits::MAX_MEMORY_ACCESS_LOG = 50000` (как memHistory).

### 2. Memory Snapshots

**Тип** (`agent_types.h`):
```cpp
struct MemorySnapshotData {
    uint32_t snapshot_id;
    uint16_t start_address;
    std::vector<uint8_t> data;
};

struct MemorySnapshotDiff {
    struct ChangedRange { uint16_t address; size_t size; };
    std::vector<ChangedRange> changed_ranges;
};
```

**Хранилище** — `std::map<uint32_t, MemorySnapshotData>` в DebugBackend.
Снимки независимы от RDB. При `loadRom` — все снимки инвалидируются.

### 3. IDebugBackend — новые методы

```cpp
// Runtime Access Map
virtual void clearRuntimeAccessMap() = 0;
virtual std::vector<RuntimeAccessBlock> getRuntimeAccessMap() const = 0;
virtual std::vector<RuntimeAccessLogEntry> getRuntimeAccessLog(size_t maxEntries) const = 0;

// Memory Snapshots
virtual uint32_t createMemorySnapshot(uint16_t start, size_t size) = 0;
virtual MemorySnapshotData getMemorySnapshot(uint32_t id) const = 0;
virtual MemorySnapshotDiff compareMemorySnapshots(uint32_t idA, uint32_t idB) const = 0;
virtual bool deleteMemorySnapshot(uint32_t id) = 0;
virtual void invalidateAllSnapshots() = 0;
```

### 4. Agent API — новые методы

```cpp
AgentApiResult<void> clearMemoryAccessMap();
AgentApiResult<std::vector<RuntimeAccessBlock>> getMemoryAccessMap();
AgentApiResult<std::vector<RuntimeAccessLogEntry>> getMemoryAccessLog(size_t maxEntries);
AgentApiResult<uint32_t> createMemorySnapshot(uint16_t start = 0, size_t size = 65536);
AgentApiResult<MemorySnapshotData> getMemorySnapshot(uint32_t snapshotId);
AgentApiResult<MemorySnapshotDiff> compareMemorySnapshots(uint32_t idA, uint32_t idB);
```

### 5. MCP tools (5 новых)

| Tool | AgentApi method |
|------|----------------|
| `debug_clear_memory_access_map` | `clearMemoryAccessMap()` |
| `debug_get_memory_access_map` | `getMemoryAccessMap()` |
| `debug_get_memory_access_log` | `getMemoryAccessLog(max)` |
| `debug_create_memory_snapshot` | `createMemorySnapshot(addr, size)` |
| `debug_compare_memory_snapshots` | `compareMemorySnapshots(idA, idB)` |

### 6. ROM loading — инвалидация

В `DebugBackend::loadRom()`:
- `clearRuntimeAccessMap()` — очистить карту и лог
- `invalidateAllSnapshots()` — удалить все снимки

### 7. readMemory — поддержка 64K

Увеличить `AgentLimits::MAX_MEMORY_READ_RANGE` с 16384 до 65536.
MCP `debug_read_memory_range` — обновить ограничение.

### 8. Thread Safety

- Runtime map накопление — в emulation thread (onMemoryRead/Write), чтение — snapshot
- Ring buffer log — `RingBuffer<RuntimeAccessLogEntry>` (уже thread-safe)
- Snapshots — создаются/читаются из paused state или через readMemorySnapshot

## Файлы для изменения

| Файл | Изменение |
|------|-----------|
| `debugger/agent/agent_types.h` | Новые типы + AgentLimits |
| `debugger/src/idebug_backend.h` | Новые virtual methods |
| `debugger/src/backend.h` | Новые поля + методы |
| `debugger/src/backend.cpp` | Реализация map accumulation, snapshots, ROM invalidation |
| `debugger/agent/agent_api.h` | Новые методы AgentApi |
| `debugger/agent/agent_api.cpp` | Реализация AgentApi methods |
| `debugger/agent/tests/mock_backend_for_agent.h` | Новые методы mock |
| `debugger/mcp/mcp_adapter.h` | `registerRuntimeAnalysisTools()` |
| `debugger/mcp/mcp_adapter.cpp` | 5 новых MCP tools |
| `debugger/mcp/mcp_json.h` | Сериализация новых типов |
| `debugger/mcp/mcp_json.cpp` | Реализация сериализации |
| `debugger/CMakeLists.txt` | Новый тестовый target |

## Тесты

- `test_agent_runtime_memory.cpp` — Agent API unit tests
  - Access map: initial empty, read/write/fetch tracked, clear, counters
  - Access log: entries created, PC recorded, bounded limit, drop oldest
  - Snapshots: create, compare, identical, changed ranges merge
  - ROM isolation: load invalidates snapshots/map/log
  - Memory range: 1 byte, 256 bytes, 64K, invalid range, wrap-around

## Порядок реализации

1. Типы (`agent_types.h`)
2. IDebugBackend interface
3. DebugBackend implementation (map, log, snapshots)
4. AgentApi methods
5. MockAgentBackend update
6. MCP tools + JSON serialization
7. Tests
8. Documentation
