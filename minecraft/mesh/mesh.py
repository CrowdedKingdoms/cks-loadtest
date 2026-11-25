#!/usr/bin/env python3
"""Lay out, start, fill and watch a grid of CrowdyCraft servers that share one Minecraft world.

One run folder (--run) holds everything about a mesh: mesh.json (the plan), cluster.json (what every
server loads), payload/ (the files every server gets) and, for a local mesh, one folder per server.

  plan     write mesh.json and cluster.json for a grid of cols x rows servers and a default server
  payload  collect the files every server needs (mod, launcher, whitelist, ages, config)
  up       start the servers (this machine for a local plan, the plan's hosts otherwise)
  down     stop them
  watch    one table from every server's last meter line, and the sums
  say      run a console command on some or all servers
  bots     start the load client where it runs and add the bots, spread over the grid's servers
  botctl   send one control line to every load client and print the replies (stats, rally, ...)

Nothing here creates or deletes a machine (loadgen-fleet.mjs does), and no secret is written to a
file in git: the servers' crowdy.env comes from --secrets-env and stays in the run folder, mode 600.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import time
import uuid
from pathlib import Path

SEED = "7270301997446181"
GAME_PORT = 25565
LINK_PORT = 26565
CONTROL_PORT = 9100
REMOTE_DIR = "/opt/mc-mesh"
BOT_DIR = "/opt/ck-mc-load"


QUIET_UPGRADES = r"""# This machine lives a few hours. Ubuntu upgrades packages by itself soon after a first boot, and needrestart then
# restarts the services that use an upgraded library, ours among them: every player on the server is dropped.
sudo mkdir -p /etc/needrestart/conf.d
echo "\$nrconf{restart} = 'l'; \$nrconf{kernelhints} = 0;" | sudo tee /etc/needrestart/conf.d/90-mesh.conf >/dev/null
sudo systemctl disable --now apt-daily.timer apt-daily-upgrade.timer >/dev/null 2>&1 || true
"""


def offline_uuid(name):
    """Java's UUID.nameUUIDFromBytes("OfflinePlayer:" + name): the id an offline-mode server gives that name."""
    digest = bytearray(hashlib.md5(("OfflinePlayer:" + name).encode()).digest())
    digest[6] = (digest[6] & 0x0F) | 0x30
    digest[8] = (digest[8] & 0x3F) | 0x80
    return str(uuid.UUID(bytes=bytes(digest)))


def load(run):
    return json.loads((Path(run) / "mesh.json").read_text())


def grid_servers(mesh):
    return [s for s in mesh["servers"] if not s.get("default")]


# ----- machines -----

def cmd_hosts(args):
    """From loadgen-fleet.mjs's state files: hosts.txt for the game servers, generators.txt for the load clients."""
    run = Path(args.run)
    run.mkdir(parents=True, exist_ok=True)
    for state, out in ((args.game_state, "hosts.txt"), (args.loadgen_state, "generators.txt")):
        if not state:
            continue
        vms = []
        for path in state.split(","):
            vms += json.loads(Path(path).read_text())["vms"]
        lines = []
        for vm in vms:
            if not vm.get("ip") or not vm.get("tailnetIp"):
                print(f"{vm['name']} has no address yet; left out")
                continue
            # name, the address servers and bots use (public, or the tailnet's with --tailnet), the address this machine uses
            lines.append(f"{vm['name']} {vm['tailnetIp'] if args.tailnet else vm['ip']} {vm['tailnetIp']}")
        (run / out).write_text("\n".join(lines) + "\n")
        print(f"{out}: {len(lines)} machines")
    # Who may reach the game and link ports besides the tailnet: every machine of the run, by its public address.
    allow = []
    for name in ("hosts.txt", "generators.txt"):
        if (run / name).exists():
            allow += [line.split()[1] for line in (run / name).read_text().splitlines() if line.strip()]
    (run / "allow.json").write_text(json.dumps(sorted(set(allow))))


def cmd_gen_install(args):
    """Builds the load client on every generator from this checkout's source and runs it as a service."""
    run = Path(args.run)
    gens = generators(run, load(run))
    source = Path(__file__).resolve().parents[1] / "cpp"
    archive = run / "ck-mc-load-src.tar"
    subprocess.run(["tar", "-cf", str(archive), "--exclude", "build", "-C", str(source), "."], check=True)

    def install(gen):
        with open(archive, "rb") as file:
            copy = subprocess.run(["tailscale", "ssh", f"ubuntu@{gen['ssh']}", "cat > /tmp/ck-mc-load-src.tar"], stdin=file, capture_output=True, timeout=300)
        if copy.returncode != 0:
            raise RuntimeError("copy: " + copy.stderr.decode(errors="replace").strip()[-200:])
        unit = "\n".join([
            "[Unit]", "Description=ck-mc-load", "After=network-online.target", "[Service]", f"WorkingDirectory={BOT_DIR}", "User=ubuntu",
            "LimitNOFILE=200000",
            f"ExecStart={BOT_DIR}/build/ck-mc-load --control {gen['ssh']}:{CONTROL_PORT} --threads {args.threads} --journal {BOT_DIR}/bots.journal",
            "Restart=on-failure", "RestartSec=2", f"StandardOutput=append:{BOT_DIR}/ck-mc-load.log", f"StandardError=append:{BOT_DIR}/ck-mc-load.log",
            "[Install]", "WantedBy=multi-user.target", ""])
        script = f"""set -euo pipefail
{QUIET_UPGRADES}if ! command -v cmake >/dev/null 2>&1 || ! command -v g++ >/dev/null 2>&1; then
  sudo DEBIAN_FRONTEND=noninteractive apt-get -o DPkg::Lock::Timeout=600 update -qq >/tmp/apt-build.log 2>&1
  sudo DEBIAN_FRONTEND=noninteractive apt-get -o DPkg::Lock::Timeout=600 install -y -qq cmake g++ make libssl-dev zlib1g-dev >>/tmp/apt-build.log 2>&1
fi
sudo systemctl stop ck-mc-load 2>/dev/null || true
sudo mkdir -p {BOT_DIR}
sudo chown -R ubuntu:ubuntu {BOT_DIR}
rm -rf {BOT_DIR}/src-tree && mkdir -p {BOT_DIR}/src-tree && tar -xf /tmp/ck-mc-load-src.tar -C {BOT_DIR}/src-tree
cmake -S {BOT_DIR}/src-tree -B {BOT_DIR}/build -DCMAKE_BUILD_TYPE=Release >/tmp/ck-mc-load-cmake.log 2>&1
cmake --build {BOT_DIR}/build -j 4 >/tmp/ck-mc-load-build.log 2>&1 || {{ tail -30 /tmp/ck-mc-load-build.log; exit 1; }}
{BOT_DIR}/build/protocol_test | tail -1
sudo tee /etc/systemd/system/ck-mc-load.service >/dev/null <<'UNIT'
{unit}UNIT
sudo systemctl daemon-reload
sudo systemctl restart ck-mc-load
sleep 1
systemctl is-active ck-mc-load
"""
        return ssh(gen, "bash -s", timeout=900, input=script).strip().splitlines()[-2:]

    done = each(gens, install, "gen-install", workers=len(gens))
    for name, lines in sorted(done.items()):
        print(name, " | ".join(lines))


# ----- plan -----

def cmd_plan(args):
    run = Path(args.run)
    run.mkdir(parents=True, exist_ok=True)
    names = {}
    for item in args.name_cell or []:
        where, name = item.split("=")
        col, row = (int(v) for v in where.split(","))
        names[(col, row)] = name
    hosts = None
    if args.hosts:
        hosts = [line.split() for line in Path(args.hosts).read_text().splitlines() if line.strip() and not line.startswith("#")]
        need = args.cols * args.rows + 1
        if len(hosts) < need:
            sys.exit(f"{args.hosts} names {len(hosts)} hosts; a {args.cols}x{args.rows} grid and its default need {need}")
    servers = []

    def place(index, sid):
        if hosts is None:
            return {"id": sid, "host": "127.0.0.1", "bind": "127.0.0.1", "port": args.local_port + index, "link": args.local_port + 1000 + index,
                    "ssh": None}
        row = hosts[index]
        # name, the address the servers and the bots use, and the address this machine reaches it by (ssh, a watching client)
        return {"id": sid, "host": row[1], "bind": "0.0.0.0", "port": GAME_PORT, "link": LINK_PORT, "ssh": row[2] if len(row) > 2 else row[1],
                "vm": row[0]}

    # The default runs everything outside the grid and holds no players: first, so it is the first host.
    out = place(0, "out")
    out["default"] = True
    servers.append(out)
    index = 1
    for row in range(args.rows):
        for col in range(args.cols):
            server = place(index, names.get((col, row), f"s{row:02d}{col:02d}"))
            server.update(col=col, row=row,
                          minChunkX=args.origin_chunk_x + col * args.cell, maxChunkX=args.origin_chunk_x + (col + 1) * args.cell - 1,
                          minChunkZ=args.origin_chunk_z + row * args.cell, maxChunkZ=args.origin_chunk_z + (row + 1) * args.cell - 1)
            servers.append(server)
            index += 1
    mesh = {"name": args.name, "cols": args.cols, "rows": args.rows, "cell": args.cell, "originChunkX": args.origin_chunk_x,
            "originChunkZ": args.origin_chunk_z, "margin": args.margin, "local": hosts is None, "servers": servers}
    (run / "mesh.json").write_text(json.dumps(mesh, indent=1))
    cluster = {"name": args.name, "margin_blocks": args.margin, "servers": []}
    for server in servers:
        entry = {"id": server["id"], "address": f"{server['host']}:{server['port']}", "internal_address": f"{server['host']}:{server['port']}",
                 "link": f"{server['host']}:{server['link']}"}
        if server.get("default"):
            entry["default"] = True
        else:
            entry["ranges"] = [{"dimension": "minecraft:overworld", "min_chunk_x": server["minChunkX"], "max_chunk_x": server["maxChunkX"],
                                "min_chunk_z": server["minChunkZ"], "max_chunk_z": server["maxChunkZ"]}]
        cluster["servers"].append(entry)
    (run / "cluster.json").write_text(json.dumps(cluster, indent=1))
    x0, z0, x1, z1 = bounds(mesh)
    print(f"{args.name}: {args.cols}x{args.rows} servers of {args.cell} chunks ({args.cell * 16} blocks), blocks x {x0}..{x1}, z {z0}..{z1}, "
          f"and a default; {'local' if hosts is None else 'on ' + args.hosts}")


def bounds(mesh):
    x0 = mesh["originChunkX"] * 16
    z0 = mesh["originChunkZ"] * 16
    return x0, z0, x0 + mesh["cols"] * mesh["cell"] * 16 - 1, z0 + mesh["rows"] * mesh["cell"] * 16 - 1


# ----- payload -----

def cmd_payload(args):
    run = Path(args.run)
    mesh = load(run)
    payload = run / "payload"
    if payload.exists():
        shutil.rmtree(payload)
    (payload / "mods").mkdir(parents=True)
    (payload / "config").mkdir()
    shutil.copy(args.mod_jar, payload / "mods" / "crowdycraft-server.jar")
    shutil.copy(args.fabric_api, payload / "mods" / "fabric-api.jar")
    shutil.copy(args.fabric_launch, payload / "fabric-server-launch.jar")
    shutil.copy(run / "cluster.json", payload / "cluster.json")
    (payload / "eula.txt").write_text("eula=true\n")
    names = [f"{args.bot_prefix}{i}" for i in range(1, args.bots + 1)] + [n for n in args.players.split(",") if n]
    (payload / "whitelist.json").write_text(json.dumps([{"uuid": offline_uuid(n), "name": n} for n in names]))
    ops = [n for n in args.ops.split(",") if n]
    (payload / "ops.json").write_text(json.dumps([{"uuid": offline_uuid(n), "name": n, "level": 4, "bypassesPlayerLimit": True} for n in ops]))
    ages = payload / "ages"
    ages.mkdir()
    answered = time.strftime("%Y-%m-%d", time.gmtime())
    for name in names:
        (ages / f"{offline_uuid(name)}.json").write_text(json.dumps({"born": "1990-04", "answered": answered}))
    config = {"version": 2, "environment": "dev", "operatorVetsPlayersOutOfBand": False,
              "viewDistanceBlocks": args.ck_view,
              "cluster": {"spawnWhereJoined": True, "presenceBlocks": args.presence, "redstone": True, "blockMirror": True, "relay": True,
                          "sessionAtHome": not args.session_everywhere},
              "loadAccounts": {"names": re.escape(args.bot_prefix) + "[0-9]+", "flyAtY": args.fly_at_y},
              "geoip": {"serverCountry": "US"}}
    if args.country_database:
        # CK takes an org-managed player only with their country: the servers look each connection's address up here.
        shutil.copy(args.country_database, payload / "country.mmdb")
        config["geoip"]["countryDatabase"] = "country.mmdb"
    (payload / "config" / "crowdedkingdoms-server.json").write_text(json.dumps(config, indent=1))
    env = payload / "crowdy.env"
    if args.fake:
        # One secret for the life of the run folder: records sealed before a restart still open after it.
        kept = run / "fake-secret"
        if not kept.exists():
            kept.write_text(hashlib.sha256(os.urandom(32)).hexdigest())
            kept.chmod(0o600)
        secret = kept.read_text().strip()
        env.write_text(f"# a local mesh on the in-memory CK: this secret seals records between its own servers only\nCROWDY_CLUSTER_SECRET={secret}\n")
    else:
        if not args.secrets_env:
            sys.exit("a mesh on live CK needs --secrets-env (CROWDY_ORG_TOKEN, CROWDY_ID_PEPPER, CROWDY_CLUSTER_SECRET)")
        text = Path(args.secrets_env).read_text()
        for key in ("CROWDY_ORG_TOKEN", "CROWDY_ID_PEPPER", "CROWDY_CLUSTER_SECRET"):
            if not re.search(rf"^{key}=.+", text, re.M):
                sys.exit(f"{args.secrets_env} has no {key}")
        env.write_text(text)
    env.chmod(0o600)
    settings = {"fake": bool(args.fake), "xmx": args.xmx, "viewDistance": args.view_distance, "simulationDistance": args.simulation_distance,
                "difficulty": args.difficulty, "bots": args.bots, "botPrefix": args.bot_prefix, "flags": args.flag or []}
    (run / "settings.json").write_text(json.dumps(settings, indent=1))
    subprocess.run(["tar", "-cf", str(run / "payload.tar"), "-C", str(payload), "."], check=True)
    print(f"payload: {len(names)} whitelisted, {len(ops)} operators, {'in-memory CK' if args.fake else 'live CK'}; {run / 'payload.tar'}")


def properties(mesh, settings, server):
    return "\n".join([
        f"server-ip={server['bind']}", f"server-port={server['port']}", "online-mode=false", "white-list=true", "enforce-whitelist=true",
        "allow-flight=true", f"view-distance={settings['viewDistance']}", f"simulation-distance={settings['simulationDistance']}",
        "max-players=4000", f"level-seed={SEED}", "level-name=world", f"motd={mesh['name']} {server['id']}", "spawn-protection=0",
        "network-compression-threshold=256", "sync-chunk-writes=false", "accepts-transfers=true", "prevent-proxy-connections=false",
        "enable-status=true", f"difficulty={settings['difficulty']}", "gamemode=survival", "pvp=true", "pause-when-empty-seconds=-1",
        "enable-rcon=false", "enable-query=false", "player-idle-timeout=0", ""])


def java_args(settings, folder, server):
    args = [f"-Xms{settings['xmx']}", f"-Xmx{settings['xmx']}", "-XX:+UseG1GC", "-XX:MaxGCPauseMillis=40", "-Djava.awt.headless=true",
            "-Dminecraft.api.discovery.host=http://127.0.0.1:9/",
            f"-Dcrowdedkingdoms.server.cluster={folder}/cluster.json", f"-Dcrowdedkingdoms.server.clusterId={server['id']}",
            f"-Dcrowdedkingdoms.env={folder}/crowdy.env", f"-Dcrowdedkingdoms.server.scriptFile={folder}/steps.txt",
            "-Dcrowdedkingdoms.server.meter=true"]
    if settings["fake"]:
        args += ["-Dcrowdedkingdoms.server.fakeCk=true", f"-Dcrowdedkingdoms.server.fakeHubDir={settings.get('fakeHubDir', folder + '/fake-hub')}"]
    args += settings.get("flags", [])
    return args + ["-jar", f"{folder}/fabric-server-launch.jar", "nogui"]


def first_steps(mesh):
    """Console commands every server runs as it starts: the world's edge at the grid's, when the grid is square."""
    x0, z0, x1, z1 = bounds(mesh)
    steps = []
    if mesh["cols"] == mesh["rows"]:
        steps += [f"console worldborder center {(x0 + x1 + 1) / 2} {(z0 + z1 + 1) / 2}", f"console worldborder set {x1 - x0 + 1}"]
    return steps


# ----- local servers -----

def local_folder(run, server):
    return Path(run) / "local" / server["id"]


def cmd_up(args):
    run = Path(args.run)
    mesh = load(run)
    settings = json.loads((run / "settings.json").read_text())
    only = set(args.only.split(",")) if args.only else None
    servers = [s for s in mesh["servers"] if only is None or s["id"] in only]
    if not mesh["local"]:
        return remote_up(run, mesh, settings, servers, args)
    settings["fakeHubDir"] = str(run / "fake-hub")
    (run / "fake-hub").mkdir(exist_ok=True)
    template = None
    starts = {}
    for server in servers:
        folder = local_folder(run, server)
        if args.fresh and folder.exists():
            shutil.rmtree(folder)
        folder.mkdir(parents=True, exist_ok=True)
        subprocess.run(["tar", "-xf", str(run / "payload.tar"), "-C", str(folder)], check=True)
        ages = folder / "world" / "crowdedkingdoms" / "ages"
        ages.mkdir(parents=True, exist_ok=True)
        for file in (folder / "ages").iterdir():
            shutil.copy(file, ages / file.name)
        (folder / "server.properties").write_text(properties(mesh, settings, server))
        if not (folder / "steps.txt").exists() or args.fresh:
            (folder / "steps.txt").write_text("\n".join(first_steps(mesh)) + "\n")
        # The launcher fetches the game and its libraries on its first run: once, then copied.
        if template is None:
            template = next((local_folder(run, s) for s in mesh["servers"] if (local_folder(run, s) / "libraries").is_dir()), None)
        if template is not None and template != folder and not (folder / "libraries").exists():
            for part in ("libraries", "versions", ".fabric", "server.jar"):
                source = template / part
                if source.is_dir():
                    shutil.copytree(source, folder / part, dirs_exist_ok=True)
                elif source.exists():
                    shutil.copy(source, folder / part)
        log = open(folder / "server.log", "ab")
        starts[server["id"]] = log.tell()
        process = subprocess.Popen(["java"] + java_args(settings, str(folder), server), cwd=folder, stdout=log, stderr=log, stdin=subprocess.DEVNULL,
                                   start_new_session=True)
        (folder / "pid").write_text(str(process.pid))
        print(f"{server['id']} started, pid {process.pid}, port {server['port']}")
        if template is None:
            # Let the first one fetch before the rest copy from it.
            wait_for(folder / "server.log", r"Done \(", 600, starts[server["id"]])
            template = folder
    for server in servers:
        if wait_for(local_folder(run, server) / "server.log", r"Done \(", 600, starts[server["id"]]) is None:
            print(f"{server['id']} did not finish starting; see {local_folder(run, server) / 'server.log'}")


def wait_for(path, pattern, seconds, start=0):
    deadline = time.time() + seconds
    regex = re.compile(pattern)
    while time.time() < deadline:
        if path.exists():
            with open(path, "rb") as file:
                file.seek(start)
                for line in file.read().decode(errors="replace").splitlines():
                    if regex.search(line):
                        return line
        time.sleep(1)
    return None


def cmd_down(args):
    run = Path(args.run)
    mesh = load(run)
    only = set(args.only.split(",")) if args.only else None
    servers = [s for s in mesh["servers"] if only is None or s["id"] in only]
    if not mesh["local"]:
        return each(servers, lambda s: ssh(s, "sudo systemctl stop mc-mesh; echo stopped"), "down")
    for server in servers:
        pid_file = local_folder(run, server) / "pid"
        if pid_file.exists():
            try:
                os.kill(int(pid_file.read_text()), signal.SIGTERM)
                print(f"{server['id']} told to stop")
            except ProcessLookupError:
                pass
            pid_file.unlink()


# ----- remote servers -----

def ssh(server, command, timeout=120, input=None):
    """Tailscale SSH as ubuntu; the reply's text, or raises with what the far side said."""
    result = subprocess.run(["tailscale", "ssh", f"ubuntu@{server['ssh']}", command], capture_output=True, text=True, timeout=timeout, input=input)
    if result.returncode != 0:
        raise RuntimeError(f"{server.get('vm', server['id'])}: {result.stderr.strip()[-300:] or result.stdout.strip()[-300:]}")
    return result.stdout


def allowed(run):
    """The addresses that may reach the game and link ports besides the servers' own: the run's machines (allow.json,
    from `hosts`) and the ones an operator added (allow-extra.json, from `allow`: a camera's, say)."""
    out = set()
    for name in ("allow.json", "allow-extra.json"):
        if (run / name).exists():
            out |= set(json.loads((run / name).read_text()))
    return out


def cmd_allow(args):
    """One more address may reach every server's game port: now, and after any later `up`."""
    import ipaddress
    address = str(ipaddress.ip_address(args.address))
    run = Path(args.run)
    mesh = load(run)
    if mesh.get("local"):
        sys.exit("allow: a local mesh has no firewall")
    extra = run / "allow-extra.json"
    known = set(json.loads(extra.read_text())) if extra.exists() else set()
    extra.write_text(json.dumps(sorted(known | {address})))
    rule = f"-s {address}/32 -j ACCEPT"
    done = each(mesh["servers"], lambda server: ssh(server, f"sudo iptables -C MESH {rule} 2>/dev/null || sudo iptables -I MESH 2 {rule}", timeout=60), "allow")
    print(f"allow: {address} on {len(done)} of {len(mesh['servers'])} servers")


def each(servers, work, what, workers=24):
    """Runs work(server) for every server at once; prints who failed; returns {id: result}."""
    results = {}
    failed = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        futures = {pool.submit(work, server): server for server in servers}
        for future in concurrent.futures.as_completed(futures):
            server = futures[future]
            try:
                results[server["id"]] = future.result()
            except Exception as e:  # noqa: BLE001 - every failure is reported by name
                failed.append(f"{server['id']}: {e}")
    if failed:
        print(f"{what}: {len(failed)} of {len(servers)} failed", file=sys.stderr)
        for line in sorted(failed)[:20]:
            print("  " + line, file=sys.stderr)
    return results


def remote_up(run, mesh, settings, servers, args):
    payload = run / "payload.tar"
    peers = sorted({s["host"] for s in mesh["servers"]} | allowed(run))

    def bring(server):
        if not args.restart_only:
            with open(payload, "rb") as file:
                copy = subprocess.run(["tailscale", "ssh", f"ubuntu@{server['ssh']}", "cat > /tmp/mc-mesh-payload.tar"], stdin=file, capture_output=True,
                                      timeout=600)
            if copy.returncode != 0:
                raise RuntimeError("copy: " + copy.stderr.decode(errors="replace").strip()[-200:])
        unit = "\n".join([
            "[Unit]", f"Description=CrowdyCraft mesh {server['id']}", "After=network-online.target", "[Service]", f"WorkingDirectory={REMOTE_DIR}",
            "User=ubuntu", "LimitNOFILE=200000", "ExecStart=/usr/bin/java " + " ".join(java_args(settings, REMOTE_DIR, server)), "Restart=on-failure",
            "RestartSec=5", f"StandardOutput=append:{REMOTE_DIR}/server.log", f"StandardError=append:{REMOTE_DIR}/server.log", "[Install]",
            "WantedBy=multi-user.target", ""])
        allow = " ".join(peers)
        script = f"""set -euo pipefail
{QUIET_UPGRADES}if ! command -v java >/dev/null 2>&1; then
  sudo DEBIAN_FRONTEND=noninteractive apt-get -o DPkg::Lock::Timeout=600 update -qq >/tmp/apt-java.log 2>&1
  sudo DEBIAN_FRONTEND=noninteractive apt-get -o DPkg::Lock::Timeout=600 install -y -qq openjdk-25-jre-headless >>/tmp/apt-java.log 2>&1
fi
sudo systemctl stop mc-mesh 2>/dev/null || true
sudo mkdir -p {REMOTE_DIR}
sudo chown -R ubuntu:ubuntu {REMOTE_DIR}
cd {REMOTE_DIR}
{'true' if args.restart_only else 'tar -xf /tmp/mc-mesh-payload.tar -C ' + REMOTE_DIR}
{'rm -rf world server.log steps.txt' if args.fresh else 'true'}
mkdir -p world/crowdedkingdoms/ages
cp -n ages/*.json world/crowdedkingdoms/ages/ 2>/dev/null || true
chmod 600 crowdy.env
cat > server.properties <<'PROP'
{properties(mesh, settings, server)}PROP
# Written anew at every start: a server reads its step file from the top when it starts, so steps a test appended
# while it ran (a scenario's, a console line) would otherwise all play again.
cat > steps.txt <<'STEPS'
{chr(10).join(first_steps(mesh))}
STEPS
sudo tee /etc/systemd/system/mc-mesh.service >/dev/null <<'UNIT'
{unit}UNIT
# The game and link ports answer only this mesh's own machines (and the tailnet); the server is in offline mode.
# One iptables chain, loaded in one go: ufw takes two seconds a rule, which is four minutes for a hundred machines.
if [ -n "{allow}" ] && [ "{server['bind']}" = "0.0.0.0" ]; then
  sudo ufw --force disable >/dev/null 2>&1 || true
  {{ echo '*filter'; echo ':MESH - [0:0]'; echo '-F MESH'; echo '-A MESH -i lo -j ACCEPT'
    for ip in {allow}; do echo "-A MESH -s $ip -j ACCEPT"; done
    echo '-A MESH -j DROP'; echo 'COMMIT'; }} | sudo iptables-restore --noflush
  sudo iptables -C INPUT ! -i tailscale0 -p tcp -m multiport --dports {GAME_PORT},{LINK_PORT} -j MESH 2>/dev/null \
    || sudo iptables -I INPUT ! -i tailscale0 -p tcp -m multiport --dports {GAME_PORT},{LINK_PORT} -j MESH
fi
sudo systemctl daemon-reload
sudo systemctl start mc-mesh
echo started
"""
        return ssh(server, "bash -s", timeout=900, input=script).strip().splitlines()[-1]

    done = each(servers, bring, "up", workers=args.workers)
    print(f"up: {sum(1 for v in done.values() if v == 'started')} of {len(servers)} started")


# ----- watching -----

METER = re.compile(r"Crowded Kingdoms meter: tps=(\S+) msptAvg=(\S+) msptP95=(\S+) msptMax=(\S+) players=(\d+) links=(\d+) heapMB=(\d+) parts\[(.*?)\](?: sent\[(.*?)\])?(?: link\[(.*?)\])?")


def last_meter(text):
    found = None
    for line in text.splitlines():
        match = METER.search(line)
        if match:
            found = match
    return found


def cmd_watch(args):
    run = Path(args.run)
    mesh = load(run)
    servers = mesh["servers"]

    def read(server):
        if mesh["local"]:
            path = local_folder(run, server) / "server.log"
            if not path.exists():
                return ""
            with open(path, "rb") as file:
                file.seek(max(0, path.stat().st_size - 200_000))
                return file.read().decode(errors="replace")
        return ssh(server, f"tail -c 60000 {REMOTE_DIR}/server.log; echo; echo LOAD $(cut -d' ' -f1 /proc/loadavg)", timeout=40)

    texts = each(servers, read, "watch", workers=32)
    rows = []
    for server in servers:
        text = texts.get(server["id"], "")
        meter = last_meter(text)
        load_avg = re.search(r"LOAD (\S+)", text)
        seam = [float(v) for v in re.findall(r"redstone seam: \S+ tick=.* tps=(\S+)", text)[-8:]]
        behind = len(re.findall(r"Can't keep up", text))
        rows.append({"id": server["id"], "tps": float(meter.group(1)) if meter else None, "avg": float(meter.group(2)) if meter else None,
                     "p95": float(meter.group(3)) if meter else None, "max": float(meter.group(4)) if meter else None,
                     "players": int(meter.group(5)) if meter else None, "links": int(meter.group(6)) if meter else None,
                     "heap": int(meter.group(7)) if meter else None, "parts": meter.group(8) if meter else "", "sent": (meter.group(9) or "") if meter else "", "link": (meter.group(10) or "") if meter else "", "load": load_avg.group(1) if load_avg else "",
                     "behind": behind, "seamTps": min(seam) if seam else None})
    if args.json:
        print(json.dumps(rows))
        return
    live = [r for r in rows if r["tps"] is not None]
    print(f"{'id':8} {'tps':>5} {'avg':>6} {'p95':>6} {'max':>7} {'plyrs':>5} {'links':>5} {'heap':>5} {'load':>5} {'late':>4}  parts")
    shown = rows if args.all else sorted(live, key=lambda r: (r["tps"], -r["p95"]))[:args.worst]
    for r in shown:
        if r["tps"] is None:
            print(f"{r['id']:8}   no meter line")
            continue
        print(f"{r['id']:8} {r['tps']:5.1f} {r['avg']:6.1f} {r['p95']:6.1f} {r['max']:7.1f} {r['players']:5d} {r['links']:5d} {r['heap']:5d} {r['load']:>5} "
              f"{r['behind']:4d}  {r['parts']} | {r['sent']} | {r['link']}")
    if live:
        tps = sorted(r["tps"] for r in live)
        p95 = sorted(r["p95"] for r in live)
        print(f"-- {len(live)} of {len(rows)} servers reporting; players {sum(r['players'] for r in live)}; tps min {tps[0]:.1f} median {tps[len(tps) // 2]:.1f}; "
              f"mspt p95 median {p95[len(p95) // 2]:.1f} worst {p95[-1]:.1f}; servers under 19.5 tps: {sum(1 for t in tps if t < 19.5)}; "
              f"over 50 ms p95: {sum(1 for p in p95 if p > 50)}")


def cmd_say(args):
    run = Path(args.run)
    mesh = load(run)
    only = set(args.only.split(",")) if args.only else None
    servers = [s for s in mesh["servers"] if only is None or s["id"] in only]
    line = "console " + args.command if not args.raw else args.command

    def tell(server):
        if mesh["local"]:
            with open(local_folder(run, server) / "steps.txt", "a") as file:
                file.write(line + "\n")
            return "ok"
        return ssh(server, f"cat >> {REMOTE_DIR}/steps.txt", input=line + "\n")

    done = each(servers, tell, "say")
    print(f"say: {len(done)} of {len(servers)} servers took: {line}")


def cmd_pregen(args):
    """Has every server make its own chunks and those within sight of them now, before anyone plays.

    A chunk made for the first time sets off block updates, scheduled ticks and edits across a line (the game's own
    finishing of new terrain), which keep the two servers there in step and waiting for each other. Made ahead, a line
    is quiet when the players come. --remove lets the chunks go again once they are made (they load from disk after).
    """
    run = Path(args.run)
    mesh = load(run)
    servers = mesh["servers"]

    def strips(cx0, cz0, cx1, cz1):
        """forceload commands for the chunks cx0..cx1 by cz0..cz1: the game forces at most 256 chunks a command."""
        out = []
        columns = max(1, min(cx1 - cx0 + 1, 256))
        for x in range(cx0, cx1 + 1, columns):
            wide = min(x + columns - 1, cx1) - x + 1
            rows = max(1, 256 // wide)
            for z in range(cz0, cz1 + 1, rows):
                out.append(f"console forceload add {x * 16} {z * 16} {min(x + columns - 1, cx1) * 16 + 15} {min(z + rows - 1, cz1) * 16 + 15}")
        return out

    def lines(server):
        if args.remove:
            return ["console forceload remove all"]
        if not server.get("default"):
            return strips(server["minChunkX"] - args.margin, server["minChunkZ"] - args.margin, server["maxChunkX"] + args.margin,
                          server["maxChunkZ"] + args.margin)
        # The default runs what lies around the grid: the ring of it within sight of the grid's edge.
        ring = args.margin + 2
        gx0, gz0 = mesh["originChunkX"], mesh["originChunkZ"]
        gx1, gz1 = gx0 + mesh["cols"] * mesh["cell"] - 1, gz0 + mesh["rows"] * mesh["cell"] - 1
        return (strips(gx0 - ring, gz0 - ring, gx1 + ring, gz0 - 1) + strips(gx0 - ring, gz1 + 1, gx1 + ring, gz1 + ring)
                + strips(gx0 - ring, gz0, gx0 - 1, gz1) + strips(gx1 + 1, gz0, gx1 + ring, gz1))

    def tell(server):
        text = "\n".join(lines(server)) + "\n"
        if mesh["local"]:
            with open(local_folder(run, server) / "steps.txt", "a") as file:
                file.write(text)
            return "ok"
        return ssh(server, f"cat >> {REMOTE_DIR}/steps.txt", input=text)

    done = each(servers, tell, "pregen")
    print(f"pregen: {len(done)} of {len(servers)} servers told to {'let their chunks go' if args.remove else 'make their chunks'}")


# ----- bots -----

def control(host, port, lines, timeout=30):
    """Sends control lines to one load client and returns its reply lines, one for each."""
    replies = []
    with socket.create_connection((host, port), timeout) as sock:
        sock.settimeout(timeout)
        sock.sendall(("\n".join(lines) + "\n").encode())
        buffer = b""
        while len(replies) < len(lines):
            chunk = sock.recv(65536)
            if not chunk:
                break
            buffer += chunk
            while b"\n" in buffer:
                line, buffer = buffer.split(b"\n", 1)
                replies.append(line.decode(errors="replace"))
    return replies


def generators(run, mesh):
    """Where the load clients run: this machine for a local mesh, else generators.txt (name, address the control is reached by)."""
    if mesh["local"]:
        return [{"id": "local", "ssh": "127.0.0.1"}]
    rows = [line.split() for line in (Path(run) / "generators.txt").read_text().splitlines() if line.strip() and not line.startswith("#")]
    return [{"id": row[0], "ssh": row[-1], "vm": row[0]} for row in rows]


def cmd_bots(args):
    run = Path(args.run)
    mesh = load(run)
    settings = json.loads((run / "settings.json").read_text())
    targets = grid_servers(mesh)
    gens = generators(run, mesh)
    x0, z0, x1, z1 = bounds(mesh)
    inset = args.inset
    box = f"{x0 + inset},{z0 + inset},{x1 - inset},{z1 - inset}"
    grid = f"{x0} {z0} {mesh['cell'] * 16} {mesh['cols']} {mesh['rows']}"
    first, last = args.first, args.first + args.count - 1
    per = {gen["id"]: [] for gen in gens}
    for i in range(first, last + 1):
        server = targets[(i - 1) % len(targets)]
        gen = gens[(i - 1) % len(gens)]
        waypoints = box if args.roam else (f"{server['minChunkX'] * 16 + inset},{server['minChunkZ'] * 16 + inset},"
                                           f"{server['maxChunkX'] * 16 + 15 - inset},{server['maxChunkZ'] * 16 + 15 - inset}")
        per[gen["id"]].append(f"add {settings['botPrefix']}{i} {server['host']} {server['port']} wander {args.bps} {args.clearance} {args.seconds} {waypoints} /dev/null")

    def add(gen):
        lines = per[gen["id"]]
        if not lines:
            return 0
        host = gen["ssh"]
        if args.start:
            start_client(run, mesh, gen, args.threads)
        control(host, CONTROL_PORT, [f"grid {grid}"])
        added = 0
        # In waves, so a server signs in a few players at a time: CK takes so many a minute.
        for at in range(0, len(lines), args.wave):
            replies = control(host, CONTROL_PORT, lines[at:at + args.wave], timeout=120)
            added += sum(1 for r in replies if r.startswith("ok"))
            bad = [r for r in replies if not r.startswith("ok")]
            if bad:
                print(f"{gen['id']}: {bad[0]}")
            time.sleep(args.wave_seconds)
        return added

    done = each(gens, add, "bots", workers=len(gens))
    print(f"bots: added {sum(done.values())} of {args.count} ({settings['botPrefix']}{first}..{settings['botPrefix']}{last}) over {len(gens)} generator(s), "
          f"{'roaming the grid' if args.roam else 'each in its own cell'}")


def start_client(run, mesh, gen, threads):
    if mesh["local"]:
        binary = os.environ.get("CK_MC_LOAD", str(Path(__file__).resolve().parents[1] / "cpp" / "build" / "ck-mc-load"))
        log = open(Path(run) / "ck-mc-load.log", "ab")
        process = subprocess.Popen([binary, "--control", f"127.0.0.1:{CONTROL_PORT}", "--threads", str(threads), "--journal", str(Path(run) / "bots.journal")],
                                   stdout=log, stderr=log, stdin=subprocess.DEVNULL, start_new_session=True)
        (Path(run) / "ck-mc-load.pid").write_text(str(process.pid))
        time.sleep(1)
        return
    ssh(gen, f"sudo systemctl restart ck-mc-load && sleep 1 && systemctl is-active ck-mc-load")


def cmd_botctl(args):
    run = Path(args.run)
    mesh = load(run)
    gens = generators(run, mesh)
    line = " ".join(args.line)
    replies = each(gens, lambda gen: control(gen["ssh"], CONTROL_PORT, [line])[0], "botctl", workers=len(gens))
    totals = {}
    for gen in gens:
        reply = replies.get(gen["id"], "no reply")
        if not args.sum:
            print(f"{gen['id']:14} {reply}")
        for key, value in re.findall(r"(\w+)=(-?\d+)", reply):
            totals[key] = max(totals.get(key, 0), int(value)) if key.endswith("_max") else totals.get(key, 0) + int(value)
    if totals:
        print("sum: " + " ".join(f"{k}={v}" for k, v in totals.items()))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    def add(name, function):
        p = sub.add_parser(name)
        p.add_argument("--run", required=True, help="the mesh's run folder")
        p.set_defaults(function=function)
        return p

    p = add("hosts", cmd_hosts)
    p.add_argument("--game-state", help="loadgen-fleet.mjs state file(s) of the game servers, comma-separated")
    p.add_argument("--loadgen-state", help="loadgen-fleet.mjs state file(s) of the generators, comma-separated")
    p.add_argument("--tailnet", action="store_true", help="servers and bots talk over the tailnet rather than the public addresses")

    p = add("gen-install", cmd_gen_install)
    p.add_argument("--threads", type=int, default=3)

    p = add("plan", cmd_plan)
    p.add_argument("--name", default="mesh")
    p.add_argument("--cols", type=int, required=True)
    p.add_argument("--rows", type=int, required=True)
    p.add_argument("--cell", type=int, default=12, help="chunks along each side of a server's square")
    p.add_argument("--origin-chunk-x", type=int, default=0)
    p.add_argument("--origin-chunk-z", type=int, default=0)
    p.add_argument("--margin", type=int, default=8)
    p.add_argument("--hosts", help="lines of: name, the address servers and bots use, the address this machine uses; none for a local mesh")
    p.add_argument("--local-port", type=int, default=25700)
    p.add_argument("--name-cell", action="append", help="COL,ROW=id names that cell's server (west and east for the mod's scenarios)")

    p = add("payload", cmd_payload)
    p.add_argument("--mod-jar", required=True)
    p.add_argument("--fabric-api", required=True)
    p.add_argument("--fabric-launch", required=True)
    p.add_argument("--bots", type=int, default=3000)
    p.add_argument("--bot-prefix", default="Load")
    p.add_argument("--players", default="Cam1,Cam2,Cam3,Cam4,Tester1,Tester2,Tester3,Tester4", help="other whitelisted names")
    p.add_argument("--ops", default="Cam1,Cam2,Cam3,Cam4")
    p.add_argument("--fly-at-y", type=int, default=210)
    p.add_argument("--presence", type=int, default=32)
    p.add_argument("--ck-view", type=int, default=64, help="blocks around a player within which CK tells them of other players (viewDistanceBlocks)")
    p.add_argument("--session-everywhere", action="store_true",
                   help="each server that plays a bot opens a CK session for it, as for any player (cluster.sessionAtHome off)")
    p.add_argument("--fake", action="store_true", help="the in-memory CK, its records shared through a folder (a local mesh)")
    p.add_argument("--secrets-env", help="live CK: a file with CROWDY_ORG_TOKEN, CROWDY_ID_PEPPER and CROWDY_CLUSTER_SECRET")
    p.add_argument("--country-database", help="live CK: an MMDB country database (GeoLite2 Country), for players who connect from the internet")
    p.add_argument("--xmx", default="2G")
    p.add_argument("--view-distance", type=int, default=6)
    p.add_argument("--simulation-distance", type=int, default=4)
    p.add_argument("--difficulty", default="normal")
    p.add_argument("--flag", action="append", help="one more JVM argument for every server")

    p = add("up", cmd_up)
    p.add_argument("--only")
    p.add_argument("--fresh", action="store_true", help="start from no world and no log")
    p.add_argument("--restart-only", action="store_true", help="remote: keep the files there, restart the service")
    p.add_argument("--workers", type=int, default=24)

    p = add("down", cmd_down)
    p.add_argument("--only")

    p = add("allow", cmd_allow)
    p.add_argument("address", help="an address outside the run (a camera's, a tester's) that may join the servers")

    p = add("watch", cmd_watch)
    p.add_argument("--all", action="store_true")
    p.add_argument("--worst", type=int, default=12)
    p.add_argument("--json", action="store_true")

    p = add("say", cmd_say)
    p.add_argument("--only")
    p.add_argument("--raw", action="store_true", help="the line is a script step, not a console command")
    p.add_argument("command")

    p = add("pregen", cmd_pregen)
    p.add_argument("--margin", type=int, default=7, help="chunks around a server's own that it makes too (its sight of the others')")
    p.add_argument("--remove", action="store_true")

    p = add("bots", cmd_bots)
    p.add_argument("--first", type=int, default=1)
    p.add_argument("--count", type=int, required=True)
    p.add_argument("--bps", type=float, default=4.0)
    p.add_argument("--clearance", type=int, default=0)
    p.add_argument("--seconds", type=int, default=86400)
    p.add_argument("--inset", type=int, default=24, help="blocks the bots keep from the edge of their box")
    p.add_argument("--roam", action="store_true", help="every bot wanders the whole grid and crosses lines; else each stays in the cell it joined")
    p.add_argument("--wave", type=int, default=20)
    p.add_argument("--wave-seconds", type=float, default=2.0)
    p.add_argument("--start", action="store_true", help="start (or restart) the load client first")
    p.add_argument("--threads", type=int, default=2)

    p = add("botctl", cmd_botctl)
    p.add_argument("--sum", action="store_true")
    p.add_argument("line", nargs="+")

    args = parser.parse_args()
    args.function(args)


if __name__ == "__main__":
    main()
