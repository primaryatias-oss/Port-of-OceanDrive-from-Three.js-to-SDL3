// Ocean Drive street: port of src/world/street.js.
#pragma once

#include "gfx/scene.h"

// Walk colliders for street furniture: { x, z, r } circles or { min, max } boxes.
typedef struct StreetCollider { bool box; double x, z, r; V3 min, max; } StreetCollider;
const StreetCollider *street_colliders(int *n);

Node *build_street(Node *scene);
