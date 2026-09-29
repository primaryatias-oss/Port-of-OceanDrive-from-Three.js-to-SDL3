// Touch controls: port of src/player/touch.js. A floating joystick under the left thumb (left
// ~40% of the screen), drag anywhere else to look, a jump ring and a small mute button
// bottom-right (and a Ride button by a vehicle). Move and look work at the same time (one tracked
// finger each). The joystick only writes walker.stick, so movement keeps the walker's
// acceleration, footsteps, collisions and jump. This module holds the input logic and the
// controls' visual state; ui.c draws them.
#pragma once

#include "audio/audio.h"
#include "player/walker.h"

#define TOUCH_R 52.0      // px, joystick throw

typedef struct Touch {
  Walker *walker;
  Audio *audio;
  void (*on_ride)(void *user);
  void *ride_user;
  bool enabled;
  // tracked fingers
  struct { bool on; SDL_FingerID id; double x0, y0; } move;
  struct { bool on; SDL_FingerID id; double x, y; } look;
  // visual state, CSS px
  bool stick_show, hint_gone;
  double stick_x, stick_y, knob_x, knob_y;
  bool jump_down, ride_down, mute_down;
  SDL_FingerID jump_id, ride_id, mute_id;
  int ride_mode;            // 0 hidden, 1 "Ride", 2 "Off"
  const char *jump_label;   // "Jump", "Hop" (bike), "Boost" (ATV)
  bool muted;
  int version;              // bumped whenever the drawing must change
} Touch;

// button geometry (CSS px, centre and radius) for a viewport w x h
typedef struct TouchButton { double x, y, r; } TouchButton;
TouchButton touch_jump_button(double w, double h);
TouchButton touch_ride_button(double w, double h);
TouchButton touch_mute_button(double w, double h);

void touch_init(Touch *t, Walker *walker, Audio *audio, void (*on_ride)(void *), void *ride_user);
void touch_set_enabled(Touch *t, bool on);
void touch_sync_mute(Touch *t);
// mode: 0 none, 1 'ride' (a vehicle in reach), 2 'off' (riding); atv: the ridden vehicle is the ATV
void touch_set_ride(Touch *t, int mode, bool atv);
// SDL finger events, window size in CSS px; `blocked`: the overlay / loader is up (no joystick
// or look). Returns true when the event was used.
bool touch_event(Touch *t, const SDL_Event *e, double w, double h, bool blocked);
void touch_release_all(Touch *t);
// a mouse click on a visible button (the buttons' click listener); true when one was hit
bool touch_click(Touch *t, double x, double y, double w, double h);
