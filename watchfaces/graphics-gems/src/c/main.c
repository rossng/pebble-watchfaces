/**
 * Graphics Gems — a native Pebble C watchface that renders classic
 * computer-graphics research models (the Utah teapot, the Stanford bunny, an
 * icosphere and a torus knot) with a tiny software rasterizer (see render.c),
 * the time floating front-and-centre in the 3D scene.
 *
 * Settings come from the phone config page (src/pkjs/index.js) over AppMessage
 * and persist on the watch: render mode (wireframe / unlit / Lambert / Phong),
 * rotation (continuous tumble or tap-to-spin), model (a specific gem or cycle),
 * time layout (wide HH:MM or stacked), translucent time, and jumble-on-shake.
 *
 * Orientation is three Euler angles advanced by an angular-velocity vector whose
 * speed (s_speed) relaxes toward the mode's resting speed and whose axis wanders
 * via a low-passed random torque. A tap/shake boosts the speed — and, with
 * jumble on, reshuffles the model / mode / colours — then it eases back to the
 * normal spin (continuous) or to a standstill (static mode does no animation at
 * rest, to save battery). Colour and text live in render.c; the gradient phases
 * are advanced here alongside the rotation.
 */

#include <pebble.h>
#include "render.h"
#include "settings.h"
#include "models.h"
#include "fastmath.h"

#define FRAME_MS 80          // ~12 fps — leaves the CPU idle between frames (no FPU)
#define ROT_SPEED 0.017f     // the normal continuous angular speed (rad/frame)
#define TORQUE_DAMP 0.97f    // low-pass on the random steering, so the axis drifts smoothly
#define TORQUE_KICK 0.00003f // how hard the steering is nudged each frame
#define TAP_SPEED 0.090f     // speed a (non-jumble) tap/shake spins up to (~5x normal)
#define JUMBLE_SPEED 0.150f  // peak spin a jumble shake winds up to before the swap
#define BOOST_RELAX 0.13f    // faster approach while spinning up to the jumble peak
#define SPEED_RELAX 0.07f    // per-frame approach back toward the mode's resting speed (~2s)
#define SETTLE_SPEED 0.008f  // below this, static mode stops animating (settled)
#define GRAD_TRAVEL_RATE 0.0010f // gradient band travel per frame
#define GRAD_HUE_RATE 0.00018f   // gradient hue drift per frame
#define PERSIST_SETTINGS 1

static Window *s_window;
static Layer *s_layer;

static Settings s_settings;
static char s_time[6]; // "HH:MM"

// Orientation, the angular-velocity axis (s_v*, wandered by the low-passed
// torque s_t*), and the current spin speed (s_speed) which relaxes toward the
// mode's resting speed after a tap boosts it.
static float s_ax, s_ay, s_az;
static float s_vx, s_vy, s_vz;
static float s_tx, s_ty, s_tz;
static float s_speed;

// Gradient animation phases (band travel + hue drift), advanced over time.
static float s_grad_travel, s_grad_hue;

static AppTimer *s_timer;
static uint8_t s_cycle_idx;    // which model "cycle" is currently showing
static uint8_t s_jumble_model; // when jumble is on, the shake-chosen model...
static uint8_t s_jumble_mode;  // ...and render mode
static bool s_jumble_pending;  // a jumble shake is winding up; swap at peak speed

static float frand(void) { return (float)rand() / (float)RAND_MAX; } // [0,1]
static float frand2(void) { return frand() * 2.0f - 1.0f; }          // [-1,1]

// When jumble-on-shake is enabled, the live model/mode come from the last shake
// instead of the configured settings.
static uint8_t active_model(void) {
  if (s_settings.jumble_on_shake) return s_jumble_model % GEM_MODEL_COUNT;
  if (s_settings.model_sel == MODEL_CYCLE) return s_cycle_idx % GEM_MODEL_COUNT;
  return s_settings.model_sel % GEM_MODEL_COUNT;
}

static RenderMode active_mode(void) {
  return s_settings.jumble_on_shake ? (RenderMode)(s_jumble_mode % 4) : s_settings.render_mode;
}

static float resting_speed(void) {
  return (s_settings.rotation_mode == ROTATE_CONTINUOUS) ? ROT_SPEED : 0.0f;
}

// How far in front of each model's centre the text plane sits. A vertex pierces
// the digits when its rotated z exceeds this, so over a tumble the model pierces
// whenever its reach toward the camera, h(r), exceeds the value. We want a *bit*
// of z-fighting for every model (it's the aesthetic), so each is set near its
// ~30th-percentile reach (measured by `node scripts/preview-orient.mjs stats`),
// giving piercing in ~70% of orientations by model-appropriate amounts — the
// near-spherical icosphere just kept a touch further back so its rim only grazes
// the digits rather than ploughing through. Order matches GEM_MODELS.
static const float MODEL_TEXT_FRONT[GEM_MODEL_COUNT] = {0.60f, 0.58f, 0.87f, 0.68f};

// Silhouette size per model (bounding radius as a fraction of the short screen
// axis). >1.0 means the model's long axis spills off-screen in some
// orientations. The icosphere fills its sphere in every orientation, so it's
// kept under 1 to stay fully visible; the rest are sized to clip the edges
// occasionally. Order matches GEM_MODELS.
static const float MODEL_FILL[GEM_MODEL_COUNT] = {1.10f, 1.12f, 0.78f, 1.00f};

// A flattering starting orientation per model (Euler ax/ay/az, radians), so each
// gem first appears clearly rather than edge-on or upside down. Chosen with
// scripts/preview-orient.mjs; order matches GEM_MODELS.
//   - Teapot:    side-on (spout + handle), tilted slightly down onto the lid.
//   - Bunny:     side profile, ears up.
//   - Icosphere: symmetric — any orientation.
//   - Torus knot: the trefoil read face-on.
static const float MODEL_START[GEM_MODEL_COUNT][3] = {
    {0.20f, 3.14159f, 0.0f},
    {0.12f, 0.0f, 0.0f},
    {0.0f, 0.0f, 0.0f},
    {0.10f, 0.0f, 0.0f},
};

// Snap the model to its flattering start orientation, spinning at the mode's
// resting speed (normal in continuous, still in static).
static void reset_orientation(void) {
  uint8_t mi = active_model();
  s_ax = MODEL_START[mi][0];
  s_ay = MODEL_START[mi][1];
  s_az = MODEL_START[mi][2];
  s_vx = s_vy = s_vz = 0.0f;
  s_tx = s_ty = s_tz = 0.0f;
  s_speed = resting_speed();
}

// Build a row-major XYZ rotation matrix from the current Euler angles.
static void mat3(float *o, const float *A, const float *B) {
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      o[i * 3 + j] = A[i * 3] * B[j] + A[i * 3 + 1] * B[3 + j] + A[i * 3 + 2] * B[6 + j];
}

static void build_rot(float ax, float ay, float az, float *o) {
  float cx = fast_cos(ax), sx = fast_sin(ax), cy = fast_cos(ay), sy = fast_sin(ay),
        cz = fast_cos(az), sz = fast_sin(az);
  float rx[9] = {1, 0, 0, 0, cx, -sx, 0, sx, cx};
  float ry[9] = {cy, 0, sy, 0, 1, 0, -sy, 0, cy};
  float rz[9] = {cz, -sz, 0, sz, cz, 0, 0, 0, 1};
  float t[9];
  mat3(t, ry, rx);
  mat3(o, rz, t);
}

// Pick a fresh random model (never the current one), render mode, and gradient
// colours — done at the peak of a jumble shake so the swap is hidden in motion.
static void jumble_swap(void) {
  uint8_t prev = s_jumble_model;
  uint8_t m = prev;
  if (GEM_MODEL_COUNT > 1) {
    do {
      m = rand() % GEM_MODEL_COUNT;
    } while (m == prev);
  }
  s_jumble_model = m;
  s_jumble_mode = rand() % 4;
  s_grad_hue = frand();
  s_grad_travel = frand() * 10.0f;
}

// One animation step. The spin speed eases toward a target — normally the mode's
// resting speed (so a tap boost slows back to normal, or to a standstill in
// static mode), but a jumble shake winds it up to JUMBLE_SPEED and swaps the
// model at the peak, after which it eases back down. The axis drifts via a
// low-passed random torque. In static mode, once it settles we stop the timer.
static void anim_cb(void *ctx) {
  s_timer = NULL;

  float target = s_jumble_pending ? JUMBLE_SPEED : resting_speed();
  float relax = s_jumble_pending ? BOOST_RELAX : SPEED_RELAX;
  s_speed += (target - s_speed) * relax;
  if (s_jumble_pending && s_speed >= JUMBLE_SPEED * 0.9f) {
    jumble_swap(); // swap at the top of the spin, then ease back down
    s_jumble_pending = false;
  }

  // Wander the axis, then renormalise the velocity vector to the current speed.
  s_tx = s_tx * TORQUE_DAMP + frand2() * TORQUE_KICK;
  s_ty = s_ty * TORQUE_DAMP + frand2() * TORQUE_KICK;
  s_tz = s_tz * TORQUE_DAMP + frand2() * TORQUE_KICK;
  s_vx += s_tx; s_vy += s_ty; s_vz += s_tz;
  float len = fast_sqrt(s_vx * s_vx + s_vy * s_vy + s_vz * s_vz);
  if (len > 1e-5f) {
    float k = s_speed / len;
    s_vx *= k; s_vy *= k; s_vz *= k;
  } else {
    s_vy = s_speed;
  }

  s_ax += s_vx; s_ay += s_vy; s_az += s_vz;
  s_grad_travel += GRAD_TRAVEL_RATE;
  s_grad_hue += GRAD_HUE_RATE;
  layer_mark_dirty(s_layer);

  // Continuous always keeps going; static stops once it has slowed to a settle
  // (but not mid-jumble, while it's still winding up to the swap).
  bool keep_going = (s_settings.rotation_mode == ROTATE_CONTINUOUS) || s_jumble_pending ||
                    (s_speed > SETTLE_SPEED);
  if (keep_going) {
    s_timer = app_timer_register(FRAME_MS, anim_cb, NULL);
  } else {
    s_speed = 0.0f;
    s_vx = s_vy = s_vz = 0.0f;
  }
}

static void ensure_animating(void) {
  if (!s_timer) s_timer = app_timer_register(FRAME_MS, anim_cb, NULL);
}

// Start/stop the animation loop to match the current rotation mode.
static void apply_rotation_mode(void) {
  if (s_settings.rotation_mode == ROTATE_CONTINUOUS) {
    s_speed = ROT_SPEED;
    ensure_animating();
  } else {
    // Static: snap to rest and stop animating (the next tap wakes it).
    s_speed = 0.0f;
    if (s_timer) {
      app_timer_cancel(s_timer);
      s_timer = NULL;
    }
  }
}

static void update_proc(Layer *layer, GContext *ctx) {
  uint8_t mi = active_model();
  RenderConfig cfg;
  cfg.mode = active_mode();
  cfg.translucent_text = s_settings.translucent_text;
  cfg.text_layout = s_settings.text_layout;
  cfg.grad_travel = s_grad_travel;
  cfg.grad_hue = s_grad_hue;
  cfg.text_front = MODEL_TEXT_FRONT[mi];
  cfg.fill = MODEL_FILL[mi];
  build_rot(s_ax, s_ay, s_az, cfg.rot);
  render_scene(ctx, layer_get_bounds(layer), &GEM_MODELS[mi], &cfg, s_time);
}

static void set_time(struct tm *t) {
  strftime(s_time, sizeof(s_time), clock_is_24h_style() ? "%H:%M" : "%I:%M", t);
}

static void tick_handler(struct tm *tick_time, TimeUnits units) {
  set_time(tick_time);
  if (s_settings.model_sel == MODEL_CYCLE && (units & HOUR_UNIT)) {
    s_cycle_idx = (s_cycle_idx + 1) % GEM_MODEL_COUNT;
    reset_orientation(); // the new model appears in its flattering pose
    apply_rotation_mode();
  }
  // When idle (tap mode at rest) there's no animation timer, so nudge the
  // gradient here too — the colours still drift, just minute by minute.
  if (!s_timer) {
    s_grad_travel += 0.18f;
    s_grad_hue += 0.05f;
  }
  layer_mark_dirty(s_layer);
}

// A tap/shake spins the model in a fresh random direction, then relaxes back to
// the mode's resting speed (continuous) or a standstill (static). Plain shakes
// boost instantly; a jumble shake instead winds *up* to JUMBLE_SPEED and swaps
// the model (to a different one) at the peak — see anim_cb.
static void tap_handler(AccelAxisType axis, int32_t direction) {
  // A large random nudge re-aims the axis (renormalised to s_speed each frame).
  s_vx += frand2();
  s_vy += frand2();
  s_vz += frand2();

  if (s_settings.jumble_on_shake) {
    s_jumble_pending = true; // wind up; the swap happens at peak speed
  } else {
    s_speed = TAP_SPEED; // instant boost, eases back down
  }
  ensure_animating();
}

// ---- settings ----

static void load_settings(void) {
  s_settings = SETTINGS_DEFAULTS;
  if (persist_exists(PERSIST_SETTINGS)) {
    persist_read_data(PERSIST_SETTINGS, &s_settings, sizeof(s_settings));
  }
}

static void save_settings(void) {
  persist_write_data(PERSIST_SETTINGS, &s_settings, sizeof(s_settings));
}

static void inbox_received(DictionaryIterator *iter, void *context) {
  uint8_t prev_model = active_model();
  Tuple *t;
  if ((t = dict_find(iter, MESSAGE_KEY_RENDER_MODE)))
    s_settings.render_mode = (RenderMode)(t->value->int32 % 4);
  if ((t = dict_find(iter, MESSAGE_KEY_ROTATION_MODE)))
    s_settings.rotation_mode = (RotationMode)(t->value->int32 & 1);
  if ((t = dict_find(iter, MESSAGE_KEY_MODEL_SEL)))
    s_settings.model_sel = (uint8_t)t->value->int32;
  if ((t = dict_find(iter, MESSAGE_KEY_TRANSLUCENT_TEXT)))
    s_settings.translucent_text = t->value->int32 != 0;
  if ((t = dict_find(iter, MESSAGE_KEY_TEXT_LAYOUT)))
    s_settings.text_layout = (TextLayout)(t->value->int32 & 1);
  if ((t = dict_find(iter, MESSAGE_KEY_JUMBLE)))
    s_settings.jumble_on_shake = t->value->int32 != 0;
  save_settings();
  if (active_model() != prev_model) reset_orientation(); // show the new pick clearly
  apply_rotation_mode();
  layer_mark_dirty(s_layer);
}

static void init(void) {
  srand(time(NULL));
  load_settings();
  render_init();

  s_cycle_idx = rand() % GEM_MODEL_COUNT;
  s_jumble_model = rand() % GEM_MODEL_COUNT; // initial jumble pick (used if jumble is on)
  s_jumble_mode = rand() % 4;
  reset_orientation();                  // start in the model's flattering pose
  s_grad_hue = frand(); // start the gradient on a random hue, not always red

  time_t now = time(NULL);
  set_time(localtime(&now));

  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  Layer *root = window_get_root_layer(s_window);
  s_layer = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_layer, update_proc);
  layer_add_child(root, s_layer);
  window_stack_push(s_window, true);

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  accel_tap_service_subscribe(tap_handler);

  app_message_register_inbox_received(inbox_received);
  app_message_open(128, 16);

  apply_rotation_mode();
}

static void deinit(void) {
  if (s_timer) app_timer_cancel(s_timer);
  accel_tap_service_unsubscribe();
  tick_timer_service_unsubscribe();
  render_deinit();
  layer_destroy(s_layer);
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
