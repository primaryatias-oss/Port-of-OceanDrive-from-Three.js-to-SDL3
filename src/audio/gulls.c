#include "audio/gulls.h"

#include <math.h>
#include <string.h>

static const double HARM[10] = { 1, 0.85, 0.75, 0.55, 0.42, 0.3, 0.22, 0.15, 0.1, 0.07 };

struct Gulls {
  AudioEnv *env;
  WaWave *wave;
  GullSourceFn source;
  void *source_user;
};

Gulls *create_gulls(AudioEnv *env) {
  Gulls *g = xcalloc(1, sizeof *g);
  g->env = env;
  float real[11] = {}, imag[11] = {};
  for (int i = 0; i < 10; i++) imag[i + 1] = (float)HARM[i];
  g->wave = wa_periodic_wave(real, imag, 11);
  return g;
}

void gulls_set_source(Gulls *g, GullSourceFn fn, void *user) {
  g->source = fn;
  g->source_user = user;
}

typedef struct NoteOpts { double bright; bool vowel; double peak; } NoteOpts;

static WaNode *note(Gulls *G, double t, double dur, double f0, const double (*contour)[2], int nc, WaNode *dest, NoteOpts o) {
  const Buffers *B = &G->env->B;
  WaNode *osc = wa_osc(WA_SINE, f0);
  wa_osc_set_wave(osc, G->wave);
  WaNode *mod = wa_osc(WA_SINE, f0);
  WaNode *modG = wa_gain(0);
  wa_connect_param(wa_connect(mod, modG), wa_param(osc, WP_FREQUENCY));
  WaParam *ps[2] = { wa_param(osc, WP_FREQUENCY), wa_param(mod, WP_FREQUENCY) };
  for (int k = 0; k < 2; k++) {
    wa_set_at(ps[k], f0 * contour[0][1], t);
    for (int i = 1; i < nc; i++) wa_linear(ps[k], f0 * contour[i][1], t + contour[i][0] * dur);
  }
  wa_set_at(wa_param(modG, WP_GAIN), f0 * rr(0.5, 1.2) * o.bright, t);
  wa_linear(wa_param(modG, WP_GAIN), f0 * 0.25, t + dur);

  WaNode *rasp = wa_osc(WA_SINE, rr(55, 95));
  WaNode *raspG = wa_gain(rr(0.2, 0.4));
  WaNode *am = wa_gain(0.7);
  wa_connect_param(wa_connect(rasp, raspG), wa_param(am, WP_GAIN));
  wa_connect(osc, am);

  WaNode *env_ = wa_gain(0);
  WaNode *f2 = bq(WA_BANDPASS, 2900, 5);
  if (o.vowel) {
    wa_set_at(wa_param(f2, WP_FREQUENCY), 2900, t);
    wa_linear(wa_param(f2, WP_FREQUENCY), 1700, t + dur);
  }
  double f1 = 1400 * rr(0.95, 1.05);
  wa_chain(am, bq(WA_BANDPASS, f1, 4), wa_gain(1), env_);
  wa_chain(am, f2, wa_gain(0.8), env_);
  wa_chain(am, bq(WA_BANDPASS, 3900, 5), wa_gain(0.4), env_);
  wa_chain(am, wa_gain(0.12), env_);

  WaNode *n = wa_buffer_source(B->white, true, 1);
  wa_chain(n, bq(WA_BANDPASS, f0 * 2, 3), wa_gain(0.25), env_);

  WaParam *eg = wa_param(env_, WP_GAIN);
  wa_set_at(eg, 0, t);
  wa_linear(eg, o.peak, t + 0.015);
  wa_linear(eg, o.peak * 0.8, t + dur * 0.6);
  wa_linear(eg, 0, t + dur);
  wa_connect(env_, dest);

  WaNode *srcs[3] = { osc, mod, rasp };
  for (int i = 0; i < 3; i++) { wa_start(srcs[i], t, 0); wa_stop(srcs[i], t + dur + 0.05); }
  wa_start(n, t, wa_random() * 5);
  wa_stop(n, t + dur + 0.05);
  return osc;
}

typedef struct Phrase { double end; WaNode *last; } Phrase;

static Phrase phrase(Gulls *G, double t, const char *kind, WaNode *dest) {
  double tt = t;
  WaNode *last = nullptr;
  if (!strcmp(kind, "kyow")) {
    double f = rr(750, 950);
    double d0 = rr(0.4, 0.5);
    last = note(G, tt, d0, f, (const double[][2]){ { 0, 0.8 }, { 0.2, 1.12 }, { 0.55, 1.05 }, { 1, 0.7 } }, 4, dest,
                (NoteOpts){ 1, true, 0.5 });
    tt += rr(0.6, 0.75);
    int n = 3 + (int)(wa_random() * 4);
    for (int i = 0; i < n; i++) {
      double d = rr(0.2, 0.28);
      last = note(G, tt, d, f, (const double[][2]){ { 0, 0.85 }, { 0.25, 1.1 }, { 1, 0.72 } }, 3, dest, (NoteOpts){ 1, true, 0.45 });
      f *= rr(0.96, 1.0);
      tt += rr(0.28, 0.36);
    }
  } else if (!strcmp(kind, "laugh")) {
    double f = rr(900, 1100);
    int n = 5 + (int)(wa_random() * 5);
    for (int i = 0; i < n; i++) {
      double d = rr(0.1, 0.14);
      last = note(G, tt, d, f, (const double[][2]){ { 0, 0.95 }, { 0.3, 1.12 }, { 1, 0.8 } }, 3, dest, (NoteOpts){ 1.4, false, 0.42 });
      f *= rr(0.95, 0.99);
      tt += rr(0.17, 0.22);
    }
  } else {
    double f = rr(1000, 1300);
    int n = 1 + (int)(wa_random() * 2);
    for (int i = 0; i < n; i++) {
      double d = rr(0.12, 0.18);
      last = note(G, tt, d, f, (const double[][2]){ { 0, 0.9 }, { 0.3, 1.08 }, { 1, 0.75 } }, 3, dest, (NoteOpts){ 1.2, true, 0.38 });
      tt += rr(0.25, 0.4);
    }
  }
  return (Phrase){ tt, last };
}

static void dispose_later(void *sp) { spatial_dispose(sp); }
static void call_ended(void *sp) { wa_timeout(0.4, dispose_later, sp); }
static void flutter_ended(void *sp) { wa_timeout(0.3, dispose_later, sp); }

void gulls_call_at(Gulls *G, double t, const Listener *L, const char *kind) {
  static const char *const KINDS[] = { "kyow", "kyow", "laugh", "kek" };
  if (!kind) kind = PICK(KINDS);
  GullSrc src;
  bool have = G->source && G->source(G->source_user, L, &src);
  double x, y, z;
  if (have) { x = src.x; y = src.y; z = src.z; }
  else { x = rr(15, 105); y = rr(10, 28); z = L->z + rr(-50, 50); }
  SpatialOpts o = spatial_opts();
  o.x = x; o.y = y; o.z = z; o.ref = 6; o.rolloff = 1; o.air_scale = 30; o.wet = 0.12;
  Spatial *sp = create_spatial(G->env, o);
  Phrase ph = phrase(G, t, kind, sp->input);
  double dur = ph.end - t + 0.3;
  if (have) spatial_set_position(sp, x + src.vx * dur, y + src.vy * dur, z + src.vz * dur, (SetPos){ .at = t, .ramp = dur });
  else {
    double nx = clamp_d(x + rr(-6, 6) * dur, 10, 120), ny = y + rr(-1, 1) * dur, nz = z + rr(-6, 6) * dur;
    spatial_set_position(sp, nx, ny, nz, (SetPos){ .at = t, .ramp = dur });
  }
  wa_on_ended(ph.last, call_ended, sp);
  if (wa_random() < 0.3) {
    static const char *const ANSWER[] = { "kek", "laugh", "kyow" };
    double ta = t + rr(0.4, 1.5);
    gulls_call_at(G, ta, L, PICK(ANSWER));
  }
}

void gulls_flutter_at(Gulls *G, double t, const GullSrc *p) {
  const Buffers *B = &G->env->B;
  SpatialOpts o = spatial_opts();
  o.x = p->x; o.y = p->y; o.z = p->z; o.ref = 2; o.rolloff = 1.4; o.air_scale = 30; o.wet = 0.04;
  Spatial *sp = create_spatial(G->env, o);
  WaNode *n = wa_buffer_source(B->white, true, 1);
  WaNode *g = wa_gain(0);
  wa_chain(n, bq(WA_BANDPASS, rr(550, 800), 0.9), bq(WA_LOWPASS, 2200, 0.707), g, sp->input);
  const int beats = 6;
  double rate = rr(4, 4.6);
  WaParam *gg = wa_param(g, WP_GAIN);
  for (int i = 0; i < beats; i++) {
    double tb = t + i / rate, pk = 0.35 * (1 - (double)i / (beats + 1));
    wa_set_at(gg, 0, tb);
    wa_linear(gg, pk, tb + 0.035);
    wa_linear(gg, 0, tb + 0.15);
  }
  double dur = beats / rate + 0.2;
  spatial_set_position(sp, p->x + p->vx * dur, p->y + p->vy * dur, p->z + p->vz * dur, (SetPos){ .at = t, .ramp = dur });
  wa_start(n, t, wa_random() * 5);
  wa_stop(n, t + dur);
  wa_on_ended(n, flutter_ended, sp);
}
