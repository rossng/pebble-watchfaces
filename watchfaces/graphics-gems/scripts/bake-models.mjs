#!/usr/bin/env node
/**
 * Bake the watchface's 3D models into src/c/models.h.
 *
 * Two are loaded from vendored OBJ files (the Utah teapot and the Stanford
 * bunny) and decimated — by vertex clustering — down to a few-hundred-triangle
 * budget the watch can transform and fill every frame. Two more (an icosphere
 * and a (2,3) torus knot) are generated procedurally. Every model is recentred
 * on the origin and scaled to a unit bounding sphere so the renderer can treat
 * them identically.
 *
 * Output is a generated header of `const` arrays (they live in flash, not the
 * ~125 KB app heap). Don't hand-edit models.h; tweak the constants below and
 * re-run `pnpm bake:models`.
 */

import { readFileSync, writeFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const __dirname = dirname(fileURLToPath(import.meta.url));
const ROOT = join(__dirname, "..");

// Per-model triangle budgets. Both rasterizing AND transforming/culling scale
// with the triangle count (profiled on-device: fill has real per-triangle and
// per-row overhead, it is not purely coverage-bound), so these are sized to keep
// the heaviest pose under the ~80 ms/12 fps frame budget after the -O2 + slope
// rasterizer speedup — and to keep the app's static data under the ~64 KB
// virtual-size limit. See GEM_MODELS perf table in the README.
const TEAPOT_TARGET = 720;
const BUNNY_TARGET = 660;

// ---------------------------------------------------------------------------
// OBJ parsing
// ---------------------------------------------------------------------------

function parseObj(text) {
  const verts = [];
  const faces = [];
  for (const line of text.split("\n")) {
    if (line.startsWith("v ")) {
      const p = line.split(/\s+/);
      verts.push([parseFloat(p[1]), parseFloat(p[2]), parseFloat(p[3])]);
    } else if (line.startsWith("f ")) {
      const p = line.trim().split(/\s+/).slice(1);
      // Each token is v, v/vt, v//vn or v/vt/vn; we only want v. Indices are
      // 1-based and may be negative (relative to the end).
      const idx = p.map((tok) => {
        let i = parseInt(tok.split("/")[0], 10);
        if (i < 0) i = verts.length + i + 1;
        return i - 1;
      });
      // Triangulate any polygon as a fan.
      for (let i = 2; i < idx.length; i++) {
        faces.push([idx[0], idx[i - 1], idx[i]]);
      }
    }
  }
  return { verts, faces };
}

// ---------------------------------------------------------------------------
// Normalisation + decimation
// ---------------------------------------------------------------------------

function bounds(verts) {
  const min = [Infinity, Infinity, Infinity];
  const max = [-Infinity, -Infinity, -Infinity];
  for (const v of verts) {
    for (let k = 0; k < 3; k++) {
      if (v[k] < min[k]) min[k] = v[k];
      if (v[k] > max[k]) max[k] = v[k];
    }
  }
  return { min, max };
}

// Recentre on the bbox centre and scale so the farthest vertex sits at radius 1.
function normalize(verts) {
  const { min, max } = bounds(verts);
  const c = [(min[0] + max[0]) / 2, (min[1] + max[1]) / 2, (min[2] + max[2]) / 2];
  let r = 0;
  for (const v of verts) {
    const dx = v[0] - c[0],
      dy = v[1] - c[1],
      dz = v[2] - c[2];
    const d = Math.sqrt(dx * dx + dy * dy + dz * dz);
    if (d > r) r = d;
  }
  if (r === 0) r = 1;
  return verts.map((v) => [(v[0] - c[0]) / r, (v[1] - c[1]) / r, (v[2] - c[2]) / r]);
}

// Vertex-clustering decimation: snap vertices to a grid, collapse each occupied
// cell to the average of the vertices in it, drop triangles that collapse onto
// a degenerate edge. `gridN` is cells along the longest axis — more cells means
// more surviving detail.
function cluster(verts, faces, gridN) {
  const { min, max } = bounds(verts);
  const ext = Math.max(max[0] - min[0], max[1] - min[1], max[2] - min[2]) || 1;
  const cell = ext / gridN;
  const key = (v) =>
    Math.floor((v[0] - min[0]) / cell) +
    "," +
    Math.floor((v[1] - min[1]) / cell) +
    "," +
    Math.floor((v[2] - min[2]) / cell);

  const acc = new Map(); // cellKey -> { sum:[x,y,z], n, index }
  const cellOf = Array.from({ length: verts.length });
  for (let i = 0; i < verts.length; i++) {
    const k = key(verts[i]);
    let a = acc.get(k);
    if (!a) {
      a = { sum: [0, 0, 0], n: 0, index: acc.size };
      acc.set(k, a);
    }
    a.sum[0] += verts[i][0];
    a.sum[1] += verts[i][1];
    a.sum[2] += verts[i][2];
    a.n++;
    cellOf[i] = a;
  }

  const outVerts = Array.from({ length: acc.size });
  for (const a of acc.values()) {
    outVerts[a.index] = [a.sum[0] / a.n, a.sum[1] / a.n, a.sum[2] / a.n];
  }

  const outFaces = [];
  const seen = new Set();
  for (const f of faces) {
    const a = cellOf[f[0]].index;
    const b = cellOf[f[1]].index;
    const c = cellOf[f[2]].index;
    if (a === b || b === c || a === c) continue; // collapsed
    const fk = [a, b, c].sort((x, y) => x - y).join(",");
    if (seen.has(fk)) continue; // duplicate after collapse
    seen.add(fk);
    outFaces.push([a, b, c]);
  }
  return { verts: outVerts, faces: outFaces };
}

// Search for the finest grid whose triangle count stays within budget.
function decimateToBudget(verts, faces, targetFaces) {
  let best = null;
  for (let g = 4; g <= 80; g++) {
    const out = cluster(verts, faces, g);
    if (out.faces.length <= targetFaces) {
      best = out;
    } else {
      break;
    }
  }
  return best || cluster(verts, faces, 4);
}

function loadModel(file, target) {
  const { verts, faces } = parseObj(readFileSync(join(ROOT, "models", file), "utf8"));
  const dec = decimateToBudget(verts, faces, target);
  return { verts: normalize(dec.verts), faces: dec.faces };
}

// ---------------------------------------------------------------------------
// Procedural models
// ---------------------------------------------------------------------------

function icosphere(subdiv) {
  const t = (1 + Math.sqrt(5)) / 2;
  let verts = [
    [-1, t, 0],
    [1, t, 0],
    [-1, -t, 0],
    [1, -t, 0],
    [0, -1, t],
    [0, 1, t],
    [0, -1, -t],
    [0, 1, -t],
    [t, 0, -1],
    [t, 0, 1],
    [-t, 0, -1],
    [-t, 0, 1],
  ];
  let faces = [
    [0, 11, 5],
    [0, 5, 1],
    [0, 1, 7],
    [0, 7, 10],
    [0, 10, 11],
    [1, 5, 9],
    [5, 11, 4],
    [11, 10, 2],
    [10, 7, 6],
    [7, 1, 8],
    [3, 9, 4],
    [3, 4, 2],
    [3, 2, 6],
    [3, 6, 8],
    [3, 8, 9],
    [4, 9, 5],
    [2, 4, 11],
    [6, 2, 10],
    [8, 6, 7],
    [9, 8, 1],
  ];
  const cache = new Map();
  const mid = (a, b) => {
    const k = a < b ? a + "_" + b : b + "_" + a;
    if (cache.has(k)) return cache.get(k);
    const m = [
      (verts[a][0] + verts[b][0]) / 2,
      (verts[a][1] + verts[b][1]) / 2,
      (verts[a][2] + verts[b][2]) / 2,
    ];
    const idx = verts.length;
    verts.push(m);
    cache.set(k, idx);
    return idx;
  };
  for (let s = 0; s < subdiv; s++) {
    const next = [];
    for (const [a, b, c] of faces) {
      const ab = mid(a, b),
        bc = mid(b, c),
        ca = mid(c, a);
      next.push([a, ab, ca], [b, bc, ab], [c, ca, bc], [ab, bc, ca]);
    }
    faces = next;
  }
  return { verts: normalize(verts), faces };
}

// A (p,q) torus knot swept with a circular tube of `tubeR`, `seg` steps along
// the knot and `ring` points around the tube.
function torusKnot(p, q, seg, ring, tubeR) {
  const verts = [];
  const centers = [];
  for (let i = 0; i < seg; i++) {
    const u = (i / seg) * Math.PI * 2;
    const r = 2 + Math.cos(q * u);
    centers.push([r * Math.cos(p * u), r * Math.sin(p * u), -Math.sin(q * u)]);
  }
  const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
  const cross = (a, b) => [
    a[1] * b[2] - a[2] * b[1],
    a[2] * b[0] - a[0] * b[2],
    a[0] * b[1] - a[1] * b[0],
  ];
  const norm = (a) => {
    const l = Math.hypot(a[0], a[1], a[2]) || 1;
    return [a[0] / l, a[1] / l, a[2] / l];
  };
  for (let i = 0; i < seg; i++) {
    const cur = centers[i];
    const next = centers[(i + 1) % seg];
    const T = norm(sub(next, cur));
    let N = norm(cross(T, [0, 0, 1]));
    const B = norm(cross(T, N));
    for (let j = 0; j < ring; j++) {
      const v = (j / ring) * Math.PI * 2;
      const cv = Math.cos(v) * tubeR;
      const sv = Math.sin(v) * tubeR;
      verts.push([
        cur[0] + cv * N[0] + sv * B[0],
        cur[1] + cv * N[1] + sv * B[1],
        cur[2] + cv * N[2] + sv * B[2],
      ]);
    }
  }
  const faces = [];
  for (let i = 0; i < seg; i++) {
    for (let j = 0; j < ring; j++) {
      const a = i * ring + j;
      const b = i * ring + ((j + 1) % ring);
      const c = ((i + 1) % seg) * ring + j;
      const d = ((i + 1) % seg) * ring + ((j + 1) % ring);
      faces.push([a, b, d], [a, d, c]);
    }
  }
  return { verts: normalize(verts), faces };
}

// ---------------------------------------------------------------------------
// Emit
// ---------------------------------------------------------------------------

const VSCALE = 16384; // vertices are int16 fixed-point (Q14) to halve their RAM footprint

function emit(name, ident, model) {
  const q = (x) => Math.max(-32767, Math.min(32767, Math.round(x * VSCALE)));
  const v = model.verts.map((p) => `  ${p.map(q).join(", ")},`).join("\n");
  const f = [];
  for (let i = 0; i < model.faces.length; i += 8) {
    f.push(
      "  " +
        model.faces
          .slice(i, i + 8)
          .map((t) => t.join(", "))
          .join(",  ") +
        ",",
    );
  }
  return {
    name,
    ident,
    vcount: model.verts.length,
    fcount: model.faces.length,
    block:
      `static const int16_t ${ident}_VERTS[] = {\n${v}\n};\n` +
      `static const uint16_t ${ident}_FACES[] = {\n${f.join("\n")}\n};\n`,
  };
}

const models = [
  emit("Teapot", "TEAPOT", loadModel("utah-teapot.obj", TEAPOT_TARGET)),
  emit("Bunny", "BUNNY", loadModel("stanford-bunny.obj", BUNNY_TARGET)),
  emit("Sphere", "SPHERE", icosphere(2)),
  emit("Knot", "KNOT", torusKnot(2, 3, 40, 6, 0.6)),
];

const maxVerts = Math.max(...models.map((m) => m.vcount));
const maxFaces = Math.max(...models.map((m) => m.fcount));

let out = `// AUTO-GENERATED by scripts/bake-models.mjs — do not edit by hand.
//
// Classic computer-graphics models, decimated/generated to a per-frame budget
// and normalised to a unit bounding sphere. The arrays are \`const\` so they live
// in flash, not the app heap.
#pragma once
#include <stdint.h>

// Vertices are int16 fixed-point; divide by GEM_VSCALE for unit-sphere floats.
#define GEM_VSCALE ${VSCALE}

typedef struct {
  const char *name;
  const int16_t *verts;   // xyz triples, Q14 fixed-point
  uint16_t vert_count;
  const uint16_t *faces;  // CCW triangle index triples
  uint16_t face_count;
} GemModel;

`;

for (const m of models) out += m.block + "\n";

out += `static const GemModel GEM_MODELS[] = {\n`;
for (const m of models) {
  out += `  { "${m.name}", ${m.ident}_VERTS, ${m.vcount}, ${m.ident}_FACES, ${m.fcount} },\n`;
}
out += `};\n\n`;
out += `#define GEM_MODEL_COUNT ${models.length}\n`;
out += `#define GEM_MAX_VERTS ${maxVerts}\n`;
out += `#define GEM_MAX_FACES ${maxFaces}\n`;

writeFileSync(join(ROOT, "src", "c", "models.h"), out);

console.log("Baked models.h:");
for (const m of models) console.log(`  ${m.name.padEnd(8)} ${m.vcount} verts  ${m.fcount} tris`);
console.log(`  max: ${maxVerts} verts, ${maxFaces} tris`);
