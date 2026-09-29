// Palms of Ocean Drive / Lummus Park: port of src/world/palms.js.
#pragma once

#include "gfx/scene.h"

typedef enum PalmRow { ROW_HOTEL, ROW_EDGE, ROW_PARK } PalmRow;
typedef enum PalmSpecies { SP_COCONUT, SP_ROYAL, SP_SABAL } PalmSpecies;

typedef struct PalmTree {
  double x, z;
  PalmRow row;
  int ground;               // 0: tree grate, 1: sand / mulch circle
  PalmSpecies species;
  int variant;
  double rotY, k, hs, scale, seed;
} PalmTree;

// Placement (pure data, also used by the audio engine): PALM_TREES and PALM_CLUSTERS
const PalmTree *palm_trees(int *n);
// wind-rustle sound sources: [x, z] pairs
const double (*palm_clusters(int *n))[2];

typedef struct Palms Palms;
Palms *build_palms(Node *scene);
void palms_update(Palms *p, double time);   // wind (frozen in shot mode through the time given)
