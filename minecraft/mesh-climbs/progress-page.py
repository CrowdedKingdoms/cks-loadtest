#!/usr/bin/env python3
"""Progress page for the mesh climbs, and a map of the nine-server world.

Reads the climb CSVs and re-reads them. The chart section also shows
the live connected counts on the four-server and 16-server meshes, read
from harness status. The map reads positions from the nine game servers.
Its headline is the live seated total on the nine generators. Dots stay
bots that have a saved position. Under the map, the generator diagram
rereads the local nine-climb log (or the snapshot beside it). That is a
disk read. It does not open a connection to a game server or a generator.
Does not place bots, clear bots, or start a climb. Binds 127.0.0.1:8768 only.
"""

from __future__ import annotations

import csv
import io
import json
import math
import os
import re
import signal
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import bend
import climb_mesh

def find_project_root() -> Path:
    if os.environ.get("CK_PROJECT_ROOT"):
        return Path(os.environ["CK_PROJECT_ROOT"]).expanduser()
    here = Path(__file__).resolve()
    for parent in here.parents:
        nested = parent / "cks-project-root" / "seam-scaling-four-2026-11-19.csv"
        if nested.exists():
            return nested.parent
        if (parent / "seam-scaling-four-2026-11-19.csv").exists():
            return parent
    return here.parents[2]


ROOT = find_project_root()
FOUR_CSV = ROOT / "seam-scaling-four-2026-11-19.csv"
SIXTEEN_CSV = ROOT / "seam-scaling-sixteen-2026-11-19.csv"
NINE_CSV = ROOT / "seam-scaling-nine-2026-11-23.csv"
HERE = Path(__file__).resolve().parent
PID_PATH = HERE / "progress-page.pid"
BIND = ("127.0.0.1", 8768)
SIXTEEN_LOADS = tuple(f"test-mc-p5-lg-{i}" for i in range(1, 19))
FOUR_FIXED = tuple(f"test-mc-lg-{i}" for i in range(1, 6)) + ("test-mc-loadgen-1",)
F4_FLEET = Path(os.environ["CK_F4_FLEET_FILE"]).expanduser() if os.environ.get("CK_F4_FLEET_FILE") else None
_pop_lock = threading.Lock()
_four_lock = threading.Lock()
_pop_ready = threading.Event()
_pop = {"value": None, "answered": 0, "sought": len(SIXTEEN_LOADS)}
_four = {"value": None, "answered": 0, "sought": 0}

COMMIT = "c9ff2a3f3c711de6d5af17ef2f36e7b913c6e252"
JAR_SHA = "b7a2ab65f87de5d0e542476fea1421ee6d89c224560ecd7f172c875c220d5aa0"
NINE_PLAYERS = 513
NINE_SERVERS = 9

SEAM_HOLDS = (
    (2, 44, "22 per server"),
    (3, 80, "about 27 per server"),
    (4, 90, "22.5 per server"),
)

DOCUMENT = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Mesh climbs</title>
<style>
  :root { color-scheme: light; --ink:#1c1917; --muted:#57534e; --line:#e7e5e4; --paper:#f7f6f3; --card:#fff; --ok:#14532d; --okbg:#ecfdf3; --wait:#92400e; --waitbg:#fffbeb; }
  * { box-sizing: border-box; }
  body { margin: 0; font: 15px/1.45 system-ui, sans-serif; color: var(--ink); background: var(--paper); }
  header, main { max-width: 980px; margin: 0 auto; padding: 0 20px; }
  header { padding-top: 20px; }
  h1 { font-size: 1.45rem; margin: 0 0 8px; }
  h2 { font-size: 1.05rem; margin: 0 0 8px; }
  .sub, .note, .foot { color: var(--muted); }
  .status { margin: 0 0 12px; padding: 10px 12px; background: var(--okbg); border: 1px solid #bbf7d0; border-radius: 10px; color: var(--ok); font-weight: 650; }
  .status.wait { background: var(--waitbg); border-color: #fde68a; color: var(--wait); }
  .nows { display: flex; flex-wrap: wrap; gap: 8px; }
  .nows .status { flex: 1 1 16rem; }
  section { background: var(--card); border: 1px solid var(--line); border-radius: 10px; padding: 14px; margin: 0 0 14px; }
  svg { width: 100%; height: auto; display: block; }
  table { width: 100%; border-collapse: collapse; font-variant-numeric: tabular-nums; }
  th, td { text-align: left; padding: 6px 8px; border-bottom: 1px solid var(--line); vertical-align: top; }
  th { font-size: 12px; text-transform: uppercase; letter-spacing: .02em; color: var(--muted); }
  td.num, th.num { text-align: right; white-space: nowrap; }
  tr.fail td { color: #9f1239; }
  tr.bend td { background: #f0fdf4; }
  .legend { display: flex; flex-wrap: wrap; gap: 8px 14px; padding: 0; margin: 8px 0 0; list-style: none; color: var(--muted); font-size: 13px; }
  .legend i { display: inline-block; width: 10px; height: 10px; border-radius: 50%; margin-right: 6px; vertical-align: -1px; }
  .legend i.diamond { border-radius: 1px; transform: rotate(45deg); }
  .scroll { overflow-x: auto; }
  .foot { font-size: 12.5px; padding-bottom: 28px; }
  code { font-size: 0.92em; }
</style>
</head>
<body data-page="mesh-climbs">
<header>
  <h1>Mesh climbs</h1>
  <p class="sub"><a href="/map">Nine-server world map</a></p>
  <p class="sub">Four-server climb and 16-server climb. Both are this jar, commit COMMIT_TOKEN, sha256 SHA_TOKEN.</p>
  <p>The nine-server point is a different build. It is the hold of 513 players on nine servers, about 57 per server, on the previous jar.</p>
  <p class="note">C(N) = N*C(1) / (1 + sigma*(N-1) + kappa*N*(N-1)). The curve is drawn after both climbs have a bend row: a hold whose load stays at or above 1.8 for the whole 180 seconds, or whose busiest core stays at or above 90%, and whose next count fails because the game servers refused it. A six-hour walker exit, a harness restart, a full index window, and a load-generator miss are not bends.</p>
</header>
<main id="live">
<!--LIVE-->
</main>
<p id="poll" class="foot"></p>
<script>
async function tick() {
  try {
    const res = await fetch("/live", {cache: "no-store"});
    if (!res.ok) throw new Error("HTTP " + res.status);
    document.getElementById("live").innerHTML = await res.text();
    const poll = document.getElementById("poll");
    if (poll) poll.textContent = "";
  } catch (err) {
    const poll = document.getElementById("poll");
    if (poll) poll.textContent = "Could not refresh the page.";
  }
}
setInterval(tick, 4000);
</script>
<noscript><meta http-equiv="refresh" content="4"></noscript>
</body>
</html>
"""


def esc(value) -> str:
    return (
        str(value)
        .replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace('"', "&quot;")
    )


def raw_num(text):
    raw = (text or "").strip()
    if not raw:
        return None, ""
    try:
        return float(raw), raw
    except ValueError:
        return None, ""


def as_number(text):
    value, _raw = raw_num(text)
    if value is None:
        return None
    if float(value).is_integer():
        return int(value)
    return value


def whole(value) -> str:
    if value is None:
        return "—"
    if isinstance(value, float) and not float(value).is_integer():
        return f"{value:.4g}"
    return str(int(value))


def format_param(value: float) -> str:
    return f"{value:.8g}"


def per_server(held, servers) -> str:
    if held is None or not servers:
        return "—"
    value = float(held) / float(servers)
    if abs(value - round(value)) < 1e-9:
        return str(int(round(value)))
    return f"{value:.1f}"


def read_changeable(path: Path) -> str:
    data = path.read_bytes()
    text = data.decode("utf-8", "replace")
    if text and not text.endswith("\n") and "\n" in text:
        text = text[: text.rfind("\n") + 1]
    return text


def sum_prefix(raw: dict, prefix: str):
    keys = [key for key in raw if key and str(key).startswith(prefix)]
    if not keys:
        return None
    if all(not (raw.get(key) or "").strip() for key in keys):
        return None
    total = 0
    for key in keys:
        value, _raw = raw_num(raw.get(key))
        if value is None:
            continue
        total += int(value) if float(value).is_integer() else value
    return total


def moving_label(text: str) -> str:
    raw = (text or "").strip()
    match = re.fullmatch(r"(\d+)\s*/\s*(\d+)", raw)
    if not match:
        return raw
    return f"{int(match.group(1))} of {int(match.group(2))}"


def parse_text(text: str) -> list[dict]:
    rows = []
    reader = csv.DictReader(io.StringIO(text))
    if not reader.fieldnames:
        return rows
    for raw in reader:
        if not raw:
            continue
        raw = {str(key).strip(): ("" if value is None else str(value).strip()) for key, value in raw.items() if key}
        result = (raw.get("result") or "").strip().lower()
        if not result:
            continue
        cpu, cpu_text = raw_num(raw.get("busiest_cpu"))
        load, load_text = raw_num(raw.get("busiest_load1"))
        handoffs = sum_prefix(raw, "handoffs_to_")
        if handoffs is None:
            handoffs = sum_prefix(raw, "handoffs_from_")
        rows.append({
            "target": as_number(raw.get("n_target")),
            "held": as_number(raw.get("n_held")),
            "result": result,
            "binding": (raw.get("binding") or "").strip().lower(),
            "busiest": (raw.get("busiest") or "").strip(),
            "cpu": cpu,
            "load": load,
            "cpu_text": cpu_text,
            "load_text": load_text,
            "handoffs": handoffs,
            "moved_wrongly": sum_prefix(raw, "moved_wrongly_"),
            "moving": moving_label(raw.get("moving_sample") or ""),
            "note": (raw.get("note") or "").strip().lower(),
            "hot_whole": bend.truthy(raw.get("hot_whole")),
            "stop_reason": (raw.get("stop_reason") or "").strip().lower(),
            "is_bend": False,
        })
    return rows


def load_csv(path: Path, filename: str):
    try:
        text = read_changeable(path)
    except OSError:
        return [], f"Could not read {filename}.", None
    try:
        rows = parse_text(text)
    except csv.Error:
        return [], f"Could not parse {filename}.", None
    try:
        stamp = datetime.fromtimestamp(path.stat().st_mtime).isoformat(timespec="seconds")
    except OSError:
        stamp = None
    return rows, None, stamp


def hot(row: dict) -> bool:
    cpu = row.get("cpu")
    load = row.get("load")
    return (cpu is not None and cpu >= 90) or (load is not None and load >= 1.8)


def servers_refused(row: dict) -> bool:
    """The next count failed because the game servers refused it.

    A six-hour walker exit, a harness restart, and a full index window are not refusals.
    """
    note = row.get("note") or ""
    if any(phrase in note for phrase in ("max runtime", "21600", "harness", "index window")):
        return False
    return row.get("binding") == "game"


def mark_bend(rows: list[dict]):
    """A bend is a hold that stayed hot the whole 180 seconds, then a count the game servers could not take.

    One CPU cell is not enough. A 60-second tick, an OOM, a generator miss, and a spawn miss are not bends.
    """
    chosen = None
    for index in range(len(rows) - 1):
        row = rows[index]
        nxt = rows[index + 1]
        note = " ".join(part for part in (nxt.get("note"), nxt.get("stop_reason")) if part)
        if not bend.is_bend(
            hot_whole=bool(row.get("hot_whole")),
            next_result=nxt.get("result") or "",
            next_binding=nxt.get("binding") or "",
            next_note=note,
        ):
            continue
        if row["held"] is None or nxt["target"] is None or nxt["target"] <= row["held"]:
            continue
        chosen = row
    for row in rows:
        row["is_bend"] = row is chosen
    return chosen


def chart_point(rows: list[dict], servers: int, color: str, name: str):
    bend = next((row for row in rows if row["is_bend"]), None)
    if bend is not None and bend["held"] is not None and bend["held"] > 0:
        return {"x": servers, "y": float(bend["held"]), "kind": "bend", "color": color, "name": name}
    for row in reversed(rows):
        if row["result"] == "clean" and row["held"] is not None and row["held"] > 0:
            return {"x": servers, "y": float(row["held"]), "kind": "latest", "color": color, "name": name}
    return None


def usl(n: float, c1: float, sigma: float, kappa: float):
    denom = 1.0 + sigma * (n - 1.0) + kappa * n * (n - 1.0)
    if not math.isfinite(denom) or denom <= 1e-12:
        return None
    value = n * c1 / denom
    if not math.isfinite(value):
        return None
    return value


def solve3(matrix, rhs):
    width = 4
    rows = [list(map(float, row)) + [float(rhs[i])] for i, row in enumerate(matrix)]
    for col in range(3):
        pivot = max(range(col, 3), key=lambda index: abs(rows[index][col]))
        if abs(rows[pivot][col]) < 1e-12:
            return None
        rows[col], rows[pivot] = rows[pivot], rows[col]
        scale = rows[col][col]
        for col_index in range(col, width):
            rows[col][col_index] /= scale
        for row_index in range(3):
            if row_index == col:
                continue
            factor = rows[row_index][col]
            if factor == 0:
                continue
            for col_index in range(col, width):
                rows[row_index][col_index] -= factor * rows[col][col_index]
    return [rows[0][3], rows[1][3], rows[2][3]]


def recover(points):
    matrix = []
    rhs = []
    for servers, players in points:
        servers = float(servers)
        players = float(players)
        matrix.append([
            servers,
            -players * (servers - 1.0),
            -players * servers * (servers - 1.0),
        ])
        rhs.append(players)
    return solve3(matrix, rhs)


def try_fit(four_rows: list[dict], sixteen_rows: list[dict]):
    four = next((row for row in four_rows if row["is_bend"]), None)
    sixteen = next((row for row in sixteen_rows if row["is_bend"]), None)
    waiting = {"ready": False, "undetermined": False, "curve": []}
    if four is None or sixteen is None:
        return waiting
    c4 = four.get("held")
    c16 = sixteen.get("held")
    if c4 is None or c16 is None or c4 <= 0 or c16 <= 0:
        return {"ready": False, "undetermined": True, "curve": []}
    solved = recover([(4, c4), (NINE_SERVERS, NINE_PLAYERS), (16, c16)])
    if not solved:
        return {"ready": False, "undetermined": True, "curve": []}
    c1, sigma, kappa = solved
    checks = ((4, float(c4)), (float(NINE_SERVERS), float(NINE_PLAYERS)), (16, float(c16)))
    for servers, players in checks:
        predicted = usl(servers, c1, sigma, kappa)
        if predicted is None or abs(predicted - players) > max(1e-4, abs(players) * 1e-6):
            return {"ready": False, "undetermined": True, "curve": []}
    curve = []
    step = 1.0
    servers = 1.0
    while servers <= 18.0001:
        curve.append((servers, usl(servers, c1, sigma, kappa)))
        servers += step
    return {
        "ready": True,
        "undetermined": False,
        "c1": c1,
        "sigma": sigma,
        "kappa": kappa,
        "c1_text": format_param(c1),
        "sigma_text": format_param(sigma),
        "kappa_text": format_param(kappa),
        "four_held": whole(c4),
        "sixteen_held": whole(c16),
        "curve": curve,
    }


def climb_state(name, servers, color, rows, error, stamp, filename):
    return {
        "name": name,
        "servers": servers,
        "color": color,
        "rows": rows,
        "error": error,
        "stamp": stamp,
        "filename": filename,
        "chart": chart_point(rows, servers, color, name),
    }


def build():
    four_rows, four_err, four_stamp = load_csv(FOUR_CSV, FOUR_CSV.name)
    sixteen_rows, sixteen_err, sixteen_stamp = load_csv(SIXTEEN_CSV, SIXTEEN_CSV.name)
    nine_rows, nine_err, nine_stamp = load_csv(NINE_CSV, NINE_CSV.name)
    mark_bend(four_rows)
    mark_bend(sixteen_rows)
    mark_bend(nine_rows)
    return {
        "fit": try_fit(four_rows, sixteen_rows),
        "four": climb_state("Four-server climb", 4, "#1d4ed8", four_rows, four_err, four_stamp, FOUR_CSV.name),
        "sixteen": climb_state("16-server climb", 16, "#c2410c", sixteen_rows, sixteen_err, sixteen_stamp, SIXTEEN_CSV.name),
        "nine": climb_state("Nine-server climb", 9, "#0f766e", nine_rows, nine_err, nine_stamp, NINE_CSV.name),
        "read_at": datetime.now().isoformat(timespec="seconds"),
    }


def result_text(row: dict) -> str:
    text = row["result"] or "—"
    if row["is_bend"]:
        text = f"{text}, bend"
    elif row["binding"] == "loadgen":
        text = f"{text}, loadgen"
    return text


def cpu_text(row: dict) -> str:
    if not row["cpu_text"]:
        return "—"
    if row["busiest"]:
        return f"{row['cpu_text']}% {row['busiest']}"
    return f"{row['cpu_text']}%"


def summary_text(climb: dict) -> str:
    if climb["error"]:
        return climb["error"]
    rows = climb["rows"]
    bend = next((row for row in rows if row["is_bend"]), None)
    if bend is not None:
        return (
            f"Bend row: held {whole(bend['held'])} of target {whole(bend['target'])}, "
            f"{per_server(bend['held'], climb['servers'])} per server, "
            f"busiest CPU {bend['cpu_text'] or '—'}%, load {bend['load_text'] or '—'}."
        )
    if not rows:
        return "No hold yet."
    if rows[-1]["binding"] == "loadgen":
        return "No bend row yet. The latest row is a load-generator miss and is not this mesh's cap."
    return "No bend row yet."


def source_text(climb: dict) -> str:
    if climb["error"]:
        return ""
    stamp = f", file time {climb['stamp']}" if climb["stamp"] else ""
    return f"Source {climb['filename']}{stamp}, {len(climb['rows'])} rows."


def fit_html(fit: dict) -> str:
    if fit["ready"]:
        return (
            '<p id="fit" class="status">C(1) = '
            f'{esc(fit["c1_text"])}, sigma = {esc(fit["sigma_text"])}, kappa = {esc(fit["kappa_text"])}. '
            f'Fitted through N=4 at {esc(fit["four_held"])} players and N=16 at {esc(fit["sixteen_held"])} players on this jar, '
            "and N=9 at 513 players. The nine-server point is a different build.</p>"
        )
    if fit.get("undetermined"):
        return (
            '<p id="fit" class="status wait">Both climbs have a bend row, and those points did not determine '
            "C(1), sigma, and kappa.</p>"
        )
    return '<p id="fit" class="status wait">The fit is waiting.</p>'


def table_html(rows: list[dict]) -> str:
    head = (
        "<thead><tr>"
        '<th class="num">Target</th><th class="num">Held</th><th class="num">Busiest CPU</th>'
        '<th class="num">Load</th><th class="num">Handoffs</th><th class="num">Moved wrongly</th>'
        '<th class="num">Still moving</th><th>Result</th>'
        "</tr></thead>"
    )
    if not rows:
        body = '<tr><td colspan="8">No hold yet.</td></tr>'
    else:
        body_rows = []
        for row in rows:
            kind = "bend" if row["is_bend"] else ("fail" if row["result"] != "clean" else "")
            attr = f' class="{kind}"' if kind else ""
            cells = (
                ("num", whole(row["target"])),
                ("num", whole(row["held"])),
                ("num", cpu_text(row)),
                ("num", row["load_text"] or "—"),
                ("num", whole(row["handoffs"])),
                ("num", whole(row["moved_wrongly"])),
                ("num", row["moving"] or "—"),
                ("", result_text(row)),
            )
            rendered = "".join(
                f'<td class="{css}">{esc(text)}</td>' if css else f"<td>{esc(text)}</td>"
                for css, text in cells
            )
            body_rows.append(f"<tr{attr}>{rendered}</tr>")
        body = "".join(body_rows)
    return f'<div class="scroll"><table>{head}<tbody>{body}</tbody></table></div>'


def climb_section(climb: dict) -> str:
    return (
        "<section>"
        f"<h2>{esc(climb['name'])}</h2>"
        f"<p>{esc(summary_text(climb))}</p>"
        f"{table_html(climb['rows'])}"
        f'<p class="note">{esc(source_text(climb))}</p>'
        "</section>"
    )


def nice_step(span: float, count: int) -> float:
    if not math.isfinite(span) or span <= 0:
        return 1.0
    step0 = span / max(count, 1)
    mag = 10 ** math.floor(math.log10(step0))
    err = step0 / mag
    if err >= 7.5:
        return 10 * mag
    if err >= 3.5:
        return 5 * mag
    if err >= 1.5:
        return 2 * mag
    return mag


def axis_ticks(low: float, high: float, count: int):
    step = nice_step(high - low, count)
    if step <= 0:
        return []
    start = math.ceil((low - step * 1e-9) / step) * step
    ticks = []
    for index in range(24):
        value = start + index * step
        if value > high + step * 1e-6:
            break
        if value >= low - step * 1e-6:
            ticks.append((value, step))
    return ticks


def format_tick(value: float, step: float) -> str:
    if step >= 1:
        return str(int(round(value)))
    if step >= 0.1:
        return f"{value:.1f}"
    return f"{value:.2f}"


def chart_svg(points, curve, xmin, xmax, ymin, ymax, x_label, y_label, width=880, height=320):
    margin = {"l": 58, "r": 18, "t": 18, "b": 42}
    inner_w = width - margin["l"] - margin["r"]
    inner_h = height - margin["t"] - margin["b"]
    xspan = xmax - xmin or 1
    yspan = ymax - ymin or 1

    def x_of(value):
        return margin["l"] + (value - xmin) / xspan * inner_w

    def y_of(value):
        return margin["t"] + (1 - (value - ymin) / yspan) * inner_h

    parts = [
        f'<svg viewBox="0 0 {width} {height}" role="img" aria-label="{esc(y_label)} against {esc(x_label)}">'
    ]
    for value, step in axis_ticks(ymin, ymax, 4):
        y_pos = y_of(value)
        parts.append(
            f'<line x1="{margin["l"]}" y1="{y_pos:.1f}" x2="{margin["l"] + inner_w}" y2="{y_pos:.1f}" stroke="#e7e5e4"/>'
        )
        parts.append(
            f'<text x="{margin["l"] - 8}" y="{y_pos + 4:.1f}" text-anchor="end" font-size="12" fill="#57534e">{esc(format_tick(value, step))}</text>'
        )
    for value, step in axis_ticks(xmin, xmax, 6):
        x_pos = x_of(value)
        if x_pos < margin["l"] - 0.5 or x_pos > margin["l"] + inner_w + 0.5:
            continue
        parts.append(
            f'<text x="{x_pos:.1f}" y="{margin["t"] + inner_h + 18}" text-anchor="middle" font-size="12" fill="#57534e">{esc(format_tick(value, step))}</text>'
        )
    if curve:
        pen = False
        path = []
        for servers, players in curve:
            if players is None or players < ymin or players > ymax:
                pen = False
                continue
            command = "L" if pen else "M"
            path.append(f"{command}{x_of(servers):.1f},{y_of(players):.1f}")
            pen = True
        if path:
            parts.append(
                f'<path d="{" ".join(path)}" fill="none" stroke="#44403c" stroke-width="1.5" stroke-dasharray="5 4"/>'
            )
    for point in points:
        x_pos = x_of(point["x"])
        y_pos = y_of(point["y"])
        title = esc(point["title"])
        if point["shape"] == "diamond":
            radius = 7
            parts.append(
                f'<polygon points="{x_pos:.1f},{y_pos - radius:.1f} {x_pos + radius:.1f},{y_pos:.1f} '
                f'{x_pos:.1f},{y_pos + radius:.1f} {x_pos - radius:.1f},{y_pos:.1f}" fill="{point["fill"]}">'
                f"<title>{title}</title></polygon>"
            )
        elif point["shape"] == "open":
            parts.append(
                f'<circle cx="{x_pos:.1f}" cy="{y_pos:.1f}" r="5.5" fill="#fff" stroke="{point["fill"]}" stroke-width="2">'
                f"<title>{title}</title></circle>"
            )
        else:
            parts.append(
                f'<circle cx="{x_pos:.1f}" cy="{y_pos:.1f}" r="5.5" fill="{point["fill"]}">'
                f"<title>{title}</title></circle>"
            )
        anchor = "middle"
        label_x = x_pos
        if point["x"] >= 15:
            anchor = "end"
            label_x = x_pos - 8
        elif point["x"] <= 2:
            anchor = "start"
            label_x = x_pos + 8
        label_y = y_pos - 12
        if label_y < margin["t"] + 10:
            label_y = y_pos + 18
        parts.append(
            f'<text x="{label_x:.1f}" y="{label_y:.1f}" text-anchor="{anchor}" font-size="12" fill="#1c1917">{esc(point["label"])}</text>'
        )
    mid = margin["t"] + inner_h / 2
    parts.append(
        f'<text x="{margin["l"] + inner_w / 2:.1f}" y="{height - 6}" text-anchor="middle" font-size="12" fill="#57534e">{esc(x_label)}</text>'
    )
    parts.append(
        f'<text x="14" y="{mid:.1f}" transform="rotate(-90 14 {mid:.1f})" text-anchor="middle" font-size="12" fill="#57534e">{esc(y_label)}</text>'
    )
    parts.append("</svg>")
    return "".join(parts)


def answered_active(row):
    """Connected count from one harness status. A miss is None, never a made-up zero."""
    if not isinstance(row, dict) or row.get("error"):
        return None
    active = row.get("active")
    if isinstance(active, bool) or not isinstance(active, int):
        return None
    return active


def sum_answered_active(rows, names):
    total = 0
    answered = 0
    for name in names:
        active = answered_active(rows.get(name) if isinstance(rows, dict) else None)
        if active is None:
            continue
        total += active
        answered += 1
    if answered == 0:
        return None, 0
    return total, answered


def refresh_sixteen_population():
    """Sum active on test-mc-p5-lg-1..18. Skip a generator that does not answer."""
    mesh = climb_mesh.sixteen_mesh()
    found, _missing = climb_mesh.resolve(SIXTEEN_LOADS)
    loads = [(name, found[name]) for name in SIXTEEN_LOADS if name in found]
    rows = {}
    if loads:
        def read_one(item):
            return climb_mesh.api(mesh, item[1], "GET", "/v1/status", timeout=20)

        rows = climb_mesh.fan(loads, read_one, workers=len(loads))
    total, answered = sum_answered_active(rows, SIXTEEN_LOADS)
    with _pop_lock:
        _pop["value"] = total
        _pop["answered"] = answered
        _pop["sought"] = len(SIXTEEN_LOADS)


def four_host_map():
    """Inventory plus the four-server fleet file, so a new f4 generator is visible."""
    found = climb_mesh.inventory()
    if F4_FLEET is not None and F4_FLEET.exists():
        try:
            climb_mesh.walk_hosts(json.loads(F4_FLEET.read_text()), found)
        except json.JSONDecodeError:
            pass
    return found


def four_load_names(found):
    """Four-server generators only: lg-1..5, loadgen-1, and f4-lg hosts that exist."""
    names = [name for name in FOUR_FIXED if name in found]
    extras = []
    for name in found:
        if not name.startswith("test-mc-f4-lg-"):
            continue
        suffix = name.rsplit("-", 1)[-1]
        if suffix.isdigit():
            extras.append(name)
    extras.sort(key=lambda item: int(item.rsplit("-", 1)[-1]))
    return tuple(names + extras)


def refresh_four_population():
    """Sum harness active on the four-server generators. Skip one that does not answer."""
    mesh = climb_mesh.four_mesh()
    found = four_host_map()
    names = four_load_names(found)
    loads = [(name, found[name]) for name in names if name in found]
    rows = {}
    if loads:
        def read_one(item):
            return climb_mesh.api(mesh, item[1], "GET", "/v1/status", timeout=20)

        rows = climb_mesh.fan(loads, read_one, workers=len(loads))
    total, answered = sum_answered_active(rows, names)
    with _four_lock:
        _four["value"] = total
        _four["answered"] = answered
        _four["sought"] = len(names)


def population_snapshot():
    with _pop_lock:
        return {"value": _pop["value"], "answered": _pop["answered"], "sought": _pop["sought"]}


def four_snapshot():
    with _four_lock:
        return {"value": _four["value"], "answered": _four["answered"], "sought": _four["sought"]}


def live_count_html(element_id, label, snap) -> str:
    value = snap["value"]
    if value is None:
        body = f"{label} —"
        kind = "status wait"
    else:
        body = f"{label} {value}"
        skipped = snap["sought"] - snap["answered"]
        if skipped > 0:
            word = "generator" if skipped == 1 else "generators"
            body += f" ({skipped} {word} did not answer)"
        kind = "status"
    return f'<p id="{element_id}" class="{kind}">{esc(body)}</p>'


def sixteen_now_html() -> str:
    return live_count_html("sixteen-now", "16-server now", population_snapshot())


def four_now_html() -> str:
    return live_count_html("four-now", "4-server now", four_snapshot())


def capacity_section(state: dict) -> str:
    fit = state["fit"]
    points = []
    player_values = [float(NINE_PLAYERS)]
    for climb in (state["four"], state["sixteen"]):
        chart = climb["chart"]
        if not chart:
            continue
        player_values.append(chart["y"])
        kind = chart["kind"]
        shape = "filled" if kind == "bend" else "open"
        per = per_server(chart["y"], chart["x"])
        word = "bend" if kind == "bend" else "latest hold"
        points.append({
            "x": chart["x"],
            "y": chart["y"],
            "shape": shape,
            "fill": chart["color"],
            "label": f"{whole(chart['y'])} {word}",
            "title": f"{chart['name']}, {whole(chart['y'])} players, {per} per server, {word}, this jar",
        })
    points.append({
        "x": NINE_SERVERS,
        "y": float(NINE_PLAYERS),
        "shape": "diamond",
        "fill": "#6d28d9",
        "label": "513 previous jar",
        "title": "Nine-server hold of 513 players, about 57 per server, previous jar. The nine-server point is a different build.",
    })
    curve = fit["curve"] if fit["ready"] else []
    curve_values = [players for _servers, players in curve if players is not None and 0 < players <= max(player_values) * 3]
    top = max(player_values + curve_values)
    ymax = max(top * 1.22, 100)
    svg = chart_svg(points, curve, 0, 18, 0, ymax, "Servers", "Players")
    legend = [
        '<li><i style="background:#1d4ed8"></i>Four-server climb, this jar</li>',
        '<li><i style="background:#c2410c"></i>16-server climb, this jar</li>',
        '<li><i class="diamond" style="background:#6d28d9"></i>Nine-server hold, 513, previous jar</li>',
    ]
    if fit["ready"]:
        legend.append("<li><i style=\"background:#44403c;border-radius:0;height:2px;vertical-align:middle\"></i>Universal Scalability Law</li>")
    note = (
        "An open circle is the latest hold and is not in the fit. A filled circle is that climb's bend row. "
        "The diamond is the nine-server hold of 513, about 57 per server. The nine-server point is a different build."
    )
    return (
        "<section>"
        "<h2>Players against server count</h2>"
        '<div class="nows">'
        f"{four_now_html()}"
        f"{sixteen_now_html()}"
        "</div>"
        f"{svg}"
        f'<ul class="legend">{"".join(legend)}</ul>'
        f'<p class="note">{esc(note)}</p>'
        "</section>"
    )


def seam_section() -> str:
    points = []
    for servers, players, label in SEAM_HOLDS:
        points.append({
            "x": servers,
            "y": players,
            "shape": "filled",
            "fill": "#0f766e",
            "label": str(players),
            "title": f"{servers} b3-8s held {players} ({label})",
        })
    svg = chart_svg(points, [], 1, 5, 0, 120, "Servers", "Players", height=280)
    return (
        "<section>"
        "<h2>Seam laps, 18 November</h2>"
        "<p>Two b3-8s held 44 (22 per server), three held 80 (about 27 per server), and four held 90 "
        "(22.5 per server). This chart is not part of the fit.</p>"
        f"{svg}"
        "</section>"
    )


def nine_section(climb: dict) -> str:
    extra = ""
    if not any(row["is_bend"] for row in climb["rows"]):
        extra = "<p>The climb is in the fast ramp.</p>"
    link = '<p><a href="/map">Open the nine-server map</a></p>'
    return climb_section(climb).replace("</h2>", "</h2>" + extra + link, 1)


def render_live(state: dict) -> str:
    return "\n".join([
        fit_html(state["fit"]),
        nine_section(state["nine"]),
        climb_section(state["four"]),
        climb_section(state["sixteen"]),
        capacity_section(state),
        seam_section(),
        f'<p class="foot">Read the climb files at {esc(state["read_at"])}. Bound to 127.0.0.1:8768.</p>',
    ])


def render_document(state: dict) -> str:
    page = DOCUMENT.replace("COMMIT_TOKEN", COMMIT).replace("SHA_TOKEN", JAR_SHA)
    return page.replace("<!--LIVE-->", render_live(state), 1)


def watch():
    first = True
    while True:
        try:
            build()
        except Exception:
            sys.stderr.write("climb file read failed\n")
        try:
            refresh_sixteen_population()
        except Exception:
            sys.stderr.write("16-server count unavailable\n")
            with _pop_lock:
                _pop["value"] = None
                _pop["answered"] = 0
        try:
            refresh_four_population()
        except Exception:
            sys.stderr.write("4-server count unavailable\n")
            with _four_lock:
                _four["value"] = None
                _four["answered"] = 0
        if first:
            _pop_ready.set()
            first = False
        time.sleep(2)


def already_serving() -> bool:
    try:
        with urllib.request.urlopen("http://127.0.0.1:8768/", timeout=1.5) as response:
            body = response.read(300000).decode("utf-8", "replace")
            status = response.status
    except (OSError, urllib.error.URLError, TimeoutError, ValueError):
        return False
    return (
        status == 200
        and 'data-page="mesh-climbs"' in body
        and "Four-server climb" in body
        and "16-server climb" in body
        and "seam-scaling-four-2026-11-19.csv" in body
        and "seam-scaling-sixteen-2026-11-19.csv" in body
    )


# Nine-server mesh. p1 is the southwest cell. Cuts are at 2000 and 4000.
# (side, host, x0, x1, z0, z1). The high edge is exclusive.
CELLS = (
    ("p1", "test-mc-p5-1", 0, 2000, 0, 2000),
    ("p2", "test-mc-p5-2", 2000, 4000, 0, 2000),
    ("p3", "test-mc-p5-3", 4000, 6000, 0, 2000),
    ("p4", "test-mc-p5-4", 0, 2000, 2000, 4000),
    ("p5", "test-mc-p5-5", 2000, 4000, 2000, 4000),
    ("p6", "test-mc-p5-6", 4000, 6000, 2000, 4000),
    ("p7", "test-mc-p5-7", 0, 2000, 4000, 6000),
    ("p8", "test-mc-p5-8", 2000, 4000, 4000, 6000),
    ("p9", "test-mc-p5-9", 4000, 6000, 4000, 6000),
)
WORLD = 6000
COLUMNS_PATH = Path.home() / ".local/state/cks-test-mc-p5/nine-climb/section-columns.json"
_columns_lock = threading.Lock()
_columns = {"mtime": None, "table": {}}
IP_RE = re.compile(r"\b(?:\d{1,3}\.){3}\d{1,3}\b")


def column_table() -> dict:
    """Ground heights from the nine-climb column table. Key is 'x,z'."""
    path = COLUMNS_PATH
    try:
        mtime = path.stat().st_mtime
    except OSError:
        return {}
    with _columns_lock:
        if _columns["mtime"] == mtime:
            return _columns["table"]
        try:
            raw = json.loads(path.read_text())
        except (OSError, json.JSONDecodeError):
            return _columns["table"]
        table = {}
        if isinstance(raw, dict):
            for key, value in raw.items():
                if isinstance(key, str) and isinstance(value, int) and not isinstance(value, bool):
                    table[key] = value
        _columns["mtime"] = mtime
        _columns["table"] = table
        return table


def column_state(table: dict, x: int, z: int) -> dict:
    """A positive table value is ground. 0 is open water. A missing key is unmeasured."""
    block_x = int(x)
    block_z = int(z)
    key = f"{block_x},{block_z}"
    if key not in table:
        return {"x": block_x, "z": block_z, "state": "unmeasured"}
    value = table[key]
    if isinstance(value, int) and not isinstance(value, bool) and value > 0:
        return {"x": block_x, "z": block_z, "state": "ground", "y": value}
    return {"x": block_x, "z": block_z, "state": "water"}


def redact_public(text: str) -> str:
    def repl(match):
        if match.group(0).startswith("127."):
            return match.group(0)
        return "<ip>"

    return IP_RE.sub(repl, text)


NAME_RE = re.compile(r"^[A-Za-z0-9_]{2,16}$")
BOT_NUMBER_RE = re.compile(r"^S\d+$")
_map_lock = threading.Lock()
_map_ready = threading.Event()
_map = {"updated": None, "players": [], "failed": []}
_nine_lock = threading.Lock()
_nine_ready = threading.Event()
_nine = {"value": None, "answered": 0, "sought": 9}

REMOTE_SCRIPT = r'''
import gzip, json, re, socket, struct
from pathlib import Path

SIDE = "SIDE_TOKEN"
ROOT = Path("/opt/mc") / SIDE
IP_RE = re.compile(r"\b(?:\d{1,3}\.){3}\d{1,3}\b")
LOGIN_RE = re.compile(
    r"\]: ([A-Za-z0-9_]{2,16})\[/<ip>:[0-9]+\] logged in with entity id \d+ at "
    r"\(([-\d.]+), ([-\d.]+), ([-\d.]+)\)"
)
LOST_RE = re.compile(r"\]: ([A-Za-z0-9_]{2,16}) lost connection")

def is_bot(name):
    return name.startswith("Load") or re.fullmatch(r"S\d+", name or "") is not None

def online_from_log(text):
    text = IP_RE.sub("<ip>", text)
    online = {}
    for line in text.splitlines():
        match = LOGIN_RE.search(line)
        if match:
            online[match.group(1)] = [float(match.group(2)), float(match.group(3)), float(match.group(4))]
            continue
        lost = LOST_RE.search(line)
        if lost:
            online.pop(lost.group(1), None)
    return online

def open_nbt(path):
    raw = path.read_bytes()
    if raw[:2] == b"\x1f\x8b":
        return gzip.decompress(raw)
    return raw

def read_pos(path):
    data = open_nbt(path)
    pos = [0]
    def u8():
        value = data[pos[0]]
        pos[0] += 1
        return value
    def u16():
        value = struct.unpack_from(">H", data, pos[0])[0]
        pos[0] += 2
        return value
    def i32():
        value = struct.unpack_from(">i", data, pos[0])[0]
        pos[0] += 4
        return value
    def read_string():
        size = u16()
        text = data[pos[0]:pos[0] + size].decode("utf-8", "replace")
        pos[0] += size
        return text
    def skip(tag):
        if tag == 1:
            pos[0] += 1
        elif tag == 2:
            pos[0] += 2
        elif tag == 3:
            pos[0] += 4
        elif tag == 4:
            pos[0] += 8
        elif tag == 5:
            pos[0] += 4
        elif tag == 6:
            pos[0] += 8
        elif tag == 7:
            pos[0] += i32()
        elif tag == 8:
            pos[0] += u16()
        elif tag == 9:
            inner = u8()
            count = i32()
            for _ in range(count):
                skip(inner)
        elif tag == 10:
            while True:
                inner = u8()
                if inner == 0:
                    break
                read_string()
                skip(inner)
        elif tag == 11:
            pos[0] += 4 * i32()
        elif tag == 12:
            pos[0] += 8 * i32()
        else:
            raise ValueError("tag")
    if u8() != 10:
        raise ValueError("root")
    read_string()
    while True:
        tag = u8()
        if tag == 0:
            break
        name = read_string()
        if name == "Pos" and tag == 9:
            inner = u8()
            count = i32()
            if inner == 6 and count == 3:
                values = [struct.unpack_from(">d", data, pos[0] + 8 * index)[0] for index in range(3)]
                return values
            for _ in range(count):
                skip(inner)
        else:
            skip(tag)
    return None

def packet(request_id, kind, payload):
    body = struct.pack("<ii", request_id, kind) + payload.encode() + b"\x00\x00"
    return struct.pack("<i", len(body)) + body

def recvall(sock, size):
    buf = b""
    while len(buf) < size:
        chunk = sock.recv(size - len(buf))
        if not chunk:
            raise EOFError
        buf += chunk
    return buf

def recv_packet(sock):
    size = struct.unpack("<i", recvall(sock, 4))[0]
    buf = recvall(sock, size)
    request_id, kind = struct.unpack("<ii", buf[:8])
    payload = buf[8:-2].decode("utf-8", "replace")
    return request_id, kind, payload

class LivePos:
    def __init__(self, secret, port):
        self._secret = secret
        self._port = port
        self._sock = None

    def connect(self):
        sock = socket.create_connection(("127.0.0.1", self._port), timeout=3)
        sock.settimeout(2)
        sock.sendall(packet(1, 3, self._secret))
        request_id, _kind, _payload = recv_packet(sock)
        if request_id != 1:
            sock.close()
            return False
        self._sock = sock
        return True

    def pos(self, name):
        if self._sock is None:
            return None
        self._sock.sendall(packet(2, 2, "data get entity " + name + " Pos"))
        _request_id, _kind, payload = recv_packet(self._sock)
        match = re.search(
            r"\[(-?\d+(?:\.\d+)?)d,\s*(-?\d+(?:\.\d+)?)d,\s*(-?\d+(?:\.\d+)?)d\]",
            payload,
        )
        if not match:
            return None
        return [float(match.group(1)), float(match.group(2)), float(match.group(3))]

    def close(self):
        if self._sock is not None:
            self._sock.close()
            self._sock = None

def live_reader(root):
    props = {}
    path = root / "server.properties"
    if not path.exists():
        return None
    for line in path.read_text(errors="replace").splitlines():
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        props[key] = value
    if props.get("enable-rcon", "").strip().lower() != "true":
        return None
    secret = props.get("rcon.password") or ""
    if not secret:
        return None
    try:
        port = int(props.get("rcon.port") or "25575")
    except ValueError:
        return None
    reader = LivePos(secret, port)
    try:
        if not reader.connect():
            return None
    except OSError:
        return None
    return reader

def main():
    players = []
    error = None
    reader = None
    try:
        if SIDE not in {"p1", "p2", "p3", "p4", "p5", "p6", "p7", "p8", "p9"}:
            raise RuntimeError("side")
        log_path = ROOT / "logs" / "latest.log"
        text = log_path.read_text(errors="replace") if log_path.exists() else ""
        online = online_from_log(text)
        by_name = {}
        cache_path = ROOT / "usercache.json"
        if cache_path.exists():
            for item in json.loads(cache_path.read_text()):
                name = item.get("name")
                uid = item.get("uuid")
                if isinstance(name, str) and isinstance(uid, str):
                    by_name[name] = uid
        saved = ROOT / "Crowded Kingdoms" / "players" / "data"
        real_left = 8
        for name, login in online.items():
            pos = None
            uid = by_name.get(name)
            if uid:
                path = saved / (uid + ".dat")
                if path.exists():
                    try:
                        pos = read_pos(path)
                    except Exception:
                        pos = None
            if not pos:
                pos = login
            live = False
            if not is_bot(name) and real_left > 0:
                if reader is None:
                    reader = live_reader(ROOT)
                if reader is not None:
                    real_left -= 1
                    try:
                        got = reader.pos(name)
                    except OSError:
                        got = None
                    if got:
                        pos = got
                        live = True
            players.append({"name": name, "x": pos[0], "y": pos[1], "z": pos[2], "live": live})
    except Exception:
        error = "read_failed"
    finally:
        if reader is not None:
            reader.close()
    raw = json.dumps({"side": SIDE, "players": players, "error": error}, separators=(",", ":"))
    print(IP_RE.sub("<ip>", raw))

main()
'''


def is_bot_name(name: str) -> bool:
    return name.startswith("Load") or BOT_NUMBER_RE.fullmatch(name) is not None


def cell_of(x: float, z: float):
    if not math.isfinite(x) or not math.isfinite(z):
        return None
    if x < 0 or z < 0 or x >= WORLD or z >= WORLD:
        return None
    column = 0 if x < 2000 else 1 if x < 4000 else 2
    row = 0 if z < 2000 else 1 if z < 4000 else 2
    return f"p{row * 3 + column + 1}"


def coord_text(value: float) -> str:
    if abs(value - round(value)) < 0.05:
        return str(int(round(value)))
    return f"{value:.1f}"


def game_hosts() -> dict:
    path = Path.home() / ".local/state/cks-test-mc-p5/vms.json"
    found = {}
    if not path.exists():
        return found
    try:
        climb_mesh.walk_hosts(json.loads(path.read_text()), found)
    except (OSError, json.JSONDecodeError):
        return {}
    return found


def ssh_text(ip: str, script: str, timeout: int = 22) -> str:
    key = Path.home() / ".local/state/cks-test-mc-cluster/id_ed25519"
    known = Path.home() / ".local/state/cks-test-mc-p5/known_hosts"
    proc = subprocess.run(
        [
            "ssh",
            "-i",
            str(key),
            "-o",
            "IdentitiesOnly=yes",
            "-o",
            "BatchMode=yes",
            "-o",
            f"UserKnownHostsFile={known}",
            "-o",
            "StrictHostKeyChecking=yes",
            "-o",
            "ConnectTimeout=12",
            "-o",
            "LogLevel=ERROR",
            f"ubuntu@{ip}",
            "sudo",
            "python3",
            "-",
        ],
        input=script.encode(),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout,
    )
    text = IP_RE.sub("<ip>", proc.stdout.decode("utf-8", "replace"))
    if proc.returncode != 0:
        raise RuntimeError(f"ssh exit {proc.returncode}")
    return text


def nine_load_names() -> tuple:
    raw = os.environ.get("CK_NINE_LOADS", "")
    names = tuple(part.strip() for part in raw.split(",") if part.strip())
    if names:
        return names
    return tuple(f"test-mc-p5-lg-{i}" for i in range(10, 19))


def nine_token() -> str:
    raw = os.environ.get("CK_NINE_TOKEN_FILE", "").strip()
    path = Path(raw).expanduser() if raw else Path.home() / ".local/state/cks-test-mc-p5/control-token"
    return path.read_text().strip()


def nine_ssh_key() -> Path:
    raw = os.environ.get("CK_SSH_KEY", "").strip()
    if raw:
        return Path(raw).expanduser()
    return Path.home() / ".local/state/cks-test-mc-cluster/id_ed25519"


def nine_known_hosts() -> Path:
    raw = os.environ.get("CK_KNOWN_HOSTS", "").strip()
    if raw:
        return Path(raw).expanduser()
    return Path.home() / ".local/state/cks-test-mc-p5/nine-climb/known_hosts"


def generator_active(ip: str, token: str) -> int:
    """Live seated bots for run nine. Returns the integer only."""
    script = r'''
import json, os, urllib.request
tok = os.environ["CK_TOKEN"]
req = urllib.request.Request(
    "http://127.0.0.1:9109/v1/status?run_id=nine",
    headers={"Authorization": "Bearer " + tok},
)
with urllib.request.urlopen(req, timeout=20) as resp:
    data = json.loads(resp.read().decode())
if data.get("run_id") != "nine":
    raise SystemExit(2)
active = data.get("active")
if isinstance(active, bool) or not isinstance(active, int):
    raise SystemExit(3)
print(active)
'''
    proc = subprocess.run(
        [
            "ssh",
            "-i",
            str(nine_ssh_key()),
            "-o",
            "IdentitiesOnly=yes",
            "-o",
            "BatchMode=yes",
            "-o",
            f"UserKnownHostsFile={nine_known_hosts()}",
            "-o",
            "StrictHostKeyChecking=yes",
            "-o",
            "ConnectTimeout=12",
            "-o",
            "LogLevel=ERROR",
            f"ubuntu@{ip}",
            "read -r CK_TOKEN && export CK_TOKEN && python3 -",
        ],
        input=(token + "\n" + script).encode(),
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        timeout=25,
    )
    if proc.returncode != 0:
        raise RuntimeError(f"ssh exit {proc.returncode}")
    line = ""
    for row in proc.stdout.decode("utf-8", "replace").splitlines():
        row = row.strip()
        if row.lstrip("-").isdigit():
            line = row
    if not line:
        raise RuntimeError("no count")
    return int(line)


def refresh_nine_seated():
    """Sum active on the nine generators. A partial answer does not replace a full sum."""
    names = nine_load_names()
    hosts = game_hosts()
    token = nine_token()
    loads = [(name, hosts[name]) for name in names if name in hosts]

    def read_one(item):
        try:
            return generator_active(item[1], token)
        except Exception:
            return None

    rows = {}
    if loads:
        with ThreadPoolExecutor(max_workers=len(loads)) as pool:
            for item, active in zip(loads, pool.map(read_one, loads)):
                rows[item[0]] = active
    answered = sum(1 for name in names if isinstance(rows.get(name), int) and not isinstance(rows.get(name), bool))
    with _nine_lock:
        _nine["sought"] = len(names)
        _nine["answered"] = answered
        if names and answered == len(names):
            _nine["value"] = sum(rows[name] for name in names)


def nine_seated_total():
    """Last complete generator sum. A later partial poll does not wipe it."""
    with _nine_lock:
        value = _nine["value"]
    if isinstance(value, int) and not isinstance(value, bool):
        return value
    return None


def nine_watch():
    while True:
        started = time.time()
        try:
            refresh_nine_seated()
        except Exception as exc:
            sys.stderr.write(f"nine seated count unavailable: {type(exc).__name__}\n")
        finally:
            if nine_seated_total() is not None:
                _nine_ready.set()
        time.sleep(max(1.0, 15.0 - (time.time() - started)))


def parse_side(text: str, side: str):
    line = ""
    for row in text.splitlines():
        row = row.strip()
        if row.startswith("{") and row.endswith("}"):
            line = row
    if not line:
        raise RuntimeError("no reading")
    data = json.loads(line)
    if data.get("side") != side:
        raise RuntimeError("wrong side")
    players = []
    for item in data.get("players") or []:
        if not isinstance(item, dict):
            continue
        name = item.get("name")
        if not isinstance(name, str) or NAME_RE.fullmatch(name) is None:
            continue
        try:
            x = float(item["x"])
            y = float(item["y"])
            z = float(item["z"])
        except (TypeError, ValueError, KeyError):
            continue
        if not all(math.isfinite(value) for value in (x, y, z)):
            continue
        players.append({
            "name": name,
            "x": x,
            "y": y,
            "z": z,
            "live": bool(item.get("live")),
            "side": side,
        })
    error = data.get("error")
    if error:
        error = "read_failed"
    return players, error


def prefer_player(current: dict, incoming: dict) -> dict:
    if current["live"] != incoming["live"]:
        return current if current["live"] else incoming
    current_home = cell_of(current["x"], current["z"]) == current["side"]
    incoming_home = cell_of(incoming["x"], incoming["z"]) == incoming["side"]
    if current_home != incoming_home:
        return current if current_home else incoming
    return incoming


def merge_players(players: list[dict]) -> list[dict]:
    best = {}
    for player in players:
        previous = best.get(player["name"])
        best[player["name"]] = player if previous is None else prefer_player(previous, player)
    return list(best.values())


def positions_from_players(players: list[dict], failed: list[str], updated=None) -> dict:
    merged = merge_players(players)
    counts = {side: {"bots": 0, "real": []} for side, *_rest in CELLS}
    dots = []
    real = []
    outside = 0
    for player in merged:
        cell = cell_of(player["x"], player["z"])
        if is_bot_name(player["name"]):
            if cell is None:
                outside += 1
            else:
                counts[cell]["bots"] += 1
            dots.append({"x": player["x"], "z": player["z"], "cell": cell})
            continue
        entry = {
            "name": player["name"],
            "x": player["x"],
            "y": player["y"],
            "z": player["z"],
            "cell": cell,
            "live": bool(player["live"]),
            "side": player["side"],
        }
        real.append(entry)
        if cell is None:
            outside += 1
        else:
            counts[cell]["real"].append(player["name"])
    cells = []
    for side, host, x0, x1, z0, z1 in CELLS:
        cells.append({
            "id": side,
            "host": host,
            "x0": x0,
            "x1": x1,
            "z0": z0,
            "z1": z1,
            "bots": counts[side]["bots"],
            "real": counts[side]["real"],
            "ok": side not in failed,
            "label": "p1 southwest" if side == "p1" else ("p5 center" if side == "p5" else side),
        })
    real.sort(key=lambda item: item["name"])
    return {
        "updated": updated,
        "bots": sum(1 for player in merged if is_bot_name(player["name"])),
        "outside": outside,
        "real": real,
        "dots": dots,
        "cells": cells,
        "failed": list(failed),
    }


def positions_payload() -> dict:
    with _map_lock:
        snapshot = {
            "updated": _map["updated"],
            "players": list(_map["players"]),
            "failed": list(_map["failed"]),
        }
    payload = positions_from_players(snapshot["players"], snapshot["failed"], snapshot["updated"])
    raw = json.dumps(payload)
    if IP_RE.search(raw):
        payload = json.loads(IP_RE.sub("<ip>", raw))
    return payload


def refresh_positions():
    hosts = game_hosts()
    script_by_side = {side: REMOTE_SCRIPT.replace("SIDE_TOKEN", side) for side, *_rest in CELLS}

    def one(cell):
        side, host = cell[0], cell[1]
        ip = hosts.get(host)
        if not ip:
            return side, [], "read_failed"
        try:
            text = ssh_text(ip, script_by_side[side])
            return (side, *parse_side(text, side))
        except Exception:
            return side, [], "read_failed"

    failed = []
    found = []
    with ThreadPoolExecutor(max_workers=9) as pool:
        for side, players, error in pool.map(one, CELLS):
            found.extend(players)
            if error and not players:
                failed.append(side)
    snapshot = {
        "updated": datetime.now().isoformat(timespec="seconds"),
        "players": merge_players(found),
        "failed": failed,
    }
    with _map_lock:
        _map.update(snapshot)


def map_watch():
    while True:
        started = time.time()
        try:
            refresh_positions()
        except Exception as exc:
            sys.stderr.write(f"position read failed: {type(exc).__name__}\n")
        finally:
            _map_ready.set()
        time.sleep(max(1.0, 8.0 - (time.time() - started)))


def map_svg(payload: dict) -> str:
    left, top, size = 52, 28, 560
    width, height = 680, 640

    def sx(x):
        return left + (float(x) / WORLD) * size

    def sz(z):
        return top + (1.0 - float(z) / WORLD) * size

    parts = [
        f'<svg viewBox="0 0 {width} {height}" role="img" aria-label="Nine-server coordinate grid" '
        f'data-plot-left="{left}" data-plot-top="{top}" data-plot-size="{size}" data-world="{WORLD}">',
        f'<rect x="{left}" y="{top}" width="{size}" height="{size}" fill="#f5f5f4" stroke="#1c1917" stroke-width="1.5"/>',
    ]
    for cell in payload["cells"]:
        x = sx(cell["x0"])
        y = sz(cell["z1"])
        w = sx(cell["x1"]) - x
        h = sz(cell["z0"]) - y
        parts.append(
            f'<g data-cell="{esc(cell["id"])}" data-host="{esc(cell["host"])}" data-label="{esc(cell["label"])}" '
            f'data-x0="{cell["x0"]}" data-x1="{cell["x1"]}" data-z0="{cell["z0"]}" data-z1="{cell["z1"]}">'
            f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" fill="#fff" stroke="#a8a29e"/>'
            f'<text x="{x + w / 2:.1f}" y="{y + 28:.1f}" text-anchor="middle" font-size="16" font-weight="650" fill="#1c1917">{esc(cell["label"])}</text>'
            f'<text x="{x + w / 2:.1f}" y="{y + 48:.1f}" text-anchor="middle" font-size="12" fill="#57534e">{esc(cell["host"])}</text>'
            f'<text x="{x + w / 2:.1f}" y="{y + h - 18:.1f}" text-anchor="middle" font-size="13" fill="#44403c">{esc(str(cell["bots"]))} bots</text>'
            + ("" if cell["ok"] else f'<text x="{x + w / 2:.1f}" y="{y + h / 2:.1f}" text-anchor="middle" font-size="13" fill="#9f1239">no reading</text>')
            + "</g>"
        )
    for cut in (2000, 4000):
        parts.append(
            f'<line data-cut="{cut}" x1="{sx(cut):.1f}" y1="{top}" x2="{sx(cut):.1f}" y2="{top + size}" stroke="#1c1917" stroke-width="1.5"/>'
        )
        parts.append(
            f'<line data-cut="{cut}" x1="{left}" y1="{sz(cut):.1f}" x2="{left + size}" y2="{sz(cut):.1f}" stroke="#1c1917" stroke-width="1.5"/>'
        )
        parts.append(
            f'<text x="{sx(cut):.1f}" y="{top - 8}" text-anchor="middle" font-size="11" fill="#57534e">{cut}</text>'
        )
        parts.append(
            f'<text x="{left - 8}" y="{sz(cut) + 4:.1f}" text-anchor="end" font-size="11" fill="#57534e">{cut}</text>'
        )
    parts.append(f'<text x="{left}" y="{height - 8}" font-size="12" fill="#57534e">Z increases up. X increases right. Southwest is p1.</text>')
    for dot in payload["dots"]:
        if dot["cell"] is None:
            continue
        parts.append(
            f'<circle data-bot="1" cx="{sx(dot["x"]):.1f}" cy="{sz(dot["z"]):.1f}" r="2.6" fill="#44403c" fill-opacity="0.85"/>'
        )
    for player in payload["real"]:
        x = min(max(player["x"], 0), WORLD)
        z = min(max(player["z"], 0), WORLD)
        px, py = sx(x), sz(z)
        kind = "live" if player["live"] else "saved"
        parts.append(
            f'<g data-player="{esc(player["name"])}">'
            f'<circle cx="{px:.1f}" cy="{py:.1f}" r="6" fill="#c2410c" stroke="#fff" stroke-width="1.5"/>'
            f'<text x="{px + 8:.1f}" y="{py - 8:.1f}" font-size="13" font-weight="650" fill="#7c2d12" stroke="#fff" stroke-width="3" paint-order="stroke">{esc(player["name"])}</text>'
            f'<title>{esc(player["name"])} {esc(kind)} {esc(coord_text(player["x"]))}, {esc(coord_text(player["y"]))}, {esc(coord_text(player["z"]))}</title>'
            "</g>"
        )
    parts.append("</svg>")
    return "".join(parts)


def map_body(payload: dict, seated=None) -> str:
    names = [player["name"] for player in payload["real"]]
    named = ", ".join(names) if names else "none online"
    if seated is None:
        seated = nine_seated_total()
    plotted = payload["bots"]
    if isinstance(seated, int) and not isinstance(seated, bool):
        lead = f"{seated} bots seated. {plotted} plotted."
    else:
        lead = f"{plotted} bots."
    rows = []
    for player in payload["real"]:
        where = player["cell"] or "outside the grid"
        kind = "live" if player["live"] else "saved"
        rows.append(
            "<li>"
            f"{esc(player['name'])} on {esc(where)} at "
            f"{esc(coord_text(player['x']))}, {esc(coord_text(player['y']))}, {esc(coord_text(player['z']))}"
            f", {kind}</li>"
        )
    if not rows:
        rows.append("<li>No real player is online.</li>")
    failed = ""
    if payload["failed"]:
        failed = f'<p class="note">No reading from {esc(", ".join(payload["failed"]))}.</p>'
    updated = payload["updated"] or "not yet"
    return (
        f'<p id="counts" class="status">{lead} Real players: {esc(named)}.</p>'
        f'<p class="note">Read {esc(updated)}. Named players are read live. Bot dots are the latest saved position. '
        "A bot name starts with Load, or is an S-numbered walker. "
        f'{payload["outside"]} outside the 0–6000 grid.</p>'
        f"{failed}"
        f'<p id="coord-readout" class="readout">Hover the map for world x and z. Click a spot to pin a teleport command.</p>'
        f'<div id="grid">{map_svg(payload)}</div>'
        "<h2>Real players</h2>"
        f'<ul>{"".join(rows)}</ul>'
    )


MAP_PAGE = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Nine-server mesh</title>
<style>
  :root { color-scheme: light; --ink:#1c1917; --muted:#57534e; --line:#e7e5e4; --paper:#f7f6f3; --card:#fff; --ok:#14532d; --okbg:#ecfdf3; }
  * { box-sizing: border-box; }
  body { margin: 0; font: 15px/1.45 system-ui, sans-serif; color: var(--ink); background: var(--paper); }
  header, main { max-width: 760px; margin: 0 auto; padding: 0 20px; }
  header { padding-top: 20px; }
  h1 { font-size: 1.45rem; margin: 0 0 8px; }
  h2 { font-size: 1.05rem; margin: 16px 0 8px; }
  a { color: #1d4ed8; }
  .sub, .note, .foot { color: var(--muted); }
  .status { margin: 0 0 12px; padding: 10px 12px; background: var(--okbg); border: 1px solid #bbf7d0; border-radius: 10px; color: var(--ok); font-weight: 650; }
  section { background: var(--card); border: 1px solid var(--line); border-radius: 10px; padding: 14px; margin: 0 0 14px; }
  svg { width: 100%; height: auto; display: block; cursor: crosshair; }
  .readout { margin: 0 0 10px; padding: 6px 10px; border: 1px solid var(--line); border-radius: 8px; background: var(--paper); color: var(--ink); font-size: 13px; font-variant-numeric: tabular-nums; white-space: pre-line; min-height: 172px; }
  .readout.frozen { border-color: #c2410c; cursor: pointer; }
  ul { margin: 0; padding-left: 18px; }
  .foot { font-size: 12.5px; padding-bottom: 28px; }
  #flow h2 { margin-top: 0; }
  #flow svg { cursor: default; }
  #flow table { width: 100%; border-collapse: collapse; font-variant-numeric: tabular-nums; }
  #flow th, #flow td { text-align: left; padding: 4px 6px; border-bottom: 1px solid var(--line); font-size: 13px; }
  #flow td.num, #flow th.num { text-align: right; white-space: nowrap; }
</style>
</head>
<body data-page="nine-map">
<header>
  <h1>Nine-server mesh</h1>
  <p class="sub">3×3 grid. Cuts at block 2000 and 4000 on X and Z. <a href="/">Climb progress</a></p>
</header>
<main>
<section id="board">
<!--BOARD-->
</section>
<section id="flow" aria-label="Generator and game server load">
<!--FLOW-->
</section>
</main>
<p class="foot">Bound to 127.0.0.1:8768. Refreshes about every 8 seconds.</p>
<script>
async function tick() {
  try {
    const res = await fetch("/map/live", {cache: "no-store"});
    if (!res.ok) throw new Error("HTTP " + res.status);
    const html = await res.text();
    const holder = document.createElement("template");
    holder.innerHTML = html;
    const mapLive = holder.content.querySelector("#map-live");
    const flowLive = holder.content.querySelector("#flow-live");
    const board = document.getElementById("board");
    if (mapLive && board) board.innerHTML = mapLive.innerHTML;
    else if (board) board.innerHTML = html;
    const flow = document.getElementById("flow");
    if (flowLive && flow) flow.innerHTML = flowLive.innerHTML;
    if (frozen) showFrozen();
    else if (pointer) paintReadout();
  } catch (err) {
    const board = document.getElementById("board");
    if (board && !board.textContent.trim()) board.textContent = "Could not read positions.";
  }
}
setInterval(tick, 6000);

const board = document.getElementById("board");
const heightCache = new Map();
let pointer = null;
let frozen = null;
let hoverSeq = 0;
let pinSeq = 0;
const lineBreak = String.fromCharCode(10);

function setReadout(text, pinned) {
  const el = document.getElementById("coord-readout");
  if (!el) return;
  el.textContent = text;
  el.classList.toggle("frozen", !!pinned);
  if (pinned) el.title = "Click to unpin";
  else el.removeAttribute("title");
}

function blockOf(value, limit) {
  let block = Math.floor(value + 1e-9);
  if (block >= limit) block = limit - 1;
  if (block < 0) block = 0;
  return block;
}

function spotAt(clientX, clientY) {
  const svg = document.querySelector("#board svg");
  if (!svg) return null;
  const plot = svg.getBoundingClientRect();
  const vb = svg.viewBox.baseVal;
  if (!plot.width || !plot.height || !vb.width || !vb.height) return null;
  const vx = ((clientX - plot.left) / plot.width) * vb.width + vb.x;
  const vy = ((clientY - plot.top) / plot.height) * vb.height + vb.y;
  const left = Number(svg.dataset.plotLeft);
  const top = Number(svg.dataset.plotTop);
  const size = Number(svg.dataset.plotSize);
  const world = Number(svg.dataset.world);
  const x = ((vx - left) / size) * world;
  const z = (1 - ((vy - top) / size)) * world;
  if (!(x >= 0 && z >= 0 && x < world && z < world)) return null;
  const blockX = blockOf(x, world);
  const blockZ = blockOf(z, world);
  let cell = null;
  for (const node of svg.querySelectorAll("[data-cell]")) {
    const x0 = Number(node.dataset.x0);
    const x1 = Number(node.dataset.x1);
    const z0 = Number(node.dataset.z0);
    const z1 = Number(node.dataset.z1);
    if (blockX >= x0 && blockX < x1 && blockZ >= z0 && blockZ < z1) {
      cell = {
        id: node.dataset.cell,
        host: node.dataset.host,
        label: node.dataset.label,
        x0: node.dataset.x0,
        x1: node.dataset.x1,
        z0: node.dataset.z0,
        z1: node.dataset.z1,
      };
      break;
    }
  }
  return {blockX: blockX, blockZ: blockZ, cell: cell};
}

function formatSpot(spot, column, pinned) {
  const lines = [];
  if (spot.cell) lines.push(spot.cell.label + " · " + spot.cell.host);
  lines.push("x " + spot.blockX + "  z " + spot.blockZ);
  let yPart = null;
  if (column && column.state === "ground" && Number.isInteger(column.y) && column.y > 0) {
    lines.push("y " + column.y);
    yPart = String(column.y);
  } else if (column && column.state === "water") {
    lines.push("open water, no dry ground");
  } else if (pinned && column) {
    lines.push("height is unmeasured");
  }
  if (spot.cell) {
    lines.push(
      "section x " + spot.cell.x0 + " to " + (Number(spot.cell.x1) - 1)
      + ", z " + spot.cell.z0 + " to " + (Number(spot.cell.z1) - 1)
    );
  }
  if (pinned && column) {
    lines.push("/tp @s " + spot.blockX + " " + (yPart == null ? "~" : yPart) + " " + spot.blockZ);
    const server = spot.cell ? spot.cell.id : "outside the grid";
    lines.push(server + ". /tp only moves you on the server you are standing on.");
    lines.push("Click this readout to unpin.");
  } else if (pinned) {
    lines.push("reading height");
  }
  return lines.join(lineBreak);
}

function commandLine(text) {
  for (const line of text.split(lineBreak)) {
    if (line.startsWith("/tp @s ")) return line;
  }
  return "";
}

function fallbackCopy(text) {
  const area = document.createElement("textarea");
  area.value = text;
  area.setAttribute("readonly", "");
  area.style.position = "fixed";
  area.style.top = "0";
  area.style.left = "-9999px";
  document.body.appendChild(area);
  area.focus();
  area.select();
  let ok = false;
  try {
    ok = document.execCommand("copy");
  } catch (err) {
    ok = false;
  }
  area.remove();
  if (!ok) throw new Error("copy");
}

function copyCommand(text) {
  const write = navigator.clipboard && navigator.clipboard.writeText
    ? navigator.clipboard.writeText(text)
    : Promise.reject(new Error("clipboard"));
  return write.catch(() => fallbackCopy(text));
}

function rememberColumn(column) {
  if (column && Number.isInteger(column.x) && Number.isInteger(column.z)) {
    heightCache.set(column.x + "," + column.z, column);
  }
  return column;
}

async function columnAt(x, z, fresh) {
  const key = x + "," + z;
  if (!fresh && heightCache.has(key)) return heightCache.get(key);
  const res = await fetch("/map/column?x=" + x + "&z=" + z, {cache: "no-store"});
  if (!res.ok) throw new Error("column");
  return rememberColumn(await res.json());
}

function clearPin() {
  const pin = document.getElementById("map-pin");
  if (pin) pin.remove();
}

function drawPin() {
  const svg = document.querySelector("#board svg");
  if (!svg || !frozen) return;
  clearPin();
  const left = Number(svg.dataset.plotLeft);
  const top = Number(svg.dataset.plotTop);
  const size = Number(svg.dataset.plotSize);
  const world = Number(svg.dataset.world);
  const cx = left + ((frozen.blockX + 0.5) / world) * size;
  const cy = top + (1 - ((frozen.blockZ + 0.5) / world)) * size;
  const pin = document.createElementNS("http://www.w3.org/2000/svg", "circle");
  pin.setAttribute("id", "map-pin");
  pin.setAttribute("cx", cx.toFixed(2));
  pin.setAttribute("cy", cy.toFixed(2));
  pin.setAttribute("r", "6");
  pin.setAttribute("fill", "none");
  pin.setAttribute("stroke", "#c2410c");
  pin.setAttribute("stroke-width", "2");
  pin.setAttribute("pointer-events", "none");
  svg.appendChild(pin);
}

function showFrozen() {
  if (!frozen) return;
  setReadout(frozen.text, true);
  drawPin();
}

function paintReadout() {
  if (frozen) return;
  const spot = pointer ? spotAt(pointer.x, pointer.y) : null;
  if (!spot) {
    setReadout("—", false);
    return;
  }
  const seq = ++hoverSeq;
  const cached = heightCache.get(spot.blockX + "," + spot.blockZ);
  setReadout(formatSpot(spot, cached || null, false), false);
  if (cached) return;
  columnAt(spot.blockX, spot.blockZ, false).then((column) => {
    if (frozen || seq !== hoverSeq) return;
    const current = pointer ? spotAt(pointer.x, pointer.y) : null;
    if (!current || current.blockX !== spot.blockX || current.blockZ !== spot.blockZ) return;
    setReadout(formatSpot(spot, column, false), false);
  }).catch(() => {});
}

async function freezeAt(spot) {
  const seq = ++pinSeq;
  frozen = {
    blockX: spot.blockX,
    blockZ: spot.blockZ,
    text: formatSpot(spot, null, true),
    seq: seq,
  };
  showFrozen();
  let column;
  try {
    column = await columnAt(spot.blockX, spot.blockZ, true);
  } catch (err) {
    column = {x: spot.blockX, z: spot.blockZ, state: "unmeasured"};
  }
  if (!frozen || frozen.seq !== seq) return;
  const text = formatSpot(spot, column, true);
  const command = commandLine(text);
  frozen.text = text;
  showFrozen();
  if (!command) return;
  let note = "Not copied.";
  try {
    await copyCommand(command);
    note = "Copied to clipboard.";
  } catch (err) {
    note = "Not copied.";
  }
  if (!frozen || frozen.seq !== seq) return;
  frozen.text = text + lineBreak + note;
  showFrozen();
}

function onMove(event) {
  if (frozen) return;
  const target = event.target;
  if (target && target.closest && target.closest("#coord-readout")) return;
  const svg = target && target.closest ? target.closest("svg") : null;
  if (!svg) {
    pointer = null;
    setReadout("—", false);
    return;
  }
  pointer = {x: event.clientX, y: event.clientY};
  paintReadout();
}

board.addEventListener("mousemove", onMove);
board.addEventListener("pointerleave", () => {
  pointer = null;
  if (frozen) return;
  setReadout("—", false);
});
board.addEventListener("click", (event) => {
  const target = event.target;
  if (target && target.closest && target.closest("#coord-readout")) {
    pinSeq += 1;
    frozen = null;
    pointer = null;
    clearPin();
    setReadout("—", false);
    return;
  }
  const svg = target && target.closest ? target.closest("svg") : null;
  if (!svg) return;
  const spot = spotAt(event.clientX, event.clientY);
  if (!spot) return;
  freezeAt(spot);
});
</script>
<noscript><meta http-equiv="refresh" content="6"></noscript>
</body>
</html>
"""


NINE_CLIMB_LOG = Path.home() / ".local/state/cks-test-mc-p5/nine-climb/climb-nine.log"
FLOW_SAMPLE_RE = re.compile(
    r"^(?P<at>\d{2}:\d{2}:\d{2}) wave (?P<side>\S+) "
    r"cpu=(?P<cpu>\S+) load=(?P<load>\S+) players=(?P<players>\S+) "
    r"over=(?P<over>\S+) step=(?P<step>\S+) next=(?P<next>\S+) add=(?P<add>\S+)\s*$"
)
FLOW_MISSING_RE = re.compile(
    r"^(?P<at>\d{2}:\d{2}:\d{2}) wave (?P<side>\S+) sample missing; step stays (?P<step>\S+);"
)
FLOW_ADMIT_RE = re.compile(
    r"^(?P<at>\d{2}:\d{2}:\d{2}) admit (?P<name>\S+) place=(?P<place>\S+) span=(?P<span>\S+)"
    r"(?: (?P<rest>.*))?$"
)
FLOW_GAINED_RE = re.compile(
    r"^(?P<at>\d{2}:\d{2}:\d{2}) wave (?P<side>\S+) gained=(?P<gained>\S+) over=(?P<over>\S+) "
    r"step \S+ -> (?P<step>\S+)\s*$"
)
FLOW_SEATED_RE = re.compile(
    r"^(?P<at>\d{2}:\d{2}:\d{2}) wave seated=(?P<seated>\S+) steps (?P<steps>.*)$"
)
FLOW_COLORS = (
    "#1d4ed8",
    "#c2410c",
    "#15803d",
    "#7c3aed",
    "#b45309",
    "#0f766e",
    "#9f1239",
    "#1e40af",
    "#a16207",
    "#0369a1",
    "#4d7c0f",
    "#6d28d9",
)
FLOW_NODE_W = 156
FLOW_LEFT_X = 8
FLOW_RIGHT_X = 276
FLOW_WIDTH = 440


def nine_climb_log() -> Path:
    raw = os.environ.get("CK_MESH_DIR", "").strip()
    if raw:
        candidate = Path(raw).expanduser() / "climb-nine.log"
        if candidate.exists():
            return candidate
    return NINE_CLIMB_LOG


def read_log_tail(path: Path, limit: int = 2_000_000) -> str:
    data = path.read_bytes()
    if len(data) > limit:
        data = data[-limit:]
        cut = data.find(b"\n")
        if cut >= 0:
            data = data[cut + 1 :]
    return data.decode("utf-8", "replace")


def present(text):
    if text is None:
        return None
    token = str(text).strip()
    if token in {"", "None", "none", "null"}:
        return None
    return token


def format_flow_value(value):
    if isinstance(value, bool) or value is None:
        return None
    if isinstance(value, float):
        return present(f"{value:.6g}")
    return present(value)


def whole_token(text):
    token = present(text)
    if token is None or not re.fullmatch(r"-?\d+", token):
        return None
    return token


def flow_sort_key(name):
    text = str(name)
    match = re.search(r"(\d+)$", text)
    if not match:
        return (text, -1, text)
    return (text[: match.start()], int(match.group(1)), text)


def short_generator(name: str) -> str:
    match = re.search(r"(lg-\d+)$", name)
    if match:
        return match.group(1)
    if IP_RE.search(name):
        return "generator"
    return name.rsplit("/", 1)[-1][-24:]


def mem_label(value):
    text = present(value)
    if text is None:
        return None
    if text.lower().startswith("mem"):
        return text
    return f"mem {text}"


def parse_seat_token(token):
    seats = {}
    for part in token.split(","):
        if ":" not in part:
            continue
        side, count = part.split(":", 1)
        count = whole_token(count)
        if not side or count is None:
            continue
        seats[side] = int(count)
    return seats


def admit_from_match(match):
    seats = {}
    mem = None
    for token in (match.group("rest") or "").split():
        if token.startswith(("mem=", "memory=", "mib=")):
            mem = token.split("=", 1)[1]
            continue
        if ":" in token:
            seats.update(parse_seat_token(token))
    admit = {
        "at": match.group("at"),
        "name": match.group("name"),
        "short": short_generator(match.group("name")),
        "seats": seats,
    }
    place = whole_token(match.group("place"))
    if place is not None:
        admit["place"] = place
    span = whole_token(match.group("span"))
    if span is not None:
        admit["span"] = span
    label = mem_label(mem)
    if label:
        admit["mem"] = label
    return admit


def blank_wave(at):
    return {
        "at": at,
        "servers": {},
        "admits": [],
        "seated": None,
        "seated_at": None,
        "closed": False,
        "sampled": False,
    }


def parse_climb_waves(text: str):
    """Latest wave from the driver's log. Missing fields stay absent."""
    waves = []
    current = None

    def open_wave(at):
        nonlocal current
        current = blank_wave(at)
        waves.append(current)

    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        match = FLOW_SAMPLE_RE.match(line)
        if match:
            side = match.group("side")
            if current is None or current["closed"] or current["admits"] or side in current["servers"]:
                open_wave(match.group("at"))
            server = {}
            cpu = present(match.group("cpu"))
            if cpu is not None:
                server["cpu"] = cpu
            load = present(match.group("load"))
            if load is not None:
                server["load"] = load
            players = present(match.group("players"))
            if players is not None:
                server["players"] = players
            over = whole_token(match.group("over"))
            if over is not None:
                server["over"] = int(over)
            step = present(match.group("step"))
            if step is not None:
                server["step"] = step
            current["servers"][side] = server
            current["sampled"] = True
            continue
        match = FLOW_MISSING_RE.match(line)
        if match:
            side = match.group("side")
            if current is None or current["closed"] or current["admits"] or side in current["servers"]:
                open_wave(match.group("at"))
            step = present(match.group("step"))
            server = {}
            if step is not None:
                server["step"] = step
            current["servers"][side] = server
            continue
        match = FLOW_ADMIT_RE.match(line)
        if match:
            if current is None or current["closed"]:
                open_wave(match.group("at"))
            admit = admit_from_match(match)
            current["admits"] = [item for item in current["admits"] if item["name"] != admit["name"]]
            current["admits"].append(admit)
            continue
        match = FLOW_GAINED_RE.match(line)
        if match:
            if current is None or current["closed"]:
                open_wave(match.group("at"))
            side = match.group("side")
            server = current["servers"].setdefault(side, {})
            over = whole_token(match.group("over"))
            if over is not None:
                server["over"] = int(over)
            step = present(match.group("step"))
            if step is not None:
                server["step"] = step
            continue
        match = FLOW_SEATED_RE.match(line)
        if match:
            if current is None or current["closed"]:
                open_wave(match.group("at"))
            seated = whole_token(match.group("seated"))
            if seated is not None:
                current["seated"] = seated
            current["seated_at"] = match.group("at")
            current["closed"] = True
            for part in match.group("steps").split():
                if ":" not in part:
                    continue
                side, step = part.split(":", 1)
                step = present(step)
                if not side or step is None:
                    continue
                current["servers"].setdefault(side, {})["step"] = step
            continue
    return waves


def parse_climb_log_text(text: str):
    for wave in reversed(parse_climb_waves(text)):
        if wave["servers"] or wave["admits"]:
            return wave
    return None


def waves_from_snapshot(data):
    if not isinstance(data, dict) or not isinstance(data.get("waves"), list):
        return []
    parsed = []
    for chosen in data["waves"]:
        if not isinstance(chosen, dict):
            continue
        servers_in = chosen.get("servers") if isinstance(chosen.get("servers"), dict) else {}
        admits_in = chosen.get("admits") if isinstance(chosen.get("admits"), list) else []
        if not servers_in and not admits_in:
            continue
        servers = {}
        for side, row in servers_in.items():
            if not isinstance(side, str) or not isinstance(row, dict):
                continue
            server = {}
            for key in ("cpu", "load", "players", "step"):
                text = format_flow_value(row.get(key))
                if text is not None:
                    server[key] = text
            over = row.get("over")
            if isinstance(over, int) and not isinstance(over, bool):
                server["over"] = over
            servers[side] = server
        admits = []
        for row in admits_in:
            if not isinstance(row, dict) or not isinstance(row.get("name"), str):
                continue
            seats = {}
            raw_seats = row.get("seats") if isinstance(row.get("seats"), dict) else {}
            for side, count in raw_seats.items():
                if isinstance(side, str) and isinstance(count, int) and not isinstance(count, bool):
                    seats[side] = count
            admit = {
                "at": row.get("at") if isinstance(row.get("at"), str) else None,
                "name": row["name"],
                "short": short_generator(row["name"]),
                "seats": seats,
            }
            place = format_flow_value(row.get("place"))
            if place is not None:
                admit["place"] = place
            span = format_flow_value(row.get("span"))
            if span is not None:
                admit["span"] = span
            label = mem_label(row.get("mem") if row.get("mem") is not None else row.get("memory"))
            if label:
                admit["mem"] = label
            admits.append(admit)
        seated = format_flow_value(chosen.get("seated"))
        parsed.append({
            "at": chosen.get("at") if isinstance(chosen.get("at"), str) else None,
            "servers": servers,
            "admits": admits,
            "seated": seated,
            "seated_at": chosen.get("seated_at") if isinstance(chosen.get("seated_at"), str) else None,
            "closed": bool(chosen.get("closed")) or seated is not None,
            "sampled": any(
                key in server for server in servers.values() for key in ("cpu", "load", "players")
            ),
        })
    return parsed


def prefer_flow_snapshot(log_mtime, snap_mtime) -> bool:
    if snap_mtime is None:
        return False
    if log_mtime is None:
        return True
    return snap_mtime + 1 >= log_mtime


def load_flow_waves():
    path = nine_climb_log()
    snap_path = path.with_name(path.stem + "-flow.json")
    log_waves = []
    log_mtime = None
    if path.exists():
        log_waves = parse_climb_waves(read_log_tail(path))
        try:
            log_mtime = path.stat().st_mtime
        except OSError:
            log_mtime = None
    snap_waves = []
    snap_mtime = None
    if snap_path.exists():
        try:
            snap_waves = waves_from_snapshot(json.loads(snap_path.read_text()))
            snap_mtime = snap_path.stat().st_mtime
        except (OSError, json.JSONDecodeError, UnicodeError, TypeError, ValueError):
            snap_waves = []
    if snap_waves and prefer_flow_snapshot(log_mtime, snap_mtime):
        return snap_waves, "snapshot"
    return log_waves, "log"


def pick_flow(waves):
    usable = [wave for wave in waves or [] if wave.get("servers") or wave.get("admits")]
    if not usable:
        return None, None
    latest = usable[-1]
    if latest.get("admits"):
        return latest, latest
    connections = None
    for wave in reversed(usable):
        if wave.get("admits"):
            connections = wave
            break
    return connections, latest


def flow_notes(wave, time_id="flow-time") -> str:
    bits = []
    if wave.get("sampled") and wave.get("at"):
        bits.append(f"Server load sampled at {wave['at']}")
    elif wave.get("at"):
        bits.append(f"Wave at {wave['at']}")
    if wave.get("seated") is not None and wave.get("seated_at"):
        bits.append(f"{wave['seated']} seated at {wave['seated_at']}")
    elif wave.get("seated") is not None:
        bits.append(f"{wave['seated']} seated")
    if not bits:
        bits.append("Wave time is not in the climb log")
    bits.append("from the climb log")
    notes = f'<p class="note" id="{esc(time_id)}">{esc(", ".join(bits))}.</p>'
    if wave.get("closed"):
        notes += (
            '<p class="note" id="flow-kind">Last completed wave in the climb log, '
            "not a live 8-second sample.</p>"
        )
    return notes


def server_lines(side, info):
    lines = [side]
    if info.get("cpu") is not None:
        lines.append(f"cpu {info['cpu']}")
    if info.get("load") is not None:
        lines.append(f"load {info['load']}")
    if info.get("players") is not None:
        lines.append(f"players {info['players']}")
    if info.get("over") is not None:
        lines.append("over" if info["over"] else "under")
    if info.get("step") is not None:
        lines.append(f"step {info['step']}")
    return lines


def generator_lines(gen):
    lines = [gen["short"]]
    if gen.get("place") is not None:
        lines.append(f"placed {gen['place']}")
    if gen.get("span") is not None and gen.get("span") != gen.get("place"):
        lines.append(f"span {gen['span']}")
    if gen.get("mem"):
        lines.append(gen["mem"])
    return lines


def server_rows(wave, note="Connections for this wave are not in the log yet.") -> str:
    servers = wave.get("servers") or {}
    columns = [
        ("cpu", "CPU"),
        ("load", "Load"),
        ("players", "Players"),
        ("over", "Line"),
        ("step", "Step"),
    ]
    shown = []
    for key, _label in columns:
        if any(key in info for info in servers.values()):
            shown.append((key, _label))
    head = "".join(f"<th>{esc(label)}</th>" for _key, label in shown)
    body = []
    for side in sorted(servers, key=flow_sort_key):
        info = servers[side]
        cells = [f'<td>{esc(side)}</td>']
        for key, _label in shown:
            if key not in info:
                cells.append("<td></td>")
                continue
            if key == "over":
                cells.append(f'<td>{"over" if info[key] else "under"}</td>')
            else:
                cells.append(f'<td class="num">{esc(info[key])}</td>')
        body.append(f'<tr data-server="{esc(side)}">{"".join(cells)}</tr>')
    table = (
        '<table class="flow-load"><thead><tr><th>Server</th>'
        f"{head}</tr></thead><tbody>{''.join(body)}</tbody></table>"
    )
    return table + f'<p class="note">{esc(note)}</p>'


def flow_box_height(line_count: int) -> int:
    return 16 + 13 * max(0, line_count - 1) + 10


def layout_flow_column(entries):
    y = 28
    placed = []
    for entry in entries:
        text_h = flow_box_height(len(entry["lines"]))
        edges = entry.get("edges", 0)
        slot = text_h if edges <= 0 else max(text_h, edges * 16)
        node_y = y + max(0, (slot - text_h) / 2)
        placed.append({**entry, "y": node_y, "h": text_h})
        y += slot + 8
    return placed, y


def separate_flow_labels(labels):
    labels.sort(key=lambda item: (item["y"], item["x"]))
    for _ in range(12):
        for index in range(1, len(labels)):
            if abs(labels[index]["x"] - labels[index - 1]["x"]) > 36:
                continue
            gap = labels[index]["y"] - labels[index - 1]["y"]
            if gap < 14:
                push = (14 - gap) / 2
                labels[index - 1]["y"] -= push
                labels[index]["y"] += push
    if not labels:
        return
    low = min(item["y"] for item in labels)
    if low < 22:
        delta = 22 - low
        for item in labels:
            item["y"] += delta


def curve_at(t, x1, y1, x2, y2):
    c1x, c1y = x1 + 46, y1
    c2x, c2y = x2 - 46, y2
    u = 1 - t
    x = (u * u * u * x1) + (3 * u * u * t * c1x) + (3 * u * t * t * c2x) + (t * t * t * x2)
    y = (u * u * u * y1) + (3 * u * u * t * c1y) + (3 * u * t * t * c2y) + (t * t * t * y2)
    return x, y, c1x, c1y, c2x, c2y


def flow_diagram(wave) -> str:
    servers = dict(wave.get("servers") or {})
    gens = []
    for admit in wave.get("admits") or []:
        seats = {}
        for side, count in (admit.get("seats") or {}).items():
            if isinstance(count, bool) or not isinstance(count, int):
                continue
            if count > 0:
                seats[side] = count
                servers.setdefault(side, {})
        gens.append({**admit, "seats": seats, "lines": generator_lines(admit), "edges": len(seats)})
    gens.sort(key=lambda item: flow_sort_key(item.get("short") or item.get("name") or ""))
    server_names = sorted(servers, key=flow_sort_key)
    incoming = {side: 0 for side in server_names}
    for gen in gens:
        for side in gen["seats"]:
            incoming[side] = incoming.get(side, 0) + 1
    server_entries = [
        {
            "side": side,
            "lines": server_lines(side, servers.get(side) or {}),
            "edges": incoming.get(side, 0),
            "over": (servers.get(side) or {}).get("over"),
        }
        for side in server_names
    ]
    gen_placed, gen_bottom = layout_flow_column(gens)
    server_placed, server_bottom = layout_flow_column(server_entries)
    server_by_side = {item["side"]: item for item in server_placed}
    edges = []
    labels = []
    for index, gen in enumerate(gen_placed):
        color = FLOW_COLORS[index % len(FLOW_COLORS)]
        gen["color"] = color
        y1 = gen["y"] + gen["h"] / 2
        x1 = FLOW_LEFT_X + FLOW_NODE_W
        for side, count in sorted(gen["seats"].items(), key=lambda item: flow_sort_key(item[0])):
            server = server_by_side.get(side)
            if server is None:
                continue
            y2 = server["y"] + server["h"] / 2
            x2 = FLOW_RIGHT_X
            mx, my, c1x, c1y, c2x, c2y = curve_at(0.5, x1, y1, x2, y2)
            edges.append({
                "short": gen["short"],
                "side": side,
                "count": count,
                "color": color,
                "x1": x1,
                "y1": y1,
                "x2": x2,
                "y2": y2,
                "c1x": c1x,
                "c1y": c1y,
                "c2x": c2x,
                "c2y": c2y,
            })
            labels.append({
                "short": gen["short"],
                "side": side,
                "count": count,
                "color": color,
                "x": mx,
                "y": my,
            })
    separate_flow_labels(labels)
    label_bottom = max((item["y"] + 12 for item in labels), default=0)
    height = max(gen_bottom, server_bottom, label_bottom, 48) + 8
    parts = [
        f'<svg class="flow" viewBox="0 0 {FLOW_WIDTH} {height:.0f}" role="group" '
        'aria-label="Generators connected to game servers" font-family="system-ui, sans-serif">',
        f'<text x="{FLOW_LEFT_X}" y="16" font-size="11" fill="#57534e">Generators</text>',
        f'<text x="{FLOW_RIGHT_X}" y="16" font-size="11" fill="#57534e">Game servers</text>',
    ]
    for edge in edges:
        parts.append(
            f'<path data-link="{esc(edge["short"])} {esc(edge["side"])}" data-count="{edge["count"]}" '
            f'd="M {edge["x1"]:.1f} {edge["y1"]:.1f} C {edge["c1x"]:.1f} {edge["c1y"]:.1f}, '
            f'{edge["c2x"]:.1f} {edge["c2y"]:.1f}, {edge["x2"]:.1f} {edge["y2"]:.1f}" '
            f'fill="none" stroke="{edge["color"]}" stroke-width="1.4"/>'
        )
    for label in labels:
        text = str(label["count"])
        width = 8 + 7 * len(text)
        parts.append(
            f'<g data-link-label="{esc(label["short"])} {esc(label["side"])}" data-count="{label["count"]}">'
            f'<rect x="{label["x"] - width / 2:.1f}" y="{label["y"] - 8:.1f}" width="{width}" height="14" rx="3" '
            f'fill="#fff" stroke="#e7e5e4"/>'
            f'<text x="{label["x"]:.1f}" y="{label["y"] + 3:.1f}" text-anchor="middle" font-size="11" '
            f'font-weight="650" fill="{label["color"]}">{esc(text)}</text>'
            f'<title>{esc(label["short"])} to {esc(label["side"])}: {esc(text)}</title>'
            "</g>"
        )
    for gen in gen_placed:
        parts.append(flow_node("generator", gen["short"], FLOW_LEFT_X, gen["y"], gen["h"], gen["lines"], gen["color"]))
    for server in server_placed:
        accent = "#9f1239" if server.get("over") else "#a8a29e"
        parts.append(flow_node("server", server["side"], FLOW_RIGHT_X, server["y"], server["h"], server["lines"], accent))
    parts.append("</svg>")
    return "".join(parts)


def flow_node(kind, key, x, y, height, lines, accent) -> str:
    parts = [
        f'<g data-{kind}="{esc(key)}">',
        f'<rect x="{x:.1f}" y="{y:.1f}" width="{FLOW_NODE_W}" height="{height:.1f}" rx="8" '
        f'fill="#fafaf9" stroke="{accent}" stroke-width="1.2"/>',
    ]
    for index, line in enumerate(lines):
        weight = ' font-weight="650"' if index == 0 else ""
        fill = "#1c1917" if index == 0 else "#44403c"
        if line == "over":
            fill = "#9f1239"
        elif line == "under":
            fill = "#14532d"
        parts.append(
            f'<text x="{x + 10:.1f}" y="{y + 16 + 13 * index:.1f}" font-size="12" fill="{fill}"{weight}>'
            f"{esc(line)}</text>"
        )
    parts.append("</g>")
    return "".join(parts)


def render_flow(wave, source="log") -> str:
    if not wave or (not wave.get("servers") and not wave.get("admits")):
        return (
            '<h2>Generators and game servers</h2>'
            '<p class="note" data-flow-source="none">No wave sample is in the climb log yet.</p>'
        )
    body = flow_diagram(wave) if wave.get("admits") else server_rows(wave)
    return (
        '<h2>Generators and game servers</h2>'
        f"{flow_notes(wave)}"
        f'<div data-flow-source="{esc(source)}">{body}</div>'
    )


def render_flow_view(connections, latest, source="log") -> str:
    if connections is None:
        return render_flow(latest, source)
    parts = [
        '<h2>Generators and game servers</h2>',
        flow_notes(connections),
    ]
    if latest is not None and latest is not connections:
        if not connections.get("closed"):
            parts.append(
                '<p class="note" id="flow-kind">These connections are from the climb log, '
                "not a live 8-second sample.</p>"
            )
        when = latest.get("at")
        later = (
            f"A later sample at {when} did not log connections."
            if when
            else "A later sample did not log connections."
        )
        parts.append(f'<p class="note" id="flow-later">{esc(later)}</p>')
    parts.append(f'<div data-flow-source="{esc(source)}">{flow_diagram(connections)}</div>')
    if latest is not None and latest is not connections and latest.get("servers"):
        parts.append("<h2>Latest server load</h2>")
        parts.append(flow_notes(latest, "flow-later-time"))
        parts.append(server_rows(latest, "This sample did not log connections."))
    return "".join(parts)


def flow_body() -> str:
    try:
        waves, source = load_flow_waves()
    except Exception as exc:
        sys.stderr.write(f"climb log read failed: {type(exc).__name__}\n")
        waves, source = [], "log"
    connections, latest = pick_flow(waves)
    return render_flow_view(connections, latest, source)


def map_live_fragment() -> str:
    payload = positions_payload()
    return f'<div id="map-live">{map_body(payload)}</div><div id="flow-live">{flow_body()}</div>'


def render_map_page() -> str:
    payload = positions_payload()
    page = MAP_PAGE.replace("<!--BOARD-->", map_body(payload), 1)
    return page.replace("<!--FLOW-->", flow_body(), 1)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        code = str(args[1]) if len(args) >= 2 else ""
        if code and code != "200":
            sys.stderr.write(f"{self.command} {code}\n")

    def do_GET(self):
        parsed = urllib.parse.urlsplit(self.path)
        path = parsed.path
        if path == "/map/column":
            self._column(urllib.parse.parse_qs(parsed.query))
            return
        if path == "/positions.json":
            body = redact_public(json.dumps(positions_payload())).encode("utf-8")
            self._send(200, "application/json; charset=utf-8", body)
            return
        if path in {"/map", "/map/"}:
            self._send(200, "text/html; charset=utf-8", redact_public(render_map_page()).encode("utf-8"))
            return
        if path == "/map/live":
            self._send(200, "text/html; charset=utf-8", redact_public(map_live_fragment()).encode("utf-8"))
            return
        try:
            state = build()
        except Exception:
            self._send(500, "text/plain; charset=utf-8", b"could not read the climb files\n")
            return
        if path in {"/", "/index.html"}:
            self._send(200, "text/html; charset=utf-8", render_document(state).encode("utf-8"))
            return
        if path == "/live":
            self._send(200, "text/html; charset=utf-8", render_live(state).encode("utf-8"))
            return
        self._send(404, "text/plain; charset=utf-8", b"not found\n")

    def _column(self, query):
        try:
            x = int(query.get("x", [""])[0])
            z = int(query.get("z", [""])[0])
        except (TypeError, ValueError):
            self._send(400, "application/json; charset=utf-8", b'{"state":"bad"}\n')
            return
        if 0 <= x < WORLD and 0 <= z < WORLD:
            payload = column_state(column_table(), x, z)
        else:
            payload = {"x": x, "z": z, "state": "unmeasured"}
        self._send(200, "application/json; charset=utf-8", json.dumps(payload).encode("utf-8"))

    def _send(self, code, content_type, body: bytes):
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)


def serve():
    signal.signal(signal.SIGHUP, signal.SIG_IGN)
    threading.Thread(target=watch, name="csv-watch", daemon=True).start()
    threading.Thread(target=map_watch, name="positions", daemon=True).start()
    threading.Thread(target=nine_watch, name="nine-seated", daemon=True).start()
    _pop_ready.wait(timeout=30)
    _map_ready.wait(timeout=25)
    _nine_ready.wait(timeout=45)
    try:
        httpd = ThreadingHTTPServer(BIND, Handler)
    except OSError:
        raise SystemExit("could not bind 127.0.0.1:8768")
    httpd.daemon_threads = True
    PID_PATH.write_text(f"{os.getpid()}\n")
    print("http://127.0.0.1:8768/", flush=True)
    print("http://127.0.0.1:8768/map", flush=True)
    print("watching seam-scaling-four-2026-11-19.csv and seam-scaling-sixteen-2026-11-19.csv", flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        httpd.server_close()


def expect(condition: bool, message: str):
    if not condition:
        raise SystemExit(message)


def self_check():
    header = (
        "n_target,n_held,result,busiest,busiest_cpu,busiest_load1,binding,"
        "handoffs_to_a,moved_wrongly_a,moving_sample,hot_whole\n"
    )

    def parsed(body: str):
        text = header + body
        if not text.endswith("\n"):
            text = read_changeable_text(text)
        rows = parse_text(text if text.endswith("\n") or "\n" not in text else text)
        mark_bend(rows)
        return rows

    hot_fail = parsed("10,10,clean,p1,95,1.0,none,1,0,2/4\n11,9,fail,p1,96,2.0,cpu,0,1,1/4\n")
    expect(not hot_fail[0]["is_bend"] and not hot_fail[1]["is_bend"], "a hot hold followed by a cpu miss is not a bend")
    expect(hot_fail[0]["handoffs"] == 1 and hot_fail[1]["moved_wrongly"] == 1, "handoff and moved-wrongly sums")
    expect(hot_fail[0]["moving"] == "2 of 4", "moving sample wording")

    refused = parsed("10,10,clean,p1,95,1.0,none,1,0,2/4,true\n11,9,fail,p1,96,2.0,game,0,1,1/4,\n")
    expect(refused[0]["is_bend"] and not refused[1]["is_bend"], "a hot hold whose next count the servers refused is the bend")

    held_next = parsed("10,10,clean,p1,95,1.0,none,0,0,1/4\n20,20,clean,p1,20,0.2,none,0,0,1/4\n")
    expect(all(not row["is_bend"] for row in held_next), "a hot hold whose next count holds is not a bend")

    alone = parsed("10,10,clean,p1,95,2.0,none,0,0,1/4\n")
    expect(not alone[0]["is_bend"], "a hot hold with no next count is not a bend yet")

    quiet = parsed("10,10,clean,p1,10,0.5,none,0,0,1/4\n11,9,fail,p1,99,3,cpu,0,0,0/4\n")
    expect(not quiet[0]["is_bend"], "a quiet hold is not a bend")

    load_edge = parsed("10,10,clean,p1,10,1.8,none,0,0,1/4,true\n11,8,fail,p1,10,1.9,game,0,0,0/4,\n")
    expect(load_edge[0]["is_bend"], "load 1.8 is hot when the servers refuse the next count")

    load_miss = parsed("10,10,clean,p1,10,1.8,none,0,0,1/4\n11,8,fail,p1,10,1.9,cpu,0,0,0/4\n")
    expect(not load_miss[0]["is_bend"], "load 1.8 followed by a cpu miss is not a bend")

    cpu_edge = parsed("10,10,clean,p1,90,0.1,none,0,0,1/4,true\n11,8,fail,p1,91,0.2,game,0,0,0/4,\n")
    expect(cpu_edge[0]["is_bend"], "cpu 90 is hot when the servers refuse the next count")

    spawn_miss = parsed("10,10,clean,p1,90,0.1,none,0,0,1/4\n11,8,fail,p1,91,0.2,spawn,0,0,0/4\n")
    expect(not spawn_miss[0]["is_bend"], "a spawn miss is not a bend")

    false_caps = parse_text(
        "n_target,n_held,result,busiest,busiest_cpu,busiest_load1,binding,note\n"
        "10,10,clean,p1,95,2.0,none,\n"
        "11,0,fail,p1,96,2.2,game,stop: max runtime (~21600s)\n"
        "12,12,clean,p1,95,2.0,none,\n"
        "13,0,fail,p1,96,2.2,game,harness restart\n"
        "14,14,clean,p1,95,2.0,none,\n"
        "15,0,fail,p1,96,2.2,game,index window full\n"
    )
    mark_bend(false_caps)
    expect(all(not row["is_bend"] for row in false_caps), "a six-hour exit, a harness restart, and a full index window are not bends")

    under = parsed("10,10,clean,p1,89.9,1.799,none,0,0,1/4\n11,8,fail,p1,90,2,cpu,0,0,0/4\n")
    expect(not under[0]["is_bend"], "just under both hot thresholds is not a bend")

    loadgen = parsed("10,10,clean,p1,95,2,none,0,0,1/4\n11,8,fail,p1,20,0.2,loadgen,0,0,0/4\n")
    expect(not loadgen[0]["is_bend"], "a load-generator miss is not the bend")

    later = parsed(
        "10,10,clean,p1,95,2,none,0,0,1/4,\n"
        "11,11,clean,p1,96,2.2,none,0,0,1/4,true\n"
        "12,9,fail,p1,97,2.4,game,0,0,0/4,\n"
    )
    expect(later[1]["is_bend"] and not later[0]["is_bend"], "the bend is the hot hold immediately before the server refusal")

    partial = header + "10,10,clean,p1,95,2,none,0,0,1/4\n11,9,fa"
    partial_rows = parse_text(read_changeable_text(partial))
    expect(len(partial_rows) == 1, "a partial trailing row is left unread")

    for c1, sigma, kappa in ((80.0, 0.04, 0.002), (57.0, 0.1, 0.01), (120.0, 0.0, 0.0)):
        points = []
        for servers in (4, 9, 16):
            value = usl(servers, c1, sigma, kappa)
            expect(value is not None and value > 0, "synthetic USL point")
            points.append((servers, value))
        got = recover(points)
        expect(got is not None, "USL solve failed")
        for actual, expected in zip(got, (c1, sigma, kappa)):
            expect(abs(actual - expected) <= 1e-6 * max(1.0, abs(expected)), f"USL recover {actual} != {expected}")

    four = parsed("200,200,clean,west,92,2.2,none,4,1,6/8,true\n209,180,fail,west,95,3,game,1,2,2/8,\n")
    sixteen = parsed("700,700,clean,p1,91,1.9,none,8,0,10/16,true\n701,640,fail,p1,96,2.5,game,2,3,4/16,\n")
    fit = try_fit(four, sixteen)
    expect(fit["ready"], "two bend rows should fit")
    for servers, players in ((4, 200.0), (9, 513.0), (16, 700.0)):
        predicted = usl(servers, fit["c1"], fit["sigma"], fit["kappa"])
        expect(predicted is not None and abs(predicted - players) < 1e-3, "fit misses a labeled point")
    fitted = fit_html(fit)
    expect("The fit is waiting." not in fitted, "a finished fit still says it is waiting")
    expect("C(1)" in fitted and "sigma" in fitted and "kappa" in fitted, "fitted page omits a parameter")
    expect("different build" in fitted, "fitted page omits the different-build sentence")
    expect("The fit is waiting." in fit_html(try_fit([], [])), "missing bends should wait")

    nasty = parsed("10,10,clean,<b>,95,2,none,1,0,3/8\n11,0,fail,<b>,99,3,cpu,0,1,0/8\n")
    rendered = climb_section({
        "name": "Four-server climb",
        "servers": 4,
        "rows": nasty,
        "error": None,
        "stamp": None,
        "filename": "seam-scaling-four-2026-11-19.csv",
        "chart": None,
        "color": "#1d4ed8",
    })
    expect("<b>" not in rendered and "&lt;b&gt;" in rendered, "csv text was not escaped")
    expect("3 of 8" in rendered, "moving sample not shown")

    for path in (FOUR_CSV, SIXTEEN_CSV):
        _rows, err, _stamp = load_csv(path, path.name)
        expect(err is None, f"could not read {path.name}")
    state = build()
    page = render_document(state)
    expect("Four-server climb" in page and "16-server climb" in page, "page does not name both climbs")
    expect("different build" in page, "page does not say the nine-server point is a different build")
    expect("seam-scaling-four-2026-11-19.csv" in page and "seam-scaling-sixteen-2026-11-19.csv" in page, "page hides a source")
    expect("not part of the fit" in page, "seam chart is not labeled as outside the fit")
    expect("513" in page and "about 57" in page, "nine-server point is missing")
    expect("held 44" in page and "held 80" in page and "held 90" in page, "seam-lap holds are missing")
    if state["fit"]["ready"]:
        expect("C(1)" in page and "sigma" in page and "kappa" in page, "ready fit is not printed")
        expect("The fit is waiting." not in page, "ready fit still says it is waiting")
    else:
        expect("The fit is waiting." in page, "the fit should be waiting")
    expect('data-page="mesh-climbs"' in page, "page marker missing")
    total, answered = sum_answered_active(
        {
            "test-mc-p5-lg-1": {"active": 10},
            "test-mc-p5-lg-2": {"error": "down", "active": 0},
            "test-mc-p5-lg-3": {"active": 4},
            "test-mc-p5-lg-4": {"busy": True},
            "test-mc-p5-lg-5": {"active": True},
        },
        SIXTEEN_LOADS,
    )
    expect(total == 14 and answered == 2, "a generator that does not answer is not counted as zero")
    live = render_live(state)
    heading = live.find("Players against server count")
    counter = live.find("16-server now", heading)
    four_counter = live.find("4-server now", heading)
    graph = live.find("<svg", heading)
    expect(heading != -1 and counter != -1 and graph != -1 and counter < graph, "16-server count is not above the chart")
    expect(four_counter != -1 and four_counter < graph, "4-server count is not above the chart")
    sample_found = {
        "test-mc-lg-1": "a",
        "test-mc-lg-2": "a",
        "test-mc-lg-3": "a",
        "test-mc-lg-4": "a",
        "test-mc-lg-5": "a",
        "test-mc-loadgen-1": "a",
        "test-mc-lg-9": "a",
        "test-mc-f4-lg-2": "a",
        "test-mc-f4-lg-11": "a",
        "test-mc-f4-lg-extra": "a",
        "test-mc-p5-lg-1": "a",
        "test-mc-p5-lg-7": "a",
        "test-mc-west3": "a",
    }
    got_names = four_load_names(sample_found)
    expect(
        got_names == (
            "test-mc-lg-1",
            "test-mc-lg-2",
            "test-mc-lg-3",
            "test-mc-lg-4",
            "test-mc-lg-5",
            "test-mc-loadgen-1",
            "test-mc-f4-lg-2",
            "test-mc-f4-lg-11",
        ),
        "four-server names include a host that is not a four-server generator",
    )
    mixed = {name: {"active": 3} for name in got_names}
    mixed["test-mc-p5-lg-1"] = {"active": 100}
    mixed["test-mc-west3"] = {"active": 50}
    four_total, four_answered = sum_answered_active(mixed, got_names)
    expect(four_total == 24 and four_answered == 8, "four-server sum counted a host outside the four-server generators")
    expect("Nine-server climb" in page, "nine climb is missing")
    expect('href="/map"' in page, "map link is missing")
    if not any(row["is_bend"] for row in state["nine"]["rows"]):
        expect("The climb is in the fast ramp." in page, "nine climb should say it is in the fast ramp")
    expect(cell_of(98, 705) == "p1", "p1 is the southwest cell")
    expect(cell_of(2000, 0) == "p2", "the cut at 2000 belongs to the next cell")
    expect(cell_of(2500, 2500) == "p5", "p5 is the center cell")
    expect(cell_of(4100, 4100) == "p9", "p9 is the northeast cell")
    expect(cell_of(6000, 10) is None, "6000 is outside the grid")
    expect(is_bot_name("Load12") and is_bot_name("S161") and not is_bot_name("BenScholtens") and not is_bot_name("Hiirukai"), "bot names")
    compile(REMOTE_SCRIPT.replace("SIDE_TOKEN", "p1"), "<remote>", "exec")
    sample = positions_from_players(
        [
            {"name": "BenScholtens", "x": 98, "y": 125, "z": 705, "live": True, "side": "p1"},
            {"name": "Load1", "x": 10, "y": 64, "z": 10, "live": False, "side": "p1"},
            {"name": "S161", "x": 2500, "y": 70, "z": 100, "live": False, "side": "p2"},
            {"name": "Hiirukai", "x": 4100, "y": 80, "z": 4100, "live": True, "side": "p9"},
        ],
        [],
        "sample",
    )
    drawn = map_body(sample)
    expect("BenScholtens" in drawn and "Hiirukai" in drawn, "real names missing from the map")
    expect("Load1" not in drawn and "S161" not in drawn, "a bot was given a name tag")
    expect("2 bots." in drawn, "bot count missing")
    expect("seated" not in drawn, "an unmeasured seated count was invented")
    labeled = map_body(sample, seated=5)
    expect("5 bots seated. 2 plotted." in labeled, "seated and plotted counts missing")
    expect("Load1" not in labeled and "S161" not in labeled, "a seated label named a bot")
    expect('data-cell="p1"' in drawn and 'data-cell="p9"' in drawn and "p1 southwest" in drawn and "p5 center" in drawn, "grid labels missing")
    expect('data-cut="2000"' in drawn and 'data-cut="4000"' in drawn, "cut lines missing")
    expect('aria-label="Nine-server coordinate grid"' in drawn, "map label missing")
    expect('data-plot-left="52"' in drawn and 'data-plot-size="560"' in drawn and 'data-world="6000"' in drawn, "map scale missing")
    expect('data-host="test-mc-p5-1"' in drawn and 'data-x0="2000"' in drawn and 'data-z1="6000"' in drawn, "cell bounds missing")
    expect('id="coord-readout"' in drawn, "hover readout missing")
    expect(
        "mousemove" in MAP_PAGE and "/tp @s" in MAP_PAGE and "height is unmeasured" in MAP_PAGE,
        "map page dropped the pinned teleport readout",
    )
    expect(
        "navigator.clipboard.writeText" in MAP_PAGE and 'document.execCommand("copy")' in MAP_PAGE
        and "Copied to clipboard." in MAP_PAGE and "Not copied." in MAP_PAGE,
        "map page dropped the teleport clipboard copy",
    )
    expect('id="flow"' in MAP_PAGE and 'id="board"' in MAP_PAGE and "#map-live" in MAP_PAGE, "map page dropped the load diagram")
    sample_log = (
        "08:41:51 wave p1 cpu=1.2 load=0.08 players=0 over=0 step=9 next=9 add=9\n"
        "08:41:51 wave p2 cpu=None load=0.11 players=0 over=0 step=9 next=9 add=9\n"
        "08:41:51 wave p10 cpu=3 load=0.2 players=1 over=1 step=4 next=4 add=4\n"
        "08:41:59 admit test-mc-p5-lg-10 place=9 span=9 p1:1,p2:0,p10:2\n"
        "08:42:00 admit test-mc-p5-lg-19 place=3 span=3 p1:3 mem=4100\n"
        "08:42:01 admit 10.1.2.3 place=1 span=1 p1:4\n"
        "08:46:56 wave p1 gained=9 over=0 step 9 -> 10\n"
        "08:47:02 wave seated=76 steps p1:10 p2:1 p10:4\n"
    )
    wave = parse_climb_log_text(sample_log)
    expect(wave["closed"] and wave["seated"] == "76", "wave seated count")
    expect("cpu" not in wave["servers"]["p2"], "missing cpu is omitted")
    expect(wave["servers"]["p2"]["load"] == "0.11", "a present load was dropped")
    expect(wave["servers"]["p1"]["step"] == "10", "the seated step was dropped")
    expect(wave["servers"]["p10"]["over"] == 1, "a server over the line was dropped")
    html = render_flow(wave)
    expect('data-server="p1"' in html and 'data-server="p10"' in html, "server nodes missing")
    expect('data-generator="lg-10"' in html and 'data-generator="lg-19"' in html, "generator nodes missing")
    expect('data-link="lg-10 p1" data-count="1"' in html, "admit count missing")
    expect('data-link="lg-10 p10" data-count="2"' in html, "a later server admit was dropped")
    expect('data-link="lg-19 p1" data-count="3"' in html, "a new generator admit was dropped")
    expect('data-link="lg-10 p2"' not in html, "a zero admit was drawn")
    expect("mem 4100" in html and "placed 9" in html, "admit fields missing")
    expect("Last completed wave in the climb log, not a live 8-second sample." in html, "completed-wave note missing")
    expect("08:41:51" in html and "76 seated at 08:47:02" in html, "wave timestamp missing")
    expect("10.1.2.3" not in html and IP_RE.search(html) is None, "flow diagram contains an address")
    quiet = parse_climb_log_text(
        "08:52:43 wave p1 cpu=13.1 load=0.42 players=19 over=0 step=11 next=11 add=11\n"
        "08:52:43 wave west cpu=None load=0.1 players=4 over=0 step=2 next=2 add=2\n"
    )
    quiet_html = render_flow(quiet)
    expect("Connections for this wave are not in the log yet." in quiet_html, "missing admits were not called out")
    expect("data-link=" not in quiet_html, "a line was drawn without an admit")
    expect(">p1<" in quiet_html and ">13.1<" in quiet_html and ">west<" in quiet_html, "server rows missing")
    expect("cpu" not in quiet["servers"]["west"], "a missing cpu was invented")
    expect("Last completed wave" not in quiet_html, "an open wave was called completed")
    expect("No wave sample is in the climb log yet." in render_flow(None), "empty log note missing")
    mixed = parse_climb_waves(
        "08:52:43 wave p1 cpu=13.1 load=0.42 players=19 over=0 step=11 next=11 add=11\n"
        "08:52:51 admit test-mc-p5-lg-10 place=2 span=2 p1:2\n"
        "08:58:29 wave seated=245 steps p1:12\n"
        "09:07:49 wave p1 cpu=16.4 load=0.28 players=30 over=0 step=12 next=12 add=12\n"
    )
    connections, latest = pick_flow(mixed)
    mixed_html = render_flow_view(connections, latest)
    expect(connections is not latest, "a later sample hid the last admit")
    expect('data-link="lg-10 p1" data-count="2"' in mixed_html, "the last admit line was dropped")
    expect("245 seated at 08:58:29" in mixed_html, "the last seated wave was dropped")
    expect("Last completed wave in the climb log, not a live 8-second sample." in mixed_html, "completed-wave note missing")
    expect("A later sample at 09:07:49 did not log connections." in mixed_html, "later sample was hidden")
    expect(">16.4<" in mixed_html and "This sample did not log connections." in mixed_html, "later server load missing")
    expect(not prefer_flow_snapshot(10, 8), "an older snapshot must not hide the log")
    expect(prefer_flow_snapshot(10, 10), "a snapshot written with the log is preferred")
    expect(prefer_flow_snapshot(None, 10), "a snapshot is used when the log is missing")
    expect(not prefer_flow_snapshot(10, None), "a missing snapshot falls back to the log")
    live_html = flow_body()
    expect("Generators and game servers" in live_html, "live flow heading missing")
    expect(IP_RE.search(live_html) is None, "live flow contains an address")
    dry = column_state({"1246,37": 68, "8,8": 0}, 1246, 37)
    expect(dry["state"] == "ground" and dry["y"] == 68 and "y" in dry, "measured ground height")
    wet = column_state({"8,8": 0}, 8, 8)
    expect(wet["state"] == "water" and "y" not in wet, "open water is not a teleport height")
    missing = column_state({}, 3, 4)
    expect(missing["state"] == "unmeasured" and "y" not in missing, "a missing column stays unmeasured")
    expect(IP_RE.search(drawn) is None, "map contains an address")
    expect(IP_RE.search(json.dumps(sample)) is None, "positions contain an address")


def read_changeable_text(text: str) -> str:
    if text and not text.endswith("\n") and "\n" in text:
        return text[: text.rfind("\n") + 1]
    return text


def main():
    self_check()
    if len(sys.argv) > 1:
        if sys.argv[1] == "--check":
            print("check ok")
            return
        raise SystemExit("unknown argument")
    if already_serving():
        print("already serving http://127.0.0.1:8768/")
        return
    serve()


if __name__ == "__main__":
    main()
