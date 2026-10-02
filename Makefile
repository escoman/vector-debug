# Build wrapper for the Vector-06C debugger.
#
#   make          -> full clean rebuild into build/ (v06c-debugger + v06c-mcp)
#   make release  -> build, then package a Linux release artifact into release/
#
# The real logic lives in scripts/make_release.sh (build) and
# scripts/package_release.sh (packaging) — single source of truth shared with CI.
# ENABLE_AI_AGENT=ON must be set at configure time or the v06c-mcp target simply
# does not exist in the generated Makefiles.

VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo 0.0.0-dev)

.PHONY: all release help
.DEFAULT_GOAL := all

## all     : build v06c-debugger + v06c-mcp into build/ (full clean rebuild)
all:
	@bash scripts/make_release.sh

## release : build, then package Linux artifact release/vector-debug-$(VERSION)-linux-*.tar.gz
release: all
	@bash scripts/package_release.sh linux "$(VERSION)"

## help    : show this help
help:
	@grep -E '^## ' $(firstword $(MAKEFILE_LIST)) | sed -E 's/^## +//'
