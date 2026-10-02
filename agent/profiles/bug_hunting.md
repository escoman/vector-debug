# Bug Hunting Profile

## ID

bug_hunting

## Description

Профиль для поиска ошибок в ROM Vector-06C: статический и динамический аудит, проверка стека, анализ VRAM и I/O на предмет аномалий.

## Workflow Reference

- AI_AGENT_WORKFLOW.md

## Tasks

1. **find_bugs** — общий аудит (memory, control flow, I/O, VRAM)
2. **stack_safety** — проверка целостности стека
3. **analyze_vram** — анализ видеоподсистемы на ошибки
4. **analyze_io** — анализ I/O на аномалии

## Knowledge

- vector06c/cpu
- vector06c/memory
- vector06c/video
- vector06c/io
- vector06c/rom_format
- vector06c/verification

## Recommended Order

1. `find_bugs` — общий обзор проблем
2. `stack_safety` — детальный анализ стека
3. `analyze_vram` — проверка видео
4. `analyze_io` — проверка I/O

## Usage

AI Agent выполняет каждый Task последовательно, агрегируя findings. Результаты классифицируются по severity (critical / probable / suspicious) и confidence (confirmed / probable / suspicious).

### Итоговый отчёт

```markdown
# Bug Hunting Report

## Goal
Find bugs and anomalies in ROM.

## ROM
- File: <name>

## Profile
bug_hunting

## Tasks
find_bugs, stack_safety, analyze_vram, analyze_io

## Findings

### Finding 1
Location:
Observed behavior:
Expected behavior:
Evidence:
Reasoning:
Confidence: high / medium / low
Next verification:

## Verified Facts

## Inferences

## Hypotheses

## Unknowns

## Limitations

## Recommended Next Steps
```
