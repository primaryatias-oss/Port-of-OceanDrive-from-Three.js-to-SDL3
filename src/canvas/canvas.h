// A software 2D canvas: the subset of the HTML Canvas 2D API the scene's procedural textures
// use. Pixels are 8-bit premultiplied RGBA like Chromium's; shapes are rasterized with exact
// area coverage (non-zero winding), composited source-over in (non-linear) sRGB values.
#pragma once

#include "core/common.h"
#include "math/vmath.h"

typedef struct Canvas Canvas;

typedef enum { STYLE_COLOR, STYLE_LINEAR, STYLE_RADIAL } StyleKind;
typedef struct GradStop { double t; double rgba[4]; } GradStop;   // rgba 0..1 (unpremultiplied)
typedef struct Gradient {
  StyleKind kind;
  double x0, y0, r0, x1, y1, r1;     // in the user space current when it was created... (see .c)
  GradStop stops[16];
  int nstops;
} Gradient;

typedef struct Style { StyleKind kind; double rgba[4]; const Gradient *grad; } Style;

typedef enum { ALIGN_START, ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT, ALIGN_END } TextAlign;
typedef enum { BASE_ALPHABETIC, BASE_MIDDLE, BASE_TOP, BASE_BOTTOM } TextBaseline;
typedef enum { CAP_BUTT, CAP_ROUND, CAP_SQUARE } LineCap;
typedef enum { JOIN_MITER, JOIN_ROUND, JOIN_BEVEL } LineJoin;

Canvas *canvas_new(int w, int h);
void canvas_free(Canvas *c);
int canvas_width(const Canvas *c);
int canvas_height(const Canvas *c);
const uint8_t *canvas_pixels(const Canvas *c);   // premultiplied RGBA8, row 0 = top
uint8_t *canvas_pixels_rw(Canvas *c);             // (procedural fills)

// state (ctx.* properties); colours take CSS strings: #rgb, #rrggbb, rgb(), rgba(), names
void cv_fill_color(Canvas *c, const char *css);
void cv_stroke_color(Canvas *c, const char *css);
// `rgba(r, g, b, a)` built from numbers (CSS: channels rounded and clamped to 0..255)
void cv_fill_rgba(Canvas *c, double r, double g, double b, double a);
void cv_stroke_rgba(Canvas *c, double r, double g, double b, double a);
void cv_fill_gradient(Canvas *c, const Gradient *g);
void cv_stroke_gradient(Canvas *c, const Gradient *g);
void cv_line_width(Canvas *c, double w);
void cv_line_cap(Canvas *c, LineCap cap);
void cv_line_join(Canvas *c, LineJoin join);
void cv_global_alpha(Canvas *c, double a);
void cv_filter_blur(Canvas *c, double px);   // ctx.filter = 'blur(Npx)' (0 = 'none')
void cv_font(Canvas *c, const char *css);    // e.g. "bold 84px Arial, Helvetica, sans-serif"
void cv_letter_spacing(Canvas *c, double px);   // ctx.letterSpacing (cased fonts)
void cv_text_align(Canvas *c, TextAlign a);
void cv_text_baseline(Canvas *c, TextBaseline b);

void cv_save(Canvas *c);
void cv_restore(Canvas *c);
void cv_translate(Canvas *c, double x, double y);
void cv_scale(Canvas *c, double x, double y);
void cv_rotate(Canvas *c, double a);

// paths
void cv_begin_path(Canvas *c);
void cv_move_to(Canvas *c, double x, double y);
void cv_line_to(Canvas *c, double x, double y);
void cv_close_path(Canvas *c);
void cv_arc(Canvas *c, double x, double y, double r, double a0, double a1, bool ccw);
void cv_ellipse(Canvas *c, double x, double y, double rx, double ry, double rot, double a0, double a1, bool ccw);
void cv_bezier_to(Canvas *c, double c1x, double c1y, double c2x, double c2y, double x, double y);
void cv_quadratic_to(Canvas *c, double cx, double cy, double x, double y);
void cv_rect(Canvas *c, double x, double y, double w, double h);
void cv_fill(Canvas *c);
void cv_stroke(Canvas *c);
void cv_clip(Canvas *c);

void cv_fill_rect(Canvas *c, double x, double y, double w, double h);
void cv_stroke_rect(Canvas *c, double x, double y, double w, double h);
void cv_clear_rect(Canvas *c, double x, double y, double w, double h);

// text (the port's own vector fonts; see font.h)
void cv_fill_text(Canvas *c, const char *text, double x, double y);
void cv_stroke_text(Canvas *c, const char *text, double x, double y);
double cv_measure_text(Canvas *c, const char *text);

// gradients (created in the current user space, like createLinearGradient)
Gradient cv_linear_gradient(Canvas *c, double x0, double y0, double x1, double y1);
Gradient cv_radial_gradient(Canvas *c, double x0, double y0, double r0, double x1, double y1, double r1);
void grad_add_stop(Gradient *g, double t, const char *css);

// image data: RGBA8 unpremultiplied, row 0 = top (getImageData / putImageData)
void cv_get_image_data(const Canvas *c, int x, int y, int w, int h, uint8_t *out);
void cv_put_image_data(Canvas *c, const uint8_t *rgba, int x, int y, int w, int h);
// drawImage(src, sx, sy, sw, sh, dx, dy, dw, dh) (area-average resampling, current transform
// must be the identity)
void cv_draw_image(Canvas *c, const Canvas *src, double sx, double sy, double sw, double sh,
                   double dx, double dy, double dw, double dh);

// parse a CSS colour to rgba 0..1 (FATAL on unsupported syntax)
void css_color(const char *css, double rgba[4]);
