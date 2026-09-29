// Distance culling for the extended district: port of src/world/lod.js. Per-block objects are
// shown only while the viewer is within a tier-dependent distance of their z range (frustum
// culling handles the rest). Objects register with a kind:
//   LOD_DETAIL - small parts of a block (window frames, furniture, patios, street furniture)
//   LOD_CARS   - parked-car rows
// plus update hooks (palm crown LOD). lod_update(camera) returns true when something changed.
#pragma once

#include "gfx/scene.h"

typedef struct LodRadius { double detail, cars, palmNear; } LodRadius;
typedef enum LodKind { LOD_DETAIL, LOD_CARS } LodKind;

// the radii for the current quality tier (QUALITY must be initialised)
LodRadius lod_radius(void);
void lod_register(Node *obj, double z0, double z1, LodKind kind);
// fn(cameraPosition, user) -> true if it changed anything
void lod_register_hook(bool (*fn)(V3 cam, void *user), void *user);
bool lod_update(const Camera *camera);
