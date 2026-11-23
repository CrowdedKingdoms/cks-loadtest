#!/usr/bin/env python3
"""Add-only mesh climbs. Never clears a bot, and never kills a running JVM.

A stuck bot and a bot still on an older wander script stay in the held count.
A newer wander script is copied into the dist directory and is loaded by the
next pack's new process. Placement code is re-read by the harness on the next
pose. This module does not start a climb unless run() is called.
"""
from __future__ import annotations

import csv
import hashlib
import json
import os
import re
import subprocess
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import bend

HERE = Path(__file__).resolve().parent
DIR = Path(os.environ.get("CK_MESH_DIR", HERE)).expanduser()
ROOT = Path(os.environ.get("CK_PROJECT_ROOT", "")).expanduser() if os.environ.get("CK_PROJECT_ROOT") else None
KEY = Path(os.environ["CK_SSH_KEY"]).expanduser() if os.environ.get("CK_SSH_KEY") else None
KNOWN = Path(os.environ.get("CK_KNOWN_HOSTS", DIR / "known_hosts")).expanduser()
SPAWN = DIR / "section-spawn.cjs"
PLACE = DIR / "section-place.mjs"
PENDING_WANDER = DIR / "pending-wander"
LOCAL_COLUMNS = DIR / "section-columns.json"

JAR_SHA = "b7a2ab65f87de5d0e542476fea1421ee6d89c224560ecd7f172c875c220d5aa0"
COMMIT = "c9ff2a3f3c711de6d5af17ef2f36e7b913c6e252"
PACK_SIZE = int(os.environ.get("CK_PACK_SIZE", "9"))
PACK_BASE_KB = 457884
PACK_EACH_KB = 1243
PACK_HEAP = "2g"
WANDER_BPS = "2"
HOLD_SEC = 180
STAGGER_MS = 1000
PIN_SPAN = 800
# Seven days. write_env copies this into lt.env only while a harness is idle.
# A running harness keeps the value it had at start; run.sh overrides it for the next JVM.
RUNTIME_S = "604800"

IP_RE = re.compile(r"\b\d{1,3}(?:\.\d{1,3}){3}\b")
KNOWN_HOSTS_WARNING = re.compile(
    r"^Warning: Permanently added .+ to the list of known hosts\.$"
)
UUID_RE = re.compile(r"\b[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\b", re.I)
SECRET_RE = re.compile(
    r"(Bearer\s+)\S+|((?:TOKEN|PASSWORD|SECRET|PASS)[A-Za-z0-9_]*=)\S+",
    re.I,
)

INVENTORIES = [
    Path(item).expanduser()
    for item in os.environ.get("CK_INVENTORY_FILES", "").split(os.pathsep)
    if item.strip()
]


def project_root():
    if ROOT is not None:
        return ROOT
    raise SystemExit("CK_PROJECT_ROOT is required")


def climb_file(name):
    base = ROOT if ROOT is not None else DIR
    return base / name


def optional_env_path(name):
    raw = os.environ.get(name, "").strip()
    return Path(raw).expanduser() if raw else None


def env_path(name):
    path = optional_env_path(name)
    if path is None:
        raise SystemExit(f"{name} is required")
    return path


def redact(text):
    out = SECRET_RE.sub(lambda m: (m.group(1) or m.group(2)) + "[redacted]", text or "")
    out = UUID_RE.sub("<id>", out)
    return IP_RE.sub("<ip>", out)


def log(mesh, msg):
    line = f"{time.strftime('%H:%M:%S')} {msg}"
    print(line, flush=True)
    with mesh.log_path.open("a") as fh:
        fh.write(line + "\n")


def four_mesh():
    return Mesh(
        name="four",
        title="Four-server mesh",
        sections=("west", "east", "north", "ne"),
        games=(
            ("test-mc-west3", "west"),
            ("test-mc-east3", "east"),
            ("test-mc-north3", "north"),
            ("test-mc-ne3", "ne"),
        ),
        loads=tuple(f"test-mc-lg-{i}" for i in range(1, 6)) + ("test-mc-loadgen-1", "test-mc-f4-lg-1", "test-mc-f4-lg-2", "test-mc-f4-lg-3", "test-mc-f4-lg-4", "test-mc-f4-lg-5", "test-mc-f4-lg-6", "test-mc-f4-lg-7", "test-mc-f4-lg-8", "test-mc-f4-lg-9", "test-mc-f4-lg-10", "test-mc-f4-lg-11", "test-mc-f4-lg-12", "test-mc-f4-lg-13", "test-mc-f4-lg-14", "test-mc-f4-lg-15", "test-mc-f4-lg-16", "test-mc-f4-lg-17", "test-mc-f4-lg-18", "test-mc-f4-lg-19", "test-mc-f4-lg-20", "test-mc-f4-lg-21", "test-mc-f4-lg-22", "test-mc-f4-lg-23", "test-mc-f4-lg-24", "test-mc-f4-lg-25"),
        prefix="F4",
        # Walker logs are named LoadN. F4 is only the climb label.
        bot_prefix="Load",
        # Armed window is 2496..2520. Index 2521's first new column is only a worldgen heightmap.
        # Index 2485 stays unknown and is outside this window. 2096..2399 are not in a live window.
        # This prefix stays at the hole. A later harness window whose own columns are measured
        # is still usable. The unknown index is not filled in, and the table is not rebuilt.
        measured_index_limit=2521,
        # Printed ceiling is this exclusive end. It stays past the armed window
        # so that window is not capped. The armed window is 5910..5910, the
        # first measured index after 2700. 2634..2700 stays inside this
        # ceiling. Index 2633 stays unknown. The harness limit, not this
        # field, stops before 2521.
        attached_index_limit=5911,
        note_path=climb_file("seam-scaling-four-2026-11-19.md"),
        csv_path=climb_file("seam-scaling-four-2026-11-19.csv"),
        log_path=DIR / "climb-four.log",
        token_path=optional_env_path("CK_FOUR_TOKEN_FILE"),
        app_path=None,
        reject_app_paths=tuple(
            Path(item).expanduser()
            for item in os.environ.get("CK_FOUR_REJECT_APP_FILES", "").split(os.pathsep)
            if item.strip()
        ),
    )


def sixteen_mesh():
    return Mesh(
        name="sixteen",
        title="16-server mesh",
        sections=tuple(f"p{i}" for i in range(1, 17)),
        games=tuple((f"test-mc-p16-{i}", f"p{i}") for i in range(1, 17)),
        loads=tuple(f"test-mc-p5-lg-{i}" for i in range(1, 6)),
        prefix="S16",
        # section-columns.json has a dry point for indexes 0..2047 and not after.
        measured_index_limit=2048,
        note_path=climb_file("seam-scaling-sixteen-2026-11-19.md"),
        csv_path=climb_file("seam-scaling-sixteen-2026-11-19.csv"),
        log_path=DIR / "climb-sixteen.log",
        token_path=optional_env_path("CK_SIXTEEN_TOKEN_FILE"),
        app_path=optional_env_path("CK_SIXTEEN_APP_FILE"),
        reject_app_paths=tuple(
            Path(item).expanduser()
            for item in os.environ.get("CK_SIXTEEN_REJECT_APP_FILES", "").split(os.pathsep)
            if item.strip()
        ),
    )


def nine_mesh():
    loads = tuple(
        name.strip()
        for name in os.environ.get(
            "CK_NINE_LOADS",
            ",".join(f"test-mc-p5-lg-{i}" for i in range(10, 19)),
        ).split(",")
        if name.strip()
    )
    return Mesh(
        name="nine",
        title="Nine-server mesh",
        sections=tuple(f"p{i}" for i in range(1, 10)),
        games=tuple((f"test-mc-p5-{i}", f"p{i}") for i in range(1, 10)),
        loads=loads,
        prefix="N9",
        bot_prefix="Load",
        note_path=climb_file("seam-scaling-nine-2026-11-23.md"),
        csv_path=climb_file("seam-scaling-nine-2026-11-23.csv"),
        log_path=DIR / "climb-nine.log",
        token_path=optional_env_path("CK_NINE_TOKEN_FILE"),
        app_path=optional_env_path("CK_NINE_APP_FILE"),
        reject_app_paths=tuple(
            Path(item).expanduser()
            for item in os.environ.get("CK_NINE_REJECT_APP_FILES", "").split(os.pathsep)
            if item.strip()
        ),
    )


class Mesh:
    def __init__(self, **kwargs):
        self.__dict__.update(kwargs)

    @property
    def servers(self):
        return len(self.sections)


def app_id(path):
    if path is None or not Path(path).exists():
        return ""
    data = json.loads(Path(path).read_text())
    return str(data.get("appId") or "")


def walk_hosts(node, found):
    if isinstance(node, dict):
        name = node.get("name")
        ip = node.get("ip") or node.get("publicIp")
        if isinstance(name, str) and isinstance(ip, str) and name.startswith("test-mc-"):
            found.setdefault(name, ip)
        for value in node.values():
            walk_hosts(value, found)
    elif isinstance(node, list):
        for item in node:
            walk_hosts(item, found)


def inventory():
    found = {}
    for path in INVENTORIES:
        if not path.exists():
            continue
        try:
            walk_hosts(json.loads(path.read_text()), found)
        except json.JSONDecodeError:
            continue
    return found


def resolve(names):
    found = inventory()
    missing = [name for name in names if name not in found]
    return {name: found[name] for name in names if name in found}, missing


def columns_for(mesh):
    sides = list(mesh.sections)
    base = [
        "speed_bps",
        "n_target",
        "n_held",
        "result",
        "busiest",
        "busiest_cpu",
        "busiest_load1",
        "busiest_mem_avail_mb",
        "binding",
        "transfers",
    ]
    extra = []
    for kind in ("handoffs_to", "handoffs_from", "moved_wrongly"):
        extra.extend(f"{kind}_{side}" for side in sides)
    return base + extra + ["moving_sample", "wander_sample", "note", "hot_whole", "stop_reason", "mod_commit", "mod_sha256", "version_certainty"]


def rules_text(mesh):
    loads = ", ".join(mesh.loads)
    games = ", ".join(name for name, _side in mesh.games)
    return "\n".join([
        "",
        "## Climb",
        "",
        "Every new bot is placed at a seeded point in its own section, inset 32 blocks from every edge of that section, with its feet one block above the ground. The section is index modulo "
        f"{mesh.servers}. A retry of the same index stands on the same block. The point is anywhere in the cell, so some start near a cut. It is not an edge band and not a junction. Open water is skipped for another dry point in the same section. A column that has not been measured is skipped the same way and is not given a sea-level Y. If every candidate is unmeasured, that index stays unplaced. The add is a spawn miss and does not clear bots that are already seated.",
        "",
        f"The driver only adds. A stuck bot, or a bot still running an older wander script, stays in the count. A newer wander script in `{PENDING_WANDER.name}/` is copied for the next pack. A running JVM is not killed and is not retargeted to pick that script up. Packs of {PACK_SIZE}. Charge {PACK_BASE_KB} KB plus {PACK_EACH_KB} KB for each further bot, on a {PACK_HEAP} heap. Wander at {WANDER_BPS} blocks/s. `ClientboundTransferPacket` is logged and not followed.",
        "",
        f"CSV: `{mesh.csv_path.name}`",
        "",
        f"Game hosts: {games}.",
        f"Bot hosts, when the climb runs: {loads}.",
        "",
        "A clean hold is 180 seconds with transfers 0. The step is +27 while the busiest server is under 40% CPU and load 1.0, +9 under 70% and load 1.8, otherwise +1. The bend is a hold where load stays at or above 1.8 for the whole 180 seconds, or a core stays at or above 90%, and the next count fails because the game servers refused it. A six-hour walker exit, a harness restart, and a full index window are not bends. Both rows are recorded. A load-generator miss is named in the CSV and is not this mesh's cap.",
    ])


def climb_preamble(mesh):
    return rules_text(mesh) + "\n\nNot started. No bots have been placed by this driver.\n"


def append_note_line(mesh, paragraph):
    """Append one paragraph. Does not rewrite earlier notes."""
    text = paragraph.strip()
    if not text:
        return
    path = mesh.note_path
    path.parent.mkdir(parents=True, exist_ok=True)
    existing = path.read_text() if path.exists() else ""
    if text in existing:
        return
    with path.open("a") as fh:
        if existing and not existing.endswith("\n"):
            fh.write("\n")
        fh.write("\n" + text + "\n")


def note_started(mesh):
    version = getattr(mesh, "version", None) or {}
    append_note_line(
        mesh,
        "The climb started. "
        f"mod_commit {version.get('mod_commit') or 'unnamed'}, "
        f"mod_sha256 {version.get('mod_sha256') or 'unread'}, "
        f"version_certainty {version.get('version_certainty') or 'unread'}.",
    )


def note_stopped(mesh, reason):
    append_note_line(
        mesh,
        f"Stopped this climb: {reason}. Not a bend. "
        "Bots already connected were left in place. Nothing was cleared.",
    )


def refresh_note(mesh, body):
    previous = mesh.note_path.read_text() if mesh.note_path.exists() else ""
    marker = "\n## Climb\n"
    if "## Climb" in previous:
        head, _, _tail = previous.partition(marker)
        mesh.note_path.write_text(head.rstrip() + "\n" + body)
    else:
        mesh.note_path.write_text(previous.rstrip() + "\n" + body)


def ensure_csv(mesh):
    if mesh.csv_path.exists():
        return
    with mesh.csv_path.open("w", newline="") as fh:
        csv.DictWriter(fh, fieldnames=columns_for(mesh)).writeheader()


def init_local(mesh):
    ensure_csv(mesh)
    refresh_note(mesh, climb_preamble(mesh))


def drop_known_hosts_warning(text):
    """Drop a leading OpenSSH known-hosts notice so callers can parse the rest."""
    lines = (text or "").splitlines()
    while lines and KNOWN_HOSTS_WARNING.match(lines[0].strip()):
        lines.pop(0)
    if not lines:
        return ""
    body = "\n".join(lines)
    if text.endswith("\n"):
        body += "\n"
    return body


def ssh(mesh, ip, remote, data=None, timeout=90):
    if KEY is None:
        raise RuntimeError("CK_SSH_KEY is required")
    KNOWN.parent.mkdir(parents=True, exist_ok=True)
    proc = subprocess.run(
        [
            "ssh",
            "-i",
            str(KEY),
            "-o",
            "IdentitiesOnly=yes",
            "-o",
            "BatchMode=yes",
            "-o",
            f"UserKnownHostsFile={KNOWN}",
            "-o",
            "StrictHostKeyChecking=accept-new",
            "-o",
            "ConnectTimeout=15",
            "-o",
            "LogLevel=ERROR",
            f"ubuntu@{ip}",
            remote,
        ],
        input=data,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout,
    )
    text = drop_known_hosts_warning(redact(proc.stdout.decode(errors="replace")))
    if proc.returncode != 0:
        raise RuntimeError(f"ssh exit {proc.returncode}: {text[-400:]}")
    return text


def token(mesh):
    path = mesh.token_path
    if path is None:
        names = {
            "four": "CK_FOUR_TOKEN_FILE",
            "sixteen": "CK_SIXTEEN_TOKEN_FILE",
            "nine": "CK_NINE_TOKEN_FILE",
        }
        path = env_path(names[mesh.name])
    return Path(path).read_text().strip()


def api(mesh, ip, method, path, body=None, timeout=40):
    payload = "" if body is None else json.dumps(body)
    remote = """
import os, sys, urllib.request, urllib.error
tok = os.environ["CK_TOKEN"]
method = %r
path = %r
payload = %r
data = payload.encode() if payload else None
req = urllib.request.Request(
    "http://127.0.0.1:9109" + path,
    data=data,
    method=method,
    headers={"Authorization": "Bearer " + tok, "Content-Type": "application/json"},
)
try:
    with urllib.request.urlopen(req, timeout=25) as resp:
        sys.stdout.write(resp.read().decode())
except urllib.error.HTTPError as exc:
    sys.stdout.write(exc.read().decode())
    raise SystemExit(22)
""" % (method, path, payload)
    stdin = token(mesh) + "\n" + remote
    try:
        text = ssh(mesh, ip, "read -r CK_TOKEN && export CK_TOKEN && python3 -", stdin.encode(), timeout=timeout)
    except RuntimeError as exc:
        err = str(exc)
        if "exit 22" in err:
            raw = err.split("exit 22:", 1)[-1].strip()
            try:
                return json.loads(raw)
            except json.JSONDecodeError:
                return {"error": raw[-300:]}
        raise
    return json.loads(text)


def fan(pairs, fn, workers):
    out = {}
    with ThreadPoolExecutor(max_workers=workers) as pool:
        futs = {pool.submit(fn, item): item for item in pairs}
        for fut in as_completed(futs):
            item = futs[fut]
            name = item[0] if isinstance(item, tuple) else item
            try:
                out[name] = fut.result()
            except Exception as exc:
                out[name] = {"error": redact(str(exc))[-300:]}
    return out


def java_count(mesh, ip):
    text = ssh(mesh, ip, "ps -C java -o pid= | wc -l", timeout=20)
    for line in reversed(text.splitlines()):
        line = line.strip()
        if line.isdigit():
            return int(line)
    raise RuntimeError("java count unreadable")


def harness_text(mesh, ip):
    remote = r"""python3 - << 'PY'
import pathlib
cands = [
    pathlib.Path("/opt/ck-mc-harness/mc-harness.mjs"),
    pathlib.Path("/opt/ck-mc-harness/minecraft/mc-harness.mjs"),
    pathlib.Path("/opt/ck-mc-harness/harness-src/minecraft/mc-harness.mjs"),
]
for path in cands:
    if path.exists():
        print(path)
        raise SystemExit
print("MISSING")
PY"""
    return ssh(mesh, ip, remote, timeout=20).strip().splitlines()[-1]


def scp_to(ip, local, dest, recursive=False):
    cmd = [
        "scp",
        "-i",
        str(KEY),
        "-o",
        "IdentitiesOnly=yes",
        "-o",
        "BatchMode=yes",
        "-o",
        "LogLevel=ERROR",
        "-o",
        f"UserKnownHostsFile={KNOWN}",
        "-o",
        "StrictHostKeyChecking=accept-new",
    ]
    if recursive:
        cmd.append("-r")
    cmd.extend([str(local), dest])
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
    if proc.returncode != 0:
        raise RuntimeError("scp failed: " + redact(proc.stdout.decode(errors="replace")[-300:]))


def copy_place_files(mesh, ip, harness_path):
    if harness_path == "MISSING":
        raise RuntimeError("mc-harness.mjs was not found")
    remote_dir = str(Path(harness_path).parent)
    harness_src = HERE.parent / "mc-harness.mjs"
    pose_src = HERE.parent / "seam-pose.mjs"
    scp_to(ip, harness_src, f"ubuntu@{ip}:{remote_dir}/mc-harness.mjs")
    if pose_src.exists():
        scp_to(ip, pose_src, f"ubuntu@{ip}:{remote_dir}/seam-pose.mjs")
    for local in (SPAWN, PLACE):
        scp_to(ip, local, f"ubuntu@{ip}:{remote_dir}/{local.name}")
    if LOCAL_COLUMNS.exists():
        scp_to(ip, LOCAL_COLUMNS, f"ubuntu@{ip}:/opt/ck-mc-harness/section-columns.json")


PATCH = r"""
import pathlib, sys
path = pathlib.Path(sys.argv[1])
text = path.read_text()
# A harness that already calls sectionPlace is done, even when the import
# text is not the exact from "./section-place.mjs" line. The old
# seamTargetForIndex block is gone on those files, so it is not required.
if "sectionPlace(" in text:
    print("patch-present")
    raise SystemExit

def fail(label):
    print("patch-mismatch " + label)
    raise SystemExit(2)

needle = '} from "./seam-pose.mjs";\n'
old_end = 'function endpointForIndex(globalIndex) {\n'
quad_target = '''    const target = seamTargetForIndex(globalIndex, {
      side: endpoint.side,
      triangle: TRIANGLE,
      quad: QUAD,
    });'''
plain_target = '  const target = seamTargetForIndex(globalIndex);\n'
plain_write = '    await writeSeamPoseWithRetries(name, globalIndex, { server: endpoint.ckServer });'
tri = '''function triangleWalkerEnv(globalIndex) {
  if (!TRIANGLE && !QUAD) return process.env;'''
pose_old = '  const target = seamTargetForIndex(index);\n'
old_wander = '''function wanderSpawnFor(globalIndex) {
  return WANDER_PLACE === "check" ? wanderCheckSpawn(globalIndex) : wanderDensitySpawn(globalIndex);
}'''

if needle not in text:
    fail("import")
if old_end not in text:
    fail("endpoint")
has_quad = quad_target in text
has_plain = plain_target in text
has_wander = old_wander in text
if has_quad and has_plain:
    fail("target")
if not has_quad and not has_plain and not has_wander:
    fail("target")
if has_plain and plain_write not in text:
    fail("pose-write")
if not has_wander and "function triangleWalkerEnv" in text and tri not in text:
    fail("walker-env")
has_tri = tri in text

pose_path = path.parent / "seam-pose.mjs"
pose_mode = "skip"
pose_text = ""
if has_plain:
    if not pose_path.exists():
        fail("seam-pose")
    pose_text = pose_path.read_text()
    if "opts.target" in pose_text:
        pose_mode = "ready"
    elif pose_text.count(pose_old) == 1:
        pose_mode = "update"
    else:
        fail("seam-pose-target")

new_end = old_end + '''  if (env("LT_MC_WANDER_PLACE", "") === "section") {
    const spawn = sectionPlace(globalIndex);
    return {
      side: spawn.server,
      host: sectionHost(spawn.server),
      port: Number(MC_PORT),
      ckServer: spawn.server,
    };
  }
'''
quad_new = '''    const placed = env("LT_MC_WANDER_PLACE", "") === "section" ? sectionPlace(globalIndex) : null;
    const target = placed
      ? { x: placed.x, y: placed.y, z: placed.z }
      : seamTargetForIndex(globalIndex, {
          side: endpoint.side,
          triangle: TRIANGLE,
          quad: QUAD,
        });'''
plain_new = '''  const placed = env("LT_MC_WANDER_PLACE", "") === "section" ? sectionPlace(globalIndex) : null;
  const target = placed
    ? { x: placed.x, y: placed.y, z: placed.z }
    : seamTargetForIndex(globalIndex);
'''
plain_write_new = '    await writeSeamPoseWithRetries(name, globalIndex, { server: endpoint.ckServer, target });'
tri_new = '''function triangleWalkerEnv(globalIndex) {
  if (env("LT_MC_WANDER_PLACE", "") === "section") return process.env;
  if (!TRIANGLE && !QUAD) return process.env;'''
wander_new = '''function wanderSpawnFor(globalIndex) {
  if (WANDER_PLACE === "section") return sectionPlace(globalIndex);
  return WANDER_PLACE === "check" ? wanderCheckSpawn(globalIndex) : wanderDensitySpawn(globalIndex);
}'''

if pose_mode == "update":
    pose_path.write_text(pose_text.replace(pose_old, '  const target = opts.target || seamTargetForIndex(index);\n', 1))
text = text.replace(needle, needle + 'import { sectionPlace, sectionHost } from "./section-place.mjs";\n', 1)
text = text.replace(old_end, new_end, 1)
if has_quad:
    text = text.replace(quad_target, quad_new, 1)
if has_plain:
    text = text.replace(plain_target, plain_new, 1)
    text = text.replace(plain_write, plain_write_new, 1)
if has_tri:
    text = text.replace(tri, tri_new, 1)
if has_wander:
    text = text.replace(old_wander, wander_new, 1)
path.write_text(text)
print("patch-applied")
"""


def patch_harness(mesh, ip, harness_path):
    text = ssh(mesh, ip, "sudo -n python3 - " + harness_path, PATCH.encode(), timeout=30).strip()
    if "patch-present" in text:
        return "patch-present"
    if "patch-mismatch" in text or "patch-applied" not in text:
        raise RuntimeError("harness patch did not match; left the file unchanged")
    return "patch-applied"


def real_pending_script():
    """A usable newer LoadWalker has its own run.sh. An empty or class-only tree does not."""
    run_sh = PENDING_WANDER / "run.sh"
    try:
        return run_sh.is_file() and run_sh.stat().st_size > 0
    except OSError:
        return False


def stage_wander(mesh, ip):
    """Copy a newer LoadWalker tree into the dist. Do not signal a running JVM.

    No real run.sh means there is nothing to copy. Leave the installed dist alone.
    """
    if not real_pending_script():
        return "no-pending-script"
    remote = r"""python3 - << 'PY'
import pathlib
text = pathlib.Path("/opt/ck-mc-harness/lt.env").read_text()
dist = ""
for line in text.splitlines():
    if line.startswith("LT_MC_DIST="):
        dist = line.split("=", 1)[1]
if not dist:
    raise SystemExit("no dist")
print(dist)
PY"""
    dist = ssh(mesh, ip, remote, timeout=20).strip().splitlines()[-1]
    if not dist.startswith("/") or any(ch in dist for ch in " \t\n'\"$"):
        raise RuntimeError("wander dist path was not a directory")
    packed = subprocess.run(
        ["tar", "-C", str(PENDING_WANDER), "-czf", "-", "."],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
    )
    ssh(mesh, ip, "tar -xzf - -C " + dist, packed.stdout, timeout=60)
    return "script-staged-for-next-pack"


def write_env(mesh, ip, base, limit, hosts_for_sections):
    updates = {
        "LT_INDEX_BASE": str(base),
        "LT_INDEX_LIMIT": str(limit),
        "LT_CLIENTS": "0",
        "LT_MC_GRID": "",
        "LT_MC_MOTION": "wander",
        "LT_MC_PATH": "wander",
        "LT_MC_WANDER_PLACE": "section",
        "LT_MC_WANDER_BPS": WANDER_BPS,
        "LT_MC_MAX_RUNTIME_S": RUNTIME_S,
        "LT_LOGIN_STAGGER_MS": str(STAGGER_MS),
        "LT_PACK_SIZE": str(PACK_SIZE),
        "LT_PACK_RSS_BASE_KB": str(PACK_BASE_KB),
        "LT_PACK_RSS_EACH_KB": str(PACK_EACH_KB),
        "LT_MC_PACK_XMX": PACK_HEAP,
        "LT_ABORT_ON_SEAM_FAIL": "0",
        "LT_SECTION_MESH": mesh.name,
        "LT_SECTION_COLUMNS": "/opt/ck-mc-harness/section-columns.json",
    }
    for side, host in hosts_for_sections.items():
        updates["LT_MC_HOST_" + side.upper()] = host
    payload = json.dumps({
        "updates": updates,
        "expect": app_id(mesh.app_path),
        "reject": [app_id(path) for path in mesh.reject_app_paths if app_id(path)],
    })
    program = "import json\nfrom pathlib import Path\nspec = json.loads(%r)\n" % payload
    program += """path = Path("/opt/ck-mc-harness/lt.env")
text = path.read_text()
updates = spec["updates"]
expect = spec["expect"]
reject = set(spec["reject"])
app = ""
kept = []
for line in text.splitlines():
    if not line or line.lstrip().startswith("#") or "=" not in line:
        kept.append(line)
        continue
    key, _, value = line.partition("=")
    if key == "CROWDY_APP_ID":
        app = value
    if key in updates:
        continue
    kept.append(line)
if expect and app != expect:
    raise SystemExit("app mismatch")
if app in reject:
    raise SystemExit("refusing this mesh app")
for key, value in updates.items():
    kept.append("%s=%s" % (key, value))
path.write_text("\\n".join(kept).rstrip() + "\\n")
path.chmod(0o600)
print("env-updated")
"""
    text = ssh(mesh, ip, "python3 -", program.encode(), timeout=30)
    if "env-updated" not in text:
        raise RuntimeError(redact(text)[-200:])


def harness_mode(mesh, ip):
    """Placement and pack settings of the running harness. Hosts are not printed."""
    remote = r"""python3 - << 'PY'
import json, pathlib
found = None
for pid in pathlib.Path("/proc").iterdir():
    if not pid.name.isdigit():
        continue
    try:
        cmd = (pid / "cmdline").read_bytes().replace(b"\0", b" ").decode("utf-8", "replace")
        env = (pid / "environ").read_bytes().split(b"\0")
    except Exception:
        continue
    if "mc-harness" not in cmd:
        continue
    vals = {}
    for item in env:
        if b"=" not in item:
            continue
        key, _, val = item.partition(b"=")
        name = key.decode("utf-8", "replace")
        if name in ("LT_MC_WANDER_PLACE", "LT_SECTION_MESH", "LT_PACK_SIZE", "LT_MC_WANDER_BPS", "LT_MC_PACK_XMX", "LT_PACK_RSS_BASE_KB", "LT_PACK_RSS_EACH_KB"):
            vals[name] = val.decode("utf-8", "replace")
    found = vals
    break
if found is None:
    found = {}
file_vals = {}
try:
    for line in pathlib.Path("/opt/ck-mc-harness/lt.env").read_text().splitlines():
        if "=" not in line or line.lstrip().startswith("#"):
            continue
        key, _, val = line.partition("=")
        if key in ("LT_MC_WANDER_PLACE", "LT_SECTION_MESH", "LT_PACK_SIZE", "LT_MC_WANDER_BPS", "LT_MC_PACK_XMX", "LT_PACK_RSS_BASE_KB", "LT_PACK_RSS_EACH_KB"):
            file_vals[key] = val
except OSError:
    file_vals = {}
for key, val in file_vals.items():
    found.setdefault(key, val)
print(json.dumps(found))
PY"""
    text = ssh(mesh, ip, remote, timeout=20).strip().splitlines()[-1]
    return json.loads(text)


def prepare_idle(mesh, loads, games_by_name):
    """Patch and reload only when no walker JVM is running."""
    java = {}
    for name, ip in loads:
        java[name] = java_count(mesh, ip)
    busy = [name for name, count in java.items() if count]
    if busy:
        for name, ip in loads:
            mode = harness_mode(mesh, ip)
            if (
                not mode
                or mode.get("LT_MC_WANDER_PLACE") != "section"
                or mode.get("LT_SECTION_MESH") != mesh.name
            ):
                raise SystemExit(
                    name + " is not on this mesh's section placement; a walker JVM is running, so it was not restarted and nothing was added"
                )
            harness = harness_text(mesh, ip)
            copy_place_files(mesh, ip, harness)
            staged = stage_wander(mesh, ip)
            log(mesh, f"{name} keeps its running JVMs; {staged}")
        log(mesh, "walker JVMs are up on " + " ".join(busy) + "; not restarting them")
        return
    section_hosts = {side: games_by_name[name] for name, side in mesh.games}
    for index, (name, ip) in enumerate(loads):
        harness = harness_text(mesh, ip)
        copy_place_files(mesh, ip, harness)
        patched = patch_harness(mesh, ip, harness)
        write_env(mesh, ip, index * PIN_SPAN, PIN_SPAN, section_hosts)
        staged = stage_wander(mesh, ip)
        log(mesh, f"{name} {patched}; {staged}; pinned {index * PIN_SPAN}+{PIN_SPAN}")
        ssh(mesh, ip, "sudo systemctl restart ck-mc-harness.service", timeout=40)
    deadline = time.time() + 90
    pending = {name for name, _ip in loads}
    while time.time() < deadline and pending:
        still = set()
        for name, ip in loads:
            try:
                text = ssh(mesh, ip, "systemctl is-active ck-mc-harness; curl -sf -m 5 http://127.0.0.1:9109/health || true", timeout=25)
            except Exception:
                still.add(name)
                continue
            if "active" in text.splitlines()[0] and '"ok":true' in text.replace(" ", ""):
                continue
            still.add(name)
        pending = still
        if pending:
            time.sleep(2)
    if pending:
        raise SystemExit("harness did not return: " + " ".join(sorted(pending)))
    log(mesh, "harness reloaded while no walker JVM was running")


def require_jar(mesh, games):
    """Hash the installed jar with sudo. Do not copy it and do not restart Minecraft.

    A permission error is not a hash mismatch. A matching sudo hash leaves the
    game process untouched.
    """
    allowed = {"west", "east", "north", "ne", *(f"p{i}" for i in range(1, 17))}
    denied = []
    digests = []
    for name, side in mesh.games:
        if side not in allowed:
            raise SystemExit("refusing jar check for " + name)
        if mesh.name != "nine" and name.startswith("test-mc-p5"):
            raise SystemExit("refusing jar check for " + name)
        ip = games[name]
        remote = "sudo -n sha256sum /opt/mc/%s/mods/crowdedkingdoms-server-0.3.0.jar" % side
        text = ""
        exc = None
        for _attempt in range(2):
            try:
                text = ssh(mesh, ip, remote, timeout=40)
                exc = None
                break
            except Exception as err:
                exc = err
                time.sleep(2)
        if exc is not None:
            err = redact(str(exc)).lower()
            if (
                "permission denied" in err
                or "a password is required" in err
                or "not permitted" in err
                or "not allowed" in err
            ):
                log(mesh, f"jar {name} permission denied; not a hash mismatch")
            else:
                log(mesh, f"jar {name} hash unreadable; not a hash mismatch")
            denied.append(name)
            continue
        digest = ""
        for line in text.splitlines():
            part = line.split()
            if part and len(part[0]) == 64 and all(c in "0123456789abcdef" for c in part[0]):
                digest = part[0]
                break
        if digest:
            digests.append(digest)
            continue
        log(mesh, f"jar {name} no digest; not a hash mismatch")
        denied.append(name)
        digests.append(None)
    known = {JAR_SHA: COMMIT}
    extra_sha = os.environ.get("CK_MOD_SHA", "").strip()
    extra_commit = os.environ.get("CK_MOD_COMMIT", "").strip()
    if extra_sha and extra_commit:
        known[extra_sha] = extra_commit
    mesh.version = bend.classify_version([item if item else None for item in digests], known)
    if denied:
        mesh.version = {"mod_commit": "", "mod_sha256": "", "version_certainty": "unread"}
        raise SystemExit(
            "could not read the jar on "
            + " ".join(denied)
            + "; not a hash mismatch; not adding bots and not restarting"
        )
    log(
        mesh,
        "jar "
        + mesh.version["version_certainty"]
        + " commit "
        + (mesh.version["mod_commit"] or "unnamed"),
    )


def park_overloaded(mesh, loads):
    """A generator at load >= 32, or a harness paused so an add cannot run away, is full.

    Its JVMs stay up and are not the climb count. A paused harness is not queried, so a
    stopped control process cannot stall the climb.
    """
    mesh.parked = set()
    for name, ip in loads:
        reason = ""
        held = 0
        try:
            text = ssh(
                mesh,
                ip,
                "pid=$(systemctl show -p MainPID --value ck-mc-harness); state=$(awk '{print $3}' /proc/$pid/stat); load=$(cut -d' ' -f1 /proc/loadavg); echo $state $load",
                timeout=20,
            )
            state, load_s = [ln for ln in text.splitlines() if ln.strip()][-1].split()[:2]
            load1 = float(load_s)
            held = java_count(mesh, ip)
        except Exception:
            continue
        if state == "T" and held > 0:
            reason = "add paused"
        elif load1 >= 32 and held > 0:
            reason = f"load {load1:.0f}"
        if not reason:
            continue
        mesh.parked.add(name)
        log(mesh, f"{name} {reason} with {held} walker JVMs still up; loadgen miss, not a mesh cap")
        row = blank(mesh, held, held)
        row["binding"] = "loadgen"
        row["result"] = "fail"
        row["note"] = f"{name} loadgen miss; walkers left running; not a mesh cap"
        append_csv(mesh, row)


def answered_count(st):
    """Seated bots on one generator. None when that generator did not answer."""
    if not isinstance(st, dict) or st.get("error") or st.get("omitted"):
        return None
    if "active" not in st:
        return None
    return int(st.get("active") or 0)


def sum_answered(rows):
    total = 0
    answered = 0
    for st in rows.values():
        count = answered_count(st)
        if count is None:
            continue
        total += count
        answered += 1
    if answered == 0:
        return None
    return total


def poll_status(mesh, loads):
    rows = {}
    parked = getattr(mesh, "parked", ())
    for name, ip in loads:
        if name in parked:
            rows[name] = {"active": 0, "busy": False, "transfers": 0, "live_transfers": 0, "index_base": 0, "index_limit": PIN_SPAN}
            continue
        try:
            rows[name] = api(mesh, ip, "GET", "/v1/status?run_id=" + mesh.name)
        except Exception as exc:
            rows[name] = {"error": redact(str(exc))[-200:], "omitted": True, "busy": False}
    return rows


def memory_text(err):
    low = (err or "").lower()
    return any(word in low for word in ("memavailable", "not enough", "reserve", "memory", "index_limit", "unknown-column"))


def add_only(mesh, loads, counts):
    """POST the shortfall. Never clear, never kill."""
    status = poll_status(mesh, loads)
    errors = {}
    for index, (name, ip) in enumerate(loads):
        want = counts.get(name, 0)
        st = status.get(name) or {}
        if st.get("error"):
            errors[name] = "status"
            continue
        base = index * PIN_SPAN
        have = answered_count(st)
        if have is None:
            errors[name] = "status"
            continue
        live = int(st.get("index_base") if st.get("index_base") not in (None, "") else -1)
        limit = int(st.get("index_limit") or 0)
        ceiling = getattr(mesh, "attached_index_limit", None)
        if ceiling is not None:
            origin = live if live >= 0 else base
            want = min(want, max(0, ceiling - origin))
        inside = ceiling is not None and 0 <= live < ceiling and 0 < limit and live + limit <= ceiling
        # A window that already starts at or above the ceiling stays there.
        # Moving it would pin another generator into this climb.
        if ceiling is not None and live >= ceiling:
            pass
        elif not inside and (live != base or limit < PIN_SPAN):
            try:
                pinned = api(mesh, ip, "POST", "/v1/index", {"base": base, "limit": PIN_SPAN})
            except Exception as exc:
                errors[name] = redact(str(exc))[-160:]
                continue
            if pinned.get("error"):
                errors[name] = str(pinned.get("error"))[:200]
                continue
        need = want - have
        used = int(st.get("index_used") or 0)
        # A count past the open slots is refused before busy is set, so the pack never starts.
        room = max(0, limit - used) if limit > 0 else need
        if need > room:
            log(mesh, f"add {name} next pack {need} clamped to {room} open slots")
            need = room
        log(mesh, f"add {name} have={have} need={need}")
        if need <= 0:
            continue
        # Load2505 already missed on index 2504. Do not pose-rewrite it or aim another bot there.
        nxt = (live if live >= 0 else base) + used
        if nxt == 2504:
            log(mesh, "spawn-miss Load2505 index 2504 left; not retrying")
            continue
        try:
            resp = api(mesh, ip, "POST", "/v1/clients/add", {"count": need, "run_id": mesh.name})
        except Exception as exc:
            errors[name] = redact(str(exc))[-160:]
            continue
        if resp.get("error"):
            errors[name] = str(resp.get("error"))[:240]
    return errors


def wait_settled(mesh, loads, want):
    deadline = time.time() + min(1500, 150 + want * 4)
    last = {}
    seen_busy = False
    idle_since = None
    while time.time() < deadline:
        last = poll_status(mesh, loads)
        if any(isinstance(st, dict) and st.get("error") for st in last.values()):
            time.sleep(5)
            continue
        active = sum_answered(last)
        if active is None:
            time.sleep(5)
            continue
        busy = any(bool(st.get("busy")) for st in last.values() if answered_count(st) is not None)
        log(mesh, f"spawn active={active}/{want} busy={busy}")
        if busy:
            seen_busy = True
            idle_since = None
            time.sleep(10)
            continue
        if active >= want and (seen_busy or want == 0):
            return last, active, True
        if seen_busy or any(isinstance(st, dict) and st.get("add_error") for st in last.values()):
            if idle_since is None:
                idle_since = time.time()
            elif time.time() - idle_since >= 20:
                return last, active, active >= want
        time.sleep(10)
    active = sum_answered(last)
    return last, active, False


def sample_game(mesh, name, side, ip):
    remote = r"""python3 - << 'PY'
import json, pathlib, socket, subprocess
load = pathlib.Path("/proc/loadavg").read_text().split()
mem = {}
for line in pathlib.Path("/proc/meminfo").read_text().splitlines():
    if line.startswith(("MemTotal:", "MemAvailable:")):
        key, val = line.split(":", 1)
        mem[key] = int(val.strip().split()[0])
stat_lines = pathlib.Path("/proc/stat").read_text().splitlines()
cpu = [int(x) for x in stat_lines[0].split()[1:8]]
cores = []
for stat_line in stat_lines[1:]:
    if not stat_line.startswith("cpu") or not stat_line[3:4].isdigit():
        break
    cores.append([int(x) for x in stat_line.split()[1:8]])
port = False
try:
    sock = socket.socket()
    sock.settimeout(2)
    port = sock.connect_ex(("127.0.0.1", 25565)) == 0
    sock.close()
except Exception:
    port = False
unit = "unknown"
try:
    unit = subprocess.check_output(["systemctl", "is-active", "mc-%s"], text=True).strip()
except subprocess.CalledProcessError as exc:
    unit = (exc.output or "inactive").strip()
print(json.dumps({"load1": float(load[0]), "mem_avail_kb": mem.get("MemAvailable"), "cpu": cpu, "cores": cores, "port_25565": port, "unit": unit}))
PY""" % side
    row = json.loads(ssh(mesh, ip, remote, timeout=30))
    row["side"] = side
    return row


def core_max_pct(start, end):
    """Highest per-core busy percent between two /proc/stat samples."""
    a = (start or {}).get("cores") or []
    b = (end or {}).get("cores") or []
    best = None
    for i in range(min(len(a), len(b))):
        pct = cpu_pct({"cpu": a[i]}, {"cpu": b[i]})
        if pct is None:
            continue
        best = pct if best is None else max(best, pct)
    return best


def cpu_pct(start, end):
    a = (start or {}).get("cpu") or []
    b = (end or {}).get("cpu") or []
    if len(a) < 7 or len(b) < 7:
        return None
    deltas = [b[i] - a[i] for i in range(7)]
    total = sum(deltas)
    if total <= 0:
        return None
    busy = deltas[0] + deltas[1] + deltas[2] + deltas[5] + deltas[6]
    return round(100.0 * busy / total, 1)


def log_mark(mesh, side, ip):
    remote = f"""python3 - << 'PY'
import json, pathlib
path = pathlib.Path("/opt/mc/{side}/logs/latest.log")
if not path.exists():
    print(json.dumps({{"missing": True}}))
else:
    st = path.stat()
    print(json.dumps({{"ino": st.st_ino, "size": st.st_size}}))
PY"""
    return json.loads(ssh(mesh, ip, "sudo " + remote, timeout=30))


def log_delta(mesh, side, ip, mark):
    if not isinstance(mark, dict) or "size" not in mark:
        return {"unmeasured": True}
    remote = f"""python3 - << 'PY'
import json, pathlib, re
path = pathlib.Path("/opt/mc/{side}/logs/latest.log")
mark_ino = {int(mark.get("ino") or 0)}
mark_size = {int(mark.get("size") or 0)}
out = {{"to": {{}}, "from_lines": 0, "moved_wrongly": 0, "oom": False}}
if not path.exists():
    out["missing"] = True
    print(json.dumps(out))
    raise SystemExit
data = path.read_bytes()
st = path.stat()
if mark_ino and st.st_ino != mark_ino:
    chunk = data
elif mark_size <= len(data):
    chunk = data[mark_size:]
else:
    chunk = data
text = chunk.decode("utf-8", "replace")
pat = re.compile(r"crossed into ([A-Za-z0-9_-]+)'s chunks and was handed on")
for match in pat.finditer(text):
    dest = match.group(1)
    out["to"][dest] = out["to"].get(dest, 0) + 1
    out["from_lines"] += 1
out["moved_wrongly"] = text.count("moved wrongly")
out["oom"] = "OutOfMemoryError" in text or "Java heap space" in text or "oom-kill" in text
out["tick"] = "Watchdog" in text or "60.00s" in text
print(json.dumps(out))
PY"""
    return json.loads(ssh(mesh, ip, "sudo " + remote, timeout=60))


def hold(mesh, loads, games, rung_id, want):
    status = poll_status(mesh, loads)
    active = sum_answered(status)
    busy = any(bool(st.get("busy")) for st in status.values() if answered_count(st) is not None)
    if active is None or active < want or busy:
        log(mesh, f"hold clock not started active={active}/{want} busy={busy}")
        spot = fan(mesh.games, lambda pair: sample_game(mesh, pair[0], pair[1], games[pair[0]]), 8)
        return {"start": spot, "end": spot, "status": status, "logs": {}, "min_active": active, "hold_sec": 0, "clock_started": False, "stopped_early": False}
    marks = fan(mesh.games, lambda pair: (pair[0], log_mark(mesh, pair[1], games[pair[0]])), 8)
    for name, ip in loads:
        try:
            api(mesh, ip, "POST", "/v1/rung/open", {"id": rung_id})
        except Exception as exc:
            log(mesh, f"rung open {name} {redact(str(exc))[-120:]}")
    start = fan(mesh.games, lambda pair: sample_game(mesh, pair[0], pair[1], games[pair[0]]), 8)
    t0 = time.time()
    min_active = None
    end_status = status
    stopped = False
    prev = start
    hot_samples = []
    while True:
        elapsed = time.time() - t0
        end_status = poll_status(mesh, loads)
        current = sum_answered(end_status)
        if current is None:
            log(mesh, f"hold t={int(elapsed)}s no generator answered; not a collapse")
            time.sleep(min(20, max(1, HOLD_SEC - elapsed)))
            continue
        transfers = sum(int(st.get("transfers") or 0) for st in end_status.values() if answered_count(st) is not None)
        min_active = current if min_active is None else min(min_active, current)
        log(mesh, f"hold t={int(elapsed)}s active={current}/{want} transfers={transfers}")
        if current == 0 and elapsed > 15:
            stopped = True
            break
        if elapsed >= HOLD_SEC:
            break
        time.sleep(min(20, HOLD_SEC - elapsed))
        nxt = fan(mesh.games, lambda pair: sample_game(mesh, pair[0], pair[1], games[pair[0]]), 8)
        hot_samples.append(sample_stayed_hot(prev, nxt))
        prev = nxt
    end = fan(mesh.games, lambda pair: sample_game(mesh, pair[0], pair[1], games[pair[0]]), 8)
    hot_samples.append(sample_stayed_hot(prev, end))
    logs = {}
    for name, side in mesh.games:
        mark = marks.get(name)
        mark = mark[1] if isinstance(mark, tuple) else mark
        try:
            logs[side] = log_delta(mesh, side, games[name], mark if isinstance(mark, dict) else {})
        except Exception as exc:
            logs[side] = {"error": redact(str(exc))[-160:]}
    for name, ip in loads:
        try:
            api(mesh, ip, "POST", "/v1/rung/close", {})
        except Exception:
            pass
    return {
        "start": start,
        "end": end,
        "status": end_status,
        "logs": logs,
        "min_active": min_active,
        "hold_sec": round(time.time() - t0, 1),
        "clock_started": True,
        "stopped_early": stopped,
        "hot_whole": bool(hot_samples) and all(hot_samples),
    }


def sample_stayed_hot(start, end):
    """Both load samples in the interval have to clear the line. One does not."""
    if not isinstance(start, dict) or not isinstance(end, dict):
        return False

    def busiest_load(sample):
        loads = []
        for item in sample.values():
            if isinstance(item, dict) and item.get("load1") is not None:
                loads.append(float(item["load1"]))
        return max(loads) if loads else None

    cores = []
    for name in set(start) | set(end):
        core = core_max_pct(start.get(name) or {}, end.get(name) or {})
        if core is not None:
            cores.append(core)
    core = max(cores) if cores else None
    return bend.interval_stayed_hot(busiest_load(start), busiest_load(end), core)


def busiest(mesh, result):
    best = None
    down = []
    for name, side in mesh.games:
        end = (result.get("end") or {}).get(name) or {}
        start = (result.get("start") or {}).get(name) or {}
        cpu = None
        load1 = None
        avail = None
        if isinstance(end, dict) and not end.get("error"):
            load1 = end.get("load1")
            if end.get("mem_avail_kb") is not None:
                avail = round(end["mem_avail_kb"] / 1024.0, 1)
            if isinstance(start, dict) and not start.get("error"):
                cpu = cpu_pct(start, end)
            if end.get("unit") not in ("active", "") or end.get("port_25565") is False:
                down.append(side)
        rank = (cpu if cpu is not None else -1, float(load1 or 0))
        if best is None or rank > best[0]:
            best = (rank, side, {"cpu": cpu, "load1": load1, "avail": avail})
    if best is None:
        return "", {"cpu": None, "load1": None, "avail": None}, down
    return best[1], best[2], down


def sample_mix(mesh, loads, counts):
    """How many sampled logs show path=wander and are still moving. Does not drop anyone."""
    moving = 0
    wander = 0
    seen = 0
    for index, (name, ip) in enumerate(loads):
        count = counts.get(name, 0)
        if count <= 0:
            continue
        base = index * PIN_SPAN
        step = max(1, count // 4)
        picks = [base + i * step for i in range(min(4, count))]
        remote = f"""python3 - << 'PY'
import json, math, pathlib, re
root = pathlib.Path("/var/lib/ck-mc-lt/stats/walkers")
prefix = {getattr(mesh, "bot_prefix", mesh.prefix)!r}
picks = {picks}
out = []
for i in picks:
    path = root / f"{{prefix}}{{i+1}}.log"
    if not path.exists():
        continue
    lines = path.read_text(errors="replace").splitlines()
    starts = [n for n, ln in enumerate(lines) if " path=" in ln and "phase=" not in ln]
    window = lines[starts[-1]:] if starts else lines
    play = [ln for ln in window if "phase=play" in ln and "path=wander" in ln][-150:]
    wandered = any("path=wander" in ln for ln in window)
    other = any("path=" in ln and "path=wander" not in ln for ln in window)
    pos = []
    heads = []
    for ln in play:
        match = re.search(r"x=([-+0-9.]+) y=[-+0-9.]+ z=([-+0-9.]+)", ln)
        heading = re.search(r"heading=([-+0-9.]+)", ln)
        if match:
            pos.append((float(match.group(1)), float(match.group(2))))
        if heading:
            heads.append(round(float(heading.group(1)), 1))
    radius = 0
    if pos:
        x0, z0 = pos[0]
        radius = max(math.hypot(x - x0, z - z0) for x, z in pos)
    out.append({{"wander": wandered and not other, "moving": len(play) >= 2 and radius > 4 and len(set(heads)) >= 2}})
print(json.dumps(out))
PY"""
        try:
            rows = json.loads(ssh(mesh, ip, remote, timeout=40))
        except Exception as exc:
            log(mesh, f"sample {name} {redact(str(exc))[-160:]}")
            continue
        for row in rows:
            seen += 1
            if row.get("wander"):
                wander += 1
            if row.get("moving"):
                moving += 1
    return f"{moving}/{seen}", f"{wander}/{seen}"


def fresh_jump(mesh, games):
    """Two samples of the game servers. Returns jump, busiest side, cpu, load."""
    start = {name: sample_game(mesh, name, side, games[name]) for name, side in mesh.games}
    time.sleep(5)
    end = {name: sample_game(mesh, name, side, games[name]) for name, side in mesh.games}
    best = None
    for name, side in mesh.games:
        cpu = cpu_pct(start[name], end[name])
        load1 = (end[name] or {}).get("load1")
        rank = (cpu if cpu is not None else -1, float(load1 or 0))
        if best is None or rank > best[0]:
            best = (rank, side, cpu, load1)
    side, cpu, load1 = best[1], best[2], best[3]
    return jump_after(cpu, load1), side, cpu, load1


def jump_after(cpu, load1):
    cpu_v = float(cpu) if cpu not in (None, "") else 0.0
    load_v = float(load1) if load1 not in (None, "") else 0.0
    if cpu_v < 40 and load_v < 1.0:
        return 27
    if cpu_v < 70 and load_v < 1.8:
        return 9
    return 1


def hot(cpu, load1):
    if cpu not in (None, "") and float(cpu) >= 90:
        return True
    if load1 not in (None, "") and float(load1) >= 1.8:
        return True
    return False


def blank(mesh, target, held):
    row = {col: "" for col in columns_for(mesh)}
    version = getattr(mesh, "version", None) or {
        "mod_commit": "",
        "mod_sha256": "",
        "version_certainty": "unknown",
    }
    row.update({"speed_bps": WANDER_BPS, "n_target": target, "n_held": held, "result": "fail", "binding": "none", "transfers": 0})
    row.update(version)
    return row


def fill_row(mesh, row, result):
    side, best, down = busiest(mesh, result)
    transfers = sum(int(st.get("transfers") or 0) for st in (result.get("status") or {}).values() if isinstance(st, dict))
    row.update({
        "busiest": side,
        "busiest_cpu": "" if best.get("cpu") is None else best.get("cpu"),
        "busiest_load1": "" if best.get("load1") is None else best.get("load1"),
        "busiest_mem_avail_mb": "" if best.get("avail") is None else best.get("avail"),
        "transfers": transfers,
    })
    logs = result.get("logs") or {}
    measured = bool(logs) and not any(isinstance(item, dict) and item.get("unmeasured") for item in logs.values())
    if measured:
        for src, item in logs.items():
            if not isinstance(item, dict) or item.get("error") or item.get("missing"):
                measured = False
                break
    if measured:
        for src, item in logs.items():
            row[f"handoffs_from_{src}"] = int(item.get("from_lines") or 0)
            row[f"moved_wrongly_{src}"] = int(item.get("moved_wrongly") or 0)
            for side_name in mesh.sections:
                row.setdefault(f"handoffs_to_{side_name}", 0)
            for dest, count in (item.get("to") or {}).items():
                key = f"handoffs_to_{dest}"
                if key in row:
                    row[key] = int(row[key] or 0) + int(count)
        for side_name in mesh.sections:
            row[f"handoffs_to_{side_name}"] = int(row.get(f"handoffs_to_{side_name}") or 0)
            row[f"handoffs_from_{side_name}"] = int(row.get(f"handoffs_from_{side_name}") or 0)
            row[f"moved_wrongly_{side_name}"] = int(row.get(f"moved_wrongly_{side_name}") or 0)
    return row, down, transfers


def append_csv(mesh, row):
    ensure_csv(mesh)
    with mesh.csv_path.open("a", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=columns_for(mesh), extrasaction="ignore")
        writer.writerow({key: row.get(key, "") for key in columns_for(mesh)})


# Same add shape as the 16-server climb: a generator under about 9 GiB, with a
# free index, takes up to 9 bots in one step. Load crossing 1.8 does not shrink
# that to +1. Skip a host at or above 9 GiB, or within 2 GiB of the 12 GiB cap.
# The budget ceiling is the cap minus 2 GiB (12288 - 2048 = 10240), same as
# p16-lg6-add.py. 9 GiB is only the skip line, not the budget ceiling.
GIB9_MIB = 9 * 1024
ADD_PACK_MIB = 384
ADD_EACH_MIB = 16
PER_GENERATOR = 9


def harness_memory(mesh, ip):
    remote = r"""python3 -c '
from pathlib import Path
def num(path):
    try:
        return int(Path(path).read_text().strip())
    except Exception:
        return -1
cur = num("/sys/fs/cgroup/system.slice/ck-mc-harness.service/memory.current")
cap = num("/sys/fs/cgroup/system.slice/ck-mc-harness.service/memory.max")
print(cur, cap)
'"""
    text = ssh(mesh, ip, remote, timeout=20).strip().splitlines()[-1]
    cur, cap = [int(part) for part in text.split()]
    cur_mib = cur // 1048576 if cur > 0 else 0
    cap_mib = cap // 1048576 if cap > 0 else 0
    return cur_mib, cap_mib


def memory_take(mem, by_index):
    """Bots that fit under cap-2048. Zero at or above 9 GiB, within 2 GiB of the cap, or without an index."""
    if by_index <= 0 or not isinstance(mem, tuple):
        return 0
    cur, cap = mem
    if cur >= GIB9_MIB or cap <= 0 or cur >= cap - 2048:
        return 0
    ceiling = cap - 2048
    budget = ceiling - cur
    n = 0
    while n < PER_GENERATOR and n < by_index:
        cost = ADD_PACK_MIB + n * ADD_EACH_MIB
        if cost > budget:
            break
        n += 1
    return n


def plan_four_adds(mesh, loads, caps):
    """Per-generator adds for the four-server climb. Does not shrink to +1."""
    status = poll_status(mesh, loads)
    mems = fan(loads, lambda pair: harness_memory(mesh, pair[1]), 8)
    counts = {}
    add = 0
    for name, _ip in loads:
        st = status.get(name) or {}
        active = int(st.get("active") or 0) if isinstance(st, dict) and not st.get("error") else 0
        if caps.get(name) is not None:
            counts[name] = active
            continue
        limit = int(st.get("index_limit") or 0) if isinstance(st, dict) else 0
        used = int(st.get("index_used") or 0) if isinstance(st, dict) else 0
        by_index = max(0, limit - used) if limit > 0 else 0
        if isinstance(st, dict) and st.get("busy"):
            take = 0
        else:
            mem = mems.get(name)
            take = memory_take(mem, by_index) if isinstance(mem, tuple) else 0
        counts[name] = active + take
        add += take
    return counts, add


def counts_for(mesh, loads, target, caps):
    """Desired active count per host. Free hosts only receive bots their open slots can hold."""
    status = poll_status(mesh, loads)
    counts = {}
    fixed = 0
    seated_free = 0
    free = []
    for name, _ip in loads:
        if caps.get(name) is not None:
            st = status.get(name) or {}
            active = int(st.get("active") or 0) if isinstance(st, dict) else 0
            # A capped window is not reopened. Bots that left are replaced on a host that still has room.
            counts[name] = active
            fixed += active
            continue
        st = status.get(name) or {}
        if not isinstance(st, dict) or st.get("error"):
            free.append((name, 0, 0))
            continue
        active = int(st.get("active") or 0)
        limit = int(st.get("index_limit") or 0)
        used = int(st.get("index_used") or 0)
        room = max(0, limit - used) if limit else 0
        free.append((name, active, room))
        seated_free += active
    rest = max(0, target - fixed - seated_free)
    if rest and not free:
        return None
    mems = {}
    if mesh.name == "four" and free:
        ips = {name: ip for name, ip in loads}
        mems = fan([(name, ips[name]) for name, _active, _room in free if name in ips], lambda pair: harness_memory(mesh, pair[1]), 8)
    for name, active, room in free:
        allowance = room
        if mesh.name == "four":
            mem = mems.get(name)
            allowance = memory_take(mem, room) if isinstance(mem, tuple) else 0
        take = min(allowance, rest)
        counts[name] = active + take
        rest -= take
    return counts


_SPAN_MEASURED = {}


def span_measured(start, end):
    """True when every index in [start, end) already has a dry column.

    An earlier unknown index is not part of this span. A missing column stays
    unknown. This does not write the column table.
    """
    start = int(start)
    end = int(end)
    if end <= start or end - start > PIN_SPAN:
        return False
    key = (start, end)
    if key in _SPAN_MEASURED:
        return _SPAN_MEASURED[key]
    script = r"""
const { sectionSpawn, sectionsFor, columnFromTable } = require("./section-spawn.cjs");
const fs = require("fs");
const table = JSON.parse(fs.readFileSync("./section-columns.json", "utf8"));
const column = columnFromTable(table);
const start = Number(process.env.SPAN_START);
const end = Number(process.env.SPAN_END);
const sections = sectionsFor("four");
for (let index = start; index < end; index++) {
  let dry = false;
  try {
    const row = sectionSpawn(index, sections, { column });
    dry = row && row.pending === false;
  } catch (err) {
    dry = false;
  }
  if (!dry) {
    process.stdout.write("gap\n");
    process.exit(0);
  }
}
process.stdout.write("measured\n");
"""
    env = dict(**{k: v for k, v in __import__("os").environ.items()})
    env["SPAN_START"] = str(start)
    env["SPAN_END"] = str(end)
    proc = subprocess.run(
        ["node", "-e", script],
        cwd=DIR,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        timeout=60,
        check=False,
    )
    if proc.returncode != 0:
        return False
    last = proc.stdout.decode().strip().splitlines()
    ok = bool(last) and last[-1] == "measured"
    _SPAN_MEASURED[key] = ok
    return ok


def column_window_blocked(mesh, base, limit, used, active):
    """True when this harness must stay at the bots it already has.

    The contiguous measured prefix still stops an unknown next index. A window
    whose own remaining columns are measured is not blocked by an earlier hole.
    """
    measured = getattr(mesh, "measured_index_limit", None)
    if measured is None:
        return False
    nxt = int(base) + int(used)
    blocked = nxt >= measured or (int(base) >= measured and int(active) == 0)
    if not blocked:
        return False
    end = int(base) + int(limit)
    if getattr(mesh, "name", "") == "four" and int(limit) > 0 and nxt < end and span_measured(nxt, end):
        return False
    return True


def apply_column_caps(mesh, loads, caps):
    """A host whose next index has no measured column stays at the bots it has."""
    if getattr(mesh, "measured_index_limit", None) is None:
        return caps
    for name, st in poll_status(mesh, loads).items():
        if not isinstance(st, dict) or st.get("error"):
            continue
        base = int(st.get("index_base") or 0)
        limit = int(st.get("index_limit") or 0)
        used = int(st.get("index_used") or 0)
        active = int(st.get("active") or 0)
        if not column_window_blocked(mesh, base, limit, used, active):
            continue
        previous = caps.get(name)
        caps[name] = active
        nxt = base + used
        if previous != active:
            log(mesh, f"{name} index {nxt} is past the measured columns; left at {active}, not a mesh cap")
    return caps


def attach_new_generators(mesh, loads):
    """Pick up a generator added to the inventory without restarting this process."""
    found = inventory()
    names = [name for name, _ip in loads]
    for name in sorted(found):
        if name.startswith("test-mc-f4-lg-") and name not in names:
            names.append(name)
            log(mesh, f"{name} joined the climb")
    return [(name, found[name]) for name in names if name in found]


def keeper_room(mesh, loads, caps):
    """Bots already up, plus index slots on hosts that are still allowed to add."""
    status = poll_status(mesh, loads)
    if not any(answered_count(st) is not None for st in status.values()):
        return None, 0
    seated = 0
    room = 0
    for name, _ip in loads:
        st = status.get(name) or {}
        if not isinstance(st, dict) or st.get("error"):
            continue
        seated += int(st.get("active") or 0)
        if caps.get(name) is not None:
            continue
        limit = int(st.get("index_limit") or 0)
        used = int(st.get("index_used") or 0)
        if limit > 0:
            room += max(0, limit - used)
    return seated, room


def note_spawn_misses(mesh, loads, floors):
    """Log a harness spawn-miss, leave that index, and return. Never exits the process."""
    probe = """python3 -c '
import re, subprocess
floor = %d
try:
    text = subprocess.check_output(["journalctl", "-u", "ck-mc-harness", "-n", "800", "--no-pager", "-o", "cat"], text=True, errors="replace")
except Exception:
    text = ""
seen = []
for line in text.splitlines():
    if "spawn-miss" not in line:
        continue
    match = re.search(r"Load(\\d+)", line)
    if not match:
        continue
    index = int(match.group(1)) - 1
    if index < floor:
        continue
    item = "Load%%d %%d" %% (index + 1, index)
    if item not in seen:
        seen.append(item)
print("\\n".join(seen) if seen else "none")
'"""
    for name, ip in loads:
        floor = int(floors.get(name, 0))
        try:
            text = ssh(mesh, ip, probe % floor, timeout=25)
        except Exception:
            continue
        for line in text.splitlines():
            bot, _, index_s = line.partition(" ")
            if not bot.startswith("Load") or not index_s.isdigit():
                continue
            log(mesh, f"spawn-miss {bot} index {index_s} left; continuing")


def one_rung(mesh, loads, games, target, caps):
    caps = apply_column_caps(mesh, loads, caps)
    planned = getattr(mesh, "planned_counts", None)
    mesh.planned_counts = None
    if isinstance(planned, dict):
        status = poll_status(mesh, loads)
        counts = {}
        for name, _ip in loads:
            st = status.get(name) or {}
            active = int(st.get("active") or 0) if isinstance(st, dict) else 0
            if caps.get(name) is not None:
                counts[name] = active
            else:
                counts[name] = max(active, int(planned.get(name, active)))
    else:
        counts = counts_for(mesh, loads, target, caps)
    if counts is None:
        return None, caps, "no-room"
    have = sum_answered(poll_status(mesh, loads))
    if have is None:
        log(mesh, f"rung target={target} no generator answered; not adding")
        return None, caps, "stop"
    log(mesh, f"rung target={target} have={have} add={max(0, target - have)} restart=no")
    floors = {}
    for name, st in poll_status(mesh, loads).items():
        if isinstance(st, dict) and not st.get("error"):
            floors[name] = int(st.get("index_base") or 0) + int(st.get("index_used") or 0)
    errors = add_only(mesh, loads, counts)
    status, active, ready = wait_settled(mesh, loads, target)
    if active is None:
        log(mesh, "spawn status missing; not treating it as zero seated")
        return None, caps, "stop"
    if active < target:
        note_spawn_misses(mesh, loads, floors)
    for name, err in errors.items():
        status.setdefault(name, {"active": 0})
        status[name]["add_error"] = err
    mem = {}
    for name, st in status.items():
        if isinstance(st, dict) and memory_text(st.get("add_error") or ""):
            mem[name] = int(st.get("active") or 0)
    if active <= 0:
        row = blank(mesh, target, 0)
        row["binding"] = "loadgen" if mem else "spawn"
        row["note"] = "nothing connected"
        return row, caps, "stop"
    result = hold(mesh, loads, games, f"{mesh.name}-{target}", active if mem or not ready else target)
    end_active = sum(int(st.get("active") or 0) for st in (result.get("status") or {}).values() if isinstance(st, dict))
    row = blank(mesh, target, end_active)
    row, down, transfers = fill_row(mesh, row, result)
    moving, wandered = sample_mix(mesh, loads, {name: int((status.get(name) or {}).get("active") or 0) for name, _ip in loads})
    row["moving_sample"] = moving
    row["wander_sample"] = wandered
    clean = (
        result.get("clock_started")
        and not result.get("stopped_early")
        and transfers == 0
        and end_active == target
        and result.get("min_active") == target
        and result.get("hold_sec", 0) >= HOLD_SEC - 5
        and not down
    )
    row["reached"] = bool(ready)
    row["hot_whole"] = "true" if result.get("hot_whole") else "false"
    stop_bits = []
    for entry in (result.get("logs") or {}).values():
        if not isinstance(entry, dict):
            continue
        if entry.get("tick"):
            stop_bits.append("60s tick")
        elif entry.get("oom"):
            stop_bits.append("oom")
    if stop_bits:
        row["stop_reason"] = stop_bits[0]
        row["binding"] = "stop"
        row["server_refused"] = False
        row["result"] = "fail"
        row["note"] = (row.get("note") or "") + ("; " if row.get("note") else "") + stop_bits[0]
        log(mesh, f"STOP target={target} held={row['n_held']} {row['stop_reason']}")
        return row, caps, "stop"
    refused = bool(down)
    row["server_refused"] = refused
    row["result"] = "clean" if clean else "fail"
    bits = [f"moving {moving}", f"path=wander {wandered}"]
    if down:
        row["binding"] = "game"
        bits.append("game down " + ",".join(down))
    elif mem:
        row["binding"] = "loadgen"
        named = []
        for name, stable in sorted(mem.items()):
            live = int((status.get(name) or {}).get("active") or stable)
            caps[name] = live
            named.append(f"{name} stable {live}")
        bits.append("loadgen " + ", ".join(named))
    elif hot(row["busiest_cpu"], row["busiest_load1"]) and not clean:
        row["binding"] = "cpu"
    elif transfers:
        row["binding"] = "transfers"
    elif not clean:
        row["binding"] = "spawn"
    row["note"] = "; ".join(bit for bit in bits if bit)
    log(mesh, f"RESULT target={target} held={row['n_held']} {row['result']} busiest={row['busiest']} cpu={row['busiest_cpu']} load={row['busiest_load1']} xfer={transfers} {row['note']}")
    why = "game" if down else "ok"
    return row, caps, why


def write_holds(mesh, bend, needs_loadgen):
    rows = []
    if mesh.csv_path.exists():
        with mesh.csv_path.open(newline="") as fh:
            rows = list(csv.DictReader(fh))
    lines = [rules_text(mesh).rstrip(), "", "### Holds", ""]
    if not rows:
        lines.append("No hold yet.")
    for row in rows:
        lines.append(
            f"- {WANDER_BPS} blocks/s, target {row.get('n_target')}, held {row.get('n_held')}, {row.get('result')}, "
            f"busiest {row.get('busiest') or 'n/a'} CPU {row.get('busiest_cpu') or 'n/a'}% "
            f"load {row.get('busiest_load1') or 'n/a'} MemAvailable {row.get('busiest_mem_avail_mb') or 'n/a'} MB, "
            f"transfers {row.get('transfers')}, binding {row.get('binding') or 'none'}"
            + (f". {row.get('note')}" if row.get("note") else "")
        )
    lines.append("")
    if bend:
        held = float(bend["n_held"] or 0)
        per = held / mesh.servers if mesh.servers else 0
        thousand = int(round(1000 / per)) if per else None
        lines.append(
            f"The curve bends at {bend['n_held']} players. The busiest server was {bend['busiest']} at "
            f"{bend['busiest_cpu']}% CPU and load {bend['busiest_load1']}. "
            f"That is {per:.1f} players per server. At that density a thousand players would take about {thousand} servers of this size."
        )
        lines.append(f"Jar sha256 {JAR_SHA}, commit {COMMIT}.")
    if needs_loadgen:
        lines.append(
            "Every load generator on this climb is full and the game servers are still quiet. "
            "Price more b3-16s from the catalog and add only enough to get past it. This driver did not create an instance."
        )
    lines.append("Bots already connected were left in place. Nothing was cleared.")
    lines.append("")
    refresh_note(mesh, "\n".join(lines) + "\n")


def resume_target(mesh):
    target = PACK_SIZE
    if not mesh.csv_path.exists():
        return target
    with mesh.csv_path.open(newline="") as fh:
        rows = list(csv.DictReader(fh))
    for prior in reversed(rows):
        if prior.get("result") != "clean":
            continue
        return int(prior["n_held"]) + jump_after(prior.get("busiest_cpu"), prior.get("busiest_load1"))
    return target


def run(mesh):
    import sys
    arg = sys.argv[1] if len(sys.argv) > 1 else ""
    if arg == "--init-local":
        init_local(mesh)
        print(f"csv {mesh.csv_path}")
        print(f"note {mesh.note_path}")
        return
    if arg == "--print-plan":
        hosts, missing = resolve([name for name, _side in mesh.games] + list(mesh.loads))
        print(f"mesh {mesh.name} sections {mesh.servers} pack {PACK_SIZE} heap {PACK_HEAP} bps {WANDER_BPS}")
        print("games " + " ".join(name for name, _side in mesh.games))
        print("loads " + " ".join(mesh.loads))
        print("resolved " + " ".join(sorted(hosts)))
        print("missing " + (" ".join(missing) if missing else "none"))
        print(f"csv {mesh.csv_path.name}")
        return
    if arg:
        raise SystemExit("unknown argument")
    hosts, missing = resolve([name for name, _side in mesh.games] + list(mesh.loads))
    if missing:
        raise SystemExit("missing inventory for " + " ".join(missing))
    games = {name: hosts[name] for name, _side in mesh.games}
    loads = [(name, hosts[name]) for name in mesh.loads]
    allowed = {name for name, _side in mesh.games} | set(mesh.loads)
    if set(hosts) - allowed:
        raise SystemExit("refusing a host that is not on this mesh")
    require_jar(mesh, games)
    prepare_idle(mesh, loads, games)
    if not LOCAL_COLUMNS.exists():
        raise SystemExit(
            f"no column table at {LOCAL_COLUMNS.name}; refusing to invent a sea-level Y"
        )
    target = resume_target(mesh)
    park_overloaded(mesh, loads)
    caps = {name: 0 for name in getattr(mesh, "parked", ())}
    measured = getattr(mesh, "attached_index_limit", None)
    if measured is None:
        measured = getattr(mesh, "measured_index_limit", None)
    if measured is not None:
        seen = []
        for name, st in poll_status(mesh, loads).items():
            if not isinstance(st, dict) or st.get("error"):
                continue
            seen.append((
                name,
                int(st.get("index_base") or 0),
                int(st.get("index_limit") or 0),
                int(st.get("index_used") or 0),
                int(st.get("active") or 0),
            ))
        # A host stops at the next host's base, so two generators do not share a
        # name. Indexes through the measured table stay placeable.
        stay = measured
        ordered = sorted(seen, key=lambda row: (row[1], row[0]))
        for i, (name, base, limit, used, active) in enumerate(ordered):
            end = base + limit
            if i + 1 < len(ordered):
                end = min(end, ordered[i + 1][1])
            end = min(end, stay)
            if base >= stay or base + used >= end:
                caps[name] = active
                log(mesh, f"{name} index {base} used {used} stops at {end}; not a mesh cap")
    last_hot = None
    bend = None
    needs_loadgen = False
    log(mesh, f"resume target={target}")
    seated, _room = keeper_room(mesh, loads, caps)
    clean_held = 0
    if mesh.csv_path.exists():
        with mesh.csv_path.open(newline="") as fh:
            for prior in csv.DictReader(fh):
                if prior.get("result") == "clean":
                    clean_held = int(prior.get("n_held") or 0)
    if seated is None:
        raise SystemExit("no generator answered; not treating that as zero seated")
    note_started(mesh)
    if (clean_held and seated < clean_held) or target < seated:
        jump, side, cpu, load1 = fresh_jump(mesh, games)
        target = seated + jump
        log(mesh, f"live seated={seated} busiest={side} cpu={cpu} load={load1} step=+{jump} target={target}")
    while target < 4000:
        loads = attach_new_generators(mesh, loads)
        seated, room = keeper_room(mesh, loads, caps)
        if seated is None:
            log(mesh, "no generator answered; not adding")
            time.sleep(20)
            continue
        jump, side, cpu, load1 = fresh_jump(mesh, games)
        players = (seated + mesh.servers - 1) // mesh.servers if mesh.servers else None
        cpu_v = float(cpu) if cpu not in (None, "") else 0.0
        load_v = float(load1) if load1 not in (None, "") else 0.0
        near_line = (players is not None and players >= 100) or cpu_v >= 70 or load_v >= 1.8
        if not near_line:
            if mesh.name == "four":
                caps = apply_column_caps(mesh, loads, caps)
            counts, add = plan_four_adds(mesh, loads, caps)
            if add <= 0:
                log(mesh, f"fast pack found no generator under the memory line; seated {seated}")
                time.sleep(20)
                continue
            log(mesh, f"fast pack +{add} even-share {players} cpu={cpu} load={load1}; no 180s hold")
            errors = add_only(mesh, loads, counts)
            _status, active, _ready = wait_settled(mesh, loads, seated + add)
            if active is None:
                log(mesh, "fast pack status missing; stopping this climb")
                note_stopped(mesh, "spawn status missing")
                break
            if active <= 0:
                log(mesh, "fast pack seated nobody; stopping this climb; not a bend")
                note_stopped(mesh, "no bot seated; unplaced indexes were spawn misses")
                break
            if errors:
                log(mesh, "fast pack errors " + ",".join(sorted(errors)))
            target = active
            continue
        if mesh.name == "four":
            caps = apply_column_caps(mesh, loads, caps)
            counts, add = plan_four_adds(mesh, loads, caps)
            if add <= 0:
                log(mesh, f"no open slot for target {target}; seated {seated}; not a bend")
                time.sleep(20)
                continue
            target = seated + add
            mesh.planned_counts = counts
        else:
            if room <= 0 and target > seated:
                log(mesh, f"no open slot for target {target}; seated {seated}; not a bend")
                time.sleep(20)
                continue
            if room > 0 and target > seated + room:
                log(mesh, f"next pack target {target} clamped to {seated + room}")
                target = seated + room
        row, caps, why = one_rung(mesh, loads, games, target, caps)
        if row is not None and row.get("stop_reason"):
            append_csv(mesh, row)
            log(mesh, "stopped this climb; not a bend")
            note_stopped(mesh, row.get("stop_reason"))
            break
        if row is None or why in ("no-room", "stop"):
            log(mesh, "pack did not start; not a bend")
            seated, _room = keeper_room(mesh, loads, caps)
            target = seated + 1
            time.sleep(10)
            continue
        append_csv(mesh, row)
        if row.get("stop_reason") or why == "stop":
            log(mesh, "stopped this climb; not a bend")
            break
        if bend.is_bend(
            hot_whole=bool(last_hot and str(last_hot.get("hot_whole")).lower() == "true"),
            next_result=row.get("result") or "",
            next_binding=row.get("binding") or "",
            next_note=row.get("note") or "",
        ):
            bend = last_hot
            break
        if (row.get("binding") or "") == "game":
            log(mesh, "a game server refused the pack before a hot hold; not a bend")
            seated, _room = keeper_room(mesh, loads, caps)
            target = max(seated or 1, 1)
            time.sleep(20)
            continue
        if row["result"] == "clean" and row["binding"] != "loadgen":
            last_hot = row if str(row.get("hot_whole")).lower() == "true" else None
            target = int(row["n_held"]) + jump_after(row["busiest_cpu"], row["busiest_load1"])
            continue
        held = int(row["n_held"] or 0)
        step = jump_after(row["busiest_cpu"], row["busiest_load1"]) if held else 1
        target = held + max(step, 1)
        log(mesh, f"hold did not stand at {row['n_target']}; not a bend; next target {target}")
        continue
    if bend:
        write_holds(mesh, bend, False)
        log(mesh, "note written; connected bots were left up")
    else:
        log(mesh, "left the climb without a bend; note not rewritten")
