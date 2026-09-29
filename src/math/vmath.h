// Vector / matrix / quaternion / colour math: a double-precision port of the three.js
// classes the scene uses (Vector2/3/4, Quaternion, Euler, Matrix3/4, Color, MathUtils).
// Conventions follow three.js exactly: column-major matrices (e[col * 4 + row]), right-handed,
// y up, Euler order 'XYZ' by default. JS numbers are doubles, so everything here is double;
// values are narrowed to float only when uploaded to the GPU.
#pragma once

#include <math.h>
#include <stdint.h>

#define PI_D 3.141592653589793
#define DEG2RAD (PI_D / 180.0)
#define RAD2DEG (180.0 / PI_D)

// ---- JS number semantics ---------------------------------------------------------------

// Math.round: halves round toward +infinity (C round() rounds them away from zero).
static inline double js_round(double x) {
  double r = floor(x);
  return (x - r >= 0.5) ? r + 1.0 : r;
}
// ToUint32 (x >>> 0)
static inline uint32_t js_u32(double x) {
  if (!isfinite(x)) return 0;
  double m = fmod(trunc(x), 4294967296.0);
  if (m < 0) m += 4294967296.0;
  return (uint32_t)m;
}
// ToInt32 (x | 0)
static inline int32_t js_i32(double x) { return (int32_t)js_u32(x); }
// Math.hypot as V8 computes it: values normalized by the largest, Kahan-summed squares
static inline double js_hypot_n(const double *v, int n) {
  double mx = 0;
  for (int i = 0; i < n; i++) {
    double a = fabs(v[i]);
    if (isinf(a)) return INFINITY;
    if (isnan(a)) return NAN;
    if (a > mx) mx = a;
  }
  if (mx == 0) return 0;
  double sum = 0, comp = 0;
  for (int i = 0; i < n; i++) {
    double r = fabs(v[i]) / mx;
    double summand = r * r - comp;
    double prelim = sum + summand;
    comp = (prelim - sum) - summand;
    sum = prelim;
  }
  return sqrt(sum) * mx;
}
static inline double js_hypot2(double a, double b) { double v[2] = { a, b }; return js_hypot_n(v, 2); }
static inline double js_hypot3(double a, double b, double c) { double v[3] = { a, b, c }; return js_hypot_n(v, 3); }
static inline double js_sign(double x) { return x > 0 ? 1.0 : x < 0 ? -1.0 : x; }
static inline double clampd(double v, double a, double b) { return v < a ? a : v > b ? b : v; }
static inline double mind(double a, double b) { return a < b ? a : b; }
static inline double maxd(double a, double b) { return a > b ? a : b; }
static inline double lerpd(double a, double b, double t) { return (1 - t) * a + t * b; }   // MathUtils.lerp
static inline double euclid_mod(double n, double m) { return fmod(fmod(n, m) + m, m); }
// MathUtils.smoothstep(x, min, max)
// +x.toFixed(digits) (JS rounding, exact ties away from zero)
double js_to_fixed(double x, int digits);
static inline double mu_smoothstep(double x, double lo, double hi) {
  if (x <= lo) return 0;
  if (x >= hi) return 1;
  x = (x - lo) / (hi - lo);
  return x * x * (3 - 2 * x);
}
// GLSL-style smoothstep(edge0, edge1, x), as the scene's own JS helpers write it
static inline double smooth3(double a, double b, double v) {
  double t = clampd((v - a) / (b - a), 0, 1);
  return t * t * (3 - 2 * t);
}
static inline double damp(double x, double y, double lambda, double dt) { return lerpd(x, y, 1 - exp(-lambda * dt)); }

// ---- mulberry32 (textures/noise.js) ----------------------------------------------------
typedef struct Rng { uint32_t a; } Rng;
static inline Rng rng_make(double seed) { return (Rng){ js_u32(seed) }; }
static inline double rng_next(Rng *r) {
  r->a += 0x6d2b79f5u;
  uint32_t t = r->a;
  t = (t ^ (t >> 15)) * (t | 1u);
  t ^= t + (t ^ (t >> 7)) * (t | 61u);
  return (double)(t ^ (t >> 14)) / 4294967296.0;
}

// ---- Vector2 ---------------------------------------------------------------------------
typedef struct V2 { double x, y; } V2;
static inline V2 v2(double x, double y) { return (V2){ x, y }; }
static inline V2 v2_add(V2 a, V2 b) { return (V2){ a.x + b.x, a.y + b.y }; }
static inline V2 v2_sub(V2 a, V2 b) { return (V2){ a.x - b.x, a.y - b.y }; }
static inline V2 v2_scale(V2 a, double s) { return (V2){ a.x * s, a.y * s }; }
static inline double v2_dot(V2 a, V2 b) { return a.x * b.x + a.y * b.y; }
static inline double v2_cross(V2 a, V2 b) { return a.x * b.y - a.y * b.x; }
static inline double v2_len(V2 a) { return sqrt(a.x * a.x + a.y * a.y); }
static inline double v2_len_sq(V2 a) { return a.x * a.x + a.y * a.y; }
static inline V2 v2_norm(V2 a) { double l = v2_len(a); return v2_scale(a, 1.0 / (l == 0 ? 1 : l)); }   // divideScalar(length() || 1)
static inline V2 v2_lerp(V2 a, V2 b, double t) { return (V2){ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t }; }
static inline double v2_dist(V2 a, V2 b) { return v2_len(v2_sub(a, b)); }
static inline double v2_dist_sq(V2 a, V2 b) { return v2_len_sq(v2_sub(a, b)); }

// ---- Vector3 ---------------------------------------------------------------------------
typedef struct V3 { double x, y, z; } V3;
static inline V3 v3(double x, double y, double z) { return (V3){ x, y, z }; }
static inline V3 v3s(double s) { return (V3){ s, s, s }; }
static inline V3 v3_add(V3 a, V3 b) { return (V3){ a.x + b.x, a.y + b.y, a.z + b.z }; }
static inline V3 v3_sub(V3 a, V3 b) { return (V3){ a.x - b.x, a.y - b.y, a.z - b.z }; }
static inline V3 v3_mul(V3 a, V3 b) { return (V3){ a.x * b.x, a.y * b.y, a.z * b.z }; }
static inline V3 v3_scale(V3 a, double s) { return (V3){ a.x * s, a.y * s, a.z * s }; }
static inline V3 v3_add_scaled(V3 a, V3 b, double s) { return (V3){ a.x + b.x * s, a.y + b.y * s, a.z + b.z * s }; }
static inline V3 v3_neg(V3 a) { return (V3){ -a.x, -a.y, -a.z }; }
static inline double v3_dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline V3 v3_cross(V3 a, V3 b) {
  return (V3){ a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
static inline double v3_len_sq(V3 a) { return a.x * a.x + a.y * a.y + a.z * a.z; }
static inline double v3_len(V3 a) { return sqrt(v3_len_sq(a)); }
static inline V3 v3_norm(V3 a) {   // divideScalar(length() || 1): divideScalar multiplies by 1/s
  double l = v3_len(a);
  return v3_scale(a, 1.0 / (l == 0 ? 1 : l));
}
static inline V3 v3_set_len(V3 a, double len) { return v3_scale(v3_norm(a), len); }
static inline double v3_dist(V3 a, V3 b) { return v3_len(v3_sub(a, b)); }
static inline double v3_dist_sq(V3 a, V3 b) { return v3_len_sq(v3_sub(a, b)); }
static inline V3 v3_lerp(V3 a, V3 b, double t) {
  return (V3){ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t };
}
static inline V3 v3_min(V3 a, V3 b) { return (V3){ fmin(a.x, b.x), fmin(a.y, b.y), fmin(a.z, b.z) }; }
static inline V3 v3_max(V3 a, V3 b) { return (V3){ fmax(a.x, b.x), fmax(a.y, b.y), fmax(a.z, b.z) }; }
static inline double v3_comp(V3 a, int i) { return i == 0 ? a.x : i == 1 ? a.y : a.z; }
static inline V3 v3_project_on_plane(V3 v, V3 n) {  // n unit length
  return v3_sub(v, v3_scale(n, v3_dot(v, n)));
}
static inline V3 v3_reflect(V3 v, V3 n) { return v3_sub(v, v3_scale(n, 2 * v3_dot(v, n))); }

// ---- Vector4 ---------------------------------------------------------------------------
typedef struct V4 { double x, y, z, w; } V4;
static inline V4 v4(double x, double y, double z, double w) { return (V4){ x, y, z, w }; }

// ---- Quaternion ------------------------------------------------------------------------
typedef struct Quat { double x, y, z, w; } Quat;
static inline Quat quat_identity(void) { return (Quat){ 0, 0, 0, 1 }; }
static inline double quat_len(Quat q) { return sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w); }
static inline Quat quat_norm(Quat q) {
  double l = quat_len(q);
  if (l == 0) return (Quat){ 0, 0, 0, 1 };
  l = 1 / l;
  return (Quat){ q.x * l, q.y * l, q.z * l, q.w * l };
}
static inline double quat_dot(Quat a, Quat b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
static inline Quat quat_conj(Quat q) { return (Quat){ -q.x, -q.y, -q.z, q.w }; }   // == invert() for unit q
static inline Quat quat_mul(Quat a, Quat b) {   // multiplyQuaternions(a, b)
  return (Quat){
    a.x * b.w + a.w * b.x + a.y * b.z - a.z * b.y,
    a.y * b.w + a.w * b.y + a.z * b.x - a.x * b.z,
    a.z * b.w + a.w * b.z + a.x * b.y - a.y * b.x,
    a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
  };
}
static inline Quat quat_axis_angle(V3 axis, double angle) {   // axis normalized
  double h = angle / 2, s = sin(h);
  return (Quat){ axis.x * s, axis.y * s, axis.z * s, cos(h) };
}
static inline Quat quat_unit_vectors(V3 from, V3 to) {
  double r = v3_dot(from, to) + 1;
  Quat q;
  if (r < 1e-8) {
    r = 0;
    if (fabs(from.x) > fabs(from.z)) q = (Quat){ -from.y, from.x, 0, r };
    else q = (Quat){ 0, -from.z, from.y, r };
  } else {
    q = (Quat){ from.y * to.z - from.z * to.y, from.z * to.x - from.x * to.z, from.x * to.y - from.y * to.x, r };
  }
  return quat_norm(q);
}
static inline Quat quat_slerp(Quat a, Quat b, double t) {   // r186: no early outs at t = 0 / 1
  double x = b.x, y = b.y, z = b.z, w = b.w;
  double d = quat_dot(a, b);
  if (d < 0) { x = -x; y = -y; z = -z; w = -w; d = -d; }
  double s = 1 - t;
  if (d < 0.9995) {
    double theta = acos(d), sn = sin(theta);
    s = sin(s * theta) / sn;
    t = sin(t * theta) / sn;
    return (Quat){ a.x * s + x * t, a.y * s + y * t, a.z * s + z * t, a.w * s + w * t };
  }
  return quat_norm((Quat){ a.x * s + x * t, a.y * s + y * t, a.z * s + z * t, a.w * s + w * t });
}
static inline V3 v3_apply_quat(V3 v, Quat q) {
  double tx = 2 * (q.y * v.z - q.z * v.y);
  double ty = 2 * (q.z * v.x - q.x * v.z);
  double tz = 2 * (q.x * v.y - q.y * v.x);
  return (V3){ v.x + q.w * tx + q.y * tz - q.z * ty,
               v.y + q.w * ty + q.z * tx - q.x * tz,
               v.z + q.w * tz + q.x * ty - q.y * tx };
}

// ---- Euler -----------------------------------------------------------------------------
typedef enum EulerOrder { EULER_XYZ, EULER_YXZ, EULER_ZXY, EULER_ZYX, EULER_YZX, EULER_XZY } EulerOrder;
typedef struct Euler { double x, y, z; EulerOrder order; } Euler;
static inline Euler euler(double x, double y, double z, EulerOrder o) { return (Euler){ x, y, z, o }; }
Quat quat_from_euler(Euler e);

// ---- Matrix4 (column-major, three.js element order) ------------------------------------
typedef struct M4 { double e[16]; } M4;
static inline M4 m4_identity(void) { return (M4){ { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 } }; }
M4 m4_mul(M4 a, M4 b);                       // a * b (multiplyMatrices)
M4 m4_compose(V3 pos, Quat q, V3 scale);
void m4_decompose(M4 m, V3 *pos, Quat *q, V3 *scale);
M4 m4_invert(M4 m);
double m4_determinant(M4 m);
M4 m4_from_quat(Quat q);                     // makeRotationFromQuaternion
M4 m4_translation(double x, double y, double z);
M4 m4_scaling(double x, double y, double z);
M4 m4_rotation_x(double t);
M4 m4_rotation_y(double t);
M4 m4_rotation_z(double t);
M4 m4_rotation_axis(V3 axis, double angle);  // axis normalized
M4 m4_look_at(V3 eye, V3 target, V3 up);     // rotation only, like Matrix4.lookAt
// Projections in three's WebGLCoordinateSystem (clip z in [-w, w], as the JS sees them on WebGL);
// every vertex shader remaps z to [0, w] for SDL_GPU the way ANGLE does, so clipping matches.
M4 m4_perspective(double fov_deg, double aspect, double near, double far, double zoom);
M4 m4_orthographic(double left, double right, double top, double bottom, double near, double far);
Quat quat_from_rotation_matrix(M4 m);        // upper 3x3 must be pure rotation
Euler euler_from_rotation_matrix(M4 m, EulerOrder order);
double m4_max_scale_on_axis(M4 m);

static inline V3 m4_col3(M4 m, int c) { return (V3){ m.e[c * 4], m.e[c * 4 + 1], m.e[c * 4 + 2] }; }
static inline V3 m4_get_position(M4 m) { return (V3){ m.e[12], m.e[13], m.e[14] }; }
static inline M4 m4_set_position(M4 m, V3 p) { m.e[12] = p.x; m.e[13] = p.y; m.e[14] = p.z; return m; }
// Vector3.applyMatrix4 (with perspective divide)
static inline V3 v3_apply_m4(V3 v, M4 m) {
  const double *e = m.e;
  double w = 1 / (e[3] * v.x + e[7] * v.y + e[11] * v.z + e[15]);
  return (V3){ (e[0] * v.x + e[4] * v.y + e[8] * v.z + e[12]) * w,
               (e[1] * v.x + e[5] * v.y + e[9] * v.z + e[13]) * w,
               (e[2] * v.x + e[6] * v.y + e[10] * v.z + e[14]) * w };
}
// Vector3.transformDirection (upper 3x3, normalized)
static inline V3 v3_transform_dir(V3 v, M4 m) {
  const double *e = m.e;
  return v3_norm((V3){ e[0] * v.x + e[4] * v.y + e[8] * v.z,
                       e[1] * v.x + e[5] * v.y + e[9] * v.z,
                       e[2] * v.x + e[6] * v.y + e[10] * v.z });
}
static inline V4 v4_apply_m4(V4 v, M4 m) {
  const double *e = m.e;
  return (V4){ e[0] * v.x + e[4] * v.y + e[8] * v.z + e[12] * v.w,
               e[1] * v.x + e[5] * v.y + e[9] * v.z + e[13] * v.w,
               e[2] * v.x + e[6] * v.y + e[10] * v.z + e[14] * v.w,
               e[3] * v.x + e[7] * v.y + e[11] * v.z + e[15] * v.w };
}

// ---- Matrix3 ---------------------------------------------------------------------------
typedef struct M3 { double e[9]; } M3;
M3 m3_normal_matrix(M4 m);                   // inverse transpose of the upper 3x3 (getNormalMatrix)
static inline V3 v3_apply_m3(V3 v, M3 m) {
  const double *e = m.e;
  return (V3){ e[0] * v.x + e[3] * v.y + e[6] * v.z,
               e[1] * v.x + e[4] * v.y + e[7] * v.z,
               e[2] * v.x + e[5] * v.y + e[8] * v.z };
}

// ---- Box3 / Sphere / Plane / Frustum ---------------------------------------------------
typedef struct Box3 { V3 min, max; } Box3;
static inline Box3 box3_empty(void) { return (Box3){ v3s(INFINITY), v3s(-INFINITY) }; }
static inline bool box3_is_empty(Box3 b) { return b.max.x < b.min.x || b.max.y < b.min.y || b.max.z < b.min.z; }
static inline Box3 box3_expand(Box3 b, V3 p) { return (Box3){ v3_min(b.min, p), v3_max(b.max, p) }; }
static inline Box3 box3_union(Box3 a, Box3 b) { return (Box3){ v3_min(a.min, b.min), v3_max(a.max, b.max) }; }
static inline V3 box3_center(Box3 b) { return box3_is_empty(b) ? v3s(0) : v3_scale(v3_add(b.min, b.max), 0.5); }
static inline V3 box3_size(Box3 b) { return box3_is_empty(b) ? v3s(0) : v3_sub(b.max, b.min); }
Box3 box3_apply_m4(Box3 b, M4 m);            // bounds of the 8 transformed corners

typedef struct Sphere { V3 center; double radius; } Sphere;
static inline Sphere sphere_apply_m4(Sphere s, M4 m) {
  return (Sphere){ v3_apply_m4(s.center, m), s.radius * m4_max_scale_on_axis(m) };
}

typedef struct Plane { V3 normal; double constant; } Plane;
typedef struct Frustum { Plane planes[6]; } Frustum;
// planes of a WebGL view-projection matrix (Frustum.setFromProjectionMatrix)
Frustum frustum_from_m4(M4 vp);
bool frustum_hits_sphere(const Frustum *f, Sphere s);
bool frustum_hits_box(const Frustum *f, Box3 b);
bool frustum_contains_point(const Frustum *f, V3 p);

// ---- Color (linear working space, like three's ColorManagement) ------------------------
typedef struct Color { double r, g, b; } Color;
static inline double srgb_to_linear(double c) {
  return (c < 0.04045) ? c * 0.0773993808 : pow(c * 0.9478672986 + 0.0521327014, 2.4);
}
static inline double linear_to_srgb(double c) {
  return (c < 0.0031308) ? c * 12.92 : 1.055 * pow(c, 0.41666) - 0.055;
}
// new THREE.Color(0xRRGGBB) / setHex: the hex is sRGB, stored linear
static inline Color color_hex(uint32_t hex) {
  return (Color){ srgb_to_linear((double)((hex >> 16) & 255) / 255),
                  srgb_to_linear((double)((hex >> 8) & 255) / 255),
                  srgb_to_linear((double)(hex & 255) / 255) };
}
// setRGB(r, g, b) with the default (working, linear) colour space: stored as given
static inline Color color_rgb(double r, double g, double b) { return (Color){ r, g, b }; }
// setRGB(r, g, b, SRGBColorSpace)
static inline Color color_srgb(double r, double g, double b) {
  return (Color){ srgb_to_linear(r), srgb_to_linear(g), srgb_to_linear(b) };
}
Color color_hsl(double h, double s, double l);          // setHSL, working (linear) space
void color_get_hsl(Color c, double *h, double *s, double *l);
static inline Color color_offset_hsl(Color c, double dh, double ds, double dl) {
  double h, s, l;
  color_get_hsl(c, &h, &s, &l);
  return color_hsl(h + dh, s + ds, l + dl);
}
static inline Color color_lerp(Color a, Color b, double t) {
  return (Color){ a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t };
}
static inline Color color_scale(Color c, double s) { return (Color){ c.r * s, c.g * s, c.b * s }; }
static inline Color color_mul(Color a, Color b) { return (Color){ a.r * b.r, a.g * b.g, a.b * b.b }; }
static inline Color color_add(Color a, Color b) { return (Color){ a.r + b.r, a.g + b.g, a.b + b.b }; }
// getHex(): back to sRGB, packed
uint32_t color_get_hex(Color c);
// setStyle for '#rgb', '#rrggbb', 'rgb(r,g,b)', 'hsl(h,s%,l%)' and the CSS names the scene uses
Color color_style(const char *style);
