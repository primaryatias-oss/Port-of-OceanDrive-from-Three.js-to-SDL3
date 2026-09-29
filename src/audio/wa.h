// A small Web Audio API in C (the subset the sound modules use), rendered in 128-frame quanta
// on the SDL audio thread: AudioParam automation (setValueAtTime, linear / exponential ramps,
// setTargetAtTime, cancelScheduledValues) with a-rate modulation inputs, and the Gain,
// Oscillator (band-limited PeriodicWave tables), AudioBufferSource, ConstantSource, BiquadFilter,
// WaveShaper, DynamicsCompressor (Chromium's kernel), Convolver, Panner (inverse distance model;
// 'HRTF' is a spherical-head model: interaural delay + head-shadow filters), StereoPanner, Delay
// and ChannelMerger nodes.
//
// Lifetime (JS garbage collection has no C counterpart): a node the caller keeps a pointer to
// must be held (wa_hold, or created held with the _held constructors); wa_release lets it go.
// Nodes nobody holds are collected once they have been idle (no active input) for a short tail.
// A source with an onended callback stays valid until the callback has run. All calls take the
// engine lock, so they are safe from the main thread while the audio thread renders; callbacks
// (onended, timers) run on the main thread inside wa_poll().
#pragma once

#include <stdint.h>

#include "core/common.h"

enum { WA_QUANTUM = 128 };

typedef struct WaNode WaNode;
typedef struct WaParam WaParam;
typedef struct WaWave WaWave;

typedef struct WaBuffer {
  int channels, length;
  double sample_rate;
  float *data[2];
} WaBuffer;

typedef enum WaBiquadType {
  WA_LOWPASS, WA_HIGHPASS, WA_BANDPASS, WA_LOWSHELF, WA_HIGHSHELF, WA_PEAKING, WA_NOTCH, WA_ALLPASS
} WaBiquadType;
typedef enum WaOscType { WA_SINE, WA_SQUARE, WA_SAWTOOTH, WA_TRIANGLE } WaOscType;

typedef enum WaParamName {
  WP_GAIN, WP_FREQUENCY, WP_DETUNE, WP_Q, WP_PLAYBACK_RATE, WP_OFFSET, WP_PAN, WP_DELAY_TIME,
  WP_POSITION_X, WP_POSITION_Y, WP_POSITION_Z,
} WaParamName;

// ---- context ----
// opens the default playback device (false if there is none); sample_rate 0 = 48000
bool wa_init(double sample_rate);
// offline context (tests): no device; render with wa_render_offline
void wa_init_offline(double sample_rate);
void wa_shutdown(void);
bool wa_running(void);
double wa_sample_rate(void);
double wa_now(void);                            // currentTime (s): frames rendered / sample rate
void wa_resume(void);
// interleaved stereo float, frames a multiple of nothing in particular
void wa_render_offline(float *out, int frames);
// main thread: delivers onended callbacks and due timers
void wa_poll(void);
// setTimeout on the audio clock
void wa_timeout(double delay_s, void (*fn)(void *user), void *user);

void wa_lock(void);
void wa_unlock(void);

// ---- buffers ----
WaBuffer *wa_buffer(int channels, int length, double sample_rate);   // zeroed; lives forever
double wa_buffer_duration(const WaBuffer *b);

// ---- nodes ----
WaNode *wa_destination(void);
WaNode *wa_gain(double gain);
WaNode *wa_biquad(WaBiquadType type, double frequency, double q, double gain);
WaNode *wa_osc(WaOscType type, double frequency);
WaWave *wa_periodic_wave(const float *real, const float *imag, int n);   // lives forever
void wa_osc_set_wave(WaNode *osc, WaWave *w);
WaNode *wa_buffer_source(WaBuffer *b, bool loop, double playback_rate);
WaNode *wa_constant(double offset);
WaNode *wa_shaper(const float *curve, int n, bool oversample2x);        // curve copied
WaNode *wa_compressor(double threshold, double knee, double ratio, double attack, double release);
WaNode *wa_convolver(const WaBuffer *ir);                                // normalize: true
typedef struct WaPannerOpts { bool hrtf; double ref, rolloff, max_distance, x, y, z; } WaPannerOpts;
WaNode *wa_panner(WaPannerOpts o);
WaNode *wa_stereo_panner(double pan);
WaNode *wa_delay(double delay_time, double max_delay);
WaNode *wa_merger(int inputs);

WaParam *wa_param(WaNode *n, WaParamName name);
WaNode *wa_connect(WaNode *from, WaNode *to);                  // returns `to` (chainable)
void wa_connect_to(WaNode *from, WaNode *to, int input);       // ChannelMerger inputs
void wa_connect_param(WaNode *from, WaParam *p);
void wa_disconnect(WaNode *n);                                  // all of its outgoing connections
// chain(a, b, c, ...): connects each node to the next, returns the last
WaNode *wa_chain_(WaNode *const *nodes, int n);
#define wa_chain(...) wa_chain_((WaNode *const[]){ __VA_ARGS__ }, (int)(sizeof((WaNode *const[]){ __VA_ARGS__ }) / sizeof(WaNode *)))

void wa_hold(WaNode *n);
void wa_release(WaNode *n);

// sources
void wa_start(WaNode *src, double when, double offset);   // offset: buffer sources only
void wa_stop(WaNode *src, double when);
void wa_set_loop(WaNode *src, bool loop);
void wa_on_ended(WaNode *src, void (*fn)(void *user), void *user);

// ---- params ----
double wa_value(WaParam *p);                          // .value
void wa_set_value(WaParam *p, double v);             // .value = v
void wa_set_at(WaParam *p, double v, double t);
void wa_linear(WaParam *p, double v, double t);
void wa_expo(WaParam *p, double v, double t);
void wa_target(WaParam *p, double target, double t, double tc);
void wa_cancel(WaParam *p, double t);

// ---- listener (AudioListener position / orientation params) ----
typedef enum WaListenerParam { WL_POS_X, WL_POS_Y, WL_POS_Z, WL_FWD_X, WL_FWD_Y, WL_FWD_Z, WL_UP_X, WL_UP_Y, WL_UP_Z } WaListenerParam;
WaParam *wa_listener(WaListenerParam p);

// Math.random for the sound modules (its own generator; not the scene's seeded ones)
double wa_random(void);
