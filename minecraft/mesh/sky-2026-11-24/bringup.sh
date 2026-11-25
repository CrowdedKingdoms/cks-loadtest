#!/bin/bash
# Remote bringup. Args: sid (s00) index (0-59)
set -euo pipefail
SID="$1"
N="$2"
TIP="$(tailscale ip -4 | head -1)"
COL=$((N % 10))
ROW=$((N / 10))
X0=$((COL * 64))
Z0=$((ROW * 64))
X1=$((X0 + 63))
Z1=$((Z0 + 63))
CX=$((X0 + 32))
CZ=$((Z0 + 32))
if ! command -v java >/dev/null 2>&1; then
  sudo DEBIAN_FRONTEND=noninteractive apt-get update -qq >/tmp/apt-java.log
  sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq openjdk-25-jre-headless >>/tmp/apt-java.log
fi
sudo mkdir -p /opt/mc-sky/mods /opt/mc-sky/config /opt/mc-sky/world/crowdedkingdoms/ages
sudo chown -R ubuntu:ubuntu /opt/mc-sky
if [ ! -f /opt/mc-sky/mods/crowdycraft-server.jar ]; then
  echo "mods missing" >&2
  exit 1
fi
if [ "$SID" != "s00" ]; then
  if ! command -v mount.nfs >/dev/null 2>&1 && ! command -v mount.nfs4 >/dev/null 2>&1; then
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq nfs-common >/tmp/apt-nfs.log
  fi
  mkdir -p /opt/mc-sky/fake-hub
  if ! mountpoint -q /opt/mc-sky/fake-hub; then
    sudo mount -t nfs -o vers=4 ${HUB_IP:?the fake hub NFS server}:/opt/mc-sky/fake-hub /opt/mc-sky/fake-hub
  fi
fi
tar -xf /opt/mc-sky/ages.tar -C /opt/mc-sky/world/crowdedkingdoms/ages
cat > /opt/mc-sky/server.properties << PROP
server-ip=${TIP}
server-port=25565
online-mode=false
white-list=true
enforce-whitelist=true
allow-flight=true
view-distance=6
simulation-distance=4
max-players=80
level-seed=7270301997446181
level-name=world
motd=mc-sky-${SID}
spawn-protection=0
network-compression-threshold=256
sync-chunk-writes=true
accepts-transfers=true
prevent-proxy-connections=false
enable-status=true
spawn-monsters=false
spawn-animals=false
spawn-npcs=false
difficulty=peaceful
gamemode=adventure
pvp=false
PROP
: > /opt/mc-sky/steps.txt
printf '%s\n' "console forceload add ${X0} ${Z0} ${X1} ${Z1}" "console setworldspawn ${CX} 90 ${CZ}" >> /opt/mc-sky/steps.txt
sudo tee /etc/systemd/system/mc-sky.service >/dev/null << UNIT
[Unit]
Description=Minecraft sky mesh ${SID}
After=network-online.target
[Service]
WorkingDirectory=/opt/mc-sky
User=ubuntu
ExecStart=/usr/bin/java -Xmx2G -Djava.awt.headless=true -Dminecraft.api.discovery.host=http://127.0.0.1:9/ -Dcrowdedkingdoms.server.fakeCk=true -Dcrowdedkingdoms.server.fakeHubDir=/opt/mc-sky/fake-hub -Dcrowdedkingdoms.server.cluster=/opt/mc-sky/cluster.json -Dcrowdedkingdoms.server.clusterId=${SID} -Dcrowdedkingdoms.env=/opt/mc-sky/crowdy.env -Dcrowdedkingdoms.server.scriptFile=/opt/mc-sky/steps.txt -jar /opt/mc-sky/fabric-server-launch.jar nogui
Restart=on-failure
RestartSec=3
StandardOutput=append:/opt/mc-sky/server.log
StandardError=append:/opt/mc-sky/server.log
[Install]
WantedBy=multi-user.target
UNIT
sudo systemctl daemon-reload
echo "BRINGUP ${SID} ${TIP} cell ${X0},${Z0}"
