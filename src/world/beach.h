// Beach: port of src/world/beach.js. Pale quartz sand with footprint-churned micro relief (baked
// with its own low-sun shadows), beach-cleaner rake lines, a lifeguard-truck tire track, the
// sargassum wrack line, dry -> damp -> wet sand, the swash sheet with lace foam riding the shared
// surf clock, dune plants by the wall, walkable lifeguard towers and a few props.
#pragma once

#include "gfx/scene.h"
#include "world/surf.h"

typedef struct BeachBox { V3 min, max; } BeachBox;
typedef struct Beach Beach;

extern const double ACCESS_Z[2];        // beach access through the seawall
extern const double MORE_ACCESS_Z[6];   // one more per block of the extended district

Beach *build_beach(Node *scene, Surf *surf);
// walkable surface height (deck / stairs when reachable from current_y, else the ground)
double beach_height_at(const Beach *b, double x, double z, double current_y);
double beach_ground_at(double x, double z);
const BeachBox *beach_colliders(const Beach *b, int *n);
void beach_update(Beach *b, double t, const Camera *camera);
