#!/usr/bin/env python3
"""Series 1 coefficient ladder for the two-node Minecraft mesh.

Writes one JSON object per plateau to coefficients.jsonl. A row is accepted
only when the 3-minute hold sticks, crossing-based relays are > 0, transfers
are 0, duplicate-UUID warnings during the window are 0, Java RSS and the
server-property retune are present, and binding is not cpu.
actors_per_player is logged at every step and does not stop the climb,
whether it is below 1.7, between 1.7 and 3.0, or above 3.0.

Hold targets are fleet totals 2, then 4, then 8, unless COEFF_LADDER sets
the first targets (this climb uses 8, then creeps). Bots are added onto the
host that owns the next free indices, in index_base order, so the global
sequence stays consecutive (even west, odd east) and a full slice spills
onto the next loadgen. fleet-ctl add --count N still means N per host; this
driver must not use that, because the fleet would jump by N times the hosts.
Creep by 4 after an accepted unbound hold. Series 2 (a new 2x b3-16 pair)
opens only when RAM is what fills and the strict 0.95–1.05 actor band
passed. Mesh width stays 2. This script does not
create or delete VMs. COEFF_CLEAR_AT_END=1 wipes walkers when the script
exits; the default leaves the last hold running.

LoadWalker max runtime must be longer than the hold. The unset default is
180s, which ends the walker as the hold ends.
"""

from __future__ import annotations

import json
import os
import socket
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

HOLD_SEC = 180
LADDER = (2, 4, 8)
CREEP = 4
MIN_MAX_RUNTIME_S = 600
MAX_RUNTIME_S = 14400
POLL_SEC = 10
RAM_FULL = 0.90
CPU_LOAD_FACTOR = 0.90
CPU_BUSY_PCT = 90.0
ACTOR_LO = 0.95
ACTOR_HI = 1.05
# Logged only. A ratio outside 1.7–3.0 used to stop the climb. It does not.
DOUBLE_LO = 1.7
DOUBLE_HI = 3.0

RUN = Path(os.environ.get("COEFF_RUN") or (Path.home() / ".local/state/cks-mc-loadtest-run"))
KEY = Path(os.environ.get("COEFF_SSH_KEY") or (Path.home() / ".local/state/cks-test-mc-cluster/id_ed25519"))
RCON_PASS_FILE = Path(
    os.environ.get("COEFF_RCON_FILE") or (Path.home() / ".local/state/cks-mc-loadtest-run/fabric-rcon.pass")
)
WEST = os.environ.get("COEFF_WEST", "40.160.15.239")
EAST = os.environ.get("COEFF_EAST", "40.160.99.136")
METRICS = Path(__file__).with_name("fabric-metrics.sh")
FLAVOR = "b3-8"
SERVERS = 2


def log(line: str) -> None:
    stamp = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    print(f"{stamp} {line}", flush=True)


def binding_of(*, hold_ok: bool, ram_frac: float | None, load1: float | None, nproc: int | None, cpu_busy: float | None) -> str:
    """ram only when the box is full and the hold did not stick. cpu when the
    CPU is saturated and RAM is still slack. Anything else is unbound."""
    ram_high = ram_frac is not None and ram_frac >= RAM_FULL
    cpu_high = False
    if nproc and load1 is not None and load1 >= CPU_LOAD_FACTOR * nproc:
        cpu_high = True
    if cpu_busy is not None and cpu_busy >= CPU_BUSY_PCT:
        cpu_high = True
    if not hold_ok and ram_high:
        return "ram"
    if cpu_high and not ram_high:
        return "cpu"
    return "unbound"


def in_actor_band(ratio: object) -> bool:
    return isinstance(ratio, (int, float)) and ACTOR_LO <= float(ratio) <= ACTOR_HI


def actor_ratio_note(ratio: object) -> str:
    """Where the ratio sits. This never decides whether the climb continues."""
    if not isinstance(ratio, (int, float)):
        return "missing"
    value = float(ratio)
    if value > DOUBLE_HI:
        return "above 3.0"
    if value < DOUBLE_LO:
        return "below 1.7"
    return "in range"


def reject_reasons(row: dict) -> list[str]:
    reasons: list[str] = []
    if not row.get("hold_ok"):
        reasons.append("hold_broken")
    if int(row.get("relays") or 0) <= 0:
        reasons.append("relays=0")
    if int(row.get("transfers") or 0) != 0:
        reasons.append("transfers")
    if row.get("n_held") != row.get("n_target"):
        reasons.append("n_held")
    if int(row.get("dup_uuid_warnings") if row.get("dup_uuid_warnings") is not None else -1) != 0:
        reasons.append("dup_uuid_warnings")
    if row.get("binding") == "cpu":
        reasons.append("binding=cpu")
    if not row.get("java_rss_ok"):
        reasons.append("java_rss")
    if not row.get("props_ok"):
        reasons.append("server_properties")
    if int(row.get("hold_sec") or 0) < HOLD_SEC and row.get("hold_ok"):
        reasons.append("hold_short")
    return reasons


def series2_eligible(row: dict) -> bool:
    """A RAM ceiling opens the 16 GB pair only when the actor gates passed.
    A ghost pile that fills RAM is not a RAM law."""
    if row.get("binding") != "ram":
        return False
    if int(row.get("relays") or 0) <= 0:
        return False
    if int(row.get("transfers") or 0) != 0:
        return False
    if int(row.get("dup_uuid_warnings") if row.get("dup_uuid_warnings") is not None else -1) != 0:
        return False
    ratio = row.get("actors_per_player")
    if not isinstance(ratio, (int, float)) or not (ACTOR_LO <= float(ratio) <= ACTOR_HI):
        return False
    return bool(row.get("java_rss_ok") and row.get("props_ok"))


def creep_allowed(row: dict) -> bool:
    """Step again when the hold kept every walker and the seam is still crossing.
    A cpu tag on a hold that stayed up is recorded. The climb stops when a hold drops walkers."""
    if not row.get("hold_ok"):
        return False
    if int(row.get("relays") or 0) <= 0:
        return False
    if int(row.get("transfers") or 0) != 0:
        return False
    if row.get("binding") == "ram":
        return False
    return True


def self_test() -> int:
    base = {
        "hold_ok": True,
        "hold_sec": HOLD_SEC,
        "relays": 2,
        "transfers": 0,
        "n_held": 8,
        "n_target": 8,
        "dup_uuid_warnings": 0,
        "actors_per_player": 1.0,
        "binding": "unbound",
        "java_rss_ok": True,
        "props_ok": True,
    }
    cases = [
        ("accept unbound", base, []),
        ("cpu out", {**base, "binding": "cpu"}, ["binding=cpu"]),
        ("no relay", {**base, "relays": 0}, ["relays=0"]),
        ("dup", {**base, "dup_uuid_warnings": 3}, ["dup_uuid_warnings"]),
        ("actors high", {**base, "actors_per_player": 9.0}, []),
        ("actors above cap", {**base, "actors_per_player": 4.2222}, []),
        ("actors below floor", {**base, "actors_per_player": 0.5}, []),
        ("double count", {**base, "actors_per_player": 2.44}, []),
        ("double count exact", {**base, "actors_per_player": 2.0}, []),
        (
            "double count plus kick",
            {**base, "actors_per_player": 2.44, "transfers": 1},
            ["transfers"],
        ),
        ("four actors", {**base, "actors_per_player": 4.0}, []),
        ("no rss", {**base, "java_rss_ok": False}, ["java_rss"]),
        ("broken", {**base, "hold_ok": False, "n_held": 6}, ["hold_broken", "n_held"]),
    ]
    failed = 0
    for name, row, expect in cases:
        got = reject_reasons(row)
        if got != expect:
            print(f"FAIL {name}: {got} != {expect}")
            failed += 1
    if binding_of(hold_ok=False, ram_frac=0.60, load1=8.4, nproc=2, cpu_busy=95) != "cpu":
        print("FAIL friday binding")
        failed += 1
    if binding_of(hold_ok=False, ram_frac=0.95, load1=1.0, nproc=2, cpu_busy=10) != "ram":
        print("FAIL ram binding")
        failed += 1
    if binding_of(hold_ok=True, ram_frac=0.40, load1=0.2, nproc=2, cpu_busy=5) != "unbound":
        print("FAIL unbound binding")
        failed += 1
    ram_row = {**base, "hold_ok": False, "n_held": 7, "binding": "ram"}
    if reject_reasons(ram_row) == [] or not series2_eligible(ram_row):
        print("FAIL series2 ram ceiling")
        failed += 1
    dirty = {**ram_row, "actors_per_player": 4}
    if series2_eligible(dirty):
        print("FAIL series2 must ignore a ghost pile")
        failed += 1
    for ratio, note in ((4.2222, "above 3.0"), (0.5, "below 1.7"), (2.44, "in range"), (1.0, "below 1.7")):
        sample = {**base, "actors_per_player": ratio}
        if reject_reasons(sample) or actor_ratio_note(ratio) != note:
            print(f"FAIL ratio {ratio} should continue and log {note}")
            failed += 1
        if creep_allowed({**sample, "accepted": not reject_reasons(sample)}) is not True:
            print(f"FAIL creep at ratio {ratio}")
            failed += 1
    if creep_allowed({**base, "accepted": True}) is not True:
        print("FAIL creep")
        failed += 1
    if not creep_allowed({**base, "accepted": False, "binding": "cpu", "hold_ok": True}):
        print("FAIL cpu tag should not stop a hold that stuck")
        failed += 1
    if creep_allowed({**base, "hold_ok": False, "binding": "cpu", "n_held": 6}):
        print("FAIL no creep when the hold broke")
        failed += 1
    full = [
        {"_host": "http://lg1:9109", "index_base": 0, "index_used": 28, "index_limit": 28},
        {"_host": "http://lg2:9109", "index_base": 28, "index_used": 0, "index_limit": 28},
        {"_host": "http://lg3:9109", "index_base": 56, "index_used": 0, "index_limit": 28},
    ]
    if place_adds(full, 4) != [("http://lg2:9109", 4)]:
        print(f"FAIL full first slice should spill onto the second: {place_adds(full, 4)}")
        failed += 1
    crossing = [
        {"_host": "http://lg2:9109", "index_base": 28, "index_used": 0, "index_limit": 28},
        {"_host": "http://lg1:9109", "index_base": 0, "index_used": 26, "index_limit": 28},
    ]
    split = place_adds(crossing, 4)
    if split != [("http://lg1:9109", 2), ("http://lg2:9109", 2)]:
        print(f"FAIL boundary split: {split}")
        failed += 1
    try:
        place_adds(full, 60)
        print("FAIL oversize delta should fail before any add")
        failed += 1
    except SliceShortfall:
        pass
    if failed:
        print(f"{failed} failed")
        return 1
    print("coeff-series self-test ok")
    return 0


def read_token() -> str:
    path = RUN / "control-token"
    token = path.read_text().strip()
    if not token:
        raise SystemExit("control token missing")
    return token


def hosts() -> list[str]:
    lines = (RUN / "hosts.txt").read_text().splitlines()
    out = [ln.strip() for ln in lines if ln.strip() and not ln.strip().startswith("#")]
    if not out:
        raise SystemExit("hosts.txt empty")
    return out


def api(host: str, token: str, method: str, path: str, body: dict | None = None) -> tuple[int, dict | str]:
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(
        host.rstrip("/") + path,
        data=data,
        method=method,
        headers={"Authorization": f"Bearer {token}", "Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(req, timeout=30) as res:
            raw = res.read().decode()
            code = res.status
    except urllib.error.HTTPError as e:
        raw = e.read().decode()
        code = e.code
    try:
        return code, json.loads(raw) if raw else {}
    except json.JSONDecodeError:
        return code, raw


def fan_status(token: str) -> list[dict]:
    rows = []
    for host in hosts():
        code, body = api(host, token, "GET", "/v1/status")
        if code != 200 or not isinstance(body, dict):
            raise RuntimeError(f"status {code} from {host}")
        body["_host"] = host
        rows.append(body)
    return rows


def fleet_active(rows: list[dict]) -> int:
    return sum(int(r.get("active") or 0) for r in rows)


def close_open_rungs(token: str) -> None:
    for row in fan_status(token):
        if row.get("current_rung"):
            api(row["_host"], token, "POST", "/v1/rung/close", {})


def ssh(ip: str, remote: str, timeout: int = 40) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            "ssh",
            "-i",
            str(KEY),
            "-o",
            "StrictHostKeyChecking=accept-new",
            "-o",
            "ConnectTimeout=12",
            f"ubuntu@{ip}",
            remote,
        ],
        capture_output=True,
        text=True,
        timeout=timeout,
    )


def tailscale_ssh(ip: str, remote: str, timeout: int = 40) -> subprocess.CompletedProcess[str]:
    # tailscale ssh re-parses the command in a remote shell, so quotes and
    # parentheses do not survive as arguments. Feed the script on stdin.
    return subprocess.run(
        ["tailscale", "ssh", f"ubuntu@{ip}", "--", "bash", "-s"],
        input=remote,
        capture_output=True,
        text=True,
        timeout=timeout,
    )


def host_ip(url: str) -> str:
    return url.split("//", 1)[-1].split(":", 1)[0]


def ensure_walker_runtime(token: str) -> None:
    """The hold is 180s. The walker default is also 180s and fires during the
    window. Require a longer cap on every generator, then restart the harness
    so new JVMs inherit it. Refuses if bots are already up."""
    rows = fan_status(token)
    if fleet_active(rows) != 0 or any(r.get("busy") for r in rows):
        raise SystemExit(f"refusing to retune runtime while the fleet is not idle: active={fleet_active(rows)}")
    restart: list[str] = []
    for row in rows:
        if not row.get("mc_alternate"):
            raise SystemExit(f"pose-split is off on {row.get('instance_id')}: mc_alternate false")
        ip = host_ip(row["_host"])
        probe = (
            "python3 - <<'PY'\n"
            "from pathlib import Path\n"
            "p = Path('/opt/ck-mc-harness/lt.env')\n"
            "text = p.read_text() if p.exists() else ''\n"
            "vals = {}\n"
            "for line in text.splitlines():\n"
            "    if not line or line.startswith('#') or '=' not in line: continue\n"
            "    k, v = line.split('=', 1)\n"
            "    vals[k] = v\n"
            "print('RUNTIME=' + vals.get('LT_MC_MAX_RUNTIME_S', ''))\n"
            "print('ALTERNATE=' + vals.get('LT_MC_ALTERNATE', ''))\n"
            "PY"
        )
        got = tailscale_ssh(ip, probe)
        if got.returncode != 0:
            raise SystemExit(f"cannot read harness env on {ip}: {got.stderr[-300:]}")
        vals = {}
        for line in got.stdout.splitlines():
            if line.startswith("RUNTIME=") or line.startswith("ALTERNATE="):
                k, v = line.split("=", 1)
                vals[k] = v.strip()
        runtime = int(vals["RUNTIME"]) if vals.get("RUNTIME", "").isdigit() else 0
        alternate = vals.get("ALTERNATE", "")
        if alternate in ("0", "false", "False"):
            raise SystemExit(f"LT_MC_ALTERNATE={alternate} on {ip}; Series 1 requires pose-split")
        live = tailscale_ssh(
            ip,
            "pid=$(pgrep -f mc-harness.mjs | head -n 1)\n"
            "python3 - \"$pid\" <<'PY'\n"
            "import sys\n"
            "from pathlib import Path\n"
            "raw = Path(f'/proc/{sys.argv[1]}/environ').read_bytes().split(b'\\0')\n"
            "for item in raw:\n"
            "    if item.startswith(b'LT_MC_MAX_RUNTIME_S='):\n"
            "        print(item.decode().split('=', 1)[1])\n"
            "PY\n",
        )
        live_runtime = 0
        for line in reversed(live.stdout.splitlines()):
            if line.strip().isdigit():
                live_runtime = int(line.strip())
                break
        if live.returncode == 0 and live_runtime < MIN_MAX_RUNTIME_S:
            runtime = live_runtime
        if runtime < MIN_MAX_RUNTIME_S:
            log(f"max runtime on {row.get('instance_id')} is {runtime or 'unset'}; setting {MAX_RUNTIME_S}")
            edit = (
                "python3 - <<'PY'\n"
                "from pathlib import Path\n"
                "p = Path('/opt/ck-mc-harness/lt.env')\n"
                "lines = p.read_text().splitlines() if p.exists() else []\n"
                "out = [ln for ln in lines if not ln.startswith('LT_MC_MAX_RUNTIME_S=')]\n"
                f"out.append('LT_MC_MAX_RUNTIME_S={MAX_RUNTIME_S}')\n"
                "p.write_text('\\n'.join(out) + '\\n')\n"
                "p.chmod(0o600)\n"
                "print('runtime-updated')\n"
                "PY"
            )
            edited = tailscale_ssh(ip, edit)
            if edited.returncode != 0 or "runtime-updated" not in edited.stdout:
                raise SystemExit(f"failed to set max runtime on {ip}: {edited.stderr[-300:]}")
            restart.append(ip)
    for ip in restart:
        log(f"restarting harness on {ip} so walkers inherit max runtime {MAX_RUNTIME_S}s")
        restarted = tailscale_ssh(ip, "sudo systemctl restart ck-mc-harness.service && sleep 2 && systemctl is-active ck-mc-harness.service")
        if restarted.returncode != 0 or "active" not in restarted.stdout:
            raise SystemExit(f"harness restart failed on {ip}: {restarted.stderr[-300:]}")
    if restart:
        time.sleep(2)
        again = fan_status(token)
        if fleet_active(again) != 0:
            raise SystemExit("fleet not idle after harness restart")
        for row in again:
            if not row.get("mc_alternate"):
                raise SystemExit(f"pose-split dropped after restart on {row.get('instance_id')}")


class SliceShortfall(RuntimeError):
    """The fleet's free index slices cannot hold this many new walkers."""


def place_adds(rows: list[dict], delta: int) -> list[tuple[str, int]]:
    """Assign `delta` new walkers onto hosts in index_base order.

    Each host only receives as many as its slice has free. A step that
    crosses a boundary is split so the caller can finish the first host
    before starting the next. This does not talk to the fleet.
    """
    if delta <= 0:
        raise ValueError(f"delta must be positive, got {delta}")
    ordered = sorted(rows, key=lambda row: int(row.get("index_base") or 0))
    plan: list[tuple[str, int]] = []
    left = delta
    for row in ordered:
        if left <= 0:
            break
        room = int(row.get("index_limit") or 0) - int(row.get("index_used") or 0)
        if room <= 0:
            continue
        host = row.get("_host") or row.get("host")
        if not host:
            raise ValueError("status row missing host")
        take = min(room, left)
        plan.append((str(host), take))
        left -= take
    if left:
        raise SliceShortfall(f"fleet index slices short by {left}")
    return plan


def wait_until(token: str, target: int, timeout: int) -> tuple[bool, str]:
    deadline = time.time() + timeout
    last = ""
    while time.time() < deadline:
        rows = fan_status(token)
        active = fleet_active(rows)
        busy = any(r.get("busy") for r in rows)
        errors = [r.get("add_error") for r in rows if r.get("add_error")]
        last = f"active={active} busy={busy} errors={errors}"
        log(f"ramp {last}")
        if errors:
            return False, last
        if active == target and not busy:
            return True, last
        if active > target and not busy:
            return False, last
        time.sleep(POLL_SEC)
    return False, f"timeout {last}"


def add_on_host(token: str, host: str, count: int) -> None:
    code, body = api(host, token, "POST", "/v1/clients/add", {"count": count})
    if code not in (200, 202):
        raise RuntimeError(f"add {count} on {host} rejected {code}: {body}")


def add_across_slices(token: str, delta: int) -> tuple[bool, str]:
    """Place `delta` walkers on the slices that own the next indices.

    SliceShortfall is raised before any HTTP call when the fleet has no room.
    A boundary split waits until the first host finishes before the next
    host starts, so logins stay one at a time.
    """
    rows = fan_status(token)
    plan = place_adds(rows, delta)
    progressed = fleet_active(rows)
    for host, count in plan:
        log(f"add {count} on {host} toward fleet {progressed + count}")
        add_on_host(token, host, count)
        progressed += count
        ok, detail = wait_until(token, progressed, timeout=max(180, 45 * count))
        if not ok:
            return False, detail
    return True, f"active={progressed}"


def sample_metrics(out: Path, seconds: int) -> subprocess.Popen[str]:
    out.parent.mkdir(parents=True, exist_ok=True)
    return subprocess.Popen(
        [
            "bash",
            str(METRICS),
            "--west",
            WEST,
            "--east",
            EAST,
            "--ssh-key",
            str(KEY),
            "--out",
            str(out),
            "--interval",
            "15",
            "--seconds",
            str(seconds),
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )


def summarize_metrics(path: Path) -> dict:
    per: dict[str, dict] = {}
    if not path.exists():
        return {"ok": False, "per": per}
    for line in path.read_text().splitlines():
        if not line.strip():
            continue
        try:
            rec = json.loads(line)
        except json.JSONDecodeError:
            continue
        host = rec.get("host")
        sample = rec.get("sample") or {}
        if not host or not sample or "mem" not in sample:
            continue
        slot = per.setdefault(host, {"mem": [], "cpu": [], "java": [], "load": [], "nproc": None, "nic": []})
        mem = sample["mem"]
        slot["mem"].append(mem)
        slot["cpu"].append((sample.get("cpu") or {}).get("busy_pct"))
        java = sample.get("java") or {}
        if java.get("rss_gb") is not None and int(java.get("java_procs") or 0) > 0:
            slot["java"].append(java["rss_gb"])
        pressure = sample.get("pressure") or {}
        if pressure.get("load1") is not None:
            slot["load"].append(pressure["load1"])
        if pressure.get("nproc"):
            slot["nproc"] = pressure["nproc"]
        slot["nic"].append(sample.get("nic") or {})
    ok = "west" in per and "east" in per and per["west"]["java"] and per["east"]["java"] and per["west"]["mem"] and per["east"]["mem"]
    return {"ok": bool(ok), "per": per}


def props_and_actors(ip: str, role: str, log_mark: int) -> dict:
    remote = f"""python3 - <<'PY'
import json, re
from pathlib import Path
role = {role!r}
roots = [Path("/opt/mc") / role, Path("/opt/mc")]
props = {{}}
prop_path = None
for root in roots:
    cand = root / "server.properties"
    if cand.exists():
        prop_path = cand
        break
wanted = {{
    "allow-flight": "allow_flight",
    "view-distance": "view_distance",
    "simulation-distance": "simulation_distance",
    "entity-broadcast-range-percentage": "entity_broadcast_range_percentage",
}}
enable_rcon = False
rcon_port = 25575
if prop_path:
    for line in prop_path.read_text(errors="replace").splitlines():
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        if k in wanted:
            props[wanted[k]] = v.strip()
        elif k == "enable-rcon":
            enable_rcon = v.strip().lower() == "true"
        elif k == "rcon.port":
            try:
                rcon_port = int(v.strip())
            except ValueError:
                pass
log_path = None
for root in roots:
    cand = root / "logs" / "latest.log"
    if cand.exists():
        log_path = cand
        break
dup = None
actors = []
online_hint = None
if log_path:
    data = log_path.read_bytes()
    mark = {int(log_mark)}
    window = data[mark if 0 <= mark <= len(data) else 0:]
    dup = window.decode("utf-8", "replace").count("Force-added player with duplicate UUID")
    text = data.decode("utf-8", "replace")
    start = text.rfind("Done (")
    chunk = text[start if start >= 0 else 0:]
    seen = {{}}
    for m in re.finditer(
        r"(\\S+) joined the shared world as CK player \\S+ \\([^)]*\\), actor ([0-9a-fA-F]{{32}}|[0-9a-fA-F-]{{36}})",
        chunk,
    ):
        seen.setdefault(m.group(1), set()).add(m.group(2))
    actors = [{{"name": name, "actors": len(uuids)}} for name, uuids in sorted(seen.items())]
print(json.dumps({{
    "props": props,
    "enable_rcon": enable_rcon,
    "rcon_port": rcon_port,
    "dup_uuid_warnings": dup,
    "actors": actors,
    "log_bytes": log_path.stat().st_size if log_path else None,
}}))
PY"""
    got = ssh(ip, "sudo -n " + remote, timeout=60)
    if got.returncode != 0:
        return {"error": got.stderr[-400:] or got.stdout[-400:]}
    try:
        return json.loads(got.stdout.strip().splitlines()[-1])
    except (json.JSONDecodeError, IndexError):
        return {"error": "unparsed actor sample"}


def rcon_list(ip: str, port: int) -> list[str] | None:
    if not RCON_PASS_FILE.exists():
        return None
    password = RCON_PASS_FILE.read_text().strip()
    if not password:
        return None

    def recvn(sock: socket.socket, n: int) -> bytes:
        buf = b""
        while len(buf) < n:
            chunk = sock.recv(n - len(buf))
            if not chunk:
                raise ConnectionError("rcon closed")
            buf += chunk
        return buf

    def packet(req_id: int, typ: int, payload: str) -> bytes:
        body = struct.pack("<ii", req_id, typ) + payload.encode() + b"\x00\x00"
        return struct.pack("<i", len(body)) + body

    try:
        sock = socket.create_connection((ip, port), 8)
    except OSError:
        return None
    try:
        sock.sendall(packet(1, 3, password))
        n = struct.unpack("<i", recvn(sock, 4))[0]
        body = recvn(sock, n)
        req_id = struct.unpack("<i", body[:4])[0]
        if req_id == -1:
            return None
        sock.sendall(packet(2, 2, "list"))
        n = struct.unpack("<i", recvn(sock, 4))[0]
        body = recvn(sock, n)
        payload = body[8:-2].decode("utf-8", "replace")
    except (OSError, struct.error):
        return None
    finally:
        sock.close()
    if ":" not in payload:
        return []
    tail = payload.split(":", 1)[1].strip()
    if not tail:
        return []
    return [name.strip() for name in tail.split(",") if name.strip()]


def log_mark(ip: str, role: str) -> int:
    remote = (
        f"sudo -n python3 -c \"from pathlib import Path; "
        f"cands=[Path('/opt/mc')/'{role}'/'logs'/'latest.log', Path('/opt/mc')/'logs'/'latest.log']; "
        "hits=[p.stat().st_size for p in cands if p.exists()]; print(hits[0] if hits else 0)\""
    )
    got = ssh(ip, remote)
    if got.returncode != 0:
        return 0
    try:
        return int(got.stdout.strip().splitlines()[-1])
    except (ValueError, IndexError):
        return 0


def build_row(
    *,
    n_target: int,
    n_held: int,
    hold_ok: bool,
    hold_sec: int,
    relays: int,
    transfers: int,
    metrics: dict,
    west_info: dict,
    east_info: dict,
    exit_reason: str,
) -> dict:
    per = metrics.get("per") or {}

    def host_numbers(name: str) -> dict:
        slot = per.get(name) or {}
        mems = slot.get("mem") or []
        last = mems[-1] if mems else {}
        total_kb = last.get("total_kb")
        used_kb = last.get("used_kb")
        java = slot.get("java") or []
        loads = slot.get("load") or []
        cpus = [c for c in (slot.get("cpu") or []) if isinstance(c, (int, float))]
        return {
            "ram_total_gb": round(total_kb / 1024 / 1024, 4) if total_kb else None,
            "ram_used_gb": round(used_kb / 1024 / 1024, 4) if used_kb else None,
            "ram_frac": (used_kb / total_kb) if total_kb and used_kb is not None else None,
            "java_rss_gb": round(max(java), 4) if java else None,
            "cpu_busy_pct": max(cpus) if cpus else None,
            "load1": max(loads) if loads else None,
            "nproc": slot.get("nproc"),
            "nic": (slot.get("nic") or [None])[-1],
        }

    west = host_numbers("west")
    east = host_numbers("east")
    totals = [v for v in (west["ram_total_gb"], east["ram_total_gb"]) if v is not None]
    useds = [v for v in (west["ram_used_gb"], east["ram_used_gb"]) if v is not None]
    ram_total = round(sum(totals), 4) if len(totals) == 2 else None
    ram_used = round(sum(useds), 4) if len(useds) == 2 else None
    fracs = [v["ram_frac"] for v in (west, east) if v["ram_frac"] is not None]
    ram_frac = max(fracs) if fracs else None
    load1 = max([v for v in (west["load1"], east["load1"]) if v is not None], default=None)
    nproc = west["nproc"] or east["nproc"]
    cpu_busy = max([v for v in (west["cpu_busy_pct"], east["cpu_busy_pct"]) if v is not None], default=None)
    binding = binding_of(hold_ok=hold_ok, ram_frac=ram_frac, load1=load1, nproc=nproc, cpu_busy=cpu_busy)

    def props_from(info: dict) -> dict:
        return info.get("props") or {}

    wp, ep = props_from(west_info), props_from(east_info)
    prop_keys = ("allow_flight", "view_distance", "simulation_distance", "entity_broadcast_range_percentage")
    props_ok = all(wp.get(k) not in (None, "") and wp.get(k) == ep.get(k) for k in prop_keys)
    java_rss_ok = west["java_rss_gb"] is not None and east["java_rss_gb"] is not None

    west_actors = west_info.get("actors") or []
    east_actors = east_info.get("actors") or []
    by_name: dict[str, int] = {}
    for item in west_actors + east_actors:
        name = item.get("name")
        if name:
            by_name[name] = by_name.get(name, 0) + int(item.get("actors") or 0)
    online = west_info.get("online")
    if online is None:
        online = east_info.get("online")
    distinct = sum(by_name.values())
    ratio = None
    if isinstance(online, int) and online > 0:
        ratio = round(distinct / online, 4)
    elif n_held > 0 and by_name:
        ratio = round(distinct / n_held, 4)
    dups = []
    for info in (west_info, east_info):
        if info.get("dup_uuid_warnings") is None:
            dups.append(None)
        else:
            dups.append(int(info["dup_uuid_warnings"]))
    dup_total = None if any(v is None for v in dups) else sum(dups)

    row = {
        "ts": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "series": "8gb",
        "flavor": FLAVOR,
        "servers": SERVERS,
        "mesh_width": SERVERS,
        "n_target": n_target,
        "n_held": n_held,
        "hold_ok": hold_ok,
        "hold_sec": hold_sec,
        "transfers": transfers,
        "relays": relays,
        "ram_total_gb": ram_total,
        "ram_used_gb": ram_used,
        "ram_used_gb_per_host": {"west": west["ram_used_gb"], "east": east["ram_used_gb"]},
        "java_rss_gb": {"west": west["java_rss_gb"], "east": east["java_rss_gb"]},
        "java_rss_ok": java_rss_ok,
        "cpu_busy_pct": {"west": west["cpu_busy_pct"], "east": east["cpu_busy_pct"]},
        "load1": {"west": west["load1"], "east": east["load1"]},
        "nproc": {"west": west["nproc"], "east": east["nproc"]},
        "nic": {"west": west["nic"], "east": east["nic"]},
        "allow_flight": wp.get("allow_flight"),
        "view_distance": wp.get("view_distance"),
        "simulation_distance": wp.get("simulation_distance"),
        "entity_broadcast_range_percentage": wp.get("entity_broadcast_range_percentage"),
        "props_ok": props_ok,
        "users_per_gb": round(n_held / ram_total, 6) if ram_total and n_held is not None else None,
        "users_per_used_gb": round(n_held / ram_used, 6) if ram_used and n_held else None,
        "binding": binding,
        "dup_uuid_warnings": dup_total,
        "actors_per_player": ratio,
        "actors_by_name": by_name,
        "online_players": online,
        "exit_reason": exit_reason,
        "alpha": False,
    }
    reasons = reject_reasons(row)
    row["accepted"] = not reasons
    row["reject_reasons"] = reasons
    note = actor_ratio_note(ratio)
    row["actors_note"] = note
    row["actors_waiver"] = "" if in_actor_band(ratio) else "logged_not_gating"
    row["opens_series_2"] = series2_eligible(row)
    return row


def hold(token: str, n_target: int, out_dir: Path) -> dict:
    rows = fan_status(token)
    if fleet_active(rows) != n_target or any(r.get("busy") for r in rows):
        return build_row(
            n_target=n_target,
            n_held=fleet_active(rows),
            hold_ok=False,
            hold_sec=0,
            relays=0,
            transfers=0,
            metrics={"ok": False, "per": {}},
            west_info={"error": "not at target"},
            east_info={"error": "not at target"},
            exit_reason="not at target before hold",
        )
    rung = f"r-coeff-{n_target}-{time.strftime('%Y%m%dT%H%M%SZ', time.gmtime())}"
    close_open_rungs(token)
    for host in hosts():
        code, body = api(host, token, "POST", "/v1/rung/open", {"id": rung})
        if code >= 300:
            raise RuntimeError(f"rung open failed on {host}: {body}")
    west_mark = log_mark(WEST, "west")
    east_mark = log_mark(EAST, "east")
    metrics_path = out_dir / f"fabric-metrics-{n_target}.jsonl"
    proc = sample_metrics(metrics_path, HOLD_SEC + 30)
    base_rows = fan_status(token)
    # transfers/relays come from the rung window, not lifetime
    log(f"hold begin target={n_target} rung={rung}")
    start = time.time()
    broken = None
    min_active = n_target
    try:
        while time.time() - start < HOLD_SEC:
            time.sleep(POLL_SEC)
            now = fan_status(token)
            active = fleet_active(now)
            min_active = min(min_active, active)
            if active != n_target:
                broken = f"active={active}"
                log(f"hold broken target={n_target} {broken}")
                break
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                proc.kill()
    held_for = int(time.time() - start)
    relays = 0
    transfers = 0
    for host in hosts():
        code, body = api(host, token, "POST", "/v1/rung/close", {"id": rung})
        if isinstance(body, dict):
            window = ((body.get("window") or {}).get("counters") or {})
            relays += int(window.get("relays") or 0)
            transfers += int(window.get("transfers") or 0)
        else:
            broken = broken or f"rung close {code}"
    west_info = props_and_actors(WEST, "west", west_mark)
    east_info = props_and_actors(EAST, "east", east_mark)
    online: list[str] | None = None
    for ip, info in ((WEST, west_info), (EAST, east_info)):
        if info.get("enable_rcon"):
            listed = rcon_list(ip, int(info.get("rcon_port") or 25575))
            if listed is not None:
                online = (online or []) + listed
    if online is not None:
        # A player listed on both sides during a relay would double-count.
        names = sorted(set(online))
        west_info["online"] = len(names)
        east_info["online"] = len(names)
        west_info["online_names"] = names
    metrics = summarize_metrics(metrics_path)
    exit_reason = broken or ""
    row = build_row(
        n_target=n_target,
        n_held=min_active if broken else n_target,
        hold_ok=broken is None and held_for >= HOLD_SEC - POLL_SEC,
        hold_sec=held_for,
        relays=relays,
        transfers=transfers,
        metrics=metrics,
        west_info=west_info,
        east_info=east_info,
        exit_reason=exit_reason,
    )
    row["rung_id"] = rung
    row["min_active"] = min_active
    if base_rows and not row["java_rss_ok"]:
        row["exit_reason"] = (row["exit_reason"] + " java rss missing").strip()
    return row


def clear_fleet(token: str) -> None:
    for host in hosts():
        api(host, token, "POST", "/v1/clients/clear", {})
    time.sleep(2)


def clear_when_finished() -> bool:
    return os.environ.get("COEFF_CLEAR_AT_END", "0").strip().lower() in {"1", "true", "yes"}


def climb_ceiling() -> int:
    raw = os.environ.get("COEFF_CEILING", "").strip()
    if not raw:
        return 0
    ceiling = int(raw)
    if ceiling <= 0:
        raise SystemExit(f"COEFF_CEILING must be a positive count, got {raw!r}")
    return ceiling


def resume_live() -> bool:
    return os.environ.get("COEFF_RESUME", "").strip().lower() in {"1", "true", "yes"}


def ladder_targets() -> list[int]:
    raw = os.environ.get("COEFF_LADDER", "").strip()
    if not raw:
        return list(LADDER)
    targets = [int(part) for part in raw.split(",") if part.strip()]
    if not targets or any(n <= 0 for n in targets):
        raise SystemExit(f"COEFF_LADDER must be positive counts, got {raw!r}")
    return targets


def append_row(path: Path, row: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a") as fh:
        fh.write(json.dumps(row, sort_keys=True) + "\n")
    log(
        f"actors_per_player={row.get('actors_per_player')} note={row.get('actors_note')} continuing"
    )
    log(
        f"row n_target={row['n_target']} n_held={row['n_held']} binding={row['binding']} "
        f"accepted={row['accepted']} relays={row['relays']} transfers={row['transfers']} "
        f"actors_per_player={row['actors_per_player']} dup={row['dup_uuid_warnings']} "
        f"reasons={row['reject_reasons']}"
    )


def main() -> int:
    if "--self-test" in sys.argv:
        return self_test()
    token = read_token()
    out_dir = RUN / f"coeff-{time.strftime('%Y%m%dT%H%M%SZ', time.gmtime())}"
    out_dir.mkdir(parents=True)
    jsonl = out_dir / "coefficients.jsonl"
    log(f"series 1 out={out_dir}")
    resuming = resume_live()
    if resuming:
        rows = fan_status(token)
        log(f"resuming live walkers active={fleet_active(rows)}; not clearing")
    else:
        ensure_walker_runtime(token)
        clear_fleet(token)
        rows = fan_status(token)
        if fleet_active(rows) != 0 or any(int(r.get("index_used") or 0) != 0 for r in rows):
            raise SystemExit("fleet not reset before the ladder")
    close_open_rungs(token)
    targets = ladder_targets()
    log(f"ladder {targets} ceiling={climb_ceiling() or 'none'} clear_at_end={clear_when_finished()}")
    index = 0
    opens_series_2 = False
    stopped = "ladder complete"
    try:
        while index < len(targets):
            target = targets[index]
            before = fleet_active(fan_status(token))
            delta = target - before
            if delta <= 0:
                raise SystemExit(f"fleet already at {before}, wanted to climb to {target}")
            log(f"add {delta} across loadgen slices toward fleet {target}")
            try:
                ok, detail = add_across_slices(token, delta)
            except SliceShortfall as exc:
                ok, detail = False, str(exc)
            if not ok:
                row = build_row(
                    n_target=target,
                    n_held=fleet_active(fan_status(token)),
                    hold_ok=False,
                    hold_sec=0,
                    relays=0,
                    transfers=0,
                    metrics={"ok": False, "per": {}},
                    west_info={},
                    east_info={},
                    exit_reason=f"ramp failed: {detail}",
                )
                append_row(jsonl, row)
                stopped = "ramp failed"
                break
            row = hold(token, target, out_dir)
            append_row(jsonl, row)
            if row.get("opens_series_2"):
                opens_series_2 = True
            if not row.get("hold_ok"):
                stopped = f"hold collapsed binding={row.get('binding')}"
                break
            index += 1
            if index == len(targets) and creep_allowed(row):
                nxt = target + CREEP
                ceiling = climb_ceiling()
                if ceiling and nxt > ceiling:
                    stopped = f"reached {target} under ceiling {ceiling}"
                else:
                    targets.append(nxt)
                    log(f"creep to {nxt}")
            elif index == len(targets):
                stopped = "no creep: hold was not an accepted unbound row"
    finally:
        if clear_when_finished():
            log("clearing walkers")
            try:
                clear_fleet(token)
            except Exception as exc:
                log(f"clear failed: {exc}")
        else:
            log("leaving walkers up")
    outcome = {
        "series": "8gb",
        "stopped": stopped,
        "opens_series_2": opens_series_2,
        "series_2": "not_opened" if not opens_series_2 else "eligible_not_created",
        "mesh_width": 2,
        "third_server_added": False,
        "coefficients": str(jsonl),
        "note": (
            "Series 2 needs a RAM binding with the actor gates passed. "
            "No b3-16 game pair was created. Mesh width stayed at 2."
        ),
    }
    (out_dir / "series-outcome.json").write_text(json.dumps(outcome, indent=2) + "\n")
    log(json.dumps(outcome))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        raise SystemExit(130)
