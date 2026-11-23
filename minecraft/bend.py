"""One bend rule for the mesh drivers, the coefficient ladder, and the progress page.

A bend is a 180-second hold that stayed hot the whole way, followed by a count
the game servers could not take. A 60-second tick or a kernel OOM stops that
climb. It is not a bend. One hot sample is not a bend. A generator that went
quiet is not a bend.
"""

from __future__ import annotations

LOAD_LINE = 1.8
CPU_LINE = 90.0
STOP_MARKERS = (
    "60s tick",
    "60.00s",
    "60 second",
    "watchdog",
    "oom-kill",
    "oom kill",
    "kernel oom",
    "outofmemory",
    "java heap space",
)
NOT_A_REFUSAL = (
    "max runtime",
    "21600",
    "harness",
    "index window",
    "cgroup",
    "spawn",
    "loadgen",
    "missing age",
)


def _num(value):
    if isinstance(value, bool) or value in (None, ""):
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def sample_hot(load1, cpu) -> bool:
    load_v = _num(load1)
    cpu_v = _num(cpu)
    return (load_v is not None and load_v >= LOAD_LINE) or (cpu_v is not None and cpu_v >= CPU_LINE)


def series_stayed_hot(samples) -> bool:
    """True only when every sample in the hold is hot.

    `samples` is a list of `{load, cpu}` points, or a metrics `per` dict whose
    `load` and `cpu` lists are the hold. Empty is not hot. The max of the list
    is not the test.
    """
    points = list(_points(samples))
    if not points:
        return False
    return all(sample_hot(load, cpu) for load, cpu in points)


def _points(samples):
    if isinstance(samples, dict):
        names = [name for name, slot in samples.items() if isinstance(slot, dict)]
        if not names:
            return
        length = 0
        for name in names:
            length = max(length, len(samples[name].get("load") or []), len(samples[name].get("cpu") or []))
        for index in range(length):
            loads = []
            cpus = []
            for name in names:
                slot = samples[name]
                load_list = slot.get("load") or []
                cpu_list = slot.get("cpu") or []
                if index < len(load_list):
                    load_v = _num(load_list[index])
                    if load_v is not None:
                        loads.append(load_v)
                if index < len(cpu_list):
                    cpu_v = _num(cpu_list[index])
                    if cpu_v is not None:
                        cpus.append(cpu_v)
            yield (max(loads) if loads else None, max(cpus) if cpus else None)
        return
    for item in samples or []:
        if isinstance(item, dict):
            yield item.get("load", item.get("load1")), item.get("cpu", item.get("cpu_busy"))
        elif isinstance(item, (tuple, list)) and len(item) >= 2:
            yield item[0], item[1]


def interval_stayed_hot(start_load, end_load, core_pct) -> bool:
    """Both load samples have to clear the line. One of them does not.

    A core at or above 90 percent covers the interval between those samples.
    It does not turn a single hot load sample into a hot interval.
    """
    start_v = _num(start_load)
    end_v = _num(end_load)
    loads_hot = (
        start_v is not None
        and end_v is not None
        and start_v >= LOAD_LINE
        and end_v >= LOAD_LINE
    )
    core = _num(core_pct)
    core_hot = core is not None and core >= CPU_LINE
    return loads_hot or core_hot


def is_stop(text: str) -> bool:
    low = (text or "").lower()
    return any(marker in low for marker in STOP_MARKERS)


def game_refused(binding: str, note: str) -> bool:
    """The next count failed because the game servers could not take it."""
    if (binding or "").strip().lower() != "game":
        return False
    low = (note or "").lower()
    if is_stop(low):
        return False
    if any(phrase in low for phrase in NOT_A_REFUSAL):
        return False
    return True


def is_bend(*, hot_whole: bool, next_result: str, next_binding: str, next_note: str) -> bool:
    if not hot_whole:
        return False
    if (next_result or "").strip().lower() == "clean":
        return False
    return game_refused(next_binding, next_note)


def classify_version(digests, known=None) -> dict:
    """digests is one sha per game server, or None when that host could not be read.

    known maps a sha256 to the commit that built it.
    """
    known = {str(sha): str(commit) for sha, commit in (known or {}).items() if sha and commit}
    blank = {"mod_commit": "", "mod_sha256": "", "version_certainty": "unknown"}
    if not digests:
        return blank
    if any(item in (None, "") for item in digests):
        return {"mod_commit": "", "mod_sha256": "", "version_certainty": "unread"}
    unique = sorted(set(digests))
    if len(unique) > 1:
        return {"mod_commit": "", "mod_sha256": " ".join(unique), "version_certainty": "mixed"}
    sha = unique[0]
    commit = known.get(sha, "")
    if commit:
        return {"mod_commit": commit, "mod_sha256": sha, "version_certainty": "measured"}
    return {"mod_commit": "", "mod_sha256": sha, "version_certainty": "recorded"}


def truthy(value) -> bool:
    return str(value or "").strip().lower() in ("1", "true", "yes")
