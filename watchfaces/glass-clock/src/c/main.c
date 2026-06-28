/**
 * Glass Clock — a native Pebble C watchface that raytraces the time as chunky
 * 7-segment glass digits (HH over MM) with refraction, reflection and randomised
 * coloured lighting, viewed obliquely with chamfered facets and cel-shaded
 * outlines.
 *
 * The render is progressive: each minute the accumulation restarts "grainy" and
 * develops toward a coherent image over the minute (Monte-Carlo sampling — see
 * render.c). An app_timer traces a small budget of samples per tick so the watch
 * stays responsive; the layer's update_proc blits the current accumulated mean.
 *
 * The digits ARE the glass geometry, so there is no 2D text drawing — the tick
 * handler just rebuilds the digit SDF (and picks a new mood/pattern/angle) when
 * the minute changes. Look options come from the phone config page (settings.h /
 * src/pkjs/index.js) over AppMessage and persist on the watch.
 */

#include <pebble.h>
#include "render.h"
#include "settings.h"

// Each tick traces only a small burst then RETURNS to the event loop, and the
// (expensive) full-screen blit runs only every few ticks — otherwise the heavy
// soft-float work starves the firmware (input + the screenshot RPC stop).
#define FRAME_MS 50          // idle gap between bursts
#define N_PIX_PER_TICK 150   // cursor steps per burst (background pixels are skipped
                             // cheaply, so this traces fewer glass samples than the
                             // old all-pixel 110 — modest convergence boost, lighter
                             // burst; raise further only after on-watch responsiveness
                             // testing)
#define BLIT_EVERY 6         // repaint every Nth tick
#define MAX_PASSES 96        // stop accumulating once converged (next minute restarts it)
#define PERSIST_SETTINGS 1

static Window *s_window;
static Layer *s_layer;
static AppTimer *s_timer;
static char s_time[5]; // "HHMM"
static int s_ticks;
static Settings s_settings;

static void apply_options(void) {
  render_set_options(s_settings.cel, s_settings.edge, s_settings.turn, s_settings.pattern,
                     s_settings.mood, s_settings.translucency);
}

#if GC_BENCH
static int s_bench_logged;
#endif

static void anim_cb(void *ctx) {
  s_timer = NULL;
  render_step(N_PIX_PER_TICK);
  if ((++s_ticks % BLIT_EVERY) == 0) layer_mark_dirty(s_layer);
#if GC_BENCH
  (void)s_bench_logged;
  if ((s_ticks % 80) == 0 && render_passes() >= 3) { // periodic; ratio stable after a few passes
    unsigned long sm = render_bench_samples(), ev = render_bench_evals();
    // evals/sample ×100 (integer, avoids float in the log).
    unsigned long eps100 = sm ? (ev * 100UL) / sm : 0;
    APP_LOG(APP_LOG_LEVEL_INFO,
            "BENCH p%d ticks=%d geom_evals=%lu shade_evals=%lu samples=%lu evals/sample(x100)=%lu heap=%d",
            render_passes(), s_ticks, render_bench_geom_evals(), ev, sm, eps100, (int)heap_bytes_free());
  }
#endif
  if (render_passes() < MAX_PASSES) {
    s_timer = app_timer_register(FRAME_MS, anim_cb, NULL);
  }
}

static void ensure_animating(void) {
  if (!s_timer) s_timer = app_timer_register(FRAME_MS, anim_cb, NULL);
}

static void update_proc(Layer *layer, GContext *ctx) {
  render_blit(ctx, layer_get_bounds(layer));
}

static void set_time(struct tm *t) {
  strftime(s_time, sizeof(s_time), clock_is_24h_style() ? "%H%M" : "%I%M", t);
}

static void tick_handler(struct tm *tick_time, TimeUnits units) {
  set_time(tick_time);
  render_restart(s_time); // new minute: rebuild digits, fresh mood/pattern/angle, reseed
  s_ticks = 0;
#if GC_BENCH
  s_bench_logged = 0;
#endif
  ensure_animating();
  layer_mark_dirty(s_layer);
}

static void restart_now(void) {
  time_t now = time(NULL);
  set_time(localtime(&now));
  apply_options();
  render_restart(s_time);
  ensure_animating();
  if (s_layer) layer_mark_dirty(s_layer);
}

// ---- settings ----

static void load_settings(void) {
  s_settings = SETTINGS_DEFAULTS;
  if (persist_exists(PERSIST_SETTINGS)) {
    persist_read_data(PERSIST_SETTINGS, &s_settings, sizeof(s_settings));
  }
}

static void inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *t;
  if ((t = dict_find(iter, MESSAGE_KEY_CEL_MODE))) s_settings.cel = (uint8_t)(t->value->int32 & 3);
  if ((t = dict_find(iter, MESSAGE_KEY_EDGE_STYLE)))
    s_settings.edge = (uint8_t)(t->value->int32 % 3);
  if ((t = dict_find(iter, MESSAGE_KEY_TURN))) s_settings.turn = (uint8_t)(t->value->int32 & 1);
  if ((t = dict_find(iter, MESSAGE_KEY_PATTERN)))
    s_settings.pattern = (uint8_t)(t->value->int32 & 1);
  if ((t = dict_find(iter, MESSAGE_KEY_MOOD))) s_settings.mood = (uint8_t)(t->value->int32 % 3);
  if ((t = dict_find(iter, MESSAGE_KEY_TRANSLUCENCY)))
    s_settings.translucency = (uint8_t)(t->value->int32 % 3);
  persist_write_data(PERSIST_SETTINGS, &s_settings, sizeof(s_settings));
  restart_now(); // re-render with the new look immediately
}

static void init(void) {
  srand(time(NULL));
  load_settings();

  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  Layer *root = window_get_root_layer(s_window);
  GRect bounds = layer_get_bounds(root);

  render_init(bounds);
  time_t now = time(NULL);
  set_time(localtime(&now));
  apply_options();
  render_restart(s_time);

  s_layer = layer_create(bounds);
  layer_set_update_proc(s_layer, update_proc);
  layer_add_child(root, s_layer);
  window_stack_push(s_window, true);

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  app_message_register_inbox_received(inbox_received);
  app_message_open(128, 16);
  ensure_animating();
}

static void deinit(void) {
  if (s_timer) app_timer_cancel(s_timer);
  tick_timer_service_unsubscribe();
  layer_destroy(s_layer);
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
