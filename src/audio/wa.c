// The Web Audio subset (see wa.h). Semantics follow the Web Audio API spec; where the spec leaves
// room, Chromium's behaviour is followed (DynamicsCompressor kernel, PeriodicWave band limiting,
// Convolver normalization).
#include "audio/wa.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <string.h>

#include "audio/fft.h"
#if defined(__SSE__) || defined(__x86_64__)
#include <xmmintrin.h>   // (compiler intrinsics: the MXCSR flush-to-zero bits)
#endif
#include "core/vec.h"

static constexpr double PI = 3.141592653589793;
enum { Q = WA_QUANTUM };

// ---------------------------------------------------------------------------------------------
// params

typedef enum EvType { EV_SET, EV_LINEAR, EV_EXPO, EV_TARGET } EvType;
typedef struct Event { EvType type; double time, value, tc; } Event;
typedef enum AnchorKind { A_CONST, A_TARGET } AnchorKind;
typedef struct Anchor { AnchorKind kind; double t, v, target, tc; } Anchor;

struct WaParam {
  WaNode *owner;
  double value;               // the most recently computed intrinsic value (.value)
  double min, max;            // nominal range
  bool arate;
  Anchor a;
  Vec(Event) ev;
  Vec(WaNode *) inputs;
  float buf[Q];
  bool constant;              // this quantum: buf[0] holds the value
  uint64_t computed_q;
};

// ---------------------------------------------------------------------------------------------
// nodes

typedef enum Kind {
  K_DEST, K_GAIN, K_BIQUAD, K_OSC, K_BUFSRC, K_CONST, K_SHAPER, K_COMP, K_CONV, K_PANNER, K_SPANNER, K_DELAY, K_MERGER
} Kind;

typedef struct Conn { WaNode *to; int port; WaParam *param; } Conn;   // param != null: to a param

enum { WAVE_N = 4096, WAVE_TABLES = 34 };
struct WaWave {
  int ntables;
  int partials[WAVE_TABLES];   // partials in each table (decreasing)
  float *table[WAVE_TABLES];   // WAVE_N + 1 samples (wrap sample for interpolation)
};

typedef struct Compressor {
  float pre[2][1024];
  int pre_read, pre_write, last_pre_frames;
  double detector_average, compressor_gain, max_attack_diff_db;
  double db_threshold, db_knee, ratio, linear_threshold, slope, knee_threshold, knee_threshold_db, y_knee_threshold_db, k;
  bool curve_valid;
} Compressor;

enum { CONV_B = 512 };   // partition size (the impulse's 12 ms of leading silence absorbs the block latency)
typedef struct Convolver {
  int nparts;
  float *ir_re[2], *ir_im[2];      // nparts x (2B) spectra per channel
  float *in_re, *in_im;            // input spectra ring: nparts x (2B), per input channel below
  float *in2_re, *in2_im;          // second input channel (stereo input)
  int ring;                        // index of the newest input spectrum
  float inbuf[2][2 * CONV_B];      // last two input blocks (overlap-save)
  float outbuf[2][CONV_B];         // output of the last block, played over the next block
  int fill;                        // samples gathered into the current block
  bool stereo_in;
  Fft fft;
  float *tmp_re, *tmp_im;
} Convolver;

struct WaNode {
  Kind kind;
  int holds;
  bool dead_mark;
  int dead_q;
  Vec(Conn) outs;
  int nports;
  Vec(WaNode *) ports[2];
  WaParam *params[4];
  int nparams;
  float out[2][Q];
  int out_ch;
  uint64_t rendered_q;
  bool silent;                     // this quantum's output is all zeros (out[] is not valid then)
  bool quiet;                      // input silent and the tail has decayed: skip until input returns
  int silent_in_q;                 // consecutive quanta of silent input
  // sources
  bool is_source, started, ended, ended_queued;
  int64_t start_frame, stop_frame;
  void (*on_ended)(void *);
  void *ended_user;
  // gain / biquad / osc / bufsrc / const / spanner / delay
  WaParam gain, frequency, detune, q, bgain, rate, offset, pan, delay_time, pos[3];
  WaBiquadType btype;
  double bx1[2], bx2[2], by1[2], by2[2];
  WaWave *wave;
  double phase;
  const WaBuffer *buffer;
  bool loop, primed;
  double playhead, start_offset;
  uint64_t born_q;
  float *curve;
  int ncurve;
  bool oversample;
  float *os_x[2], *os_e[2], *os_o[2];   // 2x oversampling histories (see do_shaper)
  Compressor *comp;
  Convolver *conv;
  WaPannerOpts po;
  double itd_prev[2], dist_prev;
  bool hrtf_init;
  float hdelay[2][64];             // [input channel] ring (interaural delay)
  int hd_pos;
  double ear_g_prev[2];            // broadband ear gain (linear)
  double ear_db[2][3];             // smoothed low / mid / high gains (dB) per ear
  double sh[2][2][4];              // shelf filter state [ear][low, high][x1, x2, y1, y2]
  double hb[2][6][4];              // HRTF base cascade state [input channel][section]
  float *dbuf[2];
  int dlen, dpos;
};

static struct {
  bool init, device;
  double sr;
  uint64_t frame, quantum;
  SDL_Mutex *lock;
  SDL_AudioStream *stream;
  Vec(WaNode *) nodes;
  WaNode *dest;
  WaParam listener[9];
  struct Pending { void (*fn)(void *); void *user; WaNode *node; } *pend;
  size_t npend, cpend;
  struct Timer { double t; void (*fn)(void *); void *user; } *timers;
  size_t ntimers, ctimers;
  uint64_t rng[2];
  WaWave *basic[4];
  float sine_table[WAVE_N + 1];
  int gc_counter;
} W;

void wa_lock(void) { if (W.lock) SDL_LockMutex(W.lock); }
void wa_unlock(void) { if (W.lock) SDL_UnlockMutex(W.lock); }

double wa_random(void) {
  // xorshift128+
  uint64_t s1 = W.rng[0];
  const uint64_t s0 = W.rng[1];
  W.rng[0] = s0;
  s1 ^= s1 << 23;
  W.rng[1] = s1 ^ s0 ^ (s1 >> 17) ^ (s0 >> 26);
  return (double)((W.rng[1] + s0) >> 11) * (1.0 / 9007199254740992.0);
}

double wa_sample_rate(void) { return W.sr; }
double wa_now(void) {
  wa_lock();
  double t = (double)W.frame / W.sr;
  wa_unlock();
  return t;
}
bool wa_running(void) { return W.init; }

// ---- params ----

static void param_init(WaParam *p, WaNode *owner, double v, double min, double max, bool arate) {
  *p = (WaParam){ .owner = owner, .value = v, .min = min, .max = max, .arate = arate };
  p->a = (Anchor){ A_CONST, (double)W.frame / (W.sr ? W.sr : 48000), v, 0, 0 };
  if (owner && owner->nparams < 4) owner->params[owner->nparams++] = p;
}

static double anchor_at(const Anchor *a, double t) {
  if (a->kind == A_CONST) return a->v;
  double dt = t - a->t;
  if (dt <= 0) return a->v;
  return a->target + (a->v - a->target) * exp(-dt / a->tc);
}

// the anchor after event e has started (ramps: ended)
static void anchor_apply(Anchor *a, const Event *e) {
  double t = fmax(e->time, a->t);
  switch (e->type) {
    case EV_SET: case EV_LINEAR: case EV_EXPO: *a = (Anchor){ A_CONST, t, e->value, 0, 0 }; break;
    case EV_TARGET: {
      double v0 = anchor_at(a, t);
      *a = e->tc > 0 ? (Anchor){ A_TARGET, t, v0, e->value, e->tc } : (Anchor){ A_CONST, t, e->value, 0, 0 };
      break;
    }
  }
}

static double ramp_at(const Anchor *a, const Event *e, double t) {
  double t0 = a->t, v0 = a->v, t1 = e->time, v1 = e->value;
  if (t1 <= t0) return v1;
  double f = (t - t0) / (t1 - t0);
  if (e->type == EV_LINEAR) return v0 + (v1 - v0) * f;
  if (v0 == 0 || v0 * v1 <= 0) return v0;   // no exponential ramp through zero: hold
  return v0 * pow(v1 / v0, f);
}

// value at time t without mutating (the .value getter for automation inserted mid-quantum)
static double param_eval(const WaParam *p, double t) {
  Anchor a = p->a;
  for (size_t i = 0; i < p->ev.len; i++) {
    const Event *e = &p->ev.data[i];
    if (e->time <= t) { anchor_apply(&a, e); continue; }
    if (e->type == EV_LINEAR || e->type == EV_EXPO) return ramp_at(&a, e, t);
    break;
  }
  return anchor_at(&a, t);
}

static void param_insert(WaParam *p, Event e) {
  if (!isfinite(e.time) || !isfinite(e.value)) return;
  size_t i = 0;
  while (i < p->ev.len && p->ev.data[i].time <= e.time) i++;
  vec_insert(&p->ev, i, e);
}

void wa_set_at(WaParam *p, double v, double t) { wa_lock(); param_insert(p, (Event){ EV_SET, t, v, 0 }); wa_unlock(); }
void wa_target(WaParam *p, double target, double t, double tc) {
  wa_lock();
  param_insert(p, (Event){ EV_TARGET, t, target, tc });
  wa_unlock();
}
static void insert_ramp(WaParam *p, EvType type, double v, double t) {
  // a ramp that follows a setTarget already under way starts from its current value now
  double now = (double)W.frame / W.sr;
  size_t i = 0;
  while (i < p->ev.len && p->ev.data[i].time <= t) i++;
  bool prev_target = i > 0 ? p->ev.data[i - 1].type == EV_TARGET && p->ev.data[i - 1].time <= now
                           : p->a.kind == A_TARGET;
  if (prev_target) param_insert(p, (Event){ EV_SET, now, param_eval(p, now), 0 });
  param_insert(p, (Event){ type, t, v, 0 });
}
void wa_linear(WaParam *p, double v, double t) { wa_lock(); insert_ramp(p, EV_LINEAR, v, t); wa_unlock(); }
void wa_expo(WaParam *p, double v, double t) { wa_lock(); insert_ramp(p, EV_EXPO, v, t); wa_unlock(); }
void wa_cancel(WaParam *p, double t) {
  wa_lock();
  size_t n = 0;
  for (size_t i = 0; i < p->ev.len; i++)
    if (p->ev.data[i].time < t) p->ev.data[n++] = p->ev.data[i];
  p->ev.len = n;
  wa_unlock();
}
double wa_value(WaParam *p) {
  wa_lock();
  double v = p->value;
  wa_unlock();
  return v;
}
void wa_set_value(WaParam *p, double v) {
  wa_lock();
  if (!p->ev.len && p->a.kind == A_CONST) { p->a.v = v; p->value = v; }
  else param_insert(p, (Event){ EV_SET, (double)W.frame / W.sr, v, 0 });
  wa_unlock();
}

static void process(WaNode *n);

static void param_compute(WaParam *p) {
  if (p->computed_q == W.quantum) return;
  p->computed_q = W.quantum;
  double t0 = (double)W.frame / W.sr, dt = 1.0 / W.sr;
  double t_end = (double)(W.frame + Q) / W.sr;
  bool arate = p->arate;
  if (p->a.kind == A_CONST && (!p->ev.len || (p->ev.data[0].time > t_end && p->ev.data[0].type != EV_LINEAR &&
                                                p->ev.data[0].type != EV_EXPO))) {
    p->constant = true;
    p->buf[0] = (float)p->a.v;
    p->value = p->a.v;
  } else if (!arate) {
    // k-rate: the value at the start of the quantum
    double v = param_eval(p, t0);
    p->constant = true;
    p->buf[0] = (float)v;
    p->value = v;
  } else {
    p->constant = false;
    Anchor a = p->a;
    size_t cur = 0;
    bool done = false;
    if (a.kind == A_TARGET && (!p->ev.len || (p->ev.data[0].time > t_end && p->ev.data[0].type != EV_LINEAR &&
                                              p->ev.data[0].type != EV_EXPO))) {
      // a setTarget curve with nothing else starting in this quantum: one multiply per sample
      double r = exp(-dt / a.tc), d = anchor_at(&a, t0) - a.target;
      for (int i = 0; i < Q; i++) { p->buf[i] = (float)(a.target + d); d *= r; }
      p->value = p->buf[Q - 1];
      done = true;
    }
    for (int i = 0; i < Q && !done; i++) {
      double t = t0 + i * dt;
      while (cur < p->ev.len && p->ev.data[cur].time <= t) anchor_apply(&a, &p->ev.data[cur++]);
      double v;
      if (cur < p->ev.len && (p->ev.data[cur].type == EV_LINEAR || p->ev.data[cur].type == EV_EXPO)) v = ramp_at(&a, &p->ev.data[cur], t);
      else v = anchor_at(&a, t);
      p->buf[i] = (float)v;
      p->value = v;
    }
  }
  // commit the events that have started by the end of this quantum
  size_t k = 0;
  while (k < p->ev.len && p->ev.data[k].time <= t_end) anchor_apply(&p->a, &p->ev.data[k++]);
  if (k) {
    memmove(p->ev.data, p->ev.data + k, (p->ev.len - k) * sizeof(Event));
    p->ev.len -= k;
  }
  if (p->a.kind == A_TARGET && fabs(p->a.target - anchor_at(&p->a, t_end)) < 1e-7 * (fabs(p->a.target) + 1e-4))
    p->a = (Anchor){ A_CONST, t_end, p->a.target, 0, 0 };
  // modulation inputs (a-rate: summed per sample; k-rate: the first sample)
  if (p->inputs.len) {
    float in[Q] = {};
    for (size_t j = 0; j < p->inputs.len; j++) {
      WaNode *s = p->inputs.data[j];
      process(s);
      if (s->silent) continue;
      if (s->out_ch == 1) for (int i = 0; i < Q; i++) in[i] += s->out[0][i];
      else for (int i = 0; i < Q; i++) in[i] += 0.5f * (s->out[0][i] + s->out[1][i]);
    }
    if (p->constant) {
      float v = p->buf[0];
      if (arate) {
        for (int i = 0; i < Q; i++) p->buf[i] = v + in[i];
        p->constant = false;
      } else p->buf[0] = v + in[0];
    } else for (int i = 0; i < Q; i++) p->buf[i] += in[i];
  }
  if (isfinite(p->min) || isfinite(p->max)) {
    int m = p->constant ? 1 : Q;
    float lo = (float)p->min, hi = (float)p->max;
    for (int i = 0; i < m; i++) p->buf[i] = p->buf[i] < lo ? lo : p->buf[i] > hi ? hi : p->buf[i];
  }
}

static inline float pv(const WaParam *p, int i) { return p->constant ? p->buf[0] : p->buf[i]; }

// ---------------------------------------------------------------------------------------------
// graph

static WaNode *node_new(Kind k, int nports) {
  WaNode *n = xcalloc(1, sizeof *n);
  n->kind = k;
  n->nports = nports;
  n->out_ch = 1;
  n->stop_frame = INT64_MAX;
  n->rendered_q = UINT64_MAX;
  n->born_q = W.quantum;
  vec_push(&W.nodes, n);
  return n;
}

WaParam *wa_param(WaNode *n, WaParamName name) {
  WaParam *p = nullptr;
  switch (name) {
    case WP_GAIN: p = n->kind == K_GAIN ? &n->gain : n->kind == K_BIQUAD ? &n->bgain : nullptr; break;
    case WP_FREQUENCY: p = n->kind == K_OSC || n->kind == K_BIQUAD ? &n->frequency : nullptr; break;
    case WP_DETUNE: p = n->kind == K_OSC || n->kind == K_BIQUAD || n->kind == K_BUFSRC ? &n->detune : nullptr; break;
    case WP_Q: p = n->kind == K_BIQUAD ? &n->q : nullptr; break;
    case WP_PLAYBACK_RATE: p = n->kind == K_BUFSRC ? &n->rate : nullptr; break;
    case WP_OFFSET: p = n->kind == K_CONST ? &n->offset : nullptr; break;
    case WP_PAN: p = n->kind == K_SPANNER ? &n->pan : nullptr; break;
    case WP_DELAY_TIME: p = n->kind == K_DELAY ? &n->delay_time : nullptr; break;
    case WP_POSITION_X: case WP_POSITION_Y: case WP_POSITION_Z:
      p = n->kind == K_PANNER ? &n->pos[name - WP_POSITION_X] : nullptr;
      break;
  }
  if (!p) FATAL("audio node kind %d has no param %d", n->kind, name);
  return p;
}

WaParam *wa_listener(WaListenerParam p) { return &W.listener[p]; }

void wa_connect_to(WaNode *from, WaNode *to, int port) {
  wa_lock();
  CHECK(port >= 0 && port < to->nports);
  vec_push(&from->outs, ((Conn){ to, port, nullptr }));
  vec_push(&to->ports[port], from);
  wa_unlock();
}
WaNode *wa_connect(WaNode *from, WaNode *to) {
  wa_connect_to(from, to, 0);
  return to;
}
void wa_connect_param(WaNode *from, WaParam *p) {
  wa_lock();
  vec_push(&from->outs, ((Conn){ p->owner, 0, p }));
  vec_push(&p->inputs, from);
  wa_unlock();
}
WaNode *wa_chain_(WaNode *const *nodes, int n) {
  for (int i = 0; i < n - 1; i++) wa_connect(nodes[i], nodes[i + 1]);
  return nodes[n - 1];
}

static void remove_ptr(void *vec_, const void *ptr) {
  Vec(void *) *v = vec_;
  for (size_t i = 0; i < v->len; i++)
    if (v->data[i] == ptr) { vec_remove(v, i); return; }
}

static void disconnect_outs(WaNode *n) {
  for (size_t i = 0; i < n->outs.len; i++) {
    Conn *c = &n->outs.data[i];
    if (c->param) remove_ptr(&c->param->inputs, n);
    else remove_ptr(&c->to->ports[c->port], n);
  }
  n->outs.len = 0;
}
void wa_disconnect(WaNode *n) {
  wa_lock();
  disconnect_outs(n);
  wa_unlock();
}

void wa_hold(WaNode *n) { wa_lock(); n->holds++; wa_unlock(); }
void wa_release(WaNode *n) { wa_lock(); if (n->holds > 0) n->holds--; wa_unlock(); }

// ---------------------------------------------------------------------------------------------
// constructors

WaNode *wa_destination(void) { return W.dest; }

WaNode *wa_gain(double gain) {
  wa_lock();
  WaNode *n = node_new(K_GAIN, 1);
  param_init(&n->gain, n, gain, -INFINITY, INFINITY, true);
  wa_unlock();
  return n;
}

WaNode *wa_biquad(WaBiquadType type, double frequency, double q, double gain) {
  wa_lock();
  WaNode *n = node_new(K_BIQUAD, 1);
  n->btype = type;
  double nyq = W.sr / 2;
  param_init(&n->frequency, n, frequency, 0, nyq, true);
  param_init(&n->detune, n, 0, -153600, 153600, true);
  param_init(&n->q, n, q, -INFINITY, INFINITY, true);
  param_init(&n->bgain, n, gain, -INFINITY, 1541, true);
  wa_unlock();
  return n;
}

static WaNode *source_new(Kind k) {
  WaNode *n = node_new(k, 0);
  n->is_source = true;
  return n;
}

WaNode *wa_osc(WaOscType type, double frequency) {
  wa_lock();
  WaNode *n = source_new(K_OSC);
  n->wave = W.basic[type];
  double nyq = W.sr / 2;
  param_init(&n->frequency, n, frequency, -nyq, nyq, true);
  param_init(&n->detune, n, 0, -153600, 153600, true);
  wa_unlock();
  return n;
}
void wa_osc_set_wave(WaNode *osc, WaWave *w) { wa_lock(); osc->wave = w; wa_unlock(); }

WaNode *wa_buffer_source(WaBuffer *b, bool loop, double playback_rate) {
  wa_lock();
  WaNode *n = source_new(K_BUFSRC);
  n->buffer = b;
  n->loop = loop;
  param_init(&n->rate, n, playback_rate, -INFINITY, INFINITY, false);
  param_init(&n->detune, n, 0, -INFINITY, INFINITY, false);
  n->out_ch = b ? b->channels : 1;
  wa_unlock();
  return n;
}
void wa_set_loop(WaNode *src, bool loop) { wa_lock(); src->loop = loop; wa_unlock(); }

WaNode *wa_constant(double offset) {
  wa_lock();
  WaNode *n = source_new(K_CONST);
  param_init(&n->offset, n, offset, -INFINITY, INFINITY, true);
  wa_unlock();
  return n;
}

WaNode *wa_shaper(const float *curve, int count, bool oversample2x) {
  wa_lock();
  WaNode *n = node_new(K_SHAPER, 1);
  n->curve = xmalloc((size_t)count * sizeof(float));
  memcpy(n->curve, curve, (size_t)count * sizeof(float));
  n->ncurve = count;
  n->oversample = oversample2x;
  wa_unlock();
  return n;
}

WaNode *wa_compressor(double threshold, double knee, double ratio, double attack, double release) {
  wa_lock();
  WaNode *n = node_new(K_COMP, 1);
  n->comp = xcalloc(1, sizeof *n->comp);
  Compressor *c = n->comp;
  c->db_threshold = threshold;
  c->db_knee = knee;
  c->ratio = ratio;
  c->detector_average = 0;
  c->compressor_gain = 1;
  c->max_attack_diff_db = -1;
  c->last_pre_frames = -1;
  // (threshold / knee / ratio / attack / release are constant here: stored in the node's params)
  param_init(&n->gain, nullptr, attack, 0, 1, false);    // attack
  param_init(&n->rate, nullptr, release, 0, 1, false);   // release
  wa_unlock();
  return n;
}

WaNode *wa_stereo_panner(double pan) {
  wa_lock();
  WaNode *n = node_new(K_SPANNER, 1);
  param_init(&n->pan, n, pan, -1, 1, true);
  n->out_ch = 2;
  wa_unlock();
  return n;
}

WaNode *wa_delay(double delay_time, double max_delay) {
  wa_lock();
  WaNode *n = node_new(K_DELAY, 1);
  param_init(&n->delay_time, n, delay_time, 0, max_delay, true);
  n->dlen = (int)ceil(max_delay * W.sr) + Q + 2;
  for (int c = 0; c < 2; c++) n->dbuf[c] = xcalloc((size_t)n->dlen, sizeof(float));
  wa_unlock();
  return n;
}

WaNode *wa_merger(int inputs) {
  CHECK(inputs >= 1 && inputs <= 2);
  wa_lock();
  WaNode *n = node_new(K_MERGER, inputs);
  n->out_ch = inputs;
  wa_unlock();
  return n;
}

WaNode *wa_panner(WaPannerOpts o) {
  wa_lock();
  WaNode *n = node_new(K_PANNER, 1);
  n->po = o;
  if (!(n->po.max_distance > 0)) n->po.max_distance = 10000;
  param_init(&n->pos[0], n, o.x, -INFINITY, INFINITY, true);
  param_init(&n->pos[1], n, o.y, -INFINITY, INFINITY, true);
  param_init(&n->pos[2], n, o.z, -INFINITY, INFINITY, true);
  n->out_ch = 2;
  n->dist_prev = -1;
  wa_unlock();
  return n;
}

// ---- buffers ----

WaBuffer *wa_buffer(int channels, int length, double sample_rate) {
  CHECK(channels >= 1 && channels <= 2 && length >= 1);
  WaBuffer *b = xcalloc(1, sizeof *b);
  b->channels = channels;
  b->length = length;
  b->sample_rate = sample_rate;
  for (int c = 0; c < channels; c++) b->data[c] = xcalloc((size_t)length, sizeof(float));
  return b;
}
double wa_buffer_duration(const WaBuffer *b) { return b->length / b->sample_rate; }

// ---- PeriodicWave: band-limited tables, 3 per octave (like Chromium), normalized to peak 1 ----

WaWave *wa_periodic_wave(const float *real, const float *imag, int count) {
  WaWave *w = xcalloc(1, sizeof *w);
  int nmax = WAVE_N / 2;
  int have = count - 1;               // partials given (index 0 is DC, ignored)
  if (have > nmax) have = nmax;
  Fft f;
  fft_init(&f, WAVE_N);
  float *re = xmalloc(WAVE_N * sizeof(float)), *im = xmalloc(WAVE_N * sizeof(float));
  double scale = 1;
  int prev = -1;
  for (int j = 0; j < WAVE_TABLES; j++) {
    int limit = (int)floor(nmax / pow(2, j / 3.0));
    if (limit < 1) limit = 1;
    int eff = limit < have ? limit : have;
    if (eff != prev) {   // (a limit above the partials given repeats the previous table)
      memset(re, 0, WAVE_N * sizeof(float));
    memset(im, 0, WAVE_N * sizeof(float));
      for (int k = 1; k <= eff; k++) {
        float a = real ? real[k] : 0, b = imag ? imag[k] : 0;
        re[k] = 0.5f * a; im[k] = -0.5f * b;
        re[WAVE_N - k] = 0.5f * a; im[WAVE_N - k] = 0.5f * b;
      }
      fft_inverse(&f, re, im);
      if (w->ntables == 0) {   // normalized by the peak of the full-band table
        double peak = 0;
        for (int i = 0; i < WAVE_N; i++) peak = fmax(peak, fabs(re[i]));
        scale = peak > 0 ? 1 / peak : 1;
      }
      float *t = xmalloc((WAVE_N + 1) * sizeof(float));
      for (int i = 0; i < WAVE_N; i++) t[i] = (float)(re[i] * scale);
      t[WAVE_N] = t[0];
      w->table[w->ntables] = t;
      w->partials[w->ntables] = eff;
      w->ntables++;
      prev = eff;
    }
    if (limit == 1) break;
  }
  free(re);
  free(im);
  fft_free(&f);
  return w;
}

static WaWave *basic_wave(WaOscType type) {
  int n = WAVE_N / 2 + 1;
  float *real = xcalloc((size_t)n, sizeof(float)), *imag = xcalloc((size_t)n, sizeof(float));
  for (int k = 1; k < n; k++) {
    double b = 0;
    switch (type) {
      case WA_SINE: b = k == 1 ? 1 : 0; break;
      case WA_SQUARE: b = (2.0 / (k * PI)) * (1 - ((k & 1) ? -1 : 1)); break;
      case WA_SAWTOOTH: b = ((k & 1) ? 2.0 : -2.0) / (k * PI); break;
      case WA_TRIANGLE: b = 8 * sin(k * PI / 2) / ((PI * k) * (PI * k)); break;
    }
    imag[k] = (float)b;
  }
  WaWave *w = wa_periodic_wave(real, imag, type == WA_SINE ? 2 : n);
  free(real);
  free(imag);
  return w;
}

// ---- convolver ----

WaNode *wa_convolver(const WaBuffer *ir) {
  wa_lock();
  WaNode *n = node_new(K_CONV, 1);
  n->out_ch = 2;
  Convolver *c = n->conv = xcalloc(1, sizeof *c);
  // spec normalization (normalize = true)
  double power = 0;
  for (int ch = 0; ch < ir->channels; ch++)
    for (int i = 0; i < ir->length; i++) power += (double)ir->data[ch][i] * ir->data[ch][i];
  power = sqrt(power / (ir->channels * ir->length));
  if (!isfinite(power) || power < 0.000125) power = 0.000125;
  double scale = (1 / power) * 0.00125 * (44100 / ir->sample_rate);
  // leading block of the impulse: the partition latency (CONV_B samples) is taken off the front
  int skip = CONV_B;
  for (int ch = 0; ch < ir->channels; ch++)
    for (int i = 0; i < skip && i < ir->length; i++)
      if (ir->data[ch][i] != 0) FATAL("convolver: impulse must start with %d silent samples", skip);
  int len = ir->length - skip;
  c->nparts = (len + CONV_B - 1) / CONV_B;
  int N = 2 * CONV_B;
  fft_init(&c->fft, N);
  for (int ch = 0; ch < 2; ch++) {
    const float *src = ir->data[ir->channels == 2 ? ch : 0];
    c->ir_re[ch] = xcalloc((size_t)c->nparts * N, sizeof(float));
    c->ir_im[ch] = xcalloc((size_t)c->nparts * N, sizeof(float));
    for (int p = 0; p < c->nparts; p++) {
      float *re = c->ir_re[ch] + (size_t)p * N, *im = c->ir_im[ch] + (size_t)p * N;
      for (int i = 0; i < CONV_B; i++) {
        int k = skip + p * CONV_B + i;
        re[i] = k < ir->length ? (float)(src[k] * scale) : 0;
      }
      fft_forward(&c->fft, re, im);
    }
  }
  c->in_re = xcalloc((size_t)c->nparts * N, sizeof(float));
  c->in_im = xcalloc((size_t)c->nparts * N, sizeof(float));
  c->in2_re = xcalloc((size_t)c->nparts * N, sizeof(float));
  c->in2_im = xcalloc((size_t)c->nparts * N, sizeof(float));
  c->tmp_re = xcalloc((size_t)N, sizeof(float));
  c->tmp_im = xcalloc((size_t)N, sizeof(float));
  wa_unlock();
  return n;
}

// one block of uniformly partitioned overlap-save convolution
static void conv_block(Convolver *c) {
  int N = 2 * CONV_B, np = c->nparts;
  c->ring = (c->ring + 1) % np;
  for (int in = 0; in < (c->stereo_in ? 2 : 1); in++) {
    float *re = (in ? c->in2_re : c->in_re) + (size_t)c->ring * N, *im = (in ? c->in2_im : c->in_im) + (size_t)c->ring * N;
    memcpy(re, c->inbuf[in], (size_t)N * sizeof(float));
    memset(im, 0, (size_t)N * sizeof(float));
    fft_forward(&c->fft, re, im);
  }
  for (int ch = 0; ch < 2; ch++) {
    const float *xre_base = c->stereo_in && ch == 1 ? c->in2_re : c->in_re;
    const float *xim_base = c->stereo_in && ch == 1 ? c->in2_im : c->in_im;
    float *ar = c->tmp_re, *ai = c->tmp_im;
    memset(ar, 0, (size_t)N * sizeof(float));
    memset(ai, 0, (size_t)N * sizeof(float));
    // real signals: bins 0..N/2 carry everything, the rest is their conjugate mirror
    int H = N / 2;
    for (int p = 0; p < np; p++) {
      int slot = (c->ring - p + np) % np;
      const float *xr = xre_base + (size_t)slot * N, *xi = xim_base + (size_t)slot * N;
      const float *hr = c->ir_re[ch] + (size_t)p * N, *hi = c->ir_im[ch] + (size_t)p * N;
      for (int k = 0; k <= H; k++) {
        ar[k] += xr[k] * hr[k] - xi[k] * hi[k];
        ai[k] += xr[k] * hi[k] + xi[k] * hr[k];
      }
    }
    for (int k = 1; k < H; k++) { ar[N - k] = ar[k]; ai[N - k] = -ai[k]; }
    fft_inverse(&c->fft, ar, ai);
    for (int i = 0; i < CONV_B; i++) c->outbuf[ch][i] = ar[CONV_B + i] / N;
  }
}

// ---------------------------------------------------------------------------------------------
// processing

static int port_channels(const WaNode *n, int port) {
  int ch = 0;
  for (size_t i = 0; i < n->ports[port].len; i++) {
    int c = n->ports[port].data[i]->out_ch;
    if (c > ch) ch = c;
  }
  return ch ? ch : 1;
}

// mixes a port's inputs into buf with `ch` channels (speakers up / down mix)
static bool mix_port(WaNode *n, int port, int ch, float buf[2][Q]) {
  memset(buf, 0, sizeof(float) * 2 * Q);
  bool any = false;
  for (size_t j = 0; j < n->ports[port].len; j++) {
    WaNode *s = n->ports[port].data[j];
    process(s);
    if (s->silent) continue;
    any = true;
    if (s->out_ch == ch) for (int c = 0; c < ch; c++) for (int i = 0; i < Q; i++) buf[c][i] += s->out[c][i];
    else if (s->out_ch == 1) for (int c = 0; c < ch; c++) for (int i = 0; i < Q; i++) buf[c][i] += s->out[0][i];
    else for (int i = 0; i < Q; i++) buf[0][i] += 0.5f * (s->out[0][i] + s->out[1][i]);
  }
  return any;
}

static void biquad_coefs(WaBiquadType type, double f0, double Qv, double G, double sr, double c[5]) {
  // c = b0, b1, b2, a1, a2 (normalized by a0)
  double nyq = sr / 2;
  double fn = f0 / nyq;
  double A = pow(10, G / 40);
  if (fn <= 0 || fn >= 1) {
    // Chromium's edge cases: at 0 / Nyquist the filters degenerate
    double b0 = 1, b1 = 0, b2 = 0;
    switch (type) {
      case WA_LOWPASS: b0 = fn >= 1 ? 1 : 0; break;
      case WA_HIGHPASS: b0 = fn >= 1 ? 0 : 1; break;
      case WA_BANDPASS: b0 = 0; break;
      case WA_LOWSHELF: b0 = fn >= 1 ? A * A : 1; break;
      case WA_HIGHSHELF: b0 = fn >= 1 ? 1 : A * A; break;
      case WA_PEAKING: case WA_NOTCH: case WA_ALLPASS: b0 = 1; break;
    }
    c[0] = b0; c[1] = b1; c[2] = b2; c[3] = 0; c[4] = 0;
    return;
  }
  double w0 = PI * fn, cw = cos(w0), sw = sin(w0);
  double b0, b1, b2, a0, a1, a2;
  switch (type) {
    case WA_LOWPASS: case WA_HIGHPASS: {
      double alpha = sw / (2 * pow(10, Qv / 20));
      if (type == WA_LOWPASS) { b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2; }
      else { b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2; }
      a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
      break;
    }
    case WA_BANDPASS: case WA_NOTCH: case WA_ALLPASS: case WA_PEAKING: {
      if (Qv <= 0) {   // Chromium: the limits as Q -> 0
        c[0] = type == WA_BANDPASS ? 1 : type == WA_PEAKING ? A * A : type == WA_ALLPASS ? -1 : 0;
        c[1] = c[2] = c[3] = c[4] = 0;
        return;
      }
      double alpha = sw / (2 * Qv);
      if (type == WA_BANDPASS) { b0 = alpha; b1 = 0; b2 = -alpha; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; }
      else if (type == WA_NOTCH) { b0 = 1; b1 = -2 * cw; b2 = 1; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; }
      else if (type == WA_ALLPASS) { b0 = 1 - alpha; b1 = -2 * cw; b2 = 1 + alpha; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; }
      else { b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A; a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A; }
      break;
    }
    case WA_LOWSHELF: case WA_HIGHSHELF: default: {
      double alpha = sw / 2 * sqrt((A + 1 / A) * (1 / 1.0 - 1) + 2), sa = 2 * sqrt(A) * alpha;
      if (type == WA_LOWSHELF) {
        b0 = A * ((A + 1) - (A - 1) * cw + sa); b1 = 2 * A * ((A - 1) - (A + 1) * cw); b2 = A * ((A + 1) - (A - 1) * cw - sa);
        a0 = (A + 1) + (A - 1) * cw + sa; a1 = -2 * ((A - 1) + (A + 1) * cw); a2 = (A + 1) + (A - 1) * cw - sa;
      } else {
        b0 = A * ((A + 1) + (A - 1) * cw + sa); b1 = -2 * A * ((A - 1) + (A + 1) * cw); b2 = A * ((A + 1) + (A - 1) * cw - sa);
        a0 = (A + 1) - (A - 1) * cw + sa; a1 = 2 * ((A - 1) - (A + 1) * cw); a2 = (A + 1) - (A - 1) * cw - sa;
      }
      break;
    }
  }
  c[0] = b0 / a0; c[1] = b1 / a0; c[2] = b2 / a0; c[3] = a1 / a0; c[4] = a2 / a0;
}

static void do_biquad(WaNode *n, float in[2][Q], int ch) {
  param_compute(&n->frequency); param_compute(&n->detune); param_compute(&n->q); param_compute(&n->bgain);
  bool k = n->frequency.constant && n->detune.constant && n->q.constant && n->bgain.constant;
  double c[5] = {};
  // moving params: coefficients every 16 samples (a glide changes by well under 1% in that span)
  for (int i = 0; i < Q; i++) {
    if (i == 0 || (!k && (i & 15) == 0)) {
      double f = pv(&n->frequency, i), dt = pv(&n->detune, i);
      if (dt != 0) f *= pow(2, dt / 1200);
      f = fmin(W.sr / 2, fmax(0, f));
      biquad_coefs(n->btype, f, pv(&n->q, i), pv(&n->bgain, i), W.sr, c);
    }
    for (int cc = 0; cc < ch; cc++) {
      double x = in[cc][i];
      double y = c[0] * x + c[1] * n->bx1[cc] + c[2] * n->bx2[cc] - c[3] * n->by1[cc] - c[4] * n->by2[cc];
      if (fabs(y) < 1e-30) y = 0;   // no denormals
      n->bx2[cc] = n->bx1[cc]; n->bx1[cc] = x;
      n->by2[cc] = n->by1[cc]; n->by1[cc] = y;
      n->out[cc][i] = (float)y;
    }
  }
}

// frames of this quantum in which a source plays: [a, b)
static bool source_window(WaNode *n, int *a, int *b) {
  if (!n->started || n->ended) return false;
  int64_t f0 = (int64_t)W.frame, f1 = f0 + Q;
  if (n->start_frame >= f1) return false;
  if (n->stop_frame <= f0) { n->ended = true; return false; }
  *a = (int)(n->start_frame > f0 ? n->start_frame - f0 : 0);
  *b = (int)(n->stop_frame < f1 ? n->stop_frame - f0 : Q);
  if (n->stop_frame <= f1) n->ended = true;   // (after this quantum)
  return *a < *b;
}

static void do_osc(WaNode *n) {
  memset(n->out, 0, sizeof n->out);
  n->out_ch = 1;
  param_compute(&n->frequency);
  param_compute(&n->detune);
  int a, b;
  n->silent = !source_window(n, &a, &b);
  if (n->silent) return;
  const WaWave *w = n->wave;
  // table: the most partials that stay below Nyquist for the highest frequency in the quantum
  double fmaxq = 0;
  for (int i = a; i < b; i++) {
    double f = pv(&n->frequency, i), d = pv(&n->detune, i);
    if (d != 0) f *= pow(2, d / 1200);
    if (fabs(f) > fmaxq) fmaxq = fabs(f);
    if (n->frequency.constant && n->detune.constant) break;
  }
  int allowed = fmaxq > 0 ? (int)floor(W.sr / 2 / fmaxq) : WAVE_N / 2;
  int ti = 0;
  while (ti < w->ntables - 1 && w->partials[ti] > allowed) ti++;
  const float *t = w->table[ti];
  bool kf = n->frequency.constant && n->detune.constant;
  double inc = 0;
  if (kf) {
    double f = n->frequency.buf[0], d = n->detune.buf[0];
    if (d != 0) f *= pow(2, d / 1200);
    inc = f / W.sr;
  }
  double ph = n->phase;
  for (int i = a; i < b; i++) {
    if (!kf) {
      double f = n->frequency.buf[n->frequency.constant ? 0 : i], d = n->detune.buf[n->detune.constant ? 0 : i];
      if (d != 0) f *= pow(2, d / 1200);
      inc = f / W.sr;
    }
    double x = ph * WAVE_N;
    int k = (int)x;
    float fr = (float)(x - k);
    n->out[0][i] = t[k] + (t[k + 1] - t[k]) * fr;
    ph += inc;
    ph -= floor(ph);
  }
  n->phase = ph;
}

static void do_bufsrc(WaNode *n) {
  memset(n->out, 0, sizeof n->out);
  param_compute(&n->rate);
  param_compute(&n->detune);
  const WaBuffer *buf = n->buffer;
  n->out_ch = buf ? buf->channels : 1;
  int a, b;
  n->silent = !buf || !source_window(n, &a, &b);
  if (n->silent) return;
  double r = n->rate.buf[0] * pow(2, n->detune.buf[0] / 1200) * (buf->sample_rate / W.sr);
  if (!n->primed) {
    // start(when, offset); a source first pulled after its start has already played that long
    n->primed = true;
    double off = n->start_offset * buf->sample_rate;
    int64_t late = (int64_t)W.frame + a - n->start_frame;
    if (late > 0) off += late * r;
    if (n->loop) off = fmod(off, buf->length);
    n->playhead = fmax(0, off);
  }
  int len = buf->length;
  double ph = n->playhead;
  for (int i = a; i < b; i++) {
    if (n->loop) {
      if (ph >= len) ph = fmod(ph, len);
      else if (ph < 0) ph = fmod(ph, len) + len;
    } else if (ph >= len || ph < 0) {
      n->ended = true;
      break;
    }
    int k = (int)ph;
    float fr = (float)(ph - k);
    int k1 = k + 1;
    if (k1 >= len) k1 = n->loop ? 0 : k;
    for (int c = 0; c < buf->channels; c++) {
      const float *d = buf->data[c];
      n->out[c][i] = d[k] + (d[k1] - d[k]) * fr;
    }
    ph += r;
  }
  n->playhead = ph;
}

static void do_const(WaNode *n) {
  memset(n->out, 0, sizeof n->out);
  n->out_ch = 1;
  param_compute(&n->offset);
  int a, b;
  n->silent = !source_window(n, &a, &b);
  if (n->silent) return;
  for (int i = a; i < b; i++) n->out[0][i] = pv(&n->offset, i);
}

static float shape(const WaNode *n, float x) {
  int N = n->ncurve;
  double v = (N - 1) / 2.0 * (x + 1);
  if (v <= 0) return n->curve[0];
  if (v >= N - 1) return n->curve[N - 1];
  int k = (int)v;
  double f = v - k;
  return (float)((1 - f) * n->curve[k] + f * n->curve[k + 1]);
}

// 2x oversampling as in Chromium's UpSampler / DownSampler: the odd samples of the 2x stream come
// from a 128-tap half-sample interpolator (Blackman-windowed sinc), the even ones are the input
// delayed to match; back down through a 255-tap half-band lowpass (only its centre and odd taps
// are non-zero). Both kernels have unit gain at DC.
enum { OS_UP = 128, OS_J = 64 };
static float g_os_up[OS_UP], g_os_down[OS_J], g_os_centre;
static void os_kernel_init(void) {
  double sum = 0;
  for (int i = 0; i < OS_UP; i++) {
    double t = i - (OS_UP - 1) / 2.0;   // half-sample positions
    double w = 0.42 - 0.5 * cos(2 * PI * (i + 0.5) / OS_UP) + 0.08 * cos(4 * PI * (i + 0.5) / OS_UP);
    g_os_up[i] = (float)(sin(PI * t) / (PI * t) * w);
    sum += g_os_up[i];
  }
  for (int i = 0; i < OS_UP; i++) g_os_up[i] = (float)(g_os_up[i] / sum);
  sum = 0.5;
  int M = 4 * OS_J;   // window span at the 2x rate
  for (int j = 0; j < OS_J; j++) {
    int d = 2 * j + 1;
    double w = 0.42 + 0.5 * cos(2 * PI * d / M) + 0.08 * cos(4 * PI * d / M);   // centred Blackman
    g_os_down[j] = (float)(sin(PI * d / 2) / (PI * d) * w);
    sum += 2 * g_os_down[j];
  }
  for (int j = 0; j < OS_J; j++) g_os_down[j] = (float)(g_os_down[j] / sum);
  g_os_centre = (float)(0.5 / sum);
}

static void do_shaper(WaNode *n, float in[2][Q], int ch) {
  n->out_ch = ch;
  for (int c = 0; c < ch; c++) {
    if (!n->oversample) {
      for (int i = 0; i < Q; i++) n->out[c][i] = shape(n, in[c][i]);
      continue;
    }
    if (!n->os_x[c]) {
      n->os_x[c] = xcalloc(OS_UP - 1 + Q, sizeof(float));
      n->os_e[c] = xcalloc(OS_J + Q, sizeof(float));
      n->os_o[c] = xcalloc(2 * OS_J + Q, sizeof(float));
    }
    float *x = n->os_x[c], *e = n->os_e[c], *o = n->os_o[c];
    memcpy(x + OS_UP - 1, in[c], Q * sizeof(float));
    for (int i = 0; i < Q; i++) {
      int p = OS_UP - 1 + i;
      float acc = 0;
      for (int k = 0; k < OS_UP; k++) acc += g_os_up[k] * x[p - (OS_UP - 1) + k];
      e[OS_J + i] = shape(n, x[p - OS_UP / 2]);     // time p - 64
      o[2 * OS_J + i] = shape(n, acc);              // time p - 63.5
    }
    for (int m = 0; m < Q; m++) {
      float acc = g_os_centre * e[m];
      const float *oc = o + OS_J + m;   // the odd sample just after the centre is oc[0]
      for (int j = 0; j < OS_J; j++) acc += g_os_down[j] * (oc[j] + oc[-j - 1]);
      n->out[c][m] = acc;
    }
    memmove(x, x + Q, (OS_UP - 1) * sizeof(float));
    memmove(e, e + Q, OS_J * sizeof(float));
    memmove(o, o + Q, 2 * OS_J * sizeof(float));
  }
}

// ---- DynamicsCompressor: Chromium's DynamicsCompressorKernel ----

static double lin2db(double x) { return 20 * log10(x); }
static double db2lin(double x) { return pow(10, 0.05 * x); }
static double knee_curve(const Compressor *c, double x, double k) {
  if (x < c->linear_threshold) return x;
  return c->linear_threshold + (1 - exp(-k * (x - c->linear_threshold))) / k;
}
static double saturate(const Compressor *c, double x, double k) {
  if (x < c->knee_threshold) return knee_curve(c, x, k);
  double xdb = lin2db(x);
  return db2lin(c->y_knee_threshold_db + c->slope * (xdb - c->knee_threshold_db));
}
static double slope_at(const Compressor *c, double x, double k) {
  if (x < c->linear_threshold) return 1;
  double x2 = x * 1.001;
  double xdb = lin2db(x), x2db = lin2db(x2);
  double ydb = lin2db(knee_curve(c, x, k)), y2db = lin2db(knee_curve(c, x2, k));
  return (y2db - ydb) / (x2db - xdb);
}
static double k_at_slope(const Compressor *c, double desired) {
  double x = db2lin(c->db_threshold + c->db_knee);
  double mn = 0.1, mx = 10000, k = 5;
  for (int i = 0; i < 15; i++) {
    double s = slope_at(c, x, k);
    if (s < desired) mx = k; else mn = k;
    k = sqrt(mn * mx);
  }
  return k;
}
static void comp_curve(Compressor *c) {
  if (c->curve_valid) return;
  c->linear_threshold = db2lin(c->db_threshold);
  c->slope = 1 / c->ratio;
  double k = k_at_slope(c, 1 / c->ratio);
  c->knee_threshold_db = c->db_threshold + c->db_knee;
  c->knee_threshold = db2lin(c->knee_threshold_db);
  c->y_knee_threshold_db = lin2db(knee_curve(c, c->knee_threshold, k));
  c->k = k;
  c->curve_valid = true;
}

static void do_comp(WaNode *n, float in[2][Q], int ch) {
  Compressor *c = n->comp;
  n->out_ch = ch;
  comp_curve(c);
  double k = c->k, sr = W.sr;
  double full_range_gain = saturate(c, 1, k);
  double master = pow(1 / full_range_gain, 0.6);
  double attack = fmax(0.001, n->gain.a.v), release = n->rate.a.v;
  double attack_frames = attack * sr, release_frames = sr * release;
  double sat_release_frames = 0.0025 * sr;
  double y1 = release_frames * 0.09, y2 = release_frames * 0.16, y3 = release_frames * 0.42, y4 = release_frames * 0.98;
  double kA = 0.9999999999999998 * y1 + 1.8432219684323923e-16 * y2 - 1.9373394351676423e-16 * y3 + 8.824516011816245e-18 * y4;
  double kB = -1.5788320352845888 * y1 + 2.3305837032074286 * y2 - 0.9141194204840429 * y3 + 0.1623677525612032 * y4;
  double kC = 0.5334142869106424 * y1 - 1.272736789213631 * y2 + 0.9258856042207512 * y3 - 0.18656310191776226 * y4;
  double kD = 0.08783463138207234 * y1 - 0.1694162967925622 * y2 + 0.08588057951595272 * y3 - 0.00429891410546283 * y4;
  double kE = -0.042416883008123074 * y1 + 0.1115693827987602 * y2 - 0.09764676325265872 * y3 + 0.028494263462021576 * y4;
  int pre_frames = (int)(0.006 * sr);
  if (pre_frames > 1023) pre_frames = 1023;
  if (c->last_pre_frames != pre_frames) {
    c->last_pre_frames = pre_frames;
    memset(c->pre, 0, sizeof c->pre);
    c->pre_read = 0;
    c->pre_write = pre_frames;
  }
  int fi = 0;
  for (int div = 0; div < Q / 32; div++) {
    if (!isfinite(c->detector_average)) c->detector_average = 1;
    double desired = c->detector_average;
    double scaled_desired = asin(desired) / (PI / 2);
    double env_rate;
    bool releasing = scaled_desired > c->compressor_gain;
    double diff_db = lin2db(c->compressor_gain / scaled_desired);
    if (releasing) {
      c->max_attack_diff_db = -1;
      if (!isfinite(diff_db)) diff_db = -1;
      double x = fmin(0, fmax(-12, diff_db));
      x = 0.25 * (x + 12);
      double x2 = x * x, x3 = x2 * x, x4 = x2 * x2;
      double rf = kA + kB * x + kC * x2 + kD * x3 + kE * x4;
      env_rate = db2lin(5 / rf);
    } else {
      if (!isfinite(diff_db)) diff_db = 1;
      if (c->max_attack_diff_db == -1 || c->max_attack_diff_db < diff_db) c->max_attack_diff_db = diff_db;
      double eff = fmax(0.5, c->max_attack_diff_db);
      env_rate = 1 - pow(0.25 / eff, 1 / attack_frames);
    }
    for (int j = 0; j < 32; j++, fi++) {
      double input = 0;
      for (int cc = 0; cc < ch; cc++) {
        float x = in[cc][fi];
        c->pre[cc][c->pre_write] = x;
        if (input < fabs(x)) input = fabs(x);
      }
      double shaped = saturate(c, input, k);
      double att = input <= 0.0001 ? 1 : shaped / input;
      double att_db = fmax(2, -lin2db(att));
      double sat_release_rate = db2lin(att_db / sat_release_frames) - 1;
      double rate = att > c->detector_average ? sat_release_rate : 1;
      c->detector_average += (att - c->detector_average) * rate;
      c->detector_average = fmin(1, c->detector_average);
      if (!isfinite(c->detector_average)) c->detector_average = 1;
      if (env_rate < 1) c->compressor_gain += (scaled_desired - c->compressor_gain) * env_rate;
      else c->compressor_gain = fmin(1, c->compressor_gain * env_rate);
      double post = sin(PI / 2 * c->compressor_gain);
      double total = master * post;
      for (int cc = 0; cc < ch; cc++) n->out[cc][fi] = (float)(c->pre[cc][c->pre_read] * total);
      c->pre_read = (c->pre_read + 1) & 1023;
      c->pre_write = (c->pre_write + 1) & 1023;
    }
  }
}

static void do_conv(WaNode *n, float in[2][Q], int ch) {
  Convolver *c = n->conv;
  n->out_ch = 2;
  if (ch == 2) c->stereo_in = true;
  int done = 0;
  while (done < Q) {
    int take = Q - done < CONV_B - c->fill ? Q - done : CONV_B - c->fill;
    for (int i = 0; i < take; i++) {
      int pos = c->fill + i;
      n->out[0][done + i] = c->outbuf[0][pos];
      n->out[1][done + i] = c->outbuf[1][pos];
      c->inbuf[0][CONV_B + pos] = in[0][done + i];
      c->inbuf[1][CONV_B + pos] = ch == 2 ? in[1][done + i] : in[0][done + i];
    }
    c->fill += take;
    done += take;
    if (c->fill == CONV_B) {
      conv_block(c);
      for (int k = 0; k < 2; k++) memmove(c->inbuf[k], c->inbuf[k] + CONV_B, CONV_B * sizeof(float));
      c->fill = 0;
    }
  }
}

// listener basis at the start of the quantum
typedef struct Basis { double px, py, pz; double rx, ry, rz, ux, uy, uz, fx, fy, fz; } Basis;
static Basis listener_basis(void) {
  double v[9];
  for (int i = 0; i < 9; i++) { param_compute(&W.listener[i]); v[i] = W.listener[i].buf[0]; }
  Basis b = { v[0], v[1], v[2], 0, 0, 0, 0, 0, 0, 0, 0, 0 };
  double fx = v[3], fy = v[4], fz = v[5], ux = v[6], uy = v[7], uz = v[8];
  double fl = sqrt(fx * fx + fy * fy + fz * fz);
  if (fl > 0) { fx /= fl; fy /= fl; fz /= fl; }
  double rx = fy * uz - fz * uy, ry = fz * ux - fx * uz, rz = fx * uy - fy * ux;
  double rl = sqrt(rx * rx + ry * ry + rz * rz);
  if (rl > 0) { rx /= rl; ry /= rl; rz /= rl; }
  b.rx = rx; b.ry = ry; b.rz = rz;
  b.fx = fx; b.fy = fy; b.fz = fz;
  b.ux = ry * fz - rz * fy; b.uy = rz * fx - rx * fz; b.uz = rx * fy - ry * fx;   // up = right x forward
  return b;
}

// the spec's azimuth (degrees, 0 = ahead, +90 = right, +-180 = behind) of a unit direction
static double listener_azimuth(const Basis *L, double dx, double dy, double dz, double d) {
  if (d == 0) return 0;
  double upd = dx * L->ux + dy * L->uy + dz * L->uz;
  double px = dx - upd * L->ux, py = dy - upd * L->uy, pz = dz - upd * L->uz;
  double pl = sqrt(px * px + py * py + pz * pz);
  if (pl == 0) return 0;
  px /= pl; py /= pl; pz /= pl;
  double az = 180 * acos(fmin(1, fmax(-1, px * L->rx + py * L->ry + pz * L->rz))) / PI;
  if (px * L->fx + py * L->fy + pz * L->fz < 0) az = 360 - az;
  return az <= 270 ? 90 - az : 450 - az;
}

// Band gains (dB, relative to the unpanned signal) of Chrome's HRTF PannerNode for a source in the
// horizontal plane, per 15 degrees from ahead (0) to behind (180): [low < 300 Hz, mid ~1.5 kHz,
// high > 6 kHz], for the ear on the source's side and the other one. The shelves of the stand-in
// model reproduce these at the band centres (tools: `oceandrive --audio-test hrtf` vs the same
// measurement in the browser).
// The response every direction shares (fitted to Chrome's HRTF straight ahead, 20 Hz - 18 kHz,
// 0.9 dB rms): a gain, a highpass (Q in dB) and five peaking sections { f, Q, gain dB }.
static constexpr double HRTF_BASE_GAIN_DB = -6.444;
static const double HRTF_BASE[6][3] = {
  { 17.645, -6.374, 0 }, { 172.3, 0.36, 10.569 }, { 1164.271, 6.0, -7.815 }, { 7414.439, 0.925, -15.0 },
  { 4464.391, 0.649, 11.713 }, { 13642.044, 4.857, 15.0 },
};
static double g_hrtf_base[6][5], g_hrtf_base_gain;
static void hrtf_base_init(double sr) {
  for (int i = 0; i < 6; i++)
    biquad_coefs(i ? WA_PEAKING : WA_HIGHPASS, HRTF_BASE[i][0], HRTF_BASE[i][1], HRTF_BASE[i][2], sr, g_hrtf_base[i]);
  g_hrtf_base_gain = pow(10, HRTF_BASE_GAIN_DB / 20);
}
static double hrtf_base(double hb[6][4], double x) {
  for (int i = 0; i < 6; i++) {
    const double *c = g_hrtf_base[i];
    double *st = hb[i];
    double y = c[0] * x + c[1] * st[0] + c[2] * st[1] - c[3] * st[2] - c[4] * st[3];
    if (fabs(y) < 1e-30) y = 0;
    st[1] = st[0]; st[0] = x;
    st[3] = st[2]; st[2] = y;
    x = y;
  }
  return x * g_hrtf_base_gain;
}

static constexpr double HRTF_LOW_F = 500;
static constexpr double HRTF_HIGH_F = 3500;
static const double HRTF_NEAR[13][3] = {
  { -0.06, 1.24, -2.28 }, { 0.37, 1.60, 3.73 }, { 0.56, 3.02, 3.06 }, { 0.75, 4.11, 1.80 }, { 1.00, 4.39, 0.00 },
  { 1.12, 3.60, 1.72 }, { 0.92, 2.72, 2.80 }, { 0.67, 2.04, 2.69 }, { 0.41, 2.30, -0.36 }, { -0.26, 2.19, -1.69 },
  { -0.88, 2.04, -3.39 }, { -1.57, 1.74, -6.28 }, { -2.19, 0.78, -8.19 },
};
static const double HRTF_FAR[13][3] = {
  { -0.06, 1.24, -2.28 }, { -0.42, -2.05, -2.10 }, { -0.70, -3.58, -6.17 }, { -0.96, -4.75, -13.71 }, { -1.18, -5.75, -17.04 },
  { -1.54, -2.92, -23.49 }, { -1.68, -1.57, -21.23 }, { -1.67, -5.03, -24.90 }, { -1.74, -7.97, -19.16 }, { -2.21, -5.07, -16.09 },
  { -2.40, -2.92, -10.12 }, { -2.40, -1.10, -7.17 }, { -2.19, 0.78, -8.19 },
};
// th: the source's azimuth seen from this ear (positive: on its side)
static void hrtf_gains(double th, double out[3]) {
  const double (*T)[3] = th >= 0 ? HRTF_NEAR : HRTF_FAR;
  double a = fmin(180, fabs(th)) / 15;
  int i = (int)a;
  if (i >= 12) i = 11;
  double f = a - i;
  for (int b = 0; b < 3; b++) out[b] = T[i][b] + (T[i + 1][b] - T[i][b]) * f;
}

static void do_panner(WaNode *n, float in[2][Q], int ch) {
  n->out_ch = 2;
  for (int k = 0; k < 3; k++) param_compute(&n->pos[k]);
  Basis L = listener_basis();
  double sx = n->pos[0].buf[0] - L.px, sy = n->pos[1].buf[0] - L.py, sz = n->pos[2].buf[0] - L.pz;
  double d = sqrt(sx * sx + sy * sy + sz * sz);
  // distance model 'inverse'
  double ref = n->po.ref, dd = fmin(fmax(d, ref), n->po.max_distance);
  double dist = ref / (ref + n->po.rolloff * (dd - ref));
  if (!(ref > 0)) dist = 1;
  double dx = 0, dy = 0, dz = 0;
  if (d > 0) { dx = sx / d; dy = sy / d; dz = sz / d; }
  double dprev = n->dist_prev < 0 ? dist : n->dist_prev;
  n->dist_prev = dist;
  if (!n->po.hrtf) {
    // equal-power (spec)
    double az = listener_azimuth(&L, dx, dy, dz, d);
    if (az < -90) az = -180 - az;
    else if (az > 90) az = 180 - az;
    double x = ch == 1 ? (az + 90) / 180 : (az <= 0 ? (az + 90) / 90 : az / 90);
    double gl = cos(x * PI / 2), gr = sin(x * PI / 2);
    double gl0 = n->itd_prev[0], gr0 = n->itd_prev[1];
    if (n->rendered_q == 0 || (gl0 == 0 && gr0 == 0)) { gl0 = gl; gr0 = gr; }
    for (int i = 0; i < Q; i++) {
      double f = (i + 1.0) / Q, g = dprev + (dist - dprev) * f;
      double a = gl0 + (gl - gl0) * f, b = gr0 + (gr - gr0) * f;
      double l, r;
      if (ch == 1) { l = in[0][i] * a; r = in[0][i] * b; }
      else if (az <= 0) { l = in[0][i] + in[1][i] * a; r = in[1][i] * b; }
      else { l = in[0][i] * a; r = in[1][i] + in[0][i] * b; }
      n->out[0][i] = (float)(l * g);
      n->out[1][i] = (float)(r * g);
    }
    n->itd_prev[0] = gl; n->itd_prev[1] = gr;
    return;
  }
  // 'HRTF': Chromium convolves with a measured head-related set; without that data this is a
  // calibrated stand-in: per ear a broadband gain and low / high shelves following the band gains
  // measured on Chrome's PannerNode (hrtf_gains), and a Woodworth interaural delay (head radius
  // 8.75 cm)
  double az = listener_azimuth(&L, dx, dy, dz, d);
  const double hr = 0.0875, c = 343;
  double lat = sin(az * PI / 180);                      // +1 = right
  double itd = (hr / c) * (asin(fabs(lat)) + fabs(lat));   // far-ear lag (s)
  double delay[2] = { lat > 0 ? itd * W.sr : 0, lat < 0 ? itd * W.sr : 0 };   // [left, right] samples
  double tgt[2][3];
  for (int e = 0; e < 2; e++) hrtf_gains(e ? az : -az, tgt[e]);
  double k = 1 - exp(-Q / (W.sr * 0.02));
  if (!n->hrtf_init) {
    n->hrtf_init = true;
    for (int e = 0; e < 2; e++) {
      for (int b = 0; b < 3; b++) n->ear_db[e][b] = tgt[e][b];
      n->itd_prev[e] = delay[e];
      n->ear_g_prev[e] = pow(10, tgt[e][1] / 20);
    }
  } else
    for (int e = 0; e < 2; e++)
      for (int b = 0; b < 3; b++) n->ear_db[e][b] += (tgt[e][b] - n->ear_db[e][b]) * k;
  double cl[2][5], ch_[2][5], eg[2];
  for (int e = 0; e < 2; e++) {
    biquad_coefs(WA_LOWSHELF, HRTF_LOW_F, 0, n->ear_db[e][0] - n->ear_db[e][1], W.sr, cl[e]);
    biquad_coefs(WA_HIGHSHELF, HRTF_HIGH_F, 0, n->ear_db[e][2] - n->ear_db[e][1], W.sr, ch_[e]);
    eg[e] = pow(10, n->ear_db[e][1] / 20);
  }
  for (int i = 0; i < Q; i++) {
    double f = (i + 1.0) / Q, g = dprev + (dist - dprev) * f;
    int w = n->hd_pos;
    for (int cc = 0; cc < ch; cc++) n->hdelay[cc][w] = (float)hrtf_base(n->hb[cc], in[cc][i]);
    n->hd_pos = (w + 1) & 63;
    for (int e = 0; e < 2; e++) {
      // stereo input: each ear hears its own channel (as in Chromium's HRTFPanner)
      int src_ch = ch == 2 ? e : 0;
      double dl = n->itd_prev[e] + (delay[e] - n->itd_prev[e]) * f;
      double rp = w - dl;
      int r0 = (int)floor(rp);
      double fr = rp - r0;
      float s0 = n->hdelay[src_ch][r0 & 63], s1 = n->hdelay[src_ch][(r0 + 1) & 63];
      double x = s0 + (s1 - s0) * fr;
      for (int s = 0; s < 2; s++) {
        const double *cf = s ? ch_[e] : cl[e];
        double *st = n->sh[e][s];
        double y = cf[0] * x + cf[1] * st[0] + cf[2] * st[1] - cf[3] * st[2] - cf[4] * st[3];
        if (fabs(y) < 1e-30) y = 0;
        st[1] = st[0]; st[0] = x;
        st[3] = st[2]; st[2] = y;
        x = y;
      }
      double ge = n->ear_g_prev[e] + (eg[e] - n->ear_g_prev[e]) * f;
      n->out[e][i] = (float)(x * ge * g);
    }
  }
  for (int e = 0; e < 2; e++) { n->itd_prev[e] = delay[e]; n->ear_g_prev[e] = eg[e]; }
}

static void do_spanner(WaNode *n, float in[2][Q], int ch) {
  n->out_ch = 2;
  param_compute(&n->pan);
  for (int i = 0; i < Q; i++) {
    double p = pv(&n->pan, i);
    if (ch == 1) {
      double x = (p + 1) / 2;
      n->out[0][i] = (float)(in[0][i] * cos(x * PI / 2));
      n->out[1][i] = (float)(in[0][i] * sin(x * PI / 2));
    } else if (p <= 0) {
      double x = p + 1;
      n->out[0][i] = (float)(in[0][i] + in[1][i] * cos(x * PI / 2));
      n->out[1][i] = (float)(in[1][i] * sin(x * PI / 2));
    } else {
      double x = p;
      n->out[0][i] = (float)(in[0][i] * cos(x * PI / 2));
      n->out[1][i] = (float)(in[1][i] + in[0][i] * sin(x * PI / 2));
    }
  }
}

static void do_delay(WaNode *n, float in[2][Q], int ch) {
  n->out_ch = ch;
  param_compute(&n->delay_time);
  for (int i = 0; i < Q; i++) {
    int w = n->dpos;
    for (int c = 0; c < ch; c++) n->dbuf[c][w] = in[c][i];
    double ds = pv(&n->delay_time, i) * W.sr;
    double rp = w - ds;
    while (rp < 0) rp += n->dlen;
    int r0 = (int)rp;
    double fr = rp - r0;
    int r1 = r0 + 1 >= n->dlen ? 0 : r0 + 1;
    for (int c = 0; c < ch; c++) n->out[c][i] = (float)(n->dbuf[c][r0] + (n->dbuf[c][r1] - n->dbuf[c][r0]) * fr);
    n->dpos = w + 1 >= n->dlen ? 0 : w + 1;
  }
}

// dev: OD_AUDIO_PROFILE=1 prints the self time per node kind at shutdown
static int g_prof = -1;
static uint64_t g_prof_t[16], g_prof_n[16];
static void process_(WaNode *n);
static void process(WaNode *n) {
  if (n->rendered_q == W.quantum) return;
  if (g_prof < 0) g_prof = SDL_getenv("OD_AUDIO_PROFILE") != nullptr;
  if (!g_prof) { process_(n); return; }
  static uint64_t child;
  uint64_t saved = child, t0 = SDL_GetPerformanceCounter();
  child = 0;
  process_(n);
  uint64_t dt = SDL_GetPerformanceCounter() - t0;
  g_prof_t[n->kind] += dt - child;
  g_prof_n[n->kind]++;
  child = saved + dt;
}
static void process_(WaNode *n) {
  n->rendered_q = W.quantum;
  switch (n->kind) {
    case K_OSC: do_osc(n); return;
    case K_BUFSRC: do_bufsrc(n); return;
    case K_CONST: do_const(n); return;
    default: break;
  }
  float in[2][Q];
  n->silent = false;
  if (n->kind == K_MERGER) {
    bool any = false;
    for (int p = 0; p < n->nports; p++) {
      any |= mix_port(n, p, 1, in);
      memcpy(n->out[p], in[0], sizeof in[0]);
    }
    n->out_ch = n->nports;
    n->silent = !any;
    return;
  }
  int ch = n->kind == K_DEST ? 2 : port_channels(n, 0);
  bool any = mix_port(n, 0, ch, in);
  // silence propagation: a node with silent input whose own tail has decayed outputs silence
  // without running (the compressors, the convolver and the destination always run)
  bool tails = n->kind == K_BIQUAD || n->kind == K_PANNER || n->kind == K_SPANNER || n->kind == K_DELAY ||
               (n->kind == K_SHAPER && !n->oversample && shape(n, 0) == 0);
  n->silent_in_q = any ? 0 : n->silent_in_q + 1;
  if (!any && (n->kind == K_GAIN || (tails && n->quiet))) {
    param_compute(&n->gain);   // (keeps the automation committed; cheap when idle)
    n->out_ch = ch;
    n->silent = true;
    return;
  }
  if (any) n->quiet = false;
  switch (n->kind) {
    case K_DEST: case K_GAIN: {
      n->out_ch = ch;
      if (n->kind == K_DEST) { memcpy(n->out, in, sizeof n->out); break; }
      param_compute(&n->gain);
      if (n->gain.constant) {
        float g = n->gain.buf[0];
        for (int c = 0; c < ch; c++) for (int i = 0; i < Q; i++) n->out[c][i] = in[c][i] * g;
      } else {
        const float *g = n->gain.buf;
        for (int c = 0; c < ch; c++) for (int i = 0; i < Q; i++) n->out[c][i] = in[c][i] * g[i];
      }
      break;
    }
    case K_BIQUAD: n->out_ch = ch; do_biquad(n, in, ch); break;
    case K_SHAPER: do_shaper(n, in, ch); break;
    case K_COMP: do_comp(n, in, ch); break;
    case K_CONV: do_conv(n, in, ch); break;
    case K_PANNER: do_panner(n, in, ch); break;
    case K_SPANNER: do_spanner(n, in, ch); break;
    case K_DELAY: do_delay(n, in, ch); break;
    default: break;
  }
  if (tails && !any && n->silent_in_q >= (n->kind == K_DELAY ? n->dlen / Q + 2 : 2)) {
    float mx = 0;
    for (int c = 0; c < n->out_ch; c++)
      for (int i = 0; i < Q; i++) mx = fmaxf(mx, fabsf(n->out[c][i]));
    if (mx < 1e-9f) n->quiet = true;
  }
}

// ---------------------------------------------------------------------------------------------
// sources

void wa_start(WaNode *src, double when, double offset) {
  wa_lock();
  CHECK(src->is_source);
  if (!src->started) {
    src->started = true;
    double t = fmax(when, (double)W.frame / W.sr);
    src->start_frame = (int64_t)llround(t * W.sr);
    src->start_offset = offset;
  }
  wa_unlock();
}
void wa_stop(WaNode *src, double when) {
  wa_lock();
  double t = fmax(when, (double)W.frame / W.sr);
  src->stop_frame = (int64_t)llround(t * W.sr);
  if (src->started && src->stop_frame < src->start_frame) src->stop_frame = src->start_frame;
  wa_unlock();
}
void wa_on_ended(WaNode *src, void (*fn)(void *), void *user) {
  wa_lock();
  if (!src->on_ended) src->holds++;   // stays valid until the callback has run
  src->on_ended = fn;
  src->ended_user = user;
  wa_unlock();
}

void wa_timeout(double delay_s, void (*fn)(void *), void *user) {
  wa_lock();
  if (W.ntimers == W.ctimers) {
    W.ctimers = W.ctimers ? W.ctimers * 2 : 16;
    W.timers = xrealloc(W.timers, W.ctimers * sizeof *W.timers);
  }
  W.timers[W.ntimers++] = (struct Timer){ (double)W.frame / W.sr + delay_s, fn, user };
  wa_unlock();
}

void wa_poll(void) {
  for (;;) {
    wa_lock();
    struct Pending p = {};
    bool have = false;
    if (W.npend) {
      p = W.pend[0];
      memmove(W.pend, W.pend + 1, (W.npend - 1) * sizeof *W.pend);
      W.npend--;
      have = true;
    }
    wa_unlock();
    if (!have) break;
    p.fn(p.user);
    wa_release(p.node);
  }
  for (;;) {
    wa_lock();
    double now = (double)W.frame / W.sr;
    struct Timer t = {};
    bool have = false;
    for (size_t i = 0; i < W.ntimers; i++)
      if (W.timers[i].t <= now) {
        t = W.timers[i];
        memmove(W.timers + i, W.timers + i + 1, (W.ntimers - i - 1) * sizeof *W.timers);
        W.ntimers--;
        have = true;
        break;
      }
    wa_unlock();
    if (!have) break;
    t.fn(t.user);
  }
}

// ---------------------------------------------------------------------------------------------
// rendering, collection

static void free_node(WaNode *n) {
  disconnect_outs(n);
  for (int p = 0; p < n->nports; p++) {
    for (size_t i = 0; i < n->ports[p].len; i++) {
      WaNode *s = n->ports[p].data[i];
      for (size_t k = 0; k < s->outs.len; k++)
        if (s->outs.data[k].to == n && !s->outs.data[k].param) { vec_remove(&s->outs, k); break; }
    }
    vec_free(&n->ports[p]);
  }
  WaParam *ps[] = { &n->gain, &n->frequency, &n->detune, &n->q, &n->bgain, &n->rate, &n->offset, &n->pan, &n->delay_time,
                    &n->pos[0], &n->pos[1], &n->pos[2] };
  for (size_t j = 0; j < ARRAY_LEN(ps); j++) {
    WaParam *p = ps[j];
    for (size_t i = 0; i < p->inputs.len; i++) {
      WaNode *s = p->inputs.data[i];
      for (size_t k = 0; k < s->outs.len; k++)
        if (s->outs.data[k].param == p) { vec_remove(&s->outs, k); break; }
    }
    vec_free(&p->inputs);
    vec_free(&p->ev);
  }
  vec_free(&n->outs);
  free(n->curve);
  free(n->comp);
  if (n->conv) {
    Convolver *c = n->conv;
    for (int k = 0; k < 2; k++) { free(c->ir_re[k]); free(c->ir_im[k]); }
    free(c->in_re); free(c->in_im); free(c->in2_re); free(c->in2_im); free(c->tmp_re); free(c->tmp_im);
    fft_free(&c->fft);
    free(c);
  }
  for (int k = 0; k < 2; k++) { free(n->dbuf[k]); free(n->os_x[k]); free(n->os_e[k]); free(n->os_o[k]); }
  free(n);
}

// A node is alive while it is held, is a source that has not finished, or is downstream of an
// alive node; the rest is freed after a short tail (filters, delays ring out).
static void collect(void) {
  size_t nn = W.nodes.len;
  WaNode **stack = xmalloc((nn + 1) * sizeof *stack);
  size_t sp = 0;
  for (size_t i = 0; i < nn; i++) {
    WaNode *n = W.nodes.data[i];
    n->dead_mark = true;
  }
  for (size_t i = 0; i < nn; i++) {
    WaNode *n = W.nodes.data[i];
    // (a node younger than ~20 ms is still being wired up by the main thread)
    bool young = W.quantum - n->born_q < 8;
    bool root = n->holds > 0 || n->kind == K_DEST || (n->is_source && n->started && !n->ended) || young;
    if (root && n->dead_mark) { n->dead_mark = false; stack[sp++] = n; }
    while (sp) {
      WaNode *m = stack[--sp];
      for (size_t k = 0; k < m->outs.len; k++) {
        WaNode *t = m->outs.data[k].to;
        if (t->dead_mark) { t->dead_mark = false; stack[sp++] = t; }
      }
    }
  }
  free(stack);
  const int tail_q = (int)(0.3 * W.sr / Q);
  size_t w = 0;
  for (size_t i = 0; i < nn; i++) {
    WaNode *n = W.nodes.data[i];
    if (!n->dead_mark) { n->dead_q = 0; W.nodes.data[w++] = n; continue; }
    if (++n->dead_q < tail_q && !(n->is_source && (n->ended || !n->started))) { W.nodes.data[w++] = n; continue; }
    W.nodes.data[i] = nullptr;
    free_node(n);
  }
  W.nodes.len = w;
}

// Denormals: decaying envelopes and filter tails reach subnormal floats, which x86 processes very
// slowly; like browsers' audio threads, rendering flushes them to zero (FTZ + DAZ).
static void flush_denormals(void) {
#if defined(__SSE__) || defined(__x86_64__)
  _mm_setcsr(_mm_getcsr() | 0x8040);
#endif
}

static void render_quantum(float *out) {
  flush_denormals();
  // sources whose end falls in this quantum queue their onended
  process(W.dest);
  for (int i = 0; i < Q; i++) {
    out[2 * i] = W.dest->out[0][i];
    out[2 * i + 1] = W.dest->out[1][i];
  }
  // listener params advance even when nothing pulls them
  for (int i = 0; i < 9; i++) param_compute(&W.listener[i]);
  int64_t f1 = (int64_t)W.frame + Q;
  for (size_t i = 0; i < W.nodes.len; i++) {
    WaNode *n = W.nodes.data[i];
    if (!n->is_source || !n->started) continue;
    if (!n->ended && n->stop_frame <= f1) n->ended = true;   // (unpulled sources)
    if (n->ended && n->on_ended && !n->ended_queued) {
      n->ended_queued = true;
      if (W.npend == W.cpend) {
        W.cpend = W.cpend ? W.cpend * 2 : 32;
        W.pend = xrealloc(W.pend, W.cpend * sizeof *W.pend);
      }
      W.pend[W.npend++] = (struct Pending){ n->on_ended, n->ended_user, n };
    }
  }
  W.frame += Q;
  W.quantum++;
  if (++W.gc_counter >= 8) { W.gc_counter = 0; collect(); }
}

static void SDLCALL device_cb(void *user, SDL_AudioStream *stream, int additional, int total) {
  (void)user; (void)total;
  float buf[2 * Q];
  int need = additional / (int)(2 * sizeof(float));
  while (need > 0) {
    wa_lock();
    render_quantum(buf);
    wa_unlock();
    SDL_PutAudioStreamData(stream, buf, (int)sizeof buf);
    need -= Q;
  }
}

static void common_init(double sr) {
  CHECK(!W.init);
  W.sr = sr > 0 ? sr : 48000;
  W.lock = SDL_CreateMutex();
  uint64_t seed = SDL_GetPerformanceCounter() ^ (uint64_t)SDL_GetTicksNS() * 0x9E3779B97F4A7C15ull;
  W.rng[0] = seed | 1;
  W.rng[1] = seed * 0xBF58476D1CE4E5B9ull + 0x94D049BB133111EBull;
  for (int i = 0; i < 20; i++) wa_random();
  os_kernel_init();
  hrtf_base_init(W.sr);
  for (int t = 0; t < 4; t++) W.basic[t] = basic_wave((WaOscType)t);
  W.dest = node_new(K_DEST, 1);
  W.dest->out_ch = 2;
  static const double L0[9] = { 0, 0, 0, 0, 0, -1, 0, 1, 0 };
  for (int i = 0; i < 9; i++) param_init(&W.listener[i], nullptr, L0[i], -INFINITY, INFINITY, true);
  W.init = true;
}

bool wa_init(double sample_rate) {
  if (W.init) return true;
  if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    LOG("audio: %s", SDL_GetError());
    return false;
  }
  common_init(sample_rate);
  SDL_AudioSpec spec = { SDL_AUDIO_F32, 2, (int)W.sr };
  W.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, device_cb, nullptr);
  if (!W.stream) {   // (like a browser without an AudioContext: no sound at all)
    LOG("audio: no playback device (%s)", SDL_GetError());
    wa_shutdown();
    return false;
  }
  W.device = true;
  SDL_ResumeAudioStreamDevice(W.stream);
  return true;
}

void wa_init_offline(double sample_rate) { common_init(sample_rate); }

void wa_resume(void) {
  if (W.stream) SDL_ResumeAudioStreamDevice(W.stream);
}

void wa_render_offline(float *out, int frames) {
  float buf[2 * Q];
  int done = 0;
  while (done < frames) {
    wa_lock();
    render_quantum(buf);
    wa_unlock();
    int take = frames - done < Q ? frames - done : Q;
    memcpy(out + 2 * done, buf, (size_t)take * 2 * sizeof(float));
    done += take;
  }
}

void wa_shutdown(void) {
  if (!W.init) return;
  if (g_prof > 0) {
    static const char *const K[] = { "dest", "gain", "biquad", "osc", "bufsrc", "const", "shaper", "comp", "conv", "panner", "spanner", "delay", "merger" };
    for (int k = 0; k < 13; k++)
      if (g_prof_n[k]) LOG("profile %-8s %8.3f s  %10llu calls", K[k], (double)g_prof_t[k] / SDL_GetPerformanceFrequency(), (unsigned long long)g_prof_n[k]);
  }
  if (SDL_getenv("OD_AUDIO_STATS"))
    LOG("audio: %zu nodes alive at %.1f s, %llu quanta", W.nodes.len, (double)W.frame / W.sr, (unsigned long long)W.quantum);
  if (W.stream) SDL_DestroyAudioStream(W.stream);
  W.stream = nullptr;
  for (size_t i = 0; i < W.nodes.len; i++) free_node(W.nodes.data[i]);
  vec_free(&W.nodes);
  SDL_DestroyMutex(W.lock);
  W.lock = nullptr;
  free(W.pend);
  free(W.timers);
  W = (typeof(W)){};
}
