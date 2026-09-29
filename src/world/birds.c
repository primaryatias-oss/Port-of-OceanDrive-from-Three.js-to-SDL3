// Port of src/world/birds.js. Wings are two-bone (shoulder, wrist); head, legs, tail and the
// folded wings are rigid parts. The vertex shader (shaders/custom/od_bird_lit.*) poses every
// part from three per-instance pose vectors that the behaviours below write each frame. The sun
// shadow map is static, so ground birds get planar-projected sun shadows instead
// (od_bird_shadow.*). 5 lit + 3 shadow draw calls in total.
//
// Exactness notes: one shared pose P is written by every bird in turn, like the JS (a field a
// behaviour leaves alone keeps the previous bird's value); every rnd() call is sequenced in
// the JS evaluation order.
#include "world/birds.h"

#include <math.h>

#include "gfx/three_mat.h"
#include "world/beach.h"
#include "world/layout.h"
#include "world/palms.h"
#include "world/sky.h"

static constexpr double TAU = PI_D * 2;

static double lerp(double a, double b, double t) { return a + (b - a) * t; }
static double smooth(double a, double b, double v) {
  double t = clampd((v - a) / (b - a), 0, 1);
  return t * t * (3 - 2 * t);
}
static double wrap_pi(double a) { return a - TAU * floor((a + PI_D) / TAU); }
static double approach(double v, double target, double rate, double dt) { return v + (target - v) * fmin(1, rate * dt); }
// yaw (forward = (sin yaw, 0, cos yaw)) for a compass bearing (0 = north = -z, 90 = east = +x)
static double yaw_of_compass(double deg) {
  double a = (deg * PI_D) / 180;
  return atan2(sin(a), -cos(a));
}

typedef struct Counts { int pelican, gull, sand, grackle, flock, frigate; } Counts;
static const Counts COUNTS_HIGH = { 6, 12, 10, 5, 9, 1 };
static const Counts COUNTS_MEDIUM = { 5, 9, 8, 4, 7, 1 };
static const Counts COUNTS_LOW = { 4, 6, 6, 3, 0, 0 };
static Counts counts_of(Tier t) { return t == TIER_LOW ? COUNTS_LOW : t == TIER_MEDIUM ? COUNTS_MEDIUM : COUNTS_HIGH; }

// part ids (vertex attribute aPart)
enum { BODY = 0, WING_IN = 1, WING_OUT = 2, FOLD = 3, HEAD = 4, LEG = 5, TAIL = 6 };

// ---------------------------------------------------------------------------------------------
// geometry

// a vertex's colours: b and u default to a, the feather coordinates to 0
typedef struct VC { Color a, b, u; bool hb, hu; double fe[3]; } VC;
static VC c_a(Color a) { return (VC){ .a = a }; }
static VC c_ab(Color a, Color b) { return (VC){ .a = a, .b = b, .hb = true }; }
static VC c_au(Color a, Color u) { return (VC){ .a = a, .u = u, .hu = true }; }
static VC c_abu(Color a, Color b, Color u) { return (VC){ .a = a, .b = b, .u = u, .hb = true, .hu = true }; }

typedef struct Geo { DVec pos, part, cA, cB, cU, fe; U32Vec idx; } Geo;

static uint32_t gv(Geo *g, double x, double y, double z, int part, VC c) {
  Color b = c.hb ? c.b : c.a, u = c.hu ? c.u : c.a;
  double p[3] = { x, y, z }, ca[3] = { c.a.r, c.a.g, c.a.b }, cb[3] = { b.r, b.g, b.b }, cu[3] = { u.r, u.g, u.b };
  vec_append(&g->pos, p, 3);
  vec_push(&g->part, (double)part);
  vec_append(&g->cA, ca, 3);
  vec_append(&g->cB, cb, 3);
  vec_append(&g->cU, cu, 3);
  vec_append(&g->fe, c.fe, 3);
  return (uint32_t)(g->part.len - 1);
}
static void tri2(Geo *g, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f) {
  uint32_t q[6] = { a, b, c, d, e, f };
  vec_append(&g->idx, q, 6);
}

// loft station [z, halfWidth, halfHeight, yCentre, xCentre = 0] (the tables leave x out, as the JS does)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
typedef struct St { double z, w, h, y, x; } St;
typedef struct LoftQ { int i; double z, up, side; int n; } LoftQ;
typedef VC (*LoftCol)(LoftQ q);

// closed loft along z
static void loft(Geo *g, const St *st, int n, int part, LoftCol col, int seg) {
  uint32_t *rings = xmalloc((size_t)n * (size_t)seg * sizeof *rings);
  for (int i = 0; i < n; i++)
    for (int k = 0; k < seg; k++) {
      double a = ((double)k / seg) * TAU, cx = cos(a), sy = sin(a);
      rings[i * seg + k] = gv(g, st[i].x + st[i].w * cx, st[i].y + st[i].h * sy, st[i].z, part,
                              col((LoftQ){ i, st[i].z, sy, cx, n }));
    }
  for (int i = 0; i < n - 1; i++)
    for (int k = 0; k < seg; k++) {
      uint32_t a = rings[i * seg + k], b = rings[i * seg + (k + 1) % seg], c = rings[(i + 1) * seg + k],
               d = rings[(i + 1) * seg + (k + 1) % seg];
      tri2(g, a, b, c, b, d, c);
    }
  free(rings);
}

// grid of vertex indices (nr rows x nc columns) -> triangles; front face up when rows run +x
// and columns run -z
static void grid(Geo *g, const uint32_t *rows, int nr, int nc, bool flip) {
  for (int r = 0; r < nr - 1; r++)
    for (int c = 0; c < nc - 1; c++) {
      uint32_t a = rows[r * nc + c], b = rows[(r + 1) * nc + c], d = rows[r * nc + c + 1], e = rows[(r + 1) * nc + c + 1];
      if (flip) tri2(g, a, d, b, b, d, e);
      else tri2(g, a, b, d, b, e, d);
    }
}

// flat plate from rows of [x, y, z] points (rows root -> tip along -z, points right -> left)
typedef VC (*PlateCol)(double r, double c);
static void plate(Geo *g, const double (*pts)[3], int nr, int nc, int part, PlateCol col) {
  uint32_t *rows = xmalloc((size_t)nr * (size_t)nc * sizeof *rows);
  for (int r = 0; r < nr; r++)
    for (int c = 0; c < nc; c++) {
      const double *p = pts[r * nc + c];
      rows[r * nc + c] = gv(g, p[0], p[1], p[2], part, col((double)r / (nr - 1), (double)c / (nc - 1)));
    }
  grid(g, rows, nr, nc, false);
  free(rows);
}

// both wings: stations [x, zLead, zTrail]; stations up to `wrist` are the arm, from it the hand
typedef VC (*WingCol)(double xf, double v);
typedef struct WingSpec { const double (*st)[3]; int n, wrist; double y, camber; int segs; double droop; WingCol col; } WingSpec;
static void wings(Geo *g, WingSpec w) {
  int segs = w.segs ? w.segs : 4;
  double xw = w.st[w.wrist][0], xt = w.st[w.n - 1][0];
  int na = w.wrist + 1, nh = w.n - w.wrist, nc = segs + 1;
  uint32_t *arm = xmalloc((size_t)na * (size_t)nc * sizeof *arm), *hand = xmalloc((size_t)nh * (size_t)nc * sizeof *hand);
  for (int si = 0; si < 2; si++) {
    double s = si ? -1 : 1;
    int ra = 0, rh = 0;
    for (int i = 0; i < w.n; i++) {
      double x = w.st[i][0], lead = w.st[i][1], trail = w.st[i][2];
      for (int outer = 0; outer < 2; outer++) {
        if (outer ? i < w.wrist : i > w.wrist) continue;   // arm rows then hand rows (both at the wrist)
        uint32_t *row = outer ? &hand[rh++ * nc] : &arm[ra++ * nc];
        for (int j = 0; j <= segs; j++) {
          double v = (double)j / segs, xf = x / xt;
          double yy = w.y + w.camber * (lead - trail) * sin(PI_D * v) * (1 - 0.6 * xf) - w.droop * xf * xf;
          VC c = w.col(xf, v);
          if (outer) { c.fe[0] = (x - xw) / (xt - xw); c.fe[1] = v; c.fe[2] = 1; }
          else { c.fe[0] = 0; c.fe[1] = v; c.fe[2] = 0; }
          row[j] = gv(g, s * x, yy, lead + (trail - lead) * v, outer ? WING_OUT : WING_IN, c);
        }
      }
    }
    grid(g, arm, na, nc, s < 0);
    grid(g, hand, nh, nc, s < 0);
  }
  free(arm);
  free(hand);
}

// thin leg from hip to ankle plus a toe fan; mirrored for both sides
static void legs(Geo *g, const double hip[3], const double foot[3], double r, double toe, double hind, VC col) {
  for (int si = 0; si < 2; si++) {
    double s = si ? -1 : 1;
    V3 p0 = { s * hip[0], hip[1], hip[2] }, p1 = { s * foot[0], foot[1], foot[2] };
    V3 ax = v3_norm(v3_sub(p1, p0));
    V3 u = v3_norm(v3_cross(v3(1, 0, 0), ax)), w = v3_cross(ax, u);
    uint32_t rings[2][4];
    for (int k = 0; k < 2; k++) {
      V3 p = k ? p1 : p0;
      for (int i = 0; i < 4; i++) {
        double a = ((double)i / 4) * TAU, rr = r * (k ? 0.8 : 1.6);
        V3 q = v3_add_scaled(v3_add_scaled(p, u, cos(a) * rr), w, sin(a) * rr);
        rings[k][i] = gv(g, q.x, q.y, q.z, LEG, col);
      }
    }
    for (int i = 0; i < 4; i++) {
      uint32_t a = rings[0][i], b = rings[0][(i + 1) % 4], c = rings[1][i], d = rings[1][(i + 1) % 4];
      tri2(g, a, b, c, b, d, c);
    }
    double fx = s * foot[0], fy = 0.003, fz = foot[2];
    uint32_t heel = gv(g, fx, fy, fz - hind, LEG, col);
    static const double TOES[3][2] = { { -0.55, 0.85 }, { 0, 1 }, { 0.55, 0.85 } };
    uint32_t toes[3];
    for (int t = 0; t < 3; t++) toes[t] = gv(g, fx + TOES[t][0] * toe, fy, fz + TOES[t][1] * toe, LEG, col);
    uint32_t mid = gv(g, fx, fy + 0.002, fz, LEG, col);
    tri2(g, heel, toes[0], mid, mid, toes[0], toes[1]);
    tri2(g, mid, toes[1], toes[2], heel, mid, toes[2]);
  }
}

static Geometry *geo_build(Geo *G) {
  Geometry *g = geo_new();
  int n = (int)G->part.len;
  geo_set_attr_d(g, "position", 3, n, G->pos.data);
  geo_set_attr_d(g, "aPart", 1, n, G->part.data);
  geo_set_attr_d(g, "cA", 3, n, G->cA.data);
  geo_set_attr_d(g, "cB", 3, n, G->cB.data);
  geo_set_attr_d(g, "cU", 3, n, G->cU.data);
  geo_set_attr_d(g, "aFeather", 3, n, G->fe.data);
  geo_set_index(g, G->idx.data, (int)G->idx.len);
  geo_compute_vertex_normals(g);
  vec_free(&G->pos); vec_free(&G->part); vec_free(&G->cA); vec_free(&G->cB); vec_free(&G->cU); vec_free(&G->fe);
  vec_free(&G->idx);
  return g;
}

// folded wings: a slim blade lying along each upper flank (stations carry their own x)
static void fold_blades(Geo *g, const St *st, int n, LoftCol col) {
  St m[8];
  CHECK(n <= 8);
  for (int si = 0; si < 2; si++) {
    double s = si ? -1 : 1;
    for (int i = 0; i < n; i++) m[i] = (St){ st[i].z, st[i].w, st[i].h, st[i].y, s * st[i].x };
    loft(g, m, n, FOLD, col, 6);
  }
}

typedef struct Rig {
  double shoulder[3], wrist[3], neck[3], hip[3], tail[3];
  double leg_swing, fingers, finger_len, height;   // fingers ?? 0, fingerLen ?? 0.3, height ?? 0.2
} Rig;
typedef struct SpeciesGeo { Geometry *geo; Rig rig; } SpeciesGeo;

#define NST(a) ((int)ARRAY_LEN(a))

// ---- laughing gull (palette A, breeding: black hood, dark mantle) / ring-billed gull (B) ----
static Color G_WHITE, G_MANTLE_A, G_MANTLE_B, G_HOOD, G_BLACK, G_UNDERTIP, G_BILL_A, G_BILL_B, G_LEG_A, G_LEG_B;
static VC gull_body(LoftQ q) {
  double top = smooth(0.2, 0.6, q.up) * (q.z < 0.07 ? 1 : 0);
  return c_ab(color_lerp(G_WHITE, G_MANTLE_A, top), color_lerp(G_WHITE, G_MANTLE_B, top));
}
static VC gull_head(LoftQ q) { return c_ab(q.z > 0.09 ? G_HOOD : G_WHITE, G_WHITE); }
static VC gull_bill(LoftQ q) { return c_ab(G_BILL_A, q.i == 2 ? G_BLACK : G_BILL_B); }
static VC gull_tail(double r, double c) { (void)r; (void)c; return c_a(G_WHITE); }
static VC gull_wing(double xf, double v) {
  bool trail = v > 0.9 && xf < 0.66;
  return c_abu(xf > 0.7 ? G_BLACK : trail ? G_WHITE : G_MANTLE_A,
               xf > 0.8 ? (xf > 0.94 && v > 0.2 && v < 0.8 ? G_WHITE : G_BLACK) : trail ? G_WHITE : G_MANTLE_B,
               xf > 0.8 ? G_UNDERTIP : G_WHITE);
}
static VC gull_fold(LoftQ q) {
  return c_ab(q.z < -0.1 ? G_BLACK : G_MANTLE_A, q.z < -0.19 ? G_WHITE : q.z < -0.11 ? G_BLACK : G_MANTLE_B);
}
static SpeciesGeo gull_geo(void) {
  Geo g = {};
  G_WHITE = color_hex(0xe6e6e2); G_MANTLE_A = color_hex(0x585c63); G_MANTLE_B = color_hex(0xa4abb3); G_HOOD = color_hex(0x1b1b1e);
  G_BLACK = color_hex(0x141416); G_UNDERTIP = color_hex(0x3a3a3d); G_BILL_A = color_hex(0x5e1818); G_BILL_B = color_hex(0xd6b444);
  G_LEG_A = color_hex(0x331818); G_LEG_B = color_hex(0xc4a23c);
  static const St body[] = { { -0.14, 0.012, 0.012, 0.15 }, { -0.10, 0.034, 0.03, 0.153 }, { -0.04, 0.054, 0.05, 0.158 },
    { 0.02, 0.06, 0.057, 0.163 }, { 0.065, 0.05, 0.052, 0.17 }, { 0.095, 0.032, 0.036, 0.187 }, { 0.11, 0, 0, 0.195 } };
  loft(&g, body, NST(body), BODY, gull_body, 12);
  static const St head[] = { { 0.07, 0.02, 0.022, 0.193 }, { 0.10, 0.026, 0.028, 0.212 }, { 0.125, 0.029, 0.03, 0.222 },
    { 0.148, 0.025, 0.025, 0.225 }, { 0.164, 0.014, 0.015, 0.222 }, { 0.17, 0, 0, 0.221 } };
  loft(&g, head, NST(head), HEAD, gull_head, 10);
  static const St bill[] = { { 0.16, 0.0075, 0.009, 0.221 }, { 0.185, 0.006, 0.0075, 0.2205 }, { 0.203, 0.0042, 0.0058, 0.219 },
    { 0.215, 0.0018, 0.0032, 0.2165 }, { 0.218, 0, 0, 0.215 } };
  loft(&g, bill, NST(bill), HEAD, gull_bill, 6);
  static const double tail[][3] = { { 0.03, 0.153, -0.115 }, { 0, 0.155, -0.115 }, { -0.03, 0.153, -0.115 },
                                    { 0.05, 0.15, -0.2 }, { 0, 0.152, -0.205 }, { -0.05, 0.15, -0.2 } };
  plate(&g, tail, 2, 3, TAIL, gull_tail);
  static const double wst[][3] = { { 0.02, 0.07, -0.065 }, { 0.10, 0.075, -0.07 }, { 0.17, 0.078, -0.06 }, { 0.22, 0.07, -0.05 },
    { 0.30, 0.052, -0.05 }, { 0.36, 0.04, -0.047 }, { 0.41, 0.028, -0.044 }, { 0.45, 0.008, -0.04 }, { 0.48, -0.01, -0.037 },
    { 0.50, -0.026, -0.034 } };
  wings(&g, (WingSpec){ wst, NST(wst), 3, 0.18, 0.07, 4, 0, gull_wing });
  static const St fold[] = { { 0.06, 0.004, 0.016, 0.186, 0.047 }, { 0.0, 0.012, 0.027, 0.19, 0.057 }, { -0.07, 0.012, 0.024, 0.183, 0.051 },
    { -0.14, 0.008, 0.012, 0.172, 0.03 }, { -0.2, 0.004, 0.005, 0.165, 0.014 }, { -0.215, 0, 0, 0.164, 0.01 } };
  fold_blades(&g, fold, NST(fold), gull_fold);
  legs(&g, (double[]){ 0.022, 0.115, 0.0 }, (double[]){ 0.022, 0.004, 0.006 }, 0.0045, 0.034, 0.012, c_ab(G_LEG_A, G_LEG_B));
  return (SpeciesGeo){ geo_build(&g), { { 0.035, 0.185, 0.035 }, { 0.22, 0.18, 0.0 }, { 0, 0.19, 0.085 }, { 0.022, 0.115, 0.0 },
                                        { 0, 0.153, -0.115 }, 0.45, 0, 0.3, 0.23 } };
}

// ---- brown pelican (flight only): pale head on a drawn-in neck, long bill and pouch, broad
// fingered wings ----
static Color P_UNDER, P_BACK, P_HEADC, P_NECK, P_NAPE, P_BILL, P_BILLTIP, P_POUCH, P_DARK, P_COVERT, P_UCOV;
static VC pel_body(LoftQ q) { return c_a(color_lerp(P_UNDER, P_BACK, smooth(0.1, 0.7, q.up))); }
static VC pel_head(LoftQ q) { return c_a(q.z > 0.3 ? P_HEADC : q.up > 0.4 ? P_NAPE : P_NECK); }
static VC pel_bill(LoftQ q) { return c_a(q.i >= 4 ? P_BILLTIP : P_BILL); }
static VC pel_pouch(LoftQ q) { (void)q; return c_a(P_POUCH); }
static VC pel_tail(double r, double c) { (void)r; (void)c; return c_a(P_DARK); }
static VC pel_wing(double xf, double v) {
  return c_au(xf > 0.53 || v > 0.55 ? P_DARK : P_COVERT, xf < 0.5 && v < 0.4 ? P_UCOV : P_DARK);
}
static SpeciesGeo pelican_geo(void) {
  Geo g = {};
  P_UNDER = color_hex(0x3a342e); P_BACK = color_hex(0x6e675e); P_HEADC = color_hex(0xd8cca6); P_NECK = color_hex(0xe0dbd0);
  P_NAPE = color_hex(0x4a3326); P_BILL = color_hex(0x8a7e6c); P_BILLTIP = color_hex(0xa06a3a); P_POUCH = color_hex(0x3e342c);
  P_DARK = color_hex(0x221f1c); P_COVERT = color_hex(0x7a736b); P_UCOV = color_hex(0x4c4640);
  static const St body[] = { { -0.40, 0.025, 0.025, 0.0 }, { -0.32, 0.08, 0.07, 0.0 }, { -0.18, 0.14, 0.12, 0.0 }, { -0.02, 0.16, 0.135, 0.01 },
    { 0.12, 0.135, 0.125, 0.03 }, { 0.22, 0.095, 0.105, 0.06 }, { 0.29, 0.055, 0.07, 0.085 }, { 0.32, 0, 0, 0.09 } };
  loft(&g, body, NST(body), BODY, pel_body, 12);
  static const St head[] = { { 0.16, 0.07, 0.08, 0.10 }, { 0.25, 0.076, 0.086, 0.14 }, { 0.33, 0.068, 0.075, 0.16 }, { 0.39, 0.058, 0.062, 0.162 },
    { 0.43, 0.04, 0.045, 0.157 }, { 0.45, 0, 0, 0.152 } };
  loft(&g, head, NST(head), HEAD, pel_head, 10);
  static const St bill[] = { { 0.41, 0.032, 0.026, 0.158 }, { 0.5, 0.03, 0.022, 0.146 }, { 0.6, 0.025, 0.018, 0.128 }, { 0.7, 0.02, 0.014, 0.108 },
    { 0.77, 0.016, 0.011, 0.093 }, { 0.8, 0.009, 0.009, 0.085 }, { 0.806, 0, 0, 0.08 } };
  loft(&g, bill, NST(bill), HEAD, pel_bill, 6);
  static const St pouch[] = { { 0.40, 0.028, 0.03, 0.125 }, { 0.52, 0.026, 0.03, 0.113 }, { 0.64, 0.02, 0.02, 0.1 }, { 0.73, 0, 0, 0.09 } };
  loft(&g, pouch, NST(pouch), HEAD, pel_pouch, 6);
  static const double tail[][3] = { { 0.07, 0, -0.36 }, { 0, 0.005, -0.36 }, { -0.07, 0, -0.36 },
                                    { 0.11, -0.005, -0.52 }, { 0, 0, -0.54 }, { -0.11, -0.005, -0.52 } };
  plate(&g, tail, 2, 3, TAIL, pel_tail);
  static const double wst[][3] = { { 0.08, 0.17, -0.2 }, { 0.3, 0.19, -0.21 }, { 0.5, 0.18, -0.18 }, { 0.55, 0.17, -0.17 },
    { 0.72, 0.135, -0.15 }, { 0.86, 0.105, -0.14 }, { 0.98, 0.075, -0.13 }, { 1.05, 0.04, -0.105 } };
  wings(&g, (WingSpec){ wst, NST(wst), 3, 0.03, 0.06, 4, 0.03, pel_wing });
  return (SpeciesGeo){ geo_build(&g), { { 0.1, 0.05, 0.07 }, { 0.55, 0.03, 0.0 }, { 0, 0.12, 0.2 }, { 0, 0, 0 }, { 0, 0, -0.36 },
                                        0, 5, 0.4, 0.2 } };
}

// ---- magnificent frigatebird: black, long angular crooked wings, deeply forked tail ----
static Color F_BLACK, F_BILL;
static VC fr_black(LoftQ q) { (void)q; return c_a(F_BLACK); }
static VC fr_bill(LoftQ q) { (void)q; return c_a(F_BILL); }
static VC fr_tail(double r, double c) { (void)r; (void)c; return c_a(F_BLACK); }
static VC fr_wing(double xf, double v) { (void)xf; (void)v; return c_a(F_BLACK); }
static SpeciesGeo frigate_geo(void) {
  Geo g = {};
  F_BLACK = color_hex(0x101012); F_BILL = color_hex(0x70727a);
  static const St body[] = { { -0.24, 0.02, 0.02, 0 }, { -0.15, 0.055, 0.055, 0 }, { 0.0, 0.07, 0.07, 0 }, { 0.12, 0.055, 0.06, 0.01 },
    { 0.2, 0.035, 0.04, 0.02 }, { 0.23, 0, 0, 0.025 } };
  loft(&g, body, NST(body), BODY, fr_black, 10);
  static const St head[] = { { 0.17, 0.03, 0.035, 0.025 }, { 0.23, 0.035, 0.036, 0.032 }, { 0.28, 0.024, 0.024, 0.032 }, { 0.3, 0, 0, 0.03 } };
  loft(&g, head, NST(head), HEAD, fr_black, 8);
  static const St bill[] = { { 0.28, 0.009, 0.011, 0.03 }, { 0.38, 0.007, 0.008, 0.027 }, { 0.44, 0.004, 0.006, 0.022 }, { 0.455, 0, 0, 0.016 } };
  loft(&g, bill, NST(bill), HEAD, fr_bill, 6);
  static const double rows[3][2][3] = { { { 0.035, 0, -0.2 }, { -0.005, 0, -0.2 } }, { { 0.075, 0, -0.45 }, { 0.05, 0, -0.45 } },
                                        { { 0.105, 0, -0.68 }, { 0.095, 0, -0.68 } } };
  for (int si = 0; si < 2; si++) {
    double pts[6][3];
    for (int r = 0; r < 3; r++)
      for (int c = 0; c < 2; c++) {
        // s < 0: each row mirrored in x and reversed
        const double *p = si ? rows[r][1 - c] : rows[r][c];
        pts[r * 2 + c][0] = si ? -p[0] : p[0];
        pts[r * 2 + c][1] = p[1];
        pts[r * 2 + c][2] = p[2];
      }
    plate(&g, (const double (*)[3])pts, 3, 2, TAIL, fr_tail);
  }
  static const double wst[][3] = { { 0.05, 0.12, -0.14 }, { 0.25, 0.13, -0.1 }, { 0.42, 0.14, -0.08 }, { 0.47, 0.13, -0.075 },
    { 0.65, 0.07, -0.08 }, { 0.82, 0.0, -0.08 }, { 0.97, -0.08, -0.1 }, { 1.1, -0.17, -0.13 } };
  wings(&g, (WingSpec){ wst, NST(wst), 3, 0.02, 0.05, 4, 0, fr_wing });
  return (SpeciesGeo){ geo_build(&g), { { 0.05, 0.02, 0.05 }, { 0.47, 0.02, 0.02 }, { 0, 0.03, 0.18 }, { 0, 0, 0 }, { 0, 0, -0.2 },
                                        0, 0, 0.3, 0.2 } };
}

// ---- sanderling: pale grey above, white below, black bill and legs ----
static Color S_WHITE, S_GREY, S_DARK, S_BLACK;
static VC sa_body(LoftQ q) { return c_a(color_lerp(S_WHITE, S_GREY, smooth(0.05, 0.45, q.up))); }
static VC sa_head(LoftQ q) { return c_a(q.up > 0.35 ? S_GREY : S_WHITE); }
static VC sa_bill(LoftQ q) { (void)q; return c_a(S_BLACK); }
static VC sa_tail(double r, double c) { (void)r; return c_a(c == 0.5 ? S_DARK : S_GREY); }
static VC sa_wing(double xf, double v) {
  bool bar = v > 0.45 && v < 0.75 && xf > 0.15 && xf < 0.8;
  return c_au(bar ? S_WHITE : v < 0.25 || xf > 0.62 ? S_DARK : S_GREY, S_WHITE);
}
static VC sa_fold(LoftQ q) { return c_a(q.z < -0.05 ? S_DARK : S_GREY); }
static SpeciesGeo sanderling_geo(void) {
  Geo g = {};
  S_WHITE = color_hex(0xe8e8e3); S_GREY = color_hex(0x9b9892); S_DARK = color_hex(0x3b3a37); S_BLACK = color_hex(0x111111);
  static const St body[] = { { -0.075, 0.008, 0.008, 0.075 }, { -0.055, 0.02, 0.019, 0.076 }, { -0.02, 0.03, 0.029, 0.078 },
    { 0.02, 0.031, 0.03, 0.08 }, { 0.045, 0.024, 0.025, 0.086 }, { 0.06, 0, 0, 0.092 } };
  loft(&g, body, NST(body), BODY, sa_body, 10);
  static const St head[] = { { 0.04, 0.013, 0.014, 0.09 }, { 0.057, 0.017, 0.018, 0.098 }, { 0.072, 0.016, 0.016, 0.1 },
    { 0.083, 0.009, 0.01, 0.1 }, { 0.087, 0, 0, 0.0995 } };
  loft(&g, head, NST(head), HEAD, sa_head, 8);
  static const St bill[] = { { 0.082, 0.0028, 0.003, 0.0995 }, { 0.1, 0.002, 0.002, 0.098 }, { 0.111, 0, 0, 0.0965 } };
  loft(&g, bill, NST(bill), HEAD, sa_bill, 5);
  static const double tail[][3] = { { 0.012, 0.078, -0.06 }, { 0, 0.079, -0.06 }, { -0.012, 0.078, -0.06 },
                                    { 0.016, 0.076, -0.084 }, { 0, 0.077, -0.086 }, { -0.016, 0.076, -0.084 } };
  plate(&g, tail, 2, 3, TAIL, sa_tail);
  static const double wst[][3] = { { 0.01, 0.035, -0.035 }, { 0.05, 0.035, -0.033 }, { 0.08, 0.03, -0.03 }, { 0.13, 0.015, -0.03 },
    { 0.17, -0.002, -0.028 }, { 0.2, -0.02, -0.026 } };
  wings(&g, (WingSpec){ wst, NST(wst), 2, 0.085, 0.05, 4, 0, sa_wing });
  static const St fold[] = { { 0.035, 0.003, 0.01, 0.087, 0.022 }, { 0.0, 0.007, 0.016, 0.09, 0.028 }, { -0.05, 0.006, 0.012, 0.085, 0.022 },
    { -0.085, 0.003, 0.004, 0.08, 0.01 }, { -0.09, 0, 0, 0.079, 0.008 } };
  fold_blades(&g, fold, NST(fold), sa_fold);
  legs(&g, (double[]){ 0.01, 0.055, 0.0 }, (double[]){ 0.01, 0.002, 0.004 }, 0.002, 0.016, 0, c_a(S_BLACK));
  return (SpeciesGeo){ geo_build(&g), { { 0.016, 0.088, 0.02 }, { 0.08, 0.085, 0.0 }, { 0, 0.09, 0.045 }, { 0.01, 0.055, 0.0 },
                                        { 0, 0.078, -0.06 }, 0.7, 0, 0.3, 0.11 } };
}

// ---- boat-tailed grackle: glossy blue-black male (A), brown female (B), long keeled tail ----
static Color K_BLACK, K_BLUE, K_F_TOP, K_F_UNDER, K_F_DARK, K_BILL;
static VC gr_body(LoftQ q) {
  return c_ab(color_lerp(K_BLACK, K_BLUE, smooth(0, 0.8, q.up)), color_lerp(K_F_UNDER, K_F_TOP, smooth(-0.2, 0.5, q.up)));
}
static VC gr_head(LoftQ q) { return c_ab(color_lerp(K_BLACK, K_BLUE, 0.5), q.up > 0.3 ? K_F_TOP : K_F_UNDER); }
static VC gr_bill(LoftQ q) { (void)q; return c_a(K_BILL); }
static VC gr_tail(double r, double c) { (void)r; (void)c; return c_ab(K_BLACK, K_F_DARK); }
static VC gr_wing(double xf, double v) { (void)xf; (void)v; return c_ab(K_BLACK, K_F_DARK); }
static VC gr_fold(LoftQ q) { (void)q; return c_ab(K_BLUE, K_F_DARK); }
static SpeciesGeo grackle_geo(void) {
  Geo g = {};
  K_BLACK = color_hex(0x0c0d14); K_BLUE = color_hex(0x121628); K_F_TOP = color_hex(0x4a3a2a); K_F_UNDER = color_hex(0x806a4c);
  K_F_DARK = color_hex(0x2e241a); K_BILL = color_hex(0x0a0a0a);
  static const St body[] = { { -0.085, 0.012, 0.012, 0.1 }, { -0.06, 0.027, 0.027, 0.1 }, { -0.02, 0.04, 0.038, 0.102 },
    { 0.025, 0.043, 0.041, 0.107 }, { 0.06, 0.034, 0.035, 0.116 }, { 0.085, 0, 0, 0.125 } };
  loft(&g, body, NST(body), BODY, gr_body, 10);
  static const St head[] = { { 0.055, 0.02, 0.024, 0.125 }, { 0.08, 0.025, 0.027, 0.137 }, { 0.1, 0.022, 0.024, 0.14 },
    { 0.115, 0.012, 0.013, 0.139 }, { 0.12, 0, 0, 0.138 } };
  loft(&g, head, NST(head), HEAD, gr_head, 8);
  static const St bill[] = { { 0.11, 0.0055, 0.007, 0.138 }, { 0.135, 0.0035, 0.0045, 0.136 }, { 0.156, 0, 0, 0.133 } };
  loft(&g, bill, NST(bill), HEAD, gr_bill, 5);
  static const double tail[][3] = { { 0.018, 0.104, -0.07 }, { 0, 0.099, -0.07 }, { -0.018, 0.104, -0.07 },
                                    { 0.028, 0.102, -0.17 }, { 0, 0.09, -0.17 }, { -0.028, 0.102, -0.17 },
                                    { 0.033, 0.098, -0.27 }, { 0, 0.085, -0.275 }, { -0.033, 0.098, -0.27 } };
  plate(&g, tail, 3, 3, TAIL, gr_tail);
  static const double wst[][3] = { { 0.02, 0.05, -0.05 }, { 0.07, 0.052, -0.048 }, { 0.1, 0.048, -0.045 }, { 0.16, 0.03, -0.042 },
    { 0.21, 0.01, -0.038 }, { 0.25, -0.012, -0.03 } };
  wings(&g, (WingSpec){ wst, NST(wst), 2, 0.118, 0.05, 4, 0, gr_wing });
  static const St fold[] = { { 0.05, 0.004, 0.014, 0.12, 0.038 }, { 0.0, 0.01, 0.022, 0.122, 0.043 }, { -0.06, 0.008, 0.016, 0.113, 0.034 },
    { -0.1, 0.004, 0.006, 0.106, 0.018 }, { -0.11, 0, 0, 0.105, 0.012 } };
  fold_blades(&g, fold, NST(fold), gr_fold);
  legs(&g, (double[]){ 0.014, 0.07, 0.0 }, (double[]){ 0.014, 0.003, 0.006 }, 0.003, 0.022, 0.016, c_a(K_BILL));
  return (SpeciesGeo){ geo_build(&g), { { 0.03, 0.12, 0.04 }, { 0.1, 0.118, 0.0 }, { 0, 0.125, 0.065 }, { 0.014, 0.07, 0.0 },
                                        { 0, 0.1, -0.07 }, 0.5, 4, 0.3, 0.15 } };
}

// ---------------------------------------------------------------------------------------------
// materials

static void rig_uniforms(Material *m, const Rig *r) {
  mat_set_vec3(m, "uShoulder", v3(r->shoulder[0], r->shoulder[1], r->shoulder[2]));
  mat_set_vec3(m, "uWrist", v3(r->wrist[0], r->wrist[1], r->wrist[2]));
  mat_set_vec3(m, "uNeck", v3(r->neck[0], r->neck[1], r->neck[2]));
  mat_set_vec3(m, "uHip", v3(r->hip[0], r->hip[1], r->hip[2]));
  mat_set_vec3(m, "uTailP", v3(r->tail[0], r->tail[1], r->tail[2]));
  mat_set_float(m, "uLegSwing", r->leg_swing);
}

typedef struct LitOpts { double rough, rim, trans; uint32_t tint; bool has_tint; } LitOpts;

static Material *lit_material(const Rig *rig, LitOpts o) {
  MatDesc d = md_standard();
  d.name = "od-bird-lit";
  d.color = color_hex(0xffffff);
  d.roughness = o.rough;
  d.metalness = 0;
  d.side = SIDE_DOUBLE;
  d.prog[MV_INSTANCED] = PROG_OD_BIRD_LIT;
  Material *m = mat_three(&d);
  Color rim = color_scale(sun_color(), o.rim);
  if (o.has_tint) rim = color_mul(rim, color_hex(o.tint));
  rig_uniforms(m, rig);
  mat_set_vec3(m, "uSunView", v3s(0));
  mat_set_color(m, "uRim", rim);
  mat_set_float(m, "uTrans", o.trans);
  mat_set_float(m, "uFingers", rig->fingers);
  mat_set_float(m, "uFingerLen", rig->finger_len);
  return m;
}

static Material *shadow_material(const Rig *rig) {
  MatDesc d = md_basic();
  d.name = "od-bird-shadow";
  d.color = color_hex(0xffffff);
  d.transparent = true;
  d.depth_write = true;
  d.depth_func = SDL_GPU_COMPAREOP_LESS;
  d.fog = false;
  d.side = SIDE_DOUBLE;
  d.polygon_offset = true;
  d.po_factor = -2;
  d.po_units = -4;
  d.blending = BLEND_CUSTOM;
  d.blend_op = SDL_GPU_BLENDOP_ADD;
  d.blend_src = SDL_GPU_BLENDFACTOR_ZERO;
  d.blend_dst = SDL_GPU_BLENDFACTOR_SRC_COLOR;
  d.blend_src_alpha = SDL_GPU_BLENDFACTOR_ZERO;
  d.blend_dst_alpha = SDL_GPU_BLENDFACTOR_ONE;
  d.prog[MV_INSTANCED] = PROG_OD_BIRD_SHADOW;
  d.prog[MV_INSTANCED_BACK] = PROG_OD_BIRD_SHADOW_BACK;
  d.prog[MV_INSTANCED_FRONT] = PROG_OD_BIRD_SHADOW;
  Material *m = mat_three(&d);
  rig_uniforms(m, rig);
  mat_set_vec3(m, "uSunDir", v3_norm(g_sun_dir));
  mat_set_color(m, "uShade", color_rgb(0.6, 0.64, 0.76));
  mat_set_float(m, "uBirdH", rig->height);
  return m;
}

// ---------------------------------------------------------------------------------------------
// instancing

typedef struct Pose { double sh, wr, fold, sw, hp, hb, leg, tuck, hy, tail, v; } Pose;

typedef struct Species {
  int n;
  float *pa, *pb, *pc, *gr;   // aPoseA / aPoseB / aPoseC / aGround (Float32Array)
  GpuGeometry *geo;
  Node *mesh, *shadow;
  Material *lit;
} Species;

static void species_hide(Species *s, int i) {
  M4 z = { { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 } };   // makeScale(0, 0, 0)
  inst_set_matrix(s->mesh, i, z);
  if (s->shadow) inst_set_matrix(s->shadow, i, z);   // (the shadow mesh shares instanceMatrix)
  if (s->gr) s->gr[i * 4 + 3] = 0;
}

static Species species_new(Node *scene, SpeciesGeo sg, int n, LitOpts o, bool with_shadow) {
  Species s = { .n = n };
  s.pa = xcalloc((size_t)n * 4, sizeof(float));
  s.pb = xcalloc((size_t)n * 4, sizeof(float));
  s.pc = xcalloc((size_t)n * 4, sizeof(float));
  geo_set_iattr(sg.geo, "aPoseA", 4, n, s.pa);
  geo_set_iattr(sg.geo, "aPoseB", 4, n, s.pb);
  geo_set_iattr(sg.geo, "aPoseC", 4, n, s.pc);
  if (with_shadow) {
    s.gr = xcalloc((size_t)n * 4, sizeof(float));
    geo_set_iattr(sg.geo, "aGround", 4, n, s.gr);
  }
  s.geo = gpu_geometry(sg.geo);
  geo_free(sg.geo);
  s.lit = lit_material(&sg.rig, o);
  s.mesh = node_instanced(s.geo, s.lit, n);
  s.mesh->frustum_culled = false;
  s.mesh->receive_shadow = true;
  snprintf(s.mesh->name, sizeof s.mesh->name, "birds");
  node_add(scene, s.mesh);
  if (with_shadow) {
    s.shadow = node_instanced(s.geo, shadow_material(&sg.rig), n);
    s.shadow->frustum_culled = false;
    s.shadow->render_order = 1;
    snprintf(s.shadow->name, sizeof s.shadow->name, "bird-shadows");
    node_add(scene, s.shadow);
  }
  for (int i = 0; i < n; i++) species_hide(&s, i);
  return s;
}

static void species_set(Species *s, int i, V3 pos, double yaw, double pitch, double roll, double scale, const Pose *P,
                        const double *ground) {
  M4 m = m4_compose(pos, quat_from_euler(euler(pitch, yaw, roll, EULER_YXZ)), v3s(scale));
  inst_set_matrix(s->mesh, i, m);
  if (s->shadow) inst_set_matrix(s->shadow, i, m);
  int k = i * 4;
  float *a = s->pa, *b = s->pb, *c = s->pc;
  a[k] = (float)P->sh; a[k + 1] = (float)P->wr; a[k + 2] = (float)P->fold; a[k + 3] = (float)P->sw;
  b[k] = (float)P->hp; b[k + 1] = (float)P->hb; b[k + 2] = (float)P->leg; b[k + 3] = (float)P->tuck;
  c[k] = (float)P->hy; c[k + 1] = (float)P->tail; c[k + 2] = 0; c[k + 3] = (float)P->v;
  if (s->gr) {
    float *g = s->gr;
    if (ground) { g[k] = (float)ground[0]; g[k + 1] = (float)ground[1]; g[k + 2] = (float)ground[2]; g[k + 3] = (float)ground[3]; }
    else g[k + 3] = 0;
  }
}

static void species_commit(Species *s) {
  // the instance matrices upload with the draw (inst_set_matrix marks them dirty)
  gpu_geometry_update_iattr(s->geo, "aPoseA", s->pa);
  gpu_geometry_update_iattr(s->geo, "aPoseB", s->pb);
  gpu_geometry_update_iattr(s->geo, "aPoseC", s->pc);
  if (s->gr) gpu_geometry_update_iattr(s->geo, "aGround", s->gr);
}

// Flap cycle blended over a glide pose. Downstroke (42% of the beat) faster than the upstroke,
// during which the hand flexes down and sweeps back.
static void wing_pose(Pose *P, const double glide[3], double w, double ph, double amp) {
  const double down = 0.42;
  ph -= floor(ph);
  double sh, wr, sw = 0;
  if (ph < down) {
    double u = ph / down;
    sh = cos(PI_D * u);
    wr = 0.3 * sin(PI_D * u);
  } else {
    double u = (ph - down) / (1 - down);
    sh = -cos(PI_D * u);
    wr = -0.95 * sin(PI_D * u);
    sw = 0.7 * sin(PI_D * u);
  }
  P->sh = lerp(glide[0], amp * sh + 0.12 * amp, w);
  P->wr = lerp(glide[1], amp * wr, w);
  P->sw = lerp(glide[2], amp * sw, w);
}

// ---------------------------------------------------------------------------------------------
// behaviours

typedef enum GullMode { G_STAND, G_WALK, G_TAKEOFF, G_SOAR, G_FLYTO, G_LAND } GullMode;
typedef struct Orbit { double cx, cz, r, dir, h; } Orbit;

typedef struct Gull {
  int i, v;
  double scale;
  V3 pos;
  double yaw, speed, vy, bank, pitch;
  GullMode mode;
  double t, dur, flap_w, flap_ph, flap_until, next_flap, fold, tuck, leg_ph, hy, hy_t, next_look;
  bool has_walk_to;
  XZ walk_to;
  double next_walk, stand_until, yaw_off, call_until;
  XZ land;
  double flee;
  bool active, has_from;
  V3 from;
  Orbit orbit;
} Gull;

typedef struct Sand {
  int i;
  double x, z, dz, dx, leg_ph, peck, peck_rate, yaw;
  XZ from, to;
  double y, flap_ph, speed;
} Sand;

typedef struct Strip { double x0, x1, z0, z1; } Strip;
typedef enum GrackleMode { K_WALK, K_PECK, K_LOOK, K_FLY } GrackleMode;
typedef struct Grackle {
  int i;
  const Strip *strip;
  int v;
  double scale, x, z, y, yaw;
  GrackleMode mode;
  double t, dur;
  XZ target;
  double leg_ph, peck, hy, hy_t, tail;
  XZ from, to;
  double flap_ph, shade;
} Grackle;

enum { MAX_PELICAN = 6, MAX_GULL = 12, MAX_SAND = 10, MAX_GRACKLE = 5, MAX_FLOCK = 9, MAX_FRIGATE = 1 };

static const Strip STRIP_HOTEL_S = { -27.5, -24.7, 46, 64 };
static const Strip STRIP_PATIO = { -27.5, -25.2, -15, -5 };
static const Strip STRIP_PARK = { -14, -10.9, 44, 62 };

struct Birds {
  Surf *surf;
  bool shot;
  Rng rnd;
  double T;
  V3 player;
  Pose P;
  Counts counts;
  double wind_yaw, seaward_yaw;
  Species pelicans, gulls, sands, grackles, frigates;
  struct { double t0, dir, lane, cycle, ph; } pass;
  Gull gull[MAX_GULL];
  struct { double zc; bool fly; double t, dur; } flock;
  Sand sand[MAX_SAND];
  Grackle grackle[MAX_GRACKLE];
  double shade_x, shade_z;
  Vec(const PalmTree *) trunks;
  struct { double t0, dir, x, y; } line;
  Frustum frustum;
  void (*on_flutter)(const BirdFlutter *f, void *user);
  void *flutter_user;
};

static double rnd(Birds *B) { return rng_next(&B->rnd); }
static double R(Birds *B, double a, double b) { return a + rnd(B) * (b - a); }
static double ground_at(double x, double z) { return beach_ground_at(x, z); }
static void ground_plane(double x, double z, double strength, double out[4]) {
  double g0 = ground_at(x, z);
  out[0] = g0;
  out[1] = ground_at(x + 0.5, z) - ground_at(x - 0.5, z);
  out[2] = ground_at(x, z + 0.5) - ground_at(x, z - 0.5);
  out[3] = strength;
}
// surf.swashAt(x, z, shot ? T : undefined)
static Swash swash_at(const Birds *B, double x, double z) {
  return surf_swash_at(B->surf, x, z, B->shot ? B->T : surf_time(B->surf));
}

// ---- brown pelicans: a line gliding low over the swell beyond the break ----
static constexpr double PEL_SPEED = 10;
static constexpr double PEL_SPACING = 5.4;
static constexpr double PEL_RANGE = 380;

static void new_pass(Birds *B, double t) {
  B->pass.t0 = t;
  B->pass.dir = rnd(B) < 0.5 ? 1 : -1;
  B->pass.lane = R(B, 100, 116);
  B->pass.cycle = R(B, 12, 18);
  B->pass.ph = R(B, 0, 10);
}

static void update_pelicans(Birds *B) {
  Pose *P = &B->P;
  double T = B->T;
  int n = B->counts.pelican;
  double dur = (2 * PEL_RANGE + n * PEL_SPACING) / PEL_SPEED;
  if (T > B->pass.t0 + dur) new_pass(B, T + (B->shot ? 0 : R(B, 0, 12)));
  double tau = T - B->pass.t0;
  double zL = -B->pass.dir * PEL_RANGE + B->pass.dir * PEL_SPEED * tau;
  for (int i = 0; i < MAX_PELICAN; i++) {
    if (i >= n || tau < 0) { species_hide(&B->pelicans, i); continue; }
    double z = zL - B->pass.dir * i * PEL_SPACING;
    if (fabs(z) > PEL_RANGE + 20) { species_hide(&B->pelicans, i); continue; }
    double x = B->pass.lane + 0.7 * sin(i * 1.9) + 0.5 * sin(T * 0.13 + i * 0.7);
    // skimming the swell: the line rides up and over each swell
    double swell = 0.35 * sin(z * 0.07 - T * 0.55 + x * 0.02);
    double y = SEA_LEVEL + 1.7 + 0.25 * sin(i * 2.3) + swell;
    double dy = 0.35 * cos(z * 0.07 - T * 0.55) * 0.07 * PEL_SPEED;
    // follow-the-leader: the flap bout ripples down the line
    double tl = tau + B->pass.ph - i * 0.42;
    double c = tl - B->pass.cycle * floor(tl / B->pass.cycle);
    double w = smooth(0, 0.35, c) * (1 - smooth(3.1, 3.6, c));
    double glide[3] = { -0.02 + 0.015 * sin(T * 1.1 + i), -0.1, 0.16 };
    wing_pose(P, glide, w, (T - i * 0.42) * 1.45, 0.48);
    P->fold = 0; P->hp = 0.04; P->hb = 0; P->tuck = 1; P->hy = 0.05 * sin(T * 0.3 + i); P->tail = 0.05; P->v = 0;
    species_set(&B->pelicans, i, v3(x, y, z), B->pass.dir > 0 ? 0 : PI_D, -dy / PEL_SPEED * 0.6 - 0.03,
                0.03 * sin(T * 0.7 + i * 1.3), 1, P, nullptr);
  }
}

// ---- gulls ----
static const double STAND_SPOTS[8][2] = { { 86.2, -31 }, { 87.4, -29.2 }, { 84.9, -33.4 }, { 88.3, -34.5 },
                                          { 80.5, 26 }, { 82, 24.2 }, { 88.6, 56 }, { 76, -55 } };

static XZ landing_spot(Birds *B, const V3 *avoid) {
  for (int k = 0; k < 20; k++) {
    double x = R(B, 77, 89.3);
    double z = R(B, -68, 68);
    if (js_hypot2(x - B->player.x, z - B->player.z) < 14) continue;
    if (avoid && js_hypot2(x - avoid->x, z - avoid->z) < 20) continue;
    return (XZ){ x, z };
  }
  return (XZ){ 84, B->player.z > 0 ? -50 : 50 };
}

// newOrbit(g, cx, cz): cx / cz null draw their own
static void new_orbit(Birds *B, Gull *g, const double *cx, const double *cz) {
  Orbit o;
  o.cx = cx ? *cx : R(B, 30, 125);
  o.cz = cz ? *cz : R(B, -75, 75);
  o.r = R(B, 12, 30);
  o.dir = rnd(B) < 0.5 ? 1 : -1;
  o.h = R(B, 7, 24);
  g->orbit = o;
}

static V3 fwd(double yaw) { return v3(sin(yaw), 0, cos(yaw)); }

static double steer(Gull *g, double tx, double tz, double dt, double max_turn) {
  double want = atan2(tx - g->pos.x, tz - g->pos.z);
  double turn = clampd(wrap_pi(want - g->yaw) * 1.4, -max_turn, max_turn);
  g->yaw = wrap_pi(g->yaw + turn * dt);
  g->bank = approach(g->bank, -atan((g->speed * turn) / 9.81), 2.5, dt);
  return turn;
}

static void takeoff(Birds *B, Gull *g, double yaw) {
  g->mode = G_TAKEOFF; g->t = 0; g->flee = yaw; g->speed = 0.6; g->vy = 0; g->flap_ph = 0.45; g->has_walk_to = false;
  double d = v3_dist(g->pos, B->player);
  if (d < 16 && B->on_flutter) {
    V3 f = fwd(yaw);
    BirdFlutter fl = { g->pos.x, g->pos.y + 0.3, g->pos.z, f.x * 5, 2.5, f.z * 5, d };
    B->on_flutter(&fl, B->flutter_user);
  }
}

static void update_gull(Birds *B, Gull *g, double dt) {
  Pose *P = &B->P;
  double T = B->T;
  const V3 player = B->player;
  double gy = ground_at(g->pos.x, g->pos.z);
  double pd = js_hypot2(g->pos.x - player.x, g->pos.z - player.z) + fmax(0, fabs(player.y - 1.7 - g->pos.y) - 1);
  g->t += dt;
  double flap_freq = 2.9, flap_amp = 0.9;
  double glide[3] = { 0.1 + 0.03 * sin(T * 1.7 + g->i), -0.2, 0.34 };
  P->hp = 0.12; P->hb = 0; P->tail = 0; P->leg = 0;
  if (g->mode == G_STAND || g->mode == G_WALK) {
    g->fold = fmin(1, g->fold + dt * 2.5); g->tuck = fmax(0, g->tuck - dt * 4);
    g->pos.y = gy; g->speed = 0; g->bank = 0; g->pitch = approach(g->pitch, 0, 6, dt);
    if (pd < 6 && !B->shot) {
      double away = atan2(g->pos.x - player.x, g->pos.z - player.z);
      takeoff(B, g, wrap_pi(away + 0.4 * wrap_pi(B->wind_yaw - away)));
    } else {
      Swash sw = swash_at(B, g->pos.x, g->pos.z);
      if (!g->has_walk_to && sw.covered && sw.depth > 0.025) {
        double x = g->pos.x - R(B, 1.2, 2.2);
        double z = g->pos.z + R(B, -0.6, 0.6);
        g->walk_to = (XZ){ x, z };
        g->has_walk_to = true;
      }
      if (!g->has_walk_to && T > g->next_walk) {
        double x = clampd(g->pos.x + R(B, -2.5, 2.5), 76, 89);
        double z = g->pos.z + R(B, -3, 3);
        g->walk_to = (XZ){ x, z };
        g->has_walk_to = true;
        g->next_walk = T + R(B, 8, 30);
      }
      if (g->has_walk_to) {
        g->mode = G_WALK;
        double dx = g->walk_to.x - g->pos.x, dz = g->walk_to.z - g->pos.z, d = js_hypot2(dx, dz);
        double want = atan2(dx, dz);
        g->yaw = wrap_pi(g->yaw + clampd(wrap_pi(want - g->yaw), -4 * dt, 4 * dt));
        if (fabs(wrap_pi(want - g->yaw)) < 0.6) {
          double sp = fmin(0.5, d * 2 + 0.1);
          g->pos.x += (dx / d) * sp * dt; g->pos.z += (dz / d) * sp * dt;
          g->leg_ph += dt * 2.6;
        }
        if (d < 0.08) { g->has_walk_to = false; g->mode = G_STAND; }
      } else {
        g->leg_ph = approach(g->leg_ph, js_round(g->leg_ph * 2) / 2, 10, dt);
        g->yaw = wrap_pi(g->yaw + clampd(wrap_pi(B->wind_yaw + g->yaw_off - g->yaw), -1.2 * dt, 1.2 * dt));
      }
      if (T > g->next_look) {
        double a = R(B, -0.9, 0.9);
        g->hy_t = a * (rnd(B) < 0.3 ? 0 : 1);
        g->next_look = T + R(B, 1.2, 5);
      }
      if (T > g->stand_until && g->mode == G_STAND) takeoff(B, g, B->wind_yaw + R(B, -0.5, 0.5));
    }
    P->hp = T < g->call_until ? -0.55 : 0.05;
    P->leg = g->leg_ph;
    g->hy = approach(g->hy, g->hy_t, 8, dt);
  } else if (g->mode == G_TAKEOFF) {
    g->fold = fmax(0, g->fold - dt * 6);
    g->speed = fmin(8.5, g->speed + dt * 5.5);
    g->vy = g->t < 1.4 ? 2.8 : lerp(2.8, 0.8, smooth(1.4, 2.6, g->t));
    if (g->t > 0.45) g->tuck = fmin(1, g->tuck + dt * 1.6);
    steer(g, g->pos.x + sin(g->flee) * 50, g->pos.z + cos(g->flee) * 50, dt, 1.5);
    g->pitch = approach(g->pitch, -0.3, 5, dt);
    g->flap_w = 1; flap_freq = 4.2; flap_amp = 1.05;
    if (g->t > 2.6) {
      g->mode = G_SOAR; g->t = 0; g->dur = R(B, 18, 50);
      V3 f = fwd(g->yaw);
      double cx = clampd(g->pos.x + f.x * 35, 20, 130), cz = clampd(g->pos.z + f.z * 35, -90, 90);
      new_orbit(B, g, &cx, &cz);
      g->orbit.h = clampd(g->pos.y - gy + R(B, 3, 10), 6, 22);
    }
  } else if (g->mode == G_SOAR || g->mode == G_FLYTO) {
    double tx, tz, hT, spT = 8.5;
    if (g->mode == G_SOAR) {
      Orbit *o = &g->orbit;
      double a = atan2(g->pos.x - o->cx, g->pos.z - o->cz) + o->dir * 0.55;
      tx = o->cx + sin(a) * o->r; tz = o->cz + cos(a) * o->r;
      o->cx += sin(T * 0.05 + g->i) * 0.4 * dt; o->cz += cos(T * 0.04 + g->i * 2) * 0.4 * dt;
      hT = o->h + 1.5 * sin(T * 0.21 + g->i);
      if (g->t > g->dur && !B->shot) {
        g->mode = G_FLYTO;
        g->land = landing_spot(B, g->has_from ? &g->from : nullptr);
        g->t = 0;
      }
    } else {
      tx = g->land.x; tz = g->land.z;
      double d = js_hypot2(tx - g->pos.x, tz - g->pos.z);
      hT = clampd(d * 0.16, 0.2, 22);
      spT = clampd(d * 0.5 + 3.5, 4, 8.5);
      if (d < 4.5 && g->pos.y - gy < 1.8) { g->mode = G_LAND; g->t = 0; g->from = g->pos; g->has_from = true; }
      if (js_hypot2(tx - player.x, tz - player.z) < 8) g->land = landing_spot(B, nullptr);
    }
    steer(g, tx, tz, dt, g->mode == G_SOAR ? 0.9 : 1.2);
    g->speed = approach(g->speed, spT, 0.8, dt);
    g->vy = approach(g->vy, clampd((hT - (g->pos.y - gy)) * 0.35, -2.2, 1.8), 1.5, dt);
    g->pitch = approach(g->pitch, -atan2(g->vy, g->speed) * 0.7, 3, dt);
    g->fold = fmax(0, g->fold - dt * 4); g->tuck = fmin(1, g->tuck + dt);
    // soaring: long glides broken by short bouts of flapping
    if (T > g->next_flap) {
      g->flap_until = T + R(B, 3, 6) / flap_freq;
      g->next_flap = g->flap_until + R(B, 4, 14);
    }
    double want = T < g->flap_until || g->vy > 1.2 ? 1 : 0;
    g->flap_w = approach(g->flap_w, want, 5, dt);
    glide[2] += fabs(g->bank) * 0.25;
  } else if (g->mode == G_LAND) {
    double u = fmin(1, g->t / 1.1);
    g->pos.x = lerp(g->from.x, g->land.x, 1 - (1 - u) * (1 - u));
    g->pos.z = lerp(g->from.z, g->land.z, 1 - (1 - u) * (1 - u));
    g->pos.y = lerp(g->from.y, ground_at(g->pos.x, g->pos.z), sin((u * PI_D) / 2));
    g->speed = 0; g->bank = approach(g->bank, 0, 5, dt); g->vy = 0;
    g->yaw = wrap_pi(g->yaw + clampd(wrap_pi(B->wind_yaw - g->yaw), -1.5 * dt, 1.5 * dt));
    g->pitch = -0.6 * sin(PI_D * fmin(1, u * 1.1));
    g->tuck = fmax(0, g->tuck - dt * 3);
    g->flap_w = u > 0.45 ? 1 : 0.3; flap_freq = 4.6; flap_amp = 0.55;
    glide[0] = 0.5; glide[1] = -0.1; glide[2] = 0.1;
    if (u >= 1) {
      g->mode = G_STAND; g->t = 0; g->stand_until = T + R(B, 50, 160); g->next_walk = T + R(B, 5, 20);
      g->yaw_off = R(B, -0.35, 0.35);
    }
  }
  if (g->mode != G_STAND && g->mode != G_WALK && g->mode != G_LAND) {
    V3 f = fwd(g->yaw);
    g->pos = v3_add_scaled(g->pos, f, g->speed * dt);
    g->pos.y += g->vy * dt;
    g->pos.y = fmax(g->pos.y, ground_at(g->pos.x, g->pos.z) + (g->mode == G_TAKEOFF ? 0 : 0.8));
  }
  g->flap_ph += dt * flap_freq;
  if (g->fold >= 1) { P->sh = 0; P->wr = 0; P->sw = 0; g->flap_w = 0; }
  else if (g->mode == G_STAND || g->mode == G_WALK) { P->sh = 0.9 * (1 - g->fold); P->wr = -0.2; P->sw = 0.3; }
  else wing_pose(P, glide, g->flap_w, g->flap_ph, flap_amp);
  P->fold = g->fold; P->tuck = g->tuck; P->hy = g->hy; P->v = g->v;
  double walk_bob = g->mode == G_WALK ? 0.004 * fabs(sin(g->leg_ph * PI_D * 2)) : 0;
  V3 p = v3(g->pos.x, g->pos.y + walk_bob, g->pos.z);
  double h = g->pos.y - ground_at(g->pos.x, g->pos.z);
  double roll = g->bank + (g->mode == G_WALK ? 0.05 * sin(g->leg_ph * TAU) : 0);
  double gp[4];
  ground_plane(g->pos.x, g->pos.z, 1 - smooth(0.3, 4, h), gp);
  species_set(&B->gulls, g->i, p, g->yaw, g->pitch, roll, g->scale, P, gp);
}

// ---- sanderlings chasing the swash edge ----
typedef struct Edge { double edge; Swash sw; } Edge;
static Edge edge_at(const Birds *B, double x, double z) {
  Swash sw = swash_at(B, x, z);
  return (Edge){ fmin(sw.front, SHORE_X + 0.3), sw };
}

static void update_sand(Birds *B, double dt) {
  Pose *P = &B->P;
  double T = B->T;
  int n = B->counts.sand;
  if (!B->flock.fly && !B->shot) {
    for (int i = 0; i < n; i++) {
      Sand *b = &B->sand[i];
      if (js_hypot2(b->x - B->player.x, b->z - B->player.z) < 5) {
        double dir = B->player.z > b->z ? -1 : 1;
        B->flock.fly = true; B->flock.t = 0; B->flock.dur = R(B, 2.6, 3.4);
        B->flock.zc = clampd(B->flock.zc + dir * R(B, 20, 30), -70, 50);
        if (fabs(B->flock.zc - B->player.z) < 10) B->flock.zc = B->player.z + dir * 14;
        for (int k = 0; k < n; k++) {
          Sand *s = &B->sand[k];
          s->from = (XZ){ s->x, s->z };
          double tz = B->flock.zc + s->dz;
          s->to = (XZ){ edge_at(B, s->x, tz).edge - 0.4 - s->dx, tz };
        }
        break;
      }
    }
  }
  if (B->flock.fly) {
    B->flock.t += dt;
    double u = fmin(1, B->flock.t / B->flock.dur);
    for (int i = 0; i < n; i++) {
      Sand *b = &B->sand[i];
      double e = u * u * (3 - 2 * u);
      double nx = lerp(b->from.x, b->to.x, e) + 3.5 * sin(PI_D * u), nz = lerp(b->from.z, b->to.z, e);
      double a = atan2(nx - b->x, nz - b->z);
      if (a != 0 && !isnan(a)) b->yaw = a;   // `atan2(...) || b.yaw`
      b->x = nx; b->z = nz;
      b->y = ground_at(b->x, b->z) + (1.2 + 0.2 * sin(i * 2.1)) * sin(PI_D * fmin(1, u * 1.08));
      b->flap_ph += dt * 13;
      P->fold = u < 0.08 ? 1 - u / 0.08 : u > 0.93 ? (u - 0.93) / 0.07 : 0;
      if (u > 0.8) { P->sh = 0.9; P->wr = 0.1; P->sw = 0.1; }
      else wing_pose(P, (double[]){ 0.2, -0.1, 0.3 }, 1, b->flap_ph, 0.95);
      P->hp = 0.1; P->hb = 0; P->leg = 0; P->tuck = u > 0.1 && u < 0.85 ? 1 : 0; P->hy = 0; P->tail = 0; P->v = 0;
      double h = b->y - ground_at(b->x, b->z);
      double gp[4];
      ground_plane(b->x, b->z, 1 - smooth(0.2, 2, h), gp);
      species_set(&B->sands, i, v3(b->x, b->y, b->z), b->yaw, -0.1, 0, 1, P, gp);
    }
    if (u >= 1) B->flock.fly = false;
  } else {
    B->flock.zc = clampd(B->flock.zc + sin(T * 0.037) * 0.12 * dt, -70, 50);
    for (int i = 0; i < n; i++) {
      Sand *b = &B->sand[i];
      Edge E = edge_at(B, b->x, b->z);
      double tx = E.edge - 0.18 - b->dx;
      double ex = tx - b->x;
      double vx = 0;
      if (E.sw.covered && b->x > E.sw.front + 0.05) vx = -2.4;
      else if (fabs(ex) > 0.12) vx = js_sign(ex) * fmin(2.3, 0.45 + fabs(ex) * 3.2);
      double vz = clampd((B->flock.zc + b->dz - b->z) * 0.4, -0.35, 0.35) * (vx != 0 ? 1 : 0.3);
      b->x += vx * dt; b->z += vz * dt;
      double sp = js_hypot2(vx, vz);
      b->speed = approach(b->speed, sp, 12, dt);
      if (sp > 0.05) {
        b->yaw = wrap_pi(b->yaw + clampd(wrap_pi(atan2(vx, vz) - b->yaw), -14 * dt, 14 * dt));
        b->leg_ph += (sp * dt) / 0.055;
        P->hp = 0.15; b->peck = 0;
      } else {
        b->leg_ph = approach(b->leg_ph, js_round(b->leg_ph * 2) / 2, 12, dt);
        b->yaw = wrap_pi(b->yaw + clampd(wrap_pi(B->seaward_yaw + 0.8 * sin(T * 0.3 + i * 1.7) - b->yaw), -2 * dt, 2 * dt));
        b->peck += dt * b->peck_rate;
        // quick "sewing machine" probes into the wet sand, with short pauses
        double pc = fmod(b->peck, 3);
        P->hp = pc < 2 ? 0.2 + 0.95 * pow(fabs(sin(pc * PI_D)), 0.6) : 0.1;
      }
      P->sh = 0; P->wr = 0; P->sw = 0; P->fold = 1; P->hb = 0; P->leg = b->leg_ph; P->tuck = 0; P->hy = 0; P->tail = 0; P->v = 0;
      b->y = ground_at(b->x, b->z);
      V3 p = v3(b->x, b->y + (sp > 0.05 ? 0.003 * fabs(sin(b->leg_ph * TAU)) : 0), b->z);
      double gp[4];
      ground_plane(b->x, b->z, 1, gp);
      species_set(&B->sands, i, p, b->yaw, P->hp > 0.5 ? 0.25 : 0.05, 0, 1, P, gp);
    }
  }
  for (int i = n; i < MAX_SAND; i++) species_hide(&B->sands, i);
}

// ---- boat-tailed grackles on the sidewalks ----
// the static shadow map can't show birds' shadows, so a sun shadow is drawn only where the
// sidewalk is sunlit: the street-side palm trunks throw long thin shadows west across it
static bool in_trunk_shade(const Birds *B, double x, double z) {
  for (size_t i = 0; i < B->trunks.len; i++) {
    const PalmTree *t = B->trunks.data[i];
    double dx = x - t->x, dz = z - t->z, along = dx * B->shade_x + dz * B->shade_z;
    if (along > -0.3 && along < 22 && fabs(dx * B->shade_z - dz * B->shade_x) < 0.4) return true;
  }
  return false;
}

static XZ pick_target(Birds *B, const Grackle *b, bool far) {
  const Strip *s = b->strip;
  for (int k = 0; k < 16; k++) {
    double x = R(B, s->x0, s->x1);
    double z = far ? R(B, s->z0, s->z1) : clampd(b->z + R(B, -2.5, 2.5), s->z0, s->z1);
    if (k < 12 && in_trunk_shade(B, x, z)) continue;
    if (!far || js_hypot2(x - B->player.x, z - B->player.z) > 8) return (XZ){ x, z };
  }
  double x = R(B, s->x0, s->x1);
  return (XZ){ x, B->player.z > (s->z0 + s->z1) / 2 ? s->z0 : s->z1 };
}

static void update_grackles(Birds *B, double dt) {
  Pose *P = &B->P;
  int n = B->counts.grackle;
  for (int i = 0; i < MAX_GRACKLE; i++) {
    Grackle *b = &B->grackle[i];
    if (i >= n) { species_hide(&B->grackles, i); continue; }
    b->t += dt;
    double gy = ground_at(b->x, b->z);
    P->sh = 0; P->wr = 0; P->sw = 0; P->fold = 1; P->hp = 0; P->hb = 0; P->tuck = 0; P->tail = b->tail; P->v = b->v;
    double pitch = 0, y = gy;
    if (b->mode != K_FLY && !B->shot && js_hypot2(b->x - B->player.x, b->z - B->player.z) < 3.5) {
      b->mode = K_FLY; b->t = 0; b->from = (XZ){ b->x, b->z }; b->to = pick_target(B, b, true);
      b->dur = js_hypot2(b->to.x - b->x, b->to.z - b->z) / 6.5 + 0.6;
    }
    if (b->mode == K_FLY) {
      double u = fmin(1, b->t / b->dur), e = u * u * (3 - 2 * u);
      double nx = lerp(b->from.x, b->to.x, e), nz = lerp(b->from.z, b->to.z, e);
      double want = atan2(b->to.x - b->from.x, b->to.z - b->from.z);
      b->yaw = wrap_pi(b->yaw + clampd(wrap_pi(want - b->yaw), -10 * dt, 10 * dt));
      b->x = nx; b->z = nz;
      y = ground_at(nx, nz) + 1.8 * sin(PI_D * u);
      b->flap_ph += dt * 7.5;
      P->fold = u < 0.08 ? 1 - u / 0.08 : u > 0.94 ? (u - 0.94) / 0.06 : 0;
      if (u > 0.78) { P->sh = 0.85; P->wr = 0.2; P->sw = 0.15; pitch = -0.5; }
      else {
        wing_pose(P, (double[]){ 0.1, -0.1, 0.2 }, u < 0.55 ? 1 : 0.6, b->flap_ph, 0.95);
        pitch = u < 0.3 ? -0.25 : 0;
      }
      P->tuck = u > 0.12 && u < 0.8 ? 1 : 0; P->tail = 0.05;
      if (u >= 1) { b->mode = K_LOOK; b->t = 0; b->dur = R(B, 0.8, 2); }
    } else {
      if (b->t > b->dur) {
        double r = rnd(B);
        b->mode = r < 0.45 ? K_WALK : r < 0.8 ? K_PECK : K_LOOK;
        b->t = 0;
        b->dur = b->mode == K_WALK ? R(B, 1.5, 4) : b->mode == K_PECK ? R(B, 0.8, 2.2) : R(B, 0.8, 2.5);
        if (b->mode == K_WALK) b->target = pick_target(B, b, false);
        if (b->mode == K_LOOK) {
          b->hy_t = R(B, -0.9, 0.9);
          if (rnd(B) < 0.5) b->tail = 0.28;
        }
      }
      b->tail = approach(b->tail, 0.06, 3, dt);
      if (b->mode == K_WALK) {
        double dx = b->target.x - b->x, dz = b->target.z - b->z, d = js_hypot2(dx, dz);
        if (d > 0.05) {
          double want = atan2(dx, dz);
          b->yaw = wrap_pi(b->yaw + clampd(wrap_pi(want - b->yaw), -5 * dt, 5 * dt));
          double sp = 0.32;
          b->x += (dx / d) * sp * dt; b->z += (dz / d) * sp * dt;
          b->leg_ph += dt * 3.3;
          // head holds still in space while the body walks under it, then thrusts forward
          double f = b->leg_ph * 2 - floor(b->leg_ph * 2);
          P->hb = 0.014 * (f < 0.75 ? 0.5 - f / 0.75 : -0.5 + (f - 0.75) / 0.25);
          P->hp = 0.1;
        } else b->mode = K_LOOK;
        b->hy = approach(b->hy, 0, 6, dt);
      } else {
        b->leg_ph = approach(b->leg_ph, js_round(b->leg_ph * 2) / 2, 10, dt);
        if (b->mode == K_PECK) {
          b->peck += dt * 2.6;
          double pc = fmod(b->peck, 1);
          P->hp = pc < 0.45 ? 1.25 * sin((pc / 0.45) * PI_D) : 0;
          pitch = P->hp * 0.25;
          b->hy = approach(b->hy, 0, 6, dt);
        } else b->hy = approach(b->hy, b->hy_t, 7, dt);
      }
      P->leg = b->leg_ph;
    }
    P->hy = b->hy;
    b->shade = approach(b->shade, in_trunk_shade(B, b->x, b->z) ? 0 : 1, 6, dt);
    double gp[4];
    ground_plane(b->x, b->z, (1 - smooth(0.2, 3, y - gy)) * 0.85 * b->shade, gp);
    species_set(&B->grackles, i, v3(b->x, y, b->z), b->yaw, pitch, 0, b->scale, P, gp);
  }
}

// ---- distant cormorants crossing the sunrise, far out over the sea ----
static void update_flock(Birds *B) {
  Pose *P = &B->P;
  double T = B->T;
  int n = B->counts.flock;
  const double speed = 13, len = 1500;
  if (T > B->line.t0 + len / speed) {
    B->line.t0 = T + (B->shot ? 0 : R(B, 20, 60));
    B->line.dir = -B->line.dir;
    B->line.x = R(B, 260, 420);
    B->line.y = R(B, 18, 34);
  }
  double zL = -B->line.dir * 750 + B->line.dir * speed * (T - B->line.t0);
  for (int k = 0; k < MAX_FLOCK; k++) {
    int i = MAX_GRACKLE + k;
    if (k >= n || T < B->line.t0) { species_hide(&B->grackles, i); continue; }
    double side = k == 0 ? 0 : (k % 2 ? 1 : -1) * ceil(k / 2.0);
    double z = zL - B->line.dir * fabs(side) * 5.5, x = B->line.x + side * 4 + 1.2 * sin(T * 0.3 + k);
    wing_pose(P, (double[]){ 0, 0, 0 }, 1, T * 3.1 + k * 0.37, 0.75);
    P->fold = 0; P->hp = 0; P->hb = 0; P->leg = 0; P->tuck = 1; P->hy = 0; P->tail = 0; P->v = 0;
    species_set(&B->grackles, i, v3(x, B->line.y + 0.8 * sin(T * 0.4 + k * 1.3) + fabs(side) * 0.3, z),
                B->line.dir > 0 ? 0 : PI_D, 0, 0, 3.4, P, nullptr);
  }
}

// ---- a magnificent frigatebird hanging high over the beach ----
static void update_frigate(Birds *B) {
  Pose *P = &B->P;
  double T = B->T;
  if (!B->counts.frigate) { species_hide(&B->frigates, 0); return; }
  double r = 75, v = 7.5, a = (T * v) / r + 1.2;
  double x = 70 + r * sin(a), z = -25 + r * cos(a), y = 88 + 5 * sin(T * 0.07);
  P->sh = 0.06 + 0.04 * sin(T * 0.9); P->wr = -0.3 + 0.03 * sin(T * 0.9 + 1); P->sw = 0.5; P->fold = 0;
  P->hp = 0.15; P->hb = 0; P->leg = 0; P->tuck = 1; P->hy = 0.2 * sin(T * 0.2); P->tail = 0.06 * sin(T * 0.5); P->v = 0;
  species_set(&B->frigates, 0, v3(x, y, z), atan2(cos(a), -sin(a)), 0.05, -0.18, 1, P, nullptr);
}

static void step(Birds *B, double dt) {
  B->T += dt;
  update_pelicans(B);
  for (int i = 0; i < MAX_GULL; i++) {
    Gull *g = &B->gull[i];
    if (!g->active) { species_hide(&B->gulls, g->i); continue; }
    update_gull(B, g, dt);
  }
  update_sand(B, dt);
  update_grackles(B, dt);
  update_flock(B);
  update_frigate(B);
}

void birds_set_quality(Birds *B, Tier tier) {
  B->counts = counts_of(tier);
  for (int i = 0; i < MAX_GULL; i++) B->gull[i].active = i < B->counts.gull;
}

void birds_on_flutter(Birds *B, void (*fn)(const BirdFlutter *f, void *user), void *user) {
  B->on_flutter = fn;
  B->flutter_user = user;
}

static void commit_all(Birds *B) {
  Species *all[5] = { &B->pelicans, &B->gulls, &B->sands, &B->grackles, &B->frigates };
  for (int i = 0; i < 5; i++) species_commit(all[i]);
}

Birds *create_birds(Node *scene, Surf *surf, bool shot) {
  Birds *B = xcalloc(1, sizeof *B);
  B->surf = surf;
  B->shot = shot;
  B->rnd = rng_make(7717);
  B->wind_yaw = yaw_of_compass(112);   // light onshore breeze: loafing gulls face it
  B->seaward_yaw = yaw_of_compass(92);
  Tier initial_tier = QUALITY.tier;

  B->pelicans = species_new(scene, pelican_geo(), MAX_PELICAN, (LitOpts){ 0.85, 0.9, 0.12 }, false);
  B->gulls = species_new(scene, gull_geo(), MAX_GULL, (LitOpts){ 0.75, 1.0, 0.45 }, true);
  B->sands = species_new(scene, sanderling_geo(), MAX_SAND, (LitOpts){ 0.8, 1.0, 0.35 }, true);
  B->grackles = species_new(scene, grackle_geo(), MAX_GRACKLE + MAX_FLOCK, (LitOpts){ 0.38, 0.8, 0.1, 0xb8b0ff, true }, true);
  B->frigates = species_new(scene, frigate_geo(), MAX_FRIGATE, (LitOpts){ 0.7, 0.6, 0.1 }, false);

  B->T = 0;
  B->player = v3(1e5, 0, 1e5);
  B->P = (Pose){ .tuck = 1 };
  B->counts = counts_of(initial_tier);

  // at the frozen shot time the pelican line is heading south, well up the beach to the north
  B->pass.t0 = -19; B->pass.dir = 1; B->pass.lane = 108; B->pass.cycle = 15; B->pass.ph = 3;

  for (int i = 0; i < MAX_GULL; i++) {
    Gull *g = &B->gull[i];
    int v = rnd(B) < 0.6 ? 0 : 1;
    *g = (Gull){ .i = i, .v = v, .scale = v ? 1.12 : 1, .mode = G_SOAR, .tuck = 1, .active = true };
    g->yaw = R(B, -3, 3);
    g->dur = R(B, 25, 80);
    g->flap_ph = rnd(B);
    g->next_flap = R(B, 0, 8);
    g->next_walk = R(B, 6, 25);
    g->yaw_off = R(B, -0.35, 0.35);
    const double *spot = i < 8 && i % 3 != 2 ? STAND_SPOTS[i] : nullptr;
    if (spot) {
      g->mode = G_STAND; g->fold = 1; g->tuck = 0;
      g->pos = v3(spot[0], ground_at(spot[0], spot[1]), spot[1]);
      g->yaw = B->wind_yaw + g->yaw_off; g->stand_until = R(B, 60, 200);
    } else {
      new_orbit(B, g, nullptr, nullptr);
      double a = rnd(B) * TAU;
      g->pos = v3(g->orbit.cx + sin(a) * g->orbit.r, 0, g->orbit.cz + cos(a) * g->orbit.r);
      g->pos.y = ground_at(g->pos.x, g->pos.z) + g->orbit.h;
      g->yaw = a + (g->orbit.dir * PI_D) / 2; g->speed = 8.5;
    }
  }

  B->flock.zc = -32; B->flock.fly = false; B->flock.t = 0; B->flock.dur = 3;
  for (int i = 0; i < MAX_SAND; i++) {
    Sand *b = &B->sand[i];
    *b = (Sand){ .i = i, .x = SHORE_X - 3, .yaw = B->seaward_yaw };
    b->z = B->flock.zc + (i - MAX_SAND / 2.0) * 0.9 + R(B, -0.3, 0.3);
    b->dz = (i - MAX_SAND / 2.0) * 0.85 + R(B, -0.35, 0.35);
    b->dx = R(B, 0.05, 0.9);
    b->leg_ph = rnd(B);
    b->peck = R(B, 0, 1);
    b->peck_rate = R(B, 2.8, 4);
    b->flap_ph = rnd(B);
  }
  for (int i = 0; i < MAX_SAND; i++) {
    Sand *b = &B->sand[i];
    b->x = edge_at(B, b->x, b->z).edge - 0.3 - b->dx;
  }

  static const struct { const Strip *s; int v; double x, z; } GRACKLES[MAX_GRACKLE] = {
    { &STRIP_HOTEL_S, 0, -26.2, 49 }, { &STRIP_HOTEL_S, 1, -25.4, 51.5 }, { &STRIP_PARK, 0, -12.4, 47 },
    { &STRIP_PATIO, 0, -26.4, -9 }, { &STRIP_HOTEL_S, 0, -26.8, 57 },
  };
  for (int i = 0; i < MAX_GRACKLE; i++) {
    Grackle *b = &B->grackle[i];
    *b = (Grackle){ .i = i, .strip = GRACKLES[i].s, .v = GRACKLES[i].v, .scale = GRACKLES[i].v ? 0.82 : 1,
                    .x = GRACKLES[i].x, .z = GRACKLES[i].z, .mode = K_WALK, .target = { GRACKLES[i].x, GRACKLES[i].z },
                    .tail = 0.06, .shade = 1 /* b.shade ?? 1 */ };
    b->yaw = R(B, -3, 3);
    b->dur = R(B, 1, 3);
    b->flap_ph = rnd(B);
  }
  double sun_h = js_hypot2(g_sun_dir.x, g_sun_dir.z);
  B->shade_x = -g_sun_dir.x / sun_h;
  B->shade_z = -g_sun_dir.z / sun_h;
  int ntrees;
  const PalmTree *trees = palm_trees(&ntrees);
  for (int i = 0; i < ntrees; i++)
    if (trees[i].x < -8 && fabs(trees[i].z) < 90) vec_push(&B->trunks, &trees[i]);

  B->line.t0 = -40; B->line.dir = 1; B->line.x = 340; B->line.y = 26;

  birds_set_quality(B, initial_tier);

  // settle into a natural state (deterministic: seeded, no player, fixed step)
  for (int k = 0; k < 360; k++) step(B, 1.0 / 30);
  commit_all(B);
  return B;
}

void birds_update(Birds *B, double dt, const Camera *camera) {
  if (!B->shot) {
    B->player = camera->node->position;
    step(B, fmin(dt, 0.1));
  }
  V3 sun_view = v3_transform_dir(g_sun_dir, camera->view);
  Species *all[5] = { &B->pelicans, &B->gulls, &B->sands, &B->grackles, &B->frigates };
  for (int i = 0; i < 5; i++) mat_set_vec3(all[i]->lit, "uSunView", sun_view);
  B->frustum = frustum_from_m4(m4_mul(camera->projection, camera->view));
  commit_all(B);
}

bool birds_gull_source(Birds *B, V3 L, BirdSource *out) {
  Gull *vis[MAX_GULL * 3], *near[MAX_GULL];
  int nv = 0, nn = 0;
  for (int i = 0; i < MAX_GULL; i++) {
    Gull *g = &B->gull[i];
    if (!g->active) continue;
    double d = js_hypot3(g->pos.x - L.x, g->pos.y - L.y, g->pos.z - L.z);
    if (d > 150) continue;
    near[nn++] = g;
    if (d < 110 && frustum_contains_point(&B->frustum, g->pos)) {
      vis[nv++] = g;
      if (g->mode != G_STAND && g->mode != G_WALK) { vis[nv++] = g; vis[nv++] = g; }   // flying gulls weigh 3x
    }
  }
  Gull **pool = nv ? vis : near;
  int np = nv ? nv : nn;
  if (!np) return false;
  Gull *g = pool[(int)floor(SDL_randf() * np)];
  if (g->mode == G_STAND) g->call_until = B->T + 1.6;
  double f = sin(g->yaw), h = cos(g->yaw);
  *out = (BirdSource){ g->pos.x, g->pos.y + 0.2, g->pos.z, f * g->speed, g->vy, h * g->speed };
  return true;
}
