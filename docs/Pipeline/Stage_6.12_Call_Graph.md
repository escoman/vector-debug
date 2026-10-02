# Stage 6.12 — Call Graph

## Построение Call Graph из RDB

### Цель

Добавить в debugger визуальный **Call Graph**, построенный на основе объектов и `links`, хранящихся в RDB.

Граф является **производным представлением RDB**:

```text
RDB
 ├── objects
 └── links
       ↓
   Call Graph
       ↓
 imgui-node-editor
```

Call Graph не является отдельной базой данных и не сохраняется в `.rdb`.

---

# 1. Источник данных

Единственным источником данных для построения графа является RDB текущего ROM.

Использовать:

```text
RdbObject
RdbObject::address
RdbObject::type
RdbObject::name
RdbObject::comment
RdbObject::links
```

Не выполнять дополнительный анализ ROM, disassembly или Board во время построения графа.

Не использовать:

```text
MAP
SymbolDatabase
DebugBackend CPU state
runtime trace
Board
Memory
```

если соответствующая информация уже отсутствует в RDB.

MAP может влиять на граф только косвенно — через ранее выполненный MAP → RDB import.

---

# 2. Семантика графа

Каждый RDB object может стать узлом графа.

Каждый `link` создаёт направленное ребро:

```text
source object
     │
     │ link
     ↓
target object
```

Например:

```json
{
    "address": "0x4000",
    "type": "function",
    "name": "main",
    "links": ["0x4100", "0x4200"]
}
```

создаёт:

```text
main
 ├──→ 0x4100
 └──→ 0x4200
```

Не предполагать, что link обязательно является именно вызовом функции.

RDB links являются универсальными отношениями.

На уровне Call Graph они визуализируются как directed edges.

---

# 3. Неизвестные targets

RDB разрешает links на адреса, для которых RDB object ещё отсутствует.

Например:

```json
{
    "address": "0x4000",
    "links": ["0x8123"]
}
```

если объекта `0x8123` нет.

В таком случае создать специальный unresolved node:

```text
0x8123
<unresolved>
```

Такой узел не должен добавляться обратно в RDB автоматически.

---

# 4. Построение графа только по кнопке Build

Это ключевое требование.

При открытии окна:

```text
Call Graph
```

граф **не строится автоматически**.

При изменении RDB:

```text
add object
update object
remove object
add link
remove link
set links
```

граф также **не перестраивается автоматически**.

Пользователь должен явно нажать:

```text
Build
```

Только после этого выполняется построение/обновление графа.

Причина:

* не нагружать debugger;
* не выполнять дорогостоящую обработку во время редактирования RDB;
* не перестраивать граф после каждого изменения;
* дать пользователю контролировать момент пересчёта.

---

# 5. Состояния графа

Окно должно различать:

```text
Graph not built
Graph built
Graph outdated
Graph building
```

После изменения RDB:

```text
Graph outdated
```

Например:

```text
Call Graph *
```

или:

```text
Call Graph — outdated
```

Но существующий граф при этом не уничтожать.

Пользователь может продолжать работать со старой версией графа.

После `Build`:

```text
Graph building
        ↓
Graph built
```

---

# 6. Кнопка Build

В верхней части окна:

```text
[ Build ]
```

При нажатии:

1. получить актуальное состояние RDB;
2. построить набор graph nodes;
3. построить набор graph edges;
4. разрешить targets;
5. создать unresolved nodes при необходимости;
6. заменить текущую graph model;
7. передать результат визуальному редактору;
8. пометить граф как актуальный.

Не выполнять Build каждый кадр GUI.

---

# 7. Graph Model

Не связывать GUI непосредственно с `RdbObject`.

Создать отдельную внутреннюю модель:

```cpp
struct CallGraphNode
{
    uint16_t address;
    std::string name;
    RdbObjectType type;
    std::string comment;
    bool unresolved;
};

struct CallGraphEdge
{
    uint16_t source;
    uint16_t target;
};
```

или эквивалентную структуру.

Graph Model является read-only snapshot данных RDB на момент `Build`.

После построения изменение RDB не должно изменять существующий graph model.

---

# 8. Узлы

Для каждого RDB object создать node.

Минимально отображать:

```text
name
address
type
```

Например:

```text
┌──────────────────────┐
│ main                 │
│ 0x4000               │
│ function             │
└──────────────────────┘
```

Если `name` отсутствует:

```text
0x4000
function
```

Если object unresolved:

```text
┌──────────────────────┐
│ <unresolved>         │
│ 0x8123               │
└──────────────────────┘
```

---

# 9. Рёбра

Каждый link отображается направленной стрелкой.

Направление:

```text
source → target
```

Стрелка должна визуально показывать направление связи.

Не создавать duplicate edge.

Если RDB содержит:

```text
0x4000 → 0x4100
0x4000 → 0x4100
```

в графе должна быть одна связь.

---

# 10. Использовать imgui-node-editor

Для визуализации использовать:

**imgui-node-editor**

Это соответствует существующей архитектуре:

```text
Dear ImGui
    ↓
SDL2/OpenGL
    ↓
imgui-node-editor
```

Не писать собственный node editor.

Не использовать отдельный GUI framework.

Основные возможности:

* nodes;
* directed links;
* pan;
* zoom;
* selection;
* node movement;
* minimap — если понадобится;
* визуальное редактирование расположения узлов.

---

# 11. Layout

Первоначально после `Build` граф должен получить автоматическое расположение узлов.

Не требуется реализовывать сложный graph-layout algorithm на этом этапе.

Допустимо начать с простого deterministic layout:

```text
nodes arranged in rows/columns
```

или другого стабильного алгоритма.

Главное:

* граф должен быть читаемым;
* узлы не должны полностью накладываться друг на друга;
* одинаковый RDB должен давать предсказуемый initial layout.

Пользователь после этого может перемещать nodes вручную.

---

# 12. Не сохранять layout в RDB

Критически важно:

координаты узлов, zoom, pan и другие визуальные параметры **не являются частью RDB**.

Не добавлять в RDB:

```json
"x": 100,
"y": 200
```

и аналогичные GUI-specific поля.

RDB содержит только:

```text
objects
links
semantic properties
```

а GUI содержит:

```text
node position
zoom
pan
selection
```

---

# 13. Состояние layout

На первом этапе layout может существовать только в памяти.

При повторном:

```text
Build
```

допустимо заново создать initial layout.

Не требуется сохранять пользовательские позиции узлов в этой стадии.

В будущем при необходимости можно добавить отдельное workspace/session storage.

Но это не должно быть частью `.rdb`.

---

# 14. Навигация из Graph Node

При выборе node предоставить возможность перейти к соответствующему адресу.

Например:

```text
Right click
    ↓
Go to Disassembly
Go to Memory
```

Для resolved node использовать:

```text
RDB object address
```

Для unresolved node:

```text
target address
```

Даже если RDB object отсутствует, переход к адресу должен быть возможен.

---

# 15. Информация о node

При hover или selection показывать дополнительную информацию:

```text
Address
Name
Type
Size
Comment
```

Если есть properties:

```text
Properties
```

можно показать их в tooltip/details panel.

Не требуется отображать все properties непосредственно внутри node.

---

# 16. Фильтрация

На первом этапе обязательная фильтрация не требуется.

Но архитектура должна позволять в дальнейшем добавить:

```text
Functions only
Objects with links
Address range
Name search
```

Не реализовывать эти функции заранее, если они не нужны для базового Call Graph.

---

# 17. Большие графы

Не выполнять:

```text
Build()
```

в каждом GUI frame.

Не выполнять полный rebuild при:

```text
mouse move
node move
zoom
pan
selection
hover
```

Эти операции работают только с уже построенной Graph Model.

---

# 18. Threading

Build должен выполняться с учётом существующей модели потоков.

GUI не должен напрямую обращаться к mutable Board state.

Call Graph должен строиться только из RDB snapshot.

Следовательно:

```text
GUI thread
    ↓
RDB snapshot
    ↓
Graph Model
    ↓
imgui-node-editor
```

Board для построения графа не нужен.

Если получение RDB snapshot требует синхронизации с DebugBackend, использовать существующий безопасный механизм доступа.

Не добавлять новый runtime instrumentation.

---

# 19. Agent API / MCP

На этом этапе **не требуется отдельный MCP tool для визуального графа**.

Call Graph является GUI presentation layer.

Agent API/MCP уже может работать с:

```text
RDB objects
RDB links
```

Через существующие RDB API.

Если потребуется программное построение графа для AI, оно должно быть производным от тех же:

```text
objects + links
```

а не отдельным хранилищем.

---

# 20. Call Graph не изменяет RDB

Операции:

```text
Build
Zoom
Pan
Move node
Select node
```

не должны менять `.rdb`.

Call Graph является полностью read-only представлением RDB.

---

# 21. GUI integration

Добавить пункт:

```text
View → Call Graph
```

и отдельное окно:

```text
Call Graph
```

Окно должно поддерживать обычный workspace/docking механизм debugger.

При открытии:

```text
Call Graph
```

показывается пустое состояние:

```text
No graph built.

[ Build ]
```

если Build ещё не выполнялся.

---

# 22. Empty graph

Если RDB не содержит объектов:

```text
No objects in RDB.

[ Build ]
```

Если objects есть, но links отсутствуют:

```text
No links.

Graph contains nodes only.
```

Это не ошибка.

---

# 23. Build statistics

После Build желательно показывать краткую статистику:

```text
Nodes: 124
Links: 317
Unresolved: 8
```

Это позволит быстро понять результат построения.

Статистика относится только к текущему Graph Model и не сохраняется в RDB.

---

# 24. Производительность

Build должен быть оптимизирован под потенциально большой RDB.

Не использовать алгоритмы:

```text
O(N²)
```

для простого поиска target.

Создать address → node lookup:

```text
std::unordered_map<uint16_t, NodeId>
```

или эквивалентный O(1)-lookup.

Построение:

```text
objects → nodes
links → edges
```

должно быть близким к:

```text
O(N + E)
```

где:

```text
N = number of objects
E = number of links
```

---

# 25. Ошибки

Build не должен падать из-за некорректного RDB link.

Например:

```text
invalid target
unresolved target
duplicate link
```

обрабатываются корректно.

Unresolved target превращается в unresolved node.

Дубликаты links игнорируются.

---

# 26. Проверка корректности

Для тестового RDB:

```text
A → B
A → C
B → C
C → D
```

граф должен содержать:

```text
A ──→ B
│     │
│     ↓
└──→ C ──→ D
```

Для:

```text
A → 0x9000
```

при отсутствии объекта `0x9000`:

```text
A ──→ <unresolved 0x9000>
```

---

# 27. Tests

Добавить unit tests для Graph Model:

### Nodes

```text
object → node
multiple objects
missing name
unresolved target
```

### Links

```text
single link
multiple links
duplicate links
self-link
cyclic links
```

Self-link:

```text
A → A
```

должен корректно отображаться.

Cycle:

```text
A → B
B → A
```

не должен вызывать бесконечную обработку.

### Build

Проверить:

```text
N objects → N nodes
E unique links → E edges
```

### RDB changes

Проверить:

```text
Build
modify RDB
graph becomes outdated
old graph remains available
Build again
graph reflects new RDB
```

### Empty RDB

Проверить корректное поведение.

---

# 28. Regression

Обязательно сохранить все существующие тесты.

Особенно:

```text
RDB Controller
Agent API
MCP
MAP import
Symbol Database
ROM loading
Backend
GUI build
```

Не должно быть изменений в:

```text
src/
```

---

# 29. Документация

Создать:

```text
debugger/docs/Pipeline/Stage_6.12_Call_Graph.md
```

Документ должен описывать:

* назначение Call Graph;
* RDB как источник;
* objects;
* links;
* unresolved nodes;
* Build model;
* Graph Model;
* imgui-node-editor;
* threading;
* performance;
* отсутствие хранения layout в RDB.

Обновить:

```text
.qoder/skills/vector06c-debugger/SKILL.md
.qoder/agents/vector06c-analyst.md
debugger/agent/AI_AGENT_WORKFLOW.md
```

если это необходимо для использования Call Graph агентом.

---

# 30. Запреты

В Stage 6.12 не делать:

* автоматический Build каждый кадр;
* автоматический Build после каждого изменения RDB;
* runtime analysis Board;
* disassembly во время Build;
* новый runtime instrumentation;
* изменение `src/`;
* отдельную базу Call Graph;
* сохранение graph layout в `.rdb`;
* изменение RDB при работе с графом;
* отдельный GUI framework;
* собственный node editor;
* новую систему graph links вместо существующих RDB links.

---

# 31. Persistence — файл `.rdb.graph`

Визуальное состояние графа (позиции узлов, zoom, pan) сохраняется `imgui-node-editor` в файл:

```text
<rom basename>.rdb.graph
```

Например:

```text
clrs.rom  →  clrs.rdb  →  clrs.rdb.graph
```

Разделение ответственности:

```text
.rdb          — семантическая база данных ROM (objects, links, properties, comments)
.rdb.graph    — визуальное состояние node editor (positions, zoom, pan, selection)
```

RDB → objects + links → Graph Model → imgui-node-editor → .rdb.graph

`.rdb.graph` не редактируется нашим кодом вручную. Файл создаётся только когда node editor действительно сохраняет своё состояние.

При смене ROM editor пересоздаётся с новым `.rdb.graph` файлом.

При Build новые узлы получают automatic layout, существующие узлы сохраняют свои позиции.

NodeId = адрес RDB object (стабильный идентификатор).

---

# 32. Критерий завершения

Stage 6.12 считается завершённым, если:

* существует окно `Call Graph`;
* используется `imgui-node-editor`;
* граф строится только по нажатию `Build`;
* RDB является единственным источником данных;
* nodes соответствуют RDB objects;
* edges соответствуют RDB links;
* unresolved targets отображаются корректно;
* duplicate links не создают duplicate edges;
* cycles и self-links обрабатываются;
* после изменения RDB граф помечается outdated;
* старый граф остаётся доступным до следующего Build;
* layout не сохраняется в RDB;
* visual state сохраняется imgui-node-editor в `.rdb.graph`;
* файл `.rdb.graph` имеет имя `<rom>.rdb.graph`;
* `.rdb.graph` не редактируется нашим кодом вручную;
* GUI не обращается к Board для построения графа;
* Build не выполняется каждый GUI frame;
* построение имеет сложность порядка O(N + E);
* все тесты проходят;
* `git diff -- src/` пустой;
* `v06c-debugger` собирается.

После выполнения **не добавлять дополнительную функциональность**.

Это этап реализации базового Call Graph поверх существующей RDB-модели.
