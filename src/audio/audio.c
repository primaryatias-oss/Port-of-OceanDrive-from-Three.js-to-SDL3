#include "audio/audio.h"

#include <math.h>

#include "audio/music.h"
#include "audio/waves.h"
#include "audio/wind.h"
#include "world/layout.h"
#include "world/palms.h"

static constexpr double STRIDE = 0.75;

typedef struct WaveCb { void (*fn)(const AudioWave *, void *); void *user; } WaveCb;

struct Audio {
  bool reduced, started, muted, auto_steps;
  double vol;
  AudioEnv *env;
  Waves *waves;
  Gulls *gulls;
  AudioCars *cars;
  Music *music;
  Footsteps *steps;
  VehicleAudio *vehicles;
  double next_wave, next_gull, next_car, spatial_acc;
  uint64_t last_tick_ns;
  Listener L;
  Vec(WaveCb) wave_cbs;
  struct { bool have; double x, z, acc, still; } walk;
  GullSourceFn gull_source;
  void *gull_user;
};

Audio *create_audio(bool reduced_voices) {
  Audio *a = xcalloc(1, sizeof *a);
  a->reduced = reduced_voices;
  a->vol = 0.8;
  a->auto_steps = true;
  a->L = (Listener){ 0, EYE_HEIGHT, 0, 0, 0, -1, 0, 1, 0 };
  return a;
}

static void apply_listener(const Listener *L, bool immediate) {
  double v[9] = { L->x, L->y, L->z, L->fx, L->fy, L->fz, L->ux, L->uy, L->uz };
  double t = wa_now();
  for (int i = 0; i < 9; i++) {
    WaParam *p = wa_listener((WaListenerParam)i);
    if (immediate) wa_set_value(p, v[i]);
    else wa_target(p, v[i], t, 0.015);
  }
}

static void apply_volume(Audio *a, double tc) {
  if (!a->started) return;
  wa_target(wa_param(a->env->volume, WP_GAIN), a->muted ? 0 : a->vol, wa_now(), tc);
}

static void tick(Audio *a) {
  double now = wa_now();
  double ahead = now + 0.35;
  music_tick(a->music, now, ahead);
  if (a->next_wave < now) a->next_wave = now + 0.1;
  while (a->next_wave < ahead) {
    WaveBreak w = waves_break_at(a->waves, a->next_wave, &a->L, NAN);
    AudioWave aw = { w.t, now, w.k, w.size, w.runup, w.z };   // w.t is on the audio clock
    for (size_t i = 0; i < a->wave_cbs.len; i++) a->wave_cbs.data[i].fn(&aw, a->wave_cbs.data[i].user);
    a->next_wave += rr(6, 10);
  }
  if (a->next_gull < now) a->next_gull = now + rr(1, 4);
  while (a->next_gull < ahead) {
    gulls_call_at(a->gulls, a->next_gull, &a->L, nullptr);
    a->next_gull += a->reduced ? rr(10, 30) : rr(5, 20);
  }
  if (now >= a->next_car) {
    cars_spawn(a->cars, (CarSpawn){ 0, NAN, NAN, NAN });
    a->next_car = now + rr(40, 70);
  }
}

bool audio_start(Audio *a) {
  if (a->started) { wa_resume(); return true; }
  if (!wa_init(48000)) return false;
  wa_lock();   // (the whole scene is wired before the audio thread may collect anything)
  a->env = create_engine();
  a->env->listener = a->L;
  apply_listener(&a->L, true);
  // buildScene: fewer simultaneous voices on small devices (every other palm-cluster rustle)
  a->waves = create_waves(a->env, SAND.waterline);
  a->gulls = create_gulls(a->env);
  int np;
  const double (*palms)[2] = palm_clusters(&np);
  if (a->reduced) {
    double (*half)[2] = xmalloc((size_t)(np + 1) / 2 * sizeof *half);
    int k = 0;
    for (int i = 0; i < np; i += 2) { half[k][0] = palms[i][0]; half[k][1] = palms[i][1]; k++; }
    create_wind(a->env, (const double (*)[2])half, k);
    free(half);
  } else create_wind(a->env, palms, np);
  a->cars = create_cars(a->env);
  a->music = create_music(a->env, -29, 1.6, -10);   // PATIO
  a->steps = create_footsteps(a->env);
  a->vehicles = create_vehicle_audio(a->env);
  gulls_set_source(a->gulls, a->gull_source, a->gull_user);
  a->started = true;
  double now = wa_now();
  a->next_wave = now + 0.8;
  a->next_gull = now + rr(2, 6);
  a->next_car = now + rr(10, 25);
  a->last_tick_ns = SDL_GetTicksNS();
  tick(a);
  apply_volume(a, 0.6);
  wa_unlock();
  return true;
}

// Step every ~0.75 m of horizontal travel while the camera is on the ground.
static void auto_step(Audio *a, double dt) {
  const Listener *L = &a->L;
  if (!a->walk.have) { a->walk.have = true; a->walk.x = L->x; a->walk.z = L->z; return; }
  double d = js_hypot2(L->x - a->walk.x, L->z - a->walk.z);
  a->walk.x = L->x; a->walk.z = L->z;
  if (d > 4 || dt <= 0) { a->walk.acc = 0; return; }
  double feet = L->y - EYE_HEIGHT;
  double ground = groundHeight(L->x, L->z);
  bool on_tower = fabs(L->x - TOWER.x) < 3.5 && fabs(L->z - TOWER.z) < 4.5 && feet > ground - 0.3 && feet < TOWER.deckHeight + 0.5;
  bool grounded = fabs(feet - ground) < 0.35 || on_tower;
  double speed = d / dt;
  if (!grounded || speed < 0.3) {
    a->walk.still += dt;
    if (a->walk.still > 0.25) a->walk.acc = STRIDE * 0.6;
    return;
  }
  a->walk.still = 0;
  double stride = STRIDE * (speed > 3 ? 1.35 : 1);
  a->walk.acc += d;
  if (a->walk.acc >= stride) {
    a->walk.acc -= stride;
    audio_footstep(a, surface_at(L->x, L->z, feet, SAND.waterline), fmin(1.25, 0.8 + speed * 0.08), NAN);
  }
}

void audio_update(Audio *a, double dt, const Camera *cam) {
  // readPose: the camera's world matrix
  const double *e = cam->node->matrix_world.e;
  Listener *L = &a->L;
  L->x = e[12]; L->y = e[13]; L->z = e[14];
  double fl = js_hypot3(e[8], e[9], e[10]), ul = js_hypot3(e[4], e[5], e[6]);
  if (fl == 0) fl = 1;
  if (ul == 0) ul = 1;
  L->fx = -e[8] / fl; L->fy = -e[9] / fl; L->fz = -e[10] / fl;
  L->ux = e[4] / ul; L->uy = e[5] / ul; L->uz = e[6] / ul;
  if (a->auto_steps) auto_step(a, dt);
  if (!a->started) return;
  wa_poll();
  wa_lock();
  a->env->listener.x = L->x; a->env->listener.y = L->y; a->env->listener.z = L->z;
  apply_listener(L, false);
  car_audio_update(a->cars, L);
  a->spatial_acc += dt;
  if (a->spatial_acc >= 0.05) {
    a->spatial_acc = 0;
    waves_track(a->waves, L);
    for (size_t i = 0; i < a->env->spatials.len; i++) spatial_update(a->env->spatials.data[i], L, false);
  }
  // setInterval(tick, 50)
  uint64_t now = SDL_GetTicksNS();
  if (now - a->last_tick_ns >= 50000000ull) {
    a->last_tick_ns = now;
    tick(a);
  }
  wa_unlock();
}

void audio_footstep(Audio *a, Surface s, double gain, double depth) {
  if (!a->started) return;
  wa_lock();
  footsteps_step(a->steps, s, gain, NAN, depth);
  wa_unlock();
}

void audio_vehicle(Audio *a, const RideState *s) {
  if (!a->started) return;
  wa_lock();
  vehicle_audio_update(a->vehicles, s);
  wa_unlock();
}

void audio_set_auto_steps(Audio *a, bool on) { a->auto_steps = on; }
void audio_set_muted(Audio *a, bool m) { a->muted = m; apply_volume(a, 0.08); }
bool audio_muted(const Audio *a) { return a->muted; }
void audio_set_volume(Audio *a, double v) { a->vol = fmax(0, fmin(1, v)); apply_volume(a, 0.08); }
void audio_on_wave(Audio *a, void (*fn)(const AudioWave *, void *), void *user) { vec_push(&a->wave_cbs, ((WaveCb){ fn, user })); }
int audio_get_cars(Audio *a, AudioCar **out, int max) { return a->started ? audio_cars_list(a->cars, out, max) : 0; }
void audio_set_gull_source(Audio *a, GullSourceFn fn, void *user) {
  a->gull_source = fn;
  a->gull_user = user;
  if (a->started) gulls_set_source(a->gulls, fn, user);
}
void audio_wing_flutter(Audio *a, const GullSrc *p) {
  if (!a->started) return;
  wa_lock();
  gulls_flutter_at(a->gulls, wa_now() + 0.02, p);
  wa_unlock();
}
// Offline render of one sound for verification (renderTest in the JS): peak / RMS of the master
// output. names: waves, waves-street, gulls, wind, car, music, music-far, steps-<surface>
// dev: HRTF panner level per ear for band-limited noise around the head (tools: compare with
// the same measurement on Chrome's PannerNode)
void audio_hrtf_table(void) {
  const double sr = 48000;
  const int N = 96000;
  static const char *const BANDS[] = { "none", "low", "mid", "high" };
  static const int AZ[] = { 0, 15, 30, 45, 60, 75, 90, 105, 120, 135, 150, 165, 180 };
  for (int b = 0; b < 4; b++)
    for (int pass = 0; pass < (b ? 2 : 1); pass++)
      for (size_t ai = 0; ai < (b && pass ? ARRAY_LEN(AZ) : 1); ai++) {
        wa_init_offline(sr);
        WaBuffer *buf = wa_buffer(1, N, sr);
        uint32_t seed = 1;
        for (int i = 0; i < N; i++) { seed = seed * 1103515245u + 12345u; buf->data[0][i] = (float)((seed / 4294967296.0) * 2 - 1); }
        WaNode *src = wa_buffer_source(buf, false, 1);
        WaNode *f = b == 1 ? bq(WA_LOWPASS, 300, 1) : b == 2 ? bq(WA_BANDPASS, 1500, 1) : b == 3 ? bq(WA_HIGHPASS, 6000, 1) : wa_gain(1);
        if (b == 1 || b == 3) wa_set_value(wa_param(f, WP_Q), 1);   // (the JS default Q is 1)
        WaNode *last = wa_connect(src, f);
        double a = AZ[ai] * 3.141592653589793 / 180;
        if (pass) last = wa_connect(last, wa_panner((WaPannerOpts){ .hrtf = true, .ref = 5, .rolloff = 1, .max_distance = 10000,
                                                                    .x = 5 * sin(a), .y = 0, .z = -5 * cos(a) }));
        wa_connect(last, wa_destination());
        wa_start(src, 0, 0);
        float *out = xmalloc((size_t)N * 2 * sizeof(float));
        wa_render_offline(out, N);
        double s[2] = {};
        for (int i = 4800; i < N; i++) for (int c = 0; c < 2; c++) s[c] += (double)out[2 * i + c] * out[2 * i + c];
        printf("%s%s %d L=%.2f R=%.2f\n", BANDS[b], pass ? "" : "-nopan", pass ? AZ[ai] : 0, 10 * log10(s[0] / (N - 4800)),
               10 * log10(s[1] / (N - 4800)));
        free(out);
        wa_shutdown();
      }
}

// dev: unit levels (oscillator types, the reverb convolver, the master chain) for comparison
// with the same graphs in the browser
static double rms_db(const float *x, int frames, int skip) {
  double s = 0;
  int n = 0;
  for (int i = skip; i < frames; i++) { s += (double)x[2 * i] * x[2 * i] + (double)x[2 * i + 1] * x[2 * i + 1]; n += 2; }
  return 10 * log10(s / n);
}
void audio_hrtf_sines(void) {
  static const double F[] = { 25, 40, 60, 90, 130, 200, 300, 450, 700, 1000, 1500, 2200, 3300, 5000, 7500, 11000, 16000 };
  static const int AZ[] = { 0, 90, 180 };
  const int N = 48000;
  float *out = xmalloc((size_t)N * 2 * sizeof(float));
  for (int a = 0; a < 3; a++)
    for (size_t k = 0; k < ARRAY_LEN(F); k++) {
      wa_init_offline(48000);
      WaNode *o = wa_osc(WA_SINE, F[k]);
      double r = AZ[a] * 3.141592653589793 / 180;
      wa_chain(o, wa_panner((WaPannerOpts){ .hrtf = true, .ref = 5, .rolloff = 1, .max_distance = 10000, .x = 5 * sin(r), .z = -5 * cos(r) }),
               wa_destination());
      wa_start(o, 0, 0);
      wa_render_offline(out, N);
      double s2[2] = {};
      for (int i = 9600; i < N; i++) for (int c = 0; c < 2; c++) s2[c] += (double)out[2 * i + c] * out[2 * i + c];
      printf("%d %g %.2f %.2f\n", AZ[a], F[k], 10 * log10(s2[0] / (N - 9600)) + 3.01, 10 * log10(s2[1] / (N - 9600)) + 3.01);
      wa_shutdown();
    }
  free(out);
}

void audio_units_test(void) {
  const double sr = 48000;
  const int N = 48000 * 3;
  float *out = xmalloc((size_t)N * 2 * sizeof(float));
  static const char *const TYPES[] = { "sine", "square", "sawtooth", "triangle" };
  for (int t = 0; t < 4; t++)
    for (int fi = 0; fi < 2; fi++) {
      wa_init_offline(sr);
      WaNode *o = wa_osc((WaOscType)t, fi ? 440 : 55);
      wa_connect(o, wa_destination());
      wa_start(o, 0, 0);
      wa_render_offline(out, N);
      printf("osc %s %d %.2f\n", TYPES[t], fi ? 440 : 55, rms_db(out, N, 4800));
      wa_shutdown();
    }
  WaBuffer *noise = nullptr;
  for (int k = 0; k < 3; k++) {
    wa_init_offline(sr);
    noise = wa_buffer(1, N, sr);
    uint32_t seed = 1;
    for (int i = 0; i < N; i++) { seed = seed * 1103515245u + 12345u; noise->data[0][i] = (float)((seed / 4294967296.0) * 2 - 1); }
    WaNode *src = wa_buffer_source(noise, false, 1);
    wa_chain(src, wa_convolver(make_impulse(2.4, 2.0)), wa_destination());
    wa_start(src, 0, 0);
    wa_render_offline(out, N);
    printf("conv %.2f\n", rms_db(out, N, 48000));
    wa_shutdown();
  }
  for (int k = 0; k < 2; k++) {   // each master compressor alone
    wa_init_offline(sr);
    noise = wa_buffer(1, N, sr);
    uint32_t seed = 1;
    for (int i = 0; i < N; i++) { seed = seed * 1103515245u + 12345u; noise->data[0][i] = (float)(((seed / 4294967296.0) * 2 - 1) * 0.05); }
    WaNode *src = wa_buffer_source(noise, false, 1);
    WaNode *cmp = k ? wa_compressor(-3, 0, 20, 0.001, 0.1) : wa_compressor(-20, 12, 2.5, 0.02, 0.35);
    wa_chain(src, cmp, wa_destination());
    wa_start(src, 0, 0);
    wa_render_offline(out, N);
    printf("comp%d %.2f\n", k, rms_db(out, N, 48000));
    wa_shutdown();
  }
  static const double LVL[] = { 0.05, 0.3, 1.0 };
  for (int k = 0; k < 3; k++) {
    wa_init_offline(sr);
    AudioEnv *env = create_engine();
    wa_set_value(wa_param(env->volume, WP_GAIN), 1);
    noise = wa_buffer(1, N, sr);
    uint32_t seed = 1;
    for (int i = 0; i < N; i++) { seed = seed * 1103515245u + 12345u; noise->data[0][i] = (float)(((seed / 4294967296.0) * 2 - 1) * LVL[k]); }
    WaNode *src = wa_buffer_source(noise, false, 1);
    wa_connect(src, env->dry);
    wa_start(src, 0, 0);
    wa_render_offline(out, N);
    printf("master %g %.2f\n", LVL[k], rms_db(out, N, 48000));
    wa_shutdown();
  }
  free(out);
}

AudioTestResult audio_render_test(const char *name, double seconds) {
  const double sr = 48000;
  wa_init_offline(sr);
  int frames = (int)ceil(sr * seconds);
  AudioEnv *env = create_engine();
  wa_set_value(wa_param(env->volume, WP_GAIN), 1);
  Listener *L = &env->listener;
#define SET_L(X, Y, Z) do { *L = (Listener){ X, Y, Z, 0, 0, -1, 0, 1, 0 }; apply_listener(L, true); } while (0)
  if (!strcmp(name, "waves") || !strcmp(name, "waves-street")) {
    SET_L(!strcmp(name, "waves") ? 82 : -26, 1.85, 0);
    Waves *w = create_waves(env, SAND.waterline);
    waves_track(w, L);
    waves_break_at(w, 0.2, L, 1);
  } else if (!strcmp(name, "gulls")) {
    SET_L(40, 1.9, 0);
    Gulls *g = create_gulls(env);
    gulls_call_at(g, 0.2, L, "kyow");
    gulls_call_at(g, fmin(3.5, seconds - 2), L, "laugh");
  } else if (!strcmp(name, "wind")) {
    SET_L(-12, 1.85, -8);
    int np;
    const double (*palms)[2] = palm_clusters(&np);
    create_wind(env, palms, np);
  } else if (!strcmp(name, "car")) {
    SET_L(-12, 1.85, 0);
    cars_spawn(create_cars(env), (CarSpawn){ 1, 25 / 3.6, -(25 / 3.6) * seconds * 0.5, 0 });
  } else if (!strcmp(name, "music") || !strcmp(name, "music-far")) {
    if (!strcmp(name, "music")) SET_L(-26, 1.85, -6);
    else SET_L(-12, 1.85, 30);
    music_tick(create_music(env, -29, 1.6, -10), 0, seconds - 1);
  } else if (!strncmp(name, "steps-", 6)) {
    SET_L(0, 1.7, 0);
    Footsteps *fs = create_footsteps(env);
    const char *s = name + 6;
    Surface surf = !strcmp(s, "sand") ? SURF_SAND : !strcmp(s, "wetsand") ? SURF_WETSAND : !strcmp(s, "wood") ? SURF_WOOD
                 : !strcmp(s, "grass") ? SURF_GRASS : !strcmp(s, "splash") ? SURF_SPLASH : SURF_PAVEMENT;
    for (double t = 0.1; t < seconds - 0.5; t += 0.55) footsteps_step(fs, surf, 1, t, NAN);
  } else FATAL("unknown audio test %s", name);
#undef SET_L
  for (size_t i = 0; i < env->spatials.len; i++) spatial_update(env->spatials.data[i], L, true);
  float *buf = xmalloc((size_t)frames * 2 * sizeof(float));
  int done = 0;
  while (done < frames) {   // (onended callbacks run between chunks, like the JS event loop)
    int n = frames - done < 4800 ? frames - done : 4800;
    wa_render_offline(buf + 2 * done, n);
    wa_poll();
    done += n;
  }
  double peak = 0, sum = 0;
  for (int i = 0; i < frames * 2; i++) { double v = fabs(buf[i]); if (v > peak) peak = v; sum += (double)buf[i] * buf[i]; }
  free(buf);
  double rms = sqrt(sum / (frames * 2.0));
  wa_shutdown();
  return (AudioTestResult){ peak, rms, 20 * log10(peak + 1e-12), 20 * log10(rms + 1e-12), peak >= 0.999 };
}

bool audio_running(const Audio *a) { return a->started; }
double audio_time(const Audio *a) { return a->started ? wa_now() : 0; }
