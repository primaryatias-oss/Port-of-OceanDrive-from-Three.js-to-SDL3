// Scene graph, GPU geometry, pipelines and draw submission (see scene.h).
#include "gfx/scene.h"

#include "gfx/three_mat.h"

#include <stdio.h>
#include <stdlib.h>

RenderStats g_render_stats;

// ---- GPU buffers -----------------------------------------------------------------------------

static SDL_GPUBuffer *upload_buffer(SDL_GPUBufferUsageFlags usage, const void *data, uint32_t bytes) {
  SDL_GPUBuffer *b = SDL_CreateGPUBuffer(g_gpu.dev, &(SDL_GPUBufferCreateInfo){ .usage = usage, .size = bytes ? bytes : 4 });
  if (!b) FATAL("buffer (%u bytes): %s", bytes, SDL_GetError());
  if (!bytes) return b;
  SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(g_gpu.dev, &(SDL_GPUTransferBufferCreateInfo){
    .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = bytes });
  if (!tb) FATAL("upload buffer: %s", SDL_GetError());
  void *dst = SDL_MapGPUTransferBuffer(g_gpu.dev, tb, false);
  memcpy(dst, data, bytes);
  SDL_UnmapGPUTransferBuffer(g_gpu.dev, tb);
  SDL_GPUCommandBuffer *cb = SDL_AcquireGPUCommandBuffer(g_gpu.dev);
  SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cb);
  SDL_UploadToGPUBuffer(cp, &(SDL_GPUTransferBufferLocation){ .transfer_buffer = tb },
                        &(SDL_GPUBufferRegion){ .buffer = b, .size = bytes }, false);
  SDL_EndGPUCopyPass(cp);
  SDL_CHECK(SDL_SubmitGPUCommandBuffer(cb));
  SDL_ReleaseGPUTransferBuffer(g_gpu.dev, tb);
  return b;
}

// rewrites a buffer's contents (instance data); the copy is ordered before later draws
static void update_buffer(SDL_GPUBuffer *b, const void *data, uint32_t bytes) {
  if (!bytes) return;
  SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(g_gpu.dev, &(SDL_GPUTransferBufferCreateInfo){
    .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = bytes });
  void *dst = SDL_MapGPUTransferBuffer(g_gpu.dev, tb, false);
  memcpy(dst, data, bytes);
  SDL_UnmapGPUTransferBuffer(g_gpu.dev, tb);
  SDL_GPUCommandBuffer *cb = SDL_AcquireGPUCommandBuffer(g_gpu.dev);
  SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cb);
  SDL_UploadToGPUBuffer(cp, &(SDL_GPUTransferBufferLocation){ .transfer_buffer = tb },
                        &(SDL_GPUBufferRegion){ .buffer = b, .size = bytes }, true);   // cycle: frames still in flight keep the old data
  SDL_EndGPUCopyPass(cp);
  SDL_CHECK(SDL_SubmitGPUCommandBuffer(cb));
  SDL_ReleaseGPUTransferBuffer(g_gpu.dev, tb);
}

bool g_no_cull;
bool g_keep_cpu_geometry;

GpuGeometry *gpu_geometry(Geometry *g) {
  GpuGeometry *gg = xcalloc(1, sizeof *gg);
  if (g_keep_cpu_geometry) gg->cpu = geo_clone(g);
  gg->count = g->count;
  gg->nattr = g->nattr;
  for (int i = 0; i < g->nattr; i++) {
    memcpy(gg->attr[i].name, g->attr[i].name, GEO_NAME_LEN);
    gg->attr[i].size = g->attr[i].size;
    gg->attr[i].buf = upload_buffer(SDL_GPU_BUFFERUSAGE_VERTEX, g->attr[i].data,
                                    (uint32_t)((size_t)g->count * (size_t)g->attr[i].size * sizeof(float)));
  }
  gg->niattr = g->niattr;
  for (int i = 0; i < g->niattr; i++) {
    memcpy(gg->iattr[i].name, g->iattr[i].name, GEO_NAME_LEN);
    gg->iattr[i].size = g->iattr[i].size;
    gg->iattr[i].count = g->iattr[i].count;
    gg->iattr[i].buf = upload_buffer(SDL_GPU_BUFFERUSAGE_VERTEX, g->iattr[i].data,
                                     (uint32_t)((size_t)g->iattr[i].count * (size_t)g->iattr[i].size * sizeof(float)));
  }
  if (g->index) {
    gg->index = upload_buffer(SDL_GPU_BUFFERUSAGE_INDEX, g->index, (uint32_t)((size_t)g->index_count * 4));
    gg->index_count = g->index_count;
  }
  gg->bsphere = g->has_bsphere ? g->bsphere : geo_compute_bsphere(g);
  gg->bbox = g->has_bbox ? g->bbox : geo_compute_bbox(g);
  gg->draw_count = -1;
  return gg;
}

void gpu_geometry_destroy(GpuGeometry *g) {
  if (!g) return;
  for (int i = 0; i < g->nattr; i++) SDL_ReleaseGPUBuffer(g_gpu.dev, g->attr[i].buf);
  for (int i = 0; i < g->niattr; i++) SDL_ReleaseGPUBuffer(g_gpu.dev, g->iattr[i].buf);
  if (g->index) SDL_ReleaseGPUBuffer(g_gpu.dev, g->index);
  free(g);
}

// ---- nodes -----------------------------------------------------------------------------------

static int g_node_ids;

Node *node_new(NodeKind kind, const char *name) {
  Node *n = xcalloc(1, sizeof *n);
  n->kind = kind;
  n->id = g_node_ids++;
  if (name) snprintf(n->name, sizeof n->name, "%s", name);
  n->scale = v3s(1);
  n->quaternion = quat_identity();
  n->rotation = euler(0, 0, 0, EULER_XYZ);
  n->matrix = n->matrix_world = m4_identity();
  n->matrix_auto_update = n->matrix_world_auto_update = true;
  n->visible = true;
  n->frustum_culled = true;
  return n;
}

Node *node_mesh(GpuGeometry *g, Material *m) {
  Node *n = node_new(NODE_MESH, nullptr);
  n->geo = g;
  n->material = m;
  return n;
}

Node *node_instanced(GpuGeometry *g, Material *m, int count) {
  Node *n = node_new(NODE_INSTANCED, nullptr);
  n->geo = g;
  n->material = m;
  n->inst_capacity = n->inst_count = count;
  n->inst_matrix = xmalloc((size_t)(count ? count : 1) * sizeof(M4));
  for (int i = 0; i < count; i++) n->inst_matrix[i] = m4_identity();   // three fills identities
  n->inst_dirty = true;
  return n;
}

void node_add(Node *parent, Node *child) {
  if (child->parent) node_remove(child->parent, child);
  child->parent = parent;
  vec_push(&parent->children, child);
}

void node_remove(Node *parent, Node *child) {
  for (size_t i = 0; i < parent->children.len; i++) {
    if (parent->children.data[i] != child) continue;
    memmove(&parent->children.data[i], &parent->children.data[i + 1], (parent->children.len - i - 1) * sizeof(Node *));
    parent->children.len--;
    child->parent = nullptr;
    return;
  }
}

void node_set_euler(Node *n, Euler e) {
  n->rotation = e;
  n->quaternion = quat_from_euler(e);
}
void node_set_rotation(Node *n, double x, double y, double z) { node_set_euler(n, euler(x, y, z, n->rotation.order)); }
void node_set_quaternion(Node *n, Quat q) {
  n->quaternion = q;
  n->rotation = euler_from_rotation_matrix(m4_from_quat(q), n->rotation.order);   // setFromQuaternion
}

void node_update_matrix(Node *n) { n->matrix = m4_compose(n->position, n->quaternion, n->scale); }

void node_update_matrix_world(Node *n, bool force) {
  if (n->matrix_auto_update) node_update_matrix(n);
  if (n->matrix_world_auto_update || force) {   // (three tracks matrixWorldNeedsUpdate; always recomputing is equivalent)
    n->matrix_world = n->parent ? m4_mul(n->parent->matrix_world, n->matrix) : n->matrix;
    force = true;
  }
  for (size_t i = 0; i < n->children.len; i++) node_update_matrix_world(n->children.data[i], force);
}

void node_traverse(Node *n, void (*fn)(Node *, void *), void *user) {
  fn(n, user);
  for (size_t i = 0; i < n->children.len; i++) node_traverse(n->children.data[i], fn, user);
}

void node_look_at(Node *n, V3 target) {
  // Object3D.lookAt (non-camera, non-light): rotate so +z faces the target
  node_update_matrix_world(n, true);
  V3 pos = m4_get_position(n->matrix_world);
  M4 m = m4_look_at(target, pos, v3(0, 1, 0));
  Quat q = quat_from_rotation_matrix(m);
  if (n->parent) {
    Quat pq;
    V3 pp, ps;
    m4_decompose(n->parent->matrix_world, &pp, &pq, &ps);
    q = quat_mul(quat_conj(pq), q);
  }
  node_set_quaternion(n, q);
}

// ---- instancing ------------------------------------------------------------------------------

void inst_set_matrix(Node *n, int i, M4 m) {
  CHECK(n->kind == NODE_INSTANCED && i >= 0 && i < n->inst_capacity);
  for (int k = 0; k < 16; k++) m.e[k] = (float)m.e[k];   // stored in a Float32Array
  n->inst_matrix[i] = m;
  n->inst_dirty = true;
}

void inst_set_color(Node *n, int i, Color c) {
  CHECK(n->kind == NODE_INSTANCED && i >= 0 && i < n->inst_capacity);
  if (!n->inst_color) {
    // three creates instanceColor filled with ones on the first setColorAt... then sets this one
    n->inst_color = xmalloc((size_t)n->inst_capacity * 3 * sizeof(float));
    for (int k = 0; k < n->inst_capacity * 3; k++) n->inst_color[k] = 1.0f;
  }
  n->inst_color[i * 3] = (float)c.r;
  n->inst_color[i * 3 + 1] = (float)c.g;
  n->inst_color[i * 3 + 2] = (float)c.b;
  n->inst_color_dirty = true;
}

static bool sphere_empty(Sphere s) { return s.radius < 0; }
static Sphere sphere_expand(Sphere s, V3 p) {
  if (sphere_empty(s)) return (Sphere){ p, 0 };
  V3 d = v3_sub(p, s.center);
  double l2 = v3_len_sq(d);
  if (l2 > s.radius * s.radius) {
    double l = sqrt(l2), delta = (l - s.radius) * 0.5;
    s.center = v3_add_scaled(s.center, d, delta / l);
    s.radius += delta;
  }
  return s;
}
static Sphere sphere_union(Sphere a, Sphere b) {
  if (sphere_empty(b)) return a;
  if (sphere_empty(a)) return b;
  if (a.center.x == b.center.x && a.center.y == b.center.y && a.center.z == b.center.z) {
    a.radius = fmax(a.radius, b.radius);
    return a;
  }
  V3 d = v3_set_len(v3_sub(b.center, a.center), b.radius);
  a = sphere_expand(a, v3_add(b.center, d));
  a = sphere_expand(a, v3_sub(b.center, d));
  return a;
}

void gpu_geometry_update_attr(GpuGeometry *g, const char *name, const float *data) {
  for (int i = 0; i < g->nattr; i++)
    if (!strcmp(g->attr[i].name, name)) {
      size_t n = (size_t)g->count * (size_t)g->attr[i].size;
      update_buffer(g->attr[i].buf, data, (uint32_t)(n * sizeof(float)));
      if (g->cpu) { float *d = geo_data(g->cpu, name); if (d) memcpy(d, data, n * sizeof(float)); }
      return;
    }
  FATAL("geometry has no attribute '%s'", name);
}

void gpu_geometry_update_iattr(GpuGeometry *g, const char *name, const float *data) {
  for (int i = 0; i < g->niattr; i++)
    if (!strcmp(g->iattr[i].name, name)) {
      size_t n = (size_t)g->iattr[i].count * (size_t)g->iattr[i].size;
      update_buffer(g->iattr[i].buf, data, (uint32_t)(n * sizeof(float)));
      if (g->cpu) memcpy(g->cpu->iattr[i].data, data, n * sizeof(float));   // debug dumps read the current values
      return;
    }
  FATAL("geometry has no instanced attribute '%s'", name);
}

void inst_compute_bsphere(Node *n) {
  Sphere s = { v3s(0), -1 };
  for (int i = 0; i < n->inst_count; i++) s = sphere_union(s, sphere_apply_m4(n->geo->bsphere, n->inst_matrix[i]));
  n->inst_bsphere = s;
  n->inst_bsphere_valid = true;
}

static void inst_upload(Node *n) {
  if (n->inst_dirty) {
    float *f = xmalloc((size_t)(n->inst_capacity ? n->inst_capacity : 1) * 16 * sizeof(float));
    for (int i = 0; i < n->inst_capacity; i++)
      for (int k = 0; k < 16; k++) f[i * 16 + k] = (float)n->inst_matrix[i].e[k];
    uint32_t bytes = (uint32_t)((size_t)n->inst_capacity * 64);
    if (!n->inst_matrix_buf) n->inst_matrix_buf = upload_buffer(SDL_GPU_BUFFERUSAGE_VERTEX, f, bytes);
    else update_buffer(n->inst_matrix_buf, f, bytes);
    free(f);
    n->inst_dirty = false;
  }
  if (n->inst_color && n->inst_color_dirty) {
    uint32_t bytes = (uint32_t)((size_t)n->inst_capacity * 12);
    if (!n->inst_color_buf) n->inst_color_buf = upload_buffer(SDL_GPU_BUFFERUSAGE_VERTEX, n->inst_color, bytes);
    else update_buffer(n->inst_color_buf, n->inst_color, bytes);
    n->inst_color_dirty = false;
  }
}

// ---- SkinnedMesh -----------------------------------------------------------------------------

Skeleton *skeleton_new(Node *const *bones, int n) {
  Skeleton *s = xcalloc(1, sizeof *s);
  s->n = n;
  s->bones = xmalloc((size_t)n * sizeof *s->bones);
  memcpy(s->bones, bones, (size_t)n * sizeof *bones);
  s->inverses = xmalloc((size_t)n * sizeof *s->inverses);
  for (int i = 0; i < n; i++) s->inverses[i] = m4_invert(bones[i]->matrix_world);   // calculateInverses
  int size = (int)ceil(sqrt(n * 4.0) / 4) * 4;
  s->size = size < 4 ? 4 : size;
  s->matrices = xcalloc((size_t)s->size * s->size * 4, sizeof(float));   // (zero until first rendered)
  return s;
}

void skeleton_update(Skeleton *s) {
  for (int i = 0; i < s->n; i++) {
    M4 m = m4_mul(s->bones[i]->matrix_world, s->inverses[i]);
    for (int k = 0; k < 16; k++) s->matrices[i * 16 + k] = (float)m.e[k];
  }
}

Node *node_skinned(GpuGeometry *g, Material *m, Skeleton *s) {
  Node *n = node_mesh(g, m);
  n->kind = NODE_SKINNED;
  n->skeleton = s;
  return n;
}

// the bone texture for this frame's poses (before the render pass)
static void skin_prepare(SDL_GPUCommandBuffer *cb, Node *n) {
  Skeleton *s = n->skeleton;
  CHECK(s);
  skeleton_update(s);   // (WebGLRenderer: skeleton.update() once the scene's matrices are current)
  size_t bytes = (size_t)s->size * s->size * 16;
  if (!s->tex) {
    SamplerDesc sd = sampler_default();
    sd.mag = sd.min = FILTER_NEAREST;
    sd.mipmaps = false;
    s->tex = tex_create_raw(s->size, s->size, SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT, s->matrices, bytes, sd);
  } else tex_upload(cb, s->tex, s->matrices, bytes);
}

// ---- BatchedMesh -----------------------------------------------------------------------------

Node *node_batched(Material *m, Geometry *const *geos, int ngeo, int max_instances) {
  CHECK(ngeo > 0 && max_instances > 0);
  Node *n = node_new(NODE_BATCHED, nullptr);
  n->material = m;
  // _initializeGeometry from the first geometry, then addGeometry for each (reserved = own counts)
  const Geometry *ref = geos[0];
  int nv = 0, ni = 0;
  for (int i = 0; i < ngeo; i++) {
    CHECK(geos[i]->index && geos[i]->nattr == ref->nattr);
    nv += geos[i]->count;
    ni += geos[i]->index_count;
  }
  Geometry *g = geo_new();
  for (int a = 0; a < ref->nattr; a++) {
    float *dst = geo_set_attr(g, ref->attr[a].name, ref->attr[a].size, nv);
    int off = 0;
    for (int i = 0; i < ngeo; i++) {
      const GeoAttr *src = geo_attr(geos[i], ref->attr[a].name);
      CHECK(src && src->size == ref->attr[a].size);
      memcpy(dst + (size_t)off * src->size, src->data, (size_t)geos[i]->count * src->size * sizeof(float));
      off += geos[i]->count;
    }
  }
  uint32_t *idx = xmalloc((size_t)ni * sizeof(uint32_t));
  n->batch_geo = xcalloc((size_t)ngeo, sizeof *n->batch_geo);
  n->batch_ngeo = ngeo;
  const float *pos = geo_data(g, "position");
  int vstart = 0, istart = 0;
  for (int i = 0; i < ngeo; i++) {
    for (int k = 0; k < geos[i]->index_count; k++) idx[istart + k] = (uint32_t)vstart + geos[i]->index[k];
    struct BatchGeo *bg = &n->batch_geo[i];
    bg->start = istart;
    bg->count = geos[i]->index_count;
    // getBoundingSphereAt: box of the indexed vertices, then the farthest one from its centre
    Box3 box = box3_empty();
    for (int k = istart; k < istart + bg->count; k++) {
      const float *p = pos + (size_t)idx[k] * 3;
      box = box3_expand(box, v3(p[0], p[1], p[2]));
    }
    V3 c = box3_center(box);
    double r2 = 0;
    for (int k = istart; k < istart + bg->count; k++) {
      const float *p = pos + (size_t)idx[k] * 3;
      double dx = c.x - p[0], dy = c.y - p[1], dz = c.z - p[2];
      r2 = fmax(r2, dx * dx + dy * dy + dz * dz);
    }
    bg->bsphere = (Sphere){ c, sqrt(r2) };
    vstart += geos[i]->count;
    istart += geos[i]->index_count;
  }
  geo_set_index(g, idx, ni);
  free(idx);
  n->geo = gpu_geometry(g);
  geo_free(g);
  n->batch_max = max_instances;
  n->batch_inst = xcalloc((size_t)max_instances, sizeof *n->batch_inst);
  int ms = (int)ceil(sqrt((double)max_instances * 4) / 4) * 4;
  n->batch_mat_size = ms < 4 ? 4 : ms;
  n->batch_matrix_data = xcalloc((size_t)n->batch_mat_size * n->batch_mat_size * 4, sizeof(float));
  n->batch_id_size = (int)ceil(sqrt((double)max_instances));
  n->batch_id_data = xcalloc((size_t)n->batch_id_size * n->batch_id_size, sizeof(uint32_t));
  n->batch_draw = xmalloc((size_t)max_instances * sizeof(int));
  n->batch_matrices_dirty = true;
  return n;
}

int batch_add_instance(Node *n, int geo) {
  CHECK(n->kind == NODE_BATCHED && n->batch_ninst < n->batch_max && geo >= 0 && geo < n->batch_ngeo);
  int id = n->batch_ninst++;
  n->batch_inst[id] = (struct BatchInst){ geo, true };
  batch_set_matrix(n, id, m4_identity());
  if (n->batch_color_data) for (int k = 0; k < 3; k++) n->batch_color_data[id * 4 + k] = 1.0f;   // white (alpha untouched)
  n->batch_colors_dirty = true;
  return id;
}

void batch_set_matrix(Node *n, int id, M4 m) {
  CHECK(id >= 0 && id < n->batch_ninst);
  for (int k = 0; k < 16; k++) n->batch_matrix_data[id * 16 + k] = (float)m.e[k];
  n->batch_matrices_dirty = true;
}

void batch_set_color(Node *n, int id, Color c) {
  CHECK(id >= 0 && id < n->batch_ninst);
  if (!n->batch_color_data) {   // _initColorsTexture: filled with ones
    size_t cnt = (size_t)n->batch_id_size * n->batch_id_size * 4;
    n->batch_color_data = xmalloc(cnt * sizeof(float));
    for (size_t k = 0; k < cnt; k++) n->batch_color_data[k] = 1.0f;
  }
  n->batch_color_data[id * 4] = (float)c.r;
  n->batch_color_data[id * 4 + 1] = (float)c.g;
  n->batch_color_data[id * 4 + 2] = (float)c.b;
  n->batch_colors_dirty = true;
}

void batch_set_visible(Node *n, int id, bool v) {
  CHECK(id >= 0 && id < n->batch_ninst);
  n->batch_inst[id].visible = v;
}

void batch_set_geometry(Node *n, int id, int geo) {
  CHECK(id >= 0 && id < n->batch_ninst && geo >= 0 && geo < n->batch_ngeo);
  n->batch_inst[id].geo = geo;
}

static M4 batch_matrix(const Node *n, int id) {
  M4 m;
  for (int k = 0; k < 16; k++) m.e[k] = n->batch_matrix_data[id * 16 + k];
  return m;
}

static void batch_compute_bsphere(Node *n) {   // BatchedMesh.computeBoundingSphere (active instances)
  Sphere s = { v3s(0), -1 };
  for (int i = 0; i < n->batch_ninst; i++)
    s = sphere_union(s, sphere_apply_m4(n->batch_geo[n->batch_inst[i].geo].bsphere, batch_matrix(n, i)));
  n->batch_bsphere = s;
  n->batch_bsphere_valid = true;
}

typedef struct BatchItem { double z; int index; } BatchItem;
static int batch_item_cmp(const void *pa, const void *pb) {   // a.z - b.z; ties keep instance order (stable sort)
  const BatchItem *a = pa, *b = pb;
  if (a->z != b->z) return a->z < b->z ? -1 : 1;
  return a->index - b->index;
}

// BatchedMesh.onBeforeRender / onBeforeShadow: the frustum-culled, depth-sorted draw list for
// this camera, written to the indirect texture (uploaded here, before the pass begins)
static void batch_prepare(SDL_GPUCommandBuffer *cb, Node *n, const Camera *cam, bool transparent) {
  // (sortTransparent is not ported: the scene has no transparent BatchedMesh)
  if (transparent) FATAL("'%s': transparent BatchedMesh not supported", n->name);
  M4 inv = m4_invert(n->matrix_world);
  Frustum f = frustum_from_m4(m4_mul(m4_mul(cam->projection, cam->view), n->matrix_world));
  V3 cp = v3_apply_m4(m4_get_position(cam->node->matrix_world), inv);
  V3 fw = v3_transform_dir(v3_transform_dir(v3(0, 0, -1), cam->node->matrix_world), inv);
  BatchItem *list = xmalloc((size_t)(n->batch_ninst ? n->batch_ninst : 1) * sizeof *list);
  int nl = 0;
  for (int i = 0; i < n->batch_ninst; i++) {
    if (!n->batch_inst[i].visible) continue;
    Sphere s = sphere_apply_m4(n->batch_geo[n->batch_inst[i].geo].bsphere, batch_matrix(n, i));
    if (!g_no_cull && !frustum_hits_sphere(&f, s)) continue;
    list[nl++] = (BatchItem){ v3_dot(v3_sub(s.center, cp), fw), i };
  }
  if (nl) qsort(list, (size_t)nl, sizeof *list, batch_item_cmp);
  n->batch_ndraw = nl;
  for (int k = 0; k < nl; k++) {
    n->batch_id_data[k] = (uint32_t)list[k].index;
    n->batch_draw[k] = n->batch_inst[list[k].index].geo;
  }
  free(list);
  SamplerDesc sd = sampler_default();
  sd.mag = sd.min = FILTER_NEAREST;
  sd.mipmaps = false;
  size_t mbytes = (size_t)n->batch_mat_size * n->batch_mat_size * 16;
  if (!n->batch_matrices) n->batch_matrices = tex_create_raw(n->batch_mat_size, n->batch_mat_size, SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT, n->batch_matrix_data, mbytes, sd);
  else if (n->batch_matrices_dirty) tex_upload(cb, n->batch_matrices, n->batch_matrix_data, mbytes);
  n->batch_matrices_dirty = false;
  if (n->batch_color_data) {
    size_t cbytes = (size_t)n->batch_id_size * n->batch_id_size * 16;
    if (!n->batch_colors) n->batch_colors = tex_create_raw(n->batch_id_size, n->batch_id_size, SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT, n->batch_color_data, cbytes, sd);
    else if (n->batch_colors_dirty) tex_upload(cb, n->batch_colors, n->batch_color_data, cbytes);
    n->batch_colors_dirty = false;
  }
  size_t ibytes = (size_t)n->batch_id_size * n->batch_id_size * 4;
  if (!n->batch_ids) n->batch_ids = tex_create_raw(n->batch_id_size, n->batch_id_size, SDL_GPU_TEXTUREFORMAT_R32_UINT, n->batch_id_data, ibytes, sd);
  else tex_upload(cb, n->batch_ids, n->batch_id_data, ibytes);
}

// ---- cameras ---------------------------------------------------------------------------------

void camera_update(Camera *c) {
  node_update_matrix_world(c->node, true);
  c->view = m4_invert(c->node->matrix_world);
  if (c->ortho) {
    double dx = (c->right - c->left) / (2 * c->zoom), dy = (c->top - c->bottom) / (2 * c->zoom);
    double cx = (c->right + c->left) / 2, cy = (c->top + c->bottom) / 2;
    c->projection = m4_orthographic(cx - dx, cx + dx, cy + dy, cy - dy, c->near, c->far);
  } else {
    c->projection = m4_perspective(c->fov, c->aspect, c->near, c->far, c->zoom);
  }
}

void camera_look_at(Camera *c, V3 target) {
  // Object3D.lookAt for cameras: -z toward the target
  Node *n = c->node;
  node_update_matrix_world(n, true);
  V3 pos = m4_get_position(n->matrix_world);
  V3 up = v3_len_sq(c->up) > 0 ? c->up : v3(0, 1, 0);
  M4 m = m4_look_at(pos, target, up);
  node_set_quaternion(n, quat_from_rotation_matrix(m));
}

// ---- pipelines -------------------------------------------------------------------------------

typedef struct PipeKey {
  SDL_GPUShader *vs, *fs;
  uint8_t nbuf, nattr, prim, cull, front, depth_test, depth_write, color_write;
  uint8_t blend, a2c, bias, has_color, has_depth, samples, depth_func;
  uint16_t color_fmt, depth_fmt;
  uint8_t src, dst, op, src_a, dst_a, op_a;
  float bias_const, bias_slope;
  struct { uint16_t pitch; uint8_t instance; } buf[20];
  struct { uint8_t loc, slot, fmt; uint16_t offset; } attr[24];
} PipeKey;

typedef struct PipeEntry { PipeKey key; SDL_GPUGraphicsPipeline *p; } PipeEntry;
static Vec(PipeEntry) g_pipes;

static SDL_GPUGraphicsPipeline *pipeline_get(const PipeKey *k) {
  for (size_t i = 0; i < g_pipes.len; i++)
    if (!memcmp(&g_pipes.data[i].key, k, sizeof *k)) return g_pipes.data[i].p;
  SDL_GPUVertexBufferDescription bufs[20];
  SDL_GPUVertexAttribute attrs[24];
  for (int i = 0; i < k->nbuf; i++)
    bufs[i] = (SDL_GPUVertexBufferDescription){ .slot = (uint32_t)i, .pitch = k->buf[i].pitch,
      .input_rate = k->buf[i].instance ? SDL_GPU_VERTEXINPUTRATE_INSTANCE : SDL_GPU_VERTEXINPUTRATE_VERTEX };
  for (int i = 0; i < k->nattr; i++)
    attrs[i] = (SDL_GPUVertexAttribute){ .location = k->attr[i].loc, .buffer_slot = k->attr[i].slot,
      .format = (SDL_GPUVertexElementFormat)k->attr[i].fmt, .offset = k->attr[i].offset };
  SDL_GPUColorTargetDescription ct = {
    .format = (SDL_GPUTextureFormat)k->color_fmt,
    .blend_state = {
      .enable_blend = k->blend,
      .src_color_blendfactor = (SDL_GPUBlendFactor)k->src, .dst_color_blendfactor = (SDL_GPUBlendFactor)k->dst,
      .color_blend_op = (SDL_GPUBlendOp)k->op,
      .src_alpha_blendfactor = (SDL_GPUBlendFactor)k->src_a, .dst_alpha_blendfactor = (SDL_GPUBlendFactor)k->dst_a,
      .alpha_blend_op = (SDL_GPUBlendOp)k->op_a,
      .enable_color_write_mask = !k->color_write,
      .color_write_mask = 0,
    },
  };
  SDL_GPUGraphicsPipelineCreateInfo ci = {
    .vertex_shader = k->vs,
    .fragment_shader = k->fs,
    .vertex_input_state = { .vertex_buffer_descriptions = bufs, .num_vertex_buffers = k->nbuf,
                            .vertex_attributes = attrs, .num_vertex_attributes = k->nattr },
    .primitive_type = (SDL_GPUPrimitiveType)k->prim,
    .rasterizer_state = {
      .fill_mode = SDL_GPU_FILLMODE_FILL,
      .cull_mode = (SDL_GPUCullMode)k->cull,
      .front_face = (SDL_GPUFrontFace)k->front,
      .enable_depth_bias = k->bias,
      .depth_bias_constant_factor = k->bias_const,
      .depth_bias_slope_factor = k->bias_slope,
      .enable_depth_clip = true,
    },
    .multisample_state = { .sample_count = (SDL_GPUSampleCount)k->samples, .enable_alpha_to_coverage = k->a2c },
    .depth_stencil_state = {
      .compare_op = (SDL_GPUCompareOp)k->depth_func,   // three: LessEqualDepth unless the material says
      .enable_depth_test = k->depth_test,
      .enable_depth_write = k->depth_write,
    },
    .target_info = {
      .color_target_descriptions = k->has_color ? &ct : nullptr,
      .num_color_targets = k->has_color ? 1u : 0u,
      .depth_stencil_format = (SDL_GPUTextureFormat)k->depth_fmt,
      .has_depth_stencil_target = k->has_depth,
    },
  };
  SDL_GPUGraphicsPipeline *p = SDL_CreateGPUGraphicsPipeline(g_gpu.dev, &ci);
  if (!p) FATAL("pipeline: %s", SDL_GetError());
  vec_push(&g_pipes, (PipeEntry){ *k, p });
  return p;
}

static SDL_GPUBuffer *g_zero_buf;

void renderer_init(void) {
  static const float zero[4] = { 0, 0, 0, 1 };   // an unbound GL attribute reads (0, 0, 0, 1)
  g_zero_buf = upload_buffer(SDL_GPU_BUFFERUSAGE_VERTEX, zero, sizeof zero);
}

void renderer_shutdown(void) {
  for (size_t i = 0; i < g_pipes.len; i++) SDL_ReleaseGPUGraphicsPipeline(g_gpu.dev, g_pipes.data[i].p);
  vec_free(&g_pipes);
  if (g_zero_buf) SDL_ReleaseGPUBuffer(g_gpu.dev, g_zero_buf);
  g_zero_buf = nullptr;
}

static uint8_t float_format(int n) {
  switch (n) {
  case 1: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT;
  case 2: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
  case 3: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
  default: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
  }
}

typedef struct DrawState {
  const RenderTargetDesc *rt;
  SDL_GPUCommandBuffer *cb;
  SDL_GPURenderPass *rp;
  M4 view;
  SDL_GPUGraphicsPipeline *bound_pipe;
  Material *bound_mat;
  MatVariant bound_var;
} DrawState;

// Binds pipeline + vertex/index buffers for (node, material, variant, side) and issues the draw.
static void draw_node(DrawState *ds, Node *n, Material *m, MatVariant var, Side side) {
  Program *p = mat_program(m, var);
  // an InstancedMesh with instanceColor needs a USE_INSTANCING_COLOR program, and only then
  if (n->kind == NODE_INSTANCED && program_has_param(p->id, "instancingColor") != (n->inst_color != nullptr))
    FATAL("'%s': instance colours %s but program %s %s them", n->name, n->inst_color ? "set" : "not set",
          p->info->name, n->inst_color ? "ignores" : "expects");
  if (n->kind == NODE_BATCHED) {
    if (program_has_param(p->id, "batchingColor") != (n->batch_colors != nullptr))
      FATAL("'%s': batch colours %s but program %s %s them", n->name, n->batch_colors ? "set" : "not set",
            p->info->name, n->batch_colors ? "ignores" : "expects");
    Texture *want[3] = { n->batch_matrices, n->batch_ids, n->batch_colors };
    const char *names[3] = { "batchingTexture", "batchingIdTexture", "batchingColorTexture" };
    for (int i = 0; i < 3; i++)
      if (want[i] && mat_get_texture(m, names[i]) != want[i]) {
        mat_set_texture(m, names[i], want[i]);
        if (ds->bound_mat == m) ds->bound_mat = nullptr;
      }
  }
  if (n->kind == NODE_SKINNED && mat_get_texture(m, "boneTexture") != n->skeleton->tex) {
    mat_set_texture(m, "boneTexture", n->skeleton->tex);
    if (ds->bound_mat == m) ds->bound_mat = nullptr;
  }
  PipeKey k = {};
  k.vs = p->vs;
  k.fs = p->fs;
  SDL_GPUBufferBinding bind[20];
  int nb = 0;
  for (const AttribSlot *a = p->pv->attribs; a && a->name; a++) {
    if (!strcmp(a->name, "instanceMatrix")) {
      CHECK(n->kind == NODE_INSTANCED);
      bind[nb] = (SDL_GPUBufferBinding){ n->inst_matrix_buf, 0 };
      k.buf[nb] = (typeof(k.buf[0])){ 64, 1 };
      for (int c = 0; c < 4; c++)
        k.attr[k.nattr++] = (typeof(k.attr[0])){ (uint8_t)(a->location + c), (uint8_t)nb, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, (uint16_t)(c * 16) };
      nb++;
      continue;
    }
    if (!strcmp(a->name, "instanceColor")) {
      CHECK(n->kind == NODE_INSTANCED && n->inst_color_buf);
      bind[nb] = (SDL_GPUBufferBinding){ n->inst_color_buf, 0 };
      k.buf[nb] = (typeof(k.buf[0])){ 12, 1 };
      k.attr[k.nattr++] = (typeof(k.attr[0])){ (uint8_t)a->location, (uint8_t)nb, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0 };
      nb++;
      continue;
    }
    CHECK(a->nloc == 1);
    int ii = -1;
    for (int i = 0; i < n->geo->niattr; i++)
      if (!strcmp(n->geo->iattr[i].name, a->name)) ii = i;
    if (ii >= 0) {
      CHECK(n->kind == NODE_INSTANCED && n->geo->iattr[ii].count >= n->inst_count);
      bind[nb] = (SDL_GPUBufferBinding){ n->geo->iattr[ii].buf, 0 };
      k.buf[nb] = (typeof(k.buf[0])){ (uint16_t)(n->geo->iattr[ii].size * 4), 1 };
      k.attr[k.nattr++] = (typeof(k.attr[0])){ (uint8_t)a->location, (uint8_t)nb, float_format(n->geo->iattr[ii].size), 0 };
      nb++;
      continue;
    }
    int gi = -1;
    for (int i = 0; i < n->geo->nattr; i++)
      if (!strcmp(n->geo->attr[i].name, a->name)) gi = i;
    if (gi >= 0) {
      bind[nb] = (SDL_GPUBufferBinding){ n->geo->attr[gi].buf, 0 };
      k.buf[nb] = (typeof(k.buf[0])){ (uint16_t)(n->geo->attr[gi].size * 4), 0 };
      k.attr[k.nattr++] = (typeof(k.attr[0])){ (uint8_t)a->location, (uint8_t)nb, float_format(n->geo->attr[gi].size), 0 };
    } else {
      bind[nb] = (SDL_GPUBufferBinding){ g_zero_buf, 0 };
      k.buf[nb] = (typeof(k.buf[0])){ 0, 0 };   // stride 0: every vertex reads (0, 0, 0, 1)
      k.attr[k.nattr++] = (typeof(k.attr[0])){ (uint8_t)a->location, (uint8_t)nb, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0 };
    }
    nb++;
  }
  k.nbuf = (uint8_t)nb;
  k.prim = n->kind == NODE_POINTS ? SDL_GPU_PRIMITIVETYPE_POINTLIST
         : n->kind == NODE_LINES ? SDL_GPU_PRIMITIVETYPE_LINELIST : SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
  // WebGLState.setMaterial: cull back faces unless DoubleSide; BackSide (xor a mirrored object)
  // flips the front face. GL's CCW front is CW here because passes render y-flipped.
  bool flip = side == SIDE_BACK;
  if (n->kind != NODE_POINTS && n->kind != NODE_LINES && m4_determinant(m4_set_position(n->matrix_world, v3s(0))) < 0) flip = !flip;
  k.cull = side == SIDE_DOUBLE ? SDL_GPU_CULLMODE_NONE : SDL_GPU_CULLMODE_BACK;
  k.front = flip ? SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE : SDL_GPU_FRONTFACE_CLOCKWISE;
  k.depth_test = m->depth_test;
  k.depth_write = m->depth_write;
  k.depth_func = (uint8_t)m->depth_func;
  k.color_write = m->color_write;
  bool blend = !(m->blending == BLEND_NONE || (m->blending == BLEND_NORMAL && !m->transparent));
  k.blend = blend;
  if (blend) {
    // WebGLState.setBlending
    k.op = k.op_a = SDL_GPU_BLENDOP_ADD;
    if (m->blending == BLEND_NORMAL) {
      k.src = m->premultiplied_alpha ? SDL_GPU_BLENDFACTOR_ONE : SDL_GPU_BLENDFACTOR_SRC_ALPHA;
      k.dst = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
      k.src_a = SDL_GPU_BLENDFACTOR_ONE;
      k.dst_a = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    } else if (m->blending == BLEND_ADDITIVE) {
      k.src = m->premultiplied_alpha ? SDL_GPU_BLENDFACTOR_ONE : SDL_GPU_BLENDFACTOR_SRC_ALPHA;
      k.dst = k.src_a = k.dst_a = SDL_GPU_BLENDFACTOR_ONE;
    } else {
      k.src = (uint8_t)m->blend_src;
      k.dst = (uint8_t)m->blend_dst;
      k.src_a = (uint8_t)(m->blend_src_alpha != SDL_GPU_BLENDFACTOR_INVALID ? m->blend_src_alpha : m->blend_src);
      k.dst_a = (uint8_t)(m->blend_dst_alpha != SDL_GPU_BLENDFACTOR_INVALID ? m->blend_dst_alpha : m->blend_dst);
      k.op = (uint8_t)m->blend_op;
      k.op_a = (uint8_t)(m->blend_op_alpha != SDL_GPU_BLENDOP_INVALID ? m->blend_op_alpha : m->blend_op);
    }
  }
  k.bias = m->polygon_offset;
  k.bias_const = m->polygon_offset ? m->po_units : 0;
  k.bias_slope = m->polygon_offset ? m->po_factor : 0;
  k.a2c = m->alpha_to_coverage;
  k.has_color = ds->rt->color != nullptr;
  k.color_fmt = (uint16_t)ds->rt->color_format;
  k.has_depth = ds->rt->depth != nullptr;
  k.depth_fmt = (uint16_t)ds->rt->depth_format;
  k.samples = (uint8_t)ds->rt->samples;
  SDL_GPUGraphicsPipeline *pipe = pipeline_get(&k);
  if (pipe != ds->bound_pipe) {
    SDL_BindGPUGraphicsPipeline(ds->rp, pipe);
    ds->bound_pipe = pipe;
    ds->bound_mat = nullptr;   // uniforms must be re-pushed for the new pipeline layout
  }
  if (ds->bound_mat != m || ds->bound_var != var) {
    mat_bind(m, var, ds->cb, ds->rp);
    ds->bound_mat = m;
    ds->bound_var = var;
  }
  M4 mv = m4_mul(ds->view, n->matrix_world);
  ObjectUniforms ou = { n->matrix_world, mv, m3_normal_matrix(mv), n->receive_shadow };
  program_push_object(p, ds->cb, &ou);
  SDL_BindGPUVertexBuffers(ds->rp, 0, bind, (uint32_t)nb);
  const GpuGeometry *g = n->geo;
  if (n->kind == NODE_BATCHED) {
    // the multi-draw: item k draws its geometry's index range with gl_DrawID (here
    // gl_InstanceIndex) = k; runs of the same geometry become one instanced draw
    SDL_BindGPUIndexBuffer(ds->rp, &(SDL_GPUBufferBinding){ g->index, 0 }, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    for (int d = 0; d < n->batch_ndraw;) {
      int e = d + 1;
      while (e < n->batch_ndraw && n->batch_draw[e] == n->batch_draw[d]) e++;
      const struct BatchGeo *bg = &n->batch_geo[n->batch_draw[d]];
      SDL_DrawGPUIndexedPrimitives(ds->rp, (uint32_t)bg->count, (uint32_t)(e - d), (uint32_t)bg->start, 0, (uint32_t)d);
      g_render_stats.calls++;
      g_render_stats.triangles += bg->count / 3 * (e - d);
      g_render_stats.instances += e - d;
      d = e;
    }
    return;
  }
  uint32_t instances = n->kind == NODE_INSTANCED ? (uint32_t)n->inst_count : 1;
  if (!instances) return;
  int start = g->draw_start, count;
  if (g->index) {
    count = g->draw_count >= 0 ? imin(g->draw_count, g->index_count - start) : g->index_count - start;
    SDL_BindGPUIndexBuffer(ds->rp, &(SDL_GPUBufferBinding){ g->index, 0 }, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    if (count > 0) SDL_DrawGPUIndexedPrimitives(ds->rp, (uint32_t)count, instances, (uint32_t)start, 0, 0);
  } else {
    count = g->draw_count >= 0 ? imin(g->draw_count, g->count - start) : g->count - start;
    if (count > 0) SDL_DrawGPUPrimitives(ds->rp, (uint32_t)count, instances, (uint32_t)start, 0);
  }
  if (count > 0) {
    g_render_stats.calls++;
    g_render_stats.triangles += count / 3 * (int)instances;
    g_render_stats.instances += (int)instances;
  }
}

// ---- render lists ----------------------------------------------------------------------------

typedef struct RenderItem {
  Node *n;
  Material *m;
  int group_order, render_order, variant, id;
  double z;
} RenderItem;

typedef Vec(RenderItem) RenderItemVec;
typedef struct Lists { RenderItemVec opaque, transparent; } Lists;

static int painter(const void *pa, const void *pb) {   // painterSortStable
  const RenderItem *a = pa, *b = pb;
  if (a->group_order != b->group_order) return a->group_order < b->group_order ? -1 : 1;
  if (a->render_order != b->render_order) return a->render_order < b->render_order ? -1 : 1;
  if (a->m->id != b->m->id) return a->m->id < b->m->id ? -1 : 1;
  if (a->variant != b->variant) return a->variant < b->variant ? -1 : 1;
  if (a->z != b->z) return a->z < b->z ? -1 : 1;
  return a->id < b->id ? -1 : a->id > b->id;
}
static int reverse_painter(const void *pa, const void *pb) {   // reversePainterSortStable
  const RenderItem *a = pa, *b = pb;
  if (a->group_order != b->group_order) return a->group_order < b->group_order ? -1 : 1;
  if (a->render_order != b->render_order) return a->render_order < b->render_order ? -1 : 1;
  if (a->z != b->z) return a->z > b->z ? -1 : 1;
  return a->id < b->id ? -1 : a->id > b->id;
}

static Sphere world_sphere(Node *n) {
  if (n->kind == NODE_BATCHED) {
    if (!n->batch_bsphere_valid) batch_compute_bsphere(n);
    return sphere_apply_m4(n->batch_bsphere, n->matrix_world);
  }
  if (n->kind == NODE_INSTANCED) {
    if (!n->inst_bsphere_valid) inst_compute_bsphere(n);
    return sphere_apply_m4(n->inst_bsphere, n->matrix_world);
  }
  return sphere_apply_m4(n->geo->bsphere, n->matrix_world);
}
static V3 local_center(Node *n) {
  if (n->kind == NODE_BATCHED) {
    if (!n->batch_bsphere_valid) batch_compute_bsphere(n);
    return n->batch_bsphere.center;
  }
  if (n->kind == NODE_INSTANCED) {
    if (!n->inst_bsphere_valid) inst_compute_bsphere(n);
    return n->inst_bsphere.center;
  }
  return n->geo->bsphere.center;
}

static void project(Node *n, const Frustum *f, M4 proj_screen, int group_order, Lists *L) {
  if (!n->visible) return;
  if (n->kind == NODE_GROUP) {
    group_order = n->render_order;   // (a Group's renderOrder sets the group order of its subtree)
  } else if (n->geo && n->material) {
    if (!n->frustum_culled || g_no_cull || frustum_hits_sphere(f, world_sphere(n))) {
      V4 c = v4_apply_m4(v4_apply_m4((V4){ local_center(n).x, local_center(n).y, local_center(n).z, 1 }, n->matrix_world), proj_screen);
      if (n->material->visible) {
        RenderItem it = { n, n->material, group_order, n->render_order,
                          (n->kind == NODE_INSTANCED ? 2 : 0) + (n->kind == NODE_SKINNED ? 1 : 0), n->id, c.z };
        if (n->material->transparent) vec_push(&L->transparent, it);
        else vec_push(&L->opaque, it);
      }
    }
  }
  for (size_t i = 0; i < n->children.len; i++) project(n->children.data[i], f, proj_screen, group_order, L);
}

static MatVariant base_variant(const Node *n) {
  return n->kind == NODE_INSTANCED ? MV_INSTANCED : n->kind == NODE_SKINNED ? MV_SKINNED
       : n->kind == NODE_BATCHED ? MV_BATCHED : MV_PLAIN;
}

static void prepare(Node *n, void *u) {
  (void)u;
  if (n->kind == NODE_INSTANCED) inst_upload(n);
}

static SDL_GPURenderPass *begin_pass(SDL_GPUCommandBuffer *cb, const RenderTargetDesc *rt) {
  SDL_GPUColorTargetInfo ci = {
    .texture = rt->color,
    .clear_color = rt->clear,
    .load_op = rt->clear_color ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD,
    .store_op = rt->resolve ? SDL_GPU_STOREOP_RESOLVE_AND_STORE : SDL_GPU_STOREOP_STORE,   // (later passes may load the samples again)
    .resolve_texture = rt->resolve,
  };
  SDL_GPUDepthStencilTargetInfo di = {
    .texture = rt->depth,
    .clear_depth = 1.0f,
    .load_op = rt->clear_depth ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD,
    .store_op = rt->depth_store,
    .stencil_load_op = SDL_GPU_LOADOP_DONT_CARE,
    .stencil_store_op = SDL_GPU_STOREOP_DONT_CARE,
  };
  SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cb, rt->color ? &ci : nullptr, rt->color ? 1 : 0, rt->depth ? &di : nullptr);
  if (!rp) FATAL("render pass: %s", SDL_GetError());
  // GL viewport / scissor rows count from the bottom; our targets hold rows in GL order, and
  // SDL_GPU's rectangles index those rows directly, so the numbers carry over unchanged
  if (rt->viewport_set)
    SDL_SetGPUViewport(rp, &(SDL_GPUViewport){ (float)rt->vp_x, (float)rt->vp_y, (float)rt->vp_w, (float)rt->vp_h, 0, 1 });
  if (rt->scissor_set) SDL_SetGPUScissor(rp, &(SDL_Rect){ rt->sc_x, rt->sc_y, rt->sc_w, rt->sc_h });
  return rp;
}

const Camera *g_draw_camera;
double g_draw_viewport[4];

void render_scene(SDL_GPUCommandBuffer *cb, Node *scene, Camera *cam, const RenderTargetDesc *rt) {
  g_draw_camera = cam;
  if (rt->viewport_set) { g_draw_viewport[0] = rt->vp_x; g_draw_viewport[1] = rt->vp_y; g_draw_viewport[2] = rt->vp_w; g_draw_viewport[3] = rt->vp_h; }
  else { g_draw_viewport[0] = 0; g_draw_viewport[1] = 0; g_draw_viewport[2] = rt->w; g_draw_viewport[3] = rt->h; }
  node_update_matrix_world(scene, false);
  camera_update(cam);
  node_traverse(scene, prepare, nullptr);
  M4 ps = m4_mul(cam->projection, cam->view);
  Frustum f = frustum_from_m4(ps);
  Lists L = {};
  project(scene, &f, ps, 0, &L);
  if (L.opaque.len) qsort(L.opaque.data, L.opaque.len, sizeof(RenderItem), painter);
  if (L.transparent.len) qsort(L.transparent.data, L.transparent.len, sizeof(RenderItem), reverse_painter);

  frame_set_m4(G_VIEW_MATRIX, cam->view);
  frame_set_m4(G_PROJECTION_MATRIX, cam->projection);
  frame_set_v3(G_CAMERA_POSITION, m4_get_position(cam->node->matrix_world));
  frame_set_bool(G_IS_ORTHOGRAPHIC, cam->ortho);

  // BatchedMesh draw lists for this camera (texture uploads must precede the render pass)
  for (size_t i = 0; i < L.opaque.len; i++)
    if (L.opaque.data[i].n->kind == NODE_BATCHED) batch_prepare(cb, L.opaque.data[i].n, cam, false);
  for (size_t i = 0; i < L.transparent.len; i++)
    if (L.transparent.data[i].n->kind == NODE_BATCHED) batch_prepare(cb, L.transparent.data[i].n, cam, true);
  // SkinnedMesh bone textures (a skeleton shared by several meshes is uploaded once each)
  for (int pass = 0; pass < 2; pass++) {
    RenderItemVec *lv = pass ? &L.transparent : &L.opaque;
    for (size_t i = 0; i < lv->len; i++) {
      Node *sn = lv->data[i].n;
      if (sn->kind != NODE_SKINNED) continue;
      bool seen = false;
      for (int q = 0; q <= pass && !seen; q++) {
        RenderItemVec *pv = q ? &L.transparent : &L.opaque;
        size_t lim = q == pass ? i : pv->len;
        for (size_t j = 0; j < lim; j++)
          if (pv->data[j].n->kind == NODE_SKINNED && pv->data[j].n->skeleton == sn->skeleton) { seen = true; break; }
      }
      if (!seen) skin_prepare(cb, sn);
    }
  }

  DrawState ds = { .rt = rt, .cb = cb, .view = cam->view };
  ds.rp = begin_pass(cb, rt);
  for (size_t i = 0; i < L.opaque.len; i++) {
    RenderItem *it = &L.opaque.data[i];
    if (it->n->on_before_render) it->n->on_before_render(it->n, it->n->user);
    draw_node(&ds, it->n, it->m, base_variant(it->n), it->m->side);
  }
  for (size_t i = 0; i < L.transparent.len; i++) {
    RenderItem *it = &L.transparent.data[i];
    if (it->n->on_before_render) it->n->on_before_render(it->n, it->n->user);
    MatVariant v = base_variant(it->n);
    if (it->m->side == SIDE_DOUBLE && !it->m->force_single_pass) {
      CHECK(v != MV_SKINNED);
      draw_node(&ds, it->n, it->m, v == MV_INSTANCED ? MV_INSTANCED_BACK : MV_PLAIN_BACK, SIDE_BACK);
      draw_node(&ds, it->n, it->m, v == MV_INSTANCED ? MV_INSTANCED_FRONT : MV_PLAIN_FRONT, SIDE_FRONT);
    } else {
      draw_node(&ds, it->n, it->m, v, it->m->side);
    }
  }
  SDL_EndGPURenderPass(ds.rp);
  vec_free(&L.opaque);
  vec_free(&L.transparent);
}

typedef struct ShadowItem { Node *n; Material *m; Side side; } ShadowItem;
typedef struct ShadowCtx {
  Frustum f;
  DepthMaterialFn fn;
  void *user;
  Vec(ShadowItem) items;
} ShadowCtx;

static void shadow_walk(Node *n, ShadowCtx *c) {
  if (!n->visible) return;
  if (n->geo && n->material && n->cast_shadow && (!n->frustum_culled || g_no_cull || frustum_hits_sphere(&c->f, world_sphere(n)))) {
    Material *src = n->material;
    if (src->visible) {
      static const Side flipped[3] = { [SIDE_FRONT] = SIDE_BACK, [SIDE_BACK] = SIDE_FRONT, [SIDE_DOUBLE] = SIDE_DOUBLE };
      Side side = src->has_shadow_side ? src->shadow_side : flipped[src->side];
      vec_push(&c->items, ((ShadowItem){ n, c->fn(n, src, side, c->user), side }));
    }
  }
  for (size_t i = 0; i < n->children.len; i++) shadow_walk(n->children.data[i], c);
}

void render_shadow(SDL_GPUCommandBuffer *cb, Node *scene, Camera *light_cam, const RenderTargetDesc *rt,
                   DepthMaterialFn depth_for, void *user) {
  node_update_matrix_world(scene, false);
  camera_update(light_cam);
  node_traverse(scene, prepare, nullptr);
  M4 ps = m4_mul(light_cam->projection, light_cam->view);
  ShadowCtx c = { frustum_from_m4(ps), depth_for, user, {} };
  shadow_walk(scene, &c);
  // BatchedMesh.onBeforeShadow (the depth material is never transparent)
  for (size_t i = 0; i < c.items.len; i++)
    if (c.items.data[i].n->kind == NODE_BATCHED) batch_prepare(cb, c.items.data[i].n, light_cam, false);
  frame_set_m4(G_VIEW_MATRIX, light_cam->view);
  frame_set_m4(G_PROJECTION_MATRIX, light_cam->projection);
  frame_set_v3(G_CAMERA_POSITION, m4_get_position(light_cam->node->matrix_world));
  frame_set_bool(G_IS_ORTHOGRAPHIC, light_cam->ortho);
  DrawState ds = { .rt = rt, .cb = cb, .view = light_cam->view };
  ds.rp = begin_pass(cb, rt);
  for (size_t i = 0; i < c.items.len; i++) {
    ShadowItem *it = &c.items.data[i];
    draw_node(&ds, it->n, it->m, base_variant(it->n), it->side);
  }
  SDL_EndGPURenderPass(ds.rp);
  vec_free(&c.items);
}
