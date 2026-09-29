// Procedural DSP building blocks: port of src/audio/dsp.js. Every sound is generated here in
// code: noise / crackle / control-signal buffers, a synthetic reverb impulse, Karplus-Strong
// plucks.
#pragma once

#include "audio/wa.h"

double rr(double a, double b);                    // a + Math.random() * (b - a)
#define PICK(arr) ((arr)[(int)(wa_random() * (double)ARRAY_LEN(arr))])
double clamp_d(double v, double a, double b);
double mtof(double m);

// new BiquadFilterNode(ctx, { type, frequency, Q, gain })
WaNode *bq(WaBiquadType type, double frequency, double q);   // gain 0 (q: 0.707 is the JS default)
WaNode *bqg(WaBiquadType type, double frequency, double q, double gain);

// linear breakpoints relative to t0: pts = { dt, value } pairs
void ramp(WaParam *p, double t0, const double (*pts)[2], int n);
#define RAMP(p, t0, ...) ramp((p), (t0), (const double[][2]){ __VA_ARGS__ }, \
                              (int)(sizeof((const double[][2]){ __VA_ARGS__ }) / sizeof(double[2])))
// percussive envelope: linear attack, exponential decay (about -35 dB after d seconds)
void perc(WaParam *p, double t, double peak, double a, double d);

// looping buffer source, started at `at` (NAN: now) from `offset` (NAN: random)
WaNode *loop_src(WaBuffer *b, double rate, double at, double offset);

typedef struct Buffers {
  WaBuffer *white, *pink, *brown, *crackle, *crackle_dense, *gust, *flutter, *swell;
} Buffers;
Buffers make_buffers(void);
// synthetic stereo room / outdoor impulse: early reflections + decaying, darkening noise
WaBuffer *make_impulse(double seconds, double rt60);

typedef struct PluckOpts { double seconds, bright, pos, t60; } PluckOpts;   // JS defaults: 1.6, 0.45, 0.18, 2
// Karplus-Strong plucked string (allpass fine tuning, pluck-position comb, lowpassed excitation)
float *render_pluck(double sr, double freq, PluckOpts o, int *len);

float *soft_clip_curve(int n, double knee);   // JS defaults: 2048, 0.85
