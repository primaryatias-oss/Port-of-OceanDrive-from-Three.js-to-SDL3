// Port of src/world/lod.js.
#include "world/lod.h"

#include "quality.h"

static const LodRadius RADIUS[3] = {
  [TIER_HIGH] = { 170, 260, 150 },
  [TIER_MEDIUM] = { 130, 190, 110 },
  [TIER_LOW] = { 80, 120, 70 },
};

LodRadius lod_radius(void) { return RADIUS[QUALITY.tier]; }

typedef struct LodItem { Node *obj; double z0, z1, r; } LodItem;
typedef struct LodHook { bool (*fn)(V3, void *); void *user; } LodHook;
static Vec(LodItem) g_items;
static Vec(LodHook) g_hooks;
static double g_last_z = NAN;

void lod_register(Node *obj, double z0, double z1, LodKind kind) {
  LodRadius L = lod_radius();
  vec_push(&g_items, ((LodItem){ obj, fmin(z0, z1), fmax(z0, z1), kind == LOD_CARS ? L.cars : L.detail }));
}

void lod_register_hook(bool (*fn)(V3, void *), void *user) { vec_push(&g_hooks, ((LodHook){ fn, user })); }

bool lod_update(const Camera *camera) {
  V3 p = camera->node->position;
  double z = p.z;
  if (fabs(z - g_last_z) < 2) return false;   // (false for NaN: the first call always runs)
  g_last_z = z;
  bool changed = false;
  for (size_t i = 0; i < g_items.len; i++) {
    LodItem *it = &g_items.data[i];
    double d = fmax(0, fmax(it->z0 - z, z - it->z1));
    bool v = d < it->r;
    if (it->obj->visible != v) { it->obj->visible = v; changed = true; }
  }
  for (size_t i = 0; i < g_hooks.len; i++)
    if (g_hooks.data[i].fn(p, g_hooks.data[i].user)) changed = true;
  return changed;
}
