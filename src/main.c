// Ocean Drive (C23 + SDL3): boot, frame loop, --shot. Port of src/main.js (growing as the
// world modules are ported).
#include <stdlib.h>

#include "core/common.h"
#include "gfx/dfg_lut.h"
#include "gfx/post.h"
#include "gfx/scene.h"
#include "quality.h"
#include "gfx/shadow_mats.h"
#include "audio/audio.h"
#include "audio/wa.h"
#include "player/touch.h"
#include "player/walker.h"
#include "ui/ui.h"
#include "vehicles/vehicles.h"
#include "world/layout.h"
#include "world/beach.h"
#include "world/birds.h"
#include "world/car.h"
#include "world/hotels.h"
#include "world/lod.h"
#include "world/ocean.h"
#include "world/palms.h"
#include "world/people.h"
#include "world/placeholders.h"
#include "world/sky.h"
#include "world/street.h"

typedef struct Options {
  const char *shot;        // --shot PATH: headless single frame (?shot=1: frozen time, 'high')
  int width, height;       // --size WxH
  int frames;              // --frames N: quit after N frames (0 = run until closed)
  const char *quality;     // --quality low|medium|high
  bool ultra;              // --ultra
  bool has_cam;            // --cam px,py,pz,qx,qy,qz,qw[,fov]: exact camera (reference diffs)
  double cam[8];
  const char *dump;        // --dump NAME: print mesh checksums of a top-level object and exit
  const char *keep;        // --keep all|sky|name1,name2: dev-only, show only these top-level objects
                           // (and the sky dome), like tools/ref/ref-shot.mjs's keep argument
  const char *tier;        // --tier low|medium|high: dev-only, forces the tier even for --shot / --dump
  const char *people;      // --people closeup: (with --shot / --dump) the close-up placements
  bool vehicles;           // --vehicles: (with --shot) show the parked vehicles, like ?shot=1&vehicles
  bool no_dynres;          // --dynres 0: no dynamic resolution
  bool no_gpuguard;        // --gpuguard 0: no GPU time guard
  bool autostart;          // --autostart: walk and hear without the click (testing)
  bool hud;                // --hud: the fps / position line (?hud)
  bool no_fs;              // --nofs: no fullscreen on the first tap (?nofs)
  bool gpureset;           // --gpureset: this run follows a GPU reset (?gpureset)
  bool has_walk_at;        // --walk-at x,z,heading: dev, start the walker there
  double walk_at[3];
  const char *audio_test;  // --audio-test NAME[,SECONDS]: offline render of one sound, prints peak / RMS
} Options;

static Options parse_args(int argc, char **argv) {
  Options o = { .width = 1280, .height = 720 };
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (!strcmp(a, "--shot") && i + 1 < argc) o.shot = argv[++i];
    else if (!strcmp(a, "--size") && i + 1 < argc) {
      if (sscanf(argv[++i], "%dx%d", &o.width, &o.height) != 2 || o.width < 16 || o.height < 16)
        FATAL("--size expects WxH, e.g. 1024x576");
    } else if (!strcmp(a, "--frames") && i + 1 < argc) o.frames = atoi(argv[++i]);
    else if (!strcmp(a, "--quality") && i + 1 < argc) o.quality = argv[++i];
    else if (!strcmp(a, "--ultra")) o.ultra = true;
    else if (!strcmp(a, "--dump") && i + 1 < argc) o.dump = argv[++i];
    else if (!strcmp(a, "--tier") && i + 1 < argc) o.tier = argv[++i];
    else if (!strcmp(a, "--keep") && i + 1 < argc) o.keep = argv[++i];
    else if (!strcmp(a, "--audio-test") && i + 1 < argc) o.audio_test = argv[++i];
    else if (!strcmp(a, "--autostart")) o.autostart = true;
    else if (!strcmp(a, "--hud")) o.hud = true;
    else if (!strcmp(a, "--nofs")) o.no_fs = true;
    else if (!strcmp(a, "--gpureset")) o.gpureset = true;
    else if (!strcmp(a, "--walk-at") && i + 1 < argc) {
      if (sscanf(argv[++i], "%lf,%lf,%lf", &o.walk_at[0], &o.walk_at[1], &o.walk_at[2]) != 3) FATAL("--walk-at expects x,z,heading");
      o.has_walk_at = true;
    }
    else if (!strcmp(a, "--dynres") && i + 1 < argc) o.no_dynres = !strcmp(argv[++i], "0");
    else if (!strcmp(a, "--gpuguard") && i + 1 < argc) o.no_gpuguard = !strcmp(argv[++i], "0");
    else if (!strcmp(a, "--vehicles")) o.vehicles = true;
    else if (!strcmp(a, "--people") && i + 1 < argc) o.people = argv[++i];
    else if (!strcmp(a, "--cam") && i + 1 < argc) {
      o.cam[7] = 50;
      int n = sscanf(argv[++i], "%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf", &o.cam[0], &o.cam[1], &o.cam[2], &o.cam[3],
                     &o.cam[4], &o.cam[5], &o.cam[6], &o.cam[7]);
      if (n < 7) FATAL("--cam expects px,py,pz,qx,qy,qz,qw[,fov]");
      o.has_cam = true;
    } else FATAL("unknown argument: %s", a);
  }
  return o;
}

static const double FROZEN_TIME = 12.0;

typedef struct App {
  Options o;
  Node *scene;
  Sky *sky;
  Camera camera;
  Post *post;
  Hotels hotels;
  Palms *palms;
  Cars *cars;
  Surf *surf;
  Beach *beach;
  Ocean *ocean;
  Birds *birds;
  WalkWorld walk;
  Walker walker;
  Audio *audio;
  People *people;
  const WalkCircle *people_cols[8];   // (the vehicles keep this list: it outlives app_init)
  int npeople_cols;
  Vehicles *vehicles;
  int render_h;            // the scene render's pixel height (spray point sizes)
  double elapsed;
  double dt;               // this frame's clamped step (0 in shot mode)
  double now_s, shadow_at; // wall clock (s) and the last shadow-map render (SHADOW_GAP)
  Ui *ui;                  // the page UI (window runs only)
} App;

static bool frozen(const App *a);

static void render_frame(App *a, SDL_GPUCommandBuffer *cb) {
  if (!frozen(a)) {
    if (!vehicles_riding(a->vehicles)) walker_update(&a->walker, a->dt);
    vehicles_update(a->vehicles, a->dt);
    camera_update(&a->camera);   // (audio.update: camera.updateMatrixWorld)
    audio_update(a->audio, a->dt, &a->camera);   // SOUND: listener pose + scheduler
  }
  sky_update(a->sky, &a->camera);
  if (lod_update(&a->camera)) a->sky->shadow_wanted = true;   // DISTRICT
  surf_update(a->surf, a->elapsed);
  ocean_update(a->ocean, a->elapsed, &a->camera);
  beach_update(a->beach, a->elapsed, &a->camera);
  palms_update(a->palms, a->elapsed);
  camera_update(&a->camera);   // (birds.update: camera.updateMatrixWorld)
  birds_update(a->birds, a->dt, &a->camera);
  people_update(a->people, a->dt, &a->camera);   // PEOPLE
  // the moving cars follow the audio engine's car passes (none in shot mode)
  CarPass passes[16];
  int np = 0;
  if (!frozen(a)) {
    AudioCar *ac[16];
    int n = audio_get_cars(a->audio, ac, 16);
    for (int i = 0; i < n; i++)
      passes[np++] = (CarPass){ ac[i]->id, ac[i]->active, audio_car_progress(ac[i]), ac[i]->x, audio_car_z(ac[i]), ac[i]->dir, ac[i]->speed };
  }
  cars_update(a->cars, a->dt, passes, np);
  g_render_stats = (RenderStats){};   // renderer.info.reset()
  // (the static sun shadow map is re-rendered at most every 0.2 s while live)
  if (a->sky->shadow_wanted && (frozen(a) || g_no_cull || a->now_s - a->shadow_at >= 0.2)) {
    a->shadow_at = a->now_s;
    Texture *sm = a->sky->shadow_map;
    RenderTargetDesc rt = { .depth = sm->gpu, .depth_format = sm->format, .samples = SDL_GPU_SAMPLECOUNT_1,
                            .w = sm->w, .h = sm->h, .clear_depth = true, .depth_store = SDL_GPU_STOREOP_STORE };
    render_shadow(cb, a->scene, &a->sky->shadow_cam, &rt, shadow_depth_material, a);
    a->sky->shadow_wanted = false;
  }
  camera_update(&a->camera);
  sky_set_frame(a->sky, &a->camera);
  post_render(a->post, cb, a->scene, &a->camera, a->elapsed, g_sun_dir);
}

// ?shot=1 semantics (frozen time, deterministic schedules): --shot, and --dump, whose JS
// counterpart tools/ref/dump-group.mjs loads ?shot=1
static bool frozen(const App *a) { return a->o.shot != nullptr || a->o.dump != nullptr; }

static double walk_height(void *beach, double x, double z, double current_y) {
  return beach_height_at(beach, x, z, current_y);
}

// walking: the hotel terraces are raised, so the facade line stops the walker at the patio
// edge; everything else collides as boxes / circles
static void build_walk_world(App *a) {
  WalkWorld *W = &a->walk;
  W->height_at = walk_height;
  W->user = a->beach;
  int n;
  const StreetCollider *sc = street_colliders(&n);
  for (int i = 0; i < n; i++)
    if (sc[i].box) vec_push(&W->boxes, ((WalkBox){ sc[i].min, sc[i].max }));
  const CarBox *cb = cars_colliders(a->cars, &n);
  for (int i = 0; i < n; i++) vec_push(&W->boxes, ((WalkBox){ cb[i].min, cb[i].max }));
  const BeachBox *bb = beach_colliders(a->beach, &n);
  for (int i = 0; i < n; i++) vec_push(&W->boxes, ((WalkBox){ bb[i].min, bb[i].max }));
  for (int i = 0; i < a->hotels.nfootprints; i++) {
    const Footprint *f = &a->hotels.footprints[i];
    vec_push(&W->boxes, ((WalkBox){ v3(-80, -5, f->z0), v3(f->fx, 60, f->z1) }));
  }
  sc = street_colliders(&n);
  for (int i = 0; i < n; i++)
    if (!sc[i].box && sc[i].r != 0) vec_push(&W->circles, ((WalkCircle){ sc[i].x, sc[i].z, sc[i].r }));
  const PalmTree *pt = palm_trees(&n);
  for (int i = 0; i < n; i++)
    if (fabs(pt[i].z) < DISTRICT.zMax + 10) vec_push(&W->circles, ((WalkCircle){ pt[i].x, pt[i].z, 0.26 }));
  W->bounds.x0 = HOTEL.patioX + 0.2;
  W->bounds.x1 = 110;
  W->bounds.z0 = DISTRICT.zMin;
  W->bounds.z1 = DISTRICT.zMax;
  W->bounds.soft = 14;
}

// ---- SOUND hooks ----
static bool gull_source(void *user, const Listener *L, GullSrc *out) {
  BirdSource s;
  if (!birds_gull_source(user, v3(L->x, L->y, L->z), &s)) return false;
  *out = (GullSrc){ s.x, s.y, s.z, s.vx, s.vy, s.vz };
  return true;
}
static void bird_flutter(const BirdFlutter *f, void *user) {
  GullSrc p = { f->x, f->y, f->z, f->vx, f->vy, f->vz };
  audio_wing_flutter(user, &p);
}
// visuals follow the audio's wave schedule (audio clock -> render clock)
static void on_wave(const AudioWave *w, void *user) {
  App *a = user;
  surf_push_audio_wave(a->surf, a->elapsed + (w->t - w->now), w->k, w->size, w->runup, w->z);
}
// footstep surface: audio's map, refined by the beach (deck, stairs, damp sand, swash)
static void on_step(const WalkStep *st, void *user) {
  App *a = user;
  double x = st->x, z = st->z, feet = st->feet_y;
  double ground = beach_ground_at(x, z);
  Surface surface;
  double depth = NAN;
  bool on_tower = false;
  for (int i = 0; i < 3; i++)
    if (fabs(x - TOWERS[i].x) < 9 && fabs(z - TOWERS[i].z) < 5) on_tower = true;
  if (feet - ground > 0.25 && on_tower) surface = SURF_WOOD;
  else if (feet - ground > 0.12 && x > 10.5 && x < 12.8) surface = SURF_PAVEMENT;   // seawall steps
  else {
    surface = surface_at(x, z, feet, SAND.waterline);
    if (x > SAND.x0 + 0.5) {
      double t = surf_time(a->surf);
      double d = surf_water_depth_at(a->surf, x, z, t);
      if (d > 0.015) { surface = SURF_SPLASH; depth = d; }
      else surface = x > WET_LINE_X - 3.5 || surf_swash_at(a->surf, x, z, t).covered ? SURF_WETSAND : SURF_SAND;
    }
  }
  double gain = st->land ? 1.3 : fmin(1.25, 0.8 + st->speed * 0.1);   // a jump lands a little harder
  audio_footstep(a->audio, surface, gain * (surface == SURF_SPLASH ? 1.1 : 1), depth);
}

// the cyclist waits for a clear road (no cars in shot mode)
static int people_cars(void *user, CarPass *out, int max) {
  App *a = user;
  if (frozen(a)) return 0;
  AudioCar *ac[16];
  int n = audio_get_cars(a->audio, ac, max < 16 ? max : 16);
  for (int i = 0; i < n; i++)
    out[i] = (CarPass){ ac[i]->id, ac[i]->active, audio_car_progress(ac[i]), ac[i]->x, audio_car_z(ac[i]), ac[i]->dir, ac[i]->speed };
  return n;
}

// LOADER hook: report a build phase to the loading screen and paint one loader frame between the
// synchronous build steps (fraction < 0: only the frame, like a bare nextFrame())
static void load_step(App *a, double fraction, const char *label) {
  if (!a->ui) return;
  if (fraction >= 0) ui_load_step(a->ui, fraction, label);
  else ui_paint(a->ui);
}

static void app_init(App *a, int w, int h) {
  layout_init();
  renderer_init();
  frame_set_texture(GT_DFG_LUT, dfg_lut_texture());
  a->scene = node_new(NODE_GROUP, "scene");
  load_step(a, 0.1, "Raising the sun…");   // LOADER
  a->sky = sky_create(a->scene, QUALITY.shadowMap, QUALITY.shadowStep);
  load_step(a, -1, nullptr);
  build_placeholders(a->scene);
  load_step(a, 0.22, "Painting the hotels…");
  a->hotels = build_hotels(a->scene);
  load_step(a, 0.4, "Planting palms…");
  a->palms = build_palms(a->scene);
  load_step(a, 0.5, "Laying Ocean Drive…");
  build_street(a->scene);
  load_step(a, -1, nullptr);
  a->cars = build_cars(a->scene, a->sky->env);
  load_step(a, 0.6, "Pouring the ocean…");
  a->surf = surf_create(frozen(a), FROZEN_TIME);
  a->beach = build_beach(a->scene, a->surf);
  load_step(a, -1, nullptr);
  a->ocean = create_ocean(a->scene, a->surf);
  a->birds = create_birds(a->scene, a->surf, frozen(a));
  load_step(a, 0.72, "Tuning the waves…");
  a->camera = (Camera){ .node = node_new(NODE_GROUP, "camera"), .fov = 50, .aspect = (double)w / h,
                        .near = 0.1, .far = 30000, .zoom = 1 };
  a->camera.node->rotation.order = EULER_YXZ;
  build_walk_world(a);
  walker_init(&a->walker, &a->camera, &a->walk);
  // first frame: hotel sidewalk, looking up the row of sunlit fronts
  walker_set(&a->walker, -26, CURB_HEIGHT + EYE_HEIGHT, 40, 342, 4);
  // SOUND: audio starts on the click-to-start gesture; never in shot mode
  a->audio = create_audio(QUALITY.audioReduced);
  audio_set_gull_source(a->audio, gull_source, a->birds);   // BIRDS: gull calls come from visible gulls
  birds_on_flutter(a->birds, bird_flutter, a->audio);       // BIRDS: wingbeats of a gull taking off nearby
  audio_set_auto_steps(a->audio, false);                    // the walker drives the footsteps
  a->walker.on_step = on_step;
  a->walker.step_user = a;
  audio_on_wave(a->audio, on_wave, a);
  // PEOPLE: a few procedural passers-by; their circle colliders move with them
  a->people = build_people(a->scene, &a->hotels, &a->walker, frozen(a), a->o.people, people_cars, a);
  a->npeople_cols = people_colliders(a->people, a->people_cols, 8);
  for (int i = 0; i < a->npeople_cols; i++) vec_push(&a->walk.moving, a->people_cols[i]);
  // rideable beach cruiser and lifeguard ATV (E to ride); parked colliders block the walker
  load_step(a, -1, nullptr);
  a->render_h = h;
  a->vehicles = create_vehicles(a->scene, (VehiclesOpts){
    .walker = &a->walker, .camera = &a->camera, .surf = a->surf,
    .static_boxes = a->walk.boxes.data, .nstatic_boxes = (int)a->walk.boxes.len,
    .static_circles = a->walk.circles.data, .nstatic_circles = (int)a->walk.circles.len,
    .dynamic_circles = a->people_cols, .ndynamic = a->npeople_cols, .audio = a->audio,
    .shot = frozen(a) && !a->o.vehicles,   // ?shot=1&vehicles: show them for close-ups
    .shadow_wanted = &a->sky->shadow_wanted, .render_height = &a->render_h });
  const WalkCircle *vc[8];
  int nvc = vehicles_colliders(a->vehicles, vc, 8);
  for (int i = 0; i < nvc; i++) vec_push(&a->walk.moving, vc[i]);
  a->post = post_create(w, h, (PostOptions){ .bloom_strength = 0.16, .samples = QUALITY.msaa, .bloom = QUALITY.bloom,
                                             .fxaa = QUALITY.fxaa, .exposure = 0.62 });
}

// the checksums tools/ref/dump-group.mjs prints for the JS scene (same order and fields)
static double fsum(const float *a, size_t n) {
  double s = 0;
  for (size_t i = 0; i < n; i++) s += a[i];
  return s;
}
// FNV-1a over 32-bit words (float bits / index values), like dump-group.mjs's ph / ih
static uint32_t fnv32(const void *p, size_t n) {
  const uint32_t *w = p;
  uint32_t h = 0x811c9dc5u;
  for (size_t i = 0; i < n; i++) h = (h ^ w[i]) * 16777619u;
  return h;
}
static void dump_node(const Node *n, const char *path) {
  if ((n->kind == NODE_MESH || n->kind == NODE_INSTANCED || n->kind == NODE_BATCHED || n->kind == NODE_SKINNED) && n->geo) {
    const Geometry *g = n->geo->cpu;
    CHECK(g);
    const GeoAttr *pa = geo_attr(g, "position"), *na = geo_attr(g, "normal"), *ca = geo_attr(g, "color"), *ua = geo_attr(g, "uv");
    printf("%s %s v=%d i=%d pos=%.17g nrm=%.17g col=%.17g uv=%.17g cast=%d recv=%d ro=%d", path,
           n->kind == NODE_INSTANCED ? "inst" : n->kind == NODE_BATCHED ? "batch" : "mesh", g->count, g->index ? g->index_count : 0,
           pa ? fsum(pa->data, (size_t)g->count * 3) : 0, na ? fsum(na->data, (size_t)g->count * 3) : 0,
           ca ? fsum(ca->data, (size_t)g->count * 3) : 0, ua ? fsum(ua->data, (size_t)g->count * 2) : 0,
           n->cast_shadow, n->receive_shadow, n->render_order);
    const char *extra[4] = { "aW", "aE", "aB", "aWin" };
    for (int k = 0; k < 4; k++) {
      const GeoAttr *x = geo_attr(g, extra[k]);
      if (x) printf(" %s=%.17g", extra[k], fsum(x->data, (size_t)g->count * x->size));
      for (int j = 0; j < g->niattr; j++)
        if (!strcmp(g->iattr[j].name, extra[k]))
          printf(" %s=%.17g", extra[k], fsum(g->iattr[j].data, (size_t)g->iattr[j].count * g->iattr[j].size));
    }
    for (int j = 0; j < g->niattr; j++)   // every InstancedBufferAttribute, in insertion order
      printf(" %s=%.17g", g->iattr[j].name, fsum(g->iattr[j].data, (size_t)g->iattr[j].count * g->iattr[j].size));
    if (n->kind == NODE_INSTANCED) {
      double m = 0;
      for (int i = 0; i < n->inst_count; i++)
        for (int k = 0; k < 16; k++) m += (float)n->inst_matrix[i].e[k];
      printf(" n=%d m=%.17g ic=%.17g", n->inst_count, m, n->inst_color ? fsum(n->inst_color, (size_t)n->inst_count * 3) : 0);
    }
    if (n->kind == NODE_BATCHED) {
      size_t ms = (size_t)n->batch_mat_size * n->batch_mat_size * 4, cs = (size_t)n->batch_id_size * n->batch_id_size * 4;
      printf(" n=%d m=%.17g ic=%.17g", n->batch_ninst, fsum(n->batch_matrix_data, ms),
             n->batch_color_data ? fsum(n->batch_color_data, cs) : 0);
    }
    if (n->kind == NODE_SKINNED) {
      if (n->visible) skeleton_update(n->skeleton);   // (three fills boneMatrices when it renders the mesh)
      printf(" bm=%.17g", fsum(n->skeleton->matrices, (size_t)n->skeleton->size * n->skeleton->size * 4));
    }
    printf(" ph=%u ih=%u\n", pa ? fnv32(pa->data, (size_t)g->count * 3) : fnv32(nullptr, 0),
           g->index ? fnv32(g->index, (size_t)g->index_count) : fnv32(nullptr, 0));
  }
  for (size_t i = 0; i < n->children.len; i++) {
    char p[256];
    snprintf(p, sizeof p, "%s/%zu", path, i);
    dump_node(n->children.data[i], p);
  }
}

// '--dump @ride': scripted vehicle rides (vehicles.simulate), for tools/ref/ride-test.mjs
static void ride_print(const char *tag, const Vehicle *v, double ms, double mb) {
  if (!v) { printf("%s none\n", tag); return; }
  printf("%s x=%.17g z=%.17g yaw=%.17g lon=%.17g gy=%.17g by=%.17g pitch=%.17g roll=%.17g wr=%.17g crank=%.17g rpm=%.17g ms=%.17g mb=%.17g\n", tag,
         v->x, v->z, v->yaw, v->lon, v->ground_y, v->body_y, v->pitch, v->roll, v->wheel_rot, v->crank, v->rpm, ms, mb);
}
static int ride_test(App *a) {
  Vehicles *V = a->vehicles;
  double ms, mb;
  bool k[WK_COUNT] = {};
#define KEYS(...) do { memset(k, 0, sizeof k); int ks_[] = { __VA_ARGS__ }; for (size_t i_ = 0; i_ < ARRAY_LEN(ks_); i_++) if (ks_[i_] >= 0) k[ks_[i_]] = true; } while (0)
  vehicles_mount(V, VK_BIKE);
  KEYS(WK_W); vehicles_simulate(V, k, 6, 1.0 / 60, &ms, &mb); ride_print("bike1", vehicles_current(V), ms, mb);
  KEYS(WK_W, WK_SHIFT_L, WK_D); vehicles_simulate(V, k, 4, 1.0 / 60, &ms, &mb); ride_print("bike2", vehicles_current(V), ms, mb);
  vehicles_place(V, 20, 0, -PI_D / 2, -1);
  KEYS(WK_W); vehicles_simulate(V, k, 8, 1.0 / 60, &ms, &mb); ride_print("bike3", vehicles_current(V), ms, mb);
  KEYS(WK_W, WK_A); vehicles_simulate(V, k, 3, 1.0 / 60, &ms, &mb); ride_print("bike4", vehicles_current(V), ms, mb);
  KEYS(WK_W, WK_SPACE); vehicles_simulate(V, k, 2, 1.0 / 60, &ms, &mb); ride_print("bike5", vehicles_current(V), ms, mb);
  KEYS(WK_S); vehicles_simulate(V, k, 3, 1.0 / 60, &ms, &mb); ride_print("bike6", vehicles_current(V), ms, mb);
  printf("dismount %d\n", vehicles_dismount(V));
  vehicles_mount(V, VK_ATV);
  KEYS(WK_W, WK_SHIFT_L); vehicles_simulate(V, k, 8, 1.0 / 60, &ms, &mb); ride_print("atv1", vehicles_current(V), ms, mb);
  KEYS(WK_W, WK_D); vehicles_simulate(V, k, 5, 1.0 / 60, &ms, &mb); ride_print("atv2", vehicles_current(V), ms, mb);
  KEYS(WK_S); vehicles_simulate(V, k, 3, 1.0 / 60, &ms, &mb); ride_print("atv3", vehicles_current(V), ms, mb);
  vehicles_place(V, 60, 30, -PI_D / 2, -1);
  KEYS(WK_W, WK_SPACE); vehicles_simulate(V, k, 6, 1.0 / 60, &ms, &mb); ride_print("atv4", vehicles_current(V), ms, mb);
  KEYS(-1); vehicles_simulate(V, k, 2, 1.0 / 60, &ms, &mb); ride_print("atv5", vehicles_current(V), ms, mb);
#undef KEYS
  return 0;
}

// '--dump @walk': scripted walker runs (Walker.simulate), for tools/ref/walk-test.mjs
static void count_step(const WalkStep *s, void *user) { (void)s; ++*(int *)user; }
static int walk_test(App *a) {
  Walker *w = &a->walker;
  typedef struct { bool tele; double x, y, z, h, p; WalkKey k[3]; int nk; double secs; } Run;
  static const Run RUNS[] = {
    { false, -26, 1.85, 40, 342, 4, { WK_W }, 1, 8 },
    { false, -20, 1.85, 0, 90, 0, { WK_W }, 1, 25 },
    { true, 60, 0, -10, 90, 0, { WK_W, WK_SHIFT_L }, 2, 20 },
    { true, 40, 0, 13, 35, 0, { WK_W }, 1, 10 },
    { false, -26, 1.85, 0, 270, 0, { WK_W }, 1, 4 },
    { false, -26, 1.85, 40, 180, 0, { WK_W, WK_SPACE }, 2, 3 },
    { false, -15, 1.85, 320, 180, 0, { WK_W }, 1, 10 },
    { false, -18, 1.85, -60, 45, 0, { WK_D, WK_S }, 2, 6 },
  };
  printf("world boxes=%zu circles=%zu\n", a->walk.boxes.len, a->walk.circles.len);
  int steps = 0;
  w->on_step = count_step;
  w->step_user = &steps;
  for (size_t i = 0; i < ARRAY_LEN(RUNS); i++) {
    const Run *r = &RUNS[i];
    if (r->tele) walker_teleport(w, r->x, r->z, r->h, r->p, -INFINITY);
    else walker_set(w, r->x, r->y, r->z, r->h, r->p);
    bool keys[WK_COUNT] = {};
    for (int k = 0; k < r->nk; k++) keys[r->k[k]] = true;
    steps = 0;
    walker_simulate(w, keys, r->secs, 1.0 / 60);
    V3 c = w->camera->node->position;
    printf("run%zu x=%.17g z=%.17g feet=%.17g phase=%.17g steps=%d cam=%.17g,%.17g,%.17g\n", i, w->pos.x, w->pos.y, w->feet_y,
           w->phase, steps, c.x, c.y, c.z);
  }
  w->on_step = nullptr;
  w->step_user = nullptr;
  return 0;
}

static int run_dump(const Options *o) {
  gpu_init("Ocean Drive", 320, 200, true);
  quality_init(o->tier ? o->tier : o->quality, o->ultra, !o->tier);
  g_program_tier = quality_program_tier();
  g_keep_cpu_geometry = true;
  App a = { .o = *o };
  app_init(&a, 320, 200);
  node_update_matrix_world(a.scene, false);   // (the JS dump reads the scene after it has rendered)
  if (!strcmp(o->dump, "@walk")) return walk_test(&a);
  if (!strcmp(o->dump, "@ride")) return ride_test(&a);
  // '@cars': the cars' top-level nodes (unnamed in the JS), numbered like dump-group.mjs's '@unnamed'
  int u = 0;
  for (size_t i = 0; i < a.scene->children.len; i++) {
    Node *n = a.scene->children.data[i];
    if (!strcmp(o->dump, "@vehicles")) {
      if (!strcmp(n->name, "vehicle") || !strcmp(n->name, "vehicle-shadow") || !strcmp(n->name, "spray")) { char p[32]; snprintf(p, sizeof p, "v/%d", u++); dump_node(n, p); }
      continue;
    }
    if (!strcmp(o->dump, "@skinned")) {
      if (n->kind == NODE_SKINNED) { char p[32]; snprintf(p, sizeof p, "s/%d", u++); dump_node(n, p); }
      continue;
    }
    if (!strcmp(o->dump, "@cars") || !strcmp(o->dump, "@beach")) {
      bool car = !strcmp(n->name, "car") || !strcmp(n->name, "sedan") || !strcmp(n->name, "fleet") || !strcmp(n->name, "fleet block");
      bool beach = !strcmp(n->name, "beach") || !strcmp(n->name, "swash");
      if (!strcmp(o->dump, "@cars") ? !car : !beach) continue;
      char p[32];
      snprintf(p, sizeof p, "u/%d", u++);
      dump_node(n, p);
    } else if (!strcmp(n->name, o->dump)) dump_node(n, o->dump);
  }
  return 0;
}

static int run_shot(const Options *o) {
  gpu_init("Ocean Drive", o->width, o->height, true);
  quality_init(o->tier ? o->tier : o->quality, o->ultra, !o->tier);
  g_program_tier = quality_program_tier();
  App a = { .o = *o };
  app_init(&a, o->width, o->height);
  a.elapsed = FROZEN_TIME;
  if (o->keep && strcmp(o->keep, "all")) {
    for (size_t i = 0; i < a.scene->children.len; i++) {
      Node *n = a.scene->children.data[i];
      if (n == a.sky->dome) continue;
      bool keep = false;
      if (strcmp(o->keep, "sky")) {   // a comma list of names
        size_t len = strlen(n->name);
        for (const char *k = o->keep; *k;) {
          size_t kl = strcspn(k, ",");
          if (kl == len && !strncmp(k, n->name, len)) keep = true;
          // 'cars': the car module's nodes (unnamed in the JS)
          if (kl == 5 && !strncmp(k, "beach", 5) && (!strcmp(n->name, "beach") || !strcmp(n->name, "swash"))) keep = true;
          if (kl == 6 && !strncmp(k, "people", 6) && !strncmp(n->name, "person", 6)) keep = true;
          if (kl == 8 && !strncmp(k, "vehicles", 8) && (!strcmp(n->name, "vehicle") || !strcmp(n->name, "vehicle-shadow") || !strcmp(n->name, "spray"))) keep = true;
          if (kl == 4 && !strncmp(k, "cars", 4) &&
              (!strcmp(n->name, "car") || !strcmp(n->name, "sedan") || !strcmp(n->name, "fleet") || !strcmp(n->name, "fleet block")))
            keep = true;
          k += kl + (k[kl] == ',');
        }
      }
      if (!keep) n->visible = false;   // (like the JS: only hides)
    }
  }
  if (o->has_cam) {
    a.camera.node->position = v3(o->cam[0], o->cam[1], o->cam[2]);
    node_set_quaternion(a.camera.node, (Quat){ o->cam[3], o->cam[4], o->cam[5], o->cam[6] });
    a.camera.fov = o->cam[7];
  }
  SDL_GPUCommandBuffer *cb = SDL_AcquireGPUCommandBuffer(g_gpu.dev);
  render_frame(&a, cb);
  SDL_CHECK(SDL_SubmitGPUCommandBuffer(cb));
  Texture *scr = post_screen(a.post);
  size_t row = (size_t)scr->w * 4;
  uint8_t *px = malloc(row * (size_t)scr->h), *img = malloc(row * (size_t)scr->h);
  CHECK(px && img);
  gpu_read_rgba8(scr->gpu, scr->w, scr->h, px);
  for (int y = 0; y < scr->h; y++) memcpy(img + (size_t)y * row, px + (size_t)(scr->h - 1 - y) * row, row);   // GL rows -> image
  bool ok = save_bmp_rgba8(o->shot, img, scr->w, scr->h);
  LOG(ok ? "wrote %s (%dx%d)" : "could not write %s", o->shot, scr->w, scr->h);
  free(px);
  free(img);
  renderer_shutdown();
  gpu_shutdown();
  return ok ? 0 : 1;
}

// ---- render scale (main.js): the tier's base scale, stepped down / up by the dynamic resolution;
// the base pixel ratio respects both the tier's DPR cap and its pixel budget ----
typedef struct Pacing {
  double render_scale, start_scale;
  struct {
    bool on;
    double min, max, step, acc, slow, fast, hold, clock;
    int n;
    struct { int key; double until, backoff; } failed[32];
    int nfailed;
  } dyn;
  // GPU time guard: frames' GPU busy time, estimated from their fences
  struct { int n, slow; double worst, last; long since; } guard;
  struct FenceWatch *watch;
  uint64_t last_done_ns;
} Pacing;

// A thread waits on each frame's fence in submission order and stamps its completion, so a stall
// of the main thread between frames is never taken for GPU time. (SDL: fences may be waited on
// from any thread; they are released on the main thread.)
typedef struct Watched { SDL_GPUFence *fence; uint64_t submit_ns, done_ns; long tag; } Watched;
typedef struct FenceWatch {
  SDL_Mutex *mu;
  SDL_Condition *cv;
  SDL_Thread *th;
  bool quit;
  Watched pending[16], done[16];
  int npending, ndone;
} FenceWatch;

static int fence_thread(void *user) {
  FenceWatch *w = user;
  SDL_LockMutex(w->mu);
  for (;;) {
    while (!w->quit && w->npending == 0) SDL_WaitCondition(w->cv, w->mu);
    if (w->npending == 0) break;   // (quit with nothing left to wait for)
    SDL_GPUFence *f = w->pending[0].fence;
    SDL_UnlockMutex(w->mu);
    bool ok = SDL_WaitForGPUFences(g_gpu.dev, true, &f, 1);
    uint64_t now = SDL_GetTicksNS();
    SDL_LockMutex(w->mu);
    Watched x = w->pending[0];
    memmove(w->pending, w->pending + 1, (size_t)(w->npending - 1) * sizeof x);
    w->npending--;
    x.done_ns = ok ? now : 0;
    if (w->ndone < 16) w->done[w->ndone++] = x;
    else SDL_Log("fence watch: result queue full");   // (never: the main thread drains it every frame)
  }
  SDL_UnlockMutex(w->mu);
  return 0;
}

static FenceWatch *fence_watch_start(void) {
  FenceWatch *w = xcalloc(1, sizeof *w);
  w->mu = SDL_CreateMutex();
  w->cv = SDL_CreateCondition();
  w->th = SDL_CreateThread(fence_thread, "gpu-guard", w);
  if (!w->mu || !w->cv || !w->th) FATAL("fence watch: %s", SDL_GetError());
  return w;
}
// hands a submitted frame's fence to the watcher (or releases it when the watcher is behind)
static void fence_watch_add(FenceWatch *w, SDL_GPUFence *f, uint64_t submit_ns, long tag) {
  SDL_LockMutex(w->mu);
  bool room = w->npending < 16;
  if (room) { w->pending[w->npending++] = (Watched){ f, submit_ns, 0, tag }; SDL_SignalCondition(w->cv); }
  SDL_UnlockMutex(w->mu);
  if (!room) SDL_ReleaseGPUFence(g_gpu.dev, f);
}
// the completed frames since the last call (their fences are the caller's to release)
static int fence_watch_take(FenceWatch *w, Watched *out) {
  SDL_LockMutex(w->mu);
  int n = w->ndone;
  memcpy(out, w->done, (size_t)n * sizeof *out);
  w->ndone = 0;
  SDL_UnlockMutex(w->mu);
  return n;
}
static void fence_watch_stop(FenceWatch *w) {
  SDL_LockMutex(w->mu);
  w->quit = true;
  SDL_SignalCondition(w->cv);
  SDL_UnlockMutex(w->mu);
  SDL_WaitThread(w->th, nullptr);
  for (int i = 0; i < w->ndone; i++) SDL_ReleaseGPUFence(g_gpu.dev, w->done[i].fence);
  SDL_DestroyCondition(w->cv);
  SDL_DestroyMutex(w->mu);
  free(w);
}

static double base_dpr(void) {
  int cw = 0, ch = 0;
  SDL_GetWindowSize(g_gpu.win, &cw, &ch);   // CSS pixels: the window's size in points
  double dpr = SDL_GetWindowPixelDensity(g_gpu.win);
  if (!(dpr > 0)) dpr = 1;
  return fmax(0.5, fmin(fmin(dpr, QUALITY.maxDpr), sqrt(QUALITY.maxPixels / ((double)cw * ch))));
}

static int *dyn_failed(Pacing *P, double scale, bool create) {   // index into failed[] or null
  static int idx;
  int key = (int)js_round(scale * 100);
  for (int i = 0; i < P->dyn.nfailed; i++) if (P->dyn.failed[i].key == key) { idx = i; return &idx; }
  if (!create || P->dyn.nfailed == 32) return nullptr;
  idx = P->dyn.nfailed++;
  P->dyn.failed[idx] = (typeof(P->dyn.failed[0])){ key, 0, 0 };
  return &idx;
}
static void set_render_scale(Pacing *P, double s) { P->render_scale = js_round(s * 100) / 100; }

// Dynamic resolution: if frames average over ~22 ms (under ~45 fps) for 2 s, step the render
// scale down (to 0.6 at least); step back up after a long calm stretch. A scale that had to be
// abandoned is not retried for a while (and longer each time), so it never oscillates.
static void dynamic_resolution(Pacing *P, double raw_dt) {
  bool hidden = SDL_GetWindowFlags(g_gpu.win) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_OCCLUDED);
  if (!P->dyn.on || hidden) return;
  P->dyn.clock += raw_dt;
  if (P->dyn.hold > 0) { P->dyn.hold -= raw_dt; return; }   // settle after start / a change
  if (raw_dt > 0.25) return;                                // a hitch, not load
  P->dyn.acc += raw_dt; P->dyn.n++;
  if (P->dyn.acc < 0.5) return;
  double avg = P->dyn.acc / P->dyn.n;                        // mean frame time over ~0.5 s
  P->dyn.acc = 0; P->dyn.n = 0;
  P->dyn.slow = avg > 0.022 ? P->dyn.slow + 0.5 : 0;
  P->dyn.fast = avg < 0.0185 ? P->dyn.fast + 0.5 : 0;
  if (P->dyn.slow >= 2 && P->render_scale > P->dyn.min + 1e-3) {
    // the scale we just left was too heavy: don't retry it for a while (longer each time)
    int *f = dyn_failed(P, P->render_scale, false);
    double backoff = f ? fmin(P->dyn.failed[*f].backoff * 2, 300) : 15;
    f = dyn_failed(P, P->render_scale, true);
    if (f) { P->dyn.failed[*f].until = P->dyn.clock + backoff; P->dyn.failed[*f].backoff = backoff; }
    P->dyn.slow = P->dyn.fast = 0; P->dyn.hold = 1.5;
    set_render_scale(P, fmax(P->dyn.min, P->render_scale - P->dyn.step));
  } else if (P->dyn.fast >= 4 && P->render_scale < P->dyn.max - 1e-3) {
    double next = fmin(P->dyn.max, P->render_scale + P->dyn.step / 2);
    int *f = dyn_failed(P, next, false);
    if (P->dyn.clock > (f ? P->dyn.failed[*f].until : 0)) {
      P->dyn.slow = P->dyn.fast = 0; P->dyn.hold = 2;
      set_render_scale(P, next);
    }
  }
}

// GPU time guard: a single frame over 250 ms or three in a row over 50 ms step the render scale
// and the sun shadow map down at once. (WebGL's timer query has no SDL_GPU counterpart: a frame's
// GPU time is its fence's completion, stamped by the watcher thread, minus the later of its
// submission and the previous frame's completion.)
static void gpu_guard(Pacing *P, App *a, long frames) {
  Watched done[16];
  int n = fence_watch_take(P->watch, done);
  for (int i = 0; i < n; i++) {
    SDL_ReleaseGPUFence(g_gpu.dev, done[i].fence);
    if (!done[i].done_ns) continue;   // (the wait failed)
    uint64_t start = done[i].submit_ns > P->last_done_ns ? done[i].submit_ns : P->last_done_ns;
    double ms = done[i].done_ns > start ? (double)(done[i].done_ns - start) / 1e6 : 0;
    long tag = done[i].tag;
    P->last_done_ns = done[i].done_ns;
    P->guard.n++; P->guard.last = ms; P->guard.worst = fmax(P->guard.worst, ms);
    if (tag < P->guard.since) continue;   // the first frames, or drawn before the last step down
    P->guard.slow = ms > 50 ? P->guard.slow + 1 : 0;
    if (ms <= 250 && P->guard.slow < 3) continue;
    P->guard.slow = 0;
    P->guard.since = frames + 1;
    if (P->render_scale > P->dyn.min + 1e-3) {
      int *f = dyn_failed(P, P->render_scale, false);
      double backoff = f ? fmin(P->dyn.failed[*f].backoff * 2, 600) : 60;
      f = dyn_failed(P, P->render_scale, true);
      if (f) { P->dyn.failed[*f].until = P->dyn.clock + backoff; P->dyn.failed[*f].backoff = backoff; }
      P->dyn.slow = P->dyn.fast = 0; P->dyn.hold = 2;
      set_render_scale(P, fmax(P->dyn.min, P->render_scale - P->dyn.step));
    }
    sky_lower_shadow(a->sky);
    LOG("gpu guard: frame %ld took %.0f ms: render scale %.2f", tag, ms, P->render_scale);
  }
}

// the render size for the window: floor(css * pixelRatio), like WebGLRenderer.setSize
static void render_size(const Pacing *P, int *w, int *h, double *aspect) {
  int cw = 0, ch = 0;
  SDL_GetWindowSize(g_gpu.win, &cw, &ch);
  double pr = base_dpr() * P->render_scale * P->start_scale;
  *w = (int)fmax(1, floor(cw * pr));
  *h = (int)fmax(1, floor(ch * pr));
  *aspect = (double)cw / (ch ? ch : 1);
}

// Warm-up (main.js): tiny-resolution renders of everything with culling off, so every pipeline
// and shadow-depth program exists before the first real frame
static void warm_up(App *a) {
  post_resize(a->post, 160, 90);
  g_no_cull = true;
  a->sky->shadow_wanted = true;
  SDL_GPUCommandBuffer *cb = SDL_AcquireGPUCommandBuffer(g_gpu.dev);
  double dt = a->dt;
  a->dt = 0;
  render_frame(a, cb);
  a->dt = dt;
  SDL_GPUFence *f = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
  if (f) { SDL_WaitForGPUFences(g_gpu.dev, true, &f, 1); SDL_ReleaseGPUFence(g_gpu.dev, f); }
  g_no_cull = false;
  a->sky->shadow_wanted = true;   // (renderer.shadowMap.needsUpdate = true)
}

// the page's input state (main.js UI): the overlay, the touch controls
typedef struct Page {
  App *a;
  bool overlay;       // #overlay shown
  Touch touch;
  bool has_touch;     // createTouchControls ran (html.touch)
} Page;

static void begin(Page *pg) {   // audio.start(); controls.active = true
  audio_start(pg->a->audio);
  pg->a->walker.active = true;
}
static void ride_toggle(void *user) { vehicles_toggle(((App *)user)->vehicles); }
// TOUCH: joystick + drag-look after the first touch
static void enable_touch(Page *pg) {
  if (pg->has_touch) return;
  touch_init(&pg->touch, &pg->a->walker, pg->a->audio, ride_toggle, pg->a);
  pg->has_touch = true;
  if (pg->a->walker.active && !pg->a->walker.locked) touch_set_enabled(&pg->touch, true);
}
static void begin_touch(Page *pg) {
  begin(pg);
  touch_set_enabled(&pg->touch, true);
  pg->overlay = false;
  if (!pg->a->o.no_fs && !(SDL_GetWindowFlags(g_gpu.win) & SDL_WINDOW_FULLSCREEN)) SDL_SetWindowFullscreen(g_gpu.win, true);
}

static void page_event(Page *pg, const SDL_Event *e, bool *running) {
  App *a = pg->a;
  Walker *wk = &a->walker;
  Ui *ui = a->ui;
  int cw = 0, ch = 0;
  SDL_GetWindowSize(g_gpu.win, &cw, &ch);
  if (e->type == SDL_EVENT_QUIT) *running = false;
  // touch: the first finger creates the controls; a tap on the overlay begins (no pointer lock)
  if (e->type == SDL_EVENT_FINGER_DOWN || e->type == SDL_EVENT_FINGER_UP || e->type == SDL_EVENT_FINGER_MOTION ||
      e->type == SDL_EVENT_FINGER_CANCELED) {
    if (e->type == SDL_EVENT_FINGER_DOWN) enable_touch(pg);
    bool blocked = pg->overlay || ui_loading(ui);
    if (e->type == SDL_EVENT_FINGER_UP && pg->overlay && !ui_loading(ui) && !a->o.autostart) { begin_touch(pg); return; }
    if (pg->has_touch) touch_event(&pg->touch, e, cw, ch, blocked);
    return;
  }
  // mouse events synthesized from touches: the touch path above handles them
  bool from_touch = ((e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_UP) && e->button.which == SDL_TOUCH_MOUSEID) ||
                    (e->type == SDL_EVENT_MOUSE_MOTION && e->motion.which == SDL_TOUCH_MOUSEID);
  if (from_touch) return;
  // Esc leaves the mouse capture (the browser's pointer-lock exit); with the mouse free it quits
  if (e->type == SDL_EVENT_KEY_DOWN && e->key.key == SDLK_ESCAPE) {
    if (wk->locked) {
      walker_unlock(wk, g_gpu.win);   // onLockChange(false)
      pg->overlay = true;
      wk->active = wk->drag_look;
    } else if (wk->active && wk->drag_look) { wk->active = false; pg->overlay = true; }
    else if (wk->active) wk->active = false;
    else *running = false;
    return;
  }
  if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
    if (pg->has_touch && touch_click(&pg->touch, e->button.x, e->button.y, cw, ch)) return;
    // the overlay click: lock first, then start the audio (not while loading: pointer-events none)
    if (pg->overlay && !ui_loading(ui) && !(pg->has_touch && pg->touch.enabled)) {
      walker_lock(wk, g_gpu.win);
      begin(pg);
      if (wk->locked) pg->overlay = false;   // onLockChange(true)
      else {                                 // onLockError({ dragLook })
        pg->overlay = !wk->drag_look;
        ui_note(ui, wk->drag_look ? "Drag to look around" : "Click again to capture the mouse");
        wk->active = wk->drag_look;
      }
      return;
    }
  }
  if (e->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
    if (pg->has_touch) touch_release_all(&pg->touch);
    if (wk->locked) {
      walker_unlock(wk, g_gpu.win);
      pg->overlay = true;
      wk->active = wk->drag_look;
    }
  }
  if (e->type == SDL_EVENT_KEY_DOWN && e->key.scancode == SDL_SCANCODE_M && !e->key.repeat) {
    audio_set_muted(a->audio, !audio_muted(a->audio));
    if (pg->has_touch) touch_sync_mute(&pg->touch);
  }
  vehicles_handle_event(a->vehicles, e);
  walker_handle_event(wk, e);
}

// A lost GPU device (a driver reset after a GPU timeout): stop drawing, say so, and relaunch once
// at a lighter tier. --gpureset marks that relaunch, so a second loss shows a manual button
// instead of looping. (SDL_GPU reports the loss as failing acquires / submits.)
static int g_argc;
static char **g_argv;

static void relaunch(const char *quality) {
  const char *args[128];
  int n = 0;
  args[n++] = g_argv[0];
  for (int i = 1; i < g_argc && n < 120; i++) {
    if (!strcmp(g_argv[i], "--quality")) { i++; continue; }
    if (!strcmp(g_argv[i], "--ultra") || !strcmp(g_argv[i], "--gpureset")) continue;
    args[n++] = g_argv[i];
  }
  args[n++] = "--quality";
  args[n++] = quality;
  args[n++] = "--gpureset";
  args[n] = nullptr;
  SDL_Process *p = SDL_CreateProcess(args, false);
  if (!p) LOG("relaunch failed: %s", SDL_GetError());
  else SDL_DestroyProcess(p);   // (only the handle: the new run carries on)
}

static int gpu_reset_page(const Options *o) {
  static const char *LIGHTER[] = { "medium", "low", "low" };   // high, medium, low
  bool again = o->gpureset;
  const char *next = again ? "low" : LIGHTER[QUALITY.tier];
  LOG("GPU device lost: %s", again ? "again (manual reload)" : "relaunching at a lighter quality");
  gpu_recreate();
  Ui *ui = ui_create(false);
  ui_gpu_reset(ui, again ? 2 : 1);
  uint64_t t0 = SDL_GetTicksNS(), last = t0;
  bool running = true, go = false;
  while (running && !go) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE)) running = false;
      if (e.type == SDL_EVENT_MOUSE_MOTION) ui_reset_button(ui, e.motion.x, e.motion.y, false);
      if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ui_reset_button(ui, e.button.x, e.button.y, true)) go = true;
    }
    uint64_t now = SDL_GetTicksNS();
    if (!again && !o->shot && (double)(now - t0) * 1e-9 >= 1.8) go = true;
    SDL_GPUCommandBuffer *cb = SDL_AcquireGPUCommandBuffer(g_gpu.dev);
    if (!cb) break;
    SDL_GPUTexture *swap = nullptr;
    uint32_t sw = 0, sh = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cb, g_gpu.win, &swap, &sw, &sh)) { SDL_CancelGPUCommandBuffer(cb); break; }
    SDL_GPUTexture *cap = swap ? ui_capture_target(ui, (int)sw, (int)sh) : nullptr;
    if (swap) {
      UiState us = {};
      ui_render(ui, cb, cap ? cap : swap, (int)sw, (int)sh, (double)(now - last) * 1e-9, &us);
      if (cap) ui_capture_present(cb, cap, swap, (int)sw, (int)sh);
    }
    last = now;
    SDL_SubmitGPUCommandBuffer(cb);
    if (cap) ui_capture_save(ui);
  }
  if (go) relaunch(next);
  SDL_WaitForGPUIdle(g_gpu.dev);
  ui_destroy(ui);
  wa_shutdown();
  gpu_shutdown();
  return 0;
}

static int run_window(const Options *o) {
  gpu_init("Ocean Drive", o->width, o->height, false);
  quality_init(o->quality, o->ultra, false);
  g_program_tier = quality_program_tier();
  Pacing P = { .render_scale = QUALITY.renderScale, .start_scale = 0.5 };
  // (DYN.on: off in shot mode and with --dynres 0)
  P.dyn.on = !o->no_dynres;
  P.dyn.min = 0.6; P.dyn.max = QUALITY.renderScale; P.dyn.step = 0.1; P.dyn.hold = 3;
  P.guard.since = 10;
  if (!o->no_gpuguard) P.watch = fence_watch_start();
  int rw, rh;
  double aspect;
  render_size(&P, &rw, &rh, &aspect);
  App a = { .o = *o };
  a.ui = ui_create(true);
  ui_paint(a.ui);
  app_init(&a, rw, rh);
  load_step(&a, 0.8, "Mixing the morning light…");   // LOADER: shader compile phase
  warm_up(&a);
  load_step(&a, 0.92, "Almost there…");
  ui_progress(a.ui, 0.94, "Almost there…");
  Page pg = { .a = &a, .overlay = !o->autostart };
  if (o->has_walk_at) walker_teleport(&a.walker, o->walk_at[0], o->walk_at[1], o->walk_at[2], 0, a.walker.feet_y);
  if (o->autostart) begin(&pg);   // testing: walk and hear without pointer lock
  uint64_t last = SDL_GetTicksNS();
  bool running = true, lost = false;
  double fps = 0, fps_acc = 0;
  int fps_frames = 0;
  char hud[256] = "";
  for (long frame = 0; running && (o->frames == 0 || frame < o->frames); frame++) {
    SDL_Event e;
    if (SDL_getenv("OD_TOUCH_TEST")) {   // dev: a synthetic tap (begin), then a joystick drag
      static const struct { long f; Uint32 type; float x, y; SDL_FingerID id; } T[] = {
        { 100, SDL_EVENT_FINGER_DOWN, 0.5f, 0.5f, 1 }, { 101, SDL_EVENT_FINGER_UP, 0.5f, 0.5f, 1 },
        { 130, SDL_EVENT_FINGER_DOWN, 0.2f, 0.7f, 2 }, { 132, SDL_EVENT_FINGER_MOTION, 0.23f, 0.62f, 2 },
        { 133, SDL_EVENT_FINGER_DOWN, 0.9f, 0.4f, 3 }, { 136, SDL_EVENT_FINGER_MOTION, 0.88f, 0.41f, 3 } };
      for (size_t i = 0; i < ARRAY_LEN(T); i++)
        if (T[i].f == frame) {
          SDL_Event te = { .type = T[i].type };
          te.tfinger.fingerID = T[i].id; te.tfinger.x = T[i].x; te.tfinger.y = T[i].y;
          SDL_PushEvent(&te);
        }
    }
    while (SDL_PollEvent(&e)) page_event(&pg, &e, &running);
    uint64_t now = SDL_GetTicksNS();
    double raw_dt = (double)(now - last) * 1e-9;
    double dt = fmin(raw_dt, 0.1);
    last = now;
    dynamic_resolution(&P, raw_dt);
    if (!o->no_gpuguard) gpu_guard(&P, &a, frame);
    a.elapsed += dt;
    a.dt = dt;
    a.now_s = (double)now * 1e-9;
    fps_acc += dt; fps_frames++;
    if (fps_acc >= 0.5) { fps = fps_frames / fps_acc; fps_acc = 0; fps_frames = 0; }
    // the first frames start at half resolution and ramp up
    if (P.start_scale < 1 && frame % 8 == 0 && frame > 0) P.start_scale = fmin(1, P.start_scale + 0.125);
    // dev: OD_FAKE_GPU_RESET=N pretends the device is lost at frame N
    const char *fake = SDL_getenv("OD_FAKE_GPU_RESET");
    if (fake && frame == atol(fake)) { lost = true; break; }
    SDL_GPUCommandBuffer *cb = SDL_AcquireGPUCommandBuffer(g_gpu.dev);
    if (!cb) { lost = true; break; }
    SDL_GPUTexture *swap = nullptr;
    uint32_t sw = 0, sh = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cb, g_gpu.win, &swap, &sw, &sh)) { SDL_CancelGPUCommandBuffer(cb); lost = true; break; }
    SDL_GPUTexture *cap = nullptr;
    if (swap) {
      cap = ui_capture_target(a.ui, (int)sw, (int)sh);
      SDL_GPUTexture *page = cap ? cap : swap;
      render_size(&P, &rw, &rh, &aspect);
      post_resize(a.post, rw, rh);
      a.render_h = rh;
      a.camera.aspect = aspect;
      render_frame(&a, cb);
      Texture *scr = post_screen(a.post);
      // the screen texture holds rows in GL order: flip while copying to the window (scaled like
      // the browser's canvas)
      bool same = (uint32_t)scr->w == sw && (uint32_t)scr->h == sh;
      SDL_BlitGPUTexture(cb, &(SDL_GPUBlitInfo){
        .source = { .texture = scr->gpu, .w = (uint32_t)scr->w, .h = (uint32_t)scr->h },
        .destination = { .texture = page, .w = sw, .h = sh },
        .load_op = SDL_GPU_LOADOP_DONT_CARE, .flip_mode = SDL_FLIP_VERTICAL,
        .filter = same ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR });
      // the page over the canvas
      if (o->hud && (frame + 1) % 15 == 0) {
        const V3 p = a.camera.node->position;
        double pr = base_dpr() * P.render_scale * P.start_scale;
        char f[16];
        double fr = js_round(fps * 10) / 10;
        if (fr == floor(fr)) snprintf(f, sizeof f, "%.0f", fr); else snprintf(f, sizeof f, "%.1f", fr);
        snprintf(hud, sizeof hud, "%s fps · %d calls · %.0fk tris · x %.1f y %.1f z %.1f · %s ×%.2f", f,
                 g_render_stats.calls, g_render_stats.triangles / 1000.0, p.x, p.y, p.z,
                 QUALITY.tier == TIER_HIGH ? "high" : QUALITY.tier == TIER_MEDIUM ? "medium" : "low", pr);
      }
      UiState us = { .overlay = pg.overlay, .touch = pg.has_touch, .touch_ctl = pg.has_touch ? &pg.touch : nullptr,
                     .hud = o->hud ? hud : nullptr };
      vehicles_prompt(a.vehicles, &us.prompt_key, &us.prompt_text);
      if (pg.has_touch) {
        VKind kind;
        int mode = vehicles_touch_mode(a.vehicles, &kind);
        touch_set_ride(&pg.touch, mode, kind == VK_ATV);
      }
      ui_render(a.ui, cb, page, (int)sw, (int)sh, raw_dt, &us);
      if (cap) ui_capture_present(cb, cap, swap, (int)sw, (int)sh);
    }
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
    if (!fence) { LOG("submit: %s", SDL_GetError()); lost = true; break; }
    if (o->no_gpuguard) SDL_ReleaseGPUFence(g_gpu.dev, fence);
    else fence_watch_add(P.watch, fence, SDL_GetTicksNS(), frame);
    if (cap) ui_capture_save(a.ui);
    if (frame + 1 == 10) ui_scene_ready(a.ui);   // window.__sceneReady
  }
  if (lost) {
    if (P.watch) fence_watch_stop(P.watch);
    return gpu_reset_page(o);
  }
  SDL_WaitForGPUIdle(g_gpu.dev);
  // (window.__dynres / __gpuGuard)
  LOG("pacing: render scale %.2f (start %.3f), %d x %d; gpu guard %d samples, worst %.1f ms, last %.1f ms",
      P.render_scale, P.start_scale, rw, rh, P.guard.n, P.guard.worst, P.guard.last);
  if (P.watch) fence_watch_stop(P.watch);
  ui_destroy(a.ui);
  wa_shutdown();
  renderer_shutdown();
  gpu_shutdown();
  return 0;
}

static int run_audio_test(const Options *o) {
  char name[64];
  double secs = 6;
  snprintf(name, sizeof name, "%s", o->audio_test);
  char *comma = strchr(name, ',');
  if (comma) { *comma = 0; secs = atof(comma + 1); }
  layout_init();
  if (!strcmp(name, "hrtf")) { audio_hrtf_table(); return 0; }
  if (!strcmp(name, "hrtf-sines")) { audio_hrtf_sines(); return 0; }
  if (!strcmp(name, "units")) { audio_units_test(); return 0; }
  AudioTestResult r = audio_render_test(name, secs);
  printf("%s peak=%.4f rms=%.5f peakDb=%.1f rmsDb=%.1f clipped=%d\n", name, r.peak, r.rms, r.peak_db, r.rms_db, r.clipped);
  return 0;
}

int main(int argc, char **argv) {
  g_argc = argc;
  g_argv = argv;
  Options o = parse_args(argc, argv);
  if (o.audio_test) return run_audio_test(&o);
  return o.dump ? run_dump(&o) : o.shot ? run_shot(&o) : run_window(&o);
}
