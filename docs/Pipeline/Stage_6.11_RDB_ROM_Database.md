# Stage 6.11 — RDB ROM Database

## ROM Database как основной формат базы знаний о ROM

### Цель

Перейти от Z88DK `.map` как рабочего хранилища символов к собственному универсальному формату `.rdb` — **ROM Database**.

`.map` используется только как необязательный источник начальных символов при загрузке ROM.

`.rdb` является основным persistent-хранилищем структурированной информации о ROM.

Формат должен быть платформонезависимым и пригодным для будущей поддержки:

```text
Vector-06C
ZX Spectrum
NES
SEGA
SNES
и других платформ
```

Базовая структура файла:

```json
{
  "format": "rdb",
  "platform": "vector06c",
  "version": 1
}
```

---

# 1. Общая архитектура

Добавить отдельный модуль **RDB Controller**.

Целевая архитектура:

```text
GUI
  ↓
IDebugBackend
  ↓
DebugBackend
  ↓
RDB Controller
  ↓
.rdb
```

и:

```text
AI Agent
  ↓
Agent API
  ↓
RDB Controller
  ↓
.rdb
```

RDB Controller является единственным компонентом, который непосредственно работает с содержимым `.rdb`.

Остальные компоненты не должны самостоятельно:

* читать JSON RDB;
* изменять JSON RDB;
* сериализовать RDB;
* писать `.rdb`;
* парсить структуру `.rdb`.

Все операции выполняются через API RDB Controller.

---

# 2. RDB — отдельный модуль

Создать отдельный модуль, например:

```text
debugger/rdb/
```

или эквивалентную структуру.

Он должен содержать:

```text
RDB
RDBObject
RDBController
```

Названия могут быть скорректированы под существующую архитектуру проекта.

Основная задача:

```text
RDBController
```

— управление жизненным циклом базы:

```text
load
inspect
query
add
update
remove
save
saveAs
reload
```

---

# 3. RDB Controller — единственный владелец данных

RDB Controller должен инкапсулировать внутреннее представление базы.

Внешний код не должен получать:

```cpp
json&
json*
internal_json
std::vector<internal_object>&
```

или аналогичный доступ к внутреннему storage.

Нельзя делать API вида:

```cpp
rdb.getJson()
rdb.getDocument()
rdb.getInternalData()
```

Внешнему коду возвращаются только типизированные данные и результаты операций.

---

# 4. Формат файла

RDB хранится как JSON-текст.

Минимальный заголовок:

```json
{
  "format": "rdb",
  "platform": "vector06c",
  "version": 1
}
```

Формат должен быть расширяемым.

Не привязывать сам формат к Vector-06Ц.

Не использовать:

```text
vector06c-rdb
vector06c_rdb
```

в качестве значения `format`.

`format` всегда:

```text
rdb
```

Платформа определяется отдельным полем:

```text
platform
```

---

# 5. ROM identity

RDB должна содержать сведения, позволяющие однозначно связать базу с ROM.

Например:

```json
"rom": {
  "file": "klad.rom",
  "size": 32768,
  "sha256": "..."
}
```

Минимально необходимо поддержать:

```text
file
size
sha256
```

RDB Controller должен проверять соответствие базы загруженному ROM.

При несовпадении SHA-256 база не должна автоматически применяться к другому ROM.

Состояние mismatch должно быть доступно через API.

---

# 6. RDB Object

Основная единица информации RDB — объект ROM.

Каждый объект должен поддерживать как минимум:

```text
address
type
name
size
comment
properties
```

Пример:

```json
{
  "address": "0x1234",
  "type": "function",
  "name": "draw_sprite",
  "size": 47,
  "comment": "Рисует спрайт",
  "properties": {}
}
```

---

# 7. Типы объектов

В базовом RDB schema предусмотреть универсальные типы:

```text
function
variable
data
table
string
code
label
unknown
```

Не ограничивать архитектуру возможностью добавления новых типов в будущем.

Не создавать платформенные типы без необходимости.

Например Vector-specific информация должна находиться в:

```text
properties
```

а не требовать изменения общего формата.

---

# 8. Properties

Каждый объект может иметь дополнительные свойства:

```json
"properties": {
  ...
}
```

Properties должны быть расширяемыми.

Для функции, например:

```json
"properties": {
  "parameters": [
    {
      "name": "x",
      "type": "uint8"
    },
    {
      "name": "y",
      "type": "uint8"
    }
  ],
  "return_type": "void",
  "calling_convention": "custom"
}
```

Для данных:

```json
"properties": {
  "element_type": "uint8",
  "element_count": 16
}
```

Для Vector-06Ц допустимы платформенные свойства:

```json
"properties": {
  "memory_region": "vram",
  "plane": 0
}
```

Core RDB Controller не должен жестко кодировать все возможные platform-specific properties.

---

# 9. Адрес

Адрес объекта должен храниться как числовое значение в диапазоне платформы.

В JSON допускается строковое hexadecimal-представление:

```json
"address": "0x8120"
```

RDB Controller должен предоставлять адрес наружу как типизированное числовое значение.

Не заставлять GUI или Agent API самостоятельно парсить:

```text
"0x8120"
```

---

# 10. Размер

Объект может иметь:

```text
size
```

Размер выражается в байтах.

Для объектов, где размер неизвестен, поле может отсутствовать или иметь явно определенное состояние.

Не делать обязательным `size` для всех типов объектов.

---

# 11. Комментарии

Каждый объект может иметь:

```text
comment
```

Комментарии должны редактироваться через API.

Например:

```text
setObjectComment(address, comment)
```

Не использовать raw JSON editing.

---

# 12. RDB Controller API

RDB Controller должен предоставлять типизированные методы примерно следующего назначения.

### Lifecycle

```text
load(path)
reload()
save()
saveAs(path)
close()
```

### State

```text
isLoaded()
isDirty()
getPath()
getPlatform()
getVersion()
getRomIdentity()
```

### Objects

```text
getObject(address)
findObject(name)
listObjects()
addObject(object)
updateObject(object)
removeObject(address)
```

### Comments

```text
setComment(address, comment)
getComment(address)
```

### Properties

```text
getProperty(address, name)
setProperty(address, name, value)
removeProperty(address, name)
```

Конкретные C++ signatures определить с учетом существующей архитектуры.

---

# 13. Dirty state

RDB Controller обязан отслеживать изменение базы.

После:

```text
load
save
reload
```

состояние:

```text
dirty = false
```

После любого успешного изменения:

```text
add
update
remove
setComment
setProperty
...
```

становится:

```text
dirty = true
```

---

# 14. Не сохранять RDB после каждой операции

Это принципиально.

Следующая последовательность:

```text
addObject()
updateObject()
setComment()
setProperty()
```

не должна автоматически приводить к четырем операциям записи файла.

Изменения остаются в памяти.

Запись выполняется только при явном:

```text
save()
```

или при предусмотренном приложением lifecycle-save.

---

# 15. Условия сохранения

Если:

```text
dirty == false
```

вызов:

```text
save()
```

не должен переписывать файл без необходимости.

Если:

```text
dirty == true
```

выполняется сохранение.

После успешного сохранения:

```text
dirty = false
```

При ошибке записи:

```text
dirty = true
```

---

# 16. Atomic save

Сохранение должно быть безопасным.

Не писать непосредственно поверх существующего файла таким образом, чтобы сбой записи мог уничтожить рабочую базу.

Использовать схему:

```text
.rdb.tmp
    ↓
flush / close
    ↓
atomic rename
    ↓
.rdb
```

Конкретная реализация определяется платформой.

---

# 17. Проверка после сохранения

После успешной записи RDB Controller должен гарантировать, что сохраненный файл корректен.

Минимальная проверка:

```text
file exists
JSON parses
format == "rdb"
version supported
platform valid
required structure valid
```

Не требуется перечитывать весь файл после каждого `save()`, если корректность может быть гарантирована уже выполненной сериализацией и atomic replacement.

---

# 18. MAP import

Существующий Z88DK MAP loader сохранить.

Но изменить его роль.

Теперь:

```text
MAP ≠ database
```

MAP является только источником начальных сведений.

Схема:

```text
ROM
 │
 ├── .map exists
 │       ↓
 │   MAP loader
 │       ↓
 │   RDB objects
 │
 └── .rdb exists
         ↓
      RDB Controller
```

Если рядом с ROM существует `.map`, его данные могут быть импортированы в RDB.

После импорта дальнейшая работа выполняется только с RDB.

---

# 19. Приоритет RDB и MAP

Если существуют:

```text
game.rom
game.map
game.rdb
```

основным источником является:

```text
game.rdb
```

MAP не должен перезаписывать существующие RDB-данные автоматически.

MAP может использоваться:

1. при первом создании RDB;
2. по явной команде Import MAP;
3. для добавления отсутствующих объектов.

Не выполнять молчаливый повторный импорт MAP при каждом запуске.

---

# 20. Автоматическое создание RDB

При загрузке ROM:

```text
game.rom
```

искать:

```text
game.rdb
```

Если RDB существует:

```text
load RDB
```

Если RDB отсутствует:

```text
если game.map существует:
    импортировать MAP
    создать RDB
иначе:
    создать пустую RDB
```

Создание нового файла `.rdb` не обязательно должно происходить немедленно.

До появления изменений база может существовать только в памяти.

---

# 21. Импорт MAP должен быть отдельной операцией

Не смешивать MAP parser и RDB storage.

Правильная схема:

```text
Z88DK MAP Loader
        ↓
typed MAP symbols
        ↓
RDB Controller
        ↓
RDB objects
```

MAP loader не должен знать внутреннюю JSON структуру RDB.

RDB Controller не должен знать детали синтаксиса MAP.

---

# 22. GUI — окно ROM Database

Добавить отдельное окно:

```text
ROM Database
```

Оно должно отображать содержимое текущей RDB.

Минимально отображать:

```text
Address
Type
Name
Size
Comment
```

Например:

```text
Address   Type       Name             Size   Comment
----------------------------------------------------------
0100      function   start            42     Entry point
1234      function   draw_sprite      47     Draw sprite
8120      variable   screen_buffer    256    Screen buffer
```

---

# 23. ROM Database GUI

Окно должно позволять:

```text
просматривать объекты
выбирать объект
редактировать имя
редактировать тип
редактировать размер
редактировать комментарий
редактировать properties
добавлять объект
удалять объект
```

Если полный editor properties слишком сложен для текущего этапа, сделать минимальный UI для основных полей, но API должен поддерживать полный набор properties.

---

# 24. Dirty indicator в GUI

Окно:

```text
ROM Database
```

должно визуально показывать наличие несохраненных изменений.

Например:

```text
ROM Database *
```

или аналогичный индикатор.

Должна существовать команда:

```text
Save ROM Database
```

Она вызывает:

```text
RDBController::save()
```

а не самостоятельно записывает JSON.

---

# 25. Закрытие ROM

Если:

```text
rdb.isDirty() == true
```

при закрытии ROM или загрузке другого ROM необходимо обработать несохраненные изменения.

Предусмотреть:

```text
Save
Discard
Cancel
```

Не терять изменения молча.

---

# 26. Agent API

Добавить работу с RDB в Agent API.

Agent API должен работать только через RDB Controller.

Не предоставлять Agent API доступ к JSON.

Добавить операции примерно такого назначения:

```text
rdb_get_info
rdb_list_objects
rdb_get_object
rdb_find_object
rdb_add_object
rdb_update_object
rdb_remove_object
rdb_set_comment
rdb_set_property
rdb_save
rdb_reload
```

Названия привести к существующей схеме Agent API.

---

# 27. Agent API — dirty state

Agent должен иметь возможность узнать:

```text
isDirty
```

и явно сохранить базу.

AI не должен вызывать `save` после каждой операции.

Правильный workflow:

```text
load
 ↓
inspect
 ↓
multiple modifications
 ↓
verify
 ↓
save
 ↓
verify
```

---

# 28. MCP

MCP должен быть тонким адаптером над Agent API.

Не реализовывать RDB logic внутри MCP.

Схема:

```text
MCP
 ↓
Agent API
 ↓
RDB Controller
 ↓
RDB
```

MCP tools должны возвращать структурированные данные.

Не возвращать raw JSON-файл вместо нормального результата API.

---

# 29. AI Agent workflow

Обновить:

```text
.qoder/skills/vector06c-debugger/SKILL.md
.qoder/agents/vector06c-analyst.md
debugger/agent/AI_AGENT_WORKFLOW.md
```

Новые правила:

### RDB является рабочей базой ROM

AI должен:

```text
прочитать RDB
↓
анализировать ROM
↓
добавлять/изменять объекты через RDB API
↓
проверять результат
↓
сохранять RDB
```

AI не должен вручную генерировать JSON RDB.

AI не должен редактировать `.rdb` как текстовый файл.

AI не должен вручную генерировать `.map` для хранения результатов анализа.

---

# 30. MCP-first сохраняется

Существующее правило MCP-first не менять.

При анализе ROM:

```text
MCP
 ↓
Agent API
 ↓
DebugBackend / RDB Controller
```

AI не должен самостоятельно реализовывать:

```text
disassembler
MAP parser
RDB parser
ROM database parser
```

---

# 31. Документация

Добавить документацию:

```text
debugger/docs/Pipeline/Stage_6.11_RDB_ROM_Database.md
```

и документацию формата, например:

```text
debugger/agent/knowledge/rdb_format.md
```

`rdb_format.md` является **единственным каноническим описанием структуры RDB**.

В нем описать:

```text
format
platform
version
rom
objects
object fields
types
properties
dirty/save semantics
ROM identity
```

---

# 32. Удалить конкурирующие описания MAP как database

Предыдущая идея редактирования MAP больше не является архитектурой Stage 6.11.

Не добавлять в документацию инструкции:

```text
MAP является рабочей базой
AI редактирует MAP
RDB является производным от MAP
```

Правильная модель:

```text
MAP → optional import
RDB → primary database
```

Существующую документацию Z88DK MAP не удалять.

Она по-прежнему необходима для понимания формата импортируемых `.map`.

Но не смешивать:

```text
MAP format
```

и:

```text
RDB format
```

---

# 33. Backward compatibility

Существующий MAP loader должен продолжить работать.

Существующий debugger functionality:

```text
symbols
functions
xrefs
call graph
disassembly
memory map
```

не должен ломаться.

Если существующая система symbols использует собственный Symbol Database, необходимо определить минимальный adapter между ним и RDB, а не дублировать два независимых хранилища.

Цель:

```text
RDB
 ↓
Symbol/Analysis data
 ↓
Debugger
```

а не:

```text
RDB
 +
MAP
 +
Symbol Database
```

с независимыми копиями одних и тех же данных.

---

# 34. Тестирование

Добавить тесты для RDB Controller:

### Basic

```text
create empty RDB
load RDB
read metadata
read ROM identity
```

### Objects

```text
add object
get object
find by name
list objects
update object
remove object
```

### Properties

```text
set property
get property
remove property
```

### Comments

```text
set comment
get comment
```

### Dirty state

```text
load → clean
add → dirty
update → dirty
save → clean
save when clean → no unnecessary rewrite
```

### Persistence

```text
save
reload
verify data
```

### Save safety

```text
atomic save
invalid target
write failure
```

### ROM identity

```text
matching SHA-256
mismatching SHA-256
```

### MAP import

```text
MAP → RDB
existing RDB is not overwritten
explicit MAP import
```

---

# 35. Agent API regression

Все существующие тесты Agent API должны продолжить проходить.

Особенно:

```text
test_agent_api
test_agent_commands
test_agent_contract
```

Добавить RDB-specific tests.

---

# 36. MCP regression

Все существующие MCP tests должны продолжить проходить.

Добавить проверки:

```text
rdb_get_info
rdb_list_objects
rdb_get_object
rdb_add_object
rdb_update_object
rdb_remove_object
rdb_set_comment
rdb_set_property
rdb_save
```

Проверять реальную цепочку:

```text
MCP
 ↓
Agent API
 ↓
RDB Controller
 ↓
filesystem
```

Не использовать mock/stub вместо реального RDB Controller в integration tests.

---

# 37. Dependency rules

RDB Controller не должен зависеть от:

```text
ImGui
SDL
OpenGL
GUI
MCP
Qoder
Vector Board
```

Если возможно, сам RDB Controller должен быть платформонезависимым.

`platform` — данные внутри RDB, а не compile-time dependency.

---

# 38. Platform-specific logic

Не помещать Vector-specific код в общий RDB Controller.

Например:

```text
Vector memory map
Vector VRAM
Vector CPU registers
Vector I/O
```

не должны быть частью RDB Controller.

Они могут храниться в:

```text
properties
```

или интерпретироваться соответствующим adapter/analysis layer.

---

# 39. Не создавать избыточную архитектуру

Не создавать отдельные интерфейсы:

```text
IRdbJson
IRdbStorage
IRdbObjectStore
IRdbSerializer
IRdbRepository
```

если они не нужны существующей архитектуре.

На этом этапе достаточно:

```text
RDB data types
RDB Controller
```

с четкой инкапсуляцией.

---

# 40. Проверка исходников эмулятора

Stage 6.11 не должен изменять:

```text
src/
```

Проверить:

```bash
git diff -- src/
```

Ожидается:

```text
empty
```

---

# 41. Итоговая архитектура

После Stage 6.11:

```text
                         ┌──────────────┐
                         │     GUI      │
                         └──────┬───────┘
                                │
                         IDebugBackend
                                │
                         ┌──────▼───────┐
                         │ DebugBackend  │
                         └──────┬───────┘
                                │
                     ┌──────────┴──────────┐
                     │                     │
                     ▼                     ▼
               DebugAdapter          RDB Controller
                     │                     │
                     ▼                     ▼
                  Vector                 .rdb
```

Для AI:

```text
Qoder
  ↓
MCP
  ↓
Agent API
  ↓
┌───────────────┬───────────────┐
│ DebugBackend  │ RDB Controller│
└───────────────┴───────────────┘
```

MAP:

```text
ROM
 │
 ├── game.map ──→ optional import ──→ RDB
 │
 └── game.rdb ──────────────────────→ primary database
```

---

# 42. Критерий завершения Stage 6.11

Stage считается завершенным только если:

* существует отдельный RDB Controller;
* RDB Controller является единственным владельцем внутреннего RDB storage;
* JSON не выходит наружу через API;
* формат имеет `format = "rdb"`;
* присутствует `platform`;
* присутствует `version`;
* RDB содержит ROM identity;
* RDB поддерживает объекты;
* объекты имеют address/type/name/size/comment/properties;
* поддерживаются функции, переменные и data objects;
* поддерживаются platform-specific properties;
* реализованы add/update/remove/query;
* реализован dirty state;
* RDB не сохраняется после каждой модификации;
* save выполняется явно;
* clean RDB не переписывается без необходимости;
* используется безопасное atomic save;
* существует окно `ROM Database`;
* GUI работает с RDB только через API;
* Agent API работает с RDB только через Controller;
* MCP является только protocol adapter;
* MAP используется только для initial/explicit import;
* существующий MAP loader продолжает работать;
* AI Agent обновлен;
* Qoder Skill обновлен;
* документация обновлена;
* `rdb_format.md` является единственным каноническим описанием RDB;
* добавлены regression/integration tests;
* существующие debugger tests проходят;
* `git diff -- src/` пуст;
* новая функциональность, не относящаяся к RDB, не добавляется.

**Главный принцип Stage 6.11:**

```text
MAP = source
RDB = database
API = единственный способ работы с RDB
JSON = внутренний формат хранения
```

После выполнения Stage 6.11 не добавлять дополнительную функциональность, не связанную непосредственно с RDB.

---

# Дополнение к Stage 6.11 — Правила создания и сохранения RDB

## 1. RDB не создаётся автоматически при загрузке ROM

Принципиально запрещено создавать `.rdb` только потому, что ROM был открыт в debugger.

Например, если существуют:

```text
game.rom
game.map
```

но отсутствует:

```text
game.rdb
```

то после:

```text
Load ROM
```

**`game.rdb` не должен автоматически создаваться.**

Если пользователь только:

* открыл ROM;
* запустил ROM;
* поставил breakpoint;
* выполнил Step;
* посмотрел память;
* посмотрел disassembly;
* посмотрел экран;
* посмотрел CPU state;
* посмотрел MAP symbols;

и не изменил данные RDB, файл `.rdb` создавать нельзя.

---

## 2. RDB может существовать только как in-memory database

При отсутствии `.rdb` разрешается создать пустой RDB Controller в памяти:

```text
game.rom
    ↓
RDB Controller
    ↓
in-memory RDB
```

но:

```text
in-memory RDB
       X
       ↓
game.rdb
```

не должен записываться на диск, пока не произошло реальное изменение пользовательских данных.

---

## 3. `dirty` и `exists-on-disk` — разные состояния

Необходимо различать:

```text
RDB loaded from disk
RDB exists only in memory
RDB dirty
```

Например:

| Состояние                             | RDB на диске | dirty |
| ------------------------------------- | -----------: | ----: |
| `.rdb` отсутствует, ROM только открыт |          нет | false |
| `.rdb` существует, загружен           |           да | false |
| добавлен объект                       |       да/нет |  true |
| изменён комментарий                   |       да/нет |  true |
| изменено property                     |       да/нет |  true |
| сохранён                              |           да | false |

Особенно важен первый случай:

```text
RDB отсутствует + dirty=false
```

При `save()` **файл всё равно не должен создаваться**, поскольку сохранять нечего.

---

## 4. Save должен проверять dirty

Логика `save()` должна быть эквивалентна:

```text
if (!dirty)
    return success;
```

То есть:

```text
RDB существует + dirty=false
    → ничего не писать

RDB отсутствует + dirty=false
    → ничего не создавать

RDB существует + dirty=true
    → сохранить

RDB отсутствует + dirty=true
    → создать и сохранить
```

---

## 5. Что считается изменением RDB

`dirty=true` устанавливается только после успешной операции, изменяющей содержимое базы.

Например:

```text
addObject()
updateObject()
removeObject()
setComment()
setProperty()
removeProperty()
```

Если операция фактически ничего не изменила, `dirty` не должен устанавливаться.

Например:

```text
setComment(address, existingComment)
```

не является изменением.

А:

```text
setComment(address, newComment)
```

является изменением.

То же относится к:

```text
name
type
size
properties
```

---

## 6. Чтение RDB не является изменением

Следующие операции **никогда не должны устанавливать `dirty`**:

```text
getObject()
findObject()
listObjects()
getComment()
getProperty()
getRomIdentity()
getPlatform()
getVersion()
isDirty()
```

Также не должны устанавливать `dirty`:

```text
Load ROM
Run
Pause
Step
Reset
Breakpoints
Memory inspection
Disassembly
Execution trace
I/O inspection
Screen inspection
```

если эти операции не изменяют непосредственно данные RDB.

---

## 7. Анализ ROM сам по себе не должен автоматически сохраняться

AI Agent или пользовательский debugger может выполнить анализ ROM.

Сам факт анализа:

```text
disassembly
find functions
find xrefs
inspect memory
inspect VRAM
inspect I/O
```

не должен создавать `.rdb`.

Только если результат анализа **явно записан в RDB как объект/свойство/комментарий**, RDB становится:

```text
dirty=true
```

После этого он может быть сохранён явным `save()`.

---

## 8. MAP import также не должен создавать мусорный RDB

Если:

```text
game.rom
game.map
```

открываются впервые, MAP можно прочитать и использовать как начальные сведения.

Но:

```text
Load ROM
→ read MAP
→ create empty/in-memory RDB
→ exit
```

не должно приводить к появлению:

```text
game.rdb
```

на диске.

Если пользователь явно выполняет:

```text
Import MAP into ROM Database
```

и импорт действительно добавляет/изменяет объекты, тогда:

```text
dirty=true
```

и RDB может быть сохранён.

---

## 9. Не создавать RDB только ради MAP

Нельзя реализовывать логику:

```text
если нет .rdb:
    импортировать .map
    save()
```

при обычном открытии ROM.

Правильно:

```text
если есть .rdb:
    load RDB

иначе:
    RDB отсутствует
    при необходимости создать in-memory состояние

если пользователь изменил RDB:
    dirty=true

если dirty:
    save()
```

---

## 10. Закрытие ROM

При закрытии ROM:

### Нет RDB и нет изменений

```text
game.rom
```

остаётся единственным файлом.

Никакого:

```text
game.rdb
```

создаваться не должно.

### Есть изменения

Показать:

```text
Save
Discard
Cancel
```

При `Save`:

```text
dirty=true
→ create game.rdb
→ save
```

При `Discard`:

```text
game.rdb
```

не создаётся.

---

## 11. Автоматический Save

Если debugger в будущем будет иметь automatic save, он должен работать только при:

```text
dirty == true
```

Запрещено:

```text
startup → save RDB
ROM load → save RDB
ROM run → save RDB
ROM pause → save RDB
ROM close → unconditional save RDB
```

Допустимо:

```text
ROM close
    ↓
dirty?
    ↓ yes
Save
```

---

## 12. Главный принцип

**Debugger никогда не должен создавать `.rdb` как побочный продукт обычной работы.**

`.rdb` появляется на диске только когда пользователь или AI Agent действительно изменил содержимое ROM Database и эти изменения были сохранены.

Таким образом:

```text
Открыл ROM
     ↓
Никакого RDB
     ↓
Исследовал ROM
     ↓
Никакого RDB
     ↓
Добавил функцию 0x1234
     ↓
RDB = dirty
     ↓
Save
     ↓
game.rdb создан
```

Это обязательное поведение Stage 6.11.

---

# Дополнение к Stage 6.11 — RDB Object Links

## 1. Связи между объектами

Каждый RDB Object может иметь необязательное поле:

```json
"links": [
  "0x4567",
  "0x8120"
]
```

`links` представляет собой массив адресов других объектов RDB.

Каждый элемент `links` является адресом объекта:

```text
source object address → target object address
```

Например:

```json
{
  "address": "0x1234",
  "type": "function",
  "name": "start",
  "links": [
    "0x2000",
    "0x3000"
  ]
}
```

означает:

```text
0x1234 → 0x2000
0x1234 → 0x3000
```

---

## 2. Поле links необязательное

Если у объекта нет связей, поле:

```text
links
```

может полностью отсутствовать.

Не требуется создавать:

```json
"links": []
```

для каждого объекта.

Это позволяет не раздувать `.rdb` пустыми массивами.

---

## 3. Links содержат только адреса

Не дублировать информацию о целевом объекте:

```json
"links": [
  {
    "address": "0x4567",
    "name": "foo",
    "type": "function"
  }
]
```

запрещено.

Использовать только:

```json
"links": [
  "0x4567"
]
```

Информация о целевом объекте находится в самом объекте RDB с соответствующим `address`.

---

## 4. Links являются направленными

Связь:

```text
A → B
```

не означает автоматически наличие:

```text
B → A
```

Если обратная связь необходима, она должна быть представлена отдельно.

Например:

```json
{
  "address": "0x1000",
  "links": ["0x2000"]
}
```

не требует автоматически создавать:

```json
{
  "address": "0x2000",
  "links": ["0x1000"]
}
```

---

## 5. Не требовать наличия target object

На момент создания связи целевой объект может ещё отсутствовать в RDB.

Например:

```json
{
  "address": "0x1000",
  "links": ["0x5000"]
}
```

даже если объекта `0x5000` пока нет.

Это необходимо для постепенного reverse engineering ROM.

Позднее объект `0x5000` может быть добавлен.

Debugger должен уметь отображать unresolved link как ссылку на адрес без существующего объекта.

---

## 6. RDB Controller API

Добавить типизированные операции:

```text
getLinks(address)
addLink(address, targetAddress)
removeLink(address, targetAddress)
setLinks(address, links)
```

При необходимости:

```text
hasLink(address, targetAddress)
```

Операции должны:

* проверять корректность адресов;
* не создавать дубликаты;
* устанавливать `dirty`, если содержимое реально изменилось;
* не устанавливать `dirty`, если операция фактически ничего не изменила.

Например:

```text
addLink(0x1000, 0x2000)
```

при уже существующей связи:

```text
0x1000 → 0x2000
```

не является изменением.

---

## 7. Agent API

Предоставить операции работы со связями:

```text
rdb_get_links
rdb_add_link
rdb_remove_link
rdb_set_links
```

Agent должен работать с links только через Agent API.

Не разрешать AI самостоятельно редактировать JSON RDB.

---

## 8. Call Graph

`links` должны использоваться как источник данных для построения Call Graph.

Например:

```text
Function A
    │
    ├──→ Function B
    │
    └──→ Function C
             │
             └──→ Function D
```

может быть непосредственно построено из:

```text
A.links = [B, C]
C.links = [D]
```

При этом RDB не обязан хранить отдельно готовый Call Graph.

Call Graph является производным представлением:

```text
RDB objects + links
        ↓
     Call Graph
```

---

## 9. Links и тип объекта

`links` не должны быть ограничены только функциями.

Они могут связывать любые RDB Objects:

```text
function → function
function → variable
function → data
data → data
table → function
variable → function
```

Конкретный смысл связи определяется контекстом и при необходимости дополнительными properties.

На данном этапе не вводить отдельную обязательную модель:

```text
link.type
link.kind
link.direction
```

---

## 10. Дубликаты

Один и тот же target address не должен присутствовать дважды:

```json
"links": [
  "0x2000",
  "0x2000"
]
```

недопустимо.

RDB Controller должен гарантировать уникальность ссылок внутри одного объекта.

---

## 11. Порядок links

Порядок элементов `links` не должен иметь семантического значения.

Для Call Graph:

```text
[
  "0x2000",
  "0x3000"
]
```

и:

```text
[
  "0x3000",
  "0x2000"
]
```

означают один и тот же набор связей.

Если реализация сохраняет стабильный порядок, это желательно для читаемости RDB, но не является обязательным требованием формата.

---

## 12. GUI

В окне:

```text
ROM Database
```

для выбранного объекта предусмотреть отображение его links.

Например:

```text
Address: 1234
Type: function
Name: start
Size: 42
Comment: Entry point

Links:
  2000  draw_sprite
  3000  update_screen
```

По link желательно иметь возможность перейти к соответствующему объекту.

Если target object отсутствует:

```text
5000  <unresolved>
```

---

## 13. Изменение links и Dirty State

Изменение:

```text
addLink
removeLink
setLinks
```

является изменением RDB.

Следовательно:

```text
dirty = true
```

если после операции содержимое links действительно изменилось.

После:

```text
save()
```

состояние:

```text
dirty = false
```

---

## 14. Сохранение

`links` сохраняются вместе с объектом при обычном:

```text
RDBController::save()
```

Не требуется отдельный файл или отдельная база для Call Graph.

---

## 15. Архитектурный принцип

Не хранить в RDB одновременно:

```text
objects
links
call_graph
```

если Call Graph можно полностью получить из `objects.links`.

Основные данные:

```text
Objects
   +
Links
```

Производные данные:

```text
Call Graph
Xrefs
Incoming References
Dependency Graph
```

могут строиться поверх них.

Это предотвращает дублирование и рассинхронизацию данных.

---

## 16. Тестирование

Добавить тесты:

```text
add link
get links
remove link
set links
duplicate link
unresolved target
dirty after link modification
no dirty when duplicate is added
save/load links
Call Graph generated from links
```

Отдельно проверить:

```text
A → B
```

не создаёт автоматически:

```text
B → A
```

и что отсутствие объекта B не делает ссылку недействительной.

---

## 17. Формат объекта

Итоговый пример:

```json
{
  "address": "0x1234",
  "type": "function",
  "name": "draw_sprite",
  "size": 47,
  "comment": "Draws a sprite on screen",
  "properties": {
    "parameters": [
      { "name": "x", "type": "uint8" },
      { "name": "y", "type": "uint8" }
    ]
  },
  "links": [
    "0x2000",
    "0x3000"
  ]
}
```
