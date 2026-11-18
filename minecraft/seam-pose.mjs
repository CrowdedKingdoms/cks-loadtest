/**
 * Write a LoadWalker's CK hub player-record Pos to the seam before spawn.
 * Pos is west-of-line or east-of-line to match the join home (`server` / `side`).
 * Env: CROWDY_ORG_TOKEN, CROWDY_ID_PEPPER, CROWDY_CLUSTER_SECRET
 * Optional: CROWDY_API_ORIGIN (default ck.test), CROWDY_APP_ID, CROWDY_MC_SERVER (default west)
 */
import { createHmac, createHash, randomBytes } from "node:crypto";
import { gzipSync, gunzipSync } from "node:zlib";
import { pathToFileURL } from "node:url";

const DEFAULT_ORIGIN = "https://ck.test.crowdedkingdoms.com";
const DEFAULT_APP_ID = "108581857289472";
const DEFAULT_SERVER = "west";

function mustEnv(name) {
  const v = process.env[name];
  if (!v) throw new Error(`missing ${name}`);
  return v;
}

function offlineUuid(name) {
  const h = createHash("md5").update("OfflinePlayer:" + name).digest();
  const b = Buffer.from(h);
  b[6] = (b[6] & 0x0f) | 0x30;
  b[8] = (b[8] & 0x3f) | 0x80;
  const hex = b.toString("hex");
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
}

function externalId(pepper, uuid) {
  const mac = createHmac("sha256", pepper).update(uuid.toLowerCase()).digest();
  return "mc-" + mac.toString("base64url");
}

function sealRecord(secret, userId, version, recordBytes) {
  const mac = createHmac("sha256", secret);
  mac.update(Buffer.from("ck-minecraft-record/1", "utf8"));
  const user = Buffer.from(userId, "utf8");
  const header = Buffer.alloc(4 + user.length + 8);
  header.writeUInt32BE(user.length, 0);
  user.copy(header, 4);
  header.writeBigInt64BE(BigInt(version), 4 + user.length);
  mac.update(header);
  mac.update(recordBytes);
  return mac.digest().toString("base64url");
}

/** Flatten err + cause chain for logs / add_error (Node fetch hides ENOTFOUND etc. in .cause). */
export function formatErrorDetail(err, extras = {}) {
  const parts = [];
  if (extras.op) parts.push(`op=${extras.op}`);
  if (extras.url) parts.push(`url=${extras.url}`);
  if (extras.attempt != null) {
    parts.push(`attempt=${extras.attempt}${extras.attempts != null ? `/${extras.attempts}` : ""}`);
  }
  if (extras.status != null) parts.push(`status=${extras.status}`);
  if (extras.body) {
    const b = String(extras.body).replace(/\s+/g, " ").slice(0, 500);
    if (b) parts.push(`body=${b}`);
  }
  let cur = err;
  let depth = 0;
  const chain = [];
  while (cur && depth < 8) {
    const bits = [];
    const name = cur.name || (cur.constructor && cur.constructor.name);
    const msg = cur.message != null ? String(cur.message) : String(cur);
    if (name && name !== "Error") bits.push(name);
    bits.push(msg);
    for (const k of ["code", "errno", "syscall", "hostname", "address", "port"]) {
      if (cur[k] != null && cur[k] !== "") bits.push(`${k}=${cur[k]}`);
    }
    chain.push(bits.join(" "));
    cur = cur.cause;
    depth++;
  }
  if (chain.length) parts.push(chain.join(" <- "));
  return parts.join(" | ");
}

export function isTransientNetworkError(err) {
  const codes = new Set();
  let cur = err;
  let depth = 0;
  while (cur && depth < 8) {
    if (cur.code) codes.add(String(cur.code));
    const msg = String(cur.message || "");
    if (/fetch failed|ECONNRESET|ECONNREFUSED|ETIMEDOUT|ENOTFOUND|UND_ERR|socket hang up|network/i.test(msg)) {
      return true;
    }
    cur = cur.cause;
    depth++;
  }
  for (const c of codes) {
    if (
      /^(ECONNRESET|ECONNREFUSED|ETIMEDOUT|ENOTFOUND|EAI_AGAIN|UND_ERR_|ABORT_ERR)/.test(c)
    ) {
      return true;
    }
  }
  return false;
}

async function gql(origin, orgToken, query, variables) {
  const url = `${origin}/graphql`;
  let res;
  try {
    res = await fetch(url, {
      method: "POST",
      headers: {
        "content-type": "application/json",
        authorization: `Bearer ${orgToken}`,
      },
      body: JSON.stringify({ query, variables }),
    });
  } catch (e) {
    throw new Error(formatErrorDetail(e, { op: "gql-fetch", url }), { cause: e });
  }
  const text = await res.text();
  let body;
  try {
    body = text ? JSON.parse(text) : {};
  } catch (e) {
    throw new Error(
      formatErrorDetail(e, {
        op: "gql-parse",
        url,
        status: res.status,
        body: text,
      }),
      { cause: e },
    );
  }
  if (!res.ok) {
    throw new Error(
      formatErrorDetail(new Error(`HTTP ${res.status}`), {
        op: "gql-http",
        url,
        status: res.status,
        body: text,
      }),
    );
  }
  if (body.errors?.length) {
    const e = body.errors[0];
    throw new Error(
      formatErrorDetail(new Error(`${e.extensions?.code || "GQL"}: ${e.message}`), {
        op: "gql",
        url,
        status: res.status,
        body: JSON.stringify(body.errors).slice(0, 500),
      }),
    );
  }
  return body.data;
}

function setPos(tag, x, y, z) {
  tag.value.Pos = { type: "list", value: { type: "double", value: [x, y, z] } };
  tag.value.Dimension = { type: "string", value: "minecraft:overworld" };
  tag.value.Motion = {
    type: "list",
    value: { type: "double", value: [0, 0, 0] },
  };
}

function readPos(tag) {
  const p = tag.value?.Pos;
  if (!p) return null;
  if (p.value && Array.isArray(p.value.value)) return p.value.value.map(Number);
  if (Array.isArray(p.value)) return p.value.map(Number);
  return null;
}

function emptyPlayerTag() {
  return {
    name: "",
    type: "compound",
    value: {
      Pos: { type: "list", value: { type: "double", value: [0, 0, 0] } },
      Motion: { type: "list", value: { type: "double", value: [0, 0, 0] } },
      Dimension: { type: "string", value: "minecraft:overworld" },
    },
  };
}

/**
 * Closed triangle, one vertex about 25 blocks inside each region.
 * Order is the loop: pause, walk to the next, pause, then back to the first.
 * West owns x≤1999 and z≤1999. East owns x≥2000 and z≤1999. North owns z≥2000.
 */
export const TRIANGLE_VERTICES = Object.freeze([
  Object.freeze({ side: "west", x: 1975, z: 1975 }),
  Object.freeze({ side: "east", x: 2025, z: 1975 }),
  Object.freeze({ side: "north", x: 2000, z: 2025 }),
]);

/** Fully owned, 8 blocks past the chunk-125 line at block 2000. */
export const OWNED = Object.freeze({
  eastX: 2008,
  westX: 1991,
  northZ: 2008,
  southZ: 1991,
});

export function triangleSideForIndex(index) {
  const i = Number(index) | 0;
  const n = ((i % 3) + 3) % 3;
  return TRIANGLE_VERTICES[n].side;
}

/**
 * Pose on the vertex for `side`, nudged further into that region so a pack of
 * bots does not share one block and does not step toward the seam.
 * @param {number} index
 * @param {"west"|"east"|"north"} side
 */
export function triangleTarget(index, side) {
  const vertex = TRIANGLE_VERTICES.find((v) => v.side === side);
  if (!vertex) {
    throw new Error(`triangle side must be west, east, or north, got ${side}`);
  }
  const i = Math.abs(Number(index) | 0);
  const sx = (i % 3) * 0.8;
  const sz = (i % 5) * 0.6;
  if (side === "west") return { x: vertex.x - sx, y: 63, z: vertex.z - sz };
  if (side === "east") return { x: vertex.x + sx, y: 63, z: vertex.z - sz };
  return { x: vertex.x, y: 63, z: vertex.z + sz };
}

/**
 * Waypoints for one bot, first vertex first, then the next, then the loop.
 * Spec is `x,y,z,side` joined by `;` for LOADWALKER_WAYPOINTS / LT_MC_WAYPOINTS.
 * @param {number} index
 */
export function triangleWaypointSpec(index) {
  const start = ((Number(index) | 0) % 3 + 3) % 3;
  const vertices = [0, 1, 2].map((k) => TRIANGLE_VERTICES[(start + k) % 3]);
  const spec = vertices.map((v) => `${v.x},63,${v.z},${v.side}`).join(";");
  return { start: vertices[0].side, spec, vertices };
}

/**
 * Which server a bot joins.
 * Two-server: even index west, odd index east, when `alternate` is set.
 * Triangle (`north` host set): index % 3 is west, east, north. Even/odd is ignored.
 * @param {number} globalIndex
 * @param {{ west: string, east?: string, north?: string, port?: number, portWest?: number, portEast?: number, portNorth?: number, alternate?: boolean }} hosts
 */
export function homeForIndex(globalIndex, hosts) {
  const west = hosts.west;
  const east = hosts.east || "";
  const north = hosts.north || "";
  const port = Number(hosts.port || 25565);
  if (north) {
    const side = triangleSideForIndex(globalIndex);
    const host = side === "east" ? east : side === "north" ? north : west;
    const sidePort =
      side === "east"
        ? hosts.portEast
        : side === "north"
          ? hosts.portNorth
          : hosts.portWest;
    return { side, host, port: Number(sidePort || port), ckServer: side };
  }
  if (!hosts.alternate || !east) {
    return {
      side: "west",
      host: west,
      port: Number(hosts.portWest || port),
      ckServer: "west",
    };
  }
  if ((Number(globalIndex) & 1) === 1) {
    return {
      side: "east",
      host: east,
      port: Number(hosts.portEast || port),
      ckServer: "east",
    };
  }
  return {
    side: "west",
    host: west,
    port: Number(hosts.portWest || port),
    ckServer: "west",
  };
}

/** Map a transfer destination host back to west, east, or north. */
export function sideForJoinHost(host, hosts) {
  const west = hosts.west || "";
  const east = hosts.east || "";
  const north = hosts.north || "";
  if (north && host === north) return "north";
  if (east && host === east) return "east";
  if (west && host === west) return "west";
  const name = String(host || "");
  if (north && name.includes("north")) return "north";
  if (east && name.includes("east")) return "east";
  return "west";
}

/**
 * Server that fully owns this block.
 * Two-server mode uses x only (east ≥ 2008, west ≤ 1991).
 * Triangle mode also requires z: north at z ≥ 2008, and west/east only once z ≤ 1991.
 * The band between those lines is still the border and returns null.
 * @param {number} x
 * @param {number | null | undefined} z
 * @param {{ triangle?: boolean }} [opts]
 */
export function ownedSide(x, z, opts = {}) {
  if (x == null || Number.isNaN(Number(x))) return null;
  const px = Number(x);
  const pz =
    z == null || z === "" || Number.isNaN(Number(z)) ? null : Number(z);
  if (opts.triangle) {
    if (pz == null) return null;
    if (pz >= OWNED.northZ) return "north";
    if (pz > OWNED.southZ) return null;
    if (px >= OWNED.eastX) return "east";
    if (px <= OWNED.westX) return "west";
    return null;
  }
  if (px >= OWNED.eastX) return "east";
  if (px <= OWNED.westX) return "west";
  return null;
}

/**
 * A lap counts only after this bot has been on west, east, and north.
 * The stay that closes the lap does not also open the next one.
 * @param {{ visited?: Iterable<string>, laps?: number, lapHold?: string | null }} state
 * @param {string | null} side
 */
export function recordOwnerVisit(state, side) {
  const visited = new Set(state?.visited || []);
  const laps = state?.laps || 0;
  const lapHold = state?.lapHold || null;
  if (!side) return { visited, laps, lapHold, counted: false };
  if (lapHold === side && visited.size === 0) {
    return { visited, laps, lapHold, counted: false };
  }
  visited.add(side);
  if (visited.has("west") && visited.has("east") && visited.has("north")) {
    return { visited: new Set(), laps: laps + 1, lapHold: side, counted: true };
  }
  return { visited, laps, lapHold: null, counted: false };
}

/**
 * Seam stagger for LoadN. index is 0-based LoadN-1.
 * Home side must match join host: west of the chunk line (x below 2000) for west
 * joins, east of the line (x 2000+) for east joins — otherwise Fabric sends
 * "belongs on west/east" and LoadWalker exits on ClientboundTransferPacket.
 *
 * Triangle (`opts.triangle`, or side `north`) poses at that side's vertex instead.
 *
 * @param {number} index
 * @param {{ side?: string, server?: string, triangle?: boolean } | string} [sideOrOpts]
 *   `"east"` / `{ side: "east" }` / `{ server: "east" }` → x≈2009.5–2011.1;
 *   otherwise west → x≈1990.5–1992.1. Same z stagger either side.
 *   `{ triangle: true, side }` or side `north` → the closed-triangle vertex.
 */
export function seamTargetForIndex(index, sideOrOpts) {
  const i = Number(index) | 0;
  const opts =
    typeof sideOrOpts === "string" ? { side: sideOrOpts } : sideOrOpts || {};
  const side = opts.side || opts.server || "west";
  if (opts.triangle || side === "north") {
    const poseSide = side === "east" || side === "west" || side === "north" ? side : "west";
    return triangleTarget(i, poseSide);
  }
  const east = side === "east";
  return {
    // west: 1990.5 + (i%3)*0.8 → 1990.5..1992.1 (west of seam)
    // east: 2009.5 + (i%3)*0.8 → 2009.5..2011.1 (east of seam, in walk band)
    x: (east ? 2009.5 : 1990.5) + (i % 3) * 0.8,
    y: 63,
    z: -55 - (i % 5) * 0.6,
  };
}

/**
 * In-game placement check for LoadWalker logs. Seam walk band is ~1986–2013;
 * world-spawn stranding is near (0, ~100, 0) — hundreds of blocks away.
 * @param {{ x: number, y: number, z: number } | null | undefined} pos
 * @param {{ x: number, y: number, z: number }} target
 * @param {{ maxHoriz?: number, maxY?: number }} [opts]
 */
export function nearSeam(pos, target, opts = {}) {
  if (!pos || !target) return false;
  const maxHoriz = opts.maxHoriz ?? 80;
  const maxY = opts.maxY ?? 25;
  const dx = Number(pos.x) - Number(target.x);
  const dz = Number(pos.z) - Number(target.z);
  const dy = Number(pos.y) - Number(target.y);
  return Math.hypot(dx, dz) <= maxHoriz && Math.abs(dy) <= maxY;
}

/** True when the bot looks stuck at overworld spawn instead of the seam. */
export function looksLikeWorldSpawn(pos) {
  if (!pos) return false;
  const x = Number(pos.x);
  const y = Number(pos.y);
  const z = Number(pos.z);
  // Spawn island / default join: |x|,|z| small; y often rooftop ~100+, not walkY 63.
  return Math.abs(x) < 64 && Math.abs(z) < 64 && (y < 50 || y > 80 || Math.hypot(x, z) < 48);
}

/**
 * Overwrite the bot's CK hub Pos to the seam so the next Fabric join places them there
 * (even if a prior leave saved world-spawn coords).
 * @param {string} name
 * @param {number} index
 * @param {{ server?: string, side?: string, target?: { x: number, y: number, z: number } }} [opts]
 *   Fabric server id for claim/save (`west` / `east` / `north`). Pos is chosen for that side
 *   unless `target` is passed explicitly. Pass `triangle: true` to pose on the
 *   closed-triangle vertex instead of the two-server seam band.
 */
export async function writeSeamPose(name, index, opts = {}) {
  const origin = process.env.CROWDY_API_ORIGIN || DEFAULT_ORIGIN;
  const appId = process.env.CROWDY_APP_ID || DEFAULT_APP_ID;
  const serverId = opts.server || process.env.CROWDY_MC_SERVER || DEFAULT_SERVER;
  const orgToken = mustEnv("CROWDY_ORG_TOKEN");
  const pepper = mustEnv("CROWDY_ID_PEPPER");
  const clusterSecret = mustEnv("CROWDY_CLUSTER_SECRET");
  const target =
    opts.target ||
    seamTargetForIndex(index, {
      side: opts.side || serverId,
      triangle: opts.triangle,
    });
  const { CrowdyClient } = await import("@crowdedkingdoms/crowdyjs");
  const nbtMod = await import("prismarine-nbt");
  const nbt = nbtMod.default ?? nbtMod;

  const mcUuid = offlineUuid(name);
  const ext = externalId(pepper, mcUuid);

  try {
    const mint = await gql(
      origin,
      orgToken,
      `mutation Mint($i: MintManagedPlayerTokenInput!) {
      mintManagedPlayerToken(input: $i) {
        token appId expiresAt gameApiUrl gameApiWsUrl discoveryUrl launchUrl
      }
    }`,
      { i: { appId, externalId: ext, ageYears: 36, country: "CA" } },
    );
    const tokenResponse = mint.mintManagedPlayerToken;
    const client = new CrowdyClient({ httpUrl: origin });
    client.portal.adoptAppToken(tokenResponse);
    const me = await client.users.me();
    const userId = String(me.userId);

    const exec = await client.exec.connect(appId, { nodeType: "player", key: userId });
    try {
      const view = await exec.call("player", userId, "mc_get");
      const version = Number(view.version ?? 0);
      let parsed;
      if (!view.data) {
        parsed = emptyPlayerTag();
      } else {
        const raw = Buffer.from(view.data, "base64");
        let uncompressed;
        try {
          uncompressed = gunzipSync(raw);
        } catch {
          uncompressed = raw;
        }
        parsed = nbt.parseUncompressed(uncompressed);
      }

      setPos(parsed, target.x, target.y, target.z);
      const outBytes = gzipSync(nbt.writeUncompressed(parsed));
      const nextVersion = version + 1;
      const seal = sealRecord(clusterSecret, userId, nextVersion, outBytes);
      const data = outBytes.toString("base64");

      await exec.call("player", userId, "mc_claim", { server: serverId });
      const rid = `${serverId}-${nextVersion}-${randomBytes(8).toString("hex")}`;
      await exec.call("player", userId, "mc_save", {
        server: serverId,
        version: nextVersion,
        data,
        seal,
        handoff: "",
        rid,
      });
      await exec.call("player", userId, "mc_release", { server: serverId });

      const check = await exec.call("player", userId, "mc_get");
      const checkRaw = Buffer.from(check.data, "base64");
      let checkUn;
      try {
        checkUn = gunzipSync(checkRaw);
      } catch {
        checkUn = checkRaw;
      }
      const after = readPos(nbt.parseUncompressed(checkUn));
      const [ax, ay, az] = after || [];
      if (
        Math.abs(ax - target.x) > 0.01 ||
        Math.abs(ay - target.y) > 0.01 ||
        Math.abs(az - target.z) > 0.01
      ) {
        throw new Error(`Pos did not stick for ${name}: got ${JSON.stringify(after)}`);
      }
      return { name, userId, target, pos: after };
    } finally {
      exec.close();
    }
  } catch (e) {
    // CrowdyClient / undici often surface only "fetch failed"; keep cause chain + URL.
    if (String(e?.message || "").includes("op=")) throw e;
    throw new Error(formatErrorDetail(e, { op: "writeSeamPose", url: origin }), {
      cause: e,
    });
  }
}

function selfTest() {
  const fails = [];
  const eq = (name, got, expect) => {
    const a = JSON.stringify(got);
    const b = JSON.stringify(expect);
    if (a !== b) fails.push(`${name}: ${a} != ${b}`);
  };
  eq("west seam 0", seamTargetForIndex(0, "west"), { x: 1990.5, y: 63, z: -55 });
  eq("east seam 1", seamTargetForIndex(1, { side: "east" }), {
    x: 2010.3,
    y: 63,
    z: -55.6,
  });
  eq("east seam via server", seamTargetForIndex(4, { server: "east" }), {
    x: 2010.3,
    y: 63,
    z: -57.4,
  });
  eq("triangle west 0", seamTargetForIndex(0, { side: "west", triangle: true }), {
    x: 1975,
    y: 63,
    z: 1975,
  });
  eq("triangle east 1", seamTargetForIndex(1, { side: "east", triangle: true }), {
    x: 2025.8,
    y: 63,
    z: 1974.4,
  });
  eq("triangle north 2", seamTargetForIndex(2, { side: "north" }), {
    x: 2000,
    y: 63,
    z: 2026.2,
  });
  const hosts = {
    west: "10.0.0.1",
    east: "10.0.0.2",
    north: "10.0.0.3",
    port: 25565,
    portWest: 25565,
    portEast: 25565,
    portNorth: 25565,
    alternate: true,
  };
  eq("even west", homeForIndex(0, { ...hosts, north: "" }).side, "west");
  eq("odd east", homeForIndex(1, { ...hosts, north: "" }).side, "east");
  eq("even again", homeForIndex(2, { ...hosts, north: "" }).side, "west");
  eq("no alternate", homeForIndex(1, { ...hosts, north: "", alternate: false }).side, "west");
  const starts = [];
  for (let i = 0; i < 10; i++) {
    const home = homeForIndex(i, hosts);
    starts.push(home.side);
    if (home.ckServer !== home.side) fails.push(`ckServer ${i}`);
    const pose = seamTargetForIndex(i, { side: home.side, triangle: true });
    if (ownedSide(pose.x, pose.z, { triangle: true }) !== home.side) {
      fails.push(`pose ${i} ${pose.x},${pose.z} not owned by ${home.side}`);
    }
  }
  eq("ten-bot starts", starts, [
    "west",
    "east",
    "north",
    "west",
    "east",
    "north",
    "west",
    "east",
    "north",
    "west",
  ]);
  eq(
    "loop 0",
    triangleWaypointSpec(0).spec,
    "1975,63,1975,west;2025,63,1975,east;2000,63,2025,north",
  );
  eq(
    "loop 1",
    triangleWaypointSpec(1).spec,
    "2025,63,1975,east;2000,63,2025,north;1975,63,1975,west",
  );
  eq("join north", sideForJoinHost("10.0.0.3", hosts), "north");
  eq("join east name", sideForJoinHost("seam-east", hosts), "east");
  eq("join north name before east", sideForJoinHost("north-east", hosts), "north");
  const own = (x, z, triangle) => ownedSide(x, z, { triangle });
  eq("two-server east", own(2013, -55, false), "east");
  eq("two-server west", own(1986, -55, false), "west");
  eq("two-server border", own(2000, -55, false), null);
  eq("tri west", own(1975, 1975, true), "west");
  eq("tri east", own(2025, 1975, true), "east");
  eq("tri north", own(2000, 2025, true), "north");
  eq("tri north any x", own(1975, 2025, true), "north");
  eq("tri border z", own(2025, 1995, true), null);
  eq("tri border x", own(2000, 1975, true), null);
  eq("tri missing z", own(2025, null, true), null);
  let lap = { visited: new Set(), laps: 0, lapHold: null };
  for (const side of ["west", "east", "west", "north"]) {
    lap = recordOwnerVisit(lap, side);
  }
  if (lap.laps !== 1 || lap.lapHold !== "north" || lap.visited.size !== 0) {
    fails.push(`lap close laps=${lap.laps} hold=${lap.lapHold} visited=${lap.visited.size}`);
  }
  lap = recordOwnerVisit(lap, "north");
  if (lap.laps !== 1) fails.push("holding north recounted");
  lap = recordOwnerVisit(lap, "west");
  lap = recordOwnerVisit(lap, "east");
  if (lap.laps !== 1) fails.push("second lap early");
  lap = recordOwnerVisit(lap, "north");
  if (lap.laps !== 2) fails.push(`second lap ${lap.laps}`);
  if (fails.length) {
    for (const line of fails) console.error(`FAIL ${line}`);
    console.error(`${fails.length} failed`);
    return 1;
  }
  console.log("seam-pose self-test ok");
  return 0;
}

function runningAsScript() {
  const entry = process.argv[1];
  if (!entry) return false;
  return import.meta.url === pathToFileURL(entry).href;
}

if (runningAsScript()) {
  if (process.argv.includes("--print-triangle")) {
    console.log(JSON.stringify(TRIANGLE_VERTICES));
  } else if (process.argv.includes("--self-test")) {
    process.exit(selfTest());
  }
}
