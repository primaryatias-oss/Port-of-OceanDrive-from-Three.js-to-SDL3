#include "audio/waves.h"

#include <math.h>

static const double BED_OFFSETS[4] = { -75, -28, 28, 75 };

struct Waves {
  AudioEnv *env;
  double wl;
  Spatial *beds[4];
  Spatial *far;
};

Waves *create_waves(AudioEnv *env, double waterline_x) {
  Waves *w = xcalloc(1, sizeof *w);
  w->env = env;
  w->wl = waterline_x;
  const Buffers *B = &env->B;
  for (int i = 0; i < 4; i++) {
    SpatialOpts o = spatial_opts();
    o.x = w->wl - 1; o.y = 0.4; o.z = BED_OFFSETS[i]; o.ref = 12; o.rolloff = 1; o.air_scale = 35; o.wet = 0.06;
    Spatial *sp = create_spatial(env, o);
    WaNode *mod = wa_gain(0.45), *depth = wa_gain(0.55);
    wa_connect_param(wa_connect(loop_src(B->swell, 1, NAN, NAN), depth), wa_param(mod, WP_GAIN));
    wa_chain(loop_src(B->brown, 1, NAN, NAN), bq(WA_LOWPASS, 380, 0.6), wa_gain(0.5), mod);
    wa_chain(loop_src(B->pink, 1, NAN, NAN), bq(WA_BANDPASS, 1100, 0.45), wa_gain(0.1), mod);
    wa_connect(mod, sp->input);
    w->beds[i] = sp;
  }
  SpatialOpts o = spatial_opts();
  o.x = w->wl + 140; o.y = 2; o.z = 0; o.ref = 80; o.rolloff = 1; o.air = false; o.wet = 0.05;
  w->far = create_spatial(env, o);
  wa_chain(loop_src(B->brown, 1, NAN, NAN), bq(WA_LOWPASS, 260, 0.6), wa_gain(0.14), w->far->input);
  return w;
}

void waves_track(Waves *w, const Listener *L) {
  for (int i = 0; i < 4; i++)
    spatial_set_position(w->beds[i], w->wl - 1, 0.4, L->z + BED_OFFSETS[i], (SetPos){ .at = NAN, .tc = 0.25 });
  spatial_set_position(w->far, w->wl + 140, 2, L->z, (SetPos){ .at = NAN, .tc = 0.25 });
}

typedef struct Noise { WaNode *s, *g; } Noise;

static Noise noise(WaBuffer *buf, double t, double dur, WaNode *const *filters, int nf, WaNode *dest, double rate) {
  WaNode *s = wa_buffer_source(buf, true, rate);
  WaNode *g = wa_gain(0);
  WaNode *prev = s;
  for (int i = 0; i < nf; i++) prev = wa_connect(prev, filters[i]);
  wa_connect(wa_connect(prev, g), dest);
  wa_start(s, t, wa_random() * wa_buffer_duration(buf));
  wa_stop(s, t + dur);
  return (Noise){ s, g };
}

typedef struct BreakEnd { Spatial *crash, *wash; } BreakEnd;
static void break_ended(void *user) {
  BreakEnd *e = user;
  spatial_dispose(e->crash);
  spatial_dispose(e->wash);
  free(e);
}

WaveBreak waves_break_at(Waves *w, double t, const Listener *L, double size) {
  if (isnan(size)) size = rr(0.55, 1);
  AudioEnv *env = w->env;
  const Buffers *B = &env->B;
  double WL = w->wl;
  double z = L->z + rr(-40, 40);
  SpatialOpts oc = spatial_opts();
  oc.x = WL - rr(2, 5); oc.y = 0.6; oc.z = z; oc.ref = 10; oc.rolloff = 1; oc.air_scale = 30; oc.wet = 0.1;
  Spatial *crash = create_spatial(env, oc);
  SpatialOpts ow = spatial_opts();
  ow.x = WL; ow.y = 0.3; ow.z = z + rr(-6, 6); ow.ref = 7; ow.rolloff = 1; ow.air_scale = 25; ow.wet = 0.08;
  Spatial *wash = create_spatial(env, ow);
  double k = rr(0.85, 1.2);   // tempo of this wave
#define T(s) ((s) * k)
  double end = T(9.5);

  // 1. swell building, crash, settling
  WaNode *lp1 = bq(WA_LOWPASS, 200, 0.7);
  RAMP(wa_param(lp1, WP_FREQUENCY), t, { 0, 200 }, { T(1.6), 700 }, { T(2.2), 2600 }, { T(4), 900 }, { end, 400 });
  Noise a = noise(B->pink, t, end, (WaNode *[]){ lp1 }, 1, crash->input, 1);
  RAMP(wa_param(a.g, WP_GAIN), t, { 0, 0 }, { T(1.6), 0.25 * size }, { T(2.15), 0.95 * size }, { T(3.2), 0.35 * size }, { T(5.5), 0 });

  // 2. low thump of the lip hitting the water
  Noise b = noise(B->brown, t, end, (WaNode *[]){ bq(WA_LOWPASS, 160, 0.8) }, 1, crash->input, 1);
  RAMP(wa_param(b.g, WP_GAIN), t, { 0, 0 }, { T(1.9), 0 }, { T(2.2), 0.9 * size }, { T(3.8), 0 });

  // 3. foam hiss rushing up the sand and dying away
  WaNode *lp3 = bq(WA_LOWPASS, 8000, 0.5);
  RAMP(wa_param(lp3, WP_FREQUENCY), t, { 0, 8000 }, { T(2.2), 8000 }, { T(7.5), 2400 });
  Noise c = noise(B->white, t, end, (WaNode *[]){ bq(WA_HIGHPASS, 1200, 0.6), lp3 }, 2, wash->input, 1);
  RAMP(wa_param(c.g, WP_GAIN), t, { 0, 0 }, { T(2.1), 0 }, { T(2.8), 0.4 * size }, { T(4.5), 0.26 * size }, { T(8.2), 0 });
  double runup = rr(4, 8) * size;
  spatial_set_position(wash, WL - runup, 0.3, wash->z, (SetPos){ .at = t + T(2.2), .ramp = T(2.3) });
  spatial_set_position(wash, WL, 0.3, wash->z, (SetPos){ .at = t + T(4.5), .ramp = T(4) });

  // 4. gravelly fizz of foam and shell grit draining back
  double fz = rr(2800, 3800), rate = rr(0.85, 1.15);
  Noise d = noise(B->crackle, t, end, (WaNode *[]){ bq(WA_BANDPASS, fz, 0.6), bq(WA_HIGHPASS, 1400, 0.6) }, 2, wash->input, rate);
  RAMP(wa_param(d.g, WP_GAIN), t, { 0, 0 }, { T(3), 0 }, { T(4.6), 1.6 * size }, { T(6.5), 0.8 * size }, { T(8.8), 0 });

  // 5. faint high sizzle of bursting bubbles
  Noise e = noise(B->white, t, end, (WaNode *[]){ bq(WA_HIGHPASS, 6000, 0.7) }, 1, wash->input, 1);
  RAMP(wa_param(e.g, WP_GAIN), t, { 0, 0 }, { T(3.2), 0 }, { T(4.2), 0.07 * size }, { T(8.5), 0 });
#undef T

  BreakEnd *be = xmalloc(sizeof *be);
  *be = (BreakEnd){ crash, wash };
  wa_on_ended(a.s, break_ended, be);
  return (WaveBreak){ t, k, size, runup, z };
}
