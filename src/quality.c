#include "quality.h"

#include <ctype.h>
#include <math.h>
#include <string.h>

#include "core/common.h"
#include "gfx/gpu.h"

Quality QUALITY;

// ---- tiny regex: literal chars, '.', '\d', '\x' escapes, postfix '*' and '+', and '|' ------
// (enough for quality.js's GPU-name patterns)

typedef struct Atom { int kind; char c; } Atom;   // kind: 0 literal, 1 any, 2 digit

static const char *atom_parse(const char *p, Atom *a) {
  if (*p == '\\') {
    p++;
    if (*p == 'd') *a = (Atom){ 2, 0 };
    else *a = (Atom){ 0, *p };
    return p + 1;
  }
  if (*p == '.') { *a = (Atom){ 1, 0 }; return p + 1; }
  *a = (Atom){ 0, *p };
  return p + 1;
}

static bool atom_match(Atom a, char c, bool icase) {
  if (!c) return false;
  if (a.kind == 1) return true;
  if (a.kind == 2) return isdigit((unsigned char)c);
  return icase ? tolower((unsigned char)a.c) == tolower((unsigned char)c) : a.c == c;
}

// matches pattern [p, end) at text t (anchored at t, not at the end)
static bool match_here(const char *p, const char *end, const char *t, bool icase) {
  if (p >= end) return true;
  Atom a;
  const char *next = atom_parse(p, &a);
  if (next < end && (*next == '*' || *next == '+')) {
    bool plus = *next == '+';
    const char *rest = next + 1;
    const char *s = t;
    if (plus) {
      if (!atom_match(a, *s, icase)) return false;
      s++;
    }
    for (;;) {   // backtracking, shortest first
      if (match_here(rest, end, s, icase)) return true;
      if (!atom_match(a, *s, icase)) return false;
      s++;
    }
  }
  if (!atom_match(a, *t, icase)) return false;
  return match_here(next, end, t + 1, icase);
}

bool rx_search(const char *pattern, const char *text, bool icase) {
  // top-level alternation
  const char *alt = pattern;
  for (;;) {
    const char *bar = alt;
    while (*bar && *bar != '|') { if (*bar == '\\' && bar[1]) bar++; bar++; }
    for (const char *t = text;; t++) {
      if (match_here(alt, bar, t, icase)) return true;
      if (!*t) break;
    }
    if (!*bar) return false;
    alt = bar + 1;
  }
}

// ---- detection ------------------------------------------------------------------------------

// largest square-ish texture the device accepts (WebGL MAX_TEXTURE_SIZE)
static int probe_max_texture(void) {
  static const int sizes[] = { 16384, 8192, 4096 };
  for (size_t i = 0; i < ARRAY_LEN(sizes); i++) {
    SDL_GPUTexture *t = SDL_CreateGPUTexture(g_gpu.dev, &(SDL_GPUTextureCreateInfo){
      .type = SDL_GPU_TEXTURETYPE_2D, .format = SDL_GPU_TEXTUREFORMAT_R8_UNORM, .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER,
      .width = (uint32_t)sizes[i], .height = 4, .layer_count_or_depth = 1, .num_levels = 1 });
    if (t) { SDL_ReleaseGPUTexture(g_gpu.dev, t); return sizes[i]; }
  }
  return 4096;
}

static void apply_tier(Tier t) {
  Quality *q = &QUALITY;
  q->tier = t;
  // maxPixels caps the rendered pixel count; shadowFilter 'lite' = 20 blocker + 25 PCF
  // samples per pixel; 'full' = 54 + 64 (ultra only)
  switch (t) {
  case TIER_HIGH:
    q->maxDpr = 1.5; q->maxPixels = 2.1e6; q->renderScale = 1; q->msaa = 4; q->fxaa = false; q->bloom = true;
    q->shadowMap[0] = 4096; q->shadowMap[1] = 1024; q->shadowTaps = 5; q->shadowFilterFull = false; q->cloudOctaves = 5;
    q->oceanRings = 400; q->oceanSegs = 320; q->sandRows = 440; q->sandDetail = 1024; q->printFade[0] = 35; q->printFade[1] = 60;
    q->wrack = 9000; q->farFoliage = 1; q->signAtlas = 1; q->hotelFar = INFINITY; q->shadowStep = 0.5; q->audioReduced = false;
    break;
  case TIER_MEDIUM:
    q->maxDpr = 1.25; q->maxPixels = 1.3e6; q->renderScale = 1; q->msaa = 2; q->fxaa = false; q->bloom = true;
    q->shadowMap[0] = 4096; q->shadowMap[1] = 1024; q->shadowTaps = 5; q->shadowFilterFull = false; q->cloudOctaves = 4;
    q->oceanRings = 300; q->oceanSegs = 256; q->sandRows = 360; q->sandDetail = 1024; q->printFade[0] = 26; q->printFade[1] = 45;
    q->wrack = 6000; q->farFoliage = 0.75; q->signAtlas = 1; q->hotelFar = INFINITY; q->shadowStep = 1; q->audioReduced = false;
    break;
  case TIER_LOW:
    q->maxDpr = 1; q->maxPixels = 0.8e6; q->renderScale = 0.85; q->msaa = 0; q->fxaa = true; q->bloom = false;
    q->shadowMap[0] = 2048; q->shadowMap[1] = 1024; q->shadowTaps = 4; q->shadowFilterFull = false; q->cloudOctaves = 3;
    q->oceanRings = 220; q->oceanSegs = 176; q->sandRows = 260; q->sandDetail = 512; q->printFade[0] = 16; q->printFade[1] = 28;
    q->wrack = 3000; q->farFoliage = 0.5; q->signAtlas = 0.5; q->hotelFar = 560; q->shadowStep = 2.5; q->audioReduced = true;
    break;
  }
}

void quality_init(const char *forced, bool ultra, bool shot) {
  Quality *q = &QUALITY;
  const char *name = SDL_GetStringProperty(SDL_GetGPUDeviceProperties(g_gpu.dev), SDL_PROP_GPU_DEVICE_NAME_STRING, "");
  snprintf(q->gpu_name, sizeof q->gpu_name, "%s", name ? name : "");
  q->max_tex = probe_max_texture();
  q->cores = SDL_GetNumLogicalCPUCores();
  // navigator.deviceMemory: GB rounded down to a power of two, capped at 8
  double gb = SDL_GetSystemRAM() / 1024.0;
  q->mem = 0.25;
  while (q->mem * 2 <= gb && q->mem < 8) q->mem *= 2;
  const char *r = q->gpu_name;
  // this is a desktop build: never a phone or tablet (quality.js: UA / touch checks)
  bool phone = false, tablet = false;
  bool software = rx_search("SwiftShader|llvmpipe|softpipe|Microsoft Basic Render", r, true);
  bool discrete = rx_search("NVIDIA|GeForce|Quadro|RTX|GTX|Radeon RX|Radeon Pro|Radeon \\(TM\\) RX|FirePro|Arc\\(TM\\) A|Intel.*Arc", r, true);
  bool integrated = rx_search("Intel|UHD|Iris|Apple|Adreno|Mali|PowerVR|Radeon\\(TM\\) Graphics|Radeon Graphics|Vega \\d+ Graphics", r, true);
  Tier t;
  if (phone || software) t = TIER_LOW;
  else if (tablet) t = TIER_MEDIUM;
  else if (discrete) t = TIER_HIGH;
  else if (integrated) t = TIER_MEDIUM;
  else t = q->cores >= 8 && q->mem >= 8 && q->max_tex >= 16384 ? TIER_HIGH : TIER_MEDIUM;
  if (t == TIER_HIGH && q->max_tex < 8192) t = TIER_MEDIUM;
  q->detected = t;
  if (shot) t = TIER_HIGH;
  else if (forced) {
    if (!strcmp(forced, "low")) t = TIER_LOW;
    else if (!strcmp(forced, "medium")) t = TIER_MEDIUM;
    else if (!strcmp(forced, "high")) t = TIER_HIGH;
  }
  apply_tier(t);
  q->ultra = false;
  if (t == TIER_HIGH && ultra && q->max_tex >= 8192) {
    q->ultra = true;
    q->shadowMap[0] = 8192; q->shadowMap[1] = 2048; q->shadowTaps = 8; q->shadowFilterFull = true;
  }
  static const char *const names[] = { "high", "medium", "low" };
  LOG("quality: %s%s (detected %s; GPU '%s', max texture %d, %d cores, %.2g GB)", names[t], q->ultra ? " (ultra)" : "",
      names[q->detected], q->gpu_name, q->max_tex, q->cores, q->mem);
}

int quality_program_tier(void) {
  if (QUALITY.ultra) return 3;
  return QUALITY.tier == TIER_HIGH ? 0 : QUALITY.tier == TIER_MEDIUM ? 1 : 2;
}
