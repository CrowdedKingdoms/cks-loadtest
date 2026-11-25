#!/usr/bin/env python3
"""Control-socket check for ck-mc-load. It needs no Minecraft server.

Starts `ck-mc-load --control 127.0.0.1:<free port> --threads 2 --journal <temp file>`,
opens control connections, and checks:

  - 500 `add` lines sent in one write each get their own `ok`, in order. The bots point
    at a closed local port, so every one of them fails to connect.
  - `stats` on a second connection answers within 100 ms the whole time, from before the
    adds until every bot has closed.
  - several commands in one write get one reply line each, in order.
  - `list closed` names every bot with its reason.
  - with --journal the bots' lines are in the journal and none are on stdout.
  - `quit` is answered `ok` and the process exits with code 0.

Usage: control_smoke.py [path/to/ck-mc-load]
Python 3, standard library only. Exit code 0 and a last line `control_smoke: ok` on success.
"""

import os
import socket
import subprocess
import sys
import tempfile
import threading
import time

BOTS = 500
STATS_LIMIT_S = 0.100
STATS_KEYS = [
    "total", "connecting", "login", "config", "play", "placed", "closed",
    "transfers", "rejoins", "deaths", "respawns", "crossings", "tick_ms_max",
]


def fail(why):
    print("control_smoke: FAILED: " + why, flush=True)
    sys.exit(1)


def free_port():
    """A loopback port nobody listens on once this returns."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Control:
    """One control connection. Replies are lines."""

    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b""

    def send(self, text):
        self.sock.sendall(text.encode())

    def line(self, timeout=10.0):
        deadline = time.monotonic() + timeout
        while b"\n" not in self.buf:
            left = deadline - time.monotonic()
            if left <= 0:
                fail("no reply line within %.1f s" % timeout)
            self.sock.settimeout(left)
            try:
                data = self.sock.recv(65536)
            except socket.timeout:
                fail("no reply line within %.1f s" % timeout)
            if not data:
                fail("control connection closed while waiting for a reply")
            self.buf += data
        raw, self.buf = self.buf.split(b"\n", 1)
        return raw.decode()

    def ask(self, command, timeout=10.0):
        self.send(command + "\n")
        return self.line(timeout)

    def closed_by_peer(self, timeout=5.0):
        self.sock.settimeout(timeout)
        try:
            return self.sock.recv(1) == b""
        except (socket.timeout, OSError):
            return False

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def parse_stats(reply):
    parts = reply.split(" ")
    if parts[0] != "ok":
        fail("stats reply does not start with ok: " + reply)
    keys = [p.split("=", 1)[0] for p in parts[1:]]
    if keys != STATS_KEYS:
        fail("stats fields are %s" % keys)
    try:
        return {p.split("=", 1)[0]: int(p.split("=", 1)[1]) for p in parts[1:]}
    except ValueError:
        fail("stats has a value that is not a whole number: " + reply)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    binary = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "build", "ck-mc-load")
    if not os.path.isfile(binary):
        fail("no binary at " + binary)

    control_port = free_port()
    dead_port = free_port()
    tmp = tempfile.mkdtemp(prefix="ck-mc-load-smoke-")
    journal = os.path.join(tmp, "journal.log")
    out_path = os.path.join(tmp, "stdout.log")
    err_path = os.path.join(tmp, "stderr.log")
    out_file = open(out_path, "wb")
    err_file = open(err_path, "wb")
    proc = subprocess.Popen(
        [binary, "--control", "127.0.0.1:%d" % control_port, "--threads", "2", "--journal", journal],
        stdin=subprocess.DEVNULL, stdout=out_file, stderr=err_file)

    def stop_process():
        if proc.poll() is None:
            proc.kill()
            proc.wait()

    try:
        # Two control connections, open at the same time.
        first = None
        deadline = time.monotonic() + 10
        while first is None:
            if proc.poll() is not None:
                fail("ck-mc-load exited with code %d before the control socket was up" % proc.returncode)
            try:
                first = Control(control_port)
            except OSError:
                if time.monotonic() > deadline:
                    fail("could not connect to the control socket")
                time.sleep(0.05)
        second = Control(control_port)

        before = parse_stats(second.ask("stats"))
        if before["total"] != 0:
            fail("a fresh process reports %d bots" % before["total"])

        # `stats` on the second connection, again and again, until told to stop.
        latencies = []
        last_stats = {}
        stop_asking = threading.Event()
        asker_error = []

        def keep_asking():
            try:
                while not stop_asking.is_set():
                    started = time.monotonic()
                    reply = second.ask("stats")
                    latencies.append(time.monotonic() - started)
                    last_stats.update(parse_stats(reply))
            except SystemExit:
                asker_error.append("stats failed")

        asker = threading.Thread(target=keep_asking)
        asker.start()
        time.sleep(0.2)
        asked_before_adds = len(latencies)

        # 500 adds in one write on the first connection.
        payload = "".join(
            "add Smoke%d 127.0.0.1 %d wander 2 8 120 - /dev/null\n" % (i + 1, dead_port) for i in range(BOTS))
        add_started = time.monotonic()
        first.send(payload)
        replies = [first.line() for _ in range(BOTS)]
        add_seconds = time.monotonic() - add_started
        not_ok = [r for r in replies if r != "ok"]
        if not_ok:
            fail("%d of %d add replies are not ok, first: %s" % (len(not_ok), BOTS, not_ok[0]))
        print("control_smoke: %d add lines in one write, %d ok replies in %.0f ms"
              % (BOTS, len(replies), add_seconds * 1000), flush=True)

        # Every bot fails to connect and ends as closed.
        deadline = time.monotonic() + 60
        while last_stats.get("closed", 0) < BOTS:
            if asker_error:
                fail(asker_error[0])
            if time.monotonic() > deadline:
                fail("bots still not closed after 60 s: %s" % last_stats)
            time.sleep(0.02)
        closed_seconds = time.monotonic() - add_started
        time.sleep(0.2)
        stop_asking.set()
        asker.join()
        if asker_error:
            fail(asker_error[0])
        during = latencies[asked_before_adds:]
        if len(during) < 5:
            fail("only %d stats replies while the bots were being added" % len(during))
        slowest = max(latencies)
        print("control_smoke: all %d bots closed %.2f s after the adds; stats asked %d times from the adds until 0.2 s "
              "after that, %d times in all, slowest reply %.1f ms, median %.2f ms"
              % (BOTS, closed_seconds, len(during), len(latencies), slowest * 1000,
                 sorted(latencies)[len(latencies) // 2] * 1000), flush=True)
        if slowest >= STATS_LIMIT_S:
            fail("a stats reply took %.1f ms, the limit is %.0f ms" % (slowest * 1000, STATS_LIMIT_S * 1000))

        stats = parse_stats(second.ask("stats"))
        if stats["total"] != BOTS or stats["closed"] != BOTS:
            fail("after the adds: %s" % stats)
        for key in ("connecting", "login", "config", "play", "placed", "transfers", "rejoins", "deaths", "respawns"):
            if stats[key] != 0:
                fail("%s should be 0: %s" % (key, stats))
        print("control_smoke: total=%d closed=%d" % (stats["total"], stats["closed"]), flush=True)

        # `list closed`: every bot, with a reason, under the 64 KB cap.
        listing = second.ask("list closed")
        if not listing.startswith("ok "):
            fail("list closed: " + listing[:80])
        if len(listing) + 1 > 64 * 1024:
            fail("list closed is %d bytes" % (len(listing) + 1))
        entries = listing[3:].split(",")
        names = set()
        for entry in entries:
            name, _, reason = entry.partition(":")
            if not reason.startswith("connect"):
                fail("unexpected closed entry: " + entry)
            names.add(name)
        if names != {"Smoke%d" % (i + 1) for i in range(BOTS)}:
            fail("list closed names %d bots, not %d" % (len(names), BOTS))
        if second.ask("list play") != "ok":
            fail("list play should be empty")

        # Several commands in one write: one reply each, in order.
        third = Control(control_port)
        third.send(
            "bounds -100 -100 100 100\n"
            "speed 6\n"
            "altitude 200\n"
            "altitude off\n"
            "grid -5000 -5000 1000 10 10\n"
            "rally 0 0 30 5\n"
            "unrally\n"
            "\n"
            "nope\n"
            "speed fast\n"
            "list nowhere\n"
            "add too few fields\n"
            "bounds off\r\n"
            "stop Smoke1\n"
            "stop-all\n"
            "stats\n")
        want = ["ok", "ok", "ok", "ok", "ok", "ok rallied=0", "ok", "err unknown", "err bad number fast",
                "err unknown phase nowhere", "err add needs 9 fields", "ok", "ok", "ok"]
        got = [third.line() for _ in want]
        if got != want:
            fail("replies to commands in one write: %s" % got)
        parse_stats(third.line())
        print("control_smoke: %d commands in one write answered in order" % (len(want) + 1), flush=True)

        # The journal has the bots' lines. stdout has none.
        deadline = time.monotonic() + 5
        closed_lines = 0
        while time.monotonic() < deadline:
            with open(journal, "r", errors="replace") as f:
                closed_lines = sum(1 for line in f if " closed: connect" in line)
            if closed_lines >= BOTS:
                break
            time.sleep(0.1)
        if closed_lines != BOTS:
            fail("the journal has %d closed lines, not %d" % (closed_lines, BOTS))

        # quit: ok, the other connections end, the process exits 0.
        if first.ask("quit") != "ok":
            fail("quit was not answered ok")
        try:
            code = proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            fail("ck-mc-load did not exit within 10 s of quit")
        if code != 0:
            fail("exit code %d after quit" % code)
        if not second.closed_by_peer():
            fail("the second control connection was not closed at exit")
        for conn in (first, second, third):
            conn.close()
        out_file.close()
        err_file.close()
        with open(out_path, "rb") as f:
            stdout = f.read()
        with open(err_path, "rb") as f:
            stderr = f.read().decode(errors="replace")
        if stdout:
            fail("with --journal nothing should be on stdout, got %d bytes: %r" % (len(stdout), stdout[:120]))
        if "ck-mc-load: control on 127.0.0.1:%d" % control_port not in stderr:
            fail("no `control on` line on stderr: %r" % stderr[:200])
        print("control_smoke: journal has %d closed lines, stdout is empty" % closed_lines, flush=True)
        print("control_smoke: quit answered ok, exit code 0", flush=True)
    finally:
        stop_process()
        for path in (journal, out_path, err_path):
            try:
                os.remove(path)
            except OSError:
                pass
        try:
            os.rmdir(tmp)
        except OSError:
            pass

    print("control_smoke: ok", flush=True)


if __name__ == "__main__":
    main()
