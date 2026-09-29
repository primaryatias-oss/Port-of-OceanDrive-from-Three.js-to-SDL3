// Footsteps: port of src/audio/footsteps.js. Heel strike + toe roll-off built from filtered noise
// bursts, grains and short tonal thumps. Every parameter is randomized per step; steps alternate
// slightly L/R. And the ground surface lookup (port of src/audio/surface.js).
#pragma once

#include "audio/engine.h"

typedef enum Surface { SURF_PAVEMENT, SURF_GRASS, SURF_SAND, SURF_WETSAND, SURF_WOOD, SURF_SPLASH } Surface;

typedef struct Footsteps Footsteps;
Footsteps *create_footsteps(AudioEnv *env);
// at: NAN = now; depth: water depth (m) for SURF_SPLASH (NAN: the JS default 0.05)
void footsteps_step(Footsteps *f, Surface s, double gain, double at, double depth);

// y = feet height (m); waterline_x NAN: SAND.waterline. Pavement / grass / sand / wetsand / wood.
Surface surface_at(double x, double z, double y, double waterline_x);
