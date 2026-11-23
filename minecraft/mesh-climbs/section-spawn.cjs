/**
 * Seeded placement for the four-server and 16-server climbs.
 * Whole section, inset 32 blocks. Not an edge band and not a junction.
 * Index modulo the section count picks the section. The same index always
 * walks the same candidate list, so a retry stands on the same block.
 * Open water is skipped for another dry point in that section. An unknown
 * column is skipped the same way. It is not treated as sea level. Feet are
 * one block above the ground. The same index and the same table always
 * walk the same candidate list. If every candidate is unmeasured, or none
 * of them is a measured dry column, the index stays unplaced.
 *
 * column(x, z) returns {ground} with ground > 0, {water: true} or ground <= 0
 * for open water, or null when that column has not been measured.
 */
"use strict";

const INSET = 32;
const ATTEMPTS = 80;

/** Southwest-to-northeast order. The square meets at chunk 125 (block 2000). */
const FOUR_SECTIONS = Object.freeze([
  Object.freeze({ id: "west", x0: 0, z0: 0, x1: 2000, z1: 2000 }),
  Object.freeze({ id: "east", x0: 2000, z0: 0, x1: 4000, z1: 2000 }),
  Object.freeze({ id: "north", x0: 0, z0: 2000, x1: 2000, z1: 4000 }),
  Object.freeze({ id: "ne", x0: 2000, z0: 2000, x1: 4000, z1: 4000 }),
]);

function p16Sections() {
  const cells = [];
  for (let n = 1; n <= 16; n++) {
    const index = n - 1;
    const col = index % 4;
    const row = Math.floor(index / 4);
    cells.push({
      id: "p" + n,
      x0: col * 2048,
      z0: row * 2048,
      x1: (col + 1) * 2048,
      z1: (row + 1) * 2048,
    });
  }
  return Object.freeze(cells);
}

const P16_SECTIONS = p16Sections();

function nineSections() {
  const cells = [];
  for (let n = 1; n <= 9; n++) {
    const index = n - 1;
    const col = index % 3;
    const row = Math.floor(index / 3);
    cells.push({
      id: "p" + n,
      x0: col * 2000,
      z0: row * 2000,
      x1: (col + 1) * 2000,
      z1: (row + 1) * 2000,
    });
  }
  return Object.freeze(cells);
}

const NINE_SECTIONS = nineSections();

function sectionsFor(mesh) {
  if (mesh === "four") return FOUR_SECTIONS;
  if (mesh === "sixteen") return P16_SECTIONS;
  if (mesh === "nine") return NINE_SECTIONS;
  throw new Error("mesh must be four, nine, or sixteen, got " + mesh);
}

function sectionSlot(index, count) {
  const i = Number(index) | 0;
  return ((i % count) + count) % count;
}

function mulberry32(seed) {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

/** Northeast owns chunk x >= 125 and z >= 125. West is the default. */
function fourOwner(x, z) {
  const cx = Math.floor(Number(x) / 16);
  const cz = Math.floor(Number(z) / 16);
  const east = cx >= 125;
  const north = cz >= 125;
  if (east && north) return "ne";
  if (north) return "north";
  if (east) return "east";
  return "west";
}

/** 4x4 of 128-chunk cells. Outside that square the default server is p1. */
function p16Owner(x, z) {
  const cx = Math.floor(Number(x) / 16);
  const cz = Math.floor(Number(z) / 16);
  if (cx < 0 || cz < 0 || cx >= 512 || cz >= 512) return "p1";
  const col = Math.floor(cx / 128);
  const row = Math.floor(cz / 128);
  return "p" + (row * 4 + col + 1);
}

function nineOwner(x, z) {
  const col = Math.floor(Number(x) / 2000);
  const row = Math.floor(Number(z) / 2000);
  if (col < 0 || row < 0 || col > 2 || row > 2) return "p1";
  return "p" + (row * 3 + col + 1);
}

function ownerFor(mesh, x, z) {
  if (mesh === "four") return fourOwner(x, z);
  if (mesh === "nine") return nineOwner(x, z);
  return p16Owner(x, z);
}

/**
 * @param {number} index
 * @param {ReadonlyArray<{id: string, x0: number, z0: number, x1: number, z1: number}>} sections
 * @param {{ column?: (x: number, z: number) => ({ground?: number, water?: boolean}|null), inset?: number, attempts?: number }} [options]
 */
function sectionSpawn(index, sections, options) {
  const opts = options || {};
  if (!sections || !sections.length) throw new Error("no sections");
  const i = Number(index) | 0;
  const cell = sections[sectionSlot(i, sections.length)];
  const inset = opts.inset == null ? INSET : opts.inset;
  const x0 = cell.x0 + inset;
  const z0 = cell.z0 + inset;
  const spanX = cell.x1 - inset - x0;
  const spanZ = cell.z1 - inset - z0;
  if (spanX <= 0 || spanZ <= 0) throw new Error("section " + cell.id + " is smaller than the inset");
  const rand = mulberry32((i + 1) >>> 0);
  const attempts = opts.attempts == null ? ATTEMPTS : opts.attempts;
  const column = opts.column || null;
  let sawMeasured = false;
  for (let n = 0; n < attempts; n++) {
    const x = x0 + Math.floor(rand() * spanX);
    const z = z0 + Math.floor(rand() * spanZ);
    if (!column) continue;
    const col = column(x, z);
    // Unknown is skipped, the same as open water. Do not invent a height.
    if (col == null) continue;
    sawMeasured = true;
    if (col.water || !(Number(col.ground) > 0)) continue;
    const ground = Math.floor(Number(col.ground));
    return { server: cell.id, x, y: ground + 1, z, pending: false, ground };
  }
  return {
    server: cell.id,
    x: null,
    y: null,
    z: null,
    pending: true,
    reason: sawMeasured ? "no-dry" : "unmeasured",
  };
}

/** Table values: ground Y, or 0 for open water. A missing key is unknown. */
function columnFromTable(table) {
  return (x, z) => {
    if (!table) return null;
    const key = x + "," + z;
    if (!Object.prototype.hasOwnProperty.call(table, key)) return null;
    const ground = Number(table[key]);
    if (!Number.isFinite(ground) || ground <= 0) return { water: true };
    return { ground };
  };
}

function edgeDistance(cell, x, z) {
  return Math.min(x - cell.x0, cell.x1 - 1 - x, z - cell.z0, cell.z1 - 1 - z);
}

function selfTest() {
  const fails = [];
  const check = (name, ok) => {
    if (!ok) fails.push(name);
  };
  const dry = (x, z) => {
    if ((x + z) % 29 === 0) return { water: true };
    return { ground: 63 + ((x * 13 + z * 7) % 80) };
  };
  for (const mesh of ["four", "nine", "sixteen"]) {
    const sections = sectionsFor(mesh);
    const seen = new Map();
    let interior = 0;
    let nearEdge = 0;
    const n = 400;
    for (let i = 0; i < n; i++) {
      const home = sectionSpawn(i, sections, { column: dry });
      const again = sectionSpawn(i, sections, { column: dry });
      check(mesh + " stable " + i, home.x === again.x && home.z === again.z && home.y === again.y && home.server === again.server);
      check(mesh + " section " + i, home.server === sections[i % sections.length].id);
      check(mesh + " owner " + i, ownerFor(mesh, home.x, home.z) === home.server);
      check(mesh + " pending " + i, home.pending === false);
      check(mesh + " feet " + i, home.y === home.ground + 1 && home.y >= 2);
      const cell = sections[i % sections.length];
      const edge = edgeDistance(cell, home.x, home.z);
      check(mesh + " inset " + i, edge >= INSET);
      check(mesh + " water " + i, (home.x + home.z) % 29 !== 0);
      if (edge > 200) interior++;
      if (edge <= 64) nearEdge++;
      const box = seen.get(home.server) || { minX: Infinity, maxX: -Infinity, minZ: Infinity, maxZ: -Infinity };
      box.minX = Math.min(box.minX, home.x);
      box.maxX = Math.max(box.maxX, home.x);
      box.minZ = Math.min(box.minZ, home.z);
      box.maxZ = Math.max(box.maxZ, home.z);
      seen.set(home.server, box);
    }
    check(mesh + " interior", interior > n * 0.2);
    check(mesh + " near a cut", nearEdge > n * 0.02);
    for (const [id, box] of seen) {
      check(mesh + " spread " + id, box.maxX - box.minX > 400 && box.maxZ - box.minZ > 400);
    }
  }
  const seen = [];
  const probed = sectionSpawn(4, FOUR_SECTIONS, {
    attempts: 6,
    column: (x, z) => {
      seen.push([x, z]);
      return null;
    },
  });
  check("unknown skipped", seen.length === 6 && probed.pending && probed.reason === "unmeasured" && probed.y == null);
  const probedAgain = sectionSpawn(4, FOUR_SECTIONS, { attempts: 6, column: () => null });
  check("unmeasured stable", probedAgain.pending && probedAgain.reason === "unmeasured" && probedAgain.server === "west");
  const table = {};
  table[seen[0][0] + "," + seen[0][1]] = 0;
  table[seen[2][0] + "," + seen[2][1]] = 150;
  const stood = sectionSpawn(4, FOUR_SECTIONS, { column: columnFromTable(table), attempts: 6 });
  check(
    "skip water and unknown",
    !stood.pending && stood.y === 151 && stood.x === seen[2][0] && stood.z === seen[2][1] && stood.server === "west",
  );
  check("sixteen count", P16_SECTIONS.length === 16);
  check("p1 origin", P16_SECTIONS[0].id === "p1" && P16_SECTIONS[0].x0 === 0 && P16_SECTIONS[0].z0 === 0);
  check("p16 corner", P16_SECTIONS[15].id === "p16" && P16_SECTIONS[15].x0 === 6144 && P16_SECTIONS[15].z0 === 6144);
  check("ne chunks", fourOwner(2000, 2000) === "ne" && fourOwner(1999, 2000) === "north" && fourOwner(2000, 1999) === "east");
  if (fails.length) {
    for (const line of fails) console.error("FAIL " + line);
    console.error(fails.length + " failed");
    return 1;
  }
  console.log("section-spawn self-test ok");
  return 0;
}

module.exports = {
  INSET,
  FOUR_SECTIONS,
  P16_SECTIONS,
  sectionsFor,
  sectionSpawn,
  columnFromTable,
  fourOwner,
  p16Owner,
  edgeDistance,
};

if (require.main === module) {
  if (process.argv.includes("--self-test")) {
    process.exit(selfTest());
  }
  if (process.argv[2] === "--place") {
    const fs = require("node:fs");
    const index = Number(process.argv[3]);
    const mesh = process.argv[4];
    const file = process.argv[5] || "";
    let table = {};
    if (file) {
      try {
        table = JSON.parse(fs.readFileSync(file, "utf8"));
      } catch (err) {
        console.log(JSON.stringify({ pending: true, reason: "columns-unreadable" }));
        process.exit(0);
      }
    }
    try {
      const row = sectionSpawn(index, sectionsFor(mesh), { column: columnFromTable(table) });
      console.log(JSON.stringify(row));
    } catch (err) {
      console.log(JSON.stringify({ pending: true, reason: String(err.message || err) }));
      process.exit(0);
    }
  }
}
