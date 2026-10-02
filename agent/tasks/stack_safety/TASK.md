# Stack Safety

## ID

stack_safety

## Description

Проверка сохранности и корректности использования стека ROM: анализ SP, PUSH/POP баланса, CALL/RET парности, потенциального выхода стека за допустимую область.

## Required Knowledge

- vector06c/cpu
- vector06c/memory
- vector06c/rom_format
- vector06c/verification

## Workflow Reference

- AI_AGENT_WORKFLOW.md

## Required Tools

- debug_get_cpu_state
- debug_read_memory
- debug_disassemble
- debug_get_symbols
- debug_get_function
- debug_get_xrefs
- debug_get_stack
- debug_get_execution_trace

## Procedure

1. **Определение области стека**
   - Получить текущий SP через `debug_get_cpu_state`.
   - Прочитать память вокруг SP через `debug_read_memory`.
   - Определить предполагаемую область стека (обычно растёт вниз от начального SP ≈ 0xC300).

2. **Статический анализ стековых операций**
   - Для каждой функции (через `debug_get_symbols` + `debug_disassemble`):
     - Подсчитать баланс PUSH/POP.
     - Проверить, что каждая CALL имеет соответствующий RET.
     - Проверить, что каждый PUSH имеет соответствующий POP.
   - Обратить внимание на условные RET (RET cc) — они могут нарушить баланс.

3. **Анализ SP-манипуляций**
   - Найти все SPHL, LXI SP, инструкции, изменяющие SP напрямую.
   - Проверить, что после SPHL значение SP указывает на допустимую область.
   - Проверить XTHL — обмен HL с вершиной стека.

4. **RST анализ**
   - Проверить все RST n инструкции — каждая добавляет 2 байта в стек.
   - Убедиться, что обработчики RST корректно возвращаются (RET в конце).

5. **Динамическая проверка (если доступно)**
   - Использовать `debug_get_execution_trace` для анализа реальных SP-значений.
   - Определить минимальный/максимальный SP во время выполнения.
   - Сравнить с предполагаемой областью стека.

6. **Проверка пересечений**
   - Проверить, не пересекается ли стек с Screen RAM (0x8000–0xFFFF).
   - Проверить, не пересекается ли стек с данными.

## Checks

- Баланс PUSH/POP = 0 для каждой функции?
- Каждая CALL имеет парный RET?
- SP не выходит за пределы RAM?
- SP не залезает в Screen RAM (0x8000–0xFFFF)?
- Нет ли некорректных XTHL (обмен с неинициализированной памятью)?
- RST-обработчики завершаются RET?

## Output

### Summary

Краткий вывод о состоянии стека.

### Stack Region

| Parameter | Value |
|-----------|-------|
| Initial SP | 0x???? |
| Min SP observed | 0x???? |
| Max SP observed | 0x???? |
| Stack direction | Down |
| Potential overflow | Yes/No |

### Function Stack Analysis

| Function | PUSH count | POP count | Balance | Issues |
|----------|-----------|-----------|---------|--------|

### Findings

| Address | Type | Severity | Confidence | Description |
|---------|------|----------|------------|-------------|

Severity: critical / warning / info

### Evidence

Для каждого finding:
- конкретные инструкции с адресами;
- значения SP до/после;
- трассировка выполнения (если доступна).

## Limitations

- Статический анализ не учитывает условные пути выполнения.
- Реальная глубина стека зависит от порядка вызовов.
- Рекурсивные вызовы могут исчерпать стек, даже если баланс PUSH/POP корректен.
- Без выполнения анализ ограничен потенциальными, а не подтверждёнными проблемами.
