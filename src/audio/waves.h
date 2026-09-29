// Ocean: port of src/audio/waves.js. A continuous surf bed along the waterline (sources track the
// listener's z so the shoreline feels endless), a distant offshore roar, and discrete wave
// events: swell -> crash -> foam wash-in running up the sand -> gravelly fizz receding.
#pragma once

#include "audio/engine.h"

typedef struct Waves Waves;
typedef struct WaveBreak { double t, k, size, runup, z; } WaveBreak;

Waves *create_waves(AudioEnv *env, double waterline_x);
void waves_track(Waves *w, const Listener *L);
// one breaking wave at t; size 0.5..1 scales loudness and timing (NAN: random 0.55..1)
WaveBreak waves_break_at(Waves *w, double t, const Listener *L, double size);
