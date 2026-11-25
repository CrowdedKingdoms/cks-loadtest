#!/usr/bin/env bash
# Builds meshcam.jar with javac alone: Minecraft 26.x ships unobfuscated, so the mod compiles against the client jar.
#   build.sh <26.3 client jar> <fabric-loader jar> <fabric-api jar>
# The client jar is in a launcher's versions/26.3/ (portablemc: <main-dir>/versions/26.3/26.3.jar). The loader's jar
# is in Gradle's cache once any Fabric mod has been built, or in a launcher's libraries/net/fabricmc/fabric-loader/.
# The Fabric API jar is the one a server's or a client's mods folder holds; its modules are jars inside it.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
client="$1"; loader="$2"; api="$3"
out="$here/build"
rm -rf "$out" && mkdir -p "$out/classes" "$out/api"
unzip -q -o "$api" 'META-INF/jars/*.jar' -d "$out/api"
libs="$(dirname "$(dirname "$(dirname "$client")")")/libraries"
cp="$client:$loader:$(find "$out/api" -name '*.jar' | tr '\n' ':')"
if [ -d "$libs" ]; then
  cp="$cp:$(find "$libs" -name '*.jar' | tr '\n' ':')"
fi
javac -nowarn -proc:none -d "$out/classes" -cp "$cp" "$here"/src/com/crowdedkingdoms/meshcam/*.java
cp "$here/fabric.mod.json" "$out/classes/"
(cd "$out/classes" && jar cf "$out/meshcam.jar" .)
echo "$out/meshcam.jar"
