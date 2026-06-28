/**
 * Progressive SDF raytracer for the Glass Clock watchface.
 *
 * The four time digits are real 3D glass: each is an extruded 7-segment signed
 * distance field (2D rounded-box segments blended with a polynomial smin for the
 * "liquid glass" joints, then rounded-extruded for thickness + beveled rims).
 * Primary rays sphere-trace to the surface; Fresnel splits each hit into a
 * reflection and a two-surface refraction sampled against procedural coloured
 * lights — so there's no scene geometry to intersect, just env(dir). Colored
 * specular sparkle is added from the lights.
 *
 * CONTRAST: the Pebble screen needs the time to read clearly, so legibility is
 * NOT left to the glass colours. At each restart we compute a cheap 2D signed
 * distance "stencil" of the digits (projecting pixels onto the digit plane, no
 * marching). The blit uses that stencil to force tone separation — digit pixels
 * get a guaranteed brightness floor (hue preserved) plus a bright glassy edge
 * rim, background pixels are pushed dark — so the time is legible from frame 0
 * and stays high-contrast whatever the raytrace produces. The background is
 * always dark; the randomised colour lives in the lights and the glass body.
 *
 * Rendering is progressive Monte-Carlo: each minute restarts and accumulates
 * jittered samples (subpixel AA + heavy glossy roughness, so the lighting reads
 * as soft natural shading rather than hard geometric blobs) into an
 * internal-resolution uint16 sum buffer, upscaled + dithered to argb2222.
 *
 * No fonts, no glyph drawing — the digits ARE the glass.
 */

#include <pebble.h>
#include "render.h"
#include "fastmath.h"

#if GC_BENCH
static unsigned long s_bench_evals;   // map_active() calls (progressive shading)
static unsigned long s_bench_samples; // trace_pixel() calls
static unsigned long s_bench_geom;    // map_active() calls in the geometry/coverage pass
static unsigned long s_bench_env;     // env_scene() calls (shading-float proxy)
unsigned long g_bench_trans;          // transcendentals (rsqrt/sin/exp); incremented in fastmath.h
unsigned long render_bench_evals(void) { return s_bench_evals; }
unsigned long render_bench_samples(void) { return s_bench_samples; }
unsigned long render_bench_geom_evals(void) { return s_bench_geom; }
unsigned long render_bench_env(void) { return s_bench_env; }
unsigned long render_bench_trans(void) { return g_bench_trans; }
#endif

// ---- tunables ----
#define RES_SHORT 96      // internal pixels along the short screen axis
#define RES_MAX_W 96
#define RES_MAX_H 112

#define CAM_DIST 3.4f     // digits sit on the plane z = -CAM_DIST
#define TANF 0.74f        // tan(vfov/2); visible half-height at the plane = CAM_DIST*TANF
#define COL_X 0.78f       // digit column offset from centre (x) — wide enough that turned digits don't merge
#define ROW_Y 1.12f       // digit row offset from centre (y)
#define DIGIT_HW 0.5f     // digit cell half-width
#define DIGIT_HH 1.0f     // digit cell half-height
#define CR 0.045f         // 2D segment corner round
#define SMK 0.05f         // smin blend radius (liquid-glass joints)
#define HZ 0.32f          // glass half-thickness in z (chunky, so the side walls read when turned)
#define CHAMF 0.10f       // 45-degree chamfer facet on the front/back extrusion edges
#define BEV 0.12f         // rounded-bevel radius (EDGE_BEVEL)

// Cel outline (computed once per minute): dark toon lines along silhouettes and
// facet creases (normal / depth discontinuities between neighbouring pixels).
#define EDGE_COS 0.84f    // crease if neighbour normals differ by more than ~33deg
#define EDGE_DT 0.10f     // ...or depth jumps by this (world units)
#define OUTLINE_STR 0.72f // how dark the outline draws
#define ETA_IN (1.0f / 1.5f)
#define ETA_OUT 1.5f
#define EPS 0.0015f
#define MAX_PRI 48
#define MAX_INT 24
#define NLIGHTS 3

// Decoupled shading (deferred): the primary hit for a solid-interior pixel is the
// same every sample, so cache its depth once per minute (from the coverage probe)
// and skip the primary march on every accumulation sample. The cache stores
// LINEAR depth (|hit.z|), quantized to 1 byte; 0 means "not cached, march it"
// (used for silhouette-edge pixels, which keep per-sample jitter for AA). Linear
// depth (vs ray distance) lets view_pos() reconstruct neighbour positions for the
// depth-normal with no per-neighbour normalize.
#define GC_DEFER 1 // 1 = deferred shading (cache primary depth, skip per-sample march)
#define DEPTH_LO 2.0f
#define DEPTH_HI 5.2f
#define DEPTH_RANGE (DEPTH_HI - DEPTH_LO)

// ---- fidelity-affecting speculative toggles (A/B with these) ----
// #1 Normal-from-depth: for deferred (cached) pixels, reconstruct the primary
// normal from neighbouring cached depths (screen-space cross product) instead of
// the 4-tap SDF gradient. Saves 4 SDF evals/sample but yields the geometric
// surface normal (and is sensitive to the 1-byte depth quantization), so it can
// look slightly flatter/noisier — hence a toggle. Falls back to the SDF gradient
// at pixels whose neighbours aren't all cached.
#define GC_NORMAL_FROM_DEPTH 1
// #4 Thin-slab refraction: replace the interior march + exit-surface normal +
// second refraction with a single-surface approximation (entry-refracted ray
// samples the scene, fixed slab thickness for Beer-Lambert). Saves the whole
// interior march + a 4-tap normal per sample, at the cost of physically-accurate
// two-surface distortion. The glass is chunky/stylised so it largely holds.
#define GC_THIN_REFRACT 1
#define THIN_GLASS_LEN (2.0f * HZ) // approximate path length through the slab
// #5 Fast glossy: skip the normalize on the perturbed reflection/refraction
// direction (saves 2 rsqrt/sample). env_scene() tolerates a slightly non-unit
// direction (the glossy lobe is already a soft random perturbation), but the
// light blobs use dot(dir, light)^n, so a non-unit dir can shift their intensity
// a touch — hence a toggle. Default OFF pending on-watch sign-off.
#define GC_FAST_GLOSSY 0

// Contrast enforcement (driven by the 2D stencil in the blit). Kept gentle so it
// guarantees a legibility floor + a dark background WITHOUT washing out the glass
// colour: a dark glass pixel is lifted just enough to read, never to flat cream.
#define FG_FLOOR 0.40f    // minimum luminance for digit pixels (kept ahead of the visible pattern)
#define FG_MAXLIFT 1.8f   // cap on how much we brighten a dark digit pixel
#define BG_DIM 0.22f      // background luminance scale (dim, but the pattern still reads)

// ---- tiny vector type ----
typedef struct {
  float x, y, z;
} V3;
static inline V3 v3(float x, float y, float z) {
  V3 r = {x, y, z};
  return r;
}
static inline V3 vadd(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline V3 vsub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline V3 vscale(V3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline V3 vmul(V3 a, V3 b) { return v3(a.x * b.x, a.y * b.y, a.z * b.z); }
static inline float vdot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline V3 vcross(V3 a, V3 b) {
  return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline V3 vnorm(V3 a) { return vscale(a, fast_rsqrt(vdot(a, a) + 1e-20f)); }
static inline V3 vlerp(V3 a, V3 b, float t) {
  return v3(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t);
}

// ---- 7-segment geometry ----
typedef struct {
  float cx, cy, bx, by;
} Seg;
// Chunky bars: half-thickness ~0.14 (≈12px on screen), so there's real glass
// interior to refract/shade, not just edges.
static const Seg SEG[7] = {
    {0.0f, 0.86f, 0.30f, 0.14f},    // a  top
    {0.36f, 0.43f, 0.14f, 0.29f},   // b  upper-right
    {0.36f, -0.43f, 0.14f, 0.29f},  // c  lower-right
    {0.0f, -0.86f, 0.30f, 0.14f},   // d  bottom
    {-0.36f, -0.43f, 0.14f, 0.29f}, // e  lower-left
    {-0.36f, 0.43f, 0.14f, 0.29f},  // f  upper-left
    {0.0f, 0.0f, 0.30f, 0.14f},     // g  middle
};
static const uint8_t DIGIT_SEG[10] = {63, 6, 91, 79, 102, 109, 125, 7, 127, 111};

static const uint8_t BAYER[4][4] = {
    {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

// ---- scene / accumulation state ----
static int s_W, s_H;
static int s_RW, s_RH, s_npix;
static uint16_t *s_sum;     // heap accumulation buffer (npix * 3)
static uint8_t *s_cov;      // heap coverage stencil: 0..255 glass coverage per pixel
static uint8_t *s_edge;     // heap cel outline: 0..255 edge strength per pixel
static uint8_t *s_pdepth;   // heap deferred-shading depth cache: quantized primary hit depth (0 = none)
static uint8_t s_xmap[256], s_ymap[256];
static int s_G, s_cursor, s_pass;
static unsigned int s_stride;

static uint8_t s_mask[4];
static uint8_t s_seg[4][7]; // lit segment indices per digit (precomputed at restart)
static uint8_t s_nseg[4];   // count of lit segments per digit
static V3 s_center[4];
static V3 s_lo[4], s_hi[4];
static float s_rc[4], s_rs[4]; // per-digit Y-rotation (cos, sin) — slight 3D turn

// lighting / mood
static V3 s_bg0, s_bg1;        // background gradient (bottom, top)
static V3 s_lcol[NLIGHTS];     // light colours
static V3 s_ldir[NLIGHTS];     // light directions
static int s_ltight;           // env blob tightness (squarings)
static V3 s_body;              // glass body / tint colour
static V3 s_absorb;            // Beer-Lambert coefficients
#if GC_THIN_REFRACT
static V3 s_thin_att;          // precomputed exp(-absorb * slab) for thin-slab refraction
#endif
static float s_rough;          // glossy jitter (high = soft natural reflections)
static float s_reflstr;        // reflection strength
static float s_obl;            // oblique view angle this minute (radians)

// Randomised procedural pattern woven into the environment, so the glass has
// something interesting to refract/reflect. The bare background is dimmed by the
// blit's stencil, so this can be vivid without hurting contrast.
static int s_ptype;            // 0 stripes, 1 checker, 2 weave/dots
static V3 s_pax, s_pby;        // two pattern axes (directions)
static float s_pfreq;          // pattern frequency
static float s_pph0, s_pph1;   // phase offsets
static V3 s_pcol0, s_pcol1;    // pattern colours

// Config options (see settings.h). Defaults match the built-in look.
static int s_opt_cel = 3;      // CEL_CONTRAST
static int s_opt_edge = 2;     // EDGE_CHAMFER
static int s_opt_turn = 1;     // TURN_OBLIQUE
static int s_opt_pattern = 1;  // pattern on
static int s_opt_mood = 0;     // MOOD_SURPRISE
static int s_opt_trans = 1;    // TRANS_MEDIUM
static float s_transw = 0.34f; // refraction weight (set from translucency)

// ---- helpers ----
static float frand(void) { return (float)rand() / (float)RAND_MAX; }
static float frand2(void) { return frand() * 2.0f - 1.0f; }
static V3 rand_dir(void) {
  V3 d;
  float l;
  do {
    d = v3(frand2(), frand2(), frand2());
    l = vdot(d, d);
  } while (l < 0.05f);
  return vscale(d, fast_rsqrt(l));
}
static V3 hsv2rgb(float h, float s, float v) {
  h = fract1(h);
  int isector = (int)(h * 6.0f);
  float f = h * 6.0f - (float)isector;
  float p = v * (1.0f - s), q = v * (1.0f - s * f), t = v * (1.0f - s * (1.0f - f));
  switch (isector % 6) {
    case 0: return v3(v, t, p);
    case 1: return v3(q, v, p);
    case 2: return v3(p, v, t);
    case 3: return v3(p, q, v);
    case 4: return v3(t, p, v);
    default: return v3(v, p, q);
  }
}
static inline float luma(V3 c) { return 0.299f * c.x + 0.587f * c.y + 0.114f * c.z; }

// Quantize a primary hit depth to 1 byte (1..255; 0 reserved for "not cached").
static inline uint8_t depth_quantize(float t) {
  int qi = (int)((t - DEPTH_LO) * (254.0f / DEPTH_RANGE) + 0.5f);
  if (qi < 0) qi = 0;
  if (qi > 254) qi = 254;
  return (uint8_t)(qi + 1);
}
static inline float depth_dequantize(uint8_t q) {
  return DEPTH_LO + (float)(q - 1) * (DEPTH_RANGE * (1.0f / 254.0f));
}

// ---- SDF ----
static inline float sdRBox2(float px, float py, float bx, float by, float r) {
  float dx = fabs_i(px) - bx, dy = fabs_i(py) - by;
  float ax = dx > 0.0f ? dx : 0.0f, ay = dy > 0.0f ? dy : 0.0f;
  // The sqrt is only needed in the corner region (both axes outside). When one
  // axis is inside, sqrt(a^2) == a — skip the soft-float rsqrt entirely. This
  // fires up to 7x per digit2D in the hottest loop, and the single-axis result
  // is exact (no rsqrt approximation error).
  float outside;
  if (ax == 0.0f) {
    outside = ay;
  } else if (ay == 0.0f) {
    outside = ax;
  } else {
    outside = fast_sqrt(ax * ax + ay * ay);
  }
  float inside = dx > dy ? dx : dy;
  if (inside > 0.0f) inside = 0.0f;
  return outside + inside - r;
}
static inline float smin(float a, float b, float k) {
  float h = 0.5f + 0.5f * (b - a) / k;
  h = clampf_i(h, 0.0f, 1.0f);
  return (b + (a - b) * h) - k * h * (1.0f - h);
}
static float digit2D(float u, float v, int k) {
  float d = 1e9f;
  int n = s_nseg[k];
  const uint8_t *seg = s_seg[k];
  for (int j = 0; j < n; j++) {
    const Seg *S = &SEG[seg[j]];
    float dd = sdRBox2(u - S->cx, v - S->cy, S->bx, S->by, CR);
    d = smin(d, dd, SMK);
  }
  return d;
}
static float digit3D(float px, float py, float pz, int k) {
  float d2 = digit2D(px, py, k);
  float wz = fabs_i(pz) - HZ;
  float box = d2 > wz ? d2 : wz; // sharp extruded box
  if (s_opt_edge == 0) return box;
  if (s_opt_edge == 1) {
    // rounded bevel
    float wx = d2 + BEV, wy = wz + BEV;
    float mx = wx > wy ? wx : wy;
    if (mx > 0.0f) mx = 0.0f;
    float ax = wx > 0.0f ? wx : 0.0f, ay = wy > 0.0f ? wy : 0.0f;
    return mx + fast_sqrt(ax * ax + ay * ay) - BEV;
  }
  // chamfer: cut the front/back edge with a 45-degree plane
  float plane = (d2 + wz) * 0.7071f + CHAMF * 0.7071f;
  return box > plane ? box : plane;
}
static float map_active(V3 p, const int *act, int nact) {
#if GC_BENCH
  s_bench_evals++;
#endif
  float d = 1e9f;
  for (int i = 0; i < nact; i++) {
    int k = act[i];
    float lx = p.x - s_center[k].x, ly = p.y - s_center[k].y, lz = p.z - s_center[k].z;
    // rotate world->digit-local by -angle around Y
    float ux = s_rc[k] * lx - s_rs[k] * lz;
    float uz = s_rs[k] * lx + s_rc[k] * lz;
    float dd = digit3D(ux, ly, uz, k);
    if (dd < d) d = dd;
  }
  return d;
}
static V3 calc_normal(V3 p, const int *act, int nact) {
  const float h = EPS;
  V3 e1 = v3(1, -1, -1), e2 = v3(-1, -1, 1), e3 = v3(-1, 1, -1), e4 = v3(1, 1, 1);
  float d1 = map_active(vadd(p, vscale(e1, h)), act, nact);
  float d2 = map_active(vadd(p, vscale(e2, h)), act, nact);
  float d3 = map_active(vadd(p, vscale(e3, h)), act, nact);
  float d4 = map_active(vadd(p, vscale(e4, h)), act, nact);
  V3 n = vadd(vadd(vscale(e1, d1), vscale(e2, d2)), vadd(vscale(e3, d3), vscale(e4, d4)));
  return vnorm(n);
}
static inline V3 reflect_v(V3 I, V3 N) { return vsub(I, vscale(N, 2.0f * vdot(I, N))); }
static V3 refract_v(V3 I, V3 N, float eta, int *tir) {
  float cosi = -vdot(N, I);
  float k = 1.0f - eta * eta * (1.0f - cosi * cosi);
  if (k < 0.0f) {
    *tir = 1;
    return reflect_v(I, N);
  }
  *tir = 0;
  V3 t = vadd(vscale(I, eta), vscale(N, eta * cosi - fast_sqrt(k)));
  return vnorm(t);
}
// Glossy direction perturbation via the R3 low-discrepancy sequence (indexed by
// the accumulation pass), Cranley-Patterson-rotated per pixel+lobe (rh) so the
// jitter tiles the reflection/refraction lobe evenly across samples. Provably no
// worse than uniform random, and converges with visibly less noise for the same
// sample count — replaces the previous xorshift jitter at zero extra cost.
static V3 glossy(V3 d, unsigned int rh, float rough) {
  if (rough <= 0.0f) return d;
  float ox = fract1((float)s_pass * 0.8191725134f + (float)(rh & 0xffu) * (1.0f / 256.0f)) - 0.5f;
  float oy =
      fract1((float)s_pass * 0.6710436067f + (float)((rh >> 8) & 0xffu) * (1.0f / 256.0f)) - 0.5f;
  float oz =
      fract1((float)s_pass * 0.5497004779f + (float)((rh >> 16) & 0xffu) * (1.0f / 256.0f)) - 0.5f;
  V3 pd = v3(d.x + ox * rough, d.y + oy * rough, d.z + oz * rough);
#if GC_FAST_GLOSSY
  return pd; // skip normalize (saves 1 rsqrt); env lookup tolerates a near-unit dir
#else
  return vnorm(pd);
#endif
}

// Coloured lights only (no base) — broad blobs, sampled for specular + as part
// of the scene. Glossy glass reads them as soft natural colour, not hard spots.
static V3 env_lights(V3 d) {
  V3 c = v3(0, 0, 0);
  for (int i = 0; i < NLIGHTS; i++) {
    float m = vdot(d, s_ldir[i]);
    if (m < 0.0f) m = 0.0f;
    float p = m;
    for (int s = 0; s < s_ltight; s++) p *= p;
    c = vadd(c, vscale(s_lcol[i], p));
  }
  return c;
}

// The full environment the glass sits in: a vertical gradient, a randomised
// procedural pattern (so refraction/reflection have structure to distort), and
// the coloured lights. Used for the background, reflections and refraction; the
// blit dims the bare background so contrast is preserved while the glass shows
// the rich pattern bent through it.
static V3 env_scene(V3 d) {
#if GC_BENCH
  s_bench_env++;
#endif
  float gt = clampf_i(0.5f * (d.y + 1.0f), 0.0f, 1.0f);
  V3 c = vlerp(s_bg0, s_bg1, gt);

  if (!s_opt_pattern) return vadd(c, vscale(env_lights(d), 0.5f));

  float pa = vdot(d, s_pax) * s_pfreq + s_pph0;
  float pb = vdot(d, s_pby) * s_pfreq + s_pph1;
  float tt;
  if (s_ptype == 0) {
    float f = fract1(pa);
    tt = f < 0.5f ? f * 2.0f : (1.0f - f) * 2.0f; // triangle stripes
    tt = tt * tt * (3.0f - 2.0f * tt);            // smoothstep
  } else if (s_ptype == 1) {
    int ia = ifloorf(pa), ib = ifloorf(pb);
    tt = ((ia + ib) & 1) ? 1.0f : 0.0f;           // checker
  } else {
    float fa = fract1(pa) - 0.5f, fb = fract1(pb) - 0.5f;
    tt = (fa * fa + fb * fb) < 0.12f ? 1.0f : 0.0f; // dots / weave
  }
  c = vadd(c, vlerp(s_pcol0, s_pcol1, tt));
  return vadd(c, vscale(env_lights(d), 0.5f)); // dimmed lights here; spec uses them full
}

static V3 primary_dir(int ix, int iy, float jx, float jy) {
  float px = (((float)ix + 0.5f + jx) / (float)s_RW) * 2.0f - 1.0f;
  float py = 1.0f - (((float)iy + 0.5f + jy) / (float)s_RH) * 2.0f;
  float aspect = (float)s_RW / (float)s_RH;
  return vnorm(v3(px * aspect * TANF, py * TANF, -1.0f));
}

// Reconstruct the view-space hit position of pixel (ix,iy) from its LINEAR depth
// zc (= |hit.z|, what the deferred cache stores). Pure multiplies — no normalize.
// This is why the cache stores linear depth, not ray distance: neighbour
// positions for the depth-normal reconstruct without a per-neighbour rsqrt.
static V3 view_pos(int ix, int iy, float zc) {
  float px = (((float)ix + 0.5f) / (float)s_RW) * 2.0f - 1.0f;
  float py = 1.0f - (((float)iy + 0.5f) / (float)s_RH) * 2.0f;
  float aspect = (float)s_RW / (float)s_RH;
  return v3(px * aspect * TANF * zc, py * TANF * zc, -zc);
}

// Conservative sphere trace from the ray's AABB entry to its exit; returns 1 on
// hit with *out_t set to the hit parameter. Shared by the primary ray, the
// coverage probe and the hit test so they can't drift apart.
//
// NOTE: Keinert-style over-relaxation (step by omega*d with a safe overlap
// fallback) was tried here and measured a net loss on this scene — the per-digit
// AABB cull already clamps each march tightly around the surface, leaving little
// open space to stride through, and the non-convex SDF (smin joints + chamfers)
// trips the overshoot fallback often. It raised evals/sample ~3% (omega 1.2) to
// ~7% (omega 1.6), so plain sphere tracing wins. See git history if revisiting.
static int march_active(V3 ro, V3 dir, float t_enter, float t_exit, const int *act, int nact,
                        float *out_t) {
  float t = t_enter;
  for (int s = 0; s < MAX_PRI; s++) {
    float d = map_active(vadd(ro, vscale(dir, t)), act, nact);
    if (d < EPS) {
      *out_t = t;
      return 1;
    }
    t += d;
    if (t > t_exit) return 0;
  }
  return 0;
}

#if GC_NORMAL_FROM_DEPTH
// Reconstruct the surface normal at a cached interior pixel from neighbouring
// cached depths (screen-space central differences), avoiding the 4-tap SDF
// gradient. Returns 0 if any 4-neighbour is off-screen or uncached, so the
// caller falls back to calc_normal. Sign is irrelevant — the caller flips N to
// face the ray. Quality depends on the depth-cache precision (see GC_NORMAL_FROM_DEPTH).
static int normal_from_depth(int ix, int iy, V3 *outN) {
  if (ix <= 0 || ix >= s_RW - 1 || iy <= 0 || iy >= s_RH - 1) return 0;
  uint8_t ql = s_pdepth[iy * s_RW + (ix - 1)], qr = s_pdepth[iy * s_RW + (ix + 1)];
  uint8_t qu = s_pdepth[(iy - 1) * s_RW + ix], qd = s_pdepth[(iy + 1) * s_RW + ix];
  if (!ql || !qr || !qu || !qd) return 0;
  // Reconstruct neighbour positions from linear depth with no per-neighbour
  // normalize (view_pos is pure muls), so the whole normal costs 1 rsqrt instead
  // of 5 — the cost model's biggest remaining per-sample transcendental sink.
  V3 pl = view_pos(ix - 1, iy, depth_dequantize(ql));
  V3 pr = view_pos(ix + 1, iy, depth_dequantize(qr));
  V3 pu = view_pos(ix, iy - 1, depth_dequantize(qu));
  V3 pd = view_pos(ix, iy + 1, depth_dequantize(qd));
  *outN = vnorm(vcross(vsub(pr, pl), vsub(pd, pu)));
  return 1;
}
#endif

// Trace one sample; returns linear RGB (unbounded, clamped by caller). All
// stochastic jitter (subpixel AA, glossy lobes) is now driven by low-discrepancy
// sequences keyed on (pixel, pass), so no per-call RNG state is needed.
static V3 trace_pixel(int idx) {
#if GC_BENCH
  s_bench_samples++;
#endif
  int ix = idx % s_RW, iy = idx / s_RW;

  // Deferred shading (decoupled sampling): a solid-interior pixel's primary hit
  // is identical every sample, so reuse the depth cached at restart and skip the
  // per-sample primary march. Edge pixels have no cache (q==0) and march a
  // jittered ray, so silhouettes keep true subpixel AA. Either way the stochastic
  // shading (glossy reflection/refraction) still varies per sample and converges.
  uint8_t q = s_pdepth ? s_pdepth[idx] : 0;
  float jx = 0.0f, jy = 0.0f;
  if (!q) {
    // R2 low-discrepancy subpixel AA (indexed by pass), Cranley-Patterson-rotated
    // per pixel so neighbours decorrelate and edges resolve with little noise.
    unsigned int rh = hash_u32((unsigned int)idx * 0x9e3779b1u);
    float rotx = (float)(rh & 0xffffu) * (1.0f / 65536.0f);
    float roty = (float)(rh >> 16) * (1.0f / 65536.0f);
    jx = fract1((float)s_pass * 0.7548776662f + rotx) - 0.5f;
    jy = fract1((float)s_pass * 0.5698402909f + roty) - 0.5f;
  }
  V3 dir = primary_dir(ix, iy, jx, jy);
  V3 ro = v3(0, 0, 0);

  V3 inv = v3(1.0f / (fabs_i(dir.x) < 1e-6f ? (dir.x < 0 ? -1e-6f : 1e-6f) : dir.x),
              1.0f / (fabs_i(dir.y) < 1e-6f ? (dir.y < 0 ? -1e-6f : 1e-6f) : dir.y),
              1.0f / (fabs_i(dir.z) < 1e-6f ? (dir.z < 0 ? -1e-6f : 1e-6f) : dir.z));
  int act[4], nact = 0;
  float t_enter = 1e9f, t_exit = -1e9f;
  for (int k = 0; k < 4; k++) {
    float x1 = s_lo[k].x * inv.x, x2 = s_hi[k].x * inv.x;
    float tmn = minf_i(x1, x2), tmx = maxf_i(x1, x2);
    float y1 = s_lo[k].y * inv.y, y2 = s_hi[k].y * inv.y;
    tmn = maxf_i(tmn, minf_i(y1, y2));
    tmx = minf_i(tmx, maxf_i(y1, y2));
    float z1 = s_lo[k].z * inv.z, z2 = s_hi[k].z * inv.z;
    tmn = maxf_i(tmn, minf_i(z1, z2));
    tmx = minf_i(tmx, maxf_i(z1, z2));
    if (tmx >= maxf_i(tmn, 0.0f)) {
      act[nact++] = k;
      if (tmn < t_enter) t_enter = tmn;
      if (tmx > t_exit) t_exit = tmx;
    }
  }
  if (nact == 0) return env_scene(dir);
  if (t_enter < 0.0f) t_enter = 0.0f;

  V3 p;
  if (q) {
    // Cached primary hit: reconstruct position from linear depth (no march, no
    // normalize — view_pos is pure muls).
    p = view_pos(ix, iy, depth_dequantize(q));
  } else {
    float t;
    if (!march_active(ro, dir, t_enter, t_exit, act, nact, &t)) return env_scene(dir);
    p = vadd(ro, vscale(dir, t));
  }

  V3 N;
#if GC_NORMAL_FROM_DEPTH
  if (!(q && normal_from_depth(ix, iy, &N))) N = calc_normal(p, act, nact);
#else
  N = calc_normal(p, act, nact);
#endif
  if (vdot(N, dir) > 0.0f) N = vscale(N, -1.0f);
  float cosi = -vdot(N, dir);
  if (cosi < 0.0f) cosi = 0.0f;

  float c1 = 1.0f - cosi, c2 = c1 * c1, c5 = c2 * c2 * c1;
  float Fr = 0.04f + 0.96f * c5;

  // Colour-forward glass: the digit's dominant colour is its vivid body tint,
  // shaded by the key light for form; reflection of the coloured lights adds
  // glassy highlights (Fresnel-weighted, strongest at grazing angles), and the
  // refraction through the tinted body adds a hint of depth. This keeps a clear
  // hue rather than summing every light toward white.
  V3 Rc = reflect_v(dir, N);

  float ndl = vdot(N, s_ldir[0]);
  if (ndl < 0.0f) ndl = 0.0f;
  float shade = 0.6f + 0.4f * ndl; // bright floor -> vivid body, less desaturating lift
  V3 base = vscale(s_body, shade);

  V3 refl = env_scene(glossy(Rc, hash_u32((unsigned int)idx * 2654435761u + 0x9e37u), s_rough));

  // Refraction: march interior, refract out, see the patterned scene through the
  // tinted glass (this is where the glass distortion reads).
  int tir;
  V3 T0 = refract_v(dir, N, ETA_IN, &tir);
  V3 trans = v3(0, 0, 0);
  if (!tir) {
    unsigned int rh = hash_u32((unsigned int)idx * 2654435761u + 0x85ebu);
#if GC_THIN_REFRACT
    // Single-surface thin-slab approximation: sample the scene along the entry-
    // refracted ray and attenuate by a fixed slab thickness — no interior march,
    // no exit normal, no second refraction. Because the slab length is constant,
    // the Beer-Lambert attenuation is constant per minute (precomputed in
    // render_restart) — saving 3 exp() per sample, the cost model's top hit.
    V3 exitdir = T0;
    V3 att = s_thin_att;
#else
    V3 ip = vadd(p, vscale(T0, EPS * 2.0f));
    float ti = 0.0f;
    V3 ep = ip;
    for (int s = 0; s < MAX_INT; s++) {
      ep = vadd(ip, vscale(T0, ti));
      float d = map_active(ep, act, nact);
      if (d > -EPS) break;
      ti += -d;
    }
    float dGlass = ti;
    V3 Ne = calc_normal(ep, act, nact);
    if (vdot(Ne, T0) > 0.0f) Ne = vscale(Ne, -1.0f);
    int tir2;
    V3 T1 = refract_v(T0, Ne, ETA_OUT, &tir2);
    V3 exitdir = tir2 ? reflect_v(T0, Ne) : T1;
    V3 att = v3(fast_exp_neg(s_absorb.x * dGlass), fast_exp_neg(s_absorb.y * dGlass),
                fast_exp_neg(s_absorb.z * dGlass));
#endif
    trans = vmul(env_scene(glossy(exitdir, rh, s_rough)), att);
  }

  // Body-tinted core dominates the digit's colour; the patterned scene shows
  // through as tinted distortion via refraction (Beer-Lambert stamps the glass
  // hue onto it) plus glassy reflection highlights at grazing angles.
  V3 color = vscale(base, 1.0f - 0.4f * Fr);
  color = vadd(color, vscale(refl, Fr * s_reflstr * 0.7f));
  color = vadd(color, vscale(trans, s_transw));

  // Coloured specular sparkle off the clean reflection.
  V3 spec = v3(0, 0, 0);
  for (int i = 0; i < NLIGHTS; i++) {
    float m = vdot(Rc, s_ldir[i]);
    if (m < 0.0f) m = 0.0f;
    float pw = m;
    for (int s = 0; s < 3; s++) pw *= pw; // m^8 (broader, brighter sparkle)
    spec = vadd(spec, vscale(s_lcol[i], pw));
  }
  color = vadd(color, vscale(spec, 0.9f * Fr + 0.06f));
  return color;
}

// Does a primary ray hit the glass? (cull + sphere-trace, no shading.) Used to
// build the coverage stencil against the actual rotated 3D geometry.
static int primary_hit(V3 dir) {
  V3 ro = v3(0, 0, 0);
  V3 inv = v3(1.0f / (fabs_i(dir.x) < 1e-6f ? (dir.x < 0 ? -1e-6f : 1e-6f) : dir.x),
              1.0f / (fabs_i(dir.y) < 1e-6f ? (dir.y < 0 ? -1e-6f : 1e-6f) : dir.y),
              1.0f / (fabs_i(dir.z) < 1e-6f ? (dir.z < 0 ? -1e-6f : 1e-6f) : dir.z));
  int act[4], nact = 0;
  float t_enter = 1e9f, t_exit = -1e9f;
  for (int k = 0; k < 4; k++) {
    float x1 = s_lo[k].x * inv.x, x2 = s_hi[k].x * inv.x;
    float tmn = minf_i(x1, x2), tmx = maxf_i(x1, x2);
    float y1 = s_lo[k].y * inv.y, y2 = s_hi[k].y * inv.y;
    tmn = maxf_i(tmn, minf_i(y1, y2));
    tmx = minf_i(tmx, maxf_i(y1, y2));
    float z1 = s_lo[k].z * inv.z, z2 = s_hi[k].z * inv.z;
    tmn = maxf_i(tmn, minf_i(z1, z2));
    tmx = minf_i(tmx, maxf_i(z1, z2));
    if (tmx >= maxf_i(tmn, 0.0f)) {
      act[nact++] = k;
      if (tmn < t_enter) t_enter = tmn;
      if (tmx > t_exit) t_exit = tmx;
    }
  }
  if (nact == 0) return 0;
  if (t_enter < 0.0f) t_enter = 0.0f;
  float t;
  return march_active(ro, dir, t_enter, t_exit, act, nact, &t);
}

// Primary-ray probe: returns hit, and on a hit fills the surface normal + depth
// (used for cel edge detection).
static int primary_probe(V3 dir, V3 *outN, float *outDepth) {
  V3 ro = v3(0, 0, 0);
  V3 inv = v3(1.0f / (fabs_i(dir.x) < 1e-6f ? (dir.x < 0 ? -1e-6f : 1e-6f) : dir.x),
              1.0f / (fabs_i(dir.y) < 1e-6f ? (dir.y < 0 ? -1e-6f : 1e-6f) : dir.y),
              1.0f / (fabs_i(dir.z) < 1e-6f ? (dir.z < 0 ? -1e-6f : 1e-6f) : dir.z));
  int act[4], nact = 0;
  float t_enter = 1e9f, t_exit = -1e9f;
  for (int k = 0; k < 4; k++) {
    float x1 = s_lo[k].x * inv.x, x2 = s_hi[k].x * inv.x;
    float tmn = minf_i(x1, x2), tmx = maxf_i(x1, x2);
    float y1 = s_lo[k].y * inv.y, y2 = s_hi[k].y * inv.y;
    tmn = maxf_i(tmn, minf_i(y1, y2));
    tmx = minf_i(tmx, maxf_i(y1, y2));
    float z1 = s_lo[k].z * inv.z, z2 = s_hi[k].z * inv.z;
    tmn = maxf_i(tmn, minf_i(z1, z2));
    tmx = minf_i(tmx, maxf_i(z1, z2));
    if (tmx >= maxf_i(tmn, 0.0f)) {
      act[nact++] = k;
      if (tmn < t_enter) t_enter = tmn;
      if (tmx > t_exit) t_exit = tmx;
    }
  }
  if (nact == 0) return 0;
  if (t_enter < 0.0f) t_enter = 0.0f;
  float t;
  if (!march_active(ro, dir, t_enter, t_exit, act, nact, &t)) return 0;
  *outN = calc_normal(vadd(ro, vscale(dir, t)), act, nact);
  *outDepth = t;
  return 1;
}

// Per-row scratch for edge detection (previous row's normal/depth/hit).
static float s_prn_x[RES_MAX_W], s_prn_y[RES_MAX_W], s_prn_z[RES_MAX_W], s_prd[RES_MAX_W];
static uint8_t s_prh[RES_MAX_W];

// Build the coverage stencil (2x2-supersampled primary coverage, for contrast)
// AND the cel outline (silhouette + facet creases from normal/depth jumps between
// neighbouring pixels). Computed once per minute.
static void build_coverage(void) {
  for (int iy = 0; iy < s_RH; iy++) {
    V3 lN = v3(0, 0, 0);
    float ldep = 0.0f;
    int lhit = 0;
    for (int ix = 0; ix < s_RW; ix++) {
      int idx = iy * s_RW + ix;
      int cov = 0;
      cov += primary_hit(primary_dir(ix, iy, -0.25f, -0.25f));
      cov += primary_hit(primary_dir(ix, iy, 0.25f, -0.25f));
      cov += primary_hit(primary_dir(ix, iy, -0.25f, 0.25f));
      cov += primary_hit(primary_dir(ix, iy, 0.25f, 0.25f));
      s_cov[idx] = (uint8_t)(cov * 63); // 0,63,126,189,252

      V3 N = v3(0, 0, 0);
      float dep = 0.0f;
      V3 cdir = primary_dir(ix, iy, 0.0f, 0.0f);
      int hh = primary_probe(cdir, &N, &dep);
      // Cache LINEAR depth (|hit.z| = ray-distance * |dir.z|) for deferred shading,
      // but ONLY for fully-covered interior pixels (cov==4). Silhouette-edge pixels
      // stay uncached so they keep per-sample jittered marching (subpixel AA).
      if (s_pdepth)
        s_pdepth[idx] = (GC_DEFER && cov == 4 && hh) ? depth_quantize(dep * fabs_i(cdir.z)) : 0;
      int edge = 0;
      if (ix > 0 && hh != lhit) edge = 255;      // silhouette (left)
      if (iy > 0 && hh != s_prh[ix]) edge = 255; // silhouette (up)
      if (hh) {
        if (ix > 0 && lhit && edge < 200) {
          if (vdot(N, lN) < EDGE_COS || fabs_i(dep - ldep) > EDGE_DT) edge = 200;
        }
        if (iy > 0 && s_prh[ix] && edge < 200) {
          V3 pN = v3(s_prn_x[ix], s_prn_y[ix], s_prn_z[ix]);
          if (vdot(N, pN) < EDGE_COS || fabs_i(dep - s_prd[ix]) > EDGE_DT) edge = 200;
        }
      }
      s_edge[idx] = (uint8_t)edge;

      lN = N;
      ldep = dep;
      lhit = hh;
      s_prn_x[ix] = N.x;
      s_prn_y[ix] = N.y;
      s_prn_z[ix] = N.z;
      s_prd[ix] = dep;
      s_prh[ix] = (uint8_t)hh;
    }
  }
}

static unsigned int gcd_u(unsigned int a, unsigned int b) {
  while (b) {
    unsigned int t = a % b;
    a = b;
    b = t;
  }
  return a;
}

void render_init(GRect bounds) {
  s_W = bounds.size.w;
  s_H = bounds.size.h;
  int shortdim = s_W < s_H ? s_W : s_H;
  s_RW = s_W * RES_SHORT / shortdim;
  s_RH = s_H * RES_SHORT / shortdim;
  if (s_RW > RES_MAX_W) s_RW = RES_MAX_W;
  if (s_RH > RES_MAX_H) s_RH = RES_MAX_H;
  s_npix = s_RW * s_RH;

  s_sum = (uint16_t *)malloc((size_t)s_npix * 3 * sizeof(uint16_t));
  s_cov = (uint8_t *)malloc((size_t)s_npix);
  s_edge = (uint8_t *)malloc((size_t)s_npix);
  s_pdepth = (uint8_t *)malloc((size_t)s_npix); // optional: deferred-shading depth cache

  for (int sx = 0; sx < s_W && sx < 256; sx++) {
    int ix = sx * s_RW / s_W;
    if (ix >= s_RW) ix = s_RW - 1;
    s_xmap[sx] = (uint8_t)ix;
  }
  for (int sy = 0; sy < s_H && sy < 256; sy++) {
    int iy = sy * s_RH / s_H;
    if (iy >= s_RH) iy = s_RH - 1;
    s_ymap[sy] = (uint8_t)iy;
  }

  unsigned int st = (unsigned int)((float)s_npix * 0.6180339f);
  if (st < 2) st = 2;
  st |= 1u;
  while (gcd_u(st, (unsigned int)s_npix) != 1u) {
    st += 2u;
    if (st >= (unsigned int)s_npix) st = 3u;
  }
  s_stride = st;
}

void render_restart(const char *hhmm) {
  if (!s_sum || !s_cov || !s_edge) return;
#if GC_BENCH
  // Force a fixed scene + seed so eval counts are directly comparable across builds.
  srand(777);
  hhmm = "1027";
  s_bench_evals = 0;
  g_bench_trans = 0;
  s_bench_env = 0;
#endif
  for (int k = 0; k < 4; k++) {
    uint8_t mask = DIGIT_SEG[hhmm[k] - '0'];
    s_mask[k] = mask;
    // Precompute the lit-segment index list so digit2D iterates only lit
    // segments (and drops the per-segment bit test) in the hot SDF loop.
    int n = 0;
    for (int s = 0; s < 7; s++)
      if (mask & (1 << s)) s_seg[k][n++] = (uint8_t)s;
    s_nseg[k] = (uint8_t)n;
  }
  s_center[0] = v3(-COL_X, ROW_Y, -CAM_DIST);
  s_center[1] = v3(COL_X, ROW_Y, -CAM_DIST);
  s_center[2] = v3(-COL_X, -ROW_Y, -CAM_DIST);
  s_center[3] = v3(COL_X, -ROW_Y, -CAM_DIST);
  // View the whole clock obliquely: turn every digit the SAME way around the up
  // axis by a sizeable angle (28-40deg, random sign per minute) so the side walls
  // and facets show and the thickness is obvious. A small per-digit jitter keeps
  // it lively without looking random.
  s_obl = (s_opt_turn == 0) ? 0.0f
                            : (0.35f + frand() * 0.14f) * (frand() < 0.5f ? -1.0f : 1.0f);
  float hwx = DIGIT_HW + 0.02f, hzz = HZ + 0.02f, ey = DIGIT_HH + 0.02f;
  for (int k = 0; k < 4; k++) {
    float ang = (s_opt_turn == 0) ? 0.0f : (s_obl + frand2() * 0.07f); // ±4deg jitter
    s_rc[k] = fast_cos(ang);
    s_rs[k] = fast_sin(ang);
    float ac = fabs_i(s_rc[k]), as = fabs_i(s_rs[k]);
    float ex = ac * hwx + as * hzz + 0.02f;
    float ez = as * hwx + ac * hzz + 0.02f;
    s_lo[k] = v3(s_center[k].x - ex, s_center[k].y - ey, s_center[k].z - ez);
    s_hi[k] = v3(s_center[k].x + ex, s_center[k].y + ey, s_center[k].z + ez);
  }

  // Dim, faintly-coloured background gradient (the blit dims it further, so the
  // digits stay high-contrast); the mood varies the lights + glass tint.
  float baseh = frand();
  int mood = (s_opt_mood == 1) ? 0 : (s_opt_mood == 2) ? 1 : (rand() & 1);
  s_bg0 = vscale(hsv2rgb(baseh, 0.5f, 1.0f), 0.10f);
  s_bg1 = vscale(hsv2rgb(fract1(baseh + 0.5f), 0.5f, 1.0f), 0.04f);
  if (mood == 0) {
    // Neon: a few saturated lights, vivid glass tint.
    s_ltight = 3; // ^8 (fairly broad)
    s_reflstr = 1.2f;
    s_rough = 0.045f; // tighter -> flatter, more distinct per-facet shading
    for (int i = 0; i < NLIGHTS; i++) {
      float hue = fract1(baseh + (float)i * 0.34f + frand2() * 0.05f);
      s_lcol[i] = vscale(hsv2rgb(hue, 0.95f, 1.0f), 1.5f);
      s_ldir[i] = rand_dir();
    }
    s_body = hsv2rgb(fract1(baseh + 0.12f), 0.85f, 1.0f);
    s_absorb = vscale(vsub(v3(1, 1, 1), s_body), 2.0f);
  } else {
    // Jewel: broader, softer lights, lighter pastel glass.
    s_ltight = 2; // ^4 (broad)
    s_reflstr = 1.05f;
    s_rough = 0.06f; // tighter -> flatter, more distinct per-facet shading
    for (int i = 0; i < NLIGHTS; i++) {
      float hue = fract1(baseh + 0.5f + (float)i * 0.18f + frand2() * 0.08f);
      s_lcol[i] = vscale(hsv2rgb(hue, 0.75f, 1.0f), 1.1f);
      s_ldir[i] = rand_dir();
    }
    s_body = hsv2rgb(fract1(baseh + 0.45f), 0.55f, 1.0f);
    s_absorb = vscale(vsub(v3(1, 1, 1), s_body), 1.4f);
  }

  // Translucency: how much of the scene shows through the glass, and how strongly
  // the body tint absorbs it.
  if (s_opt_trans == 0) {
    s_transw = 0.16f;
    s_absorb = vscale(s_absorb, 1.7f);
  } else if (s_opt_trans == 2) {
    s_transw = 0.60f;
    s_absorb = vscale(s_absorb, 0.5f);
  } else {
    s_transw = 0.34f;
  }

#if GC_THIN_REFRACT
  // Thin-slab path length is constant, so the Beer-Lambert attenuation is too —
  // precompute it once here instead of 3 exp() per sample.
  s_thin_att = v3(fast_exp_neg(s_absorb.x * THIN_GLASS_LEN), fast_exp_neg(s_absorb.y * THIN_GLASS_LEN),
                  fast_exp_neg(s_absorb.z * THIN_GLASS_LEN));
#endif

  // Randomised environment pattern for the glass to refract/reflect.
  s_ptype = rand() % 3;
  s_pax = rand_dir();
  s_pby = rand_dir();
  s_pfreq = 2.0f + frand() * 4.0f; // 2..6 bands across the view
  s_pph0 = frand() * 6.2831f;
  s_pph1 = frand() * 6.2831f;
  s_pcol0 = vscale(hsv2rgb(fract1(baseh + 0.30f), 0.8f, 1.0f), 0.85f);
  s_pcol1 = vscale(hsv2rgb(fract1(baseh + 0.72f), 0.8f, 1.0f), 0.16f);

  build_coverage();
#if GC_BENCH
  s_bench_geom = s_bench_evals; // evals spent on the per-minute geometry/coverage pass
  s_bench_evals = 0;            // from here on, count only progressive shading evals
  s_bench_samples = 0;
  g_bench_trans = 0; // count only progressive-shading transcendentals + env calls
  s_bench_env = 0;
#endif

  // Seed pass 0 so the whole image is present immediately: digit cores in the
  // body tint, background dark. The blit's contrast enforcement makes this read
  // as a crisp high-contrast time from frame 0.
  for (int i = 0; i < s_npix; i++) {
    int inside = s_cov[i] > 127;
    V3 c;
    if (inside) {
      c = s_body;
    } else {
      int ix = i % s_RW, iy = i / s_RW;
      c = env_scene(primary_dir(ix, iy, 0.0f, 0.0f));
    }
    s_sum[i * 3 + 0] = (uint16_t)(clampf_i(c.x, 0.0f, 1.0f) * 255.0f);
    s_sum[i * 3 + 1] = (uint16_t)(clampf_i(c.y, 0.0f, 1.0f) * 255.0f);
    s_sum[i * 3 + 2] = (uint16_t)(clampf_i(c.z, 0.0f, 1.0f) * 255.0f);
  }
  s_G = 1;
  s_cursor = 0;
  s_pass = 1;
}

void render_set_options(int cel, int edge, int turn, int pattern, int mood, int translucency) {
  s_opt_cel = cel;
  s_opt_edge = edge;
  s_opt_turn = turn;
  s_opt_pattern = pattern;
  s_opt_mood = mood;
  s_opt_trans = translucency;
}

int render_passes(void) { return s_G; }

void render_step(int budget) {
  if (!s_sum) return;
  // Advance the cursor by exactly `budget` steps per burst (same as before), so
  // the per-burst wall time stays bounded — the firmware must get the CPU back
  // promptly between bursts or input + the screenshot RPC stall. We only skip
  // the *work* for background pixels, never inflate the burst.
  for (int n = 0; n < budget; n++) {
    // Offset the visitation phase per pass so the transient fill pattern moves
    // around rather than re-tracing the same lattice each pass.
    unsigned int seq = (unsigned int)s_cursor + (unsigned int)s_pass * 2654435761u;
    int i = (int)((seq * s_stride) % (unsigned int)s_npix);

    // Background pixels (no glass coverage) are deterministic env_scene — already
    // seeded at restart and never re-traced. Skipping their trace makes each
    // burst cheaper than tracing the whole frame did, at no cost to the image.
    if (s_cov[i] != 0) {
      V3 c = trace_pixel(i);
      int r = (int)(clampf_i(c.x, 0.0f, 1.0f) * 255.0f);
      int g = (int)(clampf_i(c.y, 0.0f, 1.0f) * 255.0f);
      int b = (int)(clampf_i(c.z, 0.0f, 1.0f) * 255.0f);
      uint16_t *px = &s_sum[i * 3];
      int nr = px[0] + r, ng = px[1] + g, nb = px[2] + b;
      px[0] = nr > 65535 ? 65535 : (uint16_t)nr;
      px[1] = ng > 65535 ? 65535 : (uint16_t)ng;
      px[2] = nb > 65535 ? 65535 : (uint16_t)nb;
    }
    if (++s_cursor >= s_npix) {
      s_cursor = 0;
      s_pass++;
      s_G++;
    }
  }
}

static inline int dither2(int v, int th) {
  float fv = (float)v * (3.0f / 255.0f);
  int lo = (int)fv;
  if (lo > 3) lo = 3;
  int frac = (int)((fv - (float)lo) * 16.0f);
  return lo + (frac > th ? 1 : 0);
}

void render_blit(GContext *ctx, GRect bounds) {
  (void)bounds;
  if (!s_sum || !s_cov || !s_edge) return;
  GBitmap *fb = graphics_capture_frame_buffer(ctx);
  if (!fb) return;
  float invG = 1.0f / (float)s_G;
  for (int sy = 0; sy < s_H; sy++) {
    GBitmapDataRowInfo row = gbitmap_get_data_row_info(fb, sy);
    int iy = s_ymap[sy];
    const uint8_t *bayer = BAYER[sy & 3];
    int base = iy * s_RW;
    for (int sx = row.min_x; sx <= row.max_x; sx++) {
      int idx = base + s_xmap[sx];
      uint16_t *px = &s_sum[idx * 3];
      // Glass pixels accumulate one sample per pass → divide by the pass count.
      // Background pixels hold a single deterministic seed sample (never
      // re-traced) → divide by 1. Both stored as value*255.
      float sc = (s_cov[idx] == 0) ? (1.0f / 255.0f) : (invG / 255.0f);
      V3 g = v3((float)px[0] * sc, (float)px[1] * sc, (float)px[2] * sc);

      // --- contrast enforcement from the coverage stencil ---
      float fill = (float)s_cov[idx] * (1.0f / 252.0f); // 1 inside glass, 0 outside
      if (fill > 1.0f) fill = 1.0f;

      // Foreground: lift dark digit pixels to a luminance floor (hue preserved).
      float lu = luma(g);
      float lift = FG_FLOOR / (lu > 0.06f ? lu : 0.06f);
      if (lift < 1.0f) lift = 1.0f;
      if (lift > FG_MAXLIFT) lift = FG_MAXLIFT;
      V3 fg = vscale(g, lift);
      // Background: push dark.
      V3 bgc = vscale(g, BG_DIM);
      V3 fin = vlerp(bgc, fg, fill);

      // Cel outline: silhouette + facet-crease edges as toon lines.
      int e = s_edge[idx];
      if (e && s_opt_cel != 0) {
        float target = (s_opt_cel == 1)   ? 1.0f                          // white
                       : (s_opt_cel == 2) ? 0.0f                          // black
                                          : (luma(fin) < 0.5f ? 1.0f : 0.0f); // contrast
        float a = OUTLINE_STR * ((float)e * (1.0f / 255.0f));
        fin = vlerp(fin, v3(target, target, target), a);
      }

      int r = (int)(clampf_i(fin.x, 0.0f, 1.0f) * 255.0f);
      int gg = (int)(clampf_i(fin.y, 0.0f, 1.0f) * 255.0f);
      int b = (int)(clampf_i(fin.z, 0.0f, 1.0f) * 255.0f);
      int th = bayer[sx & 3];
      row.data[sx] = 0xC0 | (dither2(r, th) << 4) | (dither2(gg, th) << 2) | dither2(b, th);
    }
  }
  graphics_release_frame_buffer(ctx, fb);
}
