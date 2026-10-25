# Known Issue — гонка остановки `thread_pool` в `thirdparty/cpp-mcp` (Windows)

- **Статус:** Open (внешний submodule; в этой ветке **не** патчится). Обход
  реализован на стороне нашего теста `mcp/tests/test_mcp_protocol.cpp`.
- **Обнаружено:** прогон тестовой сюиты под MinGW/MSYS2 после починки
  [`coredsp::FIR`](Known_Issue_FIR_Aligned_Heap_Corruption_Windows.md). Убранная
  порча кучи высветила второй, независимый дефект — раньше процесс умирал от
  `STATUS_HEAP_CORRUPTION` раньше, чем успевал дойти до этой гонки.
- **Влияние:** `test_mcp_protocol` аварийно завершается с кодом выхода `3`
  (`std::terminate`) при деструкции `McpServer`; число пройденных ассертов
  плавает от прогона к прогону (28…61), итоговая строка `Results:` не печатается.
  В проде (`v06c-mcp.exe`) тоже существует риск на остановке сервера, но он
  манифестирует редко (иное время жизни/планирование потоков).

## Это дефект НЕ в этом репозитории

Корень — в vendored-подмодуле `thirdparty/cpp-mcp`
(`https://github.com/hkr04/cpp-mcp.git`), файл `include/mcp_thread_pool.h`.

## Сигнатура

```
terminate called after throwing an instance of 'std::system_error'
  what():  Invalid argument
```

gdb-бэктрейс указывает на worker-поток пула:

```
#7  mcp::thread_pool::worker_loop()   thirdparty/cpp-mcp/include/mcp_thread_pool.h:163
#8  <lambda> in thread_pool::spawn_thread()  :112
...  std::thread::_State_impl::_M_run  ->  winpthread  ->  _beginthreadex
```

Строка 163 — `std::unique_lock<std::mutex> lock(queue_mutex_);`. Бросок
`std::system_error("Invalid argument")` (EINVAL) из `lock()` = попытка залочить
**уже уничтоженный** мьютекс.

## Корень (подтверждено кодом)

Воркеры пула запускаются **detach'нутыми** потоками (`spawn_thread`, строка 113
`.detach()`), а схема остановки расходится между двумя отдельными lock-скоупами.

Деструктор (`~thread_pool`, строки 51–62):

```cpp
{ std::unique_lock<std::mutex> lock(queue_mutex_); stop_ = true; }
condition_.notify_all();
std::unique_lock<std::mutex> lock(queue_mutex_);
shutdown_cv_.wait(lock, [this] { return active_threads_ == 0; });   // ждём «все ушли»
```

Worker (`worker_loop`):

```cpp
if (stop_ && tasks_.empty()) { is_shrinking = true; active_threads_--; break; }   // декремент ВНУТРИ 1-го lock
...
// ПОСЛЕ выхода из цикла, уже БЕЗ захваченного lock:
{ std::unique_lock<std::mutex> lock(queue_mutex_);          // 2-й lock (строка 163)
  if (active_threads_ == 0) shutdown_cv_.notify_all(); }
```

`active_threads_` — атомарный, и предикат `wait` читает его напрямую. Между
декрементом (внутри первого скоупа) и `notify_all()` (во втором скоупе) есть
**окно**: деструктор может проснуться (в т.ч. ложно), увидеть `active_threads_
== 0`, выйти из `wait`, отработать и **уничтожить `queue_mutex_`** — а detached-
воркер в этот момент только собирается залочить его на строке 163 → EINVAL →
`std::terminate`. На Linux тайминги планирования потоков иные, и окно, как
правило, не реализуется (поэтому CI всегда был зелёным); на Windows оно всплывает.

Предлагаемый апстрим-фикс (платформо-независимый, в `worker_loop`): уменьшить
`active_threads_` и известить `shutdown_cv_` **под тем же единственным
захваченным lock**, после чего `return` — никогда повторно не залачивать
`queue_mutex_` после его освобождения:

```cpp
if (tasks_.empty() &&
    (stop_ || (is_timeout && active_threads_ > min_threads_))) {
    active_threads_--;
    if (active_threads_ == 0)
        shutdown_cv_.notify_all();
    return;                       // не трогаем члены пула после unlock
}
```

(Предлагаемый апстрим-фикс приведён кодом выше; отдельный `.patch`-артефакт не
хранится (весь `thirdparty/` попадает в `.gitignore`, а правка чужого submodule не
должна пережить `git submodule update`). Сам submodule в рабочем дереве **не**
патчится: `git -C thirdparty/cpp-mcp status` чист.)

## Обход на стороне нашего теста (выбран вместо патча submodule)

Все ассерты `test_mcp_protocol` проходят; небезопасен только **путь уничтожения**
сервера. Поэтому обход сводится к тому, чтобы не вызывать `~McpServer` (и,
следовательно, `~thread_pool`) во время теста:

1. `Fixture` больше не владеет `McpServer` по значению — член стал ссылкой на
   намеренно **утекающий** кучный объект (`*new McpServer(api)`, никогда не
   `delete`). Деструктор пула не запускается → гонки нет.
2. В конце `main()` — `std::fflush(nullptr); std::_Exit(...);`, чтобы обойти и
   статический/atexit teardown. `fflush` обязателен, т.к. `_Exit` не сбрасывает
   stdio (это заодно чинило обрезанный при abort вывод).

Это изолировано в тесте и не меняет прод-код. Когда submodule будет залатан
апстрим и гитлинк обновлён, обход можно убрать (поиск `Known_Issue_cpp_mcp`
возвращает нужные места в `mcp/tests/test_mcp_protocol.cpp`).

## Статус Windows-CI

`test_mcp_protocol`: 61/61, `EXIT=0` стабильно (проверено многократно). Весь
набор — 30/30 целей с `EXIT=0`.
