/**
 * Software rasterizer for the Graphics Gems watchface.
 *
 * Pebble's Poco/graphics layer can't do 3D, so this is a from-scratch fixed
 * pipeline: rotate each vertex, push the model away from the camera, project,
 * cull back faces, depth-sort the survivors (painter's algorithm — there's no
 * room for a full z-buffer in the app heap), and fill them straight into the
 * captured framebuffer.
 *
 * Colour is a travelling gradient painted onto the mesh: each face takes a hue
 * from a two-colour ramp indexed by its position along a fixed model-space axis,
 * offset by a phase the caller advances over time (so the bands travel around
 * the mesh) while the two anchor hues themselves drift (so the colours shift).
 * Lighting then modulates it per face — Lambert diffuse, plus a Blinn-Phong
 * specular highlight in Phong mode. Everything is ordered-dithered per channel
 * into the 64-colour argb2222 display.
 *
 * The time floats as large 2D text on a plane just in front of the model centre,
 * inserted into the same depth order — so the frontmost slice of the model still
 * pierces the digits. The white text can be slightly translucent: after it's
 * drawn, the faces behind it are re-rasterised on a sparse checker, peeking the
 * model back through ~25% of the glyphs (no offscreen buffer needed).
 */

#include <pebble.h>
#include "render.h"
#include "fastmath.h"

// Camera at the origin looking down -Z; the model is pushed to z = -CAM_DIST.
// Unit-sphere models (radius 1) then span depth [CAM_DIST-1, CAM_DIST+1]. The
// silhouette size (cfg->fill) and how far the text plane sits in front of the
// model centre (cfg->text_front) are per-model — see the tables in main.c.
#define CAM_DIST 3.4f
#define SPEC_STRENGTH 0.85f // Blinn-Phong highlight strength (the exponent is fixed at 16 below)
#define AMBIENT 0.22f
#define UNLIT_LEVEL 0.80f // flat brightness for the unlit mode

// Light direction in view space (upper-left, slightly toward the camera).
static const float LX = -0.40f, LY = 0.62f, LZ = 0.68f;

// Gradient axis in model space (bands run perpendicular to it) and how many
// bands span the model.
static const float GDX = 0.18f, GDY = 0.97f, GDZ = 0.30f;
#define GRAD_FREQ 1.05f
#define GRAD_SAT 0.85f
#define HUE_SPLIT 0.17f // hue gap between the gradient's two anchor colours

static GFont s_font_wide; // HH:MM on one line
static GFont s_font_tall; // HH / MM stacked, larger

// Per-frame scratch (sized for the largest model).
static float s_vx[GEM_MAX_VERTS], s_vy[GEM_MAX_VERTS], s_vz[GEM_MAX_VERTS]; // view space
static float s_sx[GEM_MAX_VERTS], s_sy[GEM_MAX_VERTS];                      // screen space
static uint16_t s_order[GEM_MAX_FACES];                                     // depth-sorted (far first)
static float s_fdepth[GEM_MAX_FACES];
static uint8_t s_fr[GEM_MAX_FACES], s_fg[GEM_MAX_FACES], s_fb[GEM_MAX_FACES]; // per-face colour 0..255

// Ordered 4x4 Bayer thresholds (0..15) for per-channel dithering into 4 levels.
static const uint8_t BAYER[4][4] = {
    {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

void render_init(void) {
  s_font_wide = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_TIME_WIDE_66));
  s_font_tall = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_TIME_TALL_96));
}

void render_deinit(void) {
  fonts_unload_custom_font(s_font_wide);
  fonts_unload_custom_font(s_font_tall);
}

static inline float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// HSV (h,s,v in 0..1) -> RGB in 0..1.
static void hsv2rgb(float h, float s, float v, float *r, float *g, float *b) {
  h = fract1(h);
  int isector = (int)(h * 6.0f);
  float i = (float)isector;
  float f = h * 6.0f - i;
  float p = v * (1.0f - s);
  float q = v * (1.0f - s * f);
  float t = v * (1.0f - s * (1.0f - f));
  switch (((int)i) % 6) {
    case 0: *r = v; *g = t; *b = p; break;
    case 1: *r = q; *g = v; *b = p; break;
    case 2: *r = p; *g = v; *b = t; break;
    case 3: *r = p; *g = q; *b = v; break;
    case 4: *r = t; *g = p; *b = v; break;
    default: *r = v; *g = p; *b = q; break;
  }
}

// One channel's two dither levels for a flat face: low level and the 0..15
// probability of bumping to low+1.
typedef struct {
  uint8_t lo;
  uint8_t frac;
} DitherCh;

static inline DitherCh dither_ch(uint8_t v) {
  float fv = (float)v * (3.0f / 255.0f);
  int lo = (int)fv;
  if (lo > 3) lo = 3;
  DitherCh d = {(uint8_t)lo, (uint8_t)((fv - lo) * 16.0f)};
  return d;
}

// Flat-fill a triangle into the captured framebuffer in (r,g,b), ordered-dithered
// per channel into argb2222. Clipped per-row to the (possibly round) display.
// `stipple` writes only 1-in-4 pixels — used to peek the model back through the
// translucent time text.
static void fill_tri(GBitmap *fb, int H, float x0, float y0, float x1, float y1, float x2,
                     float y2, uint8_t r, uint8_t g, uint8_t b, bool stipple) {
  float ymin = y0, ymax = y0;
  if (y1 < ymin) ymin = y1;
  if (y2 < ymin) ymin = y2;
  if (y1 > ymax) ymax = y1;
  if (y2 > ymax) ymax = y2;
  int ya = ifloorf(ymin);
  int yb = iceilf(ymax);
  if (ya < 0) ya = 0;
  if (yb > H - 1) yb = H - 1;

  DitherCh dr = dither_ch(r), dg = dither_ch(g), db = dither_ch(b);

  const float ex0[3] = {x0, x1, x2};
  const float ey0[3] = {y0, y1, y2};
  const float ex1[3] = {x1, x2, x0};
  const float ey1[3] = {y1, y2, y0};

  for (int y = ya; y <= yb; y++) {
    if (stipple && (y & 1)) continue; // 1-in-4 with the x test below
    float yc = (float)y + 0.5f;
    float xl = 1e9f, xr = -1e9f;
    for (int e = 0; e < 3; e++) {
      float ay = ey0[e], by = ey1[e];
      if ((yc >= ay && yc < by) || (yc >= by && yc < ay)) {
        float t = (yc - ay) / (by - ay);
        float x = ex0[e] + t * (ex1[e] - ex0[e]);
        if (x < xl) xl = x;
        if (x > xr) xr = x;
      }
    }
    if (xr < xl) continue;
    GBitmapDataRowInfo row = gbitmap_get_data_row_info(fb, y);
    int xa = ifloorf(xl);
    int xb = iceilf(xr);
    if (xa < row.min_x) xa = row.min_x;
    if (xb > row.max_x) xb = row.max_x;
    const uint8_t *brow = BAYER[y & 3];
    for (int x = xa; x <= xb; x++) {
      if (stipple && (x & 1)) continue;
      int th = brow[x & 3];
      int rl = dr.lo + (dr.frac > th ? 1 : 0);
      int gl = dg.lo + (dg.frac > th ? 1 : 0);
      int bl = db.lo + (db.frac > th ? 1 : 0);
      row.data[x] = 0xC0 | (rl << 4) | (gl << 2) | bl; // a=3 (opaque) | r | g | b
    }
  }
}

static void clear_fb(GBitmap *fb, int H, uint8_t argb) {
  for (int y = 0; y < H; y++) {
    GBitmapDataRowInfo row = gbitmap_get_data_row_info(fb, y);
    for (int x = row.min_x; x <= row.max_x; x++) row.data[x] = argb;
  }
}

static int depth_cmp(const void *a, const void *b) {
  float da = s_fdepth[*(const uint16_t *)a];
  float db = s_fdepth[*(const uint16_t *)b];
  if (da < db) return 1;
  if (da > db) return -1;
  return 0;
}

// Draw one line of digits in `font`, white with a single dark drop-shadow for
// legibility (cheap — rendering these big custom glyphs is costly, so just two
// draws), with the glyph's *visual* box (which sits low in the text box because
// of the font's ascent) centred on screen-y `cy`.
static void draw_centered_line(GContext *ctx, int w, const char *s, GFont font, int cy) {
  GSize sz = graphics_text_layout_get_content_size(s, font, GRect(0, 0, w, 200),
                                                   GTextOverflowModeFill, GTextAlignmentCenter);
  int top = cy - (sz.h * 56) / 100;
  GRect box = GRect(0, top, w, sz.h + 8);
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, s, font, GRect(box.origin.x + 2, box.origin.y + 2, box.size.w, box.size.h),
                     GTextOverflowModeFill, GTextAlignmentCenter, NULL);
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, s, font, box, GTextOverflowModeFill, GTextAlignmentCenter, NULL);
}

// Draw the time, vertically centred: either HH:MM on one near-full-width line or
// HH over MM, larger.
static void draw_time_text(GContext *ctx, GRect bounds, const char *time_str, TextLayout layout) {
  int w = bounds.size.w, mid = bounds.size.h / 2;
  if (layout == TEXT_STACKED) {
    char hh[3] = {time_str[0], time_str[1], 0};
    char mm[3] = {time_str[3], time_str[4], 0};
    int half = 46; // vertical offset of each line's centre from the middle
    draw_centered_line(ctx, w, hh, s_font_tall, mid - half);
    draw_centered_line(ctx, w, mm, s_font_tall, mid + half);
  } else {
    draw_centered_line(ctx, w, time_str, s_font_wide, mid);
  }
}

void render_scene(GContext *ctx, GRect bounds, const GemModel *model, const RenderConfig *cfg,
                  const char *time_str) {
  const int W = bounds.size.w, H = bounds.size.h;
  const float cx = W * 0.5f, cy = H * 0.5f;
  const float focal = cfg->fill * 0.5f * (W < H ? W : H) * CAM_DIST;
  const float *m = cfg->rot;
  const float text_depth = CAM_DIST - cfg->text_front;
  const float inv = 1.0f / (float)GEM_VSCALE;

  // The gradient's two anchor colours for this frame.
  float ar, ag, ab, br, bg, bb;
  hsv2rgb(cfg->grad_hue, GRAD_SAT, 1.0f, &ar, &ag, &ab);
  hsv2rgb(cfg->grad_hue + HUE_SPLIT, GRAD_SAT, 1.0f, &br, &bg, &bb);

  // 1. Transform + project every vertex.
  const int16_t *vp = model->verts;
  for (int i = 0; i < model->vert_count; i++) {
    float x = vp[0] * inv, y = vp[1] * inv, z = vp[2] * inv;
    vp += 3;
    float rx = m[0] * x + m[1] * y + m[2] * z;
    float ry = m[3] * x + m[4] * y + m[5] * z;
    float rz = m[6] * x + m[7] * y + m[8] * z;
    float vzz = rz - CAM_DIST;
    s_vx[i] = rx;
    s_vy[i] = ry;
    s_vz[i] = vzz;
    float depth = -vzz;
    if (depth < 0.1f) depth = 0.1f;
    s_sx[i] = cx + focal * rx / depth;
    s_sy[i] = cy - focal * ry / depth;
  }

  // 2. Cull, colour (gradient x lighting), and depth each face.
  int n = 0;
  const uint16_t *fp = model->faces;
  const int16_t *MV = model->verts;
  for (int f = 0; f < model->face_count; f++) {
    int a = fp[0], b = fp[1], c = fp[2];
    fp += 3;
    float ux = s_vx[b] - s_vx[a], uy = s_vy[b] - s_vy[a], uz = s_vz[b] - s_vz[a];
    float wx = s_vx[c] - s_vx[a], wy = s_vy[c] - s_vy[a], wz = s_vz[c] - s_vz[a];
    float nx = uy * wz - uz * wy;
    float ny = uz * wx - ux * wz;
    float nz = ux * wy - uy * wx;
    float gx = -(s_vx[a] + s_vx[b] + s_vx[c]) / 3.0f;
    float gy = -(s_vy[a] + s_vy[b] + s_vy[c]) / 3.0f;
    float gz = -(s_vz[a] + s_vz[b] + s_vz[c]) / 3.0f;
    if (nx * gx + ny * gy + nz * gz <= 0) continue; // back face

    float n2 = nx * nx + ny * ny + nz * nz;
    if (n2 < 1e-12f) continue;
    float rl = fast_rsqrt(n2);
    nx *= rl; ny *= rl; nz *= rl;

    s_fdepth[f] = (-s_vz[a] - s_vz[b] - s_vz[c]) / 3.0f;

    // Gradient colour from the face's model-space centroid along the grad axis,
    // offset by the travelling phase. A smoothstep'd triangle ramp wraps
    // smoothly (no libm cos in this hot, deep call).
    float mcx = (MV[a * 3] + MV[b * 3] + MV[c * 3]) * inv / 3.0f;
    float mcy = (MV[a * 3 + 1] + MV[b * 3 + 1] + MV[c * 3 + 1]) * inv / 3.0f;
    float mcz = (MV[a * 3 + 2] + MV[b * 3 + 2] + MV[c * 3 + 2]) * inv / 3.0f;
    float gp = (mcx * GDX + mcy * GDY + mcz * GDZ) * GRAD_FREQ + cfg->grad_travel;
    float fr = fract1(gp);
    float tri = (fr < 0.5f) ? fr * 2.0f : (1.0f - fr) * 2.0f;
    float mix = tri * tri * (3.0f - 2.0f * tri);
    float cr = ar + (br - ar) * mix;
    float cg = ag + (bg - ag) * mix;
    float cb = ab + (bb - ab) * mix;

    if (cfg->mode == RENDER_UNLIT || cfg->mode == RENDER_WIRE) {
      float k = UNLIT_LEVEL;
      cr *= k; cg *= k; cb *= k;
    } else {
      float diff = nx * LX + ny * LY + nz * LZ;
      if (diff < 0) diff = 0;
      float shade = AMBIENT + (1.0f - AMBIENT) * diff;
      cr *= shade; cg *= shade; cb *= shade;
      if (cfg->mode == RENDER_PHONG && diff > 0) {
        // Half-vector between the light and the view direction (both normalised).
        float rg = fast_rsqrt(gx * gx + gy * gy + gz * gz);
        float hx = LX + gx * rg, hy = LY + gy * rg, hz = LZ + gz * rg;
        float rh = fast_rsqrt(hx * hx + hy * hy + hz * hz);
        float nh = (nx * hx + ny * hy + nz * hz) * rh;
        if (nh > 0) {
          // nh^16 by repeated squaring (no libm powf); ~Blinn-Phong shininess.
          float s2 = nh * nh, s4 = s2 * s2, s8 = s4 * s4, s16 = s8 * s8;
          float spec = s16 * SPEC_STRENGTH;
          cr += spec; cg += spec; cb += spec;
        }
      }
    }
    s_fr[f] = (uint8_t)(clampf(cr, 0, 1) * 255.0f);
    s_fg[f] = (uint8_t)(clampf(cg, 0, 1) * 255.0f);
    s_fb[f] = (uint8_t)(clampf(cb, 0, 1) * 255.0f);
    s_order[n++] = f;
  }

  // 3. Depth sort (far first); find where the text plane splits the order.
  qsort(s_order, n, sizeof(uint16_t), depth_cmp);
  int split = n;
  for (int i = 0; i < n; i++) {
    if (s_fdepth[s_order[i]] < text_depth) {
      split = i;
      break;
    }
  }

  uint8_t bg_argb = GColorBlack.argb;

  if (cfg->mode == RENDER_WIRE) {
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);
    graphics_context_set_stroke_width(ctx, 1);
    for (int pass = 0; pass < 2; pass++) {
      int lo = (pass == 0) ? 0 : split;
      int hi = (pass == 0) ? split : n;
      for (int i = lo; i < hi; i++) {
        int f = s_order[i];
        graphics_context_set_stroke_color(ctx, GColorFromRGB(s_fr[f], s_fg[f], s_fb[f]));
        const uint16_t *fc = &model->faces[f * 3];
        int a = fc[0], b = fc[1], c = fc[2];
        graphics_draw_line(ctx, GPoint(s_sx[a], s_sy[a]), GPoint(s_sx[b], s_sy[b]));
        graphics_draw_line(ctx, GPoint(s_sx[b], s_sy[b]), GPoint(s_sx[c], s_sy[c]));
        graphics_draw_line(ctx, GPoint(s_sx[c], s_sy[c]), GPoint(s_sx[a], s_sy[a]));
      }
      if (pass == 0) draw_time_text(ctx, bounds, time_str, cfg->text_layout);
    }
    return;
  }

  // Filled modes: clear + back faces, the text, then the front faces. For
  // translucency the back faces are re-stippled over the text first.
  GBitmap *fb = graphics_capture_frame_buffer(ctx);
  clear_fb(fb, H, bg_argb);
  for (int i = 0; i < split; i++) {
    int f = s_order[i];
    const uint16_t *fc = &model->faces[f * 3];
    int a = fc[0], b = fc[1], c = fc[2];
    fill_tri(fb, H, s_sx[a], s_sy[a], s_sx[b], s_sy[b], s_sx[c], s_sy[c], s_fr[f], s_fg[f], s_fb[f],
             false);
  }
  graphics_release_frame_buffer(ctx, fb);

  draw_time_text(ctx, bounds, time_str, cfg->text_layout);

  fb = graphics_capture_frame_buffer(ctx);
  if (cfg->translucent_text) {
    for (int i = 0; i < split; i++) {
      int f = s_order[i];
      const uint16_t *fc = &model->faces[f * 3];
      int a = fc[0], b = fc[1], c = fc[2];
      fill_tri(fb, H, s_sx[a], s_sy[a], s_sx[b], s_sy[b], s_sx[c], s_sy[c], s_fr[f], s_fg[f],
               s_fb[f], true);
    }
  }
  for (int i = split; i < n; i++) {
    int f = s_order[i];
    const uint16_t *fc = &model->faces[f * 3];
    int a = fc[0], b = fc[1], c = fc[2];
    fill_tri(fb, H, s_sx[a], s_sy[a], s_sx[b], s_sy[b], s_sx[c], s_sy[c], s_fr[f], s_fg[f], s_fb[f],
             false);
  }
  graphics_release_frame_buffer(ctx, fb);
}
