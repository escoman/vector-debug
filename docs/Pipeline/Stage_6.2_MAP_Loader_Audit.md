# Stage 6.2 MAP Loader — Code Audit Report

**Дата:** 2026-09-06  
**Область:** `f16d0d4..42ad1d3` (MAP Loader + Functions Breakpoint)  
**Аудитор:** автоматический технический аудит

---

## Итоговый вердикт

```
PASS WITH RECOMMENDATIONS
```

Существенных ошибок, ломающих debugger, не обнаружено. Реализация корректна и соответствует архитектуре проекта. Ниже описаны замечания по приоритетам.

---

## CRITICAL

**Не обнаружено.**

---

## HIGH

### H1. `const` записи Z88DK MAP не фильтруются

```
Приоритет: HIGH
Файл:    debugger/src/map_loader.cpp
Функция: parseMapLine()
```

**Проблема:**  
Согласно документации JNEXT (сторонний потребитель Z88DK MAP), записи с `; const` являются compile-time константами и не должны загружаться как символы. Текущий parser не проверяет тип записи — `const` символы проходят через parser с `isAddress = false` и классифицируются как `Label`.

Реальный формат Z88DK:
```
_main     = $9360 ; addr, public, , terms_c, code_compiler, terms.c:1526
_myconst  = $0042 ; const, public
```

**Почему это проблема:**  
Compile-time константа (например, `$0042`) — это не адрес в памяти, а числовое значение. Если такой символ загружается как `Label` по адресу `0x0042`, он:
- Появляется в Functions/Disassembly как метка по неверному адресу
- Может конфликтовать с реальными символами по этому адресу
- Искажает Call Graph и Xrefs

**Рекомендуемое исправление:**  
В `parseMapLine()` добавить проверку на `const` в comment fields. Если найден токен `const`, возвращать `false` (пропустить строку):
```cpp
} else if (f == "const") {
    return false;  // compile-time constant, skip
}
```
Минимальное изменение — ~2 строки в `parseMapLine()`.

**Блокирует Stage 6.2:** Нет (в большинстве MAP файлов `const` записи редки, и они не ломают debugger, а лишь добавляют мусорные символы)

---

## MEDIUM

### M1. `const_cast` при установке `fromMap` / `sourceFile` / `sourceLine`

```
Приоритет: MEDIUM
Файл:    debugger/src/backend.cpp
Функция: loadRom() (строки 137-144)
```

**Проблема:**  
`SymbolDatabase::addSymbol()` создаёт `DebugSymbol` без полей `fromMap`, `sourceFile`, `sourceLine`. После добавления код использует `const_cast` для модификации:
```cpp
if (symbols_.addSymbol(sym.address, sym.name, sym.type)) {
    auto *p = const_cast<DebugSymbol*>(symbols_.findSymbol(sym.address));
    if (p) {
        p->fromMap   = true;
        p->sourceFile = ms.sourceFile;
        p->sourceLine = ms.sourceLine;
    }
}
```

**Почему это проблема:**  
`const_cast` — сигнал о несоответствии API. `findSymbol()` возвращает `const DebugSymbol*`, что нарушается через `const_cast`. Это работает, т.к. `std::map` хранит объекты по значению и не инвалидирует указатели при вставке (но не при удалении!).

**Рекомендуемое исправление:**  
Добавить в `SymbolDatabase` перегрузку:
```cpp
bool addSymbol(uint16_t addr, const std::string &name, SymbolType type,
               bool fromMap = false, const std::string &sourceFile = "",
               int sourceLine = 0);
```
Или возвращать `DebugSymbol*` из `addSymbol()` вместо `bool`.

**Блокирует Stage 6.2:** Нет

---

### M2. Двойной проход по содержимому MAP файла

```
Приоритет: MEDIUM
Файл:    debugger/src/map_loader.cpp
Функции: validateZ88dkFormat() + parseMapContent()
```

**Проблема:**  
`parseMapContent()` вызывает `validateZ88dkFormat()`, который сканирует весь контент построчно. Затем `parseMapContent()` снова сканирует весь контент построчно. Итого — 2 прохода.

**Почему это проблема:**  
Для типичных MAP файлов (сотни строк) это несущественно. Но для больших проектов (тысячи символов) можно обойтись одним проходом.

**Рекомендуемое исправление:**  
Объединить валидацию и парсинг: валидация проходит «лениво» — если после N непустых строк не найден ни один `= $`, отменить парсинг. Или: валидировать первые N строк, затем парсить.

**Блокирует Stage 6.2:** Нет

---

### M3. Отсутствие теста для `const` записей

```
Приоритет: MEDIUM
Файл:    debugger/tests/test_map_loader.cpp
```

**Проблема:**  
Нет теста, проверяющего, что записи `; const, public` корректно обрабатываются (пропускаются или загружаются как Label). Это прямо связано с H1.

**Рекомендуемое исправление:**  
Добавить тест:
```cpp
"_myconst = $0042 ; const, public\n"
"_real    = $0100 ; addr, public\n"
```
Проверить, что `_myconst` не загружается (или загружается как Label, в зависимости от решения по H1).

**Блокирует Stage 6.2:** Нет

---

### M4. `containsCI()` аллоцирует копии всего контента

```
Приоритет: MEDIUM
Файл:    debugger/src/map_loader.cpp
Функция: containsCI()
```

**Проблема:**  
```cpp
static bool containsCI(const std::string &haystack, const std::string &needle)
{
    std::string h = haystack, n = needle;  // копирует весь контент
    ...
}
```
Вызывается 3 раза для `[keyboard]`, `[mapping]`, `[keys]` — итого 3 копии всего контента.

**Почему это проблема:**  
Для типичных MAP файлов (несколько KB) несущественно. Но это неоправданные аллокации.

**Рекомендуемое исправление:**  
Использовать `std::search` с case-insensitive comparator, или искать подстроку без копирования:
```cpp
static bool containsCI(const std::string &h, const std::string &n) {
    auto it = std::search(h.begin(), h.end(), n.begin(), n.end(),
        [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); });
    return it != h.end();
}
```

**Блокирует Stage 6.2:** Нет

---

## LOW

### L1. `MapSymbol` vs `DebugSymbol` — частичное дублирование

```
Приоритет: LOW
Файлы:   map_loader.h, symbol_database.h
```

**Проблема:**  
`MapSymbol` и `DebugSymbol` имеют пересекающиеся поля: `name`, `address`, `sourceFile`, `sourceLine`. Преобразование происходит в `backend.cpp`.

**Анализ:**  
Разделение оправдано:
- `MapLoader` не зависит от `SymbolDatabase` — чистый parser
- `MapSymbol` хранит MAP-specific поля (`visibility`, `isAddress`), не нужные `DebugSymbol`
- `DebugSymbol` хранит debugger-specific поля (`comment`, `type`, `fromMap`), не нужные parser

**Вывод:** Разделение корректно. Дублирование минимально (4 поля из 7). Рефакторинг не требуется.

---

### L2. `public → Function` — эвристика, не гарантия

```
Приоритет: LOW
Файл:    debugger/src/backend.cpp (строка 125)
```

**Проблема:**  
Классификация `addr + public → Function`, иначе `Label` — это эвристика. В Z88DK MAP `public` означает «экспортируемый символ», а не обязательно «функция». Глобальная переменная тоже может быть `addr, public`.

**Последствия:**
- Functions window покажет глобальную переменную как «func»
- Size column вычислит расстояние до следующей «функции» (может быть некорректно)
- Call Graph не пострадает — xrefs строятся по opcode, не по типу символа

**Вывод:** Для подавляющего большинства Z88DK MAP `addr + public` — это действительно функции. Эвристика приемлема. В будущем можно добавить ручную реклассификацию из GUI (уже есть Rename/Delete).

---

### L3. Нет реальных MAP-файлов в проекте

```
Приоритет: LOW
```

**Проблема:**  
В проекте нет ни одного `.map` файла для верификации parser на реальных данных. Тесты используют синтетические строки.

**Рекомендация:**  
Добавить в `debugger/tests/testdata/` хотя бы один реальный MAP-файл (или его фрагмент) для regression-тестирования.

---

### L4. Формат валидации: `"=$"` vs `"= $"`

```
Приоритет: LOW
Файл:    debugger/src/map_loader.cpp
Функция: validateZ88dkFormat()
```

**Проблема:**  
Валидация проверяет оба варианта: `"= $"` и `"=$"`. Но `parseMapLine()` ищет `"= $"` первым, затем `"=$"`. Это консистентно, но `"=$"` без пробела — крайне маловероятный формат для Z88DK.

**Вывод:** Не проблема. Дополнительная проверка `"=$"` — оборонительное программирование, не несёт негативных последствий.

---

## OK — Что проверено и полностью соответствует

| # | Проверка | Результат |
|---|----------|-----------|
| 1 | Парсер Z88DK MAP формата | ✅ Корректно парсит `name = $ADDR ; addr, public/local, source` |
| 2 | Address handling ($0000, $FFFF, >$FFFF, invalid hex) | ✅ Границы 16-бит, reject >$FFFF, reject non-hex |
| 3 | Source location extraction | ✅ `main.c::main::0::1:109` → file=main.c, line=109; `clr.asm:161` → file=clr.asm, line=161 |
| 4 | Format validation (keyboard .map rejection) | ✅ Rejects `[keyboard]`/`[mapping]`/`[keys]`; requires `= $` |
| 5 | Duplicate handling (first wins) | ✅ Parser returns all, SymbolDatabase rejects duplicate addresses |
| 6 | SymbolDatabase integration | ✅ Единое хранилище, нет второго списка MAP symbols |
| 7 | clearMapSymbols() | ✅ Удаляет только fromMap=true, сохраняет user symbols |
| 8 | ROM reload: MAP A → MAP B | ✅ Old MAP cleared, new MAP loaded |
| 9 | ROM reload: MAP → no MAP | ✅ Old MAP cleared, 0 symbols |
| 10 | User symbols preserved across ROM reload | ✅ Verified in test_user_symbols_preserved |
| 11 | Functions → SymbolDatabase (no MAP dependency) | ✅ Functions uses only SymbolDatabase, no MapLoader references |
| 12 | Disassembly → SymbolDatabase (no MAP dependency) | ✅ Uses displayName() from SymbolDatabase |
| 13 | Agent API → SymbolDatabase | ✅ getSymbols(), getFunction(), getXrefs(), getCallGraph() — все через backend_.symbolDatabase() |
| 14 | Xrefs / Call Graph | ✅ Не зависят от типа символа (Function/Label), строятся по opcode |
| 15 | Breakpoint integration | ✅ Через IDebugBackend API, единый breakpoint state |
| 16 | Functions breakpoint indicator | ✅ Красный ● в Address column, динамическое меню Set/Remove |
| 17 | MapLoader independence | ✅ Не зависит от ImGui, SDL, Board, CPU |
| 18 | Thread safety | ✅ MAP loading inside loadRom(), emulation paused (running_ = false) |
| 19 | No `src/` changes | ✅ `git diff f16d0d4^..42ad1d3 -- src/` — пусто |
| 20 | Test coverage | ✅ 28 новых тестов (17 unit + 6 integration + 5 breakpoint) |
| 21 | Performance | ✅ O(n) parser, O(n) validation. MAP files typically <100KB |
| 22 | Code volume | ✅ map_loader.cpp 356 строк — оправдано (parser + validation + source extraction + file I/O) |
| 23 | Architecture | ✅ GUI → IDebugBackend → DebugBackend → SymbolDatabase — не нарушена |

---

## Ответы на главные вопросы аудита

### 1. Есть ли реальные ошибки в MAP parser?

**Нет критических ошибок.** Parser корректно обрабатывает основной формат Z88DK MAP. Единственное замечание — отсутствие фильтрации `; const` записей (H1), что добавляет мусорные символы, но не ломает debugger.

### 2. Корректна ли классификация `public → Function / local → Label`?

**Это оправданная эвристика.** В Z88DK MAP подавляющее большинство `addr + public` — это функции. Глобальные переменные с `addr + public` — редкий случай. Классификация не влияет на Xrefs/Call Graph (они работают по opcode).

### 3. Оправдан ли объём `map_loader.cpp` (~356 строк)?

**Да.** Включает: parser, format validation, source location extraction, hex parsing, file I/O, path derivation. Без раздувания.

### 4. Есть ли дублирование логики?

**Нет.** `MapLoader` и `SymbolDatabase` — отдельные слои с чёткими границами. `MapSymbol` ↔ `DebugSymbol` конвертация минимальна и оправдана.

### 5. Есть ли проблемы с SymbolDatabase?

**Нет.** Добавлены минимальные изменения: `fromMap`, `sourceFile`, `sourceLine` поля + `clearMapSymbols()`. API не сломан. Единственное замечание — `const_cast` (M1), но это cosmetic issue.

### 6. Есть ли проблемы с Functions/Disassembly/Xrefs/Call Graph?

**Нет.** Все компоненты используют `SymbolDatabase` как единый источник истины. MAP-specific логика не проникла в GUI-окна.

### 7. Есть ли проблемы с breakpoint integration?

**Нет.** Breakpoint устанавливается через существующий `IDebugBackend` API. Визуальный индикатор читает `hasBreakpoint()`. Functions не имеет собственного breakpoint state.

### 8. Есть ли проблемы с тестами?

**Незначительный пробел:** отсутствие теста для `; const` записей (M3). В остальном покрытие хорошее: 28 тестов, включая edge cases, reload scenarios, и breakpoint sync.

### 9. Есть ли изменения в `src/`?

**Нет.** `git diff f16d0d4^..42ad1d3 -- src/` — пусто. Основной эмулятор не затронут.

---

## Сводка замечаний

| # | Приоритет | Описание | Блокирует? |
|---|-----------|----------|------------|
| H1 | HIGH | `const` записи не фильтруются | Нет |
| M1 | MEDIUM | `const_cast` для установки MAP-полей | Нет |
| M2 | MEDIUM | Двойной проход по файлу | Нет |
| M3 | MEDIUM | Нет теста для `const` записей | Нет |
| M4 | MEDIUM | `containsCI()` аллоцирует копии | Нет |
| L1 | LOW | MapSymbol/DebugSymbol дублирование (оправдано) | Нет |
| L2 | LOW | `public → Function` эвристика (приемлема) | Нет |
| L3 | LOW | Нет реальных MAP-файлов для regression | Нет |
| L4 | LOW | `"=$"` validation (оборонительное) | Нет |

**Рекомендация:** Исправить H1 (фильтрация `const`) — это 2 строки кода и закрывает единственный существенный пробел в parser. Остальные замечания — технический долг, не требующий немедленного исправления.
