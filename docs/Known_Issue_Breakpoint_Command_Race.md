# Known Issue — data race при конкурентном добавлении breakpoints в `DebugBackend`

- **Статус:** Open (дефект в **этом** репозитории, файл `src/backend.cpp`; не submodule)
- **Обнаружено:** прогон тестовой сюиты на Linux после Windows-адаптации
  (коммиты `f277d77`, `a3920fa`). К Windows-правкам отношения не имеет — всплыл
  как давно живущий флакающий тест.
- **Влияние:** тест `test_agent_integration / concurrent_commands_not_lost`
  intermittently падает: `4 breakpoints total: expected 0x4, got 0x3` (line 305),
  итог `50/51 passed (1 FAILED)`. На одном и том же бинаре прогон даёт
  `exit=1,0,0,0,0,1,1,1` (~50% флака). В проде риск тот же: конкурентные вызовы
  `requestAddBreakpoint` из нескольких потоков могут потерять breakpoint или
  повредить `std::map`.

## Сигнатура

```
FAIL  4 breakpoints total: expected 0x4, got 0x3 (line 305)
FAIL  concurrent_commands_not_lost
Results: 50/51 passed  (1 FAILED)
```

Показательно: проверка `successCount == 4` («all 4 concurrent breakpoints
added») проходит, а `getBreakpoints().size()` иногда даёт 3 — то есть все 4
команда «успешны», но один элемент теряется в контейнере.

## Корень (подтверждено кодом)

Мутатор и читатель состояния breakpoints рассогласованы по локам.

`src/backend.cpp:699` — мутация общего состояния **без** какого-либо мьютекса:

```cpp
int DebugBackend::addBreakpoint(uint16_t address) {
    if (findBreakpointByAddress(address) != breakpoints_.end()) return -1; // TOCTOU-check
    int id = nextId_++;                    // неатомарный read-modify-write
    breakpoints_[id] = { address, true };  // конкурентная вставка в std::map
    return id;
}
```

`src/backend.cpp:736` — читатель **берёт** `commandMutex_`:

```cpp
std::vector<DebuggerBreakpoint> DebugBackend::getBreakpoints() const {
    std::lock_guard<std::mutex> lock(commandMutex_);   // только читатель лочится
    ...
}
```

Путь исполнения. `requestAddBreakpoint` (`backend.cpp:1077`) → `submitAndWait`
(`:1150`). В тестовом синхронном режиме (`TestFixture` ставит
`backend->testSynchronous_ = true`, `agent/tests/test_agent_integration.cpp:223`;
цикл эмуляции не запущен) `submitAndWait` выполняется по ветке `:1157` и вызывает
`executeCommand(*cmd)` **на потоке-вызывателе**. `executeCommand` →
`case AddBreakpoint` → `addBreakpoint()` (`:1221`). Когда тест запускает 4
потока, каждый со своим `requestAddBreakpoint`, все 4 вызывают
`addBreakpoint()` одновременно, и `commandMutex_` тут не участвует вовсе.

Три независимых гонки в одном `addBreakpoint()`:

1. **`nextId_++`** — неатомарный RMW. Два потока читают одно и то же значение,
   оба пишут `breakpoints_[id]` с одинаковым ключом `id`. В `std::map` вторая
   вставка перезаписывает первую → из 4 элементов остаётся 3. Это и есть
   наблюдаемый `got 0x3`.
2. **`breakpoints_[id] = ...`** — конкурентная модификация дерева `std::map` без
   локов = UB (может уронить/повредить структуру), но чаще проявляется как
   потерянная запись.
3. **`findBreakpointByAddress` + вставка** — TOCTOU: проверка «уже есть?» и сама
   вставка не атомарны.

`getBreakpoints()` держит `commandMutex_`, но писатели его не берут, поэтому лок
читателя ничего не сериализует относительно мутаций — гонка read/write тоже
возможна.

## Как воспроизвести (Linux)

```bash
cmake -DENABLE_AI_AGENT=ON .. && make test_agent_integration -j"$(nproc)"
# крутить бинарь ~10 раз — примерно половина прогонов даёт exit=1 на строке 305
for i in $(seq 1 10); do ./build/test_agent_integration >/dev/null 2>&1; echo "exit=$?"; done
```

Сборка/тесты не зависят от платформы — флак одинаково и на Linux, и на Windows.

## Предлагаемое исправление

Согласовать лок писателя с читателем. Минимальный корректный вариант — защищать
`breakpoints_`/`nextId_` одним и тем же мьютексом на всех путях мутации:

```cpp
// backend.h: добавить (или переиспользовать) dedicated мьютекс, напр. bpMutex_,
// либо использовать существующий commandMutex_ на записи.
int DebugBackend::addBreakpoint(uint16_t address) {
    std::lock_guard<std::mutex> lock(bpMutex_);
    if (findBreakpointByAddress(address) != breakpoints_.end()) return -1;
    int id = nextId_++;
    breakpoints_[id] = { address, true };
    return id;
}
```

То же для `removeBreakpoint`, `setBreakpointEnabled`, `clearBreakpoints`,
`hasBreakpoint`, `findBreakpointByAddress` и читателей (`getBreakpoints`,
`getBreakpointsList`). Важно: `syncBreakpointsToTarget()` (`:1229`) вызывается из
`executeCommand` после `addBreakpoint` — при введении лока следить за порядком
захвата, чтобы не заполучить self-deadlock (не держать `bpMutex_` при вызовах,
которые сами берут другие локи) и не брать `commandMutex_` рекурсивно.

Альтернатива — сделать `nextId_` атомарным и перевести все обращения к
`breakpoints_` под один мьютекс; но атомарный `nextId_` сам по себе не спасает от
гонки на `std::map`, поэтому лок обязателен.

Тест `concurrent_commands_not_lost` правку не требует — он корректно ловит
дефект; после фикса должен стать стабильно зелёным.
