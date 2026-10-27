# RDB Format Specification

**Version**: 2 (v1 files remain loadable; v2 only adds the optional `protocol`, `ports`, `watchlist` sections)
**Canonical status**: This document is the single authoritative description of the RDB file format.

---

## Overview

RDB (ROM Database) is a JSON-based format for storing analysis results of Vector-06C ROMs.

```
MAP = source (import data)
RDB = database (working store)
API = the only way to work with RDB
JSON = internal storage format
```

File extension: `.rdb`

---

## Top-Level Structure

```json
{
  "format": "rdb",
  "platform": "vector06c",
  "version": 2,
  "rom": {
    "file": "/path/to/rom.rom",
    "size": 16384,
    "sha256": "e3b0c44298fc1c14..."
  },
  "objects": [
    ...
  ],
  "protocol": [ ... ],
  "ports": [ ... ],
  "watchlist": [ ... ]
}
```

### Required Fields

| Field | Type | Description |
|-------|------|-------------|
| `format` | string | Always `"rdb"` |
| `platform` | string | Target platform, e.g. `"vector06c"` |
| `version` | integer | Format version. Loader accepts `<= 2`. `1` = objects only; `2` = adds `protocol`/`ports`/`watchlist` |
| `rom` | object | ROM identity (see below) |
| `objects` | array | RDB objects (may be empty) |

The `protocol`, `ports`, `watchlist` sections are **optional**. A `version: 1` file (or a v2
file that omits them) loads with those sections empty and does **not** become dirty.
The controller writes `version: 2` on save **only if at least one knowledge section is
non-empty**; otherwise it preserves the file's original version (never silently upgrades a
touch-free file). `version > 2` is rejected on load.

---

## ROM Identity

Identifies the ROM file this database belongs to.

```json
{
  "rom": {
    "file": "rom.rom",
    "size": 16384,
    "sha256": "e3b0c44298fc1c14..."
  }
}
```

| Field | Type | Description |
|-------|------|-------------|
| `file` | string | ROM filename (name only, not full path) |
| `size` | integer | File size in bytes |
| `sha256` | string | SHA-256 hex digest of the ROM file |

Used to verify that the RDB matches the currently loaded ROM.
Identity comparison uses only `size` and `sha256` (not `file`), so ROM+RDB can be moved freely between directories and computers.

---

## Objects

Each object represents a labeled entity in the ROM: function, variable, data block, etc.

```json
{
  "address": "0x1234",
  "type": "function",
  "name": "draw_sprite",
  "size": 47,
  "comment": "Draws a sprite on screen",
  "properties": {
    "source_file": "main.c",
    "source_line": "42"
  },
  "links": [
    "0x2000",
    "0x3000"
  ]
}
```

### Object Fields

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `address` | string | yes | Hex address, e.g. `"0x1234"` |
| `type` | string | yes | Object type (see Types below) |
| `name` | string | yes | Symbol name |
| `size` | integer | no | Size in bytes. Omit or `0` if unknown |
| `comment` | string | no | User comment. Omit if empty |
| `properties` | object | no | Key-value pairs (see Properties below) |
| `links` | array | no | Addresses of linked objects (see Links below) |

### Address Format

Addresses are stored as hex strings with `0x` prefix:

```json
"address": "0x1234"
```

Address range: `0x0000` to `0xFFFF` (16-bit address space).

Each address is unique — at most one object per address.

---

## Types

| Type string | Description |
|-------------|-------------|
| `function` | Subroutine (entry point with RET) |
| `variable` | Named memory location |
| `data` | Data block |
| `table` | Lookup table |
| `string` | String constant |
| `code` | Code fragment (not a full function) |
| `label` | Generic label |
| `unknown` | Unclassified |

Type comparison is case-insensitive on read. Stored as lowercase.

---

## Properties

Arbitrary key-value pairs attached to an object.

```json
"properties": {
  "source_file": "main.c",
  "source_line": "42",
  "parameters": "x, y"
}
```

- Keys are strings.
- Values are stored as strings in the JSON.
- Properties are optional. Omit if empty.
- Platform-specific metadata goes here (e.g. source locations, parameter info).

---

## Links

Semantic references from one object to target addresses.

```json
"links": [
  "0x2000",
  "0x3000"
]
```

### Link Rules

1. Links contain **addresses only** — no embedded name/type info.
2. Target info is found in the RDB object with the matching address.
3. Links are **optional** — omit if no links exist (no empty arrays).
4. Links are **directed**: `A → B` does not imply `B → A`.
5. Target object may not exist yet (unresolved link is allowed).
6. No duplicate links: `A → B` appears at most once.
7. Self-links are allowed: `A → A`.
8. Cyclic links are allowed: `A → B → C → A`.

### Link Semantics

RDB links are **semantic data** created by AI Agent analysis.
They represent confirmed relationships between objects (e.g. call targets, data references).

Links must be created by the Agent when confirmed control/data flow is discovered
during analysis (e.g. `CALL target`, `JMP target`, known function references).
Links should not be created based on address proximity or unverified hypotheses.

`.rdb.graph` files are **visual data** used by Call Graph.
`.rdb.graph` is **not** a replacement for RDB links.
Links are never saved to `.rdb.graph` — only to `.rdb`.
Call Graph visualizes existing RDB links — it does not create them.

### Object Removal and Links

- Removing source object: outgoing links are removed with it.
- Removing target object: incoming links from other objects are **preserved** (become unresolved).

### Example

```json
{
  "address": "0x1000",
  "type": "function",
  "name": "start",
  "links": ["0x2000", "0x3000"]
}
```

Means: `start` calls/references objects at `0x2000` and `0x3000`.

---

## Knowledge Sections (v2)

Version 2 promotes RDB from "a database of ROM objects" to "a knowledge base about a ROM".
Three top-level sections live next to `objects`. They are pure **data**: the controller never
interprets platform specifics — Vector-06C semantics live in the values, not in code branches.

### `protocol` — hypothesis journal (append-only)

Empirical RE findings with a provable status. At session start the agent reads the active
entries and does **not re-verify** what is already confirmed or refuted.

```json
{
  "id": "P-0001",
  "timestamp": "2026-10-27T14:32:10Z",
  "thesis": "Keyboard port 0x01 is active-low: pressed key = 0-bit",
  "status": "VERIFIED_BY_TRACE",
  "scope": "hardware",
  "address": "0x1234",
  "method": "trace",
  "evidence": "breakpoint @0x1234: after press_key('X') reader saw bit 2 = 0",
  "links": ["port:0x01"],
  "supersedes": null,
  "superseded_by": null
}
```

| Field | Required | Type | Notes |
|-------|:---:|------|-------|
| `id` | yes | string | `P-NNNN`, assigned by the controller, monotonic; counter resumes after load |
| `timestamp` | yes | string | ISO-8601 UTC, assigned by the controller |
| `thesis` | yes | string | one-line claim, normalized via `normalizeRdbText`; empty after normalize → rejected |
| `status` | yes | enum | closed set below; unknown value → `InvalidArgument` |
| `scope` | yes | enum | `rom` / `hardware` / `tooling` (default `rom`) |
| `address` | no | hex/null | ROM address binding when present |
| `method` | \* | enum | `trace` / `diff` / `primary-source` / `golden-test` / `static` |
| `evidence` | \* | string | the concrete proof, normalized |
| `links` | no | array | ids / promoted-target refs (see Promotion below) |
| `supersedes` | no | id/null | this entry replaces that one |
| `superseded_by` | no | id/null | set by the controller when this entry is replaced |

**Statuses** (borrowed from `vector06c/verification.md`):
`VERIFIED_BY_TRACE`, `VERIFIED_BY_SOURCE`, `VERIFIED`, `PARTIALLY_VERIFIED`, `CONFLICT`,
`UNVERIFIED`, `REFUTED`, `SPECULATION`.

**Evidence rule (\*):** the controller refuses to store a `VERIFIED*` / `PARTIALLY_VERIFIED` /
`CONFLICT` / `REFUTED` entry without a valid `method` **and** non-empty `evidence`. A journal
without proof degrades into rumor, and trusting plausible-looking rumor is exactly the L1 bug.

**Append-only:** history is never rewritten. To refute an old claim, `supersede` it: the new
entry gets `supersedes = oldId`, the old entry gets `superseded_by = newId` and becomes inactive.
`removeProtocolEntry` is allowed **unless** another active entry still points at it via
`supersedes` (removing a referenced entry → `Referenced` error).

**Scopes & ROM identity:** `scope: rom` claims must be re-verified when the loaded ROM's
`sha256` differs (`RdbController::matchesRom` / `debug_get_rdb_info.rom_sha256`). `hardware` /
`tooling` claims are ROM-independent and stay valid across ROMs.

### `ports` — port / register polarity table

Compiled knowledge (lesson L1: "port polarity ate 6 days"). The authoritative source for P5
disassembler auto-comments and a meaningful I/O view. Unique per `port` number.

```json
{
  "port": "0x01",
  "name": "keyboard_strobe",
  "direction": "input",
  "polarity": "active-low",
  "bits": [
    { "bit": 0, "meaning": "K1 / KEY0" },
    { "bit": 7, "meaning": "shift/CAPS" }
  ],
  "comment": "pressed key = 0-bit; injector must invert the mask"
}
```

`direction` ∈ `input` / `output` / `in-out`; `polarity` ∈ `active-high` / `active-low` / `n/a`;
unknown values → `InvalidArgument`. `bits` ≤ 8 entries, each `{bit: 0..7, meaning}`. `name` /
`comment` / bit meanings are normalized. `upsertPort` is idempotent: re-inserting an identical
row does not set `dirty`.

### `watchlist` — observed variables

Lesson L4: a checkpoint must print named variable values, not just PC. Keyed by `name`
(re-adding the same name updates in place, no duplicate growth).

```json
{
  "name": "score",
  "address": "0x2010",
  "length": 2,
  "format": "uint16-le",
  "object_address": "0x2010"
}
```

`format` ∈ `uint8` / `int8` / `uint16-le` / `bcd` / `bits` / … (default `uint8`). `object_address`
is an optional link to an existing `Variable` object — the variable data stays in `objects`,
the watchlist only records "print at stop + format". Actual checkpoint printing is backend work,
out of scope for RDB storage.

### Promotion pipeline (protocol → ports / comments)

The single legal path from "draft" (journal) to "table" (compiled knowledge):

```
protocol entry (VERIFIED_BY_TRACE / VERIFIED_BY_SOURCE)
    │  promotion
    ├─ scope=hardware        → a `ports` row
    └─ scope=rom + address   → an `objects` comment / property
```

Promotion **does not delete** the protocol entry — it records the promoted target in `links`
(e.g. `"port:0x01"`) so the "when / proven by what" history survives. You must not write
polarity knowledge straight into `ports` bypassing the journal. Exception: an initial
canonical Vector port table bulk-import is recorded as one `VERIFIED_BY_SOURCE` foundation
entry.

---

## Dirty / Save Semantics

- RDB tracks a **dirty flag** internally.
- Modifications set `dirty = true` only if data actually changed.
- Reading data does not affect dirty.
- `save()` writes to disk only if `dirty = true`.
- `save()` uses **atomic save**: write to `.tmp`, then rename to target path.
- Clean RDB is not rewritten unnecessarily.

### Workflow

```
load → inspect → multiple modifications → verify → save → verify
```

Do **not** save after every single operation. Batch modifications, then save.

After modifying objects and/or links, RDB **must** be saved via `debug_save_rdb`.
The Agent must not consider ROM mapping complete until RDB is saved successfully.

---

## File Locations

For a ROM at `/path/to/game.rom`:

| File | Purpose |
|------|---------|
| `/path/to/game.rom` | ROM binary |
| `/path/to/game.map` | Z88DK MAP file (optional import source) |
| `/path/to/game.rdb` | ROM Database (primary store) |

---

## Priority: RDB over MAP

1. If `.rdb` exists and loads successfully → use it. Do not re-import MAP.
2. If `.rdb` does not exist but `.map` exists → auto-import MAP into RDB.
3. If neither exists → empty in-memory RDB.

MAP is the **initial data source only**. RDB is the **working database**.

---

## API Access

RDB is accessed exclusively through the API:

```
GUI / Agent API / MCP → RDB Controller → .rdb file
```

- No direct JSON access through any API.
- No text editing of `.rdb` files.
- No generating `.rdb` content outside the Controller.

### MCP Tools

| Tool | Purpose |
|------|---------|
| `debug_get_rdb_info` | Database metadata and ROM identity |
| `debug_list_rdb_objects` | List all objects (with optional limit) |
| `debug_get_rdb_object` | Get object by address |
| `debug_find_rdb_object` | Find object by name |
| `debug_add_rdb_object` | Create new object |
| `debug_update_rdb_object` | Modify existing object |
| `debug_remove_rdb_object` | Delete object |
| `debug_set_rdb_comment` | Set comment |
| `debug_set_rdb_property` | Set property |
| `debug_save_rdb` | Save to disk |
| `debug_reload_rdb` | Reload from disk |
| `debug_add_rdb_link` | Add directed link from source to target |
| `debug_remove_rdb_link` | Remove directed link |
| `debug_get_rdb_links` | Get links from source object |
| `debug_add_protocol_entry` | Append a hypothesis to the protocol journal |
| `debug_supersede_protocol_entry` | Replace an entry, preserving history |
| `debug_get_protocol` | Query the journal (substring/status/scope/address/active_only/max_entries) |
| `debug_update_protocol_status` | Change an entry's status (+ method/evidence) in place |
| `debug_remove_protocol_entry` | Delete an entry (refused if still referenced) |
| `debug_upsert_port` | Insert/replace a port-polarity row |
| `debug_get_ports` | List the port table (ascending) |
| `debug_add_watch_entry` | Add/update a watchlist variable (by name) |
| `debug_remove_watch_entry` | Remove a watchlist variable |
| `debug_list_watch` | List the watchlist (ascending by address) |
