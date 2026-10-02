#!/bin/bash
# Build release binaries: v06c-debugger + v06c-mcp.
#
# Run from anywhere: the script resolves the build/ directory relative to the
# repo root (the parent of scripts/), so `..` still points at the top-level
# CMakeLists.txt after the cd into build/.
set -e

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT/build"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# v06c-mcp target only exists when the AI agent is enabled at configure time,
# so ENABLE_AI_AGENT=ON must be set BEFORE make clean (which relies on the
# generated Makefiles).
cmake -DENABLE_AI_AGENT=ON ..
make clean
make v06c-debugger v06c-mcp -j"$(nproc)"
