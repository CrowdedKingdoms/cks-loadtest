/**
 * Write a LoadWalker's CK hub player-record Pos to the west seam before spawn.
 * Env: CROWDY_ORG_TOKEN, CROWDY_ID_PEPPER, CROWDY_CLUSTER_SECRET
 * Optional: CROWDY_API_ORIGIN (default ck.test), CROWDY_APP_ID, CROWDY_MC_SERVER (default west)
 */
import { createHmac, createHash, randomBytes } from "node:crypto";
import { gzipSync, gunzipSync } from "node:zlib";
import { CrowdyClient } from "@crowdedkingdoms/crowdyjs";
import nbt from "prismarine-nbt";

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

async function gql(origin, orgToken, query, variables) {
  const res = await fetch(`${origin}/graphql`, {
    method: "POST",
    headers: {
      "content-type": "application/json",
      authorization: `Bearer ${orgToken}`,
    },
    body: JSON.stringify({ query, variables }),
  });
  const body = await res.json();
  if (body.errors?.length) {
    const e = body.errors[0];
    throw new Error(`${e.extensions?.code || "GQL"}: ${e.message}`);
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

/** Same stagger as fix-load-poses.mjs: west of the line, y=63. index is 0-based LoadN-1. */
export function seamTargetForIndex(index) {
  const i = Number(index) | 0;
  return {
    x: 1990.5 + (i % 3) * 0.8,
    y: 63,
    z: -55 - (i % 5) * 0.6,
  };
}

/**
 * Overwrite the bot's CK hub Pos to the west seam so the next Fabric join places them there
 * (even if a prior leave saved world-spawn coords).
 */
export async function writeSeamPose(name, index) {
  const origin = process.env.CROWDY_API_ORIGIN || DEFAULT_ORIGIN;
  const appId = process.env.CROWDY_APP_ID || DEFAULT_APP_ID;
  const serverId = process.env.CROWDY_MC_SERVER || DEFAULT_SERVER;
  const orgToken = mustEnv("CROWDY_ORG_TOKEN");
  const pepper = mustEnv("CROWDY_ID_PEPPER");
  const clusterSecret = mustEnv("CROWDY_CLUSTER_SECRET");
  const target = seamTargetForIndex(index);

  const mcUuid = offlineUuid(name);
  const ext = externalId(pepper, mcUuid);

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
}
