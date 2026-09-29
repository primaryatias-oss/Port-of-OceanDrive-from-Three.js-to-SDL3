// Birds of a Miami Beach sunrise: port of src/world/birds.js. Brown pelicans skimming the swell
// in single file, laughing and ring-billed gulls wheeling overhead and loafing on the sand,
// sanderlings chasing the swash, boat-tailed grackles on the sidewalks, a magnificent
// frigatebird high up and a distant line of cormorants crossing the sunrise.
//
// One InstancedMesh per species, posed in the vertex shader from three per-instance pose
// vectors the CPU behaviours write each frame; ground birds get planar-projected sun shadows
// (their own draw per species).
#pragma once

#include "gfx/scene.h"
#include "quality.h"
#include "world/surf.h"

typedef struct Birds Birds;

// a gull took off near the player (position and launch velocity, distance to the player)
typedef struct BirdFlutter { double x, y, z, vx, vy, vz, dist; } BirdFlutter;
// the gull voicing the next call: position and velocity
typedef struct BirdSource { double x, y, z, vx, vy, vz; } BirdSource;

Birds *create_birds(Node *scene, Surf *surf, bool shot);
void birds_update(Birds *b, double dt, const Camera *camera);
// audio hook: a visible gull (preferably flying) to voice the next call; false if none
bool birds_gull_source(Birds *b, V3 listener, BirdSource *out);
void birds_set_quality(Birds *b, Tier tier);
void birds_on_flutter(Birds *b, void (*fn)(const BirdFlutter *f, void *user), void *user);
