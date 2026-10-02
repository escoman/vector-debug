# ROM Audit Profile

## ID

rom_audit

## Description

Комплексная проверка ROM: объединяет основные задачи анализа для полной верификации ROM-образа Vector-06C.

## Workflow Reference

- AI_AGENT_WORKFLOW.md

## Tasks

1. **generate_map** — создание карты функций, меток и областей данных
2. **stack_safety** — проверка корректности использования стека
3. **find_bugs** — общий аудит (memory, control flow, I/O, VRAM)
4. **analyze_vram** — анализ видеоподсистемы
5. **analyze_io** — анализ ввода-вывода

## Knowledge

- vector06c/architecture
- vector06c/cpu
- vector06c/memory
- vector06c/video
- vector06c/io
- vector06c/keyboard
- vector06c/sound
- vector06c/rom_format
- vector06c/verification

## Recommended Order

1. `generate_map` — сначала построить карту символов
2. `stack_safety` — проверить стек
3. `analyze_io` — проверить I/O
4. `analyze_vram` — проверить видео
5. `find_bugs` — финальный общий аудит

## Usage

AI Agent читает этот профиль, затем последовательно выполняет каждый Task, используя общую Knowledge Base. Результаты каждого Task агрегируются в итоговый отчёт.

### Итоговый отчёт

```markdown
# ROM Audit Report

## Goal
Full ROM verification and integrity audit.

## ROM
- File: <name>
- Size: <bytes>

## Profile
rom_audit

## Tasks
generate_map, stack_safety, find_bugs, analyze_vram, analyze_io

## Findings

### Finding 1
Location:
Observation:
Evidence:
Conclusion:
Confidence: high / medium / low

## Verified Facts

## Inferences

## Hypotheses

## Unknowns

## Limitations

## Recommended Next Steps
```
