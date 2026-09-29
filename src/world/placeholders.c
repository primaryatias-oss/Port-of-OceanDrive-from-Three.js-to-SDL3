// Port of src/world/placeholders.js: park lawn, serpentine promenade, coral-stone seawall and
// the background towers behind the Deco row.
#include "world/placeholders.h"

#include "gfx/three_mat.h"
#include "textures/noise.h"
#include "world/layout.h"

// Box from explicit bounds with world-space planar UVs (u = x / tile, v = z / tile).
static Geometry *slab(double x0, double x1, double y0, double y1, double z0, double z1, double tile) {
  Geometry *g = geo_box1(x1 - x0, y1 - y0, z1 - z0);
  geo_translate(g, (x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2);
  float *pos = geo_data(g, "position"), *uv = geo_data(g, "uv"), *nrm = geo_data(g, "normal");
  for (int i = 0; i < g->count; i++) {
    bool ax = fabs(nrm[i * 3]) > 0.5;
    bool az = fabs(nrm[i * 3 + 2]) > 0.5;
    double u = ax ? pos[i * 3 + 2] : pos[i * 3];
    double v = ax || az ? pos[i * 3 + 1] : pos[i * 3 + 2];
    uv[i * 2] = (float)(u / tile);
    uv[i * 2 + 1] = (float)(v / tile);
  }
  return g;
}

static Node *mesh(Geometry *geo, Material *mat, bool cast, bool receive) {
  Node *m = node_mesh(gpu_geometry(geo), mat);
  geo_free(geo);
  m->cast_shadow = cast;
  m->receive_shadow = receive;
  return m;
}

// Park lawn: dense short blades in mixed greens and yellows (texture covers 7 m)
static Texture *blade_texture(void) {
  const int S = 1024;
  Canvas *g = canvas_new(S, S);
  Rng rnd = rng_make(3131);
  cv_fill_color(g, "rgb(86,104,48)");
  cv_fill_rect(g, 0, 0, S, S);
  for (int i = 0; i < 60000; i++) {
    double x = rng_next(&rnd) * S, y = rng_next(&rnd) * S, l = 3 + rng_next(&rnd) * 7, a = -PI_D / 2 + (rng_next(&rnd) - 0.5) * 1.4;
    double t = rng_next(&rnd);
    double r = t < 0.15 ? 150 + rng_next(&rnd) * 40 : 80 + rng_next(&rnd) * 50;
    double gg = t < 0.15 ? 150 + rng_next(&rnd) * 30 : 110 + rng_next(&rnd) * 50;
    double b = 40 + rng_next(&rnd) * 30;
    cv_stroke_rgba(g, js_i32(r), js_i32(gg), js_i32(b), 0.9);
    cv_line_width(g, 1 + rng_next(&rnd));
    cv_begin_path(g);
    cv_move_to(g, x, y);
    cv_line_to(g, x + cos(a) * l, y + sin(a) * l);
    cv_stroke(g);
  }
  Texture *t = canvas_texture(g, true, WRAP_REPEAT, 8);
  canvas_free(g);
  return t;
}

// Coral-limestone seawall: coursed blocks with pitted fossil faces and recessed joints.
static Texture *coral_stone_texture(void) {
  const int S = 1024;
  Canvas *g = canvas_new(S, S);
  Rng rnd = rng_make(6161);
  double bw = S / 4.0, bh = S / 8.0;   // 0.6 x 0.3 m blocks on a 2.4 m tile
  cv_fill_color(g, "rgb(150,136,112)");
  cv_fill_rect(g, 0, 0, S, S);
  for (int r = 0; r < 8; r++) {
    double off = r % 2 ? bw / 2 : 0;
    for (int k = -1; k < 5; k++) {
      double x = k * bw + off, v = 205 + rng_next(&rnd) * 30;
      cv_fill_rgba(g, js_i32(v), js_i32(v - 14), js_i32(v - 38), 1);
      cv_fill_rect(g, x + 5, r * bh + 5, bw - 10, bh - 10);
    }
  }
  for (int i = 0; i < 9000; i++) {   // fossil pits and pores
    double x = rng_next(&rnd) * S, y = rng_next(&rnd) * S;
    double rr = 0.6 + rng_next(&rnd) * rng_next(&rnd) * 5;
    double cr = 90 + rng_next(&rnd) * 40, cg = 80 + rng_next(&rnd) * 35, cb = 60 + rng_next(&rnd) * 30, ca = 0.35 + rng_next(&rnd) * 0.4;
    cv_fill_rgba(g, cr, cg, cb, ca);
    cv_begin_path(g);
    cv_arc(g, x, y, rr, 0, 6.28, false);
    cv_fill(g);
  }
  for (int i = 0; i < 3000; i++) {
    cv_fill_rgba(g, 250, 244, 230, 0.2 + rng_next(&rnd) * 0.3);
    double x = rng_next(&rnd) * S, y = rng_next(&rnd) * S, w = 1 + rng_next(&rnd) * 2, h = 1 + rng_next(&rnd) * 2;
    cv_fill_rect(g, x, y, w, h);
  }
  Texture *t = canvas_texture(g, true, WRAP_REPEAT, 8);
  canvas_free(g);
  return t;
}

static void concrete_joints(Canvas *ctx, int s, void *user) {   // 1.5 m slab joints (texture covers 3 m)
  (void)user;
  cv_stroke_color(ctx, "rgba(90,80,70,0.55)");
  cv_line_width(ctx, 3);
  const double ps[2] = { 0, s / 2.0 };
  for (int i = 0; i < 2; i++) {
    double p = ps[i];
    cv_begin_path(ctx); cv_move_to(ctx, p, 0); cv_line_to(ctx, p, s); cv_stroke(ctx);
    cv_begin_path(ctx); cv_move_to(ctx, 0, p); cv_line_to(ctx, s, p); cv_stroke(ctx);
  }
}

static void build_ground(Node *group) {
  const double Z = WORLD_Z;
  NoiseColorOpts co = noise_color_defaults();
  co.size = 512; co.seed = 11; co.base_cells = 6; co.speckle = 0.06;
  co.colorA[0] = 174; co.colorA[1] = 160; co.colorA[2] = 150;
  co.colorB[0] = 194; co.colorB[1] = 180; co.colorB[2] = 168;
  co.draw = concrete_joints;
  Texture *concrete_tex = noise_color_texture(&co);
  // (the JS also builds an asphalt noise texture and concrete / asphalt materials here that
  // nothing uses; street.js has its own. The asphalt texture consumed no shared state, so it
  // is not built.)

  MatDesc gd = md_standard();
  gd.name = "grass";
  gd.map = blade_texture();
  gd.roughness = 0.75;
  gd.prog[MV_PLAIN] = PROG_GRASS_TRANSLUCENT;
  Material *grass = mat_three(&gd);

  // Park lawn.
  node_add(group, mesh(slab(PARK.x0, PARK.wallX, -0.3, CURB_HEIGHT, -Z, Z, 7), grass, false, true));

  // Serpentine promenade.
  {
    const int n = 600;
    const double zs = 600, w = 2.2;
    DVec pts = {}, uvs = {};
    U32Vec idx = {};
    for (int i = 0; i <= n; i++) {
      double z = -zs + ((double)i / n) * zs * 2;
      double x = PARK.promenadeX + 2.6 * sin(z / 19) + 1.2 * sin(z / 7.3);
      double p[6] = { x - w, CURB_HEIGHT + 0.02, z, x + w, CURB_HEIGHT + 0.02, z };
      double u[4] = { (x - w) / 3, z / 3, (x + w) / 3, z / 3 };
      vec_append(&pts, p, 6);
      vec_append(&uvs, u, 4);
      if (i < n) {
        uint32_t a = (uint32_t)(i * 2);
        uint32_t q[6] = { a, a + 2, a + 1, a + 1, a + 2, a + 3 };
        vec_append(&idx, q, 6);
      }
    }
    Geometry *g = geo_new();
    geo_set_attr_d(g, "position", 3, (int)(pts.len / 3), pts.data);
    geo_set_attr_d(g, "uv", 2, (int)(uvs.len / 2), uvs.data);
    geo_set_index(g, idx.data, (int)idx.len);
    geo_compute_vertex_normals(g);
    vec_free(&pts); vec_free(&uvs); vec_free(&idx);
    MatDesc pd = md_standard();
    pd.name = "promenade";
    pd.map = concrete_tex;
    pd.color = color_hex(0xf2dcc8);
    pd.roughness = 0.85;
    pd.prog[MV_PLAIN] = PROG_STD_MAP;
    node_add(group, mesh(g, mat_three(&pd), false, true));
  }

  // Low coral-stone wall between park and sand (front faces into the shadow map).
  Texture *coral_tex = coral_stone_texture();
  MatDesc cd = md_standard();
  cd.name = "coral wall";
  cd.map = coral_tex;
  cd.color = color_hex(0xf2e6cf);
  cd.roughness = 0.95;
  cd.has_shadow_side = true;
  cd.shadow_side = SIDE_FRONT;
  cd.prog[MV_PLAIN] = PROG_CORAL_WALL;
  Material *coral = mat_three(&cd);
  MatDesc kd = md_standard();
  kd.name = "coral cap";
  kd.map = coral_tex;
  kd.color = color_hex(0xcfc2aa);
  kd.roughness = 0.9;
  kd.has_shadow_side = true;
  kd.shadow_side = SIDE_FRONT;
  kd.prog[MV_PLAIN] = PROG_STD_MAP;
  Material *cap = mat_three(&kd);
  // (texture covers 2.4 m: coursed blocks 0.6 x 0.3 m)
  node_add(group, mesh(slab(PARK.wallX - 0.25, PARK.wallX + 0.35, 0, 0.6, -Z, Z, 2.4), coral, true, true));
  node_add(group, mesh(slab(PARK.wallX - 0.3, PARK.wallX + 0.4, 0.6, 0.68, -Z, Z, 2.4), cap, true, true));
}

// ---- background towers ---------------------------------------------------------------------

typedef struct Bg {
  double x0, x1, z0, w, h, rot;
  int kind;
  bool round, hidden;
  // child (tier / rooftop box)
  int parent;           // -1: a building
  double lx, lz, sx, sz;
  bool tier;
  int above;
  double tierH;
} Bg;

typedef Vec(Bg) BgVec;

static int push_bg(BgVec *v, Bg b) {
  vec_push(v, b);
  return (int)v->len - 1;
}

static bool in_road(const BgVec *bg, int i) {
  const Bg *b = &bg->data[i];
  if (b->parent >= 0) return bg->data[b->parent].hidden;
  double reach = b->kind == 1 && b->x1 > -64 ? INFINITY : 110;
  if (b->x1 < -reach) return false;
  double slack = fabs(sin(b->rot)) * (b->x1 - b->x0);
  for (int k = 0; k < NCROSS_STREETS; k++) {
    const CrossStreet *c = &CROSS_STREETS[k];
    if ((c->far ? reach == INFINITY : true) && b->z0 - slack < c->z + CROSS.hw + 1 && b->z0 + b->w + slack > c->z - CROSS.hw - 1)
      return true;
  }
  return false;
}

static void build_background(Node *group) {
  Rng rnd = rng_make(1234);
  BgVec bg = {};
#define R() rng_next(&rnd)
  for (int i = 0; i < 90; i++) {
    double x1 = -62 - R() * 140;
    double w = 14 + R() * 30;
    double h = 14 + R() * R() * 55;
    Bg t = { .parent = -1 };
    t.x0 = x1 - (12 + R() * 25);
    t.x1 = x1;
    t.z0 = -1000 + R() * 2000;
    t.w = w;
    t.h = h;
    t.rot = (R() - 0.5) * 0.9;
    t.kind = h > 24 ? (R() < 0.75 ? 0 : 2) : 1;
    int ti = push_bg(&bg, t);
    // stepped crowns: a narrower upper tier or two; rounded ends on some slabs
    if (h > 30 && R() < 0.55) {
      push_bg(&bg, (Bg){ .parent = ti, .lx = 0.12, .lz = 0.15, .sx = 0.76, .sz = 0.7, .h = 3 + R() * 6, .kind = bg.data[ti].kind, .tier = true });
      if (R() < 0.5) push_bg(&bg, (Bg){ .parent = ti, .lx = 0.25, .lz = 0.3, .sx = 0.5, .sz = 0.4, .h = 3 + R() * 3, .kind = bg.data[ti].kind, .tier = true, .above = 1 });
    }
    if (h > 26 && R() < 0.35) bg.data[ti].round = true;
    // (the JS loop condition draws a new random number on every test)
    for (int k = 0; k < (h > 20 ? 1 + (int)floor(R() * 2) : 0); k++) {
      Bg c = { .parent = ti, .kind = 3 };
      c.lx = 0.2 + R() * 0.5;
      c.lz = 0.2 + R() * 0.5;
      c.sx = 0.15 + R() * 0.2;
      c.sz = 0.15 + R() * 0.25;
      c.h = 2 + R() * 3.5;
      push_bg(&bg, c);
    }
  }
  // the next street back: a continuous row of 2-6 storey buildings behind the hotels
  for (double z = -1000; z < 1000;) {
    double w = 12 + R() * 22;
    double x1 = -58 - R() * 4;
    Bg b = { .parent = -1, .kind = 1, .rot = 0 };
    b.x0 = x1 - (14 + R() * 16);
    b.x1 = x1;
    b.z0 = z;
    b.w = w;
    b.h = 7 + R() * R() * 16;
    push_bg(&bg, b);
    z += w + 1 + R() * 5;
  }

  MatDesc md = md_standard();
  md.name = "bg towers";
  md.roughness = 0.85;
  md.prog[MV_INSTANCED] = PROG_BG_TOWERS;
  Material *mat = mat_three(&md);
  static const uint32_t COLS[7] = { 0xf0ece4, 0xe9e3d8, 0xf2ede4, 0xe6e8e6, 0xeee6da, 0xe8ecee, 0xe4e6e2 };
  int n = (int)bg.len;
  Geometry *unit = geo_box1(1, 1, 1);
  geo_translate(unit, 0.5, 0.5, 0.5);
  float *aBg = xcalloc((size_t)n * 4, sizeof(float));
  Geometry *ug = geo_clone(unit);
  for (size_t i = 0; i < bg.len; i++)
    if (bg.data[i].tier && !bg.data[i].above) bg.data[bg.data[i].parent].tierH = bg.data[i].h;
  for (size_t i = 0; i < bg.len; i++) if (bg.data[i].parent < 0) bg.data[i].hidden = in_road(&bg, (int)i);
  Node *bm = node_instanced(nullptr, mat, n);
  const V3 Y = { 0, 1, 0 };
  for (int i = 0; i < n; i++) {
    const Bg *b = &bg.data[i];
    M4 m4;
    if (b->parent >= 0) {
      const Bg *p = &bg.data[b->parent];
      double d = p->x1 - p->x0;
      Quat bq = quat_axis_angle(Y, p->rot);
      V3 off = v3_apply_quat(v3(d * b->lx, p->h + (b->above ? p->tierH : 0), p->w * b->lz), bq);
      m4 = m4_compose(v3_add(v3(p->x0, 0, p->z0), off), bq, v3(d * b->sx, b->h, p->w * b->sz));
      inst_set_color(bm, i, color_hex(b->tier ? COLS[b->parent % 7] : 0xc9c8c2));
    } else {
      Quat bq = quat_axis_angle(Y, b->rot);
      m4 = m4_compose(v3(b->x0, 0, b->z0), bq, v3(b->x1 - b->x0, b->h, b->w));
      inst_set_color(bm, i, color_hex(COLS[i % 7]));
    }
    if (in_road(&bg, i)) m4 = m4_scaling(0, 0, 0);
    inst_set_matrix(bm, i, m4);
    double v1 = 2.9 + R() * 0.5, v2 = 1.4 + R() * 1.4, v3v = R();
    aBg[i * 4] = (float)b->kind;
    aBg[i * 4 + 1] = (float)v1;
    aBg[i * 4 + 2] = (float)v2;
    aBg[i * 4 + 3] = (float)v3v;
  }
  geo_set_iattr(ug, "aBg", 4, n, aBg);
  free(aBg);
  bm->geo = gpu_geometry(ug);
  geo_free(ug);
  // (not receiving: at a 7 deg sun the hotels' 180 m shadows would darken everything behind)
  bm->cast_shadow = true;
  bm->receive_shadow = false;
  snprintf(bm->name, sizeof bm->name, "bg towers");
  node_add(group, bm);

  // rounded ends on some slab towers (a half-cylinder bulging past the box end)
  int nr = 0;
  for (int i = 0; i < n; i++) nr += bg.data[i].round;
  if (nr) {
    Geometry *cyl = geo_cyl(0.5, 0.5, 1, 24);
    geo_translate(cyl, 0, 0.5, 0);
    Node *cm = node_instanced(nullptr, mat, nr);
    float *aR = xcalloc((size_t)nr * 4, sizeof(float));
    int k = 0;
    for (int i = 0; i < n; i++) {
      const Bg *b = &bg.data[i];
      if (!b->round) continue;
      double d = b->x1 - b->x0;
      Quat bq = quat_axis_angle(Y, b->rot);
      V3 off = v3_apply_quat(v3(d / 2, 0, 0), bq);
      M4 m4 = m4_compose(v3_add(v3(b->x0, 0, b->z0), off), bq, v3(d, b->h, d));
      if (b->hidden) m4 = m4_scaling(0, 0, 0);
      inst_set_matrix(cm, k, m4);
      inst_set_color(cm, k, color_hex(COLS[i % 7]));
      double r4 = R();
      aR[k * 4] = (float)b->kind;
      aR[k * 4 + 1] = 3.1f;
      aR[k * 4 + 2] = 1.6f;
      aR[k * 4 + 3] = (float)r4;
      k++;
    }
    geo_set_iattr(cyl, "aBg", 4, nr, aR);
    free(aR);
    cm->geo = gpu_geometry(cyl);
    geo_free(cyl);
    cm->cast_shadow = cm->receive_shadow = true;
    snprintf(cm->name, sizeof cm->name, "bg tower ends");
    node_add(group, cm);
  }
#undef R
  geo_free(unit);
  vec_free(&bg);
}

Node *build_placeholders(Node *scene) {
  Node *group = node_new(NODE_GROUP, "placeholders");
  build_ground(group);
  build_background(group);   // the deco row itself is built by hotels.c
  node_add(scene, group);
  return group;
}
