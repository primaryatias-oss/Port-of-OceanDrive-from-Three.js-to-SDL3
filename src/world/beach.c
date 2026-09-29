// Port of src/world/beach.js (see beach.h).
#include "world/beach.h"

#include "canvas/canvas.h"
#include "gfx/three_mat.h"
#include "quality.h"
#include "textures/noise.h"
#include "world/layout.h"

#define TAU (PI_D * 2)
static constexpr double DETAIL_TILE = 6;   // m covered by one tile of the micro-relief texture
const double ACCESS_Z[2] = { -30, 32 };
const double MORE_ACCESS_Z[6] = { -322, -245, -134, 134, 245, 322 };
static constexpr double ACCESS_HALF = 1.2;
typedef struct { double x0, x1, top; } WallDef;
static const WallDef WALL = { PARK.wallX - 0.3, PARK.wallX + 0.4, 0.68 };
static const double STEPS[3][3] = { { 10.8, 11.1, 0.33 }, { 11.1, 11.4, 0.5 }, { 11.4, 12.55, 0.7 } };   // x0, x1, top

static bool near_list(const double *l, int n, double z, double pad) {
  for (int i = 0; i < n; i++) if (fabs(z - l[i]) < ACCESS_HALF + pad) return true;
  return false;
}
static bool near_access(double z, double pad) { return near_list(ACCESS_Z, 2, z, pad) || near_list(MORE_ACCESS_Z, 6, z, pad); }
static bool near_orig_access(double z, double pad) { return near_list(ACCESS_Z, 2, z, pad); }
static bool near_more_access(double z, double pad) { return near_list(MORE_ACCESS_Z, 6, z, pad); }

typedef Vec(BeachBox) BoxVec;
typedef Vec(Geometry *) GeoList;
static void gl_push(GeoList *l, Geometry *g) { vec_push(l, g); }

// ---- churned-sand bake: R,G = slope (dh/dx, dh/dz), B = lit fraction under the fixed sunrise sun
// (self-shadowing), A = albedo variation. Footprints are procedural. -------------------------------
static float *periodic_noise(int N, int cells, Rng *rnd) {
  float *g = xmalloc(sizeof(float) * (size_t)cells * cells);
  for (int i = 0; i < cells * cells; i++) g[i] = (float)rng_next(rnd);
  float *out = xmalloc(sizeof(float) * (size_t)N * N);
  for (int j = 0; j < N; j++) {
    double fy = ((double)j / N) * cells, y0 = floor(fy), ty = fy - y0, sy = ty * ty * (3 - 2 * ty);
    int r0 = (int)y0 % cells, r1 = ((int)y0 + 1) % cells;
    for (int i = 0; i < N; i++) {
      double fx = ((double)i / N) * cells, x0 = floor(fx), tx = fx - x0, sx = tx * tx * (3 - 2 * tx);
      int c0 = (int)x0 % cells, c1 = ((int)x0 + 1) % cells;
      double a = g[r0 * cells + c0] + ((double)g[r0 * cells + c1] - g[r0 * cells + c0]) * sx;
      double b = g[r1 * cells + c0] + ((double)g[r1 * cells + c1] - g[r1 * cells + c0]) * sx;
      out[j * N + i] = (float)(a + (b - a) * sy);
    }
  }
  free(g);
  return out;
}
static double smooth(double a, double b, double v) {
  double t = fmin(1, fmax(0, (v - a) / (b - a)));
  return t * t * (3 - 2 * t);
}

static Texture *bake_sand_detail(int N) {
  Rng rng = rng_make(5150);
  Rng *rnd = &rng;
  double texel = DETAIL_TILE / N;
  float *H = xcalloc((size_t)N * N, sizeof(float));
  // churned sand: several octaves of tileable noise
  static const double OCT[4][2] = { { 6, 0.014 }, { 14, 0.007 }, { 32, 0.0035 }, { 80, 0.0015 } };
  for (int k = 0; k < 4; k++) {
    float *n = periodic_noise(N, (int)OCT[k][0], rnd);
    for (int i = 0; i < N * N; i++) H[i] = (float)(H[i] + ((double)n[i] - 0.5) * 2 * OCT[k][1]);
    free(n);
  }
  // slopes
  uint8_t *data = xcalloc((size_t)N * N, 4);
#define AT(i, j) ((double)H[((((j) % N) + N) % N) * N + ((((i) % N) + N) % N)])
  for (int j = 0; j < N; j++)
    for (int i = 0; i < N; i++) {
      double sx = (AT(i + 1, j) - AT(i - 1, j)) / (2 * texel);
      double sz = (AT(i, j + 1) - AT(i, j - 1)) / (2 * texel);
      size_t o = ((size_t)j * N + i) * 4;
      data[o] = (uint8_t)js_round(128 + fmax(-1, fmin(1, sx)) * 127);
      data[o + 1] = (uint8_t)js_round(128 + fmax(-1, fmin(1, sz)) * 127);
    }
  // self-shadowing under the fixed low sun
  V3 L = compassToDir(SUN.azimuthDeg, SUN.elevationDeg);
  double lx = L.x / js_hypot2(L.x, L.z), lz = L.z / js_hypot2(L.x, L.z);
  double rise = tan((SUN.elevationDeg * PI_D) / 180) * texel * 2;
  for (int j = 0; j < N; j++)
    for (int i = 0; i < N; i++) {
      double h0 = H[j * N + i];
      double occ = 0;
      for (int s = 1; s <= 30; s++) {
        double hh = AT((int)js_round(i + lx * s * 2), (int)js_round(j + lz * s * 2));
        double d = hh - (h0 + rise * s + 0.0004);
        if (d > occ) occ = d;
      }
      data[((size_t)j * N + i) * 4 + 2] = (uint8_t)js_round(255 * (1 - smooth(0, 0.004, occ)));
    }
#undef AT
  // albedo: soft mottling, dark mineral grains, pale shell bits
  float *mott = periodic_noise(N, 24, rnd);
  for (int i = 0; i < N * N; i++) data[(size_t)i * 4 + 3] = (uint8_t)js_round(255 * (0.5 + ((double)mott[i] - 0.5) * 0.35));
  free(mott);
  for (int k = 0; k < 5000; k++) {
    size_t idx = (size_t)floor(rng_next(rnd) * N * N) * 4 + 3;
    data[idx] = js_u8(60 + rng_next(rnd) * 50);
  }
  for (int k = 0; k < 220; k++) {
    int ci = (int)floor(rng_next(rnd) * N), cj = (int)floor(rng_next(rnd) * N);
    double r = 1 + rng_next(rnd) * 2.2;
    for (int dj = -3; dj <= 3; dj++)
      for (int di = -3; di <= 3; di++) {
        if (di * di + dj * dj > r * r) continue;
        data[((size_t)((((cj + dj) % N) + N) % N) * N + (((ci + di) % N) + N) % N) * 4 + 3] = 240;
      }
  }
  free(H);
  SamplerDesc sd = sampler_default();
  sd.wrap_s = sd.wrap_t = WRAP_REPEAT;
  sd.anisotropy = 8;
  TexUpload up = { .rgba = data, .w = N, .h = N, .srgb = false, .flip_y = false, .mipmaps = true, .sampler = sd };
  Texture *t = tex_create_rgba8(&up);
  free(data);
  return t;
}

// ---- sand surface ------------------------------------------------------------------------------
static Geometry *sand_geometry(double zA, double zB, int rows, bool detail) {
  double xs[512];
  int nx = 0;
  for (double x = SAND.x0; x < 100; x += 0.5) xs[nx++] = x;
  for (double x = 100; x <= 132; x += 2) xs[nx++] = x;
  int nz = rows + 1;
  float *pos = xmalloc(sizeof(float) * (size_t)nx * nz * 3);
  size_t o = 0;
  for (int j = 0; j < nz; j++) {
    double z = zA + ((zB - zA) * j) / rows;
    for (int i = 0; i < nx; i++) {
      double x = xs[i];
      pos[o++] = (float)x;
      pos[o++] = (float)(sandHeight(x) + (detail ? sandDetail(x, z) : 0));
      pos[o++] = (float)z;
    }
  }
  U32Vec idx = {};
  for (int j = 0; j < rows; j++)
    for (int i = 0; i < nx - 1; i++) {
      uint32_t a = (uint32_t)(j * nx + i), b = a + 1, c = a + (uint32_t)nx, d = c + 1;
      uint32_t q[6] = { a, c, b, b, c, d };
      vec_append(&idx, q, 6);
    }
  Geometry *g = geo_new();
  geo_set_attr_copy(g, "position", 3, nx * nz, pos);
  free(pos);
  geo_set_index(g, idx.data, (int)idx.len);
  vec_free(&idx);
  return geo_compute_vertex_normals(g);
}

static Material *sand_material(Texture *detail) {
  MatDesc d = md_standard();
  d.name = "beach sand";
  d.color = color_hex(0xe6d8c2);
  d.roughness = 0.95;
  d.prog[MV_PLAIN] = PROG_BEACH_SAND;
  Material *m = mat_three(&d);
  mat_set_texture(m, "uDetail", detail);
  return m;
}

// swash sheet: thin water with lace foam sliding up the sand and back
static Node *swash_sheet(Material **out_mat) {
  const double x0 = SHORE_X - 5.8, x1 = SHORE_X + 1.8, zl = 130;
  const int nx = 90, nz = 130;
  DVec pos = {};
  for (int j = 0; j <= nz; j++)
    for (int i = 0; i <= nx; i++) {
      double x = x0 + ((x1 - x0) * i) / nx, z = -zl + (2 * zl * j) / nz;
      double p[3] = { x, fmax(sandHeight(x), SEA_LEVEL - 0.05) + 0.012, z };
      vec_append(&pos, p, 3);
    }
  U32Vec idx = {};
  for (int j = 0; j < nz; j++)
    for (int i = 0; i < nx; i++) {
      uint32_t a = (uint32_t)(j * (nx + 1) + i), b = a + 1, c = a + (uint32_t)nx + 1, d = c + 1;
      uint32_t q[6] = { a, c, b, b, c, d };
      vec_append(&idx, q, 6);
    }
  Geometry *g = geo_new();
  geo_set_attr_d(g, "position", 3, (int)(pos.len / 3), pos.data);
  geo_set_index(g, idx.data, (int)idx.len);
  vec_free(&pos);
  vec_free(&idx);
  MatDesc d = md_shader();
  d.name = "Swash";
  d.fog = true;
  d.transparent = true;
  d.depth_write = false;
  d.polygon_offset = true;
  d.po_factor = -2;
  d.po_units = -2;
  d.prog[MV_PLAIN] = PROG_SWASH;
  Material *mat = mat_three(&d);
  *out_mat = mat;
  Node *m = node_mesh(gpu_geometry(g), mat);
  geo_free(g);
  snprintf(m->name, sizeof m->name, "swash");
  m->frustum_culled = false;
  m->render_order = 2;
  return m;
}

// ---- geometry helpers (vertex-coloured, merged) ------------------------------------------------------
static Geometry *colorize(Geometry *g, uint32_t hex) {
  if (g->index) {
    Geometry *n = geo_to_non_indexed(g);
    geo_free(g);
    g = n;
  }
  if (geo_attr(g, "uv")) geo_delete_attr(g, "uv");
  Color c = color_hex(hex);
  float *a = geo_set_attr(g, "color", 3, g->count);
  for (int i = 0; i < g->count; i++) { a[i * 3] = (float)c.r; a[i * 3 + 1] = (float)c.g; a[i * 3 + 2] = (float)c.b; }
  return g;
}
static Geometry *box_at(double x0, double x1, double y0, double y1, double z0, double z1, uint32_t hex) {
  return colorize(geo_translate(geo_box1(x1 - x0, y1 - y0, z1 - z0), (x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2), hex);
}
// a beam between two points (square section)
static Geometry *beam(V3 A, V3 B, double w, uint32_t hex) {
  double len = v3_dist(A, B);
  Geometry *g = geo_box1(w, len, w);
  geo_apply_m4(g, m4_from_quat(quat_unit_vectors(v3(0, 1, 0), v3_norm(v3_sub(B, A)))));
  geo_translate(g, (A.x + B.x) / 2, (A.y + B.y) / 2, (A.z + B.z) / 2);
  return colorize(g, hex);
}
// round rail between two points
static Geometry *tube_rail(V3 A, V3 B, double r, uint32_t hex) {
  Geometry *g = geo_cyl(r, r, v3_dist(A, B), 10);
  geo_apply_m4(g, m4_from_quat(quat_unit_vectors(v3(0, 1, 0), v3_norm(v3_sub(B, A)))));
  geo_translate(g, (A.x + B.x) / 2, (A.y + B.y) / 2, (A.z + B.z) / 2);
  return colorize(g, hex);
}
static Geometry *prep(Geometry *g) {
  if (g->index) {
    Geometry *n = geo_to_non_indexed(g);
    geo_free(g);
    g = n;
  }
  if (geo_attr(g, "uv")) geo_delete_attr(g, "uv");
  if (!geo_attr(g, "normal")) geo_compute_vertex_normals(g);
  return g;
}
static Geometry *merge_list(GeoList *l, bool prepped) {
  if (prepped) for (size_t i = 0; i < l->len; i++) l->data[i] = prep(l->data[i]);
  Geometry *m = geo_merge_free(l->data, (int)l->len);
  vec_free(l);
  return m;
}

// Painted plywood / fibreglass: board seams, salt-bleached chips showing grey wood, grime low down.
static Material *painted_material(int wear) {
  MatDesc d = md_standard();
  d.name = wear ? "beach painted" : "beach painted (fresh)";
  d.vertex_colors = true;
  d.roughness = wear ? 0.72 : 0.55;
  d.prog[MV_PLAIN] = wear ? PROG_BEACH_PAINTED_V2_1 : PROG_BEACH_PAINTED_V2_0;
  return mat_three(&d);
}

static Node *add_mesh(Node *parent, Geometry *g, Material *m, bool cast, bool receive) {
  Node *n = node_mesh(gpu_geometry(g), m);
  geo_free(g);
  n->cast_shadow = cast;
  n->receive_shadow = receive;
  snprintf(n->name, sizeof n->name, "beach");
  node_add(parent, n);
  return n;
}

// ---- lifeguard tower (local coords: origin on the sand at TOWER.x/z, +x = ocean) --------------------
typedef struct { double x0, x1, z0, z1, h; } TBox;
static const TBox T_DECK = { -2.0, 3.4, -2.0, 2.0, 0 };
static const TBox T_CABIN = { 0.7, 3.2, -1.25, 1.25, 2.35 };
static const TBox T_ROOF = { -1.5, 4.3, -2.55, 2.55, 0 };
typedef struct { double z0, z1, run; } TStair;
static const TStair T_STAIR = { -1.85, -0.75, 0.28 };

// colour schemes: the original pink / teal, a lime / violet one and an orange / blue one
typedef struct Palette { uint32_t PINK, TEAL, YEL, NAVY, ORANGE; double skirt; bool skirt3; } Palette;
static const Palette TOWER_PALETTES[3] = {
  [PALETTE_DEFAULT] = { 0xf0a0b4, 0x2fb5a8, 0xf6d24a, 0x2b3f8c, 0xf08a3c, 0.3, false },
  [PALETTE_LIME] = { 0xb9d84c, 0x7a55b4, 0xf3eed8, 0x3d2c72, 0xf4c23a, 0.26, true },
  [PALETTE_SUNSET] = { 0xf2893e, 0x2f7fcf, 0xf6de4e, 0xcf3a5c, 0x3aa96a, 0.55, false },
};

typedef struct TowerSurf {
  double base, deckY;
  struct { double x0, x1, z0, z1, run, rise; int n; } stair;
  struct { double x0, x1, z0, z1; } deck;
} TowerSurf;
typedef struct TowerOut { TowerSurf surfaces; Material *flag; } TowerOut;

typedef struct TB {   // buildTower's closures
  double bx, bz, base;
  BoxVec *colliders;
} TB;
static void tcol(TB *t, double x0, double x1, double y0, double y1, double z0, double z1) {
  vec_push(t->colliders, ((BeachBox){ { t->bx + x0, t->base + y0, t->bz + z0 }, { t->bx + x1, t->base + y1, t->bz + z1 } }));
}

typedef struct Foot { double p[64][2]; int n; } Foot;
static void foot_seg(Foot *f, double ax, double az, double bx2, double bz2, int n) {
  for (int i = 0; i < n; i++) { f->p[f->n][0] = ax + ((bx2 - ax) * i) / n; f->p[f->n][1] = az + ((bz2 - az) * i) / n; f->n++; }
}
static void foot_arc(Foot *f, double cx, double cz, double a0, double a1, int n, double rr) {
  for (int i = 0; i < n; i++) {
    double a = a0 + ((a1 - a0) * i) / n;
    f->p[f->n][0] = cx + cos(a) * rr;
    f->p[f->n][1] = cz + sin(a) * rr;
    f->n++;
  }
}

typedef enum BandKind { BAND_LOW, BAND_MID, BAND_TOP, BAND_SILL } BandKind;
static void wall_band(GeoList *parts, const Foot *foot, double y0, double y1, BandKind kind, const Palette *P, uint32_t WHITE) {
  const TBox cb = T_CABIN;
  const double rr = 0.9;
  for (int i = 0; i < foot->n; i++) {
    const double *a = foot->p[i], *b = foot->p[(i + 1) % foot->n];
    double cx = (a[0] + b[0]) / 2, cz = (a[1] + b[1]) / 2;
    bool eastWin = cx > cb.x1 - rr - 0.05;
    bool westW = cx < cb.x0 + 0.05;
    if (kind == BAND_MID && eastWin) continue;
    if (kind == BAND_SILL && !eastWin) continue;
    uint32_t col;
    switch (kind) {
    case BAND_LOW: col = westW ? P->ORANGE : P->PINK; break;
    case BAND_MID: col = westW ? (cz < 0 ? P->TEAL : P->PINK) : i % 2 ? WHITE : P->TEAL; break;
    case BAND_TOP: col = westW ? ((i / 2) % 2 ? WHITE : P->NAVY) : P->YEL; break;
    default: col = WHITE; break;
    }
    double p[18] = { a[0], y0, a[1], b[0], y0, b[1], b[0], y1, b[1], a[0], y0, a[1], b[0], y1, b[1], a[0], y1, a[1] };
    Geometry *g = geo_new();
    geo_set_attr_d(g, "position", 3, 6, p);
    geo_compute_vertex_normals(g);
    // outward normal check (footprint winding)
    double nx = b[1] - a[1], nz = -(b[0] - a[0]);
    double mx = (cb.x0 + cb.x1) / 2, mz = 0;
    if (nx * (cx - mx) + nz * (cz - mz) > 0) {
      float *q = geo_data(g, "position");
      for (int k = 0; k < 18; k += 9)
        for (int c = 0; c < 3; c++) { float tmp = q[k + 3 + c]; q[k + 3 + c] = q[k + 6 + c]; q[k + 6 + c] = tmp; }
      geo_compute_vertex_normals(g);
    }
    gl_push(parts, colorize(g, col));
  }
}

static Texture *soffit_texture(void) {
  Canvas *c = canvas_new(64, 512);
  Rng r2 = rng_make(88);
  for (int i = 0; i < 32; i++) {
    double v = 226 + floor((rng_next(&r2) - 0.5) * 12);
    cv_fill_rgba(c, v, v - 4, v - 12, 1);
    cv_fill_rect(c, 0, i * 16, 64, 16);
    cv_fill_color(c, "rgba(70,56,40,0.5)");
    cv_fill_rect(c, 0, i * 16 + 14, 64, 2);
    cv_fill_color(c, "rgba(255,255,255,0.25)");
    cv_fill_rect(c, 0, i * 16, 64, 1);
  }
  for (int k = 0; k < 40; k++) {
    cv_fill_rgba(c, 120, 100, 80, 0.08 + rng_next(&r2) * 0.1);
    double x = rng_next(&r2) * 64, y = rng_next(&r2) * 512, w = 2 + rng_next(&r2) * 10, h = 1 + rng_next(&r2) * 3;
    cv_fill_rect(c, x, y, w, h);
  }
  Texture *t = canvas_texture(c, true, WRAP_REPEAT, 8);
  canvas_free(c);
  return t;
}

static TowerOut build_tower(Node *scene, BoxVec *colliders, const Tower *TW) {
  double bx = TW->x, bz = TW->z;
  double base = sandHeight(bx) + sandDetail(bx, bz);
  double D = TW->deckHeight;
  const Palette *P = &TOWER_PALETTES[TW->palette];
  const uint32_t PINK = P->PINK, TEAL = P->TEAL, NAVY = P->NAVY, ORANGE = P->ORANGE, WHITE = 0xf3efe6, DECK = 0xd9cdb6;
  TB tb = { bx, bz, base, colliders };
  GeoList parts = {}, glass = {}, rails = {};
#define ADD(g) gl_push(&parts, (g))
#define ADDR(g) gl_push(&rails, (g))
  // wide skid beams on the sand, cross sleepers
  const double skx[2] = { -1.8, 3.2 };
  for (int k = 0; k < 2; k++) ADD(box_at(skx[k] - 0.26, skx[k] + 0.26, -0.12, 0.2, -2.6, 2.6, ORANGE));
  const double slz[2] = { -2.35, 2.35 };
  for (int k = 0; k < 2; k++) ADD(box_at(-2.2, 3.6, 0.2, 0.36, slz[k] - 0.2, slz[k] + 0.2, ORANGE));
  const double pxs[3] = { -1.8, 0.7, 3.2 }, pzs[2] = { -1.8, 1.8 };
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 2; j++) {
      double x = pxs[i], z = pzs[j];
      ADD(box_at(x - 0.1, x + 0.1, 0.1, D - 0.2, z - 0.1, z + 0.1, TEAL));
      tcol(&tb, x - 0.12, x + 0.12, 0, D - 0.2, z - 0.12, z + 0.12);
    }
  // bold X bracing (above the skirt panel)
  const double yb0 = 0.95;
  const double bzs[2] = { -1.86, 1.86 };
  for (int k = 0; k < 2; k++) {
    double z = bzs[k];
    ADD(beam(v3(-1.8, yb0, z), v3(0.7, D - 0.35, z), 0.15, NAVY));
    ADD(beam(v3(0.7, yb0, z), v3(-1.8, D - 0.35, z), 0.15, NAVY));
    ADD(beam(v3(0.7, yb0, z), v3(3.2, D - 0.35, z), 0.15, NAVY));
    ADD(beam(v3(3.2, yb0, z), v3(0.7, D - 0.35, z), 0.15, NAVY));
  }
  const double bxs[2] = { -1.86, 3.26 };
  for (int k = 0; k < 2; k++) {
    double x = bxs[k];
    ADD(beam(v3(x, yb0, -1.8), v3(x, D - 0.35, 1.8), 0.15, NAVY));
    ADD(beam(v3(x, yb0, 1.8), v3(x, D - 0.35, -1.8), 0.15, NAVY));
  }
  // solid painted skirt panel around the stilts: vertical stripes, trim
  const double skY0 = 0.3, skY1 = 0.85, sw0 = P->skirt;
#define SKIRT_COL(i) (P->skirt3 ? (uint32_t[]){ PINK, WHITE, TEAL }[(i) % 3] : (i) % 2 ? WHITE : PINK)
  const double szs[2] = { -1.92, 1.92 };
  for (int k = 0; k < 2; k++) {
    double z = szs[k];
    int i = 0;
    for (double x = -1.9; x < 3.3; x += sw0, i++) ADD(box_at(x, fmin(x + sw0, 3.3), skY0, skY1, z - 0.03, z + 0.03, SKIRT_COL(i)));
    ADD(box_at(-1.95, 3.35, skY1, skY1 + 0.08, z - 0.05, z + 0.05, TEAL));
    ADD(box_at(-1.95, 3.35, skY0 - 0.08, skY0, z - 0.05, z + 0.05, TEAL));
  }
  const double sxs[2] = { -1.92, 3.32 };
  for (int k = 0; k < 2; k++) {
    double x = sxs[k];
    int i = 0;
    for (double z = -1.9; z < 1.9; z += sw0, i++) ADD(box_at(x - 0.03, x + 0.03, skY0, skY1, z, fmin(z + sw0, 1.9), SKIRT_COL(i)));
    ADD(box_at(x - 0.05, x + 0.05, skY1, skY1 + 0.08, -1.95, 1.95, TEAL));
    ADD(box_at(x - 0.05, x + 0.05, skY0 - 0.08, skY0, -1.95, 1.95, TEAL));
  }
#undef SKIRT_COL
  tcol(&tb, -1.96, 3.36, skY0, skY1, -1.96, 1.96);

  // deck: joists, planks, a fascia band
  const TBox dk = T_DECK;
  ADD(box_at(dk.x0, dk.x1, D - 0.28, D - 0.05, dk.z0, dk.z1, DECK));
  for (double x = dk.x0 + 0.07; x < dk.x1; x += 0.145) ADD(box_at(x - 0.065, x + 0.065, D - 0.05, D, dk.z0, dk.z1, 0xe4d9c2));
  const double dzs[2] = { dk.z0, dk.z1 };
  for (int k = 0; k < 2; k++) ADD(box_at(dk.x0 - 0.03, dk.x1 + 0.03, D - 0.42, D - 0.02, dzs[k] - 0.04, dzs[k] + 0.04, PINK));
  const double dxs[2] = { dk.x0, dk.x1 };
  for (int k = 0; k < 2; k++) ADD(box_at(dxs[k] - 0.04, dxs[k] + 0.04, D - 0.42, D - 0.02, dk.z0 - 0.03, dk.z1 + 0.03, PINK));

  // cabin: rounded ocean front, horizontal colour blocks, vertical stripes in the middle
  const TBox cb = T_CABIN;
  const double rr = 0.9;
  Foot foot = {};
  // counter-clockwise seen from above (x right, z down): west wall, south, SE arc, east, NE arc, north
  foot_seg(&foot, cb.x0, cb.z0, cb.x0, cb.z1, 8);
  foot_seg(&foot, cb.x0, cb.z1, cb.x1 - rr, cb.z1, 8);
  foot_arc(&foot, cb.x1 - rr, cb.z1 - rr, PI_D / 2, 0, 8, rr);
  foot_seg(&foot, cb.x1, cb.z1 - rr, cb.x1, cb.z0 + rr, 4);
  foot_arc(&foot, cb.x1 - rr, cb.z0 + rr, 0, -PI_D / 2, 8, rr);
  foot_seg(&foot, cb.x1 - rr, cb.z0, cb.x0, cb.z0, 8);
  const double y0 = D, yA = D + 0.85, yB = D + 1.95, yC = D + cb.h;
  wall_band(&parts, &foot, y0, yA, BAND_LOW, P, WHITE);
  wall_band(&parts, &foot, yA, yB, BAND_MID, P, WHITE);
  wall_band(&parts, &foot, yB, yC, BAND_TOP, P, WHITE);
  // curved window band on the ocean front (glass), with a white sill and head
  wall_band(&parts, &foot, yA, yA + 0.12, BAND_SILL, P, WHITE);
  for (int i = 0; i < foot.n; i++) {
    const double *a = foot.p[i], *b = foot.p[(i + 1) % foot.n];
    if (!((a[0] + b[0]) / 2 > cb.x1 - rr - 0.05)) continue;
    double p[18] = { a[0], yA + 0.12, a[1], b[0], yA + 0.12, b[1], b[0], yB, b[1], a[0], yA + 0.12, a[1], b[0], yB, b[1], a[0], yB, a[1] };
    Geometry *gg = geo_new();
    geo_set_attr_d(gg, "position", 3, 6, p);
    gl_push(&glass, gg);
  }
  // floor + ceiling of the cabin (seen through the glass)
  ADD(box_at(cb.x0, cb.x1, yC - 0.02, yC, cb.z0, cb.z1, 0xe9e2d4));
  // door on the west wall, portholes on the sides
  ADD(box_at(cb.x0 - 0.03, cb.x0, D + 0.02, D + 2.0, -0.45, 0.45, NAVY));
  ADD(colorize(geo_translate(geo_rotate_z(geo_cyl(0.14, 0.14, 0.02, 20), PI_D / 2), cb.x0 - 0.04, D + 1.45, 0), 0x9fb6c4));
  const double czs[2] = { cb.z0, cb.z1 };
  for (int k = 0; k < 2; k++) {
    double z = czs[k], s = js_sign(z);
    ADD(colorize(geo_translate(geo_torus(0.24, 0.05, 8, 24, TAU, 0, TAU), 1.35, D + 1.4, z + s * 0.03), WHITE));
    ADD(colorize(geo_translate(geo_rotate_x(geo_cyl(0.22, 0.22, 0.02, 24), PI_D / 2), 1.35, D + 1.4, z + s * 0.015), 0x6f8fa3));
    // side window with a top-hinged shutter propped open
    ADD(box_at(0.95, 1.05, D + 1.0, D + 1.8, z + s * 0.005, z + s * 0.03, WHITE));
    Geometry *sh = colorize(geo_box1(0.9, 0.04, 0.7), TEAL);
    geo_translate(geo_rotate_x(geo_translate(sh, 0, 0, s * 0.35), s * -0.5), 2.05, D + 1.95, z + s * 0.02);
    ADD(sh);
    ADD(beam(v3(1.65, D + 1.55, z + s * 0.03), v3(1.65, D + 1.8, z + s * 0.6), 0.025, WHITE));
    ADD(beam(v3(2.45, D + 1.55, z + s * 0.03), v3(2.45, D + 1.8, z + s * 0.6), 0.025, WHITE));
    Geometry *gg = geo_translate(geo_translate(geo_plane(0.8, 0.5, 1, 1), 2.05, D + 1.45, 0), 0, 0, z + s * 0.012);
    if (s < 0) geo_rotate_y(gg, 0);
    gl_push(&glass, gg);
  }
  tcol(&tb, cb.x0, cb.x1, D, yC, cb.z0, cb.z1);

  // roof: overhanging slab, fascia; soffit is its own material (painted boards)
  const TBox rf = T_ROOF;
  const double ry = yC;
  ADD(box_at(rf.x0, rf.x1, ry + 0.14, ry + 0.3, rf.z0, rf.z1, WHITE));
  const double rzs[2] = { rf.z0, rf.z1 };
  for (int k = 0; k < 2; k++) ADD(box_at(rf.x0 - 0.02, rf.x1 + 0.02, ry + 0.04, ry + 0.3, rzs[k] - 0.03, rzs[k] + 0.03, TEAL));
  const double rxs[2] = { rf.x0, rf.x1 };
  for (int k = 0; k < 2; k++) ADD(box_at(rxs[k] - 0.03, rxs[k] + 0.03, ry + 0.04, ry + 0.3, rf.z0, rf.z1, TEAL));
  // porch posts holding the west overhang
  const double ppz[2] = { -1.93, 1.93 };
  for (int k = 0; k < 2; k++) {
    double z = ppz[k];
    ADD(box_at(-1.25, -1.13, D, ry + 0.05, z - 0.06, z + 0.06, WHITE));
    tcol(&tb, -1.26, -1.12, D, ry, z - 0.07, z + 0.07);
  }

  // railings around the deck (open at the stair head on the west side)
  const uint32_t RAIL = NAVY;
  const double rh = 1.0;
  const TStair st = T_STAIR;
  const double runs[4][4] = { { dk.x0, dk.z1, dk.x1, dk.z1 }, { dk.x0, dk.z0, dk.x1, dk.z0 }, { dk.x1, dk.z0, dk.x1, dk.z1 }, { dk.x0, st.z1, dk.x0, dk.z1 } };
  for (int r = 0; r < 4; r++) {   // south, north, east (ocean), west south of the stair head
    double ax = runs[r][0], az = runs[r][1], bx2 = runs[r][2], bz2 = runs[r][3];
    double len = js_hypot2(bx2 - ax, bz2 - az);
    int n = (int)fmax(1, js_round(len / 0.9));
    for (int i = 0; i <= n; i++) {
      double x = ax + ((bx2 - ax) * i) / n, z = az + ((bz2 - az) * i) / n;
      ADDR(colorize(geo_translate(geo_cyl(0.035, 0.035, rh, 10), x, D + rh / 2, z), RAIL));
    }
    const double ys[2] = { rh, rh * 0.5 };
    for (int k = 0; k < 2; k++) ADDR(tube_rail(v3(ax, D + ys[k], az), v3(bx2, D + ys[k], bz2), ys[k] == rh ? 0.035 : 0.022, RAIL));
    for (int i = 0; i < n * 4; i++) {   // balusters
      double f2 = (i + 0.5) / (n * 4), x = ax + (bx2 - ax) * f2, z = az + (bz2 - az) * f2;
      ADDR(colorize(geo_translate(geo_cyl(0.012, 0.012, rh * 0.5 - 0.05, 6), x, D + 0.05 + (rh * 0.5 - 0.05) / 2, z), WHITE));
    }
    tcol(&tb, fmin(ax, bx2) - 0.05, fmax(ax, bx2) + 0.05, D, D + rh, fmin(az, bz2) - 0.05, fmax(az, bz2) + 0.05);
  }

  // stairs down the west side: 18 cm class risers to the sand
  double gyEnd = sandHeight(bx + dk.x0 - 4) + sandDetail(bx + dk.x0 - 4, bz + (st.z0 + st.z1) / 2) - base;
  int nR = (int)fmax(2, js_round((D - gyEnd) / 0.18));
  double rise = (D - gyEnd) / nR, run = st.run;
  int nT = nR - 1;
  double stairX1 = dk.x0, stairX0 = dk.x0 - nT * run;
  for (int i = 0; i < nT; i++) {
    double top = D - (i + 1) * rise, xa = stairX1 - (i + 1) * run, xb = stairX1 - i * run;
    ADD(box_at(xa, xb + 0.02, top - 0.05, top, st.z0 + 0.04, st.z1 - 0.04, 0xe4d9c2));
    ADD(box_at(xb - 0.015, xb + 0.01, top, top + rise - 0.05, st.z0 + 0.06, st.z1 - 0.06, WHITE));   // riser board
  }
  const double stz[2] = { st.z0, st.z1 };
  for (int k = 0; k < 2; k++) {
    double z = stz[k];
    ADD(beam(v3(stairX1, D - 0.15, z), v3(stairX0, gyEnd + 0.05, z), 0.07, TEAL));             // stringer
    ADDR(tube_rail(v3(stairX1, D + rh, z), v3(stairX0 + 0.1, gyEnd + rh, z), 0.03, RAIL));     // handrail
    ADD(box_at(stairX0 + 0.06, stairX0 + 0.14, gyEnd - 0.05, gyEnd + rh, z - 0.04, z + 0.04, RAIL));
    for (int i = 1; i < nT; i += 2) {
      double x = stairX1 - i * run, y = D - i * rise;
      ADD(box_at(x - 0.02, x + 0.02, y, y + rh, z - 0.02, z + 0.02, RAIL));
    }
    tcol(&tb, stairX0, stairX1, gyEnd, D + rh, z - 0.05, z + 0.05);
  }

  // flags on a pole at the north-east corner
  ADD(colorize(geo_translate(geo_cyl(0.045, 0.06, 6.2, 8), dk.x1 - 0.08, D + 3.1, dk.z0 + 0.08), WHITE));
  tcol(&tb, dk.x1 - 0.14, dk.x1 - 0.02, D, D + 6.2, dk.z0 + 0.02, dk.z0 + 0.14);
#undef ADD
#undef ADDR

  Node *tower = node_new(NODE_GROUP, "beach");
  add_mesh(tower, merge_list(&parts, false), painted_material(1), true, true);
  add_mesh(tower, merge_list(&rails, false), painted_material(0), true, true);

  // soffit: painted tongue-and-groove, bounce-lit warm from the sunlit sand below
  Texture *soffitTex = soffit_texture();
  double sw = rf.x1 - rf.x0, sd = rf.z1 - rf.z0;
  Geometry *sg = geo_rotate_x(geo_plane(sw, sd, 1, 1), PI_D / 2);
  float *uv = geo_data(sg, "uv");
  for (int i = 0; i < sg->count; i++) {
    uv[i * 2] = (float)(uv[i * 2] * sw);
    uv[i * 2 + 1] = (float)(uv[i * 2 + 1] * sd / 3.2);
  }
  geo_translate(sg, (rf.x0 + rf.x1) / 2, ry + 0.14, 0);
  MatDesc sfd = md_standard();
  sfd.name = "tower soffit";
  sfd.map = soffitTex;
  sfd.color = color_hex(0xfffaf2);
  sfd.roughness = 0.8;
  sfd.prog[MV_PLAIN] = PROG_TOWER_SOFFIT;
  add_mesh(tower, sg, mat_three(&sfd), false, false);

  MatDesc gd = md_physical();
  gd.name = "tower glass";
  gd.color = color_hex(0x3c5058); gd.roughness = 0.05; gd.metalness = 0.1; gd.transparent = true; gd.opacity = 0.75;
  gd.side = SIDE_DOUBLE;
  gd.prog[MV_PLAIN] = PROG_PHYS_TRANSP;
  gd.prog[MV_PLAIN_BACK] = PROG_PHYS_TRANSP_BACK;
  gd.prog[MV_PLAIN_FRONT] = PROG_PHYS_TRANSP;
  add_mesh(tower, merge_list(&glass, true), mat_three(&gd), false, false);

  // flags: waving cloth
  MatDesc fd = md_standard();
  fd.name = "tower flag";
  fd.vertex_colors = true; fd.roughness = 0.8; fd.side = SIDE_DOUBLE;
  fd.prog[MV_PLAIN] = PROG_TOWER_FLAG;
  Material *flagMat = mat_three(&fd);
  GeoList fl = {};
  gl_push(&fl, colorize(geo_translate(geo_translate(geo_plane(1.7, 1.05, 16, 6), 0.85, 0, 0), 0, D + 5.6, 0), 0x2f9a52));
  gl_push(&fl, colorize(geo_translate(geo_translate(geo_plane(1.6, 1.0, 16, 6), 0.8, 0, 0), 0, D + 4.45, 0), 0xf2c21f));
  gl_push(&fl, colorize(geo_translate(geo_translate(geo_plane(1.5, 0.95, 16, 6), 0.75, 0, 0), 0, D + 3.35, 0), 0x6b3fa0));
  Node *flag = add_mesh(tower, merge_list(&fl, false), flagMat, true, false);
  // blow toward the west-south-west (sea breeze), from the pole
  flag->position = v3(dk.x1 - 0.08, 0, dk.z0 + 0.08);
  node_set_rotation(flag, 0, PI_D * 0.92, 0);

  tower->position = v3(bx, base, bz);
  node_add(scene, tower);

  TowerOut out = { .flag = flagMat };
  out.surfaces.base = base;
  out.surfaces.deckY = base + D;
  out.surfaces.stair.x0 = bx + stairX0; out.surfaces.stair.x1 = bx + stairX1;
  out.surfaces.stair.z0 = bz + st.z0; out.surfaces.stair.z1 = bz + st.z1;
  out.surfaces.stair.run = run; out.surfaces.stair.rise = rise; out.surfaces.stair.n = nT;
  out.surfaces.deck.x0 = bx + dk.x0; out.surfaces.deck.x1 = bx + dk.x1;
  out.surfaces.deck.z0 = bz + dk.z0; out.surfaces.deck.z1 = bz + dk.z1;
  return out;
}

// ---- dune plants, wrack clumps, props ------------------------------------------------------------------
// sea grape: a dome of big round leaves, green with the odd bronze-red one
static Geometry *sea_grape_geometry(Rng *rnd) {
  GeoList leaves = {};
  static const uint32_t cols[5] = { 0x55782f, 0x668a37, 0x4a6b2a, 0x76963f, 0x8a5a2c };
  for (int i = 0; i < 170; i++) {
    double u = rng_next(rnd), a = rng_next(rnd) * PI_D * 2, el = acos(1 - u * 0.95);
    V3 n = { sin(el) * cos(a), cos(el), sin(el) * sin(a) };
    double r = 0.5 + rng_next(rnd) * 0.12;
    Geometry *g = geo_circle(0.05 + rng_next(rnd) * 0.035, 8, 0, TAU);
    double tx = (rng_next(rnd) - 0.5) * 0.8, tz = (rng_next(rnd) - 0.5) * 0.8;
    V3 tilt = v3_norm(v3_add(n, v3(tx, 0.5, tz)));
    geo_apply_m4(g, m4_from_quat(quat_unit_vectors(v3(0, 0, 1), tilt)));
    geo_translate(g, n.x * r, n.y * r * 0.85 + 0.05, n.z * r);
    uint32_t c = rng_next(rnd) < 0.07 ? cols[4] : cols[(int)floor(rng_next(rnd) * 4)];
    gl_push(&leaves, colorize(g, c));
  }
  for (int i = 0; i < 5; i++) {
    double a = rng_next(rnd) * 6.28;
    gl_push(&leaves, beam(v3(0, 0, 0), v3(cos(a) * 0.35, 0.45, sin(a) * 0.35), 0.03, 0x6b5a48));
  }
  return merge_list(&leaves, true);
}

// sea oats: an irregular tuft of arching blades and a few drooping seed heads
static Geometry *sea_oat_geometry(Rng *rnd) {
  GeoList parts = {};
  Color green = color_hex(0x7c8a44), straw = color_hex(0xd6c07a), dry = color_hex(0xb59a58);
  for (int b = 0; b < 30; b++) {
    double a = rng_next(rnd) * PI_D * 2, lean = 0.3 + rng_next(rnd) * 0.8, h = 0.35 + rng_next(rnd) * 0.5, w = 0.01 + rng_next(rnd) * 0.008;
    double pos[30], colr[30];
    for (int s = 0; s <= 4; s++) {
      double f = s / 4.0, y = h * f, off = lean * f * f * h;
      double cx = cos(a) * off, cz = sin(a) * off, ww = w * (1 - f * 0.9);
      double p[6] = { cx - sin(a) * ww, y, cz + cos(a) * ww, cx + sin(a) * ww, y, cz - cos(a) * ww };
      memcpy(pos + s * 6, p, sizeof p);
      Color c = color_lerp(green, rng_next(rnd) < 0.3 ? dry : straw, f * 0.9);
      double cc[6] = { c.r, c.g, c.b, c.r, c.g, c.b };
      memcpy(colr + s * 6, cc, sizeof cc);
    }
    uint32_t idx[24];
    for (uint32_t s = 0; s < 4; s++) {
      uint32_t i = s * 2, q[6] = { i, i + 1, i + 2, i + 1, i + 3, i + 2 };
      memcpy(idx + s * 6, q, sizeof q);
    }
    Geometry *g = geo_new();
    geo_set_attr_d(g, "position", 3, 10, pos);
    geo_set_attr_d(g, "color", 3, 10, colr);
    geo_set_index(g, idx, 24);
    gl_push(&parts, g);
  }
  for (int k = 0; k < 3; k++) {
    double a = rng_next(rnd) * 6.28, h = 0.9 + rng_next(rnd) * 0.4;
    V3 tip = { cos(a) * 0.18, h, sin(a) * 0.18 };
    gl_push(&parts, beam(v3(0, 0, 0), tip, 0.008, 0xc9b378));
    for (int s = 0; s < 7; s++) {
      Geometry *e = geo_translate(geo_scale(geo_sphere3(0.02, 5, 3), 0.6, 1.5, 0.6), tip.x + cos(a + s) * 0.04, h - 0.06 - s * 0.045, tip.z + sin(a + s) * 0.04);
      gl_push(&parts, colorize(e, 0xbfa261));
    }
  }
  return merge_list(&parts, true);
}

// saw palmetto: stiff fans of narrow leaflets on short stalks
static Geometry *palmetto_geometry(Rng *rnd) {
  GeoList parts = {};
  for (int f = 0; f < 8; f++) {
    double a = ((double)f / 8) * PI_D * 2 + rng_next(rnd) * 0.5, tilt = 0.5 + rng_next(rnd) * 0.6, len = 0.45 + rng_next(rnd) * 0.3;
    V3 dir = { cos(a) * sin(tilt), cos(tilt), sin(a) * sin(tilt) };
    V3 base = v3_scale(dir, 0.35);
    gl_push(&parts, beam(v3(0, 0, 0), base, 0.02, 0x7a6a3c));
    V3 side = { -sin(a), 0, cos(a) };
    for (int k = 0; k < 13; k++) {
      double t = ((double)k / 12 - 0.5) * 2.2;
      V3 d = v3_norm(v3_add_scaled(v3_scale(dir, cos(t * 0.6)), side, sin(t)));
      V3 tipP = v3_add_scaled(base, d, len);
      V3 w = v3_scale(side, 0.018);
      V3 b1 = v3_add(base, w), b2 = v3_sub(base, w);
      double p[9] = { b1.x, b1.y, b1.z, b2.x, b2.y, b2.z, tipP.x, tipP.y, tipP.z };
      Geometry *g = geo_new();
      geo_set_attr_d(g, "position", 3, 3, p);
      gl_push(&parts, colorize(g, rng_next(rnd) < 0.15 ? 0x9a9a52 : 0x5d7a3a));
    }
  }
  return merge_list(&parts, true);
}

// One InstancedMesh along the whole beach has a single bounding sphere hundreds of metres wide,
// so neither the view nor the sun's shadow box could skip any of it: add it as blocks along z
// instead (same instances, same order within a block).
typedef struct Inst { M4 m; bool has_color; Color c; } Inst;
static void add_in_blocks(Node *scene, GpuGeometry *geo, Material *mat, const Inst *items, int count, bool cast, bool receive) {
  const double block = 60;
  typedef struct Blk { int k; IVec idx; } Blk;
  Vec(Blk) blocks = {};
  for (int i = 0; i < count; i++) {
    if (m4_determinant(items[i].m) == 0) continue;   // an empty placeholder (gap), invisible anyway
    int k = (int)floor(m4_get_position(items[i].m).z / block);
    Blk *b = nullptr;
    for (size_t j = 0; j < blocks.len && !b; j++) if (blocks.data[j].k == k) b = &blocks.data[j];
    if (!b) { vec_push(&blocks, ((Blk){ k, {} })); b = &blocks.data[blocks.len - 1]; }
    vec_push(&b->idx, i);
  }
  for (size_t i = 1; i < blocks.len; i++) {   // keys sorted ascending
    Blk x = blocks.data[i];
    size_t j = i;
    while (j > 0 && blocks.data[j - 1].k > x.k) { blocks.data[j] = blocks.data[j - 1]; j--; }
    blocks.data[j] = x;
  }
  for (size_t bi = 0; bi < blocks.len; bi++) {
    const Blk *b = &blocks.data[bi];
    Node *part = node_instanced(geo, mat, (int)b->idx.len);
    snprintf(part->name, sizeof part->name, "beach");
    for (size_t j = 0; j < b->idx.len; j++) {
      const Inst *it = &items[b->idx.data[j]];
      inst_set_matrix(part, (int)j, it->m);
      if (it->has_color) inst_set_color(part, (int)j, it->c);
    }
    part->cast_shadow = cast;
    part->receive_shadow = receive;
    node_add(scene, part);
  }
  for (size_t i = 0; i < blocks.len; i++) vec_free(&blocks.data[i].idx);
  vec_free(&blocks);
}
// setMatrixAt stores a Float32Array copy
static M4 f32m(M4 m) {
  for (int k = 0; k < 16; k++) m.e[k] = (float)m.e[k];
  return m;
}

static double ground(double x, double z) { return sandHeight(x) + sandDetail(x, z); }

typedef struct Plant { double x, z, s; } Plant;
typedef Vec(Plant) PlantVec;
typedef bool (*ZPred)(double z);
static bool path_orig(double z) { return near_orig_access(z, 1.3); }
static bool gap_more(double z) { return near_more_access(z, 1.3); }
static bool path_all(double z) { return near_access(z, 1.3); }
static bool gap_none(double z) { (void)z; return false; }

static void place_plants(Node *scene, Rng *rnd, Geometry *geo, Material *mat, const PlantVec *all, int k, ZPred gap, double sy) {
  // split(items, k): every third; lower tiers thin the clumps beyond the walkable block
  double keep = QUALITY.farFoliage;
  int step = keep < 1 ? (int)js_round(1 / keep) : 1;
  Vec(Plant) split = {};
  for (size_t i = 0; i < all->len; i++) if ((int)(i % 3) == k) vec_push(&split, all->data[i]);
  Vec(Plant) items = {};
  for (size_t i = 0; i < split.len; i++)
    if (step <= 1 || fabs(split.data[i].z) < 100 || i % (size_t)step == 0) vec_push(&items, split.data[i]);
  vec_free(&split);
  Inst *out = xcalloc(items.len ? items.len : 1, sizeof *out);
  for (size_t i = 0; i < items.len; i++) {
    double x = items.data[i].x, z = items.data[i].z, s = items.data[i].s;
    double ex = (rng_next(rnd) - 0.5) * 0.15, ey = rng_next(rnd) * 6.28, ez = (rng_next(rnd) - 0.5) * 0.15;
    Quat q = quat_from_euler(euler(ex, ey, ez, EULER_XYZ));
    double s0 = s * (0.8 + rng_next(rnd) * 0.5), s1 = s * sy * (0.7 + rng_next(rnd) * 0.4), s2 = s * (0.8 + rng_next(rnd) * 0.5);
    M4 m4 = m4_compose(v3(x, ground(x, z) - 0.03, z), q, v3(s0, s1, s2));
    if (gap(z)) m4 = m4_scaling(0, 0, 0);   // (drawn empty: the random stream stays the same)
    out[i].m = f32m(m4);
  }
  GpuGeometry *g = gpu_geometry(geo);
  geo_free(geo);
  add_in_blocks(scene, g, mat, out, (int)items.len, true, true);
  free(out);
  vec_free(&items);
}

// ranges: z spans to plant; path: the gaps that steer the random stream (the original accesses for
// the original span), gap: extra gaps left empty without touching the stream
static void dune_vegetation(Node *scene, Rng *rnd, BoxVec *colliders, const double (*ranges)[2], int nranges, ZPred path, ZPred gap) {
  const double fenceX = SAND.x0 + 4.6;
  PlantVec grape = {}, oats = {}, palms = {};
  // irregular clumps: dense where the along-shore noise is high, gaps elsewhere
  for (int r = 0; r < nranges; r++)
    for (double z = ranges[r][0]; z < ranges[r][1]; z += 0.5 + rng_next(rnd) * 1.0) {
      if (path(z)) continue;
      double dens = 0.5 + 0.5 * sin(z * 0.043 + 1.3) * sin(z * 0.11 + 0.4);
      if (rng_next(rnd) > 0.5 + 0.5 * dens) continue;
      double x = SAND.x0 + 1.0 + pow(rng_next(rnd), 0.8) * (fenceX - SAND.x0 - 1.6);
      double rr = rng_next(rnd);
      double r1 = rng_next(rnd), r2 = rng_next(rnd);
      double big = 0.55 + 1.5 * r1 * r2 + 0.5 * dens;
      if (rr < 0.35) vec_push(&grape, ((Plant){ x, z, big }));
      else if (rr < 0.5) { double s = 0.6 + rng_next(rnd) * 0.9; vec_push(&palms, ((Plant){ x, z, s })); }
      double n1 = rng_next(rnd), n2 = rng_next(rnd);
      int n = 1 + (int)floor(n1 * n2 * 9);
      for (int k = 0; k < n; k++) {
        double ox = x + (rng_next(rnd) - 0.5) * 2.6, oz = z + (rng_next(rnd) - 0.5) * 2.6, os = 0.45 + rng_next(rnd) * 0.85;
        vec_push(&oats, ((Plant){ ox, oz, os }));
      }
    }
  MatDesc ld = md_standard();
  ld.name = "dune leaves";
  ld.vertex_colors = true; ld.roughness = 0.6; ld.side = SIDE_DOUBLE;
  ld.prog[MV_INSTANCED] = PROG_STD_VCOL_INST_DBL;
  Material *leafMat = mat_three(&ld);
  MatDesc od = md_standard();
  od.name = "dune oats";
  od.vertex_colors = true; od.roughness = 0.8; od.side = SIDE_DOUBLE;
  od.prog[MV_INSTANCED] = PROG_STD_VCOL_INST_DBL;
  Material *oatMat = mat_three(&od);
  // three shape variants of each plant so neighbouring clumps never match
  for (int k = 0; k < 3; k++) {
    place_plants(scene, rnd, sea_grape_geometry(rnd), leafMat, &grape, k, gap, 1);
    place_plants(scene, rnd, palmetto_geometry(rnd), leafMat, &palms, k, gap, 1);
    place_plants(scene, rnd, sea_oat_geometry(rnd), oatMat, &oats, k, gap, 1);
  }
  vec_free(&grape); vec_free(&oats); vec_free(&palms);

  // rope-and-post dune fence
  GeoList fence = {};
  for (int r = 0; r < nranges; r++) {
    bool have_prev = false, have_drawn = false;
    double prev[3] = {}, prevTop[3] = {}, drawn[3] = {};
    for (double z = ranges[r][0]; z <= ranges[r][1]; z += 1.9 + rng_next(rnd) * 1.1) {
      if (path(z)) { have_prev = have_drawn = false; continue; }
      bool open = gap(z);
      double x = fenceX + 0.15 * sin(z * 0.07) + (rng_next(rnd) - 0.5) * 0.25, y = ground(x, z);
      double ph = 0.9 + rng_next(rnd) * 0.22, lx = (rng_next(rnd) - 0.5) * 0.16, lz = (rng_next(rnd) - 0.5) * 0.16;
      double rtop = 0.045 + rng_next(rnd) * 0.015;
      Geometry *pg = geo_cylinder(rtop, 0.055, ph, 7, 1, false, 0, TAU);
      geo_translate(geo_rotate_z(geo_rotate_x(geo_translate(pg, 0, ph / 2 - 0.08, 0), lz), lx), x, y, z);
      uint32_t pc = color_get_hex(color_scale(color_hex(0x8d8478), 0.8 + rng_next(rnd) * 0.35));
      Geometry *post = colorize(pg, pc);
      if (!open) gl_push(&fence, post);
      else geo_free(post);
      double top[3] = { x - sin(lx) * (ph - 0.2), y + ph - 0.2, z + sin(lz) * (ph - 0.2) };
      if (have_prev) {
        V3 pts[9];
        double sag = 0.05 + rng_next(rnd) * 0.2;
        for (int i = 0; i <= 8; i++) {
          double f = i / 8.0;
          pts[i] = v3(prevTop[0] + (top[0] - prevTop[0]) * f, prevTop[1] + (top[1] - prevTop[1]) * f - sag * sin(f * PI_D), prevTop[2] + (top[2] - prevTop[2]) * f);
        }
        if (have_drawn && !open) {
          CatmullRom3 c = curve_catmull(pts, 9, false, CURVE_CENTRIPETAL, 0.5);
          gl_push(&fence, colorize(geo_tube(&c, 8, 0.012, 4, false), 0xd6c6a0));
          curve_free(&c);
        }
      }
      if (have_drawn && !open && fabs(z) < DISTRICT.zMax + 5)
        vec_push(colliders, ((BeachBox){ { fmin(x, drawn[0]) - 0.08, y, drawn[2] }, { fmax(x, drawn[0]) + 0.08, y + 0.9, z } }));
      prev[0] = x; prev[1] = y; prev[2] = z;
      memcpy(prevTop, top, sizeof top);
      have_prev = true;
      if (open) have_drawn = false;
      else { memcpy(drawn, prev, sizeof prev); have_drawn = true; }
    }
  }
  MatDesc fd = md_standard();
  fd.name = "dune fence";
  fd.vertex_colors = true; fd.roughness = 0.9;
  fd.prog[MV_PLAIN] = PROG_STD_VCOL;
  add_mesh(scene, merge_list(&fence, true), mat_three(&fd), true, true);
}

typedef double (*ZAt)(double r);
static double zat_default(double r) { return (r - 0.5) * 400; }
static double g_span;
static double zat_ext(double r) { return r < 0.5 ? -200 - g_span + r * 2 * g_span : 200 + (r - 0.5) * 2 * g_span; }

static void wrack_clumps(Node *scene, Rng *rnd, ZAt zAt, int N, double keep) {
  Geometry *g = geo_rotate_z(geo_cylinder(0.007, 0.007, 1, 4, 4, false, 0, TAU), PI_D / 2);
  float *p = geo_data(g, "position");
  for (int i = 0; i < g->count; i++) {
    double x = p[i * 3], y = p[i * 3 + 1], z = p[i * 3 + 2];
    p[i * 3] = (float)x;
    p[i * 3 + 1] = (float)(y * 0.6 + sin(x * 9.0) * 0.012);
    p[i * 3 + 2] = (float)(z + sin(x * 6.0 + 1.0) * 0.03);
  }
  geo_compute_vertex_normals(g);
  MatDesc d = md_standard();
  d.name = "wrack";
  d.color = color_hex(0xffffff);
  d.roughness = 0.85;
  d.prog[MV_INSTANCED] = PROG_STD_INST_ICOL;
  Material *mat = mat_three(&d);
  Inst *items = xcalloc((size_t)N, sizeof *items);
  for (int i = 0; i < N; i++) {
    // strands tangled in clusters along the line, with bare gaps between clusters
    double z;
    do z = zAt(rng_next(rnd)); while (sin(z * 0.09) + 0.6 * sin(z * 0.23 + 2.0) < -0.5);
    double wx = WET_LINE_X - 0.9 + 0.5 * sin(z * 0.047) + 0.25 * sin(z * 0.19 + 1);
    double a = rng_next(rnd), b = rng_next(rnd);
    double x = wx + (a - 0.5) * (0.25 + 0.35 * b);
    double l1 = rng_next(rnd), l2 = rng_next(rnd);
    double len = 0.06 + l1 * l2 * 0.24;
    double ey = rng_next(rnd) * 6.28, ez = (rng_next(rnd) - 0.5) * 0.2;
    Quat q = quat_from_euler(euler(0, ey, ez, EULER_XYZ));
    double sy = 0.6 + rng_next(rnd) * 0.8;
    items[i].m = f32m(m4_compose(v3(x, sandHeight(x) + sandDetail(x, z) + 0.004, z), q, v3(len, sy, 1)));
    double gold = rng_next(rnd);
    items[i].has_color = true;
    Color c = color_srgb(0.4 + gold * 0.25, 0.3 + gold * 0.16, 0.13 + gold * 0.06);
    items[i].c = (Color){ (float)c.r, (float)c.g, (float)c.b };
  }
  int count = (int)fmin(N, js_round(QUALITY.wrack * keep));   // strands are in random order: any prefix is a uniform thinning
  GpuGeometry *gg = gpu_geometry(g);
  geo_free(g);
  add_in_blocks(scene, gg, mat, items, count, true, true);
  free(items);
}

// steps over the seawall at the access points, and the wall itself as a collider
static void seawall_access(Node *scene, BoxVec *colliders) {
  double all[8];
  memcpy(all, ACCESS_Z, sizeof ACCESS_Z);
  memcpy(all + 2, MORE_ACCESS_Z, sizeof MORE_ACCESS_Z);
  GeoList parts = {};
  for (int i = 0; i < 8; i++) {
    double az = all[i];
    for (int k = 0; k < 3; k++) gl_push(&parts, box_at(STEPS[k][0], STEPS[k][1], 0, STEPS[k][2], az - ACCESS_HALF, az + ACCESS_HALF, 0xd8cbb2));
    for (int si = 0; si < 2; si++) {   // cheek walls
      double s = si ? 1 : -1;
      gl_push(&parts, box_at(10.75, 12.6, 0, 0.78, az + s * (ACCESS_HALF + 0.1) - 0.1, az + s * (ACCESS_HALF + 0.1) + 0.1, 0xcbbd9f));
    }
  }
  add_mesh(scene, merge_list(&parts, false), painted_material(0), true, true);
  double sorted[8];
  memcpy(sorted, all, sizeof all);
  for (int i = 1; i < 8; i++) {
    double x = sorted[i];
    int j = i - 1;
    while (j >= 0 && sorted[j] > x) { sorted[j + 1] = sorted[j]; j--; }
    sorted[j + 1] = x;
  }
  double cuts[18];
  int nc = 0;
  cuts[nc++] = DISTRICT.zMin - 10;
  for (int i = 0; i < 8; i++) { cuts[nc++] = sorted[i] - ACCESS_HALF; cuts[nc++] = sorted[i] + ACCESS_HALF; }
  cuts[nc++] = DISTRICT.zMax + 10;
  for (int i = 0; i < nc; i += 2) vec_push(colliders, ((BeachBox){ { WALL.x0, 0, cuts[i] }, { WALL.x1, WALL.top, cuts[i + 1] } }));
  for (int i = 0; i < 8; i++)
    for (int si = 0; si < 2; si++) {
      double zc = all[i] + (si ? 1 : -1) * (ACCESS_HALF + 0.1);
      vec_push(colliders, ((BeachBox){ { 10.75, 0, zc - 0.1 }, { 12.6, 0.78, zc + 0.1 } }));
    }
}

static void props(Node *scene, BoxVec *colliders) {
  GeoList parts = {};
#define ADDCOL(x0, x1, y0, y1, z0, z1) vec_push(colliders, ((BeachBox){ { x0, y0, z0 }, { x1, y1, z1 } }))
  // slatted trash barrel by the tower stairs
  {
    double x = TOWER.x - 6.9, z = TOWER.z + 2.6, y = ground(x, z);
    for (int i = 0; i < 14; i++) {
      double a = ((double)i / 14) * PI_D * 2;
      gl_push(&parts, geo_translate(geo_rotate_y(box_at(-0.04, 0.04, 0, 0.95, -0.03, 0.03, i % 2 ? 0x2f6e8e : 0x2a6282), -a), cos(a) * 0.3 + x, y, sin(a) * 0.3 + z));
    }
    const double hs[2] = { 0.12, 0.85 };
    for (int k = 0; k < 2; k++) gl_push(&parts, colorize(geo_translate(geo_rotate_x(geo_torus(0.31, 0.02, 6, 20, TAU, 0, TAU), PI_D / 2), x, y + hs[k], z), 0x3a3d40));
    gl_push(&parts, colorize(geo_translate(geo_cyl(0.28, 0.28, 0.03, 16), x, y + 0.6, z), 0x1e2022));
    ADDCOL(x - 0.33, x + 0.33, y, y + 0.95, z - 0.33, z + 0.33);
  }
  // volleyball court far north: posts, net
  const double netX = 34, netZ = -58;
  double yN = ground(netX, netZ);
  const double dzs[2] = { -4.8, 4.8 };
  for (int k = 0; k < 2; k++) {
    double dz = dzs[k];
    gl_push(&parts, colorize(geo_translate(geo_cyl(0.05, 0.05, 2.55, 8), netX, yN + 1.27, netZ + dz), 0xe8e4da));
    ADDCOL(netX - 0.06, netX + 0.06, yN, yN + 2.55, netZ + dz - 0.06, netZ + dz + 0.06);
  }
#undef ADDCOL
  gl_push(&parts, box_at(netX - 0.01, netX + 0.01, yN + 2.34, yN + 2.43, netZ - 4.8, netZ + 4.8, 0xf4f2ec));
  add_mesh(scene, merge_list(&parts, false), painted_material(1), true, true);
  // net mesh
  Canvas *c = canvas_new(64, 64);
  cv_fill_color(c, "#000");
  cv_fill_rect(c, 0, 0, 64, 64);
  cv_stroke_color(c, "#fff");
  cv_line_width(c, 3);
  for (int i = 0; i <= 64; i += 16) {
    cv_begin_path(c); cv_move_to(c, i, 0); cv_line_to(c, i, 64); cv_stroke(c);
    cv_begin_path(c); cv_move_to(c, 0, i); cv_line_to(c, 64, i); cv_stroke(c);
  }
  Texture *nt = canvas_texture(c, false, WRAP_REPEAT, 1);
  canvas_free(c);
  nt->repeat = v2(96, 8);
  MatDesc d = md_standard();
  d.name = "volleyball net";
  d.alpha_map = nt; d.color = color_hex(0x222222); d.alpha_test = 0.3; d.side = SIDE_DOUBLE; d.roughness = 0.9;
  d.prog[MV_PLAIN] = PROG_STD_ATEST_DBL;
  add_mesh(scene, geo_translate(geo_rotate_y(geo_plane(9.6, 0.85, 1, 1), PI_D / 2), netX, yN + 1.9, netZ), mat_three(&d), false, false);
}

// ---- the beach -----------------------------------------------------------------------------------------
struct Beach {
  Surf *surf;
  Material *sand, *swash;
  Node *sheet;
  BoxVec colliders;
  TowerOut towers[3];
};

Beach *build_beach(Node *scene, Surf *surf) {
  Beach *B = xcalloc(1, sizeof *B);
  B->surf = surf;
  Rng rng = rng_make(2024);
  Texture *detail = bake_sand_detail(QUALITY.sandDetail);
  B->sand = sand_material(detail);
  // detailed sand through the district (the original +-220 m sheet, then the extensions at the
  // same row spacing), flat low-res sand beyond
  int extRows = (int)js_round(QUALITY.sandRows * (SAND_DETAIL_Z + 5 - 220) / 440);
  const struct { double a, b; int rows; bool detail; } SHEETS[5] = {
    { -220, 220, QUALITY.sandRows, true }, { 220, SAND_DETAIL_Z + 5, extRows, true }, { -SAND_DETAIL_Z - 5, -220, extRows, true },
    { SAND_DETAIL_Z + 5, 2500, 1, false }, { -2500, -SAND_DETAIL_Z - 5, 1, false } };
  for (int i = 0; i < 5; i++) add_mesh(scene, sand_geometry(SHEETS[i].a, SHEETS[i].b, SHEETS[i].rows, SHEETS[i].detail), B->sand, false, true);
  B->sheet = swash_sheet(&B->swash);
  node_add(scene, B->sheet);
  wrack_clumps(scene, &rng, zat_default, 9000, 1);
  const double r0[1][2] = { { -220, 220 } };
  dune_vegetation(scene, &rng, &B->colliders, r0, 1, path_orig, gap_more);
  seawall_access(scene, &B->colliders);
  B->towers[0] = build_tower(scene, &B->colliders, &TOWER);
  props(scene, &B->colliders);
  // the extended beach (own random streams): wrack, dune plants and fence, two more towers
  double span = DISTRICT.zMax + 5 - 200;
  g_span = span;
  Rng r26 = rng_make(2026);
  wrack_clumps(scene, &r26, zat_ext, 6600, span / 400);
  Rng r25 = rng_make(2025);
  const double r1[2][2] = { { -DISTRICT.zMax - 5, -221 }, { 221, DISTRICT.zMax + 5 } };
  dune_vegetation(scene, &r25, &B->colliders, r1, 2, path_all, gap_none);
  for (int i = 1; i < 3; i++) B->towers[i] = build_tower(scene, &B->colliders, &TOWERS[i]);
  beach_update(B, surf_time(surf), nullptr);
  return B;
}

double beach_ground_at(double x, double z) { return groundHeight(x, z); }

double beach_height_at(const Beach *b, double x, double z, double current_y) {
  double g = beach_ground_at(x, z);
#define REACH(y) (current_y >= (y) - 0.45)   // a step up of up to 45 cm is walkable
  if (x >= STEPS[0][0] && x < STEPS[2][1] && near_access(z, 0)) {
    int k = 0;
    while (k < 2 && !(x < STEPS[k][1])) k++;
    if (REACH(STEPS[k][2])) return STEPS[k][2];
  }
  for (int i = 0; i < 3; i++) {
    const TowerSurf *T = &b->towers[i].surfaces;
    if (fabs(z - (T->deck.z0 + T->deck.z1) / 2) > 8) continue;
    if (x >= T->deck.x0 && x <= T->deck.x1 && z >= T->deck.z0 && z <= T->deck.z1 && REACH(T->deckY)) return T->deckY;
    if (x >= T->stair.x0 && x < T->stair.x1 && z >= T->stair.z0 && z <= T->stair.z1) {
      double k = floor((T->stair.x1 - x) / T->stair.run);
      double y = T->deckY - (k + 1) * T->stair.rise;
      if (y > g && REACH(y)) return y;
    }
  }
#undef REACH
  return g;
}

const BeachBox *beach_colliders(const Beach *b, int *n) {
  *n = (int)b->colliders.len;
  return b->colliders.data;
}

void beach_update(Beach *b, double t, const Camera *camera) {
  for (int i = 0; i < 3; i++) mat_set_float(b->towers[i].flag, "uFlagT", t);
  surf_apply(b->surf, b->sand);
  surf_apply(b->surf, b->swash);
  if (camera) b->sheet->position.z = js_round(camera->node->position.z / 2) * 2;
}
