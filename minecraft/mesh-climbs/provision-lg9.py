#!/usr/bin/env python3
"""Install test-mc-f4-lg-9 and open one index window. Prints no addresses."""
import json
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import climb_mesh  # noqa: E402

FLEET = Path(os.environ["CK_F4_FLEET_FILE"]).expanduser() if os.environ.get("CK_F4_FLEET_FILE") else None
FOUR = Path(os.environ["CK_FOUR_LOADGENS_FILE"]).expanduser() if os.environ.get("CK_FOUR_LOADGENS_FILE") else None
MESH = climb_mesh.four_mesh()
GAMES = (
    ("test-mc-west3", "west"),
    ("test-mc-east3", "east"),
    ("test-mc-north3", "north"),
    ("test-mc-ne3", "ne"),
)
NAME = "test-mc-f4-lg-9"
DONOR = "test-mc-f4-lg-6"
BASE = 800
LIMIT = 48

UNIT = """[Unit]
Description=CK Minecraft harness
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=ubuntu
WorkingDirectory=/opt/ck-mc-harness
EnvironmentFile=/opt/ck-mc-harness/lt.env
ExecStart=/usr/bin/node /opt/ck-mc-harness/minecraft/mc-harness.mjs
Restart=no
OOMPolicy=continue
MemoryMax=12G
OOMScoreAdjust=-500
LimitNOFILE=200000
KillSignal=SIGTERM
TimeoutStopSec=30

[Install]
WantedBy=multi-user.target
"""

PACKAGES = r"""python3 - << 'PY'
import shutil, subprocess, time, urllib.request

def locked():
    for path in ("/var/lib/dpkg/lock-frontend", "/var/lib/dpkg/lock", "/var/lib/apt/lists/lock"):
        proc = subprocess.run(["sudo", "-n", "fuser", path], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if proc.returncode == 0:
            return True
    proc = subprocess.run(["pgrep", "-f", "unattended-upgr"], stdout=subprocess.DEVNULL)
    return proc.returncode == 0

def first_num(cmd):
    try:
        out = subprocess.check_output(cmd, stderr=subprocess.STDOUT, text=True, timeout=20)
    except subprocess.CalledProcessError as exc:
        out = exc.output or ""
    except Exception:
        return ""
    for token in out.replace('"', " ").replace("v", " ").split():
        if token[:1].isdigit():
            return token
    return ""

def apt(*args):
    return subprocess.run(["sudo", "-n", "DEBIAN_FRONTEND=noninteractive", "apt-get", *args], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

ready_java = first_num(["java", "-version"])
ready_node = first_num(["/usr/bin/node", "-v"])
ready_ran_java = subprocess.run(["java", "-version"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
ready_ran_node = subprocess.run(["/usr/bin/node", "-e", "process.exit(0)"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
if ready_java.startswith("25") and ready_node == "22.23.5" and ready_ran_java and ready_ran_node:
    print("25", "22.23.5", 1, 1)
    raise SystemExit(0)
deadline = time.time() + 3600
while time.time() < deadline and locked():
    time.sleep(15)
if locked():
    raise SystemExit("locked")
if not first_num(["java", "-version"]).startswith("25"):
    apt("update", "-qq")
    apt("install", "-y", "-qq", "openjdk-25-jre-headless")
if shutil.which("curl") is None:
    apt("install", "-y", "-qq", "curl", "ca-certificates", "xz-utils")
else:
    apt("install", "-y", "-qq", "xz-utils")
node = first_num(["/usr/bin/node", "-v"])
if node != "22.23.5":
    url = "https://nodejs.org/dist/v22.23.5/node-v22.23.5-linux-x64.tar.xz"
    urllib.request.urlretrieve(url, "/tmp/node-v22.23.5.tar.xz")
    subprocess.check_call(["sudo", "-n", "tar", "-xJf", "/tmp/node-v22.23.5.tar.xz", "-C", "/usr/local", "--strip-components=1"])
    subprocess.check_call(["sudo", "-n", "ln", "-sfn", "/usr/local/bin/node", "/usr/bin/node"])
    subprocess.check_call(["sudo", "-n", "ln", "-sfn", "/usr/local/bin/npm", "/usr/bin/npm"])
java = first_num(["java", "-version"])
node = first_num(["/usr/bin/node", "-v"])
ran_java = subprocess.run(["java", "-version"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
ran_node = subprocess.run(["/usr/bin/node", "-e", "process.exit(0)"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
print(java, node, int(ran_java), int(ran_node))
if not (java.startswith("25") and node == "22.23.5" and ran_java and ran_node):
    raise SystemExit("tools")
PY"""


def fleet_ip():
    data = json.loads(FLEET.read_text())
    for row in data.get("vms") or []:
        if row.get("name") == NAME and row.get("ip"):
            return row["ip"]
    return ""


def windows():
    names = list(MESH.loads)
    found, _missing = climb_mesh.resolve(names)
    loads = [(name, found[name]) for name in names if name in found]
    def read(item):
        return climb_mesh.api(MESH, item[1], "GET", "/v1/status", timeout=25)
    rows = climb_mesh.fan(loads, read, workers=min(8, len(loads) or 1))
    spans = []
    for name, st in rows.items():
        if not isinstance(st, dict) or st.get("error"):
            continue
        base = int(st.get("index_base") or 0)
        limit = int(st.get("index_limit") or 0)
        if limit > 0:
            spans.append((name, base, base + limit))
    return spans


def overlaps(spans, base, limit):
    end = base + limit
    return [name for name, start, stop in spans if start < end and base < stop]


def main():
    deadline = time.time() + 300
    ip = ""
    while time.time() < deadline:
        ip = fleet_ip()
        if ip:
            try:
                text = climb_mesh.ssh(MESH, ip, "echo ssh-ready", timeout=20)
            except Exception:
                time.sleep(10)
                continue
            if text.strip().splitlines()[-1:] == ["ssh-ready"]:
                break
        time.sleep(10)
    else:
        raise SystemExit("ssh")
    print("ssh-ready")
    text = climb_mesh.ssh(MESH, ip, PACKAGES, timeout=4200)
    print("tools", text.strip().splitlines()[-1])
    found, missing = climb_mesh.resolve([DONOR])
    if missing:
        raise SystemExit("donor")
    src = found[DONOR]
    if "yes" not in climb_mesh.ssh(MESH, ip, "test -f /opt/ck-mc-harness/minecraft/mc-harness.mjs && test -d /opt/ck-mc-loadwalker && echo yes || echo no", timeout=20):
        packed = subprocess.Popen(
            climb_mesh_ssh_cmd(src) + ["tar -C /opt -czf - --warning=no-file-changed --exclude='*.log' ck-mc-harness ck-mc-loadwalker"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        unpacked = subprocess.Popen(
            climb_mesh_ssh_cmd(ip) + ["sudo -n mkdir -p /opt /var/lib/ck-mc-lt/stats && sudo -n chown -R ubuntu:ubuntu /opt /var/lib/ck-mc-lt && tar -C /opt -xzf -"],
            stdin=packed.stdout,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        packed.stdout.close()
        out, err = unpacked.communicate(timeout=900)
        packed.communicate(timeout=30)
        if unpacked.returncode != 0 or packed.returncode not in (0, 1):
            raise SystemExit("copy")
    copied = climb_mesh.ssh(MESH, ip, "test -f /opt/ck-mc-harness/minecraft/mc-harness.mjs && test -d /opt/ck-mc-loadwalker && echo yes || echo no", timeout=20)
    if copied.strip().splitlines()[-1] != "yes":
        raise SystemExit("copy-missing")
    print("copied")
    games, missing = climb_mesh.resolve([name for name, _side in GAMES])
    if missing or not fleet_ip():
        raise SystemExit("resolve")
    spans = windows()
    hit = overlaps(spans, BASE, LIMIT)
    if hit:
        raise SystemExit("overlap")
    hosts = {side: games[name] for name, side in GAMES}
    climb_mesh.write_env(MESH, ip, BASE, LIMIT, hosts)
    print("env-updated")
    if climb_mesh.java_count(MESH, ip) > 0:
        raise SystemExit("walkers")
    climb_mesh.ssh(
        MESH,
        ip,
        "sudo -n tee /etc/systemd/system/ck-mc-harness.service >/dev/null && sudo -n systemctl daemon-reload && sudo -n systemctl enable ck-mc-harness.service && sudo -n systemctl restart ck-mc-harness.service && sleep 2 && systemctl is-active ck-mc-harness.service && python3 -c 'print(open(\"/sys/fs/cgroup/system.slice/ck-mc-harness.service/memory.max\").read().strip())'",
        UNIT.encode(),
        timeout=60,
    )
    deadline = time.time() + 90
    ready = False
    while time.time() < deadline:
        try:
            health = climb_mesh.ssh(MESH, ip, "systemctl is-active ck-mc-harness; curl -sf -m 5 http://127.0.0.1:9109/health || true", timeout=25)
        except Exception:
            time.sleep(2)
            continue
        lines = [ln.strip() for ln in health.splitlines() if ln.strip()]
        if lines and lines[0] == "active" and '"ok":true' in "".join(lines).replace(" ", ""):
            ready = True
            break
        time.sleep(2)
    if not ready:
        raise SystemExit("harness")
    st = climb_mesh.api(MESH, ip, "GET", "/v1/status", timeout=25)
    java = climb_mesh.java_count(MESH, ip)
    base = int(st.get("index_base") or 0)
    limit = int(st.get("index_limit") or 0)
    if base != BASE or limit != LIMIT:
        pinned = climb_mesh.api(MESH, ip, "POST", "/v1/index", {"base": BASE, "limit": LIMIT}, timeout=25)
        if isinstance(pinned, dict) and pinned.get("error"):
            raise SystemExit("pin")
        st = climb_mesh.api(MESH, ip, "GET", "/v1/status", timeout=25)
        base = int(st.get("index_base") or 0)
        limit = int(st.get("index_limit") or 0)
    if base != BASE or limit != LIMIT or java:
        raise SystemExit("pin")
    rows = json.loads(FOUR.read_text()) if FOUR.exists() else []
    if not any(isinstance(row, dict) and row.get("name") == NAME for row in rows):
        rows.append({"name": NAME, "ip": ip})
        tmp = FOUR.with_suffix(".json.tmp")
        tmp.write_text(json.dumps(rows, indent=2) + "\n")
        tmp.chmod(0o600)
        tmp.replace(FOUR)
    print("registered", "base", base, "limit", limit, "active", int(st.get("active") or 0), "java", java)


def climb_mesh_ssh_cmd(ip):
    return [
        "ssh", "-i", str(climb_mesh.KEY),
        "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes",
        "-o", f"UserKnownHostsFile={climb_mesh.KNOWN}",
        "-o", "StrictHostKeyChecking=accept-new",
        "-o", "ConnectTimeout=15", "-o", "LogLevel=ERROR",
        f"ubuntu@{ip}",
    ]


if __name__ == "__main__":
    try:
        main()
    except SystemExit as exc:
        code = exc.code
        print("failed", code if isinstance(code, str) else "exit")
        raise SystemExit(1)
    except Exception as exc:
        msg = str(exc)
        reason = "error"
        for token in ("locked", "tools", "copy", "overlap", "resolve", "pin", "harness", "walkers", "donor", "ssh", "env"):
            if token in msg:
                reason = token
                break
        print("failed", type(exc).__name__, reason, flush=True)
        raise SystemExit(1)
