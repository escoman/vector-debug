# Stage 6.2 — Z88DK MAP Loader & Symbol Visualization: Отчёт

## 1. Изменённые файлы

### Новые файлы
| Файл | Описание | Строк |
|------|----------|-------|
| `debugger/src/map_loader.h` | Header: MAP parser, MapSymbol, MapLoadResult | +74 |
| `debugger/src/map_loader.cpp` | Реализация MAP parser + format validation | +355 |
| `debugger/tests/test_map_loader.cpp` | 17 unit-тестов parser | +559 |
| `debugger/tests/test_map_integration.cpp` | 6 integration-тестов через DebugBackend | +410 |

### Изменённые файлы
| Файл | Изменение | Строк |
|------|-----------|-------|
| `debugger/src/symbol_database.h` | +sourceFile, +sourceLine, +fromMap в DebugSymbol; +clearMapSymbols() | +11 |
| `debugger/src/symbol_database.cpp` | Реализация clearMapSymbols() | +15 |
| `debugger/src/backend.cpp` | Автозагрузка MAP в loadRom() + include map_loader.h | +40 |
| `debugger/gui/functions_window.cpp` | Колонка "Source" в таблице Functions | +15 |
| `debugger/CMakeLists.txt` | +map_loader.cpp в DBG_SOURCES; +test_map_loader, +test_map_integration | +33 |

**Итого:** ~114 строк изменений в существующих файлах, ~1400 строк нового кода.

---

## 2. Поддержанный формат Z88DK MAP

Parser поддерживает формат, генерируемый zcc/z88dk linker:

```
_symbol_name = $HHHH ; addr, public|local, [source_location]
```

### Распознаваемые конструкции
- **Адреса:** `$0000` — `$FFFF` (16-bit hex, `\$` prefix)
- **Visibility:** `public`, `local`
- **Тип:** `addr` → Function (если public), Label (если local)
- **Source location:** `main.c::main::0::1:109`, `clr.asm:161`
- **Игнорируемые строки:** пустые, комментарии (`;`), нераспознаваемые
- **Duplicate symbols:** first wins (детерминированно)

### Отклоняются parser'ом
- Адреса за пределами 16-bit (> `$FFFF`)
- Строки без паттерна `= $`

---

## 3. Проверка формата (Format Validation)

**Ключевая фича:** защита от старых .map файлов с настройками клавиш.

Алгоритм валидации:
1. Сканирование файла на наличие паттерна `= $` — характерного для Z88DK MAP
2. Если ни одной строки с `= $` не найдено → файл отклоняется
3. Дополнительная проверка: наличие секций `[keyboard]`, `[mapping]`, `[keys]` → отклонение

Результат: старый `.map` файл с настройками клавиш **не вызовет падение** — будет отклонён с сообщением `"not a Z88DK MAP file (format validation failed)"`.

---

## 4. Интеграция с Symbol Database

MAP-символы загружаются в **существующий SymbolDatabase** (не создаётся отдельное хранилище):

```
MAP file → MapLoader::loadMapFile() → MapLoadResult
    → DebugBackend::loadRom() → SymbolDatabase::addSymbol()
        → Functions window (автоматически)
        → Disassembly labels (автоматически)
        → Xrefs / Call Graph (автоматически)
        → Agent API getSymbols() (автоматически)
```

### Расширения модели DebugSymbol
- `sourceFile: string` — имя исходного файла (e.g. "main.c")
- `sourceLine: int` — номер строки
- `fromMap: bool` — флаг происхождения (для очистки при ROM reload)

### Очистка при ROM reload
- `SymbolDatabase::clearMapSymbols()` — удаляет только MAP-символы
- Пользовательские символы (fromMap == false) **сохраняются**

---

## 5. Автоматический поиск MAP

При загрузке `/path/check_bugs.rom` автоматически ищется `/path/check_bugs.map`.

Реализация: `MapLoader::mapPathFromRom()` — замена расширения на `.map`.

Работает для всех расширений ROM: `.rom`, `.r0m` — `.r9m`.

---

## 6. Отображение в Functions

Functions window автоматически показывает MAP-функции:

```
Address  Name              Size  Calls  Source          Comment
0100     _main [func]      N/A   0      main.c:109      -
0103     _loop [label]     N/A   0      -               -
```

Новая колонка **Source** (Stage 6.2) показывает `sourceFile:sourceLine`.

---

## 7. Отображение в Disassembly

Disassembly автоматически показывает label'ы из MAP:

```
_main:
→   0100  31 00 C1  LXI SP, C100
    0103  76      HLT
```

Реализация: через существующий механизм `symbols.findSymbol()` — никаких изменений в disassembly renderer не потребовалось.

---

## 8. Результаты Parser Tests (17 тестов)

| # | Тест | Результат |
|---|------|-----------|
| 1 | Public function | PASS |
| 2 | Local label | PASS |
| 3 | Source location extraction | PASS |
| 4 | Hex boundaries ($0000, $FFFF) | PASS |
| 5 | Invalid address (> $FFFF) rejected | PASS |
| 6 | Malformed lines — no crash | PASS |
| 7 | Duplicate symbols — first wins | PASS |
| 8 | Old keyboard .map rejected | PASS |
| 9 | Empty file | PASS |
| 10 | Comments-only file | PASS |
| 11 | mapPathFromRom | PASS |
| 12 | Nonexistent file | PASS |
| 13 | Load from disk | PASS |
| 14 | Old keyboard from disk rejected | PASS |
| 15 | SymbolDatabase integration | PASS |
| 16 | Multiple symbols | PASS |
| 17 | Symbol without comment | PASS |

---

## 9. Результаты Integration Tests (6 тестов)

| # | Тест | Результат |
|---|------|-----------|
| 1 | ROM + MAP auto-detection | PASS |
| 2 | ROM without MAP — no error | PASS |
| 3 | ROM reload clears old MAP | PASS |
| 4 | ROM+MAP → ROM without MAP | PASS |
| 5 | Old keyboard .map — no crash | PASS |
| 6 | User symbols preserved | PASS |

---

## 10. Полный регрессионный тест

| Test Suite | Результат |
|------------|-----------|
| test_backend | 115/115 |
| test_symbol_database | 22/22 |
| test_vram_mapping | 17/17 |
| test_rom_load_address | 28/28 |
| test_rom_loading | 9/9 |
| test_reset_cpu_state | 14/14 |
| **test_map_loader** | **17/17** (NEW) |
| **test_map_integration** | **6/6** (NEW) |
| test_agent_api | 77/77 |
| test_agent_commands | 15/15 |
| test_agent_integration | 46/46 |
| test_workspace | 15/15 |
| test_config_manager | 8/8 |
| test_live_map | 21/21 |
| **ИТОГО** | **430/430** |

---

## 11. Подтверждение отсутствия изменений в `src/`

```
$ git diff --stat src/
(пусто — изменений нет)
```

---

## 12. Критерий готовности

| Критерий | Статус |
|----------|--------|
| `check_bugs.rom` автоматически находит `check_bugs.map` | ✅ |
| MAP успешно разбирается | ✅ |
| Символы попадают в существующий SymbolDatabase | ✅ |
| Функции появляются в Functions | ✅ |
| Labels появляются в Disassembly | ✅ |
| Functions и Disassembly используют одни и те же entries | ✅ |
| При загрузке нового ROM старые MAP symbols удаляются | ✅ |
| ROM без MAP продолжает работать | ✅ |
| Parser устойчив к неизвестным/повреждённым строкам | ✅ |
| Старый .map файл с настройками клавиш не вызывает падение | ✅ |
| Unit + integration tests | ✅ (23 новых теста) |
| Весь существующий тестовый набор проходит | ✅ (430/430) |
| `src/` не изменён | ✅ |
