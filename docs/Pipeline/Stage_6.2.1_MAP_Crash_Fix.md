# Stage 6.2.1 — MAP Crash Fix + const Symbols

## Причина crash

```
Файл:   debugger/src/map_loader.cpp
Функция: parseMapLine()
Строка:  ~248 (до исправления)
Причина: const-записи Z88DK MAP не фильтровались
```

### Объяснение

Почему:
```
ROM без MAP → работает (нет MAP файла → нет const символов)
ROM с MAP   → crash (22 const-записи загружаются как Label-символы)
```

**Корень проблемы:**

Z88DK MAP файл содержит два типа записей:
- `addr` — реальные адреса памяти (функции, метки)
- `const` — compile-time константы (размеры секций, head/tail указатели)

Парсер не распознавал поле `const` в комментарии и загружал **все** записи как символы.

**Каскад последствий:**

1. `STACK_TOP = $0100 ; const, local` загружался как Label
2. Занимал адрес `0x0100` в SymbolDatabase (first-wins политика)
3. Блокировал реальный символ `start = $0100 ; addr, local`
4. Аналогично для 21 другой const-записи:
   - `__bss_compiler_head = $0511` (const) блокировал `l_div_u = $0511` (addr)
   - `__code_clib_head = $013D` (const) блокировал `_v06_out = $013D` (addr)
   - и т.д.
5. В результате:
   - 22 адреса заняты ложными символами (не являющимися адресами памяти)
   - Реальные функции/метки на этих адресах потеряны
   - Functions window показывает `STACK_TOP` вместо `start`
   - Disassembly window не может резолвить символы по реальным адресам

## Исправление

**Файл:** `debugger/src/map_loader.cpp`

Добавлено распознавание поля `const` в комментарии MAP-записи:

```cpp
bool isConst = false;
// ...
if (f == "const") {
    isConst = true;
}
// ...
if (isConst) return false;
```

Минимальное изменение: +9 строк, -1 строка.

## const верификация

```
const → skipped  ✓  (22 записи отфильтрованы)
addr  → loaded   ✓  (86 addr-символов загружены)
```

### До исправления
- 108 символов (22 const + 86 addr)
- `STACK_TOP` (const) занимает `$0100`, блокируя `start` (addr)

### После исправления
- 86 символов (только addr)
- `start` (addr) корректно загружается по адресу `$0100`
- 80 уникальных адресов (6 дубликатов отклонены first-wins политикой)

## Tests

### Новые тесты (3)

| # | Имя | Что проверяет |
|---|-----|---------------|
| 18 | `test_const_entries_skipped` | `_myconst` и `_value` (const) не в output; `_main` и `_label` (addr) присутствуют |
| 19 | `test_const_does_not_block_addr` | `STACK_TOP` (const) не блокирует `start` (addr) по адресу `$0100` |
| 20 | `test_real_map_check_bugs` | Реальный `check_bugs.map`: 86 parsed, 22 skipped, 0 const, ключевые адреса корректны |

### Результаты всех тестов

| Test Suite | Passed | Failed |
|------------|--------|--------|
| test_map_loader | 20 | 0 |
| test_map_integration | 6 | 0 |
| test_backend | 120 | 0 |
| test_symbol_database | 22 | 0 |
| test_agent_api | 77 | 0 |
| test_agent_commands | 15 | 0 |
| test_agent_integration | 46 | 0 |
| test_gui_smoke | 3 | 0 |
| test_board_smoke | 2 | 0 |
| test_config_manager | 8 | 0 |
| test_live_map | 21 | 0 |
| test_reset_cpu_state | 14 | 0 |
| test_rom_load_address | 28 | 0 |
| test_rom_loading | 9 | 0 |
| test_vram_mapping | 17 | 0 |
| test_workspace | 15 | 0 |
| **TOTAL** | **423** | **0** |

```
ALL TESTS PASS
```

## src/

```
git diff --stat -- src/
```
Результат: **пустой** (нет изменений).

## Изменённые файлы

| Файл | Изменение |
|------|-----------|
| `debugger/src/map_loader.cpp` | +9/-1: const filtering в `parseMapLine()` |
| `debugger/tests/test_map_loader.cpp` | +131: 3 regression теста |
| `debugger/gui/functions_window.cpp` | +1/-1: `BeginPopupContextItem("funcctx")` — fix ImGui assertion |

## Критерии готовности

1. ✅ Причина crash точно установлена (const entries не фильтровались)
2. ✅ ROM + MAP больше не вызывает crash
3. ✅ ROM без MAP продолжает работать
4. ✅ Повреждённый MAP не вызывает crash (existing tests)
5. ✅ const symbols не попадают в SymbolDatabase
6. ✅ Обычные addr symbols продолжают загружаться
7. ✅ Functions работает с MAP (verified via test_real_map_check_bugs)
8. ✅ Disassembly работает с MAP (SymbolDatabase корректен)
9. ✅ Breakpoint на функции работает (existing test_backend tests)
10. ✅ SymbolDatabase reload semantics не нарушены (test_map_integration)
11. ✅ Agent API продолжает работать (test_agent_api: 77/77)
12. ✅ Все тесты проходят (423/423)
13. ✅ src/ не изменён
14. ✅ Не выполнены посторонние рефакторинги

---

## Дополнительный crash: Functions window

### Причина

```
Файл:   debugger/gui/functions_window.cpp
Функция: FunctionsWindow::render()
Строка:  167
Причина: ImGui::BeginPopupContextItem() без строкового ID
```

`ImGui::Text()` не создаёт ID элемента. `BeginPopupContextItem()` без аргумента
берёт `LastItemData.ID`, который равен 0 → assertion failure:

```
imgui.cpp:13536: BeginPopupContextItem: Assertion `id != 0' failed.
```

В Disassembly window используется `BeginPopupContextItem("dasmctx")` — с явным ID.
В Functions window было `BeginPopupContextItem()` — без ID.

### Исправление

```cpp
// Было:
if (ImGui::BeginPopupContextItem()) {

// Стало:
if (ImGui::BeginPopupContextItem("funcctx")) {
```

### Верификация

```
timeout 5 ./v06c-debugger check_bugs.rom
→ EXIT: 143 (SIGTERM от timeout, без assertion failure)
```
