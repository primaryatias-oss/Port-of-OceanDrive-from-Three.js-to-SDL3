#include "audio/footsteps.h"

#include <math.h>

#include "world/layout.h"

struct Footsteps {
  AudioEnv *env;
  WaNode *out;
  double side;
};

Footsteps *create_footsteps(AudioEnv *env) {
  Footsteps *f = xcalloc(1, sizeof *f);
  f->env = env;
  f->out = wa_gain(0.9);
  wa_hold(f->out);
  wa_connect(f->out, env->dry);
  wa_chain(f->out, wa_gain(0.07), env->reverb);
  f->side = 1;
  return f;
}

static void burst(double t, WaBuffer *buf, WaNode *const *filters, int nf, double peak, double a, double d, double rate, WaNode *dest) {
  WaNode *s = wa_buffer_source(buf, false, rate);
  WaNode *g = wa_gain(0);
  WaNode *prev = s;
  for (int i = 0; i < nf; i++) prev = wa_connect(prev, filters[i]);
  wa_connect(wa_connect(prev, g), dest);
  perc(wa_param(g, WP_GAIN), t, peak, a, d);
  wa_start(s, t, wa_random() * (wa_buffer_duration(buf) - 1));
  wa_stop(s, t + a + d * 1.5);
}
#define BURST(t, buf, filters, ...) burst((t), (buf), (WaNode *const[]){ filters }, 1, __VA_ARGS__)
#define BURST2(t, buf, f1, f2, ...) burst((t), (buf), (WaNode *const[]){ f1, f2 }, 2, __VA_ARGS__)

static void tone(double t, double f0, double f1, double peak, double a, double d, WaOscType type, WaNode *dest) {
  WaNode *o = wa_osc(type, f0);
  wa_set_at(wa_param(o, WP_FREQUENCY), f0, t);
  wa_expo(wa_param(o, WP_FREQUENCY), f1, t + a + d);
  WaNode *g = wa_gain(0);
  wa_connect(wa_connect(o, g), dest);
  perc(wa_param(g, WP_GAIN), t, peak, a, d);
  wa_start(o, t, 0);
  wa_stop(o, t + a + d * 1.5);
}

static WaNode *F(WaBiquadType type, double freq, double q) { return bq(type, freq, q); }

static void s_pavement(const Buffers *B, double t, double v, WaNode *d) {
  BURST(t, B->white, F(WA_BANDPASS, rr(2200, 3800), 1.3), 0.35 * v * rr(0.8, 1.2), 0.0008, 0.02, 1, d);
  tone(t, rr(95, 130), rr(60, 75), 0.4 * v, 0.002, 0.05, WA_SINE, d);
  BURST(t, B->pink, F(WA_BANDPASS, rr(700, 1100), 1.1), 0.35 * v, 0.001, 0.035, 1, d);
  BURST(t + 0.002, B->crackle_dense, F(WA_HIGHPASS, rr(2200, 3200), 0.7), 0.35 * v, 0.004, rr(0.05, 0.09), rr(0.8, 1.3), d);
  double tt = t + rr(0.06, 0.1);
  BURST(tt, B->white, F(WA_BANDPASS, rr(3000, 5200), 1.5), 0.28 * v * rr(0.7, 1.2), 0.0008, 0.014, 1, d);
  BURST(tt, B->pink, F(WA_BANDPASS, rr(1100, 1600), 1.2), 0.18 * v, 0.001, 0.025, 1, d);
  BURST(tt, B->crackle_dense, F(WA_HIGHPASS, 2800, 0.7), 0.2 * v, 0.003, 0.05, rr(0.9, 1.4), d);
}

static void s_sand(const Buffers *B, double t, double v, WaNode *d) {
  double a = rr(0.02, 0.04), dd = rr(0.13, 0.2);
  BURST(t, B->crackle_dense, F(WA_BANDPASS, rr(1300, 2300), 0.9), 0.9 * v, a, dd, rr(0.7, 1.1), d);
  BURST(t, B->white, F(WA_BANDPASS, rr(3500, 5500), 0.8), 0.08 * v, a, dd * 0.8, 1, d);
  BURST(t, B->brown, F(WA_LOWPASS, 260, 0.7), 0.45 * v, 0.012, 0.09, 1, d);
  if (wa_random() < 0.35) {
    double s = rr(650, 1000);
    tone(t + a * 0.6, s, s * rr(1.05, 1.2), 0.02 * v, 0.02, 0.06, WA_TRIANGLE, d);
  }
  double tt = t + rr(0.07, 0.11);
  BURST(tt, B->crackle_dense, F(WA_BANDPASS, rr(1800, 2800), 1), 0.45 * v, 0.015, rr(0.08, 0.12), rr(0.9, 1.2), d);
}

static void s_wetsand(const Buffers *B, double t, double v, WaNode *d) {
  BURST(t, B->brown, F(WA_LOWPASS, rr(320, 440), 0.8), 0.65 * v, 0.006, 0.11, 1, d);
  WaNode *sq = F(WA_BANDPASS, rr(260, 380), rr(5, 8));
  wa_set_at(wa_param(sq, WP_FREQUENCY), wa_value(wa_param(sq, WP_FREQUENCY)), t);
  wa_expo(wa_param(sq, WP_FREQUENCY), rr(750, 1100), t + rr(0.07, 0.11));
  BURST(t, B->pink, sq, 0.9 * v, 0.012, 0.12, 1, d);
  BURST(t + 0.004, B->white, F(WA_HIGHPASS, rr(2200, 3000), 0.7), 0.08 * v, 0.004, 0.07, 1, d);
  int n = 1 + (int)(wa_random() * 3);
  for (int i = 0; i < n; i++) {
    double b = rr(1200, 2600);
    tone(t + rr(0.02, 0.12), b, b * 1.4, 0.03 * v, 0.002, 0.018, WA_SINE, d);
  }
  WaNode *su = F(WA_BANDPASS, rr(800, 1000), 6);
  wa_set_at(wa_param(su, WP_FREQUENCY), wa_value(wa_param(su, WP_FREQUENCY)), t + 0.14);
  wa_expo(wa_param(su, WP_FREQUENCY), 350, t + 0.24);
  BURST(t + 0.14, B->pink, su, 0.35 * v, 0.02, 0.08, 1, d);
}

// foot in shallow swash: a wet slap, water sloshing off, fizzing foam
static void s_splash(const Buffers *B, double t, double v, WaNode *d, double depth) {
  double k = fmin(1, 0.4 + depth * 6);
  BURST(t, B->brown, F(WA_LOWPASS, rr(260, 360), 0.8), 0.45 * v, 0.004, 0.08, 1, d);
  BURST(t, B->pink, F(WA_BANDPASS, rr(500, 800), 1.2), 0.7 * v * k, 0.006, rr(0.12, 0.2), 1, d);
  WaNode *sl = F(WA_BANDPASS, rr(900, 1300), 2.5);
  wa_set_at(wa_param(sl, WP_FREQUENCY), wa_value(wa_param(sl, WP_FREQUENCY)), t + 0.03);
  wa_expo(wa_param(sl, WP_FREQUENCY), rr(400, 600), t + 0.3);
  BURST(t + 0.03, B->white, sl, 0.35 * v * k, 0.03, rr(0.2, 0.3), 1, d);
  BURST(t + 0.05, B->crackle_dense, F(WA_HIGHPASS, rr(2500, 3500), 0.7), 0.5 * v * k, 0.03, rr(0.25, 0.4), rr(0.9, 1.2), d);
  for (int i = 0; i < 3; i++) {
    double b = rr(700, 1600);
    tone(t + rr(0.04, 0.2), b, b * 1.6, 0.04 * v * k, 0.002, 0.03, WA_SINE, d);
  }
}

static void knock(const Buffers *B, double tt, double scale, double v, WaNode *hollow, WaNode *d) {
  double f1 = rr(130, 175);
  static const double M[3][4] = { { 1, 14, 1.5, 0.18 }, { 2.32, 11, 0.9, 0.12 }, { 3.87, 8, 0.55, 0.08 } };
  for (int i = 0; i < 3; i++)
    BURST(tt, B->pink, F(WA_BANDPASS, f1 * M[i][0] * rr(0.97, 1.03), M[i][1]), M[i][2] * v * scale, 0.001, M[i][3], 1, hollow);
  tone(tt, rr(95, 120), rr(70, 85), 0.3 * v * scale, 0.002, 0.1, WA_SINE, hollow);
  BURST(tt, B->white, F(WA_BANDPASS, 2800, 1.2), 0.3 * v * scale, 0.0008, 0.012, 1, d);
}

static void s_wood(const Buffers *B, double t, double v, WaNode *d) {
  WaNode *hollow = wa_gain(1);
  wa_connect(hollow, d);
  wa_chain(hollow, wa_delay(rr(0.009, 0.014), 1), wa_gain(0.35), d);
  knock(B, t, 1, v, hollow, d);
  knock(B, t + rr(0.06, 0.09), 0.45, v, hollow, d);
}

static void s_grass(const Buffers *B, double t, double v, WaNode *d) {
  BURST(t, B->white, F(WA_BANDPASS, rr(3000, 5000), 0.6), 0.16 * v, rr(0.025, 0.04), rr(0.1, 0.15), 1, d);
  BURST(t, B->crackle_dense, F(WA_HIGHPASS, rr(2000, 3000), 0.7), 0.35 * v, 0.02, 0.1, rr(0.8, 1.2), d);
  BURST(t, B->brown, F(WA_LOWPASS, 200, 0.7), 0.4 * v, 0.01, 0.07, 1, d);
  BURST(t + rr(0.07, 0.1), B->white, F(WA_BANDPASS, rr(3500, 6000), 0.7), 0.08 * v, 0.02, 0.08, 1, d);
}

void footsteps_step(Footsteps *f, Surface s, double gain, double at, double depth) {
  const Buffers *B = &f->env->B;
  double t = (isnan(at) ? wa_now() : at) + 0.005;
  f->side = -f->side;
  WaNode *pan = wa_stereo_panner(f->side * rr(0.06, 0.14));
  wa_connect(pan, f->out);
  double v = gain * rr(0.8, 1.1);
  switch (s) {
    case SURF_SAND: s_sand(B, t, v, pan); break;
    case SURF_WETSAND: s_wetsand(B, t, v, pan); break;
    case SURF_SPLASH: s_splash(B, t, v, pan, isnan(depth) ? 0.05 : depth); break;
    case SURF_WOOD: s_wood(B, t, v, pan); break;
    case SURF_GRASS: s_grass(B, t, v, pan); break;
    case SURF_PAVEMENT: default: s_pavement(B, t, v, pan); break;
  }
}

Surface surface_at(double x, double z, double y, double waterline_x) {
  double WL = isnan(waterline_x) ? SAND.waterline : waterline_x;
  if (y > 0.5 && fabs(x - TOWER.x) < 3.5 && fabs(z - TOWER.z) < 4.5) return SURF_WOOD;
  if (x < PARK.x0) return SURF_PAVEMENT;               // hotel sidewalk, parking, road, park sidewalk
  if (x < SAND.x0) {
    if (fabs(x - PARK.promenadeX) < 1.75) return SURF_PAVEMENT;   // promenade path through the park
    if (x > PARK.x1 - 0.6) return SURF_PAVEMENT;                  // park / beach wall cap
    return SURF_GRASS;
  }
  if (x >= WL - 6) return SURF_WETSAND;
  return SURF_SAND;
}
