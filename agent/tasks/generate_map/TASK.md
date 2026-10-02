# Generate Map

## ID

generate_map

## Description

Анализ ROM и создание/уточнение MAP-информации: определение функций, меток, областей данных и кода.

## Required Knowledge

- vector06c/cpu
- vector06c/memory
- vector06c/rom_format
- vector06c/verification

## Workflow Reference

- AI_AGENT_WORKFLOW.md

## Required Tools

- debug_disassemble
- debug_get_symbols
- debug_get_function
- debug_get_xrefs
- debug_get_call_graph
- debug_read_memory
- debug_get_memory_map
- debug_create_function
- debug_add_label
- debug_set_comment
- debug_rename_function

## Procedure

1. **Начальное сканирование**
   - Загрузить ROM (если ещё не загружен).
   - Получить текущие символы через `debug_get_symbols`.
   - Определить начальный адрес (обычно 0x0100 для .rom файлов).

2. **Рекурсивный обход кода**
   - Начиная с известных точек входа (вектор прерываний, начальный PC), дизассемблировать инструкции через `debug_disassemble`.
   - Для каждой инструкции CALL/JMP/RST — добавить целевой адрес как функцию через `debug_create_function`.
   - Для каждой инструкции CALL/JMP — добавить cross-reference через анализ `debug_get_xrefs`.

3. **Определение границ функций**
   - Функция начинается с CALL/JMP target или RST vector.
   - Функция заканчивается на RET, RET cc, или безусловном JMP.
   - Использовать `debug_get_call_graph` для определения caller/callee связей.

4. **Разделение код/данные**
   - Области, на которые ссылаются через LD/STA/LDA с константным адресом — пометить как данные.
   - Области, достижимые через последовательное выполнение — код.
   - Использовать `debug_get_memory_map` для определения текущих границ.

5. **Аннотации**
   - Добавить комментарии к ключевым адресам через `debug_set_comment`.
   - Переименовать функции через `debug_rename_function` при обнаружении известных паттернов (например, keyboard_scan, vram_write).
   - Добавить метки через `debug_add_label` для важных адресов данных.

6. **Итеративное уточнение**
   - Повторять обход до тех пор, как новые функции перестанут обнаруживаться.
   - Проверить `debug_get_symbols` — все ли символы имеют определённые границы.

## Checks

- Все ли точки входа обнаружены (RST vectors, interrupt handlers)?
- Нет ли функций без RET/JMP на конце?
- Нет ли пересечений между функциями?
- Все ли cross-references учтены?

## Output

### MAP File Format

При создании MAP-файла использовать формат z88dk (см. `rom_format.md` §MAP files):

```
symbol_name           = $ADDRESS ; type, visibility, def, module, section, source_location
```

MAP-файл сохраняется рядом с ROM в ROM Library.

### Summary

Краткий вывод: количество обнаруженных функций, меток, областей данных.

### Functions

| Address | Name | Size | Callers | Callees |
|---------|------|------|---------|---------|

### Data Regions

| Start | End | Referenced By | Description |
|-------|-----|--------------|-------------|

### Interrupt Vectors

| Vector | Handler Address | Description |
|--------|----------------|-------------|

### Evidence

Для каждого обнаруженного элемента:
- address;
- дизассемблерный вывод;
- cross-references;
- обоснование классификации (код/данные).

### Confidence

- high: функции с чёткими границами и подтверждёнными xrefs;
- medium: функции, обнаруженные через один уровень CALL;
- low: области, классифицированные как данные по косвенным признакам.

## Limitations

- Статический анализ не обнаруживает косвенные вызовы (через регистр HL, BC и т.д.).
- Данные, доступные только через вычисляемые адреса, могут быть ошибочно классифицированы как код.
- Самомодифицирующийся код не поддерживается статическим анализом.
- Размер функций определяется эвристически при отсутствии MAP-файла.
