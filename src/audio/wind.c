#include "audio/wind.h"

#include <math.h>

static double g_t0;
static const Buffers *g_B;

static WaNode *gust_at(double x) {
  return loop_src(g_B->gust, 1, g_t0, fmod(fmod(20 + x * 0.09, 71) + 71, 71));
}

void create_wind(AudioEnv *env, const double (*palms)[2], int n) {
  const Buffers *B = &env->B;
  g_B = B;
  g_t0 = wa_now() + 0.02;

  // general breeze (decorrelated L / R)
  WaNode *merger = wa_merger(2);
  for (int ch = 0; ch < 2; ch++) {
    WaNode *g = wa_gain(0.25);
    wa_connect_param(wa_connect(gust_at(-20 + ch * 3), wa_gain(0.75)), wa_param(g, WP_GAIN));
    wa_chain(loop_src(B->pink, 1, NAN, NAN), bq(WA_LOWPASS, 520, 0.5), wa_gain(0.5), g);
    wa_chain(loop_src(B->white, 1, NAN, NAN), bq(WA_BANDPASS, 1600, 0.5), wa_gain(0.05), g);
    wa_connect_to(g, merger, ch);
  }
  wa_chain(merger, wa_gain(0.16), env->dry);

  float cube[256];
  for (int i = 0; i < 256; i++) {
    double x = (double)i / 255 * 2 - 1;
    cube[i] = (float)(x * x * x);
  }

  for (int i = 0; i < n; i++) {
    double x = palms[i][0], z = palms[i][1];
    SpatialOpts o = spatial_opts();
    o.x = x; o.y = rr(6, 9); o.z = z; o.ref = 4; o.rolloff = 1.2; o.air_scale = 25; o.wet = 0.1; o.wet_fall = 40;
    Spatial *sp = create_spatial(env, o);
    WaNode *gust_src = gust_at(x);

    WaNode *gust = wa_gain(0.08);
    wa_connect_param(wa_connect(gust_src, wa_gain(0.92)), wa_param(gust, WP_GAIN));
    wa_connect(gust, sp->input);

    // leaf rustle: bright band-passed noise with fast random flutter
    WaNode *flut = wa_gain(0.35);
    wa_connect_param(wa_connect(loop_src(B->flutter, 1, NAN, NAN), wa_gain(0.65)), wa_param(flut, WP_GAIN));
    double f1 = rr(2600, 4200);
    wa_chain(loop_src(B->white, 1, NAN, NAN), bq(WA_HIGHPASS, 900, 0.6), bq(WA_BANDPASS, f1, 0.55), wa_gain(0.5), flut, gust);
    // frond body whoosh
    double f2 = rr(600, 800);
    wa_chain(loop_src(B->pink, 1, NAN, NAN), bq(WA_BANDPASS, f2, 0.6), wa_gain(0.22), gust);

    // dry frond clatter, only in strong gusts (gust^3)
    WaNode *clat = wa_gain(0);
    WaNode *shaper = wa_shaper(cube, 256, false);
    wa_connect_param(wa_connect(wa_connect(gust_src, shaper), wa_gain(0.9)), wa_param(clat, WP_GAIN));
    double f3 = rr(1400, 2200);
    wa_chain(loop_src(B->crackle_dense, 1, NAN, NAN), bq(WA_BANDPASS, f3, 1.1), clat, sp->input);
  }
}
