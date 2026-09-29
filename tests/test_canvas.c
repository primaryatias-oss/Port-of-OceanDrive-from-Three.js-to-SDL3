// Draws tests/canvas_pattern.inc with the C canvas and compares it with Chromium's rendering
// of the same calls (tools/ref/canvas-ref.mjs -> tests/data/canvas-ref.bmp).
//   test_canvas [pattern] [reference.bmp] [out.bmp] [text.bmp]
#include <stdlib.h>

#include "canvas/canvas.h"
#include "gfx/gpu.h"

static char *read_all(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) FATAL("cannot open %s", path);
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *p = malloc((size_t)n + 1);
  CHECK(p && fread(p, 1, (size_t)n, f) == (size_t)n);
  p[n] = 0;
  fclose(f);
  return p;
}

// split a line into args (quoted strings kept whole)
static int split(char *line, char **argv, int max) {
  int n = 0;
  for (char *p = line; *p && n < max;) {
    while (*p == ' ') p++;
    if (!*p) break;
    if (*p == '"') {
      char *e = strchr(p + 1, '"');
      CHECK(e);
      *e = 0;
      argv[n++] = p + 1;
      p = e + 1;
    } else {
      argv[n++] = p;
      while (*p && *p != ' ') p++;
      if (*p) *p++ = 0;
    }
  }
  return n;
}

static void draw_pattern(Canvas *c, char *text) {
  Gradient g = {};
  for (char *line = strtok(text, "\n"); line; line = strtok(nullptr, "\n")) {
    if (line[0] == '/' || !line[0]) continue;
    char *a[12];
    int n = split(line, a, 12);
    if (!n) continue;
#define F(i) atof(a[i])
    const char *op = a[0];
    if (!strcmp(op, "fillColor")) cv_fill_color(c, a[1]);
    else if (!strcmp(op, "strokeColor")) cv_stroke_color(c, a[1]);
    else if (!strcmp(op, "fillRect")) cv_fill_rect(c, F(1), F(2), F(3), F(4));
    else if (!strcmp(op, "strokeRect")) cv_stroke_rect(c, F(1), F(2), F(3), F(4));
    else if (!strcmp(op, "lineWidth")) cv_line_width(c, F(1));
    else if (!strcmp(op, "lineJoin")) cv_line_join(c, !strcmp(a[1], "round") ? JOIN_ROUND : !strcmp(a[1], "bevel") ? JOIN_BEVEL : JOIN_MITER);
    else if (!strcmp(op, "lineCap")) cv_line_cap(c, !strcmp(a[1], "round") ? CAP_ROUND : !strcmp(a[1], "square") ? CAP_SQUARE : CAP_BUTT);
    else if (!strcmp(op, "beginPath")) cv_begin_path(c);
    else if (!strcmp(op, "moveTo")) cv_move_to(c, F(1), F(2));
    else if (!strcmp(op, "lineTo")) cv_line_to(c, F(1), F(2));
    else if (!strcmp(op, "closePath")) cv_close_path(c);
    else if (!strcmp(op, "fill")) cv_fill(c);
    else if (!strcmp(op, "stroke")) cv_stroke(c);
    else if (!strcmp(op, "clip")) cv_clip(c);
    else if (!strcmp(op, "arc")) cv_arc(c, F(1), F(2), F(3), F(4), F(5), !strcmp(a[6], "true"));
    else if (!strcmp(op, "ellipse")) cv_ellipse(c, F(1), F(2), F(3), F(4), F(5), F(6), F(7), !strcmp(a[8], "true"));
    else if (!strcmp(op, "bezierTo")) cv_bezier_to(c, F(1), F(2), F(3), F(4), F(5), F(6));
    else if (!strcmp(op, "rect")) cv_rect(c, F(1), F(2), F(3), F(4));
    else if (!strcmp(op, "linearGradient")) g = cv_linear_gradient(c, F(1), F(2), F(3), F(4));
    else if (!strcmp(op, "radialGradient")) g = cv_radial_gradient(c, F(1), F(2), F(3), F(4), F(5), F(6));
    else if (!strcmp(op, "stop")) grad_add_stop(&g, F(1), a[2]);
    else if (!strcmp(op, "fillGradient")) cv_fill_gradient(c, &g);
    else if (!strcmp(op, "save")) cv_save(c);
    else if (!strcmp(op, "restore")) cv_restore(c);
    else if (!strcmp(op, "translate")) cv_translate(c, F(1), F(2));
    else if (!strcmp(op, "scale")) cv_scale(c, F(1), F(2));
    else if (!strcmp(op, "filterBlur")) cv_filter_blur(c, F(1));
    else if (!strcmp(op, "putImageData")) {
      int x = atoi(a[1]), y = atoi(a[2]), w = atoi(a[3]), h = atoi(a[4]);
      uint8_t *d = malloc((size_t)w * h * 4);
      for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
          uint8_t *p = d + ((size_t)j * w + i) * 4;
          p[0] = (uint8_t)(i * 6); p[1] = (uint8_t)(j * 25); p[2] = 128; p[3] = 200;
        }
      cv_put_image_data(c, d, x, y, w, h);
      free(d);
    } else FATAL("pattern: unknown op %s", op);
#undef F
  }
}

static void save(const Canvas *c, const char *path) {
  int w = canvas_width(c), h = canvas_height(c);
  uint8_t *px = malloc((size_t)w * h * 4);
  cv_get_image_data(c, 0, 0, w, h, px);
  // flatten on white like the page background under an image with alpha (checks use opaque)
  save_bmp_rgba8(path, px, w, h);
  free(px);
}

int main(int argc, char **argv) {
  const char *pattern = argc > 1 ? argv[1] : "tests/canvas_pattern.inc";
  const char *out = argc > 3 ? argv[3] : "build/canvas_c.bmp";
  const char *text_out = argc > 4 ? argv[4] : "build/canvas_text.bmp";
  Canvas *c = canvas_new(512, 400);
  char *text = read_all(pattern);
  draw_pattern(c, text);
  save(c, out);
  printf("wrote %s\n", out);

  // text specimen (fonts are the port's own; eyeball only)
  Canvas *t = canvas_new(1024, 560);
  cv_fill_color(t, "#20302a");
  cv_fill_rect(t, 0, 0, 1024, 560);
  const char *fonts[] = { "bold 60px Arial, Helvetica, sans-serif", "bold 60px \"Century Gothic\", \"Futura\", sans-serif",
                          "bold 60px \"Bahnschrift SemiBold Condensed\", \"Arial Narrow\", sans-serif",
                          "italic bold 60px \"Segoe Script\", \"Brush Script MT\", sans-serif", "bold 60px Georgia, serif" };
  for (int i = 0; i < 5; i++) {
    cv_font(t, fonts[i]);
    cv_fill_color(t, "#efe6cc");
    cv_text_align(t, ALIGN_LEFT);
    cv_text_baseline(t, BASE_MIDDLE);
    cv_fill_text(t, "ABCDEFGHIJKLM 0123456789", 20, 50 + i * 105);
    cv_fill_text(t, "NOPQRSTUVWXYZ -.:&!?", 20, 100 + i * 105);
  }
  cv_font(t, "bold 90px \"Century Gothic\", sans-serif");
  cv_line_width(t, 4);
  cv_stroke_color(t, "#ff9fc4");
  cv_text_align(t, ALIGN_CENTER);
  cv_stroke_text(t, "MARISOL", 800, 470);
  save(t, text_out);
  printf("wrote %s\n", text_out);
  canvas_free(c);
  canvas_free(t);
  free(text);
  return 0;
}
