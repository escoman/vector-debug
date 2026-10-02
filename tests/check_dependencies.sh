#!/usr/bin/env bash
# check_dependencies.sh — verify layer separation (Stage 3.13b)
#
# Rules:
#   1. gui/* (except main.cpp) must NOT include emulator internals
#   2. debugger/src/* must NOT include GUI headers (imgui, SDL)
#   3. backend.cpp must NOT include any emulator headers
#   4. backend.h must NOT include or forward-declare emulator types
#   5. debug_target.h must NOT expose Memory type
#
# Exit 0 if all checks pass, 1 if any violations found.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DBG_DIR="$(dirname "$SCRIPT_DIR")"
GUI_DIR="$DBG_DIR/gui"
SRC_DIR="$DBG_DIR/src"

VIOLATIONS=0

# Emulator-internal headers forbidden in GUI (except main.cpp which is composition root)
EMULATOR_HEADERS="board\.h|memory\.h|io\.h|tv\.h|filler\.h|sound\.h|options\.h|cadence\.h|fd1793\.h|ay\.h|i8080\.h|keyboard\.h|8253\.h|fsimage\.h|glextns\.h|icon\.h|server\.h|shaders\.h|scriptnik\.h|wav\.h|util\.h|tinydir\.h|globaldefs\.h|serialize\.h|vio\.h|resampler\.h|i8080_hal\.h"

# All emulator headers forbidden in backend.cpp and backend.h
ALL_EMULATOR_HEADERS="$EMULATOR_HEADERS"

# GUI headers forbidden in debugger core (src/)
GUI_HEADERS="imgui\.h|SDL\.h|SDL_.*\.h|gui\.h"

echo "=== Dependency Check (Stage 3.13b) ==="
echo ""

# --- Rule 1: GUI files must not include emulator headers ---
echo "Rule 1: gui/* (except main.cpp) must not include emulator headers..."
FOUND=0
for f in "$GUI_DIR"/*.h "$GUI_DIR"/*.cpp; do
    [ -f "$f" ] || continue
    BASENAME="$(basename "$f")"
    # main.cpp is the composition root — allowed to include everything
    [ "$BASENAME" = "main.cpp" ] && continue
    MATCHES=$(grep -nE "#include\s+\"($EMULATOR_HEADERS)\"" "$f" 2>/dev/null || true)
    if [ -n "$MATCHES" ]; then
        echo "  VIOLATION in $BASENAME:"
        echo "$MATCHES" | sed 's/^/    /'
        FOUND=$((FOUND + 1))
    fi
done
if [ "$FOUND" -eq 0 ]; then
    echo "  OK — no violations"
else
    echo "  FAIL — $FOUND file(s) with violations"
    VIOLATIONS=$((VIOLATIONS + FOUND))
fi
echo ""

# --- Rule 2: Core src/ must not include GUI headers ---
echo "Rule 2: src/* must not include GUI headers (imgui, SDL)..."
FOUND=0
for f in "$SRC_DIR"/*.h "$SRC_DIR"/*.cpp; do
    [ -f "$f" ] || continue
    BASENAME="$(basename "$f")"
    MATCHES=$(grep -nE "#include\s+\"($GUI_HEADERS)\"" "$f" 2>/dev/null || true)
    if [ -n "$MATCHES" ]; then
        echo "  VIOLATION in $BASENAME:"
        echo "$MATCHES" | sed 's/^/    /'
        FOUND=$((FOUND + 1))
    fi
done
if [ "$FOUND" -eq 0 ]; then
    echo "  OK — no violations"
else
    echo "  FAIL — $FOUND file(s) with violations"
    VIOLATIONS=$((VIOLATIONS + FOUND))
fi
echo ""

# --- Rule 3: backend.cpp must not include ANY emulator headers ---
echo "Rule 3: backend.cpp must not include any emulator headers..."
FOUND=0
MATCHES=$(grep -nE "#include\s+\"($ALL_EMULATOR_HEADERS)\"" "$SRC_DIR/backend.cpp" 2>/dev/null || true)
if [ -n "$MATCHES" ]; then
    echo "  VIOLATION in backend.cpp:"
    echo "$MATCHES" | sed 's/^/    /'
    FOUND=1
fi
if [ "$FOUND" -eq 0 ]; then
    echo "  OK — no violations"
else
    echo "  FAIL — backend.cpp includes emulator headers"
    VIOLATIONS=$((VIOLATIONS + 1))
fi
echo ""

# --- Rule 4: backend.h must not include or forward-declare emulator types ---
echo "Rule 4: backend.h must not include/forward-declare emulator types..."
FOUND=0
# Check for #include of emulator headers
MATCHES=$(grep -nE "#include\s+\"($ALL_EMULATOR_HEADERS)\"" "$SRC_DIR/backend.h" 2>/dev/null || true)
if [ -n "$MATCHES" ]; then
    echo "  VIOLATION (include) in backend.h:"
    echo "$MATCHES" | sed 's/^/    /'
    FOUND=1
fi
# Check for forward declarations of emulator types
MATCHES=$(grep -nE "^(class|struct)\s+(Board|Memory|IO|TV|PixelFiller|Soundnik|FD1793|AY|I8253|Keyboard)\b" "$SRC_DIR/backend.h" 2>/dev/null || true)
if [ -n "$MATCHES" ]; then
    echo "  VIOLATION (forward-decl) in backend.h:"
    echo "$MATCHES" | sed 's/^/    /'
    FOUND=1
fi
if [ "$FOUND" -eq 0 ]; then
    echo "  OK — no violations"
else
    echo "  FAIL — backend.h has emulator dependencies"
    VIOLATIONS=$((VIOLATIONS + 1))
fi
echo ""

# --- Rule 5: debug_target.h must not expose Memory type ---
echo "Rule 5: debug_target.h must not expose Memory type..."
FOUND=0
MATCHES=$(grep -nE "Memory\s*\*|class\s+Memory|Memory\s*&" "$SRC_DIR/debug_target.h" 2>/dev/null || true)
if [ -n "$MATCHES" ]; then
    echo "  VIOLATION in debug_target.h:"
    echo "$MATCHES" | sed 's/^/    /'
    FOUND=1
fi
if [ "$FOUND" -eq 0 ]; then
    echo "  OK — no violations"
else
    echo "  FAIL — debug_target.h exposes Memory type"
    VIOLATIONS=$((VIOLATIONS + 1))
fi
echo ""

# --- Summary ---
if [ "$VIOLATIONS" -eq 0 ]; then
    echo "=== PASS: 0 violations ==="
    exit 0
else
    echo "=== FAIL: $VIOLATIONS violation(s) ==="
    exit 1
fi
