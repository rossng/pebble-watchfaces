// Watchface settings: everything the phone config page can change, kept in one
// struct that main.c persists and hands to the renderer.
#pragma once
#include <pebble.h>

typedef enum {
  CEL_NONE = 0,     // no outline
  CEL_WHITE = 1,    // always-white outline
  CEL_BLACK = 2,    // always-black outline
  CEL_CONTRAST = 3, // white over dark, black over light (auto)
} CelMode;

typedef enum {
  EDGE_SHARP = 0,   // crisp 90-degree edges
  EDGE_BEVEL = 1,   // rounded bevel
  EDGE_CHAMFER = 2, // 45-degree chamfer facet
} EdgeStyle;

typedef enum {
  TURN_FLAT = 0,    // digits face the camera
  TURN_OBLIQUE = 1, // digits turned around the up axis (shows thickness)
} TurnMode;

typedef enum {
  MOOD_SURPRISE = 0, // random neon / jewel each minute
  MOOD_NEON = 1,     // saturated lights, vivid tint
  MOOD_JEWEL = 2,    // softer, pastel
} MoodMode;

typedef enum {
  TRANS_SOLID = 0,  // mostly opaque body colour
  TRANS_MEDIUM = 1, // some scene shows through
  TRANS_HIGH = 2,   // clearer glass, lots shows through
} TransLevel;

typedef struct {
  uint8_t cel;          // CelMode
  uint8_t edge;         // EdgeStyle
  uint8_t turn;         // TurnMode
  uint8_t pattern;      // 0 off, 1 on
  uint8_t mood;         // MoodMode
  uint8_t translucency; // TransLevel
} Settings;

#define SETTINGS_DEFAULTS                                                                   \
  ((Settings){.cel = CEL_CONTRAST, .edge = EDGE_CHAMFER, .turn = TURN_OBLIQUE,              \
              .pattern = 1, .mood = MOOD_SURPRISE, .translucency = TRANS_MEDIUM})
