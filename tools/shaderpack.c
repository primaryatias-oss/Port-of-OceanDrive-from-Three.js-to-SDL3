// shaderpack: embeds compiled SPIR-V shaders into C and reflects the resource counts SDL_GPU
// needs (samplers, storage textures/buffers, uniform buffers, compute thread counts).
//
// It also enforces SDL_GPU's SPIR-V descriptor layout at build time, so a binding mistake is a
// build error instead of a silent runtime misbinding:
//   vertex   : set 0 = samplers, storage textures, storage buffers   set 1 = uniform buffers
//   fragment : set 2 = samplers, storage textures, storage buffers   set 3 = uniform buffers
//   compute  : set 0 = samplers, read-only storage textures, read-only storage buffers
//              set 1 = read-write storage textures, read-write storage buffers
//              set 2 = uniform buffers
// Within a set the bindings must be 0..n-1 with the resource kinds in the order listed.
//
// usage: shaderpack OUT.h OUT.c NAME.vert.spv NAME.frag.spv NAME.comp.spv ...

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { OP_EXECUTION_MODE = 16, OP_TYPE_IMAGE = 25, OP_TYPE_SAMPLED_IMAGE = 27, OP_TYPE_ARRAY = 28,
       OP_TYPE_RUNTIME_ARRAY = 29, OP_TYPE_STRUCT = 30, OP_TYPE_POINTER = 32, OP_VARIABLE = 59,
       OP_DECORATE = 71 };
enum { DEC_BLOCK = 2, DEC_BUFFER_BLOCK = 3, DEC_BINDING = 33, DEC_DESCRIPTOR_SET = 34 };
enum { SC_UNIFORM_CONSTANT = 0, SC_UNIFORM = 2, SC_PUSH_CONSTANT = 9, SC_STORAGE_BUFFER = 12 };
enum { EXEC_LOCAL_SIZE = 17 };

enum Kind { K_SAMPLER, K_STORAGE_TEX, K_STORAGE_BUF, K_UNIFORM_BUF, K_NONE };
static const char *const KIND_NAME[] = { "sampler", "storage texture", "storage buffer", "uniform buffer" };

enum Stage { ST_VERT, ST_FRAG, ST_COMP };

typedef struct {
  uint32_t opcode, set, binding, has_set, has_binding, block, buffer_block;
  uint32_t a, b, c; // opcode-specific operands (see reflect)
  uint32_t image_sampled;
} Id;

typedef struct { uint32_t set, binding; enum Kind kind; } Res;

static void die(const char *file, const char *msg) {
  fprintf(stderr, "shaderpack: %s: %s\n", file, msg);
  exit(1);
}

static uint8_t *read_file(const char *path, size_t *size) {
  FILE *f = fopen(path, "rb");
  if (!f) die(path, "cannot open");
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n <= 0 || n % 4) die(path, "not a SPIR-V binary (size)");
  uint8_t *buf = malloc((size_t)n);
  if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) die(path, "read failed");
  fclose(f);
  *size = (size_t)n;
  return buf;
}

typedef struct {
  uint32_t counts[3][4]; // [set-group][kind]; see layout above
  uint32_t samplers, storage_textures, storage_buffers, uniform_buffers, rw_storage_textures,
           rw_storage_buffers;
  uint32_t threads[3];
} Reflection;

static int cmp_res(const void *pa, const void *pb) {
  const Res *a = pa, *b = pb;
  if (a->set != b->set) return a->set < b->set ? -1 : 1;
  return a->binding < b->binding ? -1 : a->binding > b->binding;
}

// the allowed kinds of each descriptor set for a stage, in the order bindings must follow
static int set_rank(enum Stage st, uint32_t set, enum Kind k, int rw) {
  switch (st) {
  case ST_VERT:
  case ST_FRAG: {
    uint32_t base = st == ST_VERT ? 0 : 2;
    if (set == base && k != K_UNIFORM_BUF) return (int)k;       // 0 sampler, 1 stex, 2 sbuf
    if (set == base + 1 && k == K_UNIFORM_BUF) return 0;
    return -1;
  }
  case ST_COMP:
    if (set == 0 && !rw && k != K_UNIFORM_BUF) return (int)k;
    if (set == 1 && rw && k == K_STORAGE_TEX) return 0;
    if (set == 1 && rw && k == K_STORAGE_BUF) return 1;
    if (set == 2 && k == K_UNIFORM_BUF) return 0;
    return -1;
  }
  return -1;
}

static Reflection reflect(const char *path, const uint32_t *w, size_t nwords, enum Stage st) {
  if (nwords < 5 || w[0] != 0x07230203u) die(path, "bad SPIR-V magic");
  uint32_t bound = w[3];
  Id *ids = calloc(bound, sizeof *ids);
  if (!ids) die(path, "out of memory");
  Reflection r = { .threads = { 1, 1, 1 } };

  for (size_t i = 5; i < nwords;) {
    uint32_t op = w[i] & 0xffffu, wc = w[i] >> 16;
    if (wc == 0 || i + wc > nwords) die(path, "malformed instruction stream");
    const uint32_t *o = w + i;
    switch (op) {
    case OP_EXECUTION_MODE:
      if (wc >= 6 && o[2] == EXEC_LOCAL_SIZE) { r.threads[0] = o[3]; r.threads[1] = o[4]; r.threads[2] = o[5]; }
      break;
    case OP_DECORATE:
      if (o[1] >= bound) die(path, "decoration target out of range");
      if (o[2] == DEC_DESCRIPTOR_SET) { ids[o[1]].set = o[3]; ids[o[1]].has_set = 1; }
      if (o[2] == DEC_BINDING) { ids[o[1]].binding = o[3]; ids[o[1]].has_binding = 1; }
      if (o[2] == DEC_BLOCK) ids[o[1]].block = 1;
      if (o[2] == DEC_BUFFER_BLOCK) ids[o[1]].buffer_block = 1;
      break;
    case OP_TYPE_IMAGE:
      ids[o[1]].opcode = op; ids[o[1]].image_sampled = o[7];
      break;
    case OP_TYPE_SAMPLED_IMAGE:
    case OP_TYPE_STRUCT:
      ids[o[1]].opcode = op;
      break;
    case OP_TYPE_ARRAY:
    case OP_TYPE_RUNTIME_ARRAY:
      ids[o[1]].opcode = op; ids[o[1]].a = o[2];
      break;
    case OP_TYPE_POINTER:
      ids[o[1]].opcode = op; ids[o[1]].a = o[2]; ids[o[1]].b = o[3];
      break;
    case OP_VARIABLE:
      ids[o[2]].opcode = op; ids[o[2]].a = o[1]; ids[o[2]].b = o[3];
      break;
    default: break;
    }
    i += wc;
  }

  Res res[128];
  uint32_t nres = 0;
  // read-write-ness in compute is carried by the set number (checked against the kind below)
  for (uint32_t id = 0; id < bound; id++) {
    if (ids[id].opcode != OP_VARIABLE) continue;
    uint32_t sc = ids[id].b;
    if (sc == SC_PUSH_CONSTANT) die(path, "push constants are not supported by SDL_GPU; use a uniform block");
    if (sc != SC_UNIFORM_CONSTANT && sc != SC_UNIFORM && sc != SC_STORAGE_BUFFER) continue;
    uint32_t ptr = ids[id].a;
    if (ids[ptr].opcode != OP_TYPE_POINTER) die(path, "variable type is not a pointer");
    uint32_t t = ids[ptr].b;
    if (ids[t].opcode == OP_TYPE_ARRAY || ids[t].opcode == OP_TYPE_RUNTIME_ARRAY)
      die(path, "arrays of resources are not supported; declare each binding separately");
    enum Kind k = K_NONE;
    if (sc == SC_UNIFORM_CONSTANT) {
      if (ids[t].opcode == OP_TYPE_SAMPLED_IMAGE) k = K_SAMPLER;
      else if (ids[t].opcode == OP_TYPE_IMAGE && ids[t].image_sampled == 2) k = K_STORAGE_TEX;
      else die(path, "unsupported uniform-constant resource (separate images/samplers are not supported)");
    } else if (sc == SC_UNIFORM) {
      if (ids[t].opcode != OP_TYPE_STRUCT) die(path, "uniform variable is not a block");
      k = ids[t].buffer_block ? K_STORAGE_BUF : K_UNIFORM_BUF;
    } else {
      k = K_STORAGE_BUF;
    }
    if (!ids[id].has_set || !ids[id].has_binding) die(path, "resource without explicit set/binding");
    if (nres == 128) die(path, "too many resources");
    res[nres++] = (Res){ ids[id].set, ids[id].binding, k };
  }
  free(ids);

  qsort(res, nres, sizeof *res, cmp_res);
  uint32_t expect_binding = 0, cur_set = UINT32_MAX;
  int last_rank = -1;
  for (uint32_t i = 0; i < nres; i++) {
    int rw = st == ST_COMP && res[i].set == 1;
    int rank = set_rank(st, res[i].set, res[i].kind, rw);
    char msg[256];
    if (rank < 0) {
      snprintf(msg, sizeof msg, "%s at set %u binding %u is in the wrong descriptor set for this stage",
               KIND_NAME[res[i].kind], res[i].set, res[i].binding);
      die(path, msg);
    }
    if (res[i].set != cur_set) { cur_set = res[i].set; expect_binding = 0; last_rank = -1; }
    if (res[i].binding != expect_binding) {
      snprintf(msg, sizeof msg, "set %u: binding %u found where %u was expected (bindings must be contiguous)",
               res[i].set, res[i].binding, expect_binding);
      die(path, msg);
    }
    if (rank < last_rank) {
      snprintf(msg, sizeof msg, "set %u binding %u: %s must come before the kinds bound earlier in the set",
               res[i].set, res[i].binding, KIND_NAME[res[i].kind]);
      die(path, msg);
    }
    last_rank = rank;
    expect_binding++;
    switch (res[i].kind) {
    case K_SAMPLER: r.samplers++; break;
    case K_STORAGE_TEX: if (rw) r.rw_storage_textures++; else r.storage_textures++; break;
    case K_STORAGE_BUF: if (rw) r.rw_storage_buffers++; else r.storage_buffers++; break;
    case K_UNIFORM_BUF: r.uniform_buffers++; break;
    case K_NONE: break;
    }
  }
  return r;
}

// "build/spv/sky.frag.spv" -> base "sky.frag", ident "SKY_FRAG"
static void names(const char *path, char *base, size_t bn, char *ident, size_t in, enum Stage *st) {
  const char *s = strrchr(path, '/');
  s = s ? s + 1 : path;
  size_t n = strlen(s);
  if (n < 5 || strcmp(s + n - 4, ".spv") != 0) die(path, "expected a .spv file");
  n -= 4;
  if (n + 1 > bn || n + 1 > in) die(path, "name too long");
  memcpy(base, s, n); base[n] = 0;
  const char *ext = strrchr(base, '.');
  if (!ext) die(path, "expected NAME.vert/.frag/.comp.spv");
  if (!strcmp(ext, ".vert")) *st = ST_VERT;
  else if (!strcmp(ext, ".frag")) *st = ST_FRAG;
  else if (!strcmp(ext, ".comp")) *st = ST_COMP;
  else die(path, "unknown shader stage extension");
  for (size_t i = 0; i <= n; i++) {
    char c = base[i];
    ident[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : (c == '.' || c == '-') ? '_' : c;
  }
}

int main(int argc, char **argv) {
  if (argc < 4) { fprintf(stderr, "usage: shaderpack OUT.h OUT.c SHADER.spv...\n"); return 2; }
  FILE *h = fopen(argv[1], "w"), *c = fopen(argv[2], "w");
  if (!h || !c) die(argv[1], "cannot write output");
  int n = argc - 3;

  fprintf(h, "// Generated by tools/shaderpack.c. Do not edit.\n#pragma once\n#include <stddef.h>\n#include <stdint.h>\n\n");
  fprintf(h, "typedef enum ShaderStage { SHADER_VERTEX, SHADER_FRAGMENT, SHADER_COMPUTE } ShaderStage;\n\n");
  fprintf(h, "typedef struct ShaderBlob {\n  const char *name;\n  ShaderStage stage;\n  const uint8_t *code;\n  size_t size;\n"
             "  uint32_t samplers, storage_textures, storage_buffers, uniform_buffers;\n"
             "  uint32_t rw_storage_textures, rw_storage_buffers; // compute only\n"
             "  uint32_t threads[3];                                // compute only\n} ShaderBlob;\n\n");
  fprintf(h, "typedef enum ShaderId {\n");
  fprintf(c, "// Generated by tools/shaderpack.c. Do not edit.\n#include \"shaders.h\"\n\n");

  char (*idents)[128] = calloc((size_t)n, 128);
  char (*bases)[128] = calloc((size_t)n, 128);
  Reflection *refl = calloc((size_t)n, sizeof *refl);
  enum Stage *stages = calloc((size_t)n, sizeof *stages);
  size_t *sizes = calloc((size_t)n, sizeof *sizes);
  if (!idents || !bases || !refl || !stages || !sizes) die(argv[0], "out of memory");

  for (int i = 0; i < n; i++) {
    const char *path = argv[3 + i];
    names(path, bases[i], 128, idents[i], 128, &stages[i]);
    uint8_t *buf = read_file(path, &sizes[i]);
    uint32_t *words = malloc(sizes[i]);
    if (!words) die(path, "out of memory");
    memcpy(words, buf, sizes[i]);
    refl[i] = reflect(path, words, sizes[i] / 4, stages[i]);
    free(words);
    fprintf(h, "  SH_%s,\n", idents[i]);
    fprintf(c, "static const uint8_t code_%s[%zu] = {", idents[i], sizes[i]);
    for (size_t b = 0; b < sizes[i]; b++) fprintf(c, "%s%u,", b % 24 ? "" : "\n  ", buf[b]);
    fprintf(c, "\n};\n\n");
    free(buf);
  }
  fprintf(h, "  SH_COUNT\n} ShaderId;\n\nextern const ShaderBlob SHADERS[SH_COUNT];\n");

  static const char *const STAGE_ENUM[] = { "SHADER_VERTEX", "SHADER_FRAGMENT", "SHADER_COMPUTE" };
  fprintf(c, "const ShaderBlob SHADERS[SH_COUNT] = {\n");
  for (int i = 0; i < n; i++) {
    Reflection *r = &refl[i];
    fprintf(c, "  [SH_%s] = { \"%s\", %s, code_%s, %zu, %u, %u, %u, %u, %u, %u, { %u, %u, %u } },\n",
            idents[i], bases[i], STAGE_ENUM[stages[i]], idents[i], sizes[i], r->samplers,
            r->storage_textures, r->storage_buffers, r->uniform_buffers, r->rw_storage_textures,
            r->rw_storage_buffers, r->threads[0], r->threads[1], r->threads[2]);
  }
  fprintf(c, "};\n");
  fclose(h);
  fclose(c);
  return 0;
}
