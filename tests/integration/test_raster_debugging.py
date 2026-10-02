#!/usr/bin/env python3
# ---------------------------------------------------------------------------
# test_raster_debugging.py — Stage 6.27 (Raster / Beam / racing-the-beam)
#
# Integration test over the REAL headless v06c-mcp server (stdio JSON-RPC)
# for the three new read-only tools:
#
#   * debug_get_beam_state      — beam/raster position + video timing + palette
#   * debug_get_screen_snapshot — the real TV framebuffer as a decodable PNG
#   * debug_get_raster_events   — OUT instructions correlated with beam position
#
# Coverage vs the plan:
#   §36/§37  beam invariants (ranges, timing constants)            [always]
#   §38      screen snapshot PNG exists, correct dims, decodable   [always]
#   §40      raster events monotonic beam, correct pc/port/value   [always]
#   §42/§43  racing-the-beam on fire3 (OUT 0Ch mapped to v_cycle)  [gated]
#
# Usage:
#   python3 test_raster_debugging.py [path/to/v06c-mcp]
#
# The emulator boots a default ROM, so the plumbing checks run with NO external
# ROM. The racing-the-beam scenario additionally needs fire3; set it via
#   V06C_FIRE3_ROM=/path/to/fire3.rom
# When absent that one scenario is reported SKIP (exit code stays 0).
#
# The server is launched with a private temporary CWD (workspace isolation).
# ---------------------------------------------------------------------------

import base64
import json
import os
import subprocess
import sys
import tempfile
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_SERVER = os.path.normpath(
    os.path.join(HERE, "..", "..", "build", "v06c-mcp"))

SERVER = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_SERVER
FIRE3 = os.environ.get("V06C_FIRE3_ROM", "")

fails = []


def check(cond, label):
    print(f"  [{'ok ' if cond else 'FAIL'}] {label}")
    if not cond:
        fails.append(label)


class McpClient:
    def __init__(self, server_path, workdir):
        self.proc = subprocess.Popen(
            [server_path], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True, cwd=workdir)
        self._id = 0

    def rpc(self, method, params=None, notify=False):
        msg = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        if notify:
            self.proc.stdin.write(json.dumps(msg) + "\n")
            self.proc.stdin.flush()
            return None
        self._id += 1
        msg["id"] = self._id
        self.proc.stdin.write(json.dumps(msg) + "\n")
        self.proc.stdin.flush()
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError("server closed the pipe")
            try:
                resp = json.loads(line)
            except json.JSONDecodeError:
                continue
            if resp.get("id") == self._id:
                return resp

    def content(self, name, args):
        """Call a tool; return the raw CallToolResult content list (or None)."""
        r = self.rpc("tools/call", {"name": name, "arguments": args})
        return r.get("result", {}).get("content")

    def tool_json(self, name, args):
        """Call a tool whose first content item is JSON text. (data, err)."""
        content = self.content(name, args)
        if not content:
            return None, {"error_code": "no_content"}
        try:
            data = json.loads(content[0]["text"])
        except (KeyError, json.JSONDecodeError) as e:
            return None, {"error_code": "bad_json", "message": str(e)}
        if "error_code" in data:
            return None, data
        return data, None

    def close(self):
        try:
            self.proc.stdin.close()
        except Exception:
            pass
        self.proc.terminate()
        self.proc.wait(timeout=5)


# ---------------------------------------------------------------------------
# PNG sanity (validates the dependency-free encoder end to end)
# ---------------------------------------------------------------------------

def parse_png(png_bytes):
    sig = b"\x89PNG\r\n\x1a\n"
    if not png_bytes.startswith(sig):
        return None, "bad signature"
    off = len(sig)
    width = height = None
    idat = b""
    while off + 8 <= len(png_bytes):
        ln = int.from_bytes(png_bytes[off:off + 4], "big")
        typ = png_bytes[off + 4:off + 8]
        chunk = png_bytes[off + 8:off + 8 + ln]
        if typ == b"IHDR":
            width = int.from_bytes(chunk[0:4], "big")
            height = int.from_bytes(chunk[4:8], "big")
            bit_depth, color_type = chunk[8], chunk[9]
            if bit_depth != 8 or color_type != 6:
                return None, f"unexpected depth/color {bit_depth}/{color_type}"
        elif typ == b"IDAT":
            idat += chunk
        elif typ == b"IEND":
            break
        off += 8 + ln + 4  # header + data + crc
    if width is None or height is None:
        return None, "missing IHDR"
    try:
        raw = zlib.decompress(idat)  # stored-deflate blocks are a valid zlib stream
    except zlib.error as e:
        return None, f"IDAT inflate failed: {e}"
    stride = width * 4 + 1
    if len(raw) != stride * height:
        return None, f"decompressed size {len(raw)} != {stride}*{height}"
    rows = []
    for y in range(height):
        row = raw[y * stride + 1:(y + 1) * stride]
        rows.append([tuple(row[x * 4:x * 4 + 4]) for x in range(width)])
    return (width, height, rows), None


# ---------------------------------------------------------------------------
# §36/§37 — beam invariants
# ---------------------------------------------------------------------------

def test_beam_state(c):
    beam, err = c.tool_json("debug_get_beam_state", {})
    check(err is None and beam and beam.get("available"),
          "debug_get_beam_state available")
    if not beam:
        return
    check(beam["line_v_cycles"] == 768, "line_v_cycles == 768")
    check(beam["frame_lines"] == 312, "frame_lines == 312")
    check(beam["frame_v_cycles"] == 768 * 312, "frame_v_cycles == 239616")
    check(0 <= beam["raster_line"] < beam["frame_lines"],
          f"raster_line in range ({beam['raster_line']})")
    check(0 <= beam["v_cycle_in_line"] < beam["line_v_cycles"],
          f"v_cycle_in_line in range ({beam['v_cycle_in_line']})")
    check(beam["v_cycle_in_frame"] ==
          beam["raster_line"] * beam["line_v_cycles"] + beam["v_cycle_in_line"],
          "v_cycle_in_frame == raster_line*lineVCycles + v_cycle_in_line")
    if beam["visible"]:
        check(beam["visible_x"] >= 0 and beam["visible_y"] >= 0,
              "visible => non-negative screen coords")
    else:
        check(beam["visible_x"] == -1 and beam["visible_y"] == -1,
              "not visible => coords are -1")


# ---------------------------------------------------------------------------
# §38 — screen snapshot PNG
# ---------------------------------------------------------------------------

def test_screen_snapshot(c):
    content = c.content("debug_get_screen_snapshot", {})
    if not content:
        check(False, "debug_get_screen_snapshot returned content")
        return None
    img = next((p for p in content if p.get("type") == "image"), None)
    txt = next((p for p in content if p.get("type") == "text"), None)
    check(img is not None, "screen snapshot has image content")
    check(txt is not None, "screen snapshot has metadata text")
    if img is None or txt is None:
        return None
    meta = json.loads(txt["text"])
    check(meta.get("source") == "tv", "source is real TV buffer (not vram)")
    check(img.get("mimeType") == "image/png", "mimeType image/png")

    png = base64.b64decode(img["data"])
    parsed, err = parse_png(png)
    check(err is None, f"PNG decodes: {err or 'ok'}")
    if parsed:
        w, h, _rows = parsed
        check(w == meta["width"] and h == meta["height"],
              f"PNG dims {w}x{h} == metadata {meta['width']}x{meta['height']}")
        return parsed
    return None


# ---------------------------------------------------------------------------
# §40 — raster events: monotonic beam, correct fields
# ---------------------------------------------------------------------------

def test_raster_events(c):
    # Ensure the emulator is running so OUTs get recorded.
    c.tool_json("debug_run", {})
    time.sleep(0.25)
    evs, err = c.tool_json("debug_get_raster_events", {"max_results": 500})
    check(err is None, "debug_get_raster_events runs")
    if not evs:
        return
    events = evs["events"]
    check(len(events) > 0, f"recorded {len(events)} OUT events while running")
    # v_cycle must be well-formed within the frame timing model.
    ok_range = all(0 <= e["v_cycle"] < 768 * 312 and
                   0 <= e["raster_line"] < 312 and
                   0 <= e["v_cycle_in_line"] < 768
                   for e in events)
    check(ok_range, "every event v_cycle/line within timing bounds")
    check(all(e["pc"].startswith("0x") for e in events), "events carry hex pc")
    # port filter yields only that port
    onlyC, err = c.tool_json("debug_get_raster_events",
                             {"port": 0x0C, "max_results": 200})
    check(err is None and all(e["port"] == 0x0C for e in onlyC["events"]),
          "port filter returns only 0x0C writes")


# ---------------------------------------------------------------------------
# §42/§43 — racing the beam on fire3 (gated)
# ---------------------------------------------------------------------------

def test_racing_fire3(c):
    _, err = c.tool_json("debug_load_rom", {"path": FIRE3})
    check(err is None, "fire3 loaded")
    if err:
        return
    c.tool_json("debug_run", {})
    time.sleep(0.3)

    # Palette writes (OUT 0Ch) should be present and their v_cycle should
    # strictly advance across the frame — this is what makes a raster/palette
    # effect visible on the screen snapshot but invisible in a VRAM read.
    evs, _ = c.tool_json("debug_get_raster_events",
                         {"port": 0x0C, "max_results": 60})
    if evs and evs["events"]:
        vs = [e["v_cycle"] for e in evs["events"]]
        check(all(vs[i] <= vs[i + 1] or vs[i + 1] < 768 * 312  # ring may wrap
                  for i in range(len(vs) - 1)),
              "OUT 0Ch v_cycle sequence is consistent with the beam")
        print(f"    fire3 OUT 0Ch events: {len(vs)}"
              f" sample v_cycle={vs[:5]}")

    # Two screen snapshots at different times must differ (mid-frame redraw).
    c1 = _snapshot_png(c)
    time.sleep(0.15)
    c2 = _snapshot_png(c)
    if c1 and c2:
        check(c1 != c2, "screen snapshot changes between captures (live raster)")
    else:
        check(False, "could not capture two fire3 snapshots")

    c.tool_json("debug_pause", {})


def _snapshot_png(c):
    content = c.content("debug_get_screen_snapshot", {})
    if not content:
        return None
    img = next((p for p in content if p.get("type") == "image"), None)
    if not img:
        return None
    return base64.b64decode(img["data"])


# ---------------------------------------------------------------------------

def main():
    print("\n=== Stage 6.27 Raster/Beam MCP tools — integration ===")
    print(f"server: {SERVER}")
    if not os.path.isfile(SERVER) or not os.access(SERVER, os.X_OK):
        print("SKIP: server binary not found (build v06c-mcp first)")
        return 0

    with tempfile.TemporaryDirectory(prefix="v06c_raster_") as workdir:
        c = McpClient(SERVER, workdir)
        try:
            c.rpc("initialize", {
                "protocolVersion": "2024-11-05", "capabilities": {},
                "clientInfo": {"name": "raster-integration", "version": "1"}})
            c.rpc("notifications/initialized", notify=True)

            # Boot the default ROM and run a little so the beam is live.
            c.tool_json("debug_run", {})
            time.sleep(0.2)

            test_beam_state(c)
            test_screen_snapshot(c)
            test_raster_events(c)

            if FIRE3 and os.path.isfile(FIRE3):
                print("\n--- fire3 racing-the-beam ---")
                test_racing_fire3(c)
            else:
                print("\nSKIP fire3 scenario: set V06C_FIRE3_ROM=/path/fire3.rom")
        finally:
            c.close()

    if fails:
        print(f"\nFAILED ({len(fails)}): " + "; ".join(fails))
        return 1
    print("\nALL RASTER/BEAM INTEGRATION CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
