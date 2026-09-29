// Port of src/player/walker.js. The browser's pointer-lock plumbing (lock promises, Chrome's
// re-lock delay and retries) has no SDL counterpart: SDL_SetWindowRelativeMouseMode either
// captures the mouse or fails, and a failure switches to drag-to-look like three refusals do in
// the JS.
#include "player/walker.h"

#include <math.h>

#include "world/layout.h"

static constexpr double WALK = 1.4;              // m/s
static constexpr double STROLL = 2.6;
static constexpr double RADIUS = 0.3;
static constexpr double STEP_UP = 0.46;          // highest ledge that can be stepped onto
static constexpr double MAX_WADE = 0.5;          // deepest water the walker goes into
static constexpr double BOB = 0.025;             // m, vertical head bob at walking pace
static constexpr double GRAVITY = 9.8;
static constexpr double DIP_K = 120;             // landing dip spring stiffness (1/s^2)
static double jump_v(void) { return sqrt(2 * GRAVITY * 0.45); }   // 0.45 m apex

static bool key1(const Walker *w, WalkKey k) { return (w->virtual_keys ? w->virtual_keys : w->keys)[k]; }
static bool key2(const Walker *w, WalkKey a, WalkKey b) { return key1(w, a) || key1(w, b); }

void walker_init(Walker *w, Camera *camera, WalkWorld *world) {
  *w = (Walker){ .camera = camera, .world = world };
  camera->node->rotation.order = EULER_YXZ;
}

void walker_lock(Walker *w, SDL_Window *win) {
  if (w->locked) return;
  if (SDL_SetWindowRelativeMouseMode(win, true)) {
    w->locked = true;
    w->drag_look = false;
  } else {
    w->drag_look = true;   // walk anyway, look by dragging
  }
}

void walker_unlock(Walker *w, SDL_Window *win) {
  if (!w->locked) return;
  SDL_SetWindowRelativeMouseMode(win, false);
  w->locked = false;
}

static int key_of(SDL_Scancode s) {
  switch (s) {
    case SDL_SCANCODE_W: return WK_W;
    case SDL_SCANCODE_S: return WK_S;
    case SDL_SCANCODE_A: return WK_A;
    case SDL_SCANCODE_D: return WK_D;
    case SDL_SCANCODE_UP: return WK_UP;
    case SDL_SCANCODE_DOWN: return WK_DOWN;
    case SDL_SCANCODE_LEFT: return WK_LEFT;
    case SDL_SCANCODE_RIGHT: return WK_RIGHT;
    case SDL_SCANCODE_LSHIFT: return WK_SHIFT_L;
    case SDL_SCANCODE_RSHIFT: return WK_SHIFT_R;
    case SDL_SCANCODE_SPACE: return WK_SPACE;
    default: return -1;
  }
}

void walker_handle_event(Walker *w, const SDL_Event *e) {
  switch (e->type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN: w->dragging = true; break;
    case SDL_EVENT_MOUSE_BUTTON_UP: w->dragging = false; break;
    case SDL_EVENT_MOUSE_MOTION: {
      if (!w->locked && !(w->drag_look && w->dragging)) return;
      // (the JS also drops warps; a relative-mode jump that large is not mouse movement either)
      if (w->locked && (fabs(e->motion.xrel) > 400 || fabs(e->motion.yrel) > 400)) return;
      double k = w->locked ? 0.0021 : 0.004;
      w->yaw -= e->motion.xrel * k;
      w->pitch = clampd(w->pitch - e->motion.yrel * k, -1.48, 1.48);   // +-85 deg
      break;
    }
    case SDL_EVENT_KEY_DOWN: {
      int k = key_of(e->key.scancode);
      if (k < 0) return;
      w->keys[k] = true;
      if (k == WK_SPACE && !e->key.repeat && w->active) w->jump_req = true;
      break;
    }
    case SDL_EVENT_KEY_UP: {
      int k = key_of(e->key.scancode);
      if (k >= 0) w->keys[k] = false;
      break;
    }
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      for (int i = 0; i < WK_COUNT; i++) w->keys[i] = false;
      break;
    default: break;
  }
}

void walker_look(Walker *w, double d_yaw, double d_pitch) {
  w->yaw -= d_yaw;
  w->pitch = clampd(w->pitch - d_pitch, -1.48, 1.48);
}

void walker_jump(Walker *w) {
  if (w->active) w->jump_req = true;
}

void walker_set(Walker *w, double x, double y, double z, double heading_deg, double pitch_deg) {
  w->pos = v2(x, z);
  w->feet_y = y - EYE_HEIGHT;
  w->vel = v2(0, 0);
  w->yaw = -(heading_deg * DEG2RAD);
  w->pitch = pitch_deg * DEG2RAD;
  w->bob_amp = 0;
  w->air_y = 0; w->vy = 0; w->dip = 0; w->dip_v = 0; w->jump_req = false;
  w->camera->node->position = v3(x, y, z);
  node_set_euler(w->camera->node, euler(w->pitch, w->yaw, 0, EULER_YXZ));
}

void walker_teleport(Walker *w, double x, double z, double heading_deg, double pitch_deg, double current_y) {
  double g = w->world->height_at(w->world->user, x, z, current_y);
  walker_set(w, x, g + EYE_HEIGHT, z, heading_deg, pitch_deg);
}

double walker_heading(const Walker *w) { return -(w->yaw * RAD2DEG); }

static bool blocked(const Walker *w, double x, double z) {
  const WalkWorld *W = w->world;
  if (x < W->bounds.x0 || x > W->bounds.x1 || z < W->bounds.z0 || z > W->bounds.z1) return true;
  double g = W->height_at(W->user, x, z, w->feet_y);
  if (g - w->feet_y > STEP_UP) return true;
  if (SEA_LEVEL - g > MAX_WADE) return true;
  double feet = w->feet_y, head = feet + 1.8;
  for (size_t i = 0; i < W->circles.len + W->moving.len; i++) {
    const WalkCircle *c = i < W->circles.len ? &W->circles.data[i] : W->moving.data[i - W->circles.len];
    double dx = x - c->x, dz = z - c->z, rr = RADIUS + c->r;
    if (dx * dx + dz * dz < rr * rr) return true;
  }
  for (size_t i = 0; i < W->boxes.len; i++) {
    const WalkBox *q = &W->boxes.data[i];
    if (q->max.y < feet + 0.05 || q->min.y > head) continue;   // below the feet, or overhead
    double cx = fmax(q->min.x, fmin(x, q->max.x)), cz = fmax(q->min.z, fmin(z, q->max.z));
    double dx = x - cx, dz = z - cz;
    if (dx * dx + dz * dz < RADIUS * RADIUS) return true;
  }
  return false;
}

bool walker_blocked(const Walker *w, double x, double z) { return blocked(w, x, z); }

static bool try_axis(Walker *w, double dx, double dz) {
  double nx = w->pos.x + dx, nz = w->pos.y + dz;
  if (blocked(w, nx, nz)) return false;
  w->pos = v2(nx, nz);
  return true;
}

// move with sliding collisions (axis-separated, then push-out)
static void move(Walker *w, double dx, double dz) {
  int steps = (int)fmax(1, ceil(js_hypot2(dx, dz) / 0.1));
  for (int i = 0; i < steps; i++) {
    try_axis(w, dx / steps, 0);
    try_axis(w, 0, dz / steps);
  }
}

static void apply(Walker *w) {
  // head bob: lowest at each foot strike, small lateral sway and roll with the stride
  double b = w->bob_amp;
  double bob = -BOB * b * (1 - fabs(sin(w->phase)));
  double sway = 0.012 * b * sin(w->phase * 0.5);
  double cy = cos(w->yaw), sy = sin(w->yaw);
  w->camera->node->position = v3(w->pos.x + cy * sway, w->feet_y + w->air_y + EYE_HEIGHT + bob + w->dip, w->pos.y - sy * sway);
  node_set_euler(w->camera->node, euler(w->pitch + 0.004 * b * sin(w->phase * 2), w->yaw, 0.0035 * b * sin(w->phase * 0.5), EULER_YXZ));
}

static void step_event(Walker *w, double speed, bool land, double impact) {
  if (!w->on_step) return;
  WalkStep s = { w->pos.x, w->pos.y, w->feet_y, speed, land, impact };
  w->on_step(&s, w->step_user);
}

void walker_update(Walker *w, double dt) {
  dt = fmin(dt, 0.05);
  if (!(dt > 0)) return;
  bool moving = w->active || w->virtual_keys;
  double f = moving ? (key2(w, WK_W, WK_UP) ? 1 : 0) - (key2(w, WK_S, WK_DOWN) ? 1 : 0) : 0;
  double r = moving ? (key2(w, WK_D, WK_RIGHT) ? 1 : 0) - (key2(w, WK_A, WK_LEFT) ? 1 : 0) : 0;
  double speed = key2(w, WK_SHIFT_L, WK_SHIFT_R) ? STROLL : WALK;
  // analog stick (touch joystick): deflection sets the pace, a light run near full tilt
  if (moving && !f && !r && w->has_stick) {
    double m = fmin(1, js_hypot2(w->stick.x, w->stick.y));
    if (m > 0.08) {
      f = w->stick.y; r = w->stick.x;
      speed = m > 0.85 ? STROLL : WALK * fmax(0.3, m / 0.85);
    }
  }
  double sy = sin(w->yaw), cy = cos(w->yaw);
  double tx = -sy * f + cy * r, tz = -cy * f - sy * r;
  double tl = js_hypot2(tx, tz);
  if (tl > 0) { tx = (tx / tl) * speed; tz = (tz / tl) * speed; }
  // soft ends of the walkable district: walking on toward z0 / z1 slows to a stop
  const WalkWorld *W = w->world;
  if (W->bounds.soft) {
    double room = tz < 0 ? w->pos.y - W->bounds.z0 : W->bounds.z1 - w->pos.y;
    tz *= pow(clampd(room / W->bounds.soft, 0, 1), 0.75);
  }
  // smooth acceleration, quicker to stop than to start
  double a = 1 - exp(-dt * (tl > 0 ? 6 : 9));
  w->vel.x += (tx - w->vel.x) * a;
  w->vel.y += (tz - w->vel.y) * a;
  if (w->vel.x * w->vel.x + w->vel.y * w->vel.y < 1e-6) w->vel = v2(0, 0);

  V2 before = w->pos;
  move(w, w->vel.x * dt, w->vel.y * dt);
  double mx = w->pos.x - before.x, mz = w->pos.y - before.y;
  double moved = sqrt(mx * mx + mz * mz);
  double v = moved / dt;
  if (moved < sqrt(w->vel.x * w->vel.x + w->vel.y * w->vel.y) * dt * 0.3) w->vel = v2(w->vel.x * 0.5, w->vel.y * 0.5);   // pushing into a wall

  // ground following: step up quickly, settle down a little softer
  double g = W->height_at(W->user, w->pos.x, w->pos.y, w->feet_y);
  double k = g > w->feet_y ? 16 : 11;
  w->feet_y += (g - w->feet_y) * (1 - exp(-dt * k));
  if (fabs(g - w->feet_y) < 0.002) w->feet_y = g;

  // jump: a ballistic offset above the followed ground. Collisions, ledges and the deck are
  // still resolved at ground level, so a jump never clears a railing or a wall.
  bool airborne = w->air_y > 0 || w->vy > 0;
  if ((w->jump_req || (w->virtual_keys && key1(w, WK_SPACE))) && !airborne && fabs(g - w->feet_y) < 0.05) w->vy = jump_v();
  w->jump_req = false;
  if (w->air_y > 0 || w->vy > 0) {
    w->vy -= GRAVITY * dt;
    w->air_y += w->vy * dt;
    if (w->air_y <= 0) {
      double impact = -w->vy;
      w->air_y = 0; w->vy = 0;
      w->dip_v -= impact * 0.3;
      step_event(w, v, true, impact);
      w->step_dist = 0;
    }
  }
  // landing dip: critically damped spring
  w->dip_v += (-DIP_K * w->dip - 2 * sqrt(DIP_K) * w->dip_v) * dt;
  w->dip += w->dip_v * dt;

  // step cycle: stride from distance walked (no steps in the air)
  double stride = v > 2.0 ? 0.95 : 0.75;
  bool in_air = w->air_y > 0;
  w->bob_amp += ((v > 0.2 && !in_air ? fmin(1, v / WALK) : 0) - w->bob_amp) * (1 - exp(-dt * 5));
  if (!in_air && v > 0.2) {
    w->phase += (moved / stride) * PI_D;
    w->step_dist += moved;
    if (w->step_dist >= stride) {
      w->step_dist -= stride;
      step_event(w, v, false, 0);
    }
  } else if (!in_air && w->step_dist > 0) {
    w->step_dist = fmax(0, w->step_dist - dt * 0.5);   // next step comes promptly when walking resumes
  }
  apply(w);
}

void walker_simulate(Walker *w, const bool keys[WK_COUNT], double seconds, double dt) {
  w->virtual_keys = keys;
  for (double t = 0; t < seconds; t += dt) walker_update(w, dt);
  w->virtual_keys = nullptr;
}
