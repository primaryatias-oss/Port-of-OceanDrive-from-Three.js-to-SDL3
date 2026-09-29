// three.js material classes on top of Material: MeshStandardMaterial, MeshPhysicalMaterial,
// MeshBasicMaterial, MeshDepthMaterial, PointsMaterial and ShaderMaterial. A MatDesc holds the
// JS material's properties (md_*() return three's defaults); mat_three() creates the Material
// and sets its uniforms the way three's WebGLMaterials.refreshUniforms* do. The program for
// each variant is chosen by the caller (generated shaders/programs/*.prog) and checked against
// the material: a map, vertex colours, alpha test, side, transparency or fog that the program
// was not built for is a fatal error rather than a wrong image.
#pragma once

#include "gfx/material.h"

typedef enum MatKind { MK_STANDARD, MK_PHYSICAL, MK_BASIC, MK_DEPTH, MK_POINTS, MK_SHADER } MatKind;

typedef struct MatDesc {
  MatKind kind;
  const char *name;
  ProgramId prog[MV_COUNT];      // per variant (PROG_COUNT = none); prog[MV_PLAIN] for ordinary meshes
  // Material
  Side side;
  bool has_shadow_side;
  Side shadow_side;
  bool transparent;
  double opacity, alpha_test;
  bool alpha_to_coverage;
  BlendMode blending;
  bool premultiplied_alpha;
  SDL_GPUBlendFactor blend_src, blend_dst, blend_src_alpha, blend_dst_alpha;
  SDL_GPUBlendOp blend_op, blend_op_alpha;
  bool depth_test, depth_write, color_write;
  SDL_GPUCompareOp depth_func;   // LessEqualDepth by default
  bool polygon_offset;
  double po_factor, po_units;
  bool vertex_colors, fog, visible, force_single_pass;
  // Mesh*Material
  Color color, emissive;
  double emissive_intensity;
  Texture *map, *alpha_map, *emissive_map, *normal_map, *env_map;
  V2 normal_scale;
  // standard
  double roughness, metalness, env_map_intensity;
  // physical
  double clearcoat, clearcoat_roughness, sheen, sheen_roughness, ior, specular_intensity;
  Color sheen_color, specular_color;
  // points
  double size;
  bool size_attenuation;
} MatDesc;

MatDesc md_standard(void);   // new THREE.MeshStandardMaterial()
MatDesc md_physical(void);   // new THREE.MeshPhysicalMaterial()
MatDesc md_basic(void);      // new THREE.MeshBasicMaterial()
MatDesc md_depth(void);      // new THREE.MeshDepthMaterial() (depthPacking: BasicDepthPacking)
MatDesc md_shader(void);     // new THREE.ShaderMaterial() (uniforms are set by the caller)

// Creates the material. The scene environment (sky PMREM) is used by standard / physical
// materials without their own env_map, with scene.environmentIntensity (1.0).
Material *mat_three(const MatDesc *d);

// runtime check used by the renderer: does program `id` include parameter `param`?
bool program_has_param(ProgramId id, const char *param);
