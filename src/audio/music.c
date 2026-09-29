#include "audio/music.h"

#include <math.h>
#include <string.h>

static constexpr double BPM = 118;
static constexpr double S16 = 60 / BPM / 4;

typedef enum Qual { MAJ9, SIX9, M9, DOM13, DOM9, D7B9, D7B13, D7S11, M7B5, MAJ7S11 } Qual;
static const int VOICING[10][4] = {
  [MAJ9] = { 4, 11, 14, 19 }, [SIX9] = { 4, 9, 14, 19 }, [M9] = { 10, 15, 19, 26 }, [DOM13] = { 10, 16, 21, 26 },
  [DOM9] = { 4, 10, 14, 19 }, [D7B9] = { 4, 10, 13, 19 }, [D7B13] = { 10, 16, 20 }, [D7S11] = { 10, 16, 18, 21 },
  [M7B5] = { 10, 15, 18 }, [MAJ7S11] = { 11, 16, 18 },
};
static const int VOICING_N[10] = { 4, 4, 4, 4, 4, 4, 3, 4, 3, 3 };

typedef enum Fn { I, I6, ii, iii, IV, iv, V, Valt, vi, VI7, II7, bII7, bVII7, vii, III7, FN_COUNT } Fn;
static const struct { int deg; Qual q; } CHORDS[FN_COUNT] = {
  [I] = { 0, MAJ9 }, [I6] = { 0, SIX9 }, [ii] = { 2, M9 }, [iii] = { 4, M9 }, [IV] = { 5, MAJ7S11 }, [iv] = { 5, M9 },
  [V] = { 7, DOM13 }, [Valt] = { 7, D7B9 }, [vi] = { 9, M9 }, [VI7] = { 9, D7B13 }, [II7] = { 2, DOM9 },
  [bII7] = { 1, D7S11 }, [bVII7] = { 10, DOM13 }, [vii] = { 11, M7B5 }, [III7] = { 4, D7B9 },
};
static const Fn NEXT[FN_COUNT][6] = {
  [I] = { vi, ii, iii, IV, II7, I6 }, [I6] = { vi, ii, IV, iii }, [vi] = { ii, II7, IV, vii }, [ii] = { V, Valt, bII7 },
  [V] = { I, I6, vi }, [Valt] = { I, I6, vi }, [bII7] = { I, I6 }, [iii] = { VI7, vi }, [VI7] = { ii, II7 },
  [IV] = { iv, V, iii, bVII7 }, [iv] = { I, bVII7, I6 }, [bVII7] = { I, I6 }, [II7] = { ii, V }, [vii] = { III7 }, [III7] = { vi },
};
static const int NEXT_N[FN_COUNT] = {
  [I] = 6, [I6] = 4, [vi] = 4, [ii] = 3, [V] = 3, [Valt] = 3, [bII7] = 2, [iii] = 2, [VI7] = 2, [IV] = 4, [iv] = 3,
  [bVII7] = 2, [II7] = 2, [vii] = 1, [III7] = 1,
};
static const int KEYS[6] = { 0, 5, -2, 3, -4, 2 };
static const double MOTIFS[6][5] = { { 0, 3, 6 }, { 2, 4, 6, 7 }, { 0, 2, 3, 6 }, { 3, 4, 6 }, { 1, 3, 4, 6, 7 }, { 0, 1.5, 3 } };
static const int MOTIF_N[6] = { 3, 4, 4, 3, 5, 3 };
static const int PENTA[5] = { 0, 2, 4, 7, 9 };

static bool guitar_hit(int cyc) { return cyc == 0 || cyc == 6 || cyc == 12 || cyc == 20 || cyc == 26; }

typedef struct Chord { int root, up[4], nup; bool tones[12]; } Chord;

typedef struct Pluck { int key; WaBuffer *b; } Pluck;

struct Music {
  AudioEnv *env;
  Spatial *sp;
  WaNode *bus, *gtr, *ep;
  Vec(Pluck) plucks;
  int key;
  Fn form[32];
  int nform;
  long step;
  double next_time;
  bool anticip;
  int prev_mel;
  Chord cur, nxt;
};

static void gen_phrase(Fn *p) {
  p[0] = I;
  for (int n = 1; n < 6; n++) {
    Fn last = p[n - 1];
    p[n] = NEXT[last][(int)(wa_random() * NEXT_N[last])];
  }
  p[6] = ii;
  static const Fn CAD[3] = { V, Valt, bII7 };
  p[7] = PICK(CAD);
}

static void new_form(Music *m) {
  Fn A[8], Bp[8];
  gen_phrase(A);
  gen_phrase(Bp);
  for (int i = 0; i < 8; i++) { m->form[i] = A[i]; m->form[8 + i] = A[i]; m->form[16 + i] = Bp[i]; m->form[24 + i] = A[i]; }
  m->nform = 32;
  if (wa_random() < 0.35) m->key = PICK(KEYS);
}

static Chord voicing(const Music *m, Fn name) {
  int deg = CHORDS[name].deg;
  Qual q = CHORDS[name].q;
  int pc = (((deg + m->key) % 12) + 12) % 12;
  Chord c = { .root = 40 + ((pc - 4 + 12) % 12) };   // E2..Eb3
  c.nup = VOICING_N[q];
  for (int i = 0; i < c.nup; i++) c.up[i] = c.root + VOICING[q][i];
  for (;;) {
    double mean = 0;
    for (int i = 0; i < c.nup; i++) mean += c.up[i];
    mean /= c.nup;
    if (mean < 57) for (int i = 0; i < c.nup; i++) c.up[i] += 12;
    else break;
  }
  for (;;) {
    double mean = 0;
    for (int i = 0; i < c.nup; i++) mean += c.up[i];
    mean /= c.nup;
    if (mean > 68) for (int i = 0; i < c.nup; i++) c.up[i] -= 12;
    else break;
  }
  c.tones[c.root % 12] = true;
  for (int i = 0; i < c.nup; i++) c.tones[c.up[i] % 12] = true;
  return c;
}

Music *create_music(AudioEnv *env, double x, double y, double z) {
  Music *m = xcalloc(1, sizeof *m);
  m->env = env;
  SpatialOpts o = spatial_opts();
  o.x = x; o.y = y; o.z = z; o.ref = 3.5; o.rolloff = 1.1; o.air_scale = 25; o.occl = true; o.occl_f = 14000; o.occl_d = 7;
  o.wet = 0.45; o.wet_fall = 25;
  m->sp = create_spatial(env, o);
  m->bus = wa_gain(0.5);
  wa_chain(m->bus, bq(WA_HIGHPASS, 75, 0.7), bq(WA_LOWPASS, 9000, 0.7), m->sp->input);
  m->gtr = wa_gain(1);
  wa_chain(m->gtr, bqg(WA_PEAKING, 105, 1.2, 4), bqg(WA_PEAKING, 230, 1.5, 2.5), bq(WA_LOWPASS, 4200, 0.6), m->bus);
  m->ep = wa_gain(0.11);
  wa_chain(m->ep, bq(WA_LOWPASS, 2800, 0.6), m->bus);
  wa_hold(m->bus);
  wa_hold(m->gtr);
  wa_hold(m->ep);
  m->key = PICK(KEYS);
  m->next_time = -1;
  m->prev_mel = 72;
  return m;
}

static WaBuffer *pluck_buf(Music *m, int midi) {
  int key = midi * 4 + (int)(wa_random() * 3);
  for (size_t i = 0; i < m->plucks.len; i++)
    if (m->plucks.data[i].key == key) return m->plucks.data[i].b;
  bool low = midi < 52;
  double f = mtof(midi);
  PluckOpts o;
  o.seconds = low ? 2.2 : 1.6;
  o.bright = low ? 0.25 : rr(0.35, 0.5);
  o.pos = rr(0.1, 0.2);
  o.t60 = (low ? 3.2 : 2.2) * pow(196 / f, 0.3);
  int len;
  float *data = render_pluck(wa_sample_rate(), f, o, &len);
  WaBuffer *b = wa_buffer(1, len, wa_sample_rate());
  memcpy(b->data[0], data, (size_t)len * sizeof(float));
  free(data);
  vec_push(&m->plucks, ((Pluck){ key, b }));
  return b;
}

static void pluck(Music *m, int midi, double t, double vel, double hold, WaNode *dest, double tc) {
  WaNode *src = wa_buffer_source(pluck_buf(m, midi), false, 1);
  WaNode *g = wa_gain(vel);
  wa_connect(wa_connect(src, g), dest);
  wa_set_at(wa_param(g, WP_GAIN), vel, t);
  wa_target(wa_param(g, WP_GAIN), 0, t + hold, tc);
  wa_start(src, t, 0);
  wa_stop(src, t + hold + tc * 8);
}

static void strum(Music *m, const Chord *ch, double t, double vel) {
  double hold = rr(0.26, 0.4);
  double tt = t;
  for (int i = 0; i < ch->nup; i++) {
    double v = vel * rr(0.8, 1) * 0.2;
    pluck(m, ch->up[i], tt, v, hold, m->gtr, 0.07);
    tt += rr(0.004, 0.012);
  }
}

static void bass(Music *m, int midi, double t, double vel, double hold) {
  pluck(m, midi, t, vel * 0.3, hold, m->gtr, 0.12);
  WaNode *o = wa_osc(WA_SINE, mtof(midi));
  WaNode *g = wa_gain(0);
  wa_connect(wa_connect(o, g), m->bus);
  perc(wa_param(g, WP_GAIN), t, vel * 0.1, 0.01, hold + 0.25);
  wa_start(o, t, 0);
  wa_stop(o, t + hold + 0.4);
}

static void noise_hit(Music *m, WaBuffer *buf, double t, WaNode *const *filters, int nf, double peak, double a, double d) {
  WaNode *s = wa_buffer_source(buf, false, 1);
  WaNode *g = wa_gain(0);
  WaNode *prev = s;
  for (int i = 0; i < nf; i++) prev = wa_connect(prev, filters[i]);
  wa_connect(wa_connect(prev, g), m->bus);
  perc(wa_param(g, WP_GAIN), t, peak, a, d);
  wa_start(s, t, wa_random() * (wa_buffer_duration(buf) - 1));
  wa_stop(s, t + a + d * 1.5);
}
static void shaker(Music *m, double t, double a) {
  double at = rr(0.008, 0.02);
  noise_hit(m, m->env->B.white, t, (WaNode *const[]){ bq(WA_HIGHPASS, 5000, 0.7), bq(WA_BANDPASS, 9000, 0.7) }, 2, a * 0.07, at, 0.06);
}
static void brush(Music *m, double t, double a) {
  noise_hit(m, m->env->B.pink, t, (WaNode *const[]){ bq(WA_BANDPASS, 2500, 0.5) }, 1, a * 0.06, 0.03, 0.22);
}

static void epiano(Music *m, double t, int midi, double dur, double vel) {
  double f = mtof(midi);
  WaNode *car = wa_osc(WA_SINE, f), *mod = wa_osc(WA_SINE, f);
  WaNode *mg = wa_gain(0);
  wa_connect_param(wa_connect(mod, mg), wa_param(car, WP_FREQUENCY));
  wa_set_at(wa_param(mg, WP_GAIN), f * 1.6, t);
  wa_target(wa_param(mg, WP_GAIN), f * 0.25, t, 0.25);
  WaNode *tine = wa_osc(WA_SINE, f * 7);
  WaNode *tg = wa_gain(0);
  wa_connect_param(wa_connect(tine, tg), wa_param(car, WP_FREQUENCY));
  wa_set_at(wa_param(tg, WP_GAIN), f * 0.8, t);
  wa_target(wa_param(tg, WP_GAIN), 0, t, 0.02);
  WaNode *g = wa_gain(0);
  wa_connect(wa_connect(car, g), m->ep);
  WaParam *gg = wa_param(g, WP_GAIN);
  wa_set_at(gg, 0, t);
  wa_linear(gg, vel, t + 0.003);
  wa_target(gg, vel * 0.3, t + 0.003, 0.6);
  wa_target(gg, 0, t + dur, 0.15);
  WaNode *os[3] = { car, mod, tine };
  for (int i = 0; i < 3; i++) { wa_start(os[i], t, 0); wa_stop(os[i], t + dur + 1); }
}

static bool in_penta(int pc) {
  for (int i = 0; i < 5; i++) if (PENTA[i] == pc) return true;
  return false;
}

static void melody(Music *m, double t, const Chord *ch) {
  int pool[17], np = 0;
  for (int n = 65; n <= 81; n++) {
    int pc = n % 12;
    if (ch->tones[pc] || (in_penta((((pc - m->key) % 12) + 12) % 12) && wa_random() < 0.3)) pool[np++] = n;
  }
  if (!np) return;
  int mi = (int)(wa_random() * 6);
  double times[5];
  int nt = MOTIF_N[mi];
  for (int i = 0; i < nt; i++) times[i] = t + MOTIFS[mi][i] * 2 * S16;
  int cur = m->prev_mel;
  for (int i = 0; i < nt; i++) {
    int near[17], nn = 0;
    for (int k = 0; k < np; k++)
      if (pool[k] != cur && abs(pool[k] - cur) <= 4) near[nn++] = pool[k];
    if (nn) cur = near[(int)(wa_random() * nn)];
    else {
      int best = pool[0];   // pool.reduce: the first nearest
      for (int k = 1; k < np; k++)
        if (abs(pool[k] - cur) < abs(best - cur)) best = pool[k];
      cur = best;
    }
    double dur = i < nt - 1 ? times[i + 1] - times[i] : rr(0.6, 1.0);
    double jit = rr(-0.006, 0.01), vel = rr(0.5, 0.8);
    epiano(m, times[i] + jit, cur, dur, vel);
  }
  m->prev_mel = cur;
}

static void play(Music *m, long st, double t) {
  int bs = (int)(st % 16);
  long bar = st / 16;
  if (bs == 0) {
    if (!m->nform || bar % m->nform == 0) new_form(m);
    m->cur = voicing(m, m->form[bar % m->nform]);
    m->nxt = voicing(m, m->form[(bar + 1) % m->nform]);
    if (bar % m->nform > 1 && wa_random() < 0.35) melody(m, t, &m->cur);
  }
  double h = t + rr(-0.004, 0.006);
  int cyc = (int)(bar % 2) * 16 + bs;
  if (bs == 0) {
    bool skip = m->anticip;
    m->anticip = false;
    if (!skip) strum(m, &m->cur, h, rr(0.6, 0.75));
  } else if (bs == 14 && wa_random() < 0.28) {
    strum(m, &m->nxt, h, rr(0.5, 0.65));
    m->anticip = true;
  } else if (guitar_hit(cyc) && wa_random() > 0.08) {
    strum(m, &m->cur, h, rr(0.45, 0.65));
  }
  if (bs == 0) bass(m, m->cur.root, h, 0.95, 0.9);
  if (bs == 8) {
    int f5 = m->cur.root + 7;
    if (f5 > 52) f5 -= 12;
    bass(m, f5, h, 0.8, 0.8);
  }
  if (bs == 6 && wa_random() < 0.25) bass(m, m->cur.root, h, 0.35, 0.2);
  static const double SH[4] = { 0.55, 0.22, 0.8, 0.3 };
  shaker(m, h, SH[bs % 4] * rr(0.8, 1.1));
  if (bs == 4 || bs == 12) brush(m, h, rr(0.7, 1));
}

void music_tick(Music *m, double now, double until) {
  if (m->next_time < 0) { m->next_time = now + 0.1; m->step = 0; }
  if (m->next_time < now - 0.05) { m->next_time = now + 0.05; m->step = (long)ceil(m->step / 16.0) * 16; }
  while (m->next_time < until) {
    play(m, m->step, m->next_time);
    m->step++;
    m->next_time += S16;
  }
}
