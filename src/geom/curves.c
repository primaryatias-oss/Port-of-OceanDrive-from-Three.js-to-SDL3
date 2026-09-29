// CatmullRomCurve3 + Curve arc-length machinery, TubeGeometry.
#include "geom/geometry.h"

CatmullRom3 curve_catmull(const V3 *pts, int n, bool closed, CurveType type, double tension) {
  CHECK(n >= 2);
  CatmullRom3 c = { .n = n, .closed = closed, .type = type, .tension = tension, .arc_divisions = 200 };
  c.pts = xmalloc((size_t)n * sizeof(V3));
  memcpy(c.pts, pts, (size_t)n * sizeof(V3));
  return c;
}

void curve_free(CatmullRom3 *c) {
  free(c->pts);
  free(c->lengths);
  *c = (CatmullRom3){};
}

typedef struct Cubic { double c0, c1, c2, c3; } Cubic;
static Cubic cubic_init(double x0, double x1, double t0, double t1) {
  return (Cubic){ x0, t0, -3 * x0 + 3 * x1 - 2 * t0 - t1, 2 * x0 - 2 * x1 + t0 + t1 };
}
static Cubic cubic_catmull(double x0, double x1, double x2, double x3, double tension) {
  return cubic_init(x1, x2, tension * (x2 - x0), tension * (x3 - x1));
}
static Cubic cubic_nonuniform(double x0, double x1, double x2, double x3, double dt0, double dt1, double dt2) {
  double t1 = (x1 - x0) / dt0 - (x2 - x0) / (dt0 + dt1) + (x2 - x1) / dt1;
  double t2 = (x2 - x1) / dt1 - (x3 - x1) / (dt1 + dt2) + (x3 - x2) / dt2;
  t1 *= dt1;
  t2 *= dt1;
  return cubic_init(x1, x2, t1, t2);
}
static double cubic_calc(Cubic p, double t) {
  double t2 = t * t, t3 = t2 * t;
  return p.c0 + p.c1 * t + p.c2 * t2 + p.c3 * t3;
}

V3 curve_point(const CatmullRom3 *c, double t) {
  const V3 *points = c->pts;
  int l = c->n;
  double p = (l - (c->closed ? 0 : 1)) * t;
  double ip = floor(p);
  double weight = p - ip;
  long intPoint = (long)ip;
  if (c->closed) {
    intPoint += intPoint > 0 ? 0 : ((long)floor(labs(intPoint) / (double)l) + 1) * l;
  } else if (weight == 0 && intPoint == l - 1) {
    intPoint = l - 2;
    weight = 1;
  }
  V3 p0, p3;
  if (c->closed || intPoint > 0) p0 = points[(intPoint - 1) % l];
  else p0 = v3_add(v3_sub(points[0], points[1]), points[0]);
  V3 p1 = points[intPoint % l];
  V3 p2 = points[(intPoint + 1) % l];
  if (c->closed || intPoint + 2 < l) p3 = points[(intPoint + 2) % l];
  else p3 = v3_add(v3_sub(points[l - 1], points[l - 2]), points[l - 1]);

  Cubic px, py, pz;
  if (c->type == CURVE_CENTRIPETAL || c->type == CURVE_CHORDAL) {
    double pw = c->type == CURVE_CHORDAL ? 0.5 : 0.25;
    double dt0 = pow(v3_dist_sq(p0, p1), pw);
    double dt1 = pow(v3_dist_sq(p1, p2), pw);
    double dt2 = pow(v3_dist_sq(p2, p3), pw);
    if (dt1 < 1e-4) dt1 = 1.0;
    if (dt0 < 1e-4) dt0 = dt1;
    if (dt2 < 1e-4) dt2 = dt1;
    px = cubic_nonuniform(p0.x, p1.x, p2.x, p3.x, dt0, dt1, dt2);
    py = cubic_nonuniform(p0.y, p1.y, p2.y, p3.y, dt0, dt1, dt2);
    pz = cubic_nonuniform(p0.z, p1.z, p2.z, p3.z, dt0, dt1, dt2);
  } else {
    px = cubic_catmull(p0.x, p1.x, p2.x, p3.x, c->tension);
    py = cubic_catmull(p0.y, p1.y, p2.y, p3.y, c->tension);
    pz = cubic_catmull(p0.z, p1.z, p2.z, p3.z, c->tension);
  }
  return v3(cubic_calc(px, weight), cubic_calc(py, weight), cubic_calc(pz, weight));
}

static const double *curve_lengths(CatmullRom3 *c) {
  if (c->lengths) return c->lengths;
  int d = c->arc_divisions;
  c->lengths = xmalloc((size_t)(d + 1) * sizeof(double));
  V3 last = curve_point(c, 0);
  double sum = 0;
  c->lengths[0] = 0;
  for (int p = 1; p <= d; p++) {
    V3 cur = curve_point(c, (double)p / d);
    sum += v3_dist(cur, last);
    c->lengths[p] = sum;
    last = cur;
  }
  return c->lengths;
}

double curve_length(CatmullRom3 *c) { return curve_lengths(c)[c->arc_divisions]; }

static double curve_u_to_t(CatmullRom3 *c, double u) {
  const double *arc = curve_lengths(c);
  int il = c->arc_divisions + 1;
  double target = u * arc[il - 1];
  int low = 0, high = il - 1, i = 0;
  while (low <= high) {
    i = (int)floor(low + (high - low) / 2.0);
    double cmp = arc[i] - target;
    if (cmp < 0) low = i + 1;
    else if (cmp > 0) high = i - 1;
    else { high = i; break; }
  }
  i = high;
  if (arc[i] == target) return (double)i / (il - 1);
  double before = arc[i], after = arc[i + 1];
  double seg = after - before;
  double frac = (target - before) / seg;
  return (i + frac) / (il - 1);
}

V3 curve_point_at(CatmullRom3 *c, double u) { return curve_point(c, curve_u_to_t(c, u)); }

void curve_points(const CatmullRom3 *c, int divisions, V3 *out) {
  for (int d = 0; d <= divisions; d++) out[d] = curve_point(c, (double)d / divisions);
}

static V3 curve_tangent(const CatmullRom3 *c, double t) {
  double delta = 0.0001, t1 = t - delta, t2 = t + delta;
  if (t1 < 0) t1 = 0;
  if (t2 > 1) t2 = 1;
  return v3_norm(v3_sub(curve_point(c, t2), curve_point(c, t1)));
}

Frames curve_frenet(CatmullRom3 *c, int segments, bool closed) {
  Frames f;
  size_t n = (size_t)segments + 1;
  f.tangents = xmalloc(n * sizeof(V3));
  f.normals = xmalloc(n * sizeof(V3));
  f.binormals = xmalloc(n * sizeof(V3));
  for (int i = 0; i <= segments; i++) f.tangents[i] = curve_tangent(c, curve_u_to_t(c, (double)i / segments));
  double mn = 1.7976931348623157e308;
  double tx = fabs(f.tangents[0].x), ty = fabs(f.tangents[0].y), tz = fabs(f.tangents[0].z);
  V3 normal = v3s(0);
  if (tx <= mn) { mn = tx; normal = v3(1, 0, 0); }
  if (ty <= mn) { mn = ty; normal = v3(0, 1, 0); }
  if (tz <= mn) normal = v3(0, 0, 1);
  V3 vec = v3_norm(v3_cross(f.tangents[0], normal));
  f.normals[0] = v3_cross(f.tangents[0], vec);
  f.binormals[0] = v3_cross(f.tangents[0], f.normals[0]);
  for (int i = 1; i <= segments; i++) {
    f.normals[i] = f.normals[i - 1];
    f.binormals[i] = f.binormals[i - 1];
    vec = v3_cross(f.tangents[i - 1], f.tangents[i]);
    if (v3_len(vec) > 2.220446049250313e-16) {
      vec = v3_norm(vec);
      double theta = acos(clampd(v3_dot(f.tangents[i - 1], f.tangents[i]), -1, 1));
      f.normals[i] = v3_apply_m4(f.normals[i], m4_rotation_axis(vec, theta));
    }
    f.binormals[i] = v3_cross(f.tangents[i], f.normals[i]);
  }
  if (closed) {
    double theta = acos(clampd(v3_dot(f.normals[0], f.normals[segments]), -1, 1));
    theta /= segments;
    if (v3_dot(f.tangents[0], v3_cross(f.normals[0], f.normals[segments])) > 0) theta = -theta;
    for (int i = 1; i <= segments; i++) {
      f.normals[i] = v3_apply_m4(f.normals[i], m4_rotation_axis(f.tangents[i], theta * i));
      f.binormals[i] = v3_cross(f.tangents[i], f.normals[i]);
    }
  }
  return f;
}

void frames_free(Frames *f) {
  free(f->tangents);
  free(f->normals);
  free(f->binormals);
  *f = (Frames){};
}

Geometry *geo_tube(CatmullRom3 *path, int tubular, double radius, int radial, bool closed) {
  Frames fr = curve_frenet(path, tubular, closed);
  DVec pos = {}, nor = {}, uv = {};
  U32Vec idx = {};
  for (int s = 0; s <= tubular; s++) {
    int i = s < tubular ? s : (closed ? 0 : tubular);
    V3 P = curve_point_at(path, (double)i / tubular);
    V3 N = fr.normals[i], B = fr.binormals[i];
    for (int j = 0; j <= radial; j++) {
      double v = (double)j / radial * PI_D * 2;
      double sn = sin(v), cs = -cos(v);
      V3 n = v3_norm(v3(cs * N.x + sn * B.x, cs * N.y + sn * B.y, cs * N.z + sn * B.z));
      vec_push(&nor, n.x); vec_push(&nor, n.y); vec_push(&nor, n.z);
      vec_push(&pos, P.x + radius * n.x); vec_push(&pos, P.y + radius * n.y); vec_push(&pos, P.z + radius * n.z);
    }
  }
  for (int i = 0; i <= tubular; i++)
    for (int j = 0; j <= radial; j++) {
      vec_push(&uv, (double)i / tubular);
      vec_push(&uv, (double)j / radial);
    }
  for (int j = 1; j <= tubular; j++)
    for (int i = 1; i <= radial; i++) {
      uint32_t a = (uint32_t)((radial + 1) * (j - 1) + (i - 1)), b = (uint32_t)((radial + 1) * j + (i - 1));
      uint32_t c = (uint32_t)((radial + 1) * j + i), d = (uint32_t)((radial + 1) * (j - 1) + i);
      vec_push(&idx, a); vec_push(&idx, b); vec_push(&idx, d);
      vec_push(&idx, b); vec_push(&idx, c); vec_push(&idx, d);
    }
  frames_free(&fr);
  Geometry *g = geo_new();
  int n = (int)(pos.len / 3);
  geo_set_index(g, idx.data, (int)idx.len);
  geo_set_attr_d(g, "position", 3, n, pos.data);
  geo_set_attr_d(g, "normal", 3, n, nor.data);
  geo_set_attr_d(g, "uv", 2, n, uv.data);
  vec_free(&pos); vec_free(&nor); vec_free(&uv); vec_free(&idx);
  return g;
}
