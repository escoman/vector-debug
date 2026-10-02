# Stage 6.12 — Call Graph Persistence

## Доработка существующего окна Call Graph

### Цель

Доработать существующее окно **Call Graph**, чтобы:

1. граф строился по кнопке `Build`;
2. источником графа оставался RDB текущего ROM;
3. `imgui-node-editor` самостоятельно хранил и восстанавливал визуальное состояние графа;
4. расположение узлов, zoom, pan и другие параметры node editor сохранялись отдельно от RDB;
5. файл состояния графа имел имя:

```text
<rom-name>.rdb.graph
```

Например:

```text
game.rom
game.rdb
game.rdb.graph
```

---

# 1. RDB остаётся источником семантических данных

`.rdb` содержит:

```text
objects
links
properties
comments
```

Call Graph строится из:

```text
RDB objects + RDB links
```

`.rdb` не должен содержать:

```text
node position
zoom
pan
node editor state
selection
GUI-specific state
```

---

# 2. Отдельный файл `.rdb.graph`

Для каждого ROM использовать отдельный файл:

```text
<rom basename>.rdb.graph
```

Примеры:

```text
game.rom
game.rdb
game.rdb.graph
```

```text
klad.rom
klad.rdb
klad.rdb.graph
```

Файл располагается рядом с `.rdb`/ROM в соответствии с существующей моделью хранения debugger data.

Не использовать:

```text
game.graph
game.graph.json
game.rdb.gui
```

Использовать именно:

```text
game.rdb.graph
```

---

# 3. Полностью передать persistence imgui-node-editor

`imgui-node-editor` должен самостоятельно сохранять:

```text
node positions
node editor state
zoom
pan
selection/state supported by the library
```

Не создавать собственный формат хранения этих данных.

Не дублировать layout в:

```text
RDB Controller
CallGraphModel
GUI settings
workspace state
```

Если `imgui-node-editor` предоставляет штатный механизм Save/Load/Serialize/Restore состояния — использовать именно его.

---

# 4. Наш код отвечает только за имя и файл

Debugger должен предоставить node editor файл:

```text
<rom>.rdb.graph
```

для сохранения/загрузки состояния.

Логика должна быть примерно такой:

```text
ROM loaded
    ↓
determine RDB path
    ↓
derive graph state path
    ↓
<rom>.rdb.graph
    ↓
imgui-node-editor loads its state
```

Само содержимое `.rdb.graph` не интерпретировать debugger-кодом без необходимости.

---

# 5. Не создавать `.rdb.graph` автоматически без необходимости

При открытии ROM:

* если `.rdb.graph` существует — его можно загрузить;
* если файла нет — работать с новым состоянием node editor;
* не создавать пустой `.rdb.graph` только из-за открытия ROM.

Файл появляется только когда `imgui-node-editor` действительно сохраняет своё состояние.

---

# 6. Build

Существующая кнопка:

```text
[ Build ]
```

остаётся главным способом построения/обновления графа.

Build должен:

1. получить актуальные RDB objects;
2. получить links;
3. построить Graph Model;
4. передать nodes/links в `imgui-node-editor`;
5. восстановить сохранённое состояние node editor, если оно совместимо с текущими nodes;
6. при необходимости создать initial layout для новых nodes.

Build **не должен выполняться автоматически каждый GUI frame**.

---

# 7. Сохранение layout

После того как пользователь перемещает nodes, изменяет zoom/pan или другое состояние editor, использовать штатный механизм persistence `imgui-node-editor`.

Не писать собственный:

```cpp
saveNodePosition()
saveZoom()
savePan()
```

если библиотека уже предоставляет соответствующий механизм.

---

# 8. Совместимость layout с RDB

`.rdb.graph` хранит визуальное состояние.

RDB хранит семантическое состояние.

Поэтому возможна ситуация:

```text
game.rdb
```

изменился, а:

```text
game.rdb.graph
```

остался от предыдущей версии RDB.

При `Build` необходимо корректно обработать:

### Existing node

Если node соответствует существующему RDB object — сохранить его существующее положение.

### New node

Если появился новый RDB object — создать node с default position.

### Removed node

Если object больше отсутствует — node не должен появляться в новом графе.

### Changed links

Links должны соответствовать актуальному RDB.

---

# 9. Node identity

Для сопоставления существующего визуального node с RDB object использовать стабильный идентификатор.

Основной идентификатор:

```text
RDB object address
```

Например:

```text
0x4000
```

Не использовать имя как уникальный идентификатор.

Причина:

```text
name может измениться
address остаётся идентификатором объекта
```

---

# 10. Unresolved nodes

Unresolved link:

```text
0x4000 → 0x8123
```

при отсутствии RDB object `0x8123` должен создавать:

```text
<unresolved>
0x8123
```

Такой node также должен иметь стабильную identity, основанную на target address.

Если при следующем Build соответствующий RDB object появился, unresolved node должен быть заменён resolved node с тем же адресом.

По возможности его визуальное положение должно сохраниться.

---

# 11. RDB и graph state независимы

Изменение RDB:

```text
add object
remove object
rename
change comment
add link
remove link
```

не должно напрямую изменять `.rdb.graph`.

После изменения RDB:

```text
Call Graph → outdated
```

После:

```text
Build
```

граф синхронизируется с новым RDB.

---

# 12. Старый граф не уничтожать до Build

Если пользователь изменил RDB:

```text
RDB changed
    ↓
Graph outdated
```

существующий граф остаётся на экране.

После `Build`:

```text
old graph
    ↓
new graph
```

Это позволяет пользователю продолжать работать с текущим layout до явного обновления.

---

# 13. Call Graph — read-only presentation

Работа с графом:

```text
move node
zoom
pan
select node
Build
```

не должна изменять RDB.

Граф не должен добавлять или удалять:

```text
objects
links
comments
properties
```

---

# 14. Навигация

Сохранить существующую возможность перехода из node:

```text
Go to Disassembly
Go to Memory
```

Использовать address node.

Для unresolved node также разрешить переход к адресу.

---

# 15. Graph Model

Оставить промежуточную Graph Model между RDB и node editor:

```text
RDB
 ↓
Graph Model
 ↓
imgui-node-editor
```

Graph Model содержит только актуальное состояние графа:

```text
nodes
edges
```

и не содержит:

```text
x
y
zoom
pan
```

если эти данные уже принадлежат `imgui-node-editor`.

---

# 16. Производительность

Build должен быть единственной операцией построения графа.

Не выполнять построение:

```text
каждый frame
при mouse move
при node move
при zoom
при pan
при hover
при selection
```

Использовать address → node lookup для построения links.

Целевая сложность:

```text
O(N + E)
```

где:

```text
N = objects
E = links
```

---

# 17. Большие графы

Не выполнять дорогостоящие операции во время обычного отображения.

После Build:

```text
Graph Model
```

остаётся неизменной до следующего Build.

Node editor работает с уже созданным графом.

---

# 18. GUI

Окно `Call Graph` уже существует.

Не создавать новое окно.

Сохранить:

```text
Call Graph
[ Build ]
```

и существующую интеграцию с workspace/docking.

При отсутствии построенного графа:

```text
No graph built.

[ Build ]
```

---

# 19. Статистика

После Build показывать:

```text
Nodes: N
Links: E
Unresolved: U
```

Статистика относится к текущему Graph Model.

Она не сохраняется в `.rdb.graph`.

Она также не должна требовать отдельного persistent storage.

---

# 20. Tests

Добавить/обновить тесты:

### Graph construction

Проверить:

```text
objects → nodes
links → edges
```

### Duplicate links

```text
A → B
A → B
```

даёт одну связь.

### Cycles

```text
A → B
B → A
```

корректно обрабатываются.

### Self-link

```text
A → A
```

корректно обрабатывается.

### Unresolved

```text
A → 0x8123
```

создаёт unresolved node.

### RDB update

Проверить:

```text
Build
RDB modification
Graph outdated
Build
Graph reflects changes
```

### Graph persistence

Проверить, насколько это возможно без GUI:

```text
game.rdb
game.rdb.graph
```

не смешиваются.

При наличии штатного API `imgui-node-editor` проверить сохранение/загрузку editor state.

---

# 21. Не тестировать внутренний формат `.rdb.graph`

Не писать тесты, завязанные на конкретную структуру файла:

```text
JSON fields
binary layout
internal node-editor records
```

если они являются внутренним форматом `imgui-node-editor`.

Наша ответственность:

```text
правильное имя файла
правильный путь
передача файла node editor
```

Ответственность библиотеки:

```text
формат
serialization
deserialization
layout state
```

---

# 22. Документация

Обновить:

```text
debugger/docs/Pipeline/Stage_6.12_Call_Graph.md
```

Добавить описание:

```text
<rom>.rdb.graph
```

с явным разделением:

```text
.rdb
    semantic ROM database

.rdb.graph
    imgui-node-editor visual state
```

Также указать:

```text
RDB → objects + links
Graph → visual representation
.rdb.graph → node editor persistence
```

---

# 23. Qoder / AI Agent

Обновить Skill/Agent documentation только в части понимания архитектуры.

AI должен знать:

```text
RDB = semantic data
RDB links = graph relations
.rdb.graph = GUI/node-editor state
```

AI не должен редактировать `.rdb.graph` вручную.

Для анализа и изменения структуры ROM использовать:

```text
RDB Agent API
MCP
```

а не graph-state file.

---

# 24. Запреты

В Stage 6.12 не делать:

* собственное хранение node coordinates;
* собственный формат layout;
* хранение layout в `.rdb`;
* хранение layout в RDB properties;
* отдельную Graph Database;
* отдельную базу links;
* автоматический Build каждый frame;
* автоматический Build после каждого изменения RDB;
* анализ Board при Build;
* runtime instrumentation;
* disassembly при Build;
* изменения `src/`;
* новый GUI framework;
* собственный node editor;
* ручное редактирование `.rdb.graph`.

---

# 25. Критерий завершения

Stage 6.12 считается завершённым, если:

* существующее окно `Call Graph` использует `imgui-node-editor`;
* граф строится по кнопке `Build`;
* автоматического постоянного rebuild нет;
* RDB является источником objects и links;
* nodes соответствуют RDB objects;
* edges соответствуют RDB links;
* unresolved links отображаются;
* duplicate links не создают duplicate edges;
* cycles и self-links работают;
* после изменения RDB граф становится `outdated`;
* старый граф остаётся до следующего `Build`;
* node positions не хранятся в RDB;
* zoom/pan не хранятся в RDB;
* visual state сохраняется `imgui-node-editor`;
* файл visual state называется:

```text
<rom>.rdb.graph
```

* `.rdb.graph` не редактируется нашим кодом вручную;
* GUI не обращается к Board при построении;
* Build имеет сложность порядка O(N + E);
* все существующие тесты проходят;
* `git diff -- src/` пустой;
* `v06c-debugger` собирается.

После выполнения **не добавлять дополнительную функциональность**.

Это финальная реализация базового Call Graph и его persistence поверх существующей RDB.
