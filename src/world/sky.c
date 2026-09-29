// Port of src/sky.js createSky(): dome, environment (PMREM of the skylight scene), the sun
// light with its fitted, texel-snapped shadow box.
#include "world/sky.h"

#include "gfx/pmrem.h"
#include "world/layout.h"

V3 g_sun_dir;

// fitShadow(light, box): aim the light at the box centre from 400 m along the sun direction and
// fit the orthographic shadow camera around the box in light space
static void fit_shadow(Sky *s, Box3 box) {
  V3 center = box3_center(box);
  s->sun_position = v3_add_scaled(center, g_sun_dir, 400);
  s->target_position = center;
  Camera *cam = &s->shadow_cam;
  cam->node->position = s->sun_position;
  camera_look_at(cam, center);
  node_update_matrix_world(cam->node, true);
  M4 inv = m4_invert(cam->node->matrix_world);
  Box3 lb = box3_empty();
  for (int i = 0; i < 8; i++) {
    V3 p = v3(i & 1 ? box.max.x : box.min.x, i & 2 ? box.max.y : box.min.y, i & 4 ? box.max.z : box.min.z);
    lb = box3_expand(lb, v3_apply_m4(p, inv));
  }
  cam->left = lb.min.x;
  cam->right = lb.max.x;
  cam->bottom = lb.min.y;
  cam->top = lb.max.y;
  cam->near = fmax(0.5, -lb.max.z - 60);
  cam->far = -lb.min.z + 5;
}

static void texels(Sky *s) {
  s->texX = (s->shadow_cam.right - s->shadow_cam.left) / s->map_w;
  s->texY = (s->shadow_cam.top - s->shadow_cam.bottom) / s->map_h;
}

static Texture *shadow_map_texture(int w, int h) {
  SamplerDesc sd = sampler_default();   // LinearFilter + LessEqualCompare (hardware PCF)
  sd.mipmaps = false;
  sd.compare = true;
  SDL_GPUTextureUsageFlags usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
  // DepthTexture(UnsignedIntType) is DEPTH_COMPONENT24 in WebGL2
  SDL_GPUTextureFormat fmt = SDL_GPUTextureSupportsFormat(g_gpu.dev, SDL_GPU_TEXTUREFORMAT_D24_UNORM, SDL_GPU_TEXTURETYPE_2D, usage)
    ? SDL_GPU_TEXTUREFORMAT_D24_UNORM : SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
  return tex_create_target(w, h, fmt, usage, 1, SDL_GPU_SAMPLECOUNT_1, sd);
}

// LightShadow.updateMatrices(light): camera at the light, looking at the target; the shadow
// matrix maps world space to [0, 1] shadow-map coordinates (z already in [0, 1] here)
static void update_shadow_matrices(Sky *s) {
  Camera *cam = &s->shadow_cam;
  cam->node->position = s->sun_position;
  camera_look_at(cam, s->target_position);
  camera_update(cam);
  M4 bias = { { 0.5, 0, 0, 0, 0, 0.5, 0, 0, 0, 0, 0.5, 0, 0.5, 0.5, 0.5, 1 } };   // (WebGL: z too)
  s->shadow_matrix = m4_mul(bias, m4_mul(cam->projection, cam->view));
}

static void place_shadow(Sky *s, double z) {
  if (fabs(z - s->last_z) < s->step) return;   // (NaN at start: never less than step)
  s->last_z = z;
  V3 d = v3(0, 0, z);
  double u = v3_dot(d, s->axX), v = v3_dot(d, s->axY);
  // keep the along-light part, snap the two light-space image axes to whole texels
  V3 c = v3_add_scaled(v3_add_scaled(d, s->axX, -u), s->axY, -v);
  c = v3_add_scaled(c, s->axX, js_round(u / s->texX) * s->texX);
  c = v3_add_scaled(c, s->axY, js_round(v / s->texY) * s->texY);
  c = v3_add(c, s->base);
  s->target_position = c;
  s->sun_position = v3_add_scaled(c, g_sun_dir, 400);
  update_shadow_matrices(s);
  s->shadow_wanted = true;
}

Sky *sky_create(Node *scene, const int shadow_map_size[2], double shadow_step) {
  Sky *s = xcalloc(1, sizeof *s);
  g_sun_dir = compassToDir(SUN.azimuthDeg, SUN.elevationDeg);

  // scene.fog = new FogExp2(0xffffff, FOG_DENSITY)
  frame_set_v3(G_FOG_COLOR, v3(1, 1, 1));
  frame_set_float(G_FOG_DENSITY, FOG_DENSITY);

  Material *dome_mat = mat_new(PROG_SKY, "Sky");
  dome_mat->side = SIDE_BACK;
  dome_mat->depth_write = false;
  Geometry *dg = geo_sphere3(1000, 64, 32);
  s->dome = node_mesh(gpu_geometry(dg), dome_mat);
  geo_free(dg);
  s->dome->frustum_culled = false;
  s->dome->render_order = -10;
  node_add(scene, s->dome);

  Node *env_scene = node_new(NODE_GROUP, "envScene");
  Material *env_mat = mat_new(PROG_SKY_ENV, "SkyEnv");
  env_mat->side = SIDE_BACK;
  env_mat->depth_write = false;
  Geometry *eg = geo_sphere3(100, 64, 32);
  node_add(env_scene, node_mesh(gpu_geometry(eg), env_mat));
  geo_free(eg);
  s->env = pmrem_from_scene(env_scene, 0.1, 500, 256);
  frame_set_texture(GT_ENV_MAP, s->env);

  // sun: DirectionalLight(SUN_COLOR, SUN_INTENSITY), castShadow
  s->map_w = shadow_map_size[0];
  s->map_h = shadow_map_size[1];
  s->bias = -0.0003;
  s->normal_bias = 0.035;
  s->radius = 1.5;
  s->shadow_cam = (Camera){ .node = node_new(NODE_GROUP, "shadowCamera"), .ortho = true, .zoom = 1 };
  const double SPAN = 80;
  fit_shadow(s, (Box3){ v3(-62, -1.5, -SPAN), v3(96, 30, SPAN) });
  update_shadow_matrices(s);
  s->base = s->target_position;
  s->axX = m4_col3(s->shadow_cam.node->matrix_world, 0);
  s->axY = m4_col3(s->shadow_cam.node->matrix_world, 1);
  texels(s);
  s->step = shadow_step;
  s->last_z = NAN;
  s->shadow_map = shadow_map_texture(s->map_w, s->map_h);
  frame_set_texture(GT_SHADOW_MAP, s->shadow_map);
  s->shadow_wanted = true;
  return s;
}

bool sky_lower_shadow(Sky *s) {
  if (s->map_w <= 1024) return false;
  s->map_w /= 2;
  s->map_h /= 2;
  tex_destroy(s->shadow_map);
  s->shadow_map = shadow_map_texture(s->map_w, s->map_h);
  frame_set_texture(GT_SHADOW_MAP, s->shadow_map);
  texels(s);
  s->step *= 2;
  s->last_z = NAN;
  s->shadow_wanted = true;
  return true;
}

void sky_update(Sky *s, Camera *cam) {
  node_update_matrix_world(cam->node, true);
  s->dome->position = m4_get_position(cam->node->matrix_world);
  place_shadow(s, s->dome->position.z);
}

void sky_set_frame(Sky *s, const Camera *cam) {
  // WebGLLights.setupView: direction = light position - target position, into view space
  V3 dir = v3_transform_dir(v3_sub(s->sun_position, s->target_position), cam->view);
  frame_set_v3(G_DIR_LIGHT_DIRECTION, dir);
  Color c = color_scale(sun_color(), SUN_INTENSITY);
  frame_set_v3(G_DIR_LIGHT_COLOR, v3(c.r, c.g, c.b));
  frame_set_v3(G_AMBIENT_LIGHT_COLOR, v3s(0));
  frame_set_float(G_DIR_SHADOW_INTENSITY, 1);
  frame_set_float(G_DIR_SHADOW_BIAS, s->bias);
  frame_set_float(G_DIR_SHADOW_NORMAL_BIAS, s->normal_bias);
  frame_set_float(G_DIR_SHADOW_RADIUS, s->radius);
  frame_set_v2(G_DIR_SHADOW_MAP_SIZE, v2(s->map_w, s->map_h));
  frame_set_m4(G_DIR_SHADOW_MATRIX, s->shadow_matrix);
}
