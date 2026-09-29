// Compares the C math / geometry port against results dumped from three.js
// (tools/ref/geom-ref.mjs -> tests/data/geom-ref.txt). Indices must match exactly; floats may
// differ only by libm rounding (sin/cos/pow are not bit-identical to V8's).
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "geom/geometry.h"

typedef struct RefCase {
  char name[64];
  int nattr;
  struct { char name[32]; int size, count; double *v; } attr[8];
  int nidx;
  double *idx;
  int nnums;
  double *nums;
} RefCase;

static RefCase cases[128];
static int ncases;
static int failures;

static double *read_numbers(FILE *f, int n) {
  double *v = xmalloc((size_t)(n ? n : 1) * sizeof(double));
  for (int i = 0; i < n; i++)
    if (fscanf(f, "%lf", &v[i]) != 1) FATAL("reference file: expected %d numbers", n);
  return v;
}

static void load(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) FATAL("cannot open %s (run: node tools/ref/geom-ref.mjs > %s)", path, path);
  char word[64];
  RefCase *c = nullptr;
  while (fscanf(f, "%63s", word) == 1) {
    if (!strcmp(word, "case")) {
      CHECK(ncases < (int)ARRAY_LEN(cases));
      c = &cases[ncases++];
      *c = (RefCase){};
      CHECK(fscanf(f, "%63s", c->name) == 1);
    } else if (!strcmp(word, "attr")) {
      CHECK(c && c->nattr < 8);
      int i = c->nattr++;
      CHECK(fscanf(f, "%31s %d %d", c->attr[i].name, &c->attr[i].size, &c->attr[i].count) == 3);
      c->attr[i].v = read_numbers(f, c->attr[i].size * c->attr[i].count);
    } else if (!strcmp(word, "index")) {
      CHECK(c && fscanf(f, "%d", &c->nidx) == 1);
      c->idx = read_numbers(f, c->nidx);
    } else if (!strcmp(word, "nums")) {
      CHECK(c && fscanf(f, "%d", &c->nnums) == 1);
      c->nums = read_numbers(f, c->nnums);
    } else if (!strcmp(word, "end")) {
      c = nullptr;
    } else {
      FATAL("reference file: unexpected token '%s'", word);
    }
  }
  fclose(f);
}

static const RefCase *ref(const char *name) {
  for (int i = 0; i < ncases; i++)
    if (!strcmp(cases[i].name, name)) return &cases[i];
  FATAL("no reference case '%s'", name);
}

static bool close_enough(double a, double b, double tol) { return fabs(a - b) <= tol * fmax(1.0, fabs(b)); }

static void fail(const char *name, const char *fmt, ...) SDL_PRINTF_VARARG_FUNC(2);
static void fail(const char *name, const char *fmt, ...) {
  char msg[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  printf("FAIL %-20s %s\n", name, msg);
  failures++;
}

static void cmp_geo(const char *name, Geometry *g) {
  const RefCase *r = ref(name);
  double worst = 0;
  bool ok = true;
  if (g->nattr != r->nattr) { fail(name, "attribute count %d, three.js has %d", g->nattr, r->nattr); ok = false; }
  for (int i = 0; ok && i < r->nattr; i++) {
    const GeoAttr *a = &g->attr[i];
    if (strcmp(a->name, r->attr[i].name)) { fail(name, "attribute %d is '%s', three.js has '%s'", i, a->name, r->attr[i].name); ok = false; break; }
    if (a->size != r->attr[i].size || g->count != r->attr[i].count) {
      fail(name, "'%s': %dx%d, three.js has %dx%d", a->name, g->count, a->size, r->attr[i].count, r->attr[i].size);
      ok = false;
      break;
    }
    for (int k = 0; k < g->count * a->size; k++) {
      double d = fabs((double)a->data[k] - r->attr[i].v[k]) / fmax(1.0, fabs(r->attr[i].v[k]));
      if (d > worst) worst = d;
      if (!close_enough(a->data[k], r->attr[i].v[k], 2e-6)) {
        fail(name, "'%s'[%d] = %.9g, three.js has %.9g", a->name, k, (double)a->data[k], r->attr[i].v[k]);
        ok = false;
        break;
      }
    }
  }
  if (ok) {
    int n = g->index ? g->index_count : 0;
    if (n != r->nidx) { fail(name, "index count %d, three.js has %d", n, r->nidx); ok = false; }
    for (int k = 0; ok && k < n; k++)
      if ((double)g->index[k] != r->idx[k]) { fail(name, "index[%d] = %u, three.js has %.0f", k, g->index[k], r->idx[k]); ok = false; }
  }
  if (ok) printf("ok   %-20s %6d verts %6d idx  max rel err %.2g\n", name, g->count, g->index ? g->index_count : 0, worst);
  geo_free(g);
}

static void cmp_nums(const char *name, const double *v, int n, double tol) {
  const RefCase *r = ref(name);
  if (n != r->nnums) { fail(name, "%d numbers, three.js has %d", n, r->nnums); return; }
  double worst = 0;
  int wi = -1;
  for (int i = 0; i < n; i++) {
    double d = fabs(v[i] - r->nums[i]) / fmax(1.0, fabs(r->nums[i]));
    if (d > worst) { worst = d; wi = i; }
    if (!close_enough(v[i], r->nums[i], tol)) { fail(name, "[%d] = %.17g, three.js has %.17g", i, v[i], r->nums[i]); return; }
  }
  printf("ok   %-20s %6d numbers  max rel err %.2g", name, n, worst);
  if (wi >= 0) printf(" (at [%d]: %.17g vs %.17g)", wi, v[wi], r->nums[wi]);
  printf("\n");
}

static V3 *pts5(void) {
  static V3 p[5] = { { 0, 0, 0 }, { 0.3, 0.5, 0.1 }, { 0.8, 0.6, -0.2 }, { 1.2, 0.2, 0.3 }, { 1.5, -0.1, 0.2 } };
  return p;
}

static Shape holes_shape(void) {
  V2 q[4] = { { -1, 0 }, { 3, 0 }, { 3, 2.5 }, { -1, 2.5 } };
  Shape s = shape_from_points(q, 4);
  Path *h1 = shape_add_hole(&s);
  path_absarc(h1, 0.2, 1.2, 0.4, 0, PI_D * 2, true);
  Path *h2 = shape_add_hole(&s);
  path_move_to(h2, 1.2, 0.4); path_line_to(h2, 1.2, 2.0); path_line_to(h2, 2.4, 2.0); path_line_to(h2, 2.4, 0.4);
  return s;
}

int main(int argc, char **argv) {
  load(argc > 1 ? argv[1] : "tests/data/geom-ref.txt");

  cmp_geo("box", geo_box(1.3, 0.7, 2.1, 2, 3, 1));
  cmp_geo("plane", geo_plane(3, 2, 4, 3));
  cmp_geo("cylinder", geo_cylinder(0.2, 0.5, 1.5, 12, 2, false, 0, GEO_TAU));
  cmp_geo("cylinder_open_arc", geo_cylinder(0.3, 0.3, 1, 7, 1, true, 0.4, 2.5));
  cmp_geo("cone", geo_cone(0.4, 1.2, 9, 1, false, 0, GEO_TAU));
  cmp_geo("sphere", geo_sphere3(1.5, 16, 9));
  cmp_geo("sphere_part", geo_sphere(1, 10, 6, 0.3, 4, 0.2, 1.3));
  cmp_geo("torus", geo_torus(1, 0.25, 8, 20, GEO_TAU, 0, GEO_TAU));
  cmp_geo("torus_arc", geo_torus(0.8, 0.1, 6, 12, 3.5, 0, GEO_TAU));
  cmp_geo("circle", geo_circle(0.7, 11, 0, GEO_TAU));
  {
    V2 p[5] = { { 0, -0.5 }, { 0.3, -0.4 }, { 0.5, 0 }, { 0.2, 0.4 }, { 0, 0.5 } };
    cmp_geo("lathe", geo_lathe(p, 5, 14, 0, GEO_TAU));
  }
  cmp_geo("capsule", geo_capsule(0.3, 1.1, 4, 10, 2));
  cmp_geo("ico0", geo_icosahedron(0.5, 0));
  cmp_geo("ico2", geo_icosahedron(0.4, 2));
  cmp_geo("rbox", geo_rounded_box(1.62, 0.18, 0.56, 3, 0.06));
  cmp_geo("rbox2", geo_rounded_box(0.1, 0.36, 1.9, 2, 0.04));

  cmp_geo("box_ops", geo_scale(geo_translate(geo_rotate_z(geo_rotate_y(geo_rotate_x(geo_box1(1, 2, 3), 0.3), -1.1), 2.2), 1, 2, 3), 2, 0.5, 1.5));
  {
    Geometry *b = geo_box(1, 1, 1, 2, 1, 1);
    cmp_geo("box_nonidx", geo_to_non_indexed(b));
    geo_free(b);
  }
  cmp_geo("sphere_vnormals", geo_compute_vertex_normals(geo_sphere3(1, 8, 6)));
  {
    Geometry *s = geo_sphere3(1, 8, 6);
    cmp_geo("sphere_flat", geo_compute_vertex_normals(geo_to_non_indexed(s)));
    geo_free(s);
  }
  {
    Geometry *l[3] = { geo_box1(1, 1, 1), geo_translate(geo_cyl(0.2, 0.2, 1, 6), 2, 0, 0), geo_translate(geo_sphere3(0.5, 6, 4), 0, 3, 0) };
    cmp_geo("merge", geo_merge_free(l, 3));
  }
  {
    Geometry *ico = geo_delete_attr(geo_delete_attr(geo_icosahedron(0.4, 2), "normal"), "uv");
    cmp_geo("mergeverts", geo_merge_vertices(ico, 1e-4));
    geo_free(ico);
  }
  cmp_geo("applym4", geo_apply_m4(geo_box1(1, 1, 1), m4_compose(v3(1, -2, 0.5), quat_from_euler(euler(0.3, 1.2, -0.7, EULER_YXZ)), v3(1, -2, 0.5))));

  {
    CatmullRom3 c = curve_catmull(pts5(), 5, false, CURVE_CENTRIPETAL, 0.5);
    cmp_geo("tube_centripetal", geo_tube(&c, 16, 0.05, 6, false));
    V3 p[13];
    curve_points(&c, 12, p);
    double v[40];
    for (int i = 0; i < 13; i++) { v[i * 3] = p[i].x; v[i * 3 + 1] = p[i].y; v[i * 3 + 2] = p[i].z; }
    v[39] = curve_length(&c);
    cmp_nums("curve_points", v, 40, 1e-12);
    curve_free(&c);
    CatmullRom3 cc = curve_catmull(pts5(), 5, true, CURVE_CENTRIPETAL, 0.5);
    cmp_geo("tube_closed", geo_tube(&cc, 20, 0.03, 5, true));
    curve_free(&cc);
    CatmullRom3 cr = curve_catmull(pts5(), 5, false, CURVE_CATMULLROM, 0.3);
    cmp_geo("tube_catmull", geo_tube(&cr, 10, 0.1, 4, false));
    curve_free(&cr);
  }
  {
    Shape s = holes_shape();
    cmp_geo("shape_holes", geo_shape(&s, 16));
    Geometry *g = geo_shape(&s, 16);
    cmp_geo("shape_holes_ni", geo_to_non_indexed(g));
    geo_free(g);
    shape_free(&s);
  }
  {
    Shape sh = shape_new();
    double rc = 0.118;
    for (int i = 0; i <= 14; i++) {
      double a = PI_D / 2 + ((double)i / 14) * PI_D, x = rc * cos(a), y = rc * sin(a);
      if (i) path_line_to(&sh.path, x, y); else path_move_to(&sh.path, x, y);
    }
    path_line_to(&sh.path, 0.47, -0.005);
    for (int i = 0; i <= 8; i++) {
      double a = -PI_D / 2 + ((double)i / 8) * PI_D;
      path_line_to(&sh.path, 0.47 + 0.045 * cos(a), 0.04 + 0.045 * sin(a));
    }
    path_line_to(&sh.path, 0.0, rc);
    cmp_geo("shape_guard", geo_shape(&sh, 6));
    shape_free(&sh);
  }
  {
    Shape s = shape_new();
    double ex = 1.3, z0 = 4.1, cw = 3.1, cp = 1.9, rr = fmin(0.6, cp * 0.5);
    path_move_to(&s.path, ex - 0.05, -(z0 - cw / 2));
    path_line_to(&s.path, ex + cp - rr, -(z0 - cw / 2));
    path_absarc(&s.path, ex + cp - rr, -(z0 - cw / 2) - rr, rr, PI_D / 2, 0, true);
    path_line_to(&s.path, ex + cp, -(z0 + cw / 2) + rr);
    path_absarc(&s.path, ex + cp - rr, -(z0 + cw / 2) + rr, rr, 0, -PI_D / 2, true);
    path_line_to(&s.path, ex - 0.05, -(z0 + cw / 2));
    cmp_geo("extrude", geo_extrude(&s, (ExtrudeOptions){ .depth = 0.16, .curve_segments = 8 }));
    shape_free(&s);
  }
  {
    Shape s = shape_new();
    for (int i = 0; i < 120; i++) {
      double a = ((double)i / 120) * PI_D * 2, r = 1 + 0.3 * sin(a * 7) + 0.1 * cos(a * 13);
      if (i) path_line_to(&s.path, r * cos(a), r * sin(a)); else path_move_to(&s.path, r * cos(a), r * sin(a));
    }
    Path *h = shape_add_hole(&s);
    path_absarc(h, 0.1, -0.05, 0.25, 0, PI_D * 2, false);
    cmp_geo("shape_big", geo_shape(&s, 12));
    shape_free(&s);
  }

  {
    DVec r = {};
#define PUSH(...) do { double t_[] = { __VA_ARGS__ }; vec_append(&r, t_, ARRAY_LEN(t_)); } while (0)
    Quat q = quat_identity();
    for (int o = 0; o < 6; o++) {
      q = quat_from_euler(euler(0.4, -1.3, 2.1, (EulerOrder)o));
      PUSH(q.x, q.y, q.z, q.w);
      Euler e = euler_from_rotation_matrix(m4_from_quat(q), (EulerOrder)o);
      PUSH(e.x, e.y, e.z);
    }
    M4 m = m4_compose(v3(1, 2, 3), q, v3(-1.5, 0.5, 2));
    vec_append(&r, m.e, 16);
    M4 mi = m4_invert(m);
    vec_append(&r, mi.e, 16);
    V3 p, s;
    Quat qq;
    m4_decompose(m, &p, &qq, &s);
    PUSH(p.x, p.y, p.z, qq.x, qq.y, qq.z, qq.w, s.x, s.y, s.z);
    M4 la = m4_look_at(v3(1, 2, 3), v3(-2, 0.5, 1), v3(0, 1, 0));
    vec_append(&r, la.e, 16);
    M4 pr = m4_perspective(50, 16.0 / 9, 0.1, 30000, 1);
    vec_append(&r, pr.e, 16);
    M4 orr = m4_orthographic(-30, 40, 20, -15, 0.5, 700);
    vec_append(&r, orr.e, 16);
    Quat u = quat_unit_vectors(v3(0, 1, 0), v3_norm(v3(0.3, 0.2, -0.9)));
    PUSH(u.x, u.y, u.z, u.w);
    Quat sl = quat_slerp(quat_from_euler(euler(0.1, 0.2, 0.3, EULER_XYZ)), u, 0.37);
    PUSH(sl.x, sl.y, sl.z, sl.w);
    V3 v = v3_apply_quat(v3(0.3, -1.2, 2.5), u);
    PUSH(v.x, v.y, v.z);
    M4 ra = m4_rotation_axis(v3_norm(v3(0.2, 0.9, -0.3)), 1.1);
    vec_append(&r, ra.e, 16);
    M3 nm = m3_normal_matrix(m);
    vec_append(&r, nm.e, 9);
    uint32_t hexes[] = { 0x8d908c, 0xf4f2ec, 0x102030, 0xffffff, 0x0a0b0c };
    for (size_t i = 0; i < ARRAY_LEN(hexes); i++) {
      Color c = color_hex(hexes[i]);
      PUSH(c.r, c.g, c.b, (double)color_get_hex(c));
    }
    Color c2 = color_hsl(0.93, 0.6, 0.4);
    PUSH(c2.r, c2.g, c2.b);
    Color c3 = color_offset_hsl(color_hex(0xd6c6a0), 0.05, -0.1, 0.07);
    PUSH(c3.r, c3.g, c3.b, (double)color_get_hex(c3));
    double h, sa, l;
    color_get_hsl(color_hex(0x3a7fb2), &h, &sa, &l);
    PUSH(h, sa, l);
    cmp_nums("math", r.data, (int)r.len, 1e-12);
    vec_free(&r);
  }
  {
    DVec r = {};
    double seeds[] = { 1, 4242, 9121, 7 * 13 + 3, -5, 8589934592.0 + 17 };
    for (size_t s = 0; s < ARRAY_LEN(seeds); s++) {
      Rng g = rng_make(seeds[s]);
      for (int i = 0; i < 6; i++) vec_push(&r, rng_next(&g));
    }
    PUSH(js_round(2.5), js_round(-2.5), js_round(0.49999999999999994), js_round(-0.5), (double)js_i32(-7.9), (double)js_i32(3e9), (double)js_u32(-1));
    cmp_nums("rng", r.data, (int)r.len, 0);
    vec_free(&r);
  }

  printf(failures ? "\n%d FAILED\n" : "\nall geometry/math checks passed\n", failures);
  return failures ? 1 : 0;
}
