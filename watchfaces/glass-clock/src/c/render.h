// Progressive SDF raytracer for the Glass Clock watchface.
//
// The four time digits are real 3D glass geometry (extruded 7-segment signed
// distance fields), raymarched with Fresnel reflection + two-surface refraction
// against a procedural coloured-light environment. There is NO 2D text — the
// digits ARE the glass.
//
// Rendering is progressive: each minute restarts a Monte-Carlo accumulation that
// develops from grainy noise toward a coherent image over ~60s. Samples are
// accumulated into an internal-resolution buffer (see render.c) and upscaled +
// dithered to the argb2222 display on each blit.
#pragma once
#include <pebble.h>

// Set up internal resolution + upscale maps for this display. Call once at init.
void render_init(GRect bounds);

// Apply config-page settings (see settings.h enums). Takes effect on the next
// render_restart, which the caller should trigger.
void render_set_options(int cel, int edge, int turn, int pattern, int mood, int translucency);

// Restart the accumulation for a new minute: rebuild the digit geometry from the
// 4-character "HHMM" string, pick a fresh random lighting mood, and seed pass 0
// (a full-screen environment sample so the whole image is present immediately).
void render_restart(const char *hhmm);

// Trace up to `budget` samples into the accumulation buffer, resuming where the
// last call left off. Cheap enough to run inside an app_timer callback.
void render_step(int budget);

// Upscale + dither the current accumulated mean into the captured framebuffer.
// Call from the layer update_proc (where the GContext is valid).
void render_blit(GContext *ctx, GRect bounds);

// Samples accumulated per pixel so far this minute (1 right after a restart).
// Used to stop the timer once the image has converged, to save battery.
int render_passes(void);

// ---- benchmarking (compile-time switch; 0 = off for release) ----
// The dominant soft-float cost is map_active() (SDF evaluation). Counting calls
// gives a deterministic, emulator-timing-independent cost metric. With a fixed
// bench scene (render_restart forces a constant time + seed under GC_BENCH) the
// counts are directly comparable across builds. Flip to 1 to re-bench changes:
// the watch then shows a fixed "10:27" and logs evals/sample over `pebble logs`.
#define GC_BENCH 0
#if GC_BENCH
unsigned long render_bench_evals(void);    // map_active() calls since last reset
unsigned long render_bench_samples(void);  // trace_pixel() calls since last reset
unsigned long render_bench_geom_evals(void); // map_active() calls in the per-minute geometry pass
unsigned long render_bench_env(void);      // env_scene() calls since last reset (shading-float proxy)
unsigned long render_bench_trans(void);    // transcendentals (rsqrt/sin/exp) since last reset
#endif
