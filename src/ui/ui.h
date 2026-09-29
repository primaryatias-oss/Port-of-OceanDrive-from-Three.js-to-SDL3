// The page UI of index.html / main.js / touch.js / the vehicles' prompt, drawn with the software
// canvas and composited over the frame: the loading title card (#loader), the caption overlay
// (#overlay), #note, the ride prompt, the HUD (?hud) and the touch controls.
//
// Everything is a stack of layers: a canvas uploaded to a texture, drawn as a quad with an
// opacity (CSS transitions become opacity / position tweens, so a fade never redraws a canvas).
// Sizes are CSS px (the window size in points) times the window's pixel density.
#pragma once

#include "core/common.h"
#include "player/touch.h"

typedef struct Ui Ui;

typedef struct UiState {
  bool overlay;             // #overlay shown (not .hidden)
  bool touch;               // html.touch: the touch caption, no ride prompt
  const Touch *touch_ctl;   // the touch controls (null until created)
  const char *prompt_key;   // the ride prompt: key ("E" or "") and text ("" = hidden)
  const char *prompt_text;
  const char *hud;          // the HUD line (null = hidden)
} UiState;

Ui *ui_create(bool loader);   // loader: false in shot mode (html.shot #loader { display: none })
void ui_destroy(Ui *u);

// LOADER: a build step (window.__loadProgress(fraction, label)), then one painted loader frame
// (the build is synchronous: this is the loadStep / nextFrame yield)
void ui_load_step(Ui *u, double fraction, const char *label);
void ui_progress(Ui *u, double fraction, const char *label);   // report only (no frame)
void ui_paint(Ui *u);                                           // a loader frame only
void ui_scene_ready(Ui *u);          // window.__sceneReady
bool ui_loading(const Ui *u);        // body.loading: the overlay stays hidden and inert

void ui_note(Ui *u, const char *text);   // #note: shown for 3.5 s

// #gpu-reset: 1 "The graphics driver reset — reloading at a lighter quality…", 2 "…reset again." with
// a "Reload at low quality" button (hit-tested / hovered by ui_reset_button, CSS px)
void ui_gpu_reset(Ui *u, int mode);
bool ui_reset_button(Ui *u, double x, double y, bool click);

// advances the UI by dt and composites it over `target` (w x h px, the window's swapchain image)
void ui_render(Ui *u, SDL_GPUCommandBuffer *cb, SDL_GPUTexture *target, int w, int h, double dt, const UiState *s);

// dev (OD_UI_SHOT=path@N): the offscreen page to compose the next frame into when it is the one to
// save (null otherwise); then copy it to the window and save it after the submit
SDL_GPUTexture *ui_capture_target(Ui *u, int w, int h);
void ui_capture_present(SDL_GPUCommandBuffer *cb, SDL_GPUTexture *cap, SDL_GPUTexture *swap, int w, int h);
void ui_capture_save(Ui *u);
