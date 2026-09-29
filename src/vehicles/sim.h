// Vehicle physics shared by the beach cruiser and the lifeguard ATV: port of src/vehicles/sim.js.
// A kinematic bicycle model with tyre slip, per-surface speed caps and rolling resistance,
// sub-stepped circle-vs-box / circle-vs-circle collisions, wheel contact heights driving pitch /
// roll / a sprung body, and a hop.
//
// Conventions (same as the walker): +x east, -z north; yaw 0 faces north, forward is
// (-sin yaw, -cos yaw), right is (cos yaw, -sin yaw). Model local: forward = -z, right = +x.
#pragma once

#include "player/walker.h"
#include "world/surf.h"

typedef enum VKind { VK_BIKE, VK_ATV } VKind;
typedef enum VSurf { VS_PAVEMENT, VS_GRASS, VS_WETSAND, VS_SAND, VS_WATER } VSurf;

typedef struct VSpec {
  VKind kind;
  double wheelbase, track, wheel_r;
  int nwheels;
  double wheels[4][2];           // local x, z: front(s) then rear
  int ncircles;
  double circles[3][2];          // local z, radius
  double max_step, access_step, max_depth, x_min;
  double vmax[4][2];             // [pavement, grass, wetsand, sand][normal, hard] top speeds, m/s
  double roll[4];                // rolling resistance per surface (same order)
  double accel, accel_hard, brake, rev_accel, rev_max, engine_brake;
  double steer_max, steer_v, steer_rate, grip_hard, grip_soft;
  double water_k, eye[3];
  double idle, redline;
} VSpec;
extern const VSpec SPEC_BIKE, SPEC_ATV;

// a collider for the walker (and the other vehicle): the vehicle's body circles
typedef struct VCircle { WalkCircle c; const void *owner; } VCircle;

typedef struct VSurface { VSurf kind; double soft, depth; bool covered; } VSurface;

typedef struct Vehicle {
  VKind kind;
  const VSpec *spec;
  double x, z, yaw;
  double vx, vz, lon, steer, lean, lean_v;
  double wheel_h[4], ground_y;
  double body_y, body_v, pitch, pitch_v, roll, roll_v, pitch_t, roll_t, base_t;
  double air_y, vy_air;
  bool grounded;
  double crank, wheel_rot, pedal, rpm, throttle, load;
  double boost_t, bump, land, t, turn;
  bool parked, ridden, coasting;
  VSurface surf;
  double wheel_depth[4];
  VCircle circles[3];
  double sx_, sz_, yaw_;         // surface cache position (_sx / _sz), last yaw (_yaw): NAN = unset
  int sf_;                       // _sf: -1 = unset
} Vehicle;

typedef struct VGrid VGrid;
// static colliders (boxes too large to be walls are skipped, like the JS), 4 m cells
VGrid *vgrid_build(const WalkBox *boxes, int nboxes, const WalkCircle *circles, int ncircles);

// world: ground / water / swash, the walk bounds, the static grid, the dynamic circles
typedef struct VDyn { const WalkCircle *c; const void *owner; } VDyn;
typedef struct VWorld {
  Surf *surf;                    // water depth and swash at the surf clock's time
  double bx0, bx1, bz0, bz1;     // walker.world.bounds
  VGrid *grid;
  const VDyn *dyn;
  int ndyn;
} VWorld;

typedef struct VInput { double throttle, steer; bool hard, hop; } VInput;

void vehicle_init(Vehicle *v, VKind kind, double x, double z, double yaw, const VWorld *w);
void vehicle_settle(Vehicle *v, const VWorld *w);            // drop onto the ground at its pose
bool vehicle_blocked(Vehicle *v, double x, double z, double yaw, const VWorld *w, double air_y);
void vehicle_step(Vehicle *v, VInput input, double dt, const VWorld *w);
void vehicle_sync_circles(Vehicle *v);
// where the rider steps off: 8 candidate [x, z] spots
void vehicle_dismount_spots(const Vehicle *v, double out[8][2]);
double vehicle_water_depth(const VWorld *w, double x, double z);
