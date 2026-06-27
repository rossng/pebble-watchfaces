#!/usr/bin/env node
/**
 * Offline orientation previewer — NOT part of the build.
 *
 * Parses the generated src/c/models.h and renders a contact sheet of a model at
 * a sweep of orientations, using the same camera/projection as the watch
 * (render.c), so the angles that look good here map straight to MODEL_START in
 * main.c. Run: `node scripts/preview-orient.mjs <modelIndex> [out.png]`.
 */

import { readFileSync, writeFileSync } from "node:fs";
import { deflateSync } from "node:zlib";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..");
const CAM_DIST = 3.4,
  FILL = 0.8,
  VSCALE = 16384;

// --- parse models.h ---
function parseModels() {
  const src = readFileSync(join(ROOT, "src", "c", "models.h"), "utf8");
  const nums = (body) => body.match(/-?\d+/g).map(Number);
  const models = [];
  const re =
    /static const int16_t (\w+)_VERTS\[\] = \{([^}]*)\};\s*static const uint16_t \1_FACES\[\] = \{([^}]*)\};/g;
  let m;
  while ((m = re.exec(src))) {
    const v = nums(m[2]).map((n) => n / VSCALE);
    const verts = [];
    for (let i = 0; i < v.length; i += 3) verts.push([v[i], v[i + 1], v[i + 2]]);
    const fi = nums(m[3]);
    const faces = [];
    for (let i = 0; i < fi.length; i += 3) faces.push([fi[i], fi[i + 1], fi[i + 2]]);
    models.push({ ident: m[1], verts, faces });
  }
  // names in registry order
  const names = [...src.matchAll(/\{ "([^"]+)",/g)].map((x) => x[1]);
  models.forEach((mm, i) => (mm.name = names[i] || mm.ident));
  return models;
}

// --- rotation: R = Rz * Ry * Rx (row-major), matching build_rot() ---
function mul(A, B) {
  const o = Array.from({ length: 9 });
  for (let i = 0; i < 3; i++)
    for (let j = 0; j < 3; j++)
      o[i * 3 + j] = A[i * 3] * B[j] + A[i * 3 + 1] * B[3 + j] + A[i * 3 + 2] * B[6 + j];
  return o;
}
function rot(ax, ay, az) {
  const cx = Math.cos(ax),
    sx = Math.sin(ax),
    cy = Math.cos(ay),
    sy = Math.sin(ay),
    cz = Math.cos(az),
    sz = Math.sin(az);
  const rx = [1, 0, 0, 0, cx, -sx, 0, sx, cx];
  const ry = [cy, 0, sy, 0, 1, 0, -sy, 0, cy];
  const rz = [cz, -sz, 0, sz, cz, 0, 0, 0, 1];
  return mul(rz, mul(ry, rx));
}

// --- render one orientation into an RGB buffer at (ox,oy) of the sheet ---
function renderInto(buf, W, model, m, ox, oy, S) {
  const cx = S / 2,
    cy = S / 2,
    focal = FILL * 0.5 * S * CAM_DIST;
  const L = [-0.4, 0.62, 0.68];
  const sv = model.verts.map(([x, y, z]) => {
    const rx = m[0] * x + m[1] * y + m[2] * z;
    const ry = m[3] * x + m[4] * y + m[5] * z;
    const rz = m[6] * x + m[7] * y + m[8] * z;
    const vz = rz - CAM_DIST,
      depth = Math.max(0.1, -vz);
    return { vx: rx, vy: ry, vz, sx: cx + (focal * rx) / depth, sy: cy - (focal * ry) / depth };
  });
  const tris = [];
  for (const [a, b, c] of model.faces) {
    const A = sv[a],
      B = sv[b],
      C = sv[c];
    const ux = B.vx - A.vx,
      uy = B.vy - A.vy,
      uz = B.vz - A.vz;
    const wx = C.vx - A.vx,
      wy = C.vy - A.vy,
      wz = C.vz - A.vz;
    let nx = uy * wz - uz * wy,
      ny = uz * wx - ux * wz,
      nz = ux * wy - uy * wx;
    const gx = -(A.vx + B.vx + C.vx) / 3,
      gy = -(A.vy + B.vy + C.vy) / 3,
      gz = -(A.vz + B.vz + C.vz) / 3;
    if (nx * gx + ny * gy + nz * gz <= 0) continue;
    const nl = Math.hypot(nx, ny, nz) || 1;
    nx /= nl;
    ny /= nl;
    nz /= nl;
    const diff = Math.max(0, nx * L[0] + ny * L[1] + nz * L[2]);
    const shade = 0.22 + 0.78 * diff;
    const depth = -(A.vz + B.vz + C.vz) / 3;
    tris.push({ A, B, C, shade, depth });
  }
  tris.sort((p, q) => q.depth - p.depth);
  const put = (x, y, g) => {
    if (x < 0 || y < 0 || x >= S || y >= S) return;
    const i = ((oy + y) * W + (ox + x)) * 3;
    buf[i] = buf[i + 1] = buf[i + 2] = g;
  };
  for (const t of tris) {
    const g = Math.round(255 * t.shade);
    const xs = [t.A.sx, t.B.sx, t.C.sx],
      ys = [t.A.sy, t.B.sy, t.C.sy];
    const y0 = Math.max(0, Math.floor(Math.min(...ys))),
      y1 = Math.min(S - 1, Math.ceil(Math.max(...ys)));
    for (let y = y0; y <= y1; y++) {
      const yc = y + 0.5;
      let xl = 1e9,
        xr = -1e9;
      for (let e = 0; e < 3; e++) {
        const ay0 = ys[e],
          by0 = ys[(e + 1) % 3],
          ax0 = xs[e],
          bx0 = xs[(e + 1) % 3];
        if ((yc >= ay0 && yc < by0) || (yc >= by0 && yc < ay0)) {
          const x = ax0 + ((yc - ay0) / (by0 - ay0)) * (bx0 - ax0);
          if (x < xl) xl = x;
          if (x > xr) xr = x;
        }
      }
      for (let x = Math.floor(xl); x <= Math.ceil(xr); x++) put(x, y, g);
    }
  }
}

// --- minimal PNG (truecolor) ---
function png(W, H, rgb) {
  const raw = Buffer.alloc((W * 3 + 1) * H);
  for (let y = 0; y < H; y++) {
    raw[y * (W * 3 + 1)] = 0;
    rgb.copy(raw, y * (W * 3 + 1) + 1, y * W * 3, (y + 1) * W * 3);
  }
  const idat = deflateSync(raw);
  const chunk = (type, data) => {
    const len = Buffer.alloc(4);
    len.writeUInt32BE(data.length);
    const td = Buffer.concat([Buffer.from(type), data]);
    const crc = Buffer.alloc(4);
    crc.writeUInt32BE(crc32(td) >>> 0);
    return Buffer.concat([len, td, crc]);
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(W, 0);
  ihdr.writeUInt32BE(H, 4);
  ihdr[8] = 8;
  ihdr[9] = 2; // 8-bit, truecolor
  return Buffer.concat([
    Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]),
    chunk("IHDR", ihdr),
    chunk("IDAT", idat),
    chunk("IEND", Buffer.alloc(0)),
  ]);
}
const CRC = (() => {
  const t = Array.from({ length: 256 });
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();
function crc32(buf) {
  let c = 0xffffffff;
  for (let i = 0; i < buf.length; i++) c = CRC[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  return c ^ 0xffffffff;
}

const models = parseModels();

// `stats`: for each model, the distribution of its support reach h(r) = max
// vertex dot(v, r) over random view directions r. A vertex pierces the text when
// its rotated z exceeds text_front, so over orientations the model pierces when
// h(r) > text_front, by (h(r) - text_front). These percentiles let us pick a
// text_front that gives "a bit" of piercing for every model.
if (process.argv[2] === "stats") {
  // deterministic pseudo-random unit directions (no Math.random for reproducibility)
  const dirs = [];
  let seed = 12345;
  const rnd = () => (seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff;
  for (let i = 0; i < 6000; i++) {
    const z = rnd() * 2 - 1,
      t = rnd() * 2 * Math.PI,
      r = Math.sqrt(1 - z * z);
    dirs.push([r * Math.cos(t), r * Math.sin(t), z]);
  }
  const pct = (arr, p) => arr[Math.min(arr.length - 1, Math.floor((p / 100) * arr.length))];
  for (const mm of models) {
    const hs = dirs
      .map((d) => Math.max(...mm.verts.map((v) => v[0] * d[0] + v[1] * d[1] + v[2] * d[2])))
      .sort((a, b) => a - b);
    const f = (x) => x.toFixed(2);
    console.log(
      `${mm.name.padEnd(8)} reach h(r): p10=${f(pct(hs, 10))} p25=${f(pct(hs, 25))} p50=${f(pct(hs, 50))} p75=${f(pct(hs, 75))} p90=${f(pct(hs, 90))} max=${f(hs[hs.length - 1])}`,
    );
  }
  process.exit(0);
}

// `final <out.png>`: render each model at its MODEL_START (must mirror main.c).
if (process.argv[2] === "final") {
  const START = [
    [0.2, 3.14159, 0],
    [0.12, 0, 0],
    [0, 0, 0],
    [0.1, 0, 0],
  ];
  const S = 150,
    W = S * models.length,
    H = S,
    buf = Buffer.alloc(W * H * 3);
  models.forEach((mm, i) => renderInto(buf, W, mm, rot(...START[i]), i * S, 0, S));
  writeFileSync(process.argv[3], png(W, H, buf));
  console.log(`final orientations -> ${process.argv[3]} (${models.map((m) => m.name).join(", ")})`);
  process.exit(0);
}

// --- contact sheet: sweep yaw across columns, pitch across rows ---
const idx = Number(process.argv[2] ?? 0);
const out = process.argv[3] || join(ROOT, `preview-${idx}.png`);
const model = models[idx];
const S = 110,
  COLS = 6,
  ROWS = 3,
  W = S * COLS,
  H = S * ROWS;
const buf = Buffer.alloc(W * H * 3);
const D = Math.PI / 180;
const pitches = [-20, 5, 25];
for (let r = 0; r < ROWS; r++) {
  for (let col = 0; col < COLS; col++) {
    const yaw = col * 60;
    renderInto(buf, W, model, rot(pitches[r] * D, yaw * D, 0), col * S, r * S, S);
  }
}
writeFileSync(out, png(W, H, buf));
console.log(`${model.name}: ${out}  (cols yaw 0..300 step 60, rows pitch ${pitches.join("/")})`);
