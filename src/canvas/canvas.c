// Software canvas (see canvas.h).
//
// Rasterization: every shape becomes closed polygons in device space; their edges are
// accumulated as signed area per pixel cell (the font-rs method), and a running sum along each
// row gives the exact area coverage of the non-zero winding region (|sum| clamped to 1).
// Strokes are the union of segment quads, joins and caps accumulated into one mask, so a
// stroke is composited once.
#include "canvas/canvas.h"

#include <ctype.h>
#include <stdlib.h>

#include "canvas/font.h"
#include "core/vec.h"

typedef struct Affine { double a, b, c, d, e, f; } Affine;   // x' = a x + c y + e, y' = b x + d y + f

static Affine aff_identity(void) { return (Affine){ 1, 0, 0, 1, 0, 0 }; }
static V2 aff_apply(Affine m, double x, double y) { return v2(m.a * x + m.c * y + m.e, m.b * x + m.d * y + m.f); }
static Affine aff_mul(Affine m, Affine n) {   // m * n (n applied first)
  return (Affine){ m.a * n.a + m.c * n.b, m.b * n.a + m.d * n.b, m.a * n.c + m.c * n.d, m.b * n.c + m.d * n.d,
                   m.a * n.e + m.c * n.f + m.e, m.b * n.e + m.d * n.f + m.f };
}
static Affine aff_invert(Affine m) {
  double det = m.a * m.d - m.b * m.c;
  if (det == 0) return aff_identity();
  double id = 1 / det;
  return (Affine){ m.d * id, -m.b * id, -m.c * id, m.a * id, (m.c * m.f - m.d * m.e) * id, (m.b * m.e - m.a * m.f) * id };
}
static double aff_scale(Affine m) { return sqrt(fabs(m.a * m.d - m.b * m.c)); }

typedef struct State {
  Affine ctm;
  Style fill, stroke;
  Gradient fill_grad, stroke_grad;
  double line_width, miter_limit, alpha, blur;
  LineCap cap;
  LineJoin join;
  FontSpec font;
  TextAlign align;
  TextBaseline baseline;
  struct ClipMask *clip;   // null = no clip (refcounted: the state and each saved copy hold one)
} State;

// clip coverage inside a device-space rectangle; 0 outside it
typedef struct ClipMask { int x0, y0, w, h, refs; float a[]; } ClipMask;
static inline float clip_cov(const ClipMask *m, int x, int y) {
  x -= m->x0;
  y -= m->y0;
  return x < 0 || y < 0 || x >= m->w || y >= m->h ? 0.0f : m->a[(size_t)y * m->w + x];
}
static void clip_release(ClipMask *m) {
  if (m && --m->refs == 0) free(m);
}

typedef struct Subpath { Vec(V2) pts; bool closed; } Subpath;

struct Canvas {
  int w, h;
  uint8_t *px;   // premultiplied RGBA8
  State st;
  Vec(State) stack;
  Vec(Subpath) path;
  bool has_current;
  V2 current;    // device space
};

Canvas *canvas_new(int w, int h) {
  CHECK(w > 0 && h > 0);
  Canvas *c = xcalloc(1, sizeof *c);
  c->w = w;
  c->h = h;
  c->px = xcalloc((size_t)w * (size_t)h, 4);   // transparent black
  c->st.ctm = aff_identity();
  c->st.fill = (Style){ STYLE_COLOR, { 0, 0, 0, 1 }, nullptr };   // '#000000'
  c->st.stroke = (Style){ STYLE_COLOR, { 0, 0, 0, 1 }, nullptr };
  c->st.line_width = 1;
  c->st.miter_limit = 10;
  c->st.alpha = 1;
  c->st.cap = CAP_BUTT;
  c->st.join = JOIN_MITER;
  c->st.font = font_parse("10px sans-serif");
  c->st.align = ALIGN_START;
  c->st.baseline = BASE_ALPHABETIC;
  return c;
}

void canvas_free(Canvas *c) {
  if (!c) return;
  free(c->px);
  for (size_t i = 0; i < c->path.len; i++) vec_free(&c->path.data[i].pts);
  vec_free(&c->path);
  clip_release(c->st.clip);
  for (size_t i = 0; i < c->stack.len; i++) clip_release(c->stack.data[i].clip);
  vec_free(&c->stack);
  free(c);
}

int canvas_width(const Canvas *c) { return c->w; }
int canvas_height(const Canvas *c) { return c->h; }

// ---- colours -----------------------------------------------------------------------------------

static const struct { const char *name; uint32_t rgb; } CSS_NAMES[] = {
  { "black", 0x000000 }, { "white", 0xffffff }, { "red", 0xff0000 }, { "green", 0x008000 }, { "blue", 0x0000ff },
  { "yellow", 0xffff00 }, { "gray", 0x808080 }, { "grey", 0x808080 },
};

static int hexv(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  ch = (char)tolower((unsigned char)ch);
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  return -1;
}

void css_color(const char *css, double rgba[4]) {
  while (isspace((unsigned char)*css)) css++;
  rgba[3] = 1;
  size_t n = strlen(css);
  if (css[0] == '#') {
    if (n == 4 || n == 7) {
      int k = n == 4 ? 1 : 2;
      for (int i = 0; i < 3; i++) {
        int v = 0;
        for (int j = 0; j < k; j++) {
          int d = hexv(css[1 + i * k + j]);
          if (d < 0) FATAL("bad colour '%s'", css);
          v = v * 16 + d;
        }
        if (k == 1) v *= 17;
        rgba[i] = v / 255.0;
      }
      return;
    }
    FATAL("bad colour '%s'", css);
  }
  if (!strncmp(css, "rgb", 3)) {
    const char *p = strchr(css, '(');
    if (!p) FATAL("bad colour '%s'", css);
    double v[4] = { 0, 0, 0, 1 };
    int k = 0;
    p++;
    while (k < 4 && *p && *p != ')') {
      while (isspace((unsigned char)*p) || *p == ',') p++;
      char *end;
      v[k] = strtod(p, &end);
      if (end == p) FATAL("bad colour '%s'", css);
      p = end;
      if (*p == '%') { v[k] = k < 3 ? v[k] * 2.55 : v[k] / 100; p++; }
      k++;
      while (isspace((unsigned char)*p) || *p == ',') p++;
    }
    if (k < 3) FATAL("bad colour '%s'", css);
    // CSS clamps; rgb channels are rounded to integers by the canvas colour parser
    for (int i = 0; i < 3; i++) rgba[i] = clampd(js_round(v[i]), 0, 255) / 255.0;
    rgba[3] = k == 4 ? clampd(v[3], 0, 1) : 1;
    return;
  }
  if (!strcmp(css, "transparent")) { rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0; return; }
  for (size_t i = 0; i < ARRAY_LEN(CSS_NAMES); i++)
    if (!strcmp(css, CSS_NAMES[i].name)) {
      rgba[0] = ((CSS_NAMES[i].rgb >> 16) & 255) / 255.0;
      rgba[1] = ((CSS_NAMES[i].rgb >> 8) & 255) / 255.0;
      rgba[2] = (CSS_NAMES[i].rgb & 255) / 255.0;
      return;
    }
  FATAL("unsupported CSS colour '%s'", css);
}

void cv_fill_color(Canvas *c, const char *css) { c->st.fill.kind = STYLE_COLOR; css_color(css, c->st.fill.rgba); }
void cv_stroke_color(Canvas *c, const char *css) { c->st.stroke.kind = STYLE_COLOR; css_color(css, c->st.stroke.rgba); }
static void rgba_style(Style *st, double r, double g, double b, double a) {
  st->kind = STYLE_COLOR;
  double v[3] = { r, g, b };
  for (int i = 0; i < 3; i++) st->rgba[i] = (isnan(v[i]) ? 0 : clampd(js_round(v[i]), 0, 255)) / 255.0;
  st->rgba[3] = isnan(a) ? 0 : clampd(a, 0, 1);
}
void cv_fill_rgba(Canvas *c, double r, double g, double b, double a) { rgba_style(&c->st.fill, r, g, b, a); }
void cv_stroke_rgba(Canvas *c, double r, double g, double b, double a) { rgba_style(&c->st.stroke, r, g, b, a); }
void cv_fill_gradient(Canvas *c, const Gradient *g) { c->st.fill_grad = *g; c->st.fill.kind = g->kind; c->st.fill.grad = &c->st.fill_grad; }
void cv_stroke_gradient(Canvas *c, const Gradient *g) { c->st.stroke_grad = *g; c->st.stroke.kind = g->kind; c->st.stroke.grad = &c->st.stroke_grad; }
void cv_line_width(Canvas *c, double w) { if (w > 0 && isfinite(w)) c->st.line_width = w; }
void cv_line_cap(Canvas *c, LineCap cap) { c->st.cap = cap; }
void cv_line_join(Canvas *c, LineJoin join) { c->st.join = join; }
void cv_global_alpha(Canvas *c, double a) { if (a >= 0 && a <= 1) c->st.alpha = a; }
void cv_filter_blur(Canvas *c, double px) { c->st.blur = px; }
void cv_font(Canvas *c, const char *css) {
  double spacing = c->st.font.spacing;   // (ctx.letterSpacing is its own property)
  c->st.font = font_parse(css);
  c->st.font.spacing = spacing;
}
void cv_letter_spacing(Canvas *c, double px) { c->st.font.spacing = px; }
const uint8_t *canvas_pixels(const Canvas *c) { return c->px; }
uint8_t *canvas_pixels_rw(Canvas *c) { return c->px; }
void cv_text_align(Canvas *c, TextAlign a) { c->st.align = a; }
void cv_text_baseline(Canvas *c, TextBaseline b) { c->st.baseline = b; }

void cv_save(Canvas *c) {
  if (c->st.clip) c->st.clip->refs++;
  vec_push(&c->stack, c->st);
}
void cv_restore(Canvas *c) {
  if (!c->stack.len) return;
  clip_release(c->st.clip);
  c->st = vec_pop(&c->stack);
  // gradient pointers refer into the state itself
  if (c->st.fill.grad) c->st.fill.grad = &c->st.fill_grad;
  if (c->st.stroke.grad) c->st.stroke.grad = &c->st.stroke_grad;
}
void cv_translate(Canvas *c, double x, double y) { c->st.ctm = aff_mul(c->st.ctm, (Affine){ 1, 0, 0, 1, x, y }); }
void cv_scale(Canvas *c, double x, double y) { c->st.ctm = aff_mul(c->st.ctm, (Affine){ x, 0, 0, y, 0, 0 }); }
void cv_rotate(Canvas *c, double a) { c->st.ctm = aff_mul(c->st.ctm, (Affine){ cos(a), sin(a), -sin(a), cos(a), 0, 0 }); }

// ---- paths -------------------------------------------------------------------------------------

void cv_begin_path(Canvas *c) {
  for (size_t i = 0; i < c->path.len; i++) vec_free(&c->path.data[i].pts);
  vec_clear(&c->path);
  c->has_current = false;
}

static void add_point(Canvas *c, V2 d) {
  Subpath *sp = &vec_last(&c->path);
  if (sp->pts.len && sp->pts.data[sp->pts.len - 1].x == d.x && sp->pts.data[sp->pts.len - 1].y == d.y) return;
  vec_push(&sp->pts, d);
}

static void start_subpath(Canvas *c, V2 d) {
  vec_push(&c->path, (Subpath){});
  vec_push(&vec_last(&c->path).pts, d);
  c->has_current = true;
  c->current = d;
}

void cv_move_to(Canvas *c, double x, double y) { start_subpath(c, aff_apply(c->st.ctm, x, y)); }

void cv_line_to(Canvas *c, double x, double y) {
  V2 d = aff_apply(c->st.ctm, x, y);
  if (!c->has_current) { start_subpath(c, d); return; }
  add_point(c, d);
  c->current = d;
}

void cv_close_path(Canvas *c) {
  if (!c->has_current || !c->path.len) return;
  Subpath *sp = &vec_last(&c->path);
  sp->closed = true;
  V2 first = sp->pts.data[0];
  start_subpath(c, first);   // a new subpath starts at the closed one's first point
}

// segments for a curve of device-space size r: tolerance ~0.1 px
static int curve_segments(double r_dev, double sweep) {
  if (r_dev <= 0) return 1;
  double da = 2 * acos(fmax(-1, 1 - 0.1 / fmax(r_dev, 0.1)));
  int n = (int)ceil(fabs(sweep) / fmax(da, 1e-3));
  return imax(1, imin(n, 4096));
}

void cv_ellipse(Canvas *c, double x, double y, double rx, double ry, double rot, double a0, double a1, bool ccw) {
  // canvas sweep rules: clockwise: a1 >= a0 (normalized, full circle when >= 2 pi); ccw likewise
  double sweep;
  const double TAU = 2 * PI_D;
  if (!ccw) {
    if (a1 - a0 >= TAU) sweep = TAU;
    else { sweep = fmod(a1 - a0, TAU); if (sweep < 0) sweep += TAU; }
  } else {
    if (a0 - a1 >= TAU) sweep = -TAU;
    else { sweep = -fmod(a0 - a1, TAU); if (sweep > 0) sweep -= TAU; }
  }
  double r_dev = fmax(rx, ry) * aff_scale(c->st.ctm);
  int n = curve_segments(r_dev, sweep);
  double cr = cos(rot), sr = sin(rot);
  for (int i = 0; i <= n; i++) {
    double a = a0 + sweep * i / n;
    double ex = rx * cos(a), ey = ry * sin(a);
    V2 d = aff_apply(c->st.ctm, x + ex * cr - ey * sr, y + ex * sr + ey * cr);
    if (i == 0) {
      if (!c->has_current) start_subpath(c, d);
      else { add_point(c, d); }
    } else {
      add_point(c, d);
    }
    c->current = d;
  }
}

void cv_arc(Canvas *c, double x, double y, double r, double a0, double a1, bool ccw) { cv_ellipse(c, x, y, r, r, 0, a0, a1, ccw); }

void cv_bezier_to(Canvas *c, double c1x, double c1y, double c2x, double c2y, double x, double y) {
  if (!c->has_current) cv_move_to(c, c1x, c1y);
  V2 p0 = c->current;
  V2 p1 = aff_apply(c->st.ctm, c1x, c1y), p2 = aff_apply(c->st.ctm, c2x, c2y), p3 = aff_apply(c->st.ctm, x, y);
  double len = v2_dist(p0, p1) + v2_dist(p1, p2) + v2_dist(p2, p3);
  int n = imax(1, imin(1024, (int)ceil(sqrt(len * 2))));
  for (int i = 1; i <= n; i++) {
    double t = (double)i / n, u = 1 - t;
    V2 q = v2_add(v2_add(v2_scale(p0, u * u * u), v2_scale(p1, 3 * u * u * t)), v2_add(v2_scale(p2, 3 * u * t * t), v2_scale(p3, t * t * t)));
    add_point(c, q);
  }
  c->current = p3;
}

void cv_quadratic_to(Canvas *c, double cx, double cy, double x, double y) {
  if (!c->has_current) cv_move_to(c, cx, cy);
  V2 p0 = c->current, p1 = aff_apply(c->st.ctm, cx, cy), p2 = aff_apply(c->st.ctm, x, y);
  double len = v2_dist(p0, p1) + v2_dist(p1, p2);
  int n = imax(1, imin(1024, (int)ceil(sqrt(len * 2))));
  for (int i = 1; i <= n; i++) {
    double t = (double)i / n, u = 1 - t;
    add_point(c, v2_add(v2_add(v2_scale(p0, u * u), v2_scale(p1, 2 * u * t)), v2_scale(p2, t * t)));
  }
  c->current = p2;
}

void cv_rect(Canvas *c, double x, double y, double w, double h) {
  cv_move_to(c, x, y);
  cv_line_to(c, x + w, y);
  cv_line_to(c, x + w, y + h);
  cv_line_to(c, x, y + h);
  cv_close_path(c);
}

// ---- coverage ----------------------------------------------------------------------------------
// Shapes are collected as edges; acc_resolve() scan-converts them with 16 sub-scanlines per pixel
// row and exact horizontal span coverage, under the non-zero winding rule (so overlapping
// pieces of a stroke form a true union).

typedef struct Edge { double x0, y0, x1, y1; int dir; } Edge;   // y0 < y1

typedef struct Acc {
  int x0, y0, w, h;     // canvas-space origin and size of the coverage buffer
  float *a;             // coverage, w * h (stride == w + 2 for the historic layout)
  int stride;
  Vec(Edge) edges;
} Acc;

static Acc acc_new(Canvas *c, double minx, double miny, double maxx, double maxy) {
  Acc A = {};
  int x0 = (int)floor(fmax(minx, 0)), y0 = (int)floor(fmax(miny, 0));
  int x1 = (int)ceil(fmin(maxx, c->w)), y1 = (int)ceil(fmin(maxy, c->h));
  if (x1 <= x0 || y1 <= y0) return A;
  A.x0 = x0; A.y0 = y0; A.w = x1 - x0; A.h = y1 - y0;
  A.stride = A.w + 2;
  A.a = xcalloc((size_t)A.stride * (size_t)A.h, sizeof(float));
  return A;
}

static void acc_line(Acc *A, V2 p, V2 q) {
  if (!A->a || p.y == q.y) return;
  if (p.y < q.y) vec_push(&A->edges, ((Edge){ p.x, p.y, q.x, q.y, 1 }));
  else vec_push(&A->edges, ((Edge){ q.x, q.y, p.x, p.y, -1 }));
}

static void acc_poly(Acc *A, const V2 *p, size_t n) {
  for (size_t i = 0; i < n; i++) acc_line(A, p[i], p[(i + 1) % n]);
}

typedef struct Cross { double x; int dir; } Cross;

static int cross_cmp(const void *a, const void *b) {
  double d = ((const Cross *)a)->x - ((const Cross *)b)->x;
  return d < 0 ? -1 : d > 0;
}

static int edge_ymin_cmp(const void *a, const void *b) {
  double d = ((const Edge *)a)->y0 - ((const Edge *)b)->y0;
  return d < 0 ? -1 : d > 0;
}

// adds coverage `k` for canvas x-span [xa, xb) into a buffer row
static void span_add(float *row, int bx0, int bw, double xa, double xb, float k) {
  xa -= bx0;
  xb -= bx0;
  if (xb <= 0 || xa >= bw) return;
  if (xa < 0) xa = 0;
  if (xb > bw) xb = bw;
  int ia = (int)floor(xa), ib = (int)floor(xb);
  if (ia == ib) { row[ia] += (float)((xb - xa) * k); return; }
  row[ia] += (float)((ia + 1 - xa) * k);
  for (int i = ia + 1; i < ib; i++) row[i] += k;
  if (ib < bw) row[ib] += (float)((xb - ib) * k);
}

enum { SUBSCAN = 16 };

static void acc_resolve(Acc *A) {
  if (!A->a) return;
  if (A->edges.len) qsort(A->edges.data, A->edges.len, sizeof(Edge), edge_ymin_cmp);
  Vec(Cross) xs = {};
  Vec(int) active = {};
  size_t next = 0;
  const float k = 1.0f / SUBSCAN;
  for (int y = 0; y < A->h; y++) {
    double top = A->y0 + y, bot = top + 1;
    // active edges: those overlapping this row
    while (next < A->edges.len && A->edges.data[next].y0 < bot) vec_push(&active, (int)next++);
    size_t keep = 0;
    for (size_t i = 0; i < active.len; i++)
      if (A->edges.data[active.data[i]].y1 > top) active.data[keep++] = active.data[i];
    active.len = keep;
    if (!active.len) continue;
    float *row = A->a + (size_t)y * A->stride;
    for (int sub = 0; sub < SUBSCAN; sub++) {
      double sy = top + (sub + 0.5) / SUBSCAN;
      vec_clear(&xs);
      for (size_t i = 0; i < active.len; i++) {
        const Edge *e = &A->edges.data[active.data[i]];
        if (sy < e->y0 || sy >= e->y1) continue;
        double t = (sy - e->y0) / (e->y1 - e->y0);
        vec_push(&xs, ((Cross){ e->x0 + (e->x1 - e->x0) * t, e->dir }));
      }
      if (xs.len < 2) continue;
      qsort(xs.data, xs.len, sizeof(Cross), cross_cmp);
      int wind = 0;
      for (size_t i = 0; i + 1 < xs.len; i++) {
        wind += xs.data[i].dir;
        if (wind != 0) span_add(row, A->x0, A->w, xs.data[i].x, xs.data[i + 1].x, k);
      }
    }
  }
  for (int y = 0; y < A->h; y++) {
    float *row = A->a + (size_t)y * A->stride;
    for (int x = 0; x < A->w; x++) if (row[x] > 1) row[x] = 1;
  }
  vec_free(&xs);
  vec_free(&active);
  vec_free(&A->edges);
}

// ---- painting ----------------------------------------------------------------------------------

static void grad_color(const Gradient *g, double t, double out[4]) {
  // Skia (Chromium's canvas) interpolates gradient stops unpremultiplied, then premultiplies
  if (g->nstops == 0) { out[0] = out[1] = out[2] = out[3] = 0; return; }
  const GradStop *s = g->stops;
  if (t <= s[0].t) memcpy(out, s[0].rgba, sizeof(double) * 4);
  else if (t >= s[g->nstops - 1].t) memcpy(out, s[g->nstops - 1].rgba, sizeof(double) * 4);
  else {
    for (int i = 0; i + 1 < g->nstops; i++) {
      if (t < s[i].t || t > s[i + 1].t) continue;
      double span = s[i + 1].t - s[i].t;
      double k = span > 0 ? (t - s[i].t) / span : 1;
      for (int ch = 0; ch < 4; ch++) out[ch] = s[i].rgba[ch] + (s[i + 1].rgba[ch] - s[i].rgba[ch]) * k;
      break;
    }
  }
  for (int ch = 0; ch < 3; ch++) out[ch] *= out[3];
}

// paint colour (premultiplied, 0..1) at device pixel centre (px, py); false = transparent
static bool style_at(const Style *s, Affine inv, double px, double py, double out[4]) {
  if (s->kind == STYLE_COLOR) {
    for (int ch = 0; ch < 3; ch++) out[ch] = s->rgba[ch] * s->rgba[3];
    out[3] = s->rgba[3];
    return true;
  }
  const Gradient *g = s->grad;
  V2 u = aff_apply(inv, px, py);
  if (s->kind == STYLE_LINEAR) {
    double dx = g->x1 - g->x0, dy = g->y1 - g->y0, l2 = dx * dx + dy * dy;
    if (l2 == 0) return false;
    double t = ((u.x - g->x0) * dx + (u.y - g->y0) * dy) / l2;
    grad_color(g, t, out);
    return true;
  }
  // two-point conical: largest t with |u - c(t)| = r(t), r(t) >= 0
  double cdx = g->x1 - g->x0, cdy = g->y1 - g->y0, dr = g->r1 - g->r0;
  double pdx = u.x - g->x0, pdy = u.y - g->y0;
  double a = cdx * cdx + cdy * cdy - dr * dr;
  double b = pdx * cdx + pdy * cdy + g->r0 * dr;
  double cc = pdx * pdx + pdy * pdy - g->r0 * g->r0;
  double t;
  if (fabs(a) < 1e-12) {
    if (b == 0) return false;
    t = cc / (2 * b);
    if (g->r0 + t * dr < 0) return false;
  } else {
    double disc = b * b - a * cc;
    if (disc < 0) return false;
    double sq = sqrt(disc);
    double t1 = (b + sq) / a, t2 = (b - sq) / a;
    t = fmax(t1, t2);
    if (g->r0 + t * dr < 0) { t = fmin(t1, t2); if (g->r0 + t * dr < 0) return false; }
  }
  grad_color(g, t, out);
  return true;
}

static uint8_t to8(double v) {
  v = v * 255 + 0.5;
  return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

// separable Gaussian blur of a premultiplied float RGBA layer (sigma in px)
static void blur_layer(float *l, int w, int h, double sigma) {
  int r = (int)ceil(sigma * 3);
  if (r < 1) return;
  float *k = xmalloc((size_t)(2 * r + 1) * sizeof(float));
  double sum = 0;
  for (int i = -r; i <= r; i++) { k[i + r] = (float)exp(-(double)(i * i) / (2 * sigma * sigma)); sum += k[i + r]; }
  for (int i = 0; i <= 2 * r; i++) k[i] = (float)(k[i] / sum);
  float *tmp = xmalloc((size_t)w * (size_t)h * 4 * sizeof(float));
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      float acc[4] = { 0, 0, 0, 0 };
      for (int i = -r; i <= r; i++) {
        int xx = x + i;
        if (xx < 0 || xx >= w) continue;   // transparent outside the canvas
        const float *p = l + ((size_t)y * w + xx) * 4;
        for (int ch = 0; ch < 4; ch++) acc[ch] += p[ch] * k[i + r];
      }
      memcpy(tmp + ((size_t)y * w + x) * 4, acc, sizeof acc);
    }
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      float acc[4] = { 0, 0, 0, 0 };
      for (int i = -r; i <= r; i++) {
        int yy = y + i;
        if (yy < 0 || yy >= h) continue;
        const float *p = tmp + ((size_t)yy * w + x) * 4;
        for (int ch = 0; ch < 4; ch++) acc[ch] += p[ch] * k[i + r];
      }
      memcpy(l + ((size_t)y * w + x) * 4, acc, sizeof acc);
    }
  free(tmp);
  free(k);
}

// composites a resolved coverage mask with a style (source-over), honouring alpha, clip, blur
static void paint(Canvas *c, Acc *A, const Style *s) {
  if (!A->a) return;
  Affine inv = aff_invert(c->st.ctm);
  const ClipMask *clip = c->st.clip;
  if (c->st.blur > 0) {
    // filter: the shape is drawn into a transparent layer, blurred, then composited
    float *layer = xcalloc((size_t)c->w * (size_t)c->h * 4, sizeof(float));
    for (int y = 0; y < A->h; y++)
      for (int x = 0; x < A->w; x++) {
        float cov = A->a[(size_t)y * A->stride + x];
        if (cov <= 0) continue;
        int dx = A->x0 + x, dy = A->y0 + y;
        double col[4];
        if (!style_at(s, inv, dx + 0.5, dy + 0.5, col)) continue;
        float *p = layer + ((size_t)dy * c->w + dx) * 4;
        for (int ch = 0; ch < 4; ch++) p[ch] = (float)(col[ch] * cov);
      }
    blur_layer(layer, c->w, c->h, c->st.blur);
    for (size_t i = 0; i < (size_t)c->w * c->h; i++) {
      float *p = layer + i * 4;
      double k = c->st.alpha * (clip ? clip_cov(clip, (int)(i % (size_t)c->w), (int)(i / (size_t)c->w)) : 1);
      double sa = p[3] * k;
      if (sa <= 0 && p[0] <= 0 && p[1] <= 0 && p[2] <= 0) continue;
      uint8_t *d = c->px + i * 4;
      for (int ch = 0; ch < 3; ch++) d[ch] = to8(p[ch] * k + d[ch] / 255.0 * (1 - sa));
      d[3] = to8(sa + d[3] / 255.0 * (1 - sa));
    }
    free(layer);
    return;
  }
  for (int y = 0; y < A->h; y++) {
    int dy = A->y0 + y;
    for (int x = 0; x < A->w; x++) {
      float cov = A->a[(size_t)y * A->stride + x];
      if (cov <= 0) continue;
      int dx = A->x0 + x;
      size_t i = (size_t)dy * c->w + dx;
      double k = cov * c->st.alpha * (clip ? clip_cov(clip, dx, dy) : 1);
      if (k <= 0) continue;
      double col[4];
      if (!style_at(s, inv, dx + 0.5, dy + 0.5, col)) continue;
      double sa = col[3] * k;
      uint8_t *d = c->px + i * 4;
      for (int ch = 0; ch < 3; ch++) d[ch] = to8(col[ch] * k + d[ch] / 255.0 * (1 - sa));
      d[3] = to8(sa + d[3] / 255.0 * (1 - sa));
    }
  }
}

static void bounds_of_path(const Canvas *c, double *minx, double *miny, double *maxx, double *maxy) {
  *minx = *miny = INFINITY;
  *maxx = *maxy = -INFINITY;
  for (size_t i = 0; i < c->path.len; i++)
    for (size_t k = 0; k < c->path.data[i].pts.len; k++) {
      V2 p = c->path.data[i].pts.data[k];
      *minx = fmin(*minx, p.x); *miny = fmin(*miny, p.y);
      *maxx = fmax(*maxx, p.x); *maxy = fmax(*maxy, p.y);
    }
}

void cv_fill(Canvas *c) {
  double x0, y0, x1, y1;
  bounds_of_path(c, &x0, &y0, &x1, &y1);
  if (!isfinite(x0)) return;
  // (acc_new clamps to the canvas; edges left of it are clamped to x = 0 and still count)
  Acc A = acc_new(c, x0 - 1, y0 - 1, x1 + 1, y1 + 1);
  for (size_t i = 0; i < c->path.len; i++) {
    const Subpath *sp = &c->path.data[i];
    if (sp->pts.len >= 2) acc_poly(&A, sp->pts.data, sp->pts.len);   // fill closes every subpath
  }
  acc_resolve(&A);
  paint(c, &A, &c->st.fill);
  free(A.a);
}

// ---- stroking ----------------------------------------------------------------------------------

typedef Vec(V2) Poly;

static void quad(Acc *A, V2 a, V2 b, V2 cc, V2 d) {
  // consistent orientation (so overlapping pieces add up instead of cancelling)
  double area = v2_cross(v2_sub(b, a), v2_sub(cc, a)) + v2_cross(v2_sub(cc, a), v2_sub(d, a));
  V2 p[4] = { a, b, cc, d };
  if (area < 0) { V2 t = p[1]; p[1] = p[3]; p[3] = t; }
  acc_poly(A, p, 4);
}

static void disc(Acc *A, V2 c0, double r) {
  int n = curve_segments(r, 2 * PI_D);
  n = imax(n, 8);
  V2 pts[4097];
  n = imin(n, 4096);
  for (int i = 0; i < n; i++) pts[i] = v2(c0.x + r * cos(2 * PI_D * i / n), c0.y + r * sin(2 * PI_D * i / n));
  acc_poly(A, pts, (size_t)n);
}

static void tri(Acc *A, V2 a, V2 b, V2 cc) {
  V2 p[3] = { a, b, cc };
  if (v2_cross(v2_sub(b, a), v2_sub(cc, a)) < 0) { p[1] = cc; p[2] = b; }
  acc_poly(A, p, 3);
}

// strokes one polyline (device space) with half width hw into the accumulator
static void stroke_polyline(Acc *A, const V2 *p, size_t n, bool closed, double hw, LineCap cap, LineJoin join, double miter_limit) {
  if (n == 0) return;
  if (n == 1) {   // zero-length subpath: round / square caps draw a dot
    if (cap == CAP_ROUND) disc(A, p[0], hw);
    else if (cap == CAP_SQUARE) quad(A, v2(p[0].x - hw, p[0].y - hw), v2(p[0].x + hw, p[0].y - hw), v2(p[0].x + hw, p[0].y + hw), v2(p[0].x - hw, p[0].y + hw));
    return;
  }
  size_t nseg = closed ? n : n - 1;
  for (size_t i = 0; i < nseg; i++) {
    V2 a = p[i], b = p[(i + 1) % n];
    V2 d = v2_norm(v2_sub(b, a));
    if (v2_len_sq(v2_sub(b, a)) == 0) continue;
    V2 nrm = v2(-d.y * hw, d.x * hw);
    V2 a2 = a, b2 = b;
    if (!closed && cap == CAP_SQUARE) {
      if (i == 0) a2 = v2_sub(a, v2_scale(d, hw));
      if (i == nseg - 1) b2 = v2_add(b, v2_scale(d, hw));
    }
    quad(A, v2_add(a2, nrm), v2_add(b2, nrm), v2_sub(b2, nrm), v2_sub(a2, nrm));
  }
  // joins
  size_t first = closed ? 0 : 1, last = closed ? n : n - 1;
  for (size_t i = first; i < last; i++) {
    V2 prev = p[(i + n - 1) % n], cur = p[i], next = p[(i + 1) % n];
    V2 d0 = v2_norm(v2_sub(cur, prev)), d1 = v2_norm(v2_sub(next, cur));
    double cr = v2_cross(d0, d1);
    if (fabs(cr) < 1e-12 && v2_dot(d0, d1) > 0) continue;   // straight
    if (join == JOIN_ROUND) { disc(A, cur, hw); continue; }
    // outer side: the side the path turns away from
    double s = cr > 0 ? -1 : 1;
    V2 o0 = v2_add(cur, v2_scale(v2(-d0.y, d0.x), hw * s)), o1 = v2_add(cur, v2_scale(v2(-d1.y, d1.x), hw * s));
    tri(A, cur, o0, o1);   // bevel
    if (join == JOIN_MITER) {
      double cos_t = v2_dot(d0, d1);
      double miter_len = 1 / sqrt(fmax((1 + cos_t) / 2, 1e-12));   // 1 / sin(theta / 2) relative to hw
      if (miter_len <= miter_limit) {
        V2 bis = v2_norm(v2_add(v2_scale(v2(-d0.y, d0.x), s), v2_scale(v2(-d1.y, d1.x), s)));
        V2 tip = v2_add(cur, v2_scale(bis, hw * miter_len));
        tri(A, o0, tip, o1);
      }
    }
  }
  if (!closed && cap == CAP_ROUND) {
    disc(A, p[0], hw);
    disc(A, p[n - 1], hw);
  }
}

// line width in device space (uniform-ish transforms)
static double device_half_width(const Canvas *c) { return c->st.line_width * aff_scale(c->st.ctm) / 2; }

void cv_stroke(Canvas *c) {
  double x0, y0, x1, y1;
  bounds_of_path(c, &x0, &y0, &x1, &y1);
  if (!isfinite(x0)) return;
  double hw = device_half_width(c);
  double pad = hw * fmax(c->st.miter_limit, 2) + 2;
  Acc A = acc_new(c, x0 - pad, y0 - pad, x1 + pad, y1 + pad);
  for (size_t i = 0; i < c->path.len; i++) {
    const Subpath *sp = &c->path.data[i];
    size_t n = sp->pts.len;
    if (n < 2) continue;   // a lone moveTo (or the subpath closePath opens) draws nothing
    // (a closed subpath's duplicate end point was never stored)
    stroke_polyline(&A, sp->pts.data, n, sp->closed && n > 2, hw, c->st.cap, c->st.join, c->st.miter_limit);
  }
  acc_resolve(&A);
  paint(c, &A, &c->st.stroke);
  free(A.a);
}

void cv_clip(Canvas *c) {
  double x0, y0, x1, y1;
  bounds_of_path(c, &x0, &y0, &x1, &y1);
  ClipMask *m;
  if (isfinite(x0)) {
    Acc A = acc_new(c, x0 - 1, y0 - 1, x1 + 1, y1 + 1);
    for (size_t i = 0; i < c->path.len; i++) {
      const Subpath *sp = &c->path.data[i];
      if (sp->pts.len >= 2) acc_poly(&A, sp->pts.data, sp->pts.len);
    }
    acc_resolve(&A);
    m = xcalloc(1, sizeof *m + (size_t)A.w * A.h * sizeof(float));
    *m = (ClipMask){ A.x0, A.y0, A.w, A.h, 1 };
    for (int y = 0; y < A.h; y++)
      for (int x = 0; x < A.w; x++) {
        float v = A.a ? A.a[(size_t)y * A.stride + x] : 0;
        if (c->st.clip) v *= clip_cov(c->st.clip, A.x0 + x, A.y0 + y);   // intersect with the current clip
        m->a[(size_t)y * A.w + x] = v;
      }
    free(A.a);
  } else {
    m = xcalloc(1, sizeof *m);   // empty path: nothing is visible
    *m = (ClipMask){ 0, 0, 0, 0, 1 };
  }
  clip_release(c->st.clip);
  c->st.clip = m;
}

void cv_fill_rect(Canvas *c, double x, double y, double w, double h) {
  // fillRect does not touch the current path
  Vec(Subpath) saved = { c->path.data, c->path.len, c->path.cap };
  bool had = c->has_current;
  V2 cur = c->current;
  c->path = (typeof(c->path)){};
  c->has_current = false;
  cv_rect(c, x, y, w, h);
  cv_fill(c);
  cv_begin_path(c);
  vec_free(&c->path);
  c->path.data = saved.data; c->path.len = saved.len; c->path.cap = saved.cap;
  c->has_current = had;
  c->current = cur;
}

void cv_stroke_rect(Canvas *c, double x, double y, double w, double h) {
  Vec(Subpath) saved = { c->path.data, c->path.len, c->path.cap };
  bool had = c->has_current;
  V2 cur = c->current;
  c->path = (typeof(c->path)){};
  c->has_current = false;
  cv_rect(c, x, y, w, h);
  cv_stroke(c);
  cv_begin_path(c);
  vec_free(&c->path);
  c->path.data = saved.data; c->path.len = saved.len; c->path.cap = saved.cap;
  c->has_current = had;
  c->current = cur;
}

void cv_clear_rect(Canvas *c, double x, double y, double w, double h) {
  // (only axis-aligned, whole-pixel use in the scene)
  CHECK(c->st.ctm.b == 0 && c->st.ctm.c == 0);
  V2 a = aff_apply(c->st.ctm, x, y), b = aff_apply(c->st.ctm, x + w, y + h);
  int x0 = imax(0, (int)js_round(fmin(a.x, b.x))), x1 = imin(c->w, (int)js_round(fmax(a.x, b.x)));
  int y0 = imax(0, (int)js_round(fmin(a.y, b.y))), y1 = imin(c->h, (int)js_round(fmax(a.y, b.y)));
  for (int yy = y0; yy < y1; yy++) memset(c->px + ((size_t)yy * c->w + x0) * 4, 0, (size_t)imax(0, x1 - x0) * 4);
}

// ---- text --------------------------------------------------------------------------------------

static double align_offset(const Canvas *c, double width) {
  switch (c->st.align) {
  case ALIGN_CENTER: return -width / 2;
  case ALIGN_RIGHT: case ALIGN_END: return -width;
  default: return 0;
  }
}

static double baseline_offset(const Canvas *c) {
  const FontSpec *f = &c->st.font;
  switch (c->st.baseline) {
  case BASE_MIDDLE: return (font_ascent(f) - font_descent(f)) / 2;   // middle of the em box
  case BASE_TOP: return font_ascent(f);
  case BASE_BOTTOM: return -font_descent(f);
  default: return 0;
  }
}

double cv_measure_text(Canvas *c, const char *text) { return font_measure(&c->st.font, text); }

typedef struct TextSink { Canvas *c; Acc *A; double hw; } TextSink;

// glyph skeleton strokes arrive in user space (text origin applied): transform and stroke
static void text_stroke_cb(void *user, const V2 *pts, int n, bool closed, double half_width) {
  TextSink *t = user;
  V2 d[512];
  CHECK(n <= 512);
  for (int i = 0; i < n; i++) d[i] = aff_apply(t->c->st.ctm, pts[i].x, pts[i].y);
  double hw = half_width * aff_scale(t->c->st.ctm) + t->hw;
  if (hw > 0) stroke_polyline(t->A, d, (size_t)n, closed, hw, CAP_ROUND, JOIN_ROUND, 10);
}

static Acc text_mask(Canvas *c, const char *text, double x, double y, double extra_hw) {
  double w = font_measure(&c->st.font, text);
  double ox = x + align_offset(c, w), oy = y + baseline_offset(c);
  double s = aff_scale(c->st.ctm);
  double pad = (c->st.font.size + fabs(extra_hw)) * s + 4;
  V2 corners[4] = { aff_apply(c->st.ctm, ox, oy - c->st.font.size * 1.2), aff_apply(c->st.ctm, ox + w, oy - c->st.font.size * 1.2),
                    aff_apply(c->st.ctm, ox, oy + c->st.font.size * 0.4), aff_apply(c->st.ctm, ox + w, oy + c->st.font.size * 0.4) };
  double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
  for (int i = 0; i < 4; i++) { x0 = fmin(x0, corners[i].x); y0 = fmin(y0, corners[i].y); x1 = fmax(x1, corners[i].x); y1 = fmax(y1, corners[i].y); }
  Acc A = acc_new(c, x0 - pad, y0 - pad, x1 + pad, y1 + pad);
  TextSink sink = { c, &A, extra_hw * s };
  font_emit(&c->st.font, text, ox, oy, text_stroke_cb, &sink);
  acc_resolve(&A);
  return A;
}

void cv_fill_text(Canvas *c, const char *text, double x, double y) {
  Acc A = text_mask(c, text, x, y, 0);
  paint(c, &A, &c->st.fill);
  free(A.a);
}

void cv_stroke_text(Canvas *c, const char *text, double x, double y) {
  // the outline of the glyph shapes, lineWidth wide: (glyphs grown by lw/2) minus (glyphs
  // shrunk by lw/2)
  double lw2 = c->st.line_width / 2;
  Acc outer = text_mask(c, text, x, y, lw2);
  Acc inner = text_mask(c, text, x, y, -lw2);
  if (outer.a && inner.a) {
    CHECK(outer.x0 <= inner.x0 && outer.y0 <= inner.y0);
    for (int yy = 0; yy < inner.h; yy++)
      for (int xx = 0; xx < inner.w; xx++) {
        int ox = inner.x0 - outer.x0 + xx, oy = inner.y0 - outer.y0 + yy;
        if (ox >= outer.w || oy >= outer.h) continue;
        float *o = &outer.a[(size_t)oy * outer.stride + ox];
        *o = fmaxf(0, *o - inner.a[(size_t)yy * inner.stride + xx]);
      }
  }
  paint(c, &outer, &c->st.stroke);
  free(outer.a);
  free(inner.a);
}

// ---- gradients ---------------------------------------------------------------------------------

Gradient cv_linear_gradient(Canvas *c, double x0, double y0, double x1, double y1) {
  (void)c;
  return (Gradient){ .kind = STYLE_LINEAR, .x0 = x0, .y0 = y0, .x1 = x1, .y1 = y1 };
}

Gradient cv_radial_gradient(Canvas *c, double x0, double y0, double r0, double x1, double y1, double r1) {
  (void)c;
  return (Gradient){ .kind = STYLE_RADIAL, .x0 = x0, .y0 = y0, .r0 = r0, .x1 = x1, .y1 = y1, .r1 = r1 };
}

void grad_add_stop(Gradient *g, double t, const char *css) {
  CHECK(g->nstops < 16 && t >= 0 && t <= 1);
  GradStop s = { .t = t };
  css_color(css, s.rgba);
  // stops keep insertion order among equal offsets (sorted stably by offset)
  int i = g->nstops;
  while (i > 0 && g->stops[i - 1].t > t) { g->stops[i] = g->stops[i - 1]; i--; }
  g->stops[i] = s;
  g->nstops++;
}

// ---- image data ---------------------------------------------------------------------------------

void cv_get_image_data(const Canvas *c, int x, int y, int w, int h, uint8_t *out) {
  for (int yy = 0; yy < h; yy++)
    for (int xx = 0; xx < w; xx++) {
      int sx = x + xx, sy = y + yy;
      uint8_t *o = out + ((size_t)yy * w + xx) * 4;
      if (sx < 0 || sy < 0 || sx >= c->w || sy >= c->h) { memset(o, 0, 4); continue; }
      const uint8_t *p = c->px + ((size_t)sy * c->w + sx) * 4;
      uint8_t a = p[3];
      if (a == 255) { memcpy(o, p, 4); continue; }
      if (a == 0) { memset(o, 0, 4); continue; }
      for (int ch = 0; ch < 3; ch++) {
        int v = (p[ch] * 255 + a / 2) / a;
        o[ch] = (uint8_t)(v > 255 ? 255 : v);
      }
      o[3] = a;
    }
}

void cv_put_image_data(Canvas *c, const uint8_t *rgba, int x, int y, int w, int h) {
  for (int yy = 0; yy < h; yy++)
    for (int xx = 0; xx < w; xx++) {
      int dx = x + xx, dy = y + yy;
      if (dx < 0 || dy < 0 || dx >= c->w || dy >= c->h) continue;
      const uint8_t *s = rgba + ((size_t)yy * w + xx) * 4;
      uint8_t *d = c->px + ((size_t)dy * c->w + dx) * 4;
      uint8_t a = s[3];
      for (int ch = 0; ch < 3; ch++) d[ch] = a == 255 ? s[ch] : (uint8_t)((s[ch] * a + 127) / 255);
      d[3] = a;
    }
}

void cv_draw_image(Canvas *c, const Canvas *src, double sx, double sy, double sw, double sh,
                   double dx, double dy, double dw, double dh) {
  CHECK(c->st.ctm.a == 1 && c->st.ctm.b == 0 && c->st.ctm.c == 0 && c->st.ctm.d == 1 && c->st.ctm.e == 0 && c->st.ctm.f == 0);
  int x0 = imax(0, (int)floor(dx)), y0 = imax(0, (int)floor(dy));
  int x1 = imin(c->w, (int)ceil(dx + dw)), y1 = imin(c->h, (int)ceil(dy + dh));
  double fx = sw / dw, fy = sh / dh;
  // with a filter the image is drawn into a transparent layer, blurred, then composited
  float *layer = c->st.blur > 0 ? xcalloc((size_t)c->w * (size_t)c->h * 4, sizeof(float)) : nullptr;
  for (int y = y0; y < y1; y++)
    for (int x = x0; x < x1; x++) {
      // area average of the source footprint of this destination pixel
      double u0 = sx + (x - dx) * fx, u1 = u0 + fx, v0 = sy + (y - dy) * fy, v1 = v0 + fy;
      double acc[4] = { 0, 0, 0, 0 }, wsum = 0;
      for (int vy = (int)floor(v0); vy < (int)ceil(v1); vy++) {
        if (vy < 0 || vy >= src->h) continue;
        double wy = fmin(v1, vy + 1.0) - fmax(v0, (double)vy);
        for (int ux = (int)floor(u0); ux < (int)ceil(u1); ux++) {
          if (ux < 0 || ux >= src->w) continue;
          double wx = fmin(u1, ux + 1.0) - fmax(u0, (double)ux);
          double wgt = wx * wy;
          const uint8_t *p = src->px + ((size_t)vy * src->w + ux) * 4;
          for (int ch = 0; ch < 4; ch++) acc[ch] += p[ch] * wgt;
          wsum += wgt;
        }
      }
      if (wsum <= 0) continue;
      if (layer) {
        float *l = layer + ((size_t)y * c->w + x) * 4;
        for (int ch = 0; ch < 4; ch++) l[ch] = (float)(acc[ch] / wsum / 255);
        continue;
      }
      uint8_t *d = c->px + ((size_t)y * c->w + x) * 4;
      double sa = acc[3] / wsum / 255 * c->st.alpha;
      for (int ch = 0; ch < 3; ch++) d[ch] = to8(acc[ch] / wsum / 255 * c->st.alpha + d[ch] / 255.0 * (1 - sa));
      d[3] = to8(sa + d[3] / 255.0 * (1 - sa));
    }
  if (!layer) return;
  blur_layer(layer, c->w, c->h, c->st.blur);
  const ClipMask *clip = c->st.clip;
  for (size_t i = 0; i < (size_t)c->w * c->h; i++) {
    float *p = layer + i * 4;
    double k = c->st.alpha * (clip ? clip_cov(clip, (int)(i % (size_t)c->w), (int)(i / (size_t)c->w)) : 1);
    double sa = p[3] * k;
    if (sa <= 0 && p[0] <= 0 && p[1] <= 0 && p[2] <= 0) continue;
    uint8_t *d = c->px + i * 4;
    for (int ch = 0; ch < 3; ch++) d[ch] = to8(p[ch] * k + d[ch] / 255.0 * (1 - sa));
    d[3] = to8(sa + d[3] / 255.0 * (1 - sa));
  }
  free(layer);
}
