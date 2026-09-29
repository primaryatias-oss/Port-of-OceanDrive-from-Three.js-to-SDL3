// Scene graph and draw submission: a port of the parts of three's Object3D / Mesh /
// InstancedMesh / WebGLRenderer the scene relies on (matrix updates, frustum culling, render
// lists sorted like WebGLRenderLists, shadow casting with the depth material).
#pragma once

#include "core/vec.h"
#include "geom/geometry.h"
#include "gfx/material.h"

typedef struct GpuGeometry {
  int count;                        // vertices
  int nattr;
  struct { char name[GEO_NAME_LEN]; int size; SDL_GPUBuffer *buf; } attr[GEO_MAX_ATTR];
  SDL_GPUBuffer *index;
  int index_count;
  int niattr;                       // per-instance attributes (InstancedBufferAttribute)
  struct { char name[GEO_NAME_LEN]; int size, count; SDL_GPUBuffer *buf; } iattr[4];
  Sphere bsphere;                   // geometry.boundingSphere
  Box3 bbox;
  int draw_start, draw_count;       // setDrawRange (count -1 = all)
  Geometry *cpu;                    // a CPU copy, kept only while g_keep_cpu_geometry (debug dumps)
} GpuGeometry;

extern bool g_keep_cpu_geometry;
// start-up warm render: no frustum culling (every object's pipelines and uploads get made)
extern bool g_no_cull;

// Uploads a Geometry (computes its bounding sphere / box first, as three does lazily).
GpuGeometry *gpu_geometry(Geometry *g);
void gpu_geometry_destroy(GpuGeometry *g);
// InstancedBufferAttribute.needsUpdate: rewrites a per-instance attribute (count * size floats)
void gpu_geometry_update_iattr(GpuGeometry *g, const char *name, const float *data);
// BufferAttribute.needsUpdate (DynamicDrawUsage): rewrites a vertex attribute (count * size floats)
void gpu_geometry_update_attr(GpuGeometry *g, const char *name, const float *data);

typedef enum NodeKind { NODE_GROUP, NODE_MESH, NODE_INSTANCED, NODE_SKINNED, NODE_BATCHED, NODE_POINTS, NODE_LINES } NodeKind;

typedef struct Node Node;

// Skeleton (three's Skeleton): bone nodes, inverse bind matrices and the bone matrix texture
// (square RGBA32F, size = max(4, ceil(sqrt(4 n) / 4) * 4), 4 texels per bone)
typedef struct Skeleton {
  int n;
  Node **bones;
  M4 *inverses;
  float *matrices;                  // boneMatrices (Float32Array, size * size * 4)
  int size;
  struct Texture *tex;
} Skeleton;

struct Node {
  NodeKind kind;
  char name[48];
  int id;
  Node *parent;
  Vec(Node *) children;

  V3 position, scale;
  Euler rotation;                   // kept in sync with quaternion like three's Object3D
  Quat quaternion;
  M4 matrix, matrix_world;
  bool matrix_auto_update, matrix_world_auto_update;
  bool visible, cast_shadow, receive_shadow, frustum_culled;
  int render_order;

  // mesh payload
  GpuGeometry *geo;
  Material *material;
  Material *custom_depth;           // customDepthMaterial
  // instancing (InstancedMesh)
  int inst_capacity, inst_count;
  M4 *inst_matrix;                  // CPU copies (double)
  float *inst_color;                // null unless instanceColor is used (3 floats each)
  SDL_GPUBuffer *inst_matrix_buf, *inst_color_buf;
  bool inst_dirty, inst_color_dirty;
  Sphere inst_bsphere;              // InstancedMesh.boundingSphere
  bool inst_bsphere_valid;

  // BatchedMesh: the geometries share one merged GpuGeometry (geo); per-instance matrices and
  // colours live in float textures, the per-pass draw list in an index texture
  int batch_ngeo, batch_ninst, batch_max;
  struct BatchGeo { int start, count; Sphere bsphere; } *batch_geo;
  struct BatchInst { int geo; bool visible; } *batch_inst;
  float *batch_matrix_data, *batch_color_data;   // Float32Array texture images
  uint32_t *batch_id_data;
  int batch_mat_size, batch_id_size;             // texture widths (square)
  Texture *batch_matrices, *batch_colors, *batch_ids;
  bool batch_matrices_dirty, batch_colors_dirty;
  int *batch_draw, batch_ndraw;                  // geometry of each draw in the current pass
  Sphere batch_bsphere;
  bool batch_bsphere_valid;

  Skeleton *skeleton;              // SkinnedMesh (bindMatrix / bindMatrixInverse: identity, DetachedBindMode)

  void (*on_before_render)(Node *n, void *user);
  void *user;
};

Node *node_new(NodeKind kind, const char *name);
Node *node_mesh(GpuGeometry *g, Material *m);
Node *node_instanced(GpuGeometry *g, Material *m, int count);
void node_add(Node *parent, Node *child);
void node_remove(Node *parent, Node *child);
void node_set_rotation(Node *n, double x, double y, double z);   // rotation.set(x, y, z) (order XYZ)
void node_set_euler(Node *n, Euler e);
void node_set_quaternion(Node *n, Quat q);
void node_update_matrix(Node *n);
void node_update_matrix_world(Node *n, bool force);
void node_traverse(Node *n, void (*fn)(Node *, void *), void *user);
// Object3D.lookAt for non-camera objects (+z of the object toward the target)
void node_look_at(Node *n, V3 target);

// Skeleton(bones): the inverses come from the bones' current matrix_world (update it first)
Skeleton *skeleton_new(Node *const *bones, int n);
void skeleton_update(Skeleton *s);   // boneMatrices = bone.matrixWorld * inverse
// SkinnedMesh bound to a skeleton with an identity bind matrix (DetachedBindMode)
Node *node_skinned(GpuGeometry *g, Material *m, Skeleton *s);

// InstancedMesh
void inst_set_matrix(Node *n, int i, M4 m);
void inst_set_color(Node *n, int i, Color c);
void inst_compute_bsphere(Node *n);

// BatchedMesh(maxInstanceCount, ...) with addGeometry(geos[i]) for each geometry (all indexed,
// with the same attributes); ids are the order of the list
Node *node_batched(Material *m, Geometry *const *geos, int ngeo, int max_instances);
int batch_add_instance(Node *n, int geo);        // addInstance: returns the instance id
void batch_set_matrix(Node *n, int id, M4 m);
void batch_set_color(Node *n, int id, Color c);
void batch_set_visible(Node *n, int id, bool v);
void batch_set_geometry(Node *n, int id, int geo);   // setGeometryIdAt

// ---- cameras ----
typedef struct Camera {
  Node *node;                       // transform (position / quaternion); matrix_world
  bool ortho;
  double fov, aspect, near, far, zoom;
  double left, right, top, bottom;  // orthographic
  V3 up;                            // Object3D.up for lookAt (zero = (0, 1, 0))
  M4 projection, view;              // view = matrixWorldInverse
} Camera;
void camera_update(Camera *c);      // updateMatrixWorld + projection + view
// camera.lookAt (the camera looks down its -z)
void camera_look_at(Camera *c, V3 target);

// ---- rendering ----
typedef struct RenderTargetDesc {
  SDL_GPUTexture *color;            // null: depth only
  SDL_GPUTextureFormat color_format;
  SDL_GPUTexture *resolve;          // MSAA resolve target (or null)
  SDL_GPUTexture *depth;
  SDL_GPUTextureFormat depth_format;
  SDL_GPUSampleCount samples;
  int w, h;
  bool clear_color;
  SDL_FColor clear;
  bool clear_depth;
  SDL_GPUStoreOp depth_store;
  // viewport / scissor in GL window coordinates (origin bottom-left); unset = whole target
  bool viewport_set, scissor_set;
  int vp_x, vp_y, vp_w, vp_h;
  int sc_x, sc_y, sc_w, sc_h;
} RenderTargetDesc;

// the camera and viewport (x, y, w, h in target pixels) of the scene render in progress: what
// three passes to onBeforeRender (renderer.getCurrentViewport)
extern const Camera *g_draw_camera;
extern double g_draw_viewport[4];

typedef struct RenderStats { int calls, triangles, instances; } RenderStats;
extern RenderStats g_render_stats;

void renderer_init(void);
void renderer_shutdown(void);
// renders `scene` from `cam` into the target (like renderer.render with a render target)
void render_scene(SDL_GPUCommandBuffer *cb, Node *scene, Camera *cam, const RenderTargetDesc *rt);
// The shadow caster material for a mesh (three's getDepthMaterial): `side` is the side the
// caster must be drawn with (material.shadowSide, else the flipped side).
typedef Material *(*DepthMaterialFn)(Node *n, Material *src, Side side, void *user);
// shadow pass: depth-only render of the casters, in scene order, like WebGLShadowMap
void render_shadow(SDL_GPUCommandBuffer *cb, Node *scene, Camera *light_cam, const RenderTargetDesc *rt,
                   DepthMaterialFn depth_for, void *user);
