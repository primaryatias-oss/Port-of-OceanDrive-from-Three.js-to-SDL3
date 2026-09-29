#include "audio/car_audio.h"

#include <math.h>

#include "world/layout.h"

static constexpr double C_SOUND = 343;

typedef struct Cb { void (*fn)(AudioCar *, void *); void *user; } Cb;

struct AudioCars {
  AudioEnv *env;
  Vec(AudioCar *) cars;
  Vec(Cb) pass_cbs, end_cbs;
  int ids;
};

typedef struct CarRec { AudioCars *owner; AudioCar car; Spatial *sp; } CarRec;

AudioCars *create_cars(AudioEnv *env) {
  AudioCars *c = xcalloc(1, sizeof *c);
  c->env = env;
  return c;
}

double audio_car_z(const AudioCar *car) {
  double z = car->z0 + car->dir * car->speed * (wa_now() - car->t0);
  return clamp_d(z, fmin(car->z0, car->z1), fmax(car->z0, car->z1));
}
double audio_car_progress(const AudioCar *car) { return clamp_d((wa_now() - car->t0) / car->duration, 0, 1); }

static void car_pos(void *user, double *x, double *y, double *z) {
  const AudioCar *car = user;
  *x = car->x;
  *y = car->y;
  *z = audio_car_z(car);
}

static void car_ended(void *user) {
  CarRec *r = user;
  AudioCars *c = r->owner;
  spatial_dispose(r->sp);
  r->car.active = false;
  for (size_t i = 0; i < c->cars.len; i++)
    if (c->cars.data[i] == &r->car) { vec_remove(&c->cars, i); break; }
  for (size_t i = 0; i < c->end_cbs.len; i++) c->end_cbs.data[i].fn(&r->car, c->end_cbs.data[i].user);
  wa_release(r->car.dop);
  free(r);
}

AudioCar *cars_spawn(AudioCars *c, CarSpawn o) {
  AudioEnv *env = c->env;
  const Buffers *B = &env->B;
  double dir = o.dir ? o.dir : wa_random() < 0.6 ? 1 : -1;
  double speed = isnan(o.speed) ? (25 / 3.6) * rr(0.9, 1.1) : o.speed;
  double t0 = (isnan(o.at) ? wa_now() : o.at) + 0.05;
  double x = dir > 0 ? LANES.centerX - 1.75 : LANES.centerX + 1.75;
  double z0 = isnan(o.z_start) ? -400 * dir : o.z_start, z1 = 400 * dir;
  double T = fabs(z1 - z0) / speed;
  double y = 0.6;
  SpatialOpts so = spatial_opts();
  so.x = x; so.y = y; so.z = z0; so.ref = 6; so.rolloff = 1; so.air_scale = 28; so.wet = 0.14; so.wet_fall = 50;
  Spatial *sp = create_spatial(env, so);
  wa_set_at(wa_param(sp->panner, WP_POSITION_Z), z0, t0);
  wa_linear(wa_param(sp->panner, WP_POSITION_Z), z1, t0 + T);

  CarRec *r = xcalloc(1, sizeof *r);
  r->owner = c;
  r->sp = sp;
  AudioCar *car = &r->car;
  *car = (AudioCar){ ++c->ids, x, y, dir, speed, t0, T, z0, z1, true, nullptr };
  sp->pos_fn = car_pos;
  sp->pos_user = car;

  WaNode *out = wa_gain(0);
  double fade = fmin(4, T / 4);
  RAMP(wa_param(out, WP_GAIN), t0, { 0, 0 }, { fade, 1 }, { T - fade, 1 }, { T, 0 });
  wa_connect(out, sp->input);

  WaNode *dop = wa_constant(0);
  WaNode *wob = wa_osc(WA_SINE, rr(0.15, 0.35));
  WaNode *wobG = wa_gain(18);
  wa_connect(wob, wobG);
  WaNode *detuned[5];
  int nd = 0;

  double f = rr(1300, 1650) / 30;   // 4-stroke, 4 cylinders: firing frequency = rpm / 30
  WaNode *eng = wa_gain(0.4);
  wa_connect(eng, out);
  struct { WaOscType type; double freq, g, lp; } partials[4] = {
    { WA_SAWTOOTH, f, 0.5, 260 }, { WA_SINE, 2 * f, 0.22, 0 }, { WA_SINE, f / 2, 0.28, 0 }, { WA_TRIANGLE, 3 * f, 0.06, 0 },
  };
  WaNode *oscs[4];
  for (int i = 0; i < 4; i++) {
    WaNode *os = wa_osc(partials[i].type, partials[i].freq);
    WaNode *gn = wa_gain(partials[i].g);
    if (partials[i].lp) wa_chain(os, bq(WA_LOWPASS, partials[i].lp, 0.9), gn, eng);
    else wa_chain(os, gn, eng);
    detuned[nd++] = os;
    oscs[i] = os;
  }
  // intake / mechanical putter: noise amplitude-modulated at the firing rate
  WaNode *put = wa_gain(0.1);
  WaNode *fire = wa_osc(WA_SINE, f);
  wa_connect_param(wa_connect(fire, wa_gain(0.09)), wa_param(put, WP_GAIN));
  detuned[nd++] = fire;
  WaNode *putN = loop_src(B->pink, 1, t0, NAN);
  wa_chain(putN, bq(WA_BANDPASS, 420, 1.2), put, eng);

  // tyres on asphalt
  WaNode *roar = loop_src(B->pink, 1, t0, NAN);
  wa_chain(roar, bq(WA_BANDPASS, 750, 0.5), wa_gain(0.22), out);
  WaNode *hiss = loop_src(B->white, 1, t0, NAN);
  wa_chain(hiss, bq(WA_BANDPASS, 2800, 0.8), wa_gain(0.04), out);
  WaNode *road = loop_src(B->brown, 1, t0, NAN);
  wa_chain(road, bq(WA_LOWPASS, 110, 0.7), wa_gain(0.35), out);
  WaNode *noises[4] = { putN, roar, hiss, road };

  for (int i = 0; i < nd; i++) { wa_connect_param(dop, wa_param(detuned[i], WP_DETUNE)); wa_connect_param(wobG, wa_param(detuned[i], WP_DETUNE)); }
  for (int i = 0; i < 4; i++) { wa_connect_param(dop, wa_param(noises[i], WP_DETUNE)); wa_connect_param(wobG, wa_param(noises[i], WP_DETUNE)); }
  wa_start(dop, t0, 0);
  wa_start(wob, t0, 0);
  for (int i = 0; i < nd; i++) wa_start(detuned[i], t0, 0);
  wa_stop(dop, t0 + T + 0.1);
  wa_stop(wob, t0 + T + 0.1);
  for (int i = 0; i < nd; i++) wa_stop(detuned[i], t0 + T + 0.1);
  for (int i = 0; i < 4; i++) wa_stop(noises[i], t0 + T + 0.1);

  car->dop = dop;
  wa_hold(dop);
  wa_on_ended(oscs[0], car_ended, r);
  vec_push(&c->cars, car);
  for (size_t i = 0; i < c->pass_cbs.len; i++) c->pass_cbs.data[i].fn(car, c->pass_cbs.data[i].user);
  return car;
}

void car_audio_update(AudioCars *c, const Listener *L) {
  double now = wa_now();
  for (size_t i = 0; i < c->cars.len; i++) {
    AudioCar *car = c->cars.data[i];
    if (now < car->t0) continue;
    double z = audio_car_z(car);
    double dx = L->x - car->x, dy = L->y - car->y, dz = L->z - z;
    double d = sqrt(dx * dx + dy * dy + dz * dz);
    if (d == 0) d = 1;
    double vr = (car->dir * car->speed * dz) / d;   // velocity component toward the listener
    double cents = 1200 * log2(C_SOUND / (C_SOUND - vr));
    wa_target(wa_param(car->dop, WP_OFFSET), cents, now, 0.05);
  }
}

int audio_cars_list(AudioCars *c, AudioCar **out, int max) {
  int n = 0;
  for (size_t i = 0; i < c->cars.len && n < max; i++) out[n++] = c->cars.data[i];
  return n;
}
void audio_cars_on_pass(AudioCars *c, void (*fn)(AudioCar *, void *), void *user) { vec_push(&c->pass_cbs, ((Cb){ fn, user })); }
void audio_cars_on_end(AudioCars *c, void (*fn)(AudioCar *, void *), void *user) { vec_push(&c->end_cbs, ((Cb){ fn, user })); }
