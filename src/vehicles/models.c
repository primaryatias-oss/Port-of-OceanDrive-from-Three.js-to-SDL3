// Port of src/vehicles/models.js.
#include "vehicles/models.h"

#include <math.h>
#include <string.h>

#include "canvas/canvas.h"
#include "gfx/three_mat.h"
#include "quality.h"
#include "textures/noise.h"

static int detail(void) { return QUALITY.tier == TIER_HIGH ? 2 : QUALITY.tier == TIER_MEDIUM ? 1 : 0; }
static int rad(void) { static const int R[3] = { 6, 8, 10 }; return R[detail()]; }   // tube radial segments

// ---------------------------------------------------------------------------------------------
// material + geometry helpers

static Material *vehicle_material(void) {
  MatDesc d = md_standard();
  d.name = "vehicle-mre";
  d.vertex_colors = true;
  d.roughness = 1;
  d.metalness = 0;
  d.prog[MV_PLAIN] = PROG_VEHICLE_MRE;
  Material *m = mat_three(&d);
  mat_set_float(m, "uLights", 0);
  mat_set_float(m, "uBeaconA", 0);
  mat_set_float(m, "uBeaconB", 0);
  return m;
}

typedef struct Parts { Vec(Geometry *) list; } Parts;

static Geometry *non_indexed(Geometry *g) {
  if (!g->index) return g;
  Geometry *n = geo_to_non_indexed(g);
  geo_free(g);
  return n;
}
static void keep_attrs(Geometry *g, const char *const *keep, int nkeep) {
  for (int i = g->nattr - 1; i >= 0; i--) {
    bool k = false;
    for (int j = 0; j < nkeep; j++) if (!strcmp(g->attr[i].name, keep[j])) k = true;
    if (!k) geo_delete_attr(g, g->attr[i].name);
  }
}
static void parts_add(Parts *P, Geometry *g, uint32_t hex, double metal, double rough, double emis) {
  g = non_indexed(g);
  static const char *const K[2] = { "position", "normal" };
  keep_attrs(g, K, 2);
  if (!geo_attr(g, "normal")) geo_compute_vertex_normals(g);
  int n = g->count;
  Color c = color_hex(hex);
  float *col = geo_set_attr(g, "color", 3, n), *mre = geo_set_attr(g, "aMRE", 3, n);
  for (int i = 0; i < n; i++) {
    col[i * 3] = (float)c.r; col[i * 3 + 1] = (float)c.g; col[i * 3 + 2] = (float)c.b;
    mre[i * 3] = (float)metal; mre[i * 3 + 1] = (float)rough; mre[i * 3 + 2] = (float)emis;
  }
  vec_push(&P->list, g);
}
// geometry that already carries per-vertex colour (tyres): keep it
static void parts_add_colored(Parts *P, Geometry *g, double metal, double rough) {
  g = non_indexed(g);
  int n = g->count;
  static const char *const K[3] = { "position", "normal", "color" };
  keep_attrs(g, K, 3);
  float *mre = geo_set_attr(g, "aMRE", 3, n);
  for (int i = 0; i < n; i++) { mre[i * 3] = (float)metal; mre[i * 3 + 1] = (float)rough; mre[i * 3 + 2] = 0; }
  vec_push(&P->list, g);
}
static Node *parts_mesh(Parts *P, Material *mat, const M4 *matrix) {
  Geometry *g = geo_merge(P->list.data, (int)P->list.len);
  for (size_t i = 0; i < P->list.len; i++) geo_free(P->list.data[i]);
  vec_free(&P->list);
  if (matrix) geo_apply_m4(g, *matrix);
  geo_compute_bsphere(g);
  Node *m = node_mesh(gpu_geometry(g), mat);
  geo_free(g);
  m->cast_shadow = true;
  m->receive_shadow = true;
  return m;
}

static Geometry *tube(const V3 *pts, int n, double r, int seg, bool closed, int radial) {
  CatmullRom3 c = curve_catmull(pts, n, closed, CURVE_CENTRIPETAL, 0.5);
  int s = seg ? seg : (int)fmax(4, js_round(curve_length(&c) / 0.03));
  Geometry *g = geo_tube(&c, s, r, radial, closed);
  curve_free(&c);
  return g;
}
#define TUBE(r, ...) tube((V3[]){ __VA_ARGS__ }, (int)(sizeof((V3[]){ __VA_ARGS__ }) / sizeof(V3)), (r), 0, false, rad())
#define TUBE_S(r, seg, ...) tube((V3[]){ __VA_ARGS__ }, (int)(sizeof((V3[]){ __VA_ARGS__ }) / sizeof(V3)), (r), (seg), false, rad())
#define P3(x, y, z) ((V3){ (x), (y), (z) })

static Geometry *rod_r(V3 A, V3 B, double r, int radial, double r2) {
  Geometry *g = geo_cylinder(r2, r, v3_dist(A, B), radial, 1, false, 0, GEO_TAU);
  geo_apply_m4(g, m4_from_quat(quat_unit_vectors(v3(0, 1, 0), v3_norm(v3_sub(B, A)))));
  return geo_translate(g, (A.x + B.x) / 2, (A.y + B.y) / 2, (A.z + B.z) / 2);
}
static Geometry *rod(V3 A, V3 B, double r) { return rod_r(A, B, r, rad(), r); }
static Geometry *rod_n(V3 A, V3 B, double r, int radial) { return rod_r(A, B, r, radial, r); }
static Geometry *box(V3 c, V3 s) { return geo_translate(geo_box1(s.x, s.y, s.z), c.x, c.y, c.z); }

static Geometry *helix(V3 c, double r, double h, int turns, double tr) {
  int n = turns * 12 + 1;
  V3 *pts = xmalloc((size_t)n * sizeof(V3));
  for (int i = 0; i < n; i++) {
    double a = ((double)i / 12) * PI_D * 2;
    pts[i] = v3(c.x + cos(a) * r, c.y + (h * i) / (turns * 12), c.z + sin(a) * r);
  }
  Geometry *g = tube(pts, n, tr, turns * 12, false, 5);
  free(pts);
  return g;
}

// rounded-rectangle loop in a horizontal plane (16 points)
static int rect_loop(double x0, double x1, double z0, double z1, double y, double rc, V3 *out) {
  const double corners[4][3] = { { x1 - rc, z0 + rc, -PI_D / 2 }, { x1 - rc, z1 - rc, 0 }, { x0 + rc, z1 - rc, PI_D / 2 }, { x0 + rc, z0 + rc, PI_D } };
  int n = 0;
  for (int k = 0; k < 4; k++)
    for (int j = 0; j <= 3; j++) {
      double a = corners[k][2] + ((double)j / 3) * (PI_D / 2);
      out[n++] = v3(corners[k][0] + cos(a) * rc, y, corners[k][1] + sin(a) * rc);
    }
  return n;
}

// grid sheet from rows of points (nr x nc), oriented so its normals face out(p), optionally
// doubled with a back face `thick` behind it
typedef V3 (*OutFn)(V3 p, const void *ctx);
static Geometry *sheet_mk(const double *pos, int np, const uint32_t *idx, int ni, bool flip) {
  Geometry *g = geo_new();
  geo_set_attr_d(g, "position", 3, np, pos);
  uint32_t *ix = xmalloc((size_t)ni * sizeof *ix);
  for (int i = 0; i < ni; i++) ix[i] = flip ? idx[i - (i % 3) + (2 - (i % 3))] : idx[i];
  geo_set_index(g, ix, ni);
  free(ix);
  geo_compute_vertex_normals(g);
  return g;
}
static Geometry *sheet(const V3 *rows, int nr, int nc, OutFn out, const void *ctx, double thick) {
  int np = nr * nc;
  double *pos = xmalloc((size_t)np * 3 * sizeof(double));
  for (int i = 0; i < np; i++) { pos[i * 3] = rows[i].x; pos[i * 3 + 1] = rows[i].y; pos[i * 3 + 2] = rows[i].z; }
  U32Vec idx = {};
  for (int i = 0; i < nr - 1; i++)
    for (int j = 0; j < nc - 1; j++) {
      uint32_t a = (uint32_t)(i * nc + j), b = a + 1, c = a + (uint32_t)nc, d = c + 1;
      uint32_t q[6] = { a, c, b, b, c, d };
      vec_append(&idx, q, 6);
    }
  Geometry *g = sheet_mk(pos, np, idx.data, (int)idx.len, false);
  int mid = (nr / 2) * nc + nc / 2;
  const float *gp = geo_data(g, "position"), *gn = geo_data(g, "normal");
  V3 P = v3(gp[mid * 3], gp[mid * 3 + 1], gp[mid * 3 + 2]), N = v3(gn[mid * 3], gn[mid * 3 + 1], gn[mid * 3 + 2]);
  bool flip = v3_dot(N, out(P, ctx)) < 0;
  if (flip) { geo_free(g); g = sheet_mk(pos, np, idx.data, (int)idx.len, true); }
  if (!thick) { free(pos); vec_free(&idx); return g; }
  Geometry *back = sheet_mk(pos, np, idx.data, (int)idx.len, !flip);
  float *bp = geo_data(back, "position");
  const float *bn = geo_data(g, "normal");
  for (int i = 0; i < np; i++)
    for (int k = 0; k < 3; k++) bp[i * 3 + k] = (float)(bp[i * 3 + k] - bn[i * 3 + k] * thick);
  geo_compute_vertex_normals(back);
  Geometry *list[2] = { g, back };
  Geometry *m = geo_merge(list, 2);
  geo_free(g);
  geo_free(back);
  free(pos);
  vec_free(&idx);
  return m;
}

// fender shell swept along an arc around a wheel (in the y-z plane). theta 0 = top, negative =
// forward. prof: [x, radialOffset] across the fender.
static V3 fender_out(V3 p, const void *ctx) { const double *c = ctx; return v3(0, p.y - c[1], p.z - c[2]); }
static Geometry *fender(const double *c, double ry, double rz, double a0, double a1, const double (*prof)[2], int np, int n) {
  V3 *rows = xmalloc((size_t)(n + 1) * np * sizeof(V3));
  for (int i = 0; i <= n; i++) {
    double a = a0 + ((a1 - a0) * i) / n, ca = cos(a), sa = sin(a);
    for (int k = 0; k < np; k++) rows[i * np + k] = v3(c[0] + prof[k][0], c[1] + (ry + prof[k][1]) * ca, c[2] + (rz + prof[k][1]) * sa);
  }
  Geometry *g = sheet(rows, n + 1, np, fender_out, c, 0.004);
  free(rows);
  return g;
}

// body loft: keys [z, halfWidth, bottom, top], sampled smoothly; ring mirrored about x = 0
static Geometry *loft_body(const double (*keys)[4], int nk, double step) {
  DVec zs = {};
  for (double z = keys[0][0]; z < keys[nk - 1][0] - 1e-6; z += step) vec_push(&zs, z);
  vec_push(&zs, keys[nk - 1][0]);
  int nz = (int)zs.len;
  enum { NH = 7, NR = 2 * NH - 1 };
  double (*rings)[NR][2] = xmalloc((size_t)nz * sizeof *rings);
  for (int k = 0; k < nz; k++) {
    double z = zs.data[k];
    int i = 0;
    while (i < nk - 2 && z > keys[i + 1][0]) i++;
    const double *A = keys[i], *B = keys[i + 1];
    double t = (z - A[0]) / (B[0] - A[0]), s = t * t * (3 - 2 * t);
    double w = A[1] + (B[1] - A[1]) * s, b = A[2] + (B[2] - A[2]) * s, tp = A[3] + (B[3] - A[3]) * s;
    const double P[NH][2] = { { 0, b }, { w - 0.05, b }, { w, b + 0.05 }, { w, tp - 0.07 }, { w - 0.025, tp - 0.02 }, { w - 0.09, tp }, { 0, tp + 0.01 } };
    int r = 0;
    for (int q = NH - 1; q > 0; q--) { rings[k][r][0] = -P[q][0]; rings[k][r][1] = P[q][1]; r++; }
    for (int q = 0; q < NH; q++) { rings[k][r][0] = P[q][0]; rings[k][r][1] = P[q][1]; r++; }
  }
  const int n = NR;
  DVec pos = {};
  U32Vec idx = {};
  for (int k = 0; k < nz; k++)
    for (int r = 0; r < n; r++) { double p[3] = { rings[k][r][0], rings[k][r][1], zs.data[k] }; vec_append(&pos, p, 3); }
  for (int k = 0; k < nz - 1; k++)
    for (int i = 0; i < n - 1; i++) {
      uint32_t a = (uint32_t)(k * n + i), b = a + (uint32_t)n;
      uint32_t q[6] = { a, b, a + 1, a + 1, b, b + 1 };
      vec_append(&idx, q, 6);
    }
  const int caps[2] = { 0, nz - 1 };
  for (int e = 0; e < 2; e++) {
    int k = caps[e];
    bool flip = e == 1;
    uint32_t c = (uint32_t)(pos.len / 3);
    double sy = 0;
    for (int r = 0; r < n; r++) sy += rings[k][r][1];
    double p[3] = { 0, sy / n, zs.data[k] };
    vec_append(&pos, p, 3);
    for (int i = 0; i < n - 1; i++) {
      uint32_t a = (uint32_t)(k * n + i);
      uint32_t q[3] = { c, flip ? a + 1 : a, flip ? a : a + 1 };
      vec_append(&idx, q, 3);
    }
  }
  Geometry *g = geo_new();
  geo_set_attr_d(g, "position", 3, (int)(pos.len / 3), pos.data);
  geo_set_index(g, idx.data, (int)idx.len);
  geo_compute_vertex_normals(g);
  // outward check on the widest ring
  int mid = (nz / 2) * n + (int)floor(n * 0.7);
  const float *gp = geo_data(g, "position"), *gn = geo_data(g, "normal");
  if (gn[mid * 3] * gp[mid * 3] + gn[mid * 3 + 1] * (gp[mid * 3 + 1] - 0.65) < 0) {
    for (int i = 0; i < g->index_count; i += 3) { uint32_t t = g->index[i + 1]; g->index[i + 1] = g->index[i + 2]; g->index[i + 2] = t; }
    geo_compute_vertex_normals(g);
  }
  vec_free(&pos);
  vec_free(&idx);
  vec_free(&zs);
  free(rings);
  return g;
}

// lathe around the x axle from [radius, axial] points, with a colour per vertex
typedef uint32_t (*ColorAt)(double r, double ax);
static Geometry *lathe_x(const double (*prof)[2], int n, int segs, ColorAt color_at) {
  V2 *pts = xmalloc((size_t)n * sizeof(V2));
  for (int i = 0; i < n; i++) pts[i] = v2(prof[i][0], prof[i][1]);
  Geometry *g = geo_rotate_z(geo_lathe(pts, n, segs, 0, GEO_TAU), PI_D / 2);
  free(pts);
  const float *p = geo_data(g, "position");
  float *col = geo_set_attr(g, "color", 3, g->count);
  for (int i = 0; i < g->count; i++) {
    Color c = color_hex(color_at(js_hypot2(p[i * 3 + 1], p[i * 3 + 2]), p[i * 3]));
    col[i * 3] = (float)c.r; col[i * 3 + 1] = (float)c.g; col[i * 3 + 2] = (float)c.b;
  }
  return g;
}

static Texture *blob_texture(void) {
  Canvas *cv = canvas_new(128, 128);
  Gradient gr = cv_radial_gradient(cv, 64, 64, 4, 64, 64, 62);
  grad_add_stop(&gr, 0, "rgba(0,0,0,0.62)");
  grad_add_stop(&gr, 0.5, "rgba(0,0,0,0.38)");
  grad_add_stop(&gr, 1, "rgba(0,0,0,0)");
  cv_fill_gradient(cv, &gr);
  cv_fill_rect(cv, 0, 0, 128, 128);
  Texture *t = canvas_texture(cv, true, WRAP_CLAMP, 1);
  canvas_free(cv);
  return t;
}
static Material *g_blob;
static Node *blob(double w, double l) {
  if (!g_blob) {
    MatDesc d = md_basic();
    d.name = "vehicle blob";
    d.map = blob_texture();
    d.transparent = true;
    d.depth_write = false;
    d.polygon_offset = true;
    d.po_factor = -4;
    d.po_units = -4;
    d.color = color_hex(0x2a1c14);
    d.prog[MV_PLAIN] = PROG_BASIC_MAP_TRANSP;
    g_blob = mat_three(&d);
  }
  Geometry *g = geo_rotate_x(geo_plane(w, l, 1, 1), -PI_D / 2);
  Node *m = node_mesh(gpu_geometry(g), g_blob);
  geo_free(g);
  m->render_order = 1;
  return m;
}

// a pivot whose local +y is the steering axis through `top`
static Node *steer_pivot(V3 top, V3 bottom, M4 *inv) {
  Node *pivot = node_new(NODE_GROUP, "pivot");
  pivot->position = top;
  node_set_quaternion(pivot, quat_unit_vectors(v3(0, 1, 0), v3_norm(v3_sub(top, bottom))));
  node_update_matrix(pivot);
  *inv = m4_invert(pivot->matrix);
  return pivot;
}

// ---------------------------------------------------------------------------------------------
// beach cruiser

enum : uint32_t {
  FRAME = 0xf0afbe, CREAM = 0xf4ecda, CHROME = 0xeeeeee, TYRE = 0x1c1b1a, WALL = 0xefe6d0,
  SADDLE = 0x5b3a26, GRIP = 0x6a4631, WICKER = 0xc9a268, WICKER2 = 0xb08650, DARK = 0x2a2b2d,
};

static double g_rc_bike, g_tr_bike;
static uint32_t bike_tyre_color(double r, double ax) {
  double Rc = g_rc_bike;
  return fabs(ax) > 0.03 && r > Rc - 0.03 && r < Rc + 0.012 ? WALL : TYRE;
}

static Parts bike_wheel(double R, double TR, bool rear) {
  Parts P = {};
  double Rc = R - TR, prof[23][2];
  for (int i = 0; i <= 22; i++) {
    double a = -2.15 + ((double)i / 22) * 4.3;
    prof[i][0] = Rc + TR * cos(a);
    prof[i][1] = 0.062 * sin(a);
  }
  g_rc_bike = Rc; g_tr_bike = TR;
  static const int SEGS[3] = { 32, 40, 56 };
  parts_add_colored(&P, lathe_x(prof, 23, SEGS[detail()], bike_tyre_color), 0, 0.85);
  double rimR = Rc - TR * 0.55;
  parts_add(&P, geo_rotate_z(geo_cylinder(rimR, rimR, 0.05, 48, 1, true, 0, GEO_TAU), PI_D / 2), CHROME, 1, 0.16, 0);
  for (int si = 0; si < 2; si++) {
    double s = si ? 1 : -1;
    parts_add(&P, geo_translate(geo_rotate_y(geo_torus(rimR, 0.006, 5, 48, GEO_TAU, 0, GEO_TAU), PI_D / 2), s * 0.025, 0, 0), CHROME, 1, 0.16, 0);
  }
  double hubR = rear ? 0.045 : 0.03;
  parts_add(&P, geo_rotate_z(geo_cyl(hubR, hubR, rear ? 0.12 : 0.1, 16), PI_D / 2), CHROME, 1, 0.2, 0);
  for (int si = 0; si < 2; si++) {
    double s = si ? 1 : -1;
    parts_add(&P, geo_translate(geo_rotate_z(geo_cyl(0.042, 0.042, 0.006, 16), PI_D / 2), s * 0.036, 0, 0), CHROME, 1, 0.2, 0);
  }
  parts_add(&P, geo_rotate_z(geo_cyl(0.009, 0.009, 0.16, 6), PI_D / 2), 0x9a9a9a, 1, 0.3, 0);
  if (rear) parts_add(&P, geo_translate(geo_rotate_z(geo_cyl(0.038, 0.038, 0.006, 18), PI_D / 2), 0.065, 0, 0), DARK, 0.7, 0.4, 0);
  static const int NS[3] = { 20, 28, 36 };
  int nS = NS[detail()];
  for (int i = 0; i < nS; i++) {
    double a = ((double)i / nS) * PI_D * 2, s = i % 2 ? 1 : -1, cr = ((i >> 1) % 2 ? 1 : -1) * 0.32;
    parts_add(&P, rod_n(P3(s * 0.036, 0.04 * cos(a), 0.04 * sin(a)), P3(s * 0.008, (rimR - 0.004) * cos(a + cr), (rimR - 0.004) * sin(a + cr)), 0.0019, 3),
              CHROME, 1, 0.25, 0);
  }
  return P;
}

static V3 basket_out(V3 p, const void *ctx) { const double *cz = ctx; return v3(p.x, 0, p.z - *cz); }
static V3 flag_out(V3 p, const void *ctx) { (void)p; (void)ctx; return v3(1, 0, 0); }

static const double FENDER_PROF[7][2] = { { -0.07, -0.032 }, { -0.05, -0.012 }, { -0.025, -0.002 }, { 0, 0 }, { 0.025, -0.002 }, { 0.05, -0.012 }, { 0.07, -0.032 } };

VehicleModel build_bike_model(void) {
  VehicleModel M = {};
  Material *mat = vehicle_material();
  const double R = 0.335, TR = 0.055;
  const double FA[3] = { 0, R, -0.7 }, RA[3] = { 0, R, 0.52 }, BB[3] = { 0, 0.29, 0 };
  V3 HT_T = v3(0, 0.93, -0.44), HT_B = v3(0, 0.735, -0.515);
  Node *root = node_new(NODE_GROUP, "vehicle"), *tilt = node_new(NODE_GROUP, "tilt");
  node_add(root, tilt);
  Parts F = {};
#define PINK(g) parts_add(&F, (g), FRAME, 0.15, 0.32, 0)
  // cantilever frame: swooping top tube into the seat stays, twin lower tube, down tube
  PINK(TUBE(0.021, P3(0, 0.75, -0.505), P3(0, 0.6, -0.45), P3(0, 0.42, -0.26), P3(0, 0.31, -0.06), P3(BB[0], BB[1], BB[2])));
  PINK(TUBE(0.02, P3(0, 0.905, -0.447), P3(0, 0.845, -0.22), P3(0, 0.795, 0.05), P3(0, 0.77, 0.2)));
  for (int si = 0; si < 2; si++) {
    double s = si ? 1 : -1;
    PINK(TUBE(0.013, P3(0, 0.797, 0.1), P3(s * 0.02, 0.75, 0.27), P3(s * 0.045, 0.57, 0.44), P3(s * 0.055, R + 0.01, 0.52)));
    PINK(rod(P3(s * 0.025, 0.29, 0.01), P3(s * 0.056, R, 0.52), 0.013));
    parts_add(&F, box(P3(s * 0.058, R, 0.52), P3(0.008, 0.05, 0.04)), 0xb9b9b9, 1, 0.3, 0);
  }
  PINK(TUBE(0.017, P3(0, 0.665, -0.475), P3(0, 0.61, -0.25), P3(0, 0.56, -0.03), P3(0, 0.5, 0.092)));
  PINK(rod(P3(BB[0], BB[1], BB[2]), P3(0, 0.87, 0.254), 0.02));
  PINK(rod(P3(0, 0.71, -0.525), P3(0, 0.955, -0.43), 0.026));
  {
    const V3 ps[2] = { P3(0, 0.71, -0.525), P3(0, 0.955, -0.43) };
    for (int i = 0; i < 2; i++) {
      V3 p = ps[i];
      parts_add(&F, rod_n(P3(p.x, p.y - 0.012, p.z - 0.005), P3(p.x, p.y + 0.012, p.z + 0.005), 0.031, 14), CHROME, 1, 0.18, 0);
    }
  }
  PINK(geo_translate(geo_rotate_z(geo_cyl(0.03, 0.03, 0.09, 14), PI_D / 2), BB[0], BB[1], BB[2]));
  // rear fender (cream) and its stays
  parts_add(&F, fender(RA, R + 0.04, R + 0.04, -0.35, 2.15, FENDER_PROF, 7, 24), CREAM, 0.1, 0.35, 0);
  for (int si = 0; si < 2; si++) {
    double s = si ? 1 : -1;
    parts_add(&F, rod_n(P3(s * 0.06, R, 0.52), P3(s * 0.062, R + 0.3 * cos(1.5), 0.52 + 0.33 * sin(1.5)), 0.004, 4), CHROME, 1, 0.2, 0);
  }
  parts_add(&F, geo_translate(geo_rotate_x(geo_rotate_x(geo_cyl(0.03, 0.03, 0.008, 16), PI_D / 2), -0.5), 0, R + 0.36 * cos(1.95), 0.52 + 0.36 * sin(1.95) + 0.01),
            0xc0302a, 0.1, 0.3, 0.3);   // tail reflector
  // chain guard: a cream teardrop plate over the chain, rolled edge
  {
    Shape sh = shape_new();
    double rc = 0.118;
    for (int i = 0; i <= 14; i++) {
      double a = PI_D / 2 + ((double)i / 14) * PI_D;
      double x = rc * cos(a), y = rc * sin(a);
      if (i) path_line_to(&sh.path, x, y);
      else path_move_to(&sh.path, x, y);
    }
    path_line_to(&sh.path, 0.47, -0.005);
    for (int i = 0; i <= 8; i++) {
      double a = -PI_D / 2 + ((double)i / 8) * PI_D;
      path_line_to(&sh.path, 0.47 + 0.045 * cos(a), 0.04 + 0.045 * sin(a));
    }
    path_line_to(&sh.path, 0.0, rc);
    Geometry *g = geo_shape(&sh, 6);
    M4 m4 = m4_identity();   // makeBasis((0,0,1), (0,1,0), (1,0,0)).setPosition(0.088, 0.29, 0)
    m4.e[0] = 0; m4.e[1] = 0; m4.e[2] = 1;
    m4.e[4] = 0; m4.e[5] = 1; m4.e[6] = 0;
    m4.e[8] = 1; m4.e[9] = 0; m4.e[10] = 0;
    m4.e[12] = 0.088; m4.e[13] = 0.29; m4.e[14] = 0;
    geo_apply_m4(g, m4);
    // a thin double-sided plate
    Geometry *flat = geo_new();
    geo_set_attr_copy(flat, "position", 3, g->count, geo_data(g, "position"));
    geo_set_index(flat, g->index, g->index_count);
    geo_compute_vertex_normals(flat);
    if (geo_data(flat, "normal")[0] < 0) {
      for (int i = 0; i < flat->index_count; i += 3) { uint32_t t = flat->index[i + 1]; flat->index[i + 1] = flat->index[i + 2]; flat->index[i + 2] = t; }
      geo_compute_vertex_normals(flat);
    }
    Geometry *backG = geo_clone(flat);
    for (int i = 0; i < backG->index_count; i += 3) { uint32_t t = backG->index[i + 1]; backG->index[i + 1] = backG->index[i + 2]; backG->index[i + 2] = t; }
    geo_translate(backG, -0.004, 0, 0);
    geo_compute_vertex_normals(backG);
    parts_add(&F, flat, CREAM, 0.1, 0.35, 0);
    parts_add(&F, backG, CREAM, 0.1, 0.35, 0);
    geo_free(g);
    V2Vec op = path_get_points(&sh.path, 4);
    V3 *outline = xmalloc(op.len * sizeof(V3));
    for (size_t i = 0; i < op.len; i++) outline[i] = v3(0.088, 0.29 + op.data[i].y, op.data[i].x);
    parts_add(&F, tube(outline, (int)op.len, 0.005, (int)op.len * 2, true, 5), CREAM, 0.1, 0.35, 0);
    free(outline);
    vec_free(&op);
    shape_free(&sh);
  }
  // chain (under the guard), coaster brake arm
  for (int si = 0; si < 2; si++) {
    double s = si ? -1 : 1;
    parts_add(&F, rod_n(P3(0.066, 0.29 + s * 0.1, 0), P3(0.066, R + s * 0.036, 0.52), 0.005, 4), DARK, 0.6, 0.5, 0);
  }
  parts_add(&F, rod_n(P3(-0.056, R, 0.5), P3(-0.034, 0.305, 0.25), 0.008, 5), DARK, 0.6, 0.5, 0);
  // seat post, sprung saddle
  parts_add(&F, rod(P3(0, 0.86, 0.25), P3(0, 0.955, 0.29), 0.013), CHROME, 1, 0.16, 0);
  {
    Geometry *g = geo_sphere3(1, 24, 12);
    float *p = geo_data(g, "position");
    for (int i = 0; i < g->count; i++) {
      double x = p[i * 3], y = p[i * 3 + 1], z = p[i * 3 + 2];
      double t = z <= -0.9 ? 0 : z >= 0.35 ? 1 : (z + 0.9) / (0.35 + 0.9);
      double f = 0.42 + 0.58 * (z <= -0.9 ? 0 : z >= 0.35 ? 1 : t * t * (3 - 2 * t));
      p[i * 3] = (float)(x * 0.135 * f);
      p[i * 3 + 1] = (float)(y * (y > 0 ? 0.05 : 0.03));
      p[i * 3 + 2] = (float)(z * 0.145);
    }
    geo_compute_vertex_normals(g);
    parts_add(&F, geo_translate(geo_rotate_x(g, 0.06), 0, 1.0, 0.32), SADDLE, 0, 0.5, 0);
    parts_add(&F, geo_translate(geo_scale(geo_cyl(0.1, 0.12, 0.012, 20), 1, 1, 1.05), 0, 0.975, 0.35), DARK, 0.5, 0.5, 0);
  }
  for (int si = 0; si < 2; si++) {
    double s = si ? 1 : -1;
    parts_add(&F, helix(P3(s * 0.075, 0.935, 0.41), 0.016, 0.042, 5, 0.0035), CHROME, 1, 0.2, 0);
    parts_add(&F, rod_n(P3(0, 0.955, 0.29), P3(s * 0.075, 0.935, 0.41), 0.005, 5), CHROME, 1, 0.2, 0);
  }
  parts_add(&F, rod_n(P3(0, 0.955, 0.29), P3(0, 0.985, 0.2), 0.006, 5), CHROME, 1, 0.2, 0);
#undef PINK
  Node *frame_mesh = parts_mesh(&F, mat, nullptr);
  node_add(tilt, frame_mesh);

  // steering: fork, stem, swept-back bars, grips, bell, front fender, wicker basket
  M4 inv;
  Node *pivot = steer_pivot(HT_T, HT_B, &inv);
  Node *steer = node_new(NODE_GROUP, "steer");
  node_add(pivot, steer);
  node_add(tilt, pivot);
  Parts S = {};
  parts_add(&S, box(P3(0, 0.712, -0.522), P3(0.13, 0.035, 0.05)), FRAME, 0.15, 0.32, 0);
  for (int si = 0; si < 2; si++) {
    double s = si ? 1 : -1;
    parts_add(&S, TUBE(0.013, P3(s * 0.05, 0.712, -0.522), P3(s * 0.052, 0.55, -0.586), P3(s * 0.053, 0.43, -0.648), P3(s * 0.054, R, -0.7)), FRAME, 0.15, 0.32, 0);
  }
  parts_add(&S, rod(P3(0, 0.93, -0.44), P3(0, 1.0, -0.413), 0.012), CHROME, 1, 0.16, 0);
  parts_add(&S, rod(P3(0, 1.0, -0.413), P3(0, 1.005, -0.47), 0.013), CHROME, 1, 0.16, 0);
  parts_add(&S, TUBE_S(0.011, 80, P3(-0.345, 1.1, -0.1), P3(-0.33, 1.096, -0.2), P3(-0.27, 1.075, -0.33), P3(-0.16, 1.035, -0.44), P3(-0.05, 1.008, -0.47),
                       P3(0.05, 1.008, -0.47), P3(0.16, 1.035, -0.44), P3(0.27, 1.075, -0.33), P3(0.33, 1.096, -0.2), P3(0.345, 1.1, -0.1)), CHROME, 1, 0.14, 0);
  for (int si = 0; si < 2; si++) {
    double s = si ? 1 : -1;
    parts_add(&S, rod_n(P3(s * 0.346, 1.1, -0.085), P3(s * 0.334, 1.097, -0.21), 0.018, 12), GRIP, 0, 0.7, 0);
    parts_add(&S, geo_translate(geo_scale(geo_sphere3(0.019, 10, 6), 1, 1, 0.5), s * 0.346, 1.1, -0.082), GRIP, 0, 0.7, 0);
  }
  parts_add(&S, geo_translate(geo_sphere(0.03, 14, 8, 0, PI_D * 2, 0, PI_D / 2), -0.2, 1.06, -0.42), CHROME, 1, 0.12, 0);
  parts_add(&S, fender(FA, R + 0.04, R + 0.04, -1.05, 1.85, FENDER_PROF, 7, 24), CREAM, 0.1, 0.35, 0);
  for (int si = 0; si < 2; si++) {
    double s = si ? 1 : -1;
    parts_add(&S, rod_n(P3(s * 0.057, R, -0.7), P3(s * 0.06, R + 0.31 * cos(-0.9), -0.7 + 0.33 * sin(-0.9)), 0.004, 4), CHROME, 1, 0.2, 0);
  }
  {
    // wicker basket: tapered woven walls, a thick rolled rim, stays to the axle and the bars
    const double y0 = 0.8, y1 = 1.03, z0 = -0.665, z1 = -0.945;
    double cz = (z0 + z1) / 2;
#define HW(y) (0.175 + ((y) - y0) * 0.12)
#define HD(y) ((z0 - z1) / 2 + ((y) - y0) * 0.08)
    V3 wall[7 * 16];
    for (int k = 0; k <= 6; k++) {
      double y = y0 + ((y1 - y0) * k) / 6;
      rect_loop(-HW(y), HW(y), cz - HD(y), cz + HD(y), y, 0.035, &wall[k * 16]);
    }
    parts_add(&S, sheet(wall, 7, 16, basket_out, &cz, 0.004), WICKER2, 0, 0.85, 0);
    const int nW = 9;
    for (int k = 0; k < nW; k++) {
      double y = y0 + 0.012 + ((y1 - y0 - 0.02) * k) / (nW - 1), e = 0.004;
      V3 lp[16];
      rect_loop(-HW(y) - e, HW(y) + e, cz - HD(y) - e, cz + HD(y) + e, y, 0.035, lp);
      parts_add(&S, tube(lp, 16, 0.0085, 64, true, 5), k % 2 ? WICKER : 0xd6b27a, 0, 0.85, 0);
    }
    V3 rim[16];
    rect_loop(-HW(y1) - 0.006, HW(y1) + 0.006, cz - HD(y1) - 0.006, cz + HD(y1) + 0.006, y1 + 0.006, 0.04, rim);
    parts_add(&S, tube(rim, 16, 0.013, 64, true, 6), WICKER2, 0, 0.8, 0);
    parts_add(&S, box(P3(0, y0 + 0.004, cz), P3(2 * HW(y0), 0.008, 2 * HD(y0))), WICKER2, 0, 0.9, 0);
    for (int si = 0; si < 2; si++) {
      double s = si ? 1 : -1;
      parts_add(&S, rod_n(P3(s * 0.13, y0, cz + 0.05), P3(s * 0.058, R + 0.01, -0.7), 0.0045, 5), CHROME, 1, 0.2, 0);
      parts_add(&S, rod_n(P3(s * 0.1, y1 - 0.02, z0 + 0.01), P3(s * 0.1, 1.03, -0.45), 0.005, 5), CHROME, 1, 0.2, 0);
    }
#undef HW
#undef HD
  }
  Node *steer_mesh = parts_mesh(&S, mat, &inv);
  node_add(steer, steer_mesh);

  // wheels (the front one rides in the steering frame)
  Parts wf = bike_wheel(R, TR, false), wr = bike_wheel(R, TR, true);
  Node *wheel_f = parts_mesh(&wf, mat, nullptr), *wheel_r = parts_mesh(&wr, mat, nullptr);
  Node *f_hold = node_new(NODE_GROUP, "hold");
  f_hold->position = v3_apply_m4(v3(FA[0], FA[1], FA[2]), inv);
  node_set_quaternion(f_hold, quat_conj(pivot->quaternion));
  node_add(f_hold, wheel_f);
  node_add(steer, f_hold);
  wheel_r->position = v3(RA[0], RA[1], RA[2]);
  node_add(tilt, wheel_r);

  // cranks and pedals (pedals stay level)
  Node *crank = node_new(NODE_GROUP, "crank");
  crank->position = v3(BB[0], BB[1], BB[2]);
  node_add(tilt, crank);
  Parts C = {};
  parts_add(&C, geo_translate(geo_rotate_z(geo_cyl(0.1, 0.1, 0.006, 32), PI_D / 2), 0.066, 0, 0), 0x8e9093, 1, 0.3, 0);
  parts_add(&C, geo_translate(geo_rotate_z(geo_cyl(0.07, 0.07, 0.008, 24), PI_D / 2), 0.067, 0, 0), FRAME, 0.15, 0.35, 0);
  parts_add(&C, rod(P3(0.08, 0, 0), P3(0.086, -0.165, 0), 0.011), CHROME, 1, 0.18, 0);
  parts_add(&C, rod(P3(-0.08, 0, 0), P3(-0.086, 0.165, 0), 0.011), CHROME, 1, 0.18, 0);
  parts_add(&C, geo_rotate_z(geo_cyl(0.012, 0.012, 0.17, 8), PI_D / 2), CHROME, 1, 0.2, 0);
  node_add(crank, parts_mesh(&C, mat, nullptr));
  for (int si = 0; si < 2; si++) {
    double s = si ? 1 : -1;
    Node *hold = node_new(NODE_GROUP, "pedal");
    hold->position = v3(s * 0.09, s * -0.165, 0);
    Parts Pp = {};
    parts_add(&Pp, box(P3(s * 0.06, 0, 0), P3(0.095, 0.022, 0.068)), 0x222222, 0, 0.75, 0);
    static const double ZS[2] = { -0.036, 0.036 };
    for (int zi = 0; zi < 2; zi++) parts_add(&Pp, box(P3(s * 0.06, 0, ZS[zi]), P3(0.1, 0.026, 0.006)), CHROME, 1, 0.25, 0);
    parts_add(&Pp, geo_translate(box(P3(s * 0.06, 0, 0), P3(0.012, 0.008, 0.07)), s * 0.05, 0, 0), 0xffb13a, 0, 0.4, 0.25);   // amber reflector
    node_add(hold, parts_mesh(&Pp, mat, nullptr));
    node_add(crank, hold);
    M.pedals[si] = hold;
  }

  // kickstand (left, behind the bottom bracket)
  Node *kick = node_new(NODE_GROUP, "kick");
  kick->position = v3(-0.05, 0.3, 0.12);
  Parts K = {};
  parts_add(&K, rod_n(P3(0, 0, 0), P3(0, -0.315, 0), 0.009, 6), CHROME, 1, 0.22, 0);
  parts_add(&K, geo_translate(geo_scale(geo_sphere3(0.014, 8, 6), 1.4, 0.5, 1.4), 0, -0.318, 0), 0x333333, 0, 0.8, 0);
  node_add(kick, parts_mesh(&K, mat, nullptr));
  node_add(tilt, kick);

  M.root = root; M.tilt = tilt; M.steer = steer; M.wheel_f = wheel_f; M.wheel_r = wheel_r; M.crank = crank; M.kick = kick;
  M.shadow = blob(0.85, 1.9);
  M.mat = mat;
  M.R = R;
  return M;
}

// ---------------------------------------------------------------------------------------------
// lifeguard ATV

enum : uint32_t { RED = 0xc4231d, BLACK = 0x1c1d1f, TUBEC = 0x232527, GREY = 0x7c8086, SEAT = 0x1a1b1d };

static uint32_t black_tyre(double r, double ax) { (void)r; (void)ax; return 0x1b1b1b; }

static Parts knobby_wheel(double R, double w, double side) {
  (void)R;
  Parts P = {};
  double hw = w / 2;
  const double prof[13][2] = { { 0.19, -hw * 0.85 }, { 0.215, -hw * 0.98 }, { 0.245, -hw }, { 0.272, -hw * 0.96 }, { 0.286, -hw * 0.8 }, { 0.292, -hw * 0.45 },
                               { 0.293, 0 }, { 0.292, hw * 0.45 }, { 0.286, hw * 0.8 }, { 0.272, hw * 0.96 }, { 0.245, hw }, { 0.215, hw * 0.98 }, { 0.19, hw * 0.85 } };
  static const int SEGS[3] = { 28, 36, 48 };
  parts_add_colored(&P, lathe_x(prof, 13, SEGS[detail()], black_tyre), 0, 0.9);
  static const int NK[3] = { 14, 18, 22 };
  int nK = NK[detail()];
  double rT = 0.293;
  for (int i = 0; i < nK; i++) {
    double a = ((double)i / nK) * PI_D * 2, a2 = a + PI_D / nK;
    const double K[2][3] = { { -hw * 0.22, a, 0.05 }, { hw * 0.22, a2, 0.05 } };
    for (int k = 0; k < 2; k++) parts_add(&P, geo_rotate_x(box(P3(K[k][0], rT + 0.011, 0), P3(K[k][2], 0.022, 0.048)), K[k][1]), 0x1e1e1e, 0, 0.9, 0);
    for (int si = 0; si < 2; si++) {
      double s = si ? 1 : -1;
      parts_add(&P, geo_rotate_x(geo_rotate_y(box(P3(s * hw * 0.8, 0.283 + 0.008, 0), P3(0.05, 0.02, 0.042)), s * 0.25), s > 0 ? a : a2), 0x1e1e1e, 0, 0.9, 0);
    }
  }
  // dished steel wheel, lug nuts on the outer face
  parts_add(&P, geo_rotate_z(geo_cylinder(0.192, 0.192, w * 0.86, 28, 1, true, 0, GEO_TAU), PI_D / 2), GREY, 0.55, 0.45, 0);
  double fx = side * w * 0.2;
  parts_add(&P, geo_translate(geo_rotate_z(geo_cyl(0.19, 0.19, 0.012, 28), PI_D / 2), fx, 0, 0), GREY, 0.55, 0.45, 0);
  parts_add(&P, geo_translate(geo_rotate_y(geo_torus(0.19, 0.012, 6, 28, GEO_TAU, 0, GEO_TAU), PI_D / 2), side * w * 0.43, 0, 0), GREY, 0.55, 0.4, 0);
  parts_add(&P, geo_translate(geo_rotate_z(geo_cyl(0.055, 0.06, 0.04, 16), side * PI_D / 2), fx + side * 0.02, 0, 0), 0x9ea2a7, 0.8, 0.3, 0);
  for (int k = 0; k < 4; k++) {
    double a = ((double)k / 4) * PI_D * 2 + 0.4;
    parts_add(&P, geo_translate(geo_rotate_z(geo_cyl(0.011, 0.011, 0.02, 6), PI_D / 2), fx + side * 0.012, cos(a) * 0.085, sin(a) * 0.085), 0xb9bcc0, 0.9, 0.3, 0);
  }
  for (int k = 0; k < 6; k++) {
    double a = ((double)k / 6) * PI_D * 2;
    parts_add(&P, geo_translate(geo_rotate_z(geo_cyl(0.022, 0.022, 0.014, 10), PI_D / 2), fx, cos(a) * 0.14, sin(a) * 0.14), 0x5f6368, 0.5, 0.5, 0);   // vent holes (dark)
  }
  return P;
}

static void rack(Parts *B, double x0, double x1, double z0, double z1, double y) {
  V3 lp[16];
  rect_loop(x0, x1, z0, z1, y, 0.05, lp);
  parts_add(B, tube(lp, 16, 0.014, 60, true, rad()), TUBEC, 0.5, 0.45, 0);
  for (int k = 1; k < 4; k++) { double x = x0 + ((x1 - x0) * k) / 4; parts_add(B, rod_n(P3(x, y, z0 + 0.02), P3(x, y, z1 - 0.02), 0.009, 6), TUBEC, 0.5, 0.45, 0); }
  for (int k = 1; k < 3; k++) { double z = z0 + ((z1 - z0) * k) / 3; parts_add(B, rod_n(P3(x0 + 0.02, y, z), P3(x1 - 0.02, y, z), 0.009, 6), TUBEC, 0.5, 0.45, 0); }
}

VehicleModel build_atv_model(void) {
  VehicleModel M = {};
  Material *mat = vehicle_material();
  const double R = 0.31;
  Node *root = node_new(NODE_GROUP, "vehicle"), *tilt = node_new(NODE_GROUP, "tilt");
  node_add(root, tilt);
  Parts B = {};
#define REDP(g) parts_add(&B, (g), RED, 0.05, 0.34, 0)
#define TUB(g) parts_add(&B, (g), TUBEC, 0.5, 0.45, 0)
  // body
  static const double BODY[10][4] = { { -1.0, 0.24, 0.5, 0.62 }, { -0.92, 0.29, 0.46, 0.74 }, { -0.7, 0.3, 0.45, 0.8 }, { -0.5, 0.27, 0.45, 0.86 },
    { -0.3, 0.22, 0.45, 0.9 }, { -0.1, 0.18, 0.42, 0.84 }, { 0.3, 0.2, 0.42, 0.84 }, { 0.55, 0.3, 0.45, 0.8 }, { 0.9, 0.3, 0.46, 0.77 }, { 1.0, 0.26, 0.5, 0.64 } };
  REDP(loft_body(BODY, 10, 0.04));
  // fenders: flat-topped arcs over each wheel, the outer lip rolled down
  static const double PROF0[6][2] = { { 0.2, -0.01 }, { 0.27, 0 }, { 0.4, 0.012 }, { 0.52, 0.004 }, { 0.6, -0.028 }, { 0.628, -0.1 } };
  for (int si = 0; si < 2; si++) {
    double s = si ? 1 : -1;
    double pf[6][2], pr[6][2];
    for (int k = 0; k < 6; k++) {
      pf[k][0] = s * PROF0[k][0] - s * 0.49; pf[k][1] = PROF0[k][1];
      pr[k][0] = pf[k][0] + s * 0.02; pr[k][1] = PROF0[k][1];
    }
    REDP(fender((double[]){ s * 0.49, R, -0.625 }, 0.39, 0.44, -1.25, 1.45, pf, 6, 28));
    REDP(fender((double[]){ s * 0.47, R, 0.625 }, 0.39, 0.44, -1.45, 1.3, pr, 6, 28));
    // floorboards: ribbed black plates with a raised outer lip
    parts_add(&B, box(P3(s * 0.4, 0.345, 0), P3(0.4, 0.03, 0.5)), BLACK, 0, 0.8, 0);
    for (double z = -0.21; z <= 0.21; z += 0.06) parts_add(&B, box(P3(s * 0.4, 0.365, z), P3(0.36, 0.012, 0.018)), 0x2a2b2d, 0, 0.8, 0);
    parts_add(&B, box(P3(s * 0.595, 0.385, 0), P3(0.025, 0.07, 0.5)), BLACK, 0, 0.8, 0);
    // front A-arms and shocks (red springs), rear shocks
    static const double YS[2] = { 0.29, 0.42 };
    for (int yi = 0; yi < 2; yi++) {
      double y = YS[yi];
      TUB(TUBE_S(0.013, 12, P3(s * 0.12, y, -0.52), P3(s * 0.36, y, -0.625), P3(s * 0.12, y, -0.72)));
    }
    parts_add(&B, rod(P3(s * 0.31, 0.33, -0.6), P3(s * 0.18, 0.68, -0.6), 0.012), 0x9fa3a8, 0.8, 0.3, 0);
    parts_add(&B, geo_translate(geo_rotate_z(helix(P3(0, 0, 0), 0.028, 0.2, 7, 0.006), s * 0.36), s * 0.285, 0.38, -0.6), 0xd23a2a, 0.3, 0.4, 0);
    parts_add(&B, rod(P3(s * 0.26, R + 0.02, 0.6), P3(s * 0.18, 0.7, 0.45), 0.013), 0x9fa3a8, 0.8, 0.3, 0);
    // headlights in the front fascia
    parts_add(&B, geo_translate(geo_scale(geo_sphere3(0.055, 16, 10), 1.2, 0.8, 0.45), s * 0.15, 0.6, -0.995), 0xfff2dc, 0.2, 0.1, 0.6);
    parts_add(&B, geo_translate(geo_scale(geo_torus(0.058, 0.009, 6, 18, GEO_TAU, 0, GEO_TAU), 1.2, 0.8, 1), s * 0.15, 0.6, -0.99), 0x2a2a2a, 0.6, 0.4, 0);
  }
  parts_add(&B, box(P3(0, 0.52, -1.0), P3(0.16, 0.08, 0.02)), BLACK, 0, 0.7, 0);   // grille
  // seat, tank cap, engine, exhaust
  parts_add(&B, geo_translate(geo_rounded_box(0.36, 0.13, 0.76, 3, 0.05), 0, 0.905, 0.25), SEAT, 0, 0.55, 0);
  parts_add(&B, geo_translate(geo_cyl(0.035, 0.035, 0.02, 16), 0, 0.905, -0.27), 0xb0b3b7, 0.9, 0.25, 0);
  parts_add(&B, box(P3(0, 0.36, 0.02), P3(0.34, 0.26, 0.5)), 0x3a3c3f, 0.4, 0.6, 0);
  parts_add(&B, geo_translate(geo_cyl(0.085, 0.085, 0.16, 14), 0, 0.48, -0.18), 0x55585c, 0.6, 0.5, 0);
  for (int k = 0; k < 5; k++) parts_add(&B, geo_translate(geo_cyl(0.1, 0.1, 0.008, 14), 0, 0.42 + k * 0.03, -0.18), 0x6a6d71, 0.6, 0.5, 0);
  parts_add(&B, TUBE_S(0.024, 30, P3(0.08, 0.5, -0.26), P3(0.25, 0.52, -0.1), P3(0.3, 0.58, 0.3), P3(0.3, 0.63, 0.46)), 0xa4a7ab, 0.8, 0.35, 0);
  parts_add(&B, geo_translate(geo_rotate_x(geo_cyl(0.062, 0.062, 0.36, 16), PI_D / 2), 0.3, 0.64, 0.64), 0x3b3d40, 0.6, 0.45, 0);
  parts_add(&B, geo_translate(geo_rotate_x(geo_cyl(0.022, 0.022, 0.05, 10), PI_D / 2), 0.3, 0.64, 0.845), 0xd4d6d9, 1, 0.2, 0);
  parts_add(&B, geo_translate(geo_rotate_z(geo_cyl(0.035, 0.035, 1.0, 12), PI_D / 2), 0, R, 0.625), 0x2c2e30, 0.5, 0.5, 0);   // rear axle
  // front and rear racks, brush guard
  rack(&B, -0.45, 0.45, -1.06, -0.62, 0.86);
  for (int si = 0; si < 2; si++) {
    double s = si ? 1 : -1;
    static const double ZS[2] = { -1.0, -0.66 };
    for (int zi = 0; zi < 2; zi++) { double z = ZS[zi]; TUB(rod_n(P3(s * 0.38, 0.86, z), P3(s * 0.36, z < -0.8 ? 0.62 : 0.72, z), 0.011, 6)); }
  }
  rack(&B, -0.48, 0.48, 0.42, 1.04, 0.86);
  for (int si = 0; si < 2; si++) {
    double s = si ? 1 : -1;
    static const double ZS[2] = { 0.46, 1.0 };
    for (int zi = 0; zi < 2; zi++) { double z = ZS[zi]; TUB(rod_n(P3(s * 0.42, 0.86, z), P3(s * 0.36, z > 0.8 ? 0.64 : 0.74, z), 0.011, 6)); }
  }
  TUB(TUBE_S(0.019, 40, P3(-0.32, 0.45, -1.02), P3(-0.33, 0.64, -1.1), P3(-0.22, 0.72, -1.13), P3(0.22, 0.72, -1.13), P3(0.33, 0.64, -1.1), P3(0.32, 0.45, -1.02)));
  TUB(rod(P3(-0.31, 0.55, -1.07), P3(0.31, 0.55, -1.07), 0.014));
  for (int si = 0; si < 2; si++) { double s = si ? 1 : -1; TUB(rod(P3(s * 0.2, 0.72, -1.12), P3(s * 0.2, 0.47, -1.02), 0.012)); }
  // light bar hoop over the rear rack, beacons, whip flag
  TUB(TUBE_S(0.02, 50, P3(-0.44, 0.86, 0.47), P3(-0.435, 1.28, 0.5), P3(-0.39, 1.41, 0.5), P3(0.39, 1.41, 0.5), P3(0.435, 1.28, 0.5), P3(0.44, 0.86, 0.47)));
  parts_add(&B, geo_translate(geo_rounded_box(0.76, 0.07, 0.11, 2, 0.02), 0, 1.465, 0.5), BLACK, 0.2, 0.5, 0);
  for (int k = 0; k < 6; k++) {
    double x = -0.3 + k * 0.12;
    bool left = k < 3;
    uint32_t col = k % 2 ? 0xff3a28 : 0xffac30;
    static const double ZS[2] = { 0.444, 0.556 };
    for (int zi = 0; zi < 2; zi++) parts_add(&B, box(P3(x, 1.47, ZS[zi]), P3(0.1, 0.045, 0.006)), col, 0, 0.25, left ? 0.8 : 0.9);
  }
  TUB(rod_n(P3(0.44, 0.86, 1.0), P3(0.47, 2.35, 1.06), 0.005, 5));
  {
    const V3 rows[4] = { P3(0.47, 2.34, 1.06), P3(0.47, 2.18, 1.06), P3(0.47, 2.29, 1.3), P3(0.47, 2.27, 1.3) };
    parts_add(&B, sheet(rows, 2, 2, flag_out, nullptr, 0.003), 0xff6a1a, 0, 0.8, 0);
  }
  // rescue can strapped across the rear rack, a white kit box
  parts_add(&B, geo_translate(geo_rotate_z(geo_capsule(0.075, 0.42, 4, 14, 1), PI_D / 2), 0.02, 0.945, 0.9), 0xd8261c, 0.05, 0.35, 0);
  static const double CAN_X[2] = { -0.12, 0.16 };
  for (int i = 0; i < 2; i++) parts_add(&B, geo_translate(geo_rotate_y(geo_torus(0.078, 0.007, 5, 20, GEO_TAU, 0, GEO_TAU), PI_D / 2), CAN_X[i], 0.945, 0.9), 0x111111, 0, 0.7, 0);
  parts_add(&B, geo_translate(geo_rounded_box(0.36, 0.2, 0.24, 2, 0.02), -0.02, 0.965, 0.62), 0xeeebe4, 0, 0.5, 0);
  parts_add(&B, box(P3(-0.02, 1.0, 0.62), P3(0.365, 0.008, 0.245)), 0xb8b6b0, 0, 0.5, 0);
#undef REDP
#undef TUB
  Node *body = parts_mesh(&B, mat, nullptr);
  node_add(tilt, body);

  // handlebars on a tilted steering column
  M4 inv;
  Node *pivot = steer_pivot(v3(0, 1.0, -0.4), v3(0, 0.86, -0.45), &inv);
  Node *steer = node_new(NODE_GROUP, "steer");
  node_add(pivot, steer);
  node_add(tilt, pivot);
  Parts H = {};
  parts_add(&H, rod(P3(0, 0.86, -0.45), P3(0, 1.0, -0.4), 0.02), TUBEC, 0.5, 0.45, 0);
  parts_add(&H, geo_translate(geo_rounded_box(0.3, 0.09, 0.16, 2, 0.03), 0, 1.03, -0.45), RED, 0.05, 0.34, 0);
  parts_add(&H, geo_translate(geo_scale(geo_sphere3(0.035, 12, 8), 1.4, 0.8, 0.5), 0, 1.035, -0.53), 0xfff2dc, 0.2, 0.1, 0.6);
  parts_add(&H, TUBE_S(0.013, 50, P3(-0.43, 1.085, -0.34), P3(-0.3, 1.065, -0.4), P3(-0.13, 1.05, -0.43), P3(0.13, 1.05, -0.43), P3(0.3, 1.065, -0.4), P3(0.43, 1.085, -0.34)),
            0x9c9fa3, 0.8, 0.3, 0);
  for (int si = 0; si < 2; si++) {
    double s = si ? 1 : -1;
    parts_add(&H, rod_n(P3(s * 0.32, 1.068, -0.39), P3(s * 0.445, 1.088, -0.33), 0.02, 12), BLACK, 0, 0.8, 0);
    parts_add(&H, tube((V3[]){ P3(s * 0.26, 1.07, -0.425), P3(s * 0.34, 1.07, -0.445), P3(s * 0.41, 1.072, -0.42) }, 3, 0.006, 10, false, 5), 0xb4b7bb, 0.9, 0.3, 0);
    parts_add(&H, box(P3(s * 0.25, 1.07, -0.41), P3(0.05, 0.04, 0.04)), BLACK, 0, 0.6, 0);
  }
  node_add(steer, parts_mesh(&H, mat, &inv));

  // wheels on suspension (front ones steer)
  static const double WH[4][3] = { { -0.49, -0.625, 0.2 }, { 0.49, -0.625, 0.2 }, { -0.47, 0.625, 0.25 }, { 0.47, 0.625, 0.25 } };
  for (int i = 0; i < 4; i++) {
    Node *hold = node_new(NODE_GROUP, "hold");
    hold->position = v3(WH[i][0], R, WH[i][1]);
    Parts wp = knobby_wheel(R, WH[i][2], js_sign(WH[i][0]));
    Node *m = parts_mesh(&wp, mat, nullptr);
    node_add(hold, m);
    node_add(tilt, hold);
    M.wheels[i] = (AtvWheel){ hold, m, WH[i][0], WH[i][1] };
  }
  M.root = root; M.tilt = tilt; M.steer = steer;
  M.shadow = blob(1.7, 2.6);
  M.mat = mat;
  M.R = R;
  return M;
}
