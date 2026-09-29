#include "gfx/three_mat.h"

static MatDesc md_base(MatKind k) {
  MatDesc d = {};
  d.kind = k;
  for (int v = 0; v < MV_COUNT; v++) d.prog[v] = PROG_COUNT;
  d.side = SIDE_FRONT;
  d.opacity = 1;
  d.blending = BLEND_NORMAL;
  d.blend_src = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
  d.blend_dst = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
  d.blend_op = SDL_GPU_BLENDOP_ADD;
  d.depth_test = d.depth_write = d.color_write = true;
  d.depth_func = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
  d.visible = true;
  d.fog = true;
  d.color = color_hex(0xffffff);
  d.emissive = color_rgb(0, 0, 0);
  d.emissive_intensity = 1;
  d.normal_scale = v2(1, 1);
  return d;
}

MatDesc md_standard(void) {
  MatDesc d = md_base(MK_STANDARD);
  d.roughness = 1;
  d.metalness = 0;
  d.env_map_intensity = 1;
  return d;
}

MatDesc md_physical(void) {
  MatDesc d = md_standard();
  d.kind = MK_PHYSICAL;
  d.ior = 1.5;
  d.sheen_roughness = 1;
  d.sheen_color = color_rgb(0, 0, 0);
  d.specular_intensity = 1;
  d.specular_color = color_rgb(1, 1, 1);
  return d;
}

MatDesc md_basic(void) { return md_base(MK_BASIC); }

MatDesc md_depth(void) {
  MatDesc d = md_base(MK_DEPTH);
  d.fog = false;
  return d;
}

MatDesc md_shader(void) {
  MatDesc d = md_base(MK_SHADER);
  d.fog = false;
  return d;
}

bool program_has_param(ProgramId id, const char *param) {
  char key[80];
  snprintf(key, sizeof key, ";%s;", param);
  return strstr(PROGRAMS[id].params, key) != nullptr;
}

static void expect(const MatDesc *d, ProgramId id, const char *param, bool want, const char *what) {
  bool has = program_has_param(id, param);
  if (has != want)
    FATAL("material '%s': program %s %s '%s' but the material %s %s", d->name ? d->name : "?", PROGRAMS[id].name,
          has ? "has" : "lacks", param, want ? "needs it" : "does not use", what);
}

// the program for variant v was built for this material?
static void check_program(const MatDesc *d, MatVariant v, ProgramId id) {
  bool lit = d->kind == MK_STANDARD || d->kind == MK_PHYSICAL;
  bool inst = v == MV_INSTANCED || v == MV_INSTANCED_BACK || v == MV_INSTANCED_FRONT;
  expect(d, id, "instancing", inst, "(object kind)");
  expect(d, id, "batching", v == MV_BATCHED, "(object kind)");
  expect(d, id, "skinning", v == MV_SKINNED, "(object kind)");
  if (d->kind == MK_SHADER) return;   // ShaderMaterial programs carry only the object defines
  expect(d, id, "map", d->map != nullptr, "map");
  expect(d, id, "alphaMap", d->alpha_map != nullptr, "alphaMap");
  expect(d, id, "alphaTest", d->alpha_test > 0, "alphaTest");
  expect(d, id, "alphaToCoverage", d->alpha_to_coverage, "alphaToCoverage");
  if (d->kind != MK_DEPTH) {
    expect(d, id, "vertexColors", d->vertex_colors, "vertexColors");
    expect(d, id, "emissiveMap", d->emissive_map != nullptr, "emissiveMap");
    expect(d, id, "normalMap", d->normal_map != nullptr, "normalMap");
    expect(d, id, "opaque", !d->transparent && d->blending == BLEND_NORMAL && !d->alpha_to_coverage, "an opaque blend");
    expect(d, id, "fog", d->fog, "fog");
    expect(d, id, "envMap", lit || d->env_map != nullptr, "an environment map");
    expect(d, id, "premultipliedAlpha", d->premultiplied_alpha, "premultipliedAlpha");
  }
  if (d->kind == MK_PHYSICAL) {
    expect(d, id, "clearcoat", d->clearcoat > 0, "clearcoat");
    expect(d, id, "sheen", d->sheen > 0, "sheen");
  }
  bool back_pass = v == MV_PLAIN_BACK || v == MV_INSTANCED_BACK;
  bool front_pass = v == MV_PLAIN_FRONT || v == MV_INSTANCED_FRONT;
  Side side = back_pass ? SIDE_BACK : front_pass ? SIDE_FRONT : d->side;
  expect(d, id, "doubleSided", side == SIDE_DOUBLE, "DoubleSide");
  expect(d, id, "flipSided", side == SIDE_BACK, "BackSide");
}

Material *mat_three(const MatDesc *d) {
  CHECK(d->prog[MV_PLAIN] != PROG_COUNT || d->prog[MV_INSTANCED] != PROG_COUNT || d->prog[MV_SKINNED] != PROG_COUNT ||
        d->prog[MV_BATCHED] != PROG_COUNT);
  Material *m = mat_new(d->prog[MV_PLAIN], d->name);
  bool dbl_transparent = d->transparent && d->side == SIDE_DOUBLE && !d->force_single_pass;
  for (int v = 0; v < MV_COUNT; v++) {
    m->prog[v] = d->prog[v];
    // (a transparent DoubleSide material is only drawn through its back / front pass programs;
    // its plain / instanced slot just marks the object kinds it is used with)
    bool pass_only = dbl_transparent && (v == MV_PLAIN || v == MV_INSTANCED);
    if (d->prog[v] != PROG_COUNT && !pass_only) check_program(d, (MatVariant)v, d->prog[v]);
  }
  if (dbl_transparent)   // drawn as a BackSide then a FrontSide pass: both programs must exist
    for (int v = 0; v < 2; v++) {
      MatVariant base = v ? MV_INSTANCED : MV_PLAIN;
      MatVariant b = v ? MV_INSTANCED_BACK : MV_PLAIN_BACK, f = v ? MV_INSTANCED_FRONT : MV_PLAIN_FRONT;
      if (d->prog[base] != PROG_COUNT && (d->prog[b] == PROG_COUNT || d->prog[f] == PROG_COUNT))
        FATAL("material '%s': transparent DoubleSide needs back / front pass programs", d->name ? d->name : "?");
    }
  m->side = d->side;
  m->has_shadow_side = d->has_shadow_side;
  m->shadow_side = d->shadow_side;
  m->transparent = d->transparent;
  m->blending = d->blending;
  m->premultiplied_alpha = d->premultiplied_alpha;
  m->blend_src = d->blend_src;
  m->blend_dst = d->blend_dst;
  m->blend_src_alpha = d->blend_src_alpha;
  m->blend_dst_alpha = d->blend_dst_alpha;
  m->blend_op = d->blend_op;
  m->blend_op_alpha = d->blend_op_alpha;
  m->depth_test = d->depth_test;
  m->depth_write = d->depth_write;
  m->depth_func = d->depth_func;
  m->color_write = d->color_write;
  m->polygon_offset = d->polygon_offset;
  m->po_factor = (float)d->po_factor;
  m->po_units = (float)d->po_units;
  m->alpha_to_coverage = d->alpha_to_coverage;
  m->force_single_pass = d->force_single_pass;
  m->visible = d->visible;
  m->alpha_test = d->alpha_test;
  m->shadow_alpha_map = d->map;
  if (d->kind == MK_SHADER) return m;

  // refreshUniformsCommon
  mat_set_float(m, "opacity", d->opacity);
  mat_set_color(m, "diffuse", d->color);
  mat_set_color(m, "emissive", color_scale(d->emissive, d->emissive_intensity));
  if (d->map) mat_set_texture(m, "map", d->map);
  if (d->alpha_map) mat_set_texture(m, "alphaMap", d->alpha_map);
  if (d->normal_map) {
    mat_set_texture(m, "normalMap", d->normal_map);
    V2 ns = d->normal_scale;
    if (d->side == SIDE_BACK) ns = v2(-ns.x, -ns.y);
    mat_set_vec2(m, "normalScale", ns);
  }
  if (d->emissive_map) mat_set_texture(m, "emissiveMap", d->emissive_map);
  if (d->alpha_test > 0) mat_set_float(m, "alphaTest", d->alpha_test);
  bool lit = d->kind == MK_STANDARD || d->kind == MK_PHYSICAL;
  if (d->env_map || lit) {
    if (d->env_map) mat_set_texture(m, "envMap", d->env_map);   // else the global scene environment
    mat_set_mat3(m, "envMapRotation", (M3){ { 1, 0, 0, 0, 1, 0, 0, 0, 1 } });   // Euler(0,0,0), transposed
  }
  if (lit) {
    // refreshUniformsStandard; with the scene environment, envMapIntensity is the scene's
    mat_set_float(m, "metalness", d->metalness);
    mat_set_float(m, "roughness", d->roughness);
    mat_set_float(m, "envMapIntensity", d->env_map ? d->env_map_intensity : 1.0 /* scene.environmentIntensity */);
  }
  if (d->kind == MK_PHYSICAL) {
    mat_set_float(m, "ior", d->ior);
    if (d->sheen > 0) {
      mat_set_color(m, "sheenColor", color_scale(d->sheen_color, d->sheen));
      mat_set_float(m, "sheenRoughness", d->sheen_roughness);
    }
    if (d->clearcoat > 0) {
      mat_set_float(m, "clearcoat", d->clearcoat);
      mat_set_float(m, "clearcoatRoughness", d->clearcoat_roughness);
    }
    mat_set_float(m, "specularIntensity", d->specular_intensity);
    mat_set_color(m, "specularColor", d->specular_color);
  }
  return m;
}
