// ---------------------------------------------------------------------------
// vdb_version.h — build provenance for the MCP server (and anything that
// wants it).
//
// The build systems stamp every v06c-mcp compilation with:
//   VDB_GIT_HASH   short commit hash  (CMake: git rev-parse --short HEAD)
//   VDB_GIT_DIRTY  "clean" | "dirty"  (uncommitted changes at configure time)
//   VDB_BUILD_TIME configure-time UTC timestamp
//   VDB_BUILD_SEQ  monotonically increasing counter, one increment per
//                  configure/build, persisted in <build-dir>/.build_seq
//
// `make` (scripts/make_release.sh) re-runs cmake configure on every build,
// so the sequence grows with each release rebuild and a running server can
// always be pinned to an exact checkout + build number. That distinguishes
// a "stale server process running an old binary" (the Oct-10 incident: an
// IDE session started v06c-mcp minutes before the rebuild and kept the
// deleted inode) from a genuine regression.
//
// Defaults below keep every consumer (GUI, unit tests, ad-hoc g++ builds)
// compilable without the -D flags.
// ---------------------------------------------------------------------------

#ifndef VDB_VERSION_H
#define VDB_VERSION_H

// Semantic version of the debugger/MCP build.
#ifndef VDB_VERSION
#define VDB_VERSION "0.6.4"
#endif

// Short git hash the binary was configured from.
#ifndef VDB_GIT_HASH
#define VDB_GIT_HASH "unknown"
#endif

// "clean", "dirty" or "unknown" (not a git checkout at configure time).
#ifndef VDB_GIT_DIRTY
#define VDB_GIT_DIRTY "unknown"
#endif

// Wall clock of compilation; per-TU __DATE__ __TIME__ fallback so loose
// builds still report something plausible.
#ifndef VDB_BUILD_TIME
#define VDB_BUILD_TIME __DATE__ " " __TIME__
#endif

// Build sequence number (0 = built without the stamping build system).
#ifndef VDB_BUILD_SEQ
#define VDB_BUILD_SEQ 0
#endif

// Stringify a numeric macro (e.g. the build sequence) for printf/logging.
#define VDB_STRINGIFY(x) #x
#define VDB_TOSTRING(x)  VDB_STRINGIFY(x)

#endif // VDB_VERSION_H
