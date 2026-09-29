// Post chain: port of src/renderer/post.js on three's EffectComposer.
//   RenderPass (HDR half-float, MSAA) -> UnrealBloomPass (strength 0.16, radius 0.25,
//   threshold 4.0) -> CameraGrade -> OutputPass (ACES filmic, exposure 0.62, sRGB) ->
//   CameraFinish -> FXAA (low tier).
// The result lands in `screen` (RGBA8, GL row order), which the caller blits to the window.
#pragma once

#include "gfx/scene.h"

typedef struct Post Post;

typedef struct PostOptions {
  double bloom_strength;   // 0.16
  int samples;             // MSAA of the scene target (0 = none)
  bool bloom, fxaa;
  double exposure;         // renderer.toneMappingExposure (0.62)
} PostOptions;

Post *post_create(int w, int h, PostOptions o);
void post_resize(Post *p, int w, int h);   // drawing-buffer size
// renders scene + post into the screen texture; `time` drives grade/finish uTime
void post_render(Post *p, SDL_GPUCommandBuffer *cb, Node *scene, Camera *cam, double time, V3 sun_dir);
Texture *post_screen(Post *p);
