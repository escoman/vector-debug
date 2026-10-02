#!/usr/bin/env python3
# ---------------------------------------------------------------------------
# test_stage626_integration.py — Stage 6.26
#
# Integration test for the MCP batch analysis tools over the REAL v06c-mcp
# server (stdio JSON-RPC) and real ROM images:
#
#   * debug_disassemble_image      (+ equivalence: image == N x disassemble_range)
#   * debug_coverage_report
#   * debug_diff_memory            (+ dense-diff completeness, no silent cut)
#   * debug_find_bytecode_sequence (+ mask, limit checks)
#   * debug_find_immediate_in_range
#   * debug_get_vram_bytes
#
# Usage:
#   python3 test_stage626_integration.py [path/to/v06c-mcp]
#
#   Server binary default: <repo>/debugger/build/v06c-mcp (resolved relative
#   to this script).  ROM paths: override via environment
#     V06C_PUTUP_ROM, V06C_TESTAY_ROM
#   Missing ROMs are reported as SKIP (exit code 0), so this test is safe to
#   run on machines without the vector-games checkout.
#
# The server is launched with a private temporary CWD so it cannot touch
# build/workspaces/ or user configuration (workspace isolation rule).
# ---------------------------------------------------------------------------

import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_SERVER = os.path.normpath(
    os.path.join(HERE, "..", "..", "build", "v06c-mcp"))

DEFAULT_ROMS = [
    ("putup.rom", os.environ.get(
        "V06C_PUTUP_ROM",
        "/home/alexey/Projects/vector-games/roms/redesign/putup/src/putup.rom")),
    ("TESTAY.ROM", os.environ.get(
        "V06C_TESTAY_ROM",
        "/home/alexey/Projects/vector-games/roms/redesign/testay/src/TESTAY.ROM")),
]

SERVER = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_SERVER


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

    def tool(self, name, args):
        """Call a tool. Returns (data, error_or_None)."""
        r = self.rpc("tools/call", {"name": name, "arguments": args})
        res = r["result"]
        data = json.loads(res["content"][0]["text"])
        if res.get("isError") or "error_code" in data:
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
# assertions
# ---------------------------------------------------------------------------

fails = []
def check(cond, label):
    print(f"  [{'ok ' if cond else 'FAIL'}] {label}")
    if not cond:
        fails.append(label)


def test_batch_disassembly(c):
    # §5: image disassembly + §20 equivalence with a chain of ranges
    img, err = c.tool("debug_disassemble_image", {"address": 0x0100, "length": 8192})
    check(err is None and img["instruction_count"] > 0,
          "debug_disassemble_image 8K")
    if err:
        check(False, f"  image error: {err}")
        return
    chunked = []
    off = 0
    while off < 8192:
        n = min(1024, 8192 - off)
        part, err = c.tool("debug_disassemble_range",
                           {"address": 0x0100 + off, "size": n})
        if err:
            break
        chunked.extend(part["instructions"])
        # continue the chain exactly where the linear sweep stopped
        off += sum(i["size"] for i in part["instructions"]) or n
    same = (len(chunked) == img["instruction_count"] and
            all(a == b for a, b in zip(chunked, img["instructions"])))
    check(same, "image == chain of debug_disassemble_range")

    # §16: 64K dense sweep must be a structured error, never a silent cut
    _, err = c.tool("debug_disassemble_image", {"address": 0x0000, "length": 65536})
    check(err and err["error_code"] == "limit_exceeded",
          "64K sweep -> limit_exceeded")


def test_coverage(c):
    cov, err = c.tool("debug_coverage_report",
                      {"start_address": 0x0100, "range_start": 0x0000,
                       "range_length": 0x8000, "max_instructions": 10000})
    check(err is None, "debug_coverage_report runs")
    if not cov:
        return
    check(len(cov["code_ranges"]) >= 1 and cov["stats"]["code_bytes"] > 0,
          "coverage has code ranges")
    for key in ("uncovered_ranges", "branch_targets",
                "uncovered_branch_targets", "stats"):
        check(key in cov, f"coverage.{key}")
    print(f"    ranges={len(cov['code_ranges'])}"
          f" uncovered={len(cov['uncovered_ranges'])}"
          f" targets={len(cov['branch_targets'])}"
          f" uncovered_targets={len(cov['uncovered_branch_targets'])}"
          f" cov={cov['stats']['coverage_percent']}%")


def test_sequence_search(c):
    # OUT 0Bh (D3 0B) port signature; no interpretation by MCP (§8)
    seq, err = c.tool("debug_find_bytecode_sequence",
                      {"range_start": 0x0000, "range_end": 0x7FFF,
                       "pattern": [0xD3, 0x0B]})
    check(err is None, "find_bytecode_sequence OUT 0Bh")
    if seq:
        print(f"    OUT 0Bh matches: {seq['match_count']} at {seq['addresses'][:5]}")
    seq2, err = c.tool("debug_find_bytecode_sequence",
                       {"range_start": 0x0000, "range_end": 0x7FFF,
                        "pattern": [0xD3, 0x00], "mask": [0xFF, 0x00]})
    check(err is None and
          seq2["match_count"] >= (seq["match_count"] if seq else 0),
          "masked search is a superset (any OUT 0x??)")


def test_immediate_search(c):
    imm, err = c.tool("debug_find_immediate_in_range",
                      {"range_start": 0x0100, "range_end": 0x1FFF, "value": 0x000B})
    check(err is None, "find_immediate_in_range runs")
    if imm:
        print(f"    imm 0x000B matches: {imm['match_count']}"
              f" e.g. {[m['text'] for m in imm['matches'][:3]]}")


def test_memory_diff(c):
    s1, err = c.tool("debug_create_memory_snapshot", {})
    check(err is None, "create snapshot A")
    if err:
        return
    c.tool("debug_write_memory", {"address": 0x6000, "data": [0x5A] * 64})
    s2, err = c.tool("debug_create_memory_snapshot", {})
    check(err is None, "create snapshot B")
    if err:
        return
    diff, err = c.tool("debug_diff_memory",
                       {"snapshot_a": s1["snapshot_id"],
                        "snapshot_b": s2["snapshot_id"],
                        "address": 0x0000, "length": 0x8000})
    check(err is None, "debug_diff_memory runs")
    if not diff:
        return
    tot = sum(r["size"] for r in diff["ranges"])
    check(diff["changed_bytes"] == tot and tot >= 64,
          "dense 64-byte change fully reported (no silent cut)")
    print(f"    changed_bytes={diff['changed_bytes']} ranges={diff['range_count']}")


def test_vram(c):
    vram, err = c.tool("debug_get_vram_bytes", {"address": 0xC000, "length": 32})
    check(err is None and len(vram["bytes"]) == 32,
          "debug_get_vram_bytes 0xC000+32")
    _, err = c.tool("debug_get_vram_bytes", {"address": 0x0100, "length": 16})
    check(err and err["error_code"] == "invalid_address",
          "vram below 0x8000 rejected")


def main():
    print("\n=== Stage 6.26 MCP batch tools — integration ===")
    print(f"server: {SERVER}")
    if not os.path.isfile(SERVER) or not os.access(SERVER, os.X_OK):
        print("SKIP: server binary not found / not executable "
              "(build v06c-mcp first)")
        return 0

    roms = [(label, path) for label, path in DEFAULT_ROMS
            if os.path.isfile(path)]
    for label, path in DEFAULT_ROMS:
        if not os.path.isfile(path):
            print(f"SKIP rom: {label} not found at {path} "
                  f"(set V06C_PUTUP_ROM / V06C_TESTAY_ROM)")
    if not roms:
        print("SKIP: no ROM images available")
        return 0

    with tempfile.TemporaryDirectory(prefix="v06c_stage626_") as workdir:
        c = McpClient(SERVER, workdir)
        try:
            c.rpc("initialize", {
                "protocolVersion": "2024-11-05", "capabilities": {},
                "clientInfo": {"name": "stage626-integration", "version": "1"}})
            c.rpc("notifications/initialized", notify=True)

            for label, path in roms:
                print(f"\n--- {label} ---")
                _, err = c.tool("debug_load_rom", {"path": path})
                check(err is None, "debug_load_rom")
                if err:
                    print(f"    {err}")
                    continue
                test_batch_disassembly(c)
                test_coverage(c)
                test_sequence_search(c)
                test_immediate_search(c)
                test_memory_diff(c)
                test_vram(c)
        finally:
            c.close()

    if fails:
        print(f"\nFAILED ({len(fails)}): " + "; ".join(fails))
        return 1
    print("\nALL INTEGRATION CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
