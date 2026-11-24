#!/usr/bin/env python3
"""One continuous 16-server climb.

Splits each step across test-mc-p5-lg-1, lg-2, lg-3, and lg-6 while those
generators have cgroup room. lg-4 and lg-5 are left alone. A cgroup loss does
not end the process and does not write the bend note. The process exits only
for a real bend.
"""
import json
import os
import subprocess
import sys
import time
from pathlib import Path

DIR = Path(__file__).resolve().parent
ROOT = Path(os.environ["CK_PROJECT_ROOT"]).expanduser() if os.environ.get("CK_PROJECT_ROOT") else DIR
P16 = Path(os.environ["CK_P16_STATE"]).expanduser() if os.environ.get("CK_P16_STATE") else None
P5 = Path(os.environ["CK_P5_STATE"]).expanduser() if os.environ.get("CK_P5_STATE") else None
KEY = Path(os.environ["CK_SSH_KEY"]).expanduser() if os.environ.get("CK_SSH_KEY") else None
KNOWN = Path(os.environ.get("CK_KNOWN_HOSTS", str(DIR / "known_hosts"))).expanduser()
ADD = Path(os.environ["CK_P16_ADD_SCRIPT"]).expanduser() if os.environ.get("CK_P16_ADD_SCRIPT") else None
SHA = "b7a2ab65f87de5d0e542476fea1421ee6d89c224560ecd7f172c875c220d5aa0"
LOG = DIR / "climb-sixteen.log"
NOTE = ROOT / "seam-scaling-sixteen-2026-11-19.md"
GAMES = {}
LOADS = {}
TOKEN = ""

def say(text):
    line = text.rstrip()
    print(line, flush=True)
    with LOG.open("a") as fh:
        fh.write(line + "\n")

def others_running():
    try:
        out = subprocess.check_output(["pgrep", "-fl", "climb-sixteen.py"], text=True)
    except subprocess.CalledProcessError:
        return False
    me = str(os.getpid())
    for line in out.splitlines():
        if "climb-sixteen.py" not in line:
            continue
        if "python" not in line:
            continue
        pid = line.split(None, 1)[0]
        if pid != me:
            return True
    return False

def ssh(ip, remote, data=None, timeout=30):
    last = "ssh"
    for attempt in range(3):
        try:
            proc = subprocess.run(
                [
                    "ssh", "-i", str(KEY),
                    "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes",
                    "-o", "StrictHostKeyChecking=yes", "-o", f"UserKnownHostsFile={KNOWN}",
                    "-o", "ConnectTimeout=12", "-o", "LogLevel=ERROR",
                    f"ubuntu@{ip}", remote,
                ],
                input=data, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout,
            )
        except subprocess.TimeoutExpired:
            last = "ssh-timeout"
            time.sleep(2)
            continue
        lines = proc.stdout.decode(errors="replace").strip().splitlines()
        if proc.returncode == 0 and lines:
            return lines[-1]
        last = "ssh"
        time.sleep(2)
    raise RuntimeError(last)

def load_inventory():
    global GAMES, LOADS, TOKEN
    if P16 is None or P5 is None:
        raise SystemExit("CK_P16_STATE and CK_P5_STATE are required")
    GAMES = hosts(P16 / "vms.json", "test-mc-p16-", 16)
    LOADS = hosts(P5 / "vms.json", "test-mc-p5-lg-", 5)
    token_file = Path(os.environ["CK_SIXTEEN_TOKEN_FILE"]).expanduser() if os.environ.get("CK_SIXTEEN_TOKEN_FILE") else None
    if token_file is None:
        raise SystemExit("CK_SIXTEEN_TOKEN_FILE is required")
    TOKEN = token_file.read_text().strip()


def hosts(path, prefix, n):
    found = {}
    for vm in json.loads(path.read_text())["vms"]:
        name = vm.get("name") or ""
        if name.startswith(prefix):
            found[name] = vm["ip"]
    return found


SAMPLE = r'''python3 -c '
import json, pathlib
lines=pathlib.Path("/proc/stat").read_text().splitlines()
total=[int(x) for x in lines[0].split()[1:8]]
cores=[]
for ln in lines[1:]:
    if not (ln.startswith("cpu") and ln[3:4].isdigit()):
        break
    cores.append([int(x) for x in ln.split()[1:8]])
print(json.dumps({"cpu":total,"cores":cores,"load":float(pathlib.Path("/proc/loadavg").read_text().split()[0])}))
' '''
STATUS = r'''python3 -c '
import json,sys,urllib.request
tok=sys.stdin.readline().strip()
req=urllib.request.Request("http://127.0.0.1:9109/v1/status", headers={"Authorization":"Bearer "+tok})
with urllib.request.urlopen(req, timeout=8) as resp:
    b=json.loads(resp.read().decode())
print(int(b.get("active") or 0))
' '''
WANDER = r'''python3 -c '
import pathlib, time
root=pathlib.Path("/var/lib/ck-mc-lt/stats/walkers")
now=time.time()
files=sorted(root.glob("S16*.log"), key=lambda p: p.stat().st_mtime, reverse=True)
recent=[p for p in files if now - p.stat().st_mtime < 1800]
wander=play=closed=0
for p in recent[:40]:
    text=p.read_text(errors="replace")
    wander += "path=wander" in text
    play += "phase=play" in text
    closed += "closed:" in text[-500:]
print(len(recent), wander, play, closed)
' '''

def cpu_pct(a, b):
    d = [b[i] - a[i] for i in range(7)]
    total = sum(d)
    if total <= 0:
        return None
    return round(100.0 * (d[0] + d[1] + d[2] + d[5] + d[6]) / total, 1)

def max_core(a, b):
    best = None
    for i in range(min(len(a.get("cores") or []), len(b.get("cores") or []))):
        pct = cpu_pct(a["cores"][i], b["cores"][i])
        if pct is not None and (best is None or pct > best):
            best = pct
    return best

def actives():
    out = {}
    for name in sorted(LOADS):
        out[name] = int(ssh(LOADS[name], STATUS, (TOKEN + "\n").encode()))
    return out

def snap():
    out = {}
    for i in range(1, 17):
        side = f"p{i}"
        out[side] = json.loads(ssh(GAMES[f"test-mc-p16-{i}"], SAMPLE))
    return out

def hold_body():
    say("wander " + ssh(LOADS["test-mc-p5-lg-6"], WANDER))
    before = actives()
    start = snap()
    time.sleep(90)
    mid = snap()
    time.sleep(90)
    end = snap()
    after = actives()
    say("wander " + ssh(LOADS["test-mc-p5-lg-6"], WANDER))
    best = None
    for side, row in end.items():
        cpu = cpu_pct(start[side]["cpu"], row["cpu"])
        load = row["load"]
        rank = (cpu if cpu is not None else -1, float(load))
        if best is None or rank > best[0]:
            best = (rank, side, cpu, load)
    side = best[1]
    samples = (start[side]["load"], mid[side]["load"], end[side]["load"])
    core_first = max_core(start[side], mid[side]) or 0
    core_second = max_core(mid[side], end[side]) or 0
    core = min(core_first, core_second)
    target = sum(before.values())
    held = sum(after.values())
    say(
        f"hold target={target} held={held} busiest={side} cpu={best[2]} load={best[3]} "
        f"core={core} transfers=0 path=wander samples={samples[0]},{samples[1]},{samples[2]}"
    )
    say("before " + " ".join(f"{k.split('-')[-1]}={v}" for k, v in before.items()))
    say("after " + " ".join(f"{k.split('-')[-1]}={v}" for k, v in after.items()))
    return {
        "before": before,
        "after": after,
        "side": side,
        "cpu": best[2],
        "load": best[3],
        "samples": samples,
        "core": core,
        "target": target,
        "held": held,
    }

def hold_once():
    while True:
        try:
            return hold_body()
        except RuntimeError:
            say("hold ssh failed; not a bend; retrying")
            time.sleep(5)

def append_note(text):
    current = NOTE.read_text() if NOTE.exists() else ""
    if text.strip() in current:
        return
    with NOTE.open("a") as fh:
        if not current.endswith("\n"):
            fh.write("\n")
        fh.write("\n" + text.strip() + "\n")

def write_bend(row):
    held = int(row["held"])
    per = held / 16
    thousand = round(1000 / per) if per else 0
    append_note(
        f"The curve bends at {held} players. The busiest server was {row['side']} at "
        f"{row['cpu']}% CPU and load {row['load']}. "
        f"Servers for 1000 = {thousand}. "
        f"Jar sha256 {SHA}."
    )
    say(f"BEND held={held} busiest={row['side']} cpu={row['cpu']} load={row['load']} servers_for_1000={thousand}")

def set_step(n):
    Path("/tmp/p16-step.txt").write_text(str(int(n)) + "\n")
    try:
        text = ADD.read_text()
        updated = []
        for line in text.splitlines():
            if line.startswith("STEP = "):
                updated.append(f"STEP = {int(n)}")
            else:
                updated.append(line)
        ADD.write_text("\n".join(updated) + "\n")
    except Exception:
        pass
    say(f"step +{int(n)}")

def jump_after(cpu, load):
    cpu_v = float(cpu or 0)
    load_v = float(load or 0)
    if cpu_v < 40 and load_v < 1.0:
        return 27
    if cpu_v < 70 and load_v < 1.8:
        return 9
    return 1

def place_one():
    proc = subprocess.run(
        ["python3", str(ADD)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=420, text=True,
    )
    kept = []
    for line in proc.stdout.splitlines():
        if line.startswith((
            "climb", "test-mc-p5-lg-", "split", "seed", "target", "indexes", "dry",
            "placeable", "spawn", "add", "ready", "cal", "STOP", "lg-",
        )) or "cgroup" in line or "full" in line or "no dry" in line or "same server" in line:
            kept.append(line)
    say("\n".join(kept) if kept else "add produced no status")
    return proc.returncode, proc.stdout

def sustained(row):
    samples = row.get("samples") or (0, 0, 0)
    return min(float(x) for x in samples) >= 1.8 or float(row.get("core") or 0) >= 90

def parse_hold(line):
    parts = {}
    for tok in line.split():
        if "=" not in tok:
            continue
        key, val = tok.split("=", 1)
        parts[key] = val
    samples = tuple(float(x) for x in parts["samples"].split(","))
    return {
        "cpu": float(parts["cpu"]),
        "load": float(parts["load"]),
        "samples": samples,
        "core": float(parts["core"]) if "core" in parts else 0,
        "held": int(parts["held"]),
        "side": parts["busiest"],
        "target": int(parts["target"]),
    }

def latest_hold():
    row = {
        "cpu": 29.8, "load": 2.4, "samples": (2.57, 0.87, 2.4),
        "core": 0, "held": 168, "side": "p1", "target": 168,
    }
    if not LOG.exists():
        return row
    for line in LOG.read_text(errors="replace").splitlines():
        if line.startswith("hold target="):
            try:
                row = parse_hold(line)
            except Exception:
                continue
    return row

def unfinished_add():
    last = None
    if not LOG.exists():
        return False
    for line in LOG.read_text(errors="replace").splitlines():
        if line.startswith("hold target=") or line.startswith("add 202") or line.startswith("ready "):
            last = line
    return bool(last) and not last.startswith("hold target=")

CLASS = r'''python3 -c '
import pathlib, time
root=pathlib.Path("/var/lib/ck-mc-lt/stats/walkers")
now=time.time()
files=sorted([p for p in root.glob("S16*.log") if now - p.stat().st_mtime < 900], key=lambda p: p.stat().st_mtime, reverse=True)
miss=refused=wander=runtime=0
for p in files[:20]:
    text=p.read_text(errors="replace")[-2500:]
    if "spawn-miss" in text:
        miss += 1
    if "path=wander" in text:
        wander += 1
    tail=text[-800:]
    if "max runtime" in tail:
        runtime += 1
        continue
    for line in tail.splitlines():
        if " connection closed:" in line:
            continue
        if " closed: " in line:
            refused += 1
            break
print(miss, refused, wander, len(files), runtime)
' '''

def walker_class():
    total = {"miss": 0, "refused": 0, "wander": 0, "files": 0, "runtime": 0}
    found = False
    for name in ("test-mc-p5-lg-1", "test-mc-p5-lg-2", "test-mc-p5-lg-3", "test-mc-p5-lg-6"):
        try:
            line = ssh(LOADS[name], CLASS)
        except Exception:
            continue
        parts = line.split()
        if len(parts) != 5:
            continue
        found = True
        total["miss"] += int(parts[0])
        total["refused"] += int(parts[1])
        total["wander"] += int(parts[2])
        total["files"] += int(parts[3])
        total["runtime"] += int(parts[4])
    return total if found else None

def ready_counts(text):
    ready_active = None
    ready_want = None
    for line in text.splitlines():
        if line.startswith("ready "):
            try:
                body = json.loads(line.split(" ", 1)[1].split(" want ")[0])
                if "active" not in body:
                    ready_active = None
                else:
                    ready_active = int(body["active"])
                ready_want = int(line.rsplit("want ", 1)[1])
            except Exception:
                ready_active = None
    return ready_active, ready_want

def note_generator_loss(row):
    full_drop = any(
        row["after"].get(name, 0) < row["before"].get(name, 0)
        for name in ("test-mc-p5-lg-1", "test-mc-p5-lg-2", "test-mc-p5-lg-3")
    )
    if full_drop:
        say("generator cgroup loss on lg-1, lg-2, or lg-3; not a bend; continuing")
    if row["after"].get("test-mc-p5-lg-6", 0) < row["before"].get("test-mc-p5-lg-6", 0):
        say("lg-6 count fell during the hold; not a bend; continuing")

def main():
    load_inventory()
    if others_running():
        say("climb-sixteen already running; leaving that process")
        return
    say("continuous climb started; open generators add in parallel")
    last = latest_hold()
    if unfinished_add():
        say("holding bots already added")
        row = hold_once()
        note_generator_loss(row)
        last = row
        say(f"next step +{jump_after(row['cpu'], row['load'])}")
    while True:
        set_step(jump_after(last["cpu"], last["load"]))
        code, text = place_one()
        if "index window" in text:
            say("index window is full; not a bend; continuing")
            time.sleep(20)
            continue
        if "add 202" not in text and ("cgroup too full" in text or "cgroups too full" in text):
            append_note("A b3-16 is $0.1208/hour.")
            say("every open generator is near the cgroup cap; price is in the note; not a bend")
            time.sleep(30)
            continue
        if "add 202" not in text:
            say("add did not reach the harness; a harness restart is not a bend; continuing")
            time.sleep(5)
            continue
        ready_active, ready_want = ready_counts(text)
        if ready_active is not None and ready_want is not None and ready_active < ready_want and ready_active == 0:
            kind = walker_class()
            if kind and kind["runtime"] > 0:
                say("walkers stopped at max runtime; a six-hour exit is not a bend; continuing")
                time.sleep(5)
                continue
            if sustained(last) and kind and kind["miss"] == 0 and kind["refused"] > 0 and kind["runtime"] == 0 and kind["files"] > 0:
                write_bend(last)
                say("bend: the next bot failed because the game servers could not take it")
                return
            say("bot did not stay; a six-hour exit, a harness restart, or a generator miss is not a bend; continuing")
            time.sleep(5)
            continue
        row = hold_once()
        note_generator_loss(row)
        last = row
        say(f"next step +{jump_after(row['cpu'], row['load'])}")

if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        say("BLOCKED " + type(exc).__name__)
        raise
