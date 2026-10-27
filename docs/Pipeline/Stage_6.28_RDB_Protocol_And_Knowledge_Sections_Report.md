# Stage 6.28 — Отчёт о внедрении: RDB разделы знания (Protocol / Ports / Watch-list)

*Отчёт по итогам реализации `Stage_6.28_RDB_Protocol_And_Knowledge_Sections.md`.
Три новых верхнеуровневых раздела добавлены в RDB, формат поднят с `version: 1` до `version: 2`,
цепочка RDB Controller → Agent API → MCP раскрыта насквозь, добавлены тесты и документация.*

---

## 0. Резюме

| Слой | Что добавлено | Статус |
|------|---------------|:------:|
| Типы (`rdb/rdb_types.h`) | `RdbOpResult`, enums статуса/scope/method/направления/полярности + парсеры, `RdbProtocolEntry`, `RdbPortBit`/`RdbPortEntry`, `RdbWatchEntry`, `ProtocolFilter` | ✅ |
| Хранилище (`rdb/rdb_controller.*`) | 3 раздела, JSON-сериализация, версионирование v1↔v2, 14 методов API | ✅ |
| Agent API (`agent/*`) | 10 методов (тонкое отображение), result-структуры, лимиты, маппинг ошибок | ✅ |
| MCP (`mcp/*`) | 10 инструментов, JSON-конвертеры, `api_version` 5 → 6 | ✅ |
| Тесты | unit (Controller) + round-trip/MCP-цепочка; регрессия agent | ✅ |
| Docs | `rdb_format.md` (v2), `AI_AGENT_WORKFLOW.md` (§11 регламент) | ✅ |

Итоговые прогоны: **63/63** `test_rdb_controller`, **66/66** `test_mcp_protocol`,
**119/119** `test_agent_api`, **49/49** `test_agent_contract`, **15/15** `test_agent_commands`,
`validate_agent_knowledge.py` — **PASSED** (0 errors, 0 warnings). `git diff -- src/` — пусто.

---

## 1. Модель данных (§4–§6)

### 1.1. `protocol` — журнал гипотез (append-only)

Запись `RdbProtocolEntry`:

```text
id            "P-NNNN", присваивает контроллер, монотонный; счётчик возобновляется после load
timestamp     ISO-8601 UTC, проставляет контроллер (gmtime)
thesis        tezис одной строкой, normalizeRdbText; пустой после нормализации → InvalidThesis
status        закрытый enum (8 значений)
scope         rom / hardware / tooling (по умолчанию rom)
address       опциональная привязка (hasAddress + uint16)
method        опциональный enum; обязателен для «твёрдых» статусов
evidence      улика, normalizeRdbText; обязателна для «твёрдых» статусов
links         id-связи + отметка промоции (напр. "port:0x01")
supersedes    какую запись замещает эта
supersededBy  какая запись замещает эту (ставит контроллер)
```

**Закрытые словари** (заимствованы из `verification.md`, §4.3):

| status | method | scope |
|--------|--------|-------|
| `VERIFIED_BY_TRACE` / `VERIFIED_BY_SOURCE` / `VERIFIED` / `PARTIALLY_VERIFIED` / `CONFLICT` / `UNVERIFIED` / `REFUTED` / `SPECULATION` | `trace` / `diff` / `primary-source` / `golden-test` / `static` | `rom` / `hardware` / `tooling` |

Парсеры регистронезависимы (`status` — UPPER, `scope`/`method` — lower); неизвестное значение →
соответствующий `RdbOpResult::Invalid*` → в Agent API — `ErrorCode::InvalidArgument`. Свободнотекстовый
статус запрещён.

### 1.2. Правило «нет улик — нет подтверждения» (§4.4)

`protocolStatusRequiresEvidence()` возвращает `true` для `VERIFIED_BY_TRACE`,
`VERIFIED_BY_SOURCE`, `VERIFIED`, `PARTIALLY_VERIFIED`, `CONFLICT`, `REFUTED`.
Контроллер (`validateProtocolEvidence`) отклоняет такую запись без валидного `method`
**и** непустого `evidence` → `RdbOpResult::MissingEvidence`. Проверка выполняется и на
`addProtocolEntry`, и на `updateProtocolStatus`, и на `supersedeProtocolEntry`.

### 1.3. Append-only и supersede (§4.6)

`supersedeProtocolEntry(oldId, newEntry, outNewId)`: новая запись получает
`supersedes = oldId`, у старой ставится `supersededBy = newId` (старая становится неактивной).
История не переписывается. `removeProtocolEntry(id)` отказывает (`RdbOpResult::Referenced`),
если какая-то запись всё ещё ссылается на `id` через `supersedes`.

> Исправленный баг: указатель `RdbProtocolEntry*` в векторе инвалидировался `push_back`
> внутри `addProtocolEntry` (UB). Переписано на индексный доступ `targetIdx`.

### 1.4. `ports` — таблица портов (§5)

`RdbPortEntry { port(uint8), name, direction, polarity, bits[], comment }`,
`RdbPortBit { index(0..7), meaning }`. Уникальность по номеру порта (`std::map<uint8_t,…>`).
`upsertPort` идемпотентен: сравнение через `portToJson` — идентичная строка не поднимает `dirty`.
Закрытые enum-ы `direction` (input/output/in-out) и `polarity` (active-high/active-low/n/a).
`bits` ≤ 8 (`AgentLimits::MAX_PORT_BITS`), `index` вне 0..7 → InvalidArgument.

### 1.5. `watchlist` — наблюдаемые переменные (§6)

`RdbWatchEntry { name, address, length(≥1), format(uint8… по умолчанию), hasObjectAddress, objectAddress }`.
Ключ — `name` (`std::map<string,…>`): повторное добавление с тем же именем обновляет содержимое,
дубликатов не плодит; идентичное значение не поднимает `dirty`. `listWatch()` — по возрастанию адреса.

---

## 2. Формат и версионирование (§3)

* Формат поднят `1 → 2`. Отличия v2 — **только** необязательные разделы `protocol`/`ports`/`watchlist`;
  структура `objects` не тронута.
* `rdbToJson`: `version: 2` пишется, только если хотя бы один новый раздел непуст
  (`hasKnowledge`), иначе сохраняется исходная версия (молча не «апгрейджим» непричастный файл).
* `rdbFromJson`: грузит `version <= 2`; `version > 2` → `return false` (отказ загрузки).
  Для v1/отсутствующих разделов — читаются пустыми, `dirty` не выставляется.
* Счётчик `nextProtocolId` восстанавливается из максимального `P-NNNN` при загрузке, id не переиспользуются.
* Даты: `nowIso8601()` (UTC, `char[64]` — исправлено усечение формата).
* Атомарный save (`.tmp` + rename) и `normalizeRdbText` применены к новым текстовым полям.

---

## 3. API: Controller → Agent API → MCP (§8)

### 3.1. RDB Controller (14 методов)

```text
Protocol: addProtocolEntry · supersedeProtocolEntry · updateProtocolStatus
          removeProtocolEntry · findProtocolEntry · getProtocol(filter) · protocolCount
Ports:    upsertPort · getPort · listPorts · portCount
Watch:    addWatchEntry · removeWatchEntry · listWatch · watchCount
```

Изменяющие операции поднимают `dirty` только при реальном изменении; чтение — никогда.

### 3.2. Agent API (10 методов, §8.2 «тонкое отображение»)

`addProtocolEntry · supersedeProtocolEntry · getProtocol · updateProtocolStatus ·
removeProtocolEntry · upsertPort · getPorts · addWatchEntry · removeWatchEntry · listWatch`.

* Строковые enum-ы парсятся на входе; неизвестное значение → `InvalidArgument` до контроллера.
* `RdbOpResult → ErrorCode` через `mapRdbOpError` (NotFound, InvalidArgument для всех Invalid/Missing/
  Referenced/Duplicate, OperationFailed как fallback).
* Конвертеры `protocolEntryToResult` / `portEntryToResult` / `watchEntryToResult`; текстовые поля
  прогоняются через `normalizeRdbText` (R2: никогда не выпускать перенос строки в вывод инструмента).
* `getProtocol` ограничивает выдачу: `maxEntries` (0 → `MAX_PROTOCOL_ENTRIES_DEFAULT=200`,
  жёсткий потолок `MAX_PROTOCOL_ENTRIES_HARD=10000`).
* `getRdbInfo` дополнен полями `protocol_count` / `port_count` / `watch_count`.

### 3.3. MCP — 10 инструментов (§8.3)

```text
debug_add_protocol_entry      debug_supersede_protocol_entry   debug_get_protocol
debug_update_protocol_status  debug_remove_protocol_entry
debug_upsert_port             debug_get_ports
debug_add_watch_entry         debug_remove_watch_entry         debug_list_watch
```

`mcp_json`: `protocolEntryToJson(s)` / `portEntryToJson` / `portsToJson` / `watchEntryToJson` /
`watchListToJson`; у protocol-записи добавлен convenience-флаг `active` (= `supersededBy` пуст).
Адреса — hex (`0xHHHH`/`0xHH`). `registerKnowledgeTools()` вызывается из `registerAllTools()`.

**Capability:** `v06c.api_version` поднят `5 → 6` (в `set_capabilities` и в выводе
`debug_get_server_info`). Обновлены регрессионные ожидания числа инструментов (76 → 86) и apiVersion (5 → 6).

---

## 4. Конвейер промоции (§9)

Реализован как **соглашение + данные**, не отдельный механизм: промотированная запись protocol
сохраняется и помечается ссылкой в `links` (напр. `"port:0x01"`) на промотированную строку/объект.
Единственный легальный путь «черновик → таблица»: знание о полярности попадает в `ports` только
из **подтверждённой** записи журнала. Промоция не удаляет protocol-запись (история «когда и чем
доказано» сохраняется). Исключение — первичный bulk-import канонической таблицы портов оформляется
как одна `VERIFIED_BY_SOURCE` запись-основание.

Тест `promotion_protocol_to_port` проверяет сквозной сценарий: VERIFIED_BY_TRACE → promo-ссылка на порт
→ supersede → save/load → исходная запись неактивна, промо-запись активна со своим `links` и статусом,
порт на месте.

---

## 5. ROM identity / mismatch (§7)

Инфраструктура уже была: `RdbController::matchesRom(...)` и `rom_sha256` в `debug_get_rdb_info`.
Stage не добавляет нового гейта в коде — это правило регламента: при несовпадении `sha256`
агент обязан перепроверять `scope: rom`-тезисы; `scope: hardware`/`tooling` от `sha256` не зависят.
Пункт зафиксирован в `AI_AGENT_WORKFLOW.md` (§11, шаг 3) и `rdb_format.md`.

---

## 6. Тестирование (§12)

### Unit — `test_rdb_controller.cpp` (новые тесты)

```text
protocol_add_assigns_id_and_timestamp        protocol_requires_evidence
protocol_speculation_no_evidence             protocol_empty_thesis_rejected
protocol_supersede_history_preserved         protocol_get_filter_substring_and_status
protocol_update_status_validates             ports_upsert_and_list_sorted
ports_upsert_idempotent_no_dirty             watch_add_update_remove
version_bumps_to_2_with_knowledge            version_stays_1_without_knowledge
knowledge_roundtrip_persistence              load_rejects_version_3
parse_helpers_reject_unknown                 promotion_protocol_to_port
```

### Интеграция / MCP-цепочка — `test_mcp_protocol.cpp` (реальный Agent API → Controller)

```text
test_protocol_lifecycle          test_protocol_evidence_rule
test_protocol_supersede          test_ports_upsert_get
test_watch_lifecycle
```

Сценарии L1 (upsert active-low порта + round-trip) и L2 (VERIFIED_BY_TRACE → supersede REFUTED
с уликой, `getProtocol(active_only)` не показывает замещённую) покрыты; удаление
referenced-записи отказывает.

### Регрессия

`test_agent_api` 119/119, `test_agent_contract` 49/49, `test_agent_commands` 15/15,
существующие RDB/Links тесты — зелёные, `version:1` грузится. `validate_agent_knowledge.py` — PASSED.

---

## 7. Документация (§10)

* `agent/knowledge/rdb_format.md` — версия поднята до 2, добавлен блок **Knowledge Sections (v2)**
  с описанием полей/enum-ов/правил (evidence rule, append-only/supersede, mismatch, промоция),
  обновлена таблица MCP-инструментов, правило «не поднимать dirty на чтении».
* `agent/AI_AGENT_WORKFLOW.md` — добавлен раздел **Knowledge Sections (Stage 6.28)** с регламентом
  (§11: старт анализа / проверка гипотезы / промоция / сохранение) и таблицей новых инструментов.
* `mcp/README.md` — в описании `v06c.api_version` добавлен уровень 6.

---

## 8. Чего НЕ делал (§13) — проверено

* Дизассемблер/экспортёр не тронуты (`git diff -- src/` пуст); генерация автокомментариев P5 — отдельный этап,
  здесь только данные-источник в `ports`.
* Печать watch-list в брейкпоинт-лог не реализована (backend-работа) — зафиксировано только место хранения.
* Семантика анализа ROM в MCP не кладётся — сервер даёт данные, не принимает решений.
* `protocol` **не** сделан как `RdbObjectType` — это отдельный верхнеуровневый раздел (без жёсткой привязки к address).
* RAG / embeddings / vector DB / LLM-runtime не добавлены.
* `.rdb` не создаётся как побочный продукт: при `dirty=false` файл не появляется.

---

## 9. Критерий завершения (§14) — чек-лист

| Критерий | Статус |
|----------|:------:|
| Разделы `protocol`/`ports`/`watchlist` в схеме; `version` = 2 | ✅ |
| `version: 1` грузится обратно-совместимо; `>2` отклоняется | ✅ |
| Закрытые enum-ы; неизвестное значение → ошибка | ✅ |
| VERIFIED*/REFUTED невозможны без method+evidence | ✅ |
| append-only + supersede, история не переписывается | ✅ |
| дедуп-фильтры `getProtocol` (substring/status/scope/address/active_only) | ✅ |
| mismatch sha256 не применяет `scope: rom` молча (через matchesRom + регламент) | ✅ |
| Controller → Agent API → MCP real (не mock); `tools/list` показывает new; `api_version`=6 | ✅ |
| промоция protocol → ports/comment описана и имеет тест | ✅ |
| `rdb_format.md` и `AI_AGENT_WORKFLOW.md` обновлены | ✅ |
| unit/integration/regression зелёные | ✅ |
| `git diff -- src/` пуст | ✅ |
| ничего из §13 не добавлено | ✅ |

---

## 10. Затронутые файлы

| Файл | Изменение |
|------|-----------|
| `rdb/rdb_types.h` | типы/enum-ы/парсеры/структуры трёх разделов |
| `rdb/rdb_controller.h` / `.cpp` | разделы, JSON, версионирование, 14 методов |
| `agent/agent_types.h` | result-структуры, лимиты, поля счётчиков |
| `agent/agent_api.h` / `.cpp` | 10 методов, конвертеры, маппинг ошибок, getRdbInfo |
| `mcp/mcp_json.h` / `.cpp` | JSON-конвертеры knowledge-типов, счётчики в rdbInfo |
| `mcp/mcp_adapter.h` / `.cpp` | `registerKnowledgeTools()`, api_version 6 |
| `mcp/tests/test_mcp_protocol.cpp` | +5 интеграционных тестов, счётчик инструментов 86, apiVersion 6 |
| `tests/test_rdb_controller.cpp` | +16 unit/round-trip тестов (вкл. promotion) |
| `agent/knowledge/rdb_format.md`, `agent/AI_AGENT_WORKFLOW.md`, `mcp/README.md` | документация |

---

## 11. Как проверить локально

```bash
cmake --build build --target v06c-mcp test_rdb_controller test_mcp_protocol \
     test_agent_api test_agent_contract test_agent_commands -j4
./build/test_rdb_controller        # 63 passed
./build/test_mcp_protocol          # 66/66 passed
python3 agent/tests/validate_agent_knowledge.py   # PASSED
git diff -- src/                   # пусто
```

**Главный принцип Stage 6.28 реализован:**

```text
protocol  = журнал доказательств (append-only, без улики нет статуса)
ports     = скомпилированное знание об аппаратуре (источник автокомментариев P5)
watchlist = что печатать при остановке
промоция  = единственный легальный путь из «черновика» в «таблицу»
JSON      = внутренний формат; API = единственный способ работы
```
