# Stage 6.4.1 — MCP Runtime Integration Fixes: План реализации

## Контекст

Stage 6.4 (MCP Adapter) реализован в коммитах `2e2755a`, `c1c76d9`. Выявлены три технических недостатка:

1. `v06c-mcp` использует `NoBoardTarget` + HAL-заглушки вместо реального `DebugAdapter`/`Board`
2. Ошибки MCP возвращаются с `isError` внутри `content[]`, а не на уровне `CallToolResult`
3. JSON schemas инструментов не отражают numeric limits

**Цель:** Исправить недостатки без добавления новой функциональности.

---

## Архитектурные ограничения

- НЕ добавлять Debugger Hooks
- НЕ изменять Agent API
- НЕ добавлять новую функциональность
- НЕ изменять `src/` без крайней необходимости
- НЕ создавать новый emulator initialization framework
- Использовать существующие headless/CLI механизмы, если они есть

---

## Фазы реализации

### Фаза 1: Исследование архитектуры (1-2 часа)

**Цель:** Понять, как реальный debugger создаёт `Board` и `DebugAdapter`.

1. Изучить `debugger/gui/main.cpp` — точка входа GUI-отладчика
2. Изучить `debugger/src/board.h/cpp` — конструктор Board, зависимости
3. Изучить `debugger/agent/debug_adapter.h/cpp` — DebugAdapter, как оборачивает Board
4. Проверить, есть ли headless/CLI режим запуска (без SDL/ImGui)
5. Проверить, какие HAL-функции реально нужны для Board
6. Документировать минимальный набор для headless Board

**Результат:** Понимание цепочки инициализации, список необходимых HAL-заглушек.

---

### Фаза 2: Headless Board для MCP (3-4 часа)

**Цель:** Создать headless runtime с реальным `Board` для `v06c-mcp`.

1. Удалить `NoBoardTarget` из `mcp_main.cpp`
2. Реализовать минимальные HAL-заглушки только для реального Board:
   - SDL video stubs (если Board требует)
   - SDL audio stubs (если Board требует)
   - File I/O (реальный, не stub)
3. Создать `Board` в headless режиме
4. Создать `DebugAdapter` поверх Board
5. Собрать цепочку: `Board → DebugAdapter → DebugBackend → AgentApi → McpServer`
6. Удалить заглушечные HAL-функции из `mcp_main.cpp`, если они больше не нужны

**Файлы:**
- `debugger/mcp/mcp_main.cpp` — переписать инициализацию
- Возможно: `debugger/mcp/mcp_headless_hal.cpp` — минимальные HAL-заглушки

**Критерий:** `v06c-mcp` использует реальный `DebugAdapter`, `NoBoardTarget` не используется.

---

### Фаза 3: ROM loading verification (1-2 часа)

**Цель:** Проверить полный путь загрузки ROM через MCP.

1. Проверить `AgentApi::loadRom()` — работает ли с реальным Board
2. Проверить `DebugBackend::loadRom()` — делегирует ли DebugAdapter
3. Проверить `DebugAdapter::loadRom()` — загружает ли в Board
4. Написать integration test:
   - Загрузить ROM через MCP (`debug_load_rom`)
   - Выполнить `debug_get_cpu_state` — проверить реальное состояние
   - Выполнить `debug_read_memory` — проверить реальную память
   - Выполнить `debug_disassemble` — проверить реальный дизассемблер
   - Выполнить `debug_step` — проверить реальное выполнение

**Файлы:**
- `debugger/mcp/tests/test_mcp_real_runtime.cpp` — integration test с реальным Board

**Критерий:** ROM загружается, CPU state/память/dizassembly/step работают на реальном Vector.

---

### Фаза 4: MCP error response fix (1-2 часа)

**Цель:** Исправить формирование `CallToolResult` для ошибок.

**Текущее поведение:**
```json
{
  "content": [
    {
      "type": "text",
      "text": "...",
      "isError": true  // ← внутри content item
    }
  ]
}
```

**Требуемое поведение:**
```json
{
  "content": [
    {
      "type": "text",
      "text": "..."
    }
  ],
  "isError": true  // ← на уровне CallToolResult
}
```

1. Изучить `cpp-mcp` — как возвращать `CallToolResult` с `isError` на верхнем уровне
2. Исправить `errorContent()` в `mcp_adapter.cpp` — возвращать структуру с `isError` на уровне результата
3. Проверить все 38 handlers — все должны возвращать корректный формат

**Файлы:**
- `debugger/mcp/mcp_adapter.cpp` — исправить `errorContent()`, возможно `textContent()`

**Критерий:** Ошибки возвращаются с `isError: true` на уровне `CallToolResult`.

---

### Фаза 5: Wire-level error tests (1 час)

**Цель:** Добавить тесты для wire-level MCP error responses.

1. Добавить тест `test_mcp_error_wire_level.cpp` или расширить `test_mcp_protocol.cpp`:
   - Вызвать tool с невалидным адресом → проверить `result.isError == true`
   - Проверить, что `content[0].text` содержит `error_code` и `message`
   - Проверить, что `content[0]` НЕ содержит `isError` (оно на верхнем уровне)
2. Проверить реальный путь: `AgentApi error → MCP tools/call → CallToolResult`

**Файлы:**
- `debugger/mcp/tests/test_mcp_protocol.cpp` — добавить wire-level tests

**Критерий:** Тесты проверяют `result.isError`, а не внутренний helper.

---

### Фаза 6: JSON Schemas improvement (1-2 часа)

**Цель:** Добавить numeric limits в schemas инструментов.

1. Проверить все 38 tools — какие параметры имеют известные диапазоны:
   - `address`: `minimum: 0, maximum: 65535` (uint16_t)
   - `port`: `minimum: 0, maximum: 255` (uint8_t)
   - `size`, `count`: `minimum: 1` (если применимо)
   - `value` (для write): `minimum: 0, maximum: 255` или `65535`
2. Обновить `tool_builder` вызовы в `mcp_adapter.cpp`:
   - Использовать `.property()` с `minimum`, `maximum` (если поддерживается cpp-mcp)
   - Если не поддерживается — добавить в description
3. Проверить, что cpp-mcp поддерживает numeric constraints в schema

**Файлы:**
- `debugger/mcp/mcp_adapter.cpp` — обновить schemas

**Критерий:** Schemas отражают известные numeric limits.

---

### Фаза 7: Audit всех 38 tools (2-3 часа)

**Цель:** Проверить каждый tool на корректность.

Для каждого из 38 tools:
1. Имя — начинается с `debug_`
2. Description — понятное, на английском
3. Input schema — корректные типы, numeric limits
4. Required parameters — все обязательные параметры marked as required
5. Отсутствие Vector-specific типов (только примитивы: integer, string, array)
6. Отсутствие лишних параметров
7. Возвращаемое значение — корректный JSON

**Метод:**
- Написать тест или скрипт, который выводит `tools/list` и проверяет все tools
- Или ручной audit с чек-листом

**Файлы:**
- `debugger/mcp/tests/test_mcp_tool_audit.cpp` — audit test (опционально)

**Критерий:** Все 38 tools проходят audit.

---

### Фаза 8: Real stdio smoke test (2-3 часа)

**Цель:** Проверить `v06c-mcp` executable через реальный stdin/stdout.

1. Написать скрипт или test, который:
   - Запускает `v06c-mcp` как subprocess
   - Отправляет `initialize` через stdin
   - Отправляет `tools/list` — проверяет 38 tools
   - Отправляет `tools/call debug_load_rom` с реальным ROM fixture
   - Отправляет `tools/call debug_get_cpu_state` — проверяет реальное состояние
   - Отправляет `tools/call debug_read_memory` — проверяет реальную память
   - Отправляет `tools/call debug_disassemble` — проверяет реальный дизассемблер
2. Использовать существующий ROM fixture из `testroms/` (например, `clrs.rom`)
3. Проверить полный цикл: load → state → read → disassemble → step

**Файлы:**
- `debugger/mcp/tests/test_mcp_stdio_smoke.cpp` — stdio smoke test
- Или shell script: `debugger/mcp/tests/mcp_stdio_smoke.sh`

**Критерий:** Реальный `v06c-mcp` работает через stdio с реальным Board.

---

### Фаза 9: CMake verification (30 мин)

**Цель:** Проверить, что зависимости MCP изолированы.

1. Проверить `debugger_core` — НЕ зависит от:
   - MCP (cpp_mcp, debugger_mcp)
   - SDL
   - ImGui
   - OpenGL
2. Проверить `v06c-mcp` — зависит от:
   - debugger_core
   - debugger_mcp
   - cpp_mcp
   - Board (реальный, не stub)
3. Проверить, что `debugger_core` по-прежнему собирается без MCP

**Команды:**
```bash
cd debugger/build
cmake ..
make debugger_core  # Должно собираться без MCP
make v06c-mcp       # Должно собираться с MCP
```

**Критерий:** Зависимости изолированы, `debugger_core` не знает про MCP.

---

### Фаза 10: Regression testing (1 час)

**Цель:** Убедиться, что все тесты проходят.

```bash
cd debugger/build
./test_agent_api        # 77 tests
./test_agent_commands   # 15 tests
./test_agent_contract   # 49 tests
./test_agent_integration # 46 tests
./test_mcp_protocol     # 37+ tests (с новыми wire-level tests)
./test_mcp_real_runtime # новый integration test
./test_mcp_stdio_smoke  # новый stdio smoke test
```

**Критерий:** Все тесты PASS, нет новых регрессий.

---

### Фаза 11: Documentation update (30 мин)

**Цель:** Обновить документацию.

1. Обновить `debugger/mcp/README.md`:
   - Указать, что используется реальный `DebugAdapter`/`Board`
   - Добавить пример загрузки ROM через MCP
   - Обновить архитектуру
2. Обновить `debugger/README.md` (если изменились зависимости или запуск)
3. Создать `debugger/docs/Pipeline/Stage_6.4.1_Report.md` — отчёт о выполнении

**Критерий:** Документация отражает текущее состояние.

---

## Чек-лист завершения Stage 6.4.1

- [ ] `v06c-mcp` использует реальный `DebugAdapter`
- [ ] `NoBoardTarget` не используется production MCP runtime
- [ ] HAL-заглушки MCP runtime удалены (или минимизированы для headless Board)
- [ ] MCP → AgentApi → DebugBackend → DebugAdapter → Board работает
- [ ] ROM можно загрузить через MCP
- [ ] После загрузки можно получить реальное CPU state
- [ ] Можно читать реальную память
- [ ] Можно выполнять disassembly
- [ ] MCP errors имеют корректный `CallToolResult.isError`
- [ ] `tools/list` содержит корректные schemas
- [ ] Известные numeric limits отражены в schemas
- [ ] Есть wire-level error test
- [ ] Есть реальный stdio smoke/integration test
- [ ] `debugger_core` не получил MCP-зависимость
- [ ] Agent API не изменён без необходимости
- [ ] Debugger Hooks не добавлялись
- [ ] Новая функциональность не добавлялась
- [ ] Все существующие тесты проходят

---

## Оценка времени

| Фаза | Описание | Время |
|------|----------|-------|
| 1 | Исследование архитектуры | 1-2 ч |
| 2 | Headless Board для MCP | 3-4 ч |
| 3 | ROM loading verification | 1-2 ч |
| 4 | MCP error response fix | 1-2 ч |
| 5 | Wire-level error tests | 1 ч |
| 6 | JSON Schemas improvement | 1-2 ч |
| 7 | Audit всех 38 tools | 2-3 ч |
| 8 | Real stdio smoke test | 2-3 ч |
| 9 | CMake verification | 30 мин |
| 10 | Regression testing | 1 ч |
| 11 | Documentation update | 30 мин |
| **Итого** | | **16-22 ч** |

---

## Риски

1. **Board требует SDL** — может потребоваться больше HAL-заглушек, чем ожидалось
2. **Headless Board не поддерживается** — если нет существующего механизма, придётся создавать
3. **cpp-mcp не поддерживает numeric constraints** — придётся добавлять в description
4. **DebugAdapter не работает без GUI** — может потребоваться доработка

---

## Коммиты

Планируется 2-3 коммита:

1. `Stage 6.4.1: Replace NoBoardTarget with real DebugAdapter/Board`
2. `Stage 6.4.1: Fix MCP error response (CallToolResult.isError)`
3. `Stage 6.4.1: Improve JSON schemas and add wire-level tests`

Или один коммит, если изменения минимальны.
