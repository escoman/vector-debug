# Дополнение к ТЗ Stage 6.2 — Breakpoint для функции из Functions

## 1. Цель

Добавить в окно **Functions** возможность установить или снять breakpoint непосредственно для выбранной функции.

Breakpoint должен использовать существующий механизм debugger и быть обычным breakpoint по адресу.

---

## 2. Контекстное меню Functions

При клике правой кнопкой мыши по функции открыть контекстное меню.

Если breakpoint на адресе функции отсутствует:

    Set Breakpoint

Если breakpoint уже установлен:

    Remove Breakpoint

Дополнительно можно оставить оба пункта, отключая неактуальный, но предпочтителен динамический вариант.

---

## 3. Установка breakpoint

При выборе:

    Set Breakpoint

использовать адрес функции из существующего `SymbolDatabase`.

Например:

    0252  _main

→ установить breakpoint на:

    0x0252

Не искать адрес повторно по имени.

Не создавать отдельный тип «function breakpoint».

Это должен быть обычный address breakpoint.

---

## 4. Архитектура

Использовать существующий путь:

    Functions GUI
          ↓
    IDebugBackend
          ↓
    Command Queue
          ↓
    DebugAdapter
          ↓
    Board

GUI не должен напрямую обращаться к:

* `Board`;
* `Memory`;
* `i8080`;
* `DebugAdapter`.

Не создавать отдельный механизм установки breakpoint.

---

## 5. Remove Breakpoint

Если breakpoint уже установлен на адресе функции:

    Remove Breakpoint

должен удалить **обычный breakpoint по этому адресу**.

Использовать существующий:

    clearBreakpoint()

или соответствующий метод текущего `IDebugBackend`.

После удаления breakpoint должен исчезнуть из общего окна **Breakpoints**.

---

## 6. Индикация в Functions

В списке Functions визуально показывать наличие breakpoint.

Например:

    ● 0252  _main
      02B3  _gfx_set_mode
    ● 030C  _gfx_set_palette

Конкретный визуальный символ выбрать в соответствии с существующим UI debugger.

Важно только, чтобы состояние было однозначно видно.

Источник состояния — **существующий список Breakpoints**, а не отдельный флаг внутри Functions.

---

## 7. Синхронизация

После установки breakpoint:

    Functions
        ↓
    Breakpoint command
        ↓
    Breakpoint list
        ↓
    Board

Breakpoint должен пройти существующую синхронизацию до Board.

После удаления — аналогично.

Не дублировать breakpoint state в Functions.

---

## 8. Двойной клик

Если в Functions уже реализована навигация по функции, сохранить её.

Рекомендуемое поведение:

* **двойной клик / обычный клик** — переход к адресу функции в Disassembly;
* **правый клик** — контекстное меню breakpoint.

Не ломать существующую навигацию.

---

## 9. Функции без адреса

Действия Set/Remove Breakpoint должны быть недоступны, если запись Functions не имеет корректного адреса.

Для обычных MAP-функций адрес всегда должен присутствовать.

---

## 10. Тесты

Добавить тесты:

### Test 1 — Set breakpoint on function

Для функции:

    _main = 0x0252

выполнить Set Breakpoint.

Проверить:

    breakpoint exists
    address == 0x0252

### Test 2 — Remove breakpoint

Установить breakpoint на `_main`, затем удалить.

Проверить отсутствие breakpoint.

### Test 3 — Functions ↔ Breakpoints

Breakpoint, установленный другим способом через окно Breakpoints, должен отображаться как установленный в Functions.

И наоборот.

Это важно: Functions не должен иметь собственного состояния breakpoint.

### Test 4 — Board synchronization

Проверить существующим способом, что breakpoint, установленный через Functions, действительно синхронизирован с Board.

### Test 5 — Reload ROM

Проверить существующую семантику breakpoint при загрузке другого ROM.

Не менять её специально ради Functions.

---

## 11. Не делать

В рамках этого дополнения не реализовывать:

* breakpoint по имени функции в Agent API;
* отдельный FunctionBreakpoint;
* отдельное хранение breakpoint state в Functions;
* breakpoint по исходной строке;
* условные breakpoint;
* breakpoint ranges.

Использовать существующий address breakpoint.

---

## 12. Критерий готовности

Функциональность считается готовой, если:

1. В Functions видны функции из MAP.
2. Правый клик по функции позволяет установить breakpoint.
3. Breakpoint устанавливается по адресу функции.
4. Он появляется в общем окне Breakpoints.
5. Он синхронизируется с Board.
6. Повторное действие позволяет снять breakpoint.
7. Состояние breakpoint визуально отображается в Functions.
8. Functions и Breakpoints используют одно состояние.
9. Навигация Functions → Disassembly продолжает работать.
10. Все существующие тесты продолжают проходить.
