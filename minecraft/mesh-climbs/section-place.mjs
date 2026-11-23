/**
 * Sync hook the load-generator harness calls when LT_MC_WANDER_PLACE=section.
 * Reloads section-spawn.cjs on every call, so a newer placement file applies
 * to the next pack. A JVM that is already running is not involved.
 */
import { createRequire } from "node:module";
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const require = createRequire(import.meta.url);
const here = dirname(fileURLToPath(import.meta.url));
const spawnFile = join(here, "section-spawn.cjs");

function freshSpawn() {
  const resolved = require.resolve(spawnFile);
  delete require.cache[resolved];
  return require(spawnFile);
}

function readTable() {
  const file = process.env.LT_SECTION_COLUMNS || "";
  if (!file) return {};
  try {
    const parsed = JSON.parse(readFileSync(file, "utf8"));
    return parsed && typeof parsed === "object" ? parsed : {};
  } catch {
    return {};
  }
}

/** Feet Y is ground + 1. An index with no measured dry column stays unplaced. */
export function sectionPlace(index) {
  const { sectionSpawn, sectionsFor, columnFromTable } = freshSpawn();
  const mesh = process.env.LT_SECTION_MESH || "";
  const placed = sectionSpawn(index, sectionsFor(mesh), { column: columnFromTable(readTable()) });
  if (placed.pending) {
    const err = new Error(
      `unplaced index ${index}: no measured dry column (${placed.reason || "unmeasured"})`,
    );
    err.code = "UNPLACED";
    throw err;
  }
  return placed;
}

/** LT_MC_HOST_WEST, LT_MC_HOST_NE, LT_MC_HOST_P1, and the same for the other section ids. */
export function sectionHost(server) {
  const key = "LT_MC_HOST_" + String(server).toUpperCase().replace(/[^A-Z0-9]/g, "_");
  const host = process.env[key] || "";
  if (!host) throw new Error("missing " + key);
  return host;
}
