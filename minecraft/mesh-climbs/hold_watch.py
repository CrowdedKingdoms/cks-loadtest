#!/usr/bin/env python3
"""Read-only watcher for a 180-second hot hold on the live nine-server climb.

Reads climb-nine.log. Does not SSH, signal the driver, clear bots, or change
a step. A bend is only what bend.is_bend already returns. hot_whole is the
string true only when every sample in the still count is hot and the log's
sample gap is a few seconds or less. The wave log records aggregate cpu, not
a per-core reading, so the hot rule here is load >= 1.8 on every sample.
"""

from __future__ import annotations

import csv
import os
import re
import signal
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import bend

STATE = Path.home() / ".local/state/cks-test-mc-p5/nine-climb"
CLIMB_LOG = STATE / "climb-nine.log"
WATCH_LOG = STATE / "hold-watch.log"
PID_PATH = STATE / "hold-watch.pid"
CSV_PATH = Path(
    os.environ.get(
        "CK_PROJECT_ROOT",
        "/Users/benjaminscholtens/Documents/CrowdedKingdoms/cks-project-root",
    )
) / "seam-scaling-nine-2026-11-23.csv"

HOLD_SEC = 180
GAP_LIMIT_SEC = 5
CANDIDATE_CPU = 70.0
POLL_SEC = 2

REQUIRED = (
    "n_target",
    "n_held",
    "result",
    "busiest",
    "busiest_cpu",
    "busiest_load1",
    "binding",
    "note",
    "hot_whole",
)

IP_RE = re.compile(r"\b\d{1,3}(?:\.\d{1,3}){3}\b")
SERVER_RE = re.compile(
    r"^(?P<ts>\d\d:\d\d:\d\d) wave (?P<side>p\d+) "
    r"cpu=(?P<cpu>\S+) load=(?P<load>\S+) players=\S+ "
    r"over=\d+ step=\S+ next=\S+ add=(?P<add>\d+)\s*$"
)
NOTHING_RE = re.compile(
    r"^(?P<ts>\d\d:\d\d:\d\d) wave nothing to add; seated (?P<seated>\d+)\s*$"
)
SEATED_RE = re.compile(
    r"^(?P<ts>\d\d:\d\d:\d\d) wave seated=(?P<seated>\d+) steps\b"
)
GAINED_RE = re.compile(
    r"^(?P<ts>\d\d:\d\d:\d\d) wave p\d+ gained=(?P<gained>-?\d+) over=\d+"
)
HARNESS_RE = re.compile(
    r"^(?P<ts>\d\d:\d\d:\d\d) wave harness missed (?P<n>\d+)"
)
SPAWN_RE = re.compile(
    r"^(?P<ts>\d\d:\d\d:\d\d) spawn active=(?P<active>\d+)/\d+ busy="
)
STOP_PHRASES = (
    "60s tick",
    "60 second",
    "oom-kill",
    "oom kill",
    "outofmemory",
    "java heap space",
    "spawn miss",
    "seated nobody",
    "max runtime",
    "index window",
)
REFUSAL_PHRASES = (
    "game down",
    "game refused",
    "servers refused",
    "could not take",
)


def scrub(text: str) -> str:
    return IP_RE.sub("<ip>", text or "")


def num(text):
    if text in (None, "", "None", "none"):
        return None
    try:
        return float(text)
    except (TypeError, ValueError):
        return None


def over_line(cpu, load) -> bool:
    """Ramp line: the busiest server is hot enough to open a candidate."""
    if load is not None and load >= bend.LOAD_LINE:
        return True
    if cpu is not None and cpu >= CANDIDATE_CPU:
        return True
    return False


def sample_is_hot(load) -> bool:
    """Load rule only. The wave log does not carry a per-core percentage."""
    return bend.sample_hot(load, None)


def read_header(path: Path):
    if not path.exists():
        return None
    with path.open(newline="") as fh:
        return next(csv.reader(fh), None)


def header_can_take(header) -> bool:
    return bool(header) and all(name in header for name in REQUIRED)


def aligned_row(header, fields):
    """One row in header order. Unknown keys or a missing hold column refuse the row."""
    if not header_can_take(header):
        return None
    if any(key not in header for key in fields):
        return None
    return [fields.get(col, "") for col in header]


def foreign_pids():
    me = os.getpid()
    found = []
    try:
        out = subprocess.check_output(["ps", "-ax", "-o", "pid=,command="], text=True)
    except (OSError, subprocess.CalledProcessError):
        return found
    for line in out.splitlines():
        pid_text, _, cmd = line.strip().partition(" ")
        if "hold_watch.py" not in cmd:
            continue
        try:
            pid = int(pid_text)
        except ValueError:
            continue
        if pid != me:
            found.append(pid)
    return found


class Watch:
    def __init__(self, emit, csv_path: Path | None = None):
        self.emit = emit
        self.csv_path = csv_path
        self.csv_header = read_header(csv_path) if csv_path else None
        self.csv_ok = bool(csv_path) and header_can_take(self.csv_header)
        self.live = False
        self.day = 0
        self.last_clock = None
        self.seated = None
        self.candidate = None
        self.after = None
        self.last_busiest = None
        self.partial_ts = None
        self.partial = []
        self.partial_seated = None
        self.partial_nothing = False
        self.wrote_csv = False

    def say(self, text, important=False):
        if self.live or important:
            self.emit(scrub(text))

    def clock(self, ts: str) -> int:
        hours, minutes, seconds = (int(part) for part in ts.split(":"))
        sec = hours * 3600 + minutes * 60 + seconds
        absolute = sec + self.day
        if self.last_clock is not None and absolute < self.last_clock - 30:
            self.day += 86400
            absolute = sec + self.day
        self.last_clock = absolute
        return absolute

    def feed(self, line: str):
        if "per-server steps start" in line:
            self.flush()
            self.candidate = None
            self.after = None
            self.seated = None
            self.say("ramp restarted in the log; candidate closed", important=self.live)
            return
        low = line.lower()
        if any(phrase in low for phrase in REFUSAL_PHRASES):
            self.flush()
            if self.after:
                self._maybe_bend()
            return
        if any(phrase in low for phrase in STOP_PHRASES):
            self.flush()
            if self.after:
                self._verdict("the next wave is a stop or a miss; not a bend")
            elif self.candidate is not None:
                self.candidate = None
                self.say("climb log shows a stop or a miss; candidate closed", important=self.live)
            return
        matched = SERVER_RE.match(line)
        if matched:
            self._server(matched)
            return
        matched = NOTHING_RE.match(line)
        if matched:
            self._nothing(matched)
            return
        matched = SEATED_RE.match(line)
        if matched:
            self._seated_line(matched)
            return
        matched = GAINED_RE.match(line)
        if matched:
            self._gained(matched)
            return
        matched = HARNESS_RE.match(line)
        if matched:
            self._harness(matched)
            return
        matched = SPAWN_RE.match(line)
        if matched:
            self._spawn(matched)

    def _server(self, matched):
        ts = matched.group("ts")
        if self.partial_ts not in (None, ts):
            self.flush()
        self.partial_ts = ts
        self.partial.append({
            "side": matched.group("side"),
            "cpu": num(matched.group("cpu")),
            "load": num(matched.group("load")),
            "add": int(matched.group("add")),
        })

    def _nothing(self, matched):
        ts = matched.group("ts")
        seated = int(matched.group("seated"))
        if self.partial_ts not in (None, ts):
            self.flush()
        if self.partial_ts == ts and self.partial:
            self.partial_seated = seated
            self.partial_nothing = True
            self.flush()
            return
        self._apply_seated(self.clock(ts), seated)

    def _seated_line(self, matched):
        if self.partial:
            self.flush()
        self._apply_seated(self.clock(matched.group("ts")), int(matched.group("seated")))

    def _gained(self, matched):
        if self.partial:
            self.flush()
        if not self.after:
            return
        gained = int(matched.group("gained"))
        self.after.setdefault("gains", []).append(gained)
        if gained > 0:
            self._verdict("the ramp added bots and the count was accepted; hot hold is not a bend")

    def _harness(self, matched):
        if self.partial:
            self.flush()
        if self.after:
            self._verdict("a harness miss followed the hold; not a bend")

    def _spawn(self, matched):
        """Harness active count. A change means the seated count did not stay still."""
        if self.partial:
            self.flush()
        self._apply_seated(self.clock(matched.group("ts")), int(matched.group("active")))

    def flush(self):
        servers = self.partial
        ts = self.partial_ts
        seated = self.partial_seated
        nothing = self.partial_nothing
        self.partial = []
        self.partial_ts = None
        self.partial_seated = None
        self.partial_nothing = False
        if not servers or ts is None:
            return
        self._on_sample(self.clock(ts), servers, seated, nothing)

    def _apply_seated(self, when, seated):
        if self.seated is None:
            self.seated = seated
            return
        if seated == self.seated:
            return
        old = self.seated
        self.seated = seated
        if self.after:
            if seated > self.after.get("seated", old):
                self._verdict("the count was accepted; hot hold is not a bend")
            else:
                self._verdict("seated moved and the log does not show a game-server refusal; not a bend")
            return
        if self.candidate is not None:
            self.candidate = None
            self.say(f"candidate reset; seated {old} -> {seated}", important=self.live)

    def _on_sample(self, when, servers, seated, nothing):
        if seated is not None:
            self._apply_seated(when, seated)
        if self.seated is None:
            return
        best = None
        adds = 0
        for row in servers:
            adds += int(row["add"])
            rank = (
                row["cpu"] if row["cpu"] is not None else -1,
                row["load"] if row["load"] is not None else -1,
            )
            if best is None or rank > best[0]:
                best = (rank, row["side"], row["cpu"], row["load"])
        if best is None:
            return
        _rank, side, cpu, load = best
        self.last_busiest = {"side": side, "cpu": cpu, "load": load, "when": when}
        if nothing:
            adds = 0
        if self.after:
            self._after_sample(adds)
            return
        point = {"when": when, "side": side, "cpu": cpu, "load": load}
        if self.candidate is None:
            if over_line(cpu, load):
                self._open(point)
            self.say(self.status_line())
            return
        if not over_line(cpu, load):
            self.candidate = None
            self.say("candidate closed; busiest server is under the line", important=self.live)
            self.say(self.status_line())
            return
        gap = when - self.candidate["last"]
        if gap > self.candidate["worst_gap"]:
            self.candidate["worst_gap"] = gap
        if gap > GAP_LIMIT_SEC:
            self.candidate["coarse"] = True
        self.candidate["samples"].append(point)
        self.candidate["last"] = when
        if when - self.candidate["start"] >= HOLD_SEC:
            self._finish()
        self.say(self.status_line())

    def _open(self, point):
        self.candidate = {
            "start": point["when"],
            "last": point["when"],
            "seated": self.seated,
            "samples": [point],
            "coarse": False,
            "worst_gap": 0,
        }
        self.say(
            f"candidate open seated={self.seated} busiest={point['side']} "
            f"cpu={_fmt(point['cpu'])} load={_fmt(point['load'])}",
            important=self.live,
        )

    def _finish(self):
        cand = self.candidate
        self.candidate = None
        span = cand["last"] - cand["start"]
        samples = cand["samples"]
        all_hot = bool(samples) and all(sample_is_hot(point["load"]) for point in samples)
        coarse = cand["coarse"] or cand["worst_gap"] > GAP_LIMIT_SEC or len(samples) < 2
        hot_whole = all_hot and not coarse
        if coarse:
            detail = (
                f"window too coarse; worst sample gap {cand['worst_gap']:.0f}s "
                f"is wider than {GAP_LIMIT_SEC:.0f}s; hot_whole false; not a bend"
            )
        elif not all_hot:
            detail = "a sample in the window was under the hot line; hot_whole false; not a bend"
        else:
            detail = "every sample stayed hot; hot_whole true"
        self.say(
            f"hold-window seated={cand['seated']} span={span:.0f}s samples={len(samples)} "
            f"worst_gap={cand['worst_gap']:.0f}s hot_whole={'true' if hot_whole else 'false'} {detail}",
            important=True,
        )
        if hot_whole:
            self._write_hold(cand)
            self.after = {"seated": cand["seated"], "gains": []}
            return
        last = samples[-1]
        if self.seated == cand["seated"] and over_line(last["cpu"], last["load"]):
            self._open(last)

    def _after_sample(self, adds):
        if adds <= 0:
            self._verdict("the ramp did not raise the count; hot hold is not a bend")
            return
        gains = self.after.get("gains") or []
        if any(gained > 0 for gained in gains):
            self._verdict("the ramp added bots and the count was accepted; hot hold is not a bend")
            return
        if gains and all(gained <= 0 for gained in gains):
            self._verdict(
                "a higher count was not seated and the log does not show a game-server refusal; not a bend"
            )

    def _maybe_bend(self):
        note = "game servers refused the next count"
        ruled = bend.is_bend(
            hot_whole=True,
            next_result="fail",
            next_binding="game",
            next_note=note,
        )
        if ruled:
            self._verdict("the bend rule is met")
            self._write_refusal()
            return
        self._verdict("bend.is_bend is false; not a bend")

    def _verdict(self, text):
        seated = None if not self.after else self.after.get("seated")
        self.after = None
        self.say(f"after-hold seated={seated} {text}", important=True)

    def _write_hold(self, cand):
        if not self.csv_ok:
            self.say("hold kept in the watch log; csv header cannot take a hold row", important=True)
            return
        last = cand["samples"][-1]
        fields = {
            "speed_bps": "2",
            "n_target": str(cand["seated"]),
            "n_held": str(cand["seated"]),
            "result": "clean",
            "busiest": last["side"],
            "busiest_cpu": _fmt(last["cpu"]),
            "busiest_load1": _fmt(last["load"]),
            "binding": "none",
            "note": (
                f"watcher 180s hold; every sample load>={bend.LOAD_LINE}; "
                f"gaps<={GAP_LIMIT_SEC:.0f}s; log has no per-core sample"
            ),
            "hot_whole": "true",
        }
        if self._append(fields):
            self.say(f"csv hold row appended seated={cand['seated']} hot_whole=true", important=True)

    def _write_refusal(self):
        if not self.csv_ok or self.seated is None:
            self.say("refusal kept in the watch log; csv header cannot take a hold row", important=True)
            return
        fields = {
            "n_target": str(self.seated),
            "n_held": str(self.seated),
            "result": "fail",
            "binding": "game",
            "note": "watcher; game servers refused the next count",
            "hot_whole": "false",
        }
        if self._append(fields):
            self.say("csv refusal row appended", important=True)

    def _append(self, fields) -> bool:
        header = read_header(self.csv_path)
        if header != self.csv_header or not header_can_take(header):
            self.say("csv left untouched; header changed or cannot take a hold row", important=True)
            self.csv_ok = False
            return False
        row = aligned_row(header, fields)
        if row is None or len(row) != len(header):
            self.say("csv left untouched; hold row would misalign", important=True)
            return False
        try:
            data = self.csv_path.read_bytes()
        except OSError:
            self.say("csv left untouched; could not read the header line", important=True)
            return False
        if not data.endswith(b"\n"):
            self.say("csv left untouched; file does not end on a row boundary", important=True)
            return False
        with self.csv_path.open("a", newline="") as fh:
            csv.writer(fh).writerow(row)
        self.wrote_csv = True
        return True

    def status_line(self) -> str:
        last = self.last_busiest
        busiest = ""
        if last:
            busiest = (
                f" busiest={last['side']} cpu={_fmt(last['cpu'])} load={_fmt(last['load'])}"
            )
        if self.candidate is None:
            return f"watching seated={_fmt(self.seated)} candidate=closed{busiest}"
        age = self.candidate["last"] - self.candidate["start"]
        return (
            f"watching seated={self.seated} candidate=open age={age:.0f}s{busiest}"
        )


def _fmt(value):
    if value is None:
        return "n/a"
    if isinstance(value, float) and value.is_integer():
        return str(int(value))
    return str(value)


def emit_line(text: str):
    stamp = time.strftime("%H:%M:%S")
    line = scrub(f"{stamp} {text}")
    print(line, flush=True)
    with WATCH_LOG.open("a") as fh:
        fh.write(line + "\n")


def follow(watch: Watch, path: Path):
    """Replay complete lines, then keep reading. A trailing partial line stays unread."""
    with path.open("rb") as fh:
        data = fh.read()
        if data and not data.endswith(b"\n"):
            cut = data.rfind(b"\n")
            if cut == -1:
                fh.seek(0)
                data = b""
            else:
                fh.seek(cut + 1)
                data = data[: cut + 1]
        lines = data.decode(errors="replace").splitlines()
        start = 0
        for index, line in enumerate(lines):
            if "per-server steps start" in line:
                start = index + 1
        for line in lines[start:]:
            watch.feed(line)
        watch.flush()
        watch.live = True
        watch.say(watch.status_line(), important=True)
        pending = b""
        while True:
            chunk = fh.readline()
            if not chunk:
                time.sleep(POLL_SEC)
                continue
            pending += chunk
            while b"\n" in pending:
                raw, pending = pending.split(b"\n", 1)
                line = raw.decode(errors="replace")
                if line:
                    watch.feed(line)


def main():
    signal.signal(signal.SIGHUP, signal.SIG_IGN)
    others = foreign_pids()
    if others:
        emit_line("another hold watcher is already running; leaving it")
        return
    PID_PATH.write_text(str(os.getpid()) + "\n")
    emit_line(
        f"watching pid={os.getpid()} ppid={os.getppid()} "
        "read-only climb-nine.log; ramp not signaled"
    )
    header = read_header(CSV_PATH)
    if header_can_take(header):
        emit_line("csv header can take a hold row; nothing appended yet")
    else:
        emit_line("csv header cannot take a hold row; observations stay in the watch log")
    if not CLIMB_LOG.exists():
        emit_line("climb-nine.log is missing; watcher exiting")
        return
    watch = Watch(emit_line, CSV_PATH if header_can_take(header) else None)
    follow(watch, CLIMB_LOG)


if __name__ == "__main__":
    main()
