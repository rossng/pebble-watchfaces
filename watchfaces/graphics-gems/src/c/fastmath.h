// Inline, call-free float helpers.
//
// emery/gabbro are soft-float ARMv7-M (no FPU), and the app render path is
// called deep in the system's stack. Pulling in libm's sqrtf/cosf/powf/floorf
// there both costs a lot (software float) and — because each adds its own stack
// frame at the deepest point — overflows the app stack and hard-faults. These
// inline approximations stay in the caller's frame and never touch libm.
#pragma once

// Fast inverse square root (two Newton steps — plenty for shading/geometry).
static inline float fast_rsqrt(float x) {
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

// sin/cos without libm. cosf/sinf call __ieee754_rem_pio2f for |x| > pi/4,
// which has a large stack frame that overflows the tiny app stack here. This
// reduces the angle to [-pi, pi] by hand, then uses the classic Bhaskara-style
// parabola with one refinement step (~0.1% error — plenty for a tumbling model).
static inline float fast_sin(float x) {
  const float TWO_PI = 6.2831853f, INV_TWO_PI = 0.15915494f;
  x -= TWO_PI * (float)((int)(x * INV_TWO_PI + (x >= 0.0f ? 0.5f : -0.5f)));
  float ax = x < 0.0f ? -x : x;
  float y = 1.2732395f * x - 0.40528473f * x * ax; // 4/pi, -4/pi^2
  float ay = y < 0.0f ? -y : y;
  return 0.225f * (y * ay - y) + y;
}
static inline float fast_cos(float x) { return fast_sin(x + 1.5707963f); }
