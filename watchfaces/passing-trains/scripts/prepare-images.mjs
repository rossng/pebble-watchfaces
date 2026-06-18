/**
 * prepare-images.mjs — turn the upstream ns-treinen PNGs into Pebble C resources.
 *
 * This is a *native* (plain C) Pebble watchface, not an Alloy/Moddable one. The
 * watch holds each train as a small colour `gbitmap` in flash and upscales it to
 * full screen height at draw time with a hand-written nearest-neighbour blit (see
 * src/c/main.c). So the source images only need to be tall enough that a ~3–4×
 * upscale still looks decent — `BODY_HEIGHT` is that quality/RAM knob (a gbitmap is
 * held *uncompressed* in RAM, and cost grows with height², so two trains must fit
 * the device's RAM budget).
 *
 * Images are posterised to Pebble's 64-colour palette (each channel 0/85/170/255)
 * with dithering off, so they map 1:1 to `GColor8` with no build-time surprises.
 *
 * Writes:
 *   - resources/images/<name>.png   (committed; the SDK bakes these into gbitmaps)
 *   - src/c/trains.h                (generated: resource-id list + count)
 *   - package.json  resources.media (regenerated to match the image set)
 *
 * Requires ImageMagick (`magick`) on PATH. Run via `pnpm prepare:images`.
 */

import { execFileSync } from "node:child_process";
import { mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, ".."); // watchfaces/passing-trains
const imagesDir = join(root, "ns-treinen", "images");
const outImagesDir = join(root, "resources", "images");

// Each train is baked into a fixed-height canvas, sitting on the *floor*
// (bottom-anchored) with sky above — like a real train on tracks. At runtime the
// whole canvas is upscaled to the screen, so the train always lands on the floor.
//
// Crucially we normalise on the train's *body* height, not its full outline: a
// raised pantograph on the roof would otherwise eat into the height and leave the
// body looking randomly shorter than a pantograph-less railcar. So we measure where
// the solid body starts (below the thin pantograph) and scale every train so its
// body is BODY_HEIGHT; pantographs then stick up into the sky strip above, clipped
// at the canvas top if unusually tall. Taller = sharper but more RAM (two gbitmaps
// resident; cost grows with height²) — these give a ~3–4× upscale on emery/gabbro.
const BODY_HEIGHT = 34; // the train body, normalised across all trains
const CANVAS_HEIGHT = 48; // body + sky/pantograph headroom above it
const BODY_COVERAGE = 0.4; // a row is "body" once this fraction of it is opaque

// Opaque sky. MUST match SKY in src/c/main.c. Chosen from Pebble's 64-colour
// palette (each channel 0/85/170/255) so posterisation is exact and seamless.
const BACKGROUND = "#55AAFF";

// Curated fleet: one short variant of each distinct NS train *type*, for maximum
// visual variety. Only two trains are resident in RAM at once (current + next), so
// the pool size is bounded by flash, not RAM — but the two widest must fit the heap
// together, which is why the very long international / loco-hauled stock (ICE,
// Thalys, ICR, and the 8-car intercity sets) is left out: a single one of those is
// 100 KB+ as a gbitmap and would blow the budget. Adding more *types* here is cheap;
// adding longer trains is what costs RAM. (Run `pnpm prepare:images` — it prints each
// train's gbitmap size; keep the two largest well under the ~125 KB heap.)
const TRAINS = [
  "dm90_2", // yellow diesel railcar (Arriva)
  "wink_2_arriva", // Arriva Wink (CAF)
  "sgmm_2", // SGMM "Plan Y" sprinter
  "sng_3", // new Sprinter (SNG / FLIRT)
  "slt_4", // Sprinter Lighttrain (SLT)
  "gtw_6_arriva", // Stadler GTW
  "icm_3", // ICM "Koploper" intercity
  "ddz_4", // DDZ double-decker
  "virm_4", // VIRM double-decker intercity
  "icng_5", // ICNG new intercity
];

function magick(args) {
  return execFileSync("magick", args, { maxBuffer: 64 * 1024 * 1024 });
}

rmSync(outImagesDir, { recursive: true, force: true });
mkdirSync(outImagesDir, { recursive: true });

// Find the body height of a trimmed train: the topmost row that is at least
// BODY_COVERAGE opaque is the body's roofline; everything above it is pantograph.
function bodyHeight(src) {
  const [w, h] = magick([src, "-trim", "+repage", "-format", "%w %h", "info:"])
    .toString()
    .trim()
    .split(" ")
    .map(Number);
  const alpha = magick([src, "-trim", "+repage", "-alpha", "extract", "-depth", "8", "GRAY:-"]);
  const threshold = BODY_COVERAGE * w;
  for (let r = 0; r < h; r++) {
    let opaque = 0;
    const off = r * w;
    for (let c = 0; c < w; c++) {
      if (alpha[off + c] > 128) opaque++;
    }
    if (opaque >= threshold) return { roofRow: r, fullH: h };
  }
  return { roofRow: 0, fullH: h };
}

const media = [];
for (const name of TRAINS) {
  const src = join(imagesDir, `${name}.png`);
  const out = join(outImagesDir, `${name}.png`);

  // Scale the whole train so its *body* (roofline to wheels) is BODY_HEIGHT, then
  // bottom-anchor it on a CANVAS_HEIGHT canvas (pantograph extends up into the sky,
  // cropped at the top if too tall). Posterise to Pebble's 64-colour GColor8 palette
  // with dithering off so the colours map exactly.
  const { roofRow, fullH } = bodyHeight(src);
  const scale = BODY_HEIGHT / (fullH - roofRow);
  const trainH = Math.round(fullH * scale);
  magick([
    src,
    "-trim",
    "+repage",
    "-resize",
    `x${trainH}`,
    "-background",
    BACKGROUND,
    "-flatten",
    "-alpha",
    "off",
    "-gravity",
    "South",
    "-background",
    BACKGROUND,
    "-extent",
    `x${CANVAS_HEIGHT}`,
    "+dither",
    "-posterize",
    "4",
    "-strip",
    out,
  ]);
  const [w, h] = magick([out, "-format", "%w %h", "info:"])
    .toString()
    .trim()
    .split(" ")
    .map(Number);

  media.push({
    type: "bitmap",
    name: resourceName(name),
    file: `images/${name}.png`,
    memoryFormat: "8Bit",
  });
  console.log(
    `  ${name.padEnd(16)} ${String(w).padStart(4)}x${h}  body=${BODY_HEIGHT} (roof@${roofRow}/${fullH})`,
  );
}

function resourceName(name) {
  return `TRAIN_${name.toUpperCase()}`;
}

// Regenerate the resources.media block in package.json to match the image set.
const pkgPath = join(root, "package.json");
const pkg = JSON.parse(readFileSync(pkgPath, "utf8"));
pkg.pebble.resources = { media };
writeFileSync(pkgPath, `${JSON.stringify(pkg, null, 2)}\n`);

// Friendly display name per train type (well-known nickname, or full name), shown
// as a subtle label scrolling above the train. Keyed by the type code before the
// first underscore; unknown types fall back to the upper-cased code.
const DISPLAY_NAMES = {
  dm90: "Buffel",
  wink: "Wink",
  sgmm: "Sprinter (Plan Y)",
  sng: "Sprinter (Nieuwe Generatie)",
  slt: "Sprinter (Lighttrain)",
  gtw: "Stadler GTW",
  icm: "Koploper",
  ddz: "Dubbeldekker",
  virm: "Regiorunner",
  icng: "Intercity Nieuwe Generatie",
};
function displayName(name) {
  const key = name.split("_")[0];
  return (DISPLAY_NAMES[key] ?? key).toUpperCase();
}

// Per-type upward nudge for the scrolling label, in source (canvas) pixels — for
// trains whose front cab rises into the label gap (e.g. the Koploper's bump).
const LABEL_DY = { icm: 4 };
function labelDy(name) {
  return LABEL_DY[name.split("_")[0]] ?? 0;
}

// Generated C header: the resource ids and names the watch picks from, in order.
const header =
  "// Generated by scripts/prepare-images.mjs — do not edit by hand.\n" +
  "#pragma once\n#include <pebble.h>\n\n" +
  `#define TRAIN_COUNT ${TRAINS.length}\n` +
  `// Sky strip baked above the train body, in source (canvas) pixels — used to place\n` +
  `// the type label in the gap above the train.\n` +
  `#define TRAIN_SKY_ABOVE ${CANVAS_HEIGHT - BODY_HEIGHT}\n\n` +
  "static const uint32_t TRAIN_RESOURCE_IDS[TRAIN_COUNT] = {\n" +
  TRAINS.map((n) => `  RESOURCE_ID_${resourceName(n)},`).join("\n") +
  "\n};\n\n" +
  "static const char *const TRAIN_NAMES[TRAIN_COUNT] = {\n" +
  TRAINS.map((n) => `  "${displayName(n)}",`).join("\n") +
  "\n};\n\n" +
  "// Per-train upward nudge for the label (source px), for tall-cab trains.\n" +
  "static const int TRAIN_LABEL_DY[TRAIN_COUNT] = {\n" +
  TRAINS.map((n) => `  ${labelDy(n)},`).join("\n") +
  "\n};\n";
writeFileSync(join(root, "src", "c", "trains.h"), header);

console.log(
  `\n  ${TRAINS.length} trains, body ${BODY_HEIGHT}px on a ${CANVAS_HEIGHT}px canvas → resources/images/*.png, trains.h, package.json media`,
);
