// Rideable vehicles: port of src/vehicles/index.js. The beach cruiser parked by the promenade
// near the z = -30 seawall access and the lifeguard ATV by the main tower. Walk up to one and look
// at it: "E - ride". E again gets off (the vehicle brakes first if it is moving; the rider steps
// off beside it and it stays there). While riding, the walker is paused: this module reads the
// keys / touch stick, steps the physics (sim.c), poses the model, puts the camera at the rider's
// eye (mouse / drag look stays free, relative to the vehicle) and drives the ride sounds.
#pragma once

#include "audio/audio.h"
#include "player/walker.h"
#include "vehicles/sim.h"
#include "world/surf.h"

typedef struct Vehicles Vehicles;

typedef struct VehiclesOpts {
  Walker *walker;
  Camera *camera;
  Surf *surf;
  const WalkBox *static_boxes;
  int nstatic_boxes;
  const WalkCircle *static_circles;
  int nstatic_circles;
  const WalkCircle *const *dynamic_circles;   // the people
  int ndynamic;
  Audio *audio;
  bool shot;                                  // hidden and inert (?shot without &vehicles)
  bool *shadow_wanted;                        // requestShadow()
  const int *render_height;                   // renderer.getDrawingBufferSize().y (spray size)
} VehiclesOpts;

Vehicles *create_vehicles(Node *scene, VehiclesOpts o);
int vehicles_colliders(Vehicles *v, const WalkCircle **out, int max);   // live circles
bool vehicles_riding(const Vehicles *v);
void vehicles_update(Vehicles *v, double dt);
void vehicles_toggle(Vehicles *v);
void vehicles_handle_event(Vehicles *v, const SDL_Event *e);   // E: ride / get off
// the on-screen prompt: key ("E" or "") and text ("ride", "get off", "Stopping…", ...)
void vehicles_prompt(const Vehicles *v, const char **key, const char **text);
// the touch Ride button: 0 hidden, 1 "Ride" (a vehicle in reach), 2 "Off" (riding `kind`)
int vehicles_touch_mode(const Vehicles *v, VKind *kind);

// test hooks (as the JS api): mount by kind, place at a pose at rest, ride with held keys
bool vehicles_mount(Vehicles *v, VKind kind);
bool vehicles_place(Vehicles *v, double x, double z, double yaw, int kind);   // kind -1: the ridden one
void vehicles_simulate(Vehicles *v, const bool keys[WK_COUNT], double seconds, double dt, double *max_speed, double *max_bump);
bool vehicles_dismount(Vehicles *v);
const Vehicle *vehicles_current(const Vehicles *v);
const Vehicle *vehicles_get(const Vehicles *v, VKind kind);
