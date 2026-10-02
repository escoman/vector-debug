# Stage 6.27 — MCP Raster / Beam Debugging (Racing-the-Beam)

Источник ТЗ: внешнее постановление «МCP Raster/Beam Debugging для Vector-06Ц»
(57 разделов). План decision-complete: имплементатор не принимает архитектурных
решений.

## 0. Резюме

Добавить в цепочку
```
MCP → Agent API → DebugBackend → DebugAdapter → Board / TV / video state
```
три новых read-only инструмента:
- `debug_get_beam_state`  — текущая позиция луча + видеотайминг + палитра под лучом (Priority 1);
- `debug_get_screen_snapshot` — реально сформированный кадр из TV-буфера как PNG + metadata (Priority 1);
- `debug_get_raster_events` — кольцо событий `OUT` c привязкой к `frame/v_cycle/raster_line/pc` (Priority 2).

Жёсткие рамки ТЗ:
- `src/` и `psp/` НЕ трогаем (`git diff -- src/` и `git diff -- psp/` пустые);
- MCP — тонкий адаптер, ничего не вычисляет сам (№2, №9, №32, №54);
- не добавляем новых хуков/коллбэков в `src/` (№29);
- существующие MCP-инструменты и их семантика не меняются (аддитивно, №50);
- read-only запросы НЕ делают pause (№15) и НЕ перевызывают рендер/CPU (№48).

## 1. Ключевая архитектурная проблема и её решение (РЕШЕНО: вариант A)

Позиция луча (`raster_line`, `raster_pixel`, `fb_row`, `fb_column`, `bmpofs`,
`vborder`, `visible`, `border_index`, `pixel32_grouped`) живёт в `PixelFiller`
(`src/filler.h`) как **private**; публичных геттеров нет, `getColorIndex()`
stateful (двигает `fb_column`/биты), вызывать вне цикла растеризации нельзя.

**Решение пользователя (Пункт A):** добавить в `src/filler.h` МИНИМАЛЬНЫЕ
READ-ONLY константные геттеры под `#ifdef V06C_DEBUGGER` (прецедент: `vio.h::
RawPaletteByte`, `Memory::peek`), которые ТОЛЬКО отдают уже посчитанные поля,
НЕ меняя видеотракт (ни одной строки в fill*/advanceLine/getColorIndex).
`git diff -- src/` при этом НЕ пуст (осознанное отступление от ТЗ §3/§53 с
разрешения пользователя); логика растеризации не затрагивается.

Доступно и без правки src (переиспользуем как есть):
- `Board::get_frame_no()`; `PixelFiller::brk/irq/irq_clk` (public);
- `i8080cpu::i8080_cycles()` → `CpuState::cycles`;
- `IO::BorderIndex()/RawPaletteByte(i)/Palette(i)/Mode512()/ScrollStart()`;
- `Options.screen_width/screen_height/center_offset`.

### 1.1 Позиция луча — напрямую из `filler` (без параллельной модели)

`DebugAdapter::getBeamState()` читает точное состояние через новые геттеры:
- `raster_line  = filler.rasterLine()`; `raster_pixel = filler.rasterPixel()`;
- `v_cycle_in_line = raster_pixel` (0..767); `v_cycle_in_frame =
  raster_line*line_v_cycles + raster_pixel`; `frame = board.get_frame_no()`;
- константы `line_v_cycles=768`, `frame_lines=312`, `frame_v_cycles=239616`
  объявляются ОДИН РАЗ в `DebugAdapter` (`struct VideoTiming`) — адаптерный
  слой, НЕ MCP; MCP читает их только из JSON-ответа (ТЗ №2/§9/§32/§54).

### 1.2 Coordinate/visible — из реальных флагов filler

`visible`/`vborder`/`first_visible_line`/`center_offset`/`screen_width` читаются
у `filler` через read-only геттеры; `visible_x = raster_pixel - center_offset`
(в пределах screen_width), `visible_y = raster_line - first_visible_line`.
Бордюр в visible не входит (ТЗ §8). Никакой своей геометрии в MCP.

### 1.3 Palette index под лучом — тот же индекс, что у видеотракта

Индекс, в который пишет `OUT 0Ch` (`vio.h::commit_palette(index)`), — это ровно
4-битный индекс, который `filler` сдвигает в текущей позиции луча. Добавляем в
`PixelFiller` один const-геттер `currentColorIndex()` под `#ifdef V06C_DEBUGGER`,
возвращающий БЕЗ изменения состояния: `vborder||hborder ? border_index :
(pixel32_grouped >> 28)` (то же значение, что вернул бы следующий
`shiftOutPixels()`, но без сдвига). `palette_value = io.RawPaletteByte(idx)`.
`border_index` — отдельное поле (ТЗ §12, не смешивать с palette_index).

## 2. Изменения по слоям

### 2.1 `debugger/src/` (DebugAdapter, DebugBackend, types)

Новые типы в `debugger/src/debugger_types.h`:
```cpp
struct BeamState {
    uint64_t frame;
    uint32_t vCycleInFrame;
    uint32_t rasterLine;
    uint32_t vCycleInLine;
    uint32_t rpixel;          // внутренняя координата видеотракта (0..767)
    bool     visible;
    int      visibleX, visibleY;   // -1 = нет
    uint32_t frameVCycles, lineVCycles, frameLines;
    uint16_t cpuPc; uint8_t cpuOpcode;
    int      paletteIndex; uint8_t paletteValue;
    int      borderIndex;
    bool     running;
};

struct RasterEvent {            // §25/§28
    uint64_t frame; uint32_t vCycle; uint32_t rasterLine; uint32_t vCycleInLine;
    uint16_t pc; uint8_t port; uint8_t value;
    uint8_t paletteIndexBefore, paletteValueBefore, paletteValueAfter;
};

struct ScreenImageSnapshot {    // §16-23
    std::vector<uint8_t> png;   // закодированный PNG (RGBA/RGB)
    int width, height; uint64_t frame; bool completeFrame;
    std::string format;         // "RGB"
    std::string paletteMode;    // "raster"
};
```

`IDebugTarget` (`debug_target.h`) — новые виртуальные методы с дефолтами:
```cpp
virtual BeamState getBeamState() { return {}; }
virtual ScreenData rawFrameBuffer() { return {}; }   // уже есть screenSnapshot — переиспользуем
virtual std::vector<RasterEvent> getRasterEvents(...) { return {}; }
virtual uint64_t currentFrame() { return 0; }
```
`DebugAdapter` реализует `getBeamState()` (секция 1) — читает `filler.brk`,
`Options`, `io.*`, счётчик `vCycleInFrame_`/`beamFrame_` (пробрасывается из
`DebugBackend` или ведётся в адаптере через существующие HAL-хуки).
`NoBoardTarget` — дефолты (пустой BeamState), чтобы тесты-без-доски не падали.

`DebugBackend`:
- ведёт атомарные `vCycleInFrame_`, `beamFrame_` (эмуляшред, в `onMemoryRead`
  по завершении инструкции) + `setBeamClock(...)`/`getBeamState() const` facade;
- ring buffer `RasterEvent` (ёмкость 50000, как `runtimeAccessLog_`) — пишит в
  `onIoOutput()` текущий `(frame, v_cycle, raster_line, pc, port, value)` +
  before/after палитры; фильтры: frame / [start,end] v_cycle / port / pc;
- реализует `screenSnapshot()` (уже есть) без изменений семантики.

### 2.2 PNG-кодирование кадра

`DebugAdapter::screenSnapshot()` уже возвращает `std::vector<uint32_t>` ARGB из
`tv.pixels()` (реальный TV-буфер, ТЗ §17 — не реконструируем из VRAM). Кодировка
в PNG — в MCP-слое НЕ делается; делаем на выходе Agent API:
- вариант A (предпочт.): переиспользовать `SDL_image` `IMG_SavePNG` (уже линкуется
  в основном эмуляторе через `tv.save_frame`). Проверить доступность SDL2_image в
  цели `v06c-mcp`; если недоступна — вариант B.
- вариант B: единовременный vendored `stb_image_write.h` в
  `debugger/thirdparty/` (паттерн «single-header» уже принят проектом) — кодирует
  RGBA-буфер в PNG в памяти (`stbi_write_png_to_func`). Без изменения геометрии,
  без масштабирования (ТЗ §19).

Размеры берутся из модели видео (`Options.screen_width/height`), не хардкод.

### 2.3 Agent API (`debugger/agent/`)

`agent_types.h`:
- `AgentBeamState` (plain, JSON-compatible, без IDebugBackend-типов);
- `AgentRasterEvent`; `AgentScreenSnapshot` — расширить полями
  `frame / format / source / paletteMode / completeFrame / pngBase64`.

`agent_api.h/.cpp` — новые методы:
- `AgentApiResult<AgentBeamState> getBeamState();`
- `AgentApiResult<AgentScreenSnapshot> getScreenSnapshot();`
  (PNG → base64 в `pngBase64` + metadata);
- `AgentApiResult<std::vector<AgentRasterEvent>> getRasterEvents(frame,
  startVCycle, endVCycle, port, pc, maxResults);`

`AgentLimits`: `MAX_RASTER_EVENTS = 50000`, `RASTER_EVENTS_DEFAULT_LIMIT = 1000`.

Ограничения (§34): beam — O(1); screen — один кадр за вызов; raster events —
`max_results` + диапазон.

### 2.4 MCP (`debugger/mcp/`)

`mcp_json.cpp`: `beamStateToJson`, `rasterEvent(s)ToJson`, расширить
`screenSnapshotToJson` (metadata + base64). Поля — смыслово как в ТЗ §6/§19/§25.

`mcp_adapter.cpp` — новые tools (имена = Agent API):
- `debug_get_beam_state` (без параметров) → text content JSON;
- `debug_get_screen_snapshot` (необязательный `frame` в будующем; сейчас
  `latest`) → content: [ image/png (base64, если клиент поддерживает image
  content), + JSON metadata ]. handler возвращает `mcp::json` content-массив
  вручную: `{"type":"image","data":"<b64>","mimeType":"image/png"}` +
  `{"type":"text","text":metadata}`;
- `debug_get_raster_events` (фильтры §27, `max_results` §34).

Новая группа-функция `registerRasterTools()`, вызывается из `registerAllTools()`.
`set_capabilities` → `v06c.api_version: 3`. README: «70 tools» → фактическое
число (73), добавить группу + раздел. `tools/list` отражает реальность (§33).

## 3. Приоритеты и разбивка на итерации

- **P1 (обязательно, итерация 1):** `debug_get_beam_state` +
  `debug_get_screen_snapshot` + unit-тесты §36–38.
- **P2 (очень желательно, итерация 2):** `debug_get_raster_events` + ring buffer
  + before/after палитры §28 + тесты §40.
- **P3 (после базы, вне этого этапа):** partial raster snapshot (§24) — API
  `debug_get_screen_snapshot` проектируем совместимым (параметр `partial`/
  отдельный инструмент позже), но не реализуем сейчас.

Acceptance (§41–§44): интеграционный тест на `fire3.rom`.

## 4. Тесты

Unit (в `debugger/agent/tests/` и/или `debugger/src` test-suite):
- `test_beam_state` — детерминированные точки: старт кадра, старт строки,
  line+N, следующая строка, граница кадра; инварианты `v_cycle_in_line<768`,
  `raster_line<312`, переход `311→0`; координаты `v_cycle=0/767/768` (§36,§37).
- `test_screen_snapshot` — fixture-ROM: clear → палитра → несколько цветов →
  завершить кадр; проверить: image существует, размеры, значения пикселей, frame
  id (§38).
- `test_racing_beam` — fixture «по мотивам fire3»: VRAM=0, `palette[0]=A`, в
  известной позиции `OUT 0Ch=B`; проверить: VRAM-snapshot не изменился, а
  screen-snapshot изменил цвет (§39). Это принципиальный acceptance.
- `test_raster_events` (P2) — 3×`OUT 0Ch` с delay → возрастающие `v_cycle`,
  `raster_line`, верные `pc`/`value` (§40).

Интеграция:
- `debugger/tests/integration/test_raster_debugging.py` — по образцу
  `test_stage626_integration.py` (тот же поиск `v06c-mcp`, изолированный CWD,
  SKIP при отсутствии ROM). ROM: `fire3.rom` (env `V06C_FIRE3_ROM`) или собранный
  fixture. Сценарий §42: load→run→beam state→screen snapshot→raster events→
  сопоставить `OUT 0Ch` с v_cycle→проверить §43/§44.

Регрессия (§51): `test_agent_api`, `test_mcp_protocol`, `test_mcp_build` + все
существующие debugger-tests должны проходить. Проверить число tools в
`test_mcp_protocol` (ожидалось 70 → станет 73) — обновить константу.

## 5. Документация (§46–§47)

Обновить:
- `debugger/mcp/README.md` — новые tools, api_version 3, число tools;
- Agent API docs (`debugger/agent/AI_AGENT_WORKFLOW.md` / knowledge
  `video.md`);
- `.qoder/skills/vector06c-debugger/SKILL.md`;
- `.qoder/agents/vector06c-analyst.md`;

Добавить раздел «Raster / Racing-the-Beam Debugging» с цепочкой
`debug_get_beam_state → debug_get_raster_events → debug_get_screen_snapshot` и
дисклеймером: «Inspection of VRAM alone is insufficient for raster effects;
use beam state + raster events + screen snapshot».

## 6. Definition of Done (из ТЗ §56)

- `debug_get_beam_state` в Agent API и MCP; согласованные frame/raster/v_cycle;
  PC; palette index/value; border index;
- `debug_get_screen_snapshot` — реальный кадр из TV-буфера, не реконструкция;
- fire3 загружается через MCP; позиция луча и кадр доступны агенту; `OUT 0Ch`
  сопоставим с положением луча;
- интеграционный racing-the-beam тест реализован;
- существующие и новые тесты проходят;
- `git diff -- src/` и `git diff -- psp/` пусты;
- документация обновлена; MCP без собственного video timing/rendering.

## 7. Риски / открытые вопросы (вынести на согласование)

1. Точность `v_cycle_in_frame` (фаза/перенос остатка `filler`) vs тесты §36/§37 —
   калибруем фазу в адаптере по `filler.brk`/`get_frame_no()`.
2. `palette_index` под лучом требует повторить адресацию VRAM из `filler` —
   только индекс, не палитра/пиксель; валидация на тест-ROM.
3. PNG-кодирование: SDL2_image в цели `v06c-mcp` vs vendored stb_image_write —
   выбираем по факту линковки.
4. image-content в MCP: если транспорт клиента не принимает бинарь — отдаём
   base64 в text content как primary, image content как best-effort.
5. Mid-frame точность в `running`: executeFrame — целый кадр; для построчной
   диагностики опираемся на `raster_events` (пишутся на треде эмуляции точно).
