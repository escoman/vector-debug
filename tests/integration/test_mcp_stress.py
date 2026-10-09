#!/usr/bin/env python3
# ---------------------------------------------------------------------------
# test_mcp_stress.py — long-running stress / hang-repro harness for v06c-mcp
#
# Purpose
#   Reproduce the reported MCP server freeze: after a while of normal traffic,
#   debug_get_screen_snapshot stops answering (call times out), the child
#   process dies, and every later request returns "broken pipe".
#
#   The harness drives the REAL headless server over stdio JSON-RPC for a
#   configurable wall-clock duration (default 15 min) through state-machine
#   sequences that mirror what an agent does in a session:
#
#     load ROM -> run -> snapshot -> pause -> snapshot
#     load ROM -> run -> reload WITHOUT pausing -> immediate snapshot
#     load ROM -> run -> debug_reset -> immediate snapshot   (hypothesis 2)
#     snapshot bursts while the emulator is running          (hypothesis 1)
#     load garbage ROM -> run (CPU derails) -> snapshot mid-crash
#     run/pause/step thrash + mixed heavy tool fan-out
#
#   Every call has a client-side deadline. On timeout/death the harness:
#     * classifies the incident (HANG = process alive but silent,
#                            DEAD  = process exited / pipe closed),
#     * probes follow-up calls to reproduce the "broken pipe" symptom,
#     * captures /proc/<pid> thread states, wchans, and a best-effort
#       `gdb -p <pid> thread apply all bt` backtrace,
#     * saves the stderr log tail and the request timeline around the stall,
#     * restarts the server, re-initializes and continues until the deadline
#       so one run accumulates many incident samples.
#
# Usage
#   python3 tests/integration/test_mcp_stress.py [--duration 900] [--timeout 15]
#       [--server build/v06c-mcp] [--out build/stress_runs/<ts>]
#       [--rom A.rom --rom B.rom ...] [--seed N] [--no-gdb] [--max-restarts 20]
#
#   ROMs default to the vector-games checkout if present; a random-byte
#   "garbage ROM" is always synthesized so the CPU-derail scenario runs even
#   with no external ROM files. Override with --rom (repeatable) or
#   V06C_STRESS_ROMS="a.rom:b.rom".
#
# Exit codes: 0 = no incidents; 2 = >=1 hang/death reproduced; 1 = harness error.
# ---------------------------------------------------------------------------

import argparse
import base64
import collections
import glob
import json
import os
import random
import re
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
DEFAULT_SERVER = os.path.join(REPO, "build", "v06c-mcp")

DEFAULT_ROM_CANDIDATES = [
    "/home/alexey/Projects/vector-games/roms/redesign/putup/src/putup.rom",
    "/home/alexey/Projects/vector-games/roms/redesign/testay/src/TESTAY.ROM",
    "/home/alexey/Projects/vector-games/.scratch/npg_fixed/nu_pogodi.rom",
]

# ---------------------------------------------------------------------------
# Exceptions
# ---------------------------------------------------------------------------

class TransportError(Exception):
    """Server died / pipe closed before answering."""
    def __init__(self, message, pid=None, exitcode=None, tool=None):
        super().__init__(message)
        self.pid = pid
        self.exitcode = exitcode
        self.tool = tool

class CallTimeout(Exception):
    """Server did not answer within the client deadline (= hang)."""
    def __init__(self, message, tool, elapsed):
        super().__init__(message)
        self.tool = tool
        self.elapsed = elapsed

class StressStopped(Exception):
    """Duration elapsed."""

# ---------------------------------------------------------------------------
# MCP stdio client with a reader thread and per-call deadlines
# ---------------------------------------------------------------------------

class McpClient:
    def __init__(self, argv, workdir, stderr_fp, handshake_timeout=30.0):
        self.workdir = workdir
        self.stderr_fp = stderr_fp
        self.handshake_timeout = handshake_timeout
        self.proc = subprocess.Popen(
            argv,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=stderr_fp, text=True, cwd=workdir)
        self.pid = self.proc.pid
        self.server_pid = self.pid   # real v06c-mcp pid (child when run under gdb)
        self._id = 0
        self._lock = threading.Lock()
        self._responses = {}          # id -> {"event": Event, "msg": dict}
        self._eof = threading.Event() # stdout closed (server died / pipe broke)
        self._dead = False
        self.reader = threading.Thread(target=self._read_loop, daemon=True)
        self.reader.start()
        if argv[0].endswith("gdb"):
            self.server_pid = self._find_child_server_pid()

    def _find_child_server_pid(self, timeout=3.0):
        """Under --under-gdb, proc is gdb; locate the real v06c-mcp child."""
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            try:
                kids = open(f"/proc/{self.pid}/task/{self.pid}/children")\
                         .read().split()
            except OSError:
                kids = []
            for kid in kids:
                try:
                    comm = open(f"/proc/{kid}/comm").read().strip()
                except OSError:
                    continue
                if comm.startswith("v06c-mcp"):
                    return int(kid)
            time.sleep(0.05)
        return self.pid

    # -- reader thread ------------------------------------------------------

    def _read_loop(self):
        try:
            for line in self.proc.stdout:
                line = line.strip()
                if not line:
                    continue
                try:
                    msg = json.loads(line)
                except json.JSONDecodeError:
                    continue
                mid = msg.get("id")
                if mid is None:
                    continue  # notification, ignore
                with self._lock:
                    entry = self._responses.get(mid)
                if entry:
                    entry["msg"] = msg
                    entry["event"].set()
        except (ValueError, OSError):
            pass  # pipe already broken
        finally:
            self._eof.set()
            with self._lock:
                for entry in self._responses.values():
                    entry["event"].set()

    # -- RPC ------------------------------------------------------------------

    def alive(self):
        return self.proc.poll() is None and not self._dead

    def rpc(self, method, params=None, timeout=15.0, notify=False, label=None):
        if not self.alive():
            self._dead = True
            raise TransportError(f"server process is gone (pid {self.pid})",
                                 pid=self.pid, exitcode=self.proc.poll(),
                                 tool=label or method)
        msg = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        if not notify:
            with self._lock:
                self._id += 1
                mid = self._id
            msg["id"] = mid
            event = threading.Event()
            entry = {"event": event, "msg": None}
            with self._lock:
                self._responses[mid] = entry
        try:
            self.proc.stdin.write(json.dumps(msg) + "\n")
            self.proc.stdin.flush()
        except (BrokenPipeError, OSError) as e:
            self._dead = True
            raise TransportError(f"broken pipe writing {method}: {e}",
                                 pid=self.pid, exitcode=self.proc.poll(),
                                 tool=label or method)
        if notify:
            return None

        deadline = time.monotonic() + timeout
        while not event.wait(0.2):
            if self._eof.is_set():
                with self._lock:
                    self._responses.pop(mid, None)
                self._dead = True
                raise TransportError(
                    f"server closed stdout while waiting for {method} "
                    f"(pid {self.pid}, exitcode {self.proc.poll()})",
                    pid=self.pid, exitcode=self.proc.poll(),
                    tool=label or method)
            if time.monotonic() >= deadline:
                with self._lock:
                    self._responses.pop(mid, None)
                raise CallTimeout(
                    f"timeout after {timeout:.1f}s waiting for "
                    f"{label or method}", tool=label or method,
                    elapsed=timeout)
        with self._lock:
            self._responses.pop(mid, None)
        if entry["msg"] is None:
            # Reader hit EOF and flushed pending waiters — pipe is broken.
            self._dead = True
            raise TransportError(
                f"server closed stdout while waiting for {method} "
                f"(pid {self.pid}, exitcode {self.proc.poll()})",
                pid=self.pid, exitcode=self.proc.poll(),
                tool=label or method)
        if "error" in entry["msg"]:
            err = entry["msg"]["error"]
            return {"json_error": err}
        return entry["msg"].get("result", {})

    def initialize(self):
        r = self.rpc("initialize", {
            "protocolVersion": "2024-11-05", "capabilities": {},
            "clientInfo": {"name": "mcp-stress", "version": "1"}},
            timeout=self.handshake_timeout, label="initialize")
        self.rpc("notifications/initialized", notify=True)
        return r

    def tool(self, name, args=None, timeout=15.0):
        """Call a tool. Returns (data, app_error). Raises on transport issues."""
        result = self.rpc("tools/call", {"name": name,
                                         "arguments": args or {}},
                          timeout=timeout, label=name)
        if "json_error" in result:
            return None, f"json_error:{result['json_error']}"
        if result.get("isError"):
            content = result.get("content") or []
            txt = content[0].get("text", "") if content else ""
            return None, f"isError:{txt[:200]}"
        content = result.get("content") or []
        if not content:
            return None, "empty_content"
        # Screen snapshot returns [image, text-meta]; others return [text].
        texts = [p for p in content if p.get("type") == "text"]
        images = [p for p in content if p.get("type") == "image"]
        payload = {"_images": len(images)}
        if texts:
            try:
                payload.update(json.loads(texts[0]["text"]))
            except (json.JSONDecodeError, KeyError, TypeError):
                payload["_text"] = texts[0].get("text", "")[:200]
        if "error_code" in payload:
            return None, f"app_error:{payload.get('error_code')}:" \
                         f"{str(payload.get('error_message', ''))[:120]}"
        return payload, None

    def close(self):
        self._dead = True
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        try:
            self.proc.terminate()
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                pass


# ---------------------------------------------------------------------------
# Incident diagnostics
# ---------------------------------------------------------------------------

def proc_snapshot(pid):
    """Thread states + wchans for the hung process (best effort)."""
    out = {"pid": pid, "alive": os.path.exists(f"/proc/{pid}")}
    if not out["alive"]:
        return out
    try:
        with open(f"/proc/{pid}/status") as f:
            out["status"] = f.read()
    except OSError:
        pass
    threads = []
    for tid in glob.glob(f"/proc/{pid}/task/*"):
        try:
            name = open(f"{tid}/comm").read().strip()
            st = open(f"{tid}/stat").read().split()
            state = st[2]
            wchan = open(f"{tid}/wchan").read().strip() or "-"
            threads.append({"tid": os.path.basename(tid), "name": name,
                            "state": state, "wchan": wchan})
        except OSError:
            continue
    out["threads"] = threads
    return out


def gdb_backtrace(pid, out_path, timeout=45):
    """Best-effort: attach gdb, dump all thread backtraces to a file."""
    gdb = shutil.which("gdb")
    if not gdb:
        with open(out_path, "w") as f:
            f.write("gdb not available\n")
        return False
    try:
        with open(out_path, "w") as f:
            p = subprocess.run(
                [gdb, "-p", str(pid), "-batch",
                 "-ex", "thread apply all bt"],
                stdout=f, stderr=subprocess.STDOUT, timeout=timeout)
        return p.returncode == 0
    except (subprocess.TimeoutExpired, OSError):
        # detach cleanly if gdb is still stuck
        try:
            subprocess.run(["kill", "-INT", str(pid)], timeout=2)
        except Exception:
            pass
        return False


# ---------------------------------------------------------------------------
# Metrics / timeline
# ---------------------------------------------------------------------------

class Recorder:
    def __init__(self, timeline_cap=400):
        self.timeline = collections.deque(maxlen=timeline_cap)
        self.latency = collections.defaultdict(list)   # tool -> [ms]
        self.calls = collections.Counter()
        self.app_errors = collections.Counter()
        self.incidents = []
        self.t0 = time.monotonic()
        self.servers_started = 0
        self.snapshot_ok = 0

    def event(self, kind, tool, detail=""):
        self.timeline.append({
            "t": round(time.monotonic() - self.t0, 3),
            "kind": kind, "tool": tool, "detail": detail})

    def call_ok(self, tool, ms):
        self.calls[tool] += 1
        self.latency[tool].append(ms)
        self.event("ok", tool, f"{ms:.0f}ms")

    def call_app_error(self, tool, err):
        self.calls[tool] += 1
        self.app_errors[tool] += 1
        self.event("app_error", tool, err[:200])

    def add_incident(self, inc):
        self.incidents.append(inc)
        self.event("INCIDENT", inc["tool"], inc["kind"])


def pct(sorted_vals, q):
    if not sorted_vals:
        return 0.0
    idx = min(len(sorted_vals) - 1, int(q / 100.0 * len(sorted_vals)))
    return sorted_vals[idx]


# ---------------------------------------------------------------------------
# Scenario context
# ---------------------------------------------------------------------------

class Ctx:
    def __init__(self, harness):
        self.h = harness
        self.rng = harness.rng

    def deadline_check(self):
        if time.monotonic() >= self.h.end_monotonic:
            raise StressStopped()

    def sleep(self, lo, hi):
        d = self.rng.uniform(lo, hi)
        if time.monotonic() + d > self.h.end_monotonic:
            raise StressStopped()
        time.sleep(d)

    def call(self, tool, args=None, timeout=None, expect_fail_ok=True):
        """Guarded tool call. Returns data or None; incidents propagate."""
        self.deadline_check()
        h = self.h
        tmo = timeout if timeout is not None else h.call_timeout
        t0 = time.monotonic()
        data, err = h.client.tool(tool, args, timeout=tmo)
        ms = (time.monotonic() - t0) * 1000.0
        if err is not None:
            h.rec.call_app_error(tool, err)
            return None
        h.rec.call_ok(tool, ms)
        if tool == "debug_get_screen_snapshot" and data is not None:
            h.rec.snapshot_ok += 1
        return data


# ---------------------------------------------------------------------------
# Scenarios — each mirrors a session pattern from the bug report
# ---------------------------------------------------------------------------

def scen_load_run_pause_snapshot(ctx):
    """Clean baseline: load, paused snapshot, run, running snapshot, pause."""
    rom = ctx.h.pick_rom()
    if rom:
        ctx.call("debug_load_rom", {"path": rom})
    ctx.call("debug_get_screen_snapshot")          # snapshot while paused
    ctx.sleep(0.02, 0.1)
    ctx.call("debug_run")
    ctx.sleep(0.05, 0.35)
    ctx.call("debug_get_screen_snapshot")          # mid-run snapshot
    ctx.call("debug_get_registers")
    ctx.call("debug_pause")
    ctx.call("debug_get_screen_snapshot")          # after pause


def scen_reset_then_snapshot(ctx):
    """Hypothesis 2: debug_reset wipes the loaded ROM; snapshot right after."""
    rom = ctx.h.pick_rom()
    if rom:
        ctx.call("debug_load_rom", {"path": rom})
    ctx.call("debug_run")
    ctx.sleep(0.1, 0.5)
    ctx.call("debug_reset")
    ctx.call("debug_get_screen_snapshot")          # immediate, no settle
    ctx.call("debug_get_state")
    if rom and ctx.rng.random() < 0.5:
        ctx.call("debug_load_rom", {"path": rom})  # reload after reset
        ctx.call("debug_get_screen_snapshot")
    ctx.call("debug_pause")


def scen_load_while_running(ctx):
    """Load a ROM WITHOUT pausing, then snapshot immediately."""
    ctx.call("debug_run")
    ctx.sleep(0.05, 0.25)
    rom = ctx.h.pick_rom()
    if rom:
        ctx.call("debug_load_rom", {"path": rom})
    ctx.call("debug_get_screen_snapshot")
    rom = ctx.h.pick_rom()
    if rom:
        ctx.call("debug_load_rom", {"path": rom})
        ctx.call("debug_get_screen_snapshot")
    ctx.call("debug_pause")
    ctx.call("debug_get_screen_snapshot")


def scen_snapshot_burst(ctx):
    """Hypothesis 1: hammer snapshots while the emulator is live."""
    ctx.call("debug_run")
    n = ctx.rng.randint(8, 20)
    for _ in range(n):
        ctx.call("debug_get_screen_snapshot")
        if ctx.rng.random() < 0.3:
            ctx.call("debug_get_beam_state")
        ctx.sleep(0.0, 0.08)
    ctx.call("debug_pause")


def scen_reload_thrash(ctx):
    """Fast state transitions: run->pause->load->run->load->pause->snapshot."""
    roms = ctx.h.roms
    if not roms:
        return
    ctx.call("debug_load_rom", {"path": ctx.h.pick_rom()})
    ctx.call("debug_run")
    ctx.sleep(0.02, 0.15)
    ctx.call("debug_pause")
    ctx.call("debug_load_rom", {"path": ctx.rng.choice(roms)})
    ctx.call("debug_run")
    ctx.call("debug_load_rom", {"path": ctx.rng.choice(roms)})
    ctx.call("debug_get_screen_snapshot")
    ctx.call("debug_pause")
    ctx.call("debug_get_screen_snapshot")


def scen_garbage_rom_derail(ctx):
    """Garbage ROM: PC derails into inconsistent state; snapshot mid-crash."""
    ctx.call("debug_load_rom", {"path": ctx.h.garbage_rom, "org": 0xC000})
    ctx.call("debug_run")
    ctx.sleep(0.05, 0.4)
    ctx.call("debug_get_screen_snapshot")
    ctx.call("debug_get_state")
    ctx.call("debug_get_stack", {"limit": 16})
    ctx.call("debug_get_screen_snapshot")
    ctx.call("debug_pause")
    ctx.call("debug_reset")
    ctx.call("debug_get_screen_snapshot")


def scen_step_run_thrash(ctx):
    """run/pause/step churn between snapshots."""
    ctx.call("debug_run")
    ctx.sleep(0.02, 0.12)
    ctx.call("debug_pause")
    for _ in range(ctx.rng.randint(5, 25)):
        ctx.call("debug_step")
        if ctx.rng.random() < 0.15:
            ctx.call("debug_get_screen_snapshot")
    ctx.call("debug_run")
    ctx.sleep(0.02, 0.2)
    ctx.call("debug_get_screen_snapshot")
    ctx.call("debug_pause")


def scen_mixed_tools(ctx):
    """Broad fan-out incl. heavy reads, snapshot sprinkled in."""
    rng = ctx.rng
    addr = rng.randrange(0x8000, 0xEFFF)
    ctx.call("debug_disassemble", {"address": addr, "count": rng.randint(4, 64)})
    ctx.call("debug_read_memory", {"address": addr,
                                   "size": rng.randint(16, 1024)})
    ctx.call("debug_get_registers")
    ctx.call("debug_get_stack", {"limit": rng.randint(8, 64)})
    ctx.call("debug_get_raster_events", {"max_results": rng.randint(10, 300)})
    ctx.call("debug_get_io_trace", {"limit": 50})
    if rng.random() < 0.6:
        ctx.call("debug_get_screen_snapshot")
    if rng.random() < 0.4:
        ctx.call("debug_run")
        ctx.sleep(0.02, 0.15)
        ctx.call("debug_get_screen_snapshot")
        ctx.call("debug_pause")


SCENARIOS = [
    (scen_load_run_pause_snapshot, 3),
    (scen_reset_then_snapshot,     4),
    (scen_load_while_running,      4),
    (scen_snapshot_burst,          5),
    (scen_reload_thrash,           3),
    (scen_garbage_rom_derail,      3),
    (scen_step_run_thrash,         2),
    (scen_mixed_tools,             2),
]


# ---------------------------------------------------------------------------
# Harness
# ---------------------------------------------------------------------------

class Harness:
    def __init__(self, opts):
        self.opts = opts
        self.rng = random.Random(opts.seed)
        self.rec = Recorder()
        self.call_timeout = opts.timeout
        self.end_monotonic = 0.0
        self.client = None
        self.instance_idx = 0
        self.out_dir = opts.out
        self.roms = opts.roms
        self.garbage_rom = opts.garbage_rom
        self._rom_cycle = 0
        self.incident_seq = 0
        self.instance_started = time.monotonic()
        self.instance_calls = 0

    def pick_rom(self):
        if not self.roms:
            return None
        # deterministic rotation + jitter so different ROMs meet all scenarios
        rom = self.roms[self._rom_cycle % len(self.roms)]
        self._rom_cycle += 1
        return rom

    # -- server lifecycle -----------------------------------------------------

    def start_server(self):
        self.instance_idx += 1
        self.rec.servers_started += 1
        workdir = tempfile.mkdtemp(prefix=f"v06c_stress_{self.instance_idx}_")
        stderr_path = os.path.join(self.out_dir,
                                   f"server_{self.instance_idx:02d}.stderr.log")
        self._stderr_fp = open(stderr_path, "w")
        argv = [self.opts.server]
        self._bt_path = None
        if self.opts.under_gdb:
            if not shutil.which("gdb"):
                print("--under-gdb requested but gdb not found; running bare")
            else:
                # Run the server under gdb: on SIGSEGV the source-level
                # backtrace lands in server_NN_bt.txt (gdb logging redirect),
                # no ptrace_scope problems since gdb is the parent tracer.
                self._bt_path = os.path.join(
                    self.out_dir, f"server_{self.instance_idx:02d}_bt.txt")
                argv = [
                    "gdb", "--batch", "--quiet", "-nx",
                    "-ex", "set pagination off",
                    "-ex", "set confirm off",
                    "-ex", "handle SIGPIPE nostop noprint pass",
                    "-ex", f"set logging file {self._bt_path}",
                    "-ex", "set logging overwrite on",
                    "-ex", "set logging redirect on",
                    "-ex", "set logging enabled on",
                    "-ex", "run",
                ]
                if self.opts.with_cores:
                    argv += ["-ex",
                             "generate-core-file "
                             f"{self.out_dir}/server_"
                             f"{self.instance_idx:02d}.core"]
                argv += ["-ex", "thread apply all bt",
                         "--args", self.opts.server]
        self.client = McpClient(argv, workdir, self._stderr_fp)
        self.client.initialize()
        self.instance_started = time.monotonic()
        self.instance_calls = 0
        self.rec.event("server_start", "__server__",
                       f"pid={self.client.server_pid} cwd={workdir}"
                       + (" under-gdb" if self.opts.under_gdb else ""))
        print(f"[server #{self.instance_idx}] pid {self.client.server_pid}"
              + (" (via gdb)" if self.opts.under_gdb else "")
              + f", workdir {workdir}")

    def stop_server(self):
        if self.client:
            wd = self.client.workdir
            self.client.close()
            self._stderr_fp.close()
            shutil.rmtree(wd, ignore_errors=True)
            self.client = None

    # -- incident handling ------------------------------------------------------

    def handle_incident(self, exc, tool, scenario_name):
        self.incident_seq += 1
        kind = "DEAD" if isinstance(exc, TransportError) else "HANG"
        inc_dir = os.path.join(self.out_dir,
                               f"incident_{self.incident_seq:02d}_{kind}_{tool}")
        os.makedirs(inc_dir, exist_ok=True)
        pid = self.client.server_pid if self.client else None
        exitcode = self.client.proc.poll() if self.client else None

        print(f"\n!!! INCIDENT #{self.incident_seq} [{kind}] during "
              f"{scenario_name} -> {tool}: {exc}\n")

        info = {
            "seq": self.incident_seq, "kind": kind, "tool": tool,
            "scenario": scenario_name,
            "server_instance": self.instance_idx,
            "pid": pid, "exitcode": exitcode,
            "seconds_into_instance": round(time.monotonic() - self.instance_started, 2),
            "calls_into_instance": self.instance_calls,
            "exception": str(exc),
            "unanswered_probe": None,
        }

        # Reproduce symptom 2: the NEXT call after the stall (registers = light).
        if kind == "HANG":
            try:
                self.client.rpc("tools/call",
                                {"name": "debug_get_registers",
                                 "arguments": {}},
                                timeout=self.opts.probe_timeout,
                                label="debug_get_registers(probe)")
                info["unanswered_probe"] = "answered — server recovered"
            except TransportError as e2:
                info["unanswered_probe"] = f"broken pipe: {e2}"
                kind = "DEAD"
                info["kind"] = "DEAD_AFTER_HANG"
                exitcode = self.client.proc.poll()
                info["exitcode"] = exitcode
            except CallTimeout:
                info["unanswered_probe"] = "still silent — hard hang"

        # /proc diagnostics (only meaningful while the process is alive).
        if pid:
            ps = proc_snapshot(pid)
            with open(os.path.join(inc_dir, "proc_status.txt"), "w") as f:
                json.dump(ps, f, indent=2)
            state_zombie = "State:\tZ" in ps.get("status", "")
            if ps.get("alive") and not state_zombie and self.opts.gdb \
                    and not self.opts.under_gdb:
                print(f"    attaching gdb to pid {pid} ...")
                ok = gdb_backtrace(pid, os.path.join(inc_dir, "bt.txt"))
                info["gdb_backtrace"] = "captured" if ok else "failed/skipped"

        # Crash backtrace captured by the --under-gdb wrapper session.
        if self.opts.under_gdb and self._bt_path \
                and os.path.isfile(self._bt_path):
            try:
                shutil.copy(self._bt_path,
                            os.path.join(inc_dir, "crash_bt.txt"))
                info["gdb_backtrace"] = "under-gdb crash_bt.txt captured"
                print(f"    crash backtrace saved: {inc_dir}/crash_bt.txt")
            except OSError:
                pass

        # stderr tail (server logs / crash messages).
        try:
            self._stderr_fp.flush()
            with open(os.path.join(inc_dir, "stderr_tail.txt")) as _f:
                pass
        except OSError:
            pass
        try:
            with open(os.path.join(self.out_dir,
                                   f"server_{self.instance_idx:02d}.stderr.log")) as f:
                lines = f.readlines()
            with open(os.path.join(inc_dir, "stderr_tail.txt"), "w") as f:
                f.writelines(lines[-200:])
        except OSError:
            pass

        # Timeline + metrics snapshot around the stall.
        tail = [e for e in list(self.rec.timeline)][-60:]
        with open(os.path.join(inc_dir, "timeline.json"), "w") as f:
            json.dump(tail, f, indent=2)
        info["timeline_tail"] = tail

        self.rec.add_incident(info)
        for line in (f"--- incident saved to {inc_dir}",
                     f"    kind={info['kind']} exitcode={exitcode} "
                     f"{info['seconds_into_instance']}s / "
                     f"{info['calls_into_instance']} calls into instance",
                     f"    probe: {info['unanswered_probe'] or 'n/a'}"):
            print(line)
        for e in tail[-12:]:
            print(f"    t={e['t']:<9} {e['kind']:<9} {e['tool']:<28} {e['detail']}")
        print()

    def restart_after_incident(self):
        self.stop_server()
        if self.rec.servers_started > self.opts.max_restarts:
            print("max restarts reached; ending stress run early")
            return False
        try:
            self.start_server()
            return True
        except Exception as e:  # noqa: BLE001 — restart failure shouldn't kill report
            print(f"server restart failed: {e}")
            return False

    # -- main loop ----------------------------------------------------------------

    def guarded(self, fn, name):
        """Run one scenario; convert transport faults into incidents."""
        ctx = Ctx(self)
        try:
            fn(ctx)
        except StressStopped:
            raise
        except (TransportError, CallTimeout) as e:
            tool = getattr(e, "tool", None) or "unknown"
            self.handle_incident(e, tool, name)
            if not self.restart_after_incident():
                raise StressStopped()

    def run(self):
        self.end_monotonic = time.monotonic() + self.opts.duration
        pool = []
        for fn, weight in SCENARIOS:
            pool += [(fn, fn.__name__)] * weight
        self.start_server()
        try:
            while time.monotonic() < self.end_monotonic:
                fn, name = self.rng.choice(pool)
                self.guarded(fn, name)
                self.instance_calls += 1
        except StressStopped:
            pass
        finally:
            self.stop_server()

    # -- report -------------------------------------------------------------------

    def write_report(self):
        rep = {
            "duration_requested_s": self.opts.duration,
            "duration_actual_s": round(time.monotonic() - self.rec.t0, 1),
            "server": self.opts.server,
            "call_timeout_s": self.opts.timeout,
            "seed": self.opts.seed,
            "roms": self.roms,
            "servers_started": self.rec.servers_started,
            "total_calls": sum(self.rec.calls.values()),
            "screen_snapshots_ok": self.rec.snapshot_ok,
            "app_errors": dict(self.rec.app_errors),
            "incidents": self.rec.incidents,
            "latency_ms": {},
        }
        for tool, vals in sorted(self.rec.latency.items()):
            vals.sort()
            rep["latency_ms"][tool] = {
                "n": len(vals), "p50": round(pct(vals, 50), 1),
                "p95": round(pct(vals, 95), 1),
                "max": round(vals[-1], 1),
            }
        path = os.path.join(self.out_dir, "report.json")
        with open(path, "w") as f:
            json.dump(rep, f, indent=2)

        print("\n================ MCP STRESS SUMMARY ================")
        print(f"servers started : {rep['servers_started']}   "
              f"calls: {rep['total_calls']}   "
              f"snapshots ok: {rep['screen_snapshots_ok']}")
        print(f"elapsed         : {rep['duration_actual_s']}s")
        if self.rec.app_errors:
            top = ", ".join(f"{t}:{n}" for t, n in
                            self.rec.app_errors.most_common(8))
            print(f"app-level errors: {top}")
        print("\nper-tool latency (ms):")
        for tool, s in rep["latency_ms"].items():
            print(f"  {tool:<34} n={s['n']:<5} p50={s['p50']:<8} "
                  f"p95={s['p95']:<8} max={s['max']}")
        if self.rec.incidents:
            print(f"\n*** {len(self.rec.incidents)} INCIDENT(S) REPRODUCED ***")
            for inc in self.rec.incidents:
                print(f"  #{inc['seq']} {inc['kind']}: {inc['tool']} "
                      f"in {inc['scenario']} "
                      f"(instance {inc['server_instance']}, "
                      f"{inc['seconds_into_instance']}s in, "
                      f"exitcode={inc['exitcode']}, "
                      f"probe={inc['unanswered_probe']})")
        else:
            print("\nno hangs/deaths reproduced in this window — consider "
                  "raising --duration or lowering --timeout")
        print(f"\nreport: {path}")
        print("====================================================")


# ---------------------------------------------------------------------------
# Setup helpers
# ---------------------------------------------------------------------------

def make_garbage_rom(path, rng):
    """Random opcode soup -> guaranteed derailment once executed."""
    data = bytes(rng.randrange(256) for _ in range(2048))
    with open(path, "wb") as f:
        f.write(data)


def resolve_roms(opts):
    roms = []
    if opts.rom:
        roms = list(opts.rom)
    elif os.environ.get("V06C_STRESS_ROMS"):
        roms = [p for p in os.environ["V06C_STRESS_ROMS"].split(":") if p]
    else:
        roms = [p for p in DEFAULT_ROM_CANDIDATES if os.path.isfile(p)]
    return [os.path.abspath(p) for p in roms if os.path.isfile(p)]


def main():
    ap = argparse.ArgumentParser(
        description="Long-running stress/hang-repro harness for v06c-mcp")
    ap.add_argument("--server", default=DEFAULT_SERVER,
                    help="path to v06c-mcp binary")
    ap.add_argument("--duration", type=float, default=900,
                    help="wall-clock seconds to stress the server (default 900)")
    ap.add_argument("--timeout", type=float, default=15.0,
                    help="per-call client deadline (default 15s, "
                         "comparable to IDE tool timeouts)")
    ap.add_argument("--probe-timeout", type=float, default=5.0,
                    help="deadline for the post-hang probe call")
    ap.add_argument("--rom", action="append", default=[],
                    help="ROM file to cycle through (repeatable)")
    ap.add_argument("--out", default=None,
                    help="output dir for logs/incidents/report.json")
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--max-restarts", type=int, default=500)
    ap.add_argument("--under-gdb", action="store_true",
                    help="run each server instance under gdb --batch so a "
                         "SIGSEGV yields a source-level backtrace in "
                         "<out>/server_NN_bt.txt (recommended for diagnosis)")
    ap.add_argument("--with-cores", action="store_true",
                    help="with --under-gdb, also generate a core file per "
                         "crash (large!) — server_NN.core")
    ap.add_argument("--no-gdb", dest="gdb", action="store_false",
                    help="do not attach gdb for backtraces on hangs")
    opts = ap.parse_args()

    opts.server = os.path.abspath(opts.server)
    if not os.path.isfile(opts.server) or not os.access(opts.server, os.X_OK):
        print(f"ERROR: server binary not found/executable: {opts.server}\n"
              f"(build with ENABLE_AI_AGENT=ON first)", file=sys.stderr)
        return 1

    if opts.seed is None:
        opts.seed = int(time.time())
    rng = random.Random(opts.seed)

    ts = time.strftime("%Y%m%d_%H%M%S")
    opts.out = os.path.abspath(opts.out or os.path.join(REPO, "build", "stress_runs", ts))
    os.makedirs(opts.out, exist_ok=True)

    opts.roms = resolve_roms(opts)
    garbage = os.path.join(opts.out, "garbage.rom")
    make_garbage_rom(garbage, rng)
    opts.garbage_rom = garbage

    print(f"mcp-stress: server={opts.server}")
    print(f"            duration={opts.duration}s  call_timeout={opts.timeout}s "
          f" seed={opts.seed}")
    print(f"            roms={opts.roms or '(none — default boot ROM only)'}")
    print(f"            garbage rom={garbage}")
    print(f"            out={opts.out}")

    harness = Harness(opts)
    t0 = time.time()
    try:
        harness.run()
    except KeyboardInterrupt:
        print("\ninterrupted — writing partial report")
        harness.stop_server()
    finally:
        harness.write_report()

    return 2 if harness.rec.incidents else 0


if __name__ == "__main__":
    sys.exit(main())
