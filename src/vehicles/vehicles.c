// Port of src/vehicles/index.js.
#include "vehicles/vehicles.h"

#include <math.h>
#include <string.h>

#include "gfx/three_mat.h"
#include "quality.h"
#include "vehicles/models.h"
#include "world/layout.h"

static const double REACH[2] = { 2.5, 2.9 };
static const double RIDE_PITCH[2] = { -13, -9 };   // deg, the view settles to this on mounting
static constexpr double BASE_FOV = 50;

static double wrap(double a) { return atan2(sin(a), cos(a)); }
static double clampv(double v, double a, double b) { return v < a ? a : v > b ? b : v; }
static double lerp(double a, double b, double t) { return a + (b - a) * t; }

// ---------------------------------------------------------------------------------------------
// spray / roost particles (one Points draw call)

typedef struct Particle { double life, max, vx, vy, vz, a, s, g; } Particle;
typedef struct Spray {
  int N;
  float *pos, *col, *al, *sz;
  Particle *P;
  GpuGeometry *geo;
  Node *pts;
  Material *mat;
  int head, live;
  Rng rng;
} Spray;

static Spray *create_spray(Node *scene) {
  Spray *S = xcalloc(1, sizeof *S);
  int N = QUALITY.tier == TIER_HIGH ? 220 : QUALITY.tier == TIER_MEDIUM ? 140 : 60;
  S->N = N;
  S->P = xcalloc((size_t)N, sizeof *S->P);
  for (int i = 0; i < N; i++) S->P[i] = (Particle){ 0, 1, 0, 0, 0, 0, 0, 1 };
  Geometry *g = geo_new();
  S->pos = geo_set_attr(g, "position", 3, N);
  geo_set_attr(g, "aCol", 3, N);
  geo_set_attr(g, "aA", 1, N);
  geo_set_attr(g, "aS", 1, N);
  S->geo = gpu_geometry(g);
  geo_free(g);
  S->pos = xcalloc((size_t)N * 3, sizeof(float));
  S->col = xcalloc((size_t)N * 3, sizeof(float));
  S->al = xcalloc((size_t)N, sizeof(float));
  S->sz = xcalloc((size_t)N, sizeof(float));
  MatDesc d = md_shader();
  d.name = "spray";
  d.transparent = true;
  d.depth_write = false;
  d.prog[MV_PLAIN] = PROG_SHADER;
  S->mat = mat_three(&d);
  mat_set_float(S->mat, "uScale", 600);
  S->pts = node_mesh(S->geo, S->mat);
  S->pts->kind = NODE_POINTS;
  snprintf(S->pts->name, sizeof S->pts->name, "spray");
  S->pts->frustum_culled = false;
  S->pts->render_order = 6;
  node_add(scene, S->pts);
  S->rng = rng_make((double)(SDL_GetPerformanceCounter() & 0xffffffff));
  return S;
}

static double rnd(Spray *S) { return rng_next(&S->rng); }   // (Math.random in the JS)

static void spray_emit(Spray *S, double x, double y, double z, double vx, double vy, double vz, bool sand) {
  int i = S->head;
  S->head = (S->head + 1) % S->N;
  Particle *p = &S->P[i];
  p->life = 0;
  p->max = sand ? 0.5 + rnd(S) * 0.4 : 0.45 + rnd(S) * 0.5;
  p->vx = vx; p->vy = vy; p->vz = vz; p->a = sand ? 0.55 : 0.8;
  p->s = (sand ? 0.035 : 0.04) + rnd(S) * 0.05;
  p->g = sand ? 1 : 0.9;
  S->pos[i * 3] = (float)x; S->pos[i * 3 + 1] = (float)y; S->pos[i * 3 + 2] = (float)z;
  static const float WATER[3] = { 0.95f, 0.95f, 0.93f }, SANDC[3] = { 0.6f, 0.5f, 0.38f };
  memcpy(&S->col[i * 3], sand ? SANDC : WATER, 3 * sizeof(float));
  S->live = S->N;
}

static void spray_update(Spray *S, double dt, const Camera *camera, int render_h) {
  if (!S->live) return;
  bool any = false;
  for (int i = 0; i < S->N; i++) {
    Particle *p = &S->P[i];
    if (p->life >= p->max) { S->al[i] = 0; continue; }
    any = true;
    p->life += dt;
    p->vy -= 9.8 * p->g * dt;
    double d = exp(-dt * 1.2);
    p->vx *= d; p->vz *= d;
    S->pos[i * 3] = (float)(S->pos[i * 3] + p->vx * dt);
    S->pos[i * 3 + 1] = (float)(S->pos[i * 3 + 1] + p->vy * dt);
    S->pos[i * 3 + 2] = (float)(S->pos[i * 3 + 2] + p->vz * dt);
    double f = p->life / p->max;
    S->al[i] = (float)(p->a * (1 - f) * fmin(1, p->life * 20));
    S->sz[i] = (float)(p->s * (1 + f * 1.5));
  }
  if (!any) S->live = 0;
  gpu_geometry_update_attr(S->geo, "position", S->pos);
  gpu_geometry_update_attr(S->geo, "aCol", S->col);
  gpu_geometry_update_attr(S->geo, "aA", S->al);
  gpu_geometry_update_attr(S->geo, "aS", S->sz);
  mat_set_float(S->mat, "uScale", render_h / (2 * tan(camera->fov * DEG2RAD / 2)));
}

// ---------------------------------------------------------------------------------------------

typedef struct Entry { VKind kind; Vehicle v; VehicleModel m; double kick; } Entry;

struct Vehicles {
  VehiclesOpts o;
  Entry list[2];
  VWorld world;
  Vec(VDyn) dyn;
  Spray *spray;
  Entry *rider, *near;
  struct { bool on; double t; V3 pos; Quat quat; double rel0, pitch0; } trans;
  bool off_req;
  double off_fail;
  double rel, last_yaw, look_idle, hint_t;
  bool has_last_shadow;
  struct { double x, z, yaw, roll; } last_shadow;
  const bool *virtual_keys;
  double fov, clock;
  const char *prompt_key, *prompt_text;
};

static void request_shadow(Vehicles *V) { if (V->o.shadow_wanted) *V->o.shadow_wanted = true; }

static void set_rot(Node *n, double x, double y, double z) { node_set_euler(n, euler(x, y, z, n->rotation.order)); }

static void pose3(Entry *e, double t) {
  Vehicle *v = &e->v;
  VehicleModel *m = &e->m;
  m->root->position = v3(v->x, v->body_y + v->air_y, v->z);
  set_rot(m->root, 0, v->yaw, 0);
  set_rot(m->tilt, v->pitch, 0, v->roll);
  if (e->kind == VK_BIKE) {
    set_rot(m->steer, m->steer->rotation.x, -v->steer * 1.15, m->steer->rotation.z);
    set_rot(m->wheel_f, -v->wheel_rot, m->wheel_f->rotation.y, m->wheel_f->rotation.z);
    set_rot(m->wheel_r, -v->wheel_rot, m->wheel_r->rotation.y, m->wheel_r->rotation.z);
    set_rot(m->crank, -v->crank, m->crank->rotation.y, m->crank->rotation.z);
    for (int i = 0; i < 2; i++) set_rot(m->pedals[i], v->crank, m->pedals[i]->rotation.y, m->pedals[i]->rotation.z);
    double k = e->kick;
    set_rot(m->kick, lerp(-1.45, -0.32, k), 0, lerp(-0.08, -0.42, k));
  } else {
    set_rot(m->steer, m->steer->rotation.x, -v->steer * 1.5, m->steer->rotation.z);
    double R = m->R, by = v->body_y + v->air_y, sp = sin(v->pitch), cp = cos(v->pitch), sr = sin(v->roll), cr = cos(v->roll);
    for (int i = 0; i < 4; i++) {
      AtvWheel *w = &m->wheels[i];
      double T = v->wheel_h[i] + R;
      double y = ((T - by + w->z * sp) / cp - w->x * sr) / cr;
      w->hold->position.y = clampv(y, R - 0.1, R + 0.16);
      if (i < 2) set_rot(w->hold, w->hold->rotation.x, -v->steer, w->hold->rotation.z);
      set_rot(w->mesh, -v->wheel_rot, w->mesh->rotation.y, w->mesh->rotation.z);
    }
    bool on = v->ridden;
    mat_set_float(m->mat, "uLights", on ? 1 : 0);
    double c = fmod(t, 0.9) / 0.9;
#define FLASH(c0) (fmod(c - (c0) + 1, 1) < 0.1 || (fmod(c - (c0) + 1, 1) > 0.17 && fmod(c - (c0) + 1, 1) < 0.27) ? 1 : 0)
    mat_set_float(m->mat, "uBeaconA", on ? FLASH(0) : 0);
    mat_set_float(m->mat, "uBeaconB", on ? FLASH(0.5) : 0);
#undef FLASH
  }
  Node *s = m->shadow;
  s->position = v3(v->x - 0.25, v->base_t + 0.025, v->z);
  node_set_euler(s, euler(v->pitch_t, v->yaw, v->roll_t, EULER_YXZ));
  s->scale = v3s(1 / (1 + (v->body_y + v->air_y - v->base_t) * 1.5));
}

static V3 eye_world(const Entry *e, double bx, double by) {
  const Vehicle *v = &e->v;
  const VSpec *S = v->spec;
  Quat q = quat_from_euler(euler(v->pitch, v->yaw, v->roll, EULER_YXZ));
  V3 out = v3_apply_quat(v3(S->eye[0] + bx, S->eye[1] + by, S->eye[2]), q);
  out.x += v->x; out.y += v->body_y + v->air_y; out.z += v->z;
  return out;
}
static Quat head_quat(const Vehicles *V, const Entry *e) {
  const Vehicle *v = &e->v;
  double k = e->kind == VK_BIKE ? 0.55 : 0.8;
  Quat a = quat_from_euler(euler(v->pitch * 0.85, v->yaw, v->roll * k, EULER_YXZ));
  Quat b = quat_from_euler(euler(V->o.walker->pitch, V->rel, 0, EULER_YXZ));
  return quat_mul(a, b);
}

// Camera.getWorldDirection
static V3 camera_dir(const Camera *c) {
  node_update_matrix_world(c->node, true);
  const double *e = c->node->matrix_world.e;
  return v3_neg(v3_norm(v3(e[8], e[9], e[10])));
}

bool vehicles_mount(Vehicles *V, VKind kind) {
  Entry *e = &V->list[kind];
  if (V->rider) return false;
  V->rider = e;
  Vehicle *v = &e->v;
  Walker *w = V->o.walker;
  v->parked = false; v->ridden = true;
  V->rel = wrap(w->yaw - v->yaw);
  V->last_yaw = w->yaw;
  V->trans.on = true;
  V->trans.t = 0;
  V->trans.pos = V->o.camera->node->position;
  V->trans.quat = V->o.camera->node->quaternion;
  V->trans.rel0 = V->rel;
  V->trans.pitch0 = w->pitch;
  w->vel = v2(0, 0);
  w->jump_req = false;
  V->hint_t = 3.5;
  V->off_req = false;
  return true;
}

static bool dismount(Vehicles *V) {
  if (!V->rider) return false;
  Entry *e = V->rider;
  Vehicle *v = &e->v;
  Walker *w = V->o.walker;
  double spots[8][2];
  vehicle_dismount_spots(v, spots);
  for (int i = 0; i < 8; i++) {
    double x = spots[i][0], z = spots[i][1];
    double feet = w->world->height_at(w->world->user, x, z, v->ground_y + 0.3);
    if (fabs(feet - v->ground_y) > 0.5) continue;
    w->feet_y = feet;
    if (walker_blocked(w, x, z)) continue;
    V3 d = camera_dir(V->o.camera);
    double heading = atan2(d.x, -d.z) * RAD2DEG;
    walker_set(w, x, feet + EYE_HEIGHT, z, heading, w->pitch * RAD2DEG);
    v->ridden = false; v->parked = true;
    v->vx = v->vz = 0; v->lon = 0; v->steer *= 0.3; v->air_y = 0; v->vy_air = 0;
    V->rider = nullptr; V->trans.on = false; V->off_req = false;
    if (V->o.audio) audio_vehicle(V->o.audio, &(RideState){ .kind = RIDE_NONE });
    request_shadow(V);
    return true;
  }
  V->off_fail = 2;
  return false;
}

void vehicles_toggle(Vehicles *V) {
  if (V->rider) {
    if (fabs(V->rider->v.lon) > 1.2) V->off_req = true;
    else dismount(V);
    return;
  }
  if (V->near) vehicles_mount(V, V->near->kind);
}

void vehicles_handle_event(Vehicles *V, const SDL_Event *e) {
  if (e->type != SDL_EVENT_KEY_DOWN || e->key.scancode != SDL_SCANCODE_E || e->key.repeat || V->o.shot) return;
  if (!V->o.walker->active && !V->rider) return;
  vehicles_toggle(V);
}

static Entry *find_near(Vehicles *V) {
  Walker *w = V->o.walker;
  if (!w->active || w->air_y > 0) return nullptr;
  V3 d = camera_dir(V->o.camera);
  double fl = js_hypot2(d.x, d.z);
  if (fl == 0) fl = 1;
  double fx = d.x / fl, fz = d.z / fl;
  Entry *best = nullptr;
  double bd = INFINITY;
  for (int i = 0; i < 2; i++) {
    Entry *e = &V->list[i];
    Vehicle *v = &e->v;
    double dmin = INFINITY;
    for (int k = 0; k < v->spec->ncircles; k++) dmin = fmin(dmin, js_hypot2(w->pos.x - v->circles[k].c.x, w->pos.y - v->circles[k].c.z));
    if (dmin > REACH[e->kind] || fabs(w->feet_y - v->ground_y) > 0.8) continue;
    double dx = v->x - w->pos.x, dz = v->z - w->pos.y, dl = js_hypot2(dx, dz);
    if (dl == 0) dl = 1;
    double facing = (dx * fx + dz * fz) / dl;
    if (dmin > 1.1 && facing < 0.62) continue;
    if (dmin < bd) { bd = dmin; best = e; }
  }
  return best;
}

static bool key2(const Vehicles *V, WalkKey a, WalkKey b) {
  const bool *k = V->virtual_keys ? V->virtual_keys : V->o.walker->keys;
  return k[a] || k[b];
}

static VInput read_input(Vehicles *V, const Entry *e) {
  Walker *w = V->o.walker;
  bool active = w->active || V->virtual_keys;
  double f = 0, r = 0;
  bool hard = false;
  if (active) {
    f = (key2(V, WK_W, WK_UP) ? 1 : 0) - (key2(V, WK_S, WK_DOWN) ? 1 : 0);
    r = (key2(V, WK_D, WK_RIGHT) ? 1 : 0) - (key2(V, WK_A, WK_LEFT) ? 1 : 0);
    hard = key2(V, WK_SHIFT_L, WK_SHIFT_R);
    if (!f && !r && w->has_stick) {
      double m = fmin(1, js_hypot2(w->stick.x, w->stick.y));
      if (m > 0.08) {
        f = w->stick.y;
        r = w->stick.x * fmin(1, 1.3 * fabs(w->stick.x) / fmax(m, 1e-3));
        hard = hard || m > 0.92;
      }
    }
  }
  bool hop = w->jump_req || (V->virtual_keys ? V->virtual_keys[WK_SPACE] : false);
  w->jump_req = false;
  if (V->off_req) {
    double lon = e->v.lon;
    f = fabs(lon) > 0.3 ? -js_sign(lon) : 0; r = 0; hard = false; hop = false;
  }
  return (VInput){ clampv(f, -1, 1), clampv(r, -1, 1), hard, hop };
}

static void prompt(Vehicles *V, const char *key, const char *text) { V->prompt_key = key; V->prompt_text = text; }

static void apply_fov(Vehicles *V) {
  if (fabs(V->o.camera->fov - V->fov) > 0.01) V->o.camera->fov = V->fov;
}

static int poisson(Spray *S, double m) {
  int n = (int)floor(m);
  if (rnd(S) < m - n) n++;
  return n;
}

void vehicles_update(Vehicles *V, double dt) {
  dt = fmin(dt, 0.05);
  V->clock += dt;
  Walker *w = V->o.walker;
  Camera *camera = V->o.camera;
  int rh = V->o.render_height ? *V->o.render_height : 720;
  V->dyn.len = 0;
  for (int i = 0; i < V->o.ndynamic; i++) vec_push(&V->dyn, ((VDyn){ V->o.dynamic_circles[i], nullptr }));
  for (int i = 0; i < 2; i++) {
    Entry *e = &V->list[i];
    if (e == V->rider) continue;
    for (int k = 0; k < e->v.spec->ncircles; k++) vec_push(&V->dyn, ((VDyn){ &e->v.circles[k].c, &e->v }));
  }
  V->world.dyn = V->dyn.data;
  V->world.ndyn = (int)V->dyn.len;

  // the other vehicles: kickstand, settle, visibility
  for (int i = 0; i < 2; i++) {
    Entry *e = &V->list[i];
    if (e == V->rider) continue;
    Vehicle *v = &e->v;
    if (e->kind == VK_BIKE && (fabs(v->lean + 0.13) > 0.002 || e->kick < 1)) {
      e->kick = fmin(1, e->kick + dt * 3);
      vehicle_step(v, (VInput){ 0, 0, false, false }, dt, &V->world);
      pose3(e, V->clock);
      request_shadow(V);
    } else if (fabs(v->body_y - v->base_t) > 0.002 || fabs(v->body_v) > 0.01) {
      vehicle_step(v, (VInput){ 0, 0, false, false }, dt, &V->world);
      pose3(e, V->clock);
    }
    bool far = js_hypot2(camera->node->position.x - v->x, camera->node->position.z - v->z) > 240;
    e->m.root->visible = e->m.shadow->visible = !far;
  }

  if (!V->rider) {
    V->near = find_near(V);
    prompt(V, V->near ? "E" : "", V->near ? "ride" : "");
    V->fov += (BASE_FOV - V->fov) * (1 - exp(-dt * 6));
    apply_fov(V);
    spray_update(V->spray, dt, camera, rh);
    return;
  }

  Entry *e = V->rider;
  Vehicle *v = &e->v;
  V->near = nullptr;
  if (e->kind == VK_BIKE) e->kick = fmax(0, e->kick - dt * 4);
  VInput input = { 0, 0, false, false };
  if (V->trans.on) {
    V->trans.t += dt / 0.4;
    w->jump_req = false;
  } else input = read_input(V, e);
  vehicle_step(v, input, dt, &V->world);
  if (V->off_req && fabs(v->lon) < 1.0) dismount(V);
  if (!V->rider) { spray_update(V->spray, dt, camera, rh); return; }
  pose3(e, V->clock);

  // look: mouse / drag deltas since the last frame move the head relative to the vehicle
  double d = wrap(w->yaw - V->last_yaw);
  if (fabs(d) > 1e-5) { V->rel = wrap(V->rel + d); V->look_idle = 0; }
  else V->look_idle += dt;
  if (V->look_idle > 1.4 && fabs(v->lon) > 1.5 && !V->trans.on) V->rel *= exp(-dt * 1.3);
  if (V->trans.on) {
    double k = fmin(1, V->trans.t), s = k * k * (3 - 2 * k);
    V->rel = V->trans.rel0 * (1 - s);
    w->pitch = lerp(V->trans.pitch0, RIDE_PITCH[e->kind] * DEG2RAD, s);
  }
  w->yaw = v->yaw + V->rel;
  V->last_yaw = w->yaw;

  // camera: rider's eye, pedalling bob / engine shake
  double bx = 0, by = 0;
  if (e->kind == VK_BIKE) {
    double a = fmin(1, fabs(v->lon) / 3) * v->pedal;
    by = 0.012 * a * sin(v->crank * 2);
    bx = 0.008 * a * sin(v->crank);
  } else {
    double j = 0.0006 + 0.0012 * (v->rpm - 1400) / 6000;
    by = j * sin(V->clock * 83) + j * 0.6 * sin(V->clock * 131);
    bx = j * 0.5 * sin(V->clock * 97);
  }
  V3 eye = eye_world(e, bx, by);
  Quat hq = head_quat(V, e);
  if (V->trans.on && V->trans.t < 1) {
    double k = V->trans.t, s = k * k * (3 - 2 * k);
    camera->node->position = v3_lerp(V->trans.pos, eye, s);
    node_set_quaternion(camera->node, quat_slerp(V->trans.quat, hq, s));
  } else {
    V->trans.on = false;
    camera->node->position = eye;
    node_set_quaternion(camera->node, hq);
  }
  w->pos = v2(v->x, v->z);
  w->feet_y = v->ground_y;
  V->fov += (56 + fmin(1, fabs(v->lon) / 12) * 5 - V->fov) * (1 - exp(-dt * 3));
  apply_fov(V);

  // spray from the wheels in the swash, roost from the ATV on soft sand
  double sy = sin(v->yaw), cy = cos(v->yaw), al = fabs(v->lon);
  for (int i = 0; i < v->spec->nwheels; i++) {
    double lx = v->spec->wheels[i][0], lz = v->spec->wheels[i][1];
    double dep = v->wheel_depth[i];
    double wx = v->x + lx * cy + lz * sy, wz = v->z - lx * sy + lz * cy, gy = v->wheel_h[i];
    double side = lx == 0 ? (rnd(V->spray) < 0.5 ? -1 : 1) : js_sign(lx);
    if (dep > 0.015 && al > 1.2) {
      double rate = (e->kind == VK_ATV ? 45 : 16) * fmin(1, al / 7) * fmin(1, dep / 0.07);
      for (int n = poisson(V->spray, rate * dt); n > 0; n--) {
        double up = (1.2 + rnd(V->spray) * 2.2) * fmin(1.3, al / 6);
        double back = -v->lon * (0.25 + rnd(V->spray) * 0.3), out = side * (0.6 + rnd(V->spray) * 1.4);
        spray_emit(V->spray, wx + side * 0.1, gy + 0.05, wz, -sy * back + cy * out, up, -cy * back - sy * out, false);
      }
    }
    if (e->kind == VK_ATV && i >= 2 && v->surf.kind == VS_SAND && v->surf.soft > 0.3 && input.throttle > 0.4 && al > 1.5) {
      for (int n = poisson(V->spray, 22 * v->surf.soft * dt); n > 0; n--) {
        double back = 1.5 + rnd(V->spray) * 2.5, out = side * rnd(V->spray) * 0.7;
        spray_emit(V->spray, wx, gy + 0.04, wz + 0, sy * back + cy * out, 0.8 + rnd(V->spray) * 1.4, cy * back - sy * out, true);
      }
    }
  }
  spray_update(V->spray, dt, camera, rh);

  // the ridden vehicle casts a live shadow: refresh the (static) shadow map as it moves
  double st = QUALITY.shadowStep;
  if (!V->has_last_shadow || js_hypot2(v->x - V->last_shadow.x, v->z - V->last_shadow.z) > st ||
      fabs(wrap(v->yaw - V->last_shadow.yaw)) > 0.06 || fabs(v->roll - V->last_shadow.roll) > 0.06) {
    V->has_last_shadow = true;
    V->last_shadow.x = v->x; V->last_shadow.z = v->z; V->last_shadow.yaw = v->yaw; V->last_shadow.roll = v->roll;
    request_shadow(V);
  }

  if (V->o.audio) {
    static const RideSurface RS[5] = { RS_PAVEMENT, RS_GRASS, RS_WETSAND, RS_SAND, RS_WATER };
    double depth = -INFINITY;
    for (int i = 0; i < v->spec->nwheels; i++) depth = fmax(depth, v->wheel_depth[i]);
    RideState rs = { .kind = e->kind == VK_BIKE ? RIDE_BIKE : RIDE_ATV, .lon = v->lon, .throttle = input.throttle, .rpm = v->rpm, .load = v->load,
                     .surface = RS[v->surf.kind], .soft = v->surf.soft, .depth = depth, .coasting = v->coasting, .pedal = v->pedal,
                     .crank = v->crank, .bump = v->bump, .land = v->land };
    audio_vehicle(V->o.audio, &rs);
  }

  V->hint_t -= dt;
  if (V->off_fail > 0) { V->off_fail -= dt; prompt(V, "", "No room to get off here"); }
  else if (V->off_req) prompt(V, "", "Stopping…");
  else if (V->hint_t > 0) prompt(V, "E", "get off");
  else prompt(V, "", "");
}

Vehicles *create_vehicles(Node *scene, VehiclesOpts o) {
  Vehicles *V = xcalloc(1, sizeof *V);
  V->o = o;
  V->fov = BASE_FOV;
  V->prompt_key = V->prompt_text = "";
  const WalkWorld *ww = o.walker->world;
  V->world = (VWorld){ o.surf, ww->bounds.x0, ww->bounds.x1, ww->bounds.z0, ww->bounds.z1,
                       vgrid_build(o.static_boxes, o.nstatic_boxes, o.static_circles, o.nstatic_circles), nullptr, 0 };
  static const double PARKED[2][3] = { { 7.5, -26.5, -0.17 }, { TOWER.x + 1.2, TOWER.z + 4.8, 0.35 - PI_D } };
  for (int kind = 0; kind < 2; kind++) {
    Entry *e = &V->list[kind];
    e->kind = (VKind)kind;
    e->m = kind == VK_BIKE ? build_bike_model() : build_atv_model();
    double px = PARKED[kind][0], pz = PARKED[kind][1], yaw = PARKED[kind][2];
    vehicle_init(&e->v, e->kind, px, pz, yaw, &V->world);
    // keep the parking spot clear of benches, palms and props (deterministic nudge)
    for (int k = 0; k < 40 && vehicle_blocked(&e->v, e->v.x, e->v.z, e->v.yaw, &V->world, 0); k++) {
      double a = k * 2.4, r = 0.3 + k * 0.08;
      vehicle_init(&e->v, e->kind, px + cos(a) * r, pz + sin(a) * r, yaw, &V->world);
    }
    e->v.parked = true;
    if (kind == VK_BIKE) { e->v.lean = -0.13; e->v.roll = 0.13; }
    snprintf(e->m.shadow->name, sizeof e->m.shadow->name, "vehicle-shadow");
    node_add(scene, e->m.root);
    node_add(scene, e->m.shadow);
    e->kick = 1;
    pose3(e, 0);
    if (o.shot) { e->m.root->visible = false; e->m.shadow->visible = false; }
  }
  V->spray = create_spray(scene);
  return V;
}

int vehicles_colliders(Vehicles *V, const WalkCircle **out, int max) {
  int n = 0;
  for (int i = 0; i < 2; i++)
    for (int k = 0; k < V->list[i].v.spec->ncircles && n < max; k++) out[n++] = &V->list[i].v.circles[k].c;
  return n;
}
bool vehicles_riding(const Vehicles *V) { return V->rider != nullptr; }
void vehicles_prompt(const Vehicles *V, const char **key, const char **text) { *key = V->prompt_key; *text = V->prompt_text; }
// the touch Ride button (setTouch): 'ride' with a vehicle in reach, 'off' while riding
int vehicles_touch_mode(const Vehicles *V, VKind *kind) {
  if (V->rider) { *kind = V->rider->kind; return 2; }
  *kind = VK_BIKE;
  return V->near ? 1 : 0;
}

bool vehicles_place(Vehicles *V, double x, double z, double yaw, int kind) {
  Entry *e = kind >= 0 ? &V->list[kind] : V->rider;
  if (!e) return false;
  e->v.x = x; e->v.z = z; e->v.yaw = yaw; e->v.vx = 0; e->v.vz = 0; e->v.lon = 0; e->v.steer = 0;
  vehicle_settle(&e->v, &V->world);
  pose3(e, V->clock);
  return !vehicle_blocked(&e->v, x, z, yaw, &V->world, 0);
}

void vehicles_simulate(Vehicles *V, const bool keys[WK_COUNT], double seconds, double dt, double *max_speed, double *max_bump) {
  V->virtual_keys = keys;
  double ms = 0, mb = 0;
  for (double t = 0; t < seconds; t += dt) {
    vehicles_update(V, dt);
    if (!V->rider) break;
    ms = fmax(ms, fabs(V->rider->v.lon));
    mb = fmax(mb, V->rider->v.bump);
  }
  V->virtual_keys = nullptr;
  if (max_speed) *max_speed = ms;
  if (max_bump) *max_bump = mb;
}

bool vehicles_dismount(Vehicles *V) { return dismount(V); }
const Vehicle *vehicles_current(const Vehicles *V) { return V->rider ? &V->rider->v : nullptr; }
const Vehicle *vehicles_get(const Vehicles *V, VKind kind) { return &V->list[kind].v; }
