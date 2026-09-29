// BufferGeometry core operations and BufferGeometryUtils (merge, mergeVertices).
#include "geom/geometry.h"

#include <string.h>

Geometry *geo_new(void) { return xcalloc(1, sizeof(Geometry)); }

void geo_free(Geometry *g) {
  if (!g) return;
  for (int i = 0; i < g->nattr; i++) free(g->attr[i].data);
  for (int i = 0; i < g->niattr; i++) free(g->iattr[i].data);
  free(g->index);
  free(g);
}

Geometry *geo_clone(const Geometry *g) {
  Geometry *c = geo_new();
  *c = *g;
  for (int i = 0; i < g->nattr; i++) {
    size_t n = (size_t)g->count * (size_t)g->attr[i].size;
    c->attr[i].data = xmalloc(n * sizeof(float));
    memcpy(c->attr[i].data, g->attr[i].data, n * sizeof(float));
  }
  for (int i = 0; i < g->niattr; i++) {
    size_t n = (size_t)g->iattr[i].count * (size_t)g->iattr[i].size;
    c->iattr[i].data = xmalloc(n * sizeof(float));
    memcpy(c->iattr[i].data, g->iattr[i].data, n * sizeof(float));
  }
  if (g->index) {
    c->index = xmalloc((size_t)g->index_count * sizeof(uint32_t));
    memcpy(c->index, g->index, (size_t)g->index_count * sizeof(uint32_t));
  }
  return c;
}

GeoAttr *geo_attr(const Geometry *g, const char *name) {
  for (int i = 0; i < g->nattr; i++)
    if (!strcmp(g->attr[i].name, name)) return (GeoAttr *)&g->attr[i];
  return nullptr;
}

float *geo_data(const Geometry *g, const char *name) {
  GeoAttr *a = geo_attr(g, name);
  return a ? a->data : nullptr;
}

float *geo_set_attr(Geometry *g, const char *name, int size, int count) {
  CHECK(size >= 1 && size <= 4 && count >= 0);
  CHECK(strlen(name) < GEO_NAME_LEN);
  GeoAttr *a = geo_attr(g, name);
  bool only = g->nattr == 0 || (g->nattr == 1 && a);
  if (!only && count != g->count) FATAL("attribute '%s': %d vertices, geometry has %d", name, count, g->count);
  g->count = count;
  float *data = xcalloc((size_t)count * (size_t)size, sizeof(float));
  if (a) {
    free(a->data);
  } else {
    CHECK(g->nattr < GEO_MAX_ATTR);
    a = &g->attr[g->nattr++];
    strcpy(a->name, name);
  }
  a->size = size;
  a->data = data;
  return data;
}

float *geo_set_attr_copy(Geometry *g, const char *name, int size, int count, const float *src) {
  float *d = geo_set_attr(g, name, size, count);
  memcpy(d, src, (size_t)count * (size_t)size * sizeof(float));
  return d;
}

float *geo_set_attr_d(Geometry *g, const char *name, int size, int count, const double *src) {
  float *d = geo_set_attr(g, name, size, count);
  for (size_t i = 0, n = (size_t)count * (size_t)size; i < n; i++) d[i] = (float)src[i];
  return d;
}

Geometry *geo_delete_attr(Geometry *g, const char *name) {
  for (int i = 0; i < g->nattr; i++) {
    if (strcmp(g->attr[i].name, name)) continue;
    free(g->attr[i].data);
    memmove(&g->attr[i], &g->attr[i + 1], (size_t)(g->nattr - i - 1) * sizeof(GeoAttr));
    g->nattr--;
    break;
  }
  return g;
}

void geo_set_iattr(Geometry *g, const char *name, int size, int count, const float *src) {
  CHECK(strlen(name) < GEO_NAME_LEN && size >= 1 && size <= 4);
  int k = -1;
  for (int i = 0; i < g->niattr; i++) if (!strcmp(g->iattr[i].name, name)) k = i;
  if (k < 0) { CHECK(g->niattr < 4); k = g->niattr++; strcpy(g->iattr[k].name, name); }
  else free(g->iattr[k].data);
  g->iattr[k].size = size;
  g->iattr[k].count = count;
  g->iattr[k].data = xmalloc((size_t)(count ? count : 1) * (size_t)size * sizeof(float));
  memcpy(g->iattr[k].data, src, (size_t)count * (size_t)size * sizeof(float));
}

void geo_set_index(Geometry *g, const uint32_t *idx, int n) {
  free(g->index);
  g->index = nullptr;
  g->index_count = 0;
  if (!idx) return;
  g->index = xmalloc((size_t)n * sizeof(uint32_t));
  memcpy(g->index, idx, (size_t)n * sizeof(uint32_t));
  g->index_count = n;
}

// ---- transforms --------------------------------------------------------------------------

Geometry *geo_apply_m4(Geometry *g, M4 m) {
  float *p = geo_data(g, "position");
  if (p) {
    for (int i = 0; i < g->count; i++) {
      V3 v = v3_apply_m4(v3(p[i * 3], p[i * 3 + 1], p[i * 3 + 2]), m);
      p[i * 3] = (float)v.x; p[i * 3 + 1] = (float)v.y; p[i * 3 + 2] = (float)v.z;
    }
  }
  float *n = geo_data(g, "normal");
  if (n) {
    M3 nm = m3_normal_matrix(m);
    for (int i = 0; i < g->count; i++) {
      V3 v = v3_norm(v3_apply_m3(v3(n[i * 3], n[i * 3 + 1], n[i * 3 + 2]), nm));
      n[i * 3] = (float)v.x; n[i * 3 + 1] = (float)v.y; n[i * 3 + 2] = (float)v.z;
    }
  }
  CHECK(!geo_attr(g, "tangent"));
  if (g->has_bbox) geo_compute_bbox(g);
  if (g->has_bsphere) geo_compute_bsphere(g);
  return g;
}

Geometry *geo_rotate_x(Geometry *g, double a) { return geo_apply_m4(g, m4_rotation_x(a)); }
Geometry *geo_rotate_y(Geometry *g, double a) { return geo_apply_m4(g, m4_rotation_y(a)); }
Geometry *geo_rotate_z(Geometry *g, double a) { return geo_apply_m4(g, m4_rotation_z(a)); }
Geometry *geo_translate(Geometry *g, double x, double y, double z) { return geo_apply_m4(g, m4_translation(x, y, z)); }
Geometry *geo_scale(Geometry *g, double x, double y, double z) { return geo_apply_m4(g, m4_scaling(x, y, z)); }

Geometry *geo_normalize_normals(Geometry *g) {
  float *n = geo_data(g, "normal");
  CHECK(n);
  for (int i = 0; i < g->count; i++) {
    V3 v = v3_norm(v3(n[i * 3], n[i * 3 + 1], n[i * 3 + 2]));
    n[i * 3] = (float)v.x; n[i * 3 + 1] = (float)v.y; n[i * 3 + 2] = (float)v.z;
  }
  return g;
}

Geometry *geo_compute_vertex_normals(Geometry *g) {
  float *p = geo_data(g, "position");
  if (!p) return g;
  GeoAttr *na = geo_attr(g, "normal");
  float *n;
  if (!na) {
    n = geo_set_attr(g, "normal", 3, g->count);
    p = geo_data(g, "position");
  } else {
    // three only reallocates when the count differs; here counts always match
    CHECK(na->size == 3);
    n = na->data;
    memset(n, 0, (size_t)g->count * 3 * sizeof(float));
  }
#define P(i) v3(p[(i) * 3], p[(i) * 3 + 1], p[(i) * 3 + 2])
#define N(i) v3(n[(i) * 3], n[(i) * 3 + 1], n[(i) * 3 + 2])
#define SETN(i, v) (n[(i) * 3] = (float)(v).x, n[(i) * 3 + 1] = (float)(v).y, n[(i) * 3 + 2] = (float)(v).z)
  if (g->index) {
    for (int i = 0; i + 2 < g->index_count; i += 3) {
      uint32_t a = g->index[i], b = g->index[i + 1], c = g->index[i + 2];
      V3 cb = v3_cross(v3_sub(P(c), P(b)), v3_sub(P(a), P(b)));
      V3 nA = v3_add(N(a), cb), nB = v3_add(N(b), cb), nC = v3_add(N(c), cb);
      SETN(a, nA);
      SETN(b, nB);
      SETN(c, nC);
    }
  } else {
    for (int i = 0; i + 2 < g->count; i += 3) {
      V3 cb = v3_cross(v3_sub(P(i + 2), P(i + 1)), v3_sub(P(i), P(i + 1)));
      SETN(i, cb);
      SETN(i + 1, cb);
      SETN(i + 2, cb);
    }
  }
#undef P
#undef N
#undef SETN
  return geo_normalize_normals(g);
}

Box3 geo_compute_bbox(Geometry *g) {
  Box3 b = box3_empty();
  float *p = geo_data(g, "position");
  if (p)
    for (int i = 0; i < g->count; i++) b = box3_expand(b, v3(p[i * 3], p[i * 3 + 1], p[i * 3 + 2]));
  g->bbox = b;
  g->has_bbox = true;
  return b;
}

Sphere geo_compute_bsphere(Geometry *g) {
  float *p = geo_data(g, "position");
  Sphere s = { v3s(0), -1 };   // three: an empty sphere until computed
  if (p) {
    Box3 b = box3_empty();
    for (int i = 0; i < g->count; i++) b = box3_expand(b, v3(p[i * 3], p[i * 3 + 1], p[i * 3 + 2]));
    s.center = box3_center(b);
    double r2 = 0;
    for (int i = 0; i < g->count; i++) r2 = fmax(r2, v3_dist_sq(s.center, v3(p[i * 3], p[i * 3 + 1], p[i * 3 + 2])));
    s.radius = sqrt(r2);
  }
  g->bsphere = s;
  g->has_bsphere = true;
  return s;
}

// ---- new geometries ----------------------------------------------------------------------

Geometry *geo_to_non_indexed(const Geometry *g) {
  // three returns `this` when already non-indexed (with a warning). The port returns a copy:
  // no scene code relies on the aliasing.
  if (!g->index) return geo_clone(g);
  Geometry *r = geo_new();
  for (int a = 0; a < g->nattr; a++) {
    const GeoAttr *src = &g->attr[a];
    float *d = geo_set_attr(r, src->name, src->size, g->index_count);
    for (int i = 0; i < g->index_count; i++)
      memcpy(d + (size_t)i * src->size, src->data + (size_t)g->index[i] * src->size, (size_t)src->size * sizeof(float));
  }
  return r;
}

Geometry *geo_merge(Geometry *const *list, int n) {
  CHECK(n > 0);
  const Geometry *g0 = list[0];
  bool indexed = g0->index != nullptr;
  Geometry *r = geo_new();
  int total = 0;
  for (int i = 0; i < n; i++) {
    const Geometry *g = list[i];
    if ((g->index != nullptr) != indexed) FATAL("mergeGeometries: geometry %d: indexed / non-indexed mismatch", i);
    if (g->nattr != g0->nattr) FATAL("mergeGeometries: geometry %d: attribute set differs", i);
    for (int a = 0; a < g->nattr; a++) {
      const GeoAttr *ga = geo_attr(g0, g->attr[a].name);
      if (!ga) FATAL("mergeGeometries: geometry %d: attribute '%s' not in the first geometry", i, g->attr[a].name);
      if (ga->size != g->attr[a].size) FATAL("mergeGeometries: attribute '%s' item size differs", g->attr[a].name);
    }
    total += g->count;
  }
  // attribute order: first appearance over the list (for..in order), which is the order of g0
  for (int a = 0; a < g0->nattr; a++) {
    const char *name = g0->attr[a].name;
    int size = g0->attr[a].size;
    float *d = geo_set_attr(r, name, size, total);
    size_t off = 0;
    for (int i = 0; i < n; i++) {
      const GeoAttr *src = geo_attr(list[i], name);
      size_t cnt = (size_t)list[i]->count * (size_t)size;
      memcpy(d + off, src->data, cnt * sizeof(float));
      off += cnt;
    }
  }
  if (indexed) {
    int ni = 0;
    for (int i = 0; i < n; i++) ni += list[i]->index_count;
    r->index = xmalloc((size_t)ni * sizeof(uint32_t));
    r->index_count = ni;
    uint32_t off = 0;
    int k = 0;
    for (int i = 0; i < n; i++) {
      for (int j = 0; j < list[i]->index_count; j++) r->index[k++] = list[i]->index[j] + off;
      off += (uint32_t)list[i]->count;
    }
  }
  return r;
}

Geometry *geo_merge_free(Geometry **list, int n) {
  Geometry *r = geo_merge(list, n);
  for (int i = 0; i < n; i++) geo_free(list[i]);
  return r;
}

// open-addressing table from a tuple of truncated attribute values to a vertex index
typedef struct HashEntry { uint64_t h; int64_t *key; int value; } HashEntry;

static uint64_t hash_tuple(const int64_t *k, int n) {
  uint64_t h = 1469598103934665603ull;
  for (int i = 0; i < n; i++) {
    uint64_t v = (uint64_t)k[i];
    for (int b = 0; b < 8; b++) { h ^= (v >> (b * 8)) & 0xff; h *= 1099511628211ull; }
  }
  return h;
}

Geometry *geo_merge_vertices(const Geometry *g, double tolerance) {
  tolerance = fmax(tolerance, 2.220446049250313e-16);
  int vcount = g->index ? g->index_count : g->count;
  int width = 0;
  for (int a = 0; a < g->nattr; a++) width += g->attr[a].size;
  double half = tolerance * 0.5;
  double exponent = log10(1 / tolerance);
  double mult = pow(10, exponent);
  double add = half * mult;

  Geometry *tmp = geo_new();
  for (int a = 0; a < g->nattr; a++) geo_set_attr(tmp, g->attr[a].name, g->attr[a].size, g->count);

  size_t cap = 16;
  while (cap < (size_t)vcount * 2) cap *= 2;
  HashEntry *table = xcalloc(cap, sizeof *table);
  int64_t *keys = xmalloc((size_t)(vcount ? vcount : 1) * (size_t)(width ? width : 1) * sizeof(int64_t));
  uint32_t *new_idx = xmalloc((size_t)(vcount ? vcount : 1) * sizeof(uint32_t));
  int next = 0;
  for (int i = 0; i < vcount; i++) {
    int index = g->index ? (int)g->index[i] : i;
    int64_t *key = keys + (size_t)i * (size_t)width;
    int w = 0;
    for (int a = 0; a < g->nattr; a++)
      for (int k = 0; k < g->attr[a].size; k++)
        key[w++] = (int64_t)trunc((double)g->attr[a].data[(size_t)index * g->attr[a].size + k] * mult + add);
    uint64_t h = hash_tuple(key, width);
    size_t slot = h & (cap - 1);
    int found = -1;
    while (table[slot].key) {
      if (table[slot].h == h && !memcmp(table[slot].key, key, (size_t)width * sizeof(int64_t))) { found = table[slot].value; break; }
      slot = (slot + 1) & (cap - 1);
    }
    if (found >= 0) {
      new_idx[i] = (uint32_t)found;
      continue;
    }
    for (int a = 0; a < g->nattr; a++) {
      int s = g->attr[a].size;
      memcpy(tmp->attr[a].data + (size_t)next * s, g->attr[a].data + (size_t)index * s, (size_t)s * sizeof(float));
    }
    table[slot] = (HashEntry){ h, key, next };
    new_idx[i] = (uint32_t)next;
    next++;
  }
  // result: clone of g with the attributes cut to `next` vertices and the new index
  Geometry *r = geo_new();
  for (int a = 0; a < g->nattr; a++) geo_set_attr_copy(r, g->attr[a].name, g->attr[a].size, next, tmp->attr[a].data);
  geo_set_index(r, new_idx, vcount);
  r->has_bbox = g->has_bbox; r->bbox = g->bbox;
  r->has_bsphere = g->has_bsphere; r->bsphere = g->bsphere;
  free(new_idx);
  free(keys);
  free(table);
  geo_free(tmp);
  return r;
}
