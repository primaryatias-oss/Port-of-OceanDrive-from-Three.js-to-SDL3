// Touch controls (see touch.h): port of src/player/touch.js.
#include "player/touch.h"

#include "core/common.h"

static const double LEFT = 0.4;   // fraction of the width that spawns the joystick

// #touch .jump / .ride / .mute (and the max-height: 420px variants)
TouchButton touch_jump_button(double w, double h) {
  double bottom = h <= 420 ? 22 : 30;
  return (TouchButton){ w - 26 - 32, h - bottom - 32, 32 };
}
TouchButton touch_ride_button(double w, double h) {
  double bottom = h <= 420 ? 104 : 196;
  return (TouchButton){ w - 30 - 28, h - bottom - 28, 28 };
}
TouchButton touch_mute_button(double w, double h) {
  double bottom = h <= 420 ? 22 : 128, right = h <= 420 ? 108 : 40;
  return (TouchButton){ w - right - 18, h - bottom - 18, 18 };
}
static bool hit(TouchButton b, double x, double y) { return js_hypot2(x - b.x, y - b.y) <= b.r; }

void touch_init(Touch *t, Walker *walker, Audio *audio, void (*on_ride)(void *), void *ride_user) {
  *t = (Touch){ .walker = walker, .audio = audio, .on_ride = on_ride, .ride_user = ride_user, .jump_label = "Jump" };
  walker->has_stick = true;   // walker.stick = stick
  walker->stick = v2(0, 0);
  touch_sync_mute(t);
}

static void release_move(Touch *t) {
  t->move.on = false;
  t->walker->stick = v2(0, 0);
  t->stick_show = false;
  t->version++;
}
void touch_release_all(Touch *t) {
  release_move(t);
  t->look.on = false;
}

void touch_set_enabled(Touch *t, bool on) {
  t->enabled = on;
  if (!on) touch_release_all(t);
  t->version++;
}

void touch_sync_mute(Touch *t) {
  bool m = audio_muted(t->audio);
  if (m != t->muted) { t->muted = m; t->version++; }
}

void touch_set_ride(Touch *t, int mode, bool atv) {
  const char *label = mode == 2 ? (atv ? "Boost" : "Hop") : "Jump";
  if (mode == t->ride_mode && label == t->jump_label) return;
  t->ride_mode = mode;
  t->jump_label = label;
  t->version++;
}

static void press_jump(Touch *t) { walker_jump(t->walker); }
static void press_ride(Touch *t) { if (t->on_ride) t->on_ride(t->ride_user); }
static void press_mute(Touch *t) {
  audio_set_muted(t->audio, !audio_muted(t->audio));
  touch_sync_mute(t);
}

bool touch_click(Touch *t, double x, double y, double w, double h) {
  if (!t->enabled) return false;
  if (hit(touch_jump_button(w, h), x, y)) { press_jump(t); return true; }
  if (t->ride_mode && hit(touch_ride_button(w, h), x, y)) { press_ride(t); return true; }
  if (hit(touch_mute_button(w, h), x, y)) { press_mute(t); return true; }
  return false;
}

bool touch_event(Touch *t, const SDL_Event *e, double w, double h, bool blocked) {
  if (e->type != SDL_EVENT_FINGER_DOWN && e->type != SDL_EVENT_FINGER_MOTION &&
      e->type != SDL_EVENT_FINGER_UP && e->type != SDL_EVENT_FINGER_CANCELED)
    return false;
  SDL_FingerID id = e->tfinger.fingerID;
  double x = e->tfinger.x * w, y = e->tfinger.y * h;
  if (e->type == SDL_EVENT_FINGER_DOWN) {
    if (!t->enabled) return false;
    // the buttons: pressed on touchstart, released on touchend
    if (hit(touch_jump_button(w, h), x, y)) { t->jump_down = true; t->jump_id = id; t->version++; press_jump(t); return true; }
    if (t->ride_mode && hit(touch_ride_button(w, h), x, y)) { t->ride_down = true; t->ride_id = id; t->version++; press_ride(t); return true; }
    if (hit(touch_mute_button(w, h), x, y)) { t->mute_down = true; t->mute_id = id; t->version++; press_mute(t); return true; }
    if (blocked) return false;   // (#overlay, #loader)
    if (!t->move.on && x < w * LEFT) {
      t->move.on = true; t->move.id = id; t->move.x0 = x; t->move.y0 = y;
      t->stick_x = t->knob_x = x; t->stick_y = t->knob_y = y;
      t->stick_show = true;
      t->hint_gone = true;
      t->version++;
    } else if (!t->look.on) {
      t->look.on = true; t->look.id = id; t->look.x = x; t->look.y = y;
    }
    return true;
  }
  if (e->type == SDL_EVENT_FINGER_MOTION) {
    if (!t->enabled) return false;
    // a full drag across the long side of the screen turns ~180 degrees
    double k = 3.2 / fmax(fmax(w, h), 1);
    if (t->move.on && id == t->move.id) {
      double dx = x - t->move.x0, dy = y - t->move.y0;
      double d = js_hypot2(dx, dy);
      // past the rim the base follows the thumb, so reversing direction is immediate
      if (d > TOUCH_R * 1.35) {
        double s = (d - TOUCH_R * 1.35) / d;
        t->move.x0 += dx * s; t->move.y0 += dy * s;
        t->stick_x = t->move.x0; t->stick_y = t->move.y0;
        dx = x - t->move.x0; dy = y - t->move.y0;
      }
      double m = fmin(1, js_hypot2(dx, dy) / TOUCH_R), a = atan2(dy, dx);
      t->walker->stick = v2(cos(a) * m, -sin(a) * m);
      t->knob_x = t->move.x0 + cos(a) * m * TOUCH_R;
      t->knob_y = t->move.y0 + sin(a) * m * TOUCH_R;
      t->version++;
    } else if (t->look.on && id == t->look.id) {
      walker_look(t->walker, (x - t->look.x) * k, (y - t->look.y) * k * 0.85);
      t->look.x = x; t->look.y = y;
    }
    return t->move.on || t->look.on;
  }
  // up / cancel
  bool used = false;
  if (t->jump_down && id == t->jump_id) { t->jump_down = false; t->version++; used = true; }
  if (t->ride_down && id == t->ride_id) { t->ride_down = false; t->version++; used = true; }
  if (t->mute_down && id == t->mute_id) { t->mute_down = false; t->version++; used = true; }
  if (t->move.on && id == t->move.id) { release_move(t); used = true; }
  if (t->look.on && id == t->look.id) { t->look.on = false; used = true; }
  return used;
}
