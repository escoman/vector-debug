# ТЗ — Stage 6.2: Z88DK MAP Loader & Symbol Visualization

## 1. Цель

Добавить в debugger поддержку загрузки MAP-файлов, генерируемых Z88DK/zcc, для автоматического сопоставления адресов ROM с именами символов, функциями и исходными позициями.

Основной сценарий:

```text
check_bugs.rom
check_bugs.map
```

При загрузке `check_bugs.rom` debugger автоматически ищет рядом:

```text
check_bugs.map
```

и, если файл существует, загружает его.

MAP-файл должен интегрироваться в **существующий Symbol Database**. Второй независимый механизм хранения символов создавать запрещено.

---

# 2. Ограничения

### Обязательно

* `src/` не изменять.
* Agent API не изменять, если для этой задачи это не требуется.
* Не создавать второй Symbol Database.
* Не дублировать существующий parser/logic Symbol Database без необходимости.
* GUI не должен напрямую работать с `Board`, `Memory`, CPU или Vector-specific классами.
* Сохранить работу debugger при отсутствии `.map`.

### Поведение без MAP

Загрузка ROM без MAP должна работать точно так же, как сейчас.

---

# 3. Автоматический поиск MAP

При загрузке:

```text
/path/check_bugs.rom
```

искать:

```text
/path/check_bugs.map
```

Имя строится заменой расширения `.rom` на `.map`.

По возможности сделать поиск нечувствительным к регистру расширения:

```text
check_bugs.rom
check_bugs.ROM
```

→ искать соответствующий `check_bugs.map`.

Не искать произвольные MAP-файлы в соседних каталогах.

### Важно при создании MAP-файлов

**MAP-файл ВСЕГДА должен располагаться в том же каталоге, что и соответствующий ROM-файл, и иметь то же базовое имя.**

Примеры корректного размещения:

```text
/path/to/check_bugs.rom
/path/to/check_bugs.map        ← правильно

/path/to/fire2.rom
/path/to/fire2.map             ← правильно
```

Примеры НЕкорректного размещения:

```text
/path/to/check_bugs.rom
/home/user/check_bugs.map      ← НЕПРАВИЛЬНО: другой каталог

/path/to/check_bugs.rom
/path/to/symbols.map           ← НЕПРАВИЛЬНО: другое имя

/path/to/check_bugs.rom
/path/to/check_bugs.MAP.txt    ← НЕПРАВИЛЬНО: другое расширение
```

При создании MAP-файла (вручную или через MCP/Agent) необходимо сразу сохранять его рядом с ROM-файлом. Debugger автоматически находит и загружает MAP только по пути `<rom_path_without_extension>.map`.

---

# 4. Загрузка MAP

После успешной загрузки ROM:

1. определить имя MAP;
2. проверить существование файла;
3. если MAP существует — прочитать его;
4. разобрать символы;
5. передать результаты в существующий Symbol Database;
6. обновить Functions / Disassembly / Xrefs / Call Graph, использующие Symbol Database.

Если MAP отсутствует:

```text
ROM loaded
MAP not found
```

Это не ошибка загрузки ROM.

Если MAP найден, но содержит отдельные неизвестные или неподдерживаемые строки, parser должен пропускать их, не прерывая загрузку остальных корректных символов.

---

# 5. Формат Z88DK MAP

Реализовать parser фактического формата MAP, используемого zcc/z88dk.

В частности поддержать строки вида:

```text
_main                    = $0252 ; addr, public, ...
_gfx_set_mode             = $02B3 ; addr, public, ...
_graph_fill_planes        = $018E ; addr, public, ...
_plane_fill               = $0228 ; addr, public, ...
fp_c000_inner             = $01D1 ; addr, local, ...
```

Не делать parser исключительно под `check_bugs.map`.

Он должен работать с аналогичными MAP-файлами zcc.

---

# 6. Symbol model

Использовать существующую модель Symbol Database.

Если текущая модель недостаточна для различения типов символов, расширить её в debugger-слое.

Минимально необходимо различать:

```text
Function
Label
Data / Variable
Constant
```

и visibility:

```text
Public
Local
```

Не создавать отдельную таблицу только для MAP.

---

# 7. Определение функций

MAP может содержать адресные символы, но не каждый адресный символ автоматически является функцией.

Использовать существующую классификацию Symbol Database, если она уже умеет определять функции.

При необходимости добавить в MAP importer правила классификации.

Для функций типа:

```text
_main
_gfx_set_mode
_graph_fill_planes
_plane_fill
```

создать соответствующие Function/Symbol entries.

Для локальных assembly labels:

```text
fp_c000_inner
```

создавать Label, если нет достаточных оснований считать его отдельной функцией.

Не превращать автоматически **каждый `addr` symbol** в Function.

---

# 8. Имена

Сохранять имя символа из MAP без потери информации.

Например:

```text
_main
```

должен быть доступен как:

```text
_main
```

Если существующий UI автоматически отображает пользовательские символы без ведущего `_`, это допустимо только на уровне presentation.

В Symbol Database желательно хранить исходное имя.

---

# 9. Source location

Из MAP необходимо извлечь source location, если она присутствует.

Например:

```text
main.c::main::0::1:109
```

или:

```text
../../lib/gfx/mode.c::gfx_set_mode::0::0:39
```

или:

```text
clr.asm:161
```

Сохранить как минимум:

```text
source file
line
```

Дополнительные поля z88dk можно сохранить, если текущая модель Symbol Database их поддерживает.

Не пытаться открывать исходные файлы в этой итерации.

---

# 10. Functions window

Существующее окно **Functions** должно использовать данные Symbol Database.

Для MAP-функций показывать минимум:

```text
Address    Name
0252       _main
02B3       _gfx_set_mode
...
```

Желательно также:

```text
Source
```

например:

```text
0252   _main              main.c:109
02B3   _gfx_set_mode      mode.c:39
```

Не создавать отдельный список функций специально для MAP.

Functions должен автоматически отражать объединённый результат:

```text
MAP symbols
+
existing debugger-discovered functions
```

При конфликте адресов/имён использовать существующие правила Symbol Database.

---

# 11. Disassembly

В окне Disassembly перед инструкцией, на которую указывает символ, показывать label.

Например:

```text
0252 <_main>:
0252  ...
0253  ...
0254  ...

02B3 <_gfx_set_mode>:
02B3  ...
```

Для локальных labels:

```text
01D1 <fp_c000_inner>:
01D1  ...
```

Если по одному адресу существует несколько символов, не дублировать строку без необходимости.

Использовать существующий механизм отображения символов в Disassembly.

Не писать отдельный MAP-specific disassembly renderer.

---

# 12. Навигация

Клик по функции в Functions должен переводить Disassembly на соответствующий адрес.

Если такая навигация уже существует для Symbol Database — использовать её без создания нового механизма.

Аналогично:

```text
Functions → Disassembly
Disassembly symbol → Functions
```

должны использовать один и тот же адрес/символ.

---

# 13. Xrefs / Call Graph

После загрузки MAP существующая аналитика должна использовать полученные символы.

Например, если Xref или Call Graph знает адрес:

```text
0252
```

UI должен иметь возможность показать:

```text
_main
```

вместо одного только:

```text
0252
```

Не требуется создавать новый алгоритм Xrefs/Call Graph.

Использовать существующий Symbol Database.

---

# 14. ROM reload

При загрузке другого ROM необходимо корректно заменить MAP symbols предыдущего ROM.

Пример:

```text
A.rom + A.map
↓
B.rom + B.map
```

После загрузки B:

* символы A не должны оставаться;
* функции A не должны отображаться;
* labels A не должны отображаться;
* source locations A не должны отображаться.

Если существующий Symbol Database уже имеет операцию очистки/замены проекта — использовать её.

---

# 15. ROM без MAP после ROM с MAP

Проверить отдельно:

```text
A.rom + A.map
↓
B.rom без B.map
```

После загрузки B:

* символы A должны быть удалены;
* Functions должен быть пуст/содержать только автоматически выявленные символы B;
* Disassembly не должен показывать A labels.

---

# 16. MAP с некорректными строками

Parser не должен падать на:

* пустых строках;
* комментариях;
* неизвестных строках;
* строках без адреса;
* неподдерживаемых типах символов.

Некорректная строка не должна мешать загрузке остальных символов.

Желательно считать:

```text
parsed symbols
skipped lines
```

для диагностического сообщения/лога.

---

# 17. Адреса

Поддержать шестнадцатеричные адреса Z88DK:

```text
$0252
$C000
$FFFF
```

Результат хранить в существующем адресном типе debugger.

Проверить границу:

```text
$0000
$FFFF
```

Адрес за пределами 16-битного пространства должен быть отвергнут parser'ом.

---

# 18. Duplicate symbols

Корректно обработать:

```text
same name + same address
```

и:

```text
different names + same address
```

Не создавать бесконечные/дублирующиеся записи.

Использовать существующие правила Symbol Database.

Если текущий Symbol Database не определяет политику конфликтов, добавить минимально необходимую детерминированную политику и покрыть её тестом.

---

# 19. API

Agent API менять **только если существующий Symbol Database API не позволяет получить загруженные MAP symbols**.

Предпочтительно:

```text
MAP Loader
    ↓
SymbolDatabase
    ↓
существующий Agent API
```

После загрузки MAP существующие:

```text
getSymbols()
getFunction()
getXrefs()
getCallGraph()
getDebugState()
```

должны автоматически видеть MAP symbols через Symbol Database.

Новый `getMapSymbols()` не нужен.

---

# 20. GUI

Не добавлять отдельное большое окно для MAP.

Можно добавить небольшую информацию в существующий UI/status bar:

```text
MAP: check_bugs.map
Symbols: 123
Functions: 37
```

если это легко интегрируется в существующий интерфейс.

Это необязательно для основной задачи.

---

# 21. Тесты parser

Добавить отдельный unit test для MAP parser.

Минимально проверить:

### Test 1 — public function

```text
_main = $0252 ; addr, public, ...
```

Результат:

```text
name = _main
address = 0x0252
visibility = public
type = function
```

### Test 2 — local label

```text
fp_c000_inner = $01D1 ; addr, local, ...
```

Проверить:

```text
address = 0x01D1
visibility = local
```

### Test 3 — source location

Проверить извлечение:

```text
main.c:109
```

из соответствующей MAP-записи.

### Test 4 — hexadecimal boundaries

Проверить:

```text
$0000
$FFFF
```

### Test 5 — invalid address

Проверить корректный отказ/skip для адреса за пределами 16 бит.

### Test 6 — malformed lines

Parser не должен падать.

### Test 7 — duplicate symbols

Проверить определённое поведение.

---

# 22. Integration tests

Добавить integration test:

```text
load ROM
    ↓
auto-detect MAP
    ↓
parse MAP
    ↓
SymbolDatabase
    ↓
getSymbols()
getFunction()
```

Проверить реальные символы из:

```text
check_bugs.map
```

Не заменять integration test искусственным маленьким MAP, если доступен настоящий пользовательский файл.

---

# 23. GUI smoke test

Добавить/расширить GUI smoke test:

1. загрузить ROM + MAP;
2. открыть Functions;
3. убедиться, что MAP-функции присутствуют;
4. открыть Disassembly;
5. убедиться, что label отображается по адресу функции;
6. перейти из Functions в Disassembly;
7. проверить корректный адрес.

Если существующий GUI smoke framework не позволяет этого сделать без чрезмерного усложнения, достаточно backend-level integration test и указать ограничение в отчёте.

---

# 24. Performance

MAP загружается только при загрузке ROM.

Не парсить MAP каждый кадр.

Не выполнять filesystem lookup MAP в render loop.

Не выполнять повторный parser при каждом открытии Functions/Disassembly.

---

# 25. Потоки

Загрузка MAP и изменение Symbol Database должны выполняться в безопасной точке относительно emulation thread.

Не допускать одновременного изменения Symbol Database из GUI и чтения его emulation/Agent API потоком.

Использовать существующую модель синхронизации debugger.

---

# 26. `src/`

**Строго запрещено изменять `src/`.**

Если для загрузки MAP требуется дополнительная функциональность, она должна быть реализована в:

```text
debugger/
```

Предпочтительные места:

```text
debugger/src/
debugger/agent/
debugger/gui/
```

в зависимости от назначения.

---

# 27. Что НЕ делать

В этой итерации не реализовывать:

* DWARF;
* полноценную source-level debugging;
* breakpoint по имени функции;
* breakpoint по номеру строки;
* автоматическую загрузку `.c/.asm`;
* компиляцию исходников;
* новый Symbol Database;
* новый механизм disassembly;
* новый механизм Functions;
* MCP;
* JSON serialization;
* изменение `src/`.

---

# 28. Финальный отчёт

После реализации предоставить:

1. список изменённых файлов;
2. описание формата Z88DK MAP, который фактически поддержан;
3. пример разобранных символов из `check_bugs.map`;
4. описание интеграции с Symbol Database;
5. подтверждение автоматического поиска `check_bugs.map`;
6. подтверждение отображения символов в Functions;
7. подтверждение отображения labels в Disassembly;
8. результаты parser tests;
9. результаты integration/GUI tests;
10. полный результат регрессионного тестирования;
11. подтверждение отсутствия изменений в `src/`;
12. commit hash.

## Критерий готовности

Stage 6.2 считается завершённой, если:

* `check_bugs.rom` автоматически находит `check_bugs.map`;
* MAP успешно разбирается;
* символы попадают в существующий Symbol Database;
* функции появляются в Functions;
* labels появляются в Disassembly;
* Functions и Disassembly используют одни и те же Symbol Database entries;
* при загрузке нового ROM старые MAP symbols удаляются;
* ROM без MAP продолжает работать;
* parser устойчив к неизвестным/повреждённым строкам;
* есть unit + integration tests;
* весь существующий тестовый набор продолжает проходить;
* `src/` не изменён.
