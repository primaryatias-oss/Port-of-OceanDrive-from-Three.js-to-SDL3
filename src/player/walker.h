// First-person walker: port of src/player/walker.js. Mouse look (SDL relative mouse mode in place
// of pointer lock, drag-to-look fallback), WASD with smooth acceleration, ground following over
// curbs, sand, seawall steps and the lifeguard tower stairs, circle-vs-box/circle collisions,
// head bob and sway synced to the step cycle, Space to jump (collisions stay at ground level;
// soft landing dip).
#pragma once

#include <SDL3/SDL.h>

#include "core/vec.h"
#include "gfx/scene.h"

typedef struct WalkBox { V3 min, max; } WalkBox;
typedef struct WalkCircle { double x, z, r; } WalkCircle;

// world: heightAt(x, z, currentY), boxes, circles (fixed, plus moving ones read through
// pointers: people, vehicles), bounds with soft z ends
typedef struct WalkWorld {
  double (*height_at)(void *user, double x, double z, double current_y);
  void *user;
  Vec(WalkBox) boxes;
  Vec(WalkCircle) circles;
  Vec(const WalkCircle *) moving;
  struct { double x0, x1, z0, z1, soft; } bounds;
} WalkWorld;

typedef enum WalkKey {
  WK_W, WK_S, WK_A, WK_D, WK_UP, WK_DOWN, WK_LEFT, WK_RIGHT, WK_SHIFT_L, WK_SHIFT_R, WK_SPACE, WK_COUNT
} WalkKey;

typedef struct WalkStep { double x, z, feet_y, speed; bool land; double impact; } WalkStep;

typedef struct Walker {
  Camera *camera;
  WalkWorld *world;
  double yaw, pitch;
  V2 pos;                       // feet x, z
  double feet_y;
  V2 vel;
  bool keys[WK_COUNT];
  const bool *virtual_keys;     // set by walker_simulate()
  bool has_stick;
  V2 stick;                     // { x: strafe, y: forward } in [-1, 1], touch joystick
  bool locked;
  bool active;                  // walking enabled (locked, or drag mode)
  double step_dist;
  double phase;                 // step cycle (radians, pi per step)
  double bob_amp;
  void (*on_step)(const WalkStep *s, void *user);
  void *step_user;
  bool drag_look, dragging;
  double air_y;                 // feet above the followed ground during a jump
  double vy;
  bool jump_req;
  double dip, dip_v;            // landing dip of the head (spring)
} Walker;

void walker_init(Walker *w, Camera *camera, WalkWorld *world);
// mouse capture (pointer lock); falls back to drag-to-look when relative mode is refused
void walker_lock(Walker *w, SDL_Window *win);
void walker_unlock(Walker *w, SDL_Window *win);
// keyboard / mouse events (key state, look)
void walker_handle_event(Walker *w, const SDL_Event *e);
// touch input: look by a drag delta (radians), queue a jump
void walker_look(Walker *w, double d_yaw, double d_pitch);
void walker_jump(Walker *w);
// harness / teleport. Compass heading: 0 = north (-z), 90 = east (+x). y = eye height.
void walker_set(Walker *w, double x, double y, double z, double heading_deg, double pitch_deg);
// put the walker on the walkable surface at (x, z) (current_y picks deck vs ground)
void walker_teleport(Walker *w, double x, double z, double heading_deg, double pitch_deg, double current_y);
double walker_heading(const Walker *w);
// would the walker's circle at (x, z) collide (bounds, ledges, water, circles, boxes)?
bool walker_blocked(const Walker *w, double x, double z);
void walker_update(Walker *w, double dt);
// test hook: hold `keys` for `seconds` of simulated time (fixed dt)
void walker_simulate(Walker *w, const bool keys[WK_COUNT], double seconds, double dt);
