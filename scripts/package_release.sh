#!/usr/bin/env bash
# Package built binaries into a per-platform release artifact.
#
#   scripts/package_release.sh <platform> [version] [build_dir] [out_dir]
#     platform : linux | windows
#     version  : e.g. 0.1.0            (default: 0.0.0-dev)
#     build_dir: where v06c-* live      (default: <repo>/build)
#     out_dir  : where the archive goes (default: <repo>/release)
#
# Produces release/vector-debug-<version>-<platform>-<arch>/{binaries,workspaces/Default.ini,...}
# and its archive (.tar.gz on linux, .zip on windows). The tuned workspace preset
# is always shipped so the debugger starts with the docking layout out of the box.
#
# Shared by local `make release` (linux) and CI (both platforms) — one packaging
# logic for every path.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PLATFORM="${1:-linux}"
VERSION="${2:-0.0.0-dev}"
BUILD_DIR="${3:-$ROOT/build}"
OUT_DIR="${4:-$ROOT/release}"

case "$PLATFORM" in
  linux)   EXT="";   ARCH="$(uname -m)";   ARCHIVE=tar ;;
  windows) EXT=".exe"; ARCH="x64";          ARCHIVE=zip ;;
  *) echo "unknown platform: $PLATFORM (expected linux|windows)" >&2; exit 2 ;;
esac

DEBUGGER="$BUILD_DIR/v06c-debugger$EXT"
MCP="$BUILD_DIR/v06c-mcp$EXT"
for b in "$DEBUGGER" "$MCP"; do
  [ -f "$b" ] || { echo "missing $b — build first (run 'make')" >&2; exit 1; }
done

NAME="vector-debug-${VERSION}-${PLATFORM}-${ARCH}"
STAGE="$OUT_DIR/$NAME"
rm -rf "$STAGE"
mkdir -p "$STAGE/workspaces"

cp "$DEBUGGER" "$MCP" "$STAGE/"

# Ship the tuned workspace preset (tracked at repo-root workspaces/) and README.
[ -f "$ROOT/workspaces/Default.ini" ] && cp "$ROOT/workspaces/Default.ini" "$STAGE/workspaces/" || true
[ -f "$ROOT/README.md" ] && cp "$ROOT/README.md" "$STAGE/" || true

# Windows: bundle the MinGW/SDL2 runtime DLLs next to the executables so the
# self-contained folder runs without a package manager. Best-effort by design.
if [ "$PLATFORM" = "windows" ]; then
  for dll in libSDL2-2.0-0.dll libgcc_s_seh-1.dll libwinpthread-1.dll libstdc++-6.dll; do
    p="$(command -v "$dll" 2>/dev/null || true)"
    if [ -n "$p" ]; then cp "$p" "$STAGE/"; else echo "warn: $dll not found on PATH, skipping" >&2; fi
  done
fi

mkdir -p "$OUT_DIR"
if [ "$ARCHIVE" = "zip" ]; then
  command -v zip >/dev/null || { echo "'zip' required to package windows" >&2; exit 1; }
  ( cd "$OUT_DIR" && zip -qr "$NAME.zip" "$NAME" )
  ARTIFACT="$OUT_DIR/$NAME.zip"
else
  ( cd "$OUT_DIR" && tar -czf "$NAME.tar.gz" "$NAME" )
  ARTIFACT="$OUT_DIR/$NAME.tar.gz"
fi

echo "Packaged: $ARTIFACT"
