# Техническое задание

# Vector-06C Debugger

## 1. Назначение

Разработать кросс-платформенный графический debugger для программ ПК Вектор-06Ц на базе эмуляционного ядра:

https://github.com/svofski/vector06sdl

Debugger должен обеспечивать полный контроль исполнения:

* запуск;
* остановка;
* продолжение;
* пошаговое выполнение инструкций;
* breakpoints;
* watchpoints;
* просмотр CPU;
* просмотр памяти;
* живая карта 64 КБ памяти;
* просмотр текущего экрана Вектора;
* просмотр портов;
* история инструкций;
* история обращений к памяти и портам;
* дизассемблер;
* отображение C-кода при наличии debug information z88dk;
* breakpoint по строке C-кода;
* просмотр stack.

Проект должен собираться под:

* Linux x86_64;
* Windows x86_64.

---

# 2. Архитектура

Debugger должен быть разделён на backend и GUI.

```text
+-------------------------+
|      Debugger GUI       |
+------------+------------+
             |
             v
+-------------------------+
|    Debugger Backend     |
|                         |
| breakpoints             |
| watchpoints             |
| stepping                |
| CPU state               |
| memory tracking         |
| I/O tracking            |
| history                 |
| symbols                 |
| source mapping          |
+------------+------------+
             |
             v
+-------------------------+
|    Vector-06C Core      |
|                         |
| CPU                     |
| Memory                  |
| I/O                     |
| Video                   |
| Sound                   |
+-------------------------+
```

Не создавать второй эмулятор.

Не создавать отдельную модель памяти.

Не дублировать CPU.

Debugger должен работать с реальным состоянием существующего эмулятора.

---

# 3. Изучение существующего vector06sdl

До написания кода необходимо изучить repository:

https://github.com/svofski/vector06sdl

Необходимо определить:

1. реализацию CPU;
2. execution loop;
3. модель памяти;
4. I/O;
5. video subsystem;
6. framebuffer;
7. существующий debugger API;
8. существующие breakpoint callbacks;
9. существующий scripting/debugging infrastructure.

После анализа сначала подготовить документ:

```text
docs/DEBUGGER_ARCHITECTURE.md
```

с описанием:

* существующей архитектуры;
* точек интеграции;
* необходимых изменений;
* новых интерфейсов;
* последовательности реализации.

До завершения этого анализа не переписывать существующие подсистемы.

---

# 4. CPU State

Debugger должен предоставлять:

```text
PC
SP

A
B
C
D
E
H
L

FLAGS
```

Флаги:

```text
S
Z
AC
P
CY
```

Создать единый debug API для получения состояния CPU.

---

# 5. Управление исполнением

Поддержать:

```text
RUN
PAUSE
STOP
RESET
STEP INTO
```

Дополнительно:

```text
STEP OVER
STEP OUT
RUN TO ADDRESS
```

после реализации базового stepping.

---

# 6. Step Into

`Step Into` должен выполнять **ровно одну инструкцию 8080**.

Не кадр.

Не фиксированное число циклов.

Не произвольное количество инструкций.

После выполнения debugger должен знать:

```text
PC before
opcode
operands
PC after
CPU state
memory accesses
I/O accesses
```

---

# 7. Breakpoints

Поддержать несколько breakpoint одновременно.

Типы:

### Address

```text
break $8123
```

Остановка перед выполнением инструкции.

### Symbol

```text
break main
```

### Source

```text
break source.c:125
```

### Memory read

```text
break read $C000
```

### Memory write

```text
break write $C000
```

### I/O

```text
break in $10
break out $20
```

Breakpoint не должен изменять opcode ROM.

---

# 8. Watchpoints

Поддержать:

```text
READ
WRITE
READ+WRITE
```

для:

* одного адреса;
* диапазона.

Например:

```text
watch $C000
watch $C000-$C0FF
```

---

# 9. Memory Access Tracking

Каждое обращение CPU к памяти должно при необходимости фиксироваться:

```text
PC
ADDRESS
VALUE
READ/WRITE
```

Например:

```text
PC=8123 READ  $4567 = 34
PC=8125 WRITE $9000 = 7F
```

Instrumentation включается только в DEBUG mode.

NORMAL mode не должен нести существенных накладных расходов.

---

# 10. Живая карта памяти 64 КБ

Создать отдельное окно:

```text
Memory Map
```

Карта должна представлять **всё 64 КБ адресного пространства RAM Вектора-06Ц**.

Карта должна быть интерактивной.

Рекомендуемый размер базового блока:

```text
256 bytes
```

Всего:

```text
65536 / 256 = 256 блоков
```

Каждый блок является clickable.

При клике по блоку соответствующий диапазон открывается в Memory Dump.

Например:

```text
Memory Map
    ↓ click
$8C00-$8CFF
    ↓
Memory Dump
```

Размер блока желательно сделать настраиваемым:

```text
64 B
256 B
1 KB
4 KB
```

На первом этапе достаточно 256 B.

---

# 11.1. Цвета Memory Map

Карта должна отображать два независимых свойства памяти:

1. текущее содержимое;
2. активность CPU.

### Состояние содержимого

Если значение ячейки:

```text
== 00
```

ячейка отображается тёмным/нейтральным цветом.

Если:

```text
!= 00
```

ячейка отображается **серым**.

Таким образом визуально сразу видно:

* где находится загруженная программа;
* где находятся данные;
* куда программа распаковала данные;
* какие области памяти реально используются.

### Состояние доступа

Недавние обращения имеют приоритет над серым состоянием:

```text
READ       → зелёный
WRITE      → красный
READ+WRITE → жёлтый
```

Итого:

| Состояние               | Отображение |
| ----------------------- | ----------- |
| `00`, нет активности    | тёмный      |
| `!= 00`, нет активности | серый       |
| READ                    | зелёный     |
| WRITE                   | красный     |
| READ + WRITE            | жёлтый      |

---

# 11.2. Затухание подсветки

Цвета READ/WRITE/READ+WRITE являются временной подсветкой.

Например:

```text
READ  → зелёный на 500 ms
WRITE → красный на 500 ms
R+W   → жёлтый на 500 ms
```

Повторный доступ обновляет таймер.

После окончания TTL ячейка возвращается:

```text
00    → тёмная
!=00  → серая
```

TTL должен быть configurable.

---

# 11.3. Накопительная статистика

Debugger должен отдельно хранить статистику обращений.

Например:

```text
$C000
READ:  152
WRITE: 38
```

Это не должно зависеть от временной подсветки.

Глубина/время хранения истории должны быть ограничены.

---

# 11.4. Карта должна быть byte-level

Хотя визуально память группируется в блоки, внутри каждого блока должны быть доступны данные об отдельных байтах.

Например:

```text
$8C00-$8CFF
```

содержит 256 отдельных состояний.

При необходимости GUI должен показывать внутри блока мини-карту:

```text
████░░███░░░██...
```

где каждый символ/пиксель соответствует одной ячейке.

---

# 11.5. Memory Map Navigation

По карте можно:

* кликнуть блок;
* открыть Memory Dump;
* перейти к адресу;
* установить breakpoint;
* установить watchpoint;
* перейти в Disassembler.

---

# 12. Memory Dump

Отдельное окно подробного просмотра:

```text
Address   Hex bytes                         ASCII

8C00      3E 12 32 00 C0 21 00 80 ...     >.2.!..
8C10      ...
```

Поддержать:

* переход к адресу;
* поиск;
* редактирование;
* копирование;
* переход из Memory Map;
* переход из Disassembler;
* переход из Registers;
* подсветку READ/WRITE;
* отображение текущего значения.

---

# 13. Текущее изображение экрана Вектора

Создать отдельное окно:

```text
Vector-06C Screen
```

Оно должно показывать **реальное текущее изображение, формируемое существующей видеосистемой эмулятора**.

Не создавать отдельную модель video rendering.

Не реконструировать изображение самостоятельно из памяти.

Использовать существующий framebuffer/video output.

Поведение:

### RUN

Экран обновляется в реальном времени.

### PAUSE

Отображается последний сформированный кадр.

### STEP

После выполнения инструкции экран обновляется при необходимости.

Это особенно важно для программ, которые:

* меняют видеопамять;
* меняют палитру;
* меняют параметры видеосистемы во время формирования кадра.

---

# 14. CPU Window

Отображать:

```text
PC
SP

A
B
C
D
E
H
L

FLAGS
```

Изменившиеся после последней инструкции значения подсвечивать.

---

# 15. Disassembler

Встроенный 8080 disassembler.

Пример:

```text
8120  LXI H,C000
8123  MOV A,M
8124  INR A
8125  MOV M,A
8126  RET
```

Показывать:

* текущий PC;
* breakpoints;
* symbols;
* source line;
* переходы.

Текущая инструкция выделяется.

---

# 16. Source View

При наличии z88dk debug information отображать C source.

Например:

```text
122    for (...)
123    {
124        p[i] = buffer[i];
125    }
```

Текущая строка выделяется.

Должна быть возможность:

```text
C source line
      ↓
machine address
      ↓
ASM
```

---

# 17. z88dk Debug Information

Исследовать и поддержать:

```text
.map
.cdb
.sym
```

Не ограничиваться только `.map`.

Создать:

```text
Z88DebugInfo
```

с функциями:

```cpp
load();

addressToSource();

sourceToAddresses();

symbolAt();

findSymbol();
```

Поддержать:

```text
PC → source file + line
source file + line → address
address → symbol
symbol → address
```

---

# 18. Source Breakpoint

Пользователь должен иметь возможность поставить breakpoint непосредственно на строку C.

Если строка соответствует нескольким машинным инструкциям — breakpoint ставится на первую соответствующую инструкцию.

---

# 19. Symbols

Отображать:

```text
function
variable
label
```

если информация присутствует.

Symbol можно:

* найти;
* открыть;
* использовать как breakpoint;
* использовать как watch;
* отображать в disassembler.

---

# 20. Stack Viewer

Показывать:

```text
SP
stack contents
return addresses
```

Если адрес соответствует symbol — отображать symbol.

---

# 21. Ports Viewer

Показывать операции IN/OUT.

Например:

```text
PORT    LAST IN    LAST OUT    COUNT
00      --         12          10
01      34         --          3
```

Также:

```text
PC
operation
value
```

---

# 22. I/O History

История:

```text
PC     TYPE   PORT   VALUE
8120   OUT    10     03
8234   IN     20     FF
```

Ограниченная глубина.

---

# 23. Instruction History

Хранить последние инструкции:

```text
PC      Instruction
8120    LXI H,C000
8123    MOV A,M
8124    INR A
8125    MOV M,A
```

Глубина настраивается.

---

# 24. Главное окно

Рекомендуемый layout:

```text
+---------------------------------------------------------------+
| File  Debug  View  Run  Tools                                 |
+---------------------------------------------------------------+
|                                                               |
| Vector-06C Screen          | CPU / Registers                  |
|                            |                                  |
|                            |                                  |
+----------------------------+----------------------------------+
| Memory Map                 | Memory Dump                      |
| 64 KB                     |                                  |
|                            |                                  |
| 🟩 🟥 🟨 ░                 |                                  |
+----------------------------+----------------------------------+
| Source / Disassembler                                         |
+---------------------------------------------------------------+
| Stack / Ports / History / Breakpoints                         |
+---------------------------------------------------------------+
```

Окна должны быть dockable.

Пользователь может менять их размер и положение.

---

# 25. Debugger Console

Предусмотреть command console.

Команды:

```text
run
pause
step
reset

break $8123
break main

watch $C000

memory $C000
disasm $8120

regs
stack
symbols
```

GUI должен использовать тот же backend API, что и console.

---

# 26. Threading

GUI и emulator могут работать в разных потоках.

CPU нельзя читать одновременно с его изменением.

При PAUSE:

```text
CPU stopped
Memory stable
CPU state stable
```

только после этого GUI получает состояние.

---

# 27. Performance

В NORMAL mode debug hooks должны быть отключены или иметь минимальную стоимость.

Не использовать тяжёлые конструкции на каждой CPU-инструкции:

```text
dynamic allocation
std::string
std::map
std::function
```

без необходимости.

Для address breakpoints предпочтительно:

```cpp
std::array<bool, 65536>
```

или эквивалентная O(1) структура.

---

# 28. Disassembler / CPU opcode table

Не создавать независимые таблицы opcode.

По возможности использовать единую информацию:

```text
opcode
mnemonic
length
cycles
flags
```

для:

* CPU;
* debugger;
* disassembler.

---

# 29. Build System

Использовать CMake.

Linux:

```bash
cmake -B build
cmake --build build
```

Windows:

```powershell
cmake -B build
cmake --build build --config Release
```

Не делать Visual Studio project главным build mechanism.

---

# 30. Загрузка ROM

Поддержать:

```text
vector-debugger program.rom
```

Автоматически искать рядом:

```text
program.map
program.cdb
program.sym
```

При необходимости принимать их явно.

После загрузки показывать:

```text
ROM: GAME.ROM

Debug information:
Symbols: 342
Source files: 7
Source mappings: 1832
```

---

# 31. Memory Model

Debugger должен работать с реальной 64-КБ памятью эмулятора.

Запрещается создавать независимую:

```cpp
uint8_t debug_memory[65536];
```

которая может рассинхронизироваться с CPU.

Memory Viewer и Memory Map должны читать состояние реальной памяти.

---

# 32. Debug Hooks

Добавить минимальные hooks:

```text
instructionBefore
instructionAfter

memoryRead
memoryWrite

portRead
portWrite
```

Точная реализация определяется существующей архитектурой `vector06sdl`.

---

# 33. Platform independence

Не разносить platform-specific код по проекту.

Использовать:

```text
platform/linux
platform/windows
```

только там, где действительно требуется platform-specific functionality.

---

# 34. GUI

Предпочтительно использовать:

```text
Dear ImGui + SDL2
```

если это хорошо интегрируется с существующей инфраструктурой.

Не использовать Qt без необходимости.

---

# 35. Тестовая программа

Создать простую тестовую программу для Вектора, например:

```c
uint8_t counter = 0;

void main()
{
    while (1)
    {
        counter++;

        if (counter == 100)
            counter = 0;
    }
}
```

Она должна использоваться для проверки:

* breakpoint;
* source mapping;
* stepping;
* memory read;
* memory write;
* registers;
* C source;
* disassembler;
* memory map.

---

# 36. Acceptance Tests

Debugger должен успешно пройти:

1. Load ROM.
2. Run.
3. Pause.
4. Reset.
5. Step Into.
6. Address breakpoint.
7. Symbol breakpoint.
8. Source breakpoint.
9. Memory watchpoint.
10. I/O breakpoint.
11. CPU register display.
12. Stack display.
13. Disassembler.
14. C source mapping.
15. Memory Dump.
16. Live 64-KB Memory Map.
17. READ green.
18. WRITE red.
19. READ+WRITE yellow.
20. Non-zero memory gray.
21. Zero memory dark.
22. Memory Map → Memory Dump navigation.
23. Live Vector screen.
24. I/O history.
25. Instruction history.
26. Linux build.
27. Windows build.

---

# 37. Future functionality

Архитектура должна позволять добавить:

```text
Step Over
Step Out

Conditional Breakpoints

Expression evaluator

Variable Watch

Call Stack

Reverse Debugging
Step Back
Reverse Continue

GDB Remote Protocol
```

Но не реализовывать это в MVP.

---

# 38. Критический приоритет

Приоритеты:

```text
1. Correct Vector-06C emulation
2. Correct debugger backend
3. Exact stepping
4. Correct memory/I/O tracking
5. Correct source mapping
6. GUI
7. Advanced debugger features
```

Debugger не должен менять поведение ROM относительно обычного эмулятора.

DEBUG mode может работать медленнее NORMAL mode.

Но результат выполнения должен быть идентичным.
