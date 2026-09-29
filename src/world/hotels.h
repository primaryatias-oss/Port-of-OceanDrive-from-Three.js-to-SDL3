// The Art Deco hotel row along Ocean Drive: port of src/world/hotels.js.
#pragma once

#include "gfx/scene.h"

// walk collision: each building's front line (patios in front are raised terraces)
typedef struct Footprint { double z0, z1, fx; } Footprint;
typedef struct Hotels {
  Node *group;
  Footprint *footprints;   // group.userData.footprints
  int nfootprints;
} Hotels;

Hotels build_hotels(Node *scene);
