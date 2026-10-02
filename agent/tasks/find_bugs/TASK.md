# Find Bugs

## ID

find_bugs

## Description

Общий статический и динамический аудит ROM: поиск ошибок доступа к памяти, проблем стека, некорректного control flow, подозрительных переходов, unreachable code, пересечений код/данные, ошибок I/O и VRAM.

## Required Knowledge

- vector06c/cpu
- vector06c/memory
- vector06c/video
- vector06c/io
- vector06c/rom_format
- vector06c/verification

## Workflow Reference

- AI_AGENT_WORKFLOW.md

## Required Tools

- debug_disassemble
- debug_read_memory
- debug_get_cpu_state
- debug_get_symbols
- debug_get_function
- debug_get_xrefs
- debug_get_call_graph
- debug_get_memory_map
- debug_get_vram_info
- debug_get_io_trace
- debug_get_execution_trace
- debug_get_stack

## Procedure

1. **Подготовка**
   - Загрузить ROM, получить начальные символы.
   - Определить границы кода и данных через `debug_get_memory_map`.
   - Получить информацию о VRAM через `debug_get_vram_info`.

2. **Проверка memory access**
   - Найти обращения к зарезервированным областям (boot ROM 0x0000–0x07FF после загрузки).
   - Найти обращения за пределы физической памяти (> 0xFFFF).
   - Проверить VRAM-доступы: запись в VRAM вне видимого режима.

3. **Проверка stack integrity**
   - Применить методику из `stack_safety` task.
   - Проверить PUSH/POP баланс, CALL/RET парность.
   - Проверить потенциальный stack overflow.

4. **Проверка control flow**
   - Найти JMP/CALL в середину инструкции (не на начало).
   - Найти JMP/CALL на данные (если область помечена как данные).
   - Найти unreachable code — области, недостижимые из известных точек входа.
   - Проверить условные переходы: нет ли зацикливаний с пустым телом.

5. **Проверка I/O**
   - Найти обращения к неизвестным/незарезервированным портам.
   - Проверить последовательности доступа к AY (адрес → данные).
   - Проверить обращения к FDC без инициализации.
   - Сравнить с известными портами из `io.md`.

6. **Проверка VRAM access**
   - Проверить, соответствует ли режим видео (256/512) способу обращения к VRAM.
   - Проверить палитные записи (порты 0x0C–0x0F).
   - Проверить scroll-значение (порт 0x03).

7. **Пересечения код/данные**
   - Найти области, которые одновременно достижимы как код и как данные.
   - Проверить самомодификацию (запись в область, содержащую JMP/CALL targets).

8. **Динамический анализ (если доступно)**
   - Использовать `debug_get_execution_trace` для проверки реальных путей выполнения.
   - Использовать `debug_get_io_trace` для проверки реальных I/O-операций.

## Checks

- Все ли JMP/CALL target находятся в допустимых областях?
- Нет ли обращений к unmapped I/O портам?
- Нет ли пересечений код/данные?
- Нет ли unreachable code?
- VRAM-доступы соответствуют текущему видеорежиму?
- Стек не выходит за допустимые пределы?

## Output

### Summary

Краткий вывод: общее количество findings, распределение по severity.

### Findings

| Address | Type | Severity | Confidence | Description |
|---------|------|----------|------------|-------------|

Severity levels:
- **critical**: подтверждённая ошибка (invalid memory access, stack corruption)
- **probable**: вероятная ошибка (suspicious jump, unreachable code)
- **suspicious**: подозрительный паттерн, требующий дополнительной проверки

Confidence levels:
- **confirmed**: подтверждено динамическим анализом или несколькими источниками
- **probable**: подтверждено статическим анализом, но не проверено динамически
- **suspicious**: основано на эвристике, требует ручной проверки

### Evidence

Для каждого finding:
- address и дизассемблерный вывод;
- relevant memory/I/O observations;
- обоснование классификации severity;
- источник (статический/динамический анализ).

### Statistics

| Category | Count |
|----------|-------|
| Invalid memory access | ? |
| Stack issues | ? |
| Control flow anomalies | ? |
| I/O anomalies | ? |
| VRAM access errors | ? |
| Code/data overlaps | ? |
| Unreachable code | ? |

## Limitations

- Статический анализ не может обнаружить все ошибки — только те, что видны из кода.
- Динамический анализ покрывает только выполненные пути.
- Подозрительный участок не обязательно является ошибкой.
- Самомодифицирующийся код может выглядеть как ошибка.
- Некоторые паттерны (например, вычисляемые JMP) не анализируются статически.
