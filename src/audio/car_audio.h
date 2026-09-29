// A car cruising along Ocean Drive at ~25 km/h: port of src/audio/car.js. Engine =
// firing-frequency sawtooth (lowpassed) + harmonics + half-order rumble + firing-modulated intake
// noise; tyres = band-passed pink roar + hiss + low road rumble. The panner glides along the
// lane; Doppler is applied as a shared detune (cents) on every oscillator and noise source from
// the radial velocity.
#pragma once

#include "audio/engine.h"

typedef struct AudioCar {
  int id;
  double x, y, dir, speed, t0, duration, z0, z1;
  bool active;
  WaNode *dop;               // (held while the car is in the list)
} AudioCar;

typedef struct AudioCars AudioCars;

AudioCars *create_cars(AudioEnv *env);
// opts: dir (0: random, 60% southbound), speed (NAN: random ~25 km/h), z_start (NAN: -400 dir),
// at (NAN: now)
typedef struct CarSpawn { int dir; double speed, z_start, at; } CarSpawn;
AudioCar *cars_spawn(AudioCars *c, CarSpawn o);
void car_audio_update(AudioCars *c, const Listener *L);
double audio_car_z(const AudioCar *car);          // live z
double audio_car_progress(const AudioCar *car);   // 0..1
// the cars currently passing (valid until the next wa_poll)
int audio_cars_list(AudioCars *c, AudioCar **out, int max);
void audio_cars_on_pass(AudioCars *c, void (*fn)(AudioCar *car, void *user), void *user);
void audio_cars_on_end(AudioCars *c, void (*fn)(AudioCar *car, void *user), void *user);
