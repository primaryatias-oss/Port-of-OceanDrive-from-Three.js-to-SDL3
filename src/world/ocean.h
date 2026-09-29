// Water surface to the real horizon: port of src/world/ocean.js. A camera-centred polar grid
// (dense at the feet, rings growing geometrically to 25 km); the Gerstner swell, shore-break
// crests, sky reflection, glitter and foam live in the captured program (shaders/custom/*/ocean.*).
#pragma once

#include "gfx/scene.h"
#include "world/surf.h"

typedef struct Ocean Ocean;

Ocean *create_ocean(Node *scene, Surf *surf);
Node *ocean_mesh(const Ocean *o);
void ocean_update(Ocean *o, double time, const Camera *camera);
