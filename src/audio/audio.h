// Ocean Drive spatial audio: port of src/audio/index.js. Everything is synthesized at runtime
// (audio/wa.h: the Web Audio subset, rendered on the SDL audio thread).
//
//   Audio *a = create_audio(voices_reduced);
//   audio_start(a);                  // from the click-to-start gesture (no-op in shot mode)
//   audio_update(a, dt, camera);     // listener pose, spatial updates, the scheduler tick
//   audio_footstep(a, SURF_SAND, 1, NAN);
#pragma once

#include "audio/car_audio.h"
#include "audio/footsteps.h"
#include "audio/gulls.h"
#include "audio/vehicle_audio.h"
#include "gfx/scene.h"

typedef struct Audio Audio;

Audio *create_audio(bool reduced_voices);
bool audio_start(Audio *a);            // false when there is no audio (shot mode, no device)
void audio_update(Audio *a, double dt, const Camera *camera);
void audio_footstep(Audio *a, Surface s, double gain, double depth);
void audio_vehicle(Audio *a, const RideState *s);
void audio_set_auto_steps(Audio *a, bool on);
void audio_set_muted(Audio *a, bool m);
bool audio_muted(const Audio *a);
void audio_set_volume(Audio *a, double v);
// wave hook for visuals: per scheduled break; t - now = seconds until the wave starts
typedef struct AudioWave { double t, now, k, size, runup, z; } AudioWave;
void audio_on_wave(Audio *a, void (*fn)(const AudioWave *w, void *user), void *user);
// the cars currently passing, as the visual car module wants them
int audio_get_cars(Audio *a, AudioCar **out, int max);
void audio_set_gull_source(Audio *a, GullSourceFn fn, void *user);
void audio_wing_flutter(Audio *a, const GullSrc *p);
bool audio_running(const Audio *a);
// offline render of one sound (renderTest in the JS): waves, waves-street, gulls, wind, car,
// music, music-far, steps-<surface>
typedef struct AudioTestResult { double peak, rms, peak_db, rms_db; bool clipped; } AudioTestResult;
AudioTestResult audio_render_test(const char *name, double seconds);
void audio_hrtf_table(void);   // dev: HRTF level per ear and band around the head
void audio_hrtf_sines(void);   // dev: HRTF response to sines at 0 / 90 / 180 degrees
void audio_units_test(void);   // dev: oscillator / convolver / master chain levels
double audio_time(const Audio *a);
