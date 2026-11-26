#!/usr/bin/env python3
"""Plays one of the server mod's two-server scenarios on a pair of a running mesh's servers, under whatever load the
mesh carries.

A scenario is the mod's tools/cluster-tests/<name>.west and .east: script steps for the server west of a line and the
one east of it, written for a line at block x = 2000. A mesh planned with a line there (mesh.py plan, a cell edge at
chunk 125) has such a pair in every row; the pair to use is the one whose chunks hold the scenario's stage (its z).

    scenario.py --run <mesh run> --tests <mod>/tools/cluster-tests --scenario blocks --west s0204 --east s0205
    scenario.py ... --scenario handoff --west west --east east --client-script <mod>/tools/vanilla-client.sh

What it does: puts the test players the scenario names on the west server (see home), then appends each side's steps to that server's step file (the servers read it as it grows), then follows both
logs. The steps' notes drive it as they drive the mod's own runner: "note client <port> [user]" starts a game client
here that joins the west server (port 25599) or the east one (25600), "note expect <user> <seconds> <text>" wants that
text in the client's log in time, "note done" ends a side. Shots and bursts are skipped (no screen is assumed). A
scenario that starts or stops a server, or asks for the admin tool, is refused before anything is sent: a mesh under
load is not restarted for a test. With --pair, a scenario whose .args set identity=fake is refused too.

    scenario.py --pair pair.json --tests <mod>/tools/cluster-tests --scenario handoff --west test-mc-p5-4 --east test-mc-p5-5 --names Tester1=A,Tester2=B --no-home --client-script ...

--pair is one rebuilt pair, not a mesh. The json names dir, log and steps, and each server's id, ssh, host and port.
The client joins that port. The 25599 in a step only chooses the west side. Do not commit the json: it holds addresses.
The seam pair is not on Tailscale, so --pair connects with ordinary ssh (CK_SSH_KEY, or the cluster key if that file exists).
A mesh --run still uses tailscale ssh.

A wait a server's script gave up on ("not met after") is a miss; the scenario passes with none, both sides done.
Prints one JSON line and exits 0 on a pass.
"""
import argparse
import concurrent.futures
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

REMOTE_DIR = "/opt/mc-mesh"
REMOTE_LOG = "/opt/mc-mesh/server.log"
REMOTE_STEPS = "/opt/mc-mesh/steps.txt"
UNSUPPORTED = ("start", "stop", "append", "admin")
PAIR = False


def ssh(server, command, timeout=60, input=None):
    # Mesh hosts are on Tailscale. The seam pair is not, so --pair uses ordinary ssh to the address the caller put in the json.
    if PAIR:
        key = os.environ.get("CK_SSH_KEY")
        if not key:
            fallback = Path.home() / ".local/state/cks-test-mc-cluster/id_ed25519"
            key = str(fallback) if fallback.is_file() else None
        argv = ["ssh", "-o", "BatchMode=yes", "-o", "IdentitiesOnly=yes"]
        if key:
            argv += ["-i", key]
        argv += [f"ubuntu@{server.get('ssh') or server.get('host')}", command]
    else:
        argv = ["tailscale", "ssh", f"ubuntu@{server['ssh']}", command]
    result = subprocess.run(argv, capture_output=True, text=True, timeout=timeout, input=input)
    if result.returncode != 0:
        raise RuntimeError(f"{server['id']}: {result.stderr.strip()[-300:] or result.stdout.strip()[-300:]}")
    return result.stdout


def steps_of(path, token):
    out = []
    for line in path.read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            out.append(line.replace("$RUN", token))
    return out


TRIPLES = {"console fill", "console setblock", "?state", "watch", "useitem", "?contains", "?!contains", "contents", "use", "rightclick",
           "?respawnat", "?position", "console summon", "console setworldspawn", "face", "console clone", "?bees", "?!bees"}
PAIRS = {"tp", "console forceload", "?land", "?near", "walkto", "?none", "spawn", "toss", "place", "break", "bot", "?home", "?back"}
NUMBER = re.compile(r"^-?\d+(\.\d+)?$")


def shift_z(line, dz, unshifted):
    """A step with every z moved by dz blocks: a scenario's stage played in another row of the mesh. Steps are told
    apart by their first words (which name x y z, which x z); an x is a number between 1880 and 2300, where every
    scenario's stage is. A step with such a number that is neither kind is left as it is and listed."""
    def moved(text):
        value = float(text) + dz
        return str(int(value)) if "." not in text else f"{value:.1f}"

    line = re.sub(r"\bz=(-?\d+(?:\.\d+)?)", lambda m: "z=" + moved(m.group(1)), line)
    words = line.split(" ")
    kind = words[0] if words[0] != "console" else " ".join(words[:2])
    step = 3 if kind in TRIPLES else 2 if kind in PAIRS else 0
    i = 0
    seen = False
    while i < len(words):
        if NUMBER.match(words[i]) and 1880 <= float(words[i]) <= 2300:
            seen = True
            at = i + step - 1
            if step and at < len(words) and all(NUMBER.match(w) for w in words[i:at + 1]):
                words[at] = moved(words[at])
                i = at + 1
                continue
        i += 1
    if seen and not step:
        unshifted.append(line)
    return " ".join(words)


# On this pair the line is block x 2000 and the margin is 8. A teleport to 2012 crossed.
# A step aimed at 2009 did not. Coordinates already at or past 2012 stay. Coordinates still
# west of the margin (below 2008), including the band the handoff scenario stands in, stay.
CROSS_X = 2012
CROSS_FROM = 2008
CROSS_PAIRS = PAIRS | {"drive"}


def nudge_cross_x(line, nudged):
    """Land a step that means cross east at x 2012, the teleport that crossed on this pair."""
    words = line.split(" ")
    kind = words[0] if words[0] != "console" else " ".join(words[:2])
    step = 3 if kind in TRIPLES else 2 if kind in CROSS_PAIRS else 0
    if not step:
        return line
    i = 0
    while i < len(words):
        if NUMBER.match(words[i]) and 1880 <= float(words[i]) <= 2300:
            at = i + step - 1
            if at < len(words) and all(NUMBER.match(w) for w in words[i:at + 1]):
                x = float(words[i])
                if CROSS_FROM <= x < CROSS_X:
                    was = words[i]
                    words[i] = str(CROSS_X) if "." not in was else f"{float(CROSS_X):.1f}"
                    nudged.append(f"{was} -> {words[i]}")
                i = at + 1
                continue
        i += 1
    return " ".join(words)


def follow(server, offset, pattern, seconds):
    """The first line of the server's log past that offset that matches, or None after that long."""
    until = time.time() + seconds
    while time.time() < until:
        try:
            text = ssh(server, f"tail -c +{offset + 1} {REMOTE_LOG} | grep -a -E '{pattern}' | head -3")
        except (RuntimeError, subprocess.TimeoutExpired):
            text = ""
        if text.strip():
            return text.strip().splitlines()[0]
        time.sleep(2)
    return None


def home_spot(west):
    return (west["minChunkX"] + west["maxChunkX"] + 1) * 8, (west["minChunkZ"] + west["maxChunkZ"] + 1) * 8


def home(mesh, sides, names, seconds):
    """Every scenario expects its test players on the west server, where a run of the mod's own two servers puts them
    first. Their records stay in CK between scenarios, wherever the last one left them, and a player who joins is sent
    to the server whose chunks their record names. So: each joins through the west server's forwarder (and lands on
    whichever server holds their place), every server is told to move them over the middle of the west server's
    chunks (only the one that has them can, and it does not hand them on from there: they are high up, unhurt and
    falling slowly), they leave, which saves that place, and join again, now the west server's, which sets them on
    the ground. Returns what went wrong, or None."""
    west = sides["west"]
    spot_x, spot_z = home_spot(west)
    token = time.strftime("%H%M%S")
    offset = int(ssh(west, f"stat -c %s {REMOTE_LOG}").strip())
    ssh(west, f"cat >> {REMOTE_STEPS}", input="\n".join([f"client {name} 25599" for name in names] + ["wait 15", f"note joined {token}"]) + "\n")
    if follow(west, offset, f"note joined {token}", 90) is None:
        return f"{west['id']} did not start the test players' clients (its script may be busy with an earlier scenario)"
    moves = []
    for i, name in enumerate(names):
        moves += [f"console effect give {name} minecraft:resistance 90 255 true", f"console effect give {name} minecraft:slow_falling 90 0 true",
                  f"console tp {name} {spot_x + 2 * i}.5 200 {spot_z}.5"]

    def move(server):
        try:
            ssh(server, f"cat >> {REMOTE_STEPS}", input="\n".join(moves) + "\n")
        except (RuntimeError, subprocess.TimeoutExpired):
            pass

    with concurrent.futures.ThreadPoolExecutor(max_workers=32) as pool:
        list(pool.map(move, mesh["servers"]))
    steps = ["wait 6"] + [f"quit {name}" for name in names] + ["wait 8", f"waitlimit {seconds}"]
    for i, name in enumerate(names):
        steps += [f"client {name} 25599", f"?home {name} {spot_x + 2 * i} {spot_z}", "wait 2", f"quit {name}", f"?gone {name}", f"?released {name}"]
    steps += ["waitlimit 120", f"note homed {token}"]
    ssh(west, f"cat >> {REMOTE_STEPS}", input="\n".join(steps) + "\n")
    line = follow(west, offset, f"note homed {token}|not met after", seconds * len(names) + 120)
    if line is None:
        return f"{west['id']} did not finish putting {', '.join(names)} home"
    if "not met after" in line:
        return f"could not put {', '.join(names)} home on {west['id']}: {line[-160:]}"
    return None


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--run", help="a mesh run directory (mesh.json). Not used with --pair")
    p.add_argument("--pair", help="json for one seam pair: dir, log, steps, and servers with id, ssh, host, port")
    p.add_argument("--tests", required=True, help="the mod's tools/cluster-tests folder")
    p.add_argument("--scenario", required=True)
    p.add_argument("--west", required=True, help="the id of the server west of the line")
    p.add_argument("--east", required=True)
    p.add_argument("--client-script", help="starts a game client: <script> <name> <port>, with CK_MC_HOST the server's address")
    p.add_argument("--out", help="a folder for the clients' logs and this run's record (default: <run>/scenarios/<scenario>)")
    p.add_argument("--timeout", type=int, default=1500)
    p.add_argument("--names", help="a prefix: the scenario's Tester1..4 play as <prefix>1..4, players new to the world, and nobody is put home first; or names one by one, Tester1=Cam3,Tester2=Cam4")
    p.add_argument("--shift-z", type=int, default=0, help="move every z in the steps by so many blocks: the scenario's stage in another row")
    p.add_argument("--dry-run", action="store_true", help="with --shift-z: print the moved steps and those left alone, and send nothing")
    p.add_argument("--drop", help="a regular expression: steps that match are left out (and listed in the result)")
    p.add_argument("--no-home", action="store_true", help="do not put the scenario's test players on the west server first")
    p.add_argument("--home-seconds", type=int, default=40)
    args = p.parse_args()
    if not args.run and not args.pair:
        p.error("name a mesh --run or a seam --pair")

    global REMOTE_DIR, REMOTE_LOG, REMOTE_STEPS, PAIR
    if args.pair:
        PAIR = True
        pair = json.loads(Path(args.pair).read_text())
        REMOTE_DIR = pair.get("dir", "/opt/mc")
        REMOTE_LOG = pair.get("log", f"{REMOTE_DIR}/logs/latest.log")
        REMOTE_STEPS = pair.get("steps", f"{REMOTE_DIR}/steps.txt")
        by_id = {s["id"]: s for s in pair["servers"]}
        mesh = {"servers": pair["servers"]}
        run = Path(args.out).parent if args.out else Path(args.pair).resolve().parent
    else:
        run = Path(args.run)
        mesh = json.loads((run / "mesh.json").read_text())
        by_id = {s["id"]: s for s in mesh["servers"]}
    sides = {"west": by_id[args.west], "east": by_id[args.east]}
    for server in sides.values():
        server.setdefault("port", 25565 if args.pair else server.get("port"))
    tests = Path(args.tests)
    args_file = tests / f"{args.scenario}.args"
    if args.pair and args_file.exists() and re.search(r"(?m)^identity=fake\b", args_file.read_text()):
        print(json.dumps({"scenario": args.scenario, "result": "refused", "why": "identity=fake is the in-memory CK, not this fabric"}))
        return 2
    out = Path(args.out) if args.out else run / "scenarios" / args.scenario
    out.mkdir(parents=True, exist_ok=True)
    token = time.strftime("r%H%M%S")
    steps = {side: steps_of(tests / f"{args.scenario}.{side}", token) for side in sides}
    # A scenario ends by sending its players to 0, 0: the west server's spawn where it was written, the default
    # server's chunks on a mesh. Here they go to the middle of the west server's chunks instead.
    home_x, home_z = home_spot(sides["west"])
    for side in steps:
        steps[side] = [re.sub(r"^tp (\S+) 0 0$", rf"tp \1 {home_x} {home_z}", line) for line in steps[side]]
    unshifted = []
    if args.shift_z:
        for side in steps:
            steps[side] = [shift_z(line, args.shift_z, unshifted) for line in steps[side]]
        if args.dry_run:
            for side in steps:
                print(f"--- {side}")
                print("\n".join(steps[side]))
            print("--- steps with a coordinate that were not moved")
            print("\n".join(unshifted))
            return 0
    dropped = []
    if args.drop:
        # A step written for two servers alone: "?relays 1" (exactly one relay on this server) can never be met on a
        # mesh server that relays dozens of other players.
        for side in steps:
            dropped += [f"{side}: {line}" for line in steps[side] if re.search(args.drop, line)]
            steps[side] = [line for line in steps[side] if not re.search(args.drop, line)]
    if args.names:
        # Players nobody has been: each starts where it first joins (the west server's chunks), so nothing has to be
        # put back between scenarios. Tester1 becomes <prefix>1, and so on; the names must be on the whitelist.
        if "=" in args.names:
            # Names given one by one: Tester1=Cam3,Tester2=Cam4.
            given = dict(pair.split("=", 1) for pair in args.names.split(","))
            for side in steps:
                steps[side] = [re.sub(r"\bTester[0-9]+\b", lambda m: given.get(m.group(0), m.group(0)), line) for line in steps[side]]
        else:
            for side in steps:
                steps[side] = [re.sub(r"\bTester([0-9]+)\b", rf"{args.names}\1", line) for line in steps[side]]
    if args.pair:
        # The step's 25599 or 25600 chooses the side. On this pair the headless client joins that side's own port.
        for side in steps:
            join = str(sides[side].get("port") or 25565)
            rewritten = []
            for line in steps[side]:
                words = line.split()
                if words and words[0] == "client" and len(words) >= 3 and words[2] in ("25599", "25600"):
                    words[2] = join
                    line = " ".join(words)
                rewritten.append(line)
            steps[side] = rewritten
        # This pair's east chunks start at block x 2000. A cross has to finish at 2012, as handoff's
        # teleport did. 2008 and 2009 are still short of that. The step files stay written for the
        # local cluster; only this pair's copy of a step moves.
        nudged = []
        for side in steps:
            steps[side] = [nudge_cross_x(line, nudged) for line in steps[side]]
    else:
        nudged = []
    for side, lines in steps.items():
        for line in lines:
            words = line.split()
            if words[0] == "note" and len(words) > 1 and words[1] in UNSUPPORTED:
                print(json.dumps({"scenario": args.scenario, "result": "refused", "why": f"its {side} side has the step: {line}"}))
                return 2
            if words[0] == "note" and len(words) > 1 and words[1] == "client" and not args.client_script:
                print(json.dumps({"scenario": args.scenario, "result": "refused", "why": "it starts a game client and no --client-script was given"}))
                return 2

    started = time.time()
    names = sorted(set(re.findall(r"\bTester[0-9]+\b", "\n".join(steps["west"] + steps["east"]))))
    if names and not args.no_home and not args.names:
        # A player whose last client is still closing is turned away once ("logged in from another location"), and is
        # then not there to be moved: the second time round they are.
        for _ in range(3):
            problem = home(mesh, sides, names, args.home_seconds)
            if not problem:
                break
        if problem:
            print(json.dumps({"scenario": args.scenario, "result": "not run", "why": problem}))
            return 2

    # Where each log stands now: only what comes after belongs to this run.
    offset = {side: int(ssh(server, f"stat -c %s {REMOTE_LOG}").strip()) for side, server in sides.items()}
    for side, server in sides.items():
        ssh(server, f"cat >> {REMOTE_STEPS}", input="\n".join(steps[side]) + "\n")

    first_player = "Tester1"
    if args.names:
        first_player = dict(pair.split("=", 1) for pair in args.names.split(",")).get("Tester1", "Tester1") if "=" in args.names else args.names + "1"
    noted = []
    done = set()
    misses = []
    notes_seen = {side: 0 for side in sides}
    clients = {}
    expects = []
    carried = {side: "" for side in sides}
    log_all = {side: [] for side in sides}

    def start_client(side_port, user):
        side = "east" if side_port == "25600" else "west"
        server = sides[side]
        old = clients.pop(user, None)
        if old:
            old["process"].terminate()
        log = open(out / f"client-{user}.log", "ab")
        env = dict(os.environ, CK_MC_HOST=server["host"], CK_MC_RESOLUTION="1280x720")
        process = subprocess.Popen([args.client_script, user, str(server["port"])], stdout=log, stderr=subprocess.STDOUT, env=env, start_new_session=True)
        clients[user] = {"process": process, "log": out / f"client-{user}.log"}

    try:
        while time.time() - started < args.timeout and len(done) < len(sides):
            for side, server in sides.items():
                try:
                    text = ssh(server, f"tail -c +{offset[side] + 1} {REMOTE_LOG} | head -c 4000000")
                except (RuntimeError, subprocess.TimeoutExpired):
                    continue
                offset[side] += len(text.encode("utf-8", errors="replace"))
                text = carried[side] + text
                lines = text.split("\n")
                carried[side] = lines.pop()
                for line in lines:
                    if "Server script" in line or "not met after" in line:
                        log_all[side].append(line)
                    # Only a wait that ran out, as in the mod's own runner. A step that "failed" (a tp of a player who is
                    # not on this server yet) is how some scenarios mark time, and passes there too; those are kept as notes.
                    if "not met after" in line:
                        misses.append(f"{side}: {line[-240:]}")
                    elif re.search(r"Server script: .* failed$", line) or "Server script: unknown step" in line:
                        noted.append(f"{side}: {line[-200:]}")
                    found = re.search(r"Server script: note (.*)$", line)
                    if not found:
                        continue
                    words = found.group(1).split()
                    notes_seen[side] += 1
                    if words[0] == "done":
                        done.add(side)
                    elif words[0] == "client":
                        start_client(words[1], words[2] if len(words) > 2 else first_player)
                    elif words[0] == "expect":
                        expects.append({"user": words[1], "until": time.time() + float(words[2]), "text": " ".join(words[3:])})
            for expect in list(expects):
                client = clients.get(expect["user"])
                heard = client and expect["text"] in client["log"].read_text(errors="replace")
                if heard:
                    expects.remove(expect)
                elif time.time() > expect["until"]:
                    expects.remove(expect)
                    misses.append(f"client {expect['user']} did not show: {expect['text']}")
            if misses and len(done) < len(sides):
                # A wait that ran out is rarely the last: what is left would hold the pair's scripts through every
                # later wait's limit. The cleanup at a scenario's end goes with it, so the caller puts back any
                # cluster-wide rule the scenario changed.
                break
            time.sleep(2)
    finally:
        for client in clients.values():
            try:
                os.killpg(client["process"].pid, 15)
            except OSError:
                pass
        for side in sides:
            (out / f"{side}-script.log").write_text("\n".join(log_all[side]) + "\n")

    seconds = round(time.time() - started)
    if len(done) < len(sides):
        misses.append(f"not done after {seconds} s: {', '.join(sorted(set(sides) - done))}")
        # What is left of it would hold the pair's scripts until every wait had run out.
        for server in sides.values():
            try:
                ssh(server, f"echo abort >> {REMOTE_STEPS}")
            except (RuntimeError, subprocess.TimeoutExpired):
                pass
    record = {"scenario": args.scenario, "west": args.west, "east": args.east, "result": "pass" if not misses else "fail", "seconds": seconds,
              "steps": {side: len(lines) for side, lines in steps.items()}, "misses": misses[:20], "dropped": dropped, "shiftZ": args.shift_z, "unshifted": unshifted[:20], "nudgedX": nudged[:20], "stepsThatFailed": len(noted), "at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(started))}
    (out / "result.json").write_text(json.dumps(record, indent=1) + "\n")
    print(json.dumps(record))
    return 0 if not misses else 1


if __name__ == "__main__":
    sys.exit(main())
