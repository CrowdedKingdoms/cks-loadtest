#!/usr/bin/env python3
"""Progress page for the four-server and 16-server mesh climbs.

Reads the two climb CSVs and re-reads them. The chart section also shows
the live connected counts on the four-server and 16-server meshes, read
from harness status. Does not place bots or start a climb. Binds 127.0.0.1:8768 only.
"""

from __future__ import annotations

import csv
import io
import json
import math
import os
import re
import signal
import sys
import threading
import time
import urllib.error
import urllib.request
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
    mark_bend(four_rows)
    mark_bend(sixteen_rows)
    return {
        "fit": try_fit(four_rows, sixteen_rows),
        "four": climb_state("Four-server climb", 4, "#1d4ed8", four_rows, four_err, four_stamp, FOUR_CSV.name),
        "sixteen": climb_state("16-server climb", 16, "#c2410c", sixteen_rows, sixteen_err, sixteen_stamp, SIXTEEN_CSV.name),
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


def render_live(state: dict) -> str:
    return "\n".join([
        fit_html(state["fit"]),
        climb_section(state["four"]),
        climb_section(state["sixteen"]),
        capacity_section(state),
        seam_section(),
        f'<p class="foot">Read both climb files at {esc(state["read_at"])}. Bound to 127.0.0.1:8768.</p>',
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


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        code = str(args[1]) if len(args) >= 2 else ""
        if code and code != "200":
            sys.stderr.write(f"{self.command} {code}\n")

    def do_GET(self):
        path = self.path.split("?", 1)[0]
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
    _pop_ready.wait(timeout=30)
    try:
        httpd = ThreadingHTTPServer(BIND, Handler)
    except OSError:
        raise SystemExit("could not bind 127.0.0.1:8768")
    httpd.daemon_threads = True
    PID_PATH.write_text(f"{os.getpid()}\n")
    print("http://127.0.0.1:8768/", flush=True)
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
