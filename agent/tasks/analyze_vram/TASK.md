# Analyze VRAM

## ID

analyze_vram

## Description

Анализ видеопамяти Vector-06C: определение текущего видеорежима, содержимого VRAM, палитры, scroll-значения, border-цвета и графических паттернов.

## Required Knowledge

- vector06c/video
- vector06c/memory
- vector06c/io
- vector06c/verification

## Workflow Reference

- AI_AGENT_WORKFLOW.md

## Required Tools

- debug_get_vram_info
- debug_get_screen_info
- debug_get_memory_map
- debug_read_memory
- debug_read_io
- debug_get_execution_trace
- debug_get_cpu_state

## Procedure

1. **Определение видеорежима**
   - Прочитать `debug_get_screen_info` для получения текущего режима.
   - Прочитать PIA1 Port B (порт 0x02) через `debug_read_io` — бит 4 определяет режим 256/512.
   - Прочитать border color (Port B bits 0–3).

2. **Анализ VRAM layout**
   - Вызвать `debug_get_vram_info` для получения текущей карты VRAM.
   - Vector-06C uses 4 bit planes at 8000h / A000h / C000h / E000h.
   - Screen RAM occupies the upper 32 KB of 64 KB main RAM (8000h–FFFFh).
   - Determine which planes are active in the current mode.

3. **Чтение содержимого VRAM**
   - Прочитать содержимое битовых плоскостей через `debug_read_memory`:
     - Plane 0: 8000h–9FFFh, Plane 1: A000h–BFFFh
     - Plane 2: C000h–DFFFh, Plane 3: E000h–FFFFh
   - Определить заполненность (пустые/заполненные блоки).

4. **Анализ палитры**
   - Прочитать текущие палитные значения (порты 0x0C–0x0F).
   - Определить активные цвета.
   - Сравнить с жёлто-синей "yeblette" палитрой по умолчанию.

5. **Анализ scroll**
   - Прочитать Port A (порт 0x03) — текущее scroll-значение.
   - Определить, используется ли scroll (значение ≠ 0).

6. **Анализ графических паттернов**
   - В режиме 256 колонок: каждый байт = 8 пикселей, бит 7 = левый.
   - В режиме 512 колонок: каждый байт = 4 пикселя, биты 7,5,3,1.
   - Определить повторяющиеся паттерны (спрайты, тайлы, текст).
   - Определить текстовые области (паттерны 8×8 или 6×10).

7. **Динамический анализ (если доступно)**
   - Использовать `debug_get_execution_trace` для определения, какие области VRAM записываются.
   - Определить паттерны обновления VRAM (полный экран, частичное обновление, анимация).

## Checks

- VRAM-адреса соответствуют заявленному видеорежиму?
- Палитра корректна (есть видимые цвета)?
- Scroll-значение в допустимом диапазоне?
- Border-цвет — допустимый индекс (0–15)?
- Нет ли записи в VRAM за пределами допустимой области?

## Output

### Summary

Краткий вывод о состоянии видеоподсистемы.

### Video Mode

| Parameter | Value |
|-----------|-------|
| Mode | 256-column / 512-column |
| Screen RAM base | 0x8000 |
| Plane 0 | 0x8000–0x9FFF |
| Plane 1 | 0xA000–0xBFFF |
| Plane 2 | 0xC000–0xDFFF |
| Plane 3 | 0xE000–0xFFFF |
| Scroll value | ??? |
| Border color index | ?? |
| Active planes | 0–3 (depends on mode) |

### Palette

| Index | R | G | B | Usage |
|-------|---|---|---|-------|

### VRAM Usage

| Region | Type | Content |
|--------|------|---------|

### Findings

| Address | Type | Description |
|---------|------|-------------|

### Evidence

- Текущие I/O значения (video mode, scroll, border);
- VRAM дампы ключевых областей;
- Трассировка VRAM-доступов (если доступна).

## Limitations

- Анализ снимка VRAM не показывает анимацию или динамические эффекты.
- Без трассировки выполнения невозможно определить, какая часть кода отвечает за отрисовку.
- Текстовые и графические паттерны различаются эвристически.
- Bigram paging может изменять физическое расположение VRAM.
