// Shared surf clock: port of src/world/surf.js. One list of wave events drives the visuals
// (shore-break crest, whitewater roller, swash sheet running up the sand) and, in live mode, is
// fed by the audio engine's own wave-break schedule so the crash and the wash-in line up. In shot
// mode (or before audio starts) a deterministic schedule is used instead.
#pragma once

#include "gfx/material.h"

enum { SURF_EVENTS = 4 };

typedef struct SurfEvent { double t0, k, size, runup, z, ph; } SurfEvent;
typedef struct Surf Surf;

Surf *surf_create(bool frozen, double anchor_time);
double surf_time(const Surf *s);
const SurfEvent *surf_events(const Surf *s, int *n);
void surf_update(Surf *s, double t);
// live mode: audio reports each scheduled break (visual_t0 is on this clock)
void surf_push_audio_wave(Surf *s, double visual_t0, double k, double size, double runup, double z);
// sets uSurfA / uSurfB / uSurfT on a material whose program uses the surf GLSL
void surf_apply(const Surf *s, Material *m);

// Swash over the sand at (x, z): covered, depth (m), foam 0..1, front (x of the water's landward
// edge), fresh 0..1 (1 = rushing up, 0 = drained). t: the surf clock (surf_time for now).
typedef struct Swash { bool covered; double depth, foam, front, fresh; } Swash;
Swash surf_swash_at(const Surf *s, double x, double z, double t);
// water depth over the ground (swash sheet or the sea itself), m; 0 = dry
double surf_water_depth_at(const Surf *s, double x, double z, double t);
