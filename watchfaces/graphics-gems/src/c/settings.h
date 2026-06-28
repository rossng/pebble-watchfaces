// Watchface settings: everything the phone config page can change, kept in one
// struct that main.c persists and hands to the renderer each frame.
#pragma once
#include <pebble.h>
#include "models.h"

typedef enum {
  RENDER_WIRE = 0,    // hidden-line wireframe (front faces only)
  RENDER_UNLIT = 1,   // flat solid fill, one colour (silhouette)
  RENDER_LAMBERT = 2, // per-face Lambert diffuse
  RENDER_PHONG = 3,   // per-face diffuse + Blinn-Phong specular
} RenderMode;

typedef enum {
  ROTATE_CONTINUOUS = 0, // always turning, with a wandering axis
  ROTATE_TAP = 1,        // still until a tap/shake spins it
} RotationMode;

typedef enum {
  TEXT_HORIZONTAL = 0, // HH:MM on one line, near full width
  TEXT_STACKED = 1,    // HH over MM, even larger
} TextLayout;

typedef enum {
  DATE_OFF = 0, // no date shown
  DATE_DMY = 1, // DD-MM (e.g. 12-05)
  DATE_MDY = 2, // MM-DD (e.g. 05-12)
} DateMode;

// MODEL_CYCLE picks a different model each time the face is shown / each hour,
// instead of pinning one.
#define MODEL_CYCLE 0xFF

typedef struct {
  RenderMode render_mode;
  RotationMode rotation_mode;
  uint8_t model_sel;       // index into GEM_MODELS, or MODEL_CYCLE
  bool translucent_text;
  TextLayout text_layout;
  bool jumble_on_shake;    // a shake randomises the model / mode / colours
  DateMode date_mode;      // small date line under/within the time (off by format)
} Settings;

#define SETTINGS_DEFAULTS \
  ((Settings){ .render_mode = RENDER_PHONG, .rotation_mode = ROTATE_CONTINUOUS, \
               .model_sel = MODEL_CYCLE, .translucent_text = true,             \
               .text_layout = TEXT_HORIZONTAL, .jumble_on_shake = false,       \
               .date_mode = DATE_DMY })
