# Stage 6.25 — Memory Operand Access Inspector (GUI)

**Каноническая копия:**
`/home/alexey/Projects/vector-debugger/debugger/docs/Pipeline/Stage_6.25_Memory_Operand_Access_Inspector.md`
**Репозиторий:** `/home/alexey/Projects/vector-debugger`

> **Нумерация.** Stage 6.24 уже занят в исходниках (`debugger/src/backend.cpp:756` —
> «instruction-start detection via pc param», замена src/-хука `Board::oninstrbegin`
> на эвристику в `DebugBackend::onMemoryRead`). Этот этап — следующий, 6.25.

---

## 1. Цель

Дать пользователю возможность видеть **конкретные обращения инструкций к выбранному
диапазону памяти** — не агрегированные по 256-байтным блокам (это делает Memory Map,
Stage 5.2 / 6.20), а поштучно:

```
PC      Instruction       Access   Address   Value
----------------------------------------------------
0552    MOV A,M           READ     8123      7F
0558    STA 9000h         WRITE    9000      3A
0561    LDAX B            READ     8120      42
```

Принципиально: **не создавать вторую систему мониторинга**. Расширить GUI-слой над
уже существующим `RuntimeAccessLog` (Stage 6.20 + 6.22 + 6.24), добавив к нему
новую window-панель `Memory Access`.

---

## 2. Архитектурные ограничения

- `git diff -- src/` — пуст. Никаких правок эмулятора, никаких новых hooks.
- Источник данных — существующие debugger-side колбэки
  `DebugBackend::onMemoryRead` / `onMemoryWrite`, которые пишут в `runtimeAccessLog_`
  (`RingBuffer<RuntimeAccessLogEntry>`, capacity 50000).
- Fetch уже отличается от Read: `fetchRemaining_` + `fetchBasePc_` выставляются
  эвристикой `!steppingInProgress_ && fetchRemaining_==0 && !stack && (virt+1)==pc`
  в `onMemoryRead` (Stage 6.24) и явно в `stepInstructionDetailed` (Stage 6.22).
  **Новый GUI только фильтрует** — ничего заново не считает.
- Дизассемблирование — только через существующий `IDebugBackend::disassemble(pc)`.
  В GUI не копировать opcode-decoder.
- MCP / Agent API на этом этапе **не расширяем** — `debug_get_memory_access_log`
  уже есть. Если понадобится отдельный Agent-доступ — это будет следующий этап.
- Тред-модель не меняется: emulation thread пишет `RuntimeAccessLogEntry` в ring
  buffer под `runtimeAccessMutex_`; GUI делает snapshot через
  `getRuntimeAccessLog(max)` (под тем же mutex).
- `git diff -- psp/` пуст.

---

## 3. Что уже есть — не дублировать

| Сущность | Файл | Статус |
|---|---|---|
| `RuntimeAccessLogEntry { address, type[Read/Write/Fetch], pc, value }` | `agent/agent_types.h:598` | ✅ (добавить `sequence`) |
| `RingBuffer<RuntimeAccessLogEntry> runtimeAccessLog_` (bounded, 50000) | `src/backend.h:414`, `src/backend.cpp:74` | ✅ |
| `onMemoryRead` / `onMemoryWrite` → push в лог + агрегация в `RuntimeAccessBlock` | `src/backend.cpp:818/867` | ✅ |
| `getRuntimeAccessLog(size_t maxEntries) const` — snapshot | `src/backend.cpp:2090` | ✅ |
| `clearRuntimeAccessMap()` — очищает и карту, и журнал | `src/backend.cpp:2068` | ⚠ см. §5.1 |
| ROM-load инвалидация | `DebugBackend::loadRom()` `src/backend.cpp:130` | ✅ |
| `MemoryMapWindow` + `onGoToDisassembly`, `onGoToMemoryInspector` | `gui/memory_map_window.h` | ✅ колбэки переиспользуем |
| MCP `debug_get_memory_access_log` / `_map` / `debug_clear_memory_access_map` | `mcp/mcp_adapter.cpp` | ✅ не трогаем |
| Instruction-start heuristics (`fetchBasePc_`, `fetchRemaining_`) | `src/backend.cpp:751-781` (Stage 6.24) | ✅ |

---

## 4. Новые типы (минимально)

### 4.1. Поле `sequence` в `RuntimeAccessLogEntry`

Индекс в snapshot плывёт при wrap ring-buffer'а, поэтому нужен монотонный
счётчик прямо в записи:

```cpp
struct RuntimeAccessLogEntry {
    uint16_t address = 0;
    enum Type { Read, Write, Fetch };
    Type type = Read;
    uint16_t pc = 0;
    uint8_t value = 0;
    uint64_t sequence = 0;   // NEW — monotonically increasing, set by DebugBackend
};
```

Поле — в конце, с default-инициализатором; существующий код (push, тесты,
MCP-сериализация) продолжает компилироваться.

### 4.2. Счётчик в `DebugBackend`

```cpp
uint64_t accessSequence_ = 0;   // под runtimeAccessMutex_
```

Присваивать **внутри существующего `runtimeAccessMutex_`-блока push**:
`logEntry.sequence = ++accessSequence_;` — в `onMemoryRead` (включая Fetch-ветку)
и в `onMemoryWrite`. Сбрасывать в `clearRuntimeAccessMap()` и в `loadRom()`.

Никаких новых hooks в `src/` — только `debugger/src/backend.{h,cpp}`.

### 4.3. GUI-side структура фильтра

Чистая, без ImGui — для unit-тестов:

```cpp
// gui/memory_access_window.h
struct MemoryAccessFilter {
    uint16_t from = 0x8000;
    uint16_t to   = 0x80FF;
    enum Mode { Read, Write, All } mode = All;   // Fetch — не показываем никогда
    bool includeStack = true;                    // PUSH/POP/CALL/RET остаются
};

// Free function — тестируется без ImGui и без backend:
std::vector<RuntimeAccessLogEntry> applyMemoryAccessFilter(
    const std::vector<RuntimeAccessLogEntry> &log,
    const MemoryAccessFilter &f,
    size_t maxRows);   // bounded, e.g. 5000
```

`Read`-режим оставляет `type == Read` (операндные чтения), **не** `Fetch`.
`All` = `Read ∪ Write`. Это соответствует §4 и §23 исходного ТЗ.

---

## 5. DebugBackend — точечные дополнения

### 5.1. Новое: `clearMemoryAccessLog()` — чистить только журнал

Требование §18 исходного ТЗ: `Clear` в окне `Memory Access` не должен трогать
Memory Map (агрегированные блоки). Сейчас `clearRuntimeAccessMap()` стирает и то
и другое.

Добавить в `IDebugBackend`:
```cpp
virtual void clearMemoryAccessLog() = 0;    // только ring buffer журнала
```

Реализация в `DebugBackend`:
```cpp
void DebugBackend::clearMemoryAccessLog() {
    std::lock_guard<std::mutex> lock(runtimeAccessMutex_);
    runtimeAccessLog_->clear();
    // accessSequence_ НЕ сбрасываем — иначе старые записи в UI «перенумеруются»
    // и потеряется связь с уже отрендеренными строками.
}
```

`clearRuntimeAccessMap()` **не меняет** семантику (по-прежнему чистит и карту, и
лог, и сбрасывает `accessSequence_`) — это API Agent/MCP, оно стабильно.

### 5.2. Mock

`debugger/agent/tests/mock_backend_for_agent.h` — заглушка `clearMemoryAccessLog()`
(чистит только `runtimeLog_`).

### 5.3. Ограничение displayed rows

В `MemoryAccessWindow` использовать `getRuntimeAccessLog(maxRows)` (по умолчанию
`maxRows = 5000`). UI показывает `Showing last N of M accesses` — требование §16.

---

## 6. GUI: `MemoryAccessWindow`

### 6.1. Файлы

| Файл | Что |
|---|---|
| `debugger/gui/memory_access_window.h` | Класс, `MemoryAccessFilter`, `applyMemoryAccessFilter()` |
| `debugger/gui/memory_access_window.cpp` | `render(IDebugBackend&)`, ImGui-таблица, виджеты; реализация фильтра |
| `debugger/CMakeLists.txt` | добавить `.cpp` в target debugger, новый тестовый target |
| `debugger/gui/gui.h` | поле `MemoryAccessWindow memoryAccess_;` |
| `debugger/gui/gui.cpp` | menu item **«View → Memory Access»**, wire visibility, navigation callbacks (§6.4) |

### 6.2. Layout окна

```
Range:  From [8000]  To [80FF]     [Use selected Memory Map range]
Access: ( ) Read   ( ) Write   (•) All
☑ Live                    [Refresh]  [Clear]
-----------------------------------------------------------
Seq    PC     Instruction     Access   Address   Value
-----------------------------------------------------------
1024   0552   MOV A,M         READ     8023      7F
1025   0558   STA 8020h       WRITE    8020      3A
1026   0561   LDAX B          READ     801F      42
-----------------------------------------------------------
Showing last 3 of 5000 accesses
```

Компоновка — в стиле существующих окон (`MemoryMapWindow`, `DisassemblyWindow`).
Без скроллбаров вне таблицы (правило «No Scroll Bars in Application Windows»).
Валидация: `from <= to`, иначе кнопка Refresh заблокирована.

Hex-инпуты — 16-бит (`ImGuiInputTextFlags_CharsHexadecimal |
ImGuiInputTextFlags_CharsUppercase`), буферы `char[5]`, маска `0000`–`FFFF`.

### 6.3. Live vs Refresh

По умолчанию `Live = OFF`.

- `Refresh` — однократно: `auto snap = backend.getRuntimeAccessLog(maxRows)`
  → `applyMemoryAccessFilter(snap, filter_, maxRows)` → сохранить в `rows_`.
- `Live = ON` — тот же путь, но раз в ~100 мс (throttle по аналогии с
  `MemoryMapWindow::SCAN_INTERVAL`). Не перестраивать GUI на каждый entry
  (§15/§25 ТЗ).

Disassembly строится лениво: кэш `std::unordered_map<uint16_t, std::string>` в
окне, сбрасывается при `Clear`/`loadRom`. На каждую visible строку максимум один
вызов `backend.disassemble(pc)`.

### 6.4. Навигация

- **Double-click на PC** → `onGoToDisassembly(pc)` (уже существует в `gui.cpp`).
- **Double-click на Address** → `onGoToMemoryInspector(address)`.
- **Context menu** строки: `Go to instruction`, `Go to memory address`,
  `Copy PC`, `Copy address`, `Copy instruction` — через ImGui `OpenPopup` +
  `SetClipboardText`, те же callback'и.
- Кнопка **`[Use selected Memory Map range]`** — колбэк
  `std::function<bool(uint16_t &outAddress)>` из `MemoryMapWindow`, возвращает
  `hoverAddress_` или адрес выбранного блока. Если нет — кнопка disabled.

### 6.5. Видимость

Окно — обычное `ImGui::Begin` с чекбоксом в меню `View`, как остальные.
Не модальное. Persistent visibility — через существующий паттерн
`getVisibleRef()` (см. `MemoryMapWindow`).

---

## 7. Thread safety (сводка)

```
emulation thread                       GUI thread
      │                                   │
onMemoryRead / onMemoryWrite              │
      │ sequence = ++accessSequence_      │
      ▼ (под runtimeAccessMutex_)         │
runtimeAccessLog_->push(entry)            │
      │                                   │
      │                    getRuntimeAccessLog(N)  [snapshot]
      │                                   │
      │                    applyMemoryAccessFilter (чистая функция)
      │                                   │
      │                    backend.disassemble(pc)  [thread-safe API]
      │                                   │
      │                    ImGui::Render rows
```

GUI **не** держит указателей на внутреннее состояние backend'а.

---

## 8. Производительность

- Push в лог на каждый доступ — уже есть, O(1), ring buffer.
- Новое: одна инкрементация `uint64_t` под существующим замком.
- Snapshot GUI — раз в кадр (Live) или по Refresh, максимум `maxRows = 5000`.
- Дизассемблирование — ленивый кэш по PC; типичный цикл ROM даёт ~100 уникальных
  PC, остальные попадания — cache hit.
- **Запрещено в hot path:** any `std::string` allocation в `onMemoryRead/Write`,
  любые вызовы в GUI из emulation thread.

Мерки (сравнение до/после на `testroms/cpuspeed.rom`):
- `Live = OFF`: разница в instructions/sec ≤ 1 %.
- `Live = ON`: ≤ 5 % (в основном из-за 100 ms polling, не из-за журнала).

---

## 9. Runtime correctness — что проверить (§23/§24 исходного ТЗ)

| Instruction | Ожидание в окне |
|---|---|
| `MVI A,42` / `LXI H,1234` | **не появляется** (это `Fetch`, отфильтрован) |
| `MOV A,M` (HL=8123) | `READ 8123 val` |
| `STA 9000h` | `WRITE 9000 value_of_A`, в колонке Instruction — `STA 9000h` |
| `LDAX B` | `READ <DE/HL/…> value` |
| `PUSH B` | 2 записи `WRITE` (stack) |
| `POP B` | 2 записи `READ` (stack) |
| `CALL nn` | 2 `WRITE` (адрес возврата) |
| `RET` | 2 `READ` |
| Instruction fetch opcode-байта | не показывается |
| Operand fetch (`nn` в `MVI A,nn`) | не показывается |

Все эти свойства уже обеспечивает Stage 6.22 + 6.24. Новый код **только
фильтрует** (`Read` vs `Fetch`), ничего заново не считает.

---

## 10. Тестирование

### 10.1. Unit-тесты фильтра (без ImGui) — новый файл

`debugger/tests/test_memory_access_filter.cpp`:
- `empty_log_returns_empty`
- `range_inclusive_boundaries` — `8000..80FF` включает 8000 и 80FF, не включает
  7FFF и 8100 (§29 «RANGE»).
- `mode_read_excludes_write_and_fetch`
- `mode_write_excludes_read_and_fetch`
- `mode_all_includes_read_and_write_but_never_fetch`
- `preserves_order_by_sequence`
- `bounded_max_rows_keeps_most_recent` (drop oldest)
- `invalid_range_from_gt_to_returns_empty`

### 10.2. Unit-тесты backend — расширить

`debugger/tests/test_runtime_accounting.cpp` (существует):
- `sequence_monotonic_across_read_and_write`
- `clear_access_log_preserves_map` — §18: после `clearMemoryAccessLog()`
  `getRuntimeAccessMap()` всё ещё возвращает блоки.
- `load_rom_resets_sequence_and_log`
- `immediate_operand_bytes_not_present_as_read` — прогон `MVI A,42`, в
  отфильтрованном `Read`-журнале записей по адресу `PC+1` нет.
- `push_pop_stack_writes_and_reads_present`
- `call_ret_stack_writes_and_reads_present`

### 10.3. GUI smoke

`test_memory_access_window_smoke`:
- создание окна, `setVisible(true)`, один `render()` с mock-backend, без крэша;
- `applyMemoryAccessFilter` вызывается, строки отрисовываются.

Перед коммитом — ручной smoke: запуск `v06c-debugger`, меню View → Memory Access,
загрузить ROM из `testdata`, `Refresh`. Проверить, что список не пустой и
Fetch-строк нет.

---

## 11. Порядок реализации

1. **Backend (мелкие правки):**
   1.1. `agent_types.h`: поле `sequence` в `RuntimeAccessLogEntry`.
   1.2. `backend.h/.cpp`: `accessSequence_` + increment в onMemoryRead/onMemoryWrite;
        новый метод `clearMemoryAccessLog()`; сброс в `loadRom`.
   1.3. `idebug_backend.h`: pure virtual `clearMemoryAccessLog()`.
   1.4. `mock_backend_for_agent.h`: заглушка.
   1.5. `mcp/mcp_json.cpp`: `to_json` — добавить `sequence`.
2. **Чистые функции:**
   2.1. `gui/memory_access_window.h`: `MemoryAccessFilter`,
        `applyMemoryAccessFilter(...)` — declaration.
   2.2. `gui/memory_access_window.cpp`: реализация фильтра (без ImGui).
3. **Тесты фильтра + backend (§10.1, §10.2)** — должны проходить до GUI.
4. **GUI:**
   4.1. `MemoryAccessWindow::render` + layout (§6.2).
   4.2. Live/Refresh/Clear buttons + hex input валидация.
   4.3. Таблица + ленивый disassembly cache.
   4.4. Double-click / context menu через существующие callback'и.
   4.5. `[Use selected Memory Map range]` — getter из `MemoryMapWindow`.
   4.6. Menu «View → Memory Access», `visible_` флаг.
5. **Smoke-тесты GUI** (§10.3) + запуск `v06c-debugger` вручную.
6. **Производительность** (§8): промер на `testroms/*.rom` с включённым и
   выключенным Live.
7. **Критерий приёмки** (§12) + `git diff -- src/` пуст.

---

## 12. Критерий приёмки

Пользователь может:

1. открыть **View → Memory Access**;
2. задать `From/To` (или взять из Memory Map кнопкой);
3. выбрать `Read` / `Write` / `All`;
4. включить `Live` **или** нажать `Refresh`;
5. увидеть конкретные инструкции, обращающиеся к диапазону;
6. увидеть `PC` и **фактический адрес операнда** (runtime, а не статический);
7. увидеть `Value` (1 байт);
8. отличить `READ` от `WRITE`;
9. **не увидеть** `Fetch` (opcode bytes, immediate bytes) в списке;
10. по двойному клику на `PC` перейти в Disassembly, на `Address` — в Memory
    Inspector;
11. нажать `Clear` — очищается только детальный журнал, `Memory Map` и RDB
    остаются;
12. журнал bounded: нет утечки памяти при долгом `Live = ON`, статус показывает
    «Showing last N of M».

Дополнительно машинно проверяемое:

```bash
git diff -- src/            # пусто
git diff -- psp/            # пусто
ctest --test-dir debugger/build \
  -R '(memory_access_filter|runtime_accounting|memory_access_window)' \
  --output-on-failure
```

---

## 13. Вне рамок этого этапа

- MCP-инструменты / Agent API — см. §28 исходного ТЗ. Если понадобится
  детальный лог через Agent — отдельный Stage 6.26.
- Экспорт журнала в файл (CSV/JSON).
- Фильтр «только эта функция» / «только этот PC».
- Colorizing по типу доступа в Disassembly.
- Интеграция с Execution Trace (Stage 3.10).

---

## 14. Итоги (заполняется по мере выполнения)

—
