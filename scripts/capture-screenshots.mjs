#!/usr/bin/env node
// Captures real emulator screenshots for every watchface, one per target
// platform, into `watchfaces/<slug>/screenshots/<platform>.png`.
//
//   node scripts/capture-screenshots.mjs            # all watchfaces
//   node scripts/capture-screenshots.mjs simple-time   # just one
//
// Requires the Pebble SDK (`pebble` on PATH + `pebble sdk install latest`). The
// emulator launches QEMU with an SDL display, so on a headless machine (CI) wrap
// the whole command in `xvfb-run -a …`. The committed PNGs are what the site
// build (scripts/build-site.mjs) embeds, so this does NOT need to run in the fast
// Pages deploy — regenerate screenshots when a watchface changes and commit them.
//
// It is deliberately best-effort: if the SDK is missing or one face fails, it
// logs and moves on rather than breaking a larger build.

import { execFileSync } from "node:child_process";
import { existsSync } from "node:fs";
import { mkdir, readdir, readFile } from "node:fs/promises";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..");
const WATCHFACES = join(ROOT, "watchfaces");

// Screenshots show the emulator's wall-clock time. (The emulator RTC is
// base=localtime and ignores `emu-set-time` for the on-watch clock, so there's
// no reliable way to pin a fixed time — regenerate screenshots when a watchface
// actually changes rather than on every commit.)

// Platforms the emulator can boot. Anything else in targetPlatforms is skipped.
const EMULATOR_PLATFORMS = new Set([
  "aplite",
  "basalt",
  "chalk",
  "diorite",
  "emery",
  "gabbro",
  "flint",
]);

function pebble(args, { cwd, timeout = 240_000 } = {}) {
  return execFileSync("pebble", args, {
    cwd,
    timeout,
    stdio: ["ignore", "pipe", "pipe"],
    encoding: "utf8",
  });
}

function hasPebble() {
  try {
    pebble(["--version"], { timeout: 15_000 });
    return true;
  } catch {
    return false;
  }
}

function killEmulators() {
  try {
    pebble(["kill"], { timeout: 30_000 });
  } catch {
    // nothing running / already gone
  }
}

// Clear persisted emulator flash. Repeated install/kill cycles can corrupt it
// and leave the watch boot-looping; starting each capture clean avoids that.
function wipeEmulators() {
  try {
    pebble(["wipe"], { timeout: 30_000 });
  } catch {
    // no state to wipe
  }
}

// A cold boot right after a wipe can exceed the installer's connect timeout; the
// emulator is booting, so a second attempt connects to the now-warm one.
function installEmulator(platform, cwd) {
  try {
    pebble(["install", "--emulator", platform], { cwd });
  } catch {
    pebble(["install", "--emulator", platform], { cwd });
  }
}

async function watchfacesToCapture(filter) {
  if (!existsSync(WATCHFACES)) return [];
  const entries = await readdir(WATCHFACES, { withFileTypes: true });
  const faces = [];
  for (const e of entries) {
    if (!e.isDirectory()) continue;
    if (filter && e.name !== filter) continue;
    const pkgPath = join(WATCHFACES, e.name, "package.json");
    if (!existsSync(pkgPath)) continue;
    const pkg = JSON.parse(await readFile(pkgPath, "utf8"));
    const platforms = (pkg.pebble?.targetPlatforms ?? []).filter((p) => EMULATOR_PLATFORMS.has(p));
    faces.push({ slug: e.name, dir: join(WATCHFACES, e.name), platforms });
  }
  return faces;
}

function captureFace(face) {
  const outDir = join(face.dir, "screenshots");
  console.log(`\n▶ ${face.slug} — platforms: ${face.platforms.join(", ") || "(none)"}`);

  // Build once; install per platform reuses the build output.
  pebble(["build"], { cwd: face.dir });

  for (const platform of face.platforms) {
    killEmulators();
    wipeEmulators();
    try {
      installEmulator(platform, face.dir);
      const out = join(outDir, `${platform}.png`);
      pebble(["screenshot", "--no-open", out], { cwd: face.dir, timeout: 120_000 });
      console.log(`  ✓ ${platform} → screenshots/${platform}.png`);
    } catch (err) {
      console.warn(`  ✗ ${platform}: ${err.message.split("\n")[0]}`);
    } finally {
      killEmulators();
    }
  }
}

async function main() {
  const filter = process.argv[2];

  if (!hasPebble()) {
    console.warn("pebble CLI not found — skipping screenshot capture.");
    console.warn("Install the SDK (see README) then re-run `pnpm screenshots`.");
    return;
  }

  const faces = await watchfacesToCapture(filter);
  if (!faces.length) {
    console.warn(filter ? `No watchface named "${filter}".` : "No watchfaces found.");
    return;
  }

  for (const face of faces) {
    await mkdir(join(face.dir, "screenshots"), { recursive: true });
    try {
      captureFace(face);
    } catch (err) {
      console.warn(`! ${face.slug}: build failed — ${err.message.split("\n")[0]}`);
    }
  }

  killEmulators();
  console.log("\nDone. Commit the PNGs under watchfaces/*/screenshots/ to publish them.");
}

main().catch((err) => {
  killEmulators();
  console.error(err);
  process.exit(1);
});
