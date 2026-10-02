# Stage 5.2 — Memory Map: Live Activity Visualization

## План реализации

### Текущее состояние

- `MemoryMapWindow` отображает 64 КБ как 256×256 пикселей (каждый пиксель = 256 байт)
- Цвет зависит от классификации Code/Data/Unknown + накопленная активность
- `rebuildMap()` проходит по 256×256 элементам, для каждого суммирует 256 адресов — медленно
- `MemoryStats` уже содержит `lastReadSequence` / `lastWriteSequence` на каждый адрес
- `memoryStatsSnapshot()` есть в `DebugBackend`, но **не** в `IDebugBackend`

### Что нужно изменить

---

## Шаг 1. Расширить `MemoryStats` timestamps реального времени

**Файлы:** `debugger/src/events.h`

Добавить в `MemoryStats`:
```cpp
using Clock = std::chrono::steady_clock;
using TimePoint = std::chrono::steady_clock::time_point;

struct MemoryStats
{
    uint64_t reads;
    uint64_t writes;
    uint64_t lastReadSequence;
    uint64_t lastWriteSequence;
    TimePoint lastReadTime;    // wall-clock для Live Map
    TimePoint lastWriteTime;   // wall-clock для Live Map
};
```

---

## Шаг 2. Записывать timestamps в `onMemoryRead` / `onMemoryWrite`

**Файлы:** `debugger/src/backend.cpp`

В `onMemoryRead()` и `onMemoryWrite()`:
```cpp
impl_->memStats[virt].lastReadTime = Clock::now();
impl_->memStats[virt].lastWriteTime = Clock::now();
```

Инициализация при создании `Impl`:
```cpp
memStats[i].lastReadTime = TimePoint::min();   // "никогда"
memStats[i].lastWriteTime = TimePoint::min();
```

---

## Шаг 3. Добавить `LiveActivitySnapshot` в API

**Файлы:** `debugger/src/idebug_backend.h`, `debugger/src/backend.h`, `debugger/src/backend.cpp`

Новый тип snapshot (в `IDebugBackend`):
```cpp
struct LiveBlockState
{
    std::chrono::steady_clock::time_point lastReadTime;
    std::chrono::steady_clock::time_point lastWriteTime;
};

struct LiveActivitySnapshot
{
    LiveBlockState blocks[256];  // 256 блоков по 256 байт
};
```

В `IDebugBackend` добавить:
```cpp
virtual LiveActivitySnapshot liveActivitySnapshot() const = 0;
```

В `DebugBackend` реализовать: пройтись по `memStats[65536]`, для каждого адреса вычислить блок (`addr >> 8`), записать max(lastReadTime) и max(lastWriteTime) в соответствующий блок.

---

## Шаг 4. Переписать `MemoryMapWindow`

**Файлы:** `debugger/gui/memory_map_window.h`, `debugger/gui/memory_map_window.cpp`

### 4.1 Новая геометрия

```cpp
static constexpr int BLOCK_SIZE   = 256;  // байт на блок
static constexpr int NUM_BLOCKS   = 256;  // 65536 / 256
static constexpr int CELL_PX      = 2;    // пикселей на сторону блока
static constexpr int GRID_PX      = 1;    // толщина линии сетки
static constexpr int COLUMNS      = 8;    // 8-КБ колонки
static constexpr int ROWS         = 32;   // блоков в колонке

// Canvas size:
// width  = COLUMNS * CELL_PX + (COLUMNS + 1) * GRID_PX = 8*2 + 9*1 = 25
// height = ROWS * CELL_PX + (ROWS + 1) * GRID_PX = 32*2 + 33*1 = 97
```

### 4.2 Новые поля класса

```cpp
bool live_ = false;

struct BlockState {
    std::chrono::steady_clock::time_point lastReadTime;
    std::chrono::steady_clock::time_point lastWriteTime;
};
BlockState blocks_[256];

static constexpr auto ACTIVITY_DURATION = std::chrono::milliseconds(500);
```

### 4.3 Удалить

- Старые константы `MAP_WIDTH`, `MAP_HEIGHT`, `BYTES_PER_PIXEL`
- `getColorForAddress()` — больше не нужен
- `rebuildMap()` в старом виде — заменить на `updateLiveMap()`
- Легенду Code/Data/Unknown

### 4.4 Новая логика отрисовки

1. Получить `liveActivitySnapshot()` из backend
2. Обновить `blocks_[256]` timestamps
3. Для каждого блока вычислить цвет:
   - `readActive = now - lastReadTime < 500ms`
   - `writeActive = now - lastWriteTime < 500ms`
   - Цвет по таблице: gray / green / red / yellow
4. Отрисовать сетку + блоки (texture или ImDrawList)
5. Над колонками — адреса `0x0000`, `0x2000`, ..., `0xE000`

### 4.5 Live checkbox

```cpp
ImGui::Checkbox("Live", &live_);
```

- Live OFF: не обновлять timestamps из snapshot, блоки серые
- Live ON: обновлять, окрашивать по таймерам
- При переходе OFF → ON: очистить все timestamps
- При переходе ON → OFF: можно сразу убрать подсветку

### 4.6 Clear Activity

```cpp
if (ImGui::Button("Clear")) {
    for (auto &b : blocks_) {
        b.lastReadTime = {};
        b.lastWriteTime = {};
    }
}
```

### 4.7 Tooltip и навигация

Сохранить существующие:
- Tooltip: `Address: 0x1200–0x12FF`, `Size: 256 bytes`
- Дополнительно: Symbol, Type (Code/Data/Unknown) — но не влияет на цвет
- Context menu: Go to Memory Inspector, Go to Disassembly
- Правый клик: Mark as Code/Data/Unknown (влияет только на Symbol Database)

### 4.8 Масштабирование

Canvas базовый: 25×97 пикселей. Масштабировать через `ImGui::Image()` с nearest-neighbor.
Или рисовать через `ImDrawList` — прямоугольники 2×2 + линии сетки.

---

## Шаг 5. Unit-тесты

**Файл:** `debugger/tests/test_live_map.cpp`

Тесты без зависимости от ImGui/OpenGL. Вынести логику определения цвета блока в тестируемую функцию:

```cpp
// В memory_map_window.h или отдельном header:
enum class LiveBlockColor { DarkGray, Green, Red, Yellow };

LiveBlockColor computeBlockColor(
    std::chrono::steady_clock::time_point lastRead,
    std::chrono::steady_clock::time_point lastWrite,
    std::chrono::steady_clock::time_point now,
    std::chrono::milliseconds duration);
```

Тесты (20 штук по ТЗ):
1–7: адрес → блок (0x0000→0, 0x00FF→0, 0x0100→1, ..., 0xFFFF→255)
8–14: цвет по timestamps (read→green, write→red, read+write→yellow, expiry)
15–16: продление таймера
17–18: Live OFF/ON поведение
19: Clear Activity
20: Code/Data не влияет на цвет

Добавить в CMakeLists.txt:
```cmake
add_executable(test_live_map
    ${DBG_TST_DIR}/test_live_map.cpp
)
```

---

## Шаг 6. Обновить CMakeLists.txt

Добавить `test_live_map` в список целей.

---

## Порядок реализации

1. **events.h** — добавить `TimePoint` в `MemoryStats`
2. **backend.cpp** — записывать `Clock::now()` в `onMemoryRead/Write`
3. **idebug_backend.h** — добавить `LiveActivitySnapshot`, `liveActivitySnapshot()`
4. **backend.h/cpp** — реализовать `liveActivitySnapshot()`
5. **memory_map_window.h/cpp** — полная переделка
6. **test_live_map.cpp** — unit-тесты
7. **CMakeLists.txt** — добавить тест

---

## Проверки

```bash
# Не изменять src/
git diff -- src/
# Результат: пусто

# Все тесты проходят
cd debugger/build
./test_backend
./test_symbol_database
./test_vram_mapping
./test_board_smoke
./test_workspace
./test_live_map

# GUI собирается
./v06c-debugger
```

---

## Критерии завершения (из ТЗ)

- [ ] карта содержит ровно 256 блоков
- [ ] каждый блок = 256 байт
- [ ] блок визуально 2×2 пикселя
- [ ] сетка тёмно-серая между блоками (1px)
- [ ] 8 горизонтальных колонок по 8 КБ
- [ ] каждая колонка — 32 блока
- [ ] над колонками адреса 0x0000...0xE000
- [ ] базовый цвет — тёмно-серый
- [ ] Code/Data/Unknown не определяют цвет
- [ ] checkbox Live
- [ ] read → green на 500 мс
- [ ] write → red на 500 мс
- [ ] read + write → yellow
- [ ] таймер продлевается новым событием
- [ ] execute/fetch не окрашивает
- [ ] Live OFF отключает подсветку
- [ ] Live ON начинается с чистого состояния
- [ ] Clear Activity очищает timestamps
- [ ] навигация Memory/Disassembly сохраняется
- [ ] GUI не обращается к Board/Memory/CPU напрямую
- [ ] не используется Memory::buffer()
- [ ] src/ не изменён
- [ ] все существующие тесты проходят
- [ ] новые тесты проходят
- [ ] GUI собирается и запускается
