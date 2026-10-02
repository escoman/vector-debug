# Changelog

All notable changes to this project are documented in this file.
Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning: [Semantic Versioning](https://semver.org/).

<!--
The top-most `## [X.Y.Z]` section is used as the GitHub Release notes body
(see the "Build release notes from CHANGELOG" step in .github/workflows/release.yml).
Prepend new versions at the top; keep this first HTML comment free of `## [` lines.
-->

## [1.0.0] - 2026-10-02

First public release of the Vector-06C debugger and its MCP server.

### Added
- Vector-06C (КР580ВМ80А / Intel 8080) emulator with a Dear ImGui docking debugger UI.
- `v06c-mcp` — MCP server for AI agents (stdio transport) with ROM analysis tools.
- CI release pipeline (GitHub Actions): native Linux and Windows builds, artifact
  packaging, and a GitHub Release published on `v*` tags.
- `make release` — local Linux release packaging into `release/`.
- Tuned workspace preset `workspaces/Default.ini` shipped in every release bundle.
