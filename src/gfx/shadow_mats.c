// WebGLShadowMap.getDepthMaterial: the MeshDepthMaterial a shadow caster is drawn with. Its
// program follows the object (plain / instanced / instance colours / batched) and the source
// material (map, alpha test or alpha-to-coverage, the side it casts with). One Material per
// (source material, side) holds the map and alphaTest, like three's per-material clones.
#include "gfx/shadow_mats.h"

#include "gfx/three_mat.h"

typedef struct DepthKey { bool batch, inst, icol, map, atest; Side side; ProgramId prog; } DepthKey;

// the depth programs the scene compiles (shaders/programs/depth_*.prog)
static const DepthKey DEPTH_PROGS[] = {
  { false, false, false, false, false, SIDE_BACK, PROG_DEPTH_BACK },
  { false, false, false, false, false, SIDE_DOUBLE, PROG_DEPTH_DBL },   // hand-derived (see the .prog)
  { true, false, false, true, false, SIDE_BACK, PROG_DEPTH_BATCH_MAP_BACK },
  { false, true, false, false, false, SIDE_DOUBLE, PROG_DEPTH_INST_DBL },
  { false, true, true, false, false, SIDE_BACK, PROG_DEPTH_INST_ICOL_BACK },
  { false, true, true, false, false, SIDE_FRONT, PROG_DEPTH_INST_ICOL_FRONT },   // hand-derived (see the .prog)
  { false, true, true, false, false, SIDE_DOUBLE, PROG_DEPTH_INST_ICOL_DBL },    // hand-derived (see the .prog)
  { false, true, true, true, true, SIDE_DOUBLE, PROG_DEPTH_INST_ICOL_MAP_ATEST_DBL },
  { false, false, false, true, true, SIDE_BACK, PROG_DEPTH_MAP_ATEST_BACK },
  { false, false, false, true, true, SIDE_DOUBLE, PROG_DEPTH_MAP_ATEST_DBL },
  { false, false, false, true, false, SIDE_BACK, PROG_DEPTH_MAP_BACK },
  { false, false, false, true, false, SIDE_FRONT, PROG_DEPTH_MAP_FRONT },
};

typedef struct Cached { const Material *src; Side side; bool inst, icol, batch; Material *m; } Cached;
static Vec(Cached) g_cache;

static const char *const SIDE_NAMES[] = { "FrontSide", "BackSide", "DoubleSide" };

Material *shadow_depth_material(Node *n, Material *src, Side side, void *user) {
  (void)user;
  bool inst = n->kind == NODE_INSTANCED, icol = inst && n->inst_color != nullptr;
  bool batch = n->kind == NODE_BATCHED;
  if (n->custom_depth) {
    // customDepthMaterial: three still sets its side, map and alphaTest from the source
    // material; the port's custom depth materials are built with the matching values
    Material *c = n->custom_depth;
    if (c->side != side) FATAL("custom depth material '%s': side %s expected", c->name, SIDE_NAMES[side]);
    return c;
  }
  for (size_t i = 0; i < g_cache.len; i++) {
    Cached *e = &g_cache.data[i];
    if (e->src == src && e->side == side && e->inst == inst && e->icol == icol && e->batch == batch) return e->m;
  }
  // alphaToCoverage is approximated by alphaTest 0.5 in the shadow pass
  double alpha_test = src->alpha_to_coverage ? 0.5 : src->alpha_test;
  bool map = src->shadow_alpha_map != nullptr, atest = alpha_test > 0;
  ProgramId prog = PROG_COUNT;
  for (size_t i = 0; i < ARRAY_LEN(DEPTH_PROGS); i++) {
    const DepthKey *k = &DEPTH_PROGS[i];
    if (k->batch == batch && k->inst == inst && k->icol == icol && k->map == map && k->atest == atest && k->side == side)
      prog = k->prog;
  }
  if (prog == PROG_COUNT)
    FATAL("no depth program for caster '%s' (material '%s'): %s%s%s%s%s %s", n->name, src->name ? src->name : "?",
          batch ? "batched " : "", inst ? "instanced " : "", icol ? "+colour " : "", map ? "map " : "", atest ? "alphaTest " : "",
          SIDE_NAMES[side]);
  MatDesc d = md_depth();
  d.name = "shadow depth";
  d.side = side;
  d.map = src->shadow_alpha_map;
  d.alpha_test = alpha_test;
  d.prog[batch ? MV_BATCHED : inst ? MV_INSTANCED : MV_PLAIN] = prog;
  Material *m = mat_three(&d);
  vec_push(&g_cache, ((Cached){ src, side, inst, icol, batch, m }));
  return m;
}
