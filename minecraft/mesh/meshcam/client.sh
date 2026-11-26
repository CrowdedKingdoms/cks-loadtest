#!/usr/bin/env bash
# A Minecraft 26.3 client with the camera mod (Fabric, Fabric API, meshcam) that joins a mesh server, through
# portablemc (pipx install portablemc). Offline name only: a load test's servers run in offline mode.
#   client.sh <name> <host> [port]
# MESHCAM_MAIN is where the game's files go (assets, libraries, versions; default ~/.local/state/meshcam; point it at
# a folder another portablemc client already filled to skip the download). MESHCAM_API is the Fabric API jar (a
# server's mods folder has one). MESHCAM_RESOLUTION is the window's size, 1280x720 by default.
# The game folder is <main>/<name>: its first-run screens are marked as seen, vsync and sound are off (on macOS a
# covered window waiting for vsync stops the game) and it does not pause when it loses focus.
set -euo pipefail
name="$1"; host="$2"; port="${3:-25565}"
here="$(cd "$(dirname "$0")" && pwd)"
main="${MESHCAM_MAIN:-$HOME/.local/state/meshcam}"
api="${MESHCAM_API:?set MESHCAM_API to a Fabric API jar for 26.3}"
[ -f "$here/build/meshcam.jar" ] || { echo "build the mod first: $here/build.sh" >&2; exit 1; }
dir="$main/$name"
mkdir -p "$dir/mods" "$dir/meshcam"
cp "$here/build/meshcam.jar" "$dir/mods/meshcam.jar"
cp "$api" "$dir/mods/fabric-api.jar"
options="$dir/options.txt"
touch "$options"
for pair in onboardAccessibility:false skipMultiplayerWarning:true joinedFirstServer:true pauseOnLostFocus:false enableVsync:false \
    soundCategory_master:0.0 maxFps:120 "inactivityFpsLimit:\"afk\"" tutorialStep:none; do
  key="${pair%%:*}"
  grep -v "^${key}:" "$options" > "$options.tmp" || true
  mv "$options.tmp" "$options"
  echo "$pair" >> "$options"
done
# servers.dat (uncompressed NBT): one entry for this server, with its resource pack accepted.
python3 - "$dir/servers.dat" "$host:$port" <<'PY'
import struct, sys
def string(name, value):
    n, v = name.encode(), value.encode()
    return b"\x08" + struct.pack(">H", len(n)) + n + struct.pack(">H", len(v)) + v
entry = string("name", "Mesh") + string("ip", sys.argv[2]) + b"\x01" + struct.pack(">H", 14) + b"acceptTextures\x01" + b"\x00"
servers = b"\x09" + struct.pack(">H", 7) + b"servers" + b"\x0a" + struct.pack(">i", 1) + entry
open(sys.argv[1], "wb").write(b"\x0a\x00\x00" + servers + b"\x00")
PY
portablemc="$(command -v portablemc || echo "$HOME/.local/bin/portablemc")"
exec "$portablemc" --main-dir "$main" start --mc-dir "$dir" fabric:26.3 -u "$name" \
  --join-server "$host" --join-server-port "$port" --resolution "${MESHCAM_RESOLUTION:-1280x720}"
