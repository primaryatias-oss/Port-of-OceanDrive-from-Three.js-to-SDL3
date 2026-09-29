#include "gfx/material.h"

#include <stdio.h>

int g_program_tier = 0;

static const char *const GLOBAL_NAMES[G_COUNT] = {
  [G_VIEW_MATRIX] = "viewMatrix",
  [G_PROJECTION_MATRIX] = "projectionMatrix",
  [G_CAMERA_POSITION] = "cameraPosition",
  [G_IS_ORTHOGRAPHIC] = "isOrthographic",
  [G_FOG_COLOR] = "fogColor",
  [G_FOG_DENSITY] = "fogDensity",
  [G_AMBIENT_LIGHT_COLOR] = "ambientLightColor",
  [G_DIR_LIGHT_DIRECTION] = "directionalLights[0].direction",
  [G_DIR_LIGHT_COLOR] = "directionalLights[0].color",
  [G_DIR_SHADOW_INTENSITY] = "directionalLightShadows[0].shadowIntensity",
  [G_DIR_SHADOW_BIAS] = "directionalLightShadows[0].shadowBias",
  [G_DIR_SHADOW_NORMAL_BIAS] = "directionalLightShadows[0].shadowNormalBias",
  [G_DIR_SHADOW_RADIUS] = "directionalLightShadows[0].shadowRadius",
  [G_DIR_SHADOW_MAP_SIZE] = "directionalLightShadows[0].shadowMapSize",
  [G_DIR_SHADOW_MATRIX] = "directionalShadowMatrix[0]",
};
static const char *const GLOBAL_TEX_NAMES[GT_COUNT] = {
  [GT_ENV_MAP] = "envMap", [GT_SHADOW_MAP] = "directionalShadowMap_0", [GT_DFG_LUT] = "dfgLUT",
};

static double g_frame[G_COUNT][16];
static Texture *g_frame_tex[GT_COUNT];

void frame_set_m4(FrameGlobal g, M4 m) { memcpy(g_frame[g], m.e, sizeof m.e); }
void frame_set_v3(FrameGlobal g, V3 v) { g_frame[g][0] = v.x; g_frame[g][1] = v.y; g_frame[g][2] = v.z; }
void frame_set_v2(FrameGlobal g, V2 v) { g_frame[g][0] = v.x; g_frame[g][1] = v.y; }
void frame_set_float(FrameGlobal g, double v) { g_frame[g][0] = v; }
void frame_set_bool(FrameGlobal g, bool v) { g_frame[g][0] = v ? 1 : 0; }
void frame_set_texture(GlobalTexture g, Texture *t) { g_frame_tex[g] = t; }

// ---- std140 writing --------------------------------------------------------------------------

void write_leaf(uint8_t *dst, UniformType t, const double *v) {
  float f[16];
  switch (t) {
  case U_FLOAT: case U_VEC2: case U_VEC3: case U_VEC4: {
    int n = t == U_FLOAT ? 1 : t == U_VEC2 ? 2 : t == U_VEC3 ? 3 : 4;
    for (int i = 0; i < n; i++) f[i] = (float)v[i];
    memcpy(dst, f, (size_t)n * 4);
    return;
  }
  case U_INT: case U_IVEC2: case U_IVEC3: case U_IVEC4: {
    int n = t == U_INT ? 1 : t == U_IVEC2 ? 2 : t == U_IVEC3 ? 3 : 4;
    int32_t iv[4];
    for (int i = 0; i < n; i++) iv[i] = (int32_t)v[i];
    memcpy(dst, iv, (size_t)n * 4);
    return;
  }
  case U_UINT: { uint32_t u = (uint32_t)v[0]; memcpy(dst, &u, 4); return; }
  case U_BOOL: { uint32_t b = v[0] != 0; memcpy(dst, &b, 4); return; }
  case U_MAT2:   // columns padded to vec4
    for (int c = 0; c < 2; c++) { f[0] = (float)v[c * 2]; f[1] = (float)v[c * 2 + 1]; memcpy(dst + c * 16, f, 8); }
    return;
  case U_MAT3:
    for (int c = 0; c < 3; c++) {
      f[0] = (float)v[c * 3]; f[1] = (float)v[c * 3 + 1]; f[2] = (float)v[c * 3 + 2];
      memcpy(dst + c * 16, f, 12);
    }
    return;
  case U_MAT4:
    for (int i = 0; i < 16; i++) f[i] = (float)v[i];
    memcpy(dst, f, 64);
    return;
  }
}

int uniform_leaf_offset(const UniformLeaf *l, const char *name, UniformType *type) {
  for (; l && l->name; l++)
    if (!strcmp(l->name, name)) { if (type) *type = l->type; return l->offset; }
  return -1;
}

static int type_width(UniformType t) {   // doubles a value of this type occupies
  switch (t) {
  case U_VEC2: case U_IVEC2: return 2;
  case U_VEC3: case U_IVEC3: return 3;
  case U_VEC4: case U_IVEC4: case U_MAT2: return 4;
  case U_MAT3: return 9;
  case U_MAT4: return 16;
  default: return 1;
  }
}

// ---- programs --------------------------------------------------------------------------------

static Program g_programs[PROG_COUNT];

Program *program_get(ProgramId id) {
  CHECK(id >= 0 && id < PROG_COUNT);
  Program *p = &g_programs[id];
  if (p->vs) return p;
  p->id = id;
  p->info = &PROGRAMS[id];
  int var = p->info->tier_variant[g_program_tier];
  if (var < 0) FATAL("program %s is not built for quality tier %d (see its `tiers` line)", p->info->name, g_program_tier);
  p->pv = &p->info->variants[var];
  p->vs = gpu_shader(p->pv->vs);
  p->fs = gpu_shader(p->pv->fs);
  Vec(FrameSlot) fs = {};
  for (int stage = 0; stage < 2; stage++) {
    const BlockInfo *bl = stage ? p->pv->fblock : p->pv->vblock;
    for (int g = 0; g < G_COUNT; g++) {
      UniformType t;
      int off = uniform_leaf_offset(bl[0].leaves, GLOBAL_NAMES[g], &t);
      if (off >= 0) vec_push(&fs, (FrameSlot){ (uint8_t)stage, (uint16_t)off, (uint8_t)t, (uint16_t)g });
    }
    p->obj[stage] = (ObjSlots){
      uniform_leaf_offset(bl[1].leaves, "modelMatrix", nullptr), uniform_leaf_offset(bl[1].leaves, "modelViewMatrix", nullptr),
      uniform_leaf_offset(bl[1].leaves, "normalMatrix", nullptr), uniform_leaf_offset(bl[1].leaves, "receiveShadow", nullptr),
    };
  }
  p->frame_slots = fs.data;
  p->nframe_slots = (int)fs.len;
  return p;
}

void programs_shutdown(void) {
  for (int i = 0; i < PROG_COUNT; i++) {
    Program *p = &g_programs[i];
    if (!p->vs) continue;
    SDL_ReleaseGPUShader(g_gpu.dev, p->vs);
    SDL_ReleaseGPUShader(g_gpu.dev, p->fs);
    free(p->frame_slots);
    *p = (Program){};
  }
}

void program_push_object(Program *p, SDL_GPUCommandBuffer *cb, const ObjectUniforms *o) {
  for (int stage = 0; stage < 2; stage++) {
    const BlockInfo *bl = stage ? &p->pv->fblock[1] : &p->pv->vblock[1];
    if (!bl->size) continue;
    uint8_t buf[512];
    CHECK(bl->size <= (int)sizeof buf);
    memset(buf, 0, (size_t)bl->size);
    const ObjSlots *s = &p->obj[stage];
    if (s->model >= 0) write_leaf(buf + s->model, U_MAT4, o->model.e);
    if (s->model_view >= 0) write_leaf(buf + s->model_view, U_MAT4, o->model_view.e);
    if (s->normal >= 0) write_leaf(buf + s->normal, U_MAT3, o->normal.e);
    if (s->receive_shadow >= 0) { double r = o->receive_shadow; write_leaf(buf + s->receive_shadow, U_BOOL, &r); }
    if (stage) SDL_PushGPUFragmentUniformData(cb, 1, buf, (uint32_t)bl->size);
    else SDL_PushGPUVertexUniformData(cb, 1, buf, (uint32_t)bl->size);
  }
}

// ---- materials -------------------------------------------------------------------------------

static int g_material_ids;

Material *mat_new(ProgramId plain, const char *name) {
  Material *m = xcalloc(1, sizeof *m);
  m->name = name;
  m->id = g_material_ids++;
  for (int v = 0; v < MV_COUNT; v++) m->prog[v] = PROG_COUNT;
  m->prog[MV_PLAIN] = plain;
  m->side = SIDE_FRONT;
  m->blending = BLEND_NORMAL;
  m->blend_src = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
  m->blend_dst = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
  m->blend_op = SDL_GPU_BLENDOP_ADD;
  m->depth_test = m->depth_write = m->color_write = true;
  m->depth_func = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
  m->visible = true;
  m->version = 1;
  m->tex_version = 1;
  return m;
}

Material *mat_clone(const Material *src) {
  Material *m = xcalloc(1, sizeof *m);
  *m = *src;
  m->id = g_material_ids++;
  memset(m->bind, 0, sizeof m->bind);
  m->values = (typeof(m->values)){};
  m->textures = (typeof(m->textures)){};
  vec_append(&m->values, src->values.data, src->values.len);
  vec_append(&m->textures, src->textures.data, src->textures.len);
  m->version = m->tex_version = 1;
  return m;
}

void mat_add_variant(Material *m, MatVariant v, ProgramId p) { m->prog[v] = p; }

static const char *const VARIANT_NAMES[MV_COUNT] = {
  "plain", "instanced", "skinned", "plain back pass", "plain front pass", "instanced back pass", "instanced front pass",
  "batched",
};

Program *mat_program(Material *m, MatVariant v) {
  if (m->prog[v] == PROG_COUNT)
    FATAL("material '%s' has no %s program (add it with mat_add_variant)", m->name ? m->name : "?", VARIANT_NAMES[v]);
  MatBinding *b = &m->bind[v];
  if (!b->prog) b->prog = program_get(m->prog[v]);
  return b->prog;
}

static void mat_set(Material *m, const char *name, UniformType type, const double *v) {
  size_t len = strlen(name);
  if (len >= sizeof(((MatValue *)0)->name)) FATAL("uniform name too long: %s", name);
  MatValue *e = nullptr;
  for (size_t i = 0; i < m->values.len; i++)
    if (!strcmp(m->values.data[i].name, name)) e = &m->values.data[i];
  if (!e) {
    vec_push(&m->values, (MatValue){});
    e = &vec_last(&m->values);
    memcpy(e->name, name, len + 1);
  }
  e->type = type;
  memcpy(e->v, v, (size_t)type_width(type) * sizeof(double));
  m->version++;
}

void mat_set_float(Material *m, const char *name, double v) { mat_set(m, name, U_FLOAT, &v); }
void mat_set_int(Material *m, const char *name, int v) { double d = v; mat_set(m, name, U_INT, &d); }
void mat_set_bool(Material *m, const char *name, bool v) { double d = v; mat_set(m, name, U_BOOL, &d); }
void mat_set_vec2(Material *m, const char *name, V2 v) { double d[2] = { v.x, v.y }; mat_set(m, name, U_VEC2, d); }
void mat_set_vec3(Material *m, const char *name, V3 v) { double d[3] = { v.x, v.y, v.z }; mat_set(m, name, U_VEC3, d); }
void mat_set_vec4(Material *m, const char *name, V4 v) { double d[4] = { v.x, v.y, v.z, v.w }; mat_set(m, name, U_VEC4, d); }
void mat_set_color(Material *m, const char *name, Color c) { double d[3] = { c.r, c.g, c.b }; mat_set(m, name, U_VEC3, d); }
void mat_set_mat3(Material *m, const char *name, M3 v) { mat_set(m, name, U_MAT3, v.e); }
void mat_set_mat4(Material *m, const char *name, M4 v) { mat_set(m, name, U_MAT4, v.e); }

void mat_set_floats(Material *m, const char *name, const double *v, int n) {
  for (int i = 0; i < n; i++) {
    char nm[64];
    snprintf(nm, sizeof nm, "%s[%d]", name, i);
    mat_set(m, nm, U_FLOAT, &v[i]);
  }
}

void mat_set_texture(Material *m, const char *name, Texture *t) {
  for (size_t i = 0; i < m->textures.len; i++)
    if (!strcmp(m->textures.data[i].name, name)) { m->textures.data[i].t = t; m->tex_version++; m->version++; return; }
  MatTex e = { .t = t };
  CHECK(strlen(name) < sizeof e.name);
  strcpy(e.name, name);
  vec_push(&m->textures, e);
  m->tex_version++;
  m->version++;
}

Texture *mat_get_texture(const Material *m, const char *name) {
  for (size_t i = 0; i < m->textures.len; i++)
    if (!strcmp(m->textures.data[i].name, name)) return m->textures.data[i].t;
  return nullptr;
}

// value types that may fill a leaf of another type (GL converts floats / ints / bools)
static bool compatible(UniformType leaf, UniformType val) {
  if (leaf == val) return true;
  bool ls = leaf == U_FLOAT || leaf == U_INT || leaf == U_BOOL || leaf == U_UINT;
  bool vs = val == U_FLOAT || val == U_INT || val == U_BOOL || val == U_UINT;
  return ls && vs;
}

static void binding_refresh(Material *m, MatBinding *b) {
  const Program *p = b->prog;
  for (int s = 0; s < 2; s++) {
    const BlockInfo *bl = s ? &p->pv->fblock[0] : &p->pv->vblock[0];
    if (!bl->size) continue;
    if (!b->ub[s]) {
      b->ub[s] = xcalloc(1, (size_t)bl->size);
      int n = 0;
      while (bl->leaves[n].name) n++;
      b->nleaves[s] = n;
      b->leaf_value[s] = xmalloc((size_t)(n ? n : 1) * sizeof(int));
      b->mapped_values = -1;
    }
  }
  if (b->mapped_values != (int)m->values.len) {
    for (int s = 0; s < 2; s++) {
      const BlockInfo *bl = s ? &p->pv->fblock[0] : &p->pv->vblock[0];
      for (int i = 0; i < b->nleaves[s]; i++) {
        b->leaf_value[s][i] = -1;
        for (size_t k = 0; k < m->values.len; k++)
          if (!strcmp(bl->leaves[i].name, m->values.data[k].name)) {
            if (!compatible(bl->leaves[i].type, m->values.data[k].type))
              FATAL("material '%s': uniform %s set with the wrong type", m->name ? m->name : "?", m->values.data[k].name);
            b->leaf_value[s][i] = (int)k;
          }
      }
    }
    b->mapped_values = (int)m->values.len;
  }
  for (int s = 0; s < 2; s++) {
    const BlockInfo *bl = s ? &p->pv->fblock[0] : &p->pv->vblock[0];
    for (int i = 0; i < b->nleaves[s]; i++) {
      int k = b->leaf_value[s][i];
      if (k >= 0) write_leaf(b->ub[s] + bl->leaves[i].offset, bl->leaves[i].type, m->values.data[k].v);
    }
    // texture uv transforms (three refreshes mapTransform etc. from the texture each render)
    for (size_t t = 0; t < m->textures.len; t++) {
      if (!m->textures.data[t].t) continue;
      char nm[64];
      snprintf(nm, sizeof nm, "%sTransform", m->textures.data[t].name);
      int off = uniform_leaf_offset(bl->leaves, nm, nullptr);
      if (off >= 0) write_leaf(b->ub[s] + off, U_MAT3, tex_uv_transform(m->textures.data[t].t).e);
    }
  }
  b->version = m->version;
}

// black defaults for samplers nothing was bound to (an unbound GL texture unit samples zero)
static Texture *g_default_tex[2];

static Texture *default_texture(SamplerType st) {
  int k = st == S_2D_SHADOW ? 1 : 0;
  if (st != S_2D && st != S_2D_SHADOW) FATAL("no default texture for sampler type %d; bind one explicitly", (int)st);
  if (g_default_tex[k]) return g_default_tex[k];
  SamplerDesc sd = sampler_default();
  sd.mipmaps = false;
  if (k == 0) {
    static const uint8_t black[4] = { 0, 0, 0, 255 };
    TexUpload u = { .rgba = black, .w = 1, .h = 1, .sampler = sd };
    g_default_tex[0] = tex_create_rgba8(&u);
  } else {
    sd.compare = true;
    g_default_tex[1] = tex_create_target(1, 1, SDL_GPU_TEXTUREFORMAT_D16_UNORM,
      SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET, 1, SDL_GPU_SAMPLECOUNT_1, sd);
  }
  return g_default_tex[k];
}

void mat_bind(Material *m, MatVariant v, SDL_GPUCommandBuffer *cb, SDL_GPURenderPass *rp) {
  Program *p = mat_program(m, v);
  MatBinding *b = &m->bind[v];
  if (b->version != m->version) binding_refresh(m, b);
  for (int i = 0; i < p->nframe_slots; i++) {
    const FrameSlot *s = &p->frame_slots[i];
    write_leaf(b->ub[s->stage] + s->offset, (UniformType)s->type, g_frame[s->global]);
  }
  if (p->pv->vblock[0].size) SDL_PushGPUVertexUniformData(cb, 0, b->ub[0], (uint32_t)p->pv->vblock[0].size);
  if (p->pv->fblock[0].size) SDL_PushGPUFragmentUniformData(cb, 0, b->ub[1], (uint32_t)p->pv->fblock[0].size);
  for (int s = 0; s < 2; s++) {
    const SamplerSlot *sl = s ? p->pv->fsamplers : p->pv->vsamplers;
    SDL_GPUTextureSamplerBinding tb[16];
    int n = 0;
    for (; sl && sl->name; sl++, n++) {
      CHECK(sl->binding == n && n < 16);
      Texture *t = mat_get_texture(m, sl->name);
      if (!t)
        for (int g = 0; g < GT_COUNT; g++)
          if (!strcmp(sl->name, GLOBAL_TEX_NAMES[g])) t = g_frame_tex[g];
      if (!t) t = default_texture(sl->type);
      tb[n] = (SDL_GPUTextureSamplerBinding){ t->gpu, sampler_get(&t->sampler) };
    }
    if (!n) continue;
    if (s) SDL_BindGPUFragmentSamplers(rp, 0, tb, (uint32_t)n);
    else SDL_BindGPUVertexSamplers(rp, 0, tb, (uint32_t)n);
  }
}
