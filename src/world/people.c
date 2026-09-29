// Port of src/world/people.js. Exactness notes: `x ** 2` is x * x and other powers are pow();
// JS object literals and argument lists are evaluated in order, so every rnd() and the loft
// callbacks run in the JS order; Float32 attributes are rounded where the JS rounds them.
#include "world/people.h"

#include <math.h>
#include <string.h>

#include "gfx/three_mat.h"
#include "world/layout.h"
#include "world/sky.h"

static constexpr double D2R = PI_D / 180;
static constexpr double TAU = PI_D * 2;
static constexpr double CULL = 150;               // m: figures further than this are hidden
static constexpr double TERRACE_Y = CURB_HEIGHT + 0.45;

static double promenade_x(double z) { return PARK.promenadeX + 2.6 * sin(z / 19) + 1.2 * sin(z / 7.3); }
static double clampd_(double v, double a, double b) { return fmin(b, fmax(a, v)); }
static double smooth(double a, double b, double v) { double t = clampd_((v - a) / (b - a), 0, 1); return t * t * (3 - 2 * t); }
static double lerp(double a, double b, double t) { return a + (b - a) * t; }
static double wrap_angle(double a) { return atan2(sin(a), cos(a)); }

// ---------------------------------------------------------------------------------------------
// Bones (every figure gets the full set; the bike bones only carry geometry on the cyclist)
enum {
  ROOT, PELVIS, SPINE, CHEST, NECK, HEAD, THIGH_L, SHIN_L, FOOT_L, TOE_L, THIGH_R, SHIN_R, FOOT_R, TOE_R,
  ARM_L, FORE_L, HAND_L, ARM_R, FORE_R, HAND_R, PONY, STEER, WHEEL_F, WHEEL_R, CRANK, PEDAL_L, PEDAL_R, NBONES
};
static const int PARENT[NBONES] = { -1, 0, 1, 2, 3, 4, 1, 6, 7, 8, 1, 10, 11, 12, 3, 14, 15, 3, 17, 18, 5, 0, 21, 0, 0, 24, 24 };

// material zones: [roughness, sheen, metalness]
typedef struct Mat3 { double r, s, m; } Mat3;
static const Mat3 M_SKIN = { 0.52, 0.18, 0 }, M_COTTON = { 0.86, 0.75, 0 }, M_LINEN = { 0.9, 0.6, 0 }, M_TECH = { 0.6, 0.35, 0 },
  M_SHOE = { 0.62, 0.25, 0 }, M_SOLE = { 0.85, 0.05, 0 }, M_HAIR = { 0.48, 0.55, 0 }, M_LIPS = { 0.4, 0.1, 0 },
  M_PAINT = { 0.32, 0.0, 0.05 }, M_CHROME = { 0.22, 0, 1 }, M_RUBBER = { 0.85, 0.05, 0 }, M_LEATHER = { 0.5, 0.15, 0 },
  M_WICKER = { 0.85, 0.3, 0 }, M_LENS = { 0.1, 0, 0.6 }, M_APRON = { 0.8, 0.8, 0 };

typedef struct J3 { double v[3]; } J3;

// Joint rest positions (person-local, feet at y = 0, facing +z, left = +x) for a 1.75 m man.
static void joints(double s, bool fem, J3 *J) {
  double hx = (fem ? 0.093 : 0.09) * s, sx = (fem ? 0.163 : 0.182) * s;
  J[ROOT] = (J3){ { 0, 0, 0 } };
  J[PELVIS] = (J3){ { 0, 0.97 * s, 0 } };
  J[SPINE] = (J3){ { 0, 1.08 * s, 0 } };
  J[CHEST] = (J3){ { 0, 1.22 * s, 0 } };
  J[NECK] = (J3){ { 0, 1.48 * s, -0.01 * s } };
  J[HEAD] = (J3){ { 0, 1.585 * s, 0.0 } };
  static const int LEGS[2][5] = { { 1, THIGH_L, SHIN_L, FOOT_L, TOE_L }, { -1, THIGH_R, SHIN_R, FOOT_R, TOE_R } };
  for (int k = 0; k < 2; k++) {
    double side = LEGS[k][0];
    J[LEGS[k][1]] = (J3){ { side * hx, 0.91 * s, 0 } };
    J[LEGS[k][2]] = (J3){ { side * hx, 0.485 * s, 0.004 * s } };
    J[LEGS[k][3]] = (J3){ { side * hx, 0.08 * s, 0 } };
    J[LEGS[k][4]] = (J3){ { side * hx, 0.025 * s, 0.135 * s } };
  }
  static const int ARMS[2][4] = { { 1, ARM_L, FORE_L, HAND_L }, { -1, ARM_R, FORE_R, HAND_R } };
  for (int k = 0; k < 2; k++) {
    double side = ARMS[k][0];
    J[ARMS[k][1]] = (J3){ { side * sx, 1.43 * s, -0.012 * s } };
    J[ARMS[k][2]] = (J3){ { side * sx, 1.14 * s, -0.018 * s } };
    J[ARMS[k][3]] = (J3){ { side * sx, 0.885 * s, -0.005 * s } };
  }
  J[PONY] = (J3){ { 0, 1.705 * s, -0.078 * s } };
  // bike (not scaled): steering at the head tube top, axles, bottom bracket, pedals
  J[STEER] = (J3){ { 0, 0.84, 0.34 } };
  J[WHEEL_F] = (J3){ { 0, 0.35, 0.56 } };
  J[WHEEL_R] = (J3){ { 0, 0.35, -0.54 } };
  J[CRANK] = (J3){ { 0, 0.29, -0.06 } };
  J[PEDAL_L] = (J3){ { 0.135, 0.29 + 0.17, -0.06 } };
  J[PEDAL_R] = (J3){ { -0.135, 0.29 - 0.17, -0.06 } };
}

// ---------------------------------------------------------------------------------------------
// Geometry assembly

typedef struct PV { double p[3]; Color c; Mat3 m; int b[2]; double w[2]; } PV;   // a vertex
typedef struct Mesher { DVec P, N, C, M, SI, SW; U32Vec I; int n; } Mesher;

static void pv_w1(PV *v, int b) { v->b[0] = b; v->b[1] = 0; v->w[0] = 1; v->w[1] = 0; }
static void pv_w2(PV *v, int b0, int b1, double t) { v->b[0] = b0; v->b[1] = b1; v->w[0] = 1 - t; v->w[1] = t; }

static void ms_push_vertex(Mesher *ms, const PV *v, double nx, double ny, double nz) {
  vec_append(&ms->P, v->p, 3);
  double n[3] = { nx, ny, nz }, c[3] = { v->c.r, v->c.g, v->c.b }, m[3] = { v->m.r, v->m.s, v->m.m };
  double si[4] = { v->b[0], v->b[1], 0, 0 }, sw[4] = { v->w[0], v->w[1], 0, 0 };
  vec_append(&ms->N, n, 3);
  vec_append(&ms->C, c, 3);
  vec_append(&ms->M, m, 3);
  vec_append(&ms->SI, si, 4);
  vec_append(&ms->SW, sw, 4);
}

// verts + tris (flat index list); ring: [first vertex, count] of a cross-section whose centre is
// inside the surface (the winding is flipped if that ring's normals point inward)
static void ms_add(Mesher *ms, const PV *verts, int nv, uint32_t *tris, int nt, int r0, int rn) {
  Geometry *g = geo_new();
  float *pos = geo_set_attr(g, "position", 3, nv);
  for (int i = 0; i < nv; i++) for (int k = 0; k < 3; k++) pos[i * 3 + k] = (float)verts[i].p[k];
  geo_set_index(g, tris, nt);
  geo_compute_vertex_normals(g);
  double cen[3] = { 0, 0, 0 };
  for (int i = r0; i < r0 + rn; i++) for (int k = 0; k < 3; k++) cen[k] += verts[i].p[k] / rn;
  const float *nrm = geo_data(g, "normal");
  double dot = 0;
  for (int i = r0; i < r0 + rn; i++) {
    const double *p = verts[i].p;
    dot += (p[0] - cen[0]) * nrm[i * 3] + (p[1] - cen[1]) * nrm[i * 3 + 1] + (p[2] - cen[2]) * nrm[i * 3 + 2];
  }
  if (dot < 0) {
    for (int i = 0; i < nt; i += 3) { uint32_t t = tris[i + 1]; tris[i + 1] = tris[i + 2]; tris[i + 2] = t; }
    geo_set_index(g, tris, nt);
    geo_compute_vertex_normals(g);
    nrm = geo_data(g, "normal");
  }
  for (int i = 0; i < nv; i++) ms_push_vertex(ms, &verts[i], nrm[i * 3], nrm[i * 3 + 1], nrm[i * 3 + 2]);
  for (int i = 0; i < nt; i++) vec_push(&ms->I, tris[i] + (uint32_t)ms->n);
  ms->n += nv;
  geo_free(g);
}

static Geometry *ms_geometry(Mesher *ms) {
  Geometry *g = geo_new();
  int n = ms->n;
  geo_set_attr_d(g, "position", 3, n, ms->P.data);
  geo_set_attr_d(g, "normal", 3, n, ms->N.data);
  geo_set_attr_d(g, "color", 3, n, ms->C.data);
  geo_set_attr_d(g, "aMat", 3, n, ms->M.data);
  geo_set_attr_d(g, "skinIndex", 4, n, ms->SI.data);   // (Uint16 in the JS: small integers, exact)
  geo_set_attr_d(g, "skinWeight", 4, n, ms->SW.data);
  geo_set_index(g, ms->I.data, (int)ms->I.len);
  return g;
}

// Loft a closed tube: the caller fills (nu + 1) rings of nv vertices, u along, v around
typedef struct Loft { int nu, nv; PV *v; } Loft;
static Loft loft_new(int nu, int nv) { return (Loft){ nu, nv, xcalloc((size_t)(nu + 1) * nv + 2, sizeof(PV)) }; }
#define LOFT_EACH(L, i, j, u, v) \
  for (int i = 0; i <= (L).nu; i++) \
    for (int j = 0; j < (L).nv; j++) \
      for (double u = (double)i / (L).nu, v = (double)j / (L).nv, once_ = 1; once_; once_ = 0)
static void loft_end(Mesher *ms, Loft *L, bool cap_start, bool cap_end) {
  int nu = L->nu, nv = L->nv, nverts = (nu + 1) * nv;
  U32Vec tris = {};
  for (int i = 0; i < nu; i++)
    for (int j = 0; j < nv; j++) {
      int j1 = (j + 1) % nv;
      uint32_t a = (uint32_t)(i * nv + j), b = (uint32_t)(i * nv + j1), c = (uint32_t)((i + 1) * nv + j), d = (uint32_t)((i + 1) * nv + j1);
      uint32_t q[6] = { a, c, b, b, c, d };
      vec_append(&tris, q, 6);
    }
  for (int e = 0; e < 2; e++) {
    bool start = e == 0;
    if (start ? !cap_start : !cap_end) continue;
    int ring = start ? 0 : nu;
    double p[3] = { 0, 0, 0 };
    for (int j = 0; j < nv; j++) for (int k = 0; k < 3; k++) p[k] += L->v[ring * nv + j].p[k] / nv;
    int k = nverts++;
    L->v[k] = L->v[ring * nv];
    memcpy(L->v[k].p, p, sizeof p);
    for (int j = 0; j < nv; j++) {
      uint32_t a = (uint32_t)(ring * nv + j), b = (uint32_t)(ring * nv + (j + 1) % nv);
      uint32_t q[3] = { (uint32_t)k, start ? a : b, start ? b : a };
      vec_append(&tris, q, 3);
    }
  }
  ms_add(ms, L->v, nverts, tris.data, (int)tris.len, (nu / 2) * nv, nv);
  vec_free(&tris);
  free(L->v);
}

// Hermite-interpolated profile keys: rows of 1 + nc doubles [t, values...] (clamped at the ends)
static void profile(const double *keys, int n, int nc, double t, double *out) {
  int W = nc + 1;
#define K(i, c) keys[(i) * W + (c)]
  if (t <= K(0, 0)) { for (int c = 0; c < nc; c++) out[c] = K(0, c + 1); return; }
  if (t >= K(n - 1, 0)) { for (int c = 0; c < nc; c++) out[c] = K(n - 1, c + 1); return; }
  int i = 0;
  while (K(i + 1, 0) < t) i++;
  int i0 = i > 0 ? i - 1 : 0, i3 = i + 2 < n - 1 ? i + 2 : n - 1;
  double h = K(i + 1, 0) - K(i, 0), s = (t - K(i, 0)) / h;
  double s3 = pow(s, 3), h00 = 2 * s3 - 3 * s * s + 1, h10 = s3 - 2 * s * s + s, h01 = -2 * s3 + 3 * s * s, h11 = s3 - s * s;
  for (int c = 1; c <= nc; c++) {
    double m1 = (K(i + 1, c) - K(i0, c)) / fmax(1e-6, K(i + 1, 0) - K(i0, 0));
    double m2 = (K(i3, c) - K(i, c)) / fmax(1e-6, K(i3, 0) - K(i, 0));
    out[c - 1] = h00 * K(i, c) + h10 * h * m1 + h01 * K(i + 1, c) + h11 * h * m2;
  }
#undef K
}
#define PROFILE(keys, t, out) profile(&(keys)[0][0], (int)ARRAY_LEN(keys), (int)ARRAY_LEN((keys)[0]) - 1, (t), (out))

// periodic keys [t, value] over [0, 1)
static double cyc(const double (*keys)[2], int n, double p) {
  p -= floor(p);
  int i = n - 1;
  for (int k = 0; k < n; k++) if (keys[k][0] <= p) i = k;
#define AT(k, o) do { int q_ = (((k) % n) + n) % n; double w_ = floor((double)(k) / n); o[0] = keys[q_][0] + w_; o[1] = keys[q_][1]; } while (0)
  double k0[2], k1[2], k2[2], k3[2];
  AT(i - 1, k0); AT(i, k1); AT(i + 1, k2); AT(i + 2, k3);
#undef AT
  double h = k2[0] - k1[0], s = (p - k1[0]) / h;
  double m1 = (k2[1] - k0[1]) / (k2[0] - k0[0]), m2 = (k3[1] - k1[1]) / (k3[0] - k1[0]);
  double s3 = pow(s, 3);
  return (2 * s3 - 3 * s * s + 1) * k1[1] + (s3 - 2 * s * s + s) * h * m1 + (-2 * s3 + 3 * s * s) * k2[1] + (s3 - s * s) * h * m2;
}

static double se(double q, double n) { return js_sign(q) * pow(fabs(q), 2 / n); }

// ---------------------------------------------------------------------------------------------
// Outfits

typedef enum TopKind { TOP_TANK, TOP_SHIRT, TOP_TEE } TopKind;
typedef enum HairStyle { HAIR_SHORT, HAIR_PONYTAIL, HAIR_BUN } HairStyle;
typedef struct Outfit {
  double height;
  bool fem;
  double girth;
  Color skin;
  struct { Color col; HairStyle style; } hair;
  bool has_top;
  struct { Color col; Mat3 mat; TopKind kind; double hem, sleeve, off; bool roll; } top;
  bool has_bottom;
  struct { Color col; Mat3 mat; double waist, hem, off; bool cuff; } bottom;
  bool has_socks;
  struct { Color col; double top; } socks;
  bool has_shoes;
  struct { Color col, sole, accent; bool has_accent; } shoes;
  bool has_sandals, has_apron, has_cloth, has_glasses, has_bike;
  Color sandals, apron, cloth, glasses, bike_frame;
  double bike_hip[2];
} Outfit;

typedef struct Zone { Color c; Mat3 m; double off; } Zone;
typedef enum Part { P_TORSO, P_THIGH, P_SHIN, P_ARM, P_FORE, P_HAND } Part;

static Zone zone(const Outfit *o, double s, Part part, double x, double y, double z, double lz) {
#define S(v) ((v) * s)
  Zone skin = { o->skin, M_SKIN, 0 };
  Zone top = o->has_top ? (Zone){ o->top.col, o->top.mat, o->top.off * s } : skin;
  Zone bot = o->has_bottom ? (Zone){ o->bottom.col, o->bottom.mat, o->bottom.off * s } : skin;
  Zone apron = o->has_apron ? (Zone){ o->apron, M_APRON, 0.013 * s } : skin;
  double ax = fabs(x);
  if (part == P_TORSO) {
    if (o->has_apron && z > 0.01 * s && y < S(1.335) && y > S(0.84) && ax < (y > S(1.07) ? S(0.105) : S(0.175))) return apron;
    if (o->has_apron && y > S(1.07) && y < S(1.1) && z <= 0.01 * s) return apron;   // waist ties
    if (o->has_top && y >= S(o->top.hem)) {
      if (y > S(1.462)) return skin;
      if (o->top.kind == TOP_TANK) {
        if (y > S(1.33) && ax > S(0.098)) return skin;
        if (y > S(1.35) && ax < S(0.066) && z > 0) return skin;
        if (y > S(1.41) && ax < S(0.06) && z < 0) return skin;
      }
      if (o->top.kind == TOP_SHIRT && z > 0 && y > S(1.38) && ax < (y - S(1.38)) * 1.2) return skin;
      if (o->top.kind == TOP_SHIRT && y > S(1.44) && y < S(1.462)) { top.off = top.off + 0.006 * s; return top; }   // collar
      return top;
    }
    if (o->has_bottom && y <= S(o->bottom.waist)) return bot;
    return skin;
  }
  if (part == P_THIGH || part == P_SHIN) {
    if (o->has_apron && part == P_THIGH && lz > -0.02 * s && y > S(0.55)) return apron;
    if (o->has_top && o->top.hem < 0.95 && part == P_THIGH && y >= S(o->top.hem)) { top.off = top.off + 0.01 * s; return top; }
    if (o->has_bottom && y >= S(o->bottom.hem)) {
      if (o->bottom.cuff && y < S(o->bottom.hem + 0.05)) { bot.off = bot.off + 0.009 * s; return bot; }
      return bot;
    }
    if (o->has_socks && y < S(o->socks.top)) return (Zone){ o->socks.col, M_COTTON, 0.004 * s };
    return skin;
  }
  if (part == P_ARM || part == P_FORE) {
    if (o->has_top && y >= S(o->top.sleeve)) {
      if (o->top.roll && y < S(o->top.sleeve + 0.05)) { top.off = top.off + 0.01 * s; return top; }
      return top;
    }
    return skin;
  }
  return skin;
#undef S
}

// ---------------------------------------------------------------------------------------------
// Body

static const double TORSO_M[12][4] = {   // y (1.75 m man), half-width, front depth, back depth
  { 0.83, 0.12, 0.08, 0.085 }, { 0.88, 0.162, 0.1, 0.118 }, { 0.94, 0.172, 0.1, 0.128 }, { 1.0, 0.163, 0.095, 0.108 },
  { 1.07, 0.148, 0.094, 0.094 }, { 1.15, 0.153, 0.1, 0.094 }, { 1.23, 0.163, 0.114, 0.1 }, { 1.31, 0.173, 0.12, 0.102 },
  { 1.38, 0.178, 0.1, 0.1 }, { 1.43, 0.162, 0.072, 0.084 }, { 1.47, 0.09, 0.05, 0.06 }, { 1.505, 0.058, 0.045, 0.05 },
};
static const double TORSO_F[12][4] = {
  { 0.83, 0.125, 0.08, 0.09 }, { 0.88, 0.172, 0.1, 0.125 }, { 0.94, 0.182, 0.1, 0.132 }, { 1.0, 0.168, 0.092, 0.11 },
  { 1.07, 0.132, 0.085, 0.085 }, { 1.15, 0.136, 0.09, 0.088 }, { 1.23, 0.148, 0.1, 0.093 }, { 1.31, 0.155, 0.1, 0.094 },
  { 1.38, 0.16, 0.09, 0.092 }, { 1.43, 0.146, 0.068, 0.078 }, { 1.47, 0.082, 0.047, 0.055 }, { 1.505, 0.052, 0.042, 0.046 },
};
static const double THIGH[8][4] = { { -0.06, 0.074, 0.08, -0.01 }, { 0, 0.084, 0.088, 0 }, { 0.15, 0.082, 0.087, 0.006 }, { 0.4, 0.073, 0.077, 0.008 },
  { 0.7, 0.061, 0.064, 0.006 }, { 0.9, 0.051, 0.054, 0.003 }, { 1.0, 0.048, 0.052, 0.005 }, { 1.07, 0.044, 0.046, 0.004 } };
static const double SHIN[7][4] = { { -0.05, 0.046, 0.05, 0.004 }, { 0.05, 0.047, 0.05, 0.002 }, { 0.28, 0.047, 0.056, -0.012 }, { 0.5, 0.04, 0.046, -0.008 },
  { 0.75, 0.031, 0.033, -0.002 }, { 0.93, 0.027, 0.029, 0 }, { 1.03, 0.028, 0.03, 0 } };
static const double UPPER[7][4] = { { -0.15, 0.022, 0.026, 0 }, { -0.09, 0.044, 0.047, 0 }, { 0, 0.051, 0.053, 0 }, { 0.18, 0.048, 0.051, -0.002 },
  { 0.5, 0.041, 0.043, 0 }, { 0.85, 0.035, 0.038, 0 }, { 1.03, 0.033, 0.035, 0 } };
static const double FORE[5][4] = { { -0.06, 0.032, 0.035, 0 }, { 0.15, 0.036, 0.037, 0.004 }, { 0.5, 0.03, 0.029, 0.002 }, { 0.85, 0.021, 0.026, 0 },
  { 1.03, 0.018, 0.025, 0 } };
static const double HAND[6][4] = { { -0.04, 0.016, 0.024, 0 }, { 0.15, 0.02, 0.037, 0.003 }, { 0.45, 0.019, 0.042, 0.005 }, { 0.62, 0.016, 0.039, 0.008 },
  { 0.85, 0.012, 0.031, 0.012 }, { 1.0, 0.006, 0.018, 0.014 } };

static void build_head(Mesher *ms, const Outfit *o, const J3 *J, double s);
static void build_foot(Mesher *ms, const Outfit *o, const J3 *J, int ft, int tb, double s);

static void limb(Mesher *ms, const Outfit *o, const J3 *J, double s, int bone, int parent, int child, Part part,
                 const double (*keys)[4], int nkeys, double len, double t0, double t1, int nu, int nv, double k) {
  const double *O = J[bone].v;
  Loft L = loft_new(nu, nv);
  LOFT_EACH(L, i, j, u, v) {
    double t = lerp(t0, t1, u), pr[3];
    profile(&keys[0][0], nkeys, 3, t, pr);
    double th = v * TAU, c = cos(th), sn = sin(th);
    double rx = pr[0] * s * k, rz = pr[1] * s * k;
    double x = O[0] + rx * c, y = O[1] - t * len, lz = pr[2] * s + rz * sn, z = O[2] + lz;
    Zone zn = zone(o, s, part, x, y, z, lz);
    PV *pv = &L.v[i * nv + j];
    *pv = (PV){ { x + c * zn.off, y, z + sn * zn.off }, zn.c, zn.m, {}, {} };
    pv_w1(pv, bone);
    if (child >= 0 && t > 0.8) pv_w2(pv, bone, child, 0.5 * smooth(0.8, 1.0, t));
    if (parent >= 0 && t < 0.2) pv_w2(pv, bone, parent, 0.5 * (1 - smooth(-0.05, 0.2, t)));
  }
  loft_end(ms, &L, true, true);
}

static void build_body(Mesher *ms, const Outfit *o, const J3 *J) {
  double s = o->height / 1.75, g = o->girth, lim = o->fem ? 0.9 : 1;
  bool fem = o->fem;
  // torso: superellipse sections, pelvis -> spine -> chest weights
  const double (*T)[4] = fem ? TORSO_F : TORSO_M;
  double y0 = T[0][0], y1 = T[11][0];
  double bust = fem ? 0.03 : 0;
  {
    Loft L = loft_new(26, 22);
    LOFT_EACH(L, i, j, u, v) {
      double yy = lerp(y0, y1, u), pr[3];
      profile(&T[0][0], 12, 3, yy, pr);
      double a0 = pr[0], bf = pr[1], bb = pr[2];
      double th = v * TAU, c = cos(th), sn = sin(th);
      double n = lerp(2.5, 2.2, smooth(1.0, 1.2, yy));
      double a = a0 * s * g, x = a * se(c, n);
      double z = (sn > 0 ? bf : bb) * s * g * se(sn, n);
      if (sn > 0 && bust) {
        double q1 = (yy - 1.27) / 0.05, q2 = (fabs(x) / s - 0.062) / 0.05;
        z += bust * s * exp(-(q1 * q1)) * exp(-(q2 * q2));
      }
      double y = yy * s;
      Zone zn = zone(o, s, P_TORSO, x, y, z, z);
      double r = js_hypot2(x, z);
      if (r == 0) r = 1;
      PV *pv = &L.v[i * L.nv + j];
      *pv = (PV){ { x + (x / r) * zn.off, y, z + (z / r) * zn.off }, zn.c, zn.m, {}, {} };
      if (yy < 0.99) pv_w1(pv, PELVIS);
      else if (yy < 1.11) pv_w2(pv, PELVIS, SPINE, smooth(0.99, 1.11, yy));
      else if (yy < 1.24) pv_w2(pv, SPINE, CHEST, smooth(1.11, 1.24, yy));
      else pv_w2(pv, CHEST, NECK, 0.35 * smooth(1.45, 1.505, yy));
    }
    loft_end(ms, &L, true, true);
  }
  // neck
  {
    const double *nk = J[NECK].v;
    Loft L = loft_new(6, 14);
    LOFT_EACH(L, i, j, u, v) {
      double th = v * TAU, y = lerp(1.44, 1.64, u) * s;
      double r = lerp(0.056, 0.05, u) * s * lim * (fem ? 1 : 1.04);
      PV *pv = &L.v[i * L.nv + j];
      *pv = (PV){ { r * cos(th), y, nk[2] + 0.012 * s * u + r * 1.04 * sin(th) }, o->skin, M_SKIN, {}, {} };
      if (u < 0.5) pv_w2(pv, CHEST, NECK, 0.3 + u);
      else pv_w2(pv, NECK, HEAD, (u - 0.5) * 1.2);
    }
    loft_end(ms, &L, true, true);
  }
  build_head(ms, o, J, s);
  // limbs along -y from their joint
  static const int SIDES[2][7] = { { THIGH_L, SHIN_L, FOOT_L, TOE_L, ARM_L, FORE_L, HAND_L },
                                   { THIGH_R, SHIN_R, FOOT_R, TOE_R, ARM_R, FORE_R, HAND_R } };
  for (int sd = 0; sd < 2; sd++) {
    int th = SIDES[sd][0], sh = SIDES[sd][1], ft = SIDES[sd][2], tb = SIDES[sd][3], ar = SIDES[sd][4], fo = SIDES[sd][5], ha = SIDES[sd][6];
    double lt = J[th].v[1] - J[sh].v[1], ls = J[sh].v[1] - J[ft].v[1];
    limb(ms, o, J, s, th, PELVIS, sh, P_THIGH, THIGH, 8, lt, -0.06, 1.07, 18, 16, (fem ? 1.0 : 1) * g);
    limb(ms, o, J, s, sh, th, ft, P_SHIN, SHIN, 7, ls, -0.05, 1.03, 16, 14, lim * g);
    double la = J[ar].v[1] - J[fo].v[1], lf = J[fo].v[1] - J[ha].v[1];
    limb(ms, o, J, s, ar, CHEST, fo, P_ARM, UPPER, 7, la, -0.15, 1.03, 14, 14, lim * g);
    limb(ms, o, J, s, fo, ar, ha, P_FORE, FORE, 5, lf, -0.06, 1.03, 12, 12, lim);
    limb(ms, o, J, s, ha, fo, -1, P_HAND, HAND, 6, 0.185 * s, -0.04, 1.0, 10, 10, lim);
    // thumb, rigid on the hand
    const double *H = J[ha].v;
    double side = js_sign(H[0]);
    Loft L = loft_new(5, 8);
    LOFT_EACH(L, i, j, u, v) {
      double th2 = v * TAU, r = lerp(0.012, 0.008, u) * s * lim;
      double a[3] = { H[0] - side * 0.004 * s, H[1] - 0.035 * s, H[2] + 0.028 * s };
      double d[3] = { 0, -0.72, 0.69 }, Ln = 0.065 * s * u;
      PV *pv = &L.v[i * L.nv + j];
      *pv = (PV){ { a[0] + r * cos(th2), a[1] + d[1] * Ln + r * 0.7 * sin(th2) * 0.7, a[2] + d[2] * Ln + r * sin(th2) * 0.7 },
                  o->skin, M_SKIN, {}, {} };
      pv_w1(pv, ha);
    }
    loft_end(ms, &L, true, true);
    build_foot(ms, o, J, ft, tb, s);
  }
  if (o->hair.style == HAIR_PONYTAIL) {
    const double *P0 = J[PONY].v;
    static const double PK[4][2] = { { 0, 0.026 }, { 0.25, 0.03 }, { 0.7, 0.02 }, { 1, 0.004 } };
    Loft L = loft_new(10, 10);
    LOFT_EACH(L, i, j, u, v) {
      double th = v * TAU, r;
      PROFILE(PK, u, &r);
      r = r * s;
      double Ln = 0.23 * s * u;
      PV *pv = &L.v[i * L.nv + j];
      *pv = (PV){ { P0[0] + r * cos(th), P0[1] - Ln * 0.92 + 0.02 * s * sin(u * 3), P0[2] - Ln * 0.38 + r * sin(th) }, o->hair.col, M_HAIR, {}, {} };
      pv_w1(pv, PONY);
    }
    loft_end(ms, &L, true, true);
  }
  if (o->hair.style == HAIR_BUN) {
    double C[3] = { 0, J[HEAD].v[1] + 0.13 * s, -0.095 * s };
    Loft L = loft_new(8, 12);
    LOFT_EACH(L, i, j, u, v) {
      double ph = lerp(0.1, PI_D - 0.1, u), th = v * TAU, r = 0.042 * s;
      PV *pv = &L.v[i * L.nv + j];
      *pv = (PV){ { C[0] + r * sin(ph) * cos(th), C[1] + r * 0.9 * -cos(ph), C[2] + r * 0.85 * sin(ph) * sin(th) }, o->hair.col, M_HAIR, {}, {} };
      pv_w1(pv, HEAD);
    }
    loft_end(ms, &L, true, true);
  }
}

static void build_head(Mesher *ms, const Outfit *o, const J3 *J, double s) {
  const double *H = J[HEAD].v;
  double f = o->fem ? 0.95 : 1;
  double C[3] = { 0, H[1] + 0.062 * s, 0.013 * s };
  double R[3] = { 0.075 * s * f, 0.106 * s * f, 0.095 * s * f };
  bool tight = o->hair.style == HAIR_PONYTAIL || o->hair.style == HAIR_BUN;
  Color lip = color_mul(o->skin, color_rgb(0.82, 0.62, 0.6));
  Loft L = loft_new(20, 24);
  LOFT_EACH(L, i, j, u, v) {
    double ph = lerp(0.04, PI_D - 0.04, u), th = v * TAU - PI_D / 2;
    double dx = sin(ph) * cos(th), dy = -cos(ph), dz = sin(ph) * sin(th);
    // v = 0.25 faces +z (front)
    double kx = 1, ky = 1, kz = 1;
    if (dy < 0) kx *= 1 - 0.3 * dy * dy;                      // jaw narrows
    if (dy < 0 && dz < 0) kz *= 1 - 0.45 * (-dy) * (-dz);     // under the skull, into the neck
    if (dy > 0 && dz < 0) kz *= 1 + 0.07 * dy * (-dz);        // occiput
    if (dz > 0.3) kz *= 1 - 0.05 * dz;                        // flatter face
    double x = dx * R[0] * kx, y = dy * R[1] * ky, z = dz * R[2] * kz;
    double front = fmax(0, dz);
    // nose (bridge to tip), brow, eye sockets, cheekbones, chin, ears
    double q1 = dx / 0.1, q2 = (dy + 0.08) / 0.15;
    double nose = 0.021 * s * exp(-(q1 * q1) - (q2 * q2)) * (0.45 + 0.55 * smooth(0.12, -0.2, dy)) * pow(front, 3);
    double q3 = (dy - 0.2) / 0.07;
    double brow = 0.005 * s * exp(-(q3 * q3)) * pow(front, 4);
    double q4 = (fabs(dx) - 0.33) / 0.12, q5 = (dy - 0.1) / 0.08;
    double eye = -0.007 * s * exp(-(q4 * q4) - (q5 * q5)) * front;
    double q6 = (fabs(dx) - 0.55) / 0.15, q7 = (dy + 0.05) / 0.12;
    double cheek = 0.004 * s * exp(-(q6 * q6) - (q7 * q7)) * front;
    double q8 = dx / 0.25, q9 = (dy + 0.82) / 0.12;
    double chin = 0.008 * s * exp(-(q8 * q8) - (q9 * q9)) * front;
    double q10 = (dz + 0.12) / 0.14, q11 = dy / 0.2;
    double ear = 0.016 * s * exp(-(q10 * q10) - (q11 * q11)) * pow(fabs(dx), 10);
    double bump = nose + brow + eye + cheek + chin + ear;
    double rl = js_hypot3(dx, dy, dz);
    if (rl == 0) rl = 1;
    // hair: above a hairline that sits high at the forehead, above the ears, low at the nape
    double hl = 0.12 + 0.32 * pow(front, 1.4) - 0.55 * fmax(0, -dz) - (tight ? 0 : 0.05) * fmax(0, -dz);
    double hk = smooth(hl, hl + 0.08, dy);
    double thick = (tight ? 0.007 : o->hair.style == HAIR_SHORT ? 0.012 : 0.016) * s * hk * (1 + 0.4 * fmax(0, dy));
    double off = bump + thick;
    x += (dx / rl) * off; y += (dy / rl) * off; z += (dz / rl) * off;
    Color c = o->skin;
    Mat3 m = M_SKIN;
    if (hk > 0.5) { c = o->hair.col; m = M_HAIR; }
    else if (front > 0.9 && fabs(dy + 0.5) < 0.05 && fabs(dx) < 0.3) { c = lip; m = M_LIPS; }
    if (o->has_glasses && front > 0.55 && fabs(dy - 0.11) < 0.055 && hk < 0.5) {
      c = o->glasses; m = M_LENS;
      x += (dx / rl) * 0.006 * s; z += (dz / rl) * 0.006 * s;
    }
    PV *pv = &L.v[i * L.nv + j];
    *pv = (PV){ { C[0] + x, C[1] + y, C[2] + z }, c, m, {}, {} };
    if (dy < -0.55 && dz < 0.2) pv_w2(pv, HEAD, NECK, 0.3);
    else pv_w1(pv, HEAD);
  }
  loft_end(ms, &L, true, true);
}

// foot / shoe lofted along +z from the heel; toes skinned to the toe bone
static void build_foot(Mesher *ms, const Outfit *o, const J3 *J, int ft, int tb, double s) {
  const double *A = J[ft].v;
  bool shoe = o->has_shoes;
  double len = (o->fem ? 0.245 : 0.265) * s, z0 = -0.058 * s;
  static const double KS[8][3] = { { 0, 0.03, 0.058 }, { 0.07, 0.038, 0.08 }, { 0.22, 0.043, 0.094 }, { 0.42, 0.046, 0.082 },
                                   { 0.62, 0.05, 0.062 }, { 0.8, 0.051, 0.05 }, { 0.93, 0.045, 0.04 }, { 1, 0.022, 0.03 } };
  static const double KB[8][3] = { { 0, 0.026, 0.05 }, { 0.07, 0.032, 0.074 }, { 0.22, 0.037, 0.088 }, { 0.42, 0.039, 0.07 },
                                   { 0.62, 0.043, 0.045 }, { 0.8, 0.045, 0.03 }, { 0.93, 0.04, 0.022 }, { 1, 0.018, 0.014 } };
  double sole = -0.08 * s - (shoe ? 0.006 * s : 0);
  Loft L = loft_new(16, 14);
  LOFT_EACH(L, i, j, u, v) {
    double wh[2];
    profile(shoe ? &KS[0][0] : &KB[0][0], 8, 2, u, wh);
    double th = v * TAU;
    double w = wh[0] * s, h = wh[1] * s;
    double c = cos(th), sn = sin(th);
    double x = A[0] + w * se(c, 3.2) + (A[0] > 0 ? 1 : -1) * 0.006 * s * u;   // toes splay slightly out
    double yl = sole + h / 2 + (h / 2) * se(sn, sn < 0 ? 5 : 2.4);
    double z = A[2] + z0 + u * len;
    Color col = o->skin;
    Mat3 m = M_SKIN;
    if (shoe) {
      if (yl < sole + 0.02 * s) { col = o->shoes.sole; m = M_SOLE; }
      else {
        col = o->shoes.col; m = M_SHOE;
        if (o->shoes.has_accent && yl < sole + 0.045 * s && u > 0.25 && u < 0.7 && fabs(c) > 0.7) col = o->shoes.accent;
      }
    }
    PV *pv = &L.v[i * L.nv + j];
    *pv = (PV){ { x, A[1] + yl, z }, col, m, {}, {} };
    if (u > 0.66) pv_w2(pv, ft, tb, smooth(0.66, 0.8, u));
    else pv_w1(pv, ft);
  }
  loft_end(ms, &L, true, true);
}

// ---------------------------------------------------------------------------------------------
// Small rigid primitives (bike, props)

typedef struct Pts { V3 *p; int n; } Pts;   // a polyline (n points)

static void tube(Mesher *ms, Pts pts, double r, int nv, int bone, Color col, Mat3 mat, double r_end) {
  int n = pts.n - 1;
  V3 *n1s = xmalloc((size_t)pts.n * sizeof(V3)), *n2s = xmalloc((size_t)pts.n * sizeof(V3));
  for (int i = 0; i < pts.n; i++) {
    V3 a = pts.p[i > 0 ? i - 1 : 0], b = pts.p[i + 1 < n ? i + 1 : n];
    V3 t = v3_norm(v3_sub(b, a));
    V3 up = fabs(t.y) < 0.9 ? v3(0, 1, 0) : v3(1, 0, 0);
    n1s[i] = v3_norm(v3_cross(t, up));
    n2s[i] = v3_norm(v3_cross(t, n1s[i]));
  }
  Loft L = loft_new(n, nv);
  LOFT_EACH(L, i, j, u, v) {
    int k = (int)js_round(u * n);
    double th = v * TAU, rr = lerp(r, r_end, u);
    V3 a = n1s[k], b = n2s[k], p = pts.p[k];
    PV *pv = &L.v[i * L.nv + j];
    *pv = (PV){ { p.x + rr * (a.x * cos(th) + b.x * sin(th)), p.y + rr * (a.y * cos(th) + b.y * sin(th)), p.z + rr * (a.z * cos(th) + b.z * sin(th)) },
                col, mat, {}, {} };
    pv_w1(pv, bone);
  }
  loft_end(ms, &L, true, true);
  free(n1s);
  free(n2s);
}
#define TUBE(ms, pts, r, nv, bone, col, mat) tube((ms), (pts), (r), (nv), (bone), (col), (mat), (r))

// seg(a, b, k): k + 1 evenly spaced points (static storage per call site is not needed: heap)
static Pts seg(V3 a, V3 b, int k) {
  Pts p = { xmalloc((size_t)(k + 1) * sizeof(V3)), k + 1 };
  for (int i = 0; i <= k; i++) {
    double t = (double)i / k;
    p.p[i] = v3(lerp(a.x, b.x, t), lerp(a.y, b.y, t), lerp(a.z, b.z, t));
  }
  return p;
}
static Pts curve(const V3 *points, int n, int k) {
  CatmullRom3 c = curve_catmull(points, n, false, CURVE_CENTRIPETAL, 0.5);
  Pts p = { xmalloc((size_t)(k + 1) * sizeof(V3)), k + 1 };
  curve_points(&c, k, p.p);
  curve_free(&c);
  return p;
}
static void tube_free(Mesher *ms, Pts pts, double r, int nv, int bone, Color col, Mat3 mat) {
  TUBE(ms, pts, r, nv, bone, col, mat);
  free(pts.p);
}

// torus normals computed analytically (around the tube centre line)
static void add_torus(Mesher *ms, PV *verts, int nv, const uint32_t *tris, int nt, const double *c, double R) {
  int base = ms->n;
  for (int i = 0; i < nv; i++) {
    const double *p = verts[i].p;
    double ry = p[1] - c[1], rz = p[2] - c[2], rl = js_hypot2(ry, rz);
    if (rl == 0) rl = 1;
    double cy = c[1] + (ry / rl) * R, cz = c[2] + (rz / rl) * R;
    double nx = p[0] - c[0], ny = p[1] - cy, nz = p[2] - cz, nl = js_hypot3(nx, ny, nz);
    if (nl == 0) nl = 1;
    ms_push_vertex(ms, &verts[i], nx / nl, ny / nl, nz / nl);
  }
  // winding: make the first triangle agree with its vertex normal
  const double *p0 = verts[tris[0]].p, *p1 = verts[tris[1]].p, *p2 = verts[tris[2]].p;
  V3 a = v3(p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]), b = v3(p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]);
  V3 n1 = v3_cross(a, b);
  const double *nn = &ms->N.data[(size_t)(base + (int)tris[0]) * 3];
  bool flip = n1.x * nn[0] + n1.y * nn[1] + n1.z * nn[2] < 0;
  for (int i = 0; i < nt; i += 3) {
    if (flip) { vec_push(&ms->I, tris[i] + (uint32_t)base); vec_push(&ms->I, tris[i + 2] + (uint32_t)base); vec_push(&ms->I, tris[i + 1] + (uint32_t)base); }
    else { vec_push(&ms->I, tris[i] + (uint32_t)base); vec_push(&ms->I, tris[i + 1] + (uint32_t)base); vec_push(&ms->I, tris[i + 2] + (uint32_t)base); }
  }
  ms->n += nv;
}

// torus around the x axis (wheels): centre c, ring radius R, tube radii (radial rr, axial rx)
static void torus_x(Mesher *ms, const double *c, double R, double rr, double rx, int nu, int nv, int bone, Color col, Mat3 mat,
                    double a0, double a1) {
  bool full = a1 - a0 >= TAU - 1e-6;
  int nU = full ? nu : nu + 1;
  PV *verts = xmalloc((size_t)nU * nv * sizeof(PV));
  int k = 0;
  for (int i = 0; i < nU; i++) {
    double a = a0 + (a1 - a0) * ((double)i / nu);
    for (int j = 0; j < nv; j++) {
      double b = ((double)j / nv) * TAU;
      double rad = R + rr * cos(b);
      verts[k] = (PV){ { c[0] + rx * sin(b), c[1] + rad * cos(a), c[2] + rad * sin(a) }, col, mat, {}, {} };
      pv_w1(&verts[k], bone);
      k++;
    }
  }
  U32Vec tris = {};
  for (int i = 0; i < nu; i++) {
    int i1 = full ? (i + 1) % nu : i + 1;
    for (int j = 0; j < nv; j++) {
      int j1 = (j + 1) % nv;
      uint32_t q[6] = { (uint32_t)(i * nv + j), (uint32_t)(i1 * nv + j), (uint32_t)(i * nv + j1),
                        (uint32_t)(i * nv + j1), (uint32_t)(i1 * nv + j), (uint32_t)(i1 * nv + j1) };
      vec_append(&tris, q, 6);
    }
  }
  add_torus(ms, verts, k, tris.data, (int)tris.len, c, R);
  vec_free(&tris);
  free(verts);
}

// axis-aligned (optionally yawed) box: flat-shaded faces
static void box(Mesher *ms, const double *c, const double *h, int bone, Color col, Mat3 mat, double rot_y) {
  static const int FACES[6][4][3] = {
    { { 1, -1, -1 }, { 1, 1, -1 }, { 1, 1, 1 }, { 1, -1, 1 } }, { { -1, -1, 1 }, { -1, 1, 1 }, { -1, 1, -1 }, { -1, -1, -1 } },
    { { -1, 1, -1 }, { -1, 1, 1 }, { 1, 1, 1 }, { 1, 1, -1 } }, { { -1, -1, 1 }, { -1, -1, -1 }, { 1, -1, -1 }, { 1, -1, 1 } },
    { { -1, -1, 1 }, { 1, -1, 1 }, { 1, 1, 1 }, { -1, 1, 1 } }, { { 1, -1, -1 }, { -1, -1, -1 }, { -1, 1, -1 }, { 1, 1, -1 } },
  };
  static const int NRM[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
  double cs = cos(rot_y), sn = sin(rot_y);
  int base = ms->n;
  for (int f = 0; f < 6; f++) {
    for (int q = 0; q < 4; q++) {
      double x = FACES[f][q][0] * h[0], y = FACES[f][q][1] * h[1], z = FACES[f][q][2] * h[2];
      PV v = { { c[0] + x * cs + z * sn, c[1] + y, c[2] - x * sn + z * cs }, col, mat, { bone, 0 }, { 1, 0 } };
      const int *n = NRM[f];
      ms_push_vertex(ms, &v, n[0] * cs + n[2] * sn, n[1], -n[0] * sn + n[2] * cs);
    }
    uint32_t k = (uint32_t)(f * 4);
    uint32_t t[6] = { k, k + 1, k + 2, k, k + 2, k + 3 };
    for (int i = 0; i < 6; i++) vec_push(&ms->I, t[i] + (uint32_t)base);
  }
  ms->n += 24;
}

static void build_bike(Mesher *ms, const Outfit *o, const J3 *J, const double *saddle) {
  Color frame = o->bike_frame, dark = color_hex(0x1d1d1e), chrome = color_hex(0xb9bcbf), tyre = color_hex(0x262626);
  Color brown = color_hex(0x5a3a24), wick = color_hex(0xa88a5c);
  const double *BB = J[CRANK].v;
  // wheels: tyre, rim, hub, 28 crossed spokes
  static const int WHEELS[2] = { WHEEL_F, WHEEL_R };
  for (int w = 0; w < 2; w++) {
    int bone = WHEELS[w];
    const double *c = J[bone].v;
    torus_x(ms, c, 0.331, 0.02, 0.02, 44, 8, bone, tyre, M_RUBBER, 0, TAU);
    torus_x(ms, c, 0.307, 0.008, 0.011, 44, 5, bone, chrome, M_CHROME, 0, TAU);
    tube_free(ms, seg(v3(c[0] - 0.05, c[1], c[2]), v3(c[0] + 0.05, c[1], c[2]), 1), 0.018, 8, bone, chrome, M_CHROME);
    for (int i = 0; i < 28; i++) {
      double a = ((double)i / 28) * TAU, side = i % 2 ? 1 : -1, cross = (i % 4 < 2 ? 1 : -1) * 0.35;
      V3 h = v3(c[0] + side * 0.03, c[1] + 0.024 * cos(a), c[2] + 0.024 * sin(a));
      V3 e = v3(c[0], c[1] + 0.301 * cos(a + cross), c[2] + 0.301 * sin(a + cross));
      tube_free(ms, seg(h, e, 1), 0.0014, 3, bone, chrome, M_CHROME);
    }
  }
  // fenders (frame colour), front on the fork
  torus_x(ms, J[WHEEL_F].v, 0.37, 0.006, 0.03, 18, 6, STEER, frame, M_PAINT, -0.35, 2.3);
  torus_x(ms, J[WHEEL_R].v, 0.37, 0.006, 0.03, 18, 6, ROOT, frame, M_PAINT, 0.9, 3.9);
  // step-through frame: curved down tube, seat tube, stays, head tube
  V3 HTb = v3(0, 0.6, 0.4), HTt = v3(J[STEER].v[0], J[STEER].v[1], J[STEER].v[2]);
  V3 bbv = v3(BB[0], BB[1], BB[2]);
  tube_free(ms, curve((V3[]){ v3(0, 0.7, 0.378), v3(0, 0.5, 0.27), v3(0, 0.34, 0.08), v3(BB[0], BB[1] + 0.02, BB[2]) }, 4, 12), 0.02, 10, ROOT, frame, M_PAINT);
  tube_free(ms, curve((V3[]){ v3(0, 0.61, 0.395), v3(0, 0.42, 0.24), v3(0, 0.33, 0.05), v3(0, 0.3, -0.03) }, 4, 10), 0.014, 8, ROOT, frame, M_PAINT);
  V3 ST = v3(0, 0.8, -0.26);
  tube_free(ms, seg(bbv, ST, 2), 0.017, 10, ROOT, frame, M_PAINT);
  tube_free(ms, seg(HTb, HTt, 2), 0.022, 10, ROOT, frame, M_PAINT);
  const double *RA = J[WHEEL_R].v;
  for (int xi = 0; xi < 2; xi++) {
    double x = xi ? 1 : -1;
    tube_free(ms, seg(v3(x * 0.02, BB[1], BB[2]), v3(x * 0.06, RA[1], RA[2]), 3), 0.01, 6, ROOT, frame, M_PAINT);
    tube_free(ms, seg(v3(x * 0.015, 0.76, -0.25), v3(x * 0.06, RA[1] + 0.01, RA[2]), 3), 0.009, 6, ROOT, frame, M_PAINT);
    // rear rack side rails and struts
    tube_free(ms, seg(v3(x * 0.075, 0.74, -0.3), v3(x * 0.075, 0.74, -0.8), 2), 0.006, 5, ROOT, dark, M_PAINT);
    tube_free(ms, seg(v3(x * 0.075, 0.74, -0.74), v3(x * 0.065, RA[1], RA[2] - 0.01), 2), 0.005, 5, ROOT, dark, M_PAINT);
  }
  static const double RACK_Z[3] = { -0.42, -0.58, -0.74 };
  for (int i = 0; i < 3; i++) tube_free(ms, seg(v3(-0.075, 0.742, RACK_Z[i]), v3(0.075, 0.742, RACK_Z[i]), 1), 0.005, 5, ROOT, dark, M_PAINT);
  // seat post + sprung saddle
  tube_free(ms, seg(ST, v3(saddle[0], saddle[1] - 0.06, saddle[2]), 2), 0.012, 8, ROOT, chrome, M_CHROME);
  static const double SPRING_X[2] = { -0.045, 0.045 };
  for (int i = 0; i < 2; i++) {
    double x = SPRING_X[i];
    tube_free(ms, curve((V3[]){ v3(x, saddle[1] - 0.06, saddle[2] - 0.02), v3(x * 1.3, saddle[1] - 0.045, saddle[2] - 0.07),
                                v3(x * 1.3, saddle[1] - 0.02, saddle[2] - 0.08) }, 3, 6), 0.007, 6, ROOT, dark, M_CHROME);
  }
  {
    static const double SK[5][3] = { { 0, 0.035, 0.03 }, { 0.12, 0.105, 0.05 }, { 0.4, 0.1, 0.048 }, { 0.7, 0.045, 0.04 }, { 1, 0.024, 0.03 } };
    Loft L = loft_new(12, 14);
    LOFT_EACH(L, i, j, u, v) {
      double wh[2];
      PROFILE(SK, u, wh);
      double th = v * TAU;
      double x = wh[0] * se(cos(th), 2.6), y = saddle[1] - 0.025 + wh[1] * 0.5 * se(sin(th), 2.6) + 0.012 * sin(u * PI_D);
      PV *pv = &L.v[i * L.nv + j];
      *pv = (PV){ { saddle[0] + x, y, saddle[2] - 0.11 + u * 0.27 }, brown, M_LEATHER, {}, {} };
      pv_w1(pv, ROOT);
    }
    loft_end(ms, &L, true, true);
  }
  // fork, stem, swept-back bars, grips, basket (all steer)
  const double *FA = J[WHEEL_F].v;
  for (int xi = 0; xi < 2; xi++) {
    double x = xi ? 1 : -1;
    tube_free(ms, curve((V3[]){ v3(x * 0.028, HTb.y, HTb.z), v3(x * 0.045, 0.47, 0.49), v3(x * 0.052, FA[1], FA[2]) }, 3, 6), 0.011, 6, STEER, frame, M_PAINT);
  }
  tube_free(ms, seg(HTt, v3(0, 0.98, 0.31), 2), 0.013, 8, STEER, chrome, M_CHROME);
  for (int xi = 0; xi < 2; xi++) {
    double x = xi ? 1 : -1;
    tube_free(ms, curve((V3[]){ v3(0, 0.98, 0.31), v3(x * 0.14, 0.99, 0.3), v3(x * 0.24, 1.0, 0.2), v3(x * 0.28, 1.01, 0.1) }, 4, 10), 0.011, 7, STEER, chrome, M_CHROME);
    tube_free(ms, seg(v3(x * 0.28, 1.01, 0.11), v3(x * 0.295, 1.012, 0.0), 2), 0.017, 8, STEER, brown, M_LEATHER);
  }
  box(ms, (double[]){ 0, 0.92, 0.53 }, (double[]){ 0.17, 0.12, 0.13 }, STEER, wick, M_WICKER, 0);
  // chainring + guard, cranks, pedals
  torus_x(ms, (double[]){ -0.058, BB[1], BB[2] }, 0.09, 0.008, 0.004, 28, 5, CRANK, dark, M_CHROME, 0, TAU);
  box(ms, (double[]){ -0.068, BB[1] + 0.02, (BB[2] + RA[2]) / 2 }, (double[]){ 0.004, 0.06, 0.3 }, ROOT, frame, M_PAINT, 0);
  tube_free(ms, seg(v3(-0.07, BB[1], BB[2]), v3(0.07, BB[1], BB[2]), 1), 0.02, 8, ROOT, chrome, M_CHROME);
  const double *PR = J[PEDAL_R].v, *PL = J[PEDAL_L].v;
  tube_free(ms, seg(v3(-0.075, BB[1], BB[2]), v3(-0.08, PR[1], PR[2]), 1), 0.011, 6, CRANK, chrome, M_CHROME);
  tube_free(ms, seg(v3(0.075, BB[1], BB[2]), v3(0.08, PL[1], PL[2]), 1), 0.011, 6, CRANK, chrome, M_CHROME);
  static const int PEDALS[2][2] = { { PEDAL_L, 1 }, { PEDAL_R, -1 } };
  for (int i = 0; i < 2; i++) {
    const double *P = J[PEDALS[i][0]].v;
    box(ms, (double[]){ P[0] + PEDALS[i][1] * 0.03, P[1], P[2] }, (double[]){ 0.045, 0.012, 0.04 }, PEDALS[i][0], dark, M_RUBBER, 0);
  }
}

// ---------------------------------------------------------------------------------------------
// Materials

static Material *g_body_mat;
static Material *body_material(void) {
  if (g_body_mat) return g_body_mat;
  MatDesc d = md_physical();
  d.name = "people-body";
  d.color = color_hex(0xffffff);
  d.vertex_colors = true;
  d.roughness = 0.8;
  d.metalness = 0;
  d.sheen = 1;
  d.sheen_roughness = 0.75;
  d.sheen_color = color_rgb(0.55, 0.55, 0.55);
  d.prog[MV_SKINNED] = PROG_PEOPLE_BODY;
  g_body_mat = mat_three(&d);
  mat_set_mat4(g_body_mat, "bindMatrix", m4_identity());
  mat_set_mat4(g_body_mat, "bindMatrixInverse", m4_identity());
  return g_body_mat;
}

// projected sun shadow for one figure (same skeleton, same geometry)
static Material *shadow_material(void) {
  MatDesc d = md_standard();
  d.name = "people-shadow";
  d.color = color_hex(0xffffff);
  d.roughness = 1;
  d.metalness = 0;
  d.fog = false;
  d.transparent = true;
  d.depth_write = true;
  d.blending = BLEND_CUSTOM;
  d.blend_op = SDL_GPU_BLENDOP_ADD;
  d.blend_src = SDL_GPU_BLENDFACTOR_ZERO;
  d.blend_dst = SDL_GPU_BLENDFACTOR_SRC_COLOR;
  d.blend_src_alpha = SDL_GPU_BLENDFACTOR_ZERO;
  d.blend_dst_alpha = SDL_GPU_BLENDFACTOR_ONE;
  d.prog[MV_SKINNED] = PROG_PEOPLE_SHADOW;
  Material *m = mat_three(&d);
  mat_set_mat4(m, "bindMatrix", m4_identity());
  mat_set_mat4(m, "bindMatrixInverse", m4_identity());
  mat_set_vec3(m, "uSun", g_sun_dir);
  mat_set_float(m, "uBaseY", 0);
  mat_set_float(m, "uWallX", HOTEL.frontX);
  mat_set_float(m, "uTerraceY", TERRACE_Y);
  mat_set_float(m, "uStrength", 1.0);
  mat_set_mat4(m, "uInvProj", m4_identity());
  mat_set_mat4(m, "uProj", m4_identity());
  mat_set_vec4(m, "uViewport", v4(0, 0, 1, 1));
  return m;
}

// onBeforeRender: the camera's projection and the current viewport (SH_SHARED in the JS)
static void shadow_before_render(Node *n, void *user) {
  (void)user;
  const Camera *cam = g_draw_camera;
  mat_set_mat4(n->material, "uInvProj", m4_invert(cam->projection));
  mat_set_mat4(n->material, "uProj", cam->projection);
  mat_set_vec4(n->material, "uViewport", v4(g_draw_viewport[0], g_draw_viewport[1], g_draw_viewport[2], g_draw_viewport[3]));
}

// ---------------------------------------------------------------------------------------------
// Figure: skeleton + skinned body + projected shadow

typedef struct Figure {
  Outfit o;
  double s;
  J3 J[NBONES];
  Node *bones[NBONES];
  V3 rest[NBONES];
  Node *root, *mesh, *shadow;
  Node *markers[6];
  double saddle[3];
  int tris;
} Figure;

// Object3D.updateWorldMatrix(true, false): the parents' chain, then this node
static void update_world_up(Node *n) {
  if (n->parent) update_world_up(n->parent);
  node_update_matrix(n);
  n->matrix_world = n->parent ? m4_mul(n->parent->matrix_world, n->matrix) : n->matrix;
}
static V3 world_pos(Node *n) {   // getWorldPosition
  update_world_up(n);
  return m4_get_position(n->matrix_world);
}
static Quat world_quat(Node *n) {   // getWorldQuaternion (decomposed from the world matrix)
  update_world_up(n);
  V3 p, s;
  Quat q;
  m4_decompose(n->matrix_world, &p, &q, &s);
  return q;
}
// rotation.set(x, y, z[, order]) / rotation.x = ... (the quaternion follows)
static void set_rot(Node *n, double x, double y, double z) { node_set_euler(n, euler(x, y, z, n->rotation.order)); }
static void set_rot_o(Node *n, double x, double y, double z, EulerOrder o) { node_set_euler(n, euler(x, y, z, o)); }

static Figure *make_figure(Node *scene, Outfit o) {
  Figure *F = xcalloc(1, sizeof *F);
  double s = o.height / 1.75;
  F->s = s;
  joints(s, o.fem, F->J);
  const J3 *J = F->J;
  Mesher ms = {};
  if (o.has_bike) {
    // saddle from the rider's leg: hip over the pedal at the bottom with a slight knee bend
    double leg = J[THIGH_L].v[1] - J[FOOT_L].v[1];
    double hipY = 0.29 - 0.17 + 0.1 + leg * 0.955, hipZ = -0.06 - 0.21;
    F->saddle[0] = 0; F->saddle[1] = hipY - 0.085; F->saddle[2] = hipZ;
    o.bike_hip[0] = hipY; o.bike_hip[1] = hipZ;
  }
  F->o = o;
  build_body(&ms, &o, J);
  if (o.has_sandals) {
    const double *H = J[HAND_R].v;
    static const double DX[2] = { -0.011, 0.011 };
    for (int i = 0; i < 2; i++) {
      double dx = DX[i];
      box(&ms, (double[]){ H[0] + dx, H[1] - 0.29 * s, H[2] + 0.02 * s + dx }, (double[]){ 0.006, 0.12 * s, 0.045 * s }, HAND_R, o.sandals, M_LEATHER, dx * 8);
    }
  }
  if (o.has_cloth) {
    const double *H = J[HAND_R].v;
    box(&ms, (double[]){ H[0], H[1] - 0.12 * s, H[2] + 0.02 * s }, (double[]){ 0.03, 0.05, 0.05 }, HAND_R, o.cloth, M_COTTON, 0.4);
  }
  if (o.has_bike) build_bike(&ms, &o, J, F->saddle);
  Geometry *geo = ms_geometry(&ms);
  F->tris = geo->index_count / 3;
  vec_free(&ms.P); vec_free(&ms.N); vec_free(&ms.C); vec_free(&ms.M); vec_free(&ms.SI); vec_free(&ms.SW); vec_free(&ms.I);

  for (int i = 0; i < NBONES; i++) F->bones[i] = node_new(NODE_GROUP, "bone");
  for (int i = 0; i < NBONES; i++) {
    int p = PARENT[i];
    if (p < 0) F->bones[i]->position = v3(J[i].v[0], J[i].v[1], J[i].v[2]);
    else {
      F->bones[i]->position = v3(J[i].v[0] - J[p].v[0], J[i].v[1] - J[p].v[1], J[i].v[2] - J[p].v[2]);
      node_add(F->bones[p], F->bones[i]);
    }
  }
  F->root = F->bones[0];
  node_update_matrix_world(F->root, true);
  Skeleton *sk = skeleton_new(F->bones, NBONES);
  for (int i = 0; i < NBONES; i++) F->rest[i] = F->bones[i]->position;

  GpuGeometry *gg = gpu_geometry(geo);
  geo_free(geo);
  F->mesh = node_skinned(gg, body_material(), sk);
  snprintf(F->mesh->name, sizeof F->mesh->name, "person");
  F->mesh->frustum_culled = false;
  F->mesh->cast_shadow = false;
  F->mesh->receive_shadow = true;
  F->shadow = node_skinned(gg, shadow_material(), sk);
  snprintf(F->shadow->name, sizeof F->shadow->name, "person-shadow");
  F->shadow->frustum_culled = false;
  F->shadow->cast_shadow = false;
  F->shadow->receive_shadow = true;
  F->shadow->render_order = 2;
  F->shadow->on_before_render = shadow_before_render;
  snprintf(F->root->name, sizeof F->root->name, "person-root");
  node_add(scene, F->root);
  node_add(scene, F->mesh);
  node_add(scene, F->shadow);

  // ground-contact markers: heel and ball under each foot, toe tip on the toe bone
  static const int FT[2][2] = { { FOOT_L, TOE_L }, { FOOT_R, TOE_R } };
  int k = 0;
  for (int f = 0; f < 2; f++) {
    double sole = -0.08 * s - (o.has_shoes ? 0.006 * s : 0);
    static const double ZS[2] = { -0.05, 0.1 };
    for (int zi = 0; zi < 2; zi++) {
      Node *m = node_new(NODE_GROUP, "marker");
      m->position = v3(0, sole, ZS[zi] * s);
      node_add(F->bones[FT[f][0]], m);
      F->markers[k++] = m;
    }
    Node *m = node_new(NODE_GROUP, "marker");
    m->position = v3(0, sole + 0.055 * s, 0.075 * s);
    node_add(F->bones[FT[f][1]], m);
    F->markers[k++] = m;
  }
  return F;
}

// ---------------------------------------------------------------------------------------------
// Gaits (degrees; phase 0 = left heel strike; foot = world pitch, toes down +)

typedef struct Keys { const double (*k)[2]; int n; } Keys;
typedef struct Gait {
  Keys hip, knee, foot, toe, lift;   // lift.n == 0: none
  double arm, arm_base, elbow, elbow_amp, yaw, chest_yaw, list, sway, lean, bob;
  bool run;
} Gait;
static const double WALK_HIP[][2] = { { 0, 25 }, { 0.15, 19 }, { 0.5, -12 }, { 0.6, -9 }, { 0.75, 12 }, { 0.88, 26 } };
static const double WALK_KNEE[][2] = { { 0, 3 }, { 0.13, 16 }, { 0.36, 5 }, { 0.5, 9 }, { 0.62, 40 }, { 0.73, 62 }, { 0.87, 24 }, { 0.95, 4 } };
static const double WALK_FOOT[][2] = { { 0, -21 }, { 0.1, 0 }, { 0.4, 2 }, { 0.52, 16 }, { 0.62, 40 }, { 0.72, 12 }, { 0.82, -3 }, { 0.93, -16 } };
static const double WALK_TOE[][2] = { { 0, 0 }, { 0.45, 0 }, { 0.55, -18 }, { 0.62, -36 }, { 0.7, -6 }, { 0.8, 0 } };
static const double RUN_HIP[][2] = { { 0, 26 }, { 0.12, 12 }, { 0.33, -14 }, { 0.45, -6 }, { 0.6, 18 }, { 0.8, 38 }, { 0.92, 32 } };
static const double RUN_KNEE[][2] = { { 0, 20 }, { 0.12, 40 }, { 0.33, 20 }, { 0.48, 62 }, { 0.62, 102 }, { 0.78, 72 }, { 0.92, 28 } };
static const double RUN_FOOT[][2] = { { 0, -6 }, { 0.08, 0 }, { 0.22, 4 }, { 0.34, 32 }, { 0.45, 30 }, { 0.6, 8 }, { 0.8, -4 }, { 0.93, -10 } };
static const double RUN_TOE[][2] = { { 0, 0 }, { 0.22, 0 }, { 0.3, -20 }, { 0.36, -30 }, { 0.44, -5 }, { 0.55, 0 } };
static const double RUN_LIFT[][2] = { { 0, 0 }, { 0.33, 0 }, { 0.42, 0.05 }, { 0.5, 0.01 } };
#define KEYS(a) ((Keys){ (a), (int)ARRAY_LEN(a) })
static const Gait WALK = { KEYS(WALK_HIP), KEYS(WALK_KNEE), KEYS(WALK_FOOT), KEYS(WALK_TOE), { nullptr, 0 },
                           15, 3, 14, 14, 4, 8, 3, 0.02, 2, 1, false };
static const Gait RUN = { KEYS(RUN_HIP), KEYS(RUN_KNEE), KEYS(RUN_FOOT), KEYS(RUN_TOE), KEYS(RUN_LIFT),
                          30, 8, 86, 10, 7, 14, 4, 0.008, 7, 1, true };

static void hip_mid(const Gait *G, double *mid, double *range) {
  double mx = -INFINITY, mn = INFINITY;
  for (int i = 0; i < G->hip.n; i++) { mx = fmax(mx, G->hip.k[i][1]); mn = fmin(mn, G->hip.k[i][1]); }
  *mid = (mx + mn) / 2;
  *range = (mx - mn) / 2;
}

typedef struct Pose {
  double pelvisX, pelvisY, pelvisZ, spineX, spineY, spineZ, chestX, chestY, chestZ, neckX, neckY, headX, headY, headZ;
  double hip[2], abd[2], knee[2], foot[2], toe[2], sh[2], armAb[2], el[2], wr[2];   // [L, R]
  double sway, lift, pony, ponyZ;
} Pose;
enum { POSE_N = (int)(sizeof(Pose) / sizeof(double)) };
static void mix_pose(const Pose *a, const Pose *b, double t, Pose *out) {
  const double *pa = (const double *)a, *pb = (const double *)b;
  double *po = (double *)out;
  for (int k = 0; k < POSE_N; k++) po[k] = pa[k] + (pb[k] - pa[k]) * t;
}

// per-person style (the JS `st` with its ?? defaults filled in)
typedef struct Style { double amp, arm, armR, elbow, armAb, lean, chest, look, seed; } Style;
static Style style(void) { return (Style){ .amp = 1, .arm = 1, .armR = 1, .armAb = 5 }; }

// (like the JS, spineY is never written here: it keeps the value `out` had)
static void gait_pose(const Gait *G, double p, const Style *st, Pose *out) {
  double A = st->amp, hm, hr;
  hip_mid(G, &hm, &hr);
  for (int sd = 0; sd < 2; sd++) {
    double ph = sd ? p + 0.5 : p;
    out->hip[sd] = cyc(G->hip.k, G->hip.n, ph) * A * D2R;
    out->knee[sd] = cyc(G->knee.k, G->knee.n, ph) * (0.85 + 0.15 * A) * D2R;
    out->foot[sd] = cyc(G->foot.k, G->foot.n, ph) * A * D2R;
    out->toe[sd] = cyc(G->toe.k, G->toe.n, ph) * D2R;
    // arm follows the opposite leg, lagging a little
    double n = (cyc(G->hip.k, G->hip.n, ph + 0.5 - 0.04) - hm) / hr;
    double amp = G->arm * st->arm * (sd ? st->armR : 1);
    out->sh[sd] = (G->arm_base + amp * n) * D2R;
    out->el[sd] = (G->elbow + G->elbow_amp * fmax(0, n) + st->elbow) * D2R;
    out->armAb[sd] = st->armAb * D2R;
    out->wr[sd] = (G->run ? 12 : 4) * D2R;
    out->abd[sd] = 0;
  }
  double c = cos(TAU * (p - 0.03)), sn = sin(TAU * p);
  out->pelvisY = -G->yaw * c * D2R;
  out->chestY = G->chest_yaw * c * D2R;
  out->pelvisZ = G->list * sn * D2R;
  out->spineZ = -0.55 * out->pelvisZ;
  out->chestZ = -0.35 * out->pelvisZ;
  out->pelvisX = (G->lean * 0.6 + st->lean) * D2R + 0.012 * cos(TAU * 2 * p);
  out->spineX = G->lean * 0.25 * D2R;
  out->chestX = G->lean * 0.15 * D2R + st->chest * D2R;
  double lean = out->pelvisX + out->spineX + out->chestX;
  out->neckX = -lean * 0.4; out->headX = -lean * 0.55 + st->look * D2R;
  out->neckY = -(out->pelvisY + out->chestY) * 0.5; out->headY = -(out->pelvisY + out->chestY) * 0.45;
  out->headZ = -(out->pelvisZ + out->spineZ + out->chestZ) * 0.8;
  out->sway = G->sway * sn;
  out->lift = G->lift.n ? cyc(G->lift.k, G->lift.n, fmod(p * 2, 1)) : 0;
  out->pony = 0.35 + 0.25 * sin(TAU * 2 * p - 1.2);
  out->ponyZ = 0.18 * sin(TAU * p);
}

static void stand_pose(double t, const Style *st, Pose *out) {
  *out = (Pose){};
  double br = sin(t * 1.4 + st->seed);
  out->knee[0] = 4 * D2R; out->knee[1] = 7 * D2R; out->hip[0] = 3 * D2R; out->hip[1] = 6 * D2R;
  out->foot[0] = 0; out->foot[1] = 0;
  out->sh[0] = 3 * D2R; out->sh[1] = 2 * D2R; out->el[0] = 12 * D2R; out->el[1] = 14 * D2R;
  out->armAb[0] = out->armAb[1] = 5 * D2R; out->wr[0] = out->wr[1] = 5 * D2R;
  out->pelvisZ = 2.5 * D2R; out->spineZ = -1.5 * D2R; out->chestX = 0.008 * br; out->headX = 2 * D2R;
  out->pony = 0.3;
}

static void apply_pose(Figure *F, const Pose *P) {
  Node **b = F->bones;
  for (int i = 1; i < NBONES; i++) { node_set_quaternion(b[i], quat_identity()); b[i]->position = F->rest[i]; }
  set_rot_o(b[PELVIS], P->pelvisX, P->pelvisY, P->pelvisZ, EULER_YXZ);
  set_rot_o(b[SPINE], P->spineX, P->spineY, P->spineZ, EULER_YXZ);
  set_rot_o(b[CHEST], P->chestX, P->chestY, P->chestZ, EULER_YXZ);
  set_rot_o(b[NECK], P->neckX, P->neckY, 0, EULER_YXZ);
  set_rot_o(b[HEAD], P->headX, P->headY, P->headZ, EULER_YXZ);
  double lean = P->pelvisX + P->spineX + P->chestX;
  static const int SIDES[2][7] = { { THIGH_L, SHIN_L, FOOT_L, TOE_L, ARM_L, FORE_L, HAND_L }, { THIGH_R, SHIN_R, FOOT_R, TOE_R, ARM_R, FORE_R, HAND_R } };
  for (int sd = 0; sd < 2; sd++) {
    double sg = sd ? -1 : 1;
    const int *S = SIDES[sd];
    set_rot(b[S[0]], -P->hip[sd], 0, sg * P->abd[sd] - P->pelvisZ * 0.9);
    set_rot(b[S[1]], P->knee[sd], 0, 0);
    set_rot(b[S[2]], P->foot[sd] - (P->pelvisX - P->hip[sd] + P->knee[sd]), 0, 0);
    set_rot(b[S[3]], P->toe[sd], 0, 0);
    set_rot(b[S[4]], -P->sh[sd] - lean, 0, sg * P->armAb[sd] - P->chestZ - P->spineZ - P->pelvisZ);
    set_rot(b[S[5]], -P->el[sd], 0, 0);
    set_rot(b[S[6]], -P->wr[sd], 0, 0);
  }
  set_rot(b[PONY], P->pony - P->headX - P->neckX - lean, 0, P->ponyZ);
}

// place the root and drop the figure so its lowest sole point touches the ground
static void settle(Figure *F, double x, double z, double heading, double ground, const Pose *P) {
  Node *r = F->root;
  double cs = cos(heading), sn = sin(heading);
  r->position = v3(x + cs * P->sway, 0, z - sn * P->sway);
  set_rot(r, 0, heading, 0);
  node_update_matrix_world(r, true);
  double mn = INFINITY;
  for (int i = 0; i < 6; i++) mn = fmin(mn, world_pos(F->markers[i]).y);
  r->position.y = ground - mn + P->lift;
  node_update_matrix_world(r, true);
}

// stride length (m per cycle) that keeps the planted foot from sliding
static double calibrate(Figure *F, const Gait *G, const Style *st) {
  Pose P = {};
  const int N = 160;
  double dist = 0;
  int contact = 0;
  bool have_prev = false;
  int prev_arg = -1;
  double prev_z = 0;
  for (int i = 0; i <= N; i++) {
    double p = (double)i / N;
    gait_pose(G, p, st, &P);
    apply_pose(F, &P);
    F->root->position = v3(0, 0, 0);
    set_rot(F->root, 0, 0, 0);
    node_update_matrix_world(F->root, true);
    double mn = INFINITY, z = 0;
    int arg = -1;
    for (int k = 0; k < 6; k++) {
      V3 w = world_pos(F->markers[k]);
      if (w.y < mn) { mn = w.y; arg = k; z = w.z; }
    }
    bool air = G->lift.n && cyc(G->lift.k, G->lift.n, fmod(p * 2, 1)) > 0.004;
    if (have_prev && !air && prev_arg == arg && i > 0) { dist += prev_z - z; contact++; }
    have_prev = true;
    prev_arg = arg;
    prev_z = z;
  }
  return contact ? (dist / contact) * N : 1.3;
}

// two-bone IK towards a world target, bending towards `pole` (world direction)
static void set_bone_dir(Node *bone, V3 child_rest, V3 dir) {
  Quat q = world_quat(bone->parent);
  V3 v = v3_apply_quat(v3_norm(child_rest), q);
  Quat q2 = quat_unit_vectors(v, v3_norm(dir));
  node_set_quaternion(bone, quat_mul(quat_mul(quat_conj(q), q2), q));
  node_update_matrix_world(bone, true);
}
static void ik2(Node *upper, Node *lower, Node *end, V3 target, V3 pole) {
  V3 S = world_pos(upper);
  double a = v3_len(lower->position), b = v3_len(end->position);
  V3 D = v3_sub(target, S);
  double d = clampd_(v3_len(D), 1e-4, (a + b) * 0.9995);
  D = v3_norm(D);
  double x = (a * a - b * b + d * d) / (2 * d), h = sqrt(fmax(0, a * a - x * x));
  V3 pp = v3_norm(v3_add_scaled(pole, D, -v3_dot(pole, D)));
  V3 E = v3_add_scaled(v3_add_scaled(S, D, x), pp, h);
  set_bone_dir(upper, lower->position, v3_sub(E, S));
  V3 T = v3_add_scaled(S, D, d);
  set_bone_dir(lower, end->position, v3_sub(T, E));
}
static void set_world_quat(Node *bone, Quat q) {
  Quat pq = world_quat(bone->parent);
  node_set_quaternion(bone, quat_mul(quat_conj(pq), q));
  node_update_matrix_world(bone, true);
}

// IK blended over the FK pose by w; pole in the figure's own frame
static void blend_ik(Node *up, Node *lo, Node *end, V3 target, V3 pole, double w) {
  Quat qa = up->quaternion, qb = lo->quaternion;
  Node *root = up->parent->parent->parent->parent;
  V3 pw = v3_apply_quat(pole, world_quat(root));
  ik2(up, lo, end, target, pw);
  Quat ia = up->quaternion, ib = lo->quaternion;
  node_set_quaternion(up, quat_slerp(qa, ia, w));
  node_set_quaternion(lo, quat_slerp(qb, ib, w));
  node_update_matrix_world(up, true);
}
static void hand_flat(Node *h, double w) {
  set_rot(h, h->rotation.x + -0.5 * w, h->rotation.y, h->rotation.z);
  node_update_matrix_world(h, true);
}

// ---------------------------------------------------------------------------------------------
// Paths

typedef struct WPathPt { double x, z, dx, dz; } WPathPt;
typedef struct WPath {
  double total;
  Vec(double) L;
  Vec(V2) pts;
  bool fixed;          // the ?shot parking spots: one point, forever
  WPathPt fixed_at;
} WPath;

static void poly_path(WPath *P) {
  vec_push(&P->L, 0.0);
  for (size_t i = 1; i < P->pts.len; i++)
    vec_push(&P->L, P->L.data[i - 1] + js_hypot2(P->pts.data[i].x - P->pts.data[i - 1].x, P->pts.data[i].y - P->pts.data[i - 1].y));
  P->total = vec_last(&P->L);
}
static WPathPt path_at(const WPath *P, double s) {
  if (P->fixed) return P->fixed_at;
  s = fmod(fmod(s, P->total) + P->total, P->total);
  size_t lo = 0, hi = P->L.len - 1;
  while (hi - lo > 1) {
    size_t m = (lo + hi) >> 1;
    if (P->L.data[m] <= s) lo = m; else hi = m;
  }
  double den = P->L.data[hi] - P->L.data[lo];
  double t = (s - P->L.data[lo]) / (den != 0 ? den : 1);
  V2 a = P->pts.data[lo], b = P->pts.data[hi];
  return (WPathPt){ lerp(a.x, b.x, t), lerp(a.y, b.y, t), b.x - a.x, b.y - a.y };
}
// out-and-back loop: two lines joined by half-circle turns at the ends
typedef double (*XofZ)(double z);
static void path_turn(WPath *P, double za, double xa, double xb, double dir) {
  double cx = (xa + xb) / 2, r = fabs(xb - xa) / 2 + 1e-3;
  for (int i = 1; i < 10; i++) {
    double a = ((double)i / 10) * PI_D;
    vec_push(&P->pts, v2(cx + (xa < xb ? -1 : 1) * r * cos(a), za + dir * r * sin(a) * 1.4));
  }
}
static WPath *loop_path(XofZ fx0, XofZ fx1, double z0, double z1, double step) {
  WPath *P = xcalloc(1, sizeof *P);
  for (double z = z0; z <= z1; z += step) vec_push(&P->pts, v2(fx0(z), z));
  path_turn(P, z1, fx0(z1), fx1(z1), 1);
  for (double z = z1; z >= z0; z -= step) vec_push(&P->pts, v2(fx1(z), z));
  path_turn(P, z0, fx1(z0), fx0(z0), -1);
  vec_push(&P->pts, P->pts.data[0]);
  poly_path(P);
  return P;
}
static double jog_x0(double z) { return promenade_x(z) - 0.65; }
static double jog_x1(double z) { return promenade_x(z) + 0.65; }
static double walk_x0(double z) { return 90.1 + 1.2 * sin(z / 16) + 0.4 * sin(z / 5.1 + 1); }
static double walk_x1(double z) { return 89.5 + 1.0 * sin(z / 13 + 2); }
static double dist_x0(double z) { return 88.2 + 0.8 * sin(z / 9); }
static double dist_x1(double z) { return 87.6 + 0.7 * sin(z / 11 + 1); }

// ---------------------------------------------------------------------------------------------
// Cast

static Outfit outfit_jogger(void) {
  Outfit o = { .height = 1.68, .fem = true, .girth = 0.95, .skin = color_hex(0x8a5a3c) };
  o.hair.col = color_hex(0x1c1410); o.hair.style = HAIR_PONYTAIL;
  o.has_top = true; o.top.col = color_hex(0x2f8c95); o.top.mat = M_TECH; o.top.kind = TOP_TANK; o.top.hem = 0.99; o.top.sleeve = 99; o.top.off = 0.004;
  o.has_bottom = true; o.bottom.col = color_hex(0x1b1b1f); o.bottom.mat = M_TECH; o.bottom.waist = 1.02; o.bottom.hem = 0.74; o.bottom.off = 0.006;
  o.has_socks = true; o.socks.col = color_hex(0xf2f2ef); o.socks.top = 0.12;
  o.has_shoes = true; o.shoes.col = color_hex(0xe9e9e6); o.shoes.sole = color_hex(0xd0cfcb); o.shoes.accent = color_hex(0xe0604e); o.shoes.has_accent = true;
  return o;
}
static Outfit outfit_walker(void) {
  Outfit o = { .height = 1.8, .fem = false, .girth = 1.02, .skin = color_hex(0xd6a487) };
  o.hair.col = color_hex(0x9a948c); o.hair.style = HAIR_SHORT;
  o.has_top = true; o.top.col = color_hex(0xbfd2de); o.top.mat = M_LINEN; o.top.kind = TOP_SHIRT; o.top.hem = 0.87; o.top.sleeve = 1.02; o.top.roll = true; o.top.off = 0.013;
  o.has_bottom = true; o.bottom.col = color_hex(0xcdbf9f); o.bottom.mat = M_LINEN; o.bottom.waist = 1.03; o.bottom.hem = 0.3; o.bottom.cuff = true; o.bottom.off = 0.011;
  o.has_sandals = true; o.sandals = color_hex(0x5c3d25);
  return o;
}
static Outfit outfit_worker(void) {
  Outfit o = { .height = 1.66, .fem = true, .girth = 1.0, .skin = color_hex(0x5b3a27) };
  o.hair.col = color_hex(0x121010); o.hair.style = HAIR_BUN;
  o.has_top = true; o.top.col = color_hex(0xf3f2ee); o.top.mat = M_COTTON; o.top.kind = TOP_SHIRT; o.top.hem = 1.0; o.top.sleeve = 1.06; o.top.roll = true; o.top.off = 0.008;
  o.has_bottom = true; o.bottom.col = color_hex(0x202023); o.bottom.mat = M_COTTON; o.bottom.waist = 1.02; o.bottom.hem = 0.1; o.bottom.off = 0.008;
  o.has_apron = true; o.apron = color_hex(0x18181a); o.has_cloth = true; o.cloth = color_hex(0xe8e6e0);
  o.has_shoes = true; o.shoes.col = color_hex(0x151515); o.shoes.sole = color_hex(0x151515);
  return o;
}
static Outfit outfit_cyclist(void) {
  Outfit o = { .height = 1.77, .fem = false, .girth = 1.0, .skin = color_hex(0xb57d5c) };
  o.hair.col = color_hex(0x241a14); o.hair.style = HAIR_SHORT; o.has_glasses = true; o.glasses = color_hex(0x0c0c0d);
  o.has_top = true; o.top.col = color_hex(0x6f7d55); o.top.mat = M_COTTON; o.top.kind = TOP_TEE; o.top.hem = 0.98; o.top.sleeve = 1.3; o.top.off = 0.009;
  o.has_bottom = true; o.bottom.col = color_hex(0x2b3552); o.bottom.mat = M_COTTON; o.bottom.waist = 1.03; o.bottom.hem = 0.56; o.bottom.off = 0.01;
  o.has_socks = true; o.socks.col = color_hex(0xdedcd6); o.socks.top = 0.1;
  o.has_shoes = true; o.shoes.col = color_hex(0x8f8f8c); o.shoes.sole = color_hex(0xe6e4df);
  o.has_bike = true; o.bike_frame = color_hex(0x9fd0c0);
  return o;
}
static Outfit outfit_distant(void) {
  Outfit o = { .height = 1.64, .fem = true, .girth = 0.97, .skin = color_hex(0xe0b596) };
  o.hair.col = color_hex(0x5a3b22); o.hair.style = HAIR_PONYTAIL;
  o.has_top = true; o.top.col = color_hex(0xf1efe8); o.top.mat = M_COTTON; o.top.kind = TOP_TEE; o.top.hem = 0.95; o.top.sleeve = 1.33; o.top.off = 0.009;
  o.has_bottom = true; o.bottom.col = color_hex(0x9e3b3b); o.bottom.mat = M_COTTON; o.bottom.waist = 1.02; o.bottom.hem = 0.76; o.bottom.off = 0.01;
  return o;
}

// cafe tables on the chosen terrace, read back from the hotel furniture instances
typedef struct Table { double x, z; } Table;
typedef struct TableFind { double x0, x1, z0, z1, y; Vec(Table) out; } TableFind;
static void find_tables_visit(Node *m, void *u) {
  TableFind *f = u;
  if (m->kind != NODE_INSTANCED) return;
  Box3 bb = m->geo->bbox;
  if (fabs(bb.max.y - 0.76) > 0.02 || fabs(bb.max.x - 0.35) > 0.02) return;
  for (int i = 0; i < m->inst_count; i++) {
    // (getMatrixAt: the Float32 instance matrix)
    double px = (float)m->inst_matrix[i].e[12], py = (float)m->inst_matrix[i].e[13], pz = (float)m->inst_matrix[i].e[14];
    if (px > f->x0 && px < f->x1 && pz > f->z0 && pz < f->z1 && fabs(py - f->y) < 0.05) vec_push(&f->out, ((Table){ px, pz }));
  }
}

// ---------------------------------------------------------------------------------------------

typedef enum PersonKind { K_PATH, K_WORKER, K_CYCLIST } PersonKind;
typedef enum WorkerTask { T_WIPE, T_CHAIR, T_MOVE } WorkerTask;
typedef enum CyState { CY_WAIT, CY_RIDE } CyState;

typedef struct Person {
  PersonKind kind;
  const char *name;
  Figure *F;
  bool visible;
  double x, z, heading, speed, phase;
  // path walkers
  WPath *path;
  double s, turn_speed_o, speed_o, stride_k, stride_w, stride_r;
  bool run, carry;
  Style st;
  WalkCircle *col;
  // worker
  Table *run_tables;
  int nrun, i, dir;
  WorkerTask task;
  double t, dur, wipe_w, chair_w, seed, stand_x;
  // cyclist
  CyState state;
  double timer, v0, wheel, crank, coast, drift;
} Person;

struct People {
  Person p[5];
  int n;
  WalkCircle cols[4];
  int ncols;
  Walker *walker;
  bool shot;
  Rng rnd;
  int (*get_cars)(void *user, CarPass *out, int max);
  void *cars_user;
  Pose P0, P1, P2;
};

static double height_at(double x, double z) { return beach_ground_at(x, z); }

static void path_update(People *PP, Person *p, double dt, const Camera *cam, bool frozen) {
  Figure *F = p->F;
  Walker *walker = PP->walker;
  double target = p->speed_o;
  WPathPt here = path_at(p->path, p->s), ahead = path_at(p->path, p->s + 3.5);
  double bend = fabs(wrap_angle(atan2(ahead.dx, ahead.dz) - atan2(here.dx, here.dz)));
  if (bend > 0.35) target = fmin(target, p->turn_speed_o);
  if (walker && !frozen) {
    double hx = sin(p->heading), hz = cos(p->heading);
    double rx = walker->pos.x - p->x, rz = walker->pos.y - p->z;
    double along = rx * hx + rz * hz, lat = fabs(rx * hz - rz * hx);
    if (along > 0 && along < 2.4 && lat < 0.8) target = 0;
  }
  if (!frozen) {
    p->speed += (target - p->speed) * (1 - exp(-dt * 2.2));
    p->s += p->speed * dt;
  }
  WPathPt q = path_at(p->path, p->s);
  p->x = q.x; p->z = q.z;
  double th = atan2(q.dx, q.dz);
  p->heading = frozen ? th : p->heading + wrap_angle(th - p->heading) * (1 - exp(-dt * 5));
  double runW = p->run ? smooth(1.5, 2.3, p->speed) : 0;
  double stride = lerp(p->stride_w * p->stride_k, p->stride_r, runW);
  if (!frozen) p->phase += (p->speed / stride) * dt;
  p->col->x = p->x; p->col->z = p->z;
  // never grow over the player (the walker would be stuck inside the circle)
  p->col->r = walker ? fmin(0.3, fmax(0, js_hypot2(walker->pos.x - p->x, walker->pos.y - p->z) - 0.305)) : 0.3;
  double d = cam ? js_hypot2(cam->node->position.x - p->x, cam->node->position.z - p->z) : 0;
  p->visible = d < CULL;
  F->mesh->visible = F->shadow->visible = p->visible;
  if (!p->visible) return;
  Pose *P0 = &PP->P0;
  gait_pose(&WALK, p->phase, &p->st, P0);
  if (runW > 0) { gait_pose(&RUN, p->phase, &p->st, &PP->P1); mix_pose(P0, &PP->P1, runW, P0); }
  double standW = 1 - smooth(0.05, 0.45, p->speed);
  if (standW > 0) { stand_pose(SDL_GetTicks() / 1000.0, &p->st, &PP->P2); mix_pose(P0, &PP->P2, standW, P0); }
  apply_pose(F, P0);
  if (p->carry) {   // the beach walker's sandals: right arm a little bent, hanging
    P0->sh[1] = 6 * D2R;
    P0->el[1] = 22 * D2R;
    Node *fo = F->bones[FORE_R], *ar = F->bones[ARM_R];
    set_rot(fo, -P0->el[1], fo->rotation.y, fo->rotation.z);
    set_rot(ar, -P0->sh[1] - (P0->pelvisX + P0->spineX + P0->chestX), ar->rotation.y, ar->rotation.z);
  }
  double g = height_at(p->x, p->z);
  settle(F, p->x, p->z, p->heading, g, P0);
  mat_set_float(F->shadow->material, "uBaseY", g);
}

static void worker_update(People *PP, Person *W, double dt, const Camera *cam, bool frozen) {
  Figure *F = W->F;
  const Table *tbl = &W->run_tables[W->i];
  if (!frozen) W->t += dt;
  double tx = W->stand_x, tz = tbl->z, face = PI_D / 2;
  if (W->task == T_CHAIR) tz = tbl->z + W->dir * 0.62;
  if (!frozen && W->t > W->dur) {
    W->t = 0;
    if (W->task == T_WIPE) { W->task = T_CHAIR; W->dur = 3.2; }
    else if (W->task == T_CHAIR) {
      if (W->i + W->dir < 0 || W->i + W->dir >= W->nrun) W->dir *= -1;
      W->i += W->dir; W->task = T_MOVE; W->dur = 20;
    } else { W->task = T_WIPE; W->dur = 6 + ((W->i * 7919) % 5); }
  }
  // steer to the spot, stepping round on the way
  double dx = tx - W->x, dz = tz - W->z, dist = js_hypot2(dx, dz);
  double target = 0, want = face;
  if (dist > 0.06) {
    want = atan2(dx, dz);
    target = fabs(wrap_angle(want - W->heading)) < 0.5 ? fmin(0.75, dist * 1.6) : 0;
  } else if (W->task == T_MOVE) { W->task = T_WIPE; W->t = 0; W->dur = 6 + ((W->i * 7919) % 5); }
  if (!frozen) {
    W->speed += (target - W->speed) * (1 - exp(-dt * 4));
    double turn = wrap_angle(want - W->heading) * (1 - exp(-dt * 3));
    W->heading += turn;
    if (dist > 1e-3) { double k = fmin(dist, W->speed * dt) / dist; W->x += dx * k; W->z += dz * k; }
    W->phase += ((W->speed + fabs(turn / dt) * 0.12) / W->stride_w) * dt;
    bool settled = dist < 0.08 && fabs(wrap_angle(face - W->heading)) < 0.2;
    W->wipe_w += ((W->task == T_WIPE && settled ? 1 : 0) - W->wipe_w) * (1 - exp(-dt * 3));
    W->chair_w += ((W->task == T_CHAIR && settled ? 1 : 0) - W->chair_w) * (1 - exp(-dt * 3));
  }
  double d = cam ? js_hypot2(cam->node->position.x - W->x, cam->node->position.z - W->z) : 0;
  W->visible = d < CULL;
  F->mesh->visible = F->shadow->visible = W->visible;
  if (!W->visible) return;
  double moveW = smooth(0.02, 0.25, W->speed + fabs(wrap_angle(want - W->heading)) * 0.3);
  Pose *P0 = &PP->P0;
  stand_pose(W->t + 3, &W->st, P0);
  if (moveW > 0) { gait_pose(&WALK, W->phase, &W->st, &PP->P1); mix_pose(P0, &PP->P1, moveW, P0); }
  // bend over the table: hips back, torso forward, legs stay upright
  double bw = W->wipe_w * 0.95 + W->chair_w * 0.55;
  P0->pelvisX += 0.3 * bw; P0->spineX += 0.2 * bw; P0->chestX += 0.12 * bw;
  P0->hip[0] += 0.3 * bw; P0->hip[1] += 0.34 * bw; P0->knee[0] += 0.08 * bw; P0->knee[1] += 0.1 * bw;
  P0->neckX += 0.1 * bw; P0->headX += 0.2 * bw;
  apply_pose(F, P0);
  double back = 0.07 * bw;
  double g = TERRACE_Y;
  settle(F, W->x - sin(W->heading) * back, W->z - cos(W->heading) * back, W->heading, g, P0);
  mat_set_float(F->shadow->material, "uBaseY", g);
  Node **b = F->bones;
  if (W->wipe_w > 0.01) {
    // right hand circles over the table top, left hand rests on its edge
    double k = W->t * 5.2 + W->seed;
    double top = g + 0.765 + 0.03;
    double hx = tbl->x - 0.14 + 0.11 * cos(k), hz = tbl->z - 0.06 + 0.14 * sin(k);
    blend_ik(b[ARM_R], b[FORE_R], b[HAND_R], v3(hx - 0.05, top + 0.07, hz), v3(0.2, -0.4, -1), W->wipe_w);
    blend_ik(b[ARM_L], b[FORE_L], b[HAND_L], v3(tbl->x - 0.33, top + 0.08, tbl->z + 0.2), v3(0.2, -0.4, 1), W->wipe_w);
    hand_flat(b[HAND_R], W->wipe_w);
    hand_flat(b[HAND_L], W->wipe_w);
  }
  if (W->chair_w > 0.01) {
    // straighten the chair: both hands on its back, a small push-pull
    double k = sin(W->t * 3.1) * 0.03;
    double cz = tbl->z + W->dir * 0.62;
    blend_ik(b[ARM_R], b[FORE_R], b[HAND_R], v3(tbl->x - 0.12 + k, g + 0.98, cz - 0.17), v3(0, -0.4, -1), W->chair_w);
    blend_ik(b[ARM_L], b[FORE_L], b[HAND_L], v3(tbl->x - 0.12 + k, g + 0.98, cz + 0.17), v3(0, -0.4, 1), W->chair_w);
  }
}

static void pose_cyclist(People *PP, Figure *F, Person *c) {
  Node **b = F->bones;
  Pose *P = &PP->P0;
  Style none = style();
  stand_pose(0, &none, P);
  P->pelvisX = 0.3; P->spineX = 0.1; P->chestX = 0.05; P->neckX = -0.12; P->headX = -0.22;
  double sway = sin(c->crank * 2) * 0.012 * (c->speed > 0.8 ? 1 : 0);
  P->pelvisZ = sway; P->chestZ = -sway * 0.5;
  apply_pose(F, P);
  double y = roadHeight(c->x);
  Node *r = F->root;
  r->position = v3(c->x, y, c->z);
  set_rot_o(r, 0, c->heading, -sway * 0.6, EULER_YXZ);
  // saddle: pelvis sits on it (the hip joints a little above and in front of the sit bones)
  b[PELVIS]->position = v3(0, F->o.bike_hip[0] + 0.06 * F->s - 0.02, F->o.bike_hip[1] + 0.02);
  set_rot(b[WHEEL_F], c->wheel, b[WHEEL_F]->rotation.y, b[WHEEL_F]->rotation.z);
  set_rot(b[WHEEL_R], c->wheel, b[WHEEL_R]->rotation.y, b[WHEEL_R]->rotation.z);
  set_rot(b[CRANK], c->crank, b[CRANK]->rotation.y, b[CRANK]->rotation.z);
  set_rot(b[PEDAL_L], -c->crank, b[PEDAL_L]->rotation.y, b[PEDAL_L]->rotation.z);
  set_rot(b[PEDAL_R], -c->crank, b[PEDAL_R]->rotation.y, b[PEDAL_R]->rotation.z);
  set_rot(b[STEER], b[STEER]->rotation.x, 0.02 * sin(c->wheel * 0.21) + sway * 0.8, b[STEER]->rotation.z);
  node_update_matrix_world(r, true);
  Quat rq = world_quat(r);
  V3 fwd = v3_apply_quat(v3(0, 0, 1), rq), up = v3(0, 1, 0);
  static const int LEGS[2][4] = { { PEDAL_L, THIGH_L, SHIN_L, FOOT_L }, { PEDAL_R, THIGH_R, SHIN_R, FOOT_R } };
  for (int sd = 0; sd < 2; sd++) {
    double side = sd ? -1 : 1, phase = sd ? PI_D : 0;
    const int *L = LEGS[sd];
    // ball of the foot on the pedal; ankle a little lower at the back of the stroke
    V3 pedal = world_pos(b[L[0]]);
    double a = c->crank + phase;
    double pitch = (14 + 10 * sin(a + 0.6)) * D2R;
    Quat fq = quat_mul(rq, quat_axis_angle(v3(1, 0, 0), pitch));
    V3 ball = v3_apply_quat(v3(side * 0.012, -0.08 * F->s + 0.012, 0.115 * F->s), fq);
    V3 ankle = v3_sub(v3_add(pedal, v3(0, 0.02, 0)), ball);
    V3 pole = v3_add_scaled(fwd, v3_apply_quat(v3(side, 0, 0), rq), 0.08);
    ik2(b[L[1]], b[L[2]], b[L[3]], ankle, pole);
    set_world_quat(b[L[3]], fq);
  }
  static const int ARMS[2][3] = { { ARM_L, FORE_L, HAND_L }, { ARM_R, FORE_R, HAND_R } };
  for (int sd = 0; sd < 2; sd++) {
    double side = sd ? -1 : 1;
    const int *A = ARMS[sd];
    V3 local = v3_sub(v3(side * 0.288, 1.011, 0.055), v3(F->J[STEER].v[0], F->J[STEER].v[1], F->J[STEER].v[2]));
    V3 grip = v3_apply_m4(local, b[STEER]->matrix_world);   // localToWorld
    V3 wrist = v3_add_scaled(v3_add_scaled(grip, fwd, -0.075), up, 0.035);
    V3 pole = v3_apply_quat(v3(side * 0.8, -0.5, -0.4), rq);
    ik2(b[A[0]], b[A[1]], b[A[2]], wrist, pole);
    Quat hq = quat_mul(rq, quat_from_euler(euler(-1.35, 0, side * 1.3, EULER_ZXY)));
    set_world_quat(b[A[2]], hq);
  }
}

static void cyclist_update(People *PP, Person *c, double dt, const Camera *cam, bool frozen) {
  Figure *F = c->F;
  Walker *walker = PP->walker;
  if (!frozen) {
    CarPass all[16], cars[16];
    int na = PP->get_cars ? PP->get_cars(PP->cars_user, all, 16) : 0, nc = 0;
    for (int i = 0; i < na; i++)
      if (all[i].active && all[i].progress > 0 && all[i].progress < 1) cars[nc++] = all[i];
    if (c->state == CY_WAIT) {
      c->timer -= dt;
      if (c->timer <= 0) {
        if (nc) c->timer = 2;   // a car is passing: wait for a clear road
        else {
          c->state = CY_RIDE;
          c->dir = rng_next(&PP->rnd) < 0.5 ? 1 : -1;
          c->z = -c->dir * 125;
          c->speed = c->v0 = 3.9 + rng_next(&PP->rnd) * 0.7;
        }
      }
    } else {
      // a car coming up behind in the same lane: keep well over to the right
      bool behind = false;
      for (int i = 0; i < nc; i++)
        if (cars[i].dir == c->dir && (c->z - cars[i].z) * c->dir > -4 && (c->z - cars[i].z) * c->dir < 45) behind = true;
      c->drift += ((behind ? 1 : 0) - c->drift) * (1 - exp(-dt * 1.5));
      double target = c->v0;
      if (walker) {
        double rx = walker->pos.x - c->x, rz = (walker->pos.y - c->z) * c->dir;
        if (rz > 0 && rz < 7 && fabs(rx) < 1.1) target = 0;
      }
      c->speed += (target - c->speed) * (1 - exp(-dt * (target < c->speed ? 2.5 : 0.8)));
      c->z += c->dir * c->speed * dt;
      c->coast = fmod(c->coast + dt, 9);
      bool pedal = c->coast < 6.5 && c->speed > 0.8;
      c->wheel += (c->speed * dt) / 0.351;
      if (pedal) c->crank += (c->speed * dt) / 0.351 / 2.3;   // freewheels while coasting
      if (fabs(c->z) > 126) { c->state = CY_WAIT; c->timer = 12 + rng_next(&PP->rnd) * 25; }
    }
  }
  double base_x = c->dir > 0 ? -20.45 : -15.55;
  c->x = base_x + (c->dir > 0 ? -0.55 : 0.5) * c->drift;
  c->heading = c->dir > 0 ? 0 : PI_D;
  bool riding = c->state == CY_RIDE;
  double d = cam ? js_hypot2(cam->node->position.x - c->x, cam->node->position.z - c->z) : 0;
  c->visible = riding && d < CULL;
  F->mesh->visible = F->shadow->visible = c->visible;
  c->col->x = c->x; c->col->z = c->z;
  c->col->r = riding && walker ? fmin(0.55, fmax(0, js_hypot2(walker->pos.x - c->x, walker->pos.y - c->z) - 0.305)) : 0;
  if (!c->visible) return;
  pose_cyclist(PP, F, c);
}

static void person_update(People *PP, Person *p, double dt, const Camera *cam, bool frozen) {
  switch (p->kind) {
    case K_PATH: path_update(PP, p, dt, cam, frozen); break;
    case K_WORKER: worker_update(PP, p, dt, cam, frozen); break;
    case K_CYCLIST: cyclist_update(PP, p, dt, cam, frozen); break;
  }
}

typedef struct PathOpts { double speed, turn_speed, stride_k, s0; bool run, carry; Style st; } PathOpts;
static Person *path_person(People *PP, Figure *F, WPath *path, PathOpts o) {
  Person *p = &PP->p[PP->n++];
  *p = (Person){ .kind = K_PATH, .F = F, .path = path, .s = o.s0, .speed = o.speed, .phase = 0, .visible = true };
  p->st = o.st;
  p->stride_w = calibrate(F, &WALK, &p->st);
  p->stride_r = o.run ? calibrate(F, &RUN, &p->st) : p->stride_w;
  p->col = &PP->cols[PP->ncols++];
  *p->col = (WalkCircle){ 0, 0, 0 };
  p->speed_o = o.speed;
  p->turn_speed_o = o.turn_speed;
  p->stride_k = o.stride_k;
  p->run = o.run;
  p->carry = o.carry;
  return p;
}

// ?shot: move along the loop to the leg that passes z heading north / south
static void set_path(Person *p, double z, bool north) {
  int n = (int)ceil(p->path->total / 0.25);
  double best = 0, bd = INFINITY;
  for (int i = 0; i < n; i++) {
    WPathPt q = path_at(p->path, i * 0.25);
    if ((q.dz < 0) != north) continue;
    double dd = fabs(q.z - z);
    if (dd < bd) { bd = dd; best = i * 0.25; }
  }
  p->s = best;
}

People *build_people(Node *scene, const Hotels *hotels, Walker *walker, bool shot, const char *mode,
                     int (*get_cars)(void *user, CarPass *out, int max), void *cars_user) {
  People *PP = xcalloc(1, sizeof *PP);
  PP->walker = walker;
  PP->shot = shot;
  PP->get_cars = get_cars;
  PP->cars_user = cars_user;
  bool closeup = shot && mode && !strcmp(mode, "closeup");

  // jogger: promenade, north <-> south over ~150 m, keeping to the right of the path
  {
    Figure *F = make_figure(scene, outfit_jogger());
    WPath *path = loop_path(jog_x0, jog_x1, -75, 75, 0.5);
    Style st = style();
    st.amp = 1.0; st.arm = 0.95; st.lean = 1; st.seed = 1;
    Person *p = path_person(PP, F, path, (PathOpts){ .speed = 2.9, .run = true, .turn_speed = 1.3, .stride_k = 1, .st = st, .s0 = 30 });
    p->name = "jogger";
  }
  // beach walker: slow stroll on the wet sand, feet in the swash at the seaward bends
  {
    Figure *F = make_figure(scene, outfit_walker());
    WPath *path = loop_path(walk_x0, walk_x1, -40, 50, 0.4);
    Style st = style();
    st.amp = 0.8; st.arm = 0.8; st.armR = 0.25; st.look = 8; st.seed = 2;
    Person *p = path_person(PP, F, path, (PathOpts){ .speed = 0.95, .stride_k = 1, .turn_speed = 0.6, .st = st, .carry = true, .s0 = 20 });
    p->name = "walker";
  }
  // distant stroller far up the beach (scale figure)
  {
    Figure *F = make_figure(scene, outfit_distant());
    WPath *path = loop_path(dist_x0, dist_x1, -160, -112, 0.5);
    Style st = style();
    st.amp = 0.9; st.arm = 0.9; st.seed = 3;
    Person *p = path_person(PP, F, path, (PathOpts){ .speed = 1.05, .turn_speed = 0.6, .stride_k = 1, .st = st, .s0 = 10 });
    p->name = "distant";
  }
  // cafe worker on the MARISOL terrace (the music patio at z ~ -10 is ORCHIDEA)
  {
    Figure *F = make_figure(scene, outfit_worker());
    Footprint fp = { 3.8, 24.6, -30.4 };
    for (int i = 0; hotels && i < hotels->nfootprints; i++)
      if (hotels->footprints[i].z0 > 3 && hotels->footprints[i].z1 < 26) { fp = hotels->footprints[i]; break; }
    TableFind tf = { fp.fx, HOTEL.patioX, fp.z0, fp.z1, TERRACE_Y, {} };
    if (hotels && hotels->group) node_traverse(hotels->group, find_tables_visit, &tf);
    // sort by z (Array.sort is stable: insertion sort)
    for (size_t i = 1; i < tf.out.len; i++) {
      Table t = tf.out.data[i];
      size_t j = i;
      while (j > 0 && tf.out.data[j - 1].z - t.z > 0) { tf.out.data[j] = tf.out.data[j - 1]; j--; }
      tf.out.data[j] = t;
    }
    if (!tf.out.len) {
      static const double ZS[6] = { 7.2, 8.6, 10, 18.4, 19.8, 21.2 };
      for (int i = 0; i < 6; i++) vec_push(&tf.out, ((Table){ fp.fx + 1.32, ZS[i] }));
    }
    // one run of tables on the north side of the entrance steps
    double zc = (fp.z0 + fp.z1) / 2;
    Table *run = xmalloc(tf.out.len * sizeof *run);
    int nrun = 0;
    for (size_t i = 0; i < tf.out.len; i++)
      if (tf.out.data[i].z < zc - 1.5) run[nrun++] = tf.out.data[i];
    if (nrun < 2) { memcpy(run, tf.out.data, tf.out.len * sizeof *run); nrun = (int)tf.out.len; }
    vec_free(&tf.out);
    double stand_x = fmax(fp.fx + 0.34, run[0].x - 0.6);
    mat_set_float(F->shadow->material, "uWallX", fp.fx);
    Style st = style();
    st.amp = 0.75; st.arm = 0.7; st.seed = 4;
    Person *W = &PP->p[PP->n++];
    *W = (Person){ .kind = K_WORKER, .name = "worker", .F = F, .x = stand_x, .z = run[nrun - 1].z, .heading = PI_D / 2, .phase = 0, .speed = 0,
                   .visible = true, .i = nrun - 1, .dir = -1, .task = T_WIPE, .t = 0, .dur = 7, .wipe_w = 0, .chair_w = 0, .seed = 0 };
    W->st = st;
    W->stride_w = calibrate(F, &WALK, &st) * 0.8;
    W->run_tables = run;
    W->nrun = nrun;
    W->stand_x = stand_x;
  }
  // cyclist: a slow city-bike ride along the road every minute or so
  {
    Figure *F = make_figure(scene, outfit_cyclist());
    WalkCircle *col = &PP->cols[PP->ncols++];
    *col = (WalkCircle){ 0, 0, 0 };
    PP->rnd = rng_make(77);
    double timer = 22 + rng_next(&PP->rnd) * 10;
    Person *C = &PP->p[PP->n++];
    *C = (Person){ .kind = K_CYCLIST, .name = "cyclist", .F = F, .state = CY_WAIT, .timer = timer, .dir = 1, .x = -20.5, .z = 0, .speed = 0,
                   .v0 = 4.2, .wheel = 0, .crank = 0, .coast = 0, .drift = 0, .visible = false, .heading = 0, .col = col };
  }

  // ?shot placements: frozen, deterministic
  if (shot) {
    Person *J = &PP->p[0], *Wk = &PP->p[1], *Ds = &PP->p[2], *Wo = &PP->p[3], *Cy = &PP->p[4];
    if (closeup) {
      set_path(J, -40, false); J->phase = 0.36; J->speed = 2.9;
      set_path(Wk, 20, true); Wk->phase = 0.12; Wk->speed = 0.95;
      set_path(Ds, -135, true); Ds->phase = 0.3; Ds->speed = 1.05;
      Wo->task = T_WIPE; Wo->t = 1.3; Wo->wipe_w = 1;
      Cy->state = CY_RIDE; Cy->dir = 1; Cy->z = -30; Cy->speed = 4.2; Cy->crank = 0.6; Cy->wheel = 1.3;
    } else {
      // clear of the five harness views: jogger south of view 1 (and left of the tower view),
      // walker north of view 5, the stroller far north; the worker is indoors and the cyclist
      // not riding
      J->path = xcalloc(1, sizeof(WPath));
      *J->path = (WPath){ .total = 1e9, .fixed = true, .fixed_at = { promenade_x(85) + 0.65, 85, 0, -1 } };
      J->phase = 0.2; J->speed = 2.9;
      Wk->path = xcalloc(1, sizeof(WPath));
      *Wk->path = (WPath){ .total = 1e9, .fixed = true, .fixed_at = { 88, -66, 0, 1 } };
      Wk->phase = 0.6; Wk->speed = 0.95;
      set_path(Ds, -140, true);
      Cy->state = CY_WAIT; Cy->timer = 1e9;
    }
    for (int i = 0; i < PP->n; i++) person_update(PP, &PP->p[i], 1.0 / 60, nullptr, true);
    if (!closeup) Wo->F->mesh->visible = Wo->F->shadow->visible = false;
  }
  return PP;
}

int people_colliders(People *PP, const WalkCircle **out, int max) {
  int n = 0;
  for (int i = 0; i < PP->ncols && n < max; i++) out[n++] = &PP->cols[i];
  return n;
}

void people_update(People *PP, double dt, const Camera *camera) {
  if (PP->shot) return;
  dt = fmin(dt, 0.1);
  for (int i = 0; i < PP->n; i++) person_update(PP, &PP->p[i], dt, camera, false);
}
