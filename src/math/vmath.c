#include "math/vmath.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/common.h"

Quat quat_from_euler(Euler e) {
  double c1 = cos(e.x / 2), c2 = cos(e.y / 2), c3 = cos(e.z / 2);
  double s1 = sin(e.x / 2), s2 = sin(e.y / 2), s3 = sin(e.z / 2);
  switch (e.order) {
  case EULER_XYZ: return (Quat){ s1 * c2 * c3 + c1 * s2 * s3, c1 * s2 * c3 - s1 * c2 * s3, c1 * c2 * s3 + s1 * s2 * c3, c1 * c2 * c3 - s1 * s2 * s3 };
  case EULER_YXZ: return (Quat){ s1 * c2 * c3 + c1 * s2 * s3, c1 * s2 * c3 - s1 * c2 * s3, c1 * c2 * s3 - s1 * s2 * c3, c1 * c2 * c3 + s1 * s2 * s3 };
  case EULER_ZXY: return (Quat){ s1 * c2 * c3 - c1 * s2 * s3, c1 * s2 * c3 + s1 * c2 * s3, c1 * c2 * s3 + s1 * s2 * c3, c1 * c2 * c3 - s1 * s2 * s3 };
  case EULER_ZYX: return (Quat){ s1 * c2 * c3 - c1 * s2 * s3, c1 * s2 * c3 + s1 * c2 * s3, c1 * c2 * s3 - s1 * s2 * c3, c1 * c2 * c3 + s1 * s2 * s3 };
  case EULER_YZX: return (Quat){ s1 * c2 * c3 + c1 * s2 * s3, c1 * s2 * c3 + s1 * c2 * s3, c1 * c2 * s3 - s1 * s2 * c3, c1 * c2 * c3 - s1 * s2 * s3 };
  case EULER_XZY: return (Quat){ s1 * c2 * c3 - c1 * s2 * s3, c1 * s2 * c3 - s1 * c2 * s3, c1 * c2 * s3 + s1 * s2 * c3, c1 * c2 * c3 + s1 * s2 * s3 };
  }
  FATAL("bad euler order %d", (int)e.order);
}

M4 m4_mul(M4 a, M4 b) {
  const double *ae = a.e, *be = b.e;
  double a11 = ae[0], a12 = ae[4], a13 = ae[8], a14 = ae[12];
  double a21 = ae[1], a22 = ae[5], a23 = ae[9], a24 = ae[13];
  double a31 = ae[2], a32 = ae[6], a33 = ae[10], a34 = ae[14];
  double a41 = ae[3], a42 = ae[7], a43 = ae[11], a44 = ae[15];
  double b11 = be[0], b12 = be[4], b13 = be[8], b14 = be[12];
  double b21 = be[1], b22 = be[5], b23 = be[9], b24 = be[13];
  double b31 = be[2], b32 = be[6], b33 = be[10], b34 = be[14];
  double b41 = be[3], b42 = be[7], b43 = be[11], b44 = be[15];
  M4 r;
  double *te = r.e;
  te[0] = a11 * b11 + a12 * b21 + a13 * b31 + a14 * b41;
  te[4] = a11 * b12 + a12 * b22 + a13 * b32 + a14 * b42;
  te[8] = a11 * b13 + a12 * b23 + a13 * b33 + a14 * b43;
  te[12] = a11 * b14 + a12 * b24 + a13 * b34 + a14 * b44;
  te[1] = a21 * b11 + a22 * b21 + a23 * b31 + a24 * b41;
  te[5] = a21 * b12 + a22 * b22 + a23 * b32 + a24 * b42;
  te[9] = a21 * b13 + a22 * b23 + a23 * b33 + a24 * b43;
  te[13] = a21 * b14 + a22 * b24 + a23 * b34 + a24 * b44;
  te[2] = a31 * b11 + a32 * b21 + a33 * b31 + a34 * b41;
  te[6] = a31 * b12 + a32 * b22 + a33 * b32 + a34 * b42;
  te[10] = a31 * b13 + a32 * b23 + a33 * b33 + a34 * b43;
  te[14] = a31 * b14 + a32 * b24 + a33 * b34 + a34 * b44;
  te[3] = a41 * b11 + a42 * b21 + a43 * b31 + a44 * b41;
  te[7] = a41 * b12 + a42 * b22 + a43 * b32 + a44 * b42;
  te[11] = a41 * b13 + a42 * b23 + a43 * b33 + a44 * b43;
  te[15] = a41 * b14 + a42 * b24 + a43 * b34 + a44 * b44;
  return r;
}

M4 m4_compose(V3 p, Quat q, V3 s) {
  double x = q.x, y = q.y, z = q.z, w = q.w;
  double x2 = x + x, y2 = y + y, z2 = z + z;
  double xx = x * x2, xy = x * y2, xz = x * z2;
  double yy = y * y2, yz = y * z2, zz = z * z2;
  double wx = w * x2, wy = w * y2, wz = w * z2;
  return (M4){ {
    (1 - (yy + zz)) * s.x, (xy + wz) * s.x, (xz - wy) * s.x, 0,
    (xy - wz) * s.y, (1 - (xx + zz)) * s.y, (yz + wx) * s.y, 0,
    (xz + wy) * s.z, (yz - wx) * s.z, (1 - (xx + yy)) * s.z, 0,
    p.x, p.y, p.z, 1,
  } };
}

M4 m4_from_quat(Quat q) { return m4_compose(v3s(0), q, v3s(1)); }

static double m4_determinant_affine(const double *te) {
  // Matrix4.determinantAffine: the upper 3x3
  double a = te[0], b = te[4], c = te[8];
  double d = te[1], e = te[5], f = te[9];
  double g = te[2], h = te[6], i = te[10];
  return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
}

void m4_decompose(M4 m, V3 *pos, Quat *q, V3 *scale) {
  const double *te = m.e;
  *pos = (V3){ te[12], te[13], te[14] };
  double det = m4_determinant_affine(te);
  if (det == 0) {
    *scale = v3s(1);
    *q = quat_identity();
    return;
  }
  double sx = v3_len(v3(te[0], te[1], te[2]));
  double sy = v3_len(v3(te[4], te[5], te[6]));
  double sz = v3_len(v3(te[8], te[9], te[10]));
  if (det < 0) sx = -sx;
  M4 r = m;
  double ix = 1 / sx, iy = 1 / sy, iz = 1 / sz;
  r.e[0] *= ix; r.e[1] *= ix; r.e[2] *= ix;
  r.e[4] *= iy; r.e[5] *= iy; r.e[6] *= iy;
  r.e[8] *= iz; r.e[9] *= iz; r.e[10] *= iz;
  *q = quat_from_rotation_matrix(r);
  *scale = (V3){ sx, sy, sz };
}

double m4_determinant(M4 m) {
  const double *te = m.e;
  double n11 = te[0], n12 = te[4], n13 = te[8], n14 = te[12];
  double n21 = te[1], n22 = te[5], n23 = te[9], n24 = te[13];
  double n31 = te[2], n32 = te[6], n33 = te[10], n34 = te[14];
  double n41 = te[3], n42 = te[7], n43 = te[11], n44 = te[15];
  return n41 * (+n14 * n23 * n32 - n13 * n24 * n32 - n14 * n22 * n33 + n12 * n24 * n33 + n13 * n22 * n34 - n12 * n23 * n34)
       + n42 * (+n11 * n23 * n34 - n11 * n24 * n33 + n14 * n21 * n33 - n13 * n21 * n34 + n13 * n24 * n31 - n14 * n23 * n31)
       + n43 * (+n11 * n24 * n32 - n11 * n22 * n34 - n14 * n21 * n32 + n12 * n21 * n34 + n14 * n22 * n31 - n12 * n24 * n31)
       + n44 * (-n13 * n22 * n31 - n11 * n23 * n32 + n11 * n22 * n33 + n13 * n21 * n32 - n12 * n21 * n33 + n12 * n23 * n31);
}

M4 m4_invert(M4 m) {
  const double *te = m.e;
  double n11 = te[0], n21 = te[1], n31 = te[2], n41 = te[3];
  double n12 = te[4], n22 = te[5], n32 = te[6], n42 = te[7];
  double n13 = te[8], n23 = te[9], n33 = te[10], n43 = te[11];
  double n14 = te[12], n24 = te[13], n34 = te[14], n44 = te[15];
  double t1 = n11 * n22 - n21 * n12, t2 = n11 * n32 - n31 * n12, t3 = n11 * n42 - n41 * n12;
  double t4 = n21 * n32 - n31 * n22, t5 = n21 * n42 - n41 * n22, t6 = n31 * n42 - n41 * n32;
  double t7 = n13 * n24 - n23 * n14, t8 = n13 * n34 - n33 * n14, t9 = n13 * n44 - n43 * n14;
  double t10 = n23 * n34 - n33 * n24, t11 = n23 * n44 - n43 * n24, t12 = n33 * n44 - n43 * n34;
  double det = t1 * t12 - t2 * t11 + t3 * t10 + t4 * t9 - t5 * t8 + t6 * t7;
  if (det == 0) return (M4){};
  double di = 1 / det;
  return (M4){ {
    (n22 * t12 - n32 * t11 + n42 * t10) * di,
    (n31 * t11 - n21 * t12 - n41 * t10) * di,
    (n24 * t6 - n34 * t5 + n44 * t4) * di,
    (n33 * t5 - n23 * t6 - n43 * t4) * di,
    (n32 * t9 - n12 * t12 - n42 * t8) * di,
    (n11 * t12 - n31 * t9 + n41 * t8) * di,
    (n34 * t3 - n14 * t6 - n44 * t2) * di,
    (n13 * t6 - n33 * t3 + n43 * t2) * di,
    (n12 * t11 - n22 * t9 + n42 * t7) * di,
    (n21 * t9 - n11 * t11 - n41 * t7) * di,
    (n14 * t5 - n24 * t3 + n44 * t1) * di,
    (n23 * t3 - n13 * t5 - n43 * t1) * di,
    (n22 * t8 - n12 * t10 - n32 * t7) * di,
    (n11 * t10 - n21 * t8 + n31 * t7) * di,
    (n24 * t2 - n14 * t4 - n34 * t1) * di,
    (n13 * t4 - n23 * t2 + n33 * t1) * di,
  } };
}

M4 m4_translation(double x, double y, double z) {
  return (M4){ { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1 } };
}
M4 m4_scaling(double x, double y, double z) {
  return (M4){ { x, 0, 0, 0, 0, y, 0, 0, 0, 0, z, 0, 0, 0, 0, 1 } };
}
M4 m4_rotation_x(double t) {
  double c = cos(t), s = sin(t);
  return (M4){ { 1, 0, 0, 0, 0, c, s, 0, 0, -s, c, 0, 0, 0, 0, 1 } };
}
M4 m4_rotation_y(double t) {
  double c = cos(t), s = sin(t);
  return (M4){ { c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 0, 0, 0, 1 } };
}
M4 m4_rotation_z(double t) {
  double c = cos(t), s = sin(t);
  return (M4){ { c, s, 0, 0, -s, c, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 } };
}
M4 m4_rotation_axis(V3 axis, double angle) {
  double c = cos(angle), s = sin(angle), t = 1 - c;
  double x = axis.x, y = axis.y, z = axis.z, tx = t * x, ty = t * y;
  // Matrix4.set() takes row-major arguments; stored here column-major
  return (M4){ {
    tx * x + c, tx * y + s * z, tx * z - s * y, 0,
    tx * y - s * z, ty * y + c, ty * z + s * x, 0,
    tx * z + s * y, ty * z - s * x, t * z * z + c, 0,
    0, 0, 0, 1,
  } };
}

M4 m4_look_at(V3 eye, V3 target, V3 up) {
  V3 z = v3_sub(eye, target);
  if (v3_len_sq(z) == 0) z.z = 1;
  z = v3_norm(z);
  V3 x = v3_cross(up, z);
  if (v3_len_sq(x) == 0) {
    if (fabs(up.z) == 1) z.x += 0.0001;
    else z.z += 0.0001;
    z = v3_norm(z);
    x = v3_cross(up, z);
  }
  x = v3_norm(x);
  V3 y = v3_cross(z, x);
  M4 m = m4_identity();
  m.e[0] = x.x; m.e[4] = y.x; m.e[8] = z.x;
  m.e[1] = x.y; m.e[5] = y.y; m.e[9] = z.y;
  m.e[2] = x.z; m.e[6] = y.z; m.e[10] = z.z;
  return m;
}

M4 m4_perspective(double fov_deg, double aspect, double near, double far, double zoom) {
  // PerspectiveCamera.updateProjectionMatrix + makePerspective (WebGL coordinate system: the
  // JS runs on WebGL, and the vertex shaders remap clip z to [0, w] the way ANGLE does)
  double top = near * tan(DEG2RAD * 0.5 * fov_deg) / zoom;
  double height = 2 * top, width = aspect * height, left = -0.5 * width;
  double right = left + width, bottom = top - height;
  double x = 2 * near / (right - left), y = 2 * near / (top - bottom);
  double a = (right + left) / (right - left), b = (top + bottom) / (top - bottom);
  double c = -(far + near) / (far - near), d = (-2 * far * near) / (far - near);
  return (M4){ { x, 0, 0, 0, 0, y, 0, 0, a, b, c, -1, 0, 0, d, 0 } };
}

M4 m4_orthographic(double left, double right, double top, double bottom, double near, double far) {
  // Matrix4.makeOrthographic (WebGL coordinate system)
  double x = 2 / (right - left), y = 2 / (top - bottom);
  double a = -(right + left) / (right - left), b = -(top + bottom) / (top - bottom);
  double c = -2 / (far - near), d = -(far + near) / (far - near);
  return (M4){ { x, 0, 0, 0, 0, y, 0, 0, 0, 0, c, 0, a, b, d, 1 } };
}

Quat quat_from_rotation_matrix(M4 m) {
  const double *te = m.e;
  double m11 = te[0], m12 = te[4], m13 = te[8];
  double m21 = te[1], m22 = te[5], m23 = te[9];
  double m31 = te[2], m32 = te[6], m33 = te[10];
  double trace = m11 + m22 + m33;
  if (trace > 0) {
    double s = 0.5 / sqrt(trace + 1.0);
    return (Quat){ (m32 - m23) * s, (m13 - m31) * s, (m21 - m12) * s, 0.25 / s };
  } else if (m11 > m22 && m11 > m33) {
    double s = 2.0 * sqrt(1.0 + m11 - m22 - m33);
    return (Quat){ 0.25 * s, (m12 + m21) / s, (m13 + m31) / s, (m32 - m23) / s };
  } else if (m22 > m33) {
    double s = 2.0 * sqrt(1.0 + m22 - m11 - m33);
    return (Quat){ (m12 + m21) / s, 0.25 * s, (m23 + m32) / s, (m13 - m31) / s };
  }
  double s = 2.0 * sqrt(1.0 + m33 - m11 - m22);
  return (Quat){ (m13 + m31) / s, (m23 + m32) / s, 0.25 * s, (m21 - m12) / s };
}

Euler euler_from_rotation_matrix(M4 m, EulerOrder order) {
  const double *te = m.e;
  double m11 = te[0], m12 = te[4], m13 = te[8];
  double m21 = te[1], m22 = te[5], m23 = te[9];
  double m31 = te[2], m32 = te[6], m33 = te[10];
  Euler e = { 0, 0, 0, order };
  switch (order) {
  case EULER_XYZ:
    e.y = asin(clampd(m13, -1, 1));
    if (fabs(m13) < 0.9999999) { e.x = atan2(-m23, m33); e.z = atan2(-m12, m11); }
    else { e.x = atan2(m32, m22); e.z = 0; }
    break;
  case EULER_YXZ:
    e.x = asin(-clampd(m23, -1, 1));
    if (fabs(m23) < 0.9999999) { e.y = atan2(m13, m33); e.z = atan2(m21, m22); }
    else { e.y = atan2(-m31, m11); e.z = 0; }
    break;
  case EULER_ZXY:
    e.x = asin(clampd(m32, -1, 1));
    if (fabs(m32) < 0.9999999) { e.y = atan2(-m31, m33); e.z = atan2(-m12, m22); }
    else { e.y = 0; e.z = atan2(m21, m11); }
    break;
  case EULER_ZYX:
    e.y = asin(-clampd(m31, -1, 1));
    if (fabs(m31) < 0.9999999) { e.x = atan2(m32, m33); e.z = atan2(m21, m11); }
    else { e.x = 0; e.z = atan2(-m12, m22); }
    break;
  case EULER_YZX:
    e.z = asin(clampd(m21, -1, 1));
    if (fabs(m21) < 0.9999999) { e.x = atan2(-m23, m22); e.y = atan2(-m31, m11); }
    else { e.x = 0; e.y = atan2(m13, m33); }
    break;
  case EULER_XZY:
    e.z = asin(-clampd(m12, -1, 1));
    if (fabs(m12) < 0.9999999) { e.x = atan2(m32, m22); e.y = atan2(m13, m11); }
    else { e.x = atan2(-m23, m33); e.y = 0; }
    break;
  }
  return e;
}

double m4_max_scale_on_axis(M4 m) {
  const double *te = m.e;
  double sx = te[0] * te[0] + te[1] * te[1] + te[2] * te[2];
  double sy = te[4] * te[4] + te[5] * te[5] + te[6] * te[6];
  double sz = te[8] * te[8] + te[9] * te[9] + te[10] * te[10];
  return sqrt(fmax(sx, fmax(sy, sz)));
}

M3 m3_normal_matrix(M4 m) {
  // setFromMatrix4 -> invert -> transpose (Matrix3)
  const double *s = m.e;
  double n11 = s[0], n21 = s[1], n31 = s[2];
  double n12 = s[4], n22 = s[5], n32 = s[6];
  double n13 = s[8], n23 = s[9], n33 = s[10];
  double t11 = n33 * n22 - n32 * n23, t12 = n32 * n13 - n33 * n12, t13 = n23 * n12 - n22 * n13;
  double det = n11 * t11 + n21 * t12 + n31 * t13;
  if (det == 0) return (M3){};
  double di = 1 / det;
  M3 inv = { {
    t11 * di, (n31 * n23 - n33 * n21) * di, (n32 * n21 - n31 * n22) * di,
    t12 * di, (n33 * n11 - n31 * n13) * di, (n31 * n12 - n32 * n11) * di,
    t13 * di, (n21 * n13 - n23 * n11) * di, (n22 * n11 - n21 * n12) * di,
  } };
  M3 t = { { inv.e[0], inv.e[3], inv.e[6], inv.e[1], inv.e[4], inv.e[7], inv.e[2], inv.e[5], inv.e[8] } };
  return t;
}

Box3 box3_apply_m4(Box3 b, M4 m) {
  if (box3_is_empty(b)) return b;
  Box3 r = box3_empty();
  for (int i = 0; i < 8; i++) {
    V3 p = { (i & 4) ? b.max.x : b.min.x, (i & 2) ? b.max.y : b.min.y, (i & 1) ? b.max.z : b.min.z };
    r = box3_expand(r, v3_apply_m4(p, m));
  }
  return r;
}

static Plane plane_norm(double x, double y, double z, double w) {
  double inv = 1.0 / v3_len(v3(x, y, z));
  return (Plane){ v3(x * inv, y * inv, z * inv), w * inv };
}

Frustum frustum_from_m4(M4 vp) {
  const double *me = vp.e;
  Frustum f;
  f.planes[0] = plane_norm(me[3] - me[0], me[7] - me[4], me[11] - me[8], me[15] - me[12]);
  f.planes[1] = plane_norm(me[3] + me[0], me[7] + me[4], me[11] + me[8], me[15] + me[12]);
  f.planes[2] = plane_norm(me[3] + me[1], me[7] + me[5], me[11] + me[9], me[15] + me[13]);
  f.planes[3] = plane_norm(me[3] - me[1], me[7] - me[5], me[11] - me[9], me[15] - me[13]);
  f.planes[4] = plane_norm(me[3] - me[2], me[7] - me[6], me[11] - me[10], me[15] - me[14]);   // far
  f.planes[5] = plane_norm(me[3] + me[2], me[7] + me[6], me[11] + me[10], me[15] + me[14]);   // near (WebGL)
  return f;
}

bool frustum_hits_sphere(const Frustum *f, Sphere s) {
  for (int i = 0; i < 6; i++)
    if (v3_dot(f->planes[i].normal, s.center) + f->planes[i].constant < -s.radius) return false;
  return true;
}

bool frustum_contains_point(const Frustum *f, V3 p) {
  for (int i = 0; i < 6; i++)
    if (v3_dot(f->planes[i].normal, p) + f->planes[i].constant < 0) return false;
  return true;
}

bool frustum_hits_box(const Frustum *f, Box3 b) {
  for (int i = 0; i < 6; i++) {
    const Plane *p = &f->planes[i];
    V3 v = { p->normal.x > 0 ? b.max.x : b.min.x, p->normal.y > 0 ? b.max.y : b.min.y, p->normal.z > 0 ? b.max.z : b.min.z };
    if (v3_dot(p->normal, v) + p->constant < 0) return false;
  }
  return true;
}

static double hue2rgb(double p, double q, double t) {
  if (t < 0) t += 1;
  if (t > 1) t -= 1;
  if (t < 1.0 / 6) return p + (q - p) * 6 * t;
  if (t < 1.0 / 2) return q;
  if (t < 2.0 / 3) return p + (q - p) * 6 * (2.0 / 3 - t);
  return p;
}

Color color_hsl(double h, double s, double l) {
  h = euclid_mod(h, 1);
  s = clampd(s, 0, 1);
  l = clampd(l, 0, 1);
  if (s == 0) return (Color){ l, l, l };
  double p = l <= 0.5 ? l * (1 + s) : l + s - (l * s);
  double q = (2 * l) - p;
  return (Color){ hue2rgb(q, p, h + 1.0 / 3), hue2rgb(q, p, h), hue2rgb(q, p, h - 1.0 / 3) };
}

void color_get_hsl(Color c, double *h, double *s, double *l) {
  double r = c.r, g = c.g, b = c.b;
  double mx = fmax(r, fmax(g, b)), mn = fmin(r, fmin(g, b));
  double hue = 0, sat = 0, lig = (mn + mx) / 2.0;
  if (mn != mx) {
    double d = mx - mn;
    sat = lig <= 0.5 ? d / (mx + mn) : d / (2 - mx - mn);
    if (mx == r) hue = (g - b) / d + (g < b ? 6 : 0);
    else if (mx == g) hue = (b - r) / d + 2;
    else hue = (r - g) / d + 4;
    hue /= 6;
  }
  *h = hue; *s = sat; *l = lig;
}

uint32_t color_get_hex(Color c) {
  double r = js_round(clampd(linear_to_srgb(c.r) * 255, 0, 255));
  double g = js_round(clampd(linear_to_srgb(c.g) * 255, 0, 255));
  double b = js_round(clampd(linear_to_srgb(c.b) * 255, 0, 255));
  return (uint32_t)r * 65536u + (uint32_t)g * 256u + (uint32_t)b;
}

static int hexval(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

Color color_style(const char *st) {
  size_t n = strlen(st);
  if (st[0] == '#') {
    if (n == 4) {
      int r = hexval(st[1]), g = hexval(st[2]), b = hexval(st[3]);
      if (r >= 0 && g >= 0 && b >= 0) return color_srgb(r / 15.0, g / 15.0, b / 15.0);
    } else if (n == 7) {
      uint32_t v = 0;
      for (int i = 1; i < 7; i++) {
        int d = hexval(st[i]);
        if (d < 0) FATAL("bad colour %s", st);
        v = v * 16 + (uint32_t)d;
      }
      return color_hex(v);
    }
  } else {
    int r, g, b;
    double h, s, l;
    if (sscanf(st, "rgb(%d,%d,%d)", &r, &g, &b) == 3 || sscanf(st, "rgba(%d,%d,%d", &r, &g, &b) == 3)
      return color_srgb(imin(255, r) / 255.0, imin(255, g) / 255.0, imin(255, b) / 255.0);
    if (sscanf(st, "hsl(%lf,%lf%%,%lf%%)", &h, &s, &l) == 3) {
      Color c = color_hsl(h / 360, s / 100, l / 100);   // hsl() is sRGB: convert to working space
      return (Color){ srgb_to_linear(c.r), srgb_to_linear(c.g), srgb_to_linear(c.b) };
    }
  }
  FATAL("unsupported colour style '%s'", st);
}

// Number.prototype.toFixed(digits) read back as a number (`+x.toFixed(d)`): the n with n / 10^d
// nearest to x, an exact tie going to the larger magnitude (the spec picks the larger n for
// |x|). glibc prints the exact decimal expansion, so a tie is a '5' followed only by zeros.
double js_to_fixed(double x, int digits) {
  if (!isfinite(x)) return x;
  double a = fabs(x);
  static char buf[1200];
  snprintf(buf, sizeof buf, "%.1100f", a);
  char *dot = strchr(buf, '.');
  bool tie = dot && dot[1 + digits] == '5';
  for (const char *p = dot ? dot + 2 + digits : buf; tie && *p; p++) if (*p != '0') tie = false;
  char out[1200];
  if (tie) {
    // round the truncated value up by one unit in the last place
    snprintf(out, sizeof out, "%.*s", (int)(dot - buf) + 1 + digits, buf);
    double t = strtod(out, nullptr), unit = pow(10, -digits);
    snprintf(out, sizeof out, "%.*f", digits, t + unit);   // (t + unit is exactly representable in decimal at this precision)
  } else {
    snprintf(out, sizeof out, "%.*f", digits, a);
  }
  double v = strtod(out, nullptr);
  return x < 0 ? -v : v;
}
