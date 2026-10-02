# Stage 6.21 — MCP Keyboard Injection

## Эмуляция нажатий виртуальной клавиатуры для AI Agent

### Цель

Дать агенту, работающему через MCP, возможность эмулировать нажатия клавиш
виртуальной клавиатуры GUI-дебаггера Vector-06Ц.

Это позволяет агенту:

* взаимодействовать с ROM'ами, читающими клавиатуру (игры, конфигурационные
  меню, ввод с клавиатуры, текстовые утилиты);
* воспроизводить сценарии ввода для проверки поведения ROM;
* переключать раскладку РУС/LAT;
* нажимать комбинации с модификаторами (СС/US).

Инструмент **не должен создавать новый механизм ввода**. Он переиспользует
уже существующий путь `IDebugBackend::pressKey/releaseKey` →
`DebugAdapter::pressKey/releaseKey` → `Keyboard::apply_key` (матрица 8×8 в
`src/keyboard.h`), которым пользуется GUI.

### Ключевое ограничение (аппаратная семантика)

ROM опрашивает клавиатурную матрицу только тогда, когда **ЦП выполняется** и
обращается к портам клавиатуры (маска строки → Port A 0x03, чтение → Port B
0x02). Поэтому:

* одиночное «прикосновение» к биту матрицы на паузе ROM не увидит;
* чтобы ROM гарантированно зарегистрировал нажатие, клавишу нужно **удерживать,
  пока эмулятор крутит кадры** (в GUI для этого держат клавишу несколько кадров,
  ~8 кадров при 50 Гц);
* модификаторы (СС/US/РУС) не являются битами матрицы — это отдельные
  флаги-защёлки (`ss`/`us`/`rus`), читаемые с Port C (0x01).

---

# 1. Модель принятия решений (что зафиксировано с пользователем)

1. **Задание клавиши — только именем** (символическое имя, регистронезависимо).
   Сырой SDL-scancode как основной способ ввода не предлагается; вместо этого
   агент открывает допустимые имена через `debug_list_keys`.
2. **Операции**: `press` / `release` (примитивы для комбинаций и удержания)
   + `type` (полный тап: нажать-держать-отпустить) + `list` (обнаружение имён).
3. **Удержание при `type` — real-time sleep ≈ 120 мс** с предупреждением, если
   эмуляция на паузе (в этом случае ROM не опросит матрицу, но клавиша остаётся
   «залащенной» до возобновления).

---

# 2. Agent API

Добавить в `AgentApi` четыре метода (все — через `IDebugBackend`, без прямого
доступа к `Board`/`Keyboard`/эмуляторным внутренностям):

```cpp
AgentApiResult<void> pressKey(const std::string &keyName);
AgentApiResult<void> releaseKey(const std::string &keyName);
AgentApiResult<void> typeKey(const std::string &keyName);
AgentApiResult<std::vector<KeyboardKeyInfo>> listKeys();
```

* `pressKey` / `releaseKey` — резолвят имя → scancode и вызывают
  `backend_.pressKey/releaseKey(scancode)`.
* `typeKey` — press → `std::this_thread::sleep_for(120ms)` → release.
* `listKeys` — возвращает полную таблицу имён.

Неизвестное имя → `AgentApiResult<void>::fail(ErrorCode::InvalidArgument,
"unknown key: <name>")`.

---

# 3. Структура результата `KeyboardKeyInfo`

Добавить в `agent_types.h`:

```cpp
struct KeyboardKeyInfo
{
    std::string name;         // каноническое имя (верхний регистр)
    int         scancode = 0; // значение SDL scancode
    std::string description;  // человекочитаемая легенда
    bool        modifier = false; // true для SS/US/RUS (защёлки, не матрица)
};
```

`listKeys()` конструируется через `AgentApiResult<std::vector<...>>::ok(value)`.

---

# 4. Таблица имён клавиш

Один источник истины — статическая таблица `имя → SDL_scancode` в
`agent_api.cpp` (в анонимном namespace). Она **зеркалит матрицу
`src/keyboard.h`**; scancode-значения — стандартные SDL2-константы,
записаны числовыми литералами, чтобы слой агента не зависел от SDL.

Объём таблицы: **70 записей**:

* Буквы `A`–`Z` (матрица, кол. 1–4);
* Цифры `0`–`9` (матрица, кол. 2–3);
* Пунктуация: `MINUS`(−/@), `EQUALS`, `LBRACKET`, `RBRACKET`, `BACKSLASH`,
  `SEMICOLON`, `APOSTROPHE`, `COMMA`, `PERIOD`, `SLASH`, `GRAVE`(^);
* Управление: `SPACE`, `TAB`, `ENTER` (ВК/Return), `BACKSPACE` (ЗАБ),
  `ESCAPE` (АР2), стрелки `UP`/`DOWN`/`LEFT`/`RIGHT`;
* Функциональные `F1`–`F8`;
* Модификаторы (`modifier = true`): `SS`→LSHIFT, `US`→LCTRL,
  `RUS`→F6 (импульс переключения РУС/LAT);
* Алиасы: `SHIFT`→SS, `CTRL`→US; матричная клавиша `PS`→RALT.

Разрешение имени — верхний регистр + линейный поиск по таблице (`findKey`).

---

# 5. MCP Tools

Зарегистрировать 4 инструмента (transport/protocol слой; вся логика — в
`AgentApi`):

| Инструмент | Параметры | Назначение |
|---|---|---|
| `debug_list_keys` | — | Таблица допустимых имён (`{name, scancode, description, modifier}`) |
| `debug_press_key` | `key` (string) | Зажать клавишу (бит/защёлка остаются) |
| `debug_release_key` | `key` (string) | Отпустить ранее зажатую |
| `debug_type_key` | `key` (string) | Полный тап: press → hold ~120 мс → release |

Хелдер `getKeyParam(params)` бросает `invalid_params`, если `key` отсутствует
или не строка. Ошибки `AgentApi` возвращаются как `errorContent("<op>_failed",
message)` — по существующей конвенции MCP-хендлеров.

Пример успешного `debug_list_keys`:

```json
{
  "count": 70,
  "keys": [
    {"name": "A", "scancode": 4, "description": "A", "modifier": false},
    {"name": "RUS", "scancode": 63, "description": "RU/LAT toggle (F6)", "modifier": true}
  ]
}
```

---

# 6. Семантика тайминга `debug_type_key`

```text
running = !backend_.isPaused()
pressKey(scancode)
sleep(120ms)        // эмуляционный тред в это время крутит кадры и опрашивает порт
releaseKey(scancode)
```

* Если ЦП **выполняется** (`debug_run` ранее) — ROM успевает опросить матрицу,
  нажатие зарегистрировано.
* Если на **паузе** — клавиша залачивается и будет отпущена; в лог пишется
  примечание `ok (was paused: ROM did not poll)`. Агенту следует сначала
  вызвать `debug_run`.

---

# 7. Комбинации с модификаторами

`type` — это одиночный тап. Для комбинаций (например, symbols via СС) агент
использует примитивы:

```text
debug_press_key   SS      // защёлка shift
debug_type_key    <key>   // базовая клавиша
debug_release_key SS      // отпустить shift
```

РУС/LAT — тумблер: `debug_type_key RUS` (или удержание F6) переключает
раскладку; состояние можно подтвердить косвенно через наблюдаемое поведение ROM.

---

# 8. Чего НЕ делать

* Не менять `src/` (основной эмулятор). `IDebugBackend::pressKey/releaseKey`,
  `DebugAdapter::pressKey/releaseKey` и `Keyboard::apply_key` уже существуют.
  `git diff -- src/` должен быть пустым.
* Не вводить новый механизм ввода/свою клавиатурную матрицу в MCP/Agent.
* Не требовать от агента сырые scancode-значения как основной способ (имя +
  discovery через `debug_list_keys`).
* Не дублировать таблицу имён в GUI и MCP — таблица живёт в слое агента.

---

# 9. Тесты

### MCP protocol (`test_mcp_protocol.cpp`)

* Счётчик зарегистрированных инструментов: **61 → 65** (+4).
* `test_expected_tools_exist`: добавить `debug_list_keys`, `debug_press_key`,
  `debug_release_key`, `debug_type_key`.

### Реальный stdio smoke-прогон (`v06c-mcp`)

* `debug_list_keys` → `count == 70`, содержит `A`, `SPACE`, `ENTER`, `RUS`;
* `debug_type_key {"key":"a"}` → успех (регистронезависимость);
* `debug_press_key {"key":"nope"}` → ошибка `unknown key: nope`.

### Knowledge validator (`validate_agent_knowledge.py`)

* `VALID_TOOLS` += 4 клавиатурных имени (чтобы будущие `TASK.md` могли
  ссылаться на них); прогон валидатора — 0 errors / 0 warnings.

---

# 10. Документация агента и скиллы

Обновить инвентарь `MCP Tools` и добавить категорию «Клавиатура»:

```text
.qoder/skills/vector06c-debugger/SKILL.md
.qoder/agents/vector06c-analyst.md
```

Добавить раздел «Key Injection via MCP Debugger» в базу знаний:

```text
debugger/agent/knowledge/vector06c/keyboard.md
```

Правило для агента: перед вводом с клавиатуры убедиться, что ЦП запущен
(`debug_run`); имена открывать через `debug_list_keys`, а не выдумывать.

---

# 11. Acceptance Criteria

Stage 6.21 считается завершённым, если:

* существуют 4 MCP-инструмента (`debug_list_keys`/`press_key`/`release_key`/`type_key`);
* `AgentApi` предоставляет press/release/type/list и делегирует в
  существующий `IDebugBackend::pressKey/releaseKey`;
* клавиша задаётся именем, регистронезависимо;
* `debug_list_keys` возвращает таблицу (70 имён) с scancode/описанием/флагом
  модификатора;
* `debug_type_key` удерживает клавишу в реальном времени (~120 мс);
* на паузе ввод залачивается и помечается предупреждением в логе;
* модификаторы SS/US/RUS поддержаны, РУС/LAT — F6-импульс;
* неизвестное имя → корректная ошибка `InvalidArgument`;
* счётчик инструментов обновлён (65), тесты MCP проходят;
* живой stdio smoke подтверждает happy path и error path;
* инвентари в SKILL/agent и база знаний `keyboard.md` обновлены;
* `validate_agent_knowledge.py` — PASSED;
* `git diff -- src/` пустой.

---

# 12. Результат Stage

```text
AI Agent
   │
   ├── debug_list_keys      → таблица имён (70)
   ├── debug_type_key NAME  → press → hold ~120мс → release   (одиночный тап)
   └── debug_press_key / debug_release_key NAME               (комбинации, удержание)
           ↓
      AgentApi (table name→scancode)
           ↓
      IDebugBackend::pressKey/releaseKey(int scancode)   [уже существовал]
           ↓
      DebugAdapter::pressKey/releaseKey                   [уже существовал]
           ↓
      Keyboard::apply_key → матрица 8×8 / флаги ss/us/rus [src/, не изменён]
           ↓
      ROM опрашивает Port A/B при выполнении ЦП
```

Главная цель Stage 6.21 — не расширить сам механизм ввода, а дать агенту
удобный именованный доступ к уже существующей виртуальной клавиатуре, с
корректной моделью реального времени (пока ЦП выполняется).

---

# 13. Статус выполнения Stage 6.21

**Статус:** ✅ Завершено

**Дата:** 2026-09-15

**Решения (зафиксированы с пользователем):**

* клавиша — только именем;
* операции press/release/type + list;
* удержание при type — real-time ~120 мс с предупреждением на паузе.

**Реализовано:**

1. ✅ `KeyboardKeyInfo` в `agent_types.h`
2. ✅ `pressKey/releaseKey/typeKey/listKeys` в `AgentApi`
3. ✅ Таблица имя→scancode (70 записей), зеркало `src/keyboard.h`, без SDL-зависимости
4. ✅ `typeKey`: press → sleep(120ms) → release; пометка при паузе
5. ✅ Модификаторы SS/US/RUS (RUS=F6-импульс), алиасы SHIFT/CTRL, PS(RALT)
6. ✅ 4 MCP-инструмента + `getKeyParam`
7. ✅ Счётчик инструментов 61 → 65
8. ✅ SKILL.md и vector06c-analyst.md: инвентарь + категория «Клавиатура»
9. ✅ `keyboard.md`: раздел «Key Injection via MCP Debugger»
10. ✅ `validate_agent_knowledge.py`: `VALID_TOOLS` += 4
11. ✅ `src/` не изменён (переиспользован существующий путь ввода)

**Файлы:**

* `debugger/agent/agent_types.h` — структура `KeyboardKeyInfo`
* `debugger/agent/agent_api.h` — декларации 4 методов
* `debugger/agent/agent_api.cpp` — таблица `kKeyTable` (70) + реализация методов (+`<cctype>`)
* `debugger/mcp/mcp_adapter.h` — декларация `registerKeyboardTools()`
* `debugger/mcp/mcp_adapter.cpp` — `registerKeyboardTools()` (4 инструмента) + `getKeyParam()` + вызов в `registerAllTools()`
* `debugger/mcp/tests/test_mcp_protocol.cpp` — счётчик 61→65, новые имена в ожидаемом списке
* `debugger/agent/knowledge/vector06c/keyboard.md` — раздел «Key Injection via MCP Debugger»
* `debugger/agent/tests/validate_agent_knowledge.py` — `VALID_TOOLS` += 4
* `.qoder/skills/vector06c-debugger/SKILL.md` — инвентарь 65 + категория «Клавиатура»
* `.qoder/agents/vector06c-analyst.md` — инвентарь 65 + строка «Клавиатура»

**Тесты / Verification:**

* `test_mcp_protocol` — 50/50 (ожидает 65 инструментов)
* `test_agent_contract` — 49/49
* живой stdio `v06c-mcp`: `list_keys`→70, `type_key "a"`→success, `press_key "nope"`→`unknown key`
* `validate_agent_knowledge.py` — PASSED (0 errors / 0 warnings)
* MCP tools: **65 registered** (было 61)
* `src/` changes: 0

**Известное не-относящееся:** `test_agent_api` падает на
`removeRdbLink — no source object fails` (`std::bad_optional_access`) —
pre-existing дефект, воспроизводится без изменений этого Stage (подтверждено
`git stash`).

**Готово к коммиту.**
