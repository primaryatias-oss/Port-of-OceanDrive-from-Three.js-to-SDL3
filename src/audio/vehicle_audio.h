// Ride sounds for the rider's own vehicle (not spatialised: the listener sits on it): port of
// src/audio/vehicles.js.
//   bike: freewheel hub ticking while coasting, a chain tick per pedal stroke, tyre hiss and
//         rolling rumble on pavement, crunch on sand, swish on grass, splash in water
//   ATV:  small 4-stroke single: a band-limited exhaust pulse train at the firing rate
//         (rpm / 120) through a soft clipper and an rpm / load dependent lowpass, a half-order
//         lope, firing-modulated intake noise, CVT whine; starter on mount, idle burble when
//         stopped, knobby-tyre crunch and hum, splash through the swash
//   both: wind rush with speed, a thump + rattle on bumps, landings and collisions
#pragma once

#include "audio/engine.h"

typedef enum RideKind { RIDE_NONE, RIDE_BIKE, RIDE_ATV } RideKind;
typedef enum RideSurface { RS_PAVEMENT, RS_GRASS, RS_SAND, RS_WETSAND, RS_WATER } RideSurface;
typedef struct RideState {
  RideKind kind;
  double lon, throttle, rpm, load;
  RideSurface surface;
  double soft, depth;
  bool coasting;
  double pedal, crank, bump, land;
} RideState;

typedef struct VehicleAudio VehicleAudio;
VehicleAudio *create_vehicle_audio(AudioEnv *env);
void vehicle_audio_update(VehicleAudio *v, const RideState *s);
