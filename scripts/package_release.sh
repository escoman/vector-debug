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
# and its archive (.tar.gz on linux, .zip on windows). All three built binaries
# ship: v06c-debugger, v06c-mcp, v06c-asm-export. The tuned workspace preset
# is always shipped so the debugger starts with the docking layout out of the box.
# On linux a relocatable v06c-debugger.desktop launcher (+ icon) is bundled for
# desktop integration. README.md is intentionally NOT shipped (build-oriented).
# The staged executables are compressed with the UPX tool vendored under upx/.
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
EXPORTER="$BUILD_DIR/v06c-asm-export$EXT"
for b in "$DEBUGGER" "$MCP" "$EXPORTER"; do
  [ -f "$b" ] || { echo "missing $b — build first (run 'make')" >&2; exit 1; }
done

NAME="vector-debug-${VERSION}-${PLATFORM}-${ARCH}"
STAGE="$OUT_DIR/$NAME"
rm -rf "$STAGE"
mkdir -p "$STAGE/workspaces"

cp "$DEBUGGER" "$MCP" "$EXPORTER" "$STAGE/"

# ---------------------------------------------------------------------------
# Compress the staged executables with UPX — the vendored tool lives under
# upx/ in this repo (upx/upx for the ELF build, upx/upx.exe for the MinGW
# build). Debug-built binaries shrink to roughly a quarter (e.g. the ~20 MB
# debugger -> ~5 MB). This runs on the STAGE copies only, so the build/ tree
# stays uncompressed for development/tests. Graceful: if the tool is absent or
# not runnable, warn and ship the uncompressed binaries rather than fail.
# ---------------------------------------------------------------------------
if [ "$PLATFORM" = "windows" ]; then
  UPX="$ROOT/upx/upx.exe"
  upx_ok() { [ -f "$UPX" ]; }          # Windows runs .exe regardless of the +x bit
else
  UPX="$ROOT/upx/upx"
  upx_ok() { [ -x "$UPX" ]; }
fi

if upx_ok; then
  for exe in "$STAGE/v06c-debugger$EXT" "$STAGE/v06c-mcp$EXT" "$STAGE/v06c-asm-export$EXT"; do
    before=$(stat -c%s "$exe" 2>/dev/null || echo 0)
    if "$UPX" --best --force "$exe" >/dev/null 2>&1; then
      after=$(stat -c%s "$exe" 2>/dev/null || echo 0)
      echo "UPX: $(basename "$exe")  ${before} -> ${after} bytes"
    else
      echo "warn: UPX failed on $(basename "$exe"), shipping it uncompressed" >&2
    fi
  done
else
  echo "warn: UPX not found at '$UPX', shipping uncompressed binaries" >&2
fi

# Ship the tuned workspace preset (tracked at repo-root workspaces/).
[ -f "$ROOT/workspaces/Default.ini" ] && cp "$ROOT/workspaces/Default.ini" "$STAGE/workspaces/" || true

# Ship the bundled font assets (tracked at repo-root res/fonts/) next to the
# executables — the GUI loads DejaVuSansMono.ttf via SDL_GetBasePath()+"fonts/".
if [ -d "$ROOT/res/fonts" ]; then
  mkdir -p "$STAGE/fonts"
  cp "$ROOT"/res/fonts/*.ttf "$STAGE/fonts/" 2>/dev/null || true
fi

# Ship the About/splash logo next to the executables — it is NOT embedded in the
# binary; the GUI loads it at runtime via SDL_GetBasePath()+"logo16.png"
# (see AboutWindow::resolveLogoPath). Without this copy the release bundle shows
# the About window with a placeholder box instead of the logo.
[ -f "$ROOT/res/logo16.png" ] && cp "$ROOT/res/logo16.png" "$STAGE/" || true

# Linux: bundle a relocatable .desktop launcher + icon. The build-time CMake
# version embeds absolute /build paths; rewrite Exec to resolve against the
# .desktop file itself (%k field code) and use a theme Icon name instead of an
# absolute path, so the extracted tarball works from any location.
if [ "$PLATFORM" = "linux" ] && [ -f "$ROOT/res/v06c-debugger.desktop.in" ]; then
  sed -e 's#@CMAKE_CURRENT_BINARY_DIR@#"$(dirname "%k")"#' \
      -e 's#^Icon=@CMAKE_CURRENT_SOURCE_DIR@/res/icon64.png#Icon=v06c-debugger#' \
      "$ROOT/res/v06c-debugger.desktop.in" > "$STAGE/v06c-debugger.desktop"
  [ -f "$ROOT/res/icon64.png" ] && cp "$ROOT/res/icon64.png" "$STAGE/icon64.png" || true
fi

# Windows: bundle the MinGW/SDL2 runtime DLLs next to the executables so the
# self-contained folder runs without a package manager. In MSYS2 MINGW64 the
# runtime DLLs live under $MINGW_PREFIX/bin (i.e. /mingw64/bin), NOT on PATH,
# so `command -v` cannot find them — resolve them from the prefix directly.
# Each logical dependency may ship under different file names across MSYS2
# versions (e.g. SDL2 renamed its runtime from libSDL2-2.0-0.dll to SDL2.dll),
# so try a list of candidates per dependency and copy the first that exists.
if [ "$PLATFORM" = "windows" ]; then
  DLL_DIR="${MINGW_PREFIX:-/mingw64}/bin"
  copy_dll() {   # $1..: candidate file names; copies the first one found
    for cand in "$@"; do
      if [ -f "$DLL_DIR/$cand" ]; then
        cp "$DLL_DIR/$cand" "$STAGE/"
        return 0
      fi
    done
    return 1
  }
  for group in "SDL2.dll libSDL2-2.0-0.dll" "libgcc_s_seh-1.dll" "libwinpthread-1.dll" "libstdc++-6.dll"; do
    # shellcheck disable=SC2086  # intentional word-splitting into candidates
    copy_dll $group || echo "warn: none of [$group] found in $DLL_DIR, skipping" >&2
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
