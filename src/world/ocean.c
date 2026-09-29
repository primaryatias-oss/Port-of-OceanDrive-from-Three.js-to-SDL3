// Port of src/world/ocean.js. The wave table, swell terms and grid spacing (GRID_K) are baked
// into the captured shader text; only the grid, the material and the per-frame uniforms are here.
#include "world/ocean.h"

#include <math.h>

#include "gfx/three_mat.h"
#include "quality.h"

struct Ocean { Node *mesh; Material *mat; Surf *surf; };

static constexpr double GRID_R0 = 0.25;
static constexpr double GRID_R1 = 25000;

// polarGrid: vertex 0 = centre, then rings x segs
static Geometry *polar_grid(int rings, int segs, double r0, double r1) {
  double q = pow(r1 / r0, 1.0 / (rings - 1));
  int nv = rings * segs + 1;
  Geometry *g = geo_new();
  float *pos = geo_set_attr(g, "position", 3, nv);
  int o = 3;
  for (int i = 0; i < rings; i++) {
    double r = r0 * pow(q, i);
    for (int j = 0; j < segs; j++) {
      double a = ((double)j / segs) * PI_D * 2;
      pos[o++] = (float)(cos(a) * r);
      pos[o++] = 0;
      pos[o++] = (float)(sin(a) * r);
    }
  }
  int n = segs * 3 + (rings - 1) * segs * 6;
  uint32_t *idx = xmalloc((size_t)n * sizeof *idx);
  int k = 0;
  for (int j = 0; j < segs; j++) {
    idx[k++] = 0;
    idx[k++] = (uint32_t)(1 + ((j + 1) % segs));
    idx[k++] = (uint32_t)(1 + j);
  }
  for (int i = 0; i < rings - 1; i++)
    for (int j = 0; j < segs; j++) {
      uint32_t a = (uint32_t)(1 + i * segs + j), b = (uint32_t)(1 + i * segs + ((j + 1) % segs));
      uint32_t c = a + (uint32_t)segs, d = b + (uint32_t)segs;
      idx[k++] = a; idx[k++] = b; idx[k++] = c;
      idx[k++] = b; idx[k++] = d; idx[k++] = c;
    }
  geo_set_index(g, idx, n);
  free(idx);
  return g;
}

Ocean *create_ocean(Node *scene, Surf *surf) {
  Ocean *O = xcalloc(1, sizeof *O);
  O->surf = surf;
  MatDesc d = md_shader();
  d.name = "Ocean";
  d.fog = true;
  d.transparent = true;
  d.prog[MV_PLAIN] = PROG_OCEAN;
  O->mat = mat_three(&d);
  mat_set_float(O->mat, "uTime", 0);
  mat_set_vec2(O->mat, "uCam", (V2){ 0, 0 });
  surf_apply(surf, O->mat);   // Object.assign(material.uniforms, surf.uniforms): shared values

  Geometry *g = polar_grid(QUALITY.oceanRings, QUALITY.oceanSegs, GRID_R0, GRID_R1);
  O->mesh = node_mesh(gpu_geometry(g), O->mat);
  geo_free(g);
  snprintf(O->mesh->name, sizeof O->mesh->name, "ocean");
  O->mesh->frustum_culled = false;
  O->mesh->render_order = 3;
  node_add(scene, O->mesh);
  return O;
}

Node *ocean_mesh(const Ocean *o) { return o->mesh; }

void ocean_update(Ocean *o, double time, const Camera *camera) {
  mat_set_float(o->mat, "uTime", time);
  if (camera) mat_set_vec2(o->mat, "uCam", (V2){ camera->node->position.x, camera->node->position.z });
  // the surf uniforms are shared objects in JS: always the surf clock's current values
  surf_apply(o->surf, o->mat);
}
