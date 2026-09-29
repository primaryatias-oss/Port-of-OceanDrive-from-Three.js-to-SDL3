// Port of src/world/street.js: cambered, weathered asphalt with worn paint, concrete curbs and
// gutter pans, pale slab sidewalks with paver sections, ADA ramps at the crosswalks, storm
// drains, manholes and the street furniture (merged, vertex-coloured).
#include "world/street.h"

#include "canvas/canvas.h"
#include "gfx/three_mat.h"
#include "textures/noise.h"
#include "world/layout.h"
#include "world/lod.h"
#include "world/palms.h"

#define TAU (PI_D * 2)
static constexpr double Z = WORLD_Z;
static constexpr double CURB_W = 0.15;
static constexpr double GUTTER_W = 0.3;
static constexpr ZRange CW = { CROSSWALK_Z - 2, CROSSWALK_Z + 2 };
typedef struct Ramp { double z0, z1; } Ramp;
static constexpr double RAMP_RUN = 1.3;
static const Ramp RAMP = { CROSSWALK_Z - 1.5, CROSSWALK_Z + 1.5 };
static const double LANE_C[2] = { LANES.centerX - 1.75, LANES.centerX + 1.75 };
static double promenadeX(double z) { return PARK.promenadeX + 2.6 * sin(z / 19) + 1.2 * sin(z / 7.3); }
static constexpr double OPEN_W = CROSS.hw + CROSS.R;   // curb opening half width
static constexpr double CROSS_WEST = -400;            // cross streets run west into the haze

// Intersections: the district's cross streets carry crosswalks on all three legs; the far ones
// are plain pavement with a stop line.
typedef struct Iv { double a, b; } Iv;
static int g_nnear;
static const CrossStreet *g_near[NCROSS_STREETS];
static Iv g_legs[NCROSS_STREETS * 2];        // Ocean Drive crosswalks
static int g_nlegs;
static Ramp g_leg_ramps[NCROSS_STREETS * 2]; // curb ramps at both curbs
static Ramp g_ramps[NCROSS_STREETS * 2 + 1]; // [RAMP, ...LEG_RAMPS] sorted by z0
static int g_nramps;
static Iv g_openings[NCROSS_STREETS];

static void sort_iv(Iv *v, int n) {   // (p, q) => p[0] - q[0], stable
  for (int i = 1; i < n; i++) {
    Iv x = v[i];
    int j = i - 1;
    while (j >= 0 && v[j].a - x.a > 0) { v[j + 1] = v[j]; j--; }
    v[j + 1] = x;
  }
}

static void init_layout(void) {
  g_nnear = 0;
  g_nlegs = 0;
  for (int i = 0; i < NCROSS_STREETS; i++) {
    const CrossStreet *c = &CROSS_STREETS[i];
    g_openings[i] = (Iv){ c->z - OPEN_W, c->z + OPEN_W };
    if (c->far) continue;
    g_near[g_nnear++] = c;
    double legs[2][2];
    crossLegs(c->z, legs);
    for (int k = 0; k < 2; k++) g_legs[g_nlegs++] = (Iv){ legs[k][0], legs[k][1] };
  }
  for (int i = 0; i < g_nlegs; i++) g_leg_ramps[i] = (Ramp){ g_legs[i].a + 0.5, g_legs[i].b - 0.5 };
  g_ramps[0] = RAMP;
  for (int i = 0; i < g_nlegs; i++) g_ramps[i + 1] = g_leg_ramps[i];
  g_nramps = g_nlegs + 1;
  for (int i = 1; i < g_nramps; i++) {   // .sort((a, b) => a.z0 - b.z0), stable
    Ramp x = g_ramps[i];
    int j = i - 1;
    while (j >= 0 && g_ramps[j].z0 - x.z0 > 0) { g_ramps[j + 1] = g_ramps[j]; j--; }
    g_ramps[j + 1] = x;
  }
}

// [a, b] minus sorted exclusion intervals
static int segments(double a, double b, const Iv *excl0, int nex, Iv *out) {
  Iv excl[64];
  CHECK(nex <= 64);
  memcpy(excl, excl0, sizeof(Iv) * (size_t)nex);
  sort_iv(excl, nex);
  int n = 0;
  double s = a;
  for (int i = 0; i < nex; i++) {
    double ea = excl[i].a, eb = excl[i].b;
    if (eb <= s) continue;
    if (ea >= b) break;
    if (ea > s) out[n++] = (Iv){ s, fmin(ea, b) };
    s = fmax(s, eb);
  }
  if (b > s) out[n++] = (Iv){ s, b };
  return n;
}
// true where hotel-side furniture would stand in a cross street or its corner ramps
static bool in_crossing(double x, double z) {
  const CrossStreet *c = crossStreetAt(z, 1.2);
  return x < SIDEWALK_W.x1 + 0.5 && c && fabs(z - c->z) < OPEN_W + 1.2;
}
static bool in_leg(double z) {
  for (int i = 0; i < g_nlegs; i++) if (z > g_legs[i].a - 1 && z < g_legs[i].b + 1) return true;
  return false;
}

// CanvasTexture with repeat wrapping (tex())
static Texture *tex(const Canvas *c, bool srgb, int aniso) { return canvas_texture(c, srgb, WRAP_REPEAT, aniso); }

// normal map from a height array (tileable): a DataTexture
static Texture *normal_tex(const float *H, int S, double strength) {
  uint8_t *d = xmalloc((size_t)S * S * 4);
  for (int y = 0; y < S; y++)
    for (int x = 0; x < S; x++) {
      double l = H[y * S + ((x - 1 + S) % S)], r = H[y * S + ((x + 1) % S)];
      double u = H[((y - 1 + S) % S) * S + x], dn = H[((y + 1) % S) * S + x];
      double nx = (l - r) * strength, ny = (dn - u) * strength, len = js_hypot3(nx, ny, 1);
      size_t i = ((size_t)y * S + x) * 4;
      d[i] = js_u8((nx / len * 0.5 + 0.5) * 255);
      d[i + 1] = js_u8((ny / len * 0.5 + 0.5) * 255);
      d[i + 2] = js_u8((1 / len * 0.5 + 0.5) * 255);
      d[i + 3] = 255;
    }
  SamplerDesc s = sampler_default();
  s.wrap_s = s.wrap_t = WRAP_REPEAT;
  s.anisotropy = 8;
  s.mag = FILTER_NEAREST;   // DataTexture's default magFilter (the JS sets only minFilter)
  TexUpload up = { .rgba = d, .w = S, .h = S, .srgb = false, .flip_y = false, .mipmaps = true, .sampler = s };
  Texture *t = tex_create_rgba8(&up);
  free(d);
  return t;
}

// ---- asphalt: fine tile (8 m) with aggregate grain, and a macro tile (48 m, linear RGB:
// r albedo, g roughness, b oil) with repair patches, blotches and stains ------------------------
static void asphalt_textures(Texture **map, Texture **normal_map, Texture **macro) {
  const int S = 1024;
  Rng rng = rng_make(71);
  Rng *rnd = &rng;
  uint8_t *img = xmalloc((size_t)S * S * 4);
  float *f = fbm_field(256, 3, 8, 4, 0.5);
  float *H = xmalloc(sizeof(float) * (size_t)S * S);
  for (int y = 0; y < S; y++)
    for (int x = 0; x < S; x++) {
      int i = y * S + x;
      double v = 152 + (f[(y >> 2) * 256 + (x >> 2)] - 0.5) * 18;
      double r = rng_next(rnd);
      double h = f[(y >> 2) * 256 + (x >> 2)] * 0.3;
      if (r < 0.05) { v += 10 + rng_next(rnd) * 12; h += 0.4; }          // fine pale aggregate
      else if (r < 0.09) { v -= 12 + rng_next(rnd) * 10; h -= 0.25; }    // pits / binder
      else v += (rng_next(rnd) - 0.5) * 10;
      H[i] = (float)h;
      img[i * 4] = js_u8clamp(v);
      img[i * 4 + 1] = js_u8clamp(v);
      img[i * 4 + 2] = js_u8clamp(v + 2);
      img[i * 4 + 3] = 255;
    }
  free(f);
  Canvas *cv = canvas_new(S, S);
  cv_put_image_data(cv, img, 0, 0, S, S);
  free(img);
  *map = tex(cv, true, 8);
  canvas_free(cv);
  *normal_map = normal_tex(H, S, 1.6);
  free(H);

  // macro
  const int M = 512;
  Rng mrng = rng_make(72);
#define MR() rng_next(&mrng)
  Canvas *xR = canvas_new(M, M), *xG = canvas_new(M, M), *xB = canvas_new(M, M);
  Canvas *layers[3] = { xR, xG, xB };
  for (int k = 0; k < 3; k++) { cv_fill_color(layers[k], "#808080"); cv_fill_rect(layers[k], 0, 0, M, M); }
  cv_fill_color(xB, "#000");
  cv_fill_rect(xB, 0, 0, M, M);
  float *fm = fbm_field(M, 9, 6, 5, 0.5);
  uint8_t *id = xmalloc((size_t)M * M * 4);
  cv_get_image_data(xR, 0, 0, M, M, id);
  for (int i = 0; i < M * M; i++) {
    double v = 128 + (fm[i] - 0.5) * 70;
    id[i * 4] = id[i * 4 + 1] = id[i * 4 + 2] = js_u8clamp(v);
  }
  cv_put_image_data(xR, id, 0, 0, M, M);
  free(fm);
  // repair patches (utility cuts): darker newer asphalt or lighter old, crisp sealed seams
  for (int k = 0; k < 20; k++) {
    double w = 10 + MR() * 40, h = 8 + MR() * 50, x = MR() * M, y = MR() * M;
    double v = MR() < 0.6 ? 72 + MR() * 20 : 160 + MR() * 22;
    for (int ox = -M; ox <= M; ox += M)
      for (int oy = -M; oy <= M; oy += M) {
        cv_fill_rgba(xR, v, v, v, 1);
        cv_fill_rect(xR, x + ox, y + oy, w, h);
        cv_stroke_color(xR, "rgb(60,60,60)");
        cv_line_width(xR, 1.5);
        cv_stroke_rect(xR, x + ox, y + oy, w, h);
        cv_fill_color(xG, v < 128 ? "rgb(110,110,110)" : "rgb(150,150,150)");
        cv_fill_rect(xG, x + ox, y + oy, w, h);
      }
  }
  // glossy worn spots (catch the low sun)
  for (int k = 0; k < 60; k++) {
    double x = MR() * M, y = MR() * M, r = 4 + MR() * 14;
    Gradient g = cv_radial_gradient(xG, x, y, 0, x, y, r);
    grad_add_stop(&g, 0, "rgba(40,40,40,0.8)");
    grad_add_stop(&g, 1, "rgba(40,40,40,0)");
    cv_fill_gradient(xG, &g);
    cv_fill_rect(xG, x - r, y - r, r * 2, r * 2);
  }
  // oil stains (used in the parking lane and down the middle of each travel lane)
  for (int k = 0; k < 300; k++) {
    double x = MR() * M, y = MR() * M, r = 2.5 + MR() * 9;
    Gradient g = cv_radial_gradient(xB, x, y, 0, x, y, r);
    char css[64];
    snprintf(css, sizeof css, "rgba(255,255,255,%.17g)", 0.5 + MR() * 0.5);
    grad_add_stop(&g, 0, css);
    grad_add_stop(&g, 0.6, "rgba(255,255,255,0.35)");
    grad_add_stop(&g, 1, "rgba(255,255,255,0)");
    cv_fill_gradient(xB, &g);
    cv_fill_rect(xB, x - r, y - r, r * 2, r * 2);
  }
#undef MR
  uint8_t *dR = xmalloc((size_t)M * M * 4), *dG = xmalloc((size_t)M * M * 4), *dB = xmalloc((size_t)M * M * 4);
  cv_get_image_data(xR, 0, 0, M, M, dR);
  cv_get_image_data(xG, 0, 0, M, M, dG);
  cv_get_image_data(xB, 0, 0, M, M, dB);
  for (int i = 0; i < M * M; i++) { id[i * 4] = dR[i * 4]; id[i * 4 + 1] = dG[i * 4]; id[i * 4 + 2] = dB[i * 4]; id[i * 4 + 3] = 255; }
  Canvas *cm = canvas_new(M, M);
  cv_put_image_data(cm, id, 0, 0, M, M);
  *macro = tex(cm, false, 8);
  canvas_free(cm);
  free(id); free(dR); free(dG); free(dB);
  for (int k = 0; k < 3; k++) canvas_free(layers[k]);
}

static Node *mesh_of(Geometry *g, Material *m, bool cast, bool receive, const char *name) {
  Node *n = node_mesh(gpu_geometry(g), m);
  geo_free(g);
  n->cast_shadow = cast;
  n->receive_shadow = receive;
  snprintf(n->name, sizeof n->name, "%s", name);
  return n;
}

static Geometry *indexed_geometry(DVec *pos, DVec *uv, DVec *col, U32Vec *idx) {
  Geometry *g = geo_new();
  int n = (int)(pos->len / 3);
  geo_set_attr_d(g, "position", 3, n, pos->data);
  if (col) geo_set_attr_d(g, "color", 3, n, col->data);
  geo_set_attr_d(g, "uv", 2, n, uv->data);
  geo_set_index(g, idx->data, (int)idx->len);
  geo_compute_vertex_normals(g);
  vec_free(pos); vec_free(uv); vec_free(idx);
  if (col) vec_free(col);
  return g;
}

static Node *road_mesh(Material **out_mat) {
  Texture *map, *normalMap, *macro;
  asphalt_textures(&map, &normalMap, &macro);
  const int nx = 38;
  DVec pos = {}, uv = {};
  U32Vec idx = {};
  for (int i = 0; i <= nx; i++) {
    double x = PARKING.x0 + (LANES.x1 - PARKING.x0) * ((double)i / nx), y = roadHeight(x);
    const double zs[2] = { -Z, Z };
    for (int k = 0; k < 2; k++) {
      double p[3] = { x, y, zs[k] }, u[2] = { x / 8, -zs[k] / 8 };
      vec_append(&pos, p, 3);
      vec_append(&uv, u, 2);
    }
    if (i < nx) {
      uint32_t a = (uint32_t)(i * 2);
      uint32_t q[6] = { a, a + 1, a + 2, a + 1, a + 3, a + 2 };
      vec_append(&idx, q, 6);
    }
  }
  MatDesc d = md_standard();
  d.name = "street asphalt";
  d.map = map;
  d.normal_map = normalMap;
  d.normal_scale = v2(0.6, 0.6);
  d.roughness = 0.9;
  d.prog[MV_PLAIN] = PROG_STREET_ASPHALT;
  Material *mat = mat_three(&d);
  mat_set_texture(mat, "odMacro", macro);
  *out_mat = mat;
  return mesh_of(indexed_geometry(&pos, &uv, nullptr, &idx), mat, false, true, "street road");
}

// Cross-street pavement (same asphalt), from Ocean Drive's west gutter into the haze. Under the
// sidewalk corners it simply runs on below the slabs.
static Node *cross_road_mesh(Material *mat) {
  static const double xs[11] = { CROSS_WEST, -120, -70, -50, -40, -34, -30, -28, -26.5, -25.2, SIDEWALK_W.x1 };
  const int nz = 16, n = 11;
  DVec pos = {}, uv = {};
  U32Vec idx = {};
  for (int ci = 0; ci < NCROSS_STREETS; ci++) {
    const CrossStreet *c = &CROSS_STREETS[ci];
    uint32_t base = (uint32_t)(pos.len / 3);
    for (int j = 0; j <= nz; j++) {
      double dz = -OPEN_W + (2 * OPEN_W * j) / nz;
      for (int k = 0; k < n; k++) {
        double x = xs[k];
        double p[3] = { x, fabs(dz) < CROSS.hw ? crossRoadHeight(x, dz) : 0, c->z + dz }, u[2] = { x / 8, -(c->z + dz) / 8 };
        vec_append(&pos, p, 3);
        vec_append(&uv, u, 2);
      }
    }
    for (int j = 0; j < nz; j++)
      for (int i = 0; i < n - 1; i++) {
        uint32_t a = base + (uint32_t)(j * n + i), b = a + 1, d = a + (uint32_t)n, e = d + 1;
        uint32_t q[6] = { a, d, b, b, d, e };
        vec_append(&idx, q, 6);
      }
  }
  return mesh_of(indexed_geometry(&pos, &uv, nullptr, &idx), mat, false, true, "street cross roads");
}

// ---- worn road paint: one merged mesh, vertex colours, faint grime ------------------------------
typedef struct Marks { DVec pos, col, uv; U32Vec idx; } Marks;
static void mark_quad(Marks *M, double x0, double x1, double z0, double z1, uint32_t hex, double fade, int segX) {
  Color color = color_scale(color_hex(hex), fade);
  uint32_t b = (uint32_t)(M->pos.len / 3);
  for (int i = 0; i <= segX; i++) {
    double x = x0 + (x1 - x0) * ((double)i / segX), y = roadHeight(x) + 0.006;
    const double zs[2] = { z0, z1 };
    for (int k = 0; k < 2; k++) {
      double p[3] = { x, y, zs[k] }, c[3] = { color.r, color.g, color.b }, u[2] = { x / 2, zs[k] / 2 };
      vec_append(&M->pos, p, 3); vec_append(&M->col, c, 3); vec_append(&M->uv, u, 2);
    }
    if (i < segX) {
      uint32_t a = b + (uint32_t)(i * 2);
      uint32_t q[6] = { a, a + 1, a + 2, a + 1, a + 3, a + 2 };
      vec_append(&M->idx, q, 6);
    }
  }
}
// (x, z) grid quad on the cross streets, following their crown
static void mark_quad_h(Marks *M, double x0, double x1, double z0, double z1, uint32_t hex, double fade, int segX, int segZ) {
  Color color = color_scale(color_hex(hex), fade);
  uint32_t b = (uint32_t)(M->pos.len / 3);
  for (int i = 0; i <= segX; i++) {
    double x = x0 + (x1 - x0) * ((double)i / segX);
    for (int j = 0; j <= segZ; j++) {
      double z = z0 + (z1 - z0) * ((double)j / segZ);
      const CrossStreet *c = crossStreetAt(z, 0);
      double y = (c && x < SIDEWALK_W.x1 ? crossRoadHeight(x, z - c->z) : roadHeight(x)) + 0.006;
      double p[3] = { x, y, z }, cc[3] = { color.r, color.g, color.b }, u[2] = { x / 2, z / 2 };
      vec_append(&M->pos, p, 3); vec_append(&M->col, cc, 3); vec_append(&M->uv, u, 2);
    }
  }
  for (int i = 0; i < segX; i++)
    for (int j = 0; j < segZ; j++) {
      uint32_t a = b + (uint32_t)(i * (segZ + 1) + j), d = a + (uint32_t)segZ + 1;
      uint32_t q[6] = { a, a + 1, d, d, a + 1, d + 1 };
      vec_append(&M->idx, q, 6);
    }
}

static Node *markings(void) {
  const int S = 256;
  Rng rng = rng_make(81);
  float *f = fbm_field(S, 12, 10, 4, 0.5);
  uint8_t *img = xmalloc((size_t)S * S * 4);
  for (int i = 0; i < S * S; i++) {
    double v = 205 + (double)f[i] * 45 + (rng_next(&rng) - 0.5) * 12;   // worn: faint grime variation only
    img[i * 4] = js_u8clamp(v);
    img[i * 4 + 1] = js_u8clamp(v);
    img[i * 4 + 2] = js_u8clamp(v - 6);
    img[i * 4 + 3] = 255;
  }
  free(f);
  Canvas *cv = canvas_new(S, S);
  cv_put_image_data(cv, img, 0, 0, S, S);
  free(img);
  Texture *map = tex(cv, true, 8);
  canvas_free(cv);
  Marks M = {};
  const uint32_t YEL = 0xf2c418, WHT = 0xf0ede4;
  const double cx = LANES.centerX;
  // double yellow centre line (interrupted by the crosswalks and through the intersections)
  Iv gaps[NCROSS_STREETS + 1];
  int ng = 0;
  gaps[ng++] = (Iv){ CW.zMin - 0.6, CW.zMax + 0.6 };
  for (int i = 0; i < g_nnear; i++) {
    double legs[2][2];
    crossLegs(g_near[i]->z, legs);
    gaps[ng++] = (Iv){ legs[0][0] - 0.6, legs[1][1] + 0.6 };
  }
  Iv segs[64];
  int ns = segments(-Z, Z, gaps, ng, segs);
  for (int i = 0; i < ns; i++) {
    mark_quad(&M, cx - 0.21, cx - 0.08, segs[i].a, segs[i].b, YEL, 1, 1);
    mark_quad(&M, cx + 0.08, cx + 0.21, segs[i].a, segs[i].b, YEL, 1, 1);
  }
  // parking lane edge line and bay ticks (none across the cross-street mouths)
  Iv pgaps[NCROSS_STREETS + 1];
  int npg = 0;
  pgaps[npg++] = (Iv){ CW.zMin - 6, CW.zMax + 3 };
  for (int i = 0; i < NCROSS_STREETS; i++) {
    const CrossStreet *c = &CROSS_STREETS[i];
    double legs[2][2];
    crossLegs(c->z, legs);
    pgaps[npg++] = c->far ? (Iv){ c->z - OPEN_W - 2, c->z + OPEN_W + 2 } : (Iv){ legs[0][0] - 6, legs[1][1] + 3 };
  }
  ns = segments(-Z, Z, pgaps, npg, segs);
  for (int i = 0; i < ns; i++) mark_quad(&M, PARKING.x1 - 0.05, PARKING.x1 + 0.05, segs[i].a, segs[i].b, WHT, 0.9, 1);
  // intersections
  const double hw = CROSS.hw, xw0 = CROSS.xw0, xw1 = CROSS.xw1;
  for (int ci = 0; ci < NCROSS_STREETS; ci++) {
    const CrossStreet *c = &CROSS_STREETS[ci];
    // cross-street double yellow and the eastbound stop line
    double stopX = c->far ? SIDEWALK_W.x0 - 1.5 : xw0 - 1.6;
    mark_quad_h(&M, CROSS_WEST, stopX - 0.6, c->z - 0.21, c->z - 0.08, YEL, 1, 40, 1);
    mark_quad_h(&M, CROSS_WEST, stopX - 0.6, c->z + 0.08, c->z + 0.21, YEL, 1, 40, 1);
    mark_quad_h(&M, stopX - 0.45, stopX, c->z + 0.2, c->z + hw - GUTTER_W, WHT, 0.9, 1, 6);
    if (c->far) continue;
    // continental crosswalk over the cross street, bars along its traffic
    for (double z = c->z - hw + GUTTER_W + 0.25; z < c->z + hw - GUTTER_W - 0.5; z += 1.2) mark_quad_h(&M, xw0 + 0.35, xw1 - 0.35, z, z + 0.6, WHT, 0.95, 4, 1);
    mark_quad_h(&M, xw0, xw0 + 0.3, c->z - hw + GUTTER_W, c->z + hw - GUTTER_W, WHT, 0.95, 1, 12);
    mark_quad_h(&M, xw1 - 0.3, xw1, c->z - hw + GUTTER_W, c->z + hw - GUTTER_W, WHT, 0.95, 1, 12);
    // crosswalks over Ocean Drive on the north and south legs
    double legs[2][2];
    crossLegs(c->z, legs);
    for (int l = 0; l < 2; l++) {
      double z0 = legs[l][0], z1 = legs[l][1];
      for (double x = PARKING.x0 + GUTTER_W + 0.2; x < LANES.x1 - GUTTER_W - 0.5; x += 1.2) mark_quad(&M, x, x + 0.6, z0 + 0.4, z1 - 0.4, WHT, 0.95, 1);
      mark_quad(&M, PARKING.x0 + GUTTER_W, LANES.x1 - GUTTER_W, z0, z0 + 0.3, WHT, 0.95, 16);
      mark_quad(&M, PARKING.x0 + GUTTER_W, LANES.x1 - GUTTER_W, z1 - 0.3, z1, WHT, 0.95, 16);
    }
    // the signalised intersection stops Ocean Drive too
    if (c->signal) {
      mark_quad(&M, cx + 0.2, LANES.x1 - GUTTER_W, legs[1][1] + 1.6, legs[1][1] + 2.0, WHT, 0.9, 6);
      mark_quad(&M, PARKING.x1, cx - 0.2, legs[0][0] - 2.0, legs[0][0] - 1.6, WHT, 0.9, 6);
    }
  }
  // east edge line
  mark_quad(&M, LANES.x1 - GUTTER_W - 0.2, LANES.x1 - GUTTER_W - 0.1, -Z, Z, WHT, 0.8, 1);
  // continental crosswalk: bars parallel to traffic, framed by two ladder rails
  for (double x = PARKING.x0 + GUTTER_W + 0.2; x < LANES.x1 - GUTTER_W - 0.5; x += 1.2) mark_quad(&M, x, x + 0.6, CW.zMin + 0.4, CW.zMax - 0.4, WHT, 0.95, 1);
  mark_quad(&M, PARKING.x0 + GUTTER_W, LANES.x1 - GUTTER_W, CW.zMin, CW.zMin + 0.3, WHT, 0.95, 16);
  mark_quad(&M, PARKING.x0 + GUTTER_W, LANES.x1 - GUTTER_W, CW.zMax - 0.3, CW.zMax, WHT, 0.95, 16);
  // stop bars
  mark_quad(&M, cx + 0.2, LANES.x1 - GUTTER_W, CW.zMax + 1.6, CW.zMax + 2.0, WHT, 0.9, 6);
  mark_quad(&M, PARKING.x1, cx - 0.2, CW.zMin - 2.0, CW.zMin - 1.6, WHT, 0.9, 6);
  Geometry *g = geo_new();
  int n = (int)(M.pos.len / 3);
  geo_set_attr_d(g, "position", 3, n, M.pos.data);
  geo_set_attr_d(g, "color", 3, n, M.col.data);
  geo_set_attr_d(g, "uv", 2, n, M.uv.data);
  geo_set_index(g, M.idx.data, (int)M.idx.len);
  geo_compute_vertex_normals(g);
  vec_free(&M.pos); vec_free(&M.col); vec_free(&M.uv); vec_free(&M.idx);
  MatDesc d = md_standard();
  d.name = "street paint";
  d.map = map;
  d.vertex_colors = true;
  d.roughness = 0.75;
  d.polygon_offset = true;
  d.po_factor = -1;
  d.po_units = -1;
  d.prog[MV_PLAIN] = PROG_STREET_PAINT;
  return mesh_of(g, mat_three(&d), false, true, "street paint");
}

// ---- concrete: sidewalk slab texture (3 m tile, 1.5 m slabs) and a brick-paver texture ---------
static void concrete_textures(Texture **concrete, Texture **pavers) {
  const int S = 1024;
  Rng rng = rng_make(91);
  Rng *rnd = &rng;
#define R() rng_next(rnd)
  float *f = fbm_field(256, 21, 6, 5, 0.5);
  uint8_t *img = xmalloc((size_t)S * S * 4);
  for (int y = 0; y < S; y++)
    for (int x = 0; x < S; x++) {
      int i = y * S + x;
      double n = f[(y >> 2) * 256 + (x >> 2)];
      double broom = sin(y * 1.9 + sin(x * 0.05) * 3) * 2.5;
      double v = 200 + (n - 0.5) * 24 + (R() - 0.5) * 8 + broom;
      img[i * 4] = js_u8clamp(v + 14);
      img[i * 4 + 1] = js_u8clamp(v - 6);
      img[i * 4 + 2] = js_u8clamp(v - 22);
      img[i * 4 + 3] = 255;
    }
  free(f);
  Canvas *c = canvas_new(S, S);
  cv_put_image_data(c, img, 0, 0, S, S);
  free(img);
  // per-slab tone differences and slight height offsets (a lit lip / shaded step at joints)
  for (int sx = 0; sx < 2; sx++)
    for (int sy = 0; sy < 2; sy++) {
      bool light = R() < 0.5;
      double a = 0.03 + R() * 0.05;
      if (light) cv_fill_rgba(c, 255, 245, 230, a);
      else cv_fill_rgba(c, 90, 80, 70, a);
      cv_fill_rect(c, sx * 512, sy * 512, 512, 512);
      if (R() < 0.6) { cv_fill_color(c, "rgba(255,250,240,0.35)"); cv_fill_rect(c, sx * 512 + 3, sy * 512 + 3, 506, 3); }
      if (R() < 0.6) { cv_fill_color(c, "rgba(60,50,40,0.3)"); cv_fill_rect(c, sx * 512 + 3, sy * 512 + 3, 3, 506); }
    }
  // stains and gum
  for (int k = 0; k < 8; k++) {
    double x = R() * S, y = R() * S, r = 20 + R() * 60;
    Gradient g = cv_radial_gradient(c, x, y, 0, x, y, r);
    char css[64];
    snprintf(css, sizeof css, "rgba(95,80,65,%.17g)", 0.08 + R() * 0.14);
    grad_add_stop(&g, 0, css);
    grad_add_stop(&g, 1, "rgba(95,80,65,0)");
    cv_fill_gradient(c, &g);
    cv_fill_rect(c, x - r, y - r, r * 2, r * 2);
  }
  for (int k = 0; k < 14; k++) {
    double cr = 90 + R() * 30, cg = 86 + R() * 25, cb = 80 + R() * 20;
    cv_fill_rgba(c, cr, cg, cb, 0.6);
    cv_begin_path(c);
    double x = R() * S, y = R() * S, r = 1 + R() * 1.5;
    cv_arc(c, x, y, r, 0, 6.28, false);
    cv_fill(c);
  }
  // expansion joints, chipped
  cv_stroke_color(c, "rgba(70,58,48,0.9)");
  cv_line_width(c, 4);
  const double ps[3] = { 0, 512, 1024 };
  for (int k = 0; k < 3; k++) {
    double p = ps[k];
    cv_begin_path(c); cv_move_to(c, p, 0); cv_line_to(c, p, S); cv_stroke(c);
    cv_begin_path(c); cv_move_to(c, 0, p); cv_line_to(c, S, p); cv_stroke(c);
  }
  for (int k = 0; k < 40; k++) {
    bool onX = R() < 0.5;
    double p = R() < 0.5 ? 0 : 512, q = R() * S;
    cv_fill_color(c, "rgba(110,100,88,0.6)");
    cv_begin_path(c);
    cv_arc(c, onX ? p : q, onX ? q : p, 2 + R() * 5, 0, 6.28, false);
    cv_fill(c);
  }
  *concrete = tex(c, true, 8);
  canvas_free(c);

  // brick pavers (1.2 m tile): herringbone of 20 x 10 cm bricks in warm buff/terracotta
  const int P = 512;
  Canvas *pc = canvas_new(P, P);
  cv_fill_color(pc, "#8a7462");
  cv_fill_rect(pc, 0, 0, P, P);
  const double u = P / 12.0;   // 10 cm
  static const char *const cols[6] = { "#c98d6a", "#b97a5a", "#d3a07c", "#c4876a", "#a8705a", "#d8b08c" };
  for (int i = -12; i < 24; i++)
    for (int j = -12; j < 24; j++) {
      double x = (i + j) * u, y = (j - i) * u;   // herringbone: pairs of L-shaped bricks on a diagonal lattice
      const double bricks[2][4] = { { x, y, 2 * u, u }, { x + u, y + u, u, 2 * u } };
      for (int b = 0; b < 2; b++) {
        double bx = bricks[b][0], by = bricks[b][1], w = bricks[b][2], h = bricks[b][3];
        for (int ox = -P; ox <= P; ox += P)
          for (int oy = -P; oy <= P; oy += P) {
            cv_fill_color(pc, cols[(int)floor(R() * 6)]);
            cv_fill_rect(pc, bx + ox + 1.5, by + oy + 1.5, w - 3, h - 3);
            cv_fill_rgba(pc, 0, 0, 0, R() * 0.12);
            cv_fill_rect(pc, bx + ox + 1.5, by + oy + 1.5, w - 3, h - 3);
          }
      }
    }
  *pavers = tex(pc, true, 8);
  canvas_free(pc);
#undef R
}

// Box from explicit bounds with world-space planar UVs.
static Geometry *slab(double x0, double x1, double y0, double y1, double z0, double z1, double tile) {
  Geometry *g = geo_box1(x1 - x0, y1 - y0, z1 - z0);
  geo_translate(g, (x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2);
  float *pos = geo_data(g, "position"), *uv = geo_data(g, "uv"), *nrm = geo_data(g, "normal");
  for (int i = 0; i < g->count; i++) {
    bool ax = fabs(nrm[i * 3]) > 0.5, az = fabs(nrm[i * 3 + 2]) > 0.5;
    uv[i * 2] = (float)((ax ? pos[i * 3 + 2] : pos[i * 3]) / tile);
    uv[i * 2 + 1] = (float)((ax || az ? pos[i * 3 + 1] : pos[i * 3 + 2]) / tile);
  }
  return g;
}
static Geometry *quad_geo(const double p[12], const double uv[8], const uint32_t idx[6]) {
  Geometry *g = geo_new();
  geo_set_attr_d(g, "position", 3, 4, p);
  geo_set_attr_d(g, "uv", 2, 4, uv);
  geo_set_index(g, idx, 6);
  geo_compute_vertex_normals(g);
  return g;
}
// sloped quad (ramp top) from x0 (y0) to x1 (y1) across z0..z1
static Geometry *ramp_top(double x0, double y0, double x1, double y1, double z0, double z1, double tile) {
  const double p[12] = { x0, y0, z0, x1, y1, z0, x0, y0, z1, x1, y1, z1 };
  const double uv[8] = { x0 / tile, z0 / tile, x1 / tile, z0 / tile, x0 / tile, z1 / tile, x1 / tile, z1 / tile };
  static const uint32_t A[6] = { 0, 2, 1, 1, 2, 3 }, B[6] = { 0, 1, 2, 1, 3, 2 };
  return quad_geo(p, uv, x1 > x0 ? A : B);
}
// sloped quad from z0 (y0) to z1 (y1) across x0..x1
static Geometry *ramp_top_z(double z0, double y0, double z1, double y1, double x0, double x1, double tile) {
  const double p[12] = { x0, y0, z0, x1, y0, z0, x0, y1, z1, x1, y1, z1 };
  const double uv[8] = { x0 / tile, z0 / tile, x1 / tile, z0 / tile, x0 / tile, z1 / tile, x1 / tile, z1 / tile };
  static const uint32_t A[6] = { 0, 2, 1, 1, 2, 3 }, B[6] = { 0, 1, 2, 1, 3, 2 };
  return quad_geo(p, uv, z1 > z0 ? A : B);
}
// world-space planar UVs by face orientation (as slab())
static Geometry *planar_uv(Geometry *g, double tile) {
  float *pos = geo_data(g, "position"), *uv = geo_data(g, "uv"), *nrm = geo_data(g, "normal");
  for (int i = 0; i < g->count; i++) {
    bool top = fabs(nrm[i * 3 + 1]) > 0.5;
    uv[i * 2] = (float)(top ? pos[i * 3] / tile : ((double)pos[i * 3] + pos[i * 3 + 2]) / tile);
    uv[i * 2 + 1] = (float)(top ? pos[i * 3 + 2] / tile : pos[i * 3 + 1] / tile);
  }
  return g;
}

typedef Vec(Geometry *) GeoList;
static void gl_push(GeoList *l, Geometry *g) { vec_push(l, g); }
static Geometry *gl_merge(GeoList *l) {
  Geometry *m = geo_merge_free(l->data, (int)l->len);
  vec_free(l);
  return m;
}

static void sidewalks(Node *out[2], Texture **concrete_out) {
  Texture *concrete, *pavers;
  concrete_textures(&concrete, &pavers);
  const double H = CURB_HEIGHT;
  const double wx = SIDEWALK_W.x1 - CURB_W, ex = SIDEWALK_E.x0 + CURB_W;
  Iv excl[64];
  int nex = 0;
  for (int i = 0; i < g_nramps; i++) excl[nex++] = (Iv){ g_ramps[i].z0, g_ramps[i].z1 };
  int nramp_excl = nex;
  for (int i = 0; i < NCROSS_STREETS; i++) excl[nex++] = g_openings[i];
  GeoList parts = {};
  Iv segs[64];
  // hotel side (continues under / behind the buildings), cut by the cross streets
  int ns = segments(-Z, Z, excl, nex, segs);
  for (int i = 0; i < ns; i++) gl_push(&parts, slab(-400, wx, -0.3, H, segs[i].a, segs[i].b, 3));
  for (int i = 0; i < g_nramps; i++) {
    const Ramp *r = &g_ramps[i];
    gl_push(&parts, slab(-400, wx - RAMP_RUN, -0.3, H, r->z0, r->z1, 3));
    gl_push(&parts, ramp_top(wx - RAMP_RUN, H, SIDEWALK_W.x1, roadHeight(SIDEWALK_W.x1) + 0.005, r->z0, r->z1, 3));
  }
  // park side
  ns = segments(-Z, Z, excl, nramp_excl, segs);
  for (int i = 0; i < ns; i++) gl_push(&parts, slab(ex, SIDEWALK_E.x1, -0.3, H, segs[i].a, segs[i].b, 3));
  for (int i = 0; i < g_nramps; i++) {
    const Ramp *r = &g_ramps[i];
    gl_push(&parts, slab(ex + RAMP_RUN, SIDEWALK_E.x1, -0.3, H, r->z0, r->z1, 3));
    gl_push(&parts, ramp_top(SIDEWALK_E.x0, roadHeight(SIDEWALK_E.x0) + 0.005, ex + RAMP_RUN, H, r->z0, r->z1, 3));
  }
  // cross-street corners: sidewalk strips along the cross street, rounded curb returns, and
  // curb ramps down to the crosswalk that runs along the hotel sidewalk
  {
    const double hw = CROSS.hw, R = CROSS.R, xw0 = CROSS.xw0, xw1 = CROSS.xw1, rampRun = CROSS.rampRun;
    const double cxr = SIDEWALK_W.x1 - R;
    for (int ci = 0; ci < NCROSS_STREETS; ci++) {
      const CrossStreet *c = &CROSS_STREETS[ci];
      for (int si = 0; si < 2; si++) {
        double s = si ? 1 : -1;
        double zIn = c->z + s * (hw + CURB_W), zOut = c->z + s * OPEN_W;   // curb back .. end of the opening
        double za = fmin(zIn, zOut), zb = fmax(zIn, zOut);
        if (c->far) gl_push(&parts, slab(-400, cxr, -0.3, H, za, zb, 3));
        else {
          gl_push(&parts, slab(-400, xw0, -0.3, H, za, zb, 3));
          gl_push(&parts, slab(xw1, cxr, -0.3, H, za, zb, 3));
        }
        if (!c->far) {
          double zr = c->z + s * (hw + rampRun), zo = c->z + s * hw;
          gl_push(&parts, slab(xw0, xw1, -0.3, H, fmin(zr, zOut), fmax(zr, zOut), 3));
          gl_push(&parts, ramp_top_z(zr, H, zo, crossRoadHeight(xw0, hw) + 0.005, xw0, xw1, 3));
        }
        Geometry *sector = geo_cylinder(R - CURB_W, R - CURB_W, H + 0.3, 12, 1, false, s < 0 ? 0 : PI_D / 2, PI_D / 2);
        geo_translate(sector, cxr, (H - 0.3) / 2, zOut);
        gl_push(&parts, planar_uv(sector, 3));
      }
    }
  }
  for (size_t i = 0; i < parts.len; i++)
    if (parts.data[i]->index) {
      Geometry *n = geo_to_non_indexed(parts.data[i]);
      geo_free(parts.data[i]);
      parts.data[i] = n;
    }
  MatDesc d = md_standard();
  d.name = "street sidewalk";
  d.map = concrete;
  d.roughness = 0.88;
  d.prog[MV_PLAIN] = PROG_STREET_SIDEWALK;
  Material *mat = mat_three(&d);
  mat_set_texture(mat, "odPavers", pavers);
  out[0] = mesh_of(gl_merge(&parts), mat, false, true, "street sidewalks");

  // yellow tactile warning panels (truncated domes) at the foot of each ramp
  const int T = 256;
  Canvas *tc = canvas_new(T, T);
  cv_fill_color(tc, "#d9b23c");
  cv_fill_rect(tc, 0, 0, T, T);
  for (int i = 0; i < 8; i++)
    for (int j = 0; j < 8; j++) {
      double x = (i + 0.5) * 32, y = (j + 0.5) * 32;
      Gradient g = cv_radial_gradient(tc, x - 3, y - 3, 1, x, y, 11);
      grad_add_stop(&g, 0, "#f6da78");
      grad_add_stop(&g, 0.7, "#d4ac36");
      grad_add_stop(&g, 1, "#8a6a1c");
      cv_fill_gradient(tc, &g);
      cv_begin_path(tc);
      cv_arc(tc, x, y, 11, 0, 6.28, false);
      cv_fill(tc);
    }
  Texture *tMap = tex(tc, true, 8);
  canvas_free(tc);
  const double hwx = SIDEWALK_W.x1, e0 = SIDEWALK_E.x0, rise = 0.61 * H / (RAMP_RUN + CURB_W);
  GeoList tiles = {};
  for (int i = 0; i < g_nramps; i++) {
    const Ramp *r = &g_ramps[i];
    gl_push(&tiles, ramp_top(hwx - 0.61, roadHeight(hwx) + 0.012 + rise, hwx, roadHeight(hwx) + 0.012, r->z0 + 0.2, r->z1 - 0.2, 0.6));
    gl_push(&tiles, ramp_top(e0, roadHeight(e0) + 0.012, e0 + 0.61, roadHeight(e0) + 0.012 + rise, r->z0 + 0.2, r->z1 - 0.2, 0.6));
  }
  const double riseZ = 0.61 * H / CROSS.rampRun;
  for (int i = 0; i < g_nnear; i++)
    for (int si = 0; si < 2; si++) {
      double s = si ? 1 : -1;
      double zo = g_near[i]->z + s * CROSS.hw, y0 = crossRoadHeight(CROSS.xw0, CROSS.hw) + 0.012;
      gl_push(&tiles, ramp_top_z(zo + s * 0.61, y0 + riseZ, zo, y0, CROSS.xw0 + 0.2, CROSS.xw1 - 0.2, 0.6));
    }
  MatDesc td = md_standard();
  td.name = "street tactile";
  td.map = tMap;
  td.roughness = 0.7;
  td.polygon_offset = true;
  td.po_factor = -2;
  td.po_units = -2;
  td.prog[MV_PLAIN] = PROG_STD_MAP;
  out[1] = mesh_of(gl_merge(&tiles), mat_three(&td), false, true, "street tactile");
  *concrete_out = concrete;
}

// ---- curbs with rounded top edges + gutter pans, faded painted segments, split at the ramps -----
typedef struct CurbBuf { DVec pos, col, uv; U32Vec idx; } CurbBuf;
typedef struct ProfPt { double x, y; bool g; } ProfPt;
// profile (s = +1: hotel curb whose face looks +x; -1 park curb), [dx from face, y]
static void prof(double s, double face, ProfPt P[7]) {
  double gx = face + s * GUTTER_W;
  P[0] = (ProfPt){ gx, roadHeight(gx) + 0.004, true };
  P[1] = (ProfPt){ face + s * 0.02, roadHeight(face) + 0.004, true };
  P[2] = (ProfPt){ face, 0.0, false };
  P[3] = (ProfPt){ face - s * 0.008, CURB_HEIGHT - 0.03, false };
  P[4] = (ProfPt){ face - s * 0.02, CURB_HEIGHT - 0.008, false };
  P[5] = (ProfPt){ face - s * 0.04, CURB_HEIGHT, false };
  P[6] = (ProfPt){ face - s * CURB_W, CURB_HEIGHT, false };
}
static void cb_vertex(CurbBuf *B, double x, double y, double z, Color c, double u, double v) {
  double p[3] = { x, y, z }, cc[3] = { c.r, c.g, c.b }, uu[2] = { u, v };
  vec_append(&B->pos, p, 3); vec_append(&B->col, cc, 3); vec_append(&B->uv, uu, 2);
}
static void curb_run(CurbBuf *B, double s, double face, double z0, double z1, const Color *paint, bool keep, Rng *r) {
  Color CON = color_hex(0xd6cbbb), GUT = color_hex(0xc9beae);
  ProfPt P[7];
  prof(s, face, P);
  uint32_t b = (uint32_t)(B->pos.len / 3);
  for (int i = 0; i < 7; i++) {
    Color color = P[i].g ? GUT : CON;
    if (!P[i].g && paint && i > 1) color = color_lerp(color, *paint, rng_next(r) < 0.12 ? 0.35 : 0.92);   // chipped here and there
    if (!keep) continue;
    const double zs[2] = { z0, z1 };
    for (int k = 0; k < 2; k++) cb_vertex(B, P[i].x, P[i].y, zs[k], color, zs[k] / 3, i * 0.05);
    if (i < 6) {
      uint32_t a = b + (uint32_t)(i * 2);
      uint32_t q1[6] = { a, a + 1, a + 2, a + 1, a + 3, a + 2 }, q2[6] = { a, a + 2, a + 1, a + 1, a + 2, a + 3 };
      vec_append(&B->idx, s < 0 ? q1 : q2, 6);
    }
  }
}
static void curb_pan(CurbBuf *B, double s, double face, double z0, double z1) {
  Color GUT = color_hex(0xc9beae);
  ProfPt P[7];
  prof(s, face, P);
  uint32_t b = (uint32_t)(B->pos.len / 3);
  for (int i = 0; i < 2; i++) {
    cb_vertex(B, P[i].x, P[i].y, z0, GUT, z0 / 3, 0);
    cb_vertex(B, P[i].x, P[i].y, z1, GUT, z1 / 3, 0);
  }
  uint32_t q1[6] = { b, b + 1, b + 2, b + 1, b + 3, b + 2 }, q2[6] = { b, b + 2, b + 1, b + 1, b + 2, b + 3 };
  vec_append(&B->idx, s < 0 ? q1 : q2, 6);
}
typedef struct PathPt { double x, z, nx, nz; } PathPt;
// pts: n pointing to the road; profile [offset along n, y, kind]
static void curb_path(CurbBuf *B, const PathPt *pts, int np, const double *cutX) {
  static const double PR[7][2] = { { GUTTER_W, 0.004 }, { 0.02, 0.004 }, { 0, 0 }, { -0.008, CURB_HEIGHT - 0.03 },
                                   { -0.02, CURB_HEIGHT - 0.008 }, { -0.04, CURB_HEIGHT }, { -CURB_W, CURB_HEIGHT } };
  Color CON = color_hex(0xd6cbbb), GUT = color_hex(0xc9beae);
  for (int k = 0; k < np - 1; k++) {
    const PathPt *p = &pts[k], *q = &pts[k + 1];
    double mx = (p->x + q->x) / 2;
    bool inCut = cutX && mx > cutX[0] && mx < cutX[1];
    for (int i = 0; i < 6; i++) {
      if (inCut && i > 0) break;
#define VV(pp, j) v3((pp)->x + (pp)->nx * PR[j][0], PR[j][1], (pp)->z + (pp)->nz * PR[j][0])
      V3 quad[4] = { VV(p, i), VV(p, i + 1), VV(q, i + 1), VV(q, i) };
#undef VV
      Color c = i < 2 ? GUT : CON;
      double e1[3] = { quad[1].x - quad[0].x, quad[1].y - quad[0].y, quad[1].z - quad[0].z };
      double e2[3] = { quad[2].x - quad[0].x, quad[2].y - quad[0].y, quad[2].z - quad[0].z };
      double n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
      bool flip = n[0] * p->nx + n[1] + n[2] * p->nz < 0;
      static const int O1[6] = { 0, 2, 1, 0, 3, 2 }, O2[6] = { 0, 1, 2, 0, 2, 3 };
      const int *order = flip ? O1 : O2;
      for (int o = 0; o < 6; o++) {
        V3 v = quad[order[o]];
        cb_vertex(B, v.x, v.y, v.z, c, (v.x + v.z) / 3, i * 0.05);
        vec_push(&B->idx, (uint32_t)(B->pos.len / 3 - 1));
      }
    }
  }
}

static Node *curbs(Texture *concrete) {
  CurbBuf B = {};
  Color YEL = color_hex(0xf0c21c), RED = color_hex(0xc23a2c);
  Rng rnd = rng_make(101);
  Rng rnd2 = rng_make(102);   // pieces cut short by the intersections (keeps rnd's sequence)
  const double sides[2][2] = { { 1, SIDEWALK_W.x1 }, { -1, SIDEWALK_E.x0 } };
  for (int si = 0; si < 2; si++) {
    double s = sides[si][0], face = sides[si][1];
    // the hotel curb opens at every cross street; both curbs dip at the leg ramps
    Iv cuts[64];
    int nc = 0;
    for (int i = 0; i < g_nlegs; i++) cuts[nc++] = (Iv){ g_leg_ramps[i].z0, g_leg_ramps[i].z1 };
    if (s > 0) for (int i = 0; i < NCROSS_STREETS; i++) cuts[nc++] = g_openings[i];
    // split into 3 m pieces near the block (random faded paint), long runs beyond
    Iv segs[64];
    int ns = segments(-Z, -300, cuts, nc, segs);
    for (int i = 0; i < ns; i++) curb_run(&B, s, face, segs[i].a, segs[i].b, nullptr, true, &rnd);
    ns = segments(300, Z, cuts, nc, segs);
    for (int i = 0; i < ns; i++) curb_run(&B, s, face, segs[i].a, segs[i].b, nullptr, true, &rnd);
    for (double z = -300; z < 300; z += 3) {
      double z1 = z + 3;
      if (z1 > RAMP.z0 && z < RAMP.z1) {
        if (z < RAMP.z0) curb_run(&B, s, face, z, RAMP.z0, nullptr, true, &rnd);
        if (z1 > RAMP.z1) curb_run(&B, s, face, RAMP.z1, z1, nullptr, true, &rnd);
        continue;
      }
      bool near = fabs(z - CROSSWALK_Z) < 12;
      const Color *paint = near ? &RED : &YEL;
      Iv pieces[16];
      int np = segments(z, z1, cuts, nc, pieces);
      if (np == 1 && pieces[0].a == z && pieces[0].b == z1) { curb_run(&B, s, face, z, z1, paint, true, &rnd); continue; }
      curb_run(&B, s, face, z, z1, paint, false, &rnd);
      for (int i = 0; i < np; i++)
        if (pieces[i].b - pieces[i].a > 0.05) curb_run(&B, s, face, pieces[i].a, pieces[i].b, paint, true, &rnd2);
    }
    // gutter pan through the ramp openings
    curb_pan(&B, s, face, RAMP.z0, RAMP.z1);
    for (int i = 0; i < g_nlegs; i++) curb_pan(&B, s, face, g_leg_ramps[i].z0, g_leg_ramps[i].z1);
  }
  // cross streets: curb returns round the corners, then straight curbs running west (cut at the
  // crosswalk ramps, where only the gutter pan continues)
  const double hw = CROSS.hw, R = CROSS.R, xw0 = CROSS.xw0, xw1 = CROSS.xw1;
  const double cxr = SIDEWALK_W.x1 - R;
  for (int ci = 0; ci < NCROSS_STREETS; ci++) {
    const CrossStreet *c = &CROSS_STREETS[ci];
    for (int si = 0; si < 2; si++) {
      double s = si ? 1 : -1;
      double cz = c->z + s * OPEN_W;
      PathPt pts[24];
      int np = 0;
      for (int i = 0; i <= 10; i++) {
        double a = ((double)i / 10) * PI_D / 2, nx = cos(a), nz = -s * sin(a);
        pts[np++] = (PathPt){ cxr + R * nx, cz + R * nz, nx, nz };
      }
      double xsW[8];
      int nw = 0;
      xsW[nw++] = cxr - 0.01;
      if (!c->far) { xsW[nw++] = xw1; xsW[nw++] = xw0; }
      xsW[nw++] = -40; xsW[nw++] = -60; xsW[nw++] = -120; xsW[nw++] = CROSS_WEST;
      for (int i = 0; i < nw; i++) pts[np++] = (PathPt){ xsW[i], c->z + s * hw, 0, -s };
      const double cut[2] = { xw0, xw1 };
      curb_path(&B, pts, np, c->far ? nullptr : cut);
    }
  }
  Geometry *g = indexed_geometry(&B.pos, &B.uv, &B.col, &B.idx);
  MatDesc d = md_standard();
  d.name = "street curbs";
  d.map = concrete;
  d.vertex_colors = true;
  d.roughness = 0.85;
  d.prog[MV_PLAIN] = PROG_STD_MAP_VCOL;
  return mesh_of(g, mat_three(&d), true, true, "street curbs");
}

// ---- street furniture, merged into vertex-coloured meshes ------------------------------------------
static Geometry *colored(Geometry *g, uint32_t hex) {
  if (g->index) {
    Geometry *n = geo_to_non_indexed(g);
    geo_free(g);
    g = n;
  }
  geo_delete_attr(g, "uv");
  Color c = color_hex(hex);
  float *a = geo_set_attr(g, "color", 3, g->count);
  for (int i = 0; i < g->count; i++) { a[i * 3] = (float)c.r; a[i * 3 + 1] = (float)c.g; a[i * 3 + 2] = (float)c.b; }
  return g;
}
static Geometry *fcyl(double r0, double r1, double h, int seg, double y, uint32_t hex) {
  return colored(geo_translate(geo_cyl(r1, r0, h, seg), 0, y + h / 2, 0), hex);
}
static Geometry *fbox(double w, double h, double d, double x, double y, double z, uint32_t hex) {
  return colored(geo_translate(geo_box1(w, h, d), x, y + h / 2, z), hex);
}
#define MERGE(...) merge_list((Geometry *[]){ __VA_ARGS__ }, (int)(sizeof((Geometry *[]){ __VA_ARGS__ }) / sizeof(Geometry *)))
static Geometry *merge_list(Geometry **l, int n) { return geo_merge_free(l, n); }

static Geometry *lamp_geo(void) {
  const uint32_t M = 0x28332f, G = 0xf1ece0;
  return MERGE(fbox(0.46, 0.14, 0.46, 0, 0, 0, M),
               fcyl(0.2, 0.16, 0.55, 12, 0.14, M),            // fluted base
               fcyl(0.19, 0.19, 0.04, 16, 0.69, M),
               fcyl(0.075, 0.055, 3.6, 10, 0.72, M),          // pole
               fcyl(0.085, 0.085, 0.05, 12, 2.2, M),
               fcyl(0.09, 0.09, 0.06, 12, 4.25, M),
               fcyl(0.1, 0.2, 0.12, 12, 4.3, M),              // lantern seat
               fcyl(0.2, 0.23, 0.5, 16, 4.42, G),             // frosted glass (off)
               fcyl(0.25, 0.25, 0.04, 16, 4.92, M),
               fcyl(0.26, 0.06, 0.24, 16, 4.96, M),           // stepped Deco cap
               colored(geo_translate(geo_sphere3(0.06, 8, 6), 0, 5.24, 0), M));
}
static Geometry *pay_station_geo(void) {
  return MERGE(fbox(0.32, 0.9, 0.24, 0, 0, 0, 0x3b4046),
               fbox(0.4, 0.55, 0.3, 0, 0.9, 0, 0x4a5058),
               fbox(0.26, 0.2, 0.02, 0, 1.18, 0.155, 0x6f8fa8),   // screen
               fbox(0.28, 0.08, 0.02, 0, 1.0, 0.155, 0xc9c4b0),   // keypad
               fbox(0.44, 0.05, 0.34, 0, 1.45, 0, 0x2f3338));
}
static Geometry *trash_geo(void) {
  return MERGE(fcyl(0.28, 0.3, 0.85, 20, 0, 0x2f4a3e),
               fcyl(0.31, 0.31, 0.05, 20, 0.85, 0x3a3f3c),
               colored(geo_translate(geo_scale(geo_sphere(0.3, 16, 6, 0, TAU, 0, PI_D / 2), 1, 0.35, 1), 0, 0.9, 0), 0x3a3f3c));
}
static Geometry *hydrant_geo(void) {
  return MERGE(fcyl(0.16, 0.16, 0.06, 12, 0, 0xc9a02a),
               fcyl(0.12, 0.11, 0.5, 12, 0.06, 0xd8ac2c),
               colored(geo_translate(geo_sphere(0.12, 12, 6, 0, TAU, 0, PI_D / 2), 0, 0.56, 0), 0xc23a2e),
               colored(geo_translate(geo_rotate_z(geo_cyl(0.05, 0.05, 0.36, 8), PI_D / 2), 0, 0.38, 0), 0xd8ac2c),
               colored(geo_translate(geo_rotate_x(geo_cyl(0.065, 0.065, 0.12, 8), PI_D / 2), 0, 0.36, 0.12), 0xd8ac2c));
}
static Geometry *bike_rack_geo(void) {
  Geometry *parts[3];
  for (int i = 0; i < 3; i++) {
    const V3 pts[5] = { { 0, 0, -0.3 }, { 0, 0.75, -0.28 }, { 0, 0.85, 0 }, { 0, 0.75, 0.28 }, { 0, 0, 0.3 } };
    CatmullRom3 path = curve_catmull(pts, 5, false, CURVE_CENTRIPETAL, 0.5);
    parts[i] = colored(geo_translate(geo_tube(&path, 16, 0.025, 6, false), (i - 1) * 0.7, 0, 0), 0xa9aca8);
    curve_free(&path);
  }
  return geo_merge_free(parts, 3);
}
static Geometry *bench_geo(void) {
  // faces +x: slats seat and back, cast-iron end frames
  const uint32_t W = 0x7c6a58, I = 0x3a3d3b;
  GeoList parts = {};
  for (int i = 0; i < 4; i++) gl_push(&parts, fbox(0.09, 0.035, 1.8, -0.12 + i * 0.11, 0.44, 0, W));
  for (int i = 0; i < 3; i++) gl_push(&parts, colored(geo_translate(geo_rotate_z(geo_box1(0.03, 0.09, 1.8), 0.2), -0.28 - i * 0.02, 0.6 + i * 0.12, 0), W));
  const double zs[2] = { -0.8, 0.8 };
  for (int k = 0; k < 2; k++) {
    double z = zs[k];
    gl_push(&parts, fbox(0.5, 0.05, 0.05, 0, 0.4, z, I));
    gl_push(&parts, fbox(0.05, 0.44, 0.05, 0.18, 0, z, I));
    gl_push(&parts, fbox(0.05, 0.44, 0.05, -0.18, 0, z, I));
    gl_push(&parts, colored(geo_translate(geo_rotate_z(geo_box1(0.04, 0.5, 0.05), 0.2), -0.3, 0.65, z), I));
    gl_push(&parts, fbox(0.06, 0.05, 0.05, 0.22, 0.6, z, I));
  }
  return gl_merge(&parts);
}
static Geometry *drain_geo(double s) {
  // gutter grate + dark curb-opening throat (s: +1 hotel curb, -1 park curb)
  GeoList parts = {};
  gl_push(&parts, fbox(0.5, 0.02, 0.9, s * 0.25, -0.01, 0, 0x2a2a28));
  for (int i = 0; i < 6; i++) gl_push(&parts, fbox(0.46, 0.012, 0.035, s * 0.25, 0.006, -0.38 + i * 0.152, 0x55544f));
  gl_push(&parts, fbox(0.012, 0.1, 1.1, s * 0.004, 0.01, 0, 0x1a1a19));
  return gl_merge(&parts);
}
static Geometry *manhole_geo(void) {
  GeoList parts = {};
  gl_push(&parts, fcyl(0.34, 0.34, 0.012, 24, 0, 0x4a4843));
  for (double r = 0.08; r < 0.32; r += 0.07)
    gl_push(&parts, colored(geo_translate(geo_rotate_x(geo_torus(r, 0.008, 3, 24, TAU, 0, TAU), PI_D / 2), 0, 0.013, 0), 0x5c5a54));
  return gl_merge(&parts);
}
static Geometry *news_box_geo(uint32_t hex) {
  return MERGE(fbox(0.46, 0.35, 0.4, 0, 0, 0, 0x2c2e30),
               fbox(0.5, 0.62, 0.44, 0, 0.35, 0, hex),
               fbox(0.36, 0.26, 0.02, 0, 0.62, 0.225, 0x9aa6ae),   // window
               fbox(0.52, 0.04, 0.46, 0, 0.97, 0, 0x2c2e30));
}
static Geometry *valet_geo(void) {
  return MERGE(fbox(0.55, 1.0, 0.42, 0, 0, 0, 0x4a3524),
               colored(geo_translate(geo_rotate_x(geo_box1(0.62, 0.05, 0.5), -0.25), 0, 1.05, 0), 0x2c2016),
               fbox(0.57, 0.08, 0.44, 0, 0.0, 0, 0x1e1a16));
}
static Geometry *sign_post_geo(double h) {
  return MERGE(fcyl(0.035, 0.035, h, 8, 0, 0x8d908c), fcyl(0.05, 0.05, 0.05, 8, h, 0x8d908c));
}
static Geometry *signal_pole_geo(double x0, double armTo) {
  const uint32_t P = 0x3a3f3c;
  double len = fabs(armTo - x0), dir = js_sign(armTo - x0);
  return MERGE(fcyl(0.2, 0.2, 0.3, 12, 0, P),
               fcyl(0.13, 0.1, 5.6, 12, 0.3, P),
               colored(geo_translate(geo_rotate_z(geo_cyl(0.06, 0.08, len, 8), PI_D / 2), dir * len / 2, 5.35, 0), P),
               colored(geo_translate(geo_rotate_z(geo_cyl(0.02, 0.02, js_hypot2(len * 0.6, 0.8), 6), -dir * atan2(len * 0.6, 0.8)), dir * len * 0.3, 5.35 - 0.4, 0), P));
}
static Geometry *signal_head_geo(void) {
  // three-lens head facing +z (lenses unlit here; the lit one is a separate emissive disc)
  const uint32_t Hs = 0x2b2d27;
  static const uint32_t LENS[3] = { 0x3a1512, 0x3a2c10, 0x0f2a1a };
  GeoList parts = {};
  gl_push(&parts, fbox(0.36, 1.05, 0.24, 0, -0.52, 0, Hs));
  gl_push(&parts, fbox(0.52, 1.2, 0.02, 0, -0.6, -0.13, 0x1c1d1a));
  for (int i = 0; i < 3; i++) {
    double y = 0.35 - i * 0.35;
    gl_push(&parts, colored(geo_translate(geo_rotate_x(geo_cyl(0.1, 0.1, 0.015, 14), PI_D / 2), 0, y, 0.125), LENS[i]));
    gl_push(&parts, fbox(0.26, 0.02, 0.16, 0, y + 0.12, 0.2, Hs));
  }
  gl_push(&parts, fbox(0.06, 0.2, 0.06, 0, 0.5, 0, 0x3a3f3c));
  return gl_merge(&parts);
}

// Walk colliders for street furniture
static Vec(StreetCollider) g_colliders;
const StreetCollider *street_colliders(int *n) {
  *n = (int)g_colliders.len;
  return g_colliders.data;
}

// col: a radius (m) for a post-like circle, COL_BOX for the footprint of the piece, COL_NONE
// (pieces that would land in a cross street, its corner ramps or a leg crosswalk ramp are left
// out; intersection furniture is placed with g_force)
static constexpr double COL_NONE = 0;
static constexpr double COL_BOX = -1;
static bool g_force;
static bool blocks_crossing(double x, double z) {
  return in_crossing(x, z) || (in_leg(z) && (fabs(x - SIDEWALK_W.x1) < 1.6 || fabs(x - SIDEWALK_E.x0) < 1.6));
}
static void place(GeoList *list, const Geometry *g, double x, double y, double z, double rotY, double col) {
  if (!g_force && blocks_crossing(x, z)) return;
  Geometry *c = geo_translate(geo_rotate_y(geo_clone(g), rotY), x, y, z);
  vec_push(list, c);
  if (col > 0) vec_push(&g_colliders, ((StreetCollider){ .x = x, .z = z, .r = col }));
  else if (col == COL_BOX) {
    Box3 b = geo_compute_bbox(c);
    vec_push(&g_colliders, ((StreetCollider){ .box = true, .min = b.min, .max = b.max }));
  }
}

static bool palm_near(double x, double z, double d) {
  int nt;
  const PalmTree *t = palm_trees(&nt);
  for (int i = 0; i < nt; i++) if (js_hypot2(t[i].x - x, t[i].z - z) < d) return true;
  return false;
}

typedef struct Furn { Geometry *lamp, *pay, *trash, *hyd, *rack, *bench, *drainW, *drainE, *manhole, *newsB, *newsR, *valet; } Furn;
typedef struct Blade { double x, z; const char *name; } Blade;
typedef struct StopSign { double x, z; } StopSign;
typedef struct Lit { double x, y, z, rot; int lamp; } Lit;

// ---- sign plates -----------------------------------------------------------------------------------
static Geometry *plate(double w, double h, double u0, double v0, double u1, double v1, const double *back) {
  const double p[24] = { -w / 2, -h / 2, 0.006, w / 2, -h / 2, 0.006, -w / 2, h / 2, 0.006, w / 2, h / 2, 0.006,
                         w / 2, -h / 2, -0.006, -w / 2, -h / 2, -0.006, w / 2, h / 2, -0.006, -w / 2, h / 2, -0.006 };
  double uv[16] = { u0, v1, u1, v1, u0, v0, u1, v0 };
  const double def[8] = { u0, v1, u1, v1, u0, v0, u1, v0 };
  memcpy(uv + 8, back ? back : def, sizeof def);
  static const uint32_t idx[12] = { 0, 1, 2, 1, 3, 2, 4, 5, 6, 5, 7, 6 };
  Geometry *g = geo_new();
  geo_set_attr_d(g, "position", 3, 8, p);
  geo_set_attr_d(g, "uv", 2, 8, uv);
  geo_set_index(g, idx, 12);
  geo_compute_vertex_normals(g);
  return g;
}
static Geometry *strip(Geometry *g) {
  if (g->index) {
    Geometry *n = geo_to_non_indexed(g);
    geo_free(g);
    g = n;
  }
  geo_delete_attr(g, "uv");
  return g;
}
static Node *two_meshes(GeoList *L, GeoList *M, Material *plates, const char *name) {
  Node *m = mesh_of(gl_merge(L), plates, true, true, name);
  for (size_t i = 0; i < M->len; i++) M->data[i] = strip(M->data[i]);
  MatDesc hd = md_standard();
  hd.name = "street sign hardware";
  hd.color = color_hex(0x8d908c);
  hd.roughness = 0.45;
  hd.metalness = 0.6;
  hd.prog[MV_PLAIN] = PROG_STD;
  Node *hw = mesh_of(gl_merge(M), mat_three(&hd), true, true, "street sign hardware");
  Node *g = node_new(NODE_GROUP, name);
  node_add(g, m);
  node_add(g, hw);
  return g;
}

static void blade_text(Canvas *c, double y, double RH, const char *text) {
  cv_fill_color(c, "#1f6b45");
  cv_fill_rect(c, 0, y, 1024, RH);
  cv_stroke_color(c, "#f2f2ea");
  cv_line_width(c, 6);
  cv_stroke_rect(c, 8, y + 8, 1008, RH - 16);
  cv_fill_color(c, "#f4f4ee");
  cv_font(c, "bold 84px Arial, Helvetica, sans-serif");
  cv_text_align(c, ALIGN_CENTER);
  cv_text_baseline(c, BASE_MIDDLE);
  cv_fill_text(c, text, 512, y + 66);
}

// Street-name blades on the corner posts and stop signs, from their own small atlas.
static Node *cross_signs(const Blade *blades, int nb, const StopSign *stops, int nst) {
  const char *names[NCROSS_STREETS];
  int nn = 0;
  for (int i = 0; i < g_nnear; i++) {
    bool seen = false;
    for (int k = 0; k < nn; k++) if (!strcmp(names[k], g_near[i]->name)) seen = true;
    if (!seen) names[nn++] = g_near[i]->name;
  }
  const int rows = nn + 1, RH = 128;
  const int Ht = RH * rows + 256;
  Canvas *c = canvas_new(1024, Ht);
  blade_text(c, 0, RH, "OCEAN DR");
  for (int i = 0; i < nn; i++) blade_text(c, RH * (i + 1), RH, names[i]);
  // stop sign octagon (256 x 256) below the blades, aluminium back beside it
  const double sy = RH * rows;
#define OCT(r, x0, y0) do { cv_begin_path(c); for (int k = 0; k < 8; k++) { double a = PI_D / 8 + (k * PI_D) / 4; cv_line_to(c, (x0) + cos(a) * (r), (y0) + sin(a) * (r)); } cv_close_path(c); } while (0)
  cv_fill_color(c, "#f2f1ea");
  OCT(126, 128, sy + 128);
  cv_fill(c);
  cv_fill_color(c, "#b3241e");
  OCT(114, 128, sy + 128);
  cv_fill(c);
  cv_fill_color(c, "#f4f2ec");
  cv_font(c, "bold 70px Arial, Helvetica, sans-serif");
  cv_text_align(c, ALIGN_CENTER);
  cv_text_baseline(c, BASE_MIDDLE);
  cv_fill_text(c, "STOP", 128, sy + 132);
  cv_fill_color(c, "#a4a7a8");
  OCT(126, 384, sy + 128);
  cv_fill(c);
#undef OCT
  Texture *map = tex(c, true, 8);
  canvas_free(c);
#define V(py) (1 - (py) / (double)Ht)   // canvas y -> texture v (flipY)
  GeoList L = {}, M = {};
  const double H = CURB_HEIGHT;
  for (int i = 0; i < nb; i++) {
    const Blade *b = &blades[i];
    double top = H + 3.2;
    int row = 0;
    for (int k = 0; k < nn; k++) if (!strcmp(names[k], b->name)) row = k + 1;
    gl_push(&L, geo_translate(geo_rotate_y(plate(0.9, 0.18, 0, V(0), 1, V(RH), nullptr), PI_D / 2), b->x, top + 0.14, b->z));
    gl_push(&L, geo_translate(plate(0.62, 0.18, 0.2, V(RH * row), 0.8, V(RH * (row + 1)), nullptr), b->x, top + 0.35, b->z));
    gl_push(&M, geo_translate(geo_cyl(0.045, 0.045, 0.06, 12), b->x, top + 0.02, b->z));
    gl_push(&M, geo_translate(geo_box1(0.035, 0.42, 0.05), b->x, top + 0.24, b->z));
    gl_push(&M, geo_translate(geo_box1(0.05, 0.42, 0.035), b->x, top + 0.24, b->z));
  }
  const double back[8] = { 0.25 + 0.0, V(sy + 256), 0.5, V(sy + 256), 0.25, V(sy), 0.5, V(sy) };
  for (int i = 0; i < nst; i++) {
    const StopSign *s = &stops[i];
    // faces west, toward eastbound cross-street traffic
    gl_push(&L, geo_translate(geo_rotate_y(geo_translate(plate(0.76, 0.76, 0, V(sy), 0.25, V(sy + 256), back), 0, 0, 0.055), -PI_D / 2), s->x, H + 2.0, s->z));
    const double bys[2] = { 0.22, -0.22 };
    for (int k = 0; k < 2; k++)
      gl_push(&M, geo_translate(geo_rotate_y(geo_translate(geo_box1(0.1, 0.035, 0.03), 0, bys[k], 0.035), -PI_D / 2), s->x, H + 2.0, s->z));
  }
#undef V
  MatDesc d = md_standard();
  d.name = "street cross signs";
  d.map = map;
  d.roughness = 0.5;
  d.metalness = 0.1;
  d.alpha_test = 0.5;
  d.prog[MV_PLAIN] = PROG_STD_MAP_ATEST;
  return two_meshes(&L, &M, mat_three(&d), "street cross signs");
}

// Sign plates (text on a canvas atlas): street-name blades on a cap bracket at the top of the
// crosswalk posts, regulation plates clamped to the front of their poles.
static Node *signs(const double (*posts)[2], int nposts, const double (*regs)[2], int nregs) {
  Canvas *c = canvas_new(1024, 512);
  blade_text(c, 0, 128, "OCEAN DR");
  blade_text(c, 128, 128, "9 ST");
  // regulation sign (white): 256 wide x 256 tall at the bottom left
  cv_text_baseline(c, BASE_ALPHABETIC);
  cv_fill_color(c, "#f2f1ea");
  cv_fill_rect(c, 0, 256, 256, 256);
  cv_stroke_color(c, "#b02a26");
  cv_line_width(c, 8);
  cv_stroke_rect(c, 10, 266, 236, 236);
  cv_fill_color(c, "#b02a26");
  cv_font(c, "bold 44px Arial, sans-serif");
  cv_text_align(c, ALIGN_CENTER);
  cv_fill_text(c, "2 HOUR", 128, 318);
  cv_fill_text(c, "PARKING", 128, 366);
  cv_fill_color(c, "#222");
  cv_font(c, "bold 30px Arial, sans-serif");
  cv_fill_text(c, "9AM - 6PM", 128, 420);
  cv_fill_text(c, "PAY AT METER", 128, 462);
  // pedestrian crossing (yellow-green diamond) at 256..512
  cv_fill_color(c, "#c8d63a");
  cv_begin_path(c);
  cv_move_to(c, 384, 262); cv_line_to(c, 506, 384); cv_line_to(c, 384, 506); cv_line_to(c, 262, 384);
  cv_close_path(c);
  cv_fill(c);
  cv_stroke_color(c, "#222");
  cv_line_width(c, 6);
  cv_stroke(c);
  cv_fill_color(c, "#222");
  cv_begin_path(c);
  cv_arc(c, 384, 330, 14, 0, 6.28, false);
  cv_fill(c);
  cv_line_width(c, 12);
  cv_begin_path(c);
  cv_move_to(c, 384, 346); cv_line_to(c, 380, 400); cv_line_to(c, 360, 450);
  cv_move_to(c, 380, 400); cv_line_to(c, 404, 448);
  cv_move_to(c, 356, 372); cv_line_to(c, 408, 380);
  cv_stroke(c);
  // valet sign (512..768 x 256..512)
  cv_fill_color(c, "#20302a");
  cv_fill_rect(c, 512, 256, 256, 256);
  cv_stroke_color(c, "#d9c28a");
  cv_line_width(c, 6);
  cv_stroke_rect(c, 522, 266, 236, 236);
  cv_fill_color(c, "#efe6cc");
  cv_font(c, "bold 58px Georgia, serif");
  cv_text_align(c, ALIGN_CENTER);
  cv_fill_text(c, "VALET", 640, 360);
  cv_font(c, "bold 34px Georgia, serif");
  cv_fill_text(c, "PARKING", 640, 420);
  // bare aluminium for the backs of plates (768..1024 x 256..512)
  cv_fill_color(c, "#a4a7a8");
  cv_fill_rect(c, 768, 256, 256, 256);
  Texture *map = tex(c, true, 8);
  canvas_free(c);
  // CanvasTexture flips Y: v0 is the top edge of the sign in the atlas, v1 the bottom. Two faces
  // 12 mm apart; a 'text' back keeps it readable, otherwise the back is plain metal.
  const double BACK_U = 0.875, BACK_V = 0.25;
  const double metal[8] = { BACK_U, BACK_V, BACK_U, BACK_V, BACK_U, BACK_V, BACK_U, BACK_V };
#define PLATE_TEXT(w, h, u0, v0, u1, v1) plate(w, h, u0, v0, u1, v1, nullptr)
#define PLATE_METAL(w, h, u0, v0, u1, v1) plate(w, h, u0, v0, u1, v1, metal)
  GeoList L = {}, M = {};
  const double H = CURB_HEIGHT, POLE_R = 0.035;
  // a plate clamped to the front of a pole: offset past the pole, two U-bracket straps
  typedef struct { double w, h, u0, v0, u1, v1; } PUV;
#define MOUNTED(P, rotY, x, y, z) do { \
    double off = POLE_R + 0.018; \
    gl_push(&L, geo_translate(geo_rotate_y(geo_translate(PLATE_METAL((P).w, (P).h, (P).u0, (P).v0, (P).u1, (P).v1), 0, 0, off), rotY), x, y, z)); \
    const double bys_[2] = { (P).h * 0.3, -(P).h * 0.3 }; \
    for (int k_ = 0; k_ < 2; k_++) { \
      double by = bys_[k_]; \
      gl_push(&M, geo_translate(geo_rotate_y(geo_translate(geo_box1(0.1, 0.035, 0.02), 0, by, off - 0.016), rotY), x, y, z)); \
      gl_push(&M, geo_translate(geo_rotate_y(geo_translate(geo_box1(0.1, 0.03, 0.014), 0, by, -POLE_R - 0.007), rotY), x, y, z)); \
      const double bxs_[2] = { -0.045, 0.045 }; \
      for (int j_ = 0; j_ < 2; j_++) \
        gl_push(&M, geo_translate(geo_rotate_y(geo_translate(geo_box1(0.012, 0.012, off + POLE_R), bxs_[j_], by, (off - POLE_R) / 2 - 0.008), rotY), x, y, z)); \
    } \
  } while (0)
  for (int i = 0; i < nposts; i++) {
    double x = posts[i][0], z = posts[i][1];
    // street-name blades stacked above the pole top in a slotted cap bracket, crossed
    double top = H + 3.2;
    gl_push(&L, geo_translate(geo_rotate_y(PLATE_TEXT(0.9, 0.18, 0, 1, 1, 0.75), PI_D / 2), x, top + 0.14, z));   // OCEAN DR, along the street
    gl_push(&L, geo_translate(PLATE_TEXT(0.6, 0.18, 0.2, 0.75, 0.8, 0.5), x, top + 0.35, z));                    // 9 ST, across it
    gl_push(&M, geo_translate(geo_cyl(0.045, 0.045, 0.06, 12), x, top + 0.02, z));
    gl_push(&M, geo_translate(geo_box1(0.035, 0.42, 0.05), x, top + 0.24, z));
    gl_push(&M, geo_translate(geo_box1(0.05, 0.42, 0.035), x, top + 0.24, z));
    gl_push(&M, geo_translate(geo_box1(0.04, 0.02, 0.04), x, top + 0.46, z));
    PUV pu = { 0.6, 0.6, 0.25, 0.5, 0.5, 0 };
    MOUNTED(pu, x < LANES.centerX ? PI_D / 2 : -PI_D / 2, x, H + 2.2, z);
  }
  for (int i = 0; i < nregs; i++) {
    PUV pu = { 0.45, 0.45, 0, 0.5, 0.25, 0 };
    MOUNTED(pu, PI_D / 2, regs[i][0], H + 2.1, regs[i][1]);
  }
  // valet A-frame sign on the sidewalk (text on the outer faces; the JS passes no `back`, so
  // the inner faces are plain metal)
  for (int si = 0; si < 2; si++) {
    double s = si ? 1 : -1;
    gl_push(&L, geo_translate(geo_rotate_y(geo_rotate_x(PLATE_METAL(0.55, 0.8, 0.5, 0.5, 0.75, 0), s * 0.18), PI_D / 2 * (s > 0 ? 1 : -1)), -25.4 + s * 0.07, H + 0.4, 24.3));
  }
#undef MOUNTED
#undef PLATE_TEXT
#undef PLATE_METAL
  MatDesc d = md_standard();
  d.name = "street signs";
  d.map = map;
  d.roughness = 0.5;
  d.metalness = 0.1;
  d.prog[MV_PLAIN] = PROG_STD_MAP;
  return two_meshes(&L, &M, mat_three(&d), "street signs");
}

// The extended blocks: lamps, bins, pay stations, drains, benches and hydrants beyond the original
// ranges, plus the intersection furniture (street-name blades, stop signs, one signal). One merged
// mesh per block so far blocks can be culled.
static int district_furniture(Material *mat, const Furn *G, Node **out) {
  Rng rng = rng_make(112);
  Rng *rnd = &rng;
#define R() rng_next(rnd)
  const double H = CURB_HEIGHT, hx = SIDEWALK_W.x1 - 0.55, px = SIDEWALK_E.x0 + 0.55;
  GeoList L = {};
  const double lz[4] = { -322, -354, 318, 350 };
  for (int k = 0; k < 4; k++) {
    double z = lz[k];
    const double xz[2][2] = { { px, z }, { hx, z + 16 } };
    for (int j = 0; j < 2; j++) {
      double x = xz[j][0], q = xz[j][1];
      while (palm_near(x, q, 3.2)) q += 1.2;
      if (!(fabs(q) <= DISTRICT.zMax + 5)) continue;
      place(&L, G->lamp, x, H, q, 0, 0.14);
      if (R() < 0.55) {
        double dz = 1.6 * (R() < 0.5 ? 1 : -1);
        place(&L, G->trash, x, H, q + dz, 0, 0.3);
      }
    }
  }
  for (int si = 0; si < 2; si++) {
    double s = si ? 1 : -1;
    for (double z = 285 + R() * 10; z <= 340; z += 42 + R() * 10) {
      double q = s * z;
      while (palm_near(hx, q, 1.4)) q += 1.5;
      place(&L, G->pay, hx + 0.05, H, q, -PI_D / 2, COL_BOX);
    }
    for (double z = 325; z <= 340; z += 45) place(&L, G->drainW, SIDEWALK_W.x1, 0, s * z, 0, COL_NONE);
    for (double z = 290 + R() * 20; z <= 340; z += 55 + R() * 20) {
      double lane = LANE_C[R() < 0.5 ? 0 : 1];
      double x = lane + (R() - 0.5) * 1.2;
      place(&L, G->manhole, x, roadHeight(x) + 0.002, s * z, 0, COL_NONE);
    }
    for (double z = 155 + R() * 10; z <= 340; z += 9 + R() * 22) {
      double x = promenadeX(s * z) + 2.8 + R() * 1.4;
      if (palm_near(x, s * z, 1.8) || x > PARK.x1 - 1 || R() < 0.2) continue;
      place(&L, G->bench, x, H, s * z, (R() - 0.5) * 0.2, COL_BOX);
    }
    const double hz[4] = { 160, 232, 268, 330 };
    for (int i = 0; i < 4; i++) place(&L, G->hyd, i % 2 ? px - 0.15 : hx + 0.15, H, s * hz[i], 0, 0.2);
  }
#undef R
  // intersections
  Lit lit[16];
  int nlit = 0;
  Blade blades[NCROSS_STREETS * 2];
  int nb = 0;
  StopSign stops[NCROSS_STREETS];
  int nst = 0;
  g_force = true;
  const double cxr = SIDEWALK_W.x1 - CROSS.R;
  for (int i = 0; i < g_nnear; i++) {
    const CrossStreet *c = g_near[i];
    for (int si = 0; si < 2; si++) {
      double s = si ? 1 : -1;
      double x = cxr + 0.75, z = c->z + s * (OPEN_W - 0.75);
      Geometry *post = sign_post_geo(3.2);
      place(&L, post, x, H, z, 0, 0.08);
      geo_free(post);
      blades[nb++] = (Blade){ x, z, c->name };
    }
    if (!c->signal) {
      double x = CROSS.xw0 - 2.3, z = c->z + CROSS.hw + 0.6;
      Geometry *post = sign_post_geo(2.2);
      place(&L, post, x, H, z, 0, 0.08);
      geo_free(post);
      stops[nst++] = (StopSign){ x, z };
      continue;
    }
    // signal: a mast arm from each side of Ocean Drive, heads for both directions of Ocean
    // Drive and far-side heads for the cross street
    double legs[2][2];
    crossLegs(c->z, legs);
    double eastX = SIDEWALK_E.x0 + 0.7, eastZ = legs[1][1] + 1.2, westX = SIDEWALK_W.x1 - 0.7, westZ = legs[0][0] - 1.2;
    const double arms[2][4] = { { eastX, eastZ, LANE_C[1], 0 }, { westX, westZ, LANE_C[0], PI_D } };
    for (int k = 0; k < 2; k++) {
      double pX = arms[k][0], pZ = arms[k][1], armTo = arms[k][2], facing = arms[k][3];
      Geometry *pole = signal_pole_geo(pX, armTo);
      place(&L, pole, pX, H, pZ, 0, 0.18);
      geo_free(pole);
      Geometry *head = signal_head_geo();
      place(&L, head, armTo, H + 4.9, pZ, facing, COL_NONE);
      geo_free(head);
      lit[nlit++] = (Lit){ armTo, H + 4.9, pZ, facing, 2 };
    }
    Geometry *head = signal_head_geo();
    place(&L, head, eastX, H + 2.6, eastZ - 0.25, -PI_D / 2, COL_NONE);
    geo_free(head);
    lit[nlit++] = (Lit){ eastX, H + 2.6, eastZ - 0.25, -PI_D / 2, 0 };
    head = signal_head_geo();
    place(&L, head, westX - 0.2, H + 2.6, westZ, -PI_D / 2, COL_NONE);
    geo_free(head);
    lit[nlit++] = (Lit){ westX - 0.2, H + 2.6, westZ, -PI_D / 2, 0 };
  }
  g_force = false;
  // one merged mesh per block (split at the cross streets) for culling
  double edges[NCROSS_STREETS + 2];
  int ne = 0;
  edges[ne++] = -INFINITY;
  for (int i = 0; i < g_nnear; i++) edges[ne++] = g_near[i]->z;
  edges[ne++] = INFINITY;
  int nout = 0;
  for (int i = 0; i < ne - 1; i++) {
    GeoList part = {};
    for (size_t k = 0; k < L.len; k++) {
      Box3 b = geo_compute_bbox(L.data[k]);
      double z = (b.min.z + b.max.z) / 2;
      if (z >= edges[i] && z < edges[i + 1]) vec_push(&part, L.data[k]);
    }
    if (!part.len) continue;
    Geometry *merged = geo_merge(part.data, (int)part.len);
    vec_free(&part);
    Node *m = mesh_of(merged, mat, true, true, "street district furniture");
    m->user = (void *)1;   // userData.cullBlock
    out[nout++] = m;
  }
  for (size_t k = 0; k < L.len; k++) geo_free(L.data[k]);
  vec_free(&L);
  // lit signal lenses (green along Ocean Drive, red for the cross street)
  if (nlit) {
    GeoList lens = {};
    for (int i = 0; i < nlit; i++) {
      const Lit *q = &lit[i];
      uint32_t hex = q->lamp == 2 ? 0x7dffb0 : 0xff4f3c;
      Geometry *g = colored(geo_translate(geo_rotate_x(geo_cyl(0.1, 0.1, 0.02, 14), PI_D / 2), 0, 0.35 - q->lamp * 0.35, 0.14), hex);
      gl_push(&lens, geo_translate(geo_rotate_y(g, q->rot), q->x, q->y, q->z));
    }
    MatDesc ld = md_basic();
    ld.name = "street signal lenses";
    ld.vertex_colors = true;
    ld.prog[MV_PLAIN] = PROG_BASIC_VCOL;
    out[nout++] = mesh_of(gl_merge(&lens), mat_three(&ld), false, false, "street signal lenses");
  }
  out[nout++] = cross_signs(blades, nb, stops, nst);
  return nout;
}

static int furniture(Node **out) {
  Rng rng = rng_make(111);
  Rng *rnd = &rng;
#define R() rng_next(rnd)
  GeoList L = {};
  Furn G = { lamp_geo(), pay_station_geo(), trash_geo(), hydrant_geo(), bike_rack_geo(), bench_geo(),
             drain_geo(1), drain_geo(-1), manhole_geo(), news_box_geo(0x2f64a8), news_box_geo(0xb3372e), valet_geo() };
  const double H = CURB_HEIGHT;
  const double hx = SIDEWALK_W.x1 - 0.55, px = SIDEWALK_E.x0 + 0.55;
  for (double z = -290; z <= 290; z += 32) {
    const double xz[2][2] = { { px, z }, { hx, z + 16 } };
    for (int j = 0; j < 2; j++) {
      double x = xz[j][0], q = xz[j][1];
      while (palm_near(x, q, 3.2)) q += 1.2;
      if (fabs(q - CROSSWALK_Z) < 3) q += 4;
      place(&L, G.lamp, x, H, q, 0, 0.14);
      if (R() < 0.55) {
        double dz = 1.6 * (R() < 0.5 ? 1 : -1);
        place(&L, G.trash, x, H, q + dz, 0, 0.3);
      }
    }
  }
  // parking pay stations on the hotel side, bike racks near hotel entrances
  for (double z = -270; z <= 270; z += 42 + R() * 10) {
    double q = z;
    while (palm_near(hx, q, 1.4) || fabs(q - CAR.z) < 3) q += 1.5;
    place(&L, G.pay, hx + 0.05, H, q, -PI_D / 2, COL_BOX);
  }
  const double racks[4] = { -58, -19, 27, 61 };
  for (int i = 0; i < 4; i++) place(&L, G.rack, SIDEWALK_W.x1 - 1.1, H, racks[i], 0, COL_BOX);
  // hotel-side bins, a valet stand at an entrance, news boxes, an extra pay kiosk
  const double bins[4] = { 16, 4, -21, -47 };
  for (int i = 0; i < 4; i++) {
    double q = bins[i];
    while (palm_near(hx, q, 1.2)) q += 1;
    place(&L, G.trash, hx - 0.1, H, q, 0, 0.3);
  }
  place(&L, G.valet, -26.9, H, 25.5, PI_D / 2, COL_BOX);
  place(&L, G.newsB, SIDEWALK_W.x1 - 1.3, H, 13.2, -PI_D / 2, COL_BOX);
  place(&L, G.newsR, SIDEWALK_W.x1 - 1.3, H, 13.8, -PI_D / 2, COL_BOX);
  place(&L, G.pay, hx + 0.05, H, 20.5, -PI_D / 2, COL_BOX);
  // hydrants
  const double hyd[6][2] = { { hx + 0.15, -33 }, { hx + 0.15, 22 }, { px - 0.15, -8 }, { px - 0.15, 46 }, { hx + 0.15, -110 }, { px - 0.15, 120 } };
  for (int i = 0; i < 6; i++) place(&L, G.hyd, hyd[i][0], H, hyd[i][1], 0, 0.2);
  // benches in the park facing the sea, just east of the promenade
  for (double z = -150; z <= 150; z += 9 + R() * 22) {
    double x = promenadeX(z) + 2.8 + R() * 1.4;
    if (palm_near(x, z, 1.8) || x > PARK.x1 - 1 || R() < 0.2) continue;
    place(&L, G.bench, x, H, z, (R() - 0.5) * 0.2, COL_BOX);
  }
  // storm drains at both curbs, manholes in the lanes
  for (double z = -280; z <= 280; z += 45) {
    if (fabs(z - CAR.z) > 3.5 && fabs(z - CROSSWALK_Z) > 3) place(&L, G.drainW, SIDEWALK_W.x1, 0, z + 5, 0, COL_NONE);
    if (fabs(z + 20 - CROSSWALK_Z) > 3) place(&L, G.drainE, SIDEWALK_E.x0, 0, z + 20, 0, COL_NONE);
  }
  for (double z = -270; z <= 270; z += 55 + R() * 20) {
    double lane = LANE_C[R() < 0.5 ? 0 : 1];
    double x = lane + (R() - 0.5) * 1.2;
    place(&L, G.manhole, x, roadHeight(x) + 0.002, z, 0, COL_NONE);
  }
#undef R
  // sign posts at the crosswalk corners, regulation signs along the parking lane
  const double posts[2][2] = { { SIDEWALK_W.x1 - 0.35, CW.zMin - 0.6 }, { SIDEWALK_E.x0 + 0.35, CW.zMax + 0.6 } };
  for (int i = 0; i < 2; i++) {
    Geometry *p = sign_post_geo(3.2);
    place(&L, p, posts[i][0], H, posts[i][1], 0, 0.08);
    geo_free(p);
  }
  const double regs0[3][2] = { { SIDEWALK_W.x1 - 0.35, -12 }, { SIDEWALK_W.x1 - 0.35, 36 }, { SIDEWALK_W.x1 - 0.35, 74 } };
  double regs[3][2];
  int nregs = 0;
  for (int i = 0; i < 3; i++)
    if (!blocks_crossing(regs0[i][0], regs0[i][1])) { regs[nregs][0] = regs0[i][0]; regs[nregs][1] = regs0[i][1]; nregs++; }
  for (int i = 0; i < nregs; i++) {
    Geometry *p = sign_post_geo(2.4);
    place(&L, p, regs[i][0], H, regs[i][1], 0, 0.08);
    geo_free(p);
  }
  MatDesc d = md_standard();
  d.name = "street furniture";
  d.vertex_colors = true;
  d.roughness = 0.55;
  d.metalness = 0.15;
  d.prog[MV_PLAIN] = PROG_STD_VCOL;
  Material *mat = mat_three(&d);
  int n = 0;
  out[n++] = mesh_of(gl_merge(&L), mat, true, true, "street furniture");
  out[n++] = signs(posts, 2, (const double (*)[2])regs, nregs);
  n += district_furniture(mat, &G, out + n);
  Geometry **all = (Geometry **)&G;
  for (size_t i = 0; i < sizeof G / sizeof(Geometry *); i++) geo_free(all[i]);
  return n;
}

Node *build_street(Node *scene) {
  init_layout();
  vec_clear(&g_colliders);
  Node *group = node_new(NODE_GROUP, "street");
  Node *walks[2];
  Texture *concrete;
  sidewalks(walks, &concrete);
  Material *roadMat;
  Node *road = road_mesh(&roadMat);
  Node *furn[32];
  int nf = furniture(furn);
  node_add(group, road);
  node_add(group, cross_road_mesh(roadMat));
  node_add(group, markings());
  node_add(group, walks[0]);
  node_add(group, walks[1]);
  node_add(group, curbs(concrete));
  for (int i = 0; i < nf; i++) node_add(group, furn[i]);
  for (int i = 0; i < nf; i++) {
    if (furn[i]->user != (void *)1) continue;
    furn[i]->user = nullptr;
    Box3 b = furn[i]->geo->bbox;
    lod_register(furn[i], b.min.z, b.max.z, LOD_DETAIL);
  }
  node_add(scene, group);
  return group;
}
