// Seagulls: port of src/audio/gulls.js. Each note is a harmonically rich custom waveform,
// FM-roughened and amplitude-rasped, shaped by three vocal-tract formant bandpasses (the second
// glides down for the "-ow" of "kyow"), plus a little breath noise. Calls come in phrases (long
// call, laughing "ha-ha-ha", short "kek") from a gull drifting across the sky.
#pragma once

#include "audio/engine.h"

typedef struct Gulls Gulls;

// BIRDS hook: a real (visible) gull to voice a call: returns false for none
typedef struct GullSrc { double x, y, z, vx, vy, vz; } GullSrc;
typedef bool (*GullSourceFn)(void *user, const Listener *L, GullSrc *out);

Gulls *create_gulls(AudioEnv *env);
void gulls_set_source(Gulls *g, GullSourceFn fn, void *user);
// kind: "kyow" | "laugh" | "kek", or nullptr for a random one
void gulls_call_at(Gulls *g, double t, const Listener *L, const char *kind);
// soft wingbeats of a gull taking off nearby
void gulls_flutter_at(Gulls *g, double t, const GullSrc *p);
