#include "audio/engine.h"

#include <math.h>

AudioEnv *create_engine(void) {
  AudioEnv *env = xcalloc(1, sizeof *env);
  env->B = make_buffers();
  WaNode *sum = wa_gain(1), *dry = wa_gain(1), *reverb = wa_gain(1);
  WaNode *conv = wa_convolver(make_impulse(2.4, 2.0));
  WaNode *rev_ret = wa_gain(0.6);
  wa_connect(dry, sum);
  wa_chain(reverb, bq(WA_HIGHPASS, 180, 0.6), conv, rev_ret, sum);

  WaNode *comp = wa_compressor(-20, 12, 2.5, 0.02, 0.35);
  WaNode *limiter = wa_compressor(-3, 0, 20, 0.001, 0.1);
  WaNode *volume = wa_gain(0);
  float *curve = soft_clip_curve(2048, 0.85);
  WaNode *clip = wa_shaper(curve, 2048, true);
  free(curve);
  wa_chain(sum, comp, limiter, volume, clip, wa_destination());
  // (the buses the modules connect to later are held: everything downstream of them stays alive)
  wa_hold(dry);
  wa_hold(reverb);
  wa_hold(sum);
  wa_hold(volume);
  env->dry = dry;
  env->reverb = reverb;
  env->sum = sum;
  env->volume = volume;
  env->listener = (Listener){ 0, 1.7, 0, 0, 0, -1, 0, 1, 0 };
  return env;
}

SpatialOpts spatial_opts(void) {
  return (SpatialOpts){ .hrtf = true, .ref = 5, .rolloff = 1, .air = true, .air_scale = 25, .wet = 0.12, .wet_fall = 60 };
}

Spatial *create_spatial(AudioEnv *env, SpatialOpts o) {
  Spatial *s = xcalloc(1, sizeof *s);
  s->env = env;
  s->o = o;
  s->input = wa_gain(1);
  s->lp = wa_biquad(WA_LOWPASS, 18000, 0.5, 0);
  s->panner = wa_panner((WaPannerOpts){ .hrtf = o.hrtf, .ref = o.ref, .rolloff = o.rolloff, .max_distance = 10000,
                                        .x = o.x, .y = o.y, .z = o.z });
  s->wet = wa_gain(0);
  wa_chain(s->input, s->lp, s->panner, env->dry);
  wa_chain(s->lp, s->wet, env->reverb);
  wa_hold(s->input);   // modules keep connecting sources to it
  s->x = o.x; s->y = o.y; s->z = o.z;
  vec_push(&env->spatials, s);
  spatial_update(s, &env->listener, true);
  return s;
}

void spatial_set_position(Spatial *s, double x, double y, double z, SetPos o) {
  WaParam *P[3] = { wa_param(s->panner, WP_POSITION_X), wa_param(s->panner, WP_POSITION_Y), wa_param(s->panner, WP_POSITION_Z) };
  double from[3] = { s->x, s->y, s->z }, to[3] = { x, y, z };
  double t = isnan(o.at) ? wa_now() : o.at;
  for (int i = 0; i < 3; i++) {
    if (o.ramp > 0) { wa_set_at(P[i], from[i], t); wa_linear(P[i], to[i], t + o.ramp); }
    else if (o.tc > 0) wa_target(P[i], to[i], t, o.tc);
    else wa_set_at(P[i], to[i], t);
  }
  s->x = x; s->y = y; s->z = z;
}

void spatial_update(Spatial *s, const Listener *L, bool immediate) {
  double px = s->x, py = s->y, pz = s->z;
  if (s->pos_fn) s->pos_fn(s->pos_user, &px, &py, &pz);
  double d = sqrt((px - L->x) * (px - L->x) + (py - L->y) * (py - L->y) + (pz - L->z) * (pz - L->z));
  double f = !s->o.air ? 20000 : 20000 / (1 + d / s->o.air_scale);
  if (s->o.occl) f = fmin(f, s->o.occl_f / (1 + d / s->o.occl_d));
  f = clamp_d(f, 150, 20000);
  double w = s->o.wet / (1 + d / s->o.wet_fall);
  if (immediate) {
    wa_set_value(wa_param(s->lp, WP_FREQUENCY), f);
    wa_set_value(wa_param(s->wet, WP_GAIN), w);
  } else {
    double t = wa_now();
    wa_target(wa_param(s->lp, WP_FREQUENCY), f, t, 0.12);
    wa_target(wa_param(s->wet, WP_GAIN), w, t, 0.12);
  }
}

void spatial_dispose(Spatial *s) {
  wa_disconnect(s->input);
  wa_disconnect(s->lp);
  wa_disconnect(s->panner);
  wa_disconnect(s->wet);
  wa_release(s->input);
  AudioEnv *env = s->env;
  for (size_t i = 0; i < env->spatials.len; i++)
    if (env->spatials.data[i] == s) { vec_remove(&env->spatials, i); break; }
  free(s);
}
