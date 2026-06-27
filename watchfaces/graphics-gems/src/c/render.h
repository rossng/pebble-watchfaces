// The software rasterizer: turns a model + a rotation matrix into pixels, with
// the time floating in the middle of the scene.
#pragma once
#include <pebble.h>
#include "settings.h"
#include "models.h"

typedef struct {
  RenderMode mode;
  bool translucent_text;
  TextLayout text_layout;
  float rot[9];       // row-major 3x3 model rotation
  float grad_travel;  // gradient band phase — advance to make bands travel the mesh
  float grad_hue;     // base hue 0..1 — advance to drift the gradient's colours
  float text_front;   // text plane this far in front of the model centre (per-model)
  float fill;         // silhouette size: bounding radius as a fraction of the short axis (>1 spills off-screen)
} RenderConfig;

// Load fonts once. Call from app init.
void render_init(void);
void render_deinit(void);

// Draw one frame: the model under `cfg`, with `time_str` as large 2D text
// floating just in front of the model centre (so the model pierces it).
void render_scene(GContext *ctx, GRect bounds, const GemModel *model,
                  const RenderConfig *cfg, const char *time_str);
