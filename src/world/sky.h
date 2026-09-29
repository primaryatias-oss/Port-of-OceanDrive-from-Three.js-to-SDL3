// Sunrise sky, sun light, image-based skylight and aerial perspective: port of src/sky.js.
// The GLSL (sky model, fog chunk, PCSS shadow chunk, ground bounce) lives in shaders/ and is
// generated from sky.js itself (tools/ref/extract-sky-glsl.mjs).
#pragma once

#include "gfx/scene.h"

extern V3 g_sun_dir;                        // compassToDir(SUN.azimuthDeg, SUN.elevationDeg)
static constexpr double SUN_INTENSITY = 5.1;
static constexpr double ENV_INTENSITY = 1.0;
static constexpr double FOG_DENSITY = 1.0 / 200;
static inline Color sun_color(void) { return color_rgb(1.0, 0.6, 0.4); }   // linear

typedef struct Sky {
  Node *dome;
  Texture *env;                             // PMREM cube-UV (scene.environment)
  // DirectionalLight + DirectionalLightShadow
  V3 sun_position, target_position;
  Camera shadow_cam;
  int map_w, map_h;
  double bias, normal_bias, radius;
  M4 shadow_matrix;
  Texture *shadow_map;                      // D24 (or D32F) with a LessEqual compare sampler
  // placeShadow state
  V3 base, axX, axY;
  double texX, texY, step, last_z;
  bool shadow_wanted;
} Sky;

// createSky(renderer, scene): adds the dome to the scene, renders the environment.
// shadow_map_size: QUALITY.shadowMap; shadow_step: QUALITY.shadowStep.
Sky *sky_create(Node *scene, const int shadow_map_size[2], double shadow_step);
// sky.update(camera): the dome follows the camera; the shadow box follows it along z
void sky_update(Sky *s, Camera *cam);
// the light / fog / shadow frame values for a view (call before rendering the main pass)
void sky_set_frame(Sky *s, const Camera *cam);
// one step lighter sun shadows (GPU guard): false at the floor
bool sky_lower_shadow(Sky *s);
