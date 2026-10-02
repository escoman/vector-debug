# Reverse Engineering Profile

## ID

reverse_engineering

## Description

Профиль для реверс-инжиниринга ROM Vector-06C: построение карты символов, анализ видеоподсистемы и I/O для понимания структуры и поведения программы.

## Workflow Reference

- AI_AGENT_WORKFLOW.md

## Tasks

1. **generate_map** — построение и уточнение карты функций/меток
2. **analyze_vram** — анализ видеоданных и паттернов отрисовки
3. **analyze_io** — анализ ввода-вывода и взаимодействия с оборудованием

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

1. `generate_map` — основа для всего последующего анализа
2. `analyze_vram` — понимание графической части
3. `analyze_io` — понимание взаимодействия с оборудованием

## Usage

AI Agent начинает с `generate_map` для построения структуры ROM. Затем `analyze_vram` и `analyze_io` помогают понять, как программа взаимодействует с видеоподсистемой и периферией.

### Итоговый отчёт

```markdown
# Reverse Engineering Report

## Goal
Understand ROM structure and subsystem behavior.

## ROM
- File: <name>

## Profile
reverse_engineering

## Tasks
generate_map, analyze_vram, analyze_io

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
