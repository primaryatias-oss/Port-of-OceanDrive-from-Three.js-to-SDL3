// Procedural vehicle models: port of src/vehicles/models.js. A pastel beach cruiser (fat
// whitewall balloon tyres, swept-back bars, cantilever frame, fenders, chain guard, sprung
// saddle, wicker basket, kickstand) and a red lifeguard ATV (knobby tyres, lofted body and
// fenders, floorboards, tubular racks and brush guard, light bar hoop with a whip flag, rescue
// can). Every moving part is one merged mesh with vertex colours and a per-vertex
// metalness / roughness / emission attribute (aMRE). Model local frame: forward = -z,
// right = +x, y up, origin on the ground.
#pragma once

#include "gfx/scene.h"

typedef struct AtvWheel { Node *hold, *mesh; double x, z; } AtvWheel;

typedef struct VehicleModel {
  Node *root, *tilt, *steer, *shadow;
  Material *mat;          // uLights / uBeaconA / uBeaconB
  double R;               // wheel radius
  // bike
  Node *wheel_f, *wheel_r, *crank, *pedals[2], *kick;
  // atv
  AtvWheel wheels[4];
} VehicleModel;

VehicleModel build_bike_model(void);
VehicleModel build_atv_model(void);
