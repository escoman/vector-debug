# Build wrapper for the Vector-06C debugger.
#
#   make     -> full clean rebuild of the two binaries into build/:
#               cmake -DENABLE_AI_AGENT=ON .. -> make clean ->
#               make v06c-debugger v06c-mcp
#
# The real logic lives in scripts/make_release.sh (single source of truth);
# ENABLE_AI_AGENT=ON must be set at configure time or the v06c-mcp target
# simply does not exist in the generated Makefiles.

.PHONY: all help
.DEFAULT_GOAL := all

## all  : build v06c-debugger + v06c-mcp into build/ (full clean rebuild)
all:
	@bash scripts/make_release.sh

## help : show this help
help:
	@grep -E '^## ' $(firstword $(MAKEFILE_LIST)) | sed -E 's/^## +//'
