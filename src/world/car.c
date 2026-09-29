// Port of src/world/car.js: a classic late-50s American-style convertible (no badges, no make):
// lofted body with long hood, tail fins and real wheel wells, open cockpit with cream bench
// seats, chrome bumpers and trim, whitewall tyres. A parked hero car at the west curb plus a
// moving variant driven by the audio engine's car passes; modern parked cars along the lane.
#include "world/car.h"

#include "canvas/canvas.h"
#include "gfx/three_mat.h"
#include "textures/noise.h"
#include "world/layout.h"
#include "world/lod.h"

#define TAU (PI_D * 2)
typedef Vec(Geometry *) GeoList;
static void gl_push(GeoList *l, Geometry *g) { vec_push(l, g); }
static Geometry *gl_merge(GeoList *l) {
  Geometry *m = geo_merge_free(l->data, (int)l->len);
  vec_free(l);
  return m;
}
// g.index ? g.toNonIndexed() : g, then deleteAttribute('uv') (and 'uv1')
static Geometry *strip_uv(Geometry *g) {
  if (g->index) {
    Geometry *n = geo_to_non_indexed(g);
    geo_free(g);
    g = n;
  }
  if (geo_attr(g, "uv")) geo_delete_attr(g, "uv");
  if (geo_attr(g, "uv1")) geo_delete_attr(g, "uv1");
  return g;
}
static Geometry *gl_merge_stripped(GeoList *l) {
  for (size_t i = 0; i < l->len; i++) l->data[i] = strip_uv(l->data[i]);
  return gl_merge(l);
}
static double sq(double x) { return x * x; }   // x ** 2

static bool blocked_bay(double zc, double hl) {
  for (int i = 0; i < NCROSS_STREETS; i++) {
    const CrossStreet *c = &CROSS_STREETS[i];
    if (fabs(zc - c->z) < CROSS.hw + CROSS.R + hl + 0.5) return true;
    if (!c->far) {
      double legs[2][2];
      crossLegs(c->z, legs);
      for (int k = 0; k < 2; k++) if (zc + hl > legs[k][0] - 0.5 && zc - hl < legs[k][1] + 0.5) return true;
    }
  }
  return false;
}

static constexpr double LEN = 5.3;
static constexpr double HALF = LEN / 2;
static constexpr double WHEEL_R = 0.36;
static constexpr double TRACK = 0.8;
static constexpr double ARCH_R = 0.45;
static const double WHEELS_Z[2] = { 1.62, -1.52 };
static const double OPEN[2] = { -1.3, 0.74 };   // cockpit (open) between these z

// ---- body section parameters along z (car local: +z forward, +x = left side, y up) -------------
static double half_width(double z) {
  double w = 1.0;
  if (z > 2.05) w -= 0.2 * sq((z - 2.05) / (HALF - 2.05));
  if (z < -2.2) w -= 0.1 * sq((-2.2 - z) / (HALF - 2.2));
  return w;
}
static double belt(double z) {
  if (z > 0.74) return 0.97 - 0.15 * pow((z - 0.74) / (HALF - 0.74), 1.6);   // hood falls to the nose
  if (z < -1.0) return 0.94 + 0.17 * pow(fmin(1, (-1.0 - z) / 1.55), 1.8);  // rising fins
  return 0.95;
}
static double crown(double z) {
  if (z > 0.74) return belt(z) + 0.035;
  return 0.92 - 0.05 * pow(fmax(0, (-2.0 - z) / 0.65), 2);                    // flat rear deck, below the fin tops
}
static double arch_y(double z) {
  double y = 0.3;
  for (int k = 0; k < 2; k++) {
    double d = z - WHEELS_Z[k];
    if (fabs(d) < ARCH_R) y = fmax(y, WHEEL_R + 0.02 + sqrt(ARCH_R * ARCH_R - d * d) * 0.95);
  }
  return y;
}

// loft half-profiles (x >= 0, bottom centre -> top centre) mirrored into closed rings
typedef struct Pt2 { double x, y; } Pt2;
typedef int (*SectionFn)(double z, Pt2 *P, void *user);   // returns the point count
static void sort_doubles(double *v, int n) {
  for (int i = 1; i < n; i++) {
    double x = v[i];
    int j = i - 1;
    while (j >= 0 && v[j] - x > 0) { v[j + 1] = v[j]; j--; }
    v[j + 1] = x;
  }
}
static Geometry *loft_geometry(const double *zs, int nz, SectionFn section, void *user, bool caps) {
  Pt2 P[64];
  int np = section(zs[0], P, user);
  int n = 2 * np - 1;
  Pt2 *rings = xmalloc(sizeof(Pt2) * (size_t)n * nz);
  for (int k = 0; k < nz; k++) {
    int m = section(zs[k], P, user);
    CHECK(m == np);
    Pt2 *ring = rings + (size_t)k * n;
    int r = 0;
    for (int i = np - 1; i > 0; i--) ring[r++] = (Pt2){ -P[i].x, P[i].y };
    for (int i = 0; i < np; i++) ring[r++] = P[i];
  }
  DVec pos = {};
  U32Vec idx = {};
  for (int k = 0; k < nz; k++)
    for (int i = 0; i < n; i++) {
      double p[3] = { rings[k * n + i].x, rings[k * n + i].y, zs[k] };
      vec_append(&pos, p, 3);
    }
  for (int k = 0; k < nz - 1; k++)
    for (int i = 0; i < n - 1; i++) {
      uint32_t a = (uint32_t)(k * n + i), b = a + (uint32_t)n;
      uint32_t q[6] = { a, a + 1, b, a + 1, b + 1, b };
      vec_append(&idx, q, 6);
    }
  if (caps) {
    const int ks[2] = { 0, nz - 1 };
    for (int e = 0; e < 2; e++) {
      int k = ks[e];
      bool flip = e == 0;
      uint32_t c = (uint32_t)(pos.len / 3);
      double cy = 0;   // ring.reduce((s, p) => s + p[1], 0) / ring.length
      for (int i = 0; i < n; i++) cy += rings[k * n + i].y;
      double cp[3] = { 0, cy / n, zs[k] };
      vec_append(&pos, cp, 3);
      for (int i = 0; i < n - 1; i++) {
        uint32_t a = (uint32_t)(k * n + i);
        uint32_t t1[3] = { c, a + 1, a }, t2[3] = { c, a, a + 1 };
        vec_append(&idx, flip ? t1 : t2, 3);
      }
    }
  }
  free(rings);
  Geometry *g = geo_new();
  geo_set_attr_d(g, "position", 3, (int)(pos.len / 3), pos.data);
  geo_set_index(g, idx.data, (int)idx.len);
  geo_compute_vertex_normals(g);
  vec_free(&pos);
  vec_free(&idx);
  return g;
}

static int body_section(double z, Pt2 *P, void *user) {
  (void)user;
  double W = half_width(z);
  bool open = z > OPEN[0] && z < OPEN[1];
  double ay = arch_y(z);
  bool inWell = ay > 0.31;
  double b = belt(z), c = crown(z);
  // right-side half profile from bottom centre to top centre (x >= 0)
  int n = 0;
  P[n++] = (Pt2){ 0, 0.27 };
  P[n++] = (Pt2){ W - 0.38, inWell ? ay : 0.27 };
  P[n++] = (Pt2){ W - 0.05, inWell ? ay : 0.31 };
  P[n++] = (Pt2){ W - 0.01, fmax(0.46, inWell ? ay + 0.02 : 0.46) };
  P[n++] = (Pt2){ W, 0.62 };
  P[n++] = (Pt2){ W - 0.015, 0.8 };
  P[n++] = (Pt2){ W - 0.05, b };
  P[n++] = (Pt2){ W - 0.1, b + 0.012 };
  P[n++] = open ? (Pt2){ W - 0.14, b - 0.04 } : (Pt2){ W - 0.3, c - 0.004 };
  P[n++] = open ? (Pt2){ W - 0.16, 0.5 } : (Pt2){ W * 0.5, c + 0.004 };
  P[n++] = open ? (Pt2){ 0, 0.5 } : (Pt2){ 0, c + 0.006 };
  return n;
}
static Geometry *body_geometry(void) {
  double zs[256];
  int nz = 0;
  for (double z = -HALF; z <= HALF + 1e-6; z += 0.05) zs[nz++] = js_to_fixed(z, 3);
  zs[nz++] = OPEN[0] - 0.001; zs[nz++] = OPEN[0] + 0.001; zs[nz++] = OPEN[1] - 0.001; zs[nz++] = OPEN[1] + 0.001;
  sort_doubles(zs, nz);
  return loft_geometry(zs, nz, body_section, nullptr, true);
}

static Geometry *tube_pts(const V3 *pts, int n, double r, int seg) {
  CatmullRom3 c = curve_catmull(pts, n, false, CURVE_CENTRIPETAL, 0.5);
  Geometry *g = geo_tube(&c, seg, r, 10, false);
  curve_free(&c);
  return g;
}
#define TUBE(r, seg, ...) tube_pts((const V3[]){ __VA_ARGS__ }, (int)(sizeof((const V3[]){ __VA_ARGS__ }) / sizeof(V3)), r, seg)

static Geometry *tyre_geometry(bool flatten) {
  // lathe profile (radius, axial) around y, then turned onto the x axle
  const double w = 0.105;
  const V2 pts[13] = { { 0.18, -w * 0.75 }, { 0.2, -w * 0.9 }, { 0.25, -w }, { 0.31, -w * 1.02 }, { 0.345, -w * 0.9 }, { WHEEL_R, -w * 0.55 },
                       { WHEEL_R + 0.004, 0 }, { WHEEL_R, w * 0.55 }, { 0.345, w * 0.9 }, { 0.31, w * 1.02 }, { 0.25, w }, { 0.2, w * 0.9 },
                       { 0.18, w * 0.75 } };
  Geometry *g = geo_rotate_z(geo_lathe(pts, 13, 48, 0, TAU), PI_D / 2);
  float *p = geo_data(g, "position");
  float *col = xmalloc(sizeof(float) * (size_t)g->count * 3);
  Color black = color_hex(0x141414), white = color_hex(0xf6f4ee);
  for (int i = 0; i < g->count; i++) {
    double r = js_hypot2(p[i * 3 + 1], p[i * 3 + 2]);
    Color c = r > 0.255 && r < 0.282 ? white : black;   // thin whitewall ring on a black sidewall
    col[i * 3] = (float)c.r; col[i * 3 + 1] = (float)c.g; col[i * 3 + 2] = (float)c.b;
    if (flatten && p[i * 3 + 1] < -(WHEEL_R - 0.018)) p[i * 3 + 1] = (float)-(WHEEL_R - 0.018);   // contact patch
  }
  geo_set_attr_copy(g, "color", 3, g->count, col);
  free(col);
  geo_compute_vertex_normals(g);
  return g;
}
static Geometry *hub_geometry(void) {
  const V2 pts[8] = { { 0.001, 0.05 }, { 0.03, 0.05 }, { 0.045, 0.042 }, { 0.08, 0.036 }, { 0.13, 0.028 }, { 0.17, 0.018 }, { 0.19, 0.01 }, { 0.2, 0.0 } };
  return geo_rotate_z(geo_lathe(pts, 8, 28, 0, TAU), -PI_D / 2);
}

static Texture *pleat_texture(void) {
  Canvas *c = canvas_new(256, 64);
  cv_fill_color(c, "#f0e6d2");
  cv_fill_rect(c, 0, 0, 256, 64);
  for (int i = 0; i < 16; i++) {
    Gradient g = cv_linear_gradient(c, i * 16, 0, i * 16 + 16, 0);
    grad_add_stop(&g, 0, "rgba(120,100,70,0.35)");
    grad_add_stop(&g, 0.2, "rgba(255,255,255,0.15)");
    grad_add_stop(&g, 0.8, "rgba(0,0,0,0)");
    grad_add_stop(&g, 1, "rgba(120,100,70,0.3)");
    cv_fill_gradient(c, &g);
    cv_fill_rect(c, i * 16, 0, 16, 64);
  }
  Texture *t = canvas_texture(c, true, WRAP_REPEAT, 1);
  canvas_free(c);
  return t;
}
static Texture *blob_texture(void) {
  Canvas *c = canvas_new(128, 128);
  cv_filter_blur(c, 3);
  cv_fill_color(c, "rgba(0,0,0,0.97)");
  cv_fill_rect(c, 30, 12, 68, 104);
  Texture *t = canvas_texture(c, true, WRAP_CLAMP, 1);
  canvas_free(c);
  return t;
}

// ---- shared (paint-independent) materials and geometry -------------------------------------------
typedef struct Shared {
  Material *chrome, *rubber, *leather, *dark, *carpet, *glass, *red, *lens, *blob, *hubMat, *canvasTop, *ivory, *plate;
  GpuGeometry *ivoryGeo, *gapGeo, *plateGeo, *body, *chromeGeo, *glassGeo, *seatGeo, *bootGeo, *darkGeo, *dashGeo, *carpetGeo,
      *tails, *lensGeo, *tyreFlat, *tyre, *hub;
} Shared;
static Shared *g_shared;
static Texture *g_env;

static Material *std_mat(const char *name, uint32_t color, double roughness, double metalness, ProgramId prog) {
  MatDesc d = md_standard();
  d.name = name;
  d.color = color_hex(color);
  d.roughness = roughness;
  d.metalness = metalness;
  d.prog[MV_PLAIN] = prog;
  return mat_three(&d);
}
static GpuGeometry *upload(Geometry *g) {
  GpuGeometry *gg = gpu_geometry(g);
  geo_free(g);
  return gg;
}

static double hood_y(double x, double z) { return belt(z) + 0.01 + 0.035 * (1 - sq(x / half_width(z))); }

static Shared *shared(void) {
  if (g_shared) return g_shared;
  Shared *S = xcalloc(1, sizeof *S);
  // (buildCars sets the env map of chrome, hub, glass and lens to scene.environment before any
  // car is made, so their own envMapIntensity applies)
  MatDesc d = md_standard();
  d.name = "car chrome";
  d.color = color_hex(0xffffff); d.metalness = 1; d.roughness = 0.07; d.env_map_intensity = 1.3; d.side = SIDE_DOUBLE;
  d.env_map = g_env;
  d.prog[MV_PLAIN] = PROG_CAR_REFL_CHROME3;
  S->chrome = mat_three(&d);
  d = md_standard();
  d.name = "car hub";
  d.color = color_hex(0xf4f4f4); d.metalness = 1; d.roughness = 0.16; d.env_map_intensity = 1.0; d.side = SIDE_DOUBLE;
  d.env_map = g_env;
  d.prog[MV_PLAIN] = PROG_CAR_REFL_HUB3;
  S->hubMat = mat_three(&d);
  d = md_standard();
  d.name = "car rubber";
  d.vertex_colors = true; d.roughness = 0.85; d.side = SIDE_DOUBLE;
  d.prog[MV_PLAIN] = PROG_CAR_CLAMP_RUBBER;
  S->rubber = mat_three(&d);
  S->canvasTop = std_mat("car canvas top", 0xb49a70, 0.95, 0, PROG_STD);
  S->ivory = std_mat("car ivory", 0xf1ead8, 0.35, 0, PROG_STD);
  S->plate = std_mat("car plate", 0xf0ecdc, 0.5, 0.2, PROG_STD);
  d = md_standard();
  d.name = "car leather";
  d.map = pleat_texture(); d.color = color_hex(0xfffaf0); d.roughness = 0.5;
  d.prog[MV_PLAIN] = PROG_STD_MAP;
  S->leather = mat_three(&d);
  S->dark = std_mat("car dark", 0x151617, 0.8, 0, PROG_STD);
  S->carpet = std_mat("car carpet", 0x3a3430, 0.95, 0, PROG_STD);
  d = md_physical();
  d.name = "car glass";
  d.color = color_hex(0xcfe4e0); d.roughness = 0.02; d.metalness = 0; d.transparent = true; d.opacity = 0.38;
  d.env_map_intensity = 2.5; d.depth_write = false; d.side = SIDE_DOUBLE;
  d.env_map = g_env;
  d.prog[MV_PLAIN] = PROG_PHYS_TRANSP;
  d.prog[MV_PLAIN_BACK] = PROG_PHYS_TRANSP_BACK;
  d.prog[MV_PLAIN_FRONT] = PROG_PHYS_TRANSP;
  S->glass = mat_three(&d);
  d = md_standard();
  d.name = "car red";
  d.color = color_hex(0xa3161a); d.roughness = 0.3; d.emissive = color_hex(0x3a0404);
  d.prog[MV_PLAIN] = PROG_STD;
  S->red = mat_three(&d);
  d = md_standard();
  d.name = "car lens";
  d.color = color_hex(0xe8e6de); d.roughness = 0.25; d.metalness = 0.1; d.env_map_intensity = 0.6;
  d.env_map = g_env;
  d.prog[MV_PLAIN] = PROG_CAR_CLAMP_LENS;
  S->lens = mat_three(&d);
  d = md_basic();
  d.name = "car blob";
  d.map = blob_texture(); d.transparent = true; d.depth_write = false;
  d.polygon_offset = true; d.po_factor = -2; d.po_units = -2;
  d.prog[MV_PLAIN] = PROG_BASIC_MAP_TRANSP;
  S->blob = mat_three(&d);

  // chrome parts merged
  double (*W)(double) = half_width;
  GeoList ch = {};
  gl_push(&ch, TUBE(0.085, 40, { -0.98, 0.42, 2.42 }, { -0.9, 0.42, 2.64 }, { -0.4, 0.41, 2.73 }, { 0.4, 0.41, 2.73 }, { 0.9, 0.42, 2.64 }, { 0.98, 0.42, 2.42 }));
  gl_push(&ch, TUBE(0.08, 40, { -0.98, 0.44, -2.5 }, { -0.9, 0.44, -2.68 }, { -0.4, 0.43, -2.74 }, { 0.4, 0.43, -2.74 }, { 0.9, 0.44, -2.68 }, { 0.98, 0.44, -2.5 }));
  const double ss[2] = { 1, -1 };
  for (int k = 0; k < 2; k++) {   // spear(1), spear(-1)
    double s = ss[k];
    gl_push(&ch, TUBE(0.012, 40, { s * (W(2.3) + 0.004), 0.66, 2.3 }, { s * (W(0) + 0.006), 0.62, 0 }, { s * (W(-1.6) + 0.006), 0.66, -1.6 }, { s * (W(-2.5) + 0.002), 0.78, -2.5 }));
  }
  for (int k = 0; k < 2; k++) {   // beltTrim(1), beltTrim(-1)
    double s = ss[k];
    gl_push(&ch, TUBE(0.014, 20, { s * (W(0.7) - 0.07), belt(0.7) + 0.02, 0.7 }, { s * (W(-0.3) - 0.07), belt(-0.3) + 0.02, -0.3 }, { s * (W(-1.25) - 0.07), belt(-1.25) + 0.02, -1.25 }));
  }
  gl_push(&ch, TUBE(0.02, 48, { -0.93, 0.98, 0.74 }, { -0.9, 1.25, 0.57 }, { -0.84, 1.4, 0.46 }, { 0, 1.43, 0.44 }, { 0.84, 1.4, 0.46 }, { 0.9, 1.25, 0.57 }, { 0.93, 0.98, 0.74 }));
  for (int i = 0; i < 5; i++) gl_push(&ch, geo_translate(geo_box1(1.36, 0.02, 0.03), 0, 0.5 + i * 0.045, 2.645));
  for (int i = 0; i < 9; i++) gl_push(&ch, geo_translate(geo_box1(0.018, 0.2, 0.03), -0.6 + i * 0.15, 0.59, 2.645));
  gl_push(&ch, TUBE(0.016, 40, { -0.7, 0.48, 2.63 }, { -0.72, 0.6, 2.63 }, { -0.7, 0.71, 2.63 }, { 0.7, 0.71, 2.63 }, { 0.72, 0.6, 2.63 }, { 0.7, 0.48, 2.63 }, { -0.7, 0.48, 2.63 }));
  // hood centre strip, door handles, mirrors
  gl_push(&ch, TUBE(0.012, 16, { 0, belt(2.5) + 0.04, 2.5 }, { 0, belt(1.6) + 0.045, 1.6 }, { 0, belt(0.8) + 0.04, 0.8 }));
  for (int k = 0; k < 2; k++) {
    double s = k ? 1 : -1;
    gl_push(&ch, geo_translate(geo_box1(0.02, 0.025, 0.14), s * (W(-0.3) + 0.01), 0.84, -0.32));
    gl_push(&ch, geo_translate(geo_cyl(0.012, 0.012, 0.12, 6), s * (W(0.55) - 0.06), belt(0.55) + 0.06, 0.55));
    gl_push(&ch, geo_translate(geo_rotate_y(geo_rotate_z(geo_cyl(0.06, 0.06, 0.03, 16), PI_D / 2), 0.3 * s), s * (W(0.55) - 0.06), belt(0.55) + 0.14, 0.55));
  }
  GeoList heads = {};
  for (int k = 0; k < 2; k++) {
    double s = k ? 1 : -1;
    const double dxs[2] = { 0.62, 0.84 };
    for (int j = 0; j < 2; j++) {
      double dx = dxs[j];
      gl_push(&ch, geo_translate(geo_torus(0.095, 0.022, 8, 20, TAU, 0, TAU), s * dx, 0.73, 2.57));   // headRings (after the grille bars)
      gl_push(&heads, geo_translate(geo_scale(geo_rotate_x(geo_sphere(0.09, 16, 8, 0, TAU, 0, PI_D / 2), PI_D / 2), 1, 1, 0.4), s * dx, 0.73, 2.565));
    }
  }
  gl_push(&ch, geo_translate(geo_rotate_x(geo_cyl(0.1, 0.1, 0.02, 20), PI_D / 2 - 0.4), 0.42, 0.97, 0.52));   // gauge
  S->chromeGeo = upload(gl_merge_stripped(&ch));

  // windshield glass: a gently curved pane inside the frame
  {
    double gp[66];
    int n = 0;
    for (int j = 0; j <= 1; j++)
      for (int i = 0; i <= 10; i++) {
        double u = (double)i / 10 * 2 - 1;
        double y = j ? 1.4 : 0.98, z = (j ? 0.46 : 0.74) + pow(fabs(u), 3) * 0.08;
        gp[n++] = u * (j ? 0.84 : 0.92); gp[n++] = y; gp[n++] = z;
      }
    uint32_t gi[60];
    int ni = 0;
    for (uint32_t i = 0; i < 10; i++) { uint32_t q[6] = { i, i + 11, i + 1, i + 1, i + 11, i + 12 }; memcpy(gi + ni, q, sizeof q); ni += 6; }
    Geometry *g = geo_new();
    geo_set_attr_d(g, "position", 3, 22, gp);
    geo_set_index(g, gi, ni);
    geo_compute_vertex_normals(g);
    S->glassGeo = upload(g);
  }

  // interior
  GeoList seat = {};
  gl_push(&seat, geo_translate(geo_rounded_box(1.62, 0.18, 0.56, 3, 0.06), 0, 0.62, 0.02));
  gl_push(&seat, geo_translate(geo_rotate_x(geo_rounded_box(1.62, 0.5, 0.14, 3, 0.06), -0.22), 0, 0.92, -0.3));
  gl_push(&seat, geo_translate(geo_rounded_box(1.62, 0.18, 0.5, 3, 0.06), 0, 0.62, -0.85));
  gl_push(&seat, geo_translate(geo_rotate_x(geo_rounded_box(1.7, 0.42, 0.14, 3, 0.06), -0.2), 0, 0.88, -1.17));
  gl_push(&seat, geo_translate(geo_rounded_box(0.1, 0.36, 1.9, 2, 0.04), 0.84, 0.72, -0.4));   // door panels
  gl_push(&seat, geo_translate(geo_rounded_box(0.1, 0.36, 1.9, 2, 0.04), -0.84, 0.72, -0.4));
  S->seatGeo = upload(gl_merge(&seat));
  S->bootGeo = upload(geo_translate(geo_rounded_box(1.72, 0.16, 0.5, 4, 0.07), 0, 0.97, -1.46));   // tan canvas boot over the folded top
  Geometry *wheelGeo = geo_translate(geo_rotate_x(geo_torus(0.21, 0.022, 10, 32, TAU, 0, TAU), 0.45), 0.42, 1.0, 0.36);
  Geometry *column = geo_translate(geo_rotate_x(geo_cylinder(0.022, 0.03, 0.42, 8, 1, false, 0, TAU), PI_D / 2 - 0.45), 0.42, 0.93, 0.5);
  GeoList iv = {};
  gl_push(&iv, wheelGeo);
  const double spokeA[3] = { 0, 2.1, 4.2 };
  for (int k = 0; k < 3; k++)
    gl_push(&iv, geo_translate(geo_rotate_x(geo_rotate_z(geo_translate(geo_box1(0.2, 0.012, 0.012), 0.1, 0, 0), spokeA[k]), 0.45), 0.42, 1.0, 0.36));
  GeoList dk = {};
  gl_push(&dk, column);
  gl_push(&dk, geo_translate(geo_box1(1.7, 0.18, 4.6), 0, 0.2, 0));        // chassis: dark core under the car
  gl_push(&dk, geo_translate(geo_box1(1.34, 0.2, 0.05), 0, 0.6, 2.625));   // grille cavity
  for (int k = 0; k < 2; k++)
    for (int j = 0; j < 2; j++) {   // well liners
      double s = j ? 1 : -1;
      gl_push(&dk, geo_translate(geo_rotate_z(geo_cylinder(ARCH_R - 0.02, ARCH_R - 0.02, 0.36, 18, 1, true, 0, TAU), PI_D / 2), s * (TRACK + 0.02), WHEEL_R + 0.03, WHEELS_Z[k]));
    }
  S->darkGeo = upload(gl_merge_stripped(&dk));
  S->dashGeo = upload(geo_translate(geo_rounded_box(1.74, 0.24, 0.34, 3, 0.07), 0, 0.86, 0.6));
  S->carpetGeo = upload(geo_translate(geo_rotate_x(geo_plane(1.7, 2.0, 1, 1), -PI_D / 2), 0, 0.505, -0.3));
  GeoList tl = {};
  for (int k = 0; k < 2; k++) {
    double s = k ? 1 : -1;
    gl_push(&tl, geo_translate(geo_rotate_x(geo_capsule(0.05, 0.12, 4, 10, 1), PI_D / 2), s * 0.86, 0.98, -2.62));
  }
  S->tails = upload(gl_merge(&tl));
  S->lensGeo = upload(gl_merge(&heads));
  S->ivoryGeo = upload(gl_merge_stripped(&iv));
  // door shut lines (dark hairlines on the flanks) and plain license plates
  GeoList gaps = {};
  V3 pts[64];
  for (int k = 0; k < 2; k++) {
    double s = k ? 1 : -1;
    const double zz[2] = { 0.72, -0.62 };
    for (int j = 0; j < 2; j++) {
      double z = zz[j];
      int n = 0;
      for (double y = 0.36; y <= belt(z) - 0.02; y += 0.06) pts[n++] = v3(s * (W(z) + 0.004), y, z);
      gl_push(&gaps, tube_pts(pts, n, 0.0065, 12));
    }
  }
  // hood and trunk shut lines across the deck, hood edges, fender seams behind the arches
  {
    int n = 0;
    for (double x = -W(1.38) + 0.1; x <= W(1.38) - 0.1 + 1e-6; x += 0.1) pts[n++] = v3(x, hood_y(x, 1.38), 1.38);
    gl_push(&gaps, tube_pts(pts, n, 0.006, 20));
  }
  {
    int n = 0;
    for (double x = -0.72; x <= 0.72 + 1e-6; x += 0.12) pts[n++] = v3(x, crown(-1.58) + 0.008, -1.58);
    gl_push(&gaps, tube_pts(pts, n, 0.006, 14));
  }
  for (int k = 0; k < 2; k++) {
    double s = k ? 1 : -1;
    int n = 0;
    for (double z = 1.38; z <= 2.45; z += 0.1) pts[n++] = v3(s * (W(z) - 0.1), hood_y(W(z) - 0.1, z), z);
    gl_push(&gaps, tube_pts(pts, n, 0.006, 12));
    const double zz[2] = { 1.62 - ARCH_R - 0.22, -1.52 + ARCH_R + 0.22 };
    for (int j = 0; j < 2; j++) {
      double z = zz[j];
      int m = 0;
      for (double y = WHEEL_R + ARCH_R * 0.6; y <= belt(z) - 0.03; y += 0.06) pts[m++] = v3(s * (W(z) + 0.004), y, z);
      if (m > 1) gl_push(&gaps, tube_pts(pts, m, 0.005, 8));
    }
  }
  S->gapGeo = upload(gl_merge(&gaps));
  GeoList pl = {};
  gl_push(&pl, geo_translate(geo_box1(0.5, 0.15, 0.01), 0, 0.28, 2.78));
  gl_push(&pl, geo_translate(geo_box1(0.5, 0.15, 0.01), 0, 0.62, -2.67));
  S->plateGeo = upload(gl_merge(&pl));
  S->body = upload(body_geometry());
  S->tyreFlat = upload(tyre_geometry(true));
  S->tyre = upload(tyre_geometry(false));
  S->hub = upload(hub_geometry());
  g_shared = S;
  return S;
}

// place a car on the cambered road: height at its centre, rolled to the cross slope
static void seat_car(Node *obj, double x, double z, double rotY) {
  double slope = (roadHeight(x + 0.9) - roadHeight(x - 0.9)) / 1.8;
  obj->position = v3(x, roadHeight(x), z);
  node_set_rotation(obj, 0, rotY, 0);
  // rotateZ: q = q * axisAngle(z)
  node_set_quaternion(obj, quat_mul(obj->quaternion, quat_axis_angle(v3(0, 0, 1), atan(slope) * (rotY == 0 ? 1 : -1))));
}

typedef struct HeroCar { Node *car; Node *wheels[4]; } HeroCar;

static Node *add_mesh(Node *parent, GpuGeometry *g, Material *m) {
  Node *o = node_mesh(g, m);
  node_add(parent, o);
  return o;
}
static void set_shadows(Node *n, void *user) {
  const Shared *S = ((void **)user)[0];
  bool cast = *(bool *)((void **)user)[1];
  if (n->kind == NODE_MESH) {
    n->cast_shadow = cast && n->material != S->glass;
    n->receive_shadow = true;
  }
}

static HeroCar make_car(uint32_t paint, bool flatten, bool cast) {
  Shared *S = shared();
  HeroCar H = { node_new(NODE_GROUP, "car"), {} };
  MatDesc d = md_physical();
  d.name = "car paint";
  d.color = color_hex(paint); d.roughness = 0.28; d.metalness = 0.05; d.clearcoat = 1; d.clearcoat_roughness = 0.03;
  d.env_map_intensity = 1.8;
  d.env_map = g_env;   // paintMat.envMap = S.chrome.envMap
  d.prog[MV_PLAIN] = PROG_CAR_REFL_PAINT0;
  Material *paintMat = mat_three(&d);
  Node *car = H.car;
  add_mesh(car, S->body, paintMat);
  add_mesh(car, S->dashGeo, paintMat);
  add_mesh(car, S->chromeGeo, S->chrome);
  add_mesh(car, S->seatGeo, S->leather);
  add_mesh(car, S->bootGeo, S->canvasTop);
  add_mesh(car, S->ivoryGeo, S->ivory);
  add_mesh(car, S->gapGeo, S->dark);
  add_mesh(car, S->plateGeo, S->plate);
  add_mesh(car, S->darkGeo, S->dark);
  add_mesh(car, S->carpetGeo, S->carpet);
  add_mesh(car, S->tails, S->red);
  add_mesh(car, S->lensGeo, S->lens);
  Node *glass = add_mesh(car, S->glassGeo, S->glass);
  glass->render_order = 2;
  int k = 0;
  for (int wi = 0; wi < 2; wi++)
    for (int si = 0; si < 2; si++) {
      double s = si ? 1 : -1;
      Node *w = node_new(NODE_GROUP, "wheel");
      node_add(w, node_mesh(flatten ? S->tyreFlat : S->tyre, S->rubber));
      Node *hub = node_mesh(S->hub, S->hubMat);
      hub->scale.x = s;
      hub->position.x = s * 0.095;
      node_add(w, hub);
      w->position = v3(s * TRACK, WHEEL_R, WHEELS_Z[wi]);
      node_add(car, w);
      H.wheels[k++] = w;
    }
  void *ctx[2] = { S, &cast };
  node_traverse(car, set_shadows, ctx);
  // soft dark contact patch under the car
  Geometry *bg = geo_rotate_x(geo_plane(2.4, 6.0, 1, 1), -PI_D / 2);
  Node *blob = node_mesh(upload(bg), S->blob);
  blob->position.y = 0.012;
  blob->render_order = 1;
  node_add(car, blob);
  return H;
}

// ---- modern parked cars (sedan / hatch / SUV / pickup) ---------------------------------------------
// Bodies are lofted like the hero car: curved cross-sections swept along the length over a rounded
// plan outline, with fender flares, a beltline crease, a sloping hood and deck; the greenhouse is
// lofted on the shoulders with tumblehome, a rounded roof edge, raked screens and a crowned
// roofline. Lamps are strips wrapped round the body corners; tyres are lathed with tread grooves
// and a flattened contact patch, on spoked rims. Geometry is built once per type.
typedef enum ModernType { MT_SEDAN, MT_HATCH, MT_SUV, MT_PICKUP } ModernType;
typedef struct ModernDef { double L, W, wb, r, belt, nose, deck, tail, zA, zRf, zRr, zR, top, Wt, doors[3]; } ModernDef;
static const ModernDef MODERN[4] = {
  [MT_SEDAN] = { 4.75, 0.9, 1.42, 0.33, 0.98, 0.74, 0.99, 0.92, 0.98, 0.18, -0.85, -1.55, 1.45, 0.64, { 0.95, -0.15, -1.2 } },
  [MT_HATCH] = { 4.15, 0.88, 1.28, 0.32, 0.96, 0.74, 0.96, 0.9, 0.9, 0.1, -1.35, -1.85, 1.47, 0.63, { 0.85, -0.35, -1.3 } },
  [MT_SUV] = { 4.7, 0.95, 1.42, 0.37, 1.13, 0.98, 1.1, 1.02, 1.32, 0.55, -1.85, -2.2, 1.75, 0.74, { 1.25, 0.0, -1.15 } },
  [MT_PICKUP] = { 5.3, 0.97, 1.65, 0.38, 1.12, 1.0, 1.14, 1.1, 1.55, 0.85, -0.38, -0.58, 1.82, 0.76, { 1.5, 0.3, -0.5 } },
};
typedef struct ModernGeo {
  const ModernDef *T;
  double wheelX;
  Geometry *paint, *glass, *trim, *lens, *red, *tyre, *rim;   // CPU (the fleet transforms copies)
  GpuGeometry *gGlass, *gTrim, *gLens, *gRed, *gTyre, *gRim;  // uploaded on first use by a sedan
} ModernGeo;
static ModernGeo *g_modern[4];

typedef struct MT { const ModernDef *T; double H, R, RC; } MT;
static double m_halfW(const MT *m, double z) {
  const ModernDef *T = m->T;
  double e = fabs(z) - (m->H - m->RC);
  double w = e > 0 ? T->W - m->RC + sqrt(fmax(0, m->RC * m->RC - e * e * 0.92)) : T->W;
  const double wzs[2] = { T->wb, -T->wb };
  for (int k = 0; k < 2; k++) w += 0.02 * exp(-sq((z - wzs[k]) / 0.55));
  return w;
}
static double m_archY(const MT *m, double z) {
  const ModernDef *T = m->T;
  double y = 0;
  const double wzs[2] = { T->wb, -T->wb };
  for (int k = 0; k < 2; k++) {
    double d = z - wzs[k];
    if (fabs(d) < m->R) y = fmax(y, T->r + sqrt(m->R * m->R - d * d) * 0.97);
  }
  return y;
}
static double m_deckTop(const MT *m, double z) {
  const ModernDef *T = m->T;
  if (z > T->zA) {
    double t = (z - T->zA) / (m->H - T->zA);
    return T->belt - (T->belt - T->nose) * pow(t, 1.8) + 0.02 * sin(PI_D * t);
  }
  if (z < T->zR) {
    double t = (T->zR - z) / (T->zR + m->H);
    return T->deck + (T->belt - T->deck) * (1 - t) - (T->deck - T->tail) * pow(t, 3);
  }
  return T->belt;
}
static double m_bottom(const MT *m, double z) {   // bumpers turn under
  return 0.25 + 0.14 * sq(fmax(0, (fabs(z) - (m->H - 0.32)) / 0.32));
}
static double m_roofY(const MT *m, double z) {
  const ModernDef *T = m->T;
  if (z >= T->zRf) {
    double t = fmax(0, (T->zA - z) / (T->zA - T->zRf));
    return T->belt + (T->top - T->belt) * (1 - pow(1 - t, 1.4));
  }
  if (z <= T->zRr) {
    double t = fmax(0, (z - T->zR) / (T->zRr - T->zR));
    return T->belt + (T->top - T->belt) * (1 - pow(1 - t, 1.3));
  }
  return T->top + 0.03 * sin(PI_D * (z - T->zRr) / (T->zRf - T->zRr));
}
static int m_body_section(double z, Pt2 *P, void *user) {
  const MT *m = user;
  double W = m_halfW(m, z), ay = m_archY(m, z), t = m_deckTop(m, z), b = m_bottom(m, z);
  bool well = ay > 0.3;
  double sill = fmax(b + 0.06, 0.32);
  int n = 0;
  P[n++] = (Pt2){ 0, b };
  P[n++] = (Pt2){ W - 0.34, well ? ay : b };
  P[n++] = (Pt2){ W - 0.07, well ? ay : b + 0.02 };
  P[n++] = (Pt2){ W - 0.03, well ? ay + 0.015 : sill };
  // flank: bulge at ~0.55 m, a crease below the shoulder, then tumblehome into the shoulder
  static const double FL[6][2] = { { 0.0, -0.012 }, { 0.35, 0.0 }, { 0.62, -0.006 }, { 0.8, -0.02 }, { 0.84, -0.012 }, { 0.93, -0.035 } };
  for (int k = 0; k < 6; k++) {
    double y = fmax(P[n - 1].y + 0.01, sill + (t - 0.035 - sill) * FL[k][0]);
    P[n++] = (Pt2){ W + FL[k][1], y };
  }
  P[n++] = (Pt2){ W - 0.07, t - 0.005 };
  P[n++] = (Pt2){ W - 0.18, t + 0.018 };
  P[n++] = (Pt2){ W * 0.5, t + 0.03 };
  P[n++] = (Pt2){ 0, t + 0.034 };
  return n;
}
typedef struct GH { Pt2 side[4], corner[4], top[2]; double y, xt, rr; } GH;
static GH gh_section(const MT *m, double z, double inset) {
  const ModernDef *T = m->T;
  GH g;
  double y = fmax(m_roofY(m, z), T->belt + 0.004), k = fmin(1, (y - T->belt) / (T->top - T->belt));
  double xb = m_halfW(m, z) - 0.075, xt = fmin(xb, xb + (T->Wt - xb) * k);
  double rr = fmin(0.09, (y - T->belt) * 0.45);
  for (int i = 0; i <= 3; i++) {
    double f = i / 3.0;
    g.side[i] = (Pt2){ xb + (xt - xb) * f + 0.018 * sin(PI_D * f) * k - inset, T->belt + (y - rr - T->belt) * f };
  }
  for (int i = 1; i <= 4; i++) {
    double a = (i / 4.0) * PI_D / 2;
    g.corner[i - 1] = (Pt2){ xt - rr + rr * cos(a) - inset, y - rr + rr * sin(a) + inset * 0.5 };
  }
  g.y = y; g.xt = xt; g.rr = rr;
  g.top[0] = (Pt2){ xt * 0.5, y + 0.02 * k + inset * 0.5 };
  g.top[1] = (Pt2){ 0, y + 0.026 * k + inset * 0.5 };
  return g;
}
static int m_glass_section(double z, Pt2 *P, void *user) {
  const MT *m = user;
  GH s = gh_section(m, z, 0);
  int n = 0;
  P[n++] = (Pt2){ 0, m->T->belt - 0.02 };
  P[n++] = (Pt2){ s.side[0].x, m->T->belt - 0.02 };
  for (int i = 0; i < 4; i++) P[n++] = s.side[i];
  for (int i = 0; i < 4; i++) P[n++] = s.corner[i];
  for (int i = 0; i < 2; i++) P[n++] = s.top[i];
  return n;
}
static int m_roof_section(double z, Pt2 *P, void *user) {
  const MT *m = user;
  GH s = gh_section(m, z, -0.006);
  int n = 0;
  P[n++] = (Pt2){ 0, s.y - 0.05 };
  P[n++] = (Pt2){ s.corner[0].x, s.y - 0.05 };
  for (int i = 1; i < 4; i++) P[n++] = s.corner[i];
  for (int i = 0; i < 2; i++) P[n++] = s.top[i];
  return n;
}
static int m_edge(const MT *m, double z0, double z1, int n, V3 *out) {
  for (int i = 0; i <= n; i++) {
    double z = z0 + ((z1 - z0) * i) / n;
    GH s = gh_section(m, z, 0);
    out[i] = v3(s.corner[1].x + 0.004, s.corner[1].y, z);
  }
  return n + 1;
}
static void flip_x(V3 *pts, int n, double s) { for (int i = 0; i < n; i++) pts[i].x *= s; }

// a strip through a list of 3D points, extruded +-h/2 vertically (lamps), both sides
static Geometry *strip_geometry(const V3 *pts, int n, double h) {
  DVec pos = {};
  U32Vec idx = {};
  for (int i = 0; i < n; i++) {
    double p[6] = { pts[i].x, pts[i].y - h / 2, pts[i].z, pts[i].x, pts[i].y + h / 2, pts[i].z };
    vec_append(&pos, p, 6);
  }
  for (int i = 0; i < n - 1; i++) {
    uint32_t a = (uint32_t)(i * 2);
    uint32_t q[6] = { a, a + 2, a + 1, a + 1, a + 2, a + 3 };
    vec_append(&idx, q, 6);
  }
  Geometry *g = geo_new();
  geo_set_attr_d(g, "position", 3, (int)(pos.len / 3), pos.data);
  geo_set_index(g, idx.data, (int)idx.len);
  geo_compute_vertex_normals(g);
  vec_free(&pos);
  vec_free(&idx);
  return g;
}
static Geometry *mirror_x(const Geometry *g0) {
  Geometry *g = geo_scale(geo_clone(g0), -1, 1, 1);
  for (int i = 0; i + 2 < g->index_count; i += 3) {
    uint32_t t = g->index[i + 1];
    g->index[i + 1] = g->index[i + 2];
    g->index[i + 2] = t;
  }
  return geo_compute_vertex_normals(g);
}
static Geometry *modern_tyre(double r) {
  // lathe profile (radius, axial): rounded shoulders, four tread ribs, sidewall bulge
  const double w = 0.11;
  V2 prof[32];
  int n = 0;
  prof[n++] = v2(r * 0.64, -w * 0.8); prof[n++] = v2(r * 0.7, -w * 0.95); prof[n++] = v2(r * 0.85, -w * 1.02);
  prof[n++] = v2(r * 0.95, -w * 0.95); prof[n++] = v2(r - 0.004, -w * 0.78);
  for (int k = 0; k <= 8; k++) {
    double a = -w * 0.7 + (1.4 * w * k) / 8;
    prof[n++] = v2(k % 2 ? r - 0.009 : r, a);
  }
  prof[n++] = v2(r - 0.004, w * 0.78); prof[n++] = v2(r * 0.95, w * 0.95); prof[n++] = v2(r * 0.85, w * 1.02);
  prof[n++] = v2(r * 0.7, w * 0.95); prof[n++] = v2(r * 0.64, w * 0.8);
  Geometry *g = geo_rotate_z(geo_lathe(prof, n, 40, 0, TAU), PI_D / 2);
  float *p = geo_data(g, "position");
  for (int i = 0; i < g->count; i++) if (p[i * 3 + 1] < -(r - 0.015)) p[i * 3 + 1] = (float)-(r - 0.015);   // contact patch
  return geo_compute_vertex_normals(g);
}
static Geometry *modern_rim(double r) {
  double rr = r * 0.62;
  const V2 dish[6] = { { 0.0, 0.06 }, { 0.05, 0.06 }, { 0.07, 0.05 }, { rr * 0.9, 0.03 }, { rr, 0.035 }, { rr, 0.0 } };
  GeoList parts = {};
  gl_push(&parts, geo_rotate_z(geo_lathe(dish, 6, 32, 0, TAU), -PI_D / 2));
  for (int k = 0; k < 5; k++)
    gl_push(&parts, geo_translate(geo_rotate_x(geo_translate(geo_box1(0.03, rr * 0.9, 0.05), 0, rr * 0.5, 0), (k * PI_D * 2) / 5), 0.07, 0, 0));
  gl_push(&parts, geo_translate(geo_rotate_z(geo_cyl(0.05, 0.05, 0.03, 12), PI_D / 2), 0.09, 0, 0));
  return gl_merge_stripped(&parts);
}

static ModernGeo *modern_geometry(ModernType type) {
  if (g_modern[type]) return g_modern[type];
  const ModernDef *T = &MODERN[type];
  MT mt = { T, T->L / 2, T->r + 0.035, 0.5 };
  const MT *m = &mt;
  const double H = mt.H, R = mt.R;
  double zs[256];
  int nz = 0;
  for (double z = -H; z <= H + 1e-6; z += 0.04) zs[nz++] = js_to_fixed(z, 3);
  Geometry *body = loft_geometry(zs, nz, m_body_section, &mt, true);
  // greenhouse on the shoulders: tumblehome sides, rounded roof edge, crowned roof
  double gz[256];
  int ng = 0;
  for (double z = T->zR; z <= T->zA + 1e-6; z += 0.04) gz[ng++] = js_to_fixed(z, 3);
  gz[ng++] = T->zA;
  // [...new Set(gz)].sort((a, b) => a - b)
  double gu[256];
  int nu = 0;
  for (int i = 0; i < ng; i++) {
    bool seen = false;
    for (int k = 0; k < nu; k++) if (gu[k] == gz[i]) seen = true;
    if (!seen) gu[nu++] = gz[i];
  }
  sort_doubles(gu, nu);
  Geometry *glass = loft_geometry(gu, nu, m_glass_section, &mt, false);
  GeoList paint = {};
  gl_push(&paint, body);
  // painted roof over the roof span, and the A / C (D) pillars along the glass edges
  double rz[256];
  int nr = 0;
  for (double z = T->zRr - 0.03; z <= T->zRf + 0.03 + 1e-6; z += 0.04) rz[nr++] = js_to_fixed(z, 3);
  gl_push(&paint, loft_geometry(rz, nr, m_roof_section, &mt, true));
  double bigC = type == MT_SUV ? 0.05 : type == MT_PICKUP ? 0.045 : 0.075;
  V3 pts[64];
  for (int si = 0; si < 2; si++) {
    double s = si ? 1 : -1;
    int n = m_edge(m, T->zRf - 0.02, T->zA - 0.05, 10, pts);
    flip_x(pts, n, s);
    gl_push(&paint, tube_pts(pts, n, 0.038, 14));
    n = m_edge(m, T->zR + 0.05, T->zRr + 0.02, 8, pts);
    flip_x(pts, n, s);
    gl_push(&paint, tube_pts(pts, n, bigC, 12));
    // mirror on a stalk
    double mz = T->zA - 0.18, mx = m_halfW(m, mz) - 0.06;
    gl_push(&paint, geo_translate(geo_rounded_box(0.19, 0.12, 0.1, 2, 0.04), s * (mx + 0.19), T->belt + 0.1, mz));
  }
  // dark trim: window surround, B pillar, stalks, door seams + handles, liners, grille, underbody
  GeoList trim = {};
  double pillarZ[2];
  int npz = 0;
  if (type == MT_SUV) { pillarZ[npz++] = T->doors[1]; pillarZ[npz++] = T->doors[2] - 0.08; }
  else pillarZ[npz++] = T->doors[1];
  for (int si = 0; si < 2; si++) {
    double s = si ? 1 : -1;
    int n = m_edge(m, T->zR + 0.08, T->zA - 0.08, 20, pts);
    for (int i = 0; i < n; i++) pts[i] = v3(pts[i].x - 0.006, pts[i].y - 0.012, pts[i].z);
    flip_x(pts, n, s);
    gl_push(&trim, tube_pts(pts, n, 0.01, 30));   // upper DLO trim
    n = 0;
    for (double z = T->zR + 0.04; z <= T->zA - 0.04 + 1e-6; z += 0.1) pts[n++] = v3(m_halfW(m, z) - 0.07, T->belt + 0.004, z);
    flip_x(pts, n, s);
    gl_push(&trim, tube_pts(pts, n, 0.011, 24));
    for (int k = 0; k < npz; k++) {
      double z = pillarZ[k];
      GH g = gh_section(m, z, 0);
      V3 pp[3] = { { g.side[0].x + 0.004, T->belt, z }, { g.side[2].x + 0.004, g.side[2].y, z }, { g.corner[1].x, g.corner[1].y - 0.01, z } };
      flip_x(pp, 3, s);
      gl_push(&trim, tube_pts(pp, 3, 0.03, 6));
    }
    double mz = T->zA - 0.18, mx = m_halfW(m, mz) - 0.06;
    gl_push(&trim, geo_translate(geo_box1(0.16, 0.035, 0.05), s * (mx + 0.06), T->belt + 0.06, mz));
    for (int k = 0; k < 3; k++) {
      double z = T->doors[k];
      n = 0;
      for (double y = 0.36; y <= T->belt - 0.02; y += 0.05) pts[n++] = v3(m_halfW(m, z) + 0.004, y, z);
      flip_x(pts, n, s);
      gl_push(&trim, tube_pts(pts, n, 0.005, 10));
    }
    for (int i = 0; i < 2; i++)
      gl_push(&trim, geo_translate(geo_rounded_box(0.02, 0.028, 0.14, 1, 0.008), s * (m_halfW(m, T->doors[i]) + 0.008), T->belt - 0.09, T->doors[i] - 0.2));
    const double wzs[2] = { T->wb, -T->wb };
    for (int k = 0; k < 2; k++)
      gl_push(&trim, geo_translate(geo_rotate_z(geo_cylinder(R - 0.01, R - 0.01, 0.36, 18, 1, true, 0, TAU), PI_D / 2), s * (m_halfW(m, wzs[k]) - 0.2), T->r, wzs[k]));
  }
  gl_push(&trim, geo_translate(geo_rounded_box((m_halfW(m, H) - 0.12) * 2, 0.14, 0.04, 2, 0.015), 0, T->nose - 0.2, H - 0.005));   // grille
  gl_push(&trim, geo_translate(geo_rounded_box((m_halfW(m, H) - 0.05) * 2, 0.08, 0.06, 2, 0.02), 0, m_bottom(m, H) + 0.05, H - 0.02));  // lower lip
  gl_push(&trim, geo_translate(geo_box1(T->W * 1.6, 0.14, T->L - 1.2), 0, 0.23, 0));
  // lamps wrapped round the corners (strip following the plan outline, just proud of it)
  V3 lp[16];
  Geometry *strips[2];
  for (int f = 0; f < 2; f++) {
    bool front = f == 0;
    double sgn = front ? 1 : -1, y = front ? T->nose - 0.08 : T->tail - 0.07;
    int n = 0;
    for (int i = 0; i <= 8; i++) {
      double z = sgn * (H - 0.32 + (0.32 * i) / 8);
      lp[n++] = v3(m_halfW(m, z) + 0.004, y, z);
    }
    double zEnd = sgn * (H + 0.004);
    for (int i = 1; i <= 3; i++) lp[n++] = v3(m_halfW(m, sgn * H) - (0.3 * i) / 3, y, zEnd);
    strips[f] = strip_geometry(lp, n, front ? 0.085 : 0.1);
  }
  Geometry *lensL[2] = { strips[0], mirror_x(strips[0]) }, *redL[2] = { strips[1], mirror_x(strips[1]) };
  ModernGeo *G = xcalloc(1, sizeof *G);
  G->T = T;
  G->wheelX = m_halfW(m, T->wb) - 0.13;
  G->paint = gl_merge_stripped(&paint);
  G->glass = strip_uv(glass);
  G->trim = gl_merge_stripped(&trim);
  G->lens = strip_uv(geo_merge_free(lensL, 2));
  G->red = strip_uv(geo_merge_free(redL, 2));
  G->tyre = modern_tyre(T->r);
  G->rim = modern_rim(T->r);
  g_modern[type] = G;
  return G;
}

// body colour per vertex: paint with road grime creeping up the lower sills
static Geometry *paint_colors(Geometry *g, uint32_t hex) {
  Color c = color_hex(hex), d = color_hex(0x5a5248);
  const float *p = geo_data(g, "position");
  float *a = xmalloc(sizeof(float) * (size_t)g->count * 3);
  for (int i = 0; i < g->count; i++) {
    Color t = color_lerp(c, d, 0.45 * (1 - mu_smoothstep(p[i * 3 + 1], 0.3, 0.52)));
    a[i * 3] = (float)t.r; a[i * 3 + 1] = (float)t.g; a[i * 3 + 2] = (float)t.b;
  }
  geo_set_attr_copy(g, "color", 3, g->count, a);
  free(a);
  return g;
}

typedef struct SedanParts { Material *glass, *alloy, *lens, *tail, *rubber; } SedanParts;
static SedanParts *g_sedan;
static SedanParts *sedan_parts(void) {
  if (g_sedan) return g_sedan;
  SedanParts *P = xcalloc(1, sizeof *P);
  // dark tinted glass: a Fresnel mirror of the sky (clearcoat), the street its dark lower half
  // (the env maps are set to scene.environment before these are used)
  MatDesc d = md_physical();
  d.name = "sedan glass";
  d.color = color_hex(0x0c1115); d.roughness = 0.05; d.metalness = 0.0; d.clearcoat = 1; d.clearcoat_roughness = 0.02;
  d.env_map_intensity = 1.5; d.side = SIDE_DOUBLE; d.env_map = g_env;
  d.prog[MV_PLAIN] = PROG_CAR_REFL_MODERN_GLASS0;
  P->glass = mat_three(&d);
  d = md_standard();
  d.name = "sedan alloy";
  d.color = color_hex(0x8a8d90); d.metalness = 0.75; d.roughness = 0.38; d.side = SIDE_DOUBLE; d.env_map = g_env;
  d.prog[MV_PLAIN] = PROG_STD_DBL;
  P->alloy = mat_three(&d);
  // parked: lamps are unlit smoked glass over a dark reflector, no emissive glow
  d = md_physical();
  d.name = "sedan lens";
  d.color = color_hex(0x3a3e42); d.roughness = 0.15; d.metalness = 0.3; d.clearcoat = 1; d.env_map_intensity = 0.7;
  d.side = SIDE_DOUBLE; d.env_map = g_env;
  d.prog[MV_PLAIN] = PROG_CAR_CLAMP_MODERN_LENS;
  P->lens = mat_three(&d);
  d = md_physical();
  d.name = "sedan tail";
  d.color = color_hex(0x5c0b0d); d.roughness = 0.2; d.clearcoat = 1; d.env_map_intensity = 0.7; d.side = SIDE_DOUBLE;
  d.env_map = g_env;
  d.prog[MV_PLAIN] = PROG_PHYS_DBL_CC;
  P->tail = mat_three(&d);
  d = md_standard();
  d.name = "sedan rubber";
  d.color = color_hex(0x19191a); d.roughness = 0.9; d.side = SIDE_DOUBLE;
  d.prog[MV_PLAIN] = PROG_STD_DBL;
  P->rubber = mat_three(&d);
  g_sedan = P;
  return P;
}
static Material *modern_paint(ProgramId prog, const char *name) {
  MatDesc d = md_physical();
  d.name = name;
  d.color = color_hex(0xffffff); d.vertex_colors = true; d.roughness = 0.3; d.metalness = 0.3; d.clearcoat = 1;
  d.clearcoat_roughness = 0.04; d.env_map = g_env; d.env_map_intensity = 1.4;
  d.prog[MV_PLAIN] = prog;
  return mat_three(&d);
}

static void all_shadows(Node *n, void *u) {
  (void)u;
  if (n->kind == NODE_MESH) n->cast_shadow = n->receive_shadow = true;
}
static Node *make_sedan(uint32_t paint, ModernType type) {
  Shared *S = shared();
  SedanParts *P = sedan_parts();
  ModernGeo *G = modern_geometry(type);
  if (!G->gGlass) {
    G->gGlass = gpu_geometry(G->glass); G->gTrim = gpu_geometry(G->trim); G->gLens = gpu_geometry(G->lens);
    G->gRed = gpu_geometry(G->red); G->gTyre = gpu_geometry(G->tyre); G->gRim = gpu_geometry(G->rim);
  }
  Node *g = node_new(NODE_GROUP, "sedan");
  node_add(g, node_mesh(upload(paint_colors(geo_clone(G->paint), paint)), modern_paint(PROG_CAR_REFL_SEDAN0, "sedan paint")));
  node_add(g, node_mesh(G->gGlass, P->glass));
  node_add(g, node_mesh(G->gTrim, S->dark));
  node_add(g, node_mesh(G->gLens, P->lens));
  node_add(g, node_mesh(G->gRed, P->tail));
  const double wzs[2] = { G->T->wb, -G->T->wb };
  for (int k = 0; k < 2; k++)
    for (int si = 0; si < 2; si++) {
      double s = si ? 1 : -1;
      Node *t = node_mesh(G->gTyre, P->rubber);
      t->position = v3(s * G->wheelX, G->T->r, wzs[k]);
      Node *r = node_mesh(G->gRim, P->alloy);
      r->position = t->position;
      r->scale.x = s;
      node_add(g, t);
      node_add(g, r);
    }
  node_traverse(g, all_shadows, nullptr);
  Node *blob = node_mesh(upload(geo_rotate_x(geo_plane(2.2, G->T->L + 0.6, 1, 1), -PI_D / 2)), S->blob);
  blob->position.y = 0.012;
  blob->render_order = 1;
  node_add(g, blob);
  return g;
}

// Nose-to-tail parked cars along the rest of the lane (a few empty bays), a muted modern palette;
// merged by material so the whole row costs a handful of draw calls.
typedef Vec(CarBox) BoxVec;
typedef struct FleetOpt { double z0, z1, seed; Node *parent; bool clip; } FleetOpt;
enum { FP_BODY, FP_GLASS, FP_TRIM, FP_LENS, FP_RED, FP_TYRE, FP_RIM, FP_BLOB, FP_N };
static void parked_fleet(BoxVec *out, const double *taken, int ntaken, FleetOpt o) {
  Shared *S = shared();
  SedanParts *P = sedan_parts();
  uint32_t a = (uint32_t)o.seed;
#define RND() ((a = (uint32_t)((uint64_t)a * 1664525u + 1013904223u)) / 4294967296.0)
  static const uint32_t cols[9] = { 0xb9bcbf, 0xe4e4e0, 0x1c1d1f, 0x1f2c44, 0x4a4d52, 0x8a1e1e, 0xb8a98a, 0xa6a9ac, 0x2e3033 };
  static const ModernType TYPES[6] = { MT_SEDAN, MT_SEDAN, MT_HATCH, MT_SUV, MT_SUV, MT_PICKUP };
  ModernGeo *types[6];
  for (int i = 0; i < 6; i++) types[i] = modern_geometry(TYPES[i]);
  GeoList parts[FP_N] = {};
  size_t ncol0 = out->len;
  for (double z = o.z0; z < o.z1;) {
    ModernGeo *ty = types[(int)floor(RND() * 6)];
    const ModernDef *T = ty->T;
    double zc = z + T->L / 2;
    z += T->L + 0.7 + RND() * 1.6;
    if (o.clip && zc + T->L / 2 > o.z1) break;
    bool skip = fabs(zc - CAR.z) < 6.5 || fabs(zc - (-10)) < 6.5 || (zc > 22 && zc < 44);
    for (int i = 0; i < ntaken && !skip; i++) skip = fabs(zc - taken[i]) < 5.6;
    if (skip || RND() < 0.2) continue;
    double x = CAR.x + 0.05 + (RND() - 0.5) * 0.3;
    size_t mark[FP_N];
    for (int k = 0; k < FP_N; k++) mark[k] = parts[k].len;
    Quat q = quat_from_euler(euler(0, (RND() - 0.5) * 0.09, 0, EULER_XYZ));
    M4 m4 = m4_compose(v3(x, roadHeight(x), zc), q, v3(1, 1, 1));
    uint32_t col = cols[(int)floor(RND() * 9)];
    gl_push(&parts[FP_BODY], paint_colors(geo_apply_m4(geo_clone(ty->paint), m4), col));
    gl_push(&parts[FP_GLASS], geo_apply_m4(geo_clone(ty->glass), m4));
    gl_push(&parts[FP_TRIM], geo_apply_m4(geo_clone(ty->trim), m4));
    gl_push(&parts[FP_LENS], geo_apply_m4(geo_clone(ty->lens), m4));
    gl_push(&parts[FP_RED], geo_apply_m4(geo_clone(ty->red), m4));
    gl_push(&parts[FP_BLOB], geo_apply_m4(geo_translate(geo_rotate_x(geo_plane(2.2, T->L + 0.6, 1, 1), -PI_D / 2), 0, 0.012, 0), m4));
    const double wzs[2] = { T->wb, -T->wb };
    for (int k = 0; k < 2; k++)
      for (int si = 0; si < 2; si++) {
        double sx = si ? 1 : -1;
        Quat wq = quat_from_euler(euler(RND() * 6.28, 0, 0, EULER_XYZ));
        M4 w = m4_mul(m4, m4_compose(v3(sx * ty->wheelX, T->r, wzs[k]), wq, v3(sx, 1, 1)));   // .premultiply(m4)
        gl_push(&parts[FP_TYRE], geo_apply_m4(geo_clone(ty->tyre), w));
        gl_push(&parts[FP_RIM], geo_apply_m4(geo_clone(ty->rim), w));
      }
    // no parking in the cross-street mouths or on their crosswalks (decided after the random
    // draws, so the rest of the row is unchanged)
    if (blocked_bay(zc, T->L / 2)) {
      for (int k = 0; k < FP_N; k++) {
        for (size_t i = mark[k]; i < parts[k].len; i++) geo_free(parts[k].data[i]);
        parts[k].len = mark[k];
      }
      continue;
    }
    vec_push(out, ((CarBox){ { x - 1.0, 0, zc - T->L / 2 - 0.05 }, { x + 1.0, T->top, zc + T->L / 2 + 0.05 } }));
  }
#undef RND
  if (out->len == ncol0) {
    for (int k = 0; k < FP_N; k++) vec_free(&parts[k]);
    return;
  }
  Material *mats[FP_N] = { modern_paint(PROG_CAR_REFL_FLEET0, "fleet paint"), P->glass, S->dark, P->lens, P->tail, P->rubber, P->alloy, S->blob };
  for (int k = 0; k < FP_N; k++) {
    Geometry *g = k == FP_BLOB ? gl_merge(&parts[k]) : gl_merge_stripped(&parts[k]);
    Node *m = node_mesh(upload(g), mats[k]);
    snprintf(m->name, sizeof m->name, "fleet");
    m->cast_shadow = m->receive_shadow = k != FP_BLOB;
    if (k == FP_BLOB) m->render_order = 1;
    node_add(o.parent, m);
  }
}

// ---- the whole set -----------------------------------------------------------------------------------
typedef struct Mover { HeroCar car; int id; double spin; } Mover;
struct Cars {
  Node *hero;
  Mover movers[3];
  BoxVec colliders;
};

Cars *build_cars(Node *scene, Texture *env) {
  g_env = env;
  Cars *C = xcalloc(1, sizeof *C);
  shared();
  // hero: parked at the west curb, facing south (the direction of the west lane), top down
  HeroCar hero = make_car(0x86cfc1, true, true);
  seat_car(hero.car, CAR.x, CAR.z, 0);
  vec_push(&C->colliders, ((CarBox){ { CAR.x - 1.0, 0, CAR.z - 2.75 }, { CAR.x + 1.0, 1.2, CAR.z + 2.75 } }));
  // a few ordinary parked cars along the lane, gaps between, the hero spot kept clear
  const struct { double z; uint32_t col; ModernType type; } FIXED[4] = {
    { -1.5, 0xb9bcbf, MT_SEDAN }, { -24, 0x1f2c44, MT_SUV }, { -31.5, 0x8a1e1e, MT_HATCH }, { 58, 0xe4e4e0, MT_PICKUP } };
  for (int i = 0; i < 4; i++) {
    Node *s = make_sedan(FIXED[i].col, FIXED[i].type);
    seat_car(s, CAR.x + 0.05, FIXED[i].z, 0);
    node_add(scene, s);
    double hl = MODERN[FIXED[i].type].L / 2 + 0.05;
    vec_push(&C->colliders, ((CarBox){ { CAR.x + 0.05 - 1.0, 0, FIXED[i].z - hl }, { CAR.x + 0.05 + 1.0, MODERN[FIXED[i].type].top, FIXED[i].z + hl } }));
  }
  node_add(scene, hero.car);
  C->hero = hero.car;
  const double taken[4] = { -1.5, -24, -31.5, 58 };
  parked_fleet(&C->colliders, taken, 4, (FleetOpt){ -88, 90, 7331, scene, false });
  // the extended blocks: one row per block, clear of the intersections, culled by distance
  double stops[NCROSS_STREETS + 4];
  int ns = 0;
  stops[ns++] = DISTRICT.zMin - 4;
  for (int i = 0; i < NCROSS_STREETS; i++)
    if (!CROSS_STREETS[i].far && fabs(CROSS_STREETS[i].z) > 100) stops[ns++] = CROSS_STREETS[i].z;
  stops[ns++] = -79;
  stops[ns++] = 79;
  stops[ns++] = DISTRICT.zMax + 4;
  sort_doubles(stops, ns);
  for (int i = 0; i < ns - 1; i++) {
    double a = stops[i], b = stops[i + 1];
    if (a == -79 && b == 79) continue;   // the authored block (row above)
    double za = a <= DISTRICT.zMin - 4 ? a : a + 17, zb = b >= DISTRICT.zMax + 4 ? b : b - 16;
    if (zb - za < 8) continue;
    Node *g = node_new(NODE_GROUP, "fleet block");
    parked_fleet(&C->colliders, nullptr, 0, (FleetOpt){ za, zb, 9100 + i * 77, g, true });
    node_add(scene, g);
    lod_register(g, za, zb, LOD_CARS);
  }
  // moving cars: one mesh per sounding audio car (hidden when none is passing)
  const uint32_t pool[3] = { 0xe8a4b8, 0xf2e6c4, 0x9fc8e0 };
  for (int i = 0; i < 3; i++) {
    C->movers[i] = (Mover){ make_car(pool[i], false, false), -1, 0 };
    C->movers[i].car.car->visible = false;
    node_add(scene, C->movers[i].car.car);
  }
  return C;
}

const CarBox *cars_colliders(const Cars *c, int *n) {
  *n = (int)c->colliders.len;
  return c->colliders.data;
}
Node *cars_hero(const Cars *c) { return c->hero; }

void cars_update(Cars *C, double dt, const CarPass *cars, int n) {
  // live: active passes between the ends
  bool live[64] = {};
  CHECK(n <= 64);
  for (int i = 0; i < n; i++) live[i] = cars[i].active && cars[i].progress > 0 && cars[i].progress < 1;
  for (int m = 0; m < 3; m++) {
    bool found = false;
    for (int i = 0; i < n; i++) if (live[i] && cars[i].id == C->movers[m].id) found = true;
    if (!found) C->movers[m].id = -1;
  }
  for (int i = 0; i < n; i++) {
    if (!live[i]) continue;
    const CarPass *c = &cars[i];
    Mover *mv = nullptr;
    for (int m = 0; m < 3 && !mv; m++) if (C->movers[m].id == c->id) mv = &C->movers[m];
    for (int m = 0; m < 3 && !mv; m++) if (C->movers[m].id == -1) mv = &C->movers[m];
    if (!mv) continue;
    mv->id = c->id;
    seat_car(mv->car.car, c->x, c->z, c->dir > 0 ? 0 : PI_D);
    mv->spin += (c->speed * dt) / WHEEL_R;
    for (int w = 0; w < 4; w++) node_set_rotation(mv->car.wheels[w], mv->spin, 0, 0);
  }
  for (int m = 0; m < 3; m++) C->movers[m].car.car->visible = C->movers[m].id != -1;
}
