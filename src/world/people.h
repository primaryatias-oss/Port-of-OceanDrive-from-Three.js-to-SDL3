// A few people on Ocean Drive at sunrise: port of src/world/people.js. A jogger on the park
// promenade, a barefoot beach walker at the waterline (sandals in hand), a cafe worker on one
// hotel terrace, a cyclist on a city bike passing now and then, and a small stroller far up the
// beach. Each figure is one SkinnedMesh (lofted body, clothing as vertex colour + per-vertex
// material zones; the cyclist's bike too) plus a second SkinnedMesh on the same skeleton that
// projects the figure along the sun onto the ground: its long 7 degree shadow. Animation is
// procedural (gait curves, two-bone IK for pedalling, handlebars and wiping).
#pragma once

#include "gfx/scene.h"
#include "player/walker.h"
#include "world/beach.h"
#include "world/car.h"
#include "world/hotels.h"

typedef struct People People;

// get_cars: the audio car passes (the cyclist waits for a clear road); may be null
// mode: "closeup" (with shot) places everyone at fixed close-up spots
People *build_people(Node *scene, const Hotels *hotels, Walker *walker, bool shot, const char *mode,
                     int (*get_cars)(void *user, CarPass *out, int max), void *cars_user);
// live walk colliders (pointers stay valid): the walker's moving circles
int people_colliders(People *p, const WalkCircle **out, int max);
void people_update(People *p, double dt, const Camera *camera);
