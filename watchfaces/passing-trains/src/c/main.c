/**
 * Passing Trains — a native Pebble C watchface.
 *
 * One Dutch (NS) train at a time glides across a sky-blue background, scaled to
 * fill the screen's full height, drifting a few pixels each minute. When a train
 * has fully passed (plus a short sky gap) another random train rolls in. A
 * tap/shake fast-forwards to the head of the next train.
 *
 * Full height on a tiny device is the whole trick. A train scaled to ~200px tall
 * is ~1500px wide and far too big to hold uncompressed in RAM. So each train
 * lives in flash *small* (a 56px-tall colour gbitmap baked by
 * scripts/prepare-images.mjs) and we upscale it on the fly: every frame, a
 * hand-written nearest-neighbour blit reads the small bitmap and writes the
 * full-height image straight into the captured framebuffer. Source `GColor8`
 * bytes (argb2222) are identical to the framebuffer's, so it's a raw copy — no
 * scaled image is ever materialised, only the on-screen pixels.
 */

#include <pebble.h>
#include "trains.h"

#define BAR_HEIGHT 34 // bottom departure-board bar
#define TRIM_H 3 // NS-yellow accent along the top of the bar
#define GAP 40 // sky gap after a train, in source pixels
#define DRIFT 8 // source pixels the train advances each minute
#define DRIFT_MS 700 // eased duration of the per-minute drift
#define SKIP_MS 1800 // eased duration of a tap-to-skip
#define FRAME_MS 33 // ~30 fps animation tick
#define LABEL_OFFSET 24 // source-x of the type label, from each train's front
#define LABEL_FONT_H 24 // line height of the label font (Gothic 24)
#define LABEL_BOX_W 600 // max label width (drawn char by char); it scrolls into view
#define LABEL_TRACK 20 // per-character advance, for spread-out caps

// Sky colour as a raw GColor8 byte: a=3,r=1,g=2,b=3 for (85,170,255).
// MUST match BACKGROUND in scripts/prepare-images.mjs.
#define SKY 0xDB

// NS departure-board palette.
#define NS_BLUE GColorFromRGB(0, 85, 170)
#define NS_YELLOW GColorFromRGB(255, 170, 0)

static Window *s_window;
static Layer *s_layer;
static GFont s_font;
static GFont s_date_font;
static GFont s_label_font;

static GBitmap *s_current;
static GBitmap *s_next;
static int s_current_idx;
static int s_next_idx;

static float s_pan; // source-x at the left screen edge, along [current|GAP|next]
static char s_time[8];
static char s_date[4]; // day of month, shown in the "platform" chip

// Animation state (a single eased glide of s_pan toward a target).
static AppTimer *s_timer;
static bool s_animating;
static bool s_is_skip; // is the current glide a tap-to-skip (vs a minute drift)?
static bool s_skip_queued; // a tap arrived mid-skip; run one more when this ends
static bool s_first_tick = true; // hold at the train's head through the first minute
static float s_anim_from;
static float s_anim_to;
static int s_anim_step;
static int s_anim_steps;

// Per-column scratch: which train (0 sky, 1 current, 2 next) and which source
// column each screen column samples. Recomputed once per frame. Sized for the
// widest supported display.
static uint8_t s_code[360];
static int16_t s_col[360];

static int random_except(int except) {
  if (TRAIN_COUNT <= 1) {
    return 0;
  }
  int i = rand() % TRAIN_COUNT;
  if (i == except) {
    i = (i + 1) % TRAIN_COUNT;
  }
  return i;
}

// Draw a name as spread-out caps: one glyph at a time, advancing LABEL_TRACK pixels
// per character (spaces just advance). Off-screen glyphs are skipped, so long names
// that scroll across with the train are cheap.
static void draw_label(GContext *ctx, const char *text, int x, int y, int w) {
  char ch[2] = {0, 0};
  for (const char *p = text; *p != '\0'; p++, x += LABEL_TRACK) {
    if (*p == ' ' || x <= -LABEL_TRACK || x >= w) {
      continue;
    }
    ch[0] = *p;
    graphics_draw_text(ctx, ch, s_label_font, GRect(x, y, LABEL_TRACK + 12, LABEL_FONT_H + 4),
                       GTextOverflowModeFill, GTextAlignmentLeft, NULL);
  }
}

static void update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  int w = bounds.size.w;
  int h = bounds.size.h;
  int band_h = h - BAR_HEIGHT;
  if (band_h < 1) {
    band_h = h;
  }
  if (w > (int)(sizeof(s_code) / sizeof(s_code[0]))) {
    w = sizeof(s_code) / sizeof(s_code[0]);
  }

  // If a bitmap failed to load, fall back to plain sky so we never dereference
  // a NULL gbitmap (which would hard-fault the watch).
  bool have_current = s_current != NULL;
  bool have_next = s_next != NULL;
  GSize cs = have_current ? gbitmap_get_bounds(s_current).size : GSize(0, 56);
  GSize ns = have_next ? gbitmap_get_bounds(s_next).size : GSize(0, 56);
  int src_h = cs.h > 0 ? cs.h : 56;
  int cw = cs.w;
  int nw = ns.w;

  // Map each screen column to a strip position (in source pixels) and resolve
  // which train it falls in: [0,cw) current, [cw,cw+GAP) sky, then next.
  for (int x = 0; x < w; x++) {
    int strip = (int)(s_pan + (float)x * src_h / band_h);
    if (strip >= 0 && strip < cw) {
      s_code[x] = 1;
      s_col[x] = strip;
    } else {
      int n = strip - (cw + GAP);
      if (n >= 0 && n < nw) {
        s_code[x] = 2;
        s_col[x] = n;
      } else {
        s_code[x] = 0;
        s_col[x] = 0;
      }
    }
  }

  uint8_t *cur_data = have_current ? gbitmap_get_data(s_current) : NULL;
  uint8_t *next_data = have_next ? gbitmap_get_data(s_next) : NULL;
  int cur_stride = have_current ? gbitmap_get_bytes_per_row(s_current) : 0;
  int next_stride = have_next ? gbitmap_get_bytes_per_row(s_next) : 0;

  GBitmap *fb = graphics_capture_frame_buffer(ctx);
  for (int y = 0; y < band_h; y++) {
    GBitmapDataRowInfo row = gbitmap_get_data_row_info(fb, y);
    int sy = y * src_h / band_h;
    if (sy >= src_h) {
      sy = src_h - 1;
    }
    uint8_t *crow = cur_data + sy * cur_stride;
    uint8_t *nrow = next_data + sy * next_stride;
    int x0 = row.min_x;
    int x1 = row.max_x;
    if (x1 >= w) {
      x1 = w - 1;
    }
    for (int x = x0; x <= x1; x++) {
      uint8_t px;
      if (s_code[x] == 1) {
        px = crow[s_col[x]];
      } else if (s_code[x] == 2) {
        px = nrow[s_col[x]];
      } else {
        px = SKY;
      }
      row.data[x] = px;
    }
  }
  graphics_release_frame_buffer(ctx, fb);

  // Subtle type labels, pinned to each train in strip-space (LABEL_OFFSET source
  // pixels from its front) so they scroll along with the train and slide off as it
  // passes. Vertically centred in the gap between the screen top and the train body's
  // roofline (TRAIN_SKY_ABOVE of the source canvas is sky above the body); some trains
  // (e.g. the Koploper) carry an extra upward nudge to clear a tall front cab.
  int roof_y = TRAIN_SKY_ABOVE * band_h / src_h;
  int base_label_y = (roof_y - LABEL_FONT_H) / 2 - 2;
  // Plain light grey, so the label reads as a faint watermark over the sky.
  graphics_context_set_text_color(ctx, GColorLightGray);
  if (have_current) {
    int lx = (int)((LABEL_OFFSET - s_pan) * band_h / src_h);
    int ly = base_label_y - TRAIN_LABEL_DY[s_current_idx] * band_h / src_h;
    if (ly < 0) {
      ly = 0;
    }
    if (lx > -LABEL_BOX_W && lx < w) {
      draw_label(ctx, TRAIN_NAMES[s_current_idx], lx, ly, w);
    }
  }
  if (have_next) {
    int lx = (int)((cw + GAP + LABEL_OFFSET - s_pan) * band_h / src_h);
    int ly = base_label_y - TRAIN_LABEL_DY[s_next_idx] * band_h / src_h;
    if (ly < 0) {
      ly = 0;
    }
    if (lx > -LABEL_BOX_W && lx < w) {
      draw_label(ctx, TRAIN_NAMES[s_next_idx], lx, ly, w);
    }
  }

  // Bottom bar styled after an NS departure board: white time on NS blue, a yellow
  // accent strip along the top, and a yellow "platform" chip with today's date.
  // (Drawn the normal way so it clips correctly on the round display.)
  int top = h - BAR_HEIGHT;
  graphics_context_set_fill_color(ctx, NS_BLUE);
  graphics_fill_rect(ctx, GRect(0, top, w, BAR_HEIGHT), 0, GCornerNone);
  graphics_context_set_fill_color(ctx, NS_YELLOW);
  graphics_fill_rect(ctx, GRect(0, top, w, TRIM_H), 0, GCornerNone);

  int chip_w = 24;
  int chip_h = 20;
  int time_w = graphics_text_layout_get_content_size(s_time, s_font, GRect(0, 0, w, 40),
                                                     GTextOverflowModeFill, GTextAlignmentLeft)
                   .w;
  int time_x, chip_x, row_y;
#if defined(PBL_ROUND)
  // Round: the very bottom of the circle is too narrow for edge content, so centre a
  // [time · chip] cluster high in the bar where the display is still wide.
  int cluster = time_w + 7 + chip_w;
  time_x = (w - cluster) / 2;
  chip_x = time_x + time_w + 7;
  row_y = top + TRIM_H + 1;
#else
  // Rect: departure-board layout — time on the left, platform chip on the right.
  time_x = 9;
  chip_x = w - chip_w - 8;
  row_y = top + TRIM_H + 3;
#endif

  // Platform-style date chip: NS yellow with the day-of-month in NS blue.
  graphics_context_set_fill_color(ctx, NS_YELLOW);
  graphics_fill_rect(ctx, GRect(chip_x, row_y, chip_w, chip_h), 3, GCornersAll);
  graphics_context_set_text_color(ctx, NS_BLUE);
  graphics_draw_text(ctx, s_date, s_date_font, GRect(chip_x, row_y - 4, chip_w, chip_h + 4),
                     GTextOverflowModeFill, GTextAlignmentCenter, NULL);

  // Time, white, like the board's departure rows.
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, s_time, s_font, GRect(time_x, row_y - 9, time_w + 6, 40),
                     GTextOverflowModeFill, GTextAlignmentLeft, NULL);
}

// Once the current train and its trailing gap have fully passed, the next train
// becomes current and a fresh random train is queued behind it.
static void promote_if_passed(void) {
  int cw = gbitmap_get_bounds(s_current).size.w;
  if (s_pan >= cw + GAP) {
    s_pan -= (cw + GAP);
    gbitmap_destroy(s_current);
    s_current = s_next;
    s_current_idx = s_next_idx;
    s_next_idx = random_except(s_current_idx);
    s_next = gbitmap_create_with_resource(TRAIN_RESOURCE_IDS[s_next_idx]);
  }
}

static void start_glide(float to, int duration_ms, bool is_skip);

static void anim_cb(void *context) {
  s_anim_step++;
  float t = (float)s_anim_step / s_anim_steps;
  if (t > 1.0f) {
    t = 1.0f;
  }
  float eased = 1.0f - (1.0f - t) * (1.0f - t); // ease-out
  s_pan = s_anim_from + (s_anim_to - s_anim_from) * eased;
  layer_mark_dirty(s_layer);
  if (t >= 1.0f) {
    s_animating = false;
    s_timer = NULL;
    promote_if_passed();
    // A tap that arrived mid-skip is honoured now, so impatient double-taps work.
    if (s_skip_queued && s_current != NULL) {
      s_skip_queued = false;
      int cw = gbitmap_get_bounds(s_current).size.w;
      start_glide(cw + GAP, SKIP_MS, true);
    } else {
      layer_mark_dirty(s_layer);
    }
  } else {
    s_timer = app_timer_register(FRAME_MS, anim_cb, NULL);
  }
}

static void start_glide(float to, int duration_ms, bool is_skip) {
  s_anim_from = s_pan;
  s_anim_to = to;
  s_anim_steps = duration_ms / FRAME_MS;
  if (s_anim_steps < 1) {
    s_anim_steps = 1;
  }
  s_anim_step = 0;
  s_animating = true;
  s_is_skip = is_skip;
  if (s_timer) {
    app_timer_cancel(s_timer);
  }
  s_timer = app_timer_register(FRAME_MS, anim_cb, NULL);
}

static void set_time(struct tm *t) {
  strftime(s_time, sizeof(s_time), clock_is_24h_style() ? "%H:%M" : "%I:%M", t);
  strftime(s_date, sizeof(s_date), "%d", t);
}

static void tick_handler(struct tm *tick_time, TimeUnits units) {
  set_time(tick_time);
  // The first tick can arrive seconds after launch (it lands on the next minute
  // boundary). Don't drift on it, so the train stays at its head for the first
  // minute instead of jumping immediately; just refresh the clock.
  if (s_first_tick) {
    s_first_tick = false;
    layer_mark_dirty(s_layer);
    return;
  }
  if (s_animating) {
    layer_mark_dirty(s_layer);
  } else {
    start_glide(s_pan + DRIFT, DRIFT_MS, false);
  }
}

// A tap fast-forwards the rest of the current train, landing on the head of the
// next one (which then becomes current via promote_if_passed). A tap interrupts a
// minute drift, but is ignored while a skip is already in flight.
static void tap_handler(AccelAxisType axis, int32_t direction) {
  if (s_current == NULL) {
    return;
  }
  if (s_animating && s_is_skip) {
    s_skip_queued = true; // already skipping; run one more when this finishes
    return;
  }
  int cw = gbitmap_get_bounds(s_current).size.w;
  start_glide(cw + GAP, SKIP_MS, true);
}

static void init(void) {
  srand(time(NULL));
  s_font = fonts_get_system_font(PBL_IF_ROUND_ELSE(FONT_KEY_GOTHIC_24_BOLD, FONT_KEY_GOTHIC_28_BOLD));
  s_date_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  s_label_font = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);

  s_current_idx = rand() % TRAIN_COUNT;
  s_next_idx = random_except(s_current_idx);
  s_current = gbitmap_create_with_resource(TRAIN_RESOURCE_IDS[s_current_idx]);
  s_next = gbitmap_create_with_resource(TRAIN_RESOURCE_IDS[s_next_idx]);

  time_t now = time(NULL);
  set_time(localtime(&now));

  s_window = window_create();
  window_set_background_color(s_window, GColorFromRGB(85, 170, 255));
  Layer *root = window_get_root_layer(s_window);
  s_layer = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_layer, update_proc);
  layer_add_child(root, s_layer);
  window_stack_push(s_window, true);

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  accel_tap_service_subscribe(tap_handler);
}

static void deinit(void) {
  accel_tap_service_unsubscribe();
  tick_timer_service_unsubscribe();
  if (s_timer) {
    app_timer_cancel(s_timer);
  }
  if (s_current) {
    gbitmap_destroy(s_current);
  }
  if (s_next) {
    gbitmap_destroy(s_next);
  }
  layer_destroy(s_layer);
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
