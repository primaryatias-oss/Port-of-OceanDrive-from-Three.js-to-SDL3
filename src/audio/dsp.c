#include "audio/dsp.h"

#include <math.h>
#include <string.h>

#include "core/vec.h"

static constexpr double PI = 3.141592653589793;
static constexpr double CTRL_RATE = 8000;

double rr(double a, double b) { return a + wa_random() * (b - a); }
double clamp_d(double v, double a, double b) { return v < a ? a : v > b ? b : v; }
double mtof(double m) { return 440 * pow(2, (m - 69) / 12); }

WaNode *bq(WaBiquadType type, double frequency, double q) { return wa_biquad(type, frequency, q, 0); }
WaNode *bqg(WaBiquadType type, double frequency, double q, double gain) { return wa_biquad(type, frequency, q, gain); }

void ramp(WaParam *p, double t0, const double (*pts)[2], int n) {
  wa_set_at(p, pts[0][1], t0 + pts[0][0]);
  for (int i = 1; i < n; i++) wa_linear(p, pts[i][1], t0 + pts[i][0]);
}

void perc(WaParam *p, double t, double peak, double a, double d) {
  wa_set_at(p, 0, t);
  wa_linear(p, peak, t + a);
  wa_target(p, 0, t + a, d / 4);
}

WaNode *loop_src(WaBuffer *b, double rate, double at, double offset) {
  WaNode *s = wa_buffer_source(b, true, rate);
  double off = isnan(offset) ? wa_random() * wa_buffer_duration(b) : offset;
  wa_start(s, isnan(at) ? wa_now() : at, off);
  return s;
}

// ---- looping buffers: the tail is crossfaded into the head so the loop point is inaudible ----

typedef void (*Fill)(float *d, int n, int len, const void *arg);

static WaBuffer *loop_buffer(double seconds, Fill fill, const void *arg, double rate, bool normalize, bool linear) {
  int len = (int)floor(seconds * rate);
  int fade = (int)floor(fmin(0.3, seconds * 0.1) * rate);
  float *tmp = xcalloc((size_t)(len + fade), sizeof(float));
  fill(tmp, len + fade, len, arg);
  for (int i = 0; i < fade; i++) {
    double w = (double)i / fade;
    double a = linear ? w : sin(w * PI * 0.5);
    double b = linear ? 1 - w : cos(w * PI * 0.5);
    tmp[i] = (float)(tmp[i] * a + tmp[len + i] * b);
  }
  if (normalize) {
    double pk = 1e-9;
    for (int i = 0; i < len; i++) pk = fmax(pk, fabs(tmp[i]));
    double k = 0.95 / pk;
    for (int i = 0; i < len; i++) tmp[i] = (float)(tmp[i] * k);
  }
  WaBuffer *buf = wa_buffer(1, len, rate);
  memcpy(buf->data[0], tmp, (size_t)len * sizeof(float));
  free(tmp);
  return buf;
}

static void fill_white(float *d, int n, int len, const void *arg) {
  (void)len; (void)arg;
  for (int i = 0; i < n; i++) d[i] = (float)(wa_random() * 2 - 1);
}

static void fill_pink(float *d, int n, int len, const void *arg) {   // Paul Kellet's refined pink filter
  (void)len; (void)arg;
  double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
  for (int i = 0; i < n; i++) {
    double w = wa_random() * 2 - 1;
    b0 = 0.99886 * b0 + w * 0.0555179; b1 = 0.99332 * b1 + w * 0.0750759;
    b2 = 0.969 * b2 + w * 0.153852; b3 = 0.8665 * b3 + w * 0.3104856;
    b4 = 0.55 * b4 + w * 0.5329522; b5 = -0.7616 * b5 - w * 0.016898;
    d[i] = (float)(b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362);
    b6 = w * 0.115926;
  }
}

static void fill_brown(float *d, int n, int len, const void *arg) {
  (void)len; (void)arg;
  double l = 0;
  for (int i = 0; i < n; i++) { l = (l + 0.02 * (wa_random() * 2 - 1)) / 1.02; d[i] = (float)l; }
}

// sparse random grains (gravel, foam fizz, sand crunch)
static void fill_crackle(float *d, int n, int len, const void *arg) {
  (void)len;
  double density = *(const double *)arg, y = 0;
  for (int i = 0; i < n; i++) {
    double x = 0;
    if (wa_random() < density) {
      double s = wa_random() * 2 - 1, r = wa_random();
      x = s * (0.15 + 0.85 * r * r);
    }
    y = x + 0.45 * y;
    d[i] = (float)y;
  }
}

// smooth random control signal in [0, 1] (periodic over the loop), pps = variation points / s
typedef struct Ctrl { double pps; int octaves; double pw; } Ctrl;
static void fill_control(float *d, int tot, int len, const void *arg) {
  const Ctrl *c = arg;
  float *acc = xcalloc((size_t)tot, sizeof(float));
  for (int o = 0; o < c->octaves; o++) {
    int n = (int)fmax(2, round(((double)len / CTRL_RATE) * c->pps * pow(2, o)));
    double *pts = xmalloc((size_t)n * sizeof(double));
    for (int i = 0; i < n; i++) pts[i] = wa_random();
    double amp = pow(0.55, o);
    for (int i = 0; i < tot; i++) {
      double p = ((double)(i % len) / len) * n;
      int k = (int)floor(p);
      double f = p - k, s = (1 - cos(f * PI)) / 2;
      acc[i] = (float)(acc[i] + (pts[k % n] * (1 - s) + pts[(k + 1) % n] * s) * amp);
    }
    free(pts);
  }
  double lo = INFINITY, hi = -INFINITY;
  for (int i = 0; i < tot; i++) { lo = fmin(lo, acc[i]); hi = fmax(hi, acc[i]); }
  double span = hi - lo != 0 ? hi - lo : 1;
  for (int i = 0; i < tot; i++) d[i] = (float)pow((acc[i] - lo) / span, c->pw);
  free(acc);
}

Buffers make_buffers(void) {
  double sr = wa_sample_rate();
  static const double D1 = 0.0025, D2 = 0.02;
  static const Ctrl GUST = { 0.09, 3, 1.6 }, FLUTTER = { 9, 2, 1 }, SWELL = { 0.12, 2, 1.2 };
  return (Buffers){
    .white = loop_buffer(6, fill_white, nullptr, sr, true, false),
    .pink = loop_buffer(7, fill_pink, nullptr, sr, true, false),
    .brown = loop_buffer(8, fill_brown, nullptr, sr, true, false),
    .crackle = loop_buffer(5, fill_crackle, &D1, sr, true, false),
    .crackle_dense = loop_buffer(4, fill_crackle, &D2, sr, true, false),
    .gust = loop_buffer(71, fill_control, &GUST, CTRL_RATE, false, true),
    .flutter = loop_buffer(23, fill_control, &FLUTTER, CTRL_RATE, false, true),
    .swell = loop_buffer(53, fill_control, &SWELL, CTRL_RATE, false, true),
  };
}

WaBuffer *make_impulse(double seconds, double rt60) {
  double sr = wa_sample_rate();
  int len = (int)floor(seconds * sr);
  WaBuffer *buf = wa_buffer(2, len, sr);
  int pre = (int)floor(0.012 * sr);
  for (int c = 0; c < 2; c++) {
    float *d = buf->data[c];
    double lp = 0;
    for (int i = pre; i < len; i++) {
      double t = (i - pre) / sr;
      double env = exp((-6.91 * t) / rt60) * fmin(1, t / 0.006);
      double a = 0.8 - 0.72 * fmin(1, t / seconds);
      lp += a * ((wa_random() * 2 - 1) * env - lp);
      d[i] = (float)lp;
    }
    for (int k = 0; k < 12; k++) {
      double tt = 0.006 + wa_random() * 0.07;
      int i0 = pre + (int)floor(tt * sr);
      double amp = (0.6 * (1 - tt / 0.08)) * (wa_random() < 0.5 ? -1 : 1);
      for (int j = 0; j < 24 && i0 + j < len; j++) d[i0 + j] = (float)(d[i0 + j] + amp * exp(-j / 5.0) * (wa_random() * 0.6 + 0.4));
    }
  }
  return buf;
}

float *render_pluck(double sr, double freq, PluckOpts o, int *out_len) {
  int len = (int)floor(o.seconds * sr);
  float *out = xcalloc((size_t)len, sizeof(float));
  double L = sr / freq - 0.5;
  int N = (int)fmax(2, floor(L));
  double frac = L - N;
  double C = (1 - frac) / (1 + frac);
  float *exc = xcalloc((size_t)N, sizeof(float)), *buf = xcalloc((size_t)N, sizeof(float));
  double lp = 0;
  double a = 0.12 + 0.7 * o.bright;
  for (int i = 0; i < N; i++) { lp += a * (wa_random() * 2 - 1 - lp); exc[i] = (float)lp; }
  int P = (int)fmax(1, round(o.pos * N));
  double mean = 0;
  for (int i = 0; i < N; i++) { buf[i] = exc[i] - exc[(i - P + N) % N]; mean += buf[i]; }
  mean /= N;
  double pk = 1e-9;
  for (int i = 0; i < N; i++) { buf[i] = (float)(buf[i] - mean); pk = fmax(pk, fabs(buf[i])); }
  for (int i = 0; i < N; i++) buf[i] = (float)(buf[i] / pk);
  double rho = pow(0.001, 1 / (freq * o.t60));
  int idx = 0;
  double last = 0, apx = 0, apy = 0;
  for (int i = 0; i < len; i++) {
    double s = buf[idx];
    out[i] = (float)s;
    double avg = rho * 0.5 * (s + last);
    last = s;
    double ap = C * avg + apx - C * apy;
    apx = avg; apy = ap;
    buf[idx] = (float)ap;
    if (++idx == N) idx = 0;
  }
  int att = (int)floor(0.0015 * sr), rel = (int)floor(0.05 * sr);
  for (int i = 0; i < att; i++) out[i] = (float)(out[i] * ((double)i / att));
  for (int i = 0; i < rel; i++) out[len - 1 - i] = (float)(out[len - 1 - i] * ((double)i / rel));
  free(exc);
  free(buf);
  *out_len = len;
  return out;
}

float *soft_clip_curve(int n, double knee) {
  float *c = xmalloc((size_t)n * sizeof(float));
  for (int i = 0; i < n; i++) {
    double x = ((double)i / (n - 1)) * 2 - 1, ax = fabs(x);
    c[i] = (float)(ax < knee ? x : (x > 0 ? 1 : x < 0 ? -1 : 0) * (knee + (1 - knee) * tanh((ax - knee) / (1 - knee))));
  }
  return c;
}
