// Wind: port of src/audio/wind.js. A soft non-spatial stereo breeze plus positioned frond-rustle
// sources at palm clusters. All layers share one slow gust control signal; each palm reads it
// with a time offset proportional to its x, so gusts roll in from the ocean and sweep west
// across the palms.
#pragma once

#include "audio/engine.h"

// palms: [x, z] pairs
void create_wind(AudioEnv *env, const double (*palms)[2], int n);
