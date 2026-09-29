// Stroke-skeleton vector font (see font.h).
//
// Glyphs are written in a tiny path language, units of em, y up from the baseline, cap height
// 0.70: `M x y` starts a stroke, `L x y` extends it, `A cx cy rx ry a0 a1` extends it along an
// elliptical arc from angle a0 to a1 (degrees, counter-clockwise positive; the arc start is
// joined to the stroke so far), `Z` closes the stroke. Strokes are drawn with round caps and
// joins by the canvas stroker.
#include "canvas/font.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "core/common.h"

typedef struct Glyph { char ch; double adv; const char *path; } Glyph;

static const Glyph GLYPHS[] = {
  { ' ', 0.28, "" },
  { 'A', 0.72, "M .05 0 L .36 .7 L .67 0 M .16 .24 L .56 .24" },
  { 'B', 0.66, "M .09 .37 L .09 .7 L .34 .7 A .34 .535 .18 .165 90 -90 L .09 .37 L .09 0 L .37 0 A .37 .185 .2 .185 -90 90 L .09 .37" },
  { 'C', 0.72, "A .39 .35 .31 .35 42 318" },
  { 'D', 0.72, "M .09 0 L .09 .7 L .31 .7 A .31 .35 .32 .35 90 -90 Z" },
  { 'E', 0.62, "M .53 .7 L .09 .7 L .09 0 L .53 0 M .09 .36 L .45 .36" },
  { 'F', 0.58, "M .53 .7 L .09 .7 L .09 0 M .09 .36 L .45 .36" },
  { 'G', 0.76, "A .39 .35 .31 .35 42 360 L .70 .12 M .70 .33 L .45 .33" },
  { 'H', 0.74, "M .09 0 L .09 .7 M .65 0 L .65 .7 M .09 .36 L .65 .36" },
  { 'I', 0.28, "M .14 0 L .14 .7" },
  { 'J', 0.52, "M .43 .7 L .43 .22 A .26 .22 .17 .20 0 -180" },
  { 'K', 0.68, "M .09 0 L .09 .7 M .62 .7 L .09 .24 M .27 .40 L .64 0" },
  { 'L', 0.56, "M .09 .7 L .09 0 L .52 0" },
  { 'M', 0.88, "M .09 0 L .09 .7 L .44 .12 L .79 .7 L .79 0" },
  { 'N', 0.74, "M .09 0 L .09 .7 L .65 0 L .65 .7" },
  { 'O', 0.80, "A .40 .35 .32 .35 0 360 Z" },
  { 'P', 0.64, "M .09 0 L .09 .7 L .35 .7 A .35 .52 .19 .18 90 -90 L .09 .34" },
  { 'Q', 0.80, "A .40 .35 .32 .35 0 360 Z M .49 .15 L .72 -.04" },
  { 'R', 0.66, "M .09 0 L .09 .7 L .35 .7 A .35 .52 .19 .18 90 -90 L .09 .34 M .31 .34 L .59 0" },
  { 'S', 0.62, "A .31 .525 .22 .175 25 270 A .31 .175 .23 .175 90 -155" },
  { 'T', 0.62, "M .03 .7 L .59 .7 M .31 .7 L .31 0" },
  { 'U', 0.72, "M .09 .7 L .09 .25 A .36 .25 .27 .25 180 360 L .63 .7" },
  { 'V', 0.70, "M .03 .7 L .35 0 L .67 .7" },
  { 'W', 0.98, "M .03 .7 L .25 0 L .49 .6 L .73 0 L .95 .7" },
  { 'X', 0.68, "M .05 .7 L .63 0 M .63 .7 L .05 0" },
  { 'Y', 0.68, "M .04 .7 L .34 .34 L .64 .7 M .34 .34 L .34 0" },
  { 'Z', 0.64, "M .07 .7 L .57 .7 L .07 0 L .57 0" },
  { '0', 0.62, "A .31 .35 .23 .35 0 360 Z" },
  { '1', 0.62, "M .15 .56 L .34 .7 L .34 0" },
  { '2', 0.62, "A .31 .5 .21 .2 160 -35 L .09 0 L .54 0" },
  { '3', 0.62, "A .30 .535 .2 .165 155 -90 A .30 .185 .22 .185 90 -155" },
  { '4', 0.62, "M .44 0 L .44 .7 L .06 .2 L .57 .2" },
  { '5', 0.62, "M .51 .7 L .15 .7 L .11 .38 A .30 .23 .22 .23 125 -150" },
  { '6', 0.62, "A .38 .30 .30 .38 72 180 A .30 .22 .22 .22 180 540" },
  { '7', 0.62, "M .07 .7 L .55 .7 L .23 0" },
  { '8', 0.62, "A .31 .535 .19 .165 -90 270 Z A .31 .185 .22 .185 90 450 Z" },
  { '9', 0.62, "A .32 .48 .22 .22 0 360 A .24 .40 .30 .38 0 -108" },
  { '-', 0.40, "M .08 .27 L .32 .27" },
  { '.', 0.28, "M .14 .035 L .14 .036" },
  { ',', 0.28, "M .15 .04 L .10 -.10" },
  { ':', 0.28, "M .14 .035 L .14 .036 M .14 .45 L .14 .451" },
  { '\'', 0.24, "M .12 .7 L .12 .52" },
  { '&', 0.74, "M .66 0 L .20 .48 A .30 .56 .12 .14 180 -60 L .12 .18 A .27 .18 .18 .18 180 330 L .62 .30" },
  { '!', 0.28, "M .14 .7 L .14 .2 M .14 .035 L .14 .036" },
  { '?', 0.58, "A .29 .52 .20 .18 160 -60 L .29 .2 M .29 .035 L .29 .036" },
  { '/', 0.40, "M .04 -.05 L .36 .75" },
};

// the UI set (`cased`): lower case (x-height 0.50, ascenders 0.72, descenders -0.22) and the
// punctuation the UI text uses, keyed by code point
typedef struct UGlyph { uint32_t cp; double adv; const char *path; } UGlyph;
static const UGlyph UGLYPHS[] = {
  { 'a', 0.60, "A .29 .25 .21 .25 0 360 M .50 .50 L .50 0" },
  { 'b', 0.60, "M .08 .72 L .08 0 M .30 .50 A .30 .25 .22 .25 90 450" },
  { 'c', 0.54, "A .30 .25 .22 .25 40 320" },
  { 'd', 0.60, "M .52 .72 L .52 0 M .30 .50 A .30 .25 .22 .25 90 450" },
  { 'e', 0.58, "M .08 .25 L .52 .25 A .30 .25 .22 .25 0 320" },
  { 'f', 0.36, "M .36 .70 A .30 .62 .10 .10 60 180 L .20 0 M .06 .48 L .34 .48" },
  { 'g', 0.60, "A .29 .25 .21 .25 0 360 M .50 .50 L .50 -.05 A .29 -.05 .21 .15 0 -160" },
  { 'h', 0.58, "M .08 .72 L .08 0 M .08 .30 A .29 .30 .21 .20 180 0 L .50 0" },
  { 'i', 0.22, "M .10 0 L .10 .50 M .10 .66 L .10 .661" },
  { 'j', 0.26, "M .14 .50 L .14 -.08 A .04 -.08 .10 .12 0 -120 M .14 .66 L .14 .661" },
  { 'k', 0.52, "M .08 .72 L .08 0 M .46 .50 L .08 .18 M .20 .28 L .48 0" },
  { 'l', 0.22, "M .10 .72 L .10 0" },
  { 'm', 0.86, "M .08 .50 L .08 0 M .08 .32 A .25 .32 .17 .18 180 0 L .42 0 M .42 .32 A .59 .32 .17 .18 180 0 L .76 0" },
  { 'n', 0.58, "M .08 .50 L .08 0 M .08 .30 A .29 .30 .21 .20 180 0 L .50 0" },
  { 'o', 0.60, "A .30 .25 .22 .25 0 360 Z" },
  { 'p', 0.60, "M .08 .50 L .08 -.22 M .30 .50 A .30 .25 .22 .25 90 450" },
  { 'q', 0.60, "M .52 .50 L .52 -.22 M .30 .50 A .30 .25 .22 .25 90 450" },
  { 'r', 0.40, "M .08 .50 L .08 0 M .08 .28 A .30 .28 .22 .22 180 70" },
  { 's', 0.50, "A .26 .375 .16 .125 20 270 A .26 .125 .17 .125 90 -150" },
  { 't', 0.38, "M .18 .66 L .18 .08 A .28 .08 .10 .08 180 300 M .04 .48 L .36 .48" },
  { 'u', 0.58, "M .08 .50 L .08 .20 A .29 .20 .21 .20 180 360 M .50 .50 L .50 0" },
  { 'v', 0.54, "M .04 .50 L .27 0 L .50 .50" },
  { 'w', 0.76, "M .03 .50 L .20 0 L .38 .44 L .56 0 L .73 .50" },
  { 'x', 0.52, "M .05 .50 L .47 0 M .47 .50 L .05 0" },
  { 'y', 0.54, "M .04 .50 L .27 0 M .50 .50 L .20 -.22 L .08 -.22" },
  { 'z', 0.52, "M .06 .50 L .46 .50 L .06 0 L .46 0" },
  { 0xB7, 0.28, "M .14 .30 L .14 .301" },                       // middle dot
  { 0x2014, 1.00, "M .04 .27 L .96 .27" },                      // em dash
  { 0x2013, 0.54, "M .04 .27 L .50 .27" },                      // en dash
  { 0x2026, 0.84, "M .14 .035 L .14 .036 M .42 .035 L .42 .036 M .70 .035 L .70 .036" },   // ellipsis
  { 0xB0, 0.36, "A .20 .60 .09 .09 0 360 Z" },                  // degree
  { 0x2032, 0.24, "M .14 .70 L .10 .52" },                      // prime
  { 0x2019, 0.24, "M .14 .70 L .10 .56" },                      // right single quote
  { 0xD7, 0.56, "M .10 .12 L .46 .48 M .46 .12 L .10 .48" },    // multiplication sign
  { '%', 0.72, "M .58 .70 L .14 0 M .26 .56 A .17 .56 .09 .12 0 360 M .64 .14 A .55 .14 .09 .12 0 360" },
  { '(', 0.34, "A .40 .30 .26 .50 130 230" },
  { ')', 0.34, "A -.06 .30 .26 .50 50 -50" },
  { '+', 0.60, "M .08 .30 L .52 .30 M .30 .08 L .30 .52" },
  { '=', 0.60, "M .08 .38 L .52 .38 M .08 .20 L .52 .20" },
  { '"', 0.32, "M .10 .7 L .10 .52 M .22 .7 L .22 .52" },
  { ';', 0.28, "M .14 .45 L .14 .451 M .15 .04 L .10 -.10" },
  { '_', 0.50, "M 0 -.12 L .5 -.12" },
};

typedef struct Style {
  double hw;        // stroke half width (em)
  double sx;        // horizontal scale
  double shear;     // italic slant (x += y * shear)
  bool serif;
  double ascent, descent;
  bool mono;        // fixed advance (FONT_MONO)
} Style;

static Style style_raw(const FontSpec *f) {
  switch (f->family) {
  case FONT_MONO: return (Style){ 0.05, 1.0, 0, false, 0.90, 0.22, true };
  case FONT_GEO: return (Style){ f->bold ? 0.068 : 0.045, 1.04, 0, false, 0.90, 0.21, false };
  case FONT_CONDENSED: return (Style){ f->bold ? 0.062 : 0.045, 0.74, 0, false, 0.93, 0.21, false };
  case FONT_SCRIPT: return (Style){ f->bold ? 0.052 : 0.038, 0.92, 0.26, false, 0.88, 0.27, false };
  case FONT_SERIF: return (Style){ f->bold ? 0.066 : 0.045, 1.0, 0, true, 0.92, 0.22, false };
  default: return (Style){ f->bold ? 0.074 : 0.05, 1.0, f->italic ? 0.2 : 0, false, 0.905, 0.212, false };
  }
}
static Style style_of(const FontSpec *f) {
  Style s = style_raw(f);
  if (f->cased) {
    s.hw *= f->weight;
    if (f->italic && f->family == FONT_SERIF) s.shear = 0.2;
  }
  return s;
}

static bool has_word(const char *css, const char *w) {
  size_t n = strlen(w);
  for (const char *p = css; (p = strstr(p, w)); p++) {
    bool l = p == css || !isalnum((unsigned char)p[-1]);
    bool r = !isalnum((unsigned char)p[n]);
    if (l && r) return true;
  }
  return false;
}

FontSpec font_parse(const char *css) {
  FontSpec f = { FONT_SANS, 10, false, false, false, 1, 0 };
  f.italic = has_word(css, "italic");
  f.bold = has_word(css, "bold");
  f.cased = has_word(css, "cased");
  if (f.cased) f.weight = has_word(css, "300") || has_word(css, "light") ? 0.7 : has_word(css, "500") ? 1.25 : 1;
  const char *px = strstr(css, "px");
  if (px) {
    const char *s = px;
    while (s > css && (isdigit((unsigned char)s[-1]) || s[-1] == '.')) s--;
    f.size = strtod(s, nullptr);
    // the family list is what follows the size; the first family decides the style
    const char *fam = px + 2;
    while (*fam == ' ') fam++;
    if (f.cased && strstr(fam, "monospace")) f.family = FONT_MONO;
    else if (f.cased && (strstr(fam, "Didot") || strstr(fam, "Bodoni"))) f.family = FONT_SERIF;
    else if (strstr(fam, "Script")) f.family = FONT_SCRIPT;
    else if (strstr(fam, "Condensed") || strstr(fam, "Narrow")) f.family = FONT_CONDENSED;
    else if (strstr(fam, "Century Gothic") || strstr(fam, "Futura") || strstr(fam, "Avenir")) f.family = FONT_GEO;
    else if (strstr(fam, "Georgia") || (strstr(fam, "serif") && !strstr(fam, "sans-serif"))) f.family = FONT_SERIF;
    else f.family = FONT_SANS;
  }
  return f;
}

double font_ascent(const FontSpec *f) { return style_of(f).ascent * f->size; }
double font_descent(const FontSpec *f) { return style_of(f).descent * f->size; }

static const Glyph *glyph_for(char ch) {
  char u = (char)toupper((unsigned char)ch);   // caps-only font: lower case falls back to caps
  for (size_t i = 0; i < ARRAY_LEN(GLYPHS); i++)
    if (GLYPHS[i].ch == u) return &GLYPHS[i];
  return &GLYPHS[0];   // unknown: a space
}

// the next character: a byte (caps set) or a UTF-8 code point (cased)
static uint32_t next_char(const FontSpec *f, const char **p) {
  const unsigned char *s = (const unsigned char *)*p;
  uint32_t c = *s++;
  if (f->cased && c >= 0x80) {
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    c &= n == 3 ? 0x07 : n == 2 ? 0x0F : 0x1F;
    for (int i = 0; i < n && (*s & 0xC0) == 0x80; i++) c = c << 6 | (*s++ & 0x3F);
  }
  *p = (const char *)s;
  return c;
}
static Glyph glyph_of(const FontSpec *f, uint32_t c) {
  if (f->cased)
    for (size_t i = 0; i < ARRAY_LEN(UGLYPHS); i++)
      if (UGLYPHS[i].cp == c) return (Glyph){ '?', UGLYPHS[i].adv, UGLYPHS[i].path };
  return *glyph_for(c < 0x80 ? (char)c : ' ');
}
// the advance of a glyph (em, before the size) and where it starts inside it
static double glyph_adv(const Style *s, const Glyph *g) { return s->mono ? 0.6 : g->adv * s->sx; }
static double glyph_lead(const Style *s, const Glyph *g) { return s->mono ? (0.6 - g->adv * s->sx) / 2 : 0; }

double font_measure(const FontSpec *f, const char *text) {
  Style s = style_of(f);
  double w = 0;
  if (!f->cased) {   // (the bakes' arithmetic, unchanged)
    for (const char *p = text; *p; p++) w += glyph_for(*p)->adv * s.sx;
    return w * f->size;
  }
  for (const char *p = text; *p;) {
    Glyph g = glyph_of(f, next_char(f, &p));
    w += glyph_adv(&s, &g) * f->size + f->spacing;
  }
  return w;
}

typedef struct Emit {
  const FontSpec *f;
  Style s;
  double ox, oy;
  FontStrokeFn fn;
  void *user;
  V2 pts[512];
  int n;
} Emit;

static V2 to_canvas(const Emit *e, double gx, double gy) {
  return v2(e->ox + (gx * e->s.sx + gy * e->s.shear) * e->f->size, e->oy - gy * e->f->size);
}

static void flush(Emit *e, bool closed) {
  if (e->n > 0) e->fn(e->user, e->pts, e->n, closed, e->s.hw * e->f->size);
  // serifs: short horizontal bars where a stroke ends on the baseline or the cap line
  if (e->s.serif && e->n >= 2 && !closed) {
    for (int k = 0; k < 2; k++) {
      V2 end = e->pts[k ? e->n - 1 : 0], nb = e->pts[k ? e->n - 2 : 1];
      double gy = (e->oy - end.y) / e->f->size;
      bool vertical = fabs(end.x - nb.x) < fabs(end.y - nb.y) * 0.6;
      if (vertical && (fabs(gy) < 0.03 || fabs(gy - 0.7) < 0.03)) {
        double half = 0.12 * e->f->size;
        V2 bar[2] = { v2(end.x - half, end.y), v2(end.x + half, end.y) };
        e->fn(e->user, bar, 2, false, 0.045 * e->f->size);
      }
    }
  }
  e->n = 0;
}

static void push(Emit *e, V2 p) {
  CHECK(e->n < 512);
  e->pts[e->n++] = p;
}

static void glyph_emit(Emit *e, const Glyph *g) {
  const char *p = g->path;
  e->n = 0;
  while (*p) {
    while (*p == ' ') p++;
    if (!*p) break;
    char op = *p++;
    double v[6];
    int need = op == 'M' || op == 'L' ? 2 : op == 'A' ? 6 : 0;
    for (int i = 0; i < need; i++) {
      char *end;
      v[i] = strtod(p, &end);
      if (end == p) FATAL("glyph '%c': bad path", g->ch);
      p = end;
    }
    if (op == 'M') { flush(e, false); push(e, to_canvas(e, v[0], v[1])); }
    else if (op == 'L') push(e, to_canvas(e, v[0], v[1]));
    else if (op == 'A') {
      double a0 = v[4] * DEG2RAD, a1 = v[5] * DEG2RAD;
      int segs = imax(6, (int)ceil(fabs(a1 - a0) / (PI_D / 24)));
      for (int i = 0; i <= segs; i++) {
        double a = a0 + (a1 - a0) * i / segs;
        push(e, to_canvas(e, v[0] + v[2] * cos(a), v[1] + v[3] * sin(a)));
      }
    } else if (op == 'Z') flush(e, true);
    else FATAL("glyph '%c': unknown op '%c'", g->ch, op);
  }
  flush(e, false);
}

void font_emit(const FontSpec *f, const char *text, double x, double y, FontStrokeFn fn, void *user) {
  Emit e = { .f = f, .s = style_of(f), .ox = x, .oy = y, .fn = fn, .user = user };
  for (const char *p = text; *p;) {
    Glyph g = glyph_of(f, next_char(f, &p));
    double lead = glyph_lead(&e.s, &g) * f->size;
    e.ox += lead;
    glyph_emit(&e, &g);
    e.ox += glyph_adv(&e.s, &g) * f->size - lead + f->spacing;
  }
}
