// Quality tier, decided once at startup before anything is built: port of src/quality.js.
//   high   - desktop / discrete GPU: the reference look
//   medium - integrated GPUs, laptops, tablets
//   low    - phones and software renderers
// Override with --quality low|medium|high (and --ultra). --shot always uses 'high'.
#pragma once

#include <stdbool.h>

typedef enum Tier { TIER_HIGH, TIER_MEDIUM, TIER_LOW } Tier;

typedef struct Quality {
  Tier tier, detected;
  bool ultra;                   // ?ultra: 8192 sun map, full contact-hardening filter
  double maxDpr, maxPixels, renderScale;
  int msaa;
  bool fxaa, bloom;
  int shadowMap[2], shadowTaps;
  bool shadowFilterFull;
  int cloudOctaves;
  int oceanRings, oceanSegs;
  int sandRows, sandDetail;
  double printFade[2];
  int wrack;
  double farFoliage, signAtlas, hotelFar, shadowStep;
  bool audioReduced;
  // detection inputs (quality.js `why`)
  char gpu_name[128];
  int max_tex, cores;
  double mem;
} Quality;

extern Quality QUALITY;

// forced: "low" / "medium" / "high" or null; ultra: --ultra; shot: --shot (forces high)
void quality_init(const char *forced, bool ultra, bool shot);
// index into the shader tiers (high, medium, low, ultra) for this quality
int quality_program_tier(void);

// minimal regex (alternation, . * + \d, escapes, optional case folding) for the GPU checks
bool rx_search(const char *pattern, const char *text, bool icase);
