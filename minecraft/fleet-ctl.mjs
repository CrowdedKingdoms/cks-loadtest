#!/usr/bin/env node
/**
 * SoftMax-style fleet control for the Minecraft harness.
 *
 * Same fan-out shape as `cks-loadtest-ctl`: every host in hosts.txt gets the
 * same command. Disjoint index ownership is fixed at install time
 * (LT_INDEX_BASE / LT_INDEX_LIMIT per VM); `add --count N` means N clients
 * **per host**, so fleet total = N × hosts.
 *
 *   minecraft/fleet-ctl.mjs --run $RUN status
 *   minecraft/fleet-ctl.mjs --run $RUN add --count 4
 *   minecraft/fleet-ctl.mjs --run $RUN wait --active-delta 4 --timeout-sec 600
 *   minecraft/fleet-ctl.mjs --run $RUN rung-open --id r1
 *   minecraft/fleet-ctl.mjs --run $RUN rung-close --id r1 --out r1.fleet.json
 *   minecraft/fleet-ctl.mjs --run $RUN stats [--out fleet-stats.json]
 *   minecraft/fleet-ctl.mjs --run $RUN clear
 *
 * Or pass --hosts PATH and --token PATH / LT_CONTROL_TOKEN.
 */
import fs from "node:fs";
import path from "node:path";

function usage() {
  console.error(`usage: fleet-ctl.mjs --run DIR <status|stats|add|wait|rung-open|rung-close|clear> …
  --hosts FILE   (default $RUN/hosts.txt)
  --token FILE   (default $RUN/control-token)
  add --count N
  wait [--active-delta N] [--timeout-sec S] [--stable-sec S]
  rung-open --id ID
  rung-close --id ID [--out FILE]
  stats [--out FILE]`);
  process.exit(2);
}

function parseArgs(argv) {
  const out = { _: [] };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === "--run") out.run = argv[++i];
    else if (a === "--hosts") out.hosts = argv[++i];
    else if (a === "--token") out.tokenFile = argv[++i];
    else if (a === "--count") out.count = Number(argv[++i]);
    else if (a === "--active-delta") out.activeDelta = Number(argv[++i]);
    else if (a === "--timeout-sec") out.timeoutSec = Number(argv[++i]);
    else if (a === "--stable-sec") out.stableSec = Number(argv[++i]);
    else if (a === "--id") out.id = argv[++i];
    else if (a === "--out") out.out = argv[++i];
    else if (a.startsWith("-")) {
      console.error("unknown flag", a);
      usage();
    } else out._.push(a);
  }
  return out;
}

async function req(host, token, method, p, body) {
  const url = host.replace(/\/$/, "") + p;
  const res = await fetch(url, {
    method,
    headers: {
      Authorization: `Bearer ${token}`,
      ...(body ? { "Content-Type": "application/json" } : {}),
    },
    ...(body ? { body } : {}),
  });
  const text = await res.text();
  let json = null;
  try {
    json = text ? JSON.parse(text) : null;
  } catch {
    /* raw */
  }
  return { host, status: res.status, text, json };
}

async function fan(hosts, token, method, p, body) {
  return Promise.all(hosts.map((h) => req(h, token, method, p, body)));
}

function pickNum(obj, ...keys) {
  for (const k of keys) {
    if (obj && obj[k] != null && obj[k] !== "") return Number(obj[k]);
  }
  return 0;
}

function mergeStats(snaps) {
  // MC harness nests counters under lifetime/window/counters; Buddy ctl used flat fields.
  const fleet = {
    hosts: snaps.length,
    active: 0,
    used: 0,
    limit: 0,
    starts: 0,
    exits: 0,
    transfers: 0,
    east_pauses: 0,
    west_pauses: 0,
    relays: 0,
    busy: false,
    per_host: [],
  };
  for (const s of snaps) {
    if (!s) continue;
    const life = s.lifetime || {};
    const win = s.window || {};
    const ctr = s.counters || {};
    fleet.active += pickNum(s, "active") || pickNum(life, "active");
    fleet.used += pickNum(s, "used", "index_used");
    fleet.limit += pickNum(s, "limit", "index_limit");
    fleet.starts += pickNum(s, "starts") || pickNum(life, "starts");
    fleet.exits += pickNum(s, "exits") || pickNum(life, "exits");
    fleet.transfers +=
      pickNum(ctr, "transfers") ||
      pickNum(win, "transfers") ||
      pickNum(life, "transfers") ||
      pickNum(s, "transfers");
    fleet.east_pauses +=
      pickNum(ctr, "east_pauses") ||
      pickNum(win, "east_pauses") ||
      pickNum(life, "east_pauses") ||
      pickNum(s, "east_pauses");
    fleet.west_pauses +=
      pickNum(ctr, "west_pauses") ||
      pickNum(win, "west_pauses") ||
      pickNum(life, "west_pauses") ||
      pickNum(s, "west_pauses");
    fleet.relays +=
      pickNum(ctr, "relays") ||
      pickNum(win, "relays") ||
      pickNum(life, "relays") ||
      pickNum(s, "relays");
    if (s.busy) fleet.busy = true;
    fleet.per_host.push(s);
  }
  return fleet;
}

const args = parseArgs(process.argv.slice(2));
const cmd = args._[0];
if (!cmd) usage();

const hostsFile =
  args.hosts ||
  (args.run ? path.join(args.run, "hosts.txt") : null);
const tokenFile =
  args.tokenFile ||
  (args.run ? path.join(args.run, "control-token") : null);
if (!hostsFile || !fs.existsSync(hostsFile)) {
  console.error("hosts file missing; pass --run or --hosts");
  process.exit(2);
}
const token =
  process.env.LT_CONTROL_TOKEN ||
  (tokenFile && fs.existsSync(tokenFile)
    ? fs.readFileSync(tokenFile, "utf8").trim()
    : "");
if (!token) {
  console.error("control token missing; pass --token or LT_CONTROL_TOKEN");
  process.exit(2);
}
const hosts = fs
  .readFileSync(hostsFile, "utf8")
  .split("\n")
  .map((l) => l.trim())
  .filter((l) => l && !l.startsWith("#"));
if (!hosts.length) {
  console.error("hosts file empty");
  process.exit(2);
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

if (cmd === "status") {
  const rows = await fan(hosts, token, "GET", "/v1/status", null);
  for (const r of rows) {
    console.log(`--- ${r.host} (${r.status})`);
    console.log(r.text);
  }
  process.exit(rows.every((r) => r.status === 200) ? 0 : 1);
}

if (cmd === "stats") {
  const rows = await fan(hosts, token, "GET", "/v1/stats", null);
  const snaps = rows.map((r) => r.json).filter(Boolean);
  const fleet = mergeStats(snaps);
  const out = JSON.stringify(fleet, null, 2);
  if (args.out) fs.writeFileSync(args.out, out + "\n");
  else console.log(out);
  process.exit(rows.every((r) => r.status === 200) ? 0 : 1);
}

if (cmd === "add") {
  const count = args.count;
  if (!(count > 0)) {
    console.error("add requires --count N (per host)");
    process.exit(2);
  }
  const rows = await fan(
    hosts,
    token,
    "POST",
    "/v1/clients/add",
    JSON.stringify({ count }),
  );
  for (const r of rows) {
    console.log(`--- ${r.host} (${r.status})`);
    console.log(r.text);
  }
  // SoftMax ctl expects 202
  process.exit(rows.every((r) => r.status === 202 || r.status === 200) ? 0 : 1);
}

if (cmd === "clear") {
  const rows = await fan(hosts, token, "POST", "/v1/clients/clear", "{}");
  for (const r of rows) {
    console.log(`--- ${r.host} (${r.status})`);
    console.log(r.text);
  }
  process.exit(rows.every((r) => r.status >= 200 && r.status < 300) ? 0 : 1);
}

if (cmd === "rung-open") {
  if (!args.id) {
    console.error("rung-open requires --id");
    process.exit(2);
  }
  const rows = await fan(
    hosts,
    token,
    "POST",
    "/v1/rung/open",
    JSON.stringify({ id: args.id }),
  );
  for (const r of rows) {
    console.log(`--- ${r.host} (${r.status})`);
    console.log(r.text);
  }
  process.exit(rows.every((r) => r.status >= 200 && r.status < 300) ? 0 : 1);
}

if (cmd === "rung-close") {
  if (!args.id) {
    console.error("rung-close requires --id");
    process.exit(2);
  }
  const rows = await fan(
    hosts,
    token,
    "POST",
    "/v1/rung/close",
    JSON.stringify({ id: args.id }),
  );
  const snaps = rows.map((r) => r.json).filter(Boolean);
  const fleet = { id: args.id, closedAt: new Date().toISOString(), hosts: snaps };
  const out = JSON.stringify(fleet, null, 2);
  if (args.out) fs.writeFileSync(args.out, out + "\n");
  else console.log(out);
  process.exit(rows.every((r) => r.status >= 200 && r.status < 300) ? 0 : 1);
}

if (cmd === "wait") {
  const timeoutSec = args.timeoutSec || 300;
  const delta = args.activeDelta || 0;
  const stableSec = args.stableSec || 0;
  const deadline = Date.now() + timeoutSec * 1000;
  let baseUsed = [];
  let baseLive = [];
  if (delta > 0) {
    const rows = await fan(hosts, token, "GET", "/v1/status", null);
    baseUsed = rows.map((r) => Number(r.json?.used || r.json?.index_used || 0));
    baseLive = rows.map(
      (r) =>
        Number(r.json?.active || 0) + Number(r.json?.suspended || 0),
    );
  }
  let stableSince = null;
  for (;;) {
    const rows = await fan(hosts, token, "GET", "/v1/status", null);
    let idle = true;
    let grown = true;
    let err = null;
    for (let i = 0; i < rows.length; i++) {
      const st = rows[i].json || {};
      if (st.busy) idle = false;
      const live = Number(st.active || 0) + Number(st.suspended || 0);
      if (
        delta > 0 &&
        (Number(st.used || st.index_used || 0) < baseUsed[i] + delta ||
          live < baseLive[i] + delta)
      ) {
        grown = false;
      }
      if (st.add_error) err = `${rows[i].host}: ${st.add_error}`;
    }
    const summary = rows
      .map((r) => {
        const st = r.json || {};
        return `${r.host.replace(/^https?:\/\//, "")} a=${st.active} u=${st.used ?? st.index_used} busy=${st.busy}`;
      })
      .join(" | ");
    console.error(`[wait] ${summary}`);
    if (err) {
      console.error(err);
      process.exit(1);
    }
    if (delta > 0 && !grown) {
      stableSince = null;
    } else if (!idle) {
      stableSince = null;
    } else if (stableSec > 0) {
      if (!stableSince) stableSince = Date.now();
      if (Date.now() - stableSince >= stableSec * 1000) {
        process.exit(0);
      }
    } else if (idle && (delta === 0 || grown)) {
      process.exit(0);
    }
    if (Date.now() > deadline) {
      console.error("wait: timeout");
      process.exit(1);
    }
    await sleep(2000);
  }
}

usage();
