// Port of src/vehicles/sim.js.
#include "vehicles/sim.h"

#include <math.h>

#include "world/layout.h"

static constexpr double G = 9.8;
// seawall accesses (beach.js ACCESS_Z + MORE_ACCESS_Z) and their step profile
static const double ACCESS_ZS[8] = { -30, 32, -322, -245, -134, 134, 245, 322 };
static constexpr double ACCESS_HALF = 1.2;
static const double STEPS[3][3] = { { 10.8, 11.1, 0.33 }, { 11.1, 11.4, 0.5 }, { 11.4, 12.55, 0.7 } };

static bool near_access(double z, double pad) {
  for (int i = 0; i < 8; i++) if (fabs(z - ACCESS_ZS[i]) < ACCESS_HALF + pad) return true;
  return false;
}
static double promenade_x(double z) { return PARK.promenadeX + 2.6 * sin(z / 19) + 1.2 * sin(z / 7.3); }
static double clampv(double v, double a, double b) { return v < a ? a : v > b ? b : v; }
static double lerp(double a, double b, double t) { return a + (b - a) * t; }
static double smooth(double a, double b, double v) { double t = clampv((v - a) / (b - a), 0, 1); return t * t * (3 - 2 * t); }

enum { S_PAVEMENT, S_GRASS, S_WETSAND, S_SAND };
const VSpec SPEC_BIKE = {
  .kind = VK_BIKE, .wheelbase = 1.22, .wheel_r = 0.335,
  .nwheels = 2, .wheels = { { 0, -0.7 }, { 0, 0.52 } },
  .ncircles = 2, .circles = { { -0.48, 0.3 }, { 0.32, 0.3 } },
  .max_step = 0.17, .access_step = 0.23, .max_depth = 0.2, .x_min = -INFINITY,
  .vmax = { { 4.6, 6.2 }, { 3.2, 4.2 }, { 4.3, 5.7 }, { 2.2, 3.0 } },
  .roll = { 0.12, 0.5, 0.35, 1.4 },
  .accel = 1.7, .accel_hard = 2.4, .brake = 4.5, .rev_accel = 1.2, .rev_max = 0.9, .engine_brake = 0,
  .steer_max = 0.62, .steer_v = 3.2, .steer_rate = 7, .grip_hard = 16, .grip_soft = 10,
  .water_k = 0.22, .eye = { 0, 1.62, 0.26 }, .idle = 0,
};
const VSpec SPEC_ATV = {
  .kind = VK_ATV, .wheelbase = 1.25, .track = 0.97, .wheel_r = 0.31,
  .nwheels = 4, .wheels = { { -0.485, -0.625 }, { 0.485, -0.625 }, { -0.485, 0.625 }, { 0.485, 0.625 } },   // FL FR RL RR
  .ncircles = 3, .circles = { { -0.62, 0.6 }, { 0.05, 0.6 }, { 0.6, 0.6 } },
  .max_step = 0.3, .access_step = 0.3, .max_depth = 0.32, .x_min = 12.9,
  .vmax = { { 10, 12.5 }, { 8, 10 }, { 10, 12.5 }, { 6.5, 8.3 } },
  .roll = { 0.3, 0.8, 0.55, 1.2 },
  .accel = 4.2, .accel_hard = 5.6, .brake = 7, .rev_accel = 2.5, .rev_max = 3.2, .engine_brake = 0.9,
  .steer_max = 0.6, .steer_v = 5, .steer_rate = 6, .grip_hard = 9, .grip_soft = 5,
  .water_k = 0.45, .eye = { 0, 1.6, 0.3 }, .idle = 1450, .redline = 7600,
};

// ground the wheels roll on: the seawall access steps for the bike, else the ground
static double ride_ground(VKind kind, double x, double z) {
  if (kind == VK_BIKE && x >= STEPS[0][0] && x < STEPS[2][1] && near_access(z, 0)) {
    for (int i = 0; i < 3; i++) if (x < STEPS[i][1]) return STEPS[i][2];
  }
  return groundHeight(x, z);
}

double vehicle_water_depth(const VWorld *w, double x, double z) {
  return surf_water_depth_at(w->surf, x, z, surf_time(w->surf));
}

// surface under (x, z): kind + softness 0..1 + water depth (dynamic swash)
static void surface_at(double x, double z, const VWorld *w, VSurface *out) {
  out->depth = 0; out->soft = 0; out->covered = false;
  if (x < PARK.x0) out->kind = VS_PAVEMENT;
  else if (x < SAND.x0 + 0.55) out->kind = fabs(x - promenade_x(z)) < 2.2 || x > PARK.x1 - 1.2 ? VS_PAVEMENT : VS_GRASS;
  else {
    double d = vehicle_water_depth(w, x, z);
    out->depth = d;
    if (d > 0.015) { out->kind = VS_WATER; out->soft = 0.2; }
    else {
      // dry sand is soft; the damp band hardens toward the wet sand the swash packs down
      out->soft = 1 - smooth(WET_LINE_X - 14, WET_LINE_X - 3, x);
      out->covered = surf_swash_at(w->surf, x, z, surf_time(w->surf)).covered;
      if (out->covered) out->soft = 0;
      out->kind = out->soft > 0.5 ? VS_SAND : VS_WETSAND;
    }
  }
}

// ---- static collider grid (boxes + circles), 4 m cells ----

typedef struct GItem { bool box; WalkBox b; WalkCircle c; } GItem;
typedef struct GCell { int i, j; Vec(int) items; bool used; } GCell;
struct VGrid { GCell *cells; size_t cap; Vec(GItem) items; };
static constexpr double CELL = 4;

static size_t cell_hash(int i, int j, size_t cap) { return ((size_t)((uint32_t)i * 73856093u) ^ (size_t)((uint32_t)j * 19349663u)) & (cap - 1); }
static GCell *cell_get(VGrid *g, int i, int j, bool create) {
  size_t h = cell_hash(i, j, g->cap);
  for (;;) {
    GCell *c = &g->cells[h];
    if (!c->used) {
      if (!create) return nullptr;
      c->used = true; c->i = i; c->j = j;
      return c;
    }
    if (c->i == i && c->j == j) return c;
    h = (h + 1) & (g->cap - 1);
  }
}
static void grid_add(VGrid *g, int item, double x0, double z0, double x1, double z1) {
  for (int i = (int)floor(x0 / CELL); i <= (int)floor(x1 / CELL); i++)
    for (int j = (int)floor(z0 / CELL); j <= (int)floor(z1 / CELL); j++) vec_push(&cell_get(g, i, j, true)->items, item);
}
VGrid *vgrid_build(const WalkBox *boxes, int nboxes, const WalkCircle *circles, int ncircles) {
  VGrid *g = xcalloc(1, sizeof *g);
  g->cap = 1 << 16;
  g->cells = xcalloc(g->cap, sizeof *g->cells);
  for (int i = 0; i < nboxes; i++) {
    const WalkBox *b = &boxes[i];
    if (!(b->max.x - b->min.x < 400 && b->max.z - b->min.z < 900)) continue;
    vec_push(&g->items, ((GItem){ true, *b, {} }));
    grid_add(g, (int)g->items.len - 1, b->min.x, b->min.z, b->max.x, b->max.z);
  }
  for (int i = 0; i < ncircles; i++) {
    const WalkCircle *c = &circles[i];
    vec_push(&g->items, ((GItem){ false, {}, *c }));
    grid_add(g, (int)g->items.len - 1, c->x - c->r, c->z - c->r, c->x + c->r, c->z + c->r);
  }
  return g;
}

// ---------------------------------------------------------------------------------------------

static void wheel_point(const Vehicle *v, int i, double x, double z, double yaw, double *px, double *pz) {
  double lx = v->spec->wheels[i][0], lz = v->spec->wheels[i][1];
  double s = sin(yaw), c = cos(yaw);
  *px = x + lx * c + lz * s;
  *pz = z - lx * s + lz * c;
}
static void wheel_heights(const Vehicle *v, double x, double z, double yaw, double *out) {
  for (int i = 0; i < v->spec->nwheels; i++) {
    double px, pz;
    wheel_point(v, i, x, z, yaw, &px, &pz);
    out[i] = ride_ground(v->kind, px, pz);
  }
}
static void targets(Vehicle *v) {
  const double *h = v->wheel_h;
  const VSpec *S = v->spec;
  if (v->kind == VK_BIKE) {
    v->base_t = (h[0] + h[1]) / 2;
    v->pitch_t = atan2(h[0] - h[1], S->wheelbase);
    v->roll_t = 0;
  } else {
    v->base_t = (h[0] + h[1] + h[2] + h[3]) / 4;
    v->pitch_t = atan2((h[0] + h[1]) - (h[2] + h[3]), 2 * S->wheelbase);
    v->roll_t = atan2((h[1] + h[3]) - (h[0] + h[2]), 2 * S->track);
  }
}
void vehicle_sync_circles(Vehicle *v) {
  double s = sin(v->yaw), c = cos(v->yaw);
  for (int i = 0; i < v->spec->ncircles; i++) {
    double lz = v->spec->circles[i][0];
    v->circles[i].c.x = v->x + lz * s;
    v->circles[i].c.z = v->z + lz * c;
  }
}

void vehicle_settle(Vehicle *v, const VWorld *w) {
  (void)w;
  wheel_heights(v, v->x, v->z, v->yaw, v->wheel_h);
  targets(v);
  v->body_y = v->base_t; v->body_v = 0; v->pitch = v->pitch_t; v->pitch_v = 0; v->roll = v->roll_t; v->roll_v = 0;
  v->ground_y = v->base_t;
  vehicle_sync_circles(v);
}

void vehicle_init(Vehicle *v, VKind kind, double x, double z, double yaw, const VWorld *w) {
  const VSpec *spec = kind == VK_BIKE ? &SPEC_BIKE : &SPEC_ATV;
  *v = (Vehicle){ .kind = kind, .spec = spec, .x = x, .z = z, .yaw = yaw, .grounded = true, .rpm = spec->idle, .parked = true,
                  .surf = { VS_PAVEMENT, 0, 0, false }, .sx_ = NAN, .sz_ = NAN, .yaw_ = NAN, .sf_ = -1 };
  for (int i = 0; i < spec->ncircles; i++) v->circles[i] = (VCircle){ { x, z, spec->circles[i][1] }, v };
  vehicle_settle(v, w);
}

typedef enum Block { B_NONE, B_STEP, B_DEEP, B_BOUNDS, B_WALL } Block;

static Block blocked_at(Vehicle *v, double x, double z, double yaw, const VWorld *w, double air_y) {
  const VSpec *S = v->spec;
  double s = sin(yaw), c = cos(yaw);
  // wheels: steps up and deep water
  double h[4];
  wheel_heights(v, x, z, yaw, h);
  for (int i = 0; i < S->nwheels; i++) {
    double px, pz;
    wheel_point(v, i, x, z, yaw, &px, &pz);
    double lim = (v->kind == VK_BIKE && near_access(pz, 0.3) && px > 10 && px < 13 ? S->access_step : S->max_step) + air_y;
    if (h[i] - v->wheel_h[i] > lim) return B_STEP;
    if (SEA_LEVEL - h[i] > S->max_depth && h[i] < v->wheel_h[i]) return B_DEEP;   // (heading back out is fine)
  }
  double gy = v->ground_y;
  for (int k = 0; k < S->ncircles; k++) {
    double lz = S->circles[k][0], r = S->circles[k][1];
    double cx = x + lz * s, cz = z + lz * c;
    if (cx - r < fmax(w->bx0, S->x_min) || cx + r > w->bx1 || cz - r < w->bz0 || cz + r > w->bz1) return B_BOUNDS;
    VGrid *g = w->grid;
    for (int i = (int)floor((cx - r) / CELL); i <= (int)floor((cx + r) / CELL); i++)
      for (int j = (int)floor((cz - r) / CELL); j <= (int)floor((cz + r) / CELL); j++) {
        GCell *cell = cell_get(g, i, j, false);
        if (!cell) continue;
        for (size_t t = 0; t < cell->items.len; t++) {
          const GItem *q = &g->items.data[cell->items.data[t]];
          if (q->box) {
            if (q->b.max.y < gy + 0.12 + air_y || q->b.min.y > gy + 1.3) continue;
            double qx = clampv(cx, q->b.min.x, q->b.max.x), qz = clampv(cz, q->b.min.z, q->b.max.z);
            if ((cx - qx) * (cx - qx) + (cz - qz) * (cz - qz) < r * r) return B_WALL;
          } else {
            double rr = r + q->c.r;
            if ((cx - q->c.x) * (cx - q->c.x) + (cz - q->c.z) * (cz - q->c.z) < rr * rr) return B_WALL;
          }
        }
      }
    for (int d = 0; d < w->ndyn; d++) {
      const WalkCircle *q = w->dyn[d].c;
      if (w->dyn[d].owner == v || !(q->r > 0)) continue;
      double rr = r + q->r;
      if ((cx - q->x) * (cx - q->x) + (cz - q->z) * (cz - q->z) < rr * rr) return B_WALL;
    }
  }
  return B_NONE;
}
bool vehicle_blocked(Vehicle *v, double x, double z, double yaw, const VWorld *w, double air_y) {
  return blocked_at(v, x, z, yaw, w, air_y) != B_NONE;
}

static int surf_index(VSurf k) { return k == VS_PAVEMENT ? S_PAVEMENT : k == VS_GRASS ? S_GRASS : k == VS_WETSAND ? S_WETSAND : S_SAND; }

static double vmax_for(const Vehicle *v, bool hard) {
  const VSpec *S = v->spec;
  const VSurface *su = &v->surf;
  int k = hard ? 1 : 0;
  if (su->kind == VS_WATER) return S->vmax[S_WETSAND][k] * clampv(1 - su->depth / S->water_k, 0.08, 1);
  if (su->kind == VS_SAND || su->kind == VS_WETSAND) return lerp(S->vmax[S_WETSAND][k], S->vmax[S_SAND][k], su->soft);
  return S->vmax[surf_index(su->kind)][k];
}
static double roll_for(const Vehicle *v) {
  const VSpec *S = v->spec;
  const VSurface *su = &v->surf;
  if (su->kind == VS_WATER) return S->roll[S_WETSAND] + 3 * su->depth;
  if (su->kind == VS_SAND || su->kind == VS_WETSAND) return lerp(S->roll[S_WETSAND], S->roll[S_SAND], su->soft);
  return S->roll[surf_index(su->kind)];
}

static void substep(Vehicle *v, VInput input, double h, const VWorld *w, bool hard) {
  const VSpec *S = v->spec;
  double sy = sin(v->yaw), cy = cos(v->yaw);
  double fx = -sy, fz = -cy, rx = cy, rz = -sy;
  double lon = v->vx * fx + v->vz * fz, lat = v->vx * rx + v->vz * rz;
  double vmax = vmax_for(v, hard);
  double thr = input.throttle;
  double a = 0;
  if (thr > 0.05) {
    if (lon < -0.3) a = S->brake * thr;
    else {
      // drive force tapers to zero at the surface's top speed (resistance included)
      double q = lon / (vmax * fmax(0.35, thr));
      a = (hard ? S->accel_hard : S->accel) * thr * clampv(1 - q * q * fabs(q), -2, 1);
    }
  } else if (thr < -0.05) {
    if (lon > 0.3) a = -S->brake * -thr;
    else a = -S->rev_accel * -thr * clampv(1 + lon / S->rev_max, -2, 1);
  }
  // gravity along the slope between the wheels (the access steps count as a ramp)
  a -= G * clampv(sin(v->pitch_t), -0.1, 0.1) * 0.8;
  lon += a * h;
  bool driving = (thr > 0.05 && lon > 0.3) || (thr < -0.05 && lon < -0.3);
  double res = (driving ? 0 : roll_for(v) + (fabs(thr) < 0.05 ? S->engine_brake : 0)) * h;
  lon = js_sign(lon) * fmax(0, fabs(lon) - res);
  double soft = v->surf.kind == VS_SAND ? v->surf.soft : 0;
  lat *= exp(-h * lerp(S->grip_hard, S->grip_soft, soft));
  // steering: less lock at speed; soft sand makes the bike wander
  double al = fabs(lon);
  double q = al / S->steer_v;
  double lock = S->steer_max / (1 + q * q);
  v->steer += (input.steer * lock - v->steer) * (1 - exp(-h * S->steer_rate));
  double d = v->steer;
  if (v->kind == VK_BIKE && soft > 0 && al > 0.3) d += soft * (0.05 + 0.07 * fmax(0, 1 - al / 3)) * sin(v->t * 6.1 + 2 * sin(v->t * 2.3));
  double yaw = v->yaw - lon * tan(d) / S->wheelbase * h;
  double vx = fx * lon + rx * lat, vz = fz * lon + rz * lat;
  double nx = v->x + vx * h, nz = v->z + vz * h;
  double air = v->air_y;
  // a blocked turn (a tail swinging into a post) still lets the vehicle roll on straight
  double Y = v->yaw;
  if (!blocked_at(v, nx, nz, yaw, w, air)) { v->x = nx; v->z = nz; v->yaw = yaw; }
  else if (!blocked_at(v, nx, nz, Y, w, air)) { v->x = nx; v->z = nz; }
  else if (!blocked_at(v, nx, v->z, Y, w, air)) { v->bump = fmax(v->bump, fabs(vz)); vz = 0; v->x = nx; }
  else if (!blocked_at(v, v->x, nz, Y, w, air)) { v->bump = fmax(v->bump, fabs(vx)); vx = 0; v->z = nz; }
  else {
    v->bump = fmax(v->bump, js_hypot2(vx, vz));
    vx *= -0.25; vz *= -0.25;
    if (!blocked_at(v, v->x, v->z, yaw, w, air)) v->yaw = yaw;
  }
  v->vx = vx; v->vz = vz;
  wheel_heights(v, v->x, v->z, v->yaw, v->wheel_h);
  targets(v);
  v->ground_y = v->base_t;
}

static void post(Vehicle *v, VInput input, double dt, const VWorld *w, bool hard) {
  const VSpec *S = v->spec;
  double sy = sin(v->yaw), cy = cos(v->yaw);
  double lon = v->vx * -sy + v->vz * -cy;
  double dlon = lon - v->lon;
  v->lon = lon;
  double al = fabs(lon);
  // body: sprung toward the wheel contacts; past full droop it falls freely
  double K = v->kind == VK_BIKE ? 220 : 70, C = 2 * (v->kind == VK_BIKE ? 0.6 : 0.35) * sqrt(K);
  double comp = v->base_t - v->body_y;
  bool was_air = comp < -0.1;
  double acc = comp > -0.1 ? K * comp - C * v->body_v : -G;
  v->body_v += acc * dt;
  v->body_y += v->body_v * dt;
  if (v->body_y < v->base_t - 0.12) {
    if (v->body_v < -1.5) v->land = fmax(v->land, -v->body_v);
    v->body_y = v->base_t - 0.12;
    v->body_v = fmax(0, v->body_v);
  }
  if (was_air && v->base_t - v->body_y >= -0.1 && v->body_v < -1) v->land = fmax(v->land, -v->body_v);
  // pitch / roll springs (with squat and dive from the acceleration)
  double accel = dlon / dt;
  double pT = v->pitch_t + (v->kind == VK_ATV ? clampv(accel, -8, 8) * 0.006 : 0);
  double Kp = v->kind == VK_BIKE ? 260 : 90, Cp = 2 * 0.45 * sqrt(Kp);
  v->pitch_v += (Kp * (pT - v->pitch) - Cp * v->pitch_v) * dt;
  v->pitch += v->pitch_v * dt;
  double yaw_prev = isnan(v->yaw_) ? v->yaw : v->yaw_;
  double turn = -(fmod(v->yaw - yaw_prev + PI_D * 3, PI_D * 2) - PI_D) / dt;
  if (v->kind == VK_ATV) {
    // body roll: terrain plus a lean out of the turn
    double rT = v->roll_t + clampv(lon * turn, -9, 9) * 0.008;
    v->roll_v += (90 * (rT - v->roll) - 2 * 0.4 * sqrt(90) * v->roll_v) * dt;
    v->roll += v->roll_v * dt;
    v->turn = turn;
  } else {
    v->turn = turn;
    double leanT = v->parked ? -0.13 : clampv(atan(lon * turn / G), -0.55, 0.55);
    if (!v->parked && v->surf.kind == VS_SAND && al > 0.3) leanT += v->surf.soft * 0.03 * sin(v->t * 4.7 + 1.3 * sin(v->t * 1.9));
    double k = v->parked ? 5 : 8;
    v->lean += (leanT - v->lean) * (1 - exp(-dt * k));
    v->roll = -v->lean + v->roll_t;
  }
  v->yaw_ = v->yaw;
  v->wheel_rot += lon / S->wheel_r * dt;
  // pedalling (single speed, 44/18): the cranks turn only while pedalling forward
  if (v->kind == VK_BIKE) {
    bool pedalling = input.throttle > 0.05 && lon > -0.1;
    v->pedal += ((pedalling ? 1 : 0) - v->pedal) * (1 - exp(-dt * 8));
    if (pedalling) v->crank += fmax(lon, 0.6) / (S->wheel_r * 2.44) * dt * (hard ? 1.05 : 1);
    v->coasting = !pedalling && lon > 0.4;
  } else {
    // CVT: revs rise with throttle, then with road speed
    double thr = fmax(0, input.throttle);
    double target = S->idle + thr * (hard ? 3600 : 2800) + al * 330 + (thr > 0 ? 400 : 0);
    v->rpm += (clampv(target, S->idle, S->redline) - v->rpm) * (1 - exp(-dt * (target > v->rpm ? 5 : 2.5)));
    v->load = thr * clampv(1 - al / (vmax_for(v, hard) + 0.5), 0.15, 1);
  }
  // water depth under the wheels (spray, splash sound)
  for (int i = 0; i < S->nwheels; i++) {
    double px, pz;
    wheel_point(v, i, v->x, v->z, v->yaw, &px, &pz);
    v->wheel_depth[i] = px > SAND.x0 + 30 ? vehicle_water_depth(w, px, pz) : 0;
  }
  vehicle_sync_circles(v);
}

// input: throttle -1..1, steer -1..1 (+ = right), hard (Shift / boost), hop (edge)
void vehicle_step(Vehicle *v, VInput input, double dt, const VWorld *w) {
  dt = fmin(dt, 0.05);
  if (!(dt > 0)) return;
  v->bump = 0; v->land = 0;
  v->t += dt;
  double c = js_hypot2(v->x - (isnan(v->sx_) ? 1e9 : v->sx_), v->z - (isnan(v->sz_) ? 1e9 : v->sz_));
  if (c > 0.25 || v->sf_ < 0 || ++v->sf_ > 3) { surface_at(v->x, v->z, w, &v->surf); v->sx_ = v->x; v->sz_ = v->z; v->sf_ = 0; }
  if (input.hop && v->kind == VK_ATV) v->boost_t = 1.3;
  v->boost_t = fmax(0, v->boost_t - dt);
  bool hard = input.hard || v->boost_t > 0;
  // hop (bike): ballistic lift; ledges up to the hop height can be cleared
  if (v->kind == VK_BIKE) {
    if (input.hop && v->grounded && v->air_y <= 0) { v->vy_air = 2.3; v->grounded = false; }
    if (v->air_y > 0 || v->vy_air > 0) {
      v->vy_air -= G * dt;
      v->air_y += v->vy_air * dt;
      if (v->air_y <= 0) { v->land = -v->vy_air; v->air_y = 0; v->vy_air = 0; v->grounded = true; v->body_v -= v->land * 0.25; }
    }
  }
  if (v->parked) { v->vx = v->vz = 0; input = (VInput){ 0, 0, false, false }; }
  double speed = js_hypot2(v->vx, v->vz);
  int n = (int)clampv(ceil(fmax(speed, 0.5) * dt / 0.08), 1, 12);
  double h = dt / n;
  for (int i = 0; i < n; i++) substep(v, input, h, w, hard);
  v->throttle = input.throttle;
  post(v, input, dt, w, hard);
}

void vehicle_dismount_spots(const Vehicle *v, double out[8][2]) {
  double wd = v->kind == VK_BIKE ? 0.75 : 1.15, l = v->kind == VK_BIKE ? 1.3 : 1.7;
  const double L[8][2] = { { -wd, 0 }, { wd, 0 }, { -wd, 0.5 }, { wd, 0.5 }, { 0, l }, { 0, -l }, { -wd - 0.5, 0 }, { wd + 0.5, 0 } };
  for (int i = 0; i < 8; i++) {
    double s = sin(v->yaw), c = cos(v->yaw);
    out[i][0] = v->x + L[i][0] * c + L[i][1] * s;
    out[i][1] = v->z - L[i][0] * s + L[i][1] * c;
  }
}
