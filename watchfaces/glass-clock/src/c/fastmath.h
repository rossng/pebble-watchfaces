// Inline, call-free float helpers for the soft-float render path.
//
// emery/gabbro are soft-float ARMv7-M (no FPU), and the render runs deep in the
// system's call stack. Pulling in libm's sqrtf/cosf/powf/expf/floorf there both
// costs a lot (software float) and — because each adds its own stack frame at the
// deepest point — can overflow the tiny app stack and hard-fault. These inline
// approximations stay in the caller's frame and never touch libm.
#pragma once

// Bench cost model: count transcendentals (the priciest soft-float ops on this
// no-FPU core). Compiled out unless GC_BENCH (defined in render.h, included first).
#if defined(GC_BENCH) && GC_BENCH
extern unsigned long g_bench_trans;
#define GC_TRANS() (g_bench_trans++)
#else
#define GC_TRANS()
#endif

// Branch-free-ish absolute value (single instruction at -O2; never libm).
static inline float fabs_i(float x) { return x < 0.0f ? -x : x; }

static inline float clampf_i(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}
static inline float minf_i(float a, float b) { return a < b ? a : b; }
static inline float maxf_i(float a, float b) { return a > b ? a : b; }

// Fast inverse square root (two Newton steps — plenty for shading/geometry).
static inline float fast_rsqrt(float x) {
  GC_TRANS();
  union {
    float f;
    int i;
  } u;
  float xh = 0.5f * x;
  u.f = x;
  u.i = 0x5f3759df - (u.i >> 1);
  float y = u.f;
  y = y * (1.5f - xh * y * y);
  y = y * (1.5f - xh * y * y);
  return y;
}

static inline float fast_sqrt(float x) {
  if (x <= 1e-12f) return 0.0f;
  return x * fast_rsqrt(x);
}

// floor/ceil to int without libm (handles negatives).
static inline int ifloorf(float x) {
  int i = (int)x;
  return (x < (float)i) ? i - 1 : i;
}
static inline int iceilf(float x) {
  int i = (int)x;
  return (x > (float)i) ? i + 1 : i;
}

// Fractional part in [0, 1).
static inline float fract1(float x) {
  float f = x - (float)(int)x;
  return (f < 0.0f) ? f + 1.0f : f;
}

// sin/cos without libm. cosf/sinf call __ieee754_rem_pio2f for |x| > pi/4, which
// has a large stack frame that overflows the tiny app stack deep in the render
// path. This reduces the angle to [-pi, pi] by hand, then uses the classic
// Bhaskara-style parabola with one refinement step (~0.1% error).
static inline float fast_sin(float x) {
  GC_TRANS();
  const float TWO_PI = 6.2831853f, INV_TWO_PI = 0.15915494f;
  x -= TWO_PI * (float)((int)(x * INV_TWO_PI + (x >= 0.0f ? 0.5f : -0.5f)));
  float ax = x < 0.0f ? -x : x;
  float y = 1.2732395f * x - 0.40528473f * x * ax;
  float ay = y < 0.0f ? -y : y;
  return 0.225f * (y * ay - y) + y;
}
static inline float fast_cos(float x) { return fast_sin(x + 1.5707963f); }

// e^-x for x >= 0 (Beer-Lambert absorption). 1/(Taylor of e^x), clamped — the
// glass paths are short so x stays small; this is monotonic and never negative.
static inline float fast_exp_neg(float x) {
  GC_TRANS();
  if (x < 0.0f) x = 0.0f;
  if (x > 8.0f) return 0.000335f;
  float v = 1.0f + x * (1.0f + x * (0.5f + x * (0.16666667f + x * 0.04166667f)));
  return 1.0f / v;
}

// 32-bit hash / xorshift for per-sample stochastic jitter (no libm, no state
// struct). Seed with the pixel index mixed with the pass number.
static inline unsigned int hash_u32(unsigned int x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

// Advance a 32-bit state and return a float in [0, 1).
static inline float rng_next(unsigned int *state) {
  unsigned int x = *state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *state = x;
  return (x >> 8) * (1.0f / 16777216.0f); // top 24 bits → [0,1)
}
