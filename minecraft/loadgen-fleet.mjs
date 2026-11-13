#!/usr/bin/env node
/**
 * SoftMax-style OVH loadgen fleet for Minecraft seam tests (test project only).
 *
 * Mirrors Crewrift SoftMax / cks-loadtest generators:
 *   - bare Ubuntu 24.04 OVH VMs (no containers for bots)
 *   - Tailscale --ssh via cloud-init
 *   - inventory in loadgen-fleet.json ({ name, id, flavor, flavorActual, vcpus, ip, … })
 *   - teardown requires an explicit confirm flag (VMs stay up after rungs)
 *
 *   LOADGEN_STATE=…/loadgen-fleet.json \
 *   LOADGEN_SECRETS_ENV=…/credentials.test.env \
 *   LOADGEN_PREFIX=test-mc-lg \
 *   node minecraft/loadgen-fleet.mjs up <count> [flavor]
 *
 *   node minecraft/loadgen-fleet.mjs status
 *   node minecraft/loadgen-fleet.mjs down --confirm-benjamin-teardown
 *
 * Default flavor is b3-16 (SoftMax). Prefer that over the one-box b3-8 shortcut.
 * Scaling SoftMax's 10 generators is `up 10 b3-16` — same script, not a rewrite.
 */
import { execSync } from "node:child_process";
import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import os from "node:os";

const __dirname = path.dirname(fileURLToPath(import.meta.url));

const DEFAULT_CRED = path.join(
  os.homedir(),
  "Documents/CrowdedKingdoms/cks-project-root/creds/remote-creds/credentials.test.env",
);
const DEFAULT_PUB = path.join(
  os.homedir(),
  ".local/state/cks-test-mc-cluster/id_ed25519.pub",
);
const DEFAULT_STATE = path.join(
  os.homedir(),
  ".local/state/cks-mc-loadtest-run/loadgen-fleet.json",
);

const CRED = process.env.LOADGEN_SECRETS_ENV || DEFAULT_CRED;
/** SoftMax Tailscale node keys often live beside OVH creds in load-testing-creds/. */
const DEFAULT_TS_CRED = path.join(
  os.homedir(),
  "Documents/CrowdedKingdoms/cks-project-root/creds/remote-creds/load-testing-creds/ben-dev-test.env",
);
const TS_CRED = process.env.LOADGEN_TS_SECRETS_ENV || DEFAULT_TS_CRED;
const PUB_PATH = process.env.LOADGEN_SSH_PUBKEY || DEFAULT_PUB;
const STATE = process.env.LOADGEN_STATE || DEFAULT_STATE;
const REGION = process.env.LOADGEN_REGION || "US-EAST-VA-1";
const IMAGE_NAME = "Ubuntu 24.04";
/** SoftMax used test-crewrift-lg-*; one-box shortcut was test-mc-loadgen-1 (b3-8). */
const PREFIX = process.env.LOADGEN_PREFIX || "test-mc-lg";
const BASE = "https://api.us.ovhcloud.com/1.0";
const SSH_KEY_NAME = process.env.LOADGEN_SSH_KEY_NAME || "test-mc-cluster-ephemeral";
const DEFAULT_FLAVOR = process.env.LOADGEN_FLAVOR || "b3-16";

const FLAVOR_META = {
  "b3-8": { vcpus: 2, linkMbps: 500 },
  "b3-16": { vcpus: 4, linkMbps: 1000 },
  "b3-32": { vcpus: 8, linkMbps: 2000 },
  "b3-64": { vcpus: 16, linkMbps: 4000 },
};

function parseEnv(filePath) {
  const out = {};
  for (const raw of fs.readFileSync(filePath, "utf8").split("\n")) {
    let line = raw.trim();
    if (!line) continue;
    const commented = line.startsWith("#");
    if (commented) line = line.replace(/^#\s*/, "");
    const m = /^(?:export\s+)?([A-Za-z_][A-Za-z0-9_]*)=(.*)$/.exec(line);
    if (!m) continue;
    let v = m[2].trim();
    if (
      (v.startsWith('"') && v.endsWith('"')) ||
      (v.startsWith("'") && v.endsWith("'"))
    ) {
      v = v.slice(1, -1);
    }
    if (!v) continue;
    if (!(m[1] in out) || (!out[m[1]] && v)) out[m[1]] = v;
  }
  return out;
}

if (!fs.existsSync(CRED)) {
  throw new Error(`LOADGEN_SECRETS_ENV missing: ${CRED}`);
}
const secrets = parseEnv(CRED);
const tsSecrets = fs.existsSync(TS_CRED) ? parseEnv(TS_CRED) : {};
const PROJECT = secrets.OVH_PROJECT_ID;
if (
  !PROJECT ||
  !secrets.OVH_APP_KEY ||
  !secrets.OVH_APP_SECRET ||
  !secrets.OVH_CONSUMER_KEY
) {
  throw new Error("credentials env missing OVH fields");
}
function firstTruthy(...vals) {
  for (const v of vals) {
    if (v && String(v).trim()) return String(v).trim();
  }
  return "";
}
// Prefer a live NODE key from load-testing-creds; credentials.test.env often has
// empty NODE/CP placeholders and a stale commented AUTH key that OVH cloud-init
// then rejects with "API key does not exist".
const TS_AUTH = firstTruthy(
  process.env.TAILSCALE_NODE_AUTH_KEY,
  process.env.TAILSCALE_AUTH_KEY,
  tsSecrets.TAILSCALE_NODE_AUTH_KEY,
  tsSecrets.TAILSCALE_AUTH_KEY,
  secrets.TAILSCALE_NODE_AUTH_KEY,
  secrets.TAILSCALE_AUTH_KEY,
  secrets.TAILSCALE_CP_AUTH_KEY,
);
if (!TS_AUTH) {
  throw new Error(
    "Tailscale auth key missing (set LOADGEN_TS_SECRETS_ENV or TAILSCALE_NODE_AUTH_KEY)",
  );
}
if (!fs.existsSync(PUB_PATH)) {
  throw new Error(`SSH pubkey missing: ${PUB_PATH}`);
}
const PUB = fs.readFileSync(PUB_PATH, "utf8").trim();

let skew = 0;
async function ovh(method, p, body) {
  const url = BASE + p;
  const payload = body === undefined ? "" : JSON.stringify(body);
  const ts = Math.floor(Date.now() / 1000) + skew;
  const sig =
    "$1$" +
    crypto
      .createHash("sha1")
      .update(
        [
          secrets.OVH_APP_SECRET,
          secrets.OVH_CONSUMER_KEY,
          method,
          url,
          payload,
          String(ts),
        ].join("+"),
      )
      .digest("hex");
  const res = await fetch(url, {
    method,
    headers: {
      "X-Ovh-Application": secrets.OVH_APP_KEY,
      "X-Ovh-Consumer": secrets.OVH_CONSUMER_KEY,
      "X-Ovh-Timestamp": String(ts),
      "X-Ovh-Signature": sig,
      ...(payload ? { "Content-Type": "application/json" } : {}),
    },
    ...(payload ? { body: payload } : {}),
  });
  const text = await res.text();
  if (!res.ok) {
    throw new Error(
      `OVH ${method} ${p.replaceAll(PROJECT, "<project>")} -> ${res.status} ${text
        .replaceAll(PROJECT, "<project>")
        .slice(0, 400)}`,
    );
  }
  return text ? JSON.parse(text) : null;
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const log = (...a) => console.log("[mc-loadgen]", ...a);
const readState = () =>
  fs.existsSync(STATE)
    ? JSON.parse(fs.readFileSync(STATE, "utf8"))
    : { prefix: PREFIX, region: REGION, vms: [], createdAt: null };
const writeState = (s) => {
  fs.mkdirSync(path.dirname(STATE), { recursive: true });
  fs.writeFileSync(STATE, JSON.stringify(s, null, 2), { mode: 0o600 });
};

function cloudInit(hostname) {
  // SoftMax / Crewrift: Tailscale --ssh, base packages. Java/Node installed at
  // harness install time (install-generators.sh), not in cloud-init — same as
  // SoftMax shipping the binary after the box is up.
  return `#cloud-config
hostname: ${hostname}
package_update: true
packages: [curl, ca-certificates]
runcmd:
  - |
    set -e
    echo '* soft nofile 200000' >> /etc/security/limits.conf
    echo '* hard nofile 200000' >> /etc/security/limits.conf
  - |
    curl -fsSL https://tailscale.com/install.sh | sh
    tailscale up --authkey=${TS_AUTH} --ssh --hostname=${hostname} --accept-routes=false || true
final_message: "mc SoftMax-style loadgen ready"
`;
}

function enrichVm(vm, flavorName, ovhDetail) {
  const meta = FLAVOR_META[flavorName] || {};
  const actualName =
    ovhDetail?.flavor?.name || vm.flavorActual || flavorName;
  const actualMeta = FLAVOR_META[actualName] || meta;
  vm.flavor = flavorName;
  vm.flavorActual = actualName;
  vm.vcpus = actualMeta.vcpus ?? vm.vcpus ?? null;
  vm.linkMbps = actualMeta.linkMbps ?? vm.linkMbps ?? null;
  return vm;
}

async function ensureKey() {
  const keys = await ovh("GET", `/cloud/project/${PROJECT}/sshkey`);
  let key = keys.find((k) => k.name === SSH_KEY_NAME);
  if (!key) {
    key = await ovh("POST", `/cloud/project/${PROJECT}/sshkey`, {
      name: SSH_KEY_NAME,
      publicKey: PUB,
    });
    log("uploaded ssh key", SSH_KEY_NAME);
  }
  return key;
}

async function up(count, flavorName) {
  const key = await ensureKey();
  const [flavors, images] = await Promise.all([
    ovh("GET", `/cloud/project/${PROJECT}/flavor?region=${REGION}`),
    ovh("GET", `/cloud/project/${PROJECT}/image?osType=linux&region=${REGION}`),
  ]);
  const flavor = flavors.find(
    (f) => f.name === flavorName && f.available !== false && !f.disabled,
  );
  if (!flavor) throw new Error(`flavor ${flavorName} unavailable in ${REGION}`);
  const image =
    images.find((i) => i.name === IMAGE_NAME) ||
    images.find((i) => /Ubuntu 24\.04/.test(i.name || ""));
  if (!image) throw new Error(`${IMAGE_NAME} image not found in ${REGION}`);

  const state = readState();
  state.prefix = PREFIX;
  state.region = REGION;
  state.notes =
    "SoftMax-style bare Ubuntu generators. Stay up until Benjamin orders teardown. No auto-down after rungs.";
  if (!state.createdAt) state.createdAt = new Date().toISOString();
  const existing = new Set(state.vms.map((v) => v.name));

  for (let i = 1; i <= count; i++) {
    const name = `${PREFIX}-${i}`;
    if (existing.has(name)) {
      log(`${name} already tracked, skipping create`);
      continue;
    }
    const inst = await ovh("POST", `/cloud/project/${PROJECT}/instance`, {
      name,
      region: REGION,
      flavorId: flavor.id,
      imageId: image.id,
      sshKeyId: key.id,
      monthlyBilling: false,
      userData: cloudInit(name),
    });
    log(`created ${name} id=${inst.id} flavor=${flavorName}`);
    const row = enrichVm(
      {
        name,
        id: inst.id,
        region: REGION,
        createdAt: new Date().toISOString(),
      },
      flavorName,
      { flavor: { name: flavorName } },
    );
    state.vms.push(row);
    writeState(state);
  }

  for (const vm of state.vms) {
    for (let t = 0; t < 60; t++) {
      const d = await ovh("GET", `/cloud/project/${PROJECT}/instance/${vm.id}`);
      const pub =
        (d.ipAddresses || []).find((a) => a.version === 4 && a.type === "public")
          ?.ip || "";
      log(`poll ${vm.name} status=${d.status} ip=${pub || "pending"}`);
      if (d.status === "ACTIVE" && pub) {
        vm.ip = pub;
        vm.publicIp = pub;
        vm.status = d.status;
        enrichVm(vm, vm.flavor || flavorName, d);
        break;
      }
      if (["ERROR", "DELETED"].includes(d.status)) {
        throw new Error(`${vm.name} entered ${d.status}`);
      }
      await sleep(10000);
    }
  }
  writeState(state);
  log("waiting for cloud-init / Tailscale (~90s)");
  await sleep(90000);

  for (const vm of state.vms) {
    let ok = false;
    for (let t = 0; t < 36; t++) {
      try {
        const status = execSync("tailscale status", { encoding: "utf8" });
        const line = status
          .split("\n")
          .find((l) => {
            const parts = l.trim().split(/\s+/);
            return (
              parts[1] === vm.name ||
              (parts[1] && parts[1].startsWith(vm.name + "-"))
            ) && !l.includes("offline");
          });
        if (line) {
          const tip = line.split(/\s+/)[0];
          vm.tailnetIp = tip;
          vm.tailnetName = vm.name;
          execSync(
            `tailscale ssh -o ConnectTimeout=10 ubuntu@${tip} -- 'echo ok && uname -a && nproc'`,
            { stdio: "pipe" },
          );
          log(`${vm.name} reachable over Tailscale ${tip}`);
          ok = true;
          break;
        }
      } catch {
        /* retry */
      }
      await sleep(10000);
    }
    if (!ok) log(`WARN ${vm.name} not yet reachable over Tailscale`);
  }
  writeState(state);
  log("READY", STATE);
  console.log(JSON.stringify(state, null, 2));
}

async function status() {
  const state = readState();
  for (const vm of state.vms) {
    try {
      const d = await ovh("GET", `/cloud/project/${PROJECT}/instance/${vm.id}`);
      const pub =
        (d.ipAddresses || []).find((a) => a.version === 4 && a.type === "public")
          ?.ip || "";
      vm.ip = pub || vm.ip;
      vm.publicIp = vm.ip;
      vm.status = d.status;
      enrichVm(vm, vm.flavor || DEFAULT_FLAVOR, d);
      log(
        `${vm.name} ovh=${d.status} ip=${pub} flavor=${vm.flavorActual || vm.flavor} vcpus=${vm.vcpus}`,
      );
    } catch (e) {
      log(`${vm.name} status error: ${e.message}`);
    }
  }
  writeState(state);
  console.log(JSON.stringify(state, null, 2));
}

async function down() {
  const state = readState();
  for (const vm of state.vms) {
    log(`destroying ${vm.name} ${vm.id}`);
    await ovh("DELETE", `/cloud/project/${PROJECT}/instance/${vm.id}`);
  }
  state.vms = [];
  state.destroyedAt = new Date().toISOString();
  writeState(state);
  log("destroyed all tracked loadgen VMs in", STATE);
}

const argv = process.argv.slice(2);
const cmd = argv[0];
const a = argv[1];
const b = argv[2];
const time = Number(await (await fetch(BASE + "/auth/time")).text());
skew = time - Math.floor(Date.now() / 1000);

if (cmd === "up") {
  const count = Number(a || "1");
  const flavor = b || DEFAULT_FLAVOR;
  if (!(count > 0)) throw new Error("up <count> [flavor]");
  if (flavor === "b3-8") {
    log(
      "NOTE: SoftMax used b3-16; b3-8 is the one-box shortcut that OOMed ~25 bots. Prefer b3-16.",
    );
  }
  await up(count, flavor);
} else if (cmd === "status") {
  await status();
} else if (cmd === "down") {
  console.error(
    "Refusing auto-teardown. Pass --confirm-benjamin-teardown if Benjamin ordered it.",
  );
  if (argv.includes("--confirm-benjamin-teardown")) {
    await down();
  } else {
    process.exit(2);
  }
} else {
  console.error(
    "usage: loadgen-fleet.mjs up <count> [flavor]|status|down --confirm-benjamin-teardown",
  );
  process.exit(2);
}
