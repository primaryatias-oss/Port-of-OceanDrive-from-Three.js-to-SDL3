// Original, procedurally generated bossa / lounge loop played on a hotel patio: port of
// src/audio/music.js. Harmony: a Markov chain over jazz chord functions builds 8-bar phrases
// (always ending on a ii-V-type cadence), arranged A A B A and regenerated each chorus.
// Instruments: Karplus-Strong nylon guitar (syncopated pinched chords + thumb bass on 1 and 3),
// a soft sine sub-bass, a brushed shaker and brush swishes, and a sparse FM electric-piano
// melody improvised from chord tones. The patio speaker is band-limited, then distance-lowpassed
// and sent to the reverb so it drifts in softly from afar.
#pragma once

#include "audio/engine.h"

typedef struct Music Music;
Music *create_music(AudioEnv *env, double x, double y, double z);
// schedule everything that starts before `until` (seconds of audio time)
void music_tick(Music *m, double now, double until);
