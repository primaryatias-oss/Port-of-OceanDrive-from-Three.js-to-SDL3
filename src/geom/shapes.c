// Path / Shape (lines and ellipse arcs), ShapeUtils + earcut, ShapeGeometry, ExtrudeGeometry.
#include "geom/geometry.h"

// ---- Path --------------------------------------------------------------------------------

Path path_new(void) { return (Path){}; }

void path_free(Path *p) { vec_free(&p->curves); }

void path_move_to(Path *p, double x, double y) { p->current = v2(x, y); }

void path_line_to(Path *p, double x, double y) {
  vec_push(&p->curves, (PathCurve){ .kind = PC_LINE, .v1 = p->current, .v2 = v2(x, y) });
  p->current = v2(x, y);
}

Path path_from_points(const V2 *pts, int n) {
  Path p = path_new();
  path_move_to(&p, pts[0].x, pts[0].y);
  for (int i = 1; i < n; i++) path_line_to(&p, pts[i].x, pts[i].y);
  return p;
}

static V2 curve2_point(const PathCurve *c, double t) {
  if (c->kind == PC_LINE) {
    if (t == 1) return c->v2;
    return v2_add(v2_scale(v2_sub(c->v2, c->v1), t), c->v1);
  }
  const double two_pi = PI_D * 2, eps = 2.220446049250313e-16;
  double delta = c->a1 - c->a0;
  bool same = fabs(delta) < eps;
  while (delta < 0) delta += two_pi;
  while (delta > two_pi) delta -= two_pi;
  if (delta < eps) delta = same ? 0 : two_pi;
  if (c->clockwise && !same) delta = delta == two_pi ? -two_pi : delta - two_pi;
  double angle = c->a0 + t * delta;
  double x = c->ax + c->xr * cos(angle), y = c->ay + c->yr * sin(angle);
  if (c->rot != 0) {
    double cs = cos(c->rot), sn = sin(c->rot), tx = x - c->ax, ty = y - c->ay;
    x = tx * cs - ty * sn + c->ax;
    y = tx * sn + ty * cs + c->ay;
  }
  return v2(x, y);
}

void path_absellipse(Path *p, double x, double y, double xr, double yr, double a0, double a1, bool cw, double rot) {
  PathCurve c = { .kind = PC_ELLIPSE, .ax = x, .ay = y, .xr = xr, .yr = yr, .a0 = a0, .a1 = a1, .clockwise = cw, .rot = rot };
  if (p->curves.len > 0) {
    V2 first = curve2_point(&c, 0);
    if (!(first.x == p->current.x && first.y == p->current.y)) path_line_to(p, first.x, first.y);
  }
  vec_push(&p->curves, c);
  p->current = curve2_point(&c, 1);
}

void path_absarc(Path *p, double x, double y, double r, double a0, double a1, bool cw) {
  path_absellipse(p, x, y, r, r, a0, a1, cw, 0);
}

V2Vec path_get_points(const Path *p, int divisions) {
  V2Vec pts = {};
  bool has_last = false;
  V2 last = {};
  for (size_t i = 0; i < p->curves.len; i++) {
    const PathCurve *c = &p->curves.data[i];
    int res = c->kind == PC_ELLIPSE ? divisions * 2 : 1;
    for (int d = 0; d <= res; d++) {
      V2 pt = curve2_point(c, (double)d / res);
      if (has_last && last.x == pt.x && last.y == pt.y) continue;
      vec_push(&pts, pt);
      last = pt;
      has_last = true;
    }
  }
  if (p->auto_close && pts.len > 1) {
    V2 a = pts.data[pts.len - 1], b = pts.data[0];
    if (!(a.x == b.x && a.y == b.y)) vec_push(&pts, b);
  }
  return pts;
}

Shape shape_new(void) { return (Shape){}; }

Shape shape_from_points(const V2 *pts, int n) {
  Shape s = shape_new();
  s.path = path_from_points(pts, n);
  return s;
}

void shape_free(Shape *s) {
  path_free(&s->path);
  for (size_t i = 0; i < s->holes.len; i++) path_free(&s->holes.data[i]);
  vec_free(&s->holes);
}

Path *shape_add_hole(Shape *s) {
  vec_push(&s->holes, path_new());
  return &vec_last(&s->holes);
}

// ---- earcut (mapbox/earcut, as bundled by three.js) ----------------------------------------

typedef struct ENode {
  int i;
  double x, y;
  int prev, next;     // indices into the node pool (-1: null)
  int32_t z;
  int prevZ, nextZ;
  bool steiner;
} ENode;

typedef struct Earcut { Vec(ENode) n; U32Vec *tris; } Earcut;

#define N(k) (E->n.data[k])

static int e_create(Earcut *E, int i, double x, double y) {
  vec_push(&E->n, (ENode){ .i = i, .x = x, .y = y, .prev = -1, .next = -1, .z = 0, .prevZ = -1, .nextZ = -1 });
  return (int)E->n.len - 1;
}
static int e_insert(Earcut *E, int i, double x, double y, int last) {
  int p = e_create(E, i, x, y);
  if (last < 0) {
    N(p).prev = p;
    N(p).next = p;
  } else {
    N(p).next = N(last).next;
    N(p).prev = last;
    N(N(last).next).prev = p;
    N(last).next = p;
  }
  return p;
}
static void e_remove(Earcut *E, int p) {
  N(N(p).next).prev = N(p).prev;
  N(N(p).prev).next = N(p).next;
  if (N(p).prevZ >= 0) N(N(p).prevZ).nextZ = N(p).nextZ;
  if (N(p).nextZ >= 0) N(N(p).nextZ).prevZ = N(p).prevZ;
}
static double e_area(Earcut *E, int p, int q, int r) {
  return (N(q).y - N(p).y) * (N(r).x - N(q).x) - (N(q).x - N(p).x) * (N(r).y - N(q).y);
}
static bool e_equals(Earcut *E, int a, int b) { return N(a).x == N(b).x && N(a).y == N(b).y; }

static double signed_area(const double *data, int start, int end) {
  double sum = 0;
  for (int i = start, j = end - 2; i < end; i += 2) {
    sum += (data[j] - data[i]) * (data[i + 1] + data[j + 1]);
    j = i;
  }
  return sum;
}

static int e_linked_list(Earcut *E, const double *data, int start, int end, bool clockwise) {
  int last = -1;
  if (clockwise == (signed_area(data, start, end) > 0)) {
    for (int i = start; i < end; i += 2) last = e_insert(E, i / 2, data[i], data[i + 1], last);
  } else {
    for (int i = end - 2; i >= start; i -= 2) last = e_insert(E, i / 2, data[i], data[i + 1], last);
  }
  if (last >= 0 && e_equals(E, last, N(last).next)) {
    e_remove(E, last);
    last = N(last).next;
  }
  return last;
}

static int e_filter(Earcut *E, int start, int end) {
  if (start < 0) return start;
  if (end < 0) end = start;
  int p = start;
  bool again;
  do {
    again = false;
    if (!N(p).steiner && (e_equals(E, p, N(p).next) || e_area(E, N(p).prev, p, N(p).next) == 0)) {
      e_remove(E, p);
      p = end = N(p).prev;
      if (p == N(p).next) break;
      again = true;
    } else {
      p = N(p).next;
    }
  } while (again || p != end);
  return end;
}

static bool point_in_tri(double ax, double ay, double bx, double by, double cx, double cy, double px, double py) {
  return (cx - px) * (ay - py) >= (ax - px) * (cy - py) &&
         (ax - px) * (by - py) >= (bx - px) * (ay - py) &&
         (bx - px) * (cy - py) >= (cx - px) * (by - py);
}
static bool point_in_tri_ex_first(double ax, double ay, double bx, double by, double cx, double cy, double px, double py) {
  return !(ax == px && ay == py) && point_in_tri(ax, ay, bx, by, cx, cy, px, py);
}

static int32_t z_order(double x0, double y0, double minX, double minY, double invSize) {
  uint32_t x = (uint32_t)js_i32((x0 - minX) * invSize);
  uint32_t y = (uint32_t)js_i32((y0 - minY) * invSize);
  x = (x | (x << 8)) & 0x00FF00FFu;
  x = (x | (x << 4)) & 0x0F0F0F0Fu;
  x = (x | (x << 2)) & 0x33333333u;
  x = (x | (x << 1)) & 0x55555555u;
  y = (y | (y << 8)) & 0x00FF00FFu;
  y = (y | (y << 4)) & 0x0F0F0F0Fu;
  y = (y | (y << 2)) & 0x33333333u;
  y = (y | (y << 1)) & 0x55555555u;
  return (int32_t)(x | (y << 1));
}

static bool e_is_ear(Earcut *E, int ear) {
  int a = N(ear).prev, b = ear, c = N(ear).next;
  if (e_area(E, a, b, c) >= 0) return false;
  double ax = N(a).x, bx = N(b).x, cx = N(c).x, ay = N(a).y, by = N(b).y, cy = N(c).y;
  double x0 = fmin(ax, fmin(bx, cx)), y0 = fmin(ay, fmin(by, cy));
  double x1 = fmax(ax, fmax(bx, cx)), y1 = fmax(ay, fmax(by, cy));
  int p = N(c).next;
  while (p != a) {
    if (N(p).x >= x0 && N(p).x <= x1 && N(p).y >= y0 && N(p).y <= y1 &&
        point_in_tri_ex_first(ax, ay, bx, by, cx, cy, N(p).x, N(p).y) &&
        e_area(E, N(p).prev, p, N(p).next) >= 0) return false;
    p = N(p).next;
  }
  return true;
}

static bool e_is_ear_hashed(Earcut *E, int ear, double minX, double minY, double invSize) {
  int a = N(ear).prev, b = ear, c = N(ear).next;
  if (e_area(E, a, b, c) >= 0) return false;
  double ax = N(a).x, bx = N(b).x, cx = N(c).x, ay = N(a).y, by = N(b).y, cy = N(c).y;
  double x0 = fmin(ax, fmin(bx, cx)), y0 = fmin(ay, fmin(by, cy));
  double x1 = fmax(ax, fmax(bx, cx)), y1 = fmax(ay, fmax(by, cy));
  int32_t minZ = z_order(x0, y0, minX, minY, invSize), maxZ = z_order(x1, y1, minX, minY, invSize);
  int p = N(ear).prevZ, n = N(ear).nextZ;
#define TEST(q) (N(q).x >= x0 && N(q).x <= x1 && N(q).y >= y0 && N(q).y <= y1 && (q) != a && (q) != c && \
                 point_in_tri_ex_first(ax, ay, bx, by, cx, cy, N(q).x, N(q).y) && e_area(E, N(q).prev, (q), N(q).next) >= 0)
  while (p >= 0 && N(p).z >= minZ && n >= 0 && N(n).z <= maxZ) {
    if (TEST(p)) return false;
    p = N(p).prevZ;
    if (TEST(n)) return false;
    n = N(n).nextZ;
  }
  while (p >= 0 && N(p).z >= minZ) {
    if (TEST(p)) return false;
    p = N(p).prevZ;
  }
  while (n >= 0 && N(n).z <= maxZ) {
    if (TEST(n)) return false;
    n = N(n).nextZ;
  }
#undef TEST
  return true;
}

static int sign3(double v) { return v > 0 ? 1 : v < 0 ? -1 : 0; }

static bool on_segment(Earcut *E, int p, int q, int r) {
  return N(q).x <= fmax(N(p).x, N(r).x) && N(q).x >= fmin(N(p).x, N(r).x) &&
         N(q).y <= fmax(N(p).y, N(r).y) && N(q).y >= fmin(N(p).y, N(r).y);
}

static bool e_intersects(Earcut *E, int p1, int q1, int p2, int q2) {
  int o1 = sign3(e_area(E, p1, q1, p2)), o2 = sign3(e_area(E, p1, q1, q2));
  int o3 = sign3(e_area(E, p2, q2, p1)), o4 = sign3(e_area(E, p2, q2, q1));
  if (o1 != o2 && o3 != o4) return true;
  if (o1 == 0 && on_segment(E, p1, p2, q1)) return true;
  if (o2 == 0 && on_segment(E, p1, q2, q1)) return true;
  if (o3 == 0 && on_segment(E, p2, p1, q2)) return true;
  if (o4 == 0 && on_segment(E, p2, q1, q2)) return true;
  return false;
}

static bool e_locally_inside(Earcut *E, int a, int b) {
  return e_area(E, N(a).prev, a, N(a).next) < 0
    ? e_area(E, a, b, N(a).next) >= 0 && e_area(E, a, N(a).prev, b) >= 0
    : e_area(E, a, b, N(a).prev) < 0 || e_area(E, a, N(a).next, b) < 0;
}

static bool e_intersects_polygon(Earcut *E, int a, int b) {
  int p = a;
  do {
    if (N(p).i != N(a).i && N(N(p).next).i != N(a).i && N(p).i != N(b).i && N(N(p).next).i != N(b).i &&
        e_intersects(E, p, N(p).next, a, b)) return true;
    p = N(p).next;
  } while (p != a);
  return false;
}

static bool e_middle_inside(Earcut *E, int a, int b) {
  int p = a;
  bool inside = false;
  double px = (N(a).x + N(b).x) / 2, py = (N(a).y + N(b).y) / 2;
  do {
    int nx = N(p).next;
    if (((N(p).y > py) != (N(nx).y > py)) && N(nx).y != N(p).y &&
        (px < (N(nx).x - N(p).x) * (py - N(p).y) / (N(nx).y - N(p).y) + N(p).x))
      inside = !inside;
    p = nx;
  } while (p != a);
  return inside;
}

static bool e_valid_diagonal(Earcut *E, int a, int b) {
  return N(N(a).next).i != N(b).i && N(N(a).prev).i != N(b).i && !e_intersects_polygon(E, a, b) &&
         ((e_locally_inside(E, a, b) && e_locally_inside(E, b, a) && e_middle_inside(E, a, b) &&
           (e_area(E, N(a).prev, a, N(b).prev) != 0 || e_area(E, a, N(b).prev, b) != 0)) ||
          (e_equals(E, a, b) && e_area(E, N(a).prev, a, N(a).next) > 0 && e_area(E, N(b).prev, b, N(b).next) > 0));
}

static int e_split(Earcut *E, int a, int b) {
  int a2 = e_create(E, N(a).i, N(a).x, N(a).y);
  int b2 = e_create(E, N(b).i, N(b).x, N(b).y);
  int an = N(a).next, bp = N(b).prev;
  N(a).next = b;
  N(b).prev = a;
  N(a2).next = an;
  N(an).prev = a2;
  N(b2).next = a2;
  N(a2).prev = b2;
  N(bp).next = b2;
  N(b2).prev = bp;
  return b2;
}

static int e_sort_linked(Earcut *E, int list) {
  int numMerges, inSize = 1;
  do {
    int p = list, e;
    list = -1;
    int tail = -1;
    numMerges = 0;
    while (p >= 0) {
      numMerges++;
      int q = p, pSize = 0;
      for (int i = 0; i < inSize; i++) {
        pSize++;
        q = N(q).nextZ;
        if (q < 0) break;
      }
      int qSize = inSize;
      while (pSize > 0 || (qSize > 0 && q >= 0)) {
        if (pSize != 0 && (qSize == 0 || q < 0 || N(p).z <= N(q).z)) {
          e = p; p = N(p).nextZ; pSize--;
        } else {
          e = q; q = N(q).nextZ; qSize--;
        }
        if (tail >= 0) N(tail).nextZ = e;
        else list = e;
        N(e).prevZ = tail;
        tail = e;
      }
      p = q;
    }
    N(tail).nextZ = -1;
    inSize *= 2;
  } while (numMerges > 1);
  return list;
}

static void e_index_curve(Earcut *E, int start, double minX, double minY, double invSize) {
  int p = start;
  do {
    if (N(p).z == 0) N(p).z = z_order(N(p).x, N(p).y, minX, minY, invSize);
    N(p).prevZ = N(p).prev;
    N(p).nextZ = N(p).next;
    p = N(p).next;
  } while (p != start);
  N(N(p).prevZ).nextZ = -1;
  N(p).prevZ = -1;
  e_sort_linked(E, p);
}

static void e_linked(Earcut *E, int ear, double minX, double minY, double invSize, int pass);

static int e_cure_local(Earcut *E, int start) {
  int p = start;
  do {
    int a = N(p).prev, b = N(N(p).next).next;
    if (!e_equals(E, a, b) && e_intersects(E, a, p, N(p).next, b) && e_locally_inside(E, a, b) && e_locally_inside(E, b, a)) {
      vec_push(E->tris, (uint32_t)N(a).i); vec_push(E->tris, (uint32_t)N(p).i); vec_push(E->tris, (uint32_t)N(b).i);
      e_remove(E, p);
      e_remove(E, N(p).next);
      p = start = b;
    }
    p = N(p).next;
  } while (p != start);
  return e_filter(E, p, -1);
}

static void e_split_earcut(Earcut *E, int start, double minX, double minY, double invSize) {
  int a = start;
  do {
    int b = N(N(a).next).next;
    while (b != N(a).prev) {
      if (N(a).i != N(b).i && e_valid_diagonal(E, a, b)) {
        int c = e_split(E, a, b);
        a = e_filter(E, a, N(a).next);
        c = e_filter(E, c, N(c).next);
        e_linked(E, a, minX, minY, invSize, 0);
        e_linked(E, c, minX, minY, invSize, 0);
        return;
      }
      b = N(b).next;
    }
    a = N(a).next;
  } while (a != start);
}

static void e_linked(Earcut *E, int ear, double minX, double minY, double invSize, int pass) {
  if (ear < 0) return;
  if (!pass && invSize != 0) e_index_curve(E, ear, minX, minY, invSize);
  int stop = ear;
  while (N(ear).prev != N(ear).next) {
    int prev = N(ear).prev, next = N(ear).next;
    if (invSize != 0 ? e_is_ear_hashed(E, ear, minX, minY, invSize) : e_is_ear(E, ear)) {
      vec_push(E->tris, (uint32_t)N(prev).i); vec_push(E->tris, (uint32_t)N(ear).i); vec_push(E->tris, (uint32_t)N(next).i);
      e_remove(E, ear);
      ear = N(next).next;
      stop = N(next).next;
      continue;
    }
    ear = next;
    if (ear == stop) {
      if (!pass) {
        e_linked(E, e_filter(E, ear, -1), minX, minY, invSize, 1);
      } else if (pass == 1) {
        ear = e_cure_local(E, e_filter(E, ear, -1));
        e_linked(E, ear, minX, minY, invSize, 2);
      } else if (pass == 2) {
        e_split_earcut(E, ear, minX, minY, invSize);
      }
      break;
    }
  }
}

static int e_leftmost(Earcut *E, int start) {
  int p = start, left = start;
  do {
    if (N(p).x < N(left).x || (N(p).x == N(left).x && N(p).y < N(left).y)) left = p;
    p = N(p).next;
  } while (p != start);
  return left;
}

static bool e_sector_contains(Earcut *E, int m, int p) {
  return e_area(E, N(m).prev, m, N(p).prev) < 0 && e_area(E, N(p).next, m, N(m).next) < 0;
}

static int e_find_bridge(Earcut *E, int hole, int outer) {
  int p = outer;
  double hx = N(hole).x, hy = N(hole).y, qx = -INFINITY;
  int m = -1;
  if (e_equals(E, hole, p)) return p;
  do {
    int nx = N(p).next;
    if (e_equals(E, hole, nx)) return nx;
    else if (hy <= N(p).y && hy >= N(nx).y && N(nx).y != N(p).y) {
      double x = N(p).x + (hy - N(p).y) * (N(nx).x - N(p).x) / (N(nx).y - N(p).y);
      if (x <= hx && x > qx) {
        qx = x;
        m = N(p).x < N(nx).x ? p : nx;
        if (x == hx) return m;
      }
    }
    p = nx;
  } while (p != outer);
  if (m < 0) return -1;
  int stop = m;
  double mx = N(m).x, my = N(m).y, tanMin = INFINITY;
  p = m;
  do {
    if (hx >= N(p).x && N(p).x >= mx && hx != N(p).x &&
        point_in_tri(hy < my ? hx : qx, hy, mx, my, hy < my ? qx : hx, hy, N(p).x, N(p).y)) {
      double tn = fabs(hy - N(p).y) / (hx - N(p).x);
      if (e_locally_inside(E, p, hole) &&
          (tn < tanMin || (tn == tanMin && (N(p).x > N(m).x || (N(p).x == N(m).x && e_sector_contains(E, m, p)))))) {
        m = p;
        tanMin = tn;
      }
    }
    p = N(p).next;
  } while (p != stop);
  return m;
}

static int e_eliminate_hole(Earcut *E, int hole, int outer) {
  int bridge = e_find_bridge(E, hole, outer);
  if (bridge < 0) return outer;
  int rev = e_split(E, bridge, hole);
  e_filter(E, rev, N(rev).next);
  return e_filter(E, bridge, N(bridge).next);
}

static double compare_xy_slope(Earcut *E, int a, int b) {
  double r = N(a).x - N(b).x;
  if (r == 0) {
    r = N(a).y - N(b).y;
    if (r == 0) {
      double as = (N(N(a).next).y - N(a).y) / (N(N(a).next).x - N(a).x);
      double bs = (N(N(b).next).y - N(b).y) / (N(N(b).next).x - N(b).x);
      r = as - bs;
    }
  }
  return r;
}

U32Vec earcut(const double *data, int ncoords, const int *hole_idx, int nholes) {
  U32Vec tris = {};
  Earcut Es = { .tris = &tris }, *E = &Es;
  bool has_holes = nholes > 0;
  int outer_len = has_holes ? hole_idx[0] * 2 : ncoords;
  int outer = e_linked_list(E, data, 0, outer_len, true);
  if (outer < 0 || N(outer).next == N(outer).prev) { vec_free(&E->n); return tris; }
  double minX = 0, minY = 0, invSize = 0;
  if (has_holes) {
    int *queue = xmalloc((size_t)nholes * sizeof(int));
    for (int i = 0; i < nholes; i++) {
      int start = hole_idx[i] * 2, end = i < nholes - 1 ? hole_idx[i + 1] * 2 : ncoords;
      int list = e_linked_list(E, data, start, end, false);
      if (list == N(list).next) N(list).steiner = true;
      queue[i] = e_leftmost(E, list);
    }
    // Array.prototype.sort is stable: insertion sort keeps equal elements in order
    for (int i = 1; i < nholes; i++) {
      int v = queue[i], j = i - 1;
      while (j >= 0 && compare_xy_slope(E, queue[j], v) > 0) { queue[j + 1] = queue[j]; j--; }
      queue[j + 1] = v;
    }
    for (int i = 0; i < nholes; i++) outer = e_eliminate_hole(E, queue[i], outer);
    free(queue);
  }
  if (ncoords > 80 * 2) {
    minX = data[0];
    minY = data[1];
    double maxX = minX, maxY = minY;
    for (int i = 2; i < outer_len; i += 2) {
      double x = data[i], y = data[i + 1];
      if (x < minX) minX = x;
      if (y < minY) minY = y;
      if (x > maxX) maxX = x;
      if (y > maxY) maxY = y;
    }
    invSize = fmax(maxX - minX, maxY - minY);
    invSize = invSize != 0 ? 32767 / invSize : 0;
  }
  e_linked(E, outer, minX, minY, invSize, 0);
  vec_free(&E->n);
  return tris;
}
#undef N

// ---- ShapeUtils ----------------------------------------------------------------------------

double shape_area(const V2 *c, int n) {
  double a = 0.0;
  for (int p = n - 1, q = 0; q < n; p = q++) a += c[p].x * c[q].y - c[q].x * c[p].y;
  return a * 0.5;
}

static void remove_dup_end(V2Vec *v) {
  size_t l = v->len;
  if (l > 2 && v->data[l - 1].x == v->data[0].x && v->data[l - 1].y == v->data[0].y) v->len--;
}

U32Vec shape_triangulate(V2Vec *contour, V2Vec *holes, int nholes) {
  DVec verts = {};
  remove_dup_end(contour);
  for (size_t i = 0; i < contour->len; i++) { vec_push(&verts, contour->data[i].x); vec_push(&verts, contour->data[i].y); }
  int hole_index = (int)contour->len;
  int *hidx = xmalloc((size_t)(nholes ? nholes : 1) * sizeof(int));
  for (int h = 0; h < nholes; h++) remove_dup_end(&holes[h]);
  for (int h = 0; h < nholes; h++) {
    hidx[h] = hole_index;
    hole_index += (int)holes[h].len;
    for (size_t i = 0; i < holes[h].len; i++) { vec_push(&verts, holes[h].data[i].x); vec_push(&verts, holes[h].data[i].y); }
  }
  U32Vec tris = earcut(verts.data, (int)verts.len, hidx, nholes);
  free(hidx);
  vec_free(&verts);
  return tris;
}

// ---- ShapeGeometry ---------------------------------------------------------------------------

Geometry *geo_shape(const Shape *s, int curve_segments) {
  V2Vec verts = path_get_points(&s->path, curve_segments);
  int nholes = (int)s->holes.len;
  V2Vec *holes = xcalloc((size_t)(nholes ? nholes : 1), sizeof(V2Vec));
  for (int h = 0; h < nholes; h++) holes[h] = path_get_points(&s->holes.data[h], curve_segments);
  if (!shape_is_clockwise(verts.data, (int)verts.len)) vec_reverse(&verts);
  for (int h = 0; h < nholes; h++)
    if (shape_is_clockwise(holes[h].data, (int)holes[h].len)) vec_reverse(&holes[h]);
  U32Vec faces = shape_triangulate(&verts, holes, nholes);
  for (int h = 0; h < nholes; h++) vec_append(&verts, holes[h].data, holes[h].len);
  int n = (int)verts.len;
  Geometry *g = geo_new();
  geo_set_index(g, faces.data, (int)faces.len);
  float *p = geo_set_attr(g, "position", 3, n);
  float *nm = geo_set_attr(g, "normal", 3, n);
  float *uv = geo_set_attr(g, "uv", 2, n);
  for (int i = 0; i < n; i++) {
    p[i * 3] = (float)verts.data[i].x; p[i * 3 + 1] = (float)verts.data[i].y; p[i * 3 + 2] = 0;
    nm[i * 3 + 2] = 1;
    uv[i * 2] = (float)verts.data[i].x; uv[i * 2 + 1] = (float)verts.data[i].y;
  }
  vec_free(&faces);
  for (int h = 0; h < nholes; h++) vec_free(&holes[h]);
  free(holes);
  vec_free(&verts);
  return g;
}

// ---- ExtrudeGeometry (bevelEnabled: false, no extrudePath) ------------------------------------

static void merge_overlapping(V2Vec *pts) {
  const double TH = 1e-10, TH2 = TH * TH;
  if (pts->len == 0) return;
  V2 prev = pts->data[0];
  for (size_t i = 1; i <= pts->len; i++) {
    size_t cur_i = i % pts->len;
    V2 cur = pts->data[cur_i];
    double dx = cur.x - prev.x, dy = cur.y - prev.y;
    double dist2 = dx * dx + dy * dy;
    double sf = fmax(fmax(fabs(cur.x), fabs(cur.y)), fmax(fabs(prev.x), fabs(prev.y)));
    if (dist2 <= TH2 * sf * sf) {
      memmove(pts->data + cur_i, pts->data + cur_i + 1, (pts->len - cur_i - 1) * sizeof(V2));
      pts->len--;
      i--;
      continue;
    }
    prev = cur;
  }
}

Geometry *geo_extrude(const Shape *s, ExtrudeOptions o) {
  int curve_segments = o.curve_segments ? o.curve_segments : 12;
  int steps = o.steps ? o.steps : 1;
  double depth = o.depth;
  V2Vec contour = path_get_points(&s->path, curve_segments);
  int nholes = (int)s->holes.len;
  V2Vec *holes = xcalloc((size_t)(nholes ? nholes : 1), sizeof(V2Vec));
  for (int h = 0; h < nholes; h++) holes[h] = path_get_points(&s->holes.data[h], curve_segments);
  if (!shape_is_clockwise(contour.data, (int)contour.len)) {
    vec_reverse(&contour);
    for (int h = 0; h < nholes; h++)
      if (shape_is_clockwise(holes[h].data, (int)holes[h].len)) vec_reverse(&holes[h]);
  }
  merge_overlapping(&contour);
  for (int h = 0; h < nholes; h++) merge_overlapping(&holes[h]);

  // vertices = contour.concat(holes...). With no holes JS aliases the two arrays, so a point
  // dropped by triangulateShape would desynchronize them; merge_overlapping rules that out.
  V2Vec vertices = {};
  vec_append(&vertices, contour.data, contour.len);
  for (int h = 0; h < nholes; h++) vec_append(&vertices, holes[h].data, holes[h].len);
  int vlen = (int)vertices.len;
  size_t contour_len_before = contour.len;
  U32Vec faces = shape_triangulate(&contour, holes, nholes);
  CHECK(contour.len == contour_len_before);

  DVec ph = {};   // placeholder: layered vertex positions
  for (int st = 0; st <= steps; st++)
    for (int i = 0; i < vlen; i++) {
      vec_push(&ph, vertices.data[i].x);
      vec_push(&ph, vertices.data[i].y);
      vec_push(&ph, st == 0 ? 0.0 : depth / steps * st);
    }
  DVec va = {}, uva = {};
#define ADDV(k) do { vec_push(&va, ph.data[(k) * 3]); vec_push(&va, ph.data[(k) * 3 + 1]); vec_push(&va, ph.data[(k) * 3 + 2]); } while (0)
  size_t flen = faces.len / 3;
  for (int lid = 0; lid < 2; lid++) {
    for (size_t i = 0; i < flen; i++) {
      uint32_t f0 = faces.data[i * 3], f1 = faces.data[i * 3 + 1], f2 = faces.data[i * 3 + 2];
      if (lid == 0) { ADDV(f2); ADDV(f1); ADDV(f0); }
      else { uint32_t off = (uint32_t)(vlen * steps); ADDV(f0 + off); ADDV(f1 + off); ADDV(f2 + off); }
      size_t b = va.len - 9;   // generateTopUV: x, y of the three vertices
      for (int k = 0; k < 3; k++) { vec_push(&uva, va.data[b + (size_t)k * 3]); vec_push(&uva, va.data[b + (size_t)k * 3 + 1]); }
    }
  }
  int layer = 0;
  for (int h = -1; h < nholes; h++) {
    const V2Vec *ct = h < 0 ? &contour : &holes[h];
    int clen = (int)ct->len;
    int i = clen;
    while (--i >= 0) {
      int j = i, k = i - 1;
      if (k < 0) k = clen - 1;
      for (int st = 0; st < steps; st++) {
        int s1 = vlen * st, s2 = vlen * (st + 1);
        int a = layer + j + s1, b = layer + k + s1, c = layer + k + s2, d = layer + j + s2;
        ADDV(a); ADDV(b); ADDV(d); ADDV(b); ADDV(c); ADDV(d);
        size_t base = va.len - 18;
        // generateSideWallUV(indexA = -6, B = -3, C = -2, D = -1)
        const double *A = &va.data[base], *B = &va.data[base + 9], *C = &va.data[base + 12], *D = &va.data[base + 15];
        double uv[4][2];
        bool xs = fabs(A[1] - B[1]) < fabs(A[0] - B[0]);
        const double *q[4] = { A, B, C, D };
        for (int m = 0; m < 4; m++) { uv[m][0] = xs ? q[m][0] : q[m][1]; uv[m][1] = 1 - q[m][2]; }
        const int order[6] = { 0, 1, 3, 1, 2, 3 };
        for (int m = 0; m < 6; m++) { vec_push(&uva, uv[order[m]][0]); vec_push(&uva, uv[order[m]][1]); }
      }
    }
    layer += clen;
  }
#undef ADDV
  Geometry *g = geo_new();
  geo_set_attr_d(g, "position", 3, (int)(va.len / 3), va.data);
  geo_set_attr_d(g, "uv", 2, (int)(uva.len / 2), uva.data);
  geo_compute_vertex_normals(g);
  vec_free(&va); vec_free(&uva); vec_free(&ph); vec_free(&faces); vec_free(&vertices); vec_free(&contour);
  for (int h = 0; h < nholes; h++) vec_free(&holes[h]);
  free(holes);
  return g;
}
