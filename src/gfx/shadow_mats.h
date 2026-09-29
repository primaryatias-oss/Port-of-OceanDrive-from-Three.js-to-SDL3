// Shadow caster materials (three's WebGLShadowMap.getDepthMaterial); a DepthMaterialFn.
#pragma once

#include "gfx/scene.h"

Material *shadow_depth_material(Node *n, Material *src, Side side, void *user);
