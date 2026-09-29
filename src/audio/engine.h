// Master graph shared by all sound modules (port of src/audio/engine.js):
// sources -> dry bus ─┬─> glue compressor -> limiter -> volume -> soft clip -> destination
//         -> reverb ──┘ (highpassed send -> generated-IR convolver)
// and positioned sources (port of src/audio/spatial.js): input -> air-absorption lowpass ->
// HRTF panner -> dry bus, plus a distance-scaled reverb send.
#pragma once

#include "audio/dsp.h"
#include "core/vec.h"

typedef struct Listener { double x, y, z, fx, fy, fz, ux, uy, uz; } Listener;

typedef struct Spatial Spatial;
typedef struct AudioEnv {
  Buffers B;
  WaNode *dry, *reverb, *sum, *volume;
  Listener listener;              // env.listener: x, y, z (orientation unused by the modules)
  Vec(Spatial *) spatials;
} AudioEnv;

AudioEnv *create_engine(void);

typedef struct SpatialOpts {
  bool hrtf;                      // panningModel 'HRTF' (default) or 'equalpower'
  double x, y, z, ref, rolloff;
  bool air;                       // air absorption lowpass (default on)
  double air_scale;
  bool occl;                      // occlusion lowpass { f, d }
  double occl_f, occl_d;
  double wet, wet_fall;
} SpatialOpts;
SpatialOpts spatial_opts(void);   // the JS defaults: HRTF, ref 5, rolloff 1, air, airScale 25, wet 0.12, wetFall 60

struct Spatial {
  AudioEnv *env;
  WaNode *input, *lp, *panner, *wet;
  double x, y, z;
  SpatialOpts o;
  // posFn: a live position (moving cars)
  void (*pos_fn)(void *user, double *x, double *y, double *z);
  void *pos_user;
};

Spatial *create_spatial(AudioEnv *env, SpatialOpts o);
// opts: at (NAN: now) + ramp > 0 schedules a linear glide; tc > 0 smooths toward the target
typedef struct SetPos { double at, ramp, tc; } SetPos;
void spatial_set_position(Spatial *s, double x, double y, double z, SetPos o);
void spatial_update(Spatial *s, const Listener *L, bool immediate);
void spatial_dispose(Spatial *s);
