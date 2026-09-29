// The port's own vector fonts: one hand-drawn stroke skeleton per glyph (caps, digits,
// punctuation), rendered in styles standing in for the families the JS asks the browser for:
//   FONT_SANS       Arial / Helvetica / sans-serif, bold
//   FONT_GEO        "Century Gothic", "Futura", "Avenir" (geometric) bold
//   FONT_CONDENSED  "Bahnschrift SemiBold Condensed", "Arial Narrow"
//   FONT_SCRIPT     "Segoe Script", "Brush Script MT" (italic)
//   FONT_SERIF      Georgia / serif, bold
//   FONT_MONO       ui-monospace (fixed advance)
// The texture bakes use the caps-only set, byte by byte, as verified against the JS scene. The
// UI asks for the port-only keyword `cased` in its font strings: UTF-8 text with lower case,
// typographic punctuation, CSS weights (300 / 400 / 500) and letter spacing.
#pragma once

#include "math/vmath.h"

typedef enum FontFamily { FONT_SANS, FONT_GEO, FONT_CONDENSED, FONT_SCRIPT, FONT_SERIF, FONT_MONO } FontFamily;

typedef struct FontSpec {
  FontFamily family;
  double size;       // px (em)
  bool bold, italic;
  bool cased;        // the UI glyph set (see above)
  double weight;     // stroke width factor (cased: 300 -> 0.7, 400 -> 1, 500 -> 1.25)
  double spacing;    // letter spacing (px after every glyph, like ctx.letterSpacing)
} FontSpec;

// parses a CSS font shorthand: [italic] [bold] <N>px <family list>
FontSpec font_parse(const char *css);
double font_ascent(const FontSpec *f);    // px above the alphabetic baseline
double font_descent(const FontSpec *f);   // px below it
double font_measure(const FontSpec *f, const char *text);
// Emits each glyph stroke (canvas space: y down) with its half width, starting the text at
// (x, y) on the alphabetic baseline.
typedef void (*FontStrokeFn)(void *user, const V2 *pts, int n, bool closed, double half_width);
void font_emit(const FontSpec *f, const char *text, double x, double y, FontStrokeFn fn, void *user);
