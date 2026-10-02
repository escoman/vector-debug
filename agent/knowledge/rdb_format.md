# RDB Format Specification

**Version**: 1
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
  "version": 1,
  "rom": {
    "file": "/path/to/rom.rom",
    "size": 16384,
    "sha256": "e3b0c44298fc1c14..."
  },
  "objects": [
    ...
  ]
}
```

### Required Fields

| Field | Type | Description |
|-------|------|-------------|
| `format` | string | Always `"rdb"` |
| `platform` | string | Target platform, e.g. `"vector06c"` |
| `version` | integer | Format version, currently `1` |
| `rom` | object | ROM identity (see below) |
| `objects` | array | RDB objects (may be empty) |

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
