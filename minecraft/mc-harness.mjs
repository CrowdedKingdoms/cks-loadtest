#!/usr/bin/env node
/**
 * Minecraft LoadWalker harness that speaks the cks-loadtest HTTP control API.
 * Each client is a LoadWalker JVM (same TCP seam choreography). Not Buddy UDP.
 */
import http from "node:http";
import { spawn } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import os from "node:os";
import { fileURLToPath } from "node:url";
import {
  writeSeamPose,
  seamTargetForIndex,
  formatErrorDetail,
  isTransientNetworkError,
} from "./seam-pose.mjs";

const __dirname = path.dirname(fileURLToPath(import.meta.url));

function env(name, fallback = "") {
  return process.env[name] ?? fallback;
}

const SEAM_POSE_ATTEMPTS = Math.max(1, Number(env("LT_SEAM_POSE_ATTEMPTS", "3")));
const SEAM_POSE_RETRY_MS = Math.max(0, Number(env("LT_SEAM_POSE_RETRY_MS", "1500")));

function ts() {
  return new Date().toISOString();
}

async function writeSeamPoseWithRetries(name, index) {
  let lastErr;
  for (let attempt = 1; attempt <= SEAM_POSE_ATTEMPTS; attempt++) {
    try {
      if (attempt > 1) {
        console.error(
          `${ts()} mc-harness: seam pose retry ${attempt}/${SEAM_POSE_ATTEMPTS} for ${name} after ${SEAM_POSE_RETRY_MS}ms`,
        );
        await new Promise((r) => setTimeout(r, SEAM_POSE_RETRY_MS));
      }
      return await writeSeamPose(name, index);
    } catch (e) {
      lastErr = e;
      const detail = formatErrorDetail(e, {
        op: "seam-pose",
        attempt,
        attempts: SEAM_POSE_ATTEMPTS,
        url: process.env.CROWDY_API_ORIGIN || "(default ck.test)",
      });
      const retry = attempt < SEAM_POSE_ATTEMPTS && isTransientNetworkError(e);
      console.error(
        `${ts()} mc-harness: seam pose failed for ${name}: ${detail}${retry ? " (will retry)" : ""}`,
      );
      if (!retry) break;
    }
  }
  throw lastErr;
}

function requireEnv(name) {
  const v = process.env[name];
  if (!v) {
    console.error(`mc-harness: ${name} is required`);
    process.exit(2);
  }
  return v;
}

const CONTROL_BIND = env("LT_CONTROL_BIND", "127.0.0.1:9109");
const CONTROL_TOKEN = env("LT_CONTROL_TOKEN", "");
const MC_HOST = requireEnv("LT_MC_HOST");
const MC_PORT = env("LT_MC_PORT", "25565");
const MC_DIST = requireEnv("LT_MC_DIST");
const INDEX_BASE = Number(env("LT_INDEX_BASE", "0"));
const INDEX_LIMIT = Number(env("LT_INDEX_LIMIT", "8"));
const INSTANCE_ID = env("LT_INSTANCE_ID", os.hostname());
const STATS_DIR = env("LT_STATS_DIR", "");
const NAME_PREFIX = env("LT_MC_NAME_PREFIX", "Load");
const JAVA = env("LT_MC_JAVA", "java");
const START_CLIENTS = Number(env("LT_CLIENTS", "0"));

if (CONTROL_BIND !== "off") {
  const host = CONTROL_BIND.split(":")[0];
  if (host !== "127.0.0.1" && host !== "localhost" && host !== "::1" && !CONTROL_TOKEN) {
    console.error("mc-harness: LT_CONTROL_TOKEN required when binding off loopback");
    process.exit(2);
  }
}

const runSh = path.join(MC_DIST, "run.sh");
if (!fs.existsSync(runSh)) {
  console.error(`mc-harness: no run.sh in LT_MC_DIST=${MC_DIST}`);
  process.exit(2);
}

/** @type {Map<number, { proc: import('node:child_process').ChildProcess, name: string, transfers: number, east: number, west: number, alive: boolean }>} */
const clients = new Map();
let used = 0;
let busy = false;
let addError = "";
let shuttingDown = false;
let currentRung = null;
/** @type {{ id: string, openedAt: number, transfers: number, east: number, west: number } | null} */
let openWindow = null;

const lifetime = {
  transfers: 0,
  east_pauses: 0,
  west_pauses: 0,
  starts: 0,
  exits: 0,
};

function botName(globalIndex) {
  return `${NAME_PREFIX}${globalIndex + 1}`;
}

function authorize(req) {
  if (!CONTROL_TOKEN) return true;
  const h = req.headers.authorization || "";
  return h === `Bearer ${CONTROL_TOKEN}`;
}

function readBody(req) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    req.on("data", (c) => chunks.push(c));
    req.on("end", () => {
      const raw = Buffer.concat(chunks).toString("utf8");
      if (!raw) return resolve({});
      try {
        resolve(JSON.parse(raw));
      } catch (e) {
        reject(e);
      }
    });
    req.on("error", reject);
  });
}

function json(res, code, body) {
  const data = JSON.stringify(body);
  res.writeHead(code, {
    "content-type": "application/json",
    "content-length": Buffer.byteLength(data),
  });
  res.end(data);
}

function spawnWalker(globalIndex) {
  const name = botName(globalIndex);
  const logDir = STATS_DIR ? path.join(STATS_DIR, "walkers") : path.join(os.tmpdir(), "ck-mc-walkers");
  fs.mkdirSync(logDir, { recursive: true });
  const logPath = path.join(logDir, `${name}.log`);
  const logFd = fs.openSync(logPath, "a");
  const proc = spawn(runSh, [name, MC_HOST, String(MC_PORT)], {
    cwd: MC_DIST,
    env: process.env,
    stdio: ["ignore", logFd, logFd],
    detached: false,
  });
  fs.closeSync(logFd);

  const state = { proc, name, transfers: 0, east: 0, west: 0, alive: true };
  clients.set(globalIndex, state);
  lifetime.starts++;

  // Tail log for metrics (best-effort)
  const stream = fs.createReadStream(logPath, { encoding: "utf8", start: 0 });
  let buf = "";
  const onChunk = (chunk) => {
    buf += chunk;
    let i;
    while ((i = buf.indexOf("\n")) >= 0) {
      const line = buf.slice(0, i);
      buf = buf.slice(i + 1);
      if (line.includes("TRANSFER FAILURE")) {
        state.transfers++;
        lifetime.transfers++;
        if (openWindow) openWindow.transfers++;
      }
      if (line.includes("east pause")) {
        state.east++;
        lifetime.east_pauses++;
        if (openWindow) openWindow.east++;
      }
      if (line.includes("west pause")) {
        state.west++;
        lifetime.west_pauses++;
        if (openWindow) openWindow.west++;
      }
    }
  };
  // Follow with fs.watchFile
  let pos = 0;
  try {
    pos = fs.statSync(logPath).size;
  } catch {
    pos = 0;
  }
  const watch = setInterval(() => {
    try {
      const st = fs.statSync(logPath);
      if (st.size > pos) {
        const fd = fs.openSync(logPath, "r");
        const len = st.size - pos;
        const buf2 = Buffer.alloc(len);
        fs.readSync(fd, buf2, 0, len, pos);
        fs.closeSync(fd);
        pos = st.size;
        onChunk(buf2.toString("utf8"));
      }
    } catch {
      /* ignore */
    }
  }, 500);

  proc.on("exit", (code, signal) => {
    clearInterval(watch);
    state.alive = false;
    lifetime.exits++;
    console.error(`mc-harness: ${name} exited code=${code} signal=${signal}`);
  });

  stream.destroy();
  console.log(`mc-harness: started ${name} -> ${MC_HOST}:${MC_PORT} log=${logPath}`);
  return state;
}

function requestAdd(count) {
  if (busy) return "busy";
  if (count <= 0) return "count must be > 0";
  if (used + count > INDEX_LIMIT) {
    return `would exceed LT_INDEX_LIMIT=${INDEX_LIMIT} (used=${used})`;
  }
  busy = true;
  addError = "";
  // Pose must land in CK before the JVM joins — every add, so a prior leave that
  // saved world-spawn Pos cannot stick into the next start.
  setImmediate(() => {
    (async () => {
      try {
        for (let i = 0; i < count; i++) {
          const gi = INDEX_BASE + used;
          const name = botName(gi);
          const target = seamTargetForIndex(gi);
          console.log(
            `${ts()} mc-harness: seam pose ${name} -> ${target.x},${target.y},${target.z}`,
          );
          await writeSeamPoseWithRetries(name, gi);
          spawnWalker(gi);
          used++;
        }
      } catch (e) {
        const detail = formatErrorDetail(e, {
          op: "clients/add",
          url: process.env.CROWDY_API_ORIGIN || "(default ck.test)",
        });
        addError = detail;
        console.error(`${ts()} mc-harness: add failed: ${detail}`);
        if (e?.stack) console.error(e.stack);
      } finally {
        busy = false;
      }
    })();
  });
  return "";
}

function statusJson() {
  let active = 0;
  for (const c of clients.values()) if (c.alive) active++;
  return {
    instance_id: INSTANCE_ID,
    index_base: INDEX_BASE,
    index_used: used,
    index_limit: INDEX_LIMIT,
    active,
    busy,
    add_error: addError || undefined,
    current_rung: currentRung,
    mode: "minecraft-loadwalker",
    mc_host: MC_HOST,
    mc_port: Number(MC_PORT),
  };
}

function statsJson() {
  let active = 0;
  let transfers = 0;
  let east = 0;
  let west = 0;
  for (const c of clients.values()) {
    if (c.alive) active++;
    transfers += c.transfers;
    east += c.east;
    west += c.west;
  }
  const window = openWindow
    ? {
        id: openWindow.id,
        transfers: openWindow.transfers,
        east_pauses: openWindow.east,
        west_pauses: openWindow.west,
        relays: openWindow.east + openWindow.west,
      }
    : null;
  return {
    instance_id: INSTANCE_ID,
    lifetime: {
      ...lifetime,
      relays: lifetime.east_pauses + lifetime.west_pauses,
      active,
      live_transfers: transfers,
      live_east_pauses: east,
      live_west_pauses: west,
    },
    window,
    counters: {
      transfers: lifetime.transfers,
      east_pauses: lifetime.east_pauses,
      west_pauses: lifetime.west_pauses,
      relays: lifetime.east_pauses + lifetime.west_pauses,
    },
  };
}

function ensureStatsDir() {
  if (STATS_DIR) fs.mkdirSync(STATS_DIR, { recursive: true });
}

function openRung(id) {
  if (!id) return "id required";
  if (openWindow) return `rung ${openWindow.id} already open`;
  openWindow = { id, openedAt: Date.now(), transfers: 0, east: 0, west: 0 };
  currentRung = id;
  return "";
}

function closeRung() {
  if (!openWindow) return { error: "no open rung" };
  const body = {
    rung_id: openWindow.id,
    instance_id: INSTANCE_ID,
    opened_at: openWindow.openedAt,
    closed_at: Date.now(),
    mode: "minecraft-loadwalker",
    window: {
      counters: {
        transfers: openWindow.transfers,
        east_pauses: openWindow.east,
        west_pauses: openWindow.west,
        relays: openWindow.east + openWindow.west,
      },
    },
    status: statusJson(),
  };
  ensureStatsDir();
  if (STATS_DIR) {
    const file = path.join(STATS_DIR, `rung-${openWindow.id}.json`);
    fs.writeFileSync(file, JSON.stringify(body, null, 2));
  }
  const closed = openWindow;
  openWindow = null;
  currentRung = null;
  return body;
}

function shutdown() {
  shuttingDown = true;
  for (const c of clients.values()) {
    try {
      c.proc.kill("SIGTERM");
    } catch {
      /* ignore */
    }
  }
  setTimeout(() => process.exit(0), 500);
}

async function handle(req, res) {
  const url = new URL(req.url || "/", `http://${req.headers.host || "localhost"}`);
  if (url.pathname === "/health") {
    return json(res, 200, { ok: true, mode: "minecraft-loadwalker" });
  }
  if (!authorize(req)) {
    return json(res, 401, { error: "unauthorized" });
  }
  try {
    if (req.method === "GET" && url.pathname === "/v1/status") {
      return json(res, 200, statusJson());
    }
    if (req.method === "GET" && url.pathname === "/v1/stats") {
      return json(res, 200, statsJson());
    }
    if (req.method === "POST" && url.pathname === "/v1/clients/add") {
      const body = await readBody(req);
      const err = requestAdd(Number(body.count || 0));
      if (err) return json(res, 400, { error: err });
      return json(res, 202, { accepted: true, status: statusJson() });
    }
    if (req.method === "POST" && url.pathname === "/v1/rung/open") {
      const body = await readBody(req);
      const err = openRung(String(body.id || ""));
      if (err) return json(res, 400, { error: err });
      return json(res, 200, { ok: true, id: body.id });
    }
    if (req.method === "POST" && url.pathname === "/v1/rung/close") {
      const body = closeRung();
      if (body.error) return json(res, 400, body);
      return json(res, 200, body);
    }
    if (req.method === "POST" && url.pathname === "/v1/shutdown") {
      json(res, 200, { ok: true });
      shutdown();
      return;
    }
    return json(res, 404, { error: "not found" });
  } catch (e) {
    return json(res, 500, { error: String(e?.message || e) });
  }
}

function listen() {
  if (CONTROL_BIND === "off") {
    console.log("mc-harness: control off");
    return;
  }
  const [host, portStr] = CONTROL_BIND.includes("]:")
    ? [CONTROL_BIND.slice(1, CONTROL_BIND.lastIndexOf("]")), CONTROL_BIND.slice(CONTROL_BIND.lastIndexOf(":") + 1)]
    : CONTROL_BIND.split(":");
  const port = Number(portStr);
  const server = http.createServer((req, res) => {
    handle(req, res);
  });
  server.listen(port, host, () => {
    console.log(`mc-harness: control on ${host}:${port} instance=${INSTANCE_ID} dist=${MC_DIST}`);
  });
}

ensureStatsDir();
listen();
if (START_CLIENTS > 0) {
  const err = requestAdd(START_CLIENTS);
  if (err) {
    console.error("mc-harness: initial add failed:", err);
    process.exit(2);
  }
}

process.on("SIGTERM", shutdown);
process.on("SIGINT", shutdown);
