// Port of src/world/surf.js. Event timing mirrors audio/waves.js breakAt(): with tempo k, the lip
// hits the water at 2.15k s, foam runs up the sand from 2.2k for 2.3k s, recedes from 4.5k over
// 4k s. (The GLSL half of surf.js is part of the generated programs.)
#include "world/surf.h"

#include "world/layout.h"

static constexpr double T_WASH = 2.2;
static constexpr double T_UP = 2.3;
static constexpr double T_DOWN = 4.0;
static constexpr double T_END = 9.5;
static constexpr double T_LEAD = 5.0;

struct Surf {
  Vec(SurfEvent) events;
  Rng rnd;
  bool external;
  double tNext, now;
  V4 uA[SURF_EVENTS], uB[SURF_EVENTS];
};

static double runup_at(const SurfEvent *e, double z) {
  double R = fmin(e->runup, SWASH_MAX);
  double q = (z - e->z) / 70;
  return R * (0.8 + 0.12 * sin(z * 0.061 + e->ph) + 0.08 * sin(z * 0.17 + 2 * e->ph)) * (0.85 + 0.15 * exp(-(q * q)));
}
// landward edge of one event's water at time t (false when none)
static bool event_front(const SurfEvent *e, double z, double t, double *front, double *fresh) {
  double tau = t - e->t0, k = e->k;
  double tb = T_WASH * k, tu = T_UP * k, td = T_DOWN * k;
  if (tau < tb || tau > tb + tu + td) return false;
  double r;
  if (tau < tb + tu) {
    double u = (tau - tb) / tu;
    r = 1 - (1 - u) * (1 - u);
    *fresh = 1;
  } else {
    double d = (tau - tb - tu) / td;
    r = 1 - pow(d, 1.4);
    *fresh = 1 - d;
  }
  // lace lobes on the leading edge
  double lobes = (0.22 * sin(z * 0.83 + e->ph * 3) + 0.12 * sin(z * 2.1 + e->ph * 5) + 0.3 * fabs(sin(z * 1.9 + e->ph * 2)) - 0.15) * r;
  *front = BREAK_X - runup_at(e, z) * r + lobes;
  return true;
}

// make(t0, o): o.k / o.size / o.z are NaN when not given (the literal's property order)
static SurfEvent make(Surf *s, double t0, double k, double size, double z) {
  SurfEvent e = { .t0 = t0 };
  e.k = isnan(k) ? 0.85 + rng_next(&s->rnd) * 0.35 : k;
  e.size = isnan(size) ? 0.55 + rng_next(&s->rnd) * 0.45 : size;
  e.runup = 0;
  e.z = isnan(z) ? (rng_next(&s->rnd) - 0.5) * 80 : z;
  e.ph = rng_next(&s->rnd) * 6.283;
  return e;
}
static SurfEvent with_runup(Surf *s, SurfEvent e, double r) {
  e.runup = isnan(r) ? (4 + rng_next(&s->rnd) * 4) * e.size : r;
  return e;
}

static void extend_to(Surf *s, double t) {
  if (s->external) return;
  while (s->tNext < t + T_LEAD + 1) {
    SurfEvent e = with_runup(s, make(s, s->tNext, NAN, NAN, NAN), NAN);
    vec_push(&s->events, e);
    s->tNext += 6 + rng_next(&s->rnd) * 4;
  }
}

static bool is_active(const SurfEvent *e, double t) { return t >= e->t0 - T_LEAD && t <= e->t0 + T_END * e->k; }

Surf *surf_create(bool frozen, double anchor_time) {
  Surf *s = xcalloc(1, sizeof *s);
  s->rnd = rng_make(9121);
  // deterministic schedule: a hero wash is 3/4 of the way up the sand at the frozen shot time,
  // the next set is building offshore
  SurfEvent hero = with_runup(s, make(s, anchor_time - 3.9, 1, 0.95, -8), 6.6);
  vec_push(&s->events, hero);
  s->tNext = hero.t0 + 6.0;
  double tPrev = hero.t0;
  for (int i = 0; i < 3; i++) {
    tPrev -= 6 + rng_next(&s->rnd) * 4;
    SurfEvent e = with_runup(s, make(s, tPrev, NAN, NAN, NAN), NAN);
    vec_insert(&s->events, 0, e);   // events.unshift
  }
  for (int i = 0; i < SURF_EVENTS; i++) { s->uA[i] = v4(-1e4, 1, 0, 0); s->uB[i] = v4(0, 0, 0, 0); }
  s->now = frozen ? anchor_time : 0;
  surf_update(s, s->now);
  return s;
}

double surf_time(const Surf *s) { return s->now; }
const SurfEvent *surf_events(const Surf *s, int *n) {
  *n = (int)s->events.len;
  return s->events.data;
}

void surf_push_audio_wave(Surf *s, double visual_t0, double k, double size, double runup, double z) {
  if (!s->external) {
    s->external = true;
    // drop deterministic waves that have not started yet
    for (size_t i = s->events.len; i-- > 0;)
      if (s->events.data[i].t0 > s->now) vec_remove(&s->events, i);
  }
  SurfEvent e = make(s, visual_t0, k, size, z);
  e.runup = runup;
  vec_push(&s->events, e);
  while (s->events.len > 24) vec_remove(&s->events, 0);
}

void surf_update(Surf *s, double t) {
  s->now = t;
  extend_to(s, t);
  // active(t).sort((a, b) => b.t0 - a.t0).slice(0, SURF_EVENTS) (stable)
  const SurfEvent *act[64];
  int n = 0;
  for (size_t i = 0; i < s->events.len && n < 64; i++)
    if (is_active(&s->events.data[i], t)) act[n++] = &s->events.data[i];
  for (int i = 1; i < n; i++) {
    const SurfEvent *x = act[i];
    int j = i - 1;
    while (j >= 0 && x->t0 - act[j]->t0 > 0) { act[j + 1] = act[j]; j--; }
    act[j + 1] = x;
  }
  for (int i = 0; i < SURF_EVENTS; i++) {
    if (i < n) {
      const SurfEvent *e = act[i];
      s->uA[i] = v4(e->t0, e->k, e->size, fmin(e->runup, SWASH_MAX));
      s->uB[i] = v4(e->z, e->ph, 0, 0);
    } else {
      s->uA[i] = v4(-1e4, 1, 0, 0);
      s->uB[i] = v4(0, 0, 0, 0);
    }
  }
}

void surf_apply(const Surf *s, Material *m) {
  for (int i = 0; i < SURF_EVENTS; i++) {
    char name[32];
    snprintf(name, sizeof name, "uSurfA[%d]", i);
    mat_set_vec4(m, name, s->uA[i]);
    snprintf(name, sizeof name, "uSurfB[%d]", i);
    mat_set_vec4(m, name, s->uB[i]);
  }
  mat_set_float(m, "uSurfT", s->now);
}

Swash surf_swash_at(const Surf *s, double x, double z, double t) {
  bool have = false;
  double bf = 0, bfresh = 0;
  for (size_t i = 0; i < s->events.len; i++) {
    const SurfEvent *e = &s->events.data[i];
    if (!is_active(e, t)) continue;
    double f, fr;
    if (event_front(e, z, t, &f, &fr) && (!have || f < bf)) { bf = f; bfresh = fr; have = true; }
  }
  if (!have || x < bf) return (Swash){ false, 0, 0, have ? bf : BREAK_X, 0 };
  double d = x - bf;
  double depth = fmin(0.12, 0.012 + d * 0.022) * (0.35 + 0.65 * bfresh);
  return (Swash){ true, depth, exp(-d / 0.5) * (0.4 + 0.6 * bfresh), bf, bfresh };
}

double surf_water_depth_at(const Surf *s, double x, double z, double t) {
  double ground = sandHeight(x) + sandDetail(x, z);
  double sea = fmax(0, SEA_LEVEL - ground);
  Swash sw = surf_swash_at(s, x, z, t);
  return fmax(sea, sw.covered ? sw.depth : 0);
}
