// Primitive generators, ported line for line from three.js (same vertex order, so scene code
// that edits vertices by index behaves identically).
#include "geom/geometry.h"

typedef struct Build { DVec pos, nor, uv; U32Vec idx; } Build;

static void push3(DVec *v, double x, double y, double z) {
  vec_reserve(v, v->len + 3);
  v->data[v->len++] = x; v->data[v->len++] = y; v->data[v->len++] = z;
}
static void push2(DVec *v, double x, double y) {
  vec_reserve(v, v->len + 2);
  v->data[v->len++] = x; v->data[v->len++] = y;
}
static void tri(U32Vec *v, uint32_t a, uint32_t b, uint32_t c) {
  vec_reserve(v, v->len + 3);
  v->data[v->len++] = a; v->data[v->len++] = b; v->data[v->len++] = c;
}

// setIndex + position/normal/uv, in the attribute order given
static Geometry *finish(Build *b, bool indexed, const char *order) {
  Geometry *g = geo_new();
  int n = (int)(b->pos.len / 3);
  if (indexed) geo_set_index(g, b->idx.data, (int)b->idx.len);
  for (const char *o = order; *o; o++) {
    if (*o == 'p') geo_set_attr_d(g, "position", 3, n, b->pos.data);
    if (*o == 'n') geo_set_attr_d(g, "normal", 3, n, b->nor.data);
    if (*o == 'u') geo_set_attr_d(g, "uv", 2, n, b->uv.data);
  }
  vec_free(&b->pos); vec_free(&b->nor); vec_free(&b->uv); vec_free(&b->idx);
  return g;
}

// ---- Box -------------------------------------------------------------------------------

static void box_plane(Build *b, int *nverts, int u, int v, int w, double udir, double vdir,
                      double width, double height, double depth, int gridX, int gridY) {
  double sw = width / gridX, sh = height / gridY;
  double wh = width / 2, hh = height / 2, dh = depth / 2;
  int gx1 = gridX + 1, gy1 = gridY + 1, counter = 0;
  for (int iy = 0; iy < gy1; iy++) {
    double y = iy * sh - hh;
    for (int ix = 0; ix < gx1; ix++) {
      double x = ix * sw - wh;
      double vec[3];
      vec[u] = x * udir; vec[v] = y * vdir; vec[w] = dh;
      push3(&b->pos, vec[0], vec[1], vec[2]);
      vec[u] = 0; vec[v] = 0; vec[w] = depth > 0 ? 1 : -1;
      push3(&b->nor, vec[0], vec[1], vec[2]);
      push2(&b->uv, (double)ix / gridX, 1 - ((double)iy / gridY));
      counter++;
    }
  }
  for (int iy = 0; iy < gridY; iy++)
    for (int ix = 0; ix < gridX; ix++) {
      uint32_t a = (uint32_t)(*nverts + ix + gx1 * iy);
      uint32_t bb = (uint32_t)(*nverts + ix + gx1 * (iy + 1));
      uint32_t c = (uint32_t)(*nverts + (ix + 1) + gx1 * (iy + 1));
      uint32_t d = (uint32_t)(*nverts + (ix + 1) + gx1 * iy);
      tri(&b->idx, a, bb, d);
      tri(&b->idx, bb, c, d);
    }
  *nverts += counter;
}

Geometry *geo_box(double w, double h, double d, int ws, int hs, int ds) {
  Build b = {};
  int nv = 0;
  enum { X, Y, Z };
  box_plane(&b, &nv, Z, Y, X, -1, -1, d, h, w, ds, hs);    // px
  box_plane(&b, &nv, Z, Y, X, 1, -1, d, h, -w, ds, hs);    // nx
  box_plane(&b, &nv, X, Z, Y, 1, 1, w, d, h, ws, ds);      // py
  box_plane(&b, &nv, X, Z, Y, 1, -1, w, d, -h, ws, ds);    // ny
  box_plane(&b, &nv, X, Y, Z, 1, -1, w, h, d, ws, hs);     // pz
  box_plane(&b, &nv, X, Y, Z, -1, -1, w, h, -d, ws, hs);   // nz
  return finish(&b, true, "pnu");
}

// ---- Plane -----------------------------------------------------------------------------

Geometry *geo_plane(double width, double height, int ws, int hs) {
  Build b = {};
  double wh = width / 2, hh = height / 2;
  int gx = ws, gy = hs, gx1 = gx + 1, gy1 = gy + 1;
  double sw = width / gx, sh = height / gy;
  for (int iy = 0; iy < gy1; iy++) {
    double y = iy * sh - hh;
    for (int ix = 0; ix < gx1; ix++) {
      double x = ix * sw - wh;
      push3(&b.pos, x, -y, 0);
      push3(&b.nor, 0, 0, 1);
      push2(&b.uv, (double)ix / gx, 1 - ((double)iy / gy));
    }
  }
  for (int iy = 0; iy < gy; iy++)
    for (int ix = 0; ix < gx; ix++) {
      uint32_t a = (uint32_t)(ix + gx1 * iy), bb = (uint32_t)(ix + gx1 * (iy + 1));
      uint32_t c = (uint32_t)((ix + 1) + gx1 * (iy + 1)), d = (uint32_t)((ix + 1) + gx1 * iy);
      tri(&b.idx, a, bb, d);
      tri(&b.idx, bb, c, d);
    }
  return finish(&b, true, "pnu");
}

// ---- Cylinder / Cone -------------------------------------------------------------------

Geometry *geo_cylinder(double rt, double rb, double height, int radial, int hsegs, bool open,
                       double ts, double tl) {
  Build b = {};
  uint32_t index = 0;
  double half = height / 2;
  uint32_t *grid = xmalloc((size_t)(hsegs + 1) * (size_t)(radial + 1) * sizeof(uint32_t));
  double slope = (rb - rt) / height;
  for (int y = 0; y <= hsegs; y++) {
    double v = (double)y / hsegs;
    double radius = v * (rb - rt) + rt;
    for (int x = 0; x <= radial; x++) {
      double u = (double)x / radial;
      double theta = u * tl + ts;
      double s = sin(theta), c = cos(theta);
      push3(&b.pos, radius * s, -v * height + half, radius * c);
      V3 n = v3_norm(v3(s, slope, c));
      push3(&b.nor, n.x, n.y, n.z);
      push2(&b.uv, u, 1 - v);
      grid[y * (radial + 1) + x] = index++;
    }
  }
  for (int x = 0; x < radial; x++)
    for (int y = 0; y < hsegs; y++) {
      uint32_t a = grid[y * (radial + 1) + x], bb = grid[(y + 1) * (radial + 1) + x];
      uint32_t c = grid[(y + 1) * (radial + 1) + x + 1], d = grid[y * (radial + 1) + x + 1];
      if (rt > 0 || y != 0) tri(&b.idx, a, bb, d);
      if (rb > 0 || y != hsegs - 1) tri(&b.idx, bb, c, d);
    }
  free(grid);
  if (!open) {
    for (int cap = 0; cap < 2; cap++) {
      bool top = cap == 0;
      double radius = top ? rt : rb;
      if (!(radius > 0)) continue;
      double sign = top ? 1 : -1;
      uint32_t center_start = index;
      for (int x = 1; x <= radial; x++) {
        push3(&b.pos, 0, half * sign, 0);
        push3(&b.nor, 0, sign, 0);
        push2(&b.uv, 0.5, 0.5);
        index++;
      }
      uint32_t center_end = index;
      for (int x = 0; x <= radial; x++) {
        double u = (double)x / radial;
        double theta = u * tl + ts;
        double c = cos(theta), s = sin(theta);
        push3(&b.pos, radius * s, half * sign, radius * c);
        push3(&b.nor, 0, sign, 0);
        push2(&b.uv, (c * 0.5) + 0.5, (s * 0.5 * sign) + 0.5);
        index++;
      }
      for (int x = 0; x < radial; x++) {
        uint32_t c = center_start + (uint32_t)x, i = center_end + (uint32_t)x;
        if (top) tri(&b.idx, i, i + 1, c);
        else tri(&b.idx, i + 1, i, c);
      }
    }
  }
  return finish(&b, true, "pnu");
}

Geometry *geo_cone(double r, double h, int radial, int hsegs, bool open, double ts, double tl) {
  return geo_cylinder(0, r, h, radial, hsegs, open, ts, tl);
}

// ---- Sphere ----------------------------------------------------------------------------

Geometry *geo_sphere(double radius, int ws, int hs, double phi_start, double phi_len, double theta_start, double theta_len) {
  ws = imax(3, ws);
  hs = imax(2, hs);
  double theta_end = fmin(theta_start + theta_len, PI_D);
  Build b = {};
  uint32_t index = 0;
  uint32_t *grid = xmalloc((size_t)(hs + 1) * (size_t)(ws + 1) * sizeof(uint32_t));
  for (int iy = 0; iy <= hs; iy++) {
    double v = (double)iy / hs;
    double theta = theta_start + v * theta_len;
    double y = radius * cos(theta);
    double ring = sqrt(radius * radius - y * y);
    double uoff = 0;
    if (iy == 0 && theta_start == 0) uoff = 0.5 / ws;
    else if (iy == hs && theta_end == PI_D) uoff = -0.5 / ws;
    for (int ix = 0; ix <= ws; ix++) {
      double u = (double)ix / ws;
      double phi = phi_start + u * phi_len;
      V3 vx = v3(-ring * cos(phi), y, ring * sin(phi));
      push3(&b.pos, vx.x, vx.y, vx.z);
      V3 n = v3_norm(vx);
      push3(&b.nor, n.x, n.y, n.z);
      push2(&b.uv, u + uoff, 1 - v);
      grid[iy * (ws + 1) + ix] = index++;
    }
  }
  for (int iy = 0; iy < hs; iy++)
    for (int ix = 0; ix < ws; ix++) {
      uint32_t a = grid[iy * (ws + 1) + ix + 1], bb = grid[iy * (ws + 1) + ix];
      uint32_t c = grid[(iy + 1) * (ws + 1) + ix], d = grid[(iy + 1) * (ws + 1) + ix + 1];
      if (iy != 0 || theta_start > 0) tri(&b.idx, a, bb, d);
      if (iy != hs - 1 || theta_end < PI_D) tri(&b.idx, bb, c, d);
    }
  free(grid);
  return finish(&b, true, "pnu");
}

// ---- Torus -----------------------------------------------------------------------------

Geometry *geo_torus(double radius, double tube, int radial, int tubular, double arc, double ts, double tl) {
  Build b = {};
  for (int j = 0; j <= radial; j++) {
    double v = ts + ((double)j / radial) * tl;
    for (int i = 0; i <= tubular; i++) {
      double u = (double)i / tubular * arc;
      V3 vx = v3((radius + tube * cos(v)) * cos(u), (radius + tube * cos(v)) * sin(u), tube * sin(v));
      push3(&b.pos, vx.x, vx.y, vx.z);
      V3 c = v3(radius * cos(u), radius * sin(u), 0);
      V3 n = v3_norm(v3_sub(vx, c));
      push3(&b.nor, n.x, n.y, n.z);
      push2(&b.uv, (double)i / tubular, (double)j / radial);
    }
  }
  for (int j = 1; j <= radial; j++)
    for (int i = 1; i <= tubular; i++) {
      uint32_t a = (uint32_t)((tubular + 1) * j + i - 1), bb = (uint32_t)((tubular + 1) * (j - 1) + i - 1);
      uint32_t c = (uint32_t)((tubular + 1) * (j - 1) + i), d = (uint32_t)((tubular + 1) * j + i);
      tri(&b.idx, a, bb, d);
      tri(&b.idx, bb, c, d);
    }
  return finish(&b, true, "pnu");
}

// ---- Circle ----------------------------------------------------------------------------

Geometry *geo_circle(double radius, int segs, double ts, double tl) {
  segs = imax(3, segs);
  Build b = {};
  push3(&b.pos, 0, 0, 0);
  push3(&b.nor, 0, 0, 1);
  push2(&b.uv, 0.5, 0.5);
  for (int s = 0, i = 3; s <= segs; s++, i += 3) {
    double seg = ts + (double)s / segs * tl;
    push3(&b.pos, radius * cos(seg), radius * sin(seg), 0);
    push3(&b.nor, 0, 0, 1);
    push2(&b.uv, (b.pos.data[i] / radius + 1) / 2, (b.pos.data[i + 1] / radius + 1) / 2);
  }
  for (int i = 1; i <= segs; i++) tri(&b.idx, (uint32_t)i, (uint32_t)i + 1, 0);
  return finish(&b, true, "pnu");
}

// ---- Lathe -----------------------------------------------------------------------------

Geometry *geo_lathe(const V2 *pts, int n, int segs, double phi_start, double phi_len) {
  CHECK(n >= 2);
  phi_len = clampd(phi_len, 0, PI_D * 2);
  Build b = {};
  double *init = xmalloc((size_t)n * 3 * sizeof(double));
  double inv = 1.0 / segs;
  V3 prev = v3s(0);
  for (int j = 0; j <= n - 1; j++) {
    if (j == 0) {
      double dx = pts[j + 1].x - pts[j].x, dy = pts[j + 1].y - pts[j].y;
      V3 nm = v3(dy * 1.0, -dx, dy * 0.0);
      prev = nm;
      nm = v3_norm(nm);
      init[3 * j] = nm.x; init[3 * j + 1] = nm.y; init[3 * j + 2] = nm.z;
    } else if (j == n - 1) {
      init[3 * j] = prev.x; init[3 * j + 1] = prev.y; init[3 * j + 2] = prev.z;
    } else {
      double dx = pts[j + 1].x - pts[j].x, dy = pts[j + 1].y - pts[j].y;
      V3 nm = v3(dy * 1.0, -dx, dy * 0.0);
      V3 cur = nm;
      nm = v3_norm(v3_add(nm, prev));
      init[3 * j] = nm.x; init[3 * j + 1] = nm.y; init[3 * j + 2] = nm.z;
      prev = cur;
    }
  }
  for (int i = 0; i <= segs; i++) {
    double phi = phi_start + i * inv * phi_len;
    double s = sin(phi), c = cos(phi);
    for (int j = 0; j <= n - 1; j++) {
      push3(&b.pos, pts[j].x * s, pts[j].y, pts[j].x * c);
      push2(&b.uv, (double)i / segs, (double)j / (n - 1));
      push3(&b.nor, init[3 * j] * s, init[3 * j + 1], init[3 * j] * c);
    }
  }
  free(init);
  for (int i = 0; i < segs; i++)
    for (int j = 0; j < n - 1; j++) {
      uint32_t base = (uint32_t)(j + i * n);
      uint32_t a = base, bb = base + (uint32_t)n, c = base + (uint32_t)n + 1, d = base + 1;
      tri(&b.idx, a, bb, d);
      tri(&b.idx, c, d, bb);
    }
  return finish(&b, true, "pun");
}

// ---- Capsule ---------------------------------------------------------------------------

Geometry *geo_capsule(double radius, double height, int cap_segs, int radial, int hsegs) {
  height = fmax(0, height);
  cap_segs = imax(1, cap_segs);
  radial = imax(3, radial);
  hsegs = imax(1, hsegs);
  Build b = {};
  double half = height / 2;
  double cap_arc = (PI_D / 2) * radius, cyl = height, total = 2 * cap_arc + cyl;
  int nvert = cap_segs * 2 + hsegs, per_row = radial + 1;
  for (int iy = 0; iy <= nvert; iy++) {
    double arc = 0, py = 0, pr = 0, ny = 0;
    if (iy <= cap_segs) {
      double sp = (double)iy / cap_segs, ang = (sp * PI_D) / 2;
      py = -half - radius * cos(ang);
      pr = radius * sin(ang);
      ny = -radius * cos(ang);
      arc = sp * cap_arc;
    } else if (iy <= cap_segs + hsegs) {
      double sp = (double)(iy - cap_segs) / hsegs;
      py = -half + sp * height;
      pr = radius;
      ny = 0;
      arc = cap_arc + sp * cyl;
    } else {
      double sp = (double)(iy - cap_segs - hsegs) / cap_segs, ang = (sp * PI_D) / 2;
      py = half + radius * sin(ang);
      pr = radius * cos(ang);
      ny = radius * sin(ang);
      arc = cap_arc + cyl + sp * cap_arc;
    }
    double v = fmax(0, fmin(1, arc / total));
    double uoff = 0;
    if (iy == 0) uoff = 0.5 / radial;
    else if (iy == nvert) uoff = -0.5 / radial;
    for (int ix = 0; ix <= radial; ix++) {
      double u = (double)ix / radial, theta = u * PI_D * 2;
      double s = sin(theta), c = cos(theta);
      push3(&b.pos, -pr * c, py, pr * s);
      V3 n = v3_norm(v3(-pr * c, ny, pr * s));
      push3(&b.nor, n.x, n.y, n.z);
      push2(&b.uv, u + uoff, v);
    }
    if (iy > 0) {
      int prow = (iy - 1) * per_row;
      for (int ix = 0; ix < radial; ix++) {
        uint32_t i1 = (uint32_t)(prow + ix), i2 = (uint32_t)(prow + ix + 1);
        uint32_t i3 = (uint32_t)(iy * per_row + ix), i4 = (uint32_t)(iy * per_row + ix + 1);
        tri(&b.idx, i1, i2, i3);
        tri(&b.idx, i2, i4, i3);
      }
    }
  }
  return finish(&b, true, "pnu");
}

// ---- Polyhedron / Icosahedron ----------------------------------------------------------

static double azimuth(V3 v) { return atan2(v.z, -v.x); }
static double inclination(V3 v) { return atan2(-v.y, sqrt((v.x * v.x) + (v.z * v.z))); }

Geometry *geo_polyhedron(const double *verts, int nverts, const int *indices, int nidx, double radius, int detail) {
  (void)nverts;
  DVec vb = {}, uvb = {};
  int cols = detail + 1;
  V3 *grid = xmalloc((size_t)(cols + 1) * (size_t)(cols + 1) * sizeof(V3));
#define G(i, j) grid[(i) * (cols + 1) + (j)]
  for (int f = 0; f < nidx; f += 3) {
    V3 a = v3(verts[indices[f] * 3], verts[indices[f] * 3 + 1], verts[indices[f] * 3 + 2]);
    V3 bv = v3(verts[indices[f + 1] * 3], verts[indices[f + 1] * 3 + 1], verts[indices[f + 1] * 3 + 2]);
    V3 c = v3(verts[indices[f + 2] * 3], verts[indices[f + 2] * 3 + 1], verts[indices[f + 2] * 3 + 2]);
    for (int i = 0; i <= cols; i++) {
      V3 aj = v3_lerp(a, c, (double)i / cols), bj = v3_lerp(bv, c, (double)i / cols);
      int rows = cols - i;
      for (int j = 0; j <= rows; j++) G(i, j) = (j == 0 && i == cols) ? aj : v3_lerp(aj, bj, (double)j / rows);
    }
    for (int i = 0; i < cols; i++)
      for (int j = 0; j < 2 * (cols - i) - 1; j++) {
        int k = j / 2;
        V3 p0, p1, p2;
        if (j % 2 == 0) { p0 = G(i, k + 1); p1 = G(i + 1, k); p2 = G(i, k); }
        else { p0 = G(i, k + 1); p1 = G(i + 1, k + 1); p2 = G(i + 1, k); }
        push3(&vb, p0.x, p0.y, p0.z);
        push3(&vb, p1.x, p1.y, p1.z);
        push3(&vb, p2.x, p2.y, p2.z);
      }
  }
#undef G
  free(grid);
  for (size_t i = 0; i < vb.len; i += 3) {   // applyRadius
    V3 v = v3_scale(v3_norm(v3(vb.data[i], vb.data[i + 1], vb.data[i + 2])), radius);
    vb.data[i] = v.x; vb.data[i + 1] = v.y; vb.data[i + 2] = v.z;
  }
  for (size_t i = 0; i < vb.len; i += 3) {   // generateUVs
    V3 v = v3(vb.data[i], vb.data[i + 1], vb.data[i + 2]);
    double u = azimuth(v) / 2 / PI_D + 0.5, vv = inclination(v) / PI_D + 0.5;
    push2(&uvb, u, 1 - vv);
  }
  for (size_t i = 0, j = 0; i < vb.len; i += 9, j += 6) {   // correctUVs
    V3 a = v3(vb.data[i], vb.data[i + 1], vb.data[i + 2]);
    V3 bb = v3(vb.data[i + 3], vb.data[i + 4], vb.data[i + 5]);
    V3 c = v3(vb.data[i + 6], vb.data[i + 7], vb.data[i + 8]);
    double uvs[3] = { uvb.data[j], uvb.data[j + 2], uvb.data[j + 4] };
    V3 centroid = v3_scale(v3_add(v3_add(a, bb), c), 1.0 / 3);
    double azi = azimuth(centroid);
    V3 pv[3] = { a, bb, c };
    for (int k = 0; k < 3; k++) {
      size_t stride = j + (size_t)k * 2;
      if (azi < 0 && uvs[k] == 1) uvb.data[stride] = uvs[k] - 1;
      if (pv[k].x == 0 && pv[k].z == 0) uvb.data[stride] = azi / 2 / PI_D + 0.5;
    }
  }
  for (size_t i = 0; i < uvb.len; i += 6) {   // correctSeam
    double x0 = uvb.data[i], x1 = uvb.data[i + 2], x2 = uvb.data[i + 4];
    double mx = fmax(x0, fmax(x1, x2)), mn = fmin(x0, fmin(x1, x2));
    if (mx > 0.9 && mn < 0.1) {
      if (x0 < 0.2) uvb.data[i] += 1;
      if (x1 < 0.2) uvb.data[i + 2] += 1;
      if (x2 < 0.2) uvb.data[i + 4] += 1;
    }
  }
  Geometry *g = geo_new();
  int n = (int)(vb.len / 3);
  geo_set_attr_d(g, "position", 3, n, vb.data);
  geo_set_attr_d(g, "normal", 3, n, vb.data);
  geo_set_attr_d(g, "uv", 2, n, uvb.data);
  vec_free(&vb);
  vec_free(&uvb);
  if (detail == 0) geo_compute_vertex_normals(g);
  else geo_normalize_normals(g);
  return g;
}

Geometry *geo_icosahedron(double radius, int detail) {
  double t = (1 + sqrt(5)) / 2;
  const double v[] = { -1, t, 0, 1, t, 0, -1, -t, 0, 1, -t, 0, 0, -1, t, 0, 1, t,
                       0, -1, -t, 0, 1, -t, t, 0, -1, t, 0, 1, -t, 0, -1, -t, 0, 1 };
  static const int idx[] = { 0, 11, 5, 0, 5, 1, 0, 1, 7, 0, 7, 10, 0, 10, 11, 1, 5, 9, 5, 11, 4,
                             11, 10, 2, 10, 7, 6, 7, 1, 8, 3, 9, 4, 3, 4, 2, 3, 2, 6, 3, 6, 8,
                             3, 8, 9, 4, 9, 5, 2, 4, 11, 6, 2, 10, 8, 6, 7, 9, 8, 1 };
  return geo_polyhedron(v, 12, idx, 60, radius, detail);
}

// ---- RoundedBox (examples/jsm/geometries/RoundedBoxGeometry.js) -------------------------

static double angle_to(V3 a, V3 b) {
  double den = sqrt(v3_len_sq(a) * v3_len_sq(b));
  if (den == 0) return PI_D / 2;
  return acos(clampd(v3_dot(a, b) / den, -1, 1));
}

static double rb_uv(V3 face_dir, V3 normal, int uv_axis, int proj_axis, double radius, double side_len) {
  double tot_arc = 2 * PI_D * radius / 4;
  double center = fmax(side_len - 2 * radius, 0);
  double half_arc = PI_D / 4;
  double t[3] = { normal.x, normal.y, normal.z };
  t[proj_axis] = 0;
  V3 tn = v3_norm(v3(t[0], t[1], t[2]));
  double arc_uv = 0.5 * tot_arc / (tot_arc + center);
  double arc_angle = 1.0 - (angle_to(tn, face_dir) / half_arc);
  if (js_sign(v3_comp(tn, uv_axis)) == 1) return arc_angle * arc_uv;
  double len_uv = center / (tot_arc + center);
  return len_uv + arc_uv + arc_uv * (1.0 - arc_angle);
}

Geometry *geo_rounded_box(double width, double height, double depth, int segs, double radius) {
  int total = segs * 2 + 1;
  radius = fmin(fmin(width / 2, height / 2), fmin(depth / 2, radius));
  Geometry *box = geo_box(1, 1, 1, total, total, total);
  if (total == 1) return box;
  Geometry *g = geo_to_non_indexed(box);
  geo_free(box);
  V3 bx = v3_sub(v3_scale(v3(width, height, depth), 1.0 / 2), v3s(radius));
  float *pos = geo_data(g, "position"), *nor = geo_data(g, "normal"), *uv = geo_data(g, "uv");
  int plen = g->count * 3;
  int face_tris = plen / 6;
  double half_seg = 0.5 / total;
  enum { X, Y, Z };
  for (int i = 0, j = 0; i < plen; i += 3, j += 2) {
    V3 p = v3(pos[i], pos[i + 1], pos[i + 2]);
    V3 n = p;
    n.x -= js_sign(n.x) * half_seg;
    n.y -= js_sign(n.y) * half_seg;
    n.z -= js_sign(n.z) * half_seg;
    n = v3_norm(n);
    pos[i] = (float)(bx.x * js_sign(p.x) + n.x * radius);
    pos[i + 1] = (float)(bx.y * js_sign(p.y) + n.y * radius);
    pos[i + 2] = (float)(bx.z * js_sign(p.z) + n.z * radius);
    nor[i] = (float)n.x; nor[i + 1] = (float)n.y; nor[i + 2] = (float)n.z;
    int side = i / face_tris;
    switch (side) {
    case 0: uv[j] = (float)rb_uv(v3(1, 0, 0), n, Z, Y, radius, depth);
            uv[j + 1] = (float)(1.0 - rb_uv(v3(1, 0, 0), n, Y, Z, radius, height)); break;
    case 1: uv[j] = (float)(1.0 - rb_uv(v3(-1, 0, 0), n, Z, Y, radius, depth));
            uv[j + 1] = (float)(1.0 - rb_uv(v3(-1, 0, 0), n, Y, Z, radius, height)); break;
    case 2: uv[j] = (float)(1.0 - rb_uv(v3(0, 1, 0), n, X, Z, radius, width));
            uv[j + 1] = (float)rb_uv(v3(0, 1, 0), n, Z, X, radius, depth); break;
    case 3: uv[j] = (float)(1.0 - rb_uv(v3(0, -1, 0), n, X, Z, radius, width));
            uv[j + 1] = (float)(1.0 - rb_uv(v3(0, -1, 0), n, Z, X, radius, depth)); break;
    case 4: uv[j] = (float)(1.0 - rb_uv(v3(0, 0, 1), n, X, Y, radius, width));
            uv[j + 1] = (float)(1.0 - rb_uv(v3(0, 0, 1), n, Y, X, radius, height)); break;
    case 5: uv[j] = (float)rb_uv(v3(0, 0, -1), n, X, Y, radius, width);
            uv[j + 1] = (float)(1.0 - rb_uv(v3(0, 0, -1), n, Y, X, radius, height)); break;
    }
  }
  return g;
}
