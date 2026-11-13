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
  nearSeam,
  looksLikeWorldSpawn,
  formatErrorDetail,
  isTransientNetworkError,
} from "./seam-pose.mjs";

const __dirname = path.dirname(fileURLToPath(import.meta.url));

function env(name, fallback = "") {
  return process.env[name] ?? fallback;
}

const SEAM_POSE_ATTEMPTS = Math.max(1, Number(env("LT_SEAM_POSE_ATTEMPTS", "3")));
const SEAM_POSE_RETRY_MS = Math.max(0, Number(env("LT_SEAM_POSE_RETRY_MS", "1500")));
/** Pause after a successful hub Pos write before starting the JVM (lets claim/save settle). */
const SEAM_POSE_SETTLE_MS = Math.max(0, Number(env("LT_SEAM_POSE_SETTLE_MS", "750")));
/**
 * How long the walker must stay off-seam after first `phase=play` before we call it a
 * spawn-miss (Load6-8: hub Pos wrote OK, in-game stayed at world spawn with lane/block).
 */
const PLACEMENT_CONFIRM_MS = Math.max(500, Number(env("LT_PLACEMENT_CONFIRM_MS", "3000")));

function ts() {
  return new Date().toISOString();
}

function seamPoseBackoffMs(attempt) {
  // attempt 1 → base, 2 → 2x, 3 → 4x (capped)
  const mult = 2 ** Math.max(0, attempt - 1);
  return Math.min(SEAM_POSE_RETRY_MS * mult, 15_000);
}

async function writeSeamPoseWithRetries(name, index, opts = {}) {
  let lastErr;
  for (let attempt = 1; attempt <= SEAM_POSE_ATTEMPTS; attempt++) {
    try {
      if (attempt > 1) {
        const wait = seamPoseBackoffMs(attempt - 1);
        console.error(
          `${ts()} mc-harness: seam pose retry ${attempt}/${SEAM_POSE_ATTEMPTS} for ${name} after ${wait}ms backoff`,
        );
        await new Promise((r) => setTimeout(r, wait));
      }
      const result = await writeSeamPose(name, index, opts);
      if (SEAM_POSE_SETTLE_MS > 0) {
        await new Promise((r) => setTimeout(r, SEAM_POSE_SETTLE_MS));
      }
      return result;
    } catch (e) {
      lastErr = e;
      const detail = formatErrorDetail(e, {
        op: "seam-pose",
        attempt,
        attempts: SEAM_POSE_ATTEMPTS,
        url: process.env.CROWDY_API_ORIGIN || "(default ck.test)",
      });
      // Retry any write failure (network, Pos did not stick, claim/save races) with backoff.
      const retry = attempt < SEAM_POSE_ATTEMPTS;
      const kind = isTransientNetworkError(e)
        ? "transient"
        : /Pos did not stick/i.test(String(e?.message || e))
          ? "pos-not-stuck"
          : "error";
      console.error(
        `${ts()} mc-harness: seam pose failed for ${name} (${kind}): ${detail}${retry ? " (will retry)" : ""}`,
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
/** Single-host fallback (west). Prefer LT_MC_HOST_WEST / LT_MC_HOST_EAST when alternating. */
const MC_HOST = env("LT_MC_HOST", "");
const MC_PORT = env("LT_MC_PORT", "25565");
const MC_HOST_WEST = env("LT_MC_HOST_WEST", MC_HOST);
const MC_HOST_EAST = env("LT_MC_HOST_EAST", "");
const MC_PORT_WEST = env("LT_MC_PORT_WEST", MC_PORT);
const MC_PORT_EAST = env("LT_MC_PORT_EAST", MC_PORT);
/** When east host is set, even global indices → west, odd → east (same seam Pos). */
const MC_ALTERNATE =
  env("LT_MC_ALTERNATE", "") === "1" ||
  env("LT_MC_ALTERNATE", "").toLowerCase() === "true" ||
  (MC_HOST_EAST !== "" && env("LT_MC_ALTERNATE", "auto") !== "0");
/** Delay after each bot's spawn before the next pose+login (ms). Default 2s. */
const LOGIN_STAGGER_MS = Math.max(0, Number(env("LT_LOGIN_STAGGER_MS", "2000")));
/** How long after spawn to treat an exit as "early" (retry window). Default 20s. */
const EARLY_STABLE_MS = Math.max(1000, Number(env("LT_EARLY_STABLE_MS", "20000")));
/** Extra attempts after the first when a bot dies early / transfer-fails. Default 2. */
const ADD_RETRIES = Math.max(0, Number(env("LT_ADD_RETRIES", "2")));
const MC_DIST = requireEnv("LT_MC_DIST");
const INDEX_BASE = Number(env("LT_INDEX_BASE", "0"));
const INDEX_LIMIT = Number(env("LT_INDEX_LIMIT", "8"));
const INSTANCE_ID = env("LT_INSTANCE_ID", os.hostname());
const STATS_DIR = env("LT_STATS_DIR", "");
const NAME_PREFIX = env("LT_MC_NAME_PREFIX", "Load");
const JAVA = env("LT_MC_JAVA", "java");
const START_CLIENTS = Number(env("LT_CLIENTS", "0"));

if (!MC_HOST_WEST && !MC_HOST) {
  console.error("mc-harness: LT_MC_HOST or LT_MC_HOST_WEST is required");
  process.exit(2);
}
if (MC_ALTERNATE && !MC_HOST_EAST) {
  console.error("mc-harness: LT_MC_ALTERNATE needs LT_MC_HOST_EAST");
  process.exit(2);
}

/** @returns {{ side: string, host: string, port: number, ckServer: string }} */
function endpointForIndex(globalIndex) {
  const westHost = MC_HOST_WEST || MC_HOST;
  if (!MC_ALTERNATE || !MC_HOST_EAST) {
    return {
      side: "west",
      host: westHost,
      port: Number(MC_PORT_WEST || MC_PORT),
      ckServer: "west",
    };
  }
  // even → west, odd → east
  if ((globalIndex & 1) === 1) {
    return {
      side: "east",
      host: MC_HOST_EAST,
      port: Number(MC_PORT_EAST || MC_PORT),
      ckServer: "east",
    };
  }
  return {
    side: "west",
    host: westHost,
    port: Number(MC_PORT_WEST || MC_PORT),
    ckServer: "west",
  };
}

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

/** @type {Map<number, { proc: import('node:child_process').ChildProcess, name: string, side: string, host: string, port: number, transfers: number, east: number, west: number, alive: boolean, stable?: boolean, suppressExitRetry?: boolean, endpoint?: { side: string, host: string, port: number, ckServer: string } }>} */
const clients = new Map();
/** Indices currently in a post-stable transfer respawn (avoid double-schedule). */
const postStableRetrying = new Set();
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

function walkerLogPath(name) {
  const logDir = STATS_DIR ? path.join(STATS_DIR, "walkers") : path.join(os.tmpdir(), "ck-mc-walkers");
  fs.mkdirSync(logDir, { recursive: true });
  return path.join(logDir, `${name}.log`);
}

function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

/** Parse LoadWalker TRANSFER FAILURE line → join destination. */
function parseTransferDest(logText) {
  const m = String(logText).match(
    /TRANSFER FAILURE\s+\S+\s+ClientboundTransferPacket to ([^\s:]+):(\d+)/,
  );
  if (!m) return null;
  return { host: m[1], port: Number(m[2]) };
}

function endpointFromHostPort(host, port) {
  const westHost = MC_HOST_WEST || MC_HOST;
  const eastHost = MC_HOST_EAST;
  let side = "west";
  let ckServer = "west";
  if (eastHost && host === eastHost) {
    side = "east";
    ckServer = "east";
  } else if (host === westHost) {
    side = "west";
    ckServer = "west";
  } else if (eastHost && host.includes("east")) {
    side = "east";
    ckServer = "east";
  }
  return { side, host, port: Number(port), ckServer };
}

function stopWalker(globalIndex) {
  const state = clients.get(globalIndex);
  if (!state) return;
  // Harness-initiated kills must not trigger post-stable transfer retries.
  state.suppressExitRetry = true;
  try {
    if (state.alive) state.proc.kill("SIGTERM");
  } catch {
    /* ignore */
  }
}

function spawnWalker(globalIndex, endpoint) {
  const name = botName(globalIndex);
  const { host, port, side } = endpoint;
  const logPath = walkerLogPath(name);
  const logFd = fs.openSync(logPath, "a");
  const proc = spawn(runSh, [name, host, String(port)], {
    cwd: MC_DIST,
    env: process.env,
    stdio: ["ignore", logFd, logFd],
    detached: false,
  });
  fs.closeSync(logFd);

  let logMark = 0;
  try {
    logMark = fs.statSync(logPath).size;
  } catch {
    logMark = 0;
  }

  const state = {
    proc,
    name,
    side,
    host,
    port,
    logPath,
    /** Byte offset when this spawn started — ignore prior sessions in the append-only log. */
    logMark,
    transfers: 0,
    east: 0,
    west: 0,
    alive: true,
    /** Set true after the early-stable window passes near the seam. */
    stable: false,
    /** When true, exit handler must not schedule a post-stable transfer retry. */
    suppressExitRetry: false,
    endpoint: { ...endpoint },
    earlyExitReason: "",
  };
  clients.set(globalIndex, state);
  lifetime.starts++;

  // Tail log for metrics (best-effort)
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
        state.earlyExitReason = "transfer";
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
  let pos = logMark;
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
    maybeSchedulePostStableTransferRetry(globalIndex, state);
  });

  console.log(
    `${ts()} mc-harness: started ${name} -> ${side} ${host}:${port} log=${logPath}`,
  );
  return state;
}

/**
 * After a bot has passed the early-stable window, a ClientboundTransferPacket still
 * kills LoadWalker (same-TCP choreography). Retry that slot with the same backoff /
 * attempt budget as early transfer failures — do not leave active permanently down.
 */
function maybeSchedulePostStableTransferRetry(globalIndex, state) {
  if (!state || state.suppressExitRetry || !state.stable) return;
  if (postStableRetrying.has(globalIndex)) return;

  const text = readSessionLog(state, globalIndex);
  const dest = parseTransferDest(text);
  const isTransfer = Boolean(dest) || state.earlyExitReason === "transfer";
  if (!isTransfer) return; // max-runtime / other clean exits — do not respawn

  const name = botName(globalIndex);
  let endpoint = state.endpoint ? { ...state.endpoint } : endpointForIndex(globalIndex);
  if (dest) {
    endpoint = endpointFromHostPort(dest.host, dest.port);
  }

  postStableRetrying.add(globalIndex);
  console.error(
    `${ts()} mc-harness: ${name} post-stable transfer death → retry on ${endpoint.side} ${endpoint.host}:${endpoint.port} (up to ${ADD_RETRIES + 1} attempts)`,
  );

  setImmediate(() => {
    (async () => {
      try {
        const result = await spawnSlotWithRetries(globalIndex, endpoint);
        if (result.ok) {
          console.log(
            `${ts()} mc-harness: ${name} post-stable transfer retry recovered on ${result.endpoint.side}`,
          );
        } else {
          console.error(
            `${ts()} mc-harness: ${name} post-stable transfer retry gave up (${result.reason})`,
          );
        }
      } catch (e) {
        console.error(
          `${ts()} mc-harness: ${name} post-stable transfer retry error: ${formatErrorDetail(e, { op: "post-stable-transfer-retry" })}`,
        );
      } finally {
        postStableRetrying.delete(globalIndex);
      }
    })();
  });
}

/** Session-only slice of the append-only walker log (bytes after this spawn's logMark). */
function readSessionLog(state, globalIndex) {
  const logPath = state?.logPath || walkerLogPath(botName(globalIndex));
  try {
    const fd = fs.openSync(logPath, "r");
    try {
      const st = fs.fstatSync(fd);
      const start = Math.min(state?.logMark ?? 0, st.size);
      const len = st.size - start;
      if (len <= 0) return "";
      const buf = Buffer.alloc(len);
      fs.readSync(fd, buf, 0, len, start);
      return buf.toString("utf8");
    } finally {
      fs.closeSync(fd);
    }
  } catch {
    return "";
  }
}

/** Last `phase=play … x= y= z=` from LoadWalker stdout. */
function parseLatestPlayPose(logText) {
  const re =
    /phase=play\b[^\n]*\bx=([-\d.]+)\s+y=([-\d.]+)\s+z=([-\d.]+)/g;
  let m;
  let last = null;
  while ((m = re.exec(logText)) !== null) {
    last = { x: Number(m[1]), y: Number(m[2]), z: Number(m[3]) };
  }
  return last;
}

/**
 * Watch a freshly spawned walker. Early death (esp. TRANSFER FAILURE) is retriable.
 * Also fails when the bot is alive but stranded at world spawn (pose write did not stick
 * in-game) — that used to be counted as "stable" and left Load6-8 invisible at the seam.
 * Resolves { ok:true } if still alive near the seam after the window, else
 * { ok:false, transferTo?, reason, pos? }.
 */
async function waitEarlyOutcome(globalIndex, timeoutMs, target) {
  const deadline = Date.now() + timeoutMs;
  let offSeamSince = 0;
  while (Date.now() < deadline) {
    const state = clients.get(globalIndex);
    if (!state || !state.alive) {
      const text = readSessionLog(state, globalIndex);
      const dest = parseTransferDest(text);
      return {
        ok: false,
        reason: dest ? "transfer" : state?.earlyExitReason || "early-exit",
        transferTo: dest,
      };
    }

    const text = readSessionLog(state, globalIndex);
    const pose = parseLatestPlayPose(text);
    if (pose && target) {
      if (nearSeam(pose, target)) {
        offSeamSince = 0;
      } else {
        const stranded =
          looksLikeWorldSpawn(pose) ||
          /\bblocked at\b/i.test(text) ||
          /\blane\b/i.test(text);
        if (!offSeamSince) offSeamSince = Date.now();
        const held = Date.now() - offSeamSince;
        // Fast-path: clearly at spawn / lane-blocked. Else wait PLACEMENT_CONFIRM_MS.
        if (stranded || held >= PLACEMENT_CONFIRM_MS) {
          return {
            ok: false,
            reason: "spawn-miss",
            pos: pose,
          };
        }
      }
    }

    await sleep(400);
  }
  const state = clients.get(globalIndex);
  if (state?.alive) {
    // Still alive: require near-seam placement when we have a pose sample.
    const text = readSessionLog(state, globalIndex);
    const pose = parseLatestPlayPose(text);
    if (pose && target && !nearSeam(pose, target)) {
      return { ok: false, reason: "spawn-miss", pos: pose };
    }
    return { ok: true, pos: pose || undefined };
  }
  const text = readSessionLog(state, globalIndex);
  const dest = parseTransferDest(text);
  return {
    ok: false,
    reason: dest ? "transfer" : "early-exit",
    transferTo: dest,
  };
}

/**
 * Pose + spawn one index with retries. Transfer-to-west (east join "belongs on west")
 * is retried against the transfer destination rather than stalling the ramp.
 * Spawn-miss (hub pose wrote but in-game still at world spawn) rewrites the pose with
 * backoff and respawns — does not silently leave the bot stranded.
 */
async function spawnSlotWithRetries(globalIndex, initialEndpoint) {
  const name = botName(globalIndex);
  let endpoint = { ...initialEndpoint };
  let lastReason = "unknown";
  const maxAttempts = ADD_RETRIES + 1;
  const target = seamTargetForIndex(globalIndex);

  for (let attempt = 1; attempt <= maxAttempts; attempt++) {
    stopWalker(globalIndex);
    await sleep(300);

    if (attempt > 1) {
      const wait = seamPoseBackoffMs(attempt - 1);
      console.error(
        `${ts()} mc-harness: ${name} pre-spawn backoff ${wait}ms before pose rewrite (attempt ${attempt}/${maxAttempts})`,
      );
      await sleep(wait);
    }

    console.log(
      `${ts()} mc-harness: seam pose ${name} attempt=${attempt}/${maxAttempts} side=${endpoint.side} -> ${target.x},${target.y},${target.z} join=${endpoint.host}:${endpoint.port}`,
    );
    await writeSeamPoseWithRetries(name, globalIndex, { server: endpoint.ckServer });
    spawnWalker(globalIndex, endpoint);

    const outcome = await waitEarlyOutcome(globalIndex, EARLY_STABLE_MS, target);
    if (outcome.ok) {
      const live = clients.get(globalIndex);
      if (live) {
        live.stable = true;
        live.suppressExitRetry = false;
        live.endpoint = { ...endpoint };
      }
      console.log(
        `${ts()} mc-harness: ${name} stable on ${endpoint.side} ${endpoint.host}:${endpoint.port}` +
          (outcome.pos
            ? ` at ${outcome.pos.x.toFixed(1)},${outcome.pos.y.toFixed(1)},${outcome.pos.z.toFixed(1)}`
            : ""),
      );
      return { ok: true, endpoint };
    }

    lastReason = outcome.reason || "early-exit";
    const posNote = outcome.pos
      ? ` pos=${outcome.pos.x.toFixed(1)},${outcome.pos.y.toFixed(1)},${outcome.pos.z.toFixed(1)} (want ~${target.x},${target.y},${target.z})`
      : "";
    console.error(
      `${ts()} mc-harness: ${name} early failure reason=${lastReason} attempt=${attempt}/${maxAttempts}${posNote}`,
    );

    if (attempt >= maxAttempts) break;

    if (outcome.transferTo) {
      const redirected = endpointFromHostPort(
        outcome.transferTo.host,
        outcome.transferTo.port,
      );
      console.error(
        `${ts()} mc-harness: ${name} transfer failure → retry on ${redirected.side} ${redirected.host}:${redirected.port} (retriable add)`,
      );
      endpoint = redirected;
    } else if (lastReason === "spawn-miss") {
      // Pose did not stick in-game: kill, rewrite hub Pos with backoff, try again.
      // Prefer west for a seam rewrite when the miss was on east (east join often
      // transfer-bounces; west is the seam-side home for these load bots).
      if (endpoint.side === "east" && (MC_HOST_WEST || MC_HOST)) {
        const west = endpointForIndex(0); // even → west shape
        console.error(
          `${ts()} mc-harness: ${name} spawn-miss → rewrite pose and retry on west ${west.host}:${west.port}`,
        );
        endpoint = west;
      } else {
        console.error(
          `${ts()} mc-harness: ${name} spawn-miss → rewrite pose and retry on ${endpoint.side} ${endpoint.host}:${endpoint.port}`,
        );
      }
    } else {
      console.error(
        `${ts()} mc-harness: ${name} retrying same endpoint ${endpoint.side} ${endpoint.host}:${endpoint.port}`,
      );
    }
  }

  stopWalker(globalIndex);
  return { ok: false, reason: lastReason, endpoint };
}

function requestAdd(count) {
  if (busy) return "busy";
  if (count <= 0) return "count must be > 0";
  if (used + count > INDEX_LIMIT) {
    return `would exceed LT_INDEX_LIMIT=${INDEX_LIMIT} (used=${used})`;
  }
  busy = true;
  addError = "";
  // Pose must land in CK before the JVM joins. Logins are staggered one-at-a-time.
  // Early deaths (transfer / crash) retry the same index; permanent failure advances
  // to the next index so the ramp never deadlocks waiting for active>=N.
  setImmediate(() => {
    (async () => {
      let okCount = 0;
      let failCount = 0;
      try {
        for (let i = 0; i < count; i++) {
          const gi = INDEX_BASE + used;
          const initial = endpointForIndex(gi);
          const result = await spawnSlotWithRetries(gi, initial);
          used++;
          if (result.ok) {
            okCount++;
          } else {
            failCount++;
            console.error(
              `${ts()} mc-harness: slot ${botName(gi)} gave up (${result.reason}); continuing ramp`,
            );
          }
          if (i + 1 < count && LOGIN_STAGGER_MS > 0) {
            console.log(
              `${ts()} mc-harness: login stagger ${LOGIN_STAGGER_MS}ms before next bot`,
            );
            await sleep(LOGIN_STAGGER_MS);
          }
        }
        if (failCount > 0 && okCount === 0) {
          addError = `all ${failCount} add slot(s) failed early (last reasons in harness log)`;
        } else if (failCount > 0) {
          console.error(
            `${ts()} mc-harness: add finished with ok=${okCount} failed=${failCount} (ramp continued)`,
          );
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
    mc_host: MC_HOST_WEST || MC_HOST,
    mc_host_west: MC_HOST_WEST || MC_HOST || undefined,
    mc_host_east: MC_HOST_EAST || undefined,
    mc_alternate: MC_ALTERNATE,
    login_stagger_ms: LOGIN_STAGGER_MS,
    early_stable_ms: EARLY_STABLE_MS,
    add_retries: ADD_RETRIES,
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
