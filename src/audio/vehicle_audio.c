#include "audio/vehicle_audio.h"

#include <math.h>

static constexpr double PI = 3.141592653589793;

static WaBuffer *click_buffer(double f, double ms) {
  double sr = wa_sample_rate();
  int n = (int)floor((sr * ms) / 1000);
  WaBuffer *buf = wa_buffer(1, n, sr);
  float *d = buf->data[0];
  for (int i = 0; i < n; i++) {
    double env = exp(-i / (sr * 0.0011));
    d[i] = (float)(((wa_random() * 2 - 1) * 0.5 + sin((2 * PI * f * i) / sr) * 0.8) * env);
  }
  return buf;
}

// exhaust pulse: strong low harmonics, a formant hump, falling off above
static WaWave *exhaust_wave(void) {
  enum { N = 90 };
  float re[N] = {}, im[N] = {};
  for (int k = 1; k < N; k++) {
    double q = (k - 7) / 3.0;
    im[k] = (float)((1 / pow(k, 0.62)) * (1 + 0.8 * exp(-(q * q))) * (k % 2 ? 1 : 0.75));
  }
  return wa_periodic_wave(re, im, N);
}

typedef struct NoiseV { WaNode *s, *g; } NoiseV;

typedef struct Bike {
  WaNode *g;
  NoiseV hiss, rumble, crunch, crunch_low, swish, water, spray, chain_n;
  WaNode *click_g;
  WaBuffer *clicks[3], *tick;
  double next;
  long half;
} Bike;

typedef struct Atv {
  WaNode *g, *eng, *osc, *sub, *am, *lp, *pulse_g, *put, *whine, *whine_g, *starter, *st, *hum, *hum_g;
  NoiseV valve, crunch, rumble, water, spray;
  bool on;
  double start_at;
} Atv;

struct VehicleAudio {
  AudioEnv *env;
  WaNode *out;
  Bike *bike;
  Atv *atv;
  struct { NoiseV wind; WaNode *thump_bus; } *common;
  RideKind kind;
};

static WaNode *G(double v) { return wa_gain(v); }
static void set(WaParam *p, double v, double tc) { wa_target(p, v, wa_now(), tc); }
#define SET(p, v) set((p), (v), 0.06)

// a looping noise through filters into a held gain (the ride keeps setting it)
static NoiseV noise(WaBuffer *buf, WaNode *const *filters, int nf, WaNode *dest, double rate) {
  WaNode *s = loop_src(buf, rate, NAN, NAN);
  WaNode *g = G(0);
  WaNode *prev = s;
  for (int i = 0; i < nf; i++) prev = wa_connect(prev, filters[i]);
  wa_connect(wa_connect(prev, g), dest);
  wa_hold(s);
  wa_hold(g);
  return (NoiseV){ s, g };
}
#define NOISE1(buf, f1, dest) noise((buf), (WaNode *const[]){ f1 }, 1, (dest), 1)

VehicleAudio *create_vehicle_audio(AudioEnv *env) {
  VehicleAudio *v = xcalloc(1, sizeof *v);
  v->env = env;
  v->out = G(1);
  wa_hold(v->out);
  wa_connect(v->out, env->dry);
  wa_chain(v->out, G(0.06), env->reverb);
  return v;
}

static void make_common(VehicleAudio *v) {
  v->common = xcalloc(1, sizeof *v->common);
  v->common->wind = NOISE1(v->env->B.pink, bq(WA_LOWPASS, 520, 0.6), v->out);
  v->common->thump_bus = G(1);
  wa_hold(v->common->thump_bus);
  wa_connect(v->common->thump_bus, v->out);
}

static void thump(VehicleAudio *V, double strength, double rattle) {
  const Buffers *B = &V->env->B;
  double t = wa_now() + 0.005, v = clamp_d(strength, 0, 1.4);
  WaNode *s = wa_buffer_source(B->brown, false, 1), *g = G(0);
  wa_chain(s, bq(WA_LOWPASS, 220, 0.8), g, V->common->thump_bus);
  WaParam *gg = wa_param(g, WP_GAIN);
  wa_set_at(gg, 0, t); wa_linear(gg, 0.9 * v, t + 0.006); wa_target(gg, 0, t + 0.006, 0.05);
  wa_start(s, t, wa_random() * 5);
  wa_stop(s, t + 0.4);
  if (rattle) {
    WaNode *r = wa_buffer_source(B->crackle_dense, false, rr(0.9, 1.2)), *rg = G(0);
    wa_chain(r, bq(WA_BANDPASS, rattle, 1.4), rg, V->common->thump_bus);
    WaParam *rp = wa_param(rg, WP_GAIN);
    wa_set_at(rp, 0, t); wa_linear(rp, 0.5 * v, t + 0.01); wa_target(rp, 0, t + 0.02, 0.07);
    wa_start(r, t, wa_random() * 3);
    wa_stop(r, t + 0.5);
  }
}

static Bike *make_bike(VehicleAudio *V) {
  const Buffers *B = &V->env->B;
  Bike *b = xcalloc(1, sizeof *b);
  b->g = G(0);
  wa_hold(b->g);
  wa_connect(b->g, V->out);
  b->hiss = NOISE1(B->white, bq(WA_BANDPASS, 3400, 0.6), b->g);
  b->rumble = NOISE1(B->brown, bq(WA_LOWPASS, 150, 0.7), b->g);
  b->crunch = NOISE1(B->crackle_dense, bq(WA_BANDPASS, 1500, 0.9), b->g);
  b->crunch_low = NOISE1(B->brown, bq(WA_LOWPASS, 320, 0.7), b->g);
  b->swish = NOISE1(B->pink, bq(WA_BANDPASS, 2600, 0.6), b->g);
  b->water = NOISE1(B->pink, bq(WA_BANDPASS, 700, 1), b->g);
  b->spray = NOISE1(B->white, bq(WA_BANDPASS, 2300, 0.8), b->g);
  b->chain_n = NOISE1(B->crackle, bq(WA_BANDPASS, 4300, 2.5), b->g);
  b->click_g = G(0.5);
  wa_hold(b->click_g);
  wa_connect(b->click_g, b->g);
  b->clicks[0] = click_buffer(4200, 5);
  b->clicks[1] = click_buffer(3900, 5);
  b->clicks[2] = click_buffer(4500, 5);
  b->tick = click_buffer(1900, 9);
  return b;
}

static Atv *make_atv(VehicleAudio *V) {
  const Buffers *B = &V->env->B;
  Atv *a = xcalloc(1, sizeof *a);
#define HOLD(n) (wa_hold(n), (n))
  a->g = HOLD(G(0));
  wa_connect(a->g, V->out);
  a->eng = HOLD(G(0));
  wa_connect(a->eng, a->g);
  a->osc = HOLD(wa_osc(WA_SINE, 12));
  wa_osc_set_wave(a->osc, exhaust_wave());
  float curve[1024];
  for (int i = 0; i < 1024; i++) curve[i] = (float)tanh(((double)i / 1023 * 2 - 1) * 2.2);
  WaNode *shaper = wa_shaper(curve, 1024, false);
  a->lp = HOLD(bq(WA_LOWPASS, 700, 0.9));
  WaNode *peak = bqg(WA_PEAKING, 170, 1.2, 5);
  a->pulse_g = HOLD(G(0.55));
  wa_chain(a->osc, shaper, a->lp, peak, a->pulse_g, a->eng);
  a->sub = HOLD(wa_osc(WA_SINE, 6));
  wa_chain(a->sub, G(0.3), a->eng);
  // intake / mechanical: noise gated at the firing rate
  a->put = HOLD(G(0.08));
  a->am = HOLD(wa_osc(WA_SQUARE, 12));
  wa_connect_param(wa_connect(a->am, G(0.07)), wa_param(a->put, WP_GAIN));
  WaNode *putN = loop_src(B->pink, 1, NAN, NAN);
  wa_chain(putN, bq(WA_BANDPASS, 1300, 1.1), a->put, a->eng);
  a->valve = NOISE1(B->white, bq(WA_HIGHPASS, 5200, 0.7), a->eng);
  // unsteady idle: slow random detune on the firing oscillators
  WaNode *flutter = wa_buffer_source(B->flutter, true, 0.6);
  WaNode *flG = G(55);
  wa_connect(flutter, flG);
  WaNode *fo[3] = { a->osc, a->sub, a->am };
  for (int i = 0; i < 3; i++) wa_connect_param(flG, wa_param(fo[i], WP_DETUNE));
  a->whine = HOLD(wa_osc(WA_TRIANGLE, 200));
  a->whine_g = HOLD(G(0));
  wa_chain(a->whine, bq(WA_BANDPASS, 900, 3), a->whine_g, a->g);
  a->starter = HOLD(G(0));
  a->st = HOLD(wa_osc(WA_SAWTOOTH, 38));
  wa_chain(a->st, bq(WA_BANDPASS, 320, 1.5), a->starter, a->g);
  a->crunch = NOISE1(B->crackle_dense, bq(WA_BANDPASS, 950, 0.8), a->g);
  a->rumble = NOISE1(B->brown, bq(WA_LOWPASS, 130, 0.7), a->g);
  a->hum = HOLD(wa_osc(WA_TRIANGLE, 60));
  a->hum_g = HOLD(G(0));
  wa_chain(a->hum, bq(WA_LOWPASS, 400, 0.8), a->hum_g, a->g);
  a->water = NOISE1(B->pink, bq(WA_BANDPASS, 620, 0.9), a->g);
  a->spray = NOISE1(B->white, bq(WA_BANDPASS, 2100, 0.7), a->g);
#undef HOLD
  WaNode *starts[6] = { a->osc, a->sub, a->am, a->whine, a->st, a->hum };
  double now = wa_now();
  for (int i = 0; i < 6; i++) wa_start(starts[i], now, 0);
  wa_start(flutter, now, 0);
  return a;
}

static void switch_to(VehicleAudio *V, RideKind k) {
  double now = wa_now();
  if (V->kind == RIDE_ATV && V->atv) {
    // engine off: revs sag, then silence
    wa_target(wa_param(V->atv->eng, WP_GAIN), 0, now, 0.18);
    wa_target(wa_param(V->atv->osc, WP_FREQUENCY), 6, now, 0.3);
    V->atv->on = false;
  }
  WaNode *gs[2] = { V->bike ? V->bike->g : nullptr, V->atv ? V->atv->g : nullptr };
  for (int i = 0; i < 2; i++) {
    if (!gs[i]) continue;
    bool mine = k != RIDE_NONE && i == (k == RIDE_BIKE ? 0 : 1);
    wa_target(wa_param(gs[i], WP_GAIN), mine ? 1 : 0, now + (V->kind == RIDE_ATV ? 0.4 : 0), 0.15);
  }
  if (k == RIDE_ATV) {
    Atv *a = V->atv;
    WaParam *g = wa_param(a->g, WP_GAIN), *sg = wa_param(a->starter, WP_GAIN), *sf = wa_param(a->st, WP_FREQUENCY);
    wa_cancel(g, now);
    wa_target(g, 1, now, 0.03);
    wa_cancel(sg, now);
    wa_set_at(sg, 0, now);
    wa_linear(sg, 0.22, now + 0.05);
    wa_set_at(sg, 0.22, now + 0.5);
    wa_linear(sg, 0, now + 0.62);
    wa_set_at(sf, 30, now);
    wa_linear(sf, 52, now + 0.55);
    a->start_at = now + 0.55;
  }
  V->kind = k;
}

static void update_bike(VehicleAudio *V, const RideState *s) {
  Bike *b = V->bike;
  double now = wa_now(), v = fabs(s->lon);
  double sv = fmin(1, v / 6);
  bool on_sand = s->surface == RS_SAND || s->surface == RS_WETSAND;
  double pave = s->surface == RS_PAVEMENT ? 1 : 0, grass = s->surface == RS_GRASS ? 1 : 0, water = s->surface == RS_WATER ? 1 : 0;
  SET(wa_param(b->hiss.g, WP_GAIN), pave * 0.05 * sv * sv);
  SET(wa_param(b->rumble.g, WP_GAIN), (pave * 0.25 + grass * 0.15) * sv);
  SET(wa_param(b->crunch.g, WP_GAIN), on_sand ? (0.12 + 0.18 * s->soft) * fmin(1, v / 3) : 0);
  wa_target(wa_param(b->crunch.s, WP_PLAYBACK_RATE), 0.7 + 0.2 * fmin(1, v / 4), now, 0.1);
  SET(wa_param(b->crunch_low.g, WP_GAIN), on_sand ? 0.25 * s->soft * fmin(1, v / 3) : 0);
  SET(wa_param(b->swish.g, WP_GAIN), grass * 0.04 * sv);
  double wd = fmin(1, s->depth / 0.12);
  SET(wa_param(b->water.g, WP_GAIN), (water || s->depth > 0.01 ? 1 : 0) * 0.35 * wd * fmin(1, v / 2.5));
  SET(wa_param(b->spray.g, WP_GAIN), water * 0.08 * wd * fmin(1, v / 3));
  SET(wa_param(b->chain_n.g, WP_GAIN), s->pedal * 0.35 * fmin(1, v / 2));
  // freewheel pawls: 18 clicks per wheel turn, only while coasting
  double rate = s->coasting ? (v / (2 * PI * 0.335)) * 18 : 0;
  if (rate > 1.5) {
    if (b->next < now) b->next = now + 0.01;
    while (b->next < now + 0.08) {
      WaBuffer *buf = b->clicks[(int)(wa_random() * 3)];
      WaNode *src = wa_buffer_source(buf, false, rr(0.94, 1.06));
      WaNode *cg = G(0.16 + 0.06 * wa_random());
      wa_chain(src, cg, b->click_g);
      wa_start(src, b->next, 0);
      b->next += (1 / rate) * rr(0.9, 1.1);
    }
  } else b->next = 0;
  // a light chain tick on each pedal stroke
  long half = (long)floor(s->crank / PI);
  if (half != b->half && s->pedal > 0.5) {
    WaNode *src = wa_buffer_source(b->tick, false, rr(0.9, 1.1));
    wa_chain(src, G(0.12), b->g);
    wa_start(src, now + 0.005, 0);
  }
  b->half = half;
}

static void update_atv(VehicleAudio *V, const RideState *s) {
  Atv *a = V->atv;
  double now = wa_now(), v = fabs(s->lon);
  bool running = now >= a->start_at;
  if (running && !a->on) {
    a->on = true;
    wa_cancel(wa_param(a->eng, WP_GAIN), now);
  }
  double fire = s->rpm / 120, rn = clamp_d((s->rpm - 1400) / 6200, 0, 1);
  if (a->on) {
    wa_target(wa_param(a->osc, WP_FREQUENCY), fire, now, 0.03);
    wa_target(wa_param(a->sub, WP_FREQUENCY), fire / 2, now, 0.03);
    wa_target(wa_param(a->am, WP_FREQUENCY), fire, now, 0.03);
    wa_target(wa_param(a->lp, WP_FREQUENCY), 420 + rn * 1500 + s->load * 900, now, 0.05);
    set(wa_param(a->eng, WP_GAIN), 0.34 * (0.5 + 0.35 * s->load + 0.35 * rn), 0.05);
    SET(wa_param(a->valve.g, WP_GAIN), 0.012 + 0.02 * rn);
    wa_target(wa_param(a->whine, WP_FREQUENCY), (s->rpm / 60) * 3.1, now, 0.05);
    SET(wa_param(a->whine_g, WP_GAIN), 0.02 * fmin(1, v / 8));
  }
  bool on_sand = s->surface == RS_SAND || s->surface == RS_WETSAND;
  SET(wa_param(a->crunch.g, WP_GAIN), (on_sand ? 0.14 + 0.16 * s->soft : 0.05) * fmin(1, v / 5));
  SET(wa_param(a->rumble.g, WP_GAIN), 0.3 * fmin(1, v / 8));
  wa_target(wa_param(a->hum, WP_FREQUENCY), fmax(20, (v / (2 * PI * 0.31)) * 20), now, 0.05);
  SET(wa_param(a->hum_g, WP_GAIN), (on_sand ? 1 - 0.6 * s->soft : 0.6) * 0.05 * fmin(1, v / 6));
  double wd = fmin(1, s->depth / 0.15);
  SET(wa_param(a->water.g, WP_GAIN), 0.45 * wd * fmin(1, v / 3));
  SET(wa_param(a->spray.g, WP_GAIN), 0.14 * wd * fmin(1, v / 5));
}

void vehicle_audio_update(VehicleAudio *V, const RideState *s) {
  RideKind k = s ? s->kind : RIDE_NONE;
  if (k == RIDE_BIKE && !V->bike) V->bike = make_bike(V);
  if (k == RIDE_ATV && !V->atv) V->atv = make_atv(V);
  if (!V->common && k) make_common(V);
  if (k != V->kind) switch_to(V, k);
  if (!k) {
    if (V->common) SET(wa_param(V->common->wind.g, WP_GAIN), 0);
    return;
  }
  double v = fabs(s->lon);
  double q = v / 12;
  SET(wa_param(V->common->wind.g, WP_GAIN), 0.12 * fmin(1, q * q));
  if (k == RIDE_BIKE) update_bike(V, s);
  else update_atv(V, s);
  double hit = fmax(s->bump, s->land * 0.6);
  if (hit > 0.8) thump(V, hit / 5, k == RIDE_BIKE ? 3200 : 1800);
}
