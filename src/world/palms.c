// Port of src/world/palms.js: palms of Ocean Drive / Lummus Park. Mostly tall slender coconut
// palms (curved, ringed grey-tan trunks, drooping pinnate fronds, dead fronds and nuts under the
// crown), some royal palms (smooth grey trunk, green crownshaft) and a few sabal palms
// (boot-covered trunk, fan fronds) in the park.
// Geometry: a handful of trunk and frond variants in two BatchedMeshes; leaflets are alpha-cut
// ribbons so frond shadows show the gaps between them. Wind sways frond tips in the vertex
// shader (frozen in shot mode through the time passed to palms_update()).
#include "world/palms.h"

#include "canvas/canvas.h"
#include "gfx/three_mat.h"
#include "quality.h"
#include "textures/noise.h"
#include "world/layout.h"
#include "world/lod.h"

#define TAU (PI_D * 2)

// ---- placement ---------------------------------------------------------------------------------
static constexpr double SPAN = 300;
static constexpr double EXT = DISTRICT.zMax + 8;

typedef Vec(PalmTree) TreeVec;

// the tree() overrides (o); unset = JS undefined
typedef struct TreeOpt {
  bool has_variant, has_rotY, has_k, has_hs, has_species;
  int variant;
  double rotY, k, hs;
  PalmSpecies species;
} TreeOpt;

typedef struct Planter { Rng *rnd; TreeVec trees; } Planter;

static int pick8(Rng *rnd, const int *arr, int n) { return arr[(int)fmod(floor(rng_next(rnd) * n), n)]; }

static void tree(Planter *P, double x, double z, PalmRow row, int ground, const TreeOpt *o) {
  Rng *rnd = P->rnd;
  double r = rng_next(rnd);
  PalmSpecies species = SP_COCONUT;
  if (row == ROW_PARK) species = r < 0.12 ? SP_SABAL : r < 0.2 ? SP_ROYAL : SP_COCONUT;
  else if (row == ROW_EDGE && r < 0.1) species = SP_SABAL;
  else if (r < 0.08) species = SP_ROYAL;
  static const int PARK_V[8] = { 0, 1, 2, 2, 3, 3, 4, 1 };
  static const int ROW_V[13] = { 0, 1, 1, 3, 3, 4, 4, 5, 0, 1, 3, 5, 4 };
  int variant = o && o->has_variant ? o->variant
              : species != SP_COCONUT ? 0
              : row == ROW_PARK ? pick8(rnd, PARK_V, 8) : pick8(rnd, ROW_V, 13);
  // lean direction and amount vary a lot: mostly toward the ocean (+x) but anything from near
  // vertical (shear cancels the variant's lean) to steep
  double spread = row == ROW_PARK ? 1.3 : 1.2;
  double rotY;
  if (o && o->has_rotY) rotY = o->rotY;
  else {
    double a = (rng_next(rnd) - 0.5) * 2 * spread;
    rotY = a + (rng_next(rnd) < (row == ROW_PARK ? 0.3 : 0.12) ? PI_D : 0);
  }
  // street-side palms mostly near upright (0-12 deg, a few ~20), stronger leans in the park
  bool street = row != ROW_PARK;
  double k;
  if (o && o->has_k) k = o->k;
  else if (street) k = rng_next(rnd) < 0.1 ? 0.08 : -0.08 - rng_next(rnd) * 0.06;
  else k = rng_next(rnd) < 0.25 ? -0.1 - rng_next(rnd) * 0.05 : (rng_next(rnd) - 0.3) * 0.18;
  // height: 6-20 m so crowns layer, some above the hotel roofs
  double hs = o && o->has_hs ? o->hs : 0.78 + rng_next(rnd) * 0.5;
  PalmTree t = { .x = x, .z = z, .row = row, .ground = ground, .species = o && o->has_species ? o->species : species,
                 .variant = variant, .rotY = rotY, .k = k, .hs = hs };
  t.scale = 0.88 + rng_next(rnd) * 0.26;
  t.seed = floor(rng_next(rnd) * 1e9);
  vec_push(&P->trees, t);
}

// irregular rows: mostly 7-14 m, the odd long gap, the odd close pair (skip is always false)
static void row(Planter *P, double x0, PalmRow rowName, int ground, double zA, double zB) {
  Rng *rnd = P->rnd;
  for (double z = zA; z <= zB;) {
    double x = x0 + (rng_next(rnd) - 0.5) * 0.3;
    double tz = z + (rng_next(rnd) - 0.5);
    tree(P, x, tz, rowName, ground, nullptr);
    if (rng_next(rnd) < 0.22) {
      // close pair, leaning apart or crossing
      double dz = 1.6 + rng_next(rnd) * 1.2;
      PalmTree first = P->trees.data[P->trees.len - 1];
      double rot = rng_next(rnd) < 0.5 ? first.rotY + PI_D * (0.6 + rng_next(rnd) * 0.4) : first.rotY + 0.5;
      double x2 = x + (rng_next(rnd) - 0.5) * 0.4;
      TreeOpt o = { .has_rotY = true, .rotY = rot, .has_hs = true };
      o.hs = first.hs * (0.7 + rng_next(rnd) * 0.25);
      tree(P, x2, z + dz, rowName, ground, &o);
    }
    z += rng_next(rnd) < 0.18 ? 17 + rng_next(rnd) * 9 : 7 + rng_next(rnd) * 7;
  }
}

static bool promenade(double x, double z) {
  return fabs(x - PARK.promenadeX - 2.6 * sin(z / 19) - 1.2 * sin(z / 7.3)) < 3.0;
}

static TreeVec plan(void) {
  Rng r1 = rng_make(3301);
  Planter P = { &r1, {} };
  Rng *rnd = P.rnd;
  // hotel side: sidewalk tree grates just behind the curb
  row(&P, SIDEWALK_W.x1 - 0.9, ROW_HOTEL, 0, -SPAN, SPAN);
  // park edge along the park-side sidewalk
  row(&P, SIDEWALK_E.x1 + 0.9, ROW_EDGE, 1, -SPAN, SPAN);
  // Lummus Park: dense scattered clusters, clear of the winding promenade
  // (a few near the ocean-side wall frame the sea and sun from inside the park)
  static const double SPECIAL[4][3] = { { 11.2, -18, 0 }, { 11.3, -8.6, 0 }, { 10.6, -26, 1 }, { 10.8, -1, 2 } };
  for (int i = 0; i < 4; i++) {
    TreeOpt o = { .has_species = true, .species = SP_COCONUT, .has_variant = true, .variant = (int)SPECIAL[i][2], .has_rotY = true };
    o.rotY = (rng_next(rnd) - 0.5) * 0.5;
    tree(&P, SPECIAL[i][0], SPECIAL[i][1], ROW_PARK, 1, &o);
  }
  for (double z = -SPAN; z <= SPAN; z += 6 + rng_next(rnd) * 7) {
    double cx = PARK.x0 + 2 + rng_next(rnd) * (PARK.x1 - PARK.x0 - 4);
    int n;
    if (rng_next(rnd) < 0.35) n = 1;
    else if (rng_next(rnd) < 0.65) n = 2;
    else n = 3 + (int)floor(rng_next(rnd) * 2);
    double pts[8][2];
    int np = 0;
    for (int k = 0; k < n * 5 && np < n; k++) {
      double a = rng_next(rnd) * PI_D * 2, d = np ? 1.3 + rng_next(rnd) * 1.6 : 0;
      double x = cx + cos(a) * d, zz = z + sin(a) * d;
      if (promenade(x, zz)) continue;
      if (x < PARK.x0 + 1.2 || x > PARK.x1 - 1.0) continue;
      if (fabs(zz + 14) < 5 && x > 2 && x < 7) continue;   // keep the park view east open around the viewer
      bool near = false;
      for (size_t i = 0; i < P.trees.len && !near; i++) near = js_hypot2(P.trees.data[i].x - x, P.trees.data[i].z - zz) < 1.3;
      for (int i = 0; i < np && !near; i++) near = js_hypot2(pts[i][0] - x, pts[i][1] - zz) < 1.3;
      if (near) continue;
      pts[np][0] = x;
      pts[np][1] = zz;
      np++;
    }
    for (int i = 0; i < np; i++) tree(&P, pts[i][0], pts[i][1], ROW_PARK, 1, nullptr);
  }
  // the ends of the extended district (own random stream: everything above is unchanged)
  Rng r2 = rng_make(3302);
  P.rnd = rnd = &r2;
  const double ends[2][2] = { { -EXT, -SPAN - 4 }, { SPAN + 4, EXT } };
  for (int e = 0; e < 2; e++) {
    double zA = ends[e][0], zB = ends[e][1];
    row(&P, SIDEWALK_W.x1 - 0.9, ROW_HOTEL, 0, zA, zB);
    row(&P, SIDEWALK_E.x1 + 0.9, ROW_EDGE, 1, zA, zB);
    for (double z = zA; z <= zB; z += 6 + rng_next(rnd) * 7) {
      double x = PARK.x0 + 2 + rng_next(rnd) * (PARK.x1 - PARK.x0 - 4);
      if (promenade(x, z) || x > PARK.x1 - 1.0) continue;
      tree(&P, x, z, ROW_PARK, 1, nullptr);
    }
  }
  // no tree grates in the cross-street mouths or on the crosswalk ramps
  TreeVec out = {};
  for (size_t i = 0; i < P.trees.len; i++) {
    const PalmTree *t = &P.trees.data[i];
    bool keep = true;
    if (t->row == ROW_HOTEL) {
      if (crossStreetAt(t->z, 1.5)) keep = false;
      for (int c = 0; c < NCROSS_STREETS && keep; c++) {
        if (CROSS_STREETS[c].far) continue;
        double legs[2][2];
        crossLegs(CROSS_STREETS[c].z, legs);
        for (int l = 0; l < 2; l++)
          if (t->z > legs[l][0] - 1.2 && t->z < legs[l][1] + 1.2) keep = false;
      }
    }
    if (keep) vec_push(&out, *t);
  }
  vec_free(&P.trees);
  return out;
}

static TreeVec g_trees;
static bool g_trees_done;

const PalmTree *palm_trees(int *n) {
  if (!g_trees_done) { g_trees = plan(); g_trees_done = true; }
  *n = (int)g_trees.len;
  return g_trees.data;
}

// Wind-rustle sound sources: one per ~45 m of each row near the block, ~100 m beyond it through
// the district.
static double (*g_clusters)[2];
static int g_nclusters;

const double (*palm_clusters(int *n))[2] {
  if (!g_clusters) {
    int nt;
    const PalmTree *trees = palm_trees(&nt);
    typedef struct Bin { char key[32]; double x, z; int n; } Bin;
    Vec(Bin) bins = {};
    for (int i = 0; i < nt; i++) {
      const PalmTree *t = &trees[i];
      if (fabs(t->z) > DISTRICT.zMax + 5) continue;
      char key[32];
      if (fabs(t->z) <= 80) snprintf(key, sizeof key, "%d|%d", t->row == ROW_HOTEL ? 0 : 1, (int)floor((t->z + 80) / 45));
      else snprintf(key, sizeof key, "%d|f%d", t->row == ROW_HOTEL ? 0 : 1, (int)floor((t->z + 1000) / 100));
      Bin *b = nullptr;
      for (size_t k = 0; k < bins.len && !b; k++) if (!strcmp(bins.data[k].key, key)) b = &bins.data[k];
      if (!b) {
        vec_push(&bins, ((Bin){}));
        b = &bins.data[bins.len - 1];
        snprintf(b->key, sizeof b->key, "%s", key);
      }
      b->x += t->x;
      b->z += t->z;
      b->n++;
    }
    g_nclusters = (int)bins.len;
    g_clusters = xmalloc(sizeof *g_clusters * (size_t)(bins.len ? bins.len : 1));
    for (size_t k = 0; k < bins.len; k++) {
      g_clusters[k][0] = js_to_fixed(bins.data[k].x / bins.data[k].n, 1);
      g_clusters[k][1] = js_to_fixed(bins.data[k].z / bins.data[k].n, 1);
    }
    vec_free(&bins);
  }
  *n = g_nclusters;
  return (const double (*)[2])g_clusters;
}

// ---- textures ----------------------------------------------------------------------------------
// Frond atlas, three columns of 256 px: leaflets (u across the leaflet, v along the rachis,
// 6 leaflets per tile, tiling in v), sabal fan (u radius, v angle), solid rachis.
static constexpr double LEAF_TILE = 0.42;   // metres of rachis per texture tile
static constexpr double LEAF_SLANT = 0.78;  // leaflet tip runs this many tiles toward the frond tip
static constexpr double LEAF_N = 7;         // leaflets per tile and side

static Texture *frond_atlas(void) {
  Canvas *c = canvas_new(768, 512);
  Rng rng = rng_make(91);
  Rng *rnd = &rng;
  // leaflets are drawn on a taller scratch canvas and blurred, so the alpha edge is a smooth
  // ramp, then the middle 512 rows are copied in (no blurred-away rows at the tile edges)
  Canvas *lc = canvas_new(256, 768);
  cv_save(lc);
  cv_translate(lc, 0, 128);
  // many long narrow leaflets with sky between them; tips of varying length separate
  for (int k = -16; k < 26; k++) {
    double y0 = k * (512 / LEAF_N) + (rng_next(rnd) - 0.5) * 8;
    double y1 = y0 + LEAF_SLANT * 512 + (rng_next(rnd) - 0.5) * 40;
    double L = 190 + rng_next(rnd) * 64, w0 = 66 + rng_next(rnd) * 12;   // ~4 cm blades, ~75% coverage
    double pts[17][2], back[17][2];
    for (int i = 0; i <= 16; i++) {
      double t = i / 16.0;
      double x = 6 + (L - 6) * t, y = y0 + (y1 - y0) * t + sin(t * 3.1) * 14 + t * t * 22;
      double w = w0 * fmin(1, t / 0.08 + 0.35) * pow(1 - t, 0.65) * 0.5;
      pts[i][0] = x; pts[i][1] = y - w;
      back[i][0] = x; back[i][1] = y + w;
    }
    Gradient g = cv_linear_gradient(lc, 0, 0, 256, 0);
    bool brown = rng_next(rnd) < 0.18;
    grad_add_stop(&g, 0, "#b8cfa6");
    grad_add_stop(&g, 0.7, "#cfdcac");
    grad_add_stop(&g, 1, brown ? "#b39462" : "#dcdca6");
    cv_fill_gradient(lc, &g);
    cv_begin_path(lc);
    cv_move_to(lc, pts[0][0], pts[0][1]);
    for (int i = 0; i <= 16; i++) cv_line_to(lc, pts[i][0], pts[i][1]);
    for (int i = 16; i >= 0; i--) cv_line_to(lc, back[i][0], back[i][1]);
    cv_close_path(lc);
    cv_fill(lc);
    cv_stroke_color(lc, "rgba(245,244,210,0.8)");
    cv_line_width(lc, 3);
    cv_begin_path(lc);
    cv_move_to(lc, 6, y0);
    cv_line_to(lc, L * 0.9, y0 + (y1 - y0) * 0.9);
    cv_stroke(lc);
  }
  cv_restore(lc);
  Canvas *bc = canvas_new(256, 768);
  cv_filter_blur(bc, 1.6);
  cv_draw_image(bc, lc, 0, 0, 256, 768, 0, 0, 256, 768);
  cv_draw_image(c, bc, 0, 128, 256, 512, 0, 0, 256, 512);
  canvas_free(lc);
  canvas_free(bc);
  cv_fill_color(c, "#c9cc9a");
  cv_fill_rect(c, 0, 0, 7, 512);   // leaflet bases along the rachis
  // sabal fan: segments joined to ~half radius, split tapering tips beyond
  const int segs = 36;
  const double sh = 512.0 / segs;
  for (int i = 0; i < segs; i++) {
    double y = i * sh, rs = (0.42 + rng_next(rnd) * 0.18) * 256, rt = (0.86 + rng_next(rnd) * 0.14) * 256;
    double l = 0.82 + rng_next(rnd) * 0.18;
    char css[64];
    snprintf(css, sizeof css, "rgb(%.0f,%.0f,%.0f)", js_round(196 * l), js_round(212 * l), js_round(160 * l));
    cv_fill_color(c, css);
    cv_fill_rect(c, 256, y, rs, sh + 0.5);
    cv_begin_path(c);
    cv_move_to(c, 256 + rs - 2, y + 1.5);
    cv_line_to(c, 256 + rt, y + sh * 0.5);
    cv_line_to(c, 256 + rs - 2, y + sh - 1.5);
    cv_close_path(c);
    cv_fill(c);
    cv_fill_color(c, "rgba(255,255,230,0.35)");
    cv_fill_rect(c, 256, y + sh * 0.45, rs, 1.5);
  }
  Gradient rg = cv_linear_gradient(c, 512, 0, 768, 0);
  grad_add_stop(&rg, 0, "#b9b27c");
  grad_add_stop(&rg, 1, "#d6cf98");
  cv_fill_gradient(c, &rg);
  cv_fill_rect(c, 512, 0, 256, 512);
  Texture *t = canvas_texture(c, true, WRAP_CLAMP, 8);
  canvas_free(c);
  return t;
}

// Trunk atlas 512 x 512 (1 m tall): left half coconut / royal leaf-scar rings and vertical
// cracks, right half sabal criss-cross leaf-base boots. Albedo + normal map.
typedef struct Ring { double y, w, a, ph, f, amp; } Ring;
static void trunk_textures(Texture **map, Texture **normal_map) {
  const int S = 512;
  float *H = xcalloc((size_t)S * S, sizeof(float));
  Rng rng = rng_make(17);
  Rng *rnd = &rng;
  uint8_t *img = xcalloc((size_t)S * S, 4);
  Ring rings[64];
  int nr = 0;
  // closely spaced, irregular, bumpy leaf-scar rings (~3-6 cm apart)
  for (double y = 0; y < S;) {
    Ring r = { .y = y };
    r.w = 2.5 + rng_next(rnd) * 3;
    r.a = 0.55 + rng_next(rnd) * 0.45;
    r.ph = rng_next(rnd) * 6.28;
    r.f = 1 + floor(rng_next(rnd) * 3);
    r.amp = 2 + rng_next(rnd) * 5;
    CHECK(nr < 64);
    rings[nr++] = r;
    y += 16 + rng_next(rnd) * 16;
  }
  float *bump = fbm_field(256, 5, 24, 2, 0.5);
  for (int y = 0; y < S; y++)
    for (int x = 0; x < S; x++) {
      double h = 0.5, alb = 0.9;
      double n = (rng_next(rnd) - 0.5) * 0.04 + (x < 256 ? (bump[(y >> 1) * 256 + x] - 0.5) * 0.18 : 0);
      if (x < 256) {
        for (int k = 0; k < nr; k++) {
          const Ring *r = &rings[k];
          double yy = r->y + r->amp * sin((x / 256.0) * PI_D * 2 * r->f + r->ph) + 1.5 * sin((x / 256.0) * PI_D * 2 * 7 + r->ph * 3);
          double d = fabs(y - yy);
          d = fmin(d, S - d);
          if (d < r->w * 2.5) {
            double kk = exp(-(d * d) / (r->w * r->w)) * r->a;
            h -= 0.55 * kk;
            alb -= 0.42 * kk;
          } else if (d < r->w * 5) h += 0.06 * r->a;   // slightly swollen between scars
        }
      } else {
        // diamond lattice of split leaf bases
        double u = (x - 256) / 256.0, v = (double)y / S;
        double a = fmod(u * 6 + v * 7, 1), b = fmod(u * 6 - v * 7 + 14, 1);
        double cell = fmin(fmin(a, 1 - a), fmin(b, 1 - b));
        h = 0.3 + fmin(1, cell * 6) * 0.6 - (fabs(a - 0.5) < 0.06 && b > 0.3 ? 0.35 : 0);
        alb = 0.55 + 0.4 * fmin(1, cell * 5);
      }
      h += n;
      alb += n;
      H[y * S + x] = (float)h;
      size_t i = ((size_t)y * S + x) * 4;
      double v = fmax(0, fmin(255, alb * 255));
      img[i] = js_u8clamp(v);
      img[i + 1] = js_u8clamp(v * 0.97);
      img[i + 2] = js_u8clamp(v * 0.92);
      img[i + 3] = 255;
    }
  free(bump);
  Canvas *cv = canvas_new(S, S);
  cv_put_image_data(cv, img, 0, 0, S, S);
  for (int y = 0; y < S; y++)
    for (int x = 0; x < S; x++) {
      int half = x >= 256 ? 256 : 0, lx = x - half;
      double hx = ((double)H[y * S + half + ((lx + 1) % 256)] - H[y * S + half + ((lx + 255) % 256)]) * 3.0;
      double hy = ((double)H[((y + 1) % S) * S + x] - H[((y + S - 1) % S) * S + x]) * 3.0;
      double l = js_hypot3(hx, hy, 1);
      size_t i = ((size_t)y * S + x) * 4;
      img[i] = js_u8clamp((-hx / l * 0.5 + 0.5) * 255);
      img[i + 1] = js_u8clamp((hy / l * 0.5 + 0.5) * 255);
      img[i + 2] = js_u8clamp((1 / l * 0.5 + 0.5) * 255);
      img[i + 3] = 255;
    }
  Canvas *nc = canvas_new(S, S);
  cv_put_image_data(nc, img, 0, 0, S, S);
  free(img);
  free(H);
  *map = canvas_texture(cv, true, WRAP_REPEAT, 8);
  *normal_map = canvas_texture(nc, false, WRAP_REPEAT, 8);
  canvas_free(cv);
  canvas_free(nc);
}

// ---- geometry helpers: indexed position / uv (+ colour for trunks) ------------------------------
typedef struct Mesher { DVec p, uv, c; bool color; U32Vec idx; } Mesher;

static void mv(Mesher *m, V3 p, double u, double v, Color col) {
  vec_push(&m->p, p.x); vec_push(&m->p, p.y); vec_push(&m->p, p.z);
  vec_push(&m->uv, u); vec_push(&m->uv, v);
  if (m->color) { vec_push(&m->c, col.r); vec_push(&m->c, col.g); vec_push(&m->c, col.b); }
}
static int mbase(const Mesher *m) { return (int)(m->p.len / 3); }
// the grid's faces, once its rows x cols vertices were emitted from `base` (row-major)
static void mgrid(Mesher *m, int base, int rows, int cols) {
  for (int i = 0; i < rows - 1; i++)
    for (int j = 0; j < cols - 1; j++) {
      uint32_t a = (uint32_t)(base + i * cols + j), b = a + 1, c = a + (uint32_t)cols, d = c + 1;
      uint32_t q[6] = { a, b, c, b, d, c };
      vec_append(&m->idx, q, 6);
    }
}
static Geometry *mgeometry(Mesher *m) {
  Geometry *g = geo_new();
  int n = (int)(m->p.len / 3);
  geo_set_attr_d(g, "position", 3, n, m->p.data);
  geo_set_attr_d(g, "uv", 2, n, m->uv.data);
  if (m->color) geo_set_attr_d(g, "color", 3, n, m->c.data);
  geo_set_index(g, m->idx.data, (int)m->idx.len);
  geo_compute_vertex_normals(g);
  vec_free(&m->p); vec_free(&m->uv); vec_free(&m->c); vec_free(&m->idx);
  return g;
}

static V3 bez(const V3 P[4], double t) {
  double u = 1 - t;
  double r[3];
  for (int k = 0; k < 3; k++) {
    double p0 = v3_comp(P[0], k), p1 = v3_comp(P[1], k), p2 = v3_comp(P[2], k), p3 = v3_comp(P[3], k);
    r[k] = u * u * u * p0 + 3 * u * u * t * p1 + 3 * u * t * t * p2 + t * t * t * p3;
  }
  return v3(r[0], r[1], r[2]);
}

typedef struct TrunkDef { PalmSpecies species; double H, lean, bend, seed, sc; } TrunkDef;
typedef struct Trunk { Geometry *geo; V3 top; } Trunk;

static double trunk_radius(PalmSpecies sp, double t, double s, double L, double seed, double crownshaft) {
  double h = t * L;
  if (sp == SP_COCONUT)   // slender, rough flared root boss
    return 0.125 + 0.045 * (1 - t) + 0.17 * exp(-h / 0.5) + 0.008 * sin(h * 1.7 + seed) + 0.035 * s * exp(-h / 0.45);
  if (sp == SP_ROYAL) {
    if (h > L - crownshaft) return 0.215 + 0.02 * sin(((h - (L - crownshaft)) / crownshaft) * PI_D);
    double q = (t - 0.4) / 0.25;
    return 0.22 + 0.06 * exp(-(q * q)) + 0.1 * exp(-h / 0.6) - 0.03 * t;
  }
  return 0.23 + 0.04 * exp(-h / 0.5) + 0.012 * s;
}

// Trunk swept along a cubic bezier in the x-y plane (lean toward +x).
static Trunk trunk_geometry(const TrunkDef *D) {
  double H = D->H, lean = D->lean, bend = D->bend, sc = D->sc;
  PalmSpecies species = D->species;
  V3 P[4];
  if (species == SP_COCONUT) {
    P[0] = v3(0, 0, 0);
    P[1] = v3(lean * bend + sc, H * 0.33, 0);
    P[2] = v3(lean * (0.55 + 0.35 * bend) - sc * 0.8, H * 0.66, 0);
    P[3] = v3(lean, H, 0);
  } else {
    P[0] = v3(0, 0, 0); P[1] = v3(lean * 0.3, H * 0.33, 0); P[2] = v3(lean * 0.7, H * 0.66, 0); P[3] = v3(lean, H, 0);
  }
  enum { SEGS = 44, RAD = 12 };
  V3 pts[SEGS + 1];
  double tans[SEGS + 1][2], len[SEGS + 1];
  len[0] = 0;
  for (int i = 0; i <= SEGS; i++) {
    double t = (double)i / SEGS;
    V3 p = bez(P, t), q = bez(P, fmin(1, t + 1e-3)), q0 = bez(P, fmax(0, t - 1e-3));
    pts[i] = p;
    double d0 = q.x - q0.x, d1 = q.y - q0.y, l = js_hypot2(d0, d1);
    tans[i][0] = d0 / l;
    tans[i][1] = d1 / l;
    if (i > 0) len[i] = len[i - 1] + js_hypot2(p.x - pts[i - 1].x, p.y - pts[i - 1].y);
  }
  double L = len[SEGS];
  double crownshaft = species == SP_ROYAL ? 1.9 : 0;
  double uOff = species == SP_SABAL ? 0.5 : 0;
  Color cA, cB;
  if (species == SP_COCONUT) { cA = color_hex(0xc2bcb0); cB = color_hex(0xd8d2c4); }
  else if (species == SP_ROYAL) { cA = color_hex(0xc4c2bb); cB = color_hex(0x5f8a3c); }
  else { cA = color_hex(0x7d6c58); cB = color_hex(0x6a5a47); }
  Mesher m = { .color = true };
  int base = mbase(&m);
  for (int i = 0; i <= SEGS; i++)
    for (int j = 0; j <= RAD; j++) {
      double t = (double)i / SEGS, a = ((double)j / RAD) * PI_D * 2;
      double tx = tans[i][0], ty = tans[i][1];
      double N0 = -ty, N1 = tx;   // in-plane normal; binormal is z
      double s = i < 6 ? sin(a * 5 + i * 1.3) * 0.6 + sin(a * 11 + i * 2.1) * 0.4 : sin(a * 3 + i);
      double r = trunk_radius(species, t, s, L, D->seed, crownshaft);
      V3 p = pts[i];
      V3 pos = { p.x + (N0 * cos(a)) * r, p.y + N1 * cos(a) * r, sin(a) * r };
      double h = len[i];
      Color col;
      if (species == SP_ROYAL) col = h > L - crownshaft ? cB : cA;
      else {
        // light grey to grey-tan; rougher and darker toward the base, a smoother lighter band below the crown
        col = color_lerp(cA, cB, fmax(0, fmin(1, (h - (L - 1.4)) / 1.4)));
        if (species == SP_COCONUT) col = color_scale(col, 0.72 + 0.28 * fmin(1, h / 1.6));
      }
      mv(&m, pos, uOff + ((double)j / RAD) * 0.5, h, col);
    }
  mgrid(&m, base, SEGS + 1, RAD + 1);
  // crown knob: a shaggy fibrous skirt of old frond bases flaring out where the crown meets
  // the trunk (coconut / sabal) or the spear leaf (royal), closing the top
  V3 top = pts[SEGS];
  static const double KNOB_ROYAL[4][2] = { { 0.2, 0.25 }, { 0.12, 0.8 }, { 0.03, 1.5 }, { 0, 1.8 } };
  static const double KNOB[7][2] = { { 0.15, -0.9 }, { 0.3, -0.45 }, { 0.46, -0.05 }, { 0.44, 0.25 }, { 0.3, 0.55 }, { 0.1, 0.75 }, { 0, 0.8 } };
  const double (*knob)[2] = species == SP_ROYAL ? KNOB_ROYAL : KNOB;
  int nk = species == SP_ROYAL ? 4 : 7;
  Color kc = color_hex(species == SP_ROYAL ? 0x6f9a44 : 0x8c6d45), kc2 = color_hex(0x5e4a33);
  base = mbase(&m);
  for (int i = 0; i < nk; i++)
    for (int j = 0; j <= RAD * 2; j++) {
      double a = ((double)j / (RAD * 2)) * PI_D * 2, r0 = knob[i][0], y = knob[i][1];
      double shag = species == SP_ROYAL ? 0 : (j % 2 ? 0.08 : -0.04) * fmin(1, i / 2.0) * (i < nk - 2 ? 1 : 0);
      double r = r0 * (1 + shag * 2);
      double yy = y - (j % 2 ? 0.12 : 0) * (i == 1 || i == 2 ? 1 : 0);
      mv(&m, v3(top.x + cos(a) * r, top.y + yy, sin(a) * r), uOff + ((double)j / (RAD * 2)) * 0.5, L + y, j % 3 ? kc : kc2);
    }
  mgrid(&m, base, nk, RAD * 2 + 1);
  return (Trunk){ mgeometry(&m), v3(top.x, top.y + (species == SP_ROYAL ? 0.25 : 0.45), 0) };
}

// Coconut clusters under the crown: two or three bunches of green / yellow nuts on orange-yellow
// fruit stalks, plus a few bare branched inflorescence stalks (local origin = crown attachment).
static void add_geo(Mesher *m, Geometry *g, uint32_t hex) {
  Color col = color_hex(hex);
  int base = mbase(m);
  const float *p = geo_data(g, "position"), *uv = geo_data(g, "uv");
  for (int k = 0; k < g->count; k++) mv(m, v3(p[k * 3], p[k * 3 + 1], p[k * 3 + 2]), uv[k * 2] * 0.4, uv[k * 2 + 1] * 0.2, col);
  for (int k = 0; k < g->index_count; k++) vec_push(&m->idx, (uint32_t)base + g->index[k]);
  geo_free(g);
}
static void stalk(Mesher *m, V3 a, V3 b, double r, uint32_t hex) {
  V3 d = v3_sub(b, a);
  double dl = v3_len(d);
  Geometry *g = geo_cylinder(r * 0.7, r, dl, 5, 1, false, 0, TAU);
  geo_translate(g, 0, dl / 2, 0);
  geo_apply_m4(g, m4_from_quat(quat_unit_vectors(v3(0, 1, 0), v3_norm(d))));
  geo_translate(g, a.x, a.y, a.z);
  add_geo(m, g, hex);
}
static Geometry *coconut_geometry(void) {
  Rng rng = rng_make(5);
  Rng *rnd = &rng;
  Mesher m = { .color = true };
  for (int b = 0; b < 3; b++) {
    double a = (b / 3.0) * PI_D * 2 + rng_next(rnd) * 0.6;
    double cx = cos(a) * 0.5, cz = sin(a) * 0.5, cy = -0.3 - rng_next(rnd) * 0.15;
    stalk(&m, v3(cos(a) * 0.15, -0.05, sin(a) * 0.15), v3(cx, cy + 0.15, cz), 0.035, 0xa8884e);
    int n = 3 + (int)floor(rng_next(rnd) * 3);
    bool yel = rng_next(rnd) < 0.25;
    for (int i = 0; i < n; i++) {
      double aa = ((double)i / n) * PI_D * 2 + rng_next(rnd) * 0.4, rr = 0.2 + rng_next(rnd) * 0.1;
      Geometry *g = geo_sphere3(0.13 + rng_next(rnd) * 0.03, 9, 7);
      geo_scale(g, 1, 1.15, 1);
      double ty = cy - rng_next(rnd) * 0.18;
      geo_translate(g, cx + cos(aa) * rr, ty, cz + sin(aa) * rr);
      uint32_t hex;
      if (yel && rng_next(rnd) < 0.6) hex = 0x9a8a32;
      else hex = rng_next(rnd) < 0.7 ? 0x4f5e22 : 0x6a5428;
      add_geo(&m, g, hex);
    }
  }
  // bare flower stalks: arching orange-yellow strands with a few branches
  for (int k = 0; k < 3; k++) {
    double a = rng_next(rnd) * PI_D * 2;
    V3 p0 = { cos(a) * 0.15, -0.05, sin(a) * 0.15 }, p1 = { cos(a) * 0.7, -0.35, sin(a) * 0.7 };
    stalk(&m, p0, p1, 0.03, 0xb49a62);
    for (int j = 0; j < 4; j++) {
      double t = 0.4 + j * 0.15;
      V3 q = { p0.x + (p1.x - p0.x) * t, p0.y + (p1.y - p0.y) * t, p0.z + (p1.z - p0.z) * t };
      double bb = a + (rng_next(rnd) - 0.5) * 1.4;
      double qy = q.y - 0.45 - rng_next(rnd) * 0.2;
      stalk(&m, q, v3(q.x + cos(bb) * 0.35, qy, q.z + sin(bb) * 0.35), 0.014, 0xbfa46c);
    }
  }
  return mgeometry(&m);
}

// Pinnate frond: arching rachis along +x (local), leaflet ribbons hanging from both sides in a V;
// uv.x = region + across (0 healthy, 1 ragged), uv.y = rachis tiles.
typedef struct FrondDef { double L, rise, droop, leaf, v0, v1, region, seed, tw; int segs; } FrondDef;
static V3 rach(const FrondDef *F, double t) {
  double L = F->L;
  return v3(L * t * (1 - 0.1 * t), L * (F->rise * t - F->droop * t * t), 0.06 * L * sin(t * 2.2 + F->seed) * t);
}
static void frond_frame(const FrondDef *F, double t, V3 *p, V3 *N) {
  *p = rach(F, t);
  V3 q = rach(F, t + 0.01);
  double T[3] = { q.x - p->x, q.y - p->y, q.z - p->z };
  double l = js_hypot3(T[0], T[1], T[2]);
  double Tn0 = T[0] / l, Tn1 = T[1] / l;
  *N = v3(-Tn1, Tn0, 0);
}
static Geometry *pinnate_frond(const FrondDef *F) {
  Rng rng = rng_make(F->seed);
  int segs = F->segs;
  double arc[32];
  arc[0] = 0;
  for (int i = 1; i <= segs; i++) {
    V3 a = rach(F, (double)(i - 1) / segs), b = rach(F, (double)i / segs);
    arc[i] = arc[i - 1] + js_hypot3(b.x - a.x, b.y - a.y, b.z - a.z);
  }
  Mesher m = {};
  const double t0 = 0.12;
  for (int si = 0; si < 2; si++) {
    double s = si ? 1 : -1;
    double jit = rng_next(&rng) * 0.1;
    int base = mbase(&m);
    for (int i = 0; i <= segs; i++)
      for (int j = 0; j < 3; j++) {
        double t = t0 + (1 - t0) * ((double)i / segs);
        V3 p, N;
        frond_frame(F, t, &p, &N);
        double a = F->v0 + (F->v1 - F->v0) * t + jit;
        // the blade plane twists along the rachis
        double ph = F->tw * t, cp = cos(ph), sp = sin(ph);
        double dn = -sin(a), dz = s * cos(a);
        V3 dir = { N.x * (dn * cp - dz * sp), N.y * (dn * cp - dz * sp), dn * sp + dz * cp };
        double w = F->leaf * (0.3 + 0.7 * sin(PI_D * fmin(1, (t - t0) * 1.35 + 0.12))) * (1 - 0.5 * t) * (i == segs ? 0.35 : 1);
        double k = j / 2.0, curl = 0.35 * w * k * k;
        V3 pos = { p.x + dir.x * w * k - N.x * curl, p.y + dir.y * w * k - N.y * curl, p.z + dir.z * w * k };
        double v = (arc[i] * (1 - t0) + F->L * t0) / LEAF_TILE;
        mv(&m, pos, F->region + 0.02 + 0.96 * k, v, (Color){});
      }
    mgrid(&m, base, segs + 1, 3);
  }
  // rachis / petiole: thin triangular prism, thicker at the base
  int base = mbase(&m);
  for (int i = 0; i <= segs; i++)
    for (int j = 0; j < 4; j++) {
      double t = (double)i / segs;
      V3 p, N;
      frond_frame(F, t, &p, &N);
      double r = 0.045 * (1 - 0.75 * t) + 0.008;
      double a = (j / 3.0) * PI_D * 2;
      mv(&m, v3(p.x + N.x * cos(a) * r, p.y + N.y * cos(a) * r, p.z + sin(a) * r), 3.5, t * 4, (Color){});
    }
  mgrid(&m, base, segs + 1, 4);
  return mgeometry(&m);
}

// Sabal costapalmate fan on a long petiole (local +x).
static Geometry *fan_frond(double seed) {
  Rng rng = rng_make(seed);
  Mesher m = {};
  const double pl = 1.3, R = 1.05, C0 = pl, C1 = pl * 0.35;
  int base = mbase(&m);
  for (int i = 0; i < 9; i++)
    for (int j = 0; j < 4; j++) {
      double t = i / 8.0, a = (j / 3.0) * PI_D * 2, r = 0.03 * (1 - 0.4 * t);
      mv(&m, v3(pl * t, pl * 0.35 * t * t + cos(a) * r, sin(a) * r), 3.5, t * 3, (Color){});
    }
  mgrid(&m, base, 9, 4);
  const int na = 24;
  base = mbase(&m);
  for (int i = 0; i < 4; i++)
    for (int j = 0; j <= na; j++) {
      double r = (i / 3.0) * R, th = -2.1 + ((double)j / na) * 4.2;
      double pleat = (j % 2 ? 1 : -1) * 0.05 * (r / R);
      double x = C0 + cos(th) * r * 0.55 + r * 0.45, z = sin(th) * r;
      double rr = r / R;
      double y = C1 + pleat + 0.25 * r * cos(th * 0.5) - 0.45 * (rr * rr) * (0.6 + 0.4 * fabs(sin(th))) + (rng_next(&rng) - 0.5) * 0.02;
      mv(&m, v3(x, y, z), 2.02 + 0.96 * (i / 3.0), (double)j / na, (Color){});
    }
  mgrid(&m, base, 4, na + 1);
  return mgeometry(&m);
}

// ---- tree grates (hotel-side planting islands) and sand / mulch circles (park) -----------------
static Node *ground_mesh(const PalmTree *trees, int nt) {
  Canvas *c = canvas_new(512, 256);
  Rng rng = rng_make(44);
  Rng *rnd = &rng;
#define R() rng_next(rnd)
  // square cut-out: mulch with low green plants, a galvanised grate with bold bars over it
  cv_fill_color(c, "#6b5238");
  cv_fill_rect(c, 0, 0, 256, 256);
  static const char *const MULCH[4] = { "#8a6a44", "#4f3b28", "#a07c50", "#5d4631" };
  for (int i = 0; i < 2500; i++) {
    cv_fill_color(c, MULCH[i & 3]);
    double x = R() * 256, y = R() * 256, w = 2 + R() * 5, h = 1 + R() * 2;
    cv_fill_rect(c, x, y, w, h);
  }
  static const char *const GREEN[3] = { "#5f7f36", "#7c9a44", "#4e6b2c" };
  for (int i = 0; i < 40; i++) {
    double x = 20 + R() * 216, y = 20 + R() * 216;
    if (js_hypot2(x - 128, y - 128) < 70) continue;
    cv_fill_color(c, GREEN[i % 3]);
    cv_begin_path(c);
    cv_arc(c, x, y, 5 + R() * 9, 0, PI_D * 2, false);
    cv_fill(c);
  }
  // dirty stain ring on the paving around the pit
  Gradient st = cv_radial_gradient(c, 128, 128, 60, 128, 128, 128);
  grad_add_stop(&st, 0, "rgba(0,0,0,0)");
  grad_add_stop(&st, 1, "rgba(50,40,30,0.35)");
  cv_fill_gradient(c, &st);
  cv_fill_rect(c, 0, 0, 256, 256);
  cv_stroke_color(c, "#4f4c46");
  cv_line_cap(c, CAP_BUTT);
  cv_line_width(c, 10);
  cv_stroke_rect(c, 10, 10, 236, 236);
  cv_line_width(c, 6);
  for (int x = 30; x < 236; x += 22) {
    cv_begin_path(c);
    if (abs(x - 128) < 40) {
      cv_move_to(c, x, 12); cv_line_to(c, x, 88); cv_move_to(c, x, 168); cv_line_to(c, x, 244);
    } else {
      cv_move_to(c, x, 12); cv_line_to(c, x, 244);
    }
    cv_stroke(c);
  }
  cv_stroke_color(c, "rgba(40,36,32,0.5)");
  cv_line_width(c, 2);
  for (int x = 33; x < 236; x += 22) { cv_begin_path(c); cv_move_to(c, x, 12); cv_line_to(c, x, 244); cv_stroke(c); }
  cv_stroke_color(c, "#4f4c46");
  cv_line_width(c, 8);
  cv_begin_path(c);
  cv_arc(c, 128, 128, 44, 0, PI_D * 2, false);
  cv_stroke(c);
  // bare trampled sand / dirt patch with a ragged edge into the lawn
  cv_save(c);
  cv_begin_path(c);
  cv_rect(c, 256, 0, 256, 256);
  cv_clip(c);
  for (int i = 0; i < 70; i++) {
    double a = R() * 6.28, d = R() * 70;
    double gx = 384 + cos(a) * d, gy = 128 + sin(a) * d;
    Gradient g = cv_radial_gradient(c, gx, gy, 4, gx, gy, 40 + R() * 30);
    grad_add_stop(&g, 0, "rgba(196,176,140,0.55)");
    grad_add_stop(&g, 1, "rgba(196,176,140,0)");
    cv_fill_gradient(c, &g);
    cv_fill_rect(c, 256, 0, 256, 256);
  }
  for (int i = 0; i < 1500; i++) {
    double a = R() * 6.28, d = sqrt(R()) * 100;
    cv_fill_color(c, R() < 0.5 ? "rgba(150,128,96,0.5)" : "rgba(220,204,170,0.5)");
    cv_fill_rect(c, 384 + cos(a) * d, 128 + sin(a) * d, 2, 2);
  }
  for (int i = 0; i < 26; i++) {
    double a = R() * 6.28, d = 20 + R() * 80, x = 384 + cos(a) * d, y = 128 + sin(a) * d, b = R() * 6.28, l = 14 + R() * 26;
    cv_stroke_color(c, R() < 0.5 ? "rgba(120,96,58,0.9)" : "rgba(150,132,90,0.9)");
    cv_line_width(c, 3);
    cv_begin_path(c);
    cv_move_to(c, x, y);
    cv_line_to(c, x + cos(b) * l, y + sin(b) * l);
    cv_stroke(c);
  }
  cv_restore(c);
#undef R
  MatDesc md = md_standard();
  md.name = "palm ground";
  md.map = canvas_texture(c, true, WRAP_CLAMP, 8);
  canvas_free(c);
  md.transparent = true;
  md.depth_write = false;
  md.roughness = 0.95;
  md.polygon_offset = true;
  md.po_factor = -2;
  md.po_units = -2;
  md.prog[MV_INSTANCED] = PROG_PALM_GROUND;
  Material *mat = mat_three(&md);
  Geometry *geo = geo_rotate_x(geo_plane(1, 1, 1, 1), -PI_D / 2);
  float *kind = xmalloc(sizeof(float) * (size_t)nt);
  for (int i = 0; i < nt; i++) kind[i] = (float)trees[i].ground;
  geo_set_iattr(geo, "aKind", 1, nt, kind);
  free(kind);
  Node *im = node_instanced(gpu_geometry(geo), mat, nt);
  geo_free(geo);
  snprintf(im->name, sizeof im->name, "palm ground");
  for (int i = 0; i < nt; i++) {
    const PalmTree *t = &trees[i];
    double size = t->ground ? 2.4 + fmod(t->seed, 7) * 0.15 : 1.4;
    Quat q = quat_axis_angle(v3(0, 1, 0), t->ground ? fmod(t->seed, 6) : 0);
    inst_set_matrix(im, i, m4_compose(v3(t->x, CURB_HEIGHT + 0.004, t->z), q, v3(size, 1, size)));
  }
  im->receive_shadow = true;
  im->render_order = 1;
  return im;
}

// ---- palms -------------------------------------------------------------------------------------
typedef struct FrondInst { int g, id, tree; M4 m; Color c; bool keep; } FrondInst;
typedef struct TrunkInst { int g; M4 m; Color c; } TrunkInst;

struct Palms {
  Node *group, *trunks, *fronds;
  Material *trunk_mat, *frond_mat;
  Vec(FrondInst) fronds_inst;
  int *tree_first, *tree_count;   // each tree's fronds (contiguous in fronds_inst)
  uint8_t *far;
  int nt;
  int frond_low;
};

// crown LOD: beyond LOD.palmNear the coarse fronds, and fewer of them
static bool palms_lod(V3 p, void *user) {
  Palms *P = user;
  int nt;
  const PalmTree *trees = palm_trees(&nt);
  double near = lod_radius().palmNear;
  bool changed = false;
  for (int i = 0; i < nt; i++) {
    uint8_t isFar = js_hypot2(trees[i].x - p.x, trees[i].z - p.z) > near ? 1 : 0;
    if (isFar == P->far[i]) continue;
    P->far[i] = isFar;
    changed = true;
    for (int k = P->tree_first[i]; k < P->tree_first[i] + P->tree_count[i]; k++) {
      const FrondInst *f = &P->fronds_inst.data[k];
      if (f->g < P->frond_low - 1) batch_set_geometry(P->fronds, f->id, isFar ? P->frond_low + f->g : f->g);
      batch_set_visible(P->fronds, f->id, !isFar || f->keep);
    }
  }
  return changed;
}

typedef struct FrondCtx {
  Palms *P;
  const PalmTree *t;
  int treeI;
  double thin, sc;
  Rng rThin;
  V3 top;
} FrondCtx;

static void add_frond(FrondCtx *F, int g, double az, double pitch, double s, Color c, double roll) {
  if (F->thin < 1 && rng_next(&F->rThin) > F->thin) return;
  Quat qa = quat_axis_angle(v3(0, 1, 0), F->t->rotY + az);
  Quat qb = quat_axis_angle(v3(0, 0, 1), pitch);
  Quat qr = quat_axis_angle(v3(1, 0, 0), roll);
  Quat qq = quat_mul(quat_mul(qa, qb), qr);
  // far crowns keep ~60% of their fronds (hashed, no random draws), always the first
  int n = (int)F->P->fronds_inst.len;
  bool first = !n || F->P->fronds_inst.data[n - 1].tree != F->treeI;
  bool keep = first || ((uint32_t)((uint32_t)(n + 1) * 2654435761u) % 10) < 6;
  double ss = s * F->sc;
  vec_push(&F->P->fronds_inst, ((FrondInst){ g, -1, F->treeI, m4_compose(F->top, qq, v3(ss, ss, ss)), c, keep }));
}

static Node *batched(Geometry **geos, int ngeo, const int *gs, const M4 *ms, const Color *cs, int n, Material *mat, int *ids,
                     const char *name) {
  Node *bm = node_batched(mat, geos, ngeo, n);
  snprintf(bm->name, sizeof bm->name, "%s", name);
  for (int i = 0; i < n; i++) {
    int id = batch_add_instance(bm, gs[i]);
    if (ids) ids[i] = id;
    batch_set_matrix(bm, id, ms[i]);
    batch_set_color(bm, id, cs[i]);
  }
  bm->cast_shadow = true;
  bm->receive_shadow = true;
  return bm;
}

Palms *build_palms(Node *scene) {
  Palms *P = xcalloc(1, sizeof *P);
  P->group = node_new(NODE_GROUP, "palms");
  int nt;
  const PalmTree *trees = palm_trees(&nt);
  // trunk variants
  static const TrunkDef TRUNK_DEFS[8] = {
    { SP_COCONUT, 5.8, 1.1, 1.0, 1, 0.3 },    // young, curved
    { SP_COCONUT, 10, 1.6, 0.8, 2, 0.35 },
    { SP_COCONUT, 11, 3.4, 1.0, 3, 0.2 },
    { SP_COCONUT, 14, 1.8, 0.6, 4, 1.1 },     // S-curve
    { SP_COCONUT, 19, 2.4, 0.5, 7, 1.4 },     // towers over the roofs
    { SP_COCONUT, 12, 0.8, 0.3, 8, 0.45 },    // near straight
    { SP_ROYAL, 14, 0.15, 0, 5, 0 },
    { SP_SABAL, 7, 0.35, 0, 6, 0 },
  };
  Trunk trunks[8];
  for (int i = 0; i < 8; i++) trunks[i] = trunk_geometry(&TRUNK_DEFS[i]);
  Geometry *nutGeo = coconut_geometry();

  // frond variants
  static const FrondDef FROND_DEFS[6] = {
    // arc out, then the tip drops well below the midrib; leaflets hang from the rachis
    { 5.0, 0.42, 0.8, 0.95, 0.45, 0.9, 0, 11, 0.5, 14 },
    { 4.6, 0.48, 0.95, 0.9, 0.5, 1.0, 0, 12, -0.6, 14 },
    { 5.2, 0.35, 0.75, 1.0, 0.45, 0.9, 1, 13, 0.8, 14 },
    { 4.4, 0.5, 1.15, 0.85, 0.55, 1.05, 0, 14, -0.4, 14 },
    { 4.0, 0.05, 0.35, 0.75, 1.25, 1.45, 1, 15, 0, 14 },    // dead, hanging
    { 3.8, 0.55, 1.1, 0.85, 0.35, 0.8, 0, 16, 0, 14 },      // royal: flatter, plumose
  };
  Geometry *frondGeos[13];
  for (int i = 0; i < 6; i++) frondGeos[i] = pinnate_frond(&FROND_DEFS[i]);
  frondGeos[6] = fan_frond(17);
  // distant crowns: the same fronds with a coarse rachis (FROND_LOW + g)
  const int FROND_LOW = 7;
  P->frond_low = FROND_LOW;
  for (int i = 0; i < 6; i++) {
    FrondDef d = FROND_DEFS[i];
    d.segs = 5;
    frondGeos[FROND_LOW + i] = pinnate_frond(&d);
  }
  Geometry *trunkTris[9];
  for (int i = 0; i < 8; i++) trunkTris[i] = trunks[i].geo;
  trunkTris[8] = nutGeo;

  // instance lists
  Vec(TrunkInst) trunkInst = {};
  P->tree_first = xcalloc((size_t)nt, sizeof(int));
  P->tree_count = xcalloc((size_t)nt, sizeof(int));
  const V3 Y = { 0, 1, 0 };
  for (int treeI = 0; treeI < nt; treeI++) {
    const PalmTree *t = &trees[treeI];
    Rng rng = rng_make(t->seed);
    Rng *rnd = &rng;
#define R() rng_next(rnd)
    int ti = t->species == SP_ROYAL ? 6 : t->species == SP_SABAL ? 7 : t->variant;
    V3 base = { t->x, CURB_HEIGHT, t->z };
    Quat q = quat_axis_angle(Y, t->rotY);
    double sc = t->scale * (t->species == SP_SABAL ? 0.95 : 1);
    // per-tree height stretch (capped ~21 m) and lean shear
    double hy = fmax(0.8, fmin(t->hs, 21 / (TRUNK_DEFS[ti].H * sc)));
    M4 shear = m4_identity();
    shear.e[4] = t->k;   // Matrix4.set(1, k, 0, 0, ...): row 0, column 1
    M4 tm = m4_mul(m4_mul(m4_compose(base, q, v3(1, 1, 1)), shear), m4_scaling(sc, sc * hy, sc));
    double gs = 0.94 + R() * 0.12;
    vec_push(&trunkInst, ((TrunkInst){ ti, tm, { gs, gs, gs } }));
    FrondCtx F = { P, t, treeI, fabs(t->z) > 100 ? QUALITY.farFoliage : 1, sc, rng_make(t->seed + 7919), v3_apply_m4(trunks[ti].top, tm) };
    // lower tiers thin the crowns of palms beyond the walkable block (separate random stream,
    // so the kept fronds are exactly the 'high' ones)
    double bleach = R();
    Color fresh = color_scale(color_lerp(color_rgb(0.1, 0.2, 0.038), color_rgb(0.2, 0.26, 0.05), bleach), 0.85 + 0.3 * R());   // mid-deep olive
    P->tree_first[treeI] = (int)P->fronds_inst.len;
    if (t->species == SP_SABAL) {
      int n = 16 + (int)floor(R() * 8);
      for (int i = 0; i < n; i++) {
        double f = (double)i / (n - 1);
        double az = i * 2.3999 + R() * 0.3, pitch = 0.9 - 1.4 * f + (R() - 0.5) * 0.2, s = 0.85 + R() * 0.25;
        Color c = color_scale(fresh, 0.8 + 0.3 * R());
        double roll = (R() - 0.5) * 0.6;
        add_frond(&F, 6, az, pitch, s, c, roll);
      }
      for (int i = 0; i < 3; i++) {
        double az = R() * 6.28, pitch = -1.3 - R() * 0.3, roll = R();
        add_frond(&F, 6, az, pitch, 0.8, color_rgb(0.3, 0.2, 0.09), roll);
      }
    } else if (t->species == SP_ROYAL) {
      int n = 12 + (int)floor(R() * 4);
      for (int i = 0; i < n; i++) {
        double f = (double)i / (n - 1);
        double az = i * 2.3999 + R() * 0.3, pitch = 1.0 - 1.35 * f + (R() - 0.5) * 0.15, s = 1.2 + R() * 0.3;
        Color c = color_lerp(color_scale(fresh, 1.05), color_rgb(0.2, 0.38, 0.06), 0.4);
        double roll = (R() - 0.5) * 0.5;
        add_frond(&F, 5, az, pitch, s, c, roll);
      }
    } else {
      // full ball: spear + young fronds up top, middle fronds horizontal, only the lower ones
      // arch out and droop; some missing / broken, lopsided per tree
      bool young = ti == 0;
      int n = (young ? 16 : 22) + (int)floor(R() * 11);
      double droopBias = (R() - 0.5) * 0.3, spin = R() * 6.28, fsc = (young ? 0.8 : 1.12) * (0.9 + R() * 0.2);
      double lop = R() * 0.35, lopAz = R() * 6.28;
      add_frond(&F, 3, spin, 1.5, 0.45 * fsc, color_scale(fresh, 1.2), 0);
      for (int i = 0; i < n; i++) {
        double f = (double)i / (n - 1);
        if (f > 0.2 && R() < 0.08) continue;
        int g = (int)floor(R() * 4);
        double az = spin + i * 2.3999 + (R() - 0.5) * 0.45;
        double pitch = 1.3 - 2.0 * pow(f, 0.9) + droopBias - lop * cos(az - lopAz) + (R() - 0.5) * 0.25;
        double s = ((f < 0.12 ? 0.55 + f * 3.5 : 0.97) + R() * 0.12) * fsc;
        s *= R() < 0.05 ? 0.6 : 1;
        // older (lower) fronds yellower
        Color c = color_scale(color_lerp(fresh, color_rgb(0.3, 0.28, 0.08), f * f * 0.6 * (0.4 + bleach)), 0.85 + 0.3 * R());
        double roll = (R() - 0.5) * 0.8;
        add_frond(&F, g, az, pitch, s, c, roll);
      }
      // brown / tan dead fronds hanging under most crowns
      int dead;
      if (young) dead = (int)floor(R() * 2);
      else dead = R() < 0.15 ? 1 : 2 + (int)floor(R() * 4);
      for (int i = 0; i < dead; i++) {
        double az = R() * 6.28, pitch = -1.52 - R() * 0.08, s = (0.8 + R() * 0.15) * fsc;
        Color c = color_scale(color_rgb(0.24, 0.21, 0.15), 0.8 + 0.4 * R());   // dull dry grey-tan, limp against the trunk
        double roll = (R() - 0.5) * 0.8;
        add_frond(&F, 4, az, pitch, s, c, roll);
      }
      if (!young && R() < 0.85) {
        Quat qa = quat_axis_angle(Y, R() * 6.28);
        vec_push(&trunkInst, ((TrunkInst){ 8, m4_compose(v3_add(F.top, v3(0, -0.05, 0)), qa, v3(sc, sc, sc)), { 1, 1, 1 } }));
      }
    }
    P->tree_count[treeI] = (int)P->fronds_inst.len - P->tree_first[treeI];
#undef R
  }

  // trunk material
  Texture *tmap, *tnormal;
  trunk_textures(&tmap, &tnormal);
  MatDesc td = md_standard();
  td.name = "palm trunk";
  td.map = tmap;
  td.normal_map = tnormal;
  td.normal_scale = v2(1.7, 1.7);
  td.vertex_colors = true;
  td.roughness = 0.9;
  td.prog[MV_BATCHED] = PROG_PALM_TRUNK;
  P->trunk_mat = mat_three(&td);
  {
    int n = (int)trunkInst.len;
    int *gs = xcalloc((size_t)n, sizeof(int));
    M4 *ms = xcalloc((size_t)n, sizeof(M4));
    Color *cs = xcalloc((size_t)n, sizeof(Color));
    for (int i = 0; i < n; i++) { gs[i] = trunkInst.data[i].g; ms[i] = trunkInst.data[i].m; cs[i] = trunkInst.data[i].c; }
    P->trunks = batched(trunkTris, 9, gs, ms, cs, n, P->trunk_mat, nullptr, "palm trunks");
    free(gs); free(ms); free(cs);
  }
  Texture *atlas = frond_atlas();
  MatDesc fd = md_standard();
  fd.name = "palm fronds";
  fd.map = atlas;
  fd.roughness = 0.52;
  fd.side = SIDE_DOUBLE;
  fd.alpha_test = 0.5;
  fd.alpha_to_coverage = true;
  fd.prog[MV_BATCHED] = PROG_PALM_FROND;
  P->frond_mat = mat_three(&fd);
  MatDesc dd = md_depth();
  dd.name = "palm frond depth";
  dd.map = atlas;
  dd.alpha_test = 0.5;
  dd.side = SIDE_DOUBLE;
  dd.prog[MV_BATCHED] = PROG_PALM_FROND_DEPTH;
  Material *depth = mat_three(&dd);
  {
    int n = (int)P->fronds_inst.len;
    int *gs = xcalloc((size_t)n, sizeof(int)), *ids = xcalloc((size_t)n, sizeof(int));
    M4 *ms = xcalloc((size_t)n, sizeof(M4));
    Color *cs = xcalloc((size_t)n, sizeof(Color));
    for (int i = 0; i < n; i++) { gs[i] = P->fronds_inst.data[i].g; ms[i] = P->fronds_inst.data[i].m; cs[i] = P->fronds_inst.data[i].c; }
    P->fronds = batched(frondGeos, 13, gs, ms, cs, n, P->frond_mat, ids, "palm fronds");
    for (int i = 0; i < n; i++) P->fronds_inst.data[i].id = ids[i];
    free(gs); free(ids); free(ms); free(cs);
  }
  P->fronds->custom_depth = depth;
  P->far = xcalloc((size_t)nt, 1);
  P->nt = nt;
  lod_register_hook(palms_lod, P);
  // (crowns only grazed by a 7 deg sun; their own shadow on each other only blackened them)
  P->fronds->receive_shadow = false;
  node_add(P->group, P->trunks);
  node_add(P->group, P->fronds);
  node_add(P->group, ground_mesh(trees, nt));
  node_add(scene, P->group);
  LOG("palms: %d trees, %d fronds, %d trunks", nt, (int)P->fronds_inst.len, (int)trunkInst.len);
  for (int i = 0; i < 9; i++) geo_free(trunkTris[i]);
  for (int i = 0; i < 13; i++) geo_free(frondGeos[i]);
  vec_free(&trunkInst);
  return P;
}

void palms_update(Palms *p, double time) {
  mat_set_float(p->frond_mat, "odWindT", time);
  mat_set_float(p->trunk_mat, "odWindT", time);
}
