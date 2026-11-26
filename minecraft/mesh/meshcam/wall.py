#!/usr/bin/env python3
"""A wall of game windows on a Mac, recorded from the screen: several real clients filmed side by side, in step.

    wall.py place  --names Cam1,Cam2,Cam3,Cam4 [--size 1920x1080] [--cols 2]
    wall.py front  --names Cam1,Cam2,Cam3,Cam4
    wall.py record --names Cam1,Cam2,Cam3,Cam4 --seconds 120 --out <folder>/<take>
    wall.py split  <folder>/<take>

`place` finds each client's process (the one whose command line names its game folder, <main>/<name>, as
client.sh starts it), puts the windows in a grid in the middle of the screen and brings them in front of every other
app's (window.swift beside this file, built on first use; Accessibility for the app this runs in). `front` only
brings them forward again, before a take.

`record` films the grid's rectangle at 60 frames a second with ffmpeg's screen capture (Screen Recording for the
app this runs in) into <take>.mp4, and writes <take>.json: where each window's picture is inside the movie, and when
the take began. One movie for all the windows keeps them in step to the frame. The screen must be unlocked and the
windows uncovered: what is filmed is the screen.

`split` cuts each window's own movie out of a take (<take>-<name>.mp4), without its title bar.

The display is assumed to run unscaled (a point is a pixel), as a 5K display does at its native resolution; on a
scaled display pass --scale 2. The frame rate a game ran at is not the movie's: read it from the game's F3 overlay
or from the camera mod's stats.csv.
"""
import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

TITLE_BAR = 28
MENU_BAR = 31


def pids(main, names):
    out = {}
    listing = subprocess.run(["ps", "-axo", "pid=,command="], capture_output=True, text=True).stdout.splitlines()
    for name in names:
        folder = f"{main}/{name}"
        for line in listing:
            pid, _, command = line.strip().partition(" ")
            # The game itself, not the launcher that started it.
            if f"{folder} " in command + " " and "/bin/java" in command.split(" ")[0]:
                out[name] = int(pid)
                break
    return out


def screen():
    """The main display's size in points."""
    text = subprocess.run(["system_profiler", "SPDisplaysDataType"], capture_output=True, text=True).stdout
    import re
    found = re.search(r"Resolution: (\d+) x (\d+)", text)
    if not found:
        sys.exit("wall: could not read the display's resolution")
    return int(found.group(1)), int(found.group(2))


def layout(names, size, cols, scale):
    """Each window's frame in points from the screen's top left, and the rectangle around them all."""
    width, height = (int(v) for v in size.split("x"))
    screen_w, screen_h = (v // scale for v in screen())
    rows = (len(names) + cols - 1) // cols
    frame_h = height + TITLE_BAR
    total_w, total_h = cols * width, rows * frame_h
    if total_w > screen_w or total_h > screen_h - MENU_BAR:
        sys.exit(f"wall: {cols} by {rows} windows of {size} need {total_w}x{total_h}, and the screen has {screen_w}x{screen_h - MENU_BAR}")
    x0 = (screen_w - total_w) // 2
    y0 = MENU_BAR + (screen_h - MENU_BAR - total_h) // 2
    frames = {}
    for i, name in enumerate(names):
        frames[name] = {"x": x0 + (i % cols) * width, "y": y0 + (i // cols) * frame_h, "w": width, "h": frame_h}
    return frames, {"x": x0, "y": y0, "w": total_w, "h": total_h}


def tool():
    """The window mover, built from window.swift beside this file the first time it is wanted."""
    here = Path(__file__).resolve().parent
    binary = here / "build" / "window"
    source = here / "window.swift"
    if not binary.exists() or binary.stat().st_mtime < source.stat().st_mtime:
        binary.parent.mkdir(exist_ok=True)
        built = subprocess.run(["swiftc", "-O", str(source), "-o", str(binary)], capture_output=True, text=True)
        if built.returncode != 0:
            sys.exit("wall: could not build window.swift: " + built.stderr.strip()[-300:])
    return str(binary)


def cmd_place(args):
    names = args.names.split(",")
    found = pids(args.main, names)
    missing = [n for n in names if n not in found]
    if missing:
        sys.exit(f"wall: no game process for {', '.join(missing)} under {args.main}")
    frames, _ = layout(names, args.size, args.cols, args.scale)
    for name in names:
        f = frames[name]
        # Twice: the first move can land before the game has finished sizing its own window.
        for _ in range(2):
            done = subprocess.run([tool(), "place", str(found[name]), str(f["x"]), str(f["y"]), str(f["w"]), str(f["h"])], capture_output=True, text=True)
            time.sleep(0.3)
        print(f"{name}: pid {found[name]} at {f['x']},{f['y']} {f['w']}x{f['h']}" + ("" if done.returncode == 0 else f" (place failed: {(done.stdout + done.stderr).strip()[:120]})"))
    front(found, names)


def front(found, names):
    """Each game's window in front of every other app's: what is filmed is the screen."""
    for name in names:
        subprocess.run([tool(), "front", str(found[name])], capture_output=True, text=True)
        time.sleep(0.4)


def cmd_front(args):
    names = args.names.split(",")
    front(pids(args.main, names), names)


def cmd_record(args):
    names = args.names.split(",")
    frames, box = layout(names, args.size, args.cols, args.scale)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    s = args.scale
    crop = f"crop={box['w'] * s}:{box['h'] * s}:{box['x'] * s}:{box['y'] * s}"
    # H.264 in hardware takes at most 4096 across: a larger wall is HEVC.
    codec = "h264_videotoolbox" if box["w"] * s <= 4096 and box["h"] * s <= 2304 else "hevc_videotoolbox"
    if not args.no_front:
        front(pids(args.main, names), names)
    began = time.time()
    command = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "avfoundation", "-framerate", "60", "-capture_cursor", "0", "-pixel_format", "nv12",
               "-i", f"Capture screen {args.screen}", "-t", str(args.seconds), "-vf", crop, "-c:v", codec, "-b:v", args.bitrate, "-r", "60",
               "-movflags", "+faststart", str(out) + ".mp4"]
    done = subprocess.run(command, capture_output=True, text=True)
    windows = {name: {"x": (f["x"] - box["x"]) * s, "y": (f["y"] - box["y"] + TITLE_BAR) * s, "w": f["w"] * s, "h": (f["h"] - TITLE_BAR) * s}
               for name, f in frames.items()}
    Path(str(out) + ".json").write_text(json.dumps({"began": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(began)), "seconds": args.seconds,
                                                    "windows": windows, "note": args.note or ""}, indent=1) + "\n")
    if done.returncode != 0 or not Path(str(out) + ".mp4").exists():
        sys.exit("wall: the recording failed: " + done.stderr.strip()[-300:])
    probe = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=width,height,nb_frames,duration", "-of", "csv=p=0",
                            str(out) + ".mp4"], capture_output=True, text=True).stdout.strip()
    print(f"{out}.mp4: {probe} (width,height,seconds,frames), began {time.strftime('%H:%M:%SZ', time.gmtime(began))}")


def cmd_split(args):
    take = Path(args.take)
    meta = json.loads(Path(str(take) + ".json").read_text())
    for name, w in meta["windows"].items():
        target = f"{take}-{name}.mp4"
        done = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", str(take) + ".mp4", "-vf", f"crop={w['w']}:{w['h']}:{w['x']}:{w['y']}",
                               "-c:v", "h264_videotoolbox", "-b:v", args.bitrate, "-movflags", "+faststart", target], capture_output=True, text=True)
        print(target if done.returncode == 0 else f"{target}: failed: {done.stderr.strip()[-200:]}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    def common(p):
        p.add_argument("--names", required=True, help="the clients, in reading order: Cam1,Cam2,Cam3,Cam4")
        p.add_argument("--size", default="1920x1080", help="each window's picture, in points")
        p.add_argument("--cols", type=int, default=2)
        p.add_argument("--scale", type=int, default=1, help="pixels a point: 1 on an unscaled display, 2 on a Retina one")

    p = sub.add_parser("place")
    common(p)
    p.add_argument("--main", default=os.environ.get("MESHCAM_MAIN", str(Path.home() / ".local/state/meshcam")), help="where client.sh keeps the games' folders")
    p.set_defaults(run=cmd_place)

    p = sub.add_parser("front")
    p.add_argument("--names", required=True)
    p.add_argument("--main", default=os.environ.get("MESHCAM_MAIN", str(Path.home() / ".local/state/meshcam")))
    p.set_defaults(run=cmd_front)

    p = sub.add_parser("record")
    common(p)
    p.add_argument("--seconds", type=int, required=True)
    p.add_argument("--out", required=True, help="the take's path without its ending")
    p.add_argument("--screen", type=int, default=0)
    p.add_argument("--bitrate", default="30M")
    p.add_argument("--note")
    p.add_argument("--main", default=os.environ.get("MESHCAM_MAIN", str(Path.home() / ".local/state/meshcam")))
    p.add_argument("--no-front", action="store_true", help="leave the windows' order as it is")
    p.set_defaults(run=cmd_record)

    p = sub.add_parser("split")
    p.add_argument("take")
    p.add_argument("--bitrate", default="16M")
    p.set_defaults(run=cmd_split)

    args = parser.parse_args()
    args.run(args)


if __name__ == "__main__":
    main()
