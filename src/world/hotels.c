// Port of src/world/hotels.js: the Art Deco hotel row along Ocean Drive (west side, facades at
// x ~ -30). Every building is assembled procedurally from a seeded spec: stucco volumes with
// rounded corners, eyebrow sunshades, central pylons / fins / bays, stepped parapets, speed
// lines, recessed windows (instanced glass that reflects the analytic sky), porches, cafe
// patios and invented neon-letter signs. Static geometry is merged per material and per street
// chunk; windows, reveals and furniture are instanced.
#include "world/hotels.h"

#include "canvas/canvas.h"
#include "gfx/three_mat.h"
#include "quality.h"
#include "textures/noise.h"
#include "world/layout.h"
#include "world/lod.h"

static constexpr double G = CURB_HEIGHT;        // sidewalk level, where the buildings stand
static constexpr double PATIO_X = HOTEL.patioX; // porches come forward to here
static const V3 UP = { 0, 1, 0 };
#define TAU (PI_D * 2)
#define NONE_HEX 0xffffffffu                    // an undefined colour (JS undefined)

// ---- small vector helpers (the JS array helpers, same operation order) ----------------------
static inline V3 add(V3 a, V3 b) { return (V3){ a.x + b.x, a.y + b.y, a.z + b.z }; }
static inline V3 sub(V3 a, V3 b) { return (V3){ a.x - b.x, a.y - b.y, a.z - b.z }; }
static inline V3 scl(V3 a, double s) { return (V3){ a.x * s, a.y * s, a.z * s }; }
static inline double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline V3 cross(V3 a, V3 b) { return v3_cross(a, b); }
static inline V3 norm(V3 a) {   // Math.hypot(...) || 1, then a / l
  double l = js_hypot3(a.x, a.y, a.z);
  if (l == 0 || isnan(l)) l = 1;
  return (V3){ a.x / l, a.y / l, a.z / l };
}
static inline V3 neg(V3 a) { return (V3){ -a.x, -a.y, -a.z }; }

// ---- colours ---------------------------------------------------------------------------------
static uint32_t lighten(uint32_t hex, double t) { return color_get_hex(color_lerp(color_hex(hex), color_hex(0xffffff), t)); }
static uint32_t darken(uint32_t hex, double k) { return color_get_hex(color_scale(color_hex(hex), k)); }
static bool is_white(uint32_t hex) {
  Color c = color_hex(hex);
  return fmin(c.r, fmin(c.g, c.b)) > 0.85;
}
static double hsl_l(Color c) {
  double h, s, l;
  color_get_hsl(c, &h, &s, &l);
  return l;
}

// ---- geometry accumulator ---------------------------------------------------------------------
// Non-indexed triangles with normal, colour, uv (metres) and a per-vertex weathering record
// (first upper-floor height, floor height, amount), eyebrow record and paint band.
typedef struct Buf {
  DVec pos, nrm, col, uv, aw, ae, ab;
  double c[3], w[3], e[3], b[4];
  int count;   // vertices, once geometry() ran
} Buf;

static void buf_init(Buf *b) {
  *b = (Buf){};
  b->c[0] = b->c[1] = b->c[2] = 1;
  b->w[1] = 3;
  b->b[0] = b->b[1] = b->b[2] = 1;
}
static void buf_band(Buf *b, uint32_t hex, int mode) {
  Color c = color_hex(hex);
  b->b[0] = c.r; b->b[1] = c.g; b->b[2] = c.b; b->b[3] = mode;
}
static void buf_color(Buf *b, uint32_t hex) {
  Color c = color_hex(hex);
  b->c[0] = c.r; b->c[1] = c.g; b->c[2] = c.b;
}
static void set3(double d[3], double x, double y, double z) { d[0] = x; d[1] = y; d[2] = z; }
static void buf_v(Buf *b, V3 p, V3 n, const double *uv) {
  double pp[3] = { p.x, p.y, p.z }, nn[3] = { n.x, n.y, n.z };
  vec_append(&b->pos, pp, 3);
  vec_append(&b->nrm, nn, 3);
  vec_append(&b->col, b->c, 3);
  if (uv) vec_append(&b->uv, uv, 2);
  else if (fabs(n.y) > 0.7) { vec_push(&b->uv, p.x); vec_push(&b->uv, p.z); }
  else if (fabs(n.x) >= fabs(n.z)) { vec_push(&b->uv, -p.z); vec_push(&b->uv, p.y); }
  else { vec_push(&b->uv, p.x); vec_push(&b->uv, p.y); }
  vec_append(&b->aw, b->w, 3);
  vec_append(&b->ae, b->e, 3);
  vec_append(&b->ab, b->b, 4);
}
static void buf_tri(Buf *b, V3 a, V3 bb, V3 c, V3 na, V3 nb, V3 nc) {
  buf_v(b, a, na, nullptr);
  buf_v(b, bb, nb, nullptr);
  buf_v(b, c, nc, nullptr);
}
static void quad4(Buf *b, V3 a, V3 bb, V3 c, V3 d, V3 na, V3 nb, V3 nc, V3 nd) {
  V3 fn = cross(sub(bb, a), sub(c, a));
  if (dot(fn, add(add(na, nb), nc)) < 0) { buf_tri(b, a, c, bb, na, nc, nb); buf_tri(b, a, d, c, na, nd, nc); }
  else { buf_tri(b, a, bb, c, na, nb, nc); buf_tri(b, a, c, d, na, nc, nd); }
}
static void quad(Buf *b, V3 a, V3 bb, V3 c, V3 d, V3 n) { quad4(b, a, bb, c, d, n, n, n, n); }
static void quad_uv(Buf *b, V3 a, V3 bb, V3 c, V3 d, V3 n, const double ua[2], const double ub[2], const double uc[2],
                    const double ud[2]) {
  V3 fn = cross(sub(bb, a), sub(c, a));
  if (dot(fn, n) < 0) {
    buf_v(b, a, n, ua); buf_v(b, c, n, uc); buf_v(b, bb, n, ub); buf_v(b, a, n, ua); buf_v(b, d, n, ud); buf_v(b, c, n, uc);
  } else {
    buf_v(b, a, n, ua); buf_v(b, bb, n, ub); buf_v(b, c, n, uc); buf_v(b, a, n, ua); buf_v(b, c, n, uc); buf_v(b, d, n, ud);
  }
}
#define UV(u, v) ((const double[2]){ (u), (v) })
static bool buf_empty(const Buf *b) { return b->pos.len == 0; }
static Geometry *buf_geometry(Buf *b) {
  Geometry *g = geo_new();
  int n = (int)(b->pos.len / 3);
  geo_set_attr_d(g, "position", 3, n, b->pos.data);
  geo_set_attr_d(g, "normal", 3, n, b->nrm.data);
  geo_set_attr_d(g, "color", 3, n, b->col.data);
  geo_set_attr_d(g, "uv", 2, n, b->uv.data);
  geo_set_attr_d(g, "aW", 3, n, b->aw.data);
  geo_set_attr_d(g, "aE", 3, n, b->ae.data);
  geo_set_attr_d(g, "aB", 4, n, b->ab.data);
  geo_compute_bsphere(g);
  b->count = n;
  vec_free(&b->pos); vec_free(&b->nrm); vec_free(&b->col); vec_free(&b->uv);
  vec_free(&b->aw); vec_free(&b->ae); vec_free(&b->ab);
  return g;
}

// ---- local frames and primitives --------------------------------------------------------------
typedef struct Frame { V3 o, X, Y, Z; } Frame;
static const Frame WF = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
static V3 P(const Frame *F, double x, double y, double z) {
  return (V3){ F->o.x + F->X.x * x + F->Y.x * y + F->Z.x * z, F->o.y + F->X.y * x + F->Y.y * y + F->Z.y * z,
               F->o.z + F->X.z * x + F->Y.z * y + F->Z.z * z };
}

// faces to leave out of a box
enum { SKIP_PX = 1, SKIP_NX = 2, SKIP_PY = 4, SKIP_NY = 8, SKIP_PZ = 16, SKIP_NZ = 32 };

// Box in a local frame F (origin + orthonormal axes).
static void lbox(Buf *b, const Frame *F, double x0, double x1, double y0, double y1, double z0, double z1, int s) {
  if (!(s & SKIP_PX)) quad(b, P(F, x1, y0, z0), P(F, x1, y1, z0), P(F, x1, y1, z1), P(F, x1, y0, z1), F->X);
  if (!(s & SKIP_NX)) quad(b, P(F, x0, y0, z0), P(F, x0, y1, z0), P(F, x0, y1, z1), P(F, x0, y0, z1), neg(F->X));
  if (!(s & SKIP_PY)) quad(b, P(F, x0, y1, z0), P(F, x1, y1, z0), P(F, x1, y1, z1), P(F, x0, y1, z1), F->Y);
  if (!(s & SKIP_NY)) quad(b, P(F, x0, y0, z0), P(F, x1, y0, z0), P(F, x1, y0, z1), P(F, x0, y0, z1), neg(F->Y));
  if (!(s & SKIP_PZ)) quad(b, P(F, x0, y0, z1), P(F, x1, y0, z1), P(F, x1, y1, z1), P(F, x0, y1, z1), F->Z);
  if (!(s & SKIP_NZ)) quad(b, P(F, x0, y0, z0), P(F, x1, y0, z0), P(F, x1, y1, z0), P(F, x0, y1, z0), neg(F->Z));
}
static void box(Buf *b, double x0, double x1, double y0, double y1, double z0, double z1, int skip) {
  lbox(b, &WF, x0, x1, y0, y1, z0, z1, skip);
}

// Cylinder (or part of one) from p0 to p1; angle 0 points along `ref`.
static void cyl_arc(Buf *b, V3 p0, V3 p1, double r, int seg, V3 ref, double a0, double a1, bool caps) {
  V3 A = norm(sub(p1, p0));
  V3 B = norm(sub(ref, scl(A, dot(ref, A))));
  V3 C = cross(A, B);
  bool have = false;
  V3 pq0 = {}, pq1 = {}, pd = {};
  for (int i = 0; i <= seg; i++) {
    double a = a0 + ((a1 - a0) * i) / seg;
    V3 d = add(scl(B, cos(a)), scl(C, sin(a)));
    V3 q0 = add(p0, scl(d, r)), q1 = add(p1, scl(d, r));
    if (have) {
      quad4(b, pq0, q0, q1, pq1, pd, d, d, pd);
      if (caps) {
        buf_tri(b, p0, pq0, q0, neg(A), neg(A), neg(A));
        buf_tri(b, p1, q1, pq1, A, A, A);
      }
    }
    have = true;
    pq0 = q0; pq1 = q1; pd = d;
  }
}
static void cyl(Buf *b, V3 p0, V3 p1, double r, int seg, V3 ref) { cyl_arc(b, p0, p1, r, seg, ref, 0, TAU, false); }
static void cyl_capped(Buf *b, V3 p0, V3 p1, double r, int seg, V3 ref) { cyl_arc(b, p0, p1, r, seg, ref, 0, TAU, true); }

// Vertical wall on a circular arc in the xz plane; angle a -> (cos a, sin a) in (x, z).
static void arc_wall(Buf *b, double cx, double cz, double r, double a0, double a1, double y0, double y1, int seg) {
  for (int i = 0; i < seg; i++) {
    double aa = a0 + ((a1 - a0) * i) / seg, ab = a0 + ((a1 - a0) * (i + 1)) / seg;
    V3 na = { cos(aa), 0, sin(aa) }, nb = { cos(ab), 0, sin(ab) };
    V3 pa = { cx + r * na.x, 0, cz + r * na.z }, pb = { cx + r * nb.x, 0, cz + r * nb.z };
    quad4(b, v3(pa.x, y0, pa.z), v3(pb.x, y0, pb.z), v3(pb.x, y1, pb.z), v3(pa.x, y1, pa.z), na, nb, nb, na);
  }
}

// A wall opening: { u, v, w, h } or { u, v, r, round: true }.
typedef struct Hole { double u, v, w, h, r; bool round; } Hole;
typedef Vec(Hole) HoleVec;
typedef struct Wall { V3 o, N; HoleVec holes; uint32_t color; } Wall;

// Flat wall in the plane through o with normal N, u along R = up x N, v = world y.
static void planar_wall(Buf *b, V3 o, V3 N, double u0, double u1, double v0, double v1, const HoleVec *holes) {
  V3 R = cross(UP, N);
  V2 corners[4] = { { u0, v0 }, { u1, v0 }, { u1, v1 }, { u0, v1 } };
  Shape shape = shape_from_points(corners, 4);
  for (size_t i = 0; i < holes->len; i++) {
    const Hole *h = &holes->data[i];
    Path *p = shape_add_hole(&shape);
    if (h->round) path_absarc(p, h->u, h->v, h->r, 0, TAU, true);
    else {
      path_move_to(p, h->u - h->w / 2, h->v - h->h / 2);
      path_line_to(p, h->u - h->w / 2, h->v + h->h / 2);
      path_line_to(p, h->u + h->w / 2, h->v + h->h / 2);
      path_line_to(p, h->u + h->w / 2, h->v - h->h / 2);
    }
  }
  Geometry *sg = geo_shape(&shape, 16);
  Geometry *g = geo_to_non_indexed(sg);
  geo_free(sg);
  shape_free(&shape);
  const float *pa = geo_data(g, "position");
  for (int i = 0; i < g->count; i++) {
    double u = pa[i * 3], v = pa[i * 3 + 1];
    buf_v(b, v3(o.x + R.x * u, v, o.z + R.z * u), N, nullptr);
  }
  geo_free(g);
}

// Point list along a facade outline: rounded corners (radius r0 at z0, r1 at z1) and the
// straight front at x = fx, clipped to [zFrom, zTo]; each point carries its normal.
typedef struct OPt { double x, z; V3 n; } OPt;
typedef struct Outline { OPt p[64]; int n; } Outline;
static void ol_push(Outline *o, double x, double z, double nx, double nz) {
  if (o->n) {
    const OPt *l = &o->p[o->n - 1];
    if (js_hypot2(l->x - x, l->z - z) < 1e-4) return;
  }
  CHECK(o->n < (int)ARRAY_LEN(o->p));
  o->p[o->n++] = (OPt){ x, z, { nx, 0, nz } };
}
static Outline outline_seg(double fx, double z0, double z1, double r0, double r1, double zFrom, double zTo, int seg) {
  Outline o = {};
  if (r0 > 0 && zFrom <= z0 + 1e-3) {
    for (int i = 0; i <= seg; i++) {
      double a = -PI_D / 2 + (PI_D / 2) * ((double)i / seg);
      ol_push(&o, fx - r0 + r0 * cos(a), z0 + r0 + r0 * sin(a), cos(a), sin(a));
    }
  }
  double za = fmax(zFrom, z0 + r0), zb = fmin(zTo, z1 - r1);
  if (zb > za) { ol_push(&o, fx, za, 1, 0); ol_push(&o, fx, zb, 1, 0); }
  if (r1 > 0 && zTo >= z1 - 1e-3) {
    for (int i = 0; i <= seg; i++) {
      double a = (PI_D / 2) * ((double)i / seg);
      ol_push(&o, fx - r1 + r1 * cos(a), z1 - r1 + r1 * sin(a), cos(a), sin(a));
    }
  }
  return o;
}
static Outline outline(double fx, double z0, double z1, double r0, double r1) {
  return outline_seg(fx, z0, z1, r0, r1, -INFINITY, INFINITY, 10);
}
static Outline outline2(double za, double zb, double fx) {   // a straight two-point run
  Outline o = { .n = 2 };
  o.p[0] = (OPt){ fx, za, { 1, 0, 0 } };
  o.p[1] = (OPt){ fx, zb, { 1, 0, 0 } };
  return o;
}

// Solid strip along an outline: projects `out` metres from the wall between y0 and y1. Used
// for eyebrow sunshades, copings, racing stripes, speed lines and plinths. under: colour of
// the underside (NONE_HEX: the same colour).
static void ribbon_u(Buf *b, const Outline *pts, double y0, double y1, double out, bool caps, uint32_t under) {
  int n = pts->n;
  if (n < 2) return;
  const double inset = 0.03;
  V3 A[64], B[64];
  for (int i = 0; i < n; i++) {
    const OPt *p = &pts->p[i];
    A[i] = v3(p->x - p->n.x * inset, 0, p->z - p->n.z * inset);
    B[i] = v3(p->x + p->n.x * out, 0, p->z + p->n.z * out);
  }
#define Y(p, y) v3((p).x, (y), (p).z)
  double top[3] = { b->c[0], b->c[1], b->c[2] };
  for (int i = 0; i < n - 1; i++) {
    V3 n0 = pts->p[i].n, n1 = pts->p[i + 1].n;
    quad4(b, Y(B[i], y0), Y(B[i + 1], y0), Y(B[i + 1], y1), Y(B[i], y1), n0, n1, n1, n0);
    quad(b, Y(A[i], y1), Y(B[i], y1), Y(B[i + 1], y1), Y(A[i + 1], y1), UP);
    if (under != NONE_HEX) buf_color(b, under);
    quad(b, Y(A[i], y0), Y(B[i], y0), Y(B[i + 1], y0), Y(A[i + 1], y0), v3(0, -1, 0));
    memcpy(b->c, top, sizeof top);
  }
  if (caps) {
    int k = n - 1;
    V3 t0 = norm(sub(v3(pts->p[0].x, 0, pts->p[0].z), v3(pts->p[1].x, 0, pts->p[1].z)));
    V3 t1 = norm(sub(v3(pts->p[k].x, 0, pts->p[k].z), v3(pts->p[k - 1].x, 0, pts->p[k - 1].z)));
    quad(b, Y(A[0], y0), Y(B[0], y0), Y(B[0], y1), Y(A[0], y1), t0);
    quad(b, Y(A[k], y0), Y(B[k], y0), Y(B[k], y1), Y(A[k], y1), t1);
  }
#undef Y
}
static void ribbon(Buf *b, const Outline *pts, double y0, double y1, double out) { ribbon_u(b, pts, y0, y1, out, true, NONE_HEX); }

static void push_geometry(Buf *b, const Geometry *geo, M4 matrix) {
  Geometry *g = geo->index ? geo_to_non_indexed(geo) : geo_clone(geo);
  geo_apply_m4(g, matrix);
  const float *p = geo_data(g, "position"), *n = geo_data(g, "normal");
  for (int i = 0; i < g->count; i++)
    buf_v(b, v3(p[i * 3], p[i * 3 + 1], p[i * 3 + 2]), v3(n[i * 3], n[i * 3 + 1], n[i * 3 + 2]), nullptr);
  geo_free(g);
}
// new THREE.Matrix4().makeRotationY(PI / 2).setPosition(x, y, z)
static M4 rot_y_at(double x, double y, double z) { return m4_set_position(m4_rotation_y(PI_D / 2), v3(x, y, z)); }

// ---- palette ---------------------------------------------------------------------------------
// Realistic pastel albedos (sRGB). Ocean Drive reads mostly white and cream with pastel bodies
// here and there and stronger pastel trim.
enum : uint32_t {
  C_WHITE = 0xe9e8e3, C_WARMWHITE = 0xe8e3d8, C_CREAM = 0xe8dcc4, C_PINK = 0xeccad0, C_BLUSH = 0xe8c6c2,
  C_MINT = 0xc4e3d3, C_SEAFOAM = 0xb6dfd2, C_AQUABODY = 0xbfe0e0, C_LEMON = 0xefe4b4, C_LAVENDER = 0xd6cce6,
  C_PEACH = 0xefd0bc, C_POWDER = 0xc6dbe8,
  C_TEAL = 0x4fa79f, C_AQUA = 0x7cc7c4, C_CORAL = 0xe0938d, C_ROSE = 0xe09aae, C_SEAGREEN = 0x7fc2a3,
  C_BUTTER = 0xefe0a4, C_LILAC = 0xb3a2d4, C_SKY = 0x86b6d6, C_SALMON = 0xe6a58e, C_SAND = 0xd9c9a8,
  C_STONEGREY = 0xcfcec6, C_MINTDEEP = 0x80d4d8, C_PINKDEEP = 0xe6a3b3,
};
typedef struct Scheme { uint32_t body, trim, accent; } Scheme;
static const Scheme SCHEMES[] = {
  { C_WHITE, C_TEAL, C_ROSE }, { C_WHITE, C_AQUA, C_PINKDEEP }, { C_PINK, C_WHITE, C_CORAL },
  { C_MINT, C_WHITE, C_TEAL }, { C_LEMON, C_WHITE, C_AQUA }, { C_LAVENDER, C_WHITE, C_LILAC },
  { C_CREAM, C_TEAL, C_CORAL }, { C_WARMWHITE, C_ROSE, C_SEAGREEN }, { C_POWDER, C_WHITE, C_SKY },
  { C_WHITE, C_SEAGREEN, C_SALMON }, { C_SEAFOAM, C_WHITE, C_TEAL }, { C_WHITE, C_CORAL, C_MINTDEEP },
  { C_AQUABODY, C_WHITE, C_ROSE }, { C_WHITE, C_LILAC, C_AQUA }, { C_BLUSH, C_WHITE, C_SEAGREEN },
};
// white / silver / anodised aluminium; the odd dark bronze or teal frame
static const uint32_t FRAME_COLS[] = { 0xf0f0ec, 0xf0f0ec, 0xe6e7e4, 0xc4c8c9, 0xb3b8ba, 0xf0f0ec, 0x5a5550, 0x5f9d96 };
static const uint32_t UMBRELLA_COLS[] = { 0xf1efe9, 0xece6d6, 0x2f8f7f, 0xd9477a, 0xf1efe9, 0x2d6f9f, 0xe9e2d0, 0xc9343e, 0x3f8a5a, 0xf2ede2 };
static const uint32_t AWNING_COLS[] = { 0x2e7fa8, 0x2f8f7f, 0xd24a74, 0x3d7d4e, 0xe0a33a, 0x7a4f9a, 0xcf5a3c };

// Invented names only.
static const char *const NAMES[] = {
  "CORALINE", "SEAGROVE", "BELLA MAR", "ORCHIDEA", "MARISOL", "ORIANA", "MARINELLA", "SOLANA",
  "DUNEHAVEN", "VISTAMAR", "LA PERLITA", "HALLORAN", "ROSALIND", "FAIRHOLM", "CALYPSO", "WYNDMERE",
  "MARBELLE", "ISLA VERDE", "COQUINA", "PALOMA", "LUNA MAR", "ASHBY", "HELIOS", "SEAFOAM",
  "BRIARCLIFF", "MONTCLAIRE", "ALDEMAR", "NEREIDA", "SUNHAVEN", "CORAL BAY", "MAREVISTA", "LINDEN",
};
// more invented names for the extended district
static const char *const MORE_NAMES[] = {
  "SOLMARE", "AZULEJO", "BAHIA LUZ", "VERANDINE", "CORALETTE", "MAREA", "ESTRELLITA", "LAGUNITA",
  "PERLAMAR", "SEABRIGHT", "BRISA", "MAR AZUL", "GLENWOOD", "LINDAMAR", "CALLOWAY", "ALMIRA",
  "VISTA SOL", "COSTA LUNA", "AURELIA", "LUMARA", "BAYBERRY", "MARIGOLD", "PALMETTE", "CORINNA",
  "BELLWOOD", "LA GAVIOTA", "ROSEMERE", "FLORAMAR", "OCEANETTE", "SUNMERE", "ALBA MAR", "NOVAMAR",
  "AMBERLY", "KESTREL",
};

// pick(rnd, arr): arr[Math.floor(rnd() * arr.length) % arr.length]
static int pick_i(Rng *rnd, int n) { return (int)fmod(floor(rng_next(rnd) * n), n); }
#define PICK(rnd, arr) ((arr)[pick_i((rnd), (int)ARRAY_LEN(arr))])
#define PICK_U(rnd, ...) PICK(rnd, ((const uint32_t[]){ __VA_ARGS__ }))
#define PICK_D(rnd, ...) PICK(rnd, ((const double[]){ __VA_ARGS__ }))
#define PICK_I(rnd, ...) PICK(rnd, ((const int[]){ __VA_ARGS__ }))
#define PICK_S(rnd, ...) PICK(rnd, ((const char *const[]){ __VA_ARGS__ }))

// ---- sign atlas ------------------------------------------------------------------------------
// Painted metal letters with thin neon tubes (faint at sunrise).
typedef enum SignFont { SF_GEO, SF_CONDENSED, SF_SCRIPT } SignFont;
typedef struct SignStyle { const char *fill, *edge, *tube; SignFont font; } SignStyle;
typedef struct SignRes { double uv[4], aspect; int n; } SignRes;
typedef struct Shelf { double x, y, rowH, x0, x1; } Shelf;
typedef struct SignCacheEntry { char key[160]; bool ok; SignRes r; } SignCacheEntry;

typedef struct SignAtlas {
  int W, H;
  Canvas *cv, *ev;
  double VX;
  Shelf hp, vp;
  int miss;
  double scale;
  Vec(SignCacheEntry) cache;
} SignAtlas;

static void atlas_init(SignAtlas *a) {
  *a = (SignAtlas){ .W = 4096, .H = 4096, .scale = 1 };
  a->cv = canvas_new(a->W, a->H);
  a->ev = canvas_new(a->W, a->H);
  cv_fill_color(a->ev, "#000");
  cv_fill_rect(a->ev, 0, 0, a->W, a->H);
  // horizontal signs pack in shelves on the left, vertical columns on the right
  a->VX = a->W - 1500;
  a->hp = (Shelf){ 4, 4, 0, 4, a->VX - 4 };
  a->vp = (Shelf){ a->VX, 4, 0, a->VX, a->W - 4 };
}

typedef struct Rect { double x, y, w, h; } Rect;
static bool atlas_alloc(SignAtlas *a, double w, double h, bool vertical, Rect *out) {
  Shelf *s = vertical ? &a->vp : &a->hp;
  if (s->x + w > s->x1) { s->x = s->x0; s->y += s->rowH + 24; s->rowH = 0; }
  if (s->y + h > a->H - 4) { a->miss++; return false; }
  *out = (Rect){ s->x, s->y, w, h };
  s->x += w + 24;
  s->rowH = fmax(s->rowH, h);
  return true;
}
static void atlas_uv(const SignAtlas *a, Rect r, double uv[4]) {
  uv[0] = r.x / a->W;
  uv[1] = 1 - (r.y + r.h) / a->H;
  uv[2] = (r.x + r.w) / a->W;
  uv[3] = 1 - r.y / a->H;
}
static void atlas_font(double px, SignFont style, char *out, size_t n) {
  if (style == SF_SCRIPT) snprintf(out, n, "italic bold %.17gpx \"Segoe Script\", \"Brush Script MT\", \"Century Gothic\", sans-serif", px);
  else if (style == SF_CONDENSED) snprintf(out, n, "bold %.17gpx \"Bahnschrift SemiBold Condensed\", \"Bahnschrift\", \"Arial Narrow\", sans-serif", px);
  else snprintf(out, n, "bold %.17gpx \"Century Gothic\", \"Futura\", \"Avenir\", \"Bahnschrift\", \"Segoe UI\", sans-serif", px);
}
static void atlas_letter(SignAtlas *a, char ch, double x, double y, double px, const SignStyle *st) {
  Canvas *ctx = a->cv, *ectx = a->ev;
  char f[160], s[2] = { ch, 0 };
  atlas_font(px, st->font, f, sizeof f);
  cv_font(ctx, f); cv_font(ectx, f);
  cv_text_align(ctx, ALIGN_CENTER); cv_text_align(ectx, ALIGN_CENTER);
  cv_text_baseline(ctx, BASE_MIDDLE); cv_text_baseline(ectx, BASE_MIDDLE);
  cv_line_join(ctx, JOIN_ROUND);
  // return edge of the channel letter (reads as depth), then the painted face, thickened so
  // strokes survive mip filtering at 80 m
  cv_fill_color(ctx, st->edge); cv_stroke_color(ctx, st->edge);
  cv_line_width(ctx, px * 0.09);
  cv_fill_text(ctx, s, x + px * 0.04, y + px * 0.05);
  cv_stroke_text(ctx, s, x + px * 0.04, y + px * 0.05);
  cv_fill_color(ctx, st->fill); cv_stroke_color(ctx, st->fill);
  cv_line_width(ctx, px * 0.07);
  cv_fill_text(ctx, s, x, y);
  cv_stroke_text(ctx, s, x, y);
  // neon tube (unlit at sunrise): only a faint emissive trace
  cv_line_width(ectx, fmax(2, px * 0.02));
  cv_stroke_color(ectx, st->tube);
  cv_save(ectx);
  cv_translate(ectx, x, y);
  cv_scale(ectx, 0.86, 0.86);
  cv_stroke_text(ectx, s, 0, 0);
  cv_restore(ectx);
}
static void atlas_clip_begin(SignAtlas *a, Rect r) {
  Canvas *cs[2] = { a->cv, a->ev };
  for (int i = 0; i < 2; i++) {
    cv_save(cs[i]);
    cv_begin_path(cs[i]);
    cv_rect(cs[i], r.x, r.y, r.w, r.h);
    cv_clip(cs[i]);
  }
}
static void atlas_clip_end(SignAtlas *a) {
  cv_restore(a->cv);
  cv_restore(a->ev);
}
static double measure_char(Canvas *c, char ch) {
  char s[2] = { ch, 0 };
  return cv_measure_text(c, s);
}
static bool atlas_horizontal0(SignAtlas *a, const char *text, const SignStyle *st, double px, SignRes *out) {
  char f[160];
  atlas_font(px, st->font, f, sizeof f);
  cv_font(a->cv, f);
  double sp = px * (st->font == SF_SCRIPT ? 0.02 : 0.16);
  int n = (int)strlen(text);
  double ws[64], total = 0;
  CHECK(n < 64);
  for (int i = 0; i < n; i++) ws[i] = measure_char(a->cv, text[i]);
  for (int i = 0; i < n; i++) total += ws[i];
  total += sp * (n - 1);
  Rect r;
  if (!atlas_alloc(a, ceil(total + px * 0.4), ceil(px * 1.45), false, &r)) return false;
  double x = r.x + px * 0.2;
  atlas_clip_begin(a, r);
  for (int i = 0; i < n; i++) {
    if (text[i] != ' ') atlas_letter(a, text[i], x + ws[i] / 2, r.y + r.h / 2, px, st);
    x += ws[i] + sp;
  }
  atlas_clip_end(a);
  atlas_uv(a, r, out->uv);
  out->aspect = r.w / r.h;
  out->n = 0;
  return true;
}
static const char *const SF_NAMES[] = { "geo", "condensed", "script" };
static bool atlas_horizontal(SignAtlas *a, const char *text, const SignStyle *st, SignRes *out) {
  double px = 150 * a->scale;
  char key[160];
  snprintf(key, sizeof key, "%s|%s|%s|%s", text, st->fill, SF_NAMES[st->font], st->tube);
  for (size_t i = 0; i < a->cache.len; i++)
    if (!strcmp(a->cache.data[i].key, key)) {
      *out = a->cache.data[i].r;
      return a->cache.data[i].ok;
    }
  SignCacheEntry e = {};
  snprintf(e.key, sizeof e.key, "%s", key);
  e.ok = atlas_horizontal0(a, text, st, px, &e.r);
  vec_push(&a->cache, e);
  *out = e.r;
  return e.ok;
}
static bool atlas_vertical(SignAtlas *a, const char *text, const SignStyle *st0, SignRes *out) {
  double px = 130 * a->scale;
  // upright letters stacked top to bottom (never rotated text); script does not stack
  SignStyle st = *st0;
  if (st.font == SF_SCRIPT) st.font = SF_GEO;
  char chars[64];
  int n = 0;
  for (const char *p = text; *p; p++)
    if (*p != ' ') { CHECK(n < 63); chars[n++] = *p; }
  double cell = px * 1.12;
  char f[160];
  atlas_font(px, st.font, f, sizeof f);
  cv_font(a->cv, f);
  double wMax = -INFINITY;
  for (int i = 0; i < n; i++) wMax = fmax(wMax, measure_char(a->cv, chars[i]));
  Rect r;
  if (!atlas_alloc(a, ceil(fmax(px * 1.25, wMax + px * 0.3)), ceil(cell * n + px * 0.25), true, &r)) return false;
  atlas_clip_begin(a, r);
  for (int i = 0; i < n; i++) atlas_letter(a, chars[i], r.x + r.w / 2, r.y + px * 0.12 + cell * (i + 0.5), px, &st);
  atlas_clip_end(a);
  atlas_uv(a, r, out->uv);
  out->aspect = r.w / r.h;
  out->n = n;
  return true;
}
// Uploads only the rows the shelves used (the atlas fills from the top), the faint neon layer
// at half resolution and, on lower tiers, a downscaled copy; the full-size canvases are freed.
// The UVs were laid out for the full atlas, so the textures remap v -> 1 - (1 - v) * H / usedH.
static void atlas_textures(SignAtlas *a, Texture **map, Texture **em) {
  double s = QUALITY.signAtlas;
  double usedH = fmin(a->H, 4 * ceil((fmax(a->hp.y + a->hp.rowH, a->vp.y + a->vp.rowH) + 8) / 4));
  Canvas *srcs[2] = { a->cv, a->ev };
  double fs[2] = { s, s * 0.5 };
  Canvas *outs[2];
  for (int i = 0; i < 2; i++) {
    double f = fs[i];
    if (f == 1 && usedH == a->H) { outs[i] = srcs[i]; continue; }
    Canvas *c = canvas_new((int)js_round(a->W * f), (int)js_round(usedH * f));
    cv_draw_image(c, srcs[i], 0, 0, a->W, usedH, 0, 0, canvas_width(c), canvas_height(c));
    canvas_free(srcs[i]);
    outs[i] = c;
  }
  a->cv = a->ev = nullptr;
  double k = a->H / usedH;
  *map = canvas_texture(outs[0], true, WRAP_CLAMP, 8);
  *em = canvas_texture(outs[1], true, WRAP_CLAMP, 1);
  for (int i = 0; i < 2; i++) {
    Texture *t = i ? *em : *map;
    t->repeat = v2(1, k);
    t->offset = v2(0, 1 - k);
    canvas_free(outs[i]);
  }
  vec_free(&a->cache);
}

// Plane of letters standing just off a wall (normal N), centred at c.
static void sign_quad(Buf *b, V3 c, V3 N, double w, double h, const double uv[4]) {
  c = add(c, scl(N, 0.06));   // channel letters stand on stand-offs, clear of the wall
  V3 R = norm(cross(UP, N));
  V3 hw = scl(R, w / 2), hh = { 0, h / 2, 0 };
#define SP(sx, sy) add(add(c, scl(hw, (sx))), scl(hh, (sy)))
  quad_uv(b, SP(-1, -1), SP(1, -1), SP(1, 1), SP(-1, 1), N, UV(uv[0], uv[1]), UV(uv[2], uv[1]), UV(uv[2], uv[3]), UV(uv[0], uv[3]));
#undef SP
}

// ---- building spec ---------------------------------------------------------------------------
typedef enum BStyle { ST_UNSET, ST_PYLON, ST_FIN, ST_BAY, ST_ZIGGURAT, ST_BAND, ST_PLAIN, ST_CORNER, ST_TWIN, ST_TOWER } BStyle;
typedef enum WinLayout { WL_UNSET, WL_PUNCHED, WL_TRIPLE, WL_RIBBON, WL_PAIR } WinLayout;
typedef enum Eyebrow { EB_UNSET, EB_FULL, EB_WINDOW, EB_BAND } Eyebrow;
typedef enum EyeCol { EC_UNSET, EC_WHITE, EC_BODY, EC_TRIM } EyeCol;
typedef enum Canopy { CN_UNSET, CN_FULL, CN_ENTRANCE, CN_AWNING, CN_NONE } Canopy;
typedef enum Patio { PT_UNSET, PT_UMBRELLA, PT_TENT, PT_AWNING, PT_CANOPY, PT_PORCH } Patio;
typedef enum Rail { RL_UNSET, RL_WALL, RL_PIPE } Rail;
typedef enum Roof { RF_UNSET, RF_TANK, RF_AC, RF_SIGN, RF_NONE } Roof;
typedef enum FrameStyle { FS_UNSET, FS_H3, FS_CROSS, FS_GRID, FS_PLAIN, FS_PANES } FrameStyle;

typedef struct Band { uint32_t col; int mode; } Band;
typedef struct OptD { bool has; double v; } OptD;
typedef struct OptU { bool has; uint32_t v; } OptU;
#define SOME(x) { true, (x) }
enum { B_UNSET, B_FALSE, B_TRUE };   // optional booleans

// makeSpec's overrides (`o`); unset fields are JS undefined
typedef struct Over {
  int floors;                       // 0: unset
  const Scheme *scheme;
  OptD setback, r0, r1, ph, winH, eyeOut, signTop, signBottom;
  BStyle style;
  WinLayout winLayout;
  const Band *band;
  Eyebrow eyebrow;
  EyeCol eyeCol;
  OptU frame, umbrellaCol, canopyCol, canopyAlt, awningColor;
  Canopy canopy;
  Patio patio;
  Rail rail;
  Roof roof;
  int porch, portholes, medallions, fountain, fins, parapetStep, finial;   // B_*
  const char *name;
  bool noSidewalk;
  bool has_exposed;
  bool exposed[2];
  OptD detail;
} Over;

typedef struct Spec {
  double z0, z1;
  int floors;
  double gH, fh, H, ph;
  Scheme scheme;
  BStyle style;
  Band band;
  WinLayout winLayout;
  double fx, r0, r1, ww, pier;
  bool paired, ribbonWin;
  double winH, paneW;
  Eyebrow eyebrow;
  double eyeOut, eyeT;
  EyeCol eyeCol;
  uint32_t frame;
  FrameStyle frameStyle;
  Canopy canopy;
  bool porch;
  Patio patio;
  uint32_t umbrellaCol, canopyCol;
  OptU canopyAlt;
  OptD signTop, signBottom;
  Rail rail;
  bool portholes, glassBlock, medallions, fountain, fins, parapetStep, finial;
  Roof roof;
  bool ac;
  const char *name;
  SignFont signFont;
  bool noSidewalk;
  OptU awningColor;
  bool exposed[2];
  double detail;
  double seed;
} Spec;


static Spec make_spec(Rng *rnd, double z0, double z1, const Over *o) {
#define R() rng_next(rnd)
  Spec S = { .z0 = z0, .z1 = z1 };
  S.floors = o->floors ? o->floors : PICK_I(rnd, 2, 2, 3, 3, 4, 4, 5, 6, 7);
  S.gH = 3.9 + R() * 0.5;
  S.fh = 3.0 + R() * 0.3;
  S.H = G + S.gH + (S.floors - 1) * S.fh;
  S.scheme = o->scheme ? *o->scheme : PICK(rnd, SCHEMES);
  double setback = o->setback.has ? o->setback.v : (R() < 0.6 ? 0 : 0.5 + R() * 1.5);
  S.style = o->style ? o->style : (BStyle)PICK_I(rnd, ST_PYLON, ST_FIN, ST_BAY, ST_ZIGGURAT, ST_BAND, ST_PLAIN, ST_CORNER, ST_TWIN, ST_TOWER, ST_FIN, ST_PYLON, ST_TWIN);
  S.ww = PICK_D(rnd, 0.9, 1.05, 1.2, 1.35, 1.5);
  double r1d;
  if (S.style == ST_CORNER) r1d = 2.6 + R() * 1.2;
  else r1d = R() < 0.22 ? 1.4 + R() * 1.4 : 0;
  S.winLayout = o->winLayout ? o->winLayout
                : S.style == ST_ZIGGURAT ? WL_RIBBON
                : (WinLayout)PICK_I(rnd, WL_PUNCHED, WL_TRIPLE, WL_RIBBON, WL_PAIR, WL_TRIPLE, WL_PUNCHED);
  bool whiteBody = is_white(S.scheme.body) || S.scheme.body == C_CREAM;
  if (o->band) S.band = *o->band;
  else if (whiteBody) {
    S.band.col = is_white(S.scheme.trim) ? S.scheme.accent : lighten(S.scheme.trim, 0.35);
    S.band.mode = R() < 0.5 ? 1 : 2;
  } else {
    S.band.col = C_WHITE;
    S.band.mode = R() < 0.6 ? 2 : 1;
  }
  // the returned object literal, in its property order
  S.ph = o->ph.has ? o->ph.v : 0.7 + R() * 1.1;
  S.fx = HOTEL.frontX - setback;
  if (o->r0.has) S.r0 = o->r0.v;
  else S.r0 = R() < 0.22 ? 1.4 + R() * 1.4 : 0;
  S.r1 = o->r1.has ? o->r1.v : r1d;
  S.pier = 0.6 + R() * 0.9;
  S.paired = S.winLayout == WL_PAIR;
  S.ribbonWin = S.winLayout == WL_RIBBON;
  S.winH = o->winH.has ? o->winH.v : 1.35 + R() * 0.3;
  S.paneW = 0.75 + R() * 0.35;
  S.eyebrow = o->eyebrow ? o->eyebrow : (Eyebrow)PICK_I(rnd, EB_FULL, EB_WINDOW, EB_WINDOW, EB_BAND);
  S.eyeOut = o->eyeOut.has ? o->eyeOut.v : 0.6 + R() * 0.35;
  S.eyeT = 0.15 + R() * 0.05;
  S.eyeCol = o->eyeCol ? o->eyeCol : (EyeCol)PICK_I(rnd, EC_WHITE, EC_WHITE, EC_BODY, EC_TRIM);
  S.frame = o->frame.has ? o->frame.v : PICK(rnd, FRAME_COLS);
  S.frameStyle = (FrameStyle)PICK_I(rnd, FS_H3, FS_CROSS, FS_GRID, FS_H3, FS_CROSS, FS_PLAIN, FS_PLAIN);
  S.canopy = o->canopy ? o->canopy : (Canopy)PICK_I(rnd, CN_FULL, CN_ENTRANCE, CN_ENTRANCE, CN_AWNING, CN_NONE);
  S.porch = o->porch ? o->porch == B_TRUE : R() < 0.8;
  S.patio = o->patio ? o->patio : (Patio)PICK_I(rnd, PT_UMBRELLA, PT_TENT, PT_AWNING, PT_CANOPY, PT_UMBRELLA, PT_PORCH);
  S.umbrellaCol = o->umbrellaCol.has ? o->umbrellaCol.v : PICK(rnd, UMBRELLA_COLS);
  S.canopyCol = o->canopyCol.has ? o->canopyCol.v : PICK_U(rnd, 0x3f9a5e, 0x3d9ad6, 0xd9668c, 0x2f8f7f);
  S.canopyAlt = o->canopyAlt;
  S.signTop = o->signTop;
  S.signBottom = o->signBottom;
  S.rail = o->rail ? o->rail : (Rail)PICK_I(rnd, RL_WALL, RL_PIPE, RL_WALL, RL_PIPE);
  S.portholes = o->portholes ? o->portholes == B_TRUE : R() < 0.45;
  S.glassBlock = R() < 0.5;
  S.medallions = o->medallions ? o->medallions == B_TRUE : R() < 0.45;
  S.fountain = o->fountain ? o->fountain == B_TRUE : R() < 0.55;   // "frozen fountain" relief over the entrance
  if (o->fins) S.fins = o->fins == B_TRUE;
  else S.fins = S.style == ST_BAND || S.style == ST_PLAIN || S.style == ST_TWIN ? R() < 0.6 : false;
  if (o->parapetStep) S.parapetStep = o->parapetStep == B_TRUE;
  else S.parapetStep = S.style != ST_ZIGGURAT && S.style != ST_TOWER && R() < 0.75;
  S.finial = o->finial ? o->finial == B_TRUE : R() < 0.6;
  S.roof = o->roof ? o->roof : (Roof)PICK_I(rnd, RF_TANK, RF_AC, RF_SIGN, RF_AC, RF_TANK, RF_NONE);
  S.ac = R() < 0.7;
  S.name = o->name;
  S.signFont = (SignFont)PICK_I(rnd, SF_GEO, SF_GEO, SF_CONDENSED, SF_SCRIPT);
  S.noSidewalk = o->noSidewalk;
  S.awningColor = o->awningColor;
  if (o->has_exposed) { S.exposed[0] = o->exposed[0]; S.exposed[1] = o->exposed[1]; }
  S.detail = o->detail.has ? o->detail.v : 1;
  S.seed = floor(R() * 1e9);
  return S;
#undef R
}

// ---- window records and furniture lists ---------------------------------------------------------
typedef enum WinKind { WK_WIN, WK_DOOR, WK_STORE, WK_BLOCK } WinKind;
typedef struct Win {
  V3 c, N;
  double w, h, depth;
  bool round;
  WinKind kind;
  bool has_interior;
  double interior[4];
  uint32_t reveal, collar, frame;
  FrameStyle frameStyle;
  int panes;
  uint32_t surround;
  bool sill;
} Win;

// furniture / palms / shrubs / bulbs / tables / chairs / umbrellas (the JS literal records)
typedef struct Item {
  double x, y, z, rot, s, r, h;
  uint32_t color;       // NONE_HEX: none
  bool pot;
  uint32_t potCol;
} Item;
typedef Vec(Item) ItemVec;

typedef struct Ctx {
  Vec(Win) windows;
  ItemVec chairs, tables, umbrellas, shrubs, palms, bulbs;
  uint32_t chairCol;
  // the current building's defaults
  uint32_t frame, collar, surround;
  FrameStyle frameStyle;
} Ctx;

// windowRecord options (o)
typedef struct WinOpt {
  bool round;
  OptD depth;
  WinKind kind;
  const double *interior;   // null: undefined
  OptU collar, frame;
  FrameStyle frameStyle;
  int panes;
  OptU surround;            // JS `surround: null` is nullish too: leave unset
  int sill;                 // B_*
} WinOpt;

static void window_record(Ctx *ctx, Wall *wall, double u, double v, double w, double h, const WinOpt *o) {
  Hole hole = o->round ? (Hole){ .u = u, .v = v, .r = w / 2, .round = true } : (Hole){ .u = u, .v = v, .w = w, .h = h };
  vec_push(&wall->holes, hole);
  V3 R = cross(UP, wall->N);
  Win rec = {
    .c = { wall->o.x + R.x * u, v, wall->o.z + R.z * u }, .N = wall->N, .w = w, .h = o->round ? w : h,
    .depth = o->depth.has ? o->depth.v : 0.3, .round = o->round, .kind = o->kind,
    .reveal = wall->color, .collar = o->collar.has ? o->collar.v : ctx->collar, .frame = o->frame.has ? o->frame.v : ctx->frame,
    .frameStyle = o->frameStyle ? o->frameStyle : ctx->frameStyle, .panes = o->panes ? o->panes : 1,
    .surround = o->surround.has ? o->surround.v : ctx->surround, .sill = o->sill ? o->sill == B_TRUE : true,
  };
  if (o->interior) { rec.has_interior = true; memcpy(rec.interior, o->interior, sizeof rec.interior); }
  vec_push(&ctx->windows, rec);
}

typedef enum IntKind { IK_WIN, IK_LOBBY, IK_STORE } IntKind;
static void interior_for(Rng *rnd, IntKind kind, double out[4]) {
#define R() rng_next(rnd)
#define SET(a, b, c, d) do { out[0] = (a); out[1] = (b); out[2] = (c); out[3] = (d); } while (0)
  // (array literal elements evaluate left to right)
  if (kind == IK_LOBBY) { double c = R(), d = R(); SET(6, 0, c, d); return; }
  if (kind == IK_STORE) {
    double a = R() < 0.5 ? 6 : 2, b = 0.3 + R() * 0.3, c = R(), d = R();
    SET(a, b, c, d);
    return;
  }
  // mostly plain reflective panes; blinds / curtains here and there, the odd one open
  double r = R();
  if (r < 0.46) { double b = R(), c = R(), d = R(); SET(0, b, c, d); return; }
  if (r < 0.62) { double b = 0.1 + R() * 0.8, c = R(), d = R(); SET(1, b, c, d); return; }
  if (r < 0.8) { double b = 0.2 + R() * 0.6, c = R(), d = R(); SET(2, b, c, d); return; }
  if (r < 0.94) { double b = 0.4 + R() * 0.5, c = R(), d = R(); SET(3, b, c, d); return; }
  double c = R(), d = R();
  SET(7, 0, c, d);
#undef SET
#undef R
}
// [a, b, rnd(), rnd()] and [a, rnd(), rnd(), rnd()]
static const double *int_fixed(Rng *rnd, double a, double b, double out[4]) {
  out[0] = a; out[1] = b; out[2] = rng_next(rnd); out[3] = rng_next(rnd);
  return out;
}
static const double *int_rrr(Rng *rnd, double a, double out[4]) {
  out[0] = a; out[1] = rng_next(rnd); out[2] = rng_next(rnd); out[3] = rng_next(rnd);
  return out;
}

// Evenly spaced window centres in [za, zb].
static int spread(double za, double zb, double unit, double pier, double *out, int cap) {
  double L = zb - za;
  double n = floor((L + pier) / (unit + pier));
  if (n <= 0) return 0;
  double gap = (L - n * unit) / (n + 1);
  CHECK(n <= cap);
  for (int i = 0; i < n; i++) out[i] = za + gap * (i + 1) + unit * i + unit / 2;
  return (int)n;
}

// Grime decal on a wall: a streaky alpha quad hanging down from `c` (top centre).
static void grime(Buf *b, V3 c, V3 N, double w, double h, double strength, Rng *rnd) {
  V3 R = norm(cross(UP, N));
  V3 o = add(c, scl(N, 0.012));
  V3 hw = scl(R, w / 2);
  double u0 = rng_next(rnd) * 0.75, u1 = u0 + 0.12 + rng_next(rnd) * 0.13;
  double vTop = rng_next(rnd) < 0.5 ? 1.0 : 0.5;   // two streak families in the atlas
  b->c[0] = strength;                               // g: dirt kind (soot / rust / algae), b: warmth
  b->c[1] = rng_next(rnd);
  b->c[2] = rng_next(rnd);
#define GP(s, y) add(add(o, scl(hw, (s))), v3(0, (y), 0))
  quad_uv(b, GP(-1, -h), GP(1, -h), GP(1, 0), GP(-1, 0), N, UV(u0, vTop - 0.5), UV(u1, vTop - 0.5), UV(u1, vTop), UV(u0, vTop));
#undef GP
}

// Segments of [a, b] left after removing the (sorted, merged) exclusion intervals.
typedef struct Iv { double a, b; } Iv;
static int segments_of(double a, double b, const Iv *excl, int nex, Iv *out) {
  int n = 0;
  double s = a;
  for (int i = 0; i < nex; i++) {
    double ea = excl[i].a, eb = excl[i].b;
    if (eb <= s) continue;
    if (ea >= b) break;
    if (ea > s) out[n++] = (Iv){ s, fmin(ea, b) };
    s = fmax(s, eb);
  }
  if (b > s) out[n++] = (Iv){ s, b };
  return n;
}
// Array.prototype.sort((p, q) => p[0] - q[0]): stable
static void sort_iv(Iv *v, int n) {
  for (int i = 1; i < n; i++) {
    Iv x = v[i];
    int j = i - 1;
    while (j >= 0 && v[j].a - x.a > 0) { v[j + 1] = v[j]; j--; }
    v[j + 1] = x;
  }
}
static void sort_d(double *v, int n) {   // (p, q) => p - q, stable
  for (int i = 1; i < n; i++) {
    double x = v[i];
    int j = i - 1;
    while (j >= 0 && v[j] - x > 0) { v[j + 1] = v[j]; j--; }
    v[j + 1] = x;
  }
}

// ---- street chunks ---------------------------------------------------------------------------
typedef struct Chunk {
  double min, max;
  Buf paint, metal, fabric, terrazzo, block, signs, grime, wire;
  Node *base, *detail;
} Chunk;

// ---- porch, patio and furniture ----------------------------------------------------------------
// (C leaves the order of argument evaluation open: every call with more than one rnd() in its
// arguments evaluates them into locals first, in the JS order.)

// Catenary wire with bulbs every ~0.6 m.
static void festoon(Ctx *ctx, Buf *metal, V3 a, V3 b, double sagK) {
  double L = js_hypot2(b.x - a.x, b.z - a.z);
  int n = (int)fmax(2, js_round(L / 0.6));
  buf_color(metal, 0x2a2826);
  bool have = false;
  V3 prev = {};
  for (int i = 0; i <= n; i++) {
    double t = (double)i / n;
    V3 p = { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t - sagK * L * 0.15 * 4 * t * (1 - t), a.z + (b.z - a.z) * t };
    if (have) cyl(metal, prev, p, 0.006, 3, v3(1, 0, 0));
    if (i > 0 && i < n) vec_push(&ctx->bulbs, ((Item){ .x = p.x, .y = p.y - 0.07, .z = p.z, .color = NONE_HEX }));
    prev = p;
    have = true;
  }
}

static void table_set(Ctx *ctx, Rng *rnd, double x, double y, double z, int chairs, double a0) {
  vec_push(&ctx->tables, ((Item){ .x = x, .y = y, .z = z, .rot = a0, .color = NONE_HEX }));
  uint32_t chairCol = ctx->chairCol;
  for (int i = 0; i < chairs; i++) {
    double a = a0 + ((double)i / chairs) * PI_D * 2 + (rng_next(rnd) - 0.5) * 0.35;
    double r = 0.58 + rng_next(rnd) * 0.12;
    double rot = -a - PI_D / 2 + (rng_next(rnd) - 0.5) * 0.5;
    vec_push(&ctx->chairs, ((Item){ .x = x + cos(a) * r, .y = y, .z = z + sin(a) * r, .rot = rot, .color = chairCol }));
  }
}

// Canvas awning over a storefront: sagging cloth on a thin tubular frame, open sides,
// scalloped valance.
typedef struct AwningP { double fx, zc, w, yTop, d, drop, sag; int nu, nv; } AwningP;
static V3 awning_p(const AwningP *A, int i, int j) {
  double t = (double)i / A->nu, s = (double)j / A->nv;
  double z = A->zc - A->w / 2 + A->w * t;
  double x = A->fx + A->d * s;
  double y = A->yTop - A->drop * s - A->sag * sin(PI_D * s) * (0.6 + 0.4 * sin(PI_D * t * 1.0));
  return v3(x, y, z);
}
static void awning(Buf *b, Buf *metal, double fx, double zc, double w, double yTop, double d, uint32_t color, bool striped,
                   Rng *rnd, double drop) {
  const double val = 0.3;
  int nu = (int)fmax(8, js_round(w / 0.64)), nv = 4;
  double sag = (0.05 + rng_next(rnd) * 0.03) * fmax(1, d / 2);
  buf_color(b, color);
  double uOff = striped ? 0 : 100;
#define SU(z) (uOff + ((z) - (zc - w / 2)) / 0.64)
  AwningP A = { fx, zc, w, yTop, d, drop, sag, nu, nv };
  V3 n = norm(v3(drop, d, 0));
  for (int i = 0; i < nu; i++)
    for (int j = 0; j < nv; j++) {
      V3 a = awning_p(&A, i, j), bb = awning_p(&A, i + 1, j), c = awning_p(&A, i + 1, j + 1), e = awning_p(&A, i, j + 1);
      quad_uv(b, a, bb, c, e, n, UV(SU(a.z), 0.7), UV(SU(bb.z), 0.7), UV(SU(c.z), 0.7), UV(SU(e.z), 0.7));
    }
  // valance hanging from the front bar, scalloped along its lower edge (alpha)
  for (int i = 0; i < nu; i++) {
    V3 a = awning_p(&A, i, nv), bb = awning_p(&A, i + 1, nv);
    V3 a2 = { a.x + 0.02, a.y - val, a.z }, b2 = { bb.x + 0.02, bb.y - val, bb.z };
    quad_uv(b, a2, b2, bb, a, v3(1, 0, 0), UV(SU(a.z), 0.0), UV(SU(bb.z), 0.0), UV(SU(bb.z), 0.25), UV(SU(a.z), 0.25));
  }
#undef SU
  buf_color(metal, 0xcfcdc6);
  V3 f0 = awning_p(&A, 0, nv), f1 = awning_p(&A, nu, nv);
  cyl(metal, v3(f0.x, f0.y + 0.01, f0.z), v3(f1.x, f1.y + 0.01, f1.z), 0.018, 5, v3(0, 1, 0));
  const double zs[2] = { zc - w / 2 + 0.03, zc + w / 2 - 0.03 };
  for (int k = 0; k < 2; k++) {
    double z = zs[k];
    cyl(metal, v3(fx, yTop, z), v3(fx + d, yTop - drop, z), 0.016, 5, v3(0, 1, 0));
    cyl(metal, v3(fx, yTop - drop - 0.35, z), v3(fx + d * 0.75, yTop - drop * 0.8, z), 0.013, 5, v3(0, 1, 0));
  }
}

// Row of white peaked cafe tents: pyramid roofs on thin posts, scalloped valance.
static void tent_row(Ctx *ctx, Buf *b, Buf *metal, double x0, double x1, double za, double zb, uint32_t color) {
  int n = (int)fmax(1, js_round((zb - za) / 2.8));
  double st = (zb - za) / n, eave = G + 2.45, apex = eave + 0.75, val = 0.28;
  double xc = (x0 + x1) / 2;
  buf_color(b, color);
#define TUV(p) UV(100 + (p).z / 0.64, 0.7)
  for (int i = 0; i < n; i++) {
    double z0 = za + st * i, z1 = z0 + st, zc = (z0 + z1) / 2;
    V3 A = { xc, apex, zc };
    V3 c[4] = { { x0, eave, z0 }, { x1, eave, z0 }, { x1, eave, z1 }, { x0, eave, z1 } };
    for (int k = 0; k < 4; k++) {
      V3 p = c[k], q = c[(k + 1) % 4];
      V3 n0 = norm(cross(sub(q, p), sub(A, p)));
      V3 nn = n0.y < 0 ? neg(n0) : n0;
      buf_v(b, p, nn, TUV(p)); buf_v(b, q, nn, TUV(q)); buf_v(b, A, nn, TUV(A));
    }
    // valance on the street side and the ends
    const struct { V3 p, q, nv; } V[3] = { { c[1], c[2], { 1, 0, 0 } }, { c[0], c[1], { 0, 0, -1 } }, { c[3], c[2], { 0, 0, 1 } } };
    for (int k = 0; k < 3; k++) {
      V3 p = V[k].p, q = V[k].q;
      double u0 = 100 + (p.z + p.x) / 0.64, u1 = 100 + (q.z + q.x) / 0.64;
      quad_uv(b, v3(p.x, p.y - val, p.z), v3(q.x, q.y - val, q.z), q, p, V[k].nv, UV(u0, 0.0), UV(u1, 0.0), UV(u1, 0.25), UV(u0, 0.25));
    }
    buf_color(metal, 0xe6e4de);
    for (int k = 0; k < 4; k++) cyl(metal, v3(c[k].x, G, c[k].z), v3(c[k].x, eave, c[k].z), 0.03, 5, v3(1, 0, 0));
  }
#undef TUV
  festoon(ctx, metal, v3(x1 - 0.05, eave - 0.05, za + 0.1), v3(x1 - 0.05, eave - 0.05, zb - 0.1), 0.05);
}

// A-frame chalkboard: two leaves meeting at the top
static void menu_board(Buf *metal, double mx, double mz) {
  for (int si = 0; si < 2; si++) {
    double s = si ? 1 : -1;
    V3 Yv = norm(v3(-s * 0.2, 1, 0));
    V3 Xv = norm(v3(1, s * 0.2, 0));
    buf_color(metal, 0x5a4430);
    Frame F1 = { { mx + s * 0.18, G, mz }, Xv, Yv, { 0, 0, 1 } };
    lbox(metal, &F1, -0.015, 0.015, 0, 0.92, -0.28, 0.28, 0);
    buf_color(metal, 0x2b2d2c);
    Frame F2 = { { mx + s * 0.18, G, mz }, { s * Xv.x, s * Xv.y, 0 }, Yv, { 0, 0, 1 } };
    lbox(metal, &F2, 0.015, 0.02, 0.1, 0.84, -0.23, 0.23, 0);
  }
}

static void build_porch(const Spec *S, Chunk *B, Ctx *ctx, Rng *rnd, double doorW, double zc) {
#define R() rng_next(rnd)
  double fx = S->fx, z0 = S->z0, z1 = S->z1;
  const Scheme *scheme = &S->scheme;
  Buf *paint = &B->paint, *metal = &B->metal, *terr = &B->terrazzo;
  double px = PATIO_X, py = G + 0.45;
  double pz0 = z0 + 0.5 + (S->r0 > 0 ? S->r0 : 0), pz1 = z1 - 0.5 - (S->r1 > 0 ? S->r1 : 0);
  if (pz1 - pz0 < 5) return;
  set3(paint->w, 100, 3, 0.9);
  set3(paint->e, 0, 0, 0);
  quad(terr, v3(fx, py, pz0), v3(px, py, pz0), v3(px, py, pz1), v3(fx, py, pz1), UP);
  buf_color(paint, darken(is_white(scheme->body) ? C_STONEGREY : scheme->body, 0.92));
  box(paint, fx - 0.1, px, G - 0.1, py, pz0, pz1, SKIP_PY | SKIP_NX | SKIP_NY);
  double sw = doorW + 0.8;
  for (int i = 0; i < 3; i++) {
    double y = G + 0.15 * (i + 1), x1 = px + 0.32 * (3 - i);
    buf_color(paint, C_STONEGREY);
    box(paint, px - 0.05, x1, G - 0.05, y, zc - sw / 2, zc + sw / 2, SKIP_PY | SKIP_NX | SKIP_NY);
    quad(terr, v3(px - 0.05, y, zc - sw / 2), v3(x1, y, zc - sw / 2), v3(x1, y, zc + sw / 2), v3(px - 0.05, y, zc + sw / 2), UP);
  }
  // handrails beside the steps
  buf_color(metal, 0xd9d7d0);
  for (int si = 0; si < 2; si++) {
    double s = si ? 1 : -1;
    double z = zc + s * (sw / 2 - 0.08);
    cyl(metal, v3(px + 0.96, G + 0.95, z), v3(px, py + 0.9, z), 0.02, 5, v3(0, 1, 0));
    cyl(metal, v3(px + 0.96, G, z), v3(px + 0.96, G + 0.95, z), 0.022, 5, v3(1, 0, 0));
  }
  // low front wall (rounded cap, accent band) or pipe railing, open at the steps
  const Iv rails[2] = { { pz0, zc - sw / 2 }, { zc + sw / 2, pz1 } };
  uint32_t wallCol = is_white(scheme->body) ? (is_white(scheme->trim) ? C_WHITE : lighten(scheme->trim, 0.35)) : lighten(scheme->body, 0.2);
  uint32_t bandCol = is_white(scheme->trim) ? scheme->accent : scheme->trim;
  for (int ri = 0; ri < 2; ri++) {
    double a = rails[ri].a, b = rails[ri].b;
    if (b - a < 0.5) continue;
    if (S->rail == RL_WALL) {
      buf_color(paint, wallCol);
      box(paint, px - 0.22, px, py, py + 0.62, a, b, SKIP_NY);
      buf_color(paint, C_WHITE);
      cyl_arc(paint, v3(px - 0.11, py + 0.62, a), v3(px - 0.11, py + 0.62, b), 0.12, 8, v3(0, 1, 0), -PI_D / 2, PI_D / 2, false);
      buf_color(paint, bandCol);
      box(paint, px - 0.005, px + 0.012, py + 0.36, py + 0.44, a, b, 0);
      // porthole cut-outs / ring motifs along the wall
      for (double z = a + 1.2; z < b - 0.8; z += 2.4) {
        Geometry *tor = geo_torus(0.16, 0.035, 5, 18, TAU, 0, TAU);
        push_geometry(paint, tor, rot_y_at(px + 0.015, py + 0.2, z));
        geo_free(tor);
      }
    } else {
      // low stucco wall with a capped top, Deco grille panels above between square posts:
      // grouped vertical bars around a ring, flat top rail
      double wy = py + 0.42;
      buf_color(paint, wallCol);
      box(paint, px - 0.22, px, py, wy, a, b, SKIP_NY);
      buf_color(paint, C_WHITE);
      box(paint, px - 0.26, px + 0.04, wy, wy + 0.06, a, b, 0);
      uint32_t gcol = is_white(scheme->trim) ? scheme->accent : scheme->trim;
      int np = (int)fmax(1, js_round((b - a) / 1.25));
      double pw = (b - a) / np;
      double top = wy + 0.5, gx = px - 0.11;
      for (int i = 0; i <= np; i++) {
        double z = a + pw * i;
        buf_color(paint, wallCol);
        box(paint, px - 0.24, px + 0.02, wy, top + 0.08, z - 0.1, z + 0.1, 0);
        buf_color(paint, C_WHITE);
        box(paint, px - 0.27, px + 0.05, top + 0.08, top + 0.13, z - 0.13, z + 0.13, 0);
      }
      buf_color(metal, gcol);
      box(metal, gx - 0.025, gx + 0.025, top - 0.03, top, a, b, 0);
      box(metal, gx - 0.015, gx + 0.015, wy + 0.06, wy + 0.09, a, b, 0);
      for (int i = 0; i < np; i++) {
        double zc2 = a + pw * (i + 0.5), yc = (wy + 0.09 + top - 0.03) / 2, rr = 0.13;
        Geometry *tor = geo_torus(rr, 0.013, 4, 20, TAU, 0, TAU);
        push_geometry(metal, tor, rot_y_at(gx, yc, zc2));
        geo_free(tor);
        const double dzs[3] = { -0.07, 0, 0.07 };
        for (int k = 0; k < 3; k++) {
          double dz = dzs[k];
          box(metal, gx - 0.008, gx + 0.008, wy + 0.09, yc - sqrt(fmax(0, rr * rr - dz * dz)), zc2 + dz - 0.008, zc2 + dz + 0.008, 0);
        }
        for (int k = 0; k < 3; k++) {
          double dz = dzs[k];
          box(metal, gx - 0.008, gx + 0.008, yc + sqrt(fmax(0, rr * rr - dz * dz)), top - 0.03, zc2 + dz - 0.008, zc2 + dz + 0.008, 0);
        }
        for (int si = 0; si < 2; si++) {
          double s = si ? 1 : -1;
          const double ks[2] = { 0.3, 0.42 };
          for (int k = 0; k < 2; k++) {
            double z = zc2 + s * pw * ks[k];
            box(metal, gx - 0.008, gx + 0.008, wy + 0.09, top - 0.03, z - 0.008, z + 0.008, 0);
          }
        }
        for (int si = 0; si < 2; si++) {
          double s = si ? 1 : -1;
          box(metal, gx - 0.008, gx + 0.008, yc - 0.008, yc + 0.008, zc2 + s * rr, zc2 + s * pw * 0.42, 0);
        }
      }
    }
    // planters with palms or shrubs at the wall ends
    buf_color(paint, PICK_U(rnd, C_WHITE, bandCol, 0xd8d3c8));
    const double zs[2] = { a + 0.45, b - 0.45 };
    for (int k = 0; k < 2; k++) {
      double z = zs[k];
      box(paint, px - 0.8, px - 0.25, py, py + 0.5, z - 0.3, z + 0.3, SKIP_NY);
      if (R() < 0.5) {
        double s = 0.5 + R() * 0.2;
        vec_push(&ctx->shrubs, ((Item){ .x = px - 0.52, .y = py + 0.58, .z = z, .s = s, .color = NONE_HEX }));
      } else {
        double s = 0.9 + R() * 0.4;
        vec_push(&ctx->palms, ((Item){ .x = px - 0.52, .y = py + 0.5, .z = z, .s = s, .pot = false, .color = NONE_HEX }));
      }
    }
  }
  // potted palms flanking the steps, a menu board on the sidewalk
  for (int si = 0; si < 2; si++) {
    double s = si ? 1 : -1;
    double sz = 1.1 + R() * 0.4;
    uint32_t potCol = PICK_U(rnd, 0xe9e6de, 0xb8704f, 0x4d4a46, 0xe9e6de);
    vec_push(&ctx->palms, ((Item){ .x = px - 0.45, .y = py, .z = zc + s * (sw / 2 + 0.45), .s = sz, .pot = true, .potCol = potCol, .color = NONE_HEX }));
  }
  if (!S->noSidewalk) {
    double mz = zc + (R() < 0.5 ? -1 : 1) * (sw / 2 + 0.7), mx = px + 0.5;
    menu_board(metal, mx, mz);
  }
  // cafe tables packed along the porch: two-tops, two rows where it is deep enough
  double depth = px - fx;
  if (depth > 1.5) {   // (S.patio is never 'none')
    double rows[2];
    int nrows;
    if (depth > 2.6) { rows[0] = fx + 0.9; rows[1] = px - 0.75; nrows = 2; }
    else { rows[0] = fx + fmin(depth * 0.55, 1.4); nrows = 1; }
    for (int ri = 0; ri < nrows; ri++) {
      double tx = rows[ri];
      for (double z = pz0 + 0.9; z < pz1 - 0.7; z += 1.3 + R() * 0.25) {
        if (fabs(z - zc) < sw / 2 + 0.5) continue;
        table_set(ctx, rnd, tx, py, z, 2, PI_D / 2);
      }
    }
  }
  // bulbs strung under a full-length canopy
  if (S->canopy == CN_FULL && R() < 0.7) {
    double y = G + S->gH - 0.45;
    festoon(ctx, &B->wire, v3(fx + 1.15, y, pz0 + 0.3), v3(fx + 1.15, y, zc - 1.2), 0.18);
    festoon(ctx, &B->wire, v3(fx + 1.15, y, zc + 1.2), v3(fx + 1.15, y, pz1 - 0.3), 0.18);
  }
  if (S->noSidewalk || S->patio == PT_PORCH) return;
  // dense sidewalk cafe: two rows of tables between the porch and the walkway, covered
  // according to the house (umbrellas, white tents, a stretched canopy, awnings)
  Iv segs[2];
  int nseg = 0;
  const Iv cand[2] = { { pz0 + 0.2, zc - sw / 2 - 0.9 }, { zc + sw / 2 + 0.9, pz1 - 0.2 } };
  for (int i = 0; i < 2; i++) if (cand[i].b - cand[i].a > 1.8) segs[nseg++] = cand[i];
  const double xs[2] = { px + 0.85, px + 2.25 };
  for (int si = 0; si < nseg; si++) {
    double a = segs[si].a, b = segs[si].b;
    for (int xi = 0; xi < 2; xi++) {
      double x = xs[xi];
      for (double z = a + 0.6; z < b - 0.5; z += 1.35 + R() * 0.25) {
        int chairs = R() < 0.6 ? 2 : 4;
        double rot0 = R() < 0.7 ? PI_D / 2 : R() * 0.3;
        table_set(ctx, rnd, x, G, z, chairs, rot0);
      }
    }
    // planters along the walkway edge
    for (double z = a + 0.4; z < b; z += 2.8 + R() * 0.6) {
      buf_color(paint, PICK_U(rnd, C_WHITE, 0xd8d3c8, is_white(scheme->trim) ? scheme->accent : scheme->trim));
      box(paint, px + 2.95, px + 3.35, G, G + 0.55, z - 0.35, z + 0.35, SKIP_NY);
      double s = 0.45 + R() * 0.15;
      vec_push(&ctx->shrubs, ((Item){ .x = px + 3.15, .y = G + 0.62, .z = z, .s = s, .color = NONE_HEX }));
    }
  }
  double ux = px + 1.55;
  if (S->patio == PT_UMBRELLA) {
    // big square umbrellas, edges overlapping; the odd one in a second colour
    uint32_t alt = PICK(rnd, UMBRELLA_COLS);
    for (int si = 0; si < nseg; si++) {
      double a = segs[si].a, b = segs[si].b;
      int n = (int)fmax(1, js_round((b - a) / 2.45));
      double st = (b - a) / n;
      size_t first = ctx->umbrellas.len;
      for (int i = 0; i < n; i++) {
        double z = a + st * (i + 0.5);
        uint32_t color = R() < 0.8 ? S->umbrellaCol : alt;
        double h = 2.4 + R() * 0.15;
        vec_push(&ctx->umbrellas, ((Item){ .x = ux, .y = G, .z = z, .color = color, .r = fmin(1.4, st * 0.56), .h = h }));
      }
      for (size_t i = first; i + 1 < ctx->umbrellas.len; i++) {
        const Item *u0 = &ctx->umbrellas.data[i], *u1 = &ctx->umbrellas.data[i + 1];
        festoon(ctx, &B->wire, v3(ux, G + u0->h - 0.35, u0->z), v3(ux, G + u1->h - 0.35, u1->z), 0.25);
      }
    }
  } else if (S->patio == PT_TENT) {
    for (int si = 0; si < nseg; si++) tent_row(ctx, &B->fabric, &B->wire, px + 0.25, px + 2.95, segs[si].a, segs[si].b, 0xf2efe8);
  } else if (S->patio == PT_CANOPY) {
    double yTop = S->canopy == CN_FULL ? G + S->gH - 0.4 : G + S->gH - 0.25;
    // separate sagging bays (never one sheet), the house colour with the odd second colour
    uint32_t alt = S->canopyAlt.has ? S->canopyAlt.v : PICK_U(rnd, 0xe07a9a, 0x4fa36a, 0xf1eee6);
    bool striped = R() < 0.4;
    for (int si = 0; si < nseg; si++) {
      double a = segs[si].a, b = segs[si].b;
      double d = px + 3.0 - fx;
      int nb = (int)fmax(1, js_round((b - a) / 3.8));
      double bw = (b - a) / nb;
      for (int k = 0; k < nb; k++) {
        double bz0 = a + bw * k;
        awning(&B->fabric, &B->wire, fx, bz0 + bw / 2, bw - 0.28, yTop - (k % 2) * 0.06, d, k % 3 == 2 ? alt : S->canopyCol, striped, rnd, 1.0);
      }
      buf_color(metal, 0xd8d6cf);
      for (int i = 0; i <= nb; i++)
        cyl(metal, v3(fx + d, G, a + 0.05 + (b - a - 0.1) * ((double)i / nb)), v3(fx + d, yTop - 1.0, a + 0.05 + (b - a - 0.1) * ((double)i / nb)), 0.035, 6, v3(1, 0, 0));
      festoon(ctx, &B->wire, v3(fx + d - 0.1, yTop - 1.05, a + 0.2), v3(fx + d - 0.1, yTop - 1.05, b - 0.2), 0.12);
    }
  }
  // second menu board at the other end
  menu_board(metal, px + 3.6, pz1 - 0.9);
#undef R
}

// ---- one building ------------------------------------------------------------------------------
typedef struct AcUnit { V3 o, N; double u, y; } AcUnit;
typedef struct ZMap { double z[128]; double a[128], b[128]; int n; } ZMap;   // Map<number, ...>
static int zmap_find(const ZMap *m, double z) {
  for (int i = 0; i < m->n; i++) if (m->z[i] == z) return i;
  return -1;
}
static void zmap_set(ZMap *m, double z, double a, double b) {
  int i = zmap_find(m, z);
  if (i < 0) { CHECK(m->n < 128); i = m->n++; m->z[i] = z; }
  m->a[i] = a;
  m->b[i] = b;
}

typedef struct HB {   // the closures' shared state in buildHotel
  const Spec *S;
  Chunk *B;
  Ctx *ctx;
  Rng *rnd;
  bool detail;
  double winH, Y0, fh, sill;
  int floors;
  uint32_t body;
  Vec(AcUnit) acUnits;
} HB;

static void upper_win(HB *h, Wall *wall, double u, double yc, double w, const WinOpt *extra) {
  double it[4];
  interior_for(h->rnd, IK_WIN, it);
  WinOpt o = { .interior = it, .depth = SOME(0.34) };
  if (extra) {
    if (extra->panes) o.panes = extra->panes;
    if (extra->frameStyle) o.frameStyle = extra->frameStyle;
    if (extra->depth.has) o.depth = extra->depth;
  }
  window_record(h->ctx, wall, u, yc, w, h->winH, &o);
  if (h->detail && h->S->ac && rng_next(h->rnd) < 0.2 && w < 1.8) {
    vec_push(&h->acUnits, ((AcUnit){ wall->o, wall->N, u, yc - h->winH / 2 - 0.08 }));
  } else if (h->detail && rng_next(h->rnd) < 0.16) {
    // run off one sill corner or the middle, varied width, length and strength
    V3 Rv = cross(UP, wall->N);
    double uu = u + (rng_next(h->rnd) - 0.5) * w * 0.7;
    V3 c = { wall->o.x + Rv.x * uu, yc - h->winH / 2 - 0.08, wall->o.z + Rv.z * uu };
    double gw = w * (0.2 + rng_next(h->rnd) * 0.6);
    double r1 = rng_next(h->rnd), r2 = rng_next(h->rnd);
    double gh = 0.3 + r1 * r2 * 2.2;
    double gs = 0.12 + rng_next(h->rnd) * 0.35;
    grime(&h->B->grime, c, wall->N, gw, gh, gs, h->rnd);
  }
}

// Rounded corner / bay end with glass wrapping round it on every upper floor: solid stucco
// bands between the floors, faceted panes in shallow reveals.
static void arc_win(HB *h, double cx, double cz, double r, double a0, double a1, double yTop) {
  int seg = (int)fmax(3, js_round((r * fabs(a1 - a0)) / 0.55));
  double ys[64];
  int ny = 0;
  ys[ny++] = G - 0.2;
  for (int f = 1; f < h->floors; f++) {
    double y0 = h->Y0 + (f - 1) * h->fh;
    ys[ny++] = y0 + h->sill;
    ys[ny++] = y0 + h->sill + h->winH;
  }
  ys[ny++] = yTop;
  for (int k = 0; k < ny; k += 2) arc_wall(&h->B->paint, cx, cz, r, a0, a1, ys[k], ys[k + 1], seg);
  double dA = (a1 - a0) / seg, ch = 2 * r * sin(fabs(dA) / 2), rc = r * cos(dA / 2);
  for (int f = 1; f < h->floors; f++) {
    double yc = h->Y0 + (f - 1) * h->fh + h->sill + h->winH / 2;
    for (int i = 0; i < seg; i++) {
      double am = a0 + dA * (i + 0.5);
      V3 N = { cos(am), 0, sin(am) };
      Wall wl = { { cx + rc * N.x, 0, cz + rc * N.z }, N, {}, h->body };
      double it[4];
      interior_for(h->rnd, IK_WIN, it);
      WinOpt o = { .depth = SOME(0.1), .frameStyle = FS_PLAIN, .sill = B_FALSE, .interior = it };
      window_record(h->ctx, &wl, 0, yc, ch, h->winH, &o);
      vec_free(&wl.holes);
    }
  }
}

// relief medallions in the parapet band
static void medal(Buf *paint, double fx, double z, double y, double r, uint32_t accent, uint32_t trim, uint32_t body) {
  buf_color(paint, accent);
  cyl_capped(paint, v3(fx - 0.02, y, z), v3(fx + 0.05, y, z), r, 20, v3(0, 1, 0));
  buf_color(paint, is_white(trim) ? body : trim);
  cyl_capped(paint, v3(fx + 0.05, y, z), v3(fx + 0.08, y, z), r * 0.45, 16, v3(0, 1, 0));
  Geometry *tor = geo_torus(r + 0.06, 0.04, 6, 28, TAU, 0, TAU);
  buf_color(paint, is_white(trim) ? lighten(body, 0.5) : trim);
  push_geometry(paint, tor, rot_y_at(fx + 0.02, y, z));
  geo_free(tor);
}

static void build_hotel(const Spec *S, Chunk *B, Ctx *ctx, SignAtlas *atlas) {
  Rng rng = rng_make(S->seed);
  Rng *rnd = &rng;
#define R() rng_next(rnd)
  const double z0 = S->z0, z1 = S->z1, fx = S->fx, gH = S->gH, fh = S->fh, H = S->H, ph = S->ph;
  const int floors = S->floors;
  const double zc = (z0 + z1) / 2, W = z1 - z0;
  const double top = H + ph;
  const double back = HOTEL.backX;
  const uint32_t body = S->scheme.body, trim = S->scheme.trim, accent = S->scheme.accent;
  Buf *paint = &B->paint, *metal = &B->metal;
  const bool detail = S->detail > 0;
  ctx->frame = S->frame;
  ctx->frameStyle = S->frameStyle;
  ctx->collar = is_white(trim) ? (is_white(accent) ? C_STONEGREY : accent) : trim;
  const double winH = fmin(S->winH, fh - 1.4);
  const double sill = 0.8;
  const double eyeOff = sill + winH + 0.1;   // eyebrow underside above each floor line
  const bool whiteBody = is_white(body) || body == C_CREAM;
  const uint32_t bandCol = S->band.col;
  // white hotels carry pastel eyebrows, pastel hotels white ones
  const uint32_t eyeCol = whiteBody ? (S->eyeCol == EC_TRIM && !is_white(trim) ? trim : (is_white(bandCol) ? trim : bandCol)) : C_WHITE;
  const uint32_t eyeUnder = whiteBody ? darken(eyeCol, 0.97) : lighten(body, 0.55);
  ctx->surround = whiteBody ? (is_white(trim) ? bandCol : trim) : C_WHITE;
  const double Y0 = G + gH;   // first upper floor line
  const double lastEye = Y0 + (floors - 2) * fh + eyeOff;
  set3(paint->w, Y0, fh, 1);
  set3(paint->e, 0, 0, 0);
  HB h = { S, B, ctx, rnd, detail, winH, Y0, fh, sill, floors, body, {} };

  // ---- facade planning ---------------------------------------------------------------------
  const double r0 = fmin(S->r0, W * 0.22), r1 = fmin(S->r1, W * 0.22);
  double cw = 0;
  if (S->style == ST_PYLON) cw = 2.4 + R() * 0.6;
  else if (S->style == ST_FIN) cw = 1.0;
  else if (S->style == ST_BAY) cw = fmin(W * 0.42, 5.5 + R() * 1.5);
  else if (S->style == ST_ZIGGURAT) cw = 3.0;
  else if (S->style == ST_TOWER) cw = fmin(W * 0.3, 3.4 + R() * 1.0);
  else if (S->style == ST_TWIN) cw = 1.5;
  struct { double a, b; bool door; } bays[2];
  int nbays = 0;
  if (S->style == ST_BAY) { bays[nbays].a = zc - cw / 2; bays[nbays].b = zc + cw / 2; bays[nbays++].door = true; }
  if (S->style == ST_TWIN) {
    double bw = fmin(W * 0.24, 3.8 + R() * 1.2);
    for (int si = 0; si < 2; si++) {
      double c = zc + (si ? 1 : -1) * W * 0.26;
      bays[nbays].a = c - bw / 2; bays[nbays].b = c + bw / 2; bays[nbays++].door = false;
    }
  }
  Wall front = { { fx, 0, 0 }, { 1, 0, 0 }, {}, body };   // u = -z
  const double zf0 = z0 + r0 + (r0 > 0 ? 0.7 : 0.9), zf1 = z1 - r1 - (r1 > 0 ? 0.7 : 0.9);
  Iv excl[4];
  int nex = 0;
  if (cw > 0 && S->style != ST_BAY) excl[nex++] = (Iv){ zc - cw / 2 - 0.45, zc + cw / 2 + 0.45 };
  for (int i = 0; i < nbays; i++) excl[nex++] = (Iv){ bays[i].a - 0.4, bays[i].b + 0.4 };
  sort_iv(excl, nex);
  Iv segs[8];
  int nsegs = segments_of(zf0, zf1, excl, nex, segs);
  const bool triple = S->winLayout == WL_TRIPLE;
  const double unit = S->paired ? S->ww * 2 + 0.22 : triple ? S->paneW * 3 : S->ww;
  const double pier = triple ? 0.9 + S->pier * 0.6 : S->pier;
  double upperCols[128];
  int ncols = 0;
  ZMap colW = {};   // z -> { w, n }: opening width and pane count
  for (int si = 0; si < nsegs; si++) {
    double a = segs[si].a, b = segs[si].b;
    if (S->ribbonWin) {
      // continuous ribbon glazing, broken by a pier every ~7 m
      double L = b - a;
      if (L < 1.4) continue;
      double k = ceil((L - 0.4) / 7.5), sb = (L - 0.4 - 0.8 * (k - 1)) / k;
      for (int i = 0; i < k; i++) {
        double n = fmax(1, js_round(sb / S->paneW));
        double z = a + 0.2 + sb / 2 + i * (sb + 0.8);
        upperCols[ncols++] = z;
        zmap_set(&colW, z, sb, n);
      }
    } else {
      double zs[64];
      int nz = spread(a, b, unit, pier, zs, 64);
      for (int i = 0; i < nz; i++) {
        CHECK(ncols < 128);
        upperCols[ncols++] = zs[i];
        zmap_set(&colW, zs[i], unit, triple ? 3 : 1);
      }
    }
  }
  sort_d(upperCols, ncols);

  // porthole columns: flanking the central element, else the outermost columns
  ZMap portCols = {};   // z -> lowest floor index with portholes
  if (S->portholes && ncols >= 2 && !S->ribbonWin) {
    if (cw > 0 && S->style != ST_BAY && S->style != ST_TWIN) {
      int li = -1, ri = -1;   // l[l.length - 1], r[0]
      for (int i = 0; i < ncols; i++) if (upperCols[i] < zc) li = i;
      for (int i = ncols - 1; i >= 0; i--) if (upperCols[i] > zc) ri = i;
      if (li >= 0 && ri >= 0) { zmap_set(&portCols, upperCols[li], 1, 0); zmap_set(&portCols, upperCols[ri], 1, 0); }
    } else {
      zmap_set(&portCols, upperCols[0], fmax(1, floors - 2), 0);
      zmap_set(&portCols, upperCols[ncols - 1], fmax(1, floors - 2), 0);
    }
  }

  double eyebrowYs[32];
  int neye = 0;
  for (int f = 1; f < floors; f++) {
    double y0 = Y0 + (f - 1) * fh;
    double yc = y0 + sill + winH / 2;
    eyebrowYs[neye++] = y0 + eyeOff;
    for (int ci = 0; ci < ncols; ci++) {
      double zcol = upperCols[ci];
      int pi = zmap_find(&portCols, zcol);
      if (pi >= 0 && f >= portCols.a[pi]) {
        double it[4];
        WinOpt o = { .round = true, .depth = SOME(0.28), .interior = int_rrr(rnd, 0, it) };
        window_record(ctx, &front, -zcol, yc + 0.05, 0.86, 0.86, &o);
        continue;
      }
      int wi = zmap_find(&colW, zcol);
      double cww = colW.a[wi], cwn = colW.b[wi];
      if (S->paired) {
        for (int si = 0; si < 2; si++) upper_win(&h, &front, -(zcol + (si ? 1 : -1) * (S->ww / 2 + 0.11)), yc, S->ww, nullptr);
      } else if (cwn > 1) {
        WinOpt x = { .panes = (int)cwn, .frameStyle = FS_PANES, .depth = SOME(0.3) };
        upper_win(&h, &front, -zcol, yc, cww, &x);
      } else {
        upper_win(&h, &front, -zcol, yc, cww, nullptr);
      }
    }
  }

  // ground floor: storefronts either side of a central entrance
  const double doorW = 2.0 + R() * 0.6, doorH = 2.6;
  const double storeH = 2.45, storeSill = 0.5;
  const bool entranceInBay = S->style == ST_BAY;
  const bool entranceOnTower = S->style == ST_TOWER;
  if (!entranceInBay && !entranceOnTower) {
    double it[4];
    WinOpt o = { .kind = WK_DOOR, .interior = int_fixed(rnd, 6, 0, it), .depth = SOME(0.35) };
    window_record(ctx, &front, -zc, G + doorH / 2, doorW, doorH, &o);
  }
  Iv gExcl[8];
  int ngex = 0;
  for (int i = 0; i < nex; i++)
    if (S->style == ST_TWIN ? excl[i].b - excl[i].a > 2 : true) gExcl[ngex++] = excl[i];
  if (!entranceInBay && !entranceOnTower) gExcl[ngex++] = (Iv){ zc - doorW / 2 - 0.9, zc + doorW / 2 + 0.9 };
  sort_iv(gExcl, ngex);
  double storeAll[64];
  int nstore = 0;
  {
    Iv gs[8];
    int ng = segments_of(zf0, zf1, gExcl, ngex, gs);
    for (int i = 0; i < ng; i++) nstore += spread(gs[i].a, gs[i].b, 2.6, 0.7, storeAll + nstore, 64 - nstore);
  }
  double storeCols[64];
  int nsc = 0;
  for (int i = 0; i < nstore; i++) if (storeAll[i] > zc) storeCols[nsc++] = storeAll[i];
  for (int i = 0; i < nstore; i++) {
    double it[4];
    interior_for(rnd, IK_STORE, it);
    WinOpt o = { .kind = WK_STORE, .interior = it, .depth = SOME(0.3) };
    window_record(ctx, &front, -storeAll[i], G + storeSill + storeH / 2, 2.6, storeH, &o);
  }
  bool gbDone = false;
  if (S->glassBlock && !entranceInBay && !entranceOnTower && nsc) {
    double gz = zc + doorW / 2 + 0.55;
    if (storeCols[0] - 1.3 > gz + 0.4) {
      for (int si = 0; si < 2; si++) {
        WinOpt o = { .kind = WK_BLOCK, .depth = SOME(0.12) };
        window_record(ctx, &front, -(zc + (si ? 1 : -1) * (gz - zc)), G + 0.3 + 1.2, 0.6, 2.4, &o);
      }
      gbDone = true;
    }
  }
  if (S->portholes && !gbDone && !entranceInBay && !entranceOnTower && nsc) {
    double gz = zc + doorW / 2 + 0.75;
    if (storeCols[0] - 1.3 > gz + 0.35) {
      for (int si = 0; si < 2; si++) {
        double it[4];
        WinOpt o = { .round = true, .depth = SOME(0.25), .interior = int_fixed(rnd, 6, 0, it) };
        window_record(ctx, &front, -(zc + (si ? 1 : -1) * (gz - zc)), G + 1.75, 0.7, 0.7, &o);
      }
    }
  }

  buf_color(paint, body);
  buf_band(paint, bandCol, S->band.mode);
  set3(paint->e, eyeOff, lastEye + 0.02, top);
  planar_wall(paint, front.o, front.N, -(z1 - r1), -(z0 + r0), G - 0.2, top, &front.holes);
  if (r0 > 0) arc_win(&h, fx - r0, z0 + r0, r0, -PI_D / 2, 0, top);
  if (r1 > 0) arc_win(&h, fx - r1, z1 - r1, r1, 0, PI_D / 2, top);
  set3(paint->e, 0, 0, top);
  paint->b[0] = paint->b[1] = paint->b[2] = 1; paint->b[3] = 0;
  for (int si = 0; si < 2; si++) {
    double side = si ? 1 : -1;
    double z = side < 0 ? z0 : z1, r = side < 0 ? r0 : r1;
    V3 N = { 0, 0, side };
    Wall wall = { { 0, 0, z }, N, {}, body };
    V3 Rv = cross(UP, N);
    double uF = (fx - r) * Rv.x, uB = back * Rv.x;
    bool exposed = side < 0 ? S->exposed[0] : S->exposed[1];
    double ds[64];
    int nds = 0;
    if (exposed) for (double d = 2.4; d < fx - r - back - 2; d += 3.0 + R() * 0.6) { CHECK(nds < 64); ds[nds++] = d; }
    else { ds[0] = 2.4; ds[1] = 6.2; ds[2] = 10.4; nds = 3; }
    if (detail || exposed) {
      for (int f = 1; f < floors; f++) {
        double y = Y0 + (f - 1) * fh + sill + 0.65;
        for (int di = 0; di < nds; di++) {
          double d = ds[di];
          if (d > 7 && !exposed && R() < 0.5) continue;
          double x = fx - r - d;
          double it[4];
          interior_for(rnd, IK_WIN, it);
          WinOpt o = { .interior = it, .depth = SOME(0.2) };
          window_record(ctx, &wall, x * Rv.x, y, 0.9, 1.25, &o);
          if (S->ac && R() < 0.25) vec_push(&h.acUnits, ((AcUnit){ wall.o, wall.N, x * Rv.x, y - 0.7 }));
        }
      }
    }
    planar_wall(paint, wall.o, N, fmin(uF, uB), fmax(uF, uB), G - 0.2, top, &wall.holes);
    vec_free(&wall.holes);
  }
  buf_color(paint, body);
  box(paint, back, back + 0.2, G - 0.2, top, z0, z1, SKIP_PX);
  buf_color(paint, C_STONEGREY);
  quad(paint, v3(back, H, z0), v3(fx - 0.2, H, z0), v3(fx - 0.2, H, z1), v3(back, H, z1), UP);
  buf_color(paint, body);
  {
    double bh = 2.2 + R() * 1.2, bx = back + 3 + R() * 6, bz = z0 + 2 + R() * (W - 7);
    double bx1 = bx + 3 + R() * 2;
    box(paint, bx, bx1, H, H + bh, bz, bz + 3, SKIP_NY);
  }

  // AC units under windows, each dribbling a rust-brown streak down the stucco
  for (size_t i = 0; i < h.acUnits.len; i++) {
    const AcUnit *a = &h.acUnits.data[i];
    V3 N = a->N, Rv = cross(UP, N);
    V3 c = { a->o.x + Rv.x * a->u, a->y, a->o.z + Rv.z * a->u };
    Frame F = { c, N, UP, cross(N, UP) };
    buf_color(paint, PICK_U(rnd, 0xcfcdc5, 0xbdbbb3, 0xd8d6cf));
    lbox(paint, &F, -0.05, 0.36, -0.5, -0.06, -0.34, 0.34, SKIP_NX);
    buf_color(paint, 0x77756f);
    lbox(paint, &F, 0.36, 0.372, -0.44, -0.12, -0.28, 0.28, SKIP_NX);
    buf_color(metal, 0x6b6a66);
    lbox(metal, &F, 0.05, 0.3, -0.53, -0.5, -0.3, -0.26, 0);
    lbox(metal, &F, 0.05, 0.3, -0.53, -0.5, 0.26, 0.3, 0);
    double gw = 0.35 + R() * 0.35, gh = 1.3 + R() * 2.2, gs = 0.55 + R() * 0.3;
    grime(&B->grime, add(c, v3(0, -0.5, 0)), N, gw, gh, gs, rnd);
  }
  // coping drips and dirty patches under the parapet and along the base
  if (detail) {
    for (double z = z0 + r0 + 0.5; z < z1 - r1 - 0.5; z += 0.8 + R() * 2.4) {
      if (R() < 0.45) {
        double gw = 0.3 + R() * 0.8, gh = 0.4 + R() * 1.4, gs = 0.2 + R() * 0.3;
        grime(&B->grime, v3(fx, top - 0.14, z), v3(1, 0, 0), gw, gh, gs, rnd);
      }
    }
    // downspouts at the square ends of the front, with a stain down each
    uint32_t pipeCol = R() < 0.5 ? darken(body, 0.9) : 0xb9b7b0;
    const double pz[2][2] = { { z0 + 0.22, r0 }, { z1 - 0.22, r1 } };
    for (int k = 0; k < 2; k++) {
      double z = pz[k][0], r = pz[k][1];
      if (r > 0 || R() < 0.35) continue;
      buf_color(metal, pipeCol);
      cyl(metal, v3(fx + 0.09, G + 0.05, z), v3(fx + 0.09, H - 0.1, z), 0.05, 6, v3(1, 0, 0));
      cyl(metal, v3(fx + 0.09, H - 0.1, z), v3(fx - 0.3, H + 0.15, z), 0.05, 6, v3(0, 1, 0));
      for (double y = G + 1.2; y < H; y += 1.8) box(metal, fx - 0.01, fx + 0.1, y, y + 0.05, z - 0.07, z + 0.07, 0);
      grime(&B->grime, v3(fx, H - 0.2, z), v3(1, 0, 0), 0.35, H - G - 0.5, 0.35, rnd);
    }
    // conduit run along the top of the ground floor to a meter box, wall vents
    if (R() < 0.7) {
      buf_color(metal, 0x9c9a94);
      double cz0 = z0 + r0 + 0.6, cz1 = cz0 + 2 + R() * 3, cy = G + gH - 0.12;
      cyl(metal, v3(fx + 0.03, cy, cz0), v3(fx + 0.03, cy, cz1), 0.018, 5, v3(0, 1, 0));
      cyl(metal, v3(fx + 0.03, cy, cz0), v3(fx + 0.03, G + 1.4, cz0), 0.018, 5, v3(1, 0, 0));
      buf_color(metal, 0x8d8b85);
      box(metal, fx - 0.01, fx + 0.14, G + 0.9, G + 1.45, cz0 - 0.22, cz0 + 0.22, 0);
    }
    for (int i = 0; i < 2; i++) {
      double vz = z0 + r0 + 1 + R() * (W - r0 - r1 - 2), vy = Y0 + floor(R() * fmax(1, floors - 1)) * fh + 0.25;
      buf_color(metal, 0xcfccc4);
      box(metal, fx - 0.01, fx + 0.03, vy, vy + 0.3, vz - 0.2, vz + 0.2, 0);
      buf_color(metal, 0x3a3936);
      for (int k = 0; k < 4; k++) box(metal, fx + 0.03, fx + 0.035, vy + 0.05 + k * 0.06, vy + 0.08 + k * 0.06, vz - 0.16, vz + 0.16, 0);
    }
  }

  // ---- ornament --------------------------------------------------------------------------------
  set3(paint->w, Y0, fh, 0.5);
  const Outline full = outline(fx, z0, z1, r0, r1);
  const bool cut = cw > 0 && S->style != ST_BAY && S->style != ST_TWIN;
  Outline splitAt[2];
  int nsplit;
  if (cut) {
    splitAt[0] = outline_seg(fx, z0, z1, r0, r1, -INFINITY, zc - cw / 2, 10);
    splitAt[1] = outline_seg(fx, z0, z1, r0, r1, zc + cw / 2, INFINITY, 10);
    nsplit = 2;
  } else {
    splitAt[0] = full;
    nsplit = 1;
  }
  buf_color(paint, darken(body, 0.88));
  ribbon(paint, &full, G - 0.2, G + 0.5, 0.05);
  buf_color(paint, trim);
  ribbon(paint, &full, top - 0.12, top + 0.05, 0.08);
  if (S->style == ST_PYLON || S->style == ST_TWIN || S->style == ST_TOWER || S->style == ST_ZIGGURAT || S->style == ST_FIN) {
    // triple speed lines across the parapet band
    buf_color(paint, is_white(trim) ? accent : trim);
    for (int k = 0; k < 3; k++) ribbon(paint, &full, top - 0.85 + k * 0.2, top - 0.77 + k * 0.2, 0.03);
  } else if (S->style != ST_PLAIN || R() < 0.5) {
    buf_color(paint, is_white(trim) ? accent : trim);
    ribbon(paint, &full, top - 0.55, top - 0.4, 0.02);
  }
  // eyebrows: deep cantilevered slabs (0.6-0.95 m) with a thick front edge, pastel underside
  // on white hotels
  const double eT = S->eyeT;
  for (int ei = 0; ei < neye; ei++) {
    double y = eyebrowYs[ei];
    buf_color(paint, eyeCol);
    if (S->eyebrow == EB_WINDOW) {
      for (int ci = 0; ci < ncols; ci++) {
        double zcol = upperCols[ci];
        int pi = zmap_find(&portCols, zcol);
        if (pi >= 0 && portCols.a[pi] <= 1) continue;
        double hw = colW.a[zmap_find(&colW, zcol)] / 2 + 0.35;
        Outline o = outline2(zcol - hw, zcol + hw, fx);
        ribbon_u(paint, &o, y, y + eT, S->eyeOut, true, eyeUnder);
      }
      if (r0 > 0) {
        Outline o = outline_seg(fx, z0, z1, r0, r1, -INFINITY, z0 + r0 + 0.4, 12);
        ribbon_u(paint, &o, y, y + eT, S->eyeOut, true, eyeUnder);
      }
      if (r1 > 0) {
        Outline o = outline_seg(fx, z0, z1, r0, r1, z1 - r1 - 0.4, INFINITY, 12);
        ribbon_u(paint, &o, y, y + eT, S->eyeOut, true, eyeUnder);
      }
    } else {
      for (int k = 0; k < nsplit; k++) ribbon_u(paint, &splitAt[k], y, y + eT, S->eyeOut, true, eyeUnder);
    }
    if (S->eyebrow == EB_BAND || S->style == ST_BAND) {
      buf_color(paint, is_white(trim) ? accent : trim);
      for (int k = 0; k < nsplit; k++) ribbon(paint, &splitAt[k], y + 0.22, y + 0.36, 0.012);
    }
  }
  // speed lines wrapping rounded corners
  {
    const double sl[2][3] = { { r0, -INFINITY, z0 + r0 + 1.6 }, { r1, z1 - r1 - 1.6, INFINITY } };
    for (int k = 0; k < 2; k++) {
      if (sl[k][0] <= 0) continue;
      Outline pts = outline_seg(fx, z0, z1, r0, r1, sl[k][1], sl[k][2], 12);
      buf_color(paint, is_white(trim) ? accent : trim);
      for (int j = 0; j < 3; j++) ribbon(paint, &pts, H - 0.3 + j * 0.2, H - 0.24 + j * 0.2, 0.035);
    }
  }
  if (S->medallions && S->style != ST_BAY) {
    double y = (top - 0.4 + (lastEye + 0.1)) / 2;
    if (top - 0.4 - (lastEye + 0.1) > 0.75) {
      double r = fmin(0.42, (top - 0.4 - lastEye - 0.1) * 0.42);
      double cands[128];
      int nc = 0;
      for (int i = 0; i < ncols - 1; i++) {
        double m = (upperCols[i] + upperCols[i + 1]) / 2;
        if (!cut || fabs(m - zc) > cw / 2 + 0.8) cands[nc++] = m;
      }
      int n = nc < 4 ? nc : 4;
      // [...cands].sort((p, q) => |p - zc| - |q - zc|) (stable)
      for (int i = 1; i < nc; i++) {
        double x = cands[i];
        int j = i - 1;
        while (j >= 0 && fabs(cands[j] - zc) - fabs(x - zc) > 0) { cands[j + 1] = cands[j]; j--; }
        cands[j + 1] = x;
      }
      for (int i = 0; i < n; i++) medal(paint, fx, cands[i], y, r, accent, trim, body);
    }
  }

  // ---- central element / massing ---------------------------------------------------------------
  bool signDone = false;
  SignStyle signStyle;
  if (S->style == ST_PYLON || S->style == ST_FIN || S->style == ST_TOWER || S->style == ST_TWIN)
    signStyle.fill = PICK_S(rnd, "#f5f1e8", "#f7efd8");
  else signStyle.fill = PICK_S(rnd, "#2f6a6a", "#8a3f45", "#34506e", "#3c3f45", "#b85f5a", "#3f7a5e", "#7a5a8a");
  signStyle.edge = "rgba(30,25,22,0.6)";
  signStyle.tube = PICK_S(rnd, "#ff9fc4", "#9ff2ea", "#fff2c0", "#ffc59a", "#c8b8ff");
  signStyle.font = S->signFont;
  const char *name = S->name;
  SignRes hs, vs;

  if (S->style == ST_PYLON || S->style == ST_TWIN) {
    double pd = S->style == ST_TWIN ? 0.3 : 0.35, yb = Y0 - 0.25, yt = top + (S->style == ST_TWIN ? 1.8 : 2.4) + R() * 2.2;
    uint32_t col = is_white(trim) ? accent : trim;
    // letters must read against the pylon: dark metal on a pale pylon
    if (hsl_l(color_hex(col)) > 0.62) signStyle.fill = PICK_S(rnd, "#2f5f63", "#7c3a40", "#3a4a66");
    buf_color(paint, col);
    Wall pw = { { fx + pd, 0, 0 }, { 1, 0, 0 }, {}, col };
    // letters down the face of the building (readable from the street), a porthole above
    double letterTop = fmin(yt - 0.7, fmin(top + 0.2, S->signTop.has ? S->signTop.v : INFINITY));
    if (yt - letterTop > 1.8) {
      double it[4];
      WinOpt o = { .round = true, .depth = SOME(0.3), .interior = int_rrr(rnd, 0, it), .collar = SOME(is_white(col) ? accent : C_WHITE) };
      window_record(ctx, &pw, -zc, (yt + letterTop) / 2 + 0.1, fmin(0.62, cw * 0.4), 0, &o);
    }
    bool hasV = name ? atlas_vertical(atlas, name, &signStyle, &vs) : false;
    double lh = hasV ? fmin(1.3, fmin(cw * 0.78, (letterTop - (S->signBottom.has ? S->signBottom.v : yb + 1.6)) / vs.n)) : 0;
    if (lh < 0.4) hasV = false;
    double signBottom = hasV ? letterTop - lh * vs.n : letterTop;
    if (S->glassBlock && signBottom - yb > 3.5) {
      WinOpt o = { .kind = WK_BLOCK, .depth = SOME(0.1) };
      window_record(ctx, &pw, -zc, (yb + 0.5 + signBottom - 0.5) / 2, 0.7, signBottom - yb - 1.0, &o);
    } else if (hasV) {
      // speed-line grooves under the letters (portholes there read as more letters)
      buf_color(paint, is_white(col) ? accent : C_WHITE);
      for (int k = 0; k < 3; k++) {
        double y = signBottom - 0.45 - k * 0.32;
        if (y > yb + 0.8) box(paint, fx + pd, fx + pd + 0.05, y - 0.07, y, zc - cw * 0.36, zc + cw * 0.36, 0);
      }
      buf_color(paint, col);
    } else {
      // a stack of portholes down the pylon
      for (double y = signBottom - 0.75; y > yb + 1.1; y -= 1.35) {
        double it[4];
        WinOpt o = { .round = true, .depth = SOME(0.3), .interior = int_rrr(rnd, 0, it), .collar = SOME(is_white(col) ? accent : C_WHITE) };
        window_record(ctx, &pw, -zc, y, fmin(0.62, cw * 0.4), 0, &o);
      }
    }
    planar_wall(paint, pw.o, pw.N, -(zc + cw / 2), -(zc - cw / 2), yb, yt, &pw.holes);
    vec_free(&pw.holes);
    box(paint, fx - 0.3, fx + pd, yb, yt, zc - cw / 2, zc + cw / 2, SKIP_PX | SKIP_NX);
    buf_color(paint, is_white(col) ? accent : C_WHITE);
    box(paint, fx - 0.3, fx + pd + 0.08, yt, yt + 0.25, zc - cw / 2 - 0.08, zc + cw / 2 + 0.08, 0);
    box(paint, fx - 0.3, fx + pd + 0.02, yt + 0.25, yt + 0.6, zc - cw / 2 + 0.35, zc + cw / 2 - 0.35, 0);
    box(paint, fx - 0.3, fx + pd - 0.04, yt + 0.6, yt + 0.9, zc - cw / 2 + 0.75, zc + cw / 2 - 0.75, 0);
    for (int k = 0; k < 4; k++) {
      double y = top - 0.4 - k * 0.45;
      for (int si = 0; si < 2; si++) {
        double s = si ? 1 : -1;
        box(paint, fx + pd - 0.02, fx + pd + 0.05, y, y + 0.14, zc + s * (cw / 2) - (s > 0 ? 0.45 : 0), zc + s * (cw / 2) + (s < 0 ? 0.45 : 0), 0);
      }
    }
    if (hasV) {
      sign_quad(&B->signs, v3(fx + pd + 0.06, (letterTop + signBottom) / 2, zc), v3(1, 0, 0), lh * vs.aspect * vs.n, lh * vs.n, vs.uv);
      signDone = true;
    }
  } else if (S->style == ST_FIN) {
    uint32_t col = is_white(trim) ? accent : trim;
    double fz = zc, yb = Y0 + 0.2, yt = top + 2.6 + R() * 2.0, fd = 1.15, ft = 0.32;
    // finSign(fz, col, yb, yt)
    buf_color(paint, col);
    box(paint, fx - 0.2, fx + fd - ft / 2, yb, yt, fz - ft / 2, fz + ft / 2, SKIP_NY);
    cyl_arc(paint, v3(fx + fd - ft / 2, yb, fz), v3(fx + fd - ft / 2, yt, fz), ft / 2, 10, v3(1, 0, 0), -PI_D / 2, PI_D / 2, false);
    buf_color(paint, trim == col ? accent : trim);
    box(paint, fx - 0.2, fx + fd * 0.75, yt, yt + 0.45, fz - ft / 2 - 0.06, fz + ft / 2 + 0.06, 0);
    box(paint, fx - 0.2, fx + fd * 0.45, yt + 0.45, yt + 0.9, fz - ft / 2 - 0.03, fz + ft / 2 + 0.03, 0);
    cyl(paint, v3(fx + fd * 0.2, yt + 0.9, fz), v3(fx + fd * 0.2, yt + 2.2, fz), 0.05, 6, v3(1, 0, 0));
    cyl_capped(paint, v3(fx + fd * 0.2, yt + 2.2, fz), v3(fx + fd * 0.2, yt + 2.42, fz), 0.12, 10, v3(1, 0, 0));
    buf_color(paint, col);
    quad(paint, v3(fx - 0.2, yb, fz - ft / 2), v3(fx + fd - ft / 2, yb, fz - ft / 2), v3(fx + fd - ft / 2, yb, fz + ft / 2), v3(fx - 0.2, yb, fz + ft / 2), v3(0, -1, 0));
    if (name && atlas_vertical(atlas, name, &signStyle, &vs)) {
      double lh = fmin(0.8, (yt - 0.4 - (yb + 1.0)) / vs.n);
      double yc = yt - 0.35 - (lh * vs.n) / 2;
      double w = lh * vs.aspect * vs.n;
      for (int si = 0; si < 2; si++) {
        double s = si ? 1 : -1;
        sign_quad(&B->signs, v3(fx + 0.1 + fd / 2, yc, fz + s * (ft / 2 + 0.04)), v3(0, 0, s), w, lh * vs.n, vs.uv);
      }
      signDone = true;
    }
  } else if (S->style == ST_TOWER) {
    double td = 0.55, tfx = fx + td, ttop = top + 3.0 + R() * 2.2;
    Wall tw = { { tfx, 0, 0 }, { 1, 0, 0 }, {}, body };
    double nw = fmin(1.3, cw - 1.8);
    for (int f = 1; f < floors; f++) {
      double y0 = Y0 + (f - 1) * fh;
      double it[4];
      interior_for(rnd, IK_WIN, it);
      WinOpt o = { .interior = it };
      window_record(ctx, &tw, -zc, y0 + sill + winH / 2, nw, winH, &o);
    }
    {
      double it[4];
      WinOpt o = { .kind = WK_DOOR, .interior = int_fixed(rnd, 6, 0, it), .depth = SOME(0.35) };
      window_record(ctx, &tw, -zc, G + doorH / 2, doorW, doorH, &o);
    }
    // glass block slot up the tower above the roofline
    {
      WinOpt o = { .kind = WK_BLOCK, .depth = SOME(0.1) };
      window_record(ctx, &tw, -zc, (top + ttop - 1.2) / 2, 0.8, ttop - top - 1.6, &o);
    }
    buf_color(paint, body);
    buf_band(paint, bandCol, S->band.mode);
    set3(paint->e, eyeOff, lastEye + 0.02, 0);
    planar_wall(paint, tw.o, tw.N, -(zc + cw / 2), -(zc - cw / 2), G - 0.2, ttop, &tw.holes);
    vec_free(&tw.holes);
    set3(paint->e, 0, 0, 0);
    paint->b[0] = paint->b[1] = paint->b[2] = 1; paint->b[3] = 0;
    buf_color(paint, eyeCol);
    for (int ei = 0; ei < neye; ei++) {
      double y = eyebrowYs[ei];
      Outline o = outline2(zc - nw / 2 - 0.35, zc + nw / 2 + 0.35, tfx);
      ribbon_u(paint, &o, y, y + eT, S->eyeOut * 0.8, true, eyeUnder);
    }
    buf_color(paint, body);
    box(paint, fx - 0.3, tfx, G - 0.2, ttop, zc - cw / 2, zc + cw / 2, SKIP_PX | SKIP_NY);
    box(paint, fx - 3, fx - 0.3, top, ttop, zc - cw / 2, zc + cw / 2, SKIP_NY | SKIP_PX);
    uint32_t tc = is_white(trim) ? accent : trim;
    buf_color(paint, tc);
    for (int si = 0; si < 2; si++) {
      double s = si ? 1 : -1;
      box(paint, tfx - 0.01, tfx + 0.06, Y0, ttop - 0.6, zc + s * (cw / 2 - 0.35) - 0.08, zc + s * (cw / 2 - 0.35) + 0.08, 0);
    }
    for (int k = 0; k < 3; k++) box(paint, fx - 0.3, tfx + 0.04, ttop - 1.3 + k * 0.22, ttop - 1.2 + k * 0.22, zc - cw / 2 - 0.04, zc + cw / 2 + 0.04, 0);
    // stepped crown and finial
    double w = cw + 0.2, y = ttop, d = td + 0.3;
    for (int s = 0; s < 3; s++) {
      buf_color(paint, s % 2 ? body : tc);
      double hh = s == 0 ? 0.3 : 0.7;
      box(paint, fx - 2.4 + s * 0.6, fx - 0.3 + d, y, y + hh, zc - w / 2, zc + w / 2, 0);
      y += hh; w *= 0.62; d -= 0.25;
    }
    buf_color(paint, tc);
    cyl(paint, v3(fx - 0.9, y, zc), v3(fx - 0.9, y + 2.6, zc), 0.06, 6, v3(1, 0, 0));
    cyl_capped(paint, v3(fx - 0.9, y + 1.0, zc), v3(fx - 0.9, y + 1.12, zc), 0.2, 10, v3(1, 0, 0));
    cyl_capped(paint, v3(fx - 0.9, y + 2.6, zc), v3(fx - 0.9, y + 2.85, zc), 0.13, 10, v3(1, 0, 0));
    if (name && atlas_horizontal(atlas, name, &signStyle, &hs)) {
      double hh = fmin(0.55, (cw - 0.4) / hs.aspect);
      sign_quad(&B->signs, v3(tfx + 0.06, ttop - 1.7 - hh / 2, zc), v3(1, 0, 0), hh * hs.aspect, hh, hs.uv);
      signDone = true;
    }
  } else if (S->style == ST_ZIGGURAT) {
    buf_color(paint, C_STONEGREY);
    box(paint, fx - 0.2, fx + 0.05, Y0 + 0.1, top, zc - cw / 2, zc + cw / 2, SKIP_NX);
    // fluted relief strips down the middle panel
    buf_color(paint, lighten(C_STONEGREY, 0.3));
    for (int k = -2; k <= 2; k++) box(paint, fx + 0.05, fx + 0.1, Y0 + 0.4, top - 0.3, zc + k * 0.5 - 0.09, zc + k * 0.5 + 0.09, SKIP_NX);
    buf_color(paint, body);
    double w = fmin(W * 0.55, 10);
    for (int s = 0; s < 4; s++) {
      double y0 = top + s * 0.7;
      box(paint, fx - 1.2, fx + 0.03, y0, y0 + 0.7, zc - w / 2, zc + w / 2, SKIP_NY);
      buf_color(paint, trim);
      box(paint, fx - 1.2, fx + 0.09, y0 + 0.6, y0 + 0.7, zc - w / 2 - 0.05, zc + w / 2 + 0.05, 0);
      buf_color(paint, body);
      w -= 2.2;
      if (w < 1.4) break;
    }
  }

  // bays (single central bay, or twin bays)
  for (int bi = 0; bi < nbays; bi++) {
    double bz0 = bays[bi].a, bz1 = bays[bi].b;
    bool withDoor = bays[bi].door;
    double bd = 0.9, rb = 0.8, bfx = fx + bd, btop = top + (withDoor ? 0.5 : 0.25);
    Wall bw = { { bfx, 0, 0 }, { 1, 0, 0 }, {}, body };
    double bc = (bz0 + bz1) / 2;
    double bL = bz1 - bz0 - 2 * rb - 0.4;
    double bn = fmax(2, js_round(bL / S->paneW));
    for (int f = 1; f < floors; f++) {
      double y0 = Y0 + (f - 1) * fh;
      WinOpt x = { .panes = (int)bn, .frameStyle = FS_PANES, .depth = SOME(0.28) };
      upper_win(&h, &bw, -bc, y0 + sill + winH / 2, bL, &x);
    }
    if (withDoor) {
      double it[4];
      WinOpt o = { .kind = WK_DOOR, .interior = int_fixed(rnd, 6, 0, it), .depth = SOME(0.35) };
      window_record(ctx, &bw, -bc, G + doorH / 2, doorW, doorH, &o);
    } else {
      double zs[32];
      int nz = spread(bz0 + rb, bz1 - rb, 2.2, 0.5, zs, 32);
      for (int i = 0; i < nz; i++) {
        double it[4];
        interior_for(rnd, IK_STORE, it);
        WinOpt o = { .kind = WK_STORE, .interior = it, .depth = SOME(0.3) };
        window_record(ctx, &bw, -zs[i], G + storeSill + storeH / 2, 2.2, storeH, &o);
      }
    }
    buf_color(paint, body);
    buf_band(paint, bandCol, S->band.mode);
    set3(paint->e, eyeOff, lastEye + 0.02, btop);
    planar_wall(paint, bw.o, bw.N, -(bz1 - rb), -(bz0 + rb), G - 0.2, btop, &bw.holes);
    vec_free(&bw.holes);
    arc_win(&h, bfx - rb, bz0 + rb, rb, -PI_D / 2, 0, btop);
    arc_win(&h, bfx - rb, bz1 - rb, rb, 0, PI_D / 2, btop);
    set3(paint->e, 0, 0, 0);
    paint->b[0] = paint->b[1] = paint->b[2] = 1; paint->b[3] = 0;
    box(paint, fx - 0.1, bfx - rb, G - 0.2, btop, bz0, bz1, SKIP_PX | SKIP_NX | SKIP_NY);
    Outline bo = outline_seg(bfx, bz0, bz1, rb, rb, -INFINITY, INFINITY, 8);
    buf_color(paint, trim);
    ribbon(paint, &bo, btop - 0.1, btop + 0.06, 0.07);
    buf_color(paint, is_white(trim) ? accent : trim);
    for (int k = 0; k < 3; k++) ribbon(paint, &bo, btop - 0.75 + k * 0.17, btop - 0.69 + k * 0.17, 0.03);
    buf_color(paint, eyeCol);
    for (int ei = 0; ei < neye; ei++) ribbon_u(paint, &bo, eyebrowYs[ei], eyebrowYs[ei] + eT, S->eyeOut * 0.9, true, eyeUnder);
    buf_color(paint, accent);
    for (int si = 0; si < 2; si++) {
      double s = si ? 1 : -1;
      box(paint, bfx - 0.01, bfx + 0.02, Y0, btop - 0.9, bc + s * ((bz1 - bz0) / 2 - rb - 0.12) - 0.07, bc + s * ((bz1 - bz0) / 2 - rb - 0.12) + 0.07, 0);
    }
    if (withDoor && name && !signDone && atlas_horizontal(atlas, name, &signStyle, &hs)) {
      double hh = fmin(0.7, ((bz1 - bz0) - 1.2) / hs.aspect);
      sign_quad(&B->signs, v3(bfx + 0.06, top - 0.35, bc), v3(1, 0, 0), hh * hs.aspect, hh, hs.uv);
      signDone = true;
    }
  }

  // corner tower rising over the rounded corner at z1
  if (S->style == ST_CORNER && r1 > 0) {
    double cTop = top + fh * 0.75 + R() * 1.2;
    double cx = fx - r1, cz = z1 - r1;
    buf_color(paint, body);
    arc_wall(paint, cx, cz, r1, 0, PI_D / 2, top, cTop, 14);
    box(paint, cx - 0.01, cx, top, cTop, cz, z1, SKIP_PX | SKIP_PY | SKIP_NY | SKIP_PZ | SKIP_NZ);
    quad(paint, v3(cx, top, cz - 0.01), v3(fx, top, cz - 0.01), v3(fx, cTop, cz - 0.01), v3(cx, cTop, cz - 0.01), v3(0, 0, -1));
    quad(paint, v3(cx, top, cz), v3(cx, top, z1), v3(cx, cTop, z1), v3(cx, cTop, cz), v3(-1, 0, 0));
    buf_color(paint, C_STONEGREY);
    for (int i = 0; i < 12; i++) {
      double a = (PI_D / 2) * (i / 12.0), b2 = (PI_D / 2) * ((i + 1) / 12.0);
      buf_tri(paint, v3(cx, cTop, cz), v3(cx + r1 * cos(a), cTop, cz + r1 * sin(a)), v3(cx + r1 * cos(b2), cTop, cz + r1 * sin(b2)), UP, UP, UP);
    }
    Outline arc0 = outline_seg(fx, z0, z1, 0, r1, z1 - r1 - 0.001, INFINITY, 14), arc = {};
    for (int i = 0; i < arc0.n; i++) if (arc0.p[i].z >= z1 - r1 - 0.01) arc.p[arc.n++] = arc0.p[i];
    buf_color(paint, trim);
    ribbon(paint, &arc, cTop - 0.12, cTop + 0.06, 0.1);
    buf_color(paint, is_white(trim) ? accent : trim);
    for (int k = 0; k < 4; k++) ribbon(paint, &arc, cTop - 1.2 + k * 0.2, cTop - 1.13 + k * 0.2, 0.035);
    buf_color(paint, eyeCol);
    ribbon(paint, &arc, top, top + 0.09, 0.45);
    // drum, mast and ball finial
    double dx = cx + r1 * 0.35, dz = cz + r1 * 0.35;
    buf_color(paint, body);
    cyl_capped(paint, v3(dx, cTop, dz), v3(dx, cTop + 0.7, dz), fmin(1.1, r1 * 0.55), 18, v3(1, 0, 0));
    buf_color(paint, trim);
    cyl_capped(paint, v3(dx, cTop + 0.7, dz), v3(dx, cTop + 0.82, dz), fmin(1.1, r1 * 0.55) + 0.08, 18, v3(1, 0, 0));
    buf_color(paint, is_white(trim) ? accent : trim);
    cyl(paint, v3(dx, cTop + 0.82, dz), v3(dx, cTop + 3.6, dz), 0.06, 6, v3(1, 0, 0));
    cyl_capped(paint, v3(dx, cTop + 3.6, dz), v3(dx, cTop + 3.85, dz), 0.15, 10, v3(1, 0, 0));
    for (int k = 0; k < 3; k++) cyl_capped(paint, v3(dx, cTop + 1.5 + k * 0.5, dz), v3(dx, cTop + 1.56 + k * 0.5, dz), 0.32 - k * 0.08, 12, v3(1, 0, 0));
  }

  // stepped parapet over the middle
  double raisedTop = top;
  if (S->parapetStep && S->style != ST_ZIGGURAT && S->style != ST_TOWER) {
    // stepped (ziggurat) parapet rising to the middle, trim-capped, finials on the shoulders
    double ws = fmin(W * (0.45 + R() * 0.2), 13), y = top;
    int tiers = 2 + (R() < 0.5 ? 1 : 0);
    uint32_t capCol = whiteBody ? (is_white(trim) ? bandCol : trim) : C_WHITE;
    for (int t = 0; t < tiers; t++) {
      double dh = 0.55 + R() * 0.4;
      buf_color(paint, body);
      box(paint, fx - 0.3, fx, y, y + dh, zc - ws / 2, zc + ws / 2, SKIP_NY);
      buf_color(paint, capCol);
      box(paint, fx - 0.32, fx + 0.1, y + dh - 0.14, y + dh + 0.04, zc - ws / 2 - 0.08, zc + ws / 2 + 0.08, 0);
      if (S->finial && t == 0) {
        for (int si = 0; si < 2; si++) {
          double fz = zc + (si ? 1 : -1) * (ws / 2 - 0.2);
          box(paint, fx - 0.25, fx + 0.05, y + dh + 0.04, y + dh + 0.5, fz - 0.15, fz + 0.15, 0);
          cyl(paint, v3(fx - 0.1, y + dh + 0.5, fz), v3(fx - 0.1, y + dh + 1.3, fz), 0.05, 6, v3(1, 0, 0));
          cyl_capped(paint, v3(fx - 0.1, y + dh + 1.3, fz), v3(fx - 0.1, y + dh + 1.5, fz), 0.12, 10, v3(1, 0, 0));
        }
      }
      y += dh; ws *= 0.62;
      if (ws < 1.6) break;
    }
    raisedTop = y;
    if (S->finial && S->style != ST_PYLON && S->style != ST_FIN && S->style != ST_TWIN) {
      buf_color(paint, capCol);
      cyl(paint, v3(fx - 0.15, y, zc), v3(fx - 0.15, y + 2.2, zc), 0.06, 6, v3(1, 0, 0));
      for (int k = 0; k < 3; k++) cyl_capped(paint, v3(fx - 0.15, y + 0.5 + k * 0.45, zc), v3(fx - 0.15, y + 0.56 + k * 0.45, zc), 0.28 - k * 0.07, 12, v3(1, 0, 0));
      cyl_capped(paint, v3(fx - 0.15, y + 2.2, zc), v3(fx - 0.15, y + 2.42, zc), 0.13, 10, v3(1, 0, 0));
    }
  }

  // extra projecting fins rising above the roofline
  if (S->fins && ncols >= 4 && S->style != ST_TWIN) {
    double mids[128];
    for (int i = 0; i < ncols - 1; i++) mids[i] = (upperCols[i] + upperCols[i + 1]) / 2;
    uint32_t fcol = is_white(trim) ? accent : trim;
    double used[2];
    int nused = 0;
    const double ts[2] = { zc - W * 0.3, zc + W * 0.3 };
    for (int ti = 0; ti < 2; ti++) {
      double t = ts[ti];
      double m = mids[0];   // mids.reduce((p, q) => |q - t| < |p - t| ? q : p, mids[0])
      for (int i = 0; i < ncols - 1; i++) if (fabs(mids[i] - t) < fabs(m - t)) m = mids[i];
      bool seen = false;
      for (int i = 0; i < nused; i++) if (used[i] == m) seen = true;
      if (seen) continue;
      used[nused++] = m;
      double yt = raisedTop + 1.0 + R() * 0.8;
      buf_color(paint, fcol);
      box(paint, fx - 0.1, fx + 0.38, Y0 - 0.1, yt, m - 0.12, m + 0.12, SKIP_NY);
      cyl_arc(paint, v3(fx + 0.38, Y0 - 0.1, m), v3(fx + 0.38, yt, m), 0.12, 8, v3(1, 0, 0), -PI_D / 2, PI_D / 2, false);
      quad(paint, v3(fx - 0.1, Y0 - 0.1, m - 0.12), v3(fx + 0.38, Y0 - 0.1, m - 0.12), v3(fx + 0.38, Y0 - 0.1, m + 0.12), v3(fx - 0.1, Y0 - 0.1, m + 0.12), v3(0, -1, 0));
      buf_color(paint, trim == fcol ? C_WHITE : trim);
      box(paint, fx - 0.1, fx + 0.32, yt, yt + 0.3, m - 0.2, m + 0.2, 0);
    }
  }

  // roof clutter that shows over the parapet
  if (S->roof == RF_TANK && detail) {
    double tx = fx - 4 - R() * 4, tz = z0 + 2.5 + R() * (W - 5), lh = 2.0 + R() * 0.8, tr = 0.9 + R() * 0.4;
    buf_color(metal, 0x6f6e69);
    const double legs[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
    for (int k = 0; k < 4; k++) {
      double dx = legs[k][0], dz = legs[k][1];
      cyl(metal, v3(tx + dx * tr * 0.7, H, tz + dz * tr * 0.7), v3(tx + dx * tr * 0.7, H + lh, tz + dz * tr * 0.7), 0.06, 5, v3(1, 0, 0));
    }
    buf_color(metal, PICK_U(rnd, 0xb8b4aa, 0x9c9a94, 0xd2cec4));
    cyl_capped(metal, v3(tx, H + lh, tz), v3(tx, H + lh + 2.0, tz), tr, 16, v3(1, 0, 0));
    Geometry *cone = geo_cone(tr + 0.05, 0.5, 16, 1, true, 0, TAU);
    push_geometry(metal, cone, m4_translation(tx, H + lh + 2.25, tz));
    geo_free(cone);
  } else if (S->roof == RF_AC && detail) {
    for (int i = 0; i < 2 + R() * 3; i++) {   // (the condition draws on every test, as in the JS)
      double ax = fx - 2.5 - R() * 5, az = z0 + 1.5 + R() * (W - 4);
      buf_color(paint, PICK_U(rnd, 0xbdbbb3, 0xa9a7a0, 0xcfcdc6));
      double y1 = H + 1.1 + R() * 0.5;
      box(paint, ax - 0.6, ax + 0.6, H, y1, az - 0.7, az + 0.7, SKIP_NY);
    }
    buf_color(metal, 0x8a8984);
    double dz = z0 + 2 + R() * (W - 4);
    box(metal, fx - 7, fx - 2, H + 1.4, H + 1.9, dz - 0.25, dz + 0.25, 0);
  }
  if (detail) {
    // rooftop odds and ends over the parapet: a TV mast, a flagpole, a terrace rail
    buf_color(metal, 0x7a7974);
    if (R() < 0.6) {
      double ax = fx - 3 - R() * 4, az = z0 + 2 + R() * (W - 4), ah = 2.5 + R() * 2.5;
      cyl(metal, v3(ax, H, az), v3(ax, H + ah, az), 0.025, 5, v3(1, 0, 0));
      for (int k = 0; k < 3; k++)
        cyl(metal, v3(ax, H + ah - 0.3 - k * 0.4, az - 0.5 + k * 0.1), v3(ax, H + ah - 0.3 - k * 0.4, az + 0.5 - k * 0.1), 0.012, 4, v3(0, 1, 0));
    }
    if (R() < 0.45) {
      double fz = z0 + 1 + R() * (W - 2), fh2 = 3.5 + R() * 1.5, py0 = raisedTop > top ? raisedTop : top + 0.2;
      buf_color(metal, 0xe8e6e0);
      cyl(metal, v3(fx - 0.4, py0, fz), v3(fx - 0.4, py0 + fh2, fz), 0.03, 6, v3(1, 0, 0));
      cyl_capped(metal, v3(fx - 0.4, py0 + fh2, fz), v3(fx - 0.4, py0 + fh2 + 0.08, fz), 0.06, 8, v3(1, 0, 0));
      buf_color(&B->fabric, PICK_U(rnd, 0x2d6f9f, 0xc0343c, 0x2f8f7f, 0xe07a9a));
      double fy = py0 + fh2 - 0.05;
      quad_uv(&B->fabric, v3(fx - 0.4, fy - 0.7, fz), v3(fx - 0.35, fy - 0.72, fz + 1.1), v3(fx - 0.35, fy - 0.02, fz + 1.1), v3(fx - 0.4, fy, fz),
              v3(1, 0, 0), UV(200, 0.7), UV(200, 0.7), UV(200, 0.7), UV(200, 0.7));
    }
    if (R() < 0.5) {
      double rx = fx - 1.6, ra = z0 + 1 + R() * W * 0.3;
      double rb = fmin(z1 - 1, ra + W * (0.3 + R() * 0.3));
      buf_color(metal, is_white(trim) ? accent : trim);
      const double ys[2] = { H + 0.55, H + 1.0 };
      for (int k = 0; k < 2; k++) cyl(metal, v3(rx, ys[k], ra), v3(rx, ys[k], rb), 0.022, 5, v3(0, 1, 0));
      for (double z = ra; z <= rb + 1e-3; z += fmax(0.8, (rb - ra) / ceil((rb - ra) / 1.2)))
        cyl(metal, v3(rx, H, z), v3(rx, H + 1.0, z), 0.022, 5, v3(1, 0, 0));
    }
  }
  if (S->roof == RF_SIGN && name && !signDone) {
    SignStyle st = signStyle;
    st.fill = PICK_S(rnd, "#f5f1e8", "#2f6a6a", "#8a3f45");
    if (atlas_horizontal(atlas, name, &st, &hs)) {
      double hh = fmin(1.3, (W * 0.55) / hs.aspect), w = hh * hs.aspect;
      double sx = fx - 1.2, y0 = raisedTop + 0.35;
      buf_color(metal, 0x5d5c58);
      for (double z = zc - w / 2; z <= zc + w / 2 + 0.01; z += w / fmax(2, js_round(w / 1.6))) {
        cyl(metal, v3(sx - 0.05, top - 0.3, z), v3(sx - 0.05, y0 + hh + 0.15, z), 0.04, 5, v3(1, 0, 0));
        cyl(metal, v3(sx - 0.05, y0 + hh + 0.1, z), v3(sx - 0.9, H, z), 0.03, 5, v3(1, 0, 0));
      }
      const double ys[2] = { y0 - 0.05, y0 + hh + 0.1 };
      for (int k = 0; k < 2; k++) cyl(metal, v3(sx - 0.05, ys[k], zc - w / 2), v3(sx - 0.05, ys[k], zc + w / 2), 0.035, 5, v3(0, 1, 0));
      sign_quad(&B->signs, v3(sx + 0.02, y0 + hh / 2, zc), v3(1, 0, 0), w, hh, hs.uv);
      signDone = true;
    }
  }

  // ---- ground floor: canopy, entrance, relief, porch, patio ---------------------------------
  set3(paint->w, 100, fh, 0.8);
  const double canY = G + gH - 0.35;
  const bool upperSign = signDone;
  bool entSign = false;
  SignStyle entStyle = signStyle;
  if (hsl_l(color_style(signStyle.fill)) > 0.6) entStyle.fill = PICK_S(rnd, "#2f5f63", "#7c3a40", "#34506e", "#3c3f45");
  if (S->canopy == CN_FULL) {
    buf_color(paint, is_white(trim) ? trim : eyeCol);
    for (int k = 0; k < nsplit; k++) ribbon(paint, &splitAt[k], canY, canY + 0.14, 1.3);
    buf_color(paint, accent);
    for (int k = 0; k < nsplit; k++) ribbon(paint, &splitAt[k], canY + 0.14, canY + 0.2, 0.02);
  }
  const double entranceX = S->style == ST_BAY ? fx + 0.9 : S->style == ST_TOWER ? fx + 0.55 : fx;
  double pilasterTop = G + gH - 0.4;
  double canopyTop = G + doorH + 0.2;
  if (S->canopy == CN_ENTRANCE || S->canopy == CN_AWNING || S->style == ST_BAY || S->style == ST_TOWER) {
    double cwid = doorW + 1.6 + R() * 1.2, cp = 1.6 + R() * 0.6;
    double cy = G + doorH + 0.45;
    pilasterTop = cy - 0.01;
    canopyTop = cy + 0.16;
    Shape shape = shape_new();
    double rr = fmin(0.6, cp * 0.5);
    path_move_to(&shape.path, entranceX - 0.05, -(zc - cwid / 2));
    path_line_to(&shape.path, entranceX + cp - rr, -(zc - cwid / 2));
    path_absarc(&shape.path, entranceX + cp - rr, -(zc - cwid / 2) - rr, rr, PI_D / 2, 0, true);
    path_line_to(&shape.path, entranceX + cp, -(zc + cwid / 2) + rr);
    path_absarc(&shape.path, entranceX + cp - rr, -(zc + cwid / 2) + rr, rr, 0, -PI_D / 2, true);
    path_line_to(&shape.path, entranceX - 0.05, -(zc + cwid / 2));
    Geometry *eg = geo_extrude(&shape, (ExtrudeOptions){ .depth = 0.16, .steps = 1, .curve_segments = 8 });
    shape_free(&shape);
    geo_rotate_x(eg, -PI_D / 2);
    buf_color(paint, is_white(trim) ? trim : C_WHITE);
    push_geometry(paint, eg, m4_translation(0, cy, 0));
    geo_free(eg);
    buf_color(paint, accent);
    box(paint, entranceX + cp - 0.02, entranceX + cp + 0.01, cy + 0.02, cy + 0.12, zc - cwid / 2 + rr, zc + cwid / 2 - rr, 0);
    // tie rods back to the wall
    buf_color(&B->wire, 0xd8d6cf);
    for (int si = 0; si < 2; si++) {
      double s = si ? 1 : -1;
      cyl(&B->wire, v3(entranceX + cp - 0.25, cy + 0.16, zc + s * (cwid / 2 - 0.3)), v3(entranceX, cy + 1.2, zc + s * (cwid / 2 - 0.3)), 0.018, 5, v3(0, 1, 0));
    }
    if (name && atlas_horizontal(atlas, name, &entStyle, &hs)) {
      double hh = fmin(0.7, (cwid + 1.6) / hs.aspect);
      // fascia board the channel letters stand on (their shadow lands right behind them)
      double lw = hh * hs.aspect;
      buf_color(paint, hsl_l(color_style(entStyle.fill)) > 0.5 ? accent : C_WHITE);
      box(paint, entranceX + cp - 0.3, entranceX + cp - 0.2, cy + 0.16, cy + 0.3 + hh, zc - lw / 2 - 0.12, zc + lw / 2 + 0.12, 0);
      sign_quad(&B->signs, v3(entranceX + cp - 0.2, cy + 0.23 + hh / 2, zc), v3(1, 0, 0), lw, hh, hs.uv);
      signDone = true;
      entSign = true;
      canopyTop = cy + 0.25 + hh;
    }
  }
  if (!entSign && name && !entranceInBay && atlas_horizontal(atlas, name, &entStyle, &hs)) {
    // raised letters on the wall over the door
    double hh = fmin(0.5, (doorW + 2.4) / hs.aspect);
    double y = S->canopy == CN_FULL ? canY - 0.12 - hh / 2 : G + doorH + 0.3 + hh / 2;
    sign_quad(&B->signs, v3(entranceX + (S->canopy == CN_FULL ? 1.32 : 0.06), y, zc), v3(1, 0, 0), hh * hs.aspect, hh, hs.uv);
    if (S->canopy != CN_FULL) canopyTop = y + hh / 2 + 0.05;
    signDone = true;
  }
  if (!upperSign && name && atlas_horizontal(atlas, name, &signStyle, &hs)) {
    bool onStep = raisedTop > top + 0.5;
    double hh = fmin(1.0, fmin((W * 0.6) / hs.aspect, onStep ? raisedTop - top - 0.1 : ph + 0.5));
    double y = S->style == ST_ZIGGURAT ? top + 0.55 : onStep ? (top + raisedTop) / 2 - 0.05 : top - ph / 2 - 0.05;
    sign_quad(&B->signs, v3(fx + (S->style == ST_ZIGGURAT ? 0.09 : 0.06), y, zc), v3(1, 0, 0), hh * hs.aspect, hh, hs.uv);
  }
  // "frozen fountain" relief panel over the entrance
  if (S->fountain && detail && S->style != ST_BAY) {
    double fy0 = fmax(canopyTop + 0.15, S->canopy == CN_FULL ? canY + 0.3 : 0), fy1 = Y0 + sill - 0.2;
    if (fy1 - fy0 > 0.7) {
      double fh2 = fmin(1.5, fy1 - fy0), fw = fmin(3.2, doorW + 1.0);
      double yb = fy0, x = entranceX;
      uint32_t rc = is_white(body) ? (is_white(trim) ? accent : trim) : lighten(body, 0.55);
      Frame F = { { x, yb, zc }, { 1, 0, 0 }, UP, { 0, 0, 1 } };
      buf_color(paint, rc);
      // recessed-looking panel frame: proud base course and cornice
      lbox(paint, &F, -0.02, 0.14, 0, 0.12, -fw / 2 - 0.1, fw / 2 + 0.1, SKIP_NX);
      lbox(paint, &F, -0.02, 0.16, fh2 - 0.1, fh2, -fw / 2 - 0.15, fw / 2 + 0.15, SKIP_NX);
      lbox(paint, &F, -0.02, 0.1, fh2 - 0.2, fh2 - 0.1, -fw / 2 - 0.05, fw / 2 + 0.05, SKIP_NX);
      // rising stepped bars, tallest (and most projecting) in the middle
      const int nb = 7;
      for (int i = 0; i < nb; i++) {
        double t = i - (nb - 1) / 2.0, hh = (fh2 - 0.2) * (0.95 - fabs(t) * 0.13);
        lbox(paint, &F, -0.02, 0.2 - fabs(t) * 0.025, 0.12, hh, t * 0.24 - 0.08, t * 0.24 + 0.08, SKIP_NX);
        lbox(paint, &F, -0.02, 0.24 - fabs(t) * 0.025, hh - 0.12, hh, t * 0.24 - 0.1, t * 0.24 + 0.1, SKIP_NX);
      }
      // half sunburst either side
      for (int si = 0; si < 2; si++) {
        double s = si ? 1 : -1;
        V3 c = { x + 0.03, yb + 0.12, zc + s * (nb * 0.24 / 2 + 0.6) };
        cyl_arc(paint, v3(c.x - 0.05, c.y, c.z), v3(c.x + 0.13, c.y, c.z), 0.34, 12, v3(0, 1, 0), -PI_D / 2, PI_D / 2, true);
        for (int k = 0; k < 5; k++) {
          double a = (k / 4.0 - 0.5) * PI_D * 0.9;
          V3 d = { 0, cos(a), sin(a) };
          Frame RF = { c, { 1, 0, 0 }, d, cross(v3(1, 0, 0), d) };
          lbox(paint, &RF, -0.05, 0.12, 0.4, fmin(0.9, fh2 - 0.3), -0.065, 0.065, SKIP_NX);
        }
      }
    }
  }
  // fluted pilasters flanking the entrance
  if (!entranceInBay && !entranceOnTower && R() < 0.6) {
    buf_color(paint, is_white(body) ? trim : C_WHITE);
    for (int si = 0; si < 2; si++) {
      double pz = zc + (si ? 1 : -1) * (doorW / 2 + 0.3);
      box(paint, fx - 0.05, fx + 0.1, G, pilasterTop, pz - 0.2, pz + 0.2, SKIP_NX);
      for (int k = 0; k < 4; k++) {
        double z = pz - 0.15 + k * 0.1;
        cyl_arc(paint, v3(fx + 0.1, G, z), v3(fx + 0.1, pilasterTop, z), 0.045, 5, v3(1, 0, 0), -PI_D / 2, PI_D / 2, false);
      }
    }
  }

  if (detail && S->porch) build_porch(S, B, ctx, rnd, doorW, zc);
  if (detail && (S->canopy == CN_AWNING || S->patio == PT_AWNING)) {
    uint32_t acol = S->awningColor.has ? S->awningColor.v : PICK(rnd, AWNING_COLS);
    bool striped = R() < 0.65;
    bool deep = S->patio == PT_AWNING;
    for (int i = 0; i < nstore; i++)
      awning(&B->fabric, &B->wire, fx, storeAll[i], 3.0, G + storeSill + storeH + (deep ? 0.55 : 0.35), deep ? 2.4 : 1.3, acol, striped, rnd, deep ? 0.8 : 0.62);
  }
  vec_free(&h.acUnits);
  vec_free(&front.holes);
#undef R
}

// ---- materials ---------------------------------------------------------------------------------
static Material *paint_material(void) {
  // Smooth painted stucco: broad soft albedo patches, a normal map carrying only fine
  // sand-finish grain (the broad trowel undulation is procedural in the shader).
  NoiseColorOpts co = noise_color_defaults();
  co.size = 512; co.seed = 91; co.base_cells = 3; co.speckle = 0.004; co.contrast = 1.0;
  co.colorA[0] = 242; co.colorA[1] = 240; co.colorA[2] = 236;
  co.colorB[0] = 255; co.colorB[1] = 255; co.colorB[2] = 255;
  Texture *map = noise_color_texture(&co);
  map->repeat = v2(1 / 7.0, 1 / 7.0);
  NoiseNormalOpts no = { 512, 17, 48, 0.11, 3 };
  Texture *normalMap = noise_normal_texture(&no);
  normalMap->repeat = v2(1.0, 1.0);
  MatDesc d = md_standard();
  d.name = "hotel paint";
  d.vertex_colors = true;
  d.map = map;
  d.normal_map = normalMap;
  d.normal_scale = v2(0.32, 0.32);
  d.roughness = 0.9;
  d.prog[MV_PLAIN] = PROG_HOTEL_PAINT;
  return mat_three(&d);
}

static Material *glass_material(void) {
  MatDesc d = md_standard();
  d.name = "hotel glass";
  d.color = color_hex(0x0a0c0f);
  d.roughness = 0.08;
  d.metalness = 0.0;
  d.env_map_intensity = 0;
  d.prog[MV_INSTANCED] = PROG_HOTEL_GLASS;
  return mat_three(&d);
}

static Texture *put_texture(const uint8_t *img, int S, bool srgb, Wrap wrap) {
  Canvas *cv = canvas_new(S, S);
  cv_put_image_data(cv, img, 0, 0, S, S);
  Texture *t = canvas_texture(cv, srgb, wrap, 1);
  canvas_free(cv);
  return t;
}

static Material *fabric_material(void) {
  const int S = 128;
  uint8_t *img = xcalloc((size_t)S * S, 4);
  for (int y = 0; y < S; y++)
    for (int x = 0; x < S; x++) {
      double u = (x + 0.5) / S, v = 1 - (y + 0.5) / S;
      int stripe = u < 0.5 ? 255 : 0;
      int a = 255;
      if (v < 0.25) {
        double vv = v / 0.25, x2 = fmod(u * 2, 1) * 2 - 1;
        a = vv > 0.42 * (1 - sqrt(fmax(0, 1 - x2 * x2))) + 0.04 ? 255 : 0;
      }
      int i = (y * S + x) * 4;
      img[i] = img[i + 1] = img[i + 2] = (uint8_t)stripe;
      img[i + 3] = (uint8_t)a;
    }
  Texture *map = put_texture(img, S, false, WRAP_REPEAT);   // (no colour space: data)
  free(img);
  map->sampler.mag = FILTER_NEAREST;
  MatDesc d = md_standard();
  d.name = "hotel fabric";
  d.vertex_colors = true;
  d.map = map;
  d.roughness = 0.85;
  d.side = SIDE_DOUBLE;
  d.alpha_test = 0.5;
  d.prog[MV_PLAIN] = PROG_HOTEL_FABRIC;
  return mat_three(&d);
}

typedef struct GrimeCol { double x0, x1, a, L; } GrimeCol;
static Material *grime_material(void) {
  const int S = 256;
  uint8_t *img = xcalloc((size_t)S * S, 4);
  Rng rnd = rng_make(606);
  for (int half = 0; half < 2; half++) {
    Vec(GrimeCol) cols = {};
    double x = 0;
    while (x < S) {
      double w = half ? 6 + rng_next(&rnd) * 18 : 2 + rng_next(&rnd) * 7;
      double a = rng_next(&rnd) < 0.35 ? 0 : 0.35 + rng_next(&rnd) * 0.65;
      double L = 0.3 + rng_next(&rnd) * 0.7;
      vec_push(&cols, ((GrimeCol){ x, x + w, a, L }));
      x += w;
    }
    for (int y = 0; y < S / 2; y++) {
      double t = y / (S / 2.0);
      for (int xx = 0; xx < S; xx++) {
        const GrimeCol *col = nullptr;
        for (size_t k = 0; k < cols.len && !col; k++) if (xx < cols.data[k].x1) col = &cols.data[k];
        double e = fmin(xx - col->x0, col->x1 - xx) / fmax(1, (col->x1 - col->x0) * 0.5);
        double edge = fmin(1, e * 1.5);
        double fall = t < col->L ? pow(1 - t / col->L, half ? 0.7 : 1.1) : 0;
        double a = col->a * edge * fall * (0.8 + 0.2 * rng_next(&rnd));
        int i = ((y + half * S / 2) * S + xx) * 4;
        img[i] = img[i + 1] = img[i + 2] = 255;
        img[i + 3] = js_u8clamp(js_round(255 * fmin(1, a)));
      }
    }
    vec_free(&cols);
  }
  Texture *map = put_texture(img, S, true, WRAP_CLAMP);
  free(img);
  MatDesc d = md_standard();
  d.name = "hotel grime";
  d.color = color_hex(0x463a2e);
  d.map = map;
  d.vertex_colors = true;
  d.transparent = true;
  d.depth_write = false;
  d.roughness = 0.95;
  d.polygon_offset = true;
  d.po_factor = -2;
  d.po_units = -2;
  d.prog[MV_PLAIN] = PROG_HOTEL_GRIME;
  return mat_three(&d);
}

static Material *terrazzo_material(void) {
  const int size = 512;
  Canvas *c = canvas_new(size, size);
  cv_fill_color(c, "#e6dfd2");
  cv_fill_rect(c, 0, 0, size, size);
  Rng rnd = rng_make(55);
  static const char *const chips[] = { "#b9b2a6", "#8f8a82", "#d7a9a2", "#9fc1b3", "#f3efe8", "#6f6a63", "#c9b48f" };
  for (int i = 0; i < 9000; i++) {
    cv_fill_color(c, chips[(int)floor(rng_next(&rnd) * 7)]);
    double r1 = rng_next(&rnd), r2 = rng_next(&rnd);
    double r = 0.6 + r1 * r2 * 3.2;
    cv_begin_path(c);
    double ex = rng_next(&rnd) * size, ey = rng_next(&rnd) * size;
    double ry = r * (0.6 + rng_next(&rnd) * 0.4), rot = rng_next(&rnd) * 3;
    cv_ellipse(c, ex, ey, r, ry, rot, 0, PI_D * 2, false);
    cv_fill(c);
  }
  // brass divider strips
  cv_stroke_color(c, "rgba(170,135,70,0.8)");
  cv_line_width(c, 2);
  cv_stroke_rect(c, 1, 1, size - 2, size - 2);
  Texture *map = canvas_texture(c, true, WRAP_REPEAT, 8);
  canvas_free(c);
  map->repeat = v2(1 / 1.2, 1 / 1.2);
  MatDesc d = md_standard();
  d.name = "hotel terrazzo";
  d.map = map;
  d.roughness = 0.4;
  d.prog[MV_PLAIN] = PROG_STD_MAP;
  return mat_three(&d);
}

static Material *glass_block_material(void) {
  const int s = 256, n = 4;
  Canvas *c = canvas_new(s, s);
  cv_fill_color(c, "#d9dcd8");
  cv_fill_rect(c, 0, 0, s, s);
  double cell = (double)s / n;
  for (int i = 0; i < n; i++)
    for (int j = 0; j < n; j++) {
      Gradient g = cv_radial_gradient(c, i * cell + cell * 0.45, j * cell + cell * 0.4, 2, i * cell + cell / 2, j * cell + cell / 2, cell * 0.6);
      grad_add_stop(&g, 0, "#dfeaec");
      grad_add_stop(&g, 0.6, "#a9bcc2");
      grad_add_stop(&g, 1, "#8aa0a8");
      cv_fill_gradient(c, &g);
      cv_fill_rect(c, i * cell + 4, j * cell + 4, cell - 8, cell - 8);
      cv_stroke_color(c, "rgba(255,255,255,0.35)");
      cv_line_width(c, 2);
      for (int k = 0; k < 3; k++) {
        cv_begin_path(c);
        cv_move_to(c, i * cell + 10, j * cell + 14 + k * 14);
        cv_bezier_to(c, i * cell + 24, j * cell + 8 + k * 14, i * cell + 40, j * cell + 22 + k * 14, i * cell + cell - 10, j * cell + 14 + k * 14);
        cv_stroke(c);
      }
    }
  Texture *map = canvas_texture(c, true, WRAP_REPEAT, 1);
  canvas_free(c);
  map->repeat = v2(1 / 0.8, 1 / 0.8);
  MatDesc d = md_standard();
  d.name = "hotel glass block";
  d.map = map;
  d.roughness = 0.25;
  d.prog[MV_PLAIN] = PROG_STD_MAP;
  return mat_three(&d);
}

static Texture *g_leaf_tex;
static Texture *leaf_texture(void) {
  if (g_leaf_tex) return g_leaf_tex;
  Canvas *c = canvas_new(128, 128);
  Rng rnd = rng_make(515);
  cv_fill_color(c, "#2c3a22");
  cv_fill_rect(c, 0, 0, 8, 8);   // opaque core swatch
  for (int i = 0; i < 26; i++) {
    double x = 18 + rng_next(&rnd) * 96, y = 18 + rng_next(&rnd) * 96, a = rng_next(&rnd) * PI_D, l = 10 + rng_next(&rnd) * 9;
    double v = 150 + rng_next(&rnd) * 105;
    char css[64];
    snprintf(css, sizeof css, "rgb(%d,%d,%d)", js_i32(v * 0.82), js_i32(v), js_i32(v * 0.7));
    cv_fill_color(c, css);
    cv_begin_path(c);
    cv_ellipse(c, x, y, l, l * 0.42, a, 0, PI_D * 2, false);
    cv_fill(c);
    cv_stroke_color(c, "rgba(40,50,30,0.5)");
    cv_line_width(c, 1);
    cv_begin_path(c);
    cv_move_to(c, x - cos(a) * l, y - sin(a) * l);
    cv_line_to(c, x + cos(a) * l, y + sin(a) * l);
    cv_stroke(c);
  }
  g_leaf_tex = canvas_texture(c, true, WRAP_CLAMP, 1);
  canvas_free(c);
  return g_leaf_tex;
}

// ---- instanced furniture geometry -------------------------------------------------------------
typedef Vec(Geometry *) GeoList;
static void add_box(GeoList *parts, double w, double h, double d, double x, double y, double z) {
  Geometry *g = geo_box1(w, h, d);
  geo_translate(g, x, y, z);
  vec_push(parts, g);
}
static Geometry *merge_non_indexed(GeoList *parts) {
  for (size_t i = 0; i < parts->len; i++)
    if (parts->data[i]->index) {
      Geometry *n = geo_to_non_indexed(parts->data[i]);
      geo_free(parts->data[i]);
      parts->data[i] = n;
    }
  Geometry *m = geo_merge_free(parts->data, (int)parts->len);
  vec_free(parts);
  return m;
}

static Geometry *chair_geometry(void) {
  GeoList parts = {};
  // bistro chair: slatted seat in a frame, two back uprights with three curved-back slats,
  // splayed legs
  add_box(&parts, 0.42, 0.025, 0.03, 0, 0.45, 0.195); add_box(&parts, 0.42, 0.025, 0.03, 0, 0.45, -0.195);
  add_box(&parts, 0.03, 0.025, 0.42, 0.195, 0.45, 0); add_box(&parts, 0.03, 0.025, 0.42, -0.195, 0.45, 0);
  for (int i = 0; i < 5; i++) add_box(&parts, 0.36, 0.018, 0.06, 0, 0.455, -0.15 + i * 0.075);
  const double ux[2] = { -0.19, 0.19 };
  for (int k = 0; k < 2; k++) add_box(&parts, 0.028, 0.46, 0.028, ux[k], 0.69, -0.21);
  for (int i = 0; i < 3; i++) {
    double y = 0.64 + i * 0.1;
    for (int k = 0; k < 4; k++) add_box(&parts, 0.1, 0.05, 0.02, -0.15 + k * 0.1, y, -0.215 - 0.018 * cos((k - 1.5) * 0.7));
  }
  const double legs[4][2] = { { -0.19, -0.19 }, { 0.19, -0.19 }, { -0.19, 0.19 }, { 0.19, 0.19 } };
  for (int k = 0; k < 4; k++) {
    double x = legs[k][0], z = legs[k][1];
    Geometry *g = geo_cyl(0.012, 0.014, 0.46, 5);
    geo_translate(geo_rotate_x(geo_rotate_z(g, -x * 0.25), z * 0.25), x * 1.06, 0.225, z * 1.06);
    vec_push(&parts, geo_to_non_indexed(g));
    geo_free(g);
  }
  return merge_non_indexed(&parts);
}

static Geometry *table_geometry(void) {
  Geometry *top = geo_translate(geo_box1(0.7, 0.03, 0.7), 0, 0.745, 0);
  Geometry *pole = geo_translate(geo_cyl(0.03, 0.03, 0.72, 6), 0, 0.37, 0);
  Geometry *base = geo_translate(geo_cyl(0.22, 0.25, 0.03, 12), 0, 0.015, 0);
  GeoList parts = {};
  vec_push(&parts, top);
  vec_push(&parts, pole);
  vec_push(&parts, base);
  return merge_non_indexed(&parts);
}

// Square market umbrella (half-size 1 before scaling): four panels on diagonal ribs, the cloth
// sagging between the ribs, and a straight valance all round.
static Geometry *umbrella_geometry(void) {
  const int n = 12;
  Geometry *g = geo_rotate_x(geo_plane(2, 2, n, n), -PI_D / 2);
  float *p = geo_data(g, "position");
  for (int i = 0; i < g->count; i++) {
    double x = p[i * 3], z = p[i * 3 + 2];
    double r = fmax(fabs(x), fabs(z));
    double q = r > 1e-4 ? fmin(fabs(x), fabs(z)) / r : 1;
    p[i * 3 + 1] = (float)(0.34 * (1 - r) - 0.07 * r * (1 - q));
  }
  GeoList parts = {};
  vec_push(&parts, geo_to_non_indexed(g));
  geo_free(g);
  for (int s = 0; s < 4; s++) {
    Geometry *v = geo_rotate_y(geo_translate(geo_plane(2, 0.2, 1, 1), 0, -0.1, 1), (s * PI_D) / 2);
    vec_push(&parts, geo_to_non_indexed(v));
    geo_free(v);
  }
  Geometry *m = geo_merge_free(parts.data, (int)parts.len);
  vec_free(&parts);
  geo_compute_vertex_normals(m);
  return m;
}

// Leafy shrub: a dark, lumpy core (the shade inside the bush) wrapped in ~90 alpha leaf-spray
// cards facing outward, lit with radial normals so the clump reads round but ragged.
static double lobe(V3 d) {
  return 0.86 + 0.12 * sin(d.x * 5.1 + 1.3) * sin(d.z * 4.3 + 0.7) + 0.08 * sin(d.y * 6.0 + d.x * 3.0);
}
static Geometry *shrub_geometry(void) {
  Rng rnd = rng_make(8181);
  Geometry *ico = geo_icosahedron(0.4, 2);
  geo_delete_attr(ico, "normal");
  geo_delete_attr(ico, "uv");
  Geometry *core = geo_merge_vertices(ico, 1e-4);
  geo_free(ico);
  float *cp = geo_data(core, "position");
  for (int i = 0; i < core->count; i++) {
    V3 v = v3_norm(v3(cp[i * 3], cp[i * 3 + 1], cp[i * 3 + 2]));
    double r = 0.4 * lobe(v) * (0.9 + 0.12 * rng_next(&rnd));
    cp[i * 3] = (float)(v.x * r);
    cp[i * 3 + 1] = (float)(v.y * r * (v.y < -0.3 ? 0.7 : 0.92));
    cp[i * 3 + 2] = (float)(v.z * r);
  }
  geo_compute_vertex_normals(core);
  Geometry *cn = core->index ? geo_to_non_indexed(core) : geo_clone(core);
  geo_free(core);
  float *cuv = geo_set_attr(cn, "uv", 2, cn->count);
  for (int i = 0; i < cn->count * 2; i++) cuv[i] = 0.02f;
  DVec pos = {}, nor = {}, uv = {};
  for (int k = 0; k < 90; k++) {
    double nx = rng_next(&rnd) * 2 - 1, ny = rng_next(&rnd) * 1.6 - 0.5, nz = rng_next(&rnd) * 2 - 1;
    V3 n = v3_norm(v3(nx, ny, nz));
    double r = 0.5 * lobe(n) * (0.82 + 0.28 * rng_next(&rnd));
    V3 c = v3_scale(n, r);
    c.y *= n.y < -0.3 ? 0.75 : 0.92;
    // card roughly tangent to the surface, tilted at random
    V3 t = v3_norm(v3_cross(n, fabs(n.y) > 0.9 ? v3(1, 0, 0) : v3(0, 1, 0)));
    V3 b = v3_cross(n, t);
    double a = rng_next(&rnd) * 6.28, tilt = (rng_next(&rnd) - 0.5) * 1.2;
    V3 u = v3_scale(t, cos(a));
    u = v3(u.x + b.x * sin(a), u.y + b.y * sin(a), u.z + b.z * sin(a));
    V3 w = v3_apply_quat(v3_norm(v3_cross(n, u)), quat_axis_angle(u, tilt));
    double sz = 0.2 + rng_next(&rnd) * 0.12;
    static const double Q[6][4] = { { -1, -1, 0, 0 }, { 1, -1, 1, 0 }, { 1, 1, 1, 1 }, { -1, -1, 0, 0 }, { 1, 1, 1, 1 }, { -1, 1, 0, 1 } };
    for (int q = 0; q < 6; q++) {
      double qx = Q[q][0], qy = Q[q][1], qu = Q[q][2], qv = Q[q][3];
      double su = qx * sz * 0.5, sv = qy * sz * 0.5;
      V3 Pp = v3(c.x + u.x * su, c.y + u.y * su, c.z + u.z * su);
      Pp = v3(Pp.x + w.x * sv, Pp.y + w.y * sv, Pp.z + w.z * sv);
      double pp[3] = { Pp.x, Pp.y, Pp.z };
      vec_append(&pos, pp, 3);
      V3 N = v3_norm(Pp);
      N = v3(N.x + (n.x - N.x) * 0.4, N.y + (n.y - N.y) * 0.4, N.z + (n.z - N.z) * 0.4);
      N = v3_norm(N);
      double nn[3] = { N.x, N.y, N.z };
      vec_append(&nor, nn, 3);
      double uu[2] = { 0.12 + qu * 0.86, 0.12 + qv * 0.86 };
      vec_append(&uv, uu, 2);
    }
  }
  Geometry *cards = geo_new();
  geo_set_attr_d(cards, "position", 3, (int)(pos.len / 3), pos.data);
  geo_set_attr_d(cards, "normal", 3, (int)(nor.len / 3), nor.data);
  geo_set_attr_d(cards, "uv", 2, (int)(uv.len / 2), uv.data);
  vec_free(&pos); vec_free(&nor); vec_free(&uv);
  Geometry *list[2] = { cn, cards };
  return geo_merge_free(list, 2);
}

// ---- street plan -------------------------------------------------------------------------------
// The authored block z in [-70, 70], then simpler buildings beyond.
typedef Vec(Spec) SpecVec;
static SpecVec street_plan(void) {
  Rng rng = rng_make(20260927);
  Rng *rnd = &rng;
#define R() rng_next(rnd)
  const Scheme *S = SCHEMES;
  // about half white / cream with strong pastel trim and bands, half pale pastel with white bands
  static const Scheme s0 = { C_WARMWHITE, C_TEAL, C_ROSE }, s1 = { C_MINT, C_WHITE, C_TEAL }, s2 = { C_WHITE, C_AQUA, C_PINKDEEP },
                      s3 = { C_PINK, C_WHITE, C_CORAL }, s4 = { C_WARMWHITE, C_MINTDEEP, C_TEAL }, s5 = { C_LAVENDER, C_WHITE, C_LILAC },
                      s6 = { C_WHITE, C_SKY, C_CORAL }, s7 = { C_LEMON, C_WHITE, C_AQUA };
  (void)S;
  static const Band b0 = { 0xb3dcc6, 1 }, bW2 = { C_WHITE, 2 }, b2 = { 0xb4dedb, 1 }, b4 = { 0x9ee6ec, 1 }, b6 = { 0xb5d0e6, 1 };
  const struct { double a, b; Over o; } plan[8] = {
    { -70, -52.5, { .floors = 2, .style = ST_BAND, .r0 = SOME(3.2), .r1 = SOME(0), .scheme = &s0, .band = &b0, .winLayout = WL_TRIPLE,
                    .name = "THE CORALINE", .canopy = CN_ENTRANCE, .patio = PT_AWNING, .awningColor = SOME(0x2e7fa8), .porch = B_TRUE,
                    .portholes = B_TRUE, .medallions = B_TRUE, .roof = RF_AC, .has_exposed = true, .exposed = { true, false } } },
    { -50.8, -35.6, { .floors = 4, .style = ST_FIN, .r0 = SOME(0), .r1 = SOME(0), .scheme = &s1, .band = &bW2, .winLayout = WL_PAIR,
                      .eyebrow = EB_WINDOW, .name = "SEAGROVE", .canopy = CN_FULL, .patio = PT_UMBRELLA, .umbrellaCol = SOME(0xd9477a),
                      .portholes = B_TRUE, .roof = RF_TANK } },
    { -33.8, -15.2, { .floors = 5, .style = ST_ZIGGURAT, .r0 = SOME(0), .r1 = SOME(0), .scheme = &s2, .band = &b2, .winLayout = WL_RIBBON,
                      .eyebrow = EB_FULL, .name = "BELLA MAR", .canopy = CN_FULL, .patio = PT_TENT, .setback = SOME(0.8), .eyeCol = EC_TRIM,
                      .fountain = B_TRUE, .roof = RF_TANK } },
    { -13.6, 2.2, { .floors = 3, .style = ST_PYLON, .r0 = SOME(0), .r1 = SOME(0), .scheme = &s3, .band = &bW2, .winLayout = WL_PUNCHED,
                    .eyebrow = EB_WINDOW, .name = "ORCHIDEA", .canopy = CN_ENTRANCE, .patio = PT_UMBRELLA, .umbrellaCol = SOME(0x3f8a5a),
                    .porch = B_TRUE, .setback = SOME(0), .portholes = B_TRUE, .fountain = B_TRUE, .medallions = B_TRUE,
                    .parapetStep = B_TRUE, .finial = B_TRUE } },
    { 3.8, 24.6, { .floors = 4, .style = ST_TWIN, .r0 = SOME(1.8), .r1 = SOME(1.8), .scheme = &s4, .band = &b4, .winLayout = WL_TRIPLE,
                   .eyebrow = EB_WINDOW, .name = "MARISOL", .signTop = SOME(10.4), .signBottom = SOME(5.4), .canopy = CN_ENTRANCE,
                   .patio = PT_CANOPY, .canopyCol = SOME(0x3d9ad6), .canopyAlt = SOME(0xe98fae), .setback = SOME(0.4),
                   .portholes = B_FALSE, .fountain = B_TRUE, .medallions = B_TRUE, .roof = RF_AC, .parapetStep = B_TRUE } },
    { 26.4, 40.2, { .floors = 3, .style = ST_CORNER, .r0 = SOME(0), .r1 = SOME(3.2), .scheme = &s5, .band = &bW2, .winLayout = WL_RIBBON,
                    .eyebrow = EB_FULL, .name = "ORIANA", .canopy = CN_FULL, .patio = PT_PORCH, .noSidewalk = true, .portholes = B_TRUE,
                    .parapetStep = B_TRUE, .rail = RL_PIPE } },
    { 41.8, 56.0, { .floors = 7, .style = ST_TOWER, .r0 = SOME(0), .r1 = SOME(0), .scheme = &s6, .band = &b6, .winLayout = WL_TRIPLE,
                    .eyebrow = EB_WINDOW, .name = "MARINELLA", .canopy = CN_ENTRANCE, .patio = PT_UMBRELLA, .umbrellaCol = SOME(0xf1efe9),
                    .setback = SOME(1.2), .roof = RF_TANK } },
    { 57.6, 70, { .floors = 2, .style = ST_PLAIN, .r0 = SOME(0), .r1 = SOME(3.0), .scheme = &s7, .band = &bW2, .winLayout = WL_PAIR,
                  .eyebrow = EB_FULL, .name = "SOLANA", .canopy = CN_FULL, .patio = PT_AWNING, .awningColor = SOME(0x3d7d4e),
                  .parapetStep = B_TRUE, .roof = RF_SIGN, .fins = B_TRUE, .has_exposed = true, .exposed = { false, true } } },
  };
  SpecVec specs = {};
  for (int i = 0; i < 8; i++) {
    Over o = plan[i].o;   // { porch: true, ...o, detail: 1 }
    if (!o.porch) o.porch = B_TRUE;
    o.detail = (OptD)SOME(1);
    vec_push(&specs, make_spec(rnd, plan[i].a, plan[i].b, &o));
  }
  // names = [...NAMES.slice(8), ...MORE_NAMES]
  const char *names[ARRAY_LEN(NAMES) - 8 + ARRAY_LEN(MORE_NAMES)];
  int nnames = 0;
  for (size_t i = 8; i < ARRAY_LEN(NAMES); i++) names[nnames++] = NAMES[i];
  for (size_t i = 0; i < ARRAY_LEN(MORE_NAMES); i++) names[nnames++] = MORE_NAMES[i];
  int nameI = 0;
  // continuing blocks between the cross streets: full detail through the walkable district,
  // the simpler far row beyond it
  for (int di = 0; di < 2; di++) {
    double dir = di ? -1 : 1;
    double ends[NCROSS_STREETS + 1];
    int ne = 0;
    for (int i = 0; i < NCROSS_STREETS; i++) if (CROSS_STREETS[i].z * dir > 0) ends[ne++] = fabs(CROSS_STREETS[i].z);
    sort_d(ends, ne);
    ends[ne++] = 960;
    for (int k = 0; k < ne - 1; k++) {
      double blockStart = ends[k] + CROSS.gap, blockLen = ends[k + 1] - CROSS.gap - blockStart;
      double z = 0;
      while (z < blockLen - 8) {
        double w = fmin(12 + R() * 13, blockLen - z);
        if (blockLen - (z + w) < 10) w = blockLen - z;   // the corner building runs to the cross street
        bool first = z == 0, last = z + w >= blockLen - 0.01;
        double za = dir > 0 ? blockStart + z : -(blockStart + z + w);
        double zb = za + w;
        bool near = fabs((za + zb) / 2) < DISTRICT.zMax + 5;
        Over o = {};
        if (dir > 0) {
          if (first) o.r0 = (OptD)SOME(2.5 + R());
          if (last) o.r1 = (OptD)SOME(2.5 + R());
        } else {
          if (first) o.r1 = (OptD)SOME(2.5 + R());
          if (last) o.r0 = (OptD)SOME(2.5 + R());
        }
        o.detail = (OptD)SOME(near ? 1 : 0);
        o.name = near || R() < 0.5 ? names[nameI++ % nnames] : nullptr;
        o.has_exposed = true;
        o.exposed[0] = dir > 0 ? first : last;
        o.exposed[1] = dir > 0 ? last : first;
        vec_push(&specs, make_spec(rnd, za, zb, &o));
        z += w + (R() < 0.5 ? 1.2 + R() * 2.5 : 0.4);
      }
    }
  }
  return specs;
#undef R
}

// ---- instancing ---------------------------------------------------------------------------------
typedef struct Rec { double z; M4 m; bool has_color; Color color; float a[4]; } Rec;
typedef Vec(Rec) RecVec;

typedef struct Build {
  Chunk chunks[NCROSS_STREETS + 1];
  int nchunks;
} Build;

static int chunk_of(const Build *b, double z) {
  for (int i = 0; i < b->nchunks; i++) if (z < b->chunks[i].max) return i;
  return -1;
}

typedef enum Where { W_BASE, W_DETAIL } Where;
// per-chunk instanced mesh from a list of records. aWin: the glass, whose per-instance
// attribute needs its own geometry per chunk (a clone of `geo`); else all chunks share `gpu`.
static void per_chunk(Build *b, Geometry *geo, GpuGeometry *gpu, Material *mat, const RecVec *recs, Where where, bool cast,
                      bool receive, bool aWin, const char *name) {
  for (int ci = 0; ci < b->nchunks; ci++) {
    int n = 0;
    for (size_t i = 0; i < recs->len; i++) n += chunk_of(b, recs->data[i].z) == ci;
    if (!n) continue;
    Node *mesh = node_instanced(nullptr, mat, n);
    snprintf(mesh->name, sizeof mesh->name, "%s", name);
    float *attr = aWin ? xcalloc((size_t)n * 4, sizeof(float)) : nullptr;
    int k = 0;
    for (size_t i = 0; i < recs->len; i++) {
      const Rec *r = &recs->data[i];
      if (chunk_of(b, r->z) != ci) continue;
      inst_set_matrix(mesh, k, r->m);
      if (r->has_color) inst_set_color(mesh, k, r->color);
      if (attr) memcpy(attr + k * 4, r->a, sizeof r->a);
      k++;
    }
    if (aWin) {
      Geometry *g = geo_clone(geo);
      geo_set_iattr(g, "aWin", 4, n, attr);
      mesh->geo = gpu_geometry(g);
      geo_free(g);
      free(attr);
    } else {
      mesh->geo = gpu;
    }
    mesh->cast_shadow = cast;
    mesh->receive_shadow = receive;
    node_add(where == W_BASE ? b->chunks[ci].base : b->chunks[ci].detail, mesh);
  }
}

// setM(w, depthOffset, sx, sy, sz, u)
static M4 set_m(const Win *w, double depthOffset, double sx, double sy, double sz, double u) {
  V3 Z = cross(w->N, UP);
  V3 vX = scl(w->N, sx), vY = { 0, sy, 0 }, vZ = scl(Z, sz);
  V3 p = add(add(w->c, scl(w->N, depthOffset)), scl(Z, u));
  return (M4){ { vX.x, vX.y, vX.z, 0, vY.x, vY.y, vY.z, 0, vZ.x, vZ.y, vZ.z, 0, p.x, p.y, p.z, 1 } };
}

// place(): records composed from { x, y, z, rot } with a per-item scale
typedef V3 (*ScaleFn)(const Item *it, void *user);
static void place(Build *b, Geometry *geo, Material *mat, const ItemVec *items, ScaleFn fn, void *user, const char *name) {
  RecVec recs = {};
  for (size_t i = 0; i < items->len; i++) {
    const Item *it = &items->data[i];
    Quat q = quat_axis_angle(UP, it->rot);
    Rec r = { .z = it->z, .m = m4_compose(v3(it->x, it->y, it->z), q, fn ? fn(it, user) : v3(1, 1, 1)) };
    if (it->color != NONE_HEX) { r.has_color = true; r.color = color_hex(it->color); }
    vec_push(&recs, r);
  }
  per_chunk(b, nullptr, gpu_geometry(geo), mat, &recs, W_DETAIL, true, true, false, name);
  vec_free(&recs);
}
static V3 scale_umbrella(const Item *u, void *user) { (void)user; return v3(u->r, 1, u->r); }
static V3 scale_pole(const Item *u, void *user) { (void)u; return v3(1, *(const double *)user, 1); }
static V3 scale_shrub(const Item *s, void *user) { (void)user; return v3(s->s * 1.3, s->s * 1.1, s->s * 1.3); }
static V3 scale_palm(const Item *p, void *user) { (void)user; return v3(p->s * 0.8, p->s * 0.7, p->s * 0.8); }

// merged static mesh of one chunk's buffer
static Node *add_merged(Chunk *c, Buf *buf, Material *mat, bool cast, Where where, bool drop_color, bool is_grime, const char *name) {
  if (buf_empty(buf)) return nullptr;
  Geometry *g = buf_geometry(buf);
  if (drop_color) geo_delete_attr(g, "color");
  Node *mesh = node_mesh(gpu_geometry(g), mat);
  geo_free(g);
  snprintf(mesh->name, sizeof mesh->name, "%s", name);
  if (is_grime) {
    mesh->receive_shadow = true;
    mesh->render_order = 2;
  } else {
    mesh->cast_shadow = cast;
    mesh->receive_shadow = true;
  }
  node_add(where == W_BASE ? c->base : c->detail, mesh);
  return mesh;
}

// stable sort of the specs beyond the authored block by |z0 + z1|
static void sort_specs(Spec **v, int n) {
  for (int i = 1; i < n; i++) {
    Spec *x = v[i];
    int j = i - 1;
    while (j >= 0 && fabs(v[j]->z0 + v[j]->z1) - fabs(x->z0 + x->z1) > 0) { v[j + 1] = v[j]; j--; }
    v[j + 1] = x;
  }
}

Hotels build_hotels(Node *scene) {
  Node *group = node_new(NODE_GROUP, "hotels");
  SpecVec specs = street_plan();
  // sign atlases: the original block and its neighbouring blocks share the first; the outer
  // district and the far row use a second one
  SignAtlas atlases[2];
  atlas_init(&atlases[0]);
  atlas_init(&atlases[1]);
  // one chunk per block (split at the cross streets): merged and instanced meshes per chunk,
  // so frustum culling works per block and the small parts of far blocks can be dropped by
  // distance (lod.c)
  static Build bld;
  Build *b = &bld;
  double edges[NCROSS_STREETS];
  for (int i = 0; i < NCROSS_STREETS; i++) edges[i] = CROSS_STREETS[i].z;
  sort_d(edges, NCROSS_STREETS);
  b->nchunks = NCROSS_STREETS + 1;
  for (int i = 0; i < b->nchunks; i++) {
    Chunk *c = &b->chunks[i];
    c->max = i < NCROSS_STREETS ? edges[i] : INFINITY;
    c->min = i ? edges[i - 1] : -INFINITY;
    buf_init(&c->paint); buf_init(&c->metal); buf_init(&c->fabric); buf_init(&c->terrazzo);
    buf_init(&c->block); buf_init(&c->signs); buf_init(&c->grime); buf_init(&c->wire);
    c->base = node_new(NODE_GROUP, "hotels base");
    c->detail = node_new(NODE_GROUP, "hotels detail");
  }
#define ATLAS_OF(ci) (b->chunks[ci].min >= -200 && b->chunks[ci].max <= 200 ? 0 : 1)
  Ctx ctx = { .chairCol = 0x6b5a45 };
  Rng rndC = rng_make(99);
  // the authored block first, then outward, so the nearest buildings get their sign-atlas space
  // first
  int ns = (int)specs.len;
  Spec **order = xmalloc(sizeof(Spec *) * (size_t)ns);
  for (int i = 0; i < ns; i++) order[i] = &specs.data[i];
  sort_specs(order + 8, ns - 8);
  for (int i = 0; i < ns; i++) {
    const Spec *S = order[i];
    double zc = (S->z0 + S->z1) / 2;
    Chunk *B = &b->chunks[chunk_of(b, zc)];
    ctx.chairCol = PICK_U(&rndC, 0xc0343c, 0xe07a9a, 0xefece6, 0x3f8a5a, 0x2d6f9f, 0x6b5a45, 0xd9a13a, 0x2f8f7f, 0xefece6, 0xc0343c);
    // far LOD: lower tiers leave out the row beyond the haze distance (kept as footprints)
    if (fabs(zc) > QUALITY.hotelFar) continue;
    SignAtlas *atlas = &atlases[ATLAS_OF(chunk_of(b, zc))];
    atlas->scale = S->detail > 0 ? 1 : 0.6;   // the far row's letters need less resolution
    build_hotel(S, B, &ctx, atlas);
  }
  free(order);
  LOG("hotels: %d buildings, %d windows, %d chairs, %d umbrellas, sign atlas misses %d/%d", ns, (int)ctx.windows.len,
      (int)ctx.chairs.len, (int)ctx.umbrellas.len, atlases[0].miss, atlases[1].miss);

  // window frames, sills, glass-block infill -> merged; glass and reveals -> instanced
  Rng rng = rng_make(4711);
  Rng *rnd = &rng;
  typedef struct GlassIdx { const Win *w; double u, pw; bool has_it; double it[4]; } GlassIdx;
  Vec(GlassIdx) glassIdx = {};
  Vec(const Win *) revealRect = {}, revealRound = {};
  for (size_t wi = 0; wi < ctx.windows.len; wi++) {
    const Win *w = &ctx.windows.data[wi];
    Chunk *B = &b->chunks[chunk_of(b, w->c.z)];
    V3 Nv = w->N, Z = cross(Nv, UP);
    Frame F = { w->c, Nv, UP, Z };
    if (w->kind == WK_BLOCK) {
      set3(B->block.w, 0, 3, 0);
      double d = w->depth;
      quad(&B->block, P(&F, -d, -w->h / 2, -w->w / 2), P(&F, -d, w->h / 2, -w->w / 2), P(&F, -d, w->h / 2, w->w / 2), P(&F, -d, -w->h / 2, w->w / 2), Nv);
      if (w->round) vec_push(&revealRound, w);
      else vec_push(&revealRect, w);
      continue;
    }
    // one glass instance per pane, each with its own interior and reflection seed
    int np = w->panes ? w->panes : 1;
    for (int k = 0; k < np; k++) {
      double pw = w->w / np, u = -w->w / 2 + pw * (k + 0.5);
      GlassIdx g = { w, u, pw, w->has_interior, {} };
      if (k == 0 || w->kind != WK_WIN) memcpy(g.it, w->interior, sizeof g.it);
      else { interior_for(rnd, IK_WIN, g.it); g.has_it = true; }
      vec_push(&glassIdx, g);
    }
    if (w->round) vec_push(&revealRound, w);
    else vec_push(&revealRect, w);
    Buf *m = &B->metal;
    double t = w->kind == WK_DOOR ? 0.08 : w->kind == WK_STORE ? 0.07 : 0.05;
    double x0 = -w->depth - 0.015, x1 = -w->depth + 0.04;
    double hw = w->w / 2, hh = w->h / 2;
    uint32_t fc = w->kind == WK_DOOR || w->kind == WK_STORE ? (is_white(w->frame) || w->frame == 0x5f9d96 ? 0xbfc3c3 : w->frame) : w->frame;
    buf_color(m, fc);
    set3(B->paint.w, 100, 3, 0.3);
    set3(B->paint.e, 0, 0, 0);
    B->paint.b[0] = B->paint.b[1] = B->paint.b[2] = 1; B->paint.b[3] = 0;
    if (w->round) {
      // porthole: a fat projecting collar with a stepped inner ring, metal frame at the glass
      M4 basis = { { -Z.x, -Z.y, -Z.z, 0, 0, 1, 0, 0, Nv.x, Nv.y, Nv.z, 0, 0, 0, 0, 1 } };   // makeBasis(-Z, UP, N)
      Geometry *col = geo_torus(hw + 0.1, 0.1, 10, 36, TAU, 0, TAU);
      buf_color(&B->paint, w->collar);
      push_geometry(&B->paint, col, m4_set_position(basis, add(w->c, scl(Nv, 0.05))));
      geo_free(col);
      Geometry *ring = geo_torus(hw + 0.01, 0.04, 6, 32, TAU, 0, TAU);
      buf_color(&B->paint, lighten(w->collar, 0.4));
      push_geometry(&B->paint, ring, m4_set_position(basis, add(w->c, scl(Nv, 0.0))));
      geo_free(ring);
      Geometry *tor = geo_torus(hw - 0.03, 0.045, 6, 24, TAU, 0, TAU);
      push_geometry(m, tor, m4_set_position(basis, add(w->c, scl(Nv, -w->depth + 0.03))));
      geo_free(tor);
      lbox(m, &F, x0, x1, -0.02, 0.02, -hw, hw, 0);
      continue;
    }
    // projecting surround moulding round upper windows, in the trim / band colour
    if (w->kind == WK_WIN) {   // (w.surround is always set)
      double s = 0.09, o = 0.05;
      buf_color(&B->paint, w->surround);
      lbox(&B->paint, &F, -0.02, o, hh, hh + s, -hw - s, hw + s, SKIP_NX);
      lbox(&B->paint, &F, -0.02, o, -hh - s, hh, -hw - s, -hw, SKIP_NX);
      lbox(&B->paint, &F, -0.02, o, -hh - s, hh, hw, hw + s, SKIP_NX);
    }
    lbox(m, &F, x0, x1, hh - t, hh, -hw, hw, 0);
    lbox(m, &F, x0, x1, -hh, -hh + t, -hw, hw, 0);
    lbox(m, &F, x0, x1, -hh + t, hh - t, -hw, -hw + t, 0);
    lbox(m, &F, x0, x1, -hh + t, hh - t, hw - t, hw, 0);
    enum { STY_H3, STY_CROSS, STY_GRID, STY_PANES, STY_DOOR, STY_STORE, STY_PLAIN } style;
    if (w->kind == WK_DOOR) style = STY_DOOR;
    else if (w->kind == WK_STORE) style = STY_STORE;
    else {
      FrameStyle fs = w->frameStyle == FS_PLAIN && w->w > 1.4 ? FS_CROSS : w->frameStyle;
      style = fs == FS_CROSS ? STY_CROSS : fs == FS_GRID ? STY_GRID : fs == FS_PANES ? STY_PANES : fs == FS_PLAIN ? STY_PLAIN : STY_H3;
    }
    double bar = t * 0.6;
    if (style == STY_PANES) {
      for (int i = 1; i < np; i++)
        lbox(m, &F, x0, x1 + 0.01, -hh, hh, -hw + (w->w * i) / np - t * 0.55, -hw + (w->w * i) / np + t * 0.55, 0);
      if (w->h > 1.3) lbox(m, &F, x0, x1, hh - 0.36 - bar / 2, hh - 0.36 + bar / 2, -hw, hw, 0);
    } else if (style == STY_H3) {
      const double fs[2] = { 1 / 3.0, 2 / 3.0 };
      for (int k = 0; k < 2; k++) lbox(m, &F, x0, x1, -hh + w->h * fs[k] - bar / 2, -hh + w->h * fs[k] + bar / 2, -hw, hw, 0);
      if (w->w > 1.3) lbox(m, &F, x0, x1, -hh, hh, -bar / 2, bar / 2, 0);
    } else if (style == STY_CROSS) {
      lbox(m, &F, x0, x1, -hh, hh, -bar / 2, bar / 2, 0);
      lbox(m, &F, x0, x1, hh - w->h * 0.3 - bar / 2, hh - w->h * 0.3 + bar / 2, -hw, hw, 0);
    } else if (style == STY_GRID) {
      const double fs[2] = { 1 / 3.0, 2 / 3.0 };
      for (int k = 0; k < 2; k++) {
        lbox(m, &F, x0, x1, -hh, hh, -hw + w->w * fs[k] - bar / 2, -hw + w->w * fs[k] + bar / 2, 0);
        lbox(m, &F, x0, x1, -hh + w->h * fs[k] - bar / 2, -hh + w->h * fs[k] + bar / 2, -hw, hw, 0);
      }
    } else if (style == STY_DOOR) {
      lbox(m, &F, x0, x1 + 0.02, -hh, hh, -t / 2, t / 2, 0);
      lbox(m, &F, x0, x1, hh - 0.55, hh - 0.55 + t, -hw, hw, 0);
      lbox(m, &F, x1, x1 + 0.05, -0.1, -0.04, -hw + 0.15, -0.15, 0);
      lbox(m, &F, x1, x1 + 0.05, -0.1, -0.04, 0.15, hw - 0.15, 0);
    } else if (style == STY_STORE) {
      int n = (int)fmax(1, js_round(w->w / 1.3));
      for (int i = 1; i < n; i++) lbox(m, &F, x0, x1, -hh, hh, -hw + (w->w * i) / n - bar / 2, -hw + (w->w * i) / n + bar / 2, 0);
      lbox(m, &F, x0, x1, hh - 0.5, hh - 0.5 + bar, -hw, hw, 0);
    }
    // projecting sill under upper windows
    if (w->kind == WK_WIN && w->sill) {
      buf_color(&B->paint, w->surround);
      lbox(&B->paint, &F, -w->depth, 0.15, -hh - 0.1, -hh, -hw - 0.14, hw + 0.14, SKIP_NX);
    }
  }

  Material *glassMat = glass_material();
  Geometry *quadGeo = geo_rotate_y(geo_plane(1, 1, 1, 1), PI_D / 2);
  RecVec glassRecs = {};
  for (size_t i = 0; i < glassIdx.len; i++) {
    const GlassIdx *g = &glassIdx.data[i];
    const Win *w = g->w;
    Rec r = { .z = w->c.z, .m = set_m(w, -w->depth, 1, w->h, g->pw, g->u) };
    double it[4];
    if (g->has_it) memcpy(it, g->it, sizeof it);
    else { it[0] = 0; it[1] = 0; it[2] = rng_next(rnd); it[3] = rng_next(rnd); }
    // storefront / door kinds keep their type; x encodes 4 = round, +10 = ground-floor glass
    double ground = w->kind == WK_DOOR || w->kind == WK_STORE ? 10 : 0;
    r.a[0] = (float)((w->round ? 4 : it[0]) + ground);
    r.a[1] = (float)it[1];
    r.a[2] = (float)it[2];
    r.a[3] = (float)rng_next(rnd);
    vec_push(&glassRecs, r);
  }
  // (the pane only picked up shadow acne deep in its reveal: no shadows on the glass)
  per_chunk(b, quadGeo, nullptr, glassMat, &glassRecs, W_BASE, false, false, true, "hotel glass");
  vec_free(&glassRecs);

  // reveals: open boxes (rect) and open tubes (round), coloured like their wall
  Geometry *revealGeo;
  {
    Buf rb;
    buf_init(&rb);
    const Frame *F = &WF;
    // local x from 0 (wall plane) to -1 (glass); y, z in [-0.5, 0.5]; faces point inward
    quad(&rb, P(F, 0, 0.5, -0.5), P(F, -1, 0.5, -0.5), P(F, -1, 0.5, 0.5), P(F, 0, 0.5, 0.5), v3(0, -1, 0));
    quad(&rb, P(F, 0, -0.5, -0.5), P(F, -1, -0.5, -0.5), P(F, -1, -0.5, 0.5), P(F, 0, -0.5, 0.5), v3(0, 1, 0));
    quad(&rb, P(F, 0, -0.5, 0.5), P(F, -1, -0.5, 0.5), P(F, -1, 0.5, 0.5), P(F, 0, 0.5, 0.5), v3(0, 0, -1));
    quad(&rb, P(F, 0, -0.5, -0.5), P(F, -1, -0.5, -0.5), P(F, -1, 0.5, -0.5), P(F, 0, 0.5, -0.5), v3(0, 0, 1));
    revealGeo = buf_geometry(&rb);
    geo_delete_attr(revealGeo, "color");
    geo_delete_attr(revealGeo, "aW");
    geo_delete_attr(revealGeo, "aE");
    geo_delete_attr(revealGeo, "aB");
  }
  MatDesc rd = md_standard();
  rd.name = "hotel reveal";
  rd.roughness = 0.9;
  rd.prog[MV_INSTANCED] = PROG_HOTEL_REVEAL;
  Material *revealMat = mat_three(&rd);
  {
    RecVec recs = {};
    for (size_t i = 0; i < revealRect.len; i++) {
      const Win *w = revealRect.data[i];
      vec_push(&recs, ((Rec){ .z = w->c.z, .m = set_m(w, 0, w->depth, w->h, w->w, 0), .has_color = true, .color = color_scale(color_hex(w->reveal), 0.9) }));
    }
    per_chunk(b, nullptr, gpu_geometry(revealGeo), revealMat, &recs, W_BASE, true, true, false, "hotel reveals");
    vec_free(&recs);
  }
  if (revealRound.len) {
    Geometry *tube = geo_translate(geo_rotate_z(geo_cylinder(0.5, 0.5, 1, 20, 1, true, 0, TAU), PI_D / 2), -0.5, 0, 0);
    MatDesc rr = md_standard();
    rr.name = "hotel reveal round";
    rr.roughness = 0.9;
    rr.side = SIDE_BACK;
    rr.prog[MV_INSTANCED] = PROG_HOTEL_REVEAL_ROUND;
    Material *rMat = mat_three(&rr);
    RecVec recs = {};
    for (size_t i = 0; i < revealRound.len; i++) {
      const Win *w = revealRound.data[i];
      vec_push(&recs, ((Rec){ .z = w->c.z, .m = set_m(w, 0, w->depth, w->h, w->w, 0), .has_color = true, .color = color_scale(color_hex(w->reveal), 0.93) }));
    }
    per_chunk(b, nullptr, gpu_geometry(tube), rMat, &recs, W_BASE, true, true, false, "hotel round reveals");
    vec_free(&recs);
    geo_free(tube);
  }

  // merged static meshes per chunk
  Material *paintMat = paint_material();
  MatDesc md = md_standard();
  md.name = "hotel metal";
  md.vertex_colors = true;
  md.roughness = 0.42;
  md.metalness = 0.08;
  md.prog[MV_PLAIN] = PROG_STD_VCOL;
  Material *metalMat = mat_three(&md);
  Material *grimeMat = grime_material();
  Material *fabricMat = fabric_material();
  Material *terrMat = terrazzo_material();
  Material *blockMat = glass_block_material();
  // one sign material pair per atlas
  Material *signMat[2], *signShadowMat[2];
  for (int i = 0; i < 2; i++) {
    Texture *signMap, *signEm;
    atlas_textures(&atlases[i], &signMap, &signEm);
    MatDesc sd = md_standard();
    sd.name = "hotel sign";
    sd.map = signMap;
    sd.alpha_test = 0.5;
    sd.alpha_to_coverage = true;
    sd.roughness = 0.5;
    sd.metalness = 0.25;
    sd.emissive = color_hex(0xffffff);
    sd.emissive_map = signEm;
    sd.emissive_intensity = 0.08;
    sd.side = SIDE_DOUBLE;
    sd.prog[MV_PLAIN] = PROG_STD_MAP_ATEST_A2C_DBL_EMAP_TRANSP;
    signMat[i] = mat_three(&sd);
    // soft contact shadow of the channel letters on the wall just behind them
    MatDesc hd = md_basic();
    hd.name = "hotel sign shadow";
    hd.color = color_hex(0x000000);
    hd.map = signMap;
    hd.transparent = true;
    hd.opacity = 0.4;
    hd.depth_write = false;
    hd.prog[MV_PLAIN] = PROG_SIGN_SHADOW;
    signShadowMat[i] = mat_three(&hd);
  }
  for (int ci = 0; ci < b->nchunks; ci++) {
    Chunk *c = &b->chunks[ci];
    // base: the building shells, awnings, signs and glass (always drawn); detail: frames,
    // grime, wires, terrazzo, glass block and furniture (dropped with distance)
    add_merged(c, &c->paint, paintMat, true, W_BASE, false, false, "hotel paint");
    add_merged(c, &c->metal, metalMat, true, W_DETAIL, false, false, "hotel metal");
    add_merged(c, &c->fabric, fabricMat, true, W_BASE, false, false, "hotel fabric");
    add_merged(c, &c->terrazzo, terrMat, false, W_DETAIL, true, false, "hotel terrazzo");
    add_merged(c, &c->block, blockMat, false, W_DETAIL, true, false, "hotel glass block");
    if (!buf_empty(&c->signs)) {
      int a = ATLAS_OF(ci);
      Node *sm = add_merged(c, &c->signs, signMat[a], true, W_BASE, true, false, "hotel signs");
      Node *sh = node_mesh(sm->geo, signShadowMat[a]);
      snprintf(sh->name, sizeof sh->name, "hotel sign shadow");
      sh->render_order = 3;
      node_add(c->detail, sh);
    }
    add_merged(c, &c->grime, grimeMat, false, W_DETAIL, false, true, "hotel grime");
    add_merged(c, &c->wire, metalMat, false, W_DETAIL, false, false, "hotel wire");
  }

  // furniture
  MatDesc fd = md_standard();
  fd.name = "hotel furniture";
  fd.roughness = 0.6;
  fd.metalness = 0.2;
  fd.prog[MV_INSTANCED] = PROG_STD_INST_ICOL;
  Material *furnMat = mat_three(&fd);
  if (ctx.chairs.len) {
    Geometry *g = chair_geometry();
    place(b, g, furnMat, &ctx.chairs, nullptr, nullptr, "hotel chairs");
    geo_free(g);
  }
  if (ctx.tables.len) {
    for (size_t i = 0; i < ctx.tables.len; i++) ctx.tables.data[i].color = 0xe9e6df;
    Geometry *g = table_geometry();
    place(b, g, furnMat, &ctx.tables, nullptr, nullptr, "hotel tables");
    geo_free(g);
  }
  if (ctx.umbrellas.len) {
    MatDesc ud = md_standard();
    ud.name = "hotel umbrella";
    ud.roughness = 0.85;
    ud.side = SIDE_DOUBLE;
    ud.prog[MV_INSTANCED] = PROG_HOTEL_UMBRELLA;
    Material *umMat = mat_three(&ud);
    ItemVec ums = {};
    for (size_t i = 0; i < ctx.umbrellas.len; i++) {
      Item u = ctx.umbrellas.data[i];
      u.y = u.y + u.h - 0.3;
      u.rot = 0;
      vec_push(&ums, u);
    }
    Geometry *g = umbrella_geometry();
    place(b, g, umMat, &ums, scale_umbrella, nullptr, "hotel umbrellas");
    geo_free(g);
    vec_free(&ums);
    Geometry *pole = geo_translate(geo_cyl(0.022, 0.022, 1, 6), 0, 0.5, 0);
    ItemVec poles = {};
    for (size_t i = 0; i < ctx.umbrellas.len; i++) {
      const Item *u = &ctx.umbrellas.data[i];
      vec_push(&poles, ((Item){ .x = u->x, .y = u->y, .z = u->z, .color = 0xd8d4cc }));
    }
    double h0 = ctx.umbrellas.data[0].h;
    place(b, pole, furnMat, &poles, scale_pole, &h0, "hotel umbrella poles");
    geo_free(pole);
    vec_free(&poles);
  }
  if (ctx.shrubs.len) {
    MatDesc sd = md_standard();
    sd.name = "hotel shrubs";
    sd.roughness = 0.85;
    sd.map = leaf_texture();
    sd.alpha_test = 0.5;
    sd.side = SIDE_DOUBLE;
    sd.prog[MV_INSTANCED] = PROG_STD_MAP_INST_ICOL_ATEST_DBL;
    Material *shrubMat = mat_three(&sd);
    Rng rs = rng_make(8);
    static const uint32_t SC[4] = { 0x3f5a2c, 0x4a6632, 0x355026, 0x56703a };
    ItemVec items = {};
    for (size_t i = 0; i < ctx.shrubs.len; i++) {
      Item s = ctx.shrubs.data[i];
      s.rot = rng_next(&rs) * 6;
      s.color = SC[(int)floor(rng_next(&rs) * 4)];
      vec_push(&items, s);
    }
    Geometry *g = shrub_geometry();
    place(b, g, shrubMat, &items, scale_shrub, nullptr, "hotel shrubs");
    geo_free(g);
    vec_free(&items);
  }
  if (ctx.palms.len) {
    Rng rp = rng_make(31);
    ItemVec potted = {};
    for (size_t i = 0; i < ctx.palms.len; i++)
      if (ctx.palms.data[i].pot) {
        const Item *p = &ctx.palms.data[i];
        vec_push(&potted, ((Item){ .x = p->x, .y = p->y, .z = p->z, .color = p->potCol }));
      }
    if (potted.len) {
      Geometry *pot = geo_translate(geo_cyl(0.3, 0.22, 0.55, 14), 0, 0.275, 0);
      MatDesc pd = md_standard();
      pd.name = "hotel pots";
      pd.roughness = 0.7;
      pd.prog[MV_INSTANCED] = PROG_STD_INST_ICOL;
      place(b, pot, mat_three(&pd), &potted, nullptr, nullptr, "hotel pots");
      geo_free(pot);
    }
    vec_free(&potted);
    MatDesc ld = md_standard();
    ld.name = "hotel potted palms";
    ld.color = color_hex(0xffffff);
    ld.roughness = 0.6;
    ld.side = SIDE_DOUBLE;
    ld.map = leaf_texture();
    ld.alpha_test = 0.5;
    ld.prog[MV_INSTANCED] = PROG_HOTEL_POTPALM;
    Material *leafMat = mat_three(&ld);
    // bushy clipped shrubs in the terrace planters and pots
    static const uint32_t PC[4] = { 0x4f7a2e, 0x5b8636, 0x46702a, 0x668a3a };
    ItemVec items = {};
    for (size_t i = 0; i < ctx.palms.len; i++) {
      const Item *p = &ctx.palms.data[i];
      double rot = rng_next(&rp) * 6.28;
      uint32_t color = PC[(int)floor(rng_next(&rp) * 4)];
      vec_push(&items, ((Item){ .x = p->x, .y = p->y + (p->pot ? 0.5 : 0), .z = p->z, .s = p->s, .rot = rot, .color = color }));
    }
    Geometry *g = geo_translate(geo_scale(shrub_geometry(), 1.1, 1.0, 1.1), 0, 0.42, 0);
    place(b, g, leafMat, &items, scale_palm, nullptr, "hotel potted palms");
    geo_free(g);
    vec_free(&items);
  }
  if (ctx.bulbs.len) {
    MatDesc bd = md_standard();
    bd.name = "hotel bulbs";
    bd.color = color_hex(0xfff1d8);
    bd.emissive = color_hex(0xffc27a);
    bd.emissive_intensity = 0.9;
    bd.roughness = 0.3;
    bd.prog[MV_INSTANCED] = PROG_STD_INST;
    Material *bulbMat = mat_three(&bd);
    RecVec recs = {};
    for (size_t i = 0; i < ctx.bulbs.len; i++) {
      const Item *bb = &ctx.bulbs.data[i];
      vec_push(&recs, ((Rec){ .z = bb->z, .m = m4_translation(bb->x, bb->y, bb->z) }));
    }
    Geometry *ico = geo_icosahedron(0.03, 1);
    per_chunk(b, nullptr, gpu_geometry(ico), bulbMat, &recs, W_DETAIL, false, false, false, "hotel bulbs");
    geo_free(ico);
    vec_free(&recs);
  }

  for (int ci = 0; ci < b->nchunks; ci++) {
    Chunk *c = &b->chunks[ci];
    node_add(group, c->base);
    node_add(group, c->detail);
    if (c->detail->children.len) lod_register(c->detail, fmax(c->min, -5000), fmin(c->max, 5000), LOD_DETAIL);
  }
  // walk collision: each building's front line (patios in front are raised terraces)
  Hotels out = { group, xmalloc(sizeof(Footprint) * (size_t)ns), ns };
  for (int i = 0; i < ns; i++) out.footprints[i] = (Footprint){ specs.data[i].z0, specs.data[i].z1, specs.data[i].fx };
  node_add(scene, group);

  geo_free(quadGeo);
  geo_free(revealGeo);
  vec_free(&glassIdx);
  vec_free(&revealRect);
  vec_free(&revealRound);
  vec_free(&ctx.windows);
  vec_free(&ctx.chairs); vec_free(&ctx.tables); vec_free(&ctx.umbrellas);
  vec_free(&ctx.shrubs); vec_free(&ctx.palms); vec_free(&ctx.bulbs);
  vec_free(&specs);
#undef ATLAS_OF
  return out;
}
