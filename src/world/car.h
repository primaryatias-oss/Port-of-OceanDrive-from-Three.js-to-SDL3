// The parked hero convertible, the modern parked cars and the moving cars: port of
// src/world/car.js.
#pragma once

#include "gfx/scene.h"

typedef struct CarBox { V3 min, max; } CarBox;   // walk colliders

typedef struct CarPass { int id; bool active; double progress, x, z, dir, speed; } CarPass;   // audio car passes

typedef struct Cars Cars;
// env: scene.environment (the sky PMREM)
Cars *build_cars(Node *scene, Texture *env);
const CarBox *cars_colliders(const Cars *c, int *n);
Node *cars_hero(const Cars *c);
// the moving cars follow the audio engine's car passes (none in shot mode)
void cars_update(Cars *c, double dt, const CarPass *passes, int n);
