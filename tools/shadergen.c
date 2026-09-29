// shadergen: builds the scene's GPU programs from three.js's shader library, the way three's
// WebGLProgram does it, then turns them into Vulkan GLSL for SDL_GPU.
//
// Input: shaders/programs/*.prog. A .prog names a template (a three.js ShaderLib entry or a
// custom source), the material parameters that decide the prefix defines, and the text
// patches the JS code applied in onBeforeCompile. Assembly reproduces three.js r186 byte for
// byte (prefix, patches, #include <chunk> resolution, light counts, loop unrolling), so the
// result can be diffed against shaders captured from the running JS app.
//
// Conversion (GLSL ES 3.00 -> GLSL 4.50 for Vulkan): the assembled text is preprocessed with
// `glslangValidator -E`, then top-level declarations are rewritten: loose uniforms go into
// std140 blocks (per-object matrices in their own block so they can be pushed per draw),
// samplers get set/binding decorations (arrays of samplers are split), inputs/outputs get
// locations (varyings matched by name across stages). A C table of the resulting layout is
// written so the runtime can set uniforms by name, as three.js does.
//
// usage:
//   shadergen assemble PROG TIER OUT.vert OUT.frag      assembled GLSL ES (for diffing)
//   shadergen build OUTDIR GEN_H GEN_C PROG...          all tiers -> OUTDIR/*.vert|frag + tables
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- utilities ------------------------------------------------------------------------------

[[noreturn]] static void die(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "shadergen: ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  exit(1);
}

typedef struct Str { char *p; size_t n, cap; } Str;

static void s_reserve(Str *s, size_t need) {
  if (need + 1 <= s->cap) return;
  size_t c = s->cap ? s->cap : 256;
  while (c < need + 1) c *= 2;
  s->p = realloc(s->p, c);
  if (!s->p) die("out of memory");
  s->cap = c;
}
static void s_addn(Str *s, const char *t, size_t n) {
  s_reserve(s, s->n + n);
  memcpy(s->p + s->n, t, n);
  s->n += n;
  s->p[s->n] = 0;
}
static void s_add(Str *s, const char *t) { s_addn(s, t, strlen(t)); }
static void s_addc(Str *s, char c) { s_addn(s, &c, 1); }
static void s_printf(Str *s, const char *fmt, ...) {
  va_list ap, ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);
  int n = vsnprintf(nullptr, 0, fmt, ap);
  va_end(ap);
  s_reserve(s, s->n + (size_t)n);
  vsnprintf(s->p + s->n, (size_t)n + 1, fmt, ap2);
  va_end(ap2);
  s->n += (size_t)n;
}
static char *s_take(Str *s) {
  if (!s->p) { s_reserve(s, 0); s->p[0] = 0; }
  char *p = s->p;
  *s = (Str){};
  return p;
}
static char *xstrdup(const char *t) {
  size_t n = strlen(t);
  char *p = malloc(n + 1);
  if (!p) die("out of memory");
  memcpy(p, t, n + 1);
  return p;
}

static char *read_text(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) die("cannot open %s", path);
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *p = malloc((size_t)n + 1);
  if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) die("cannot read %s", path);
  p[n] = 0;
  fclose(f);
  return p;
}
static void write_text(const char *path, const char *t) {
  FILE *f = fopen(path, "wb");
  if (!f || fwrite(t, 1, strlen(t), f) != strlen(t)) die("cannot write %s", path);
  fclose(f);
}

// replace every occurrence of `find` (no regex)
static char *replace_all(const char *src, const char *find, const char *with) {
  Str o = {};
  size_t fl = strlen(find);
  for (const char *p = src;;) {
    const char *q = strstr(p, find);
    if (!q) { s_add(&o, p); break; }
    s_addn(&o, p, (size_t)(q - p));
    s_add(&o, with);
    p = q + fl;
  }
  return s_take(&o);
}

// String.prototype.replace(string, string): first occurrence, with $$ $& $` $' patterns
static char *js_replace_first(const char *src, const char *find, const char *with, bool *found) {
  const char *q = strstr(src, find);
  *found = q != nullptr;
  if (!q) return xstrdup(src);
  size_t fl = strlen(find);
  Str o = {};
  s_addn(&o, src, (size_t)(q - src));
  for (const char *w = with; *w; w++) {
    if (*w == '$' && w[1]) {
      if (w[1] == '$') { s_addc(&o, '$'); w++; continue; }
      if (w[1] == '&') { s_addn(&o, q, fl); w++; continue; }
      if (w[1] == '`') { s_addn(&o, src, (size_t)(q - src)); w++; continue; }
      if (w[1] == '\'') { s_add(&o, q + fl); w++; continue; }
    }
    s_addc(&o, *w);
  }
  s_add(&o, q + fl);
  return s_take(&o);
}

// ---- program description (.prog) ------------------------------------------------------------

enum { MAX_KV = 128, MAX_PATCH = 64, MAX_VARIANTS = 4 };
static const char *const TIERS[] = { "high", "medium", "low", "ultra" };
enum { NTIERS = 4 };

typedef struct KV { char *k, *v; } KV;
typedef struct Patch { int stage; int kind; char *find, *with; } Patch;   // kind 0: replace first, 1: replace all
enum { PATCH_FIRST, PATCH_ALL, PATCH_PREPEND, PATCH_APPEND };

typedef struct Prog {
  char *name, *path;
  char *vert_tpl, *frag_tpl;         // template text (before patches)
  KV params[MAX_KV]; int nparams;    // material parameters (WebGLProgram `parameters`)
  KV defines[MAX_KV]; int ndefines;  // material.defines (ordered)
  KV vars[MAX_KV]; int nvars;        // ${NAME} substitutions in patches / custom sources
  Patch patches[MAX_PATCH]; int npatches;
  bool tier_used[4];                 // `tiers` directive (default: all)
} Prog;

static const char *kv_get(const KV *a, int n, const char *k) {
  for (int i = n - 1; i >= 0; i--)
    if (!strcmp(a[i].k, k)) return a[i].v;
  return nullptr;
}
static void kv_set(KV *a, int *n, const char *k, const char *v) {
  for (int i = 0; i < *n; i++)
    if (!strcmp(a[i].k, k)) { a[i].v = xstrdup(v); return; }
  if (*n >= MAX_KV) die("too many entries");
  a[*n].k = xstrdup(k);
  a[*n].v = xstrdup(v);
  (*n)++;
}

static char *shader_root;   // "shaders"
static const char *cur_tier;
static bool file_exists(const char *path);

static char *load_template(const char *spec, const char *ext) {
  char path[1024];
  if (!strncmp(spec, "lib:", 4)) {
    snprintf(path, sizeof path, "%s/three/lib/%s.%s", shader_root, spec + 4, ext);
    return read_text(path);
  }
  // a tier-specific copy (dir/<tier>/file) wins over the shared one (dir/file)
  const char *slash = strrchr(spec, '/');
  if (slash) {
    snprintf(path, sizeof path, "%s/%.*s/%s/%s", shader_root, (int)(slash - spec), spec, cur_tier, slash + 1);
    if (file_exists(path)) return read_text(path);
  }
  snprintf(path, sizeof path, "%s/%s", shader_root, spec);
  return read_text(path);
}

// ${NAME} substitution from the program's and the tier's variables
static char *subst_vars(const char *t, const Prog *p, const KV *tv, int ntv, const char *where) {
  Str o = {};
  for (const char *c = t; *c;) {
    if (c[0] == '$' && c[1] == '{') {
      const char *e = strchr(c, '}');
      if (!e) die("%s: unterminated ${ in %s", p->path, where);
      char name[128];
      size_t n = (size_t)(e - c - 2);
      if (n >= sizeof name) die("%s: variable name too long", p->path);
      memcpy(name, c + 2, n);
      name[n] = 0;
      const char *v = kv_get(p->vars, p->nvars, name);
      if (!v) v = kv_get(tv, ntv, name);
      if (!v) {   // shaders/vars/<tier>/NAME.glsl, then shaders/vars/NAME.glsl (exact text)
        char path[1024];
        snprintf(path, sizeof path, "%s/vars/%s/%s.glsl", shader_root, cur_tier, name);
        if (!file_exists(path)) snprintf(path, sizeof path, "%s/vars/%s.glsl", shader_root, name);
        if (file_exists(path)) v = read_text(path);
      }
      if (!v) die("%s: undefined variable ${%s} in %s", p->path, name, where);
      s_add(&o, v);
      c = e + 1;
    } else {
      s_addc(&o, *c++);
    }
  }
  return s_take(&o);
}

// Reads a .prog for one tier. Lines: `key args`; `@if tier...` / `@else` / `@endif` select
// lines per tier; `@replace vertex|fragment <find>` + text + `@end` (also @replace-all,
// @prepend, @append). Text blocks are taken verbatim (the line breaks between their lines).
static Prog *load_prog(const char *path, const char *tier) {
  cur_tier = tier;
  Prog *p = calloc(1, sizeof *p);
  if (!p) die("out of memory");
  p->path = xstrdup(path);
  for (int t = 0; t < NTIERS; t++) p->tier_used[t] = true;
  char *text = read_text(path);
  // split lines (keep them exact)
  int cap = 1024, nl = 0;
  char **lines = malloc(sizeof(char *) * (size_t)cap);
  for (char *s = text;;) {
    char *e = strchr(s, '\n');
    if (nl == cap) { cap *= 2; lines = realloc(lines, sizeof(char *) * (size_t)cap); }
    lines[nl++] = s;
    if (!e) break;
    *e = 0;
    s = e + 1;
  }
  // tier selection
  bool keep_stack[32];
  int depth = 0;
  bool keep = true;
  char **sel = malloc(sizeof(char *) * (size_t)nl);
  int ns = 0;
  for (int i = 0; i < nl; i++) {
    char *l = lines[i];
    if (!strncmp(l, "@if ", 4)) {
      if (depth >= 32) die("%s: @if nesting too deep", path);
      keep_stack[depth++] = keep;
      bool match = false;
      char buf[256];
      snprintf(buf, sizeof buf, "%s", l + 4);
      for (char *t = strtok(buf, " \t"); t; t = strtok(nullptr, " \t"))
        if (!strcmp(t, tier)) match = true;
      keep = keep && match;
      continue;
    }
    if (!strcmp(l, "@else")) {
      if (!depth) die("%s:%d: @else without @if", path, i + 1);
      keep = keep_stack[depth - 1] && !keep;
      continue;
    }
    if (!strcmp(l, "@endif")) {
      if (!depth) die("%s:%d: @endif without @if", path, i + 1);
      keep = keep_stack[--depth];
      continue;
    }
    if (keep) sel[ns++] = l;
  }
  if (depth) die("%s: unterminated @if", path);

  char *vspec = nullptr, *fspec = nullptr;
  for (int i = 0; i < ns; i++) {
    char *l = sel[i];
    if (!l[0] || l[0] == '#') continue;
    if (l[0] == '@') {
      int kind;
      const char *rest;
      if (!strncmp(l, "@replace-all ", 13)) { kind = PATCH_ALL; rest = l + 13; }
      else if (!strncmp(l, "@replace ", 9)) { kind = PATCH_FIRST; rest = l + 9; }
      else if (!strncmp(l, "@prepend ", 9)) { kind = PATCH_PREPEND; rest = l + 9; }
      else if (!strncmp(l, "@append ", 8)) { kind = PATCH_APPEND; rest = l + 8; }
      else die("%s: unknown directive: %s", path, l);
      int stage;
      if (!strncmp(rest, "vertex", 6)) { stage = 0; rest += 6; }
      else if (!strncmp(rest, "fragment", 8)) { stage = 1; rest += 8; }
      else die("%s: patch needs a stage: %s", path, l);
      if (*rest == ' ') rest++;
      if ((kind == PATCH_FIRST || kind == PATCH_ALL) && !*rest) die("%s: @replace needs a search string", path);
      Str body = {};
      int j = i + 1;
      bool first = true;
      for (; j < ns && strcmp(sel[j], "@end"); j++) {
        if (!first) s_addc(&body, '\n');
        s_add(&body, sel[j]);
        first = false;
      }
      if (j >= ns) die("%s: missing @end after %s", path, l);
      if (p->npatches >= MAX_PATCH) die("%s: too many patches", path);
      p->patches[p->npatches++] = (Patch){ stage, kind, xstrdup(rest), s_take(&body) };
      i = j;
      continue;
    }
    char key[64] = { 0 };
    const char *sp = strchr(l, ' ');
    size_t kl = sp ? (size_t)(sp - l) : strlen(l);
    if (kl >= sizeof key) die("%s: key too long", path);
    memcpy(key, l, kl);
    const char *arg = sp ? sp + 1 : "";
    if (!strcmp(key, "name")) p->name = xstrdup(arg);
    else if (!strcmp(key, "tiers")) {
      for (int t = 0; t < NTIERS; t++) p->tier_used[t] = false;
      char b[256];
      snprintf(b, sizeof b, "%s", arg);
      for (char *t = strtok(b, " \t"); t; t = strtok(nullptr, " \t")) {
        int k = -1;
        for (int i = 0; i < NTIERS; i++) if (!strcmp(t, TIERS[i])) k = i;
        if (k < 0) die("%s: unknown tier '%s'", path, t);
        p->tier_used[k] = true;
      }
    }
    else if (!strcmp(key, "lib")) {
      char b[256];
      snprintf(b, sizeof b, "lib:%s", arg);
      vspec = xstrdup(b);
      fspec = xstrdup(b);
    } else if (!strcmp(key, "vertex")) vspec = xstrdup(arg);
    else if (!strcmp(key, "fragment")) fspec = xstrdup(arg);
    else if (!strcmp(key, "define")) {
      // "#define NAME VALUE" (three writes '#define ' + name + ' ' + value)
      char n[128] = { 0 };
      const char *v = strchr(arg, ' ');
      size_t nlen = v ? (size_t)(v - arg) : strlen(arg);
      memcpy(n, arg, nlen < 127 ? nlen : 127);
      kv_set(p->defines, &p->ndefines, n, v ? v + 1 : "");
    } else if (!strcmp(key, "param") || !strcmp(key, "var")) {
      char n[128] = { 0 };
      const char *v = strchr(arg, ' ');
      size_t nlen = v ? (size_t)(v - arg) : strlen(arg);
      memcpy(n, arg, nlen < 127 ? nlen : 127);
      // a bare `param` is true, except the name/type strings which default to empty
      const char *dv = (!strcmp(n, "shaderName") || !strcmp(n, "shaderType")) ? "" : "1";
      if (!strcmp(key, "param")) kv_set(p->params, &p->nparams, n, v ? v + 1 : dv);
      else kv_set(p->vars, &p->nvars, n, v ? v + 1 : "");
    } else {
      die("%s: unknown key '%s'", path, key);
    }
  }
  if (!p->name) die("%s: missing name", path);
  if (!vspec || !fspec) die("%s: missing template (lib / vertex / fragment)", path);
  p->vert_tpl = load_template(vspec, "vert");
  p->frag_tpl = load_template(fspec, "frag");
  return p;
}

// ---- WebGLProgram assembly --------------------------------------------------------------------

static bool P(const Prog *p, const char *k) {   // truthy parameter
  const char *v = kv_get(p->params, p->nparams, k);
  return v && strcmp(v, "0") && strcmp(v, "false") && *v;
}
static const char *PV(const Prog *p, const char *k, const char *def) {
  const char *v = kv_get(p->params, p->nparams, k);
  return v ? v : def;
}

static const char *cur_tier = "high";

static bool file_exists(const char *path) {
  FILE *f = fopen(path, "rb");
  if (f) fclose(f);
  return f != nullptr;
}

// Chunk lookup: the port's overrides of three's chunks (the JS code replaced some ShaderChunk
// entries globally, e.g. fog and shadows in sky.js) per tier, then shared, then three's own.
static char *chunk_text(const char *name) {
  char path[1024];
  snprintf(path, sizeof path, "%s/chunk/%s/%s.glsl", shader_root, cur_tier, name);
  if (file_exists(path)) return read_text(path);
  snprintf(path, sizeof path, "%s/chunk/%s.glsl", shader_root, name);
  if (file_exists(path)) return read_text(path);
  snprintf(path, sizeof path, "%s/three/chunk/%s.glsl", shader_root, name);
  return read_text(path);
}

// resolveIncludes: /^[ \t]*#include +<([\w\d./]+)>/gm, recursively
static char *resolve_includes(const char *s) {
  Str o = {};
  const char *line = s;
  while (*line) {
    const char *eol = strchr(line, '\n');
    size_t len = eol ? (size_t)(eol - line) : strlen(line);
    const char *c = line;
    while (c < line + len && (*c == ' ' || *c == '\t')) c++;
    bool done = false;
    if ((size_t)(line + len - c) > 9 && !strncmp(c, "#include", 8) && c[8] == ' ') {
      const char *q = c + 8;
      while (*q == ' ') q++;
      if (*q == '<') {
        const char *nb = q + 1, *ne = nb;
        while (isalnum((unsigned char)*ne) || *ne == '_' || *ne == '.' || *ne == '/') ne++;
        if (*ne == '>' && ne > nb) {
          char name[256];
          size_t nn = (size_t)(ne - nb);
          if (nn >= sizeof name) die("include name too long");
          memcpy(name, nb, nn);
          name[nn] = 0;
          char *ct = chunk_text(name);
          char *r = resolve_includes(ct);
          s_add(&o, r);
          free(r);
          free(ct);
          s_addn(&o, ne + 1, (size_t)(line + len - (ne + 1)));   // the rest of the line stays
          done = true;
        }
      }
    }
    if (!done) s_addn(&o, line, len);
    if (!eol) break;
    s_addc(&o, '\n');
    line = eol + 1;
  }
  return s_take(&o);
}

static char *replace_light_nums(const char *s, const Prog *p) {
  static const char *const K[][2] = {
    { "NUM_SUN_LIGHTS", "numSunLights" }, { "NUM_DIR_LIGHTS", "numDirLights" },
    { "NUM_SPOT_LIGHTS", "numSpotLights" }, { "NUM_SPOT_LIGHT_MAPS", "numSpotLightMaps" },
    { "NUM_SPOT_LIGHT_COORDS", "numSpotLightCoords" }, { "NUM_RECT_AREA_LIGHTS", "numRectAreaLights" },
    { "NUM_POINT_LIGHTS", "numPointLights" }, { "NUM_HEMI_LIGHTS", "numHemiLights" },
    { "NUM_SUN_LIGHT_SHADOWS", "numSunLightShadows" }, { "NUM_DIR_LIGHT_SHADOWS", "numDirLightShadows" },
    { "NUM_SPOT_LIGHT_SHADOWS_WITH_MAPS", "numSpotLightShadowsWithMaps" }, { "NUM_SPOT_LIGHT_SHADOWS", "numSpotLightShadows" },
    { "NUM_POINT_LIGHT_SHADOWS", "numPointLightShadows" },
  };
  char *cur = xstrdup(s);
  for (size_t i = 0; i < sizeof K / sizeof K[0]; i++) {
    char *n = replace_all(cur, K[i][0], PV(p, K[i][1], "0"));
    free(cur);
    cur = n;
  }
  char *a = replace_all(cur, "NUM_CLIPPING_PLANES", PV(p, "numClippingPlanes", "0"));
  free(cur);
  char *b = replace_all(a, "UNION_CLIPPING_PLANES", PV(p, "unionClippingPlanes", "0"));
  free(a);
  return b;
}

static bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

// unrollLoops: #pragma unroll_loop_start\s+for\s*\(\s*int\s+i\s*=\s*(\d+)\s*;\s*i\s*<\s*(\d+)\s*;
//              \s*i\s*\+\+\s*\)\s*{([\s\S]+?)}\s+#pragma unroll_loop_end   (global)
static bool match_loop_header(const char *c, long *start, long *end, const char **body) {
  const char *k = "#pragma unroll_loop_start";
  if (strncmp(c, k, strlen(k))) return false;
  c += strlen(k);
  if (!is_ws(*c)) return false;
  while (is_ws(*c)) c++;
  if (strncmp(c, "for", 3)) return false;
  c += 3;
  while (is_ws(*c)) c++;
  if (*c++ != '(') return false;
  while (is_ws(*c)) c++;
  if (strncmp(c, "int", 3)) return false;
  c += 3;
  if (!is_ws(*c)) return false;
  while (is_ws(*c)) c++;
  if (*c++ != 'i') return false;
  while (is_ws(*c)) c++;
  if (*c++ != '=') return false;
  while (is_ws(*c)) c++;
  if (!isdigit((unsigned char)*c)) return false;
  *start = strtol(c, (char **)&c, 10);
  while (is_ws(*c)) c++;
  if (*c++ != ';') return false;
  while (is_ws(*c)) c++;
  if (*c++ != 'i') return false;
  while (is_ws(*c)) c++;
  if (*c++ != '<') return false;
  while (is_ws(*c)) c++;
  if (!isdigit((unsigned char)*c)) return false;
  *end = strtol(c, (char **)&c, 10);
  while (is_ws(*c)) c++;
  if (*c++ != ';') return false;
  while (is_ws(*c)) c++;
  if (*c++ != 'i') return false;
  while (is_ws(*c)) c++;
  if (c[0] != '+' || c[1] != '+') return false;
  c += 2;
  while (is_ws(*c)) c++;
  if (*c++ != ')') return false;
  while (is_ws(*c)) c++;
  if (*c++ != '{') return false;
  *body = c;
  return true;
}

static char *unroll_loops(const char *s) {
  Str o = {};
  const char *c = s;
  while (*c) {
    long a, b;
    const char *body;
    if (match_loop_header(c, &a, &b, &body)) {
      // lazy body: shortest (>= 1 char) followed by "}" \s+ "#pragma unroll_loop_end"
      const char *close = nullptr;
      for (const char *q = body + 1; *q; q++) {
        if (*q != '}') continue;
        const char *r = q + 1;
        if (!is_ws(*r)) continue;
        while (is_ws(*r)) r++;
        if (!strncmp(r, "#pragma unroll_loop_end", 23)) { close = q; break; }
      }
      if (close) {
        const char *r = close + 1;
        while (is_ws(*r)) r++;
        r += 23;
        size_t blen = (size_t)(close - body);
        char *snippet = malloc(blen + 1);
        memcpy(snippet, body, blen);
        snippet[blen] = 0;
        for (long i = a; i < b; i++) {
          // .replace(/\[\s*i\s*\]/g, '[ ' + i + ' ]').replace(/UNROLLED_LOOP_INDEX/g, i)
          Str t = {};
          for (const char *q = snippet; *q;) {
            if (*q == '[') {
              const char *r2 = q + 1;
              while (is_ws(*r2)) r2++;
              if (*r2 == 'i') {
                const char *r3 = r2 + 1;
                while (is_ws(*r3)) r3++;
                if (*r3 == ']') { s_printf(&t, "[ %ld ]", i); q = r3 + 1; continue; }
              }
            }
            s_addc(&t, *q++);
          }
          char num[32];
          snprintf(num, sizeof num, "%ld", i);
          char *u = replace_all(t.p ? t.p : "", "UNROLLED_LOOP_INDEX", num);
          s_add(&o, u);
          free(u);
          free(t.p);
        }
        free(snippet);
        c = r;
        continue;
      }
    }
    s_addc(&o, *c++);
  }
  return s_take(&o);
}

static const char *PRECISION_BLOCK =
  "precision highp float;\n\tprecision highp int;\n\tprecision highp sampler2D;\n\tprecision highp samplerCube;\n"
  "\tprecision highp sampler3D;\n\tprecision highp sampler2DArray;\n\tprecision highp sampler2DShadow;\n"
  "\tprecision highp samplerCubeShadow;\n\tprecision highp sampler2DArrayShadow;\n\tprecision highp isampler2D;\n"
  "\tprecision highp isampler3D;\n\tprecision highp isamplerCube;\n\tprecision highp isampler2DArray;\n"
  "\tprecision highp usampler2D;\n\tprecision highp usampler3D;\n\tprecision highp usamplerCube;\n"
  "\tprecision highp usampler2DArray;\n\t\n#define HIGH_PRECISION";

typedef struct Lines { const char *l[256]; int n; } Lines;
static void L(Lines *ls, const char *s) { if (s && *s) ls->l[ls->n++] = s; }   // filter( filterEmptyLine )
static void L_if(Lines *ls, bool c, const char *s) { if (c) L(ls, s); }
static char *L_join(const Lines *ls) {
  Str o = {};
  for (int i = 0; i < ls->n; i++) {
    if (i) s_addc(&o, '\n');
    s_add(&o, ls->l[i]);
  }
  return s_take(&o);
}
static const char *fmt(const char *f, ...) {   // leaked small strings (tool lifetime)
  va_list ap;
  va_start(ap, f);
  char buf[1024];
  vsnprintf(buf, sizeof buf, f, ap);
  va_end(ap);
  return xstrdup(buf);
}

// the "*MapUv" parameters and the define each produces
static const char *const UV_PARAMS[][2] = {
  { "mapUv", "MAP_UV" }, { "alphaMapUv", "ALPHAMAP_UV" }, { "lightMapUv", "LIGHTMAP_UV" }, { "aoMapUv", "AOMAP_UV" },
  { "emissiveMapUv", "EMISSIVEMAP_UV" }, { "bumpMapUv", "BUMPMAP_UV" }, { "normalMapUv", "NORMALMAP_UV" },
  { "displacementMapUv", "DISPLACEMENTMAP_UV" }, { "metalnessMapUv", "METALNESSMAP_UV" },
  { "roughnessMapUv", "ROUGHNESSMAP_UV" }, { "anisotropyMapUv", "ANISOTROPYMAP_UV" },
  { "clearcoatMapUv", "CLEARCOATMAP_UV" }, { "clearcoatNormalMapUv", "CLEARCOAT_NORMALMAP_UV" },
  { "clearcoatRoughnessMapUv", "CLEARCOAT_ROUGHNESSMAP_UV" }, { "iridescenceMapUv", "IRIDESCENCEMAP_UV" },
  { "iridescenceThicknessMapUv", "IRIDESCENCE_THICKNESSMAP_UV" }, { "sheenColorMapUv", "SHEEN_COLORMAP_UV" },
  { "sheenRoughnessMapUv", "SHEEN_ROUGHNESSMAP_UV" }, { "specularMapUv", "SPECULARMAP_UV" },
  { "specularColorMapUv", "SPECULAR_COLORMAP_UV" }, { "specularIntensityMapUv", "SPECULAR_INTENSITYMAP_UV" },
  { "transmissionMapUv", "TRANSMISSIONMAP_UV" }, { "thicknessMapUv", "THICKNESSMAP_UV" },
};

static char *custom_defines(const Prog *p) {
  Str o = {};
  for (int i = 0; i < p->ndefines; i++) {
    if (!strcmp(p->defines[i].v, "false")) continue;   // (value === false) is skipped
    if (i && o.n) s_addc(&o, '\n');
    s_printf(&o, "#define %s %s", p->defines[i].k, p->defines[i].v);
  }
  return s_take(&o);
}

// envMapCubeUVHeight -> CUBEUV_* (generateCubeUVSize)
static void cube_uv(const Prog *p, const char **tw, const char **th, const char **mm) {
  const char *h = PV(p, "envMapCubeUVHeight", nullptr);
  *tw = *th = *mm = nullptr;
  if (!h) return;
  // precomputed as three prints them (JS number -> string)
  *tw = PV(p, "cubeUVTexelWidth", nullptr);
  *th = PV(p, "cubeUVTexelHeight", nullptr);
  *mm = PV(p, "cubeUVMaxMip", nullptr);
  if (!*tw || !*th || !*mm) die("%s: envMapCubeUVHeight needs cubeUVTexelWidth/Height/MaxMip", p->path);
}

static char *prefix_vertex(const Prog *p) {
  if (P(p, "isRawShaderMaterial")) {
    Lines ls = {};
    L(&ls, fmt("#define SHADER_TYPE %s", PV(p, "shaderType", "")));
    L(&ls, fmt("#define SHADER_NAME %s", PV(p, "shaderName", "")));
    L(&ls, custom_defines(p));
    char *j = L_join(&ls);
    if (!*j) return j;
    Str o = {};
    s_add(&o, j);
    s_addc(&o, '\n');
    return s_take(&o);
  }
  Lines ls = {};
  L(&ls, PRECISION_BLOCK);
  L(&ls, fmt("#define SHADER_TYPE %s", PV(p, "shaderType", "")));
  L(&ls, fmt("#define SHADER_NAME %s", PV(p, "shaderName", "")));
  L(&ls, custom_defines(p));
  L_if(&ls, P(p, "extensionClipCullDistance"), "#define USE_CLIP_DISTANCE");
  L_if(&ls, P(p, "batching"), "#define USE_BATCHING");
  L_if(&ls, P(p, "batchingColor"), "#define USE_BATCHING_COLOR");
  L_if(&ls, P(p, "instancing"), "#define USE_INSTANCING");
  L_if(&ls, P(p, "instancingColor"), "#define USE_INSTANCING_COLOR");
  L_if(&ls, P(p, "instancingMorph"), "#define USE_INSTANCING_MORPH");
  L_if(&ls, P(p, "useFog") && P(p, "fog"), "#define USE_FOG");
  L_if(&ls, P(p, "useFog") && P(p, "fogExp2"), "#define FOG_EXP2");
  L_if(&ls, P(p, "map"), "#define USE_MAP");
  L_if(&ls, P(p, "envMap"), "#define USE_ENVMAP");
  L_if(&ls, P(p, "envMap"), fmt("#define %s", PV(p, "envMapModeDefine", "ENVMAP_MODE_REFLECTION")));
  L_if(&ls, P(p, "lightMap"), "#define USE_LIGHTMAP");
  L_if(&ls, P(p, "aoMap"), "#define USE_AOMAP");
  L_if(&ls, P(p, "bumpMap"), "#define USE_BUMPMAP");
  L_if(&ls, P(p, "normalMap"), "#define USE_NORMALMAP");
  L_if(&ls, P(p, "normalMapObjectSpace"), "#define USE_NORMALMAP_OBJECTSPACE");
  L_if(&ls, P(p, "normalMapTangentSpace"), "#define USE_NORMALMAP_TANGENTSPACE");
  L_if(&ls, P(p, "displacementMap"), "#define USE_DISPLACEMENTMAP");
  L_if(&ls, P(p, "emissiveMap"), "#define USE_EMISSIVEMAP");
  L_if(&ls, P(p, "anisotropy"), "#define USE_ANISOTROPY");
  L_if(&ls, P(p, "anisotropyMap"), "#define USE_ANISOTROPYMAP");
  L_if(&ls, P(p, "clearcoatMap"), "#define USE_CLEARCOATMAP");
  L_if(&ls, P(p, "clearcoatRoughnessMap"), "#define USE_CLEARCOAT_ROUGHNESSMAP");
  L_if(&ls, P(p, "clearcoatNormalMap"), "#define USE_CLEARCOAT_NORMALMAP");
  L_if(&ls, P(p, "iridescenceMap"), "#define USE_IRIDESCENCEMAP");
  L_if(&ls, P(p, "iridescenceThicknessMap"), "#define USE_IRIDESCENCE_THICKNESSMAP");
  L_if(&ls, P(p, "specularMap"), "#define USE_SPECULARMAP");
  L_if(&ls, P(p, "specularColorMap"), "#define USE_SPECULAR_COLORMAP");
  L_if(&ls, P(p, "specularIntensityMap"), "#define USE_SPECULAR_INTENSITYMAP");
  L_if(&ls, P(p, "roughnessMap"), "#define USE_ROUGHNESSMAP");
  L_if(&ls, P(p, "metalnessMap"), "#define USE_METALNESSMAP");
  L_if(&ls, P(p, "alphaMap"), "#define USE_ALPHAMAP");
  L_if(&ls, P(p, "alphaHash"), "#define USE_ALPHAHASH");
  L_if(&ls, P(p, "transmission"), "#define USE_TRANSMISSION");
  L_if(&ls, P(p, "transmissionMap"), "#define USE_TRANSMISSIONMAP");
  L_if(&ls, P(p, "thicknessMap"), "#define USE_THICKNESSMAP");
  L_if(&ls, P(p, "sheenColorMap"), "#define USE_SHEEN_COLORMAP");
  L_if(&ls, P(p, "sheenRoughnessMap"), "#define USE_SHEEN_ROUGHNESSMAP");
  for (size_t i = 0; i < sizeof UV_PARAMS / sizeof UV_PARAMS[0]; i++)
    L_if(&ls, P(p, UV_PARAMS[i][0]), fmt("#define %s %s", UV_PARAMS[i][1], PV(p, UV_PARAMS[i][0], "")));
  L_if(&ls, P(p, "vertexTangents") && !P(p, "flatShading"), "#define USE_TANGENT");
  L_if(&ls, P(p, "vertexNormals"), "#define HAS_NORMAL");
  L_if(&ls, P(p, "vertexColors"), "#define USE_COLOR");
  L_if(&ls, P(p, "vertexAlphas"), "#define USE_COLOR_ALPHA");
  L_if(&ls, P(p, "vertexUv1s"), "#define USE_UV1");
  L_if(&ls, P(p, "vertexUv2s"), "#define USE_UV2");
  L_if(&ls, P(p, "vertexUv3s"), "#define USE_UV3");
  L_if(&ls, P(p, "pointsUvs"), "#define USE_POINTS_UV");
  L_if(&ls, P(p, "flatShading"), "#define FLAT_SHADED");
  L_if(&ls, P(p, "skinning"), "#define USE_SKINNING");
  L_if(&ls, P(p, "morphTargets"), "#define USE_MORPHTARGETS");
  L_if(&ls, P(p, "morphNormals") && !P(p, "flatShading"), "#define USE_MORPHNORMALS");
  L_if(&ls, P(p, "morphColors"), "#define USE_MORPHCOLORS");
  L_if(&ls, P(p, "doubleSided"), "#define DOUBLE_SIDED");
  L_if(&ls, P(p, "flipSided"), "#define FLIP_SIDED");
  L_if(&ls, P(p, "shadowMapEnabled"), "#define USE_SHADOWMAP");
  L_if(&ls, P(p, "shadowMapEnabled"), fmt("#define %s", PV(p, "shadowMapTypeDefine", "SHADOWMAP_TYPE_PCF")));
  L_if(&ls, P(p, "sizeAttenuation"), "#define USE_SIZEATTENUATION");
  L_if(&ls, P(p, "numLightProbes"), "#define USE_LIGHT_PROBES");
  L_if(&ls, P(p, "logarithmicDepthBuffer"), "#define USE_LOGARITHMIC_DEPTH_BUFFER");
  L_if(&ls, P(p, "reversedDepthBuffer"), "#define USE_REVERSED_DEPTH_BUFFER");
  static const char *const tail[] = {
    "uniform mat4 modelMatrix;", "uniform mat4 modelViewMatrix;", "uniform mat4 projectionMatrix;",
    "uniform mat4 viewMatrix;", "uniform mat3 normalMatrix;", "uniform vec3 cameraPosition;",
    "uniform bool isOrthographic;", "#ifdef USE_INSTANCING", "\tattribute mat4 instanceMatrix;", "#endif",
    "#ifdef USE_INSTANCING_COLOR", "\tattribute vec3 instanceColor;", "#endif", "#ifdef USE_INSTANCING_MORPH",
    "\tuniform sampler2D morphTexture;", "#endif", "attribute vec3 position;", "attribute vec3 normal;",
    "attribute vec2 uv;", "#ifdef USE_UV1", "\tattribute vec2 uv1;", "#endif", "#ifdef USE_UV2",
    "\tattribute vec2 uv2;", "#endif", "#ifdef USE_UV3", "\tattribute vec2 uv3;", "#endif",
    "#ifdef USE_TANGENT", "\tattribute vec4 tangent;", "#endif", "#if defined( USE_COLOR_ALPHA )",
    "\tattribute vec4 color;", "#elif defined( USE_COLOR )", "\tattribute vec3 color;", "#endif",
    "#ifdef USE_SKINNING", "\tattribute vec4 skinIndex;", "\tattribute vec4 skinWeight;", "#endif", "\n",
  };
  for (size_t i = 0; i < sizeof tail / sizeof tail[0]; i++) L(&ls, tail[i]);
  return L_join(&ls);
}

static char *prefix_fragment(const Prog *p) {
  if (P(p, "isRawShaderMaterial")) return prefix_vertex(p);
  const char *tw, *th, *mm;
  cube_uv(p, &tw, &th, &mm);
  Lines ls = {};
  L(&ls, PRECISION_BLOCK);
  L(&ls, fmt("#define SHADER_TYPE %s", PV(p, "shaderType", "")));
  L(&ls, fmt("#define SHADER_NAME %s", PV(p, "shaderName", "")));
  L(&ls, custom_defines(p));
  L_if(&ls, P(p, "useFog") && P(p, "fog"), "#define USE_FOG");
  L_if(&ls, P(p, "useFog") && P(p, "fogExp2"), "#define FOG_EXP2");
  L_if(&ls, P(p, "alphaToCoverage"), "#define ALPHA_TO_COVERAGE");
  L_if(&ls, P(p, "map"), "#define USE_MAP");
  L_if(&ls, P(p, "matcap"), "#define USE_MATCAP");
  L_if(&ls, P(p, "envMap"), "#define USE_ENVMAP");
  L_if(&ls, P(p, "envMap"), fmt("#define %s", PV(p, "envMapTypeDefine", "ENVMAP_TYPE_CUBE")));
  L_if(&ls, P(p, "envMap"), fmt("#define %s", PV(p, "envMapModeDefine", "ENVMAP_MODE_REFLECTION")));
  L_if(&ls, P(p, "envMap"), fmt("#define %s", PV(p, "envMapBlendingDefine", "ENVMAP_BLENDING_NONE")));
  L_if(&ls, tw != nullptr, tw ? fmt("#define CUBEUV_TEXEL_WIDTH %s", tw) : "");
  L_if(&ls, th != nullptr, th ? fmt("#define CUBEUV_TEXEL_HEIGHT %s", th) : "");
  L_if(&ls, mm != nullptr, mm ? fmt("#define CUBEUV_MAX_MIP %s.0", mm) : "");
  L_if(&ls, P(p, "lightMap"), "#define USE_LIGHTMAP");
  L_if(&ls, P(p, "aoMap"), "#define USE_AOMAP");
  L_if(&ls, P(p, "bumpMap"), "#define USE_BUMPMAP");
  L_if(&ls, P(p, "normalMap"), "#define USE_NORMALMAP");
  L_if(&ls, P(p, "normalMapObjectSpace"), "#define USE_NORMALMAP_OBJECTSPACE");
  L_if(&ls, P(p, "normalMapTangentSpace"), "#define USE_NORMALMAP_TANGENTSPACE");
  L_if(&ls, P(p, "packedNormalMap"), "#define USE_PACKED_NORMALMAP");
  L_if(&ls, P(p, "emissiveMap"), "#define USE_EMISSIVEMAP");
  L_if(&ls, P(p, "anisotropy"), "#define USE_ANISOTROPY");
  L_if(&ls, P(p, "anisotropyMap"), "#define USE_ANISOTROPYMAP");
  L_if(&ls, P(p, "clearcoat"), "#define USE_CLEARCOAT");
  L_if(&ls, P(p, "clearcoatMap"), "#define USE_CLEARCOATMAP");
  L_if(&ls, P(p, "clearcoatRoughnessMap"), "#define USE_CLEARCOAT_ROUGHNESSMAP");
  L_if(&ls, P(p, "clearcoatNormalMap"), "#define USE_CLEARCOAT_NORMALMAP");
  L_if(&ls, P(p, "dispersion"), "#define USE_DISPERSION");
  L_if(&ls, P(p, "retroreflection"), "#define USE_RETROREFLECTION");
  L_if(&ls, P(p, "iridescence"), "#define USE_IRIDESCENCE");
  L_if(&ls, P(p, "iridescenceMap"), "#define USE_IRIDESCENCEMAP");
  L_if(&ls, P(p, "iridescenceThicknessMap"), "#define USE_IRIDESCENCE_THICKNESSMAP");
  L_if(&ls, P(p, "specularMap"), "#define USE_SPECULARMAP");
  L_if(&ls, P(p, "specularColorMap"), "#define USE_SPECULAR_COLORMAP");
  L_if(&ls, P(p, "specularIntensityMap"), "#define USE_SPECULAR_INTENSITYMAP");
  L_if(&ls, P(p, "roughnessMap"), "#define USE_ROUGHNESSMAP");
  L_if(&ls, P(p, "metalnessMap"), "#define USE_METALNESSMAP");
  L_if(&ls, P(p, "alphaMap"), "#define USE_ALPHAMAP");
  L_if(&ls, P(p, "alphaTest"), "#define USE_ALPHATEST");
  L_if(&ls, P(p, "alphaHash"), "#define USE_ALPHAHASH");
  L_if(&ls, P(p, "sheen"), "#define USE_SHEEN");
  L_if(&ls, P(p, "sheenColorMap"), "#define USE_SHEEN_COLORMAP");
  L_if(&ls, P(p, "sheenRoughnessMap"), "#define USE_SHEEN_ROUGHNESSMAP");
  L_if(&ls, P(p, "transmission"), "#define USE_TRANSMISSION");
  L_if(&ls, P(p, "transmissionMap"), "#define USE_TRANSMISSIONMAP");
  L_if(&ls, P(p, "thicknessMap"), "#define USE_THICKNESSMAP");
  L_if(&ls, P(p, "vertexTangents") && !P(p, "flatShading"), "#define USE_TANGENT");
  L_if(&ls, P(p, "vertexColors") || P(p, "instancingColor"), "#define USE_COLOR");
  L_if(&ls, P(p, "vertexAlphas") || P(p, "batchingColor"), "#define USE_COLOR_ALPHA");
  L_if(&ls, P(p, "vertexUv1s"), "#define USE_UV1");
  L_if(&ls, P(p, "vertexUv2s"), "#define USE_UV2");
  L_if(&ls, P(p, "vertexUv3s"), "#define USE_UV3");
  L_if(&ls, P(p, "pointsUvs"), "#define USE_POINTS_UV");
  L_if(&ls, P(p, "gradientMap"), "#define USE_GRADIENTMAP");
  L_if(&ls, P(p, "flatShading"), "#define FLAT_SHADED");
  L_if(&ls, P(p, "doubleSided"), "#define DOUBLE_SIDED");
  L_if(&ls, P(p, "flipSided"), "#define FLIP_SIDED");
  L_if(&ls, P(p, "shadowMapEnabled"), "#define USE_SHADOWMAP");
  L_if(&ls, P(p, "shadowMapEnabled"), fmt("#define %s", PV(p, "shadowMapTypeDefine", "SHADOWMAP_TYPE_PCF")));
  L_if(&ls, P(p, "premultipliedAlpha"), "#define PREMULTIPLIED_ALPHA");
  L_if(&ls, P(p, "numLightProbes"), "#define USE_LIGHT_PROBES");
  L_if(&ls, P(p, "numLightProbeGrids"), "#define USE_LIGHT_PROBES_GRID");
  L_if(&ls, P(p, "decodeVideoTexture"), "#define DECODE_VIDEO_TEXTURE");
  L_if(&ls, P(p, "decodeVideoTextureEmissive"), "#define DECODE_VIDEO_TEXTURE_EMISSIVE");
  L_if(&ls, P(p, "logarithmicDepthBuffer"), "#define USE_LOGARITHMIC_DEPTH_BUFFER");
  L_if(&ls, P(p, "reversedDepthBuffer"), "#define USE_REVERSED_DEPTH_BUFFER");
  L(&ls, "uniform mat4 viewMatrix;");
  L(&ls, "uniform vec3 cameraPosition;");
  L(&ls, "uniform bool isOrthographic;");
  const char *tm = PV(p, "toneMapping", "none");
  bool tone = strcmp(tm, "none") != 0;
  L_if(&ls, tone, "#define TONE_MAPPING");
  if (tone) {
    L(&ls, chunk_text("tonemapping_pars_fragment"));
    L(&ls, fmt("vec3 toneMapping( vec3 color ) { return %sToneMapping( color ); }", tm));
  }
  L_if(&ls, P(p, "dithering"), "#define DITHERING");
  L_if(&ls, P(p, "opaque"), "#define OPAQUE");
  L(&ls, chunk_text("colorspace_pars_fragment"));
  // getTexelEncodingFunction( 'linearToOutputTexel', outputColorSpace ): working space is
  // linear sRGB, so the matrix is the identity three prints from its XYZ round trip
  const char *oetf = !strcmp(PV(p, "outputColorSpace", "linear"), "srgb") ? "sRGBTransferOETF" : "LinearTransferOETF";
  L(&ls, fmt("vec4 linearToOutputTexel( vec4 value ) {\n\treturn %s( vec4( value.rgb * mat3( %s ), value.a ) );\n}", oetf,
             "1.0000,-0.0000,-0.0000,-0.0000,1.0000,0.0000,0.0000,0.0000,1.0000"));
  L(&ls, "float luminance( const in vec3 rgb ) {\n\tconst vec3 weights = vec3( 0.2126, 0.7152, 0.0722 );\n\treturn dot( weights, rgb );\n}");
  L_if(&ls, P(p, "useDepthPacking"), fmt("#define DEPTH_PACKING %s", PV(p, "depthPacking", "0")));
  L(&ls, "\n");
  return L_join(&ls);
}

typedef struct Assembled { char *vs, *fs; } Assembled;

static char *apply_patches(const Prog *p, int stage, const char *tpl, const KV *tv, int ntv) {
  char *cur = subst_vars(tpl, p, tv, ntv, stage ? "fragment template" : "vertex template");
  for (int i = 0; i < p->npatches; i++) {
    const Patch *pt = &p->patches[i];
    if (pt->stage != stage) continue;
    char *with = subst_vars(pt->with, p, tv, ntv, "patch");
    char *find = subst_vars(pt->find, p, tv, ntv, "patch");
    char *next;
    if (pt->kind == PATCH_PREPEND) {
      Str o = {};
      s_add(&o, with);
      s_add(&o, cur);
      next = s_take(&o);
    } else if (pt->kind == PATCH_APPEND) {
      Str o = {};
      s_add(&o, cur);
      s_add(&o, with);
      next = s_take(&o);
    } else if (pt->kind == PATCH_ALL) {
      if (!strstr(cur, find)) die("%s: @replace-all: '%s' not found", p->path, find);
      next = replace_all(cur, find, with);
    } else {
      bool found;
      next = js_replace_first(cur, find, with, &found);
      if (!found) die("%s: @replace: '%s' not found in the %s shader", p->path, find, stage ? "fragment" : "vertex");
    }
    free(cur);
    free(with);
    free(find);
    cur = next;
  }
  return cur;
}

static Assembled assemble(const Prog *p, const KV *tv, int ntv) {
  char *vs = apply_patches(p, 0, p->vert_tpl, tv, ntv);
  char *fs = apply_patches(p, 1, p->frag_tpl, tv, ntv);
  char *stages[2] = { vs, fs };
  for (int s = 0; s < 2; s++) {
    char *a = resolve_includes(stages[s]);
    char *b = replace_light_nums(a, p);
    char *c = unroll_loops(b);
    free(a);
    free(b);
    free(stages[s]);
    stages[s] = c;
  }
  Str v = {}, f = {};
  bool raw = P(p, "isRawShaderMaterial");
  char *pv = prefix_vertex(p), *pf = prefix_fragment(p);
  if (!raw) {
    s_add(&v, "#version 300 es\n");
    // customVertexExtensions (multi-draw for BatchedMesh)
    s_add(&v, P(p, "extensionMultiDraw") ? "#extension GL_ANGLE_multi_draw : require" : "");
    s_add(&v, "\n#define attribute in\n#define varying out\n#define texture2D texture\n");
    s_add(&v, pv);
    bool glsl3 = P(p, "glsl3");
    s_add(&f, "#version 300 es\n");
    s_add(&f, "#define varying in\n");
    s_add(&f, glsl3 ? "" : "layout(location = 0) out highp vec4 pc_fragColor;");
    s_add(&f, "\n");
    s_add(&f, glsl3 ? "" : "#define gl_FragColor pc_fragColor");
    s_add(&f, "\n#define gl_FragDepthEXT gl_FragDepth\n#define texture2D texture\n#define textureCube texture\n"
              "#define texture2DProj textureProj\n#define texture2DLodEXT textureLod\n#define texture2DProjLodEXT textureProjLod\n"
              "#define textureCubeLodEXT textureLod\n#define texture2DGradEXT textureGrad\n#define texture2DProjGradEXT textureProjGrad\n"
              "#define textureCubeGradEXT textureGrad\n");
    s_add(&f, pf);
  } else {
    const char *ver = PV(p, "glslVersion", nullptr);
    if (ver) { s_printf(&v, "#version %s\n", ver); s_printf(&f, "#version %s\n", ver); }
    s_add(&v, pv);
    s_add(&f, pf);
  }
  s_add(&v, stages[0]);
  s_add(&f, stages[1]);
  free(pv);
  free(pf);
  free(stages[0]);
  free(stages[1]);
  return (Assembled){ s_take(&v), s_take(&f) };
}

// ---- tier variables ------------------------------------------------------------------------
// Values the JS code interpolates into shader text (${...}), per quality tier. Filled from the
// .prog `var` lines; the tier name itself is always available as ${TIER}.
static int tier_vars(const char *tier, KV *out) {
  int n = 0;
  kv_set(out, &n, "TIER", tier);
  return n;
}

// ---- GLSL ES 3.00 -> Vulkan GLSL 4.50 --------------------------------------------------------

typedef enum TType { T_FLOAT, T_VEC2, T_VEC3, T_VEC4, T_INT, T_IVEC2, T_IVEC3, T_IVEC4, T_UINT, T_BOOL,
                     T_MAT2, T_MAT3, T_MAT4, T_STRUCT,
                     T_SAMPLER2D, T_SAMPLERCUBE, T_SAMPLER3D, T_SAMPLER2DSHADOW, T_SAMPLER2DARRAY, T_ISAMPLER2D, T_USAMPLER2D,
                     T_NONE } TType;
static const struct { const char *name; TType t; } TYPE_NAMES[] = {
  { "float", T_FLOAT }, { "vec2", T_VEC2 }, { "vec3", T_VEC3 }, { "vec4", T_VEC4 }, { "int", T_INT },
  { "ivec2", T_IVEC2 }, { "ivec3", T_IVEC3 }, { "ivec4", T_IVEC4 }, { "uint", T_UINT }, { "bool", T_BOOL },
  { "mat2", T_MAT2 }, { "mat3", T_MAT3 }, { "mat4", T_MAT4 }, { "sampler2D", T_SAMPLER2D },
  { "samplerCube", T_SAMPLERCUBE }, { "sampler3D", T_SAMPLER3D }, { "sampler2DShadow", T_SAMPLER2DSHADOW },
  { "sampler2DArray", T_SAMPLER2DARRAY }, { "isampler2D", T_ISAMPLER2D }, { "usampler2D", T_USAMPLER2D },
};
static bool is_sampler(TType t) { return t >= T_SAMPLER2D && t <= T_USAMPLER2D; }

// leaf type codes shared with the runtime (gfx/program.h)
static const char *leaf_code(TType t) {
  switch (t) {
  case T_FLOAT: return "U_FLOAT"; case T_VEC2: return "U_VEC2"; case T_VEC3: return "U_VEC3"; case T_VEC4: return "U_VEC4";
  case T_INT: return "U_INT"; case T_IVEC2: return "U_IVEC2"; case T_IVEC3: return "U_IVEC3"; case T_IVEC4: return "U_IVEC4";
  case T_UINT: return "U_UINT"; case T_BOOL: return "U_BOOL"; case T_MAT2: return "U_MAT2"; case T_MAT3: return "U_MAT3";
  case T_MAT4: return "U_MAT4";
  default: die("no leaf code for type %d", (int)t);
  }
}
static const char *sampler_code(TType t) {
  switch (t) {
  case T_SAMPLER2D: return "S_2D"; case T_SAMPLERCUBE: return "S_CUBE"; case T_SAMPLER3D: return "S_3D";
  case T_SAMPLER2DSHADOW: return "S_2D_SHADOW"; case T_SAMPLER2DARRAY: return "S_2D_ARRAY";
  case T_ISAMPLER2D: return "S_2D_INT"; case T_USAMPLER2D: return "S_2D_UINT";
  default: die("not a sampler: %d", (int)t);
  }
}

typedef struct Member { char name[64]; TType t; int struct_idx; int array; } Member;   // array 0 = scalar
typedef struct StructDef { char name[64]; Member m[32]; int n; } StructDef;
static StructDef structs[64];
static int nstructs;

static int find_struct(const char *name) {
  for (int i = 0; i < nstructs; i++)
    if (!strcmp(structs[i].name, name)) return i;
  return -1;
}
static TType type_of(const char *name, int *sidx) {
  for (size_t i = 0; i < sizeof TYPE_NAMES / sizeof TYPE_NAMES[0]; i++)
    if (!strcmp(TYPE_NAMES[i].name, name)) { *sidx = -1; return TYPE_NAMES[i].t; }
  int s = find_struct(name);
  if (s >= 0) { *sidx = s; return T_STRUCT; }
  return T_NONE;
}

// std140
static int round_up(int v, int a) { return (v + a - 1) / a * a; }
static void std140(TType t, int sidx, int *size, int *align);
static void std140_struct(int sidx, int *size, int *align) {
  int off = 0, al = 16;
  for (int i = 0; i < structs[sidx].n; i++) {
    const Member *m = &structs[sidx].m[i];
    int s, a;
    std140(m->t, m->struct_idx, &s, &a);
    if (m->array) { a = round_up(a, 16); s = round_up(s, 16) * m->array; }
    off = round_up(off, a) + s;
    if (a > al) al = a;
  }
  *size = round_up(off, 16);
  *align = al;
}
static void std140(TType t, int sidx, int *size, int *align) {
  switch (t) {
  case T_FLOAT: case T_INT: case T_UINT: case T_BOOL: *size = 4; *align = 4; return;
  case T_VEC2: case T_IVEC2: *size = 8; *align = 8; return;
  case T_VEC3: case T_IVEC3: *size = 12; *align = 16; return;
  case T_VEC4: case T_IVEC4: *size = 16; *align = 16; return;
  case T_MAT2: *size = 32; *align = 16; return;
  case T_MAT3: *size = 48; *align = 16; return;
  case T_MAT4: *size = 64; *align = 16; return;
  case T_STRUCT: std140_struct(sidx, size, align); return;
  default: die("type %d has no std140 layout", (int)t);
  }
}

// flattened leaves for the runtime table
typedef struct Leaf { char name[512]; int offset; TType t; } Leaf;
typedef struct Block { Leaf leaf[2048]; int nleaf; int size; } Block;

static void add_leaves(Block *b, const char *name, TType t, int sidx, int array, int offset) {
  int s, a;
  std140(t, sidx, &s, &a);
  int stride = array ? round_up(s, 16) : s;
  int n = array ? array : 1;
  for (int e = 0; e < n; e++) {
    char nm[256];
    if (array) snprintf(nm, sizeof nm, "%s[%d]", name, e);
    else snprintf(nm, sizeof nm, "%s", name);
    int base = offset + e * stride;
    if (t == T_STRUCT) {
      int off = 0;
      for (int i = 0; i < structs[sidx].n; i++) {
        const Member *m = &structs[sidx].m[i];
        int ms, ma;
        std140(m->t, m->struct_idx, &ms, &ma);
        if (m->array) { ma = round_up(ma, 16); ms = round_up(ms, 16) * m->array; }
        off = round_up(off, ma);
        char sub[512];
        snprintf(sub, sizeof sub, "%s.%s", nm, m->name);
        add_leaves(b, sub, m->t, m->struct_idx, m->array, base + off);
        off += ms;
      }
    } else {
      if (b->nleaf >= 2048) die("too many uniform leaves");
      Leaf *l = &b->leaf[b->nleaf++];
      snprintf(l->name, sizeof l->name, "%s", nm);
      l->offset = base;
      l->t = t;
    }
  }
}

// --- tokenizer over preprocessed text
typedef struct Tok { const char *p; int n; } Tok;

static bool is_id0(char c) { return isalpha((unsigned char)c) || c == '_'; }
static bool is_id(char c) { return isalnum((unsigned char)c) || c == '_'; }

static int tokenize(const char *s, size_t len, Tok *out, int max) {
  int n = 0;
  const char *e = s + len;
  for (const char *c = s; c < e;) {
    if (is_ws(*c)) { c++; continue; }
    const char *st = c;
    if (is_id0(*c)) { while (c < e && is_id(*c)) c++; }
    else if (isdigit((unsigned char)*c) || (*c == '.' && c + 1 < e && isdigit((unsigned char)c[1]))) {
      while (c < e && (is_id(*c) || *c == '.' || ((*c == '+' || *c == '-') && (c[-1] == 'e' || c[-1] == 'E')))) c++;
    } else c++;
    if (n >= max) die("statement too long to tokenize");
    out[n++] = (Tok){ st, (int)(c - st) };
  }
  return n;
}
static bool tok_is(Tok t, const char *s) { return (int)strlen(s) == t.n && !strncmp(t.p, s, (size_t)t.n); }
static void tok_str(Tok t, char *buf, size_t n) {
  size_t k = (size_t)t.n < n - 1 ? (size_t)t.n : n - 1;
  memcpy(buf, t.p, k);
  buf[k] = 0;
}

typedef struct Decl {
  bool flat;
  TType t;
  int sidx;
  char type_name[64];
  char name[64];
  int array;     // 0: not an array
} Decl;

// parses `qualifiers type name [N] (, name [N])* ;` into decls; returns count (0 if not a decl)
static bool g_frag_stage;   // for `varying` in GLSL ES 1.00 sources (raw shader materials)

static int parse_decl(Tok *t, int nt, bool *is_uniform, bool *is_in, bool *is_out, Decl *out, int max) {
  int i = 0;
  bool flat = false;
  *is_uniform = *is_in = *is_out = false;
  while (i < nt) {
    if (tok_is(t[i], "layout")) {   // skip layout( ... )
      int d = 0;
      i++;
      for (; i < nt; i++) {
        if (tok_is(t[i], "(")) d++;
        else if (tok_is(t[i], ")") && --d == 0) { i++; break; }
      }
      continue;
    }
    if (tok_is(t[i], "uniform")) { *is_uniform = true; i++; continue; }
    if (tok_is(t[i], "in") || tok_is(t[i], "attribute")) { *is_in = true; i++; continue; }
    if (tok_is(t[i], "varying")) { if (g_frag_stage) *is_in = true; else *is_out = true; i++; continue; }
    if (tok_is(t[i], "out")) { *is_out = true; i++; continue; }
    if (tok_is(t[i], "flat")) { flat = true; i++; continue; }
    if (tok_is(t[i], "highp") || tok_is(t[i], "mediump") || tok_is(t[i], "lowp") || tok_is(t[i], "smooth") ||
        tok_is(t[i], "centroid") || tok_is(t[i], "invariant")) { i++; continue; }
    break;
  }
  if (!*is_uniform && !*is_in && !*is_out) return 0;
  if (i >= nt) return 0;
  char tn[64];
  tok_str(t[i], tn, sizeof tn);
  int sidx;
  TType ty = type_of(tn, &sidx);
  if (ty == T_NONE) die("unknown type '%s' in a global declaration", tn);
  i++;
  int n = 0;
  while (i < nt) {
    if (n >= max) die("too many declarators");
    Decl *d = &out[n++];
    *d = (Decl){ .flat = flat, .t = ty, .sidx = sidx };
    snprintf(d->type_name, sizeof d->type_name, "%s", tn);
    tok_str(t[i++], d->name, sizeof d->name);
    if (i < nt && tok_is(t[i], "[")) {
      d->array = (int)strtol(t[i + 1].p, nullptr, 10);
      if (!tok_is(t[i + 2], "]") || d->array <= 0) die("unsupported array size in declaration of %s", d->name);
      i += 3;
    }
    if (i < nt && tok_is(t[i], ",")) { i++; continue; }
    if (i < nt && tok_is(t[i], ";")) break;
    die("unexpected token after declaration of %s", d->name);
  }
  return n;
}

typedef struct Attr { char name[64]; TType t; int loc, nloc; } Attr;
typedef struct Samp { char name[64]; TType t; int binding; } Samp;
typedef struct Var { char name[64]; char type_name[64]; TType t; int sidx, array, loc, nloc; bool flat; } Var;

typedef struct StageOut {
  Str code;
  Block ublock[2];        // [0] material/frame (binding 0), [1] per-object (binding 1)
  Samp samp[32]; int nsamp;
  Attr attr[32]; int nattr;
  Var vary[48]; int nvary;
} StageOut;

static bool is_object_uniform(const char *n) {
  return !strcmp(n, "modelMatrix") || !strcmp(n, "modelViewMatrix") || !strcmp(n, "normalMatrix") ||
         !strcmp(n, "receiveShadow") || !strcmp(n, "_gl_DrawID");
}

static int loc_count(TType t, int array) {
  int per = t == T_MAT4 ? 4 : t == T_MAT3 ? 3 : t == T_MAT2 ? 2 : 1;
  return per * (array ? array : 1);
}

// replaces whole-word identifier `from` with `to` in s
static char *replace_ident(const char *s, const char *from, const char *to) {
  Str o = {};
  size_t fl = strlen(from);
  for (const char *c = s; *c;) {
    if (!strncmp(c, from, fl) && (c == s || !is_id(c[-1])) && !is_id(c[fl])) {
      s_add(&o, to);
      c += fl;
    } else {
      s_addc(&o, *c++);
    }
  }
  return s_take(&o);
}

// name [ k ] -> name_k (sampler arrays)
static char *split_sampler_array(const char *s, const char *name) {
  Str o = {};
  size_t fl = strlen(name);
  for (const char *c = s; *c;) {
    if (!strncmp(c, name, fl) && (c == s || !is_id(c[-1])) && !is_id(c[fl])) {
      const char *q = c + fl;
      while (is_ws(*q)) q++;
      if (*q == '[') {
        q++;
        while (is_ws(*q)) q++;
        if (isdigit((unsigned char)*q)) {
          long k = strtol(q, (char **)&q, 10);
          while (is_ws(*q)) q++;
          if (*q == ']') {
            s_printf(&o, "%s_%ld", name, k);
            c = q + 1;
            continue;
          }
        }
      }
      die("sampler array %s is indexed with a non-constant index", name);
    }
    s_addc(&o, *c++);
  }
  return s_take(&o);
}

static const char *glslang = "glslangValidator";

static char *preprocess(const char *src0, const char *stage, const char *tmpdir) {
  // Without GL_ANGLE_multi_draw, three's batching chunk falls back to a uniform _gl_DrawID
  // (set per draw here), so the extension line is dropped before preprocessing.
  char *src = replace_all(src0, "#extension GL_ANGLE_multi_draw : require", "");
  char in[1024], out[1024], cmd[4096];
  snprintf(in, sizeof in, "%s/pp.%s", tmpdir, stage);
  snprintf(out, sizeof out, "%s/pp.%s.out", tmpdir, stage);
  write_text(in, src);
  free(src);
  snprintf(cmd, sizeof cmd, "%s -E %s > %s 2>&1", glslang, in, out);
  if (system(cmd) != 0) {
    char *log = read_text(out);
    die("glslang -E failed for %s:\n%s", in, log);
  }
  char *pre = read_text(out);
  if (strcmp(stage, "vert") || !strstr(pre, "_gl_DrawID")) return pre;
  // The fallback's per-draw uniform becomes the instance index instead: the renderer issues
  // each batched item as a draw with first_instance = its draw index (and merges runs of the
  // same geometry into one instanced draw). Only vertex shaders read gl_DrawID.
  char *a = replace_ident(pre, "_gl_DrawID", "gl_InstanceIndex");
  free(pre);
  const char *decl = "uniform int gl_InstanceIndex;";
  char *d = strstr(a, decl);
  if (!d) die("%s: the _gl_DrawID uniform declaration was not found", in);
  memset(d, ' ', strlen(decl));
  return a;
}

// Converts one preprocessed stage. For the fragment stage, `vs` holds the vertex outputs.
static void convert_stage(const char *pre, bool frag, const StageOut *vs, StageOut *so) {
  g_frag_stage = frag;
  bool es100 = strstr(pre, "#version 300 es") == nullptr;   // raw shaders without a version line
  // pass 1: split top-level statements; collect structs first (declared before use)
  nstructs = 0;
  const char *c = pre;
  typedef struct Stmt { const char *p; size_t n; int kind; } Stmt;   // kind: 0 keep, 1 uniform, 2 io, 3 drop, 4 struct
  static Stmt st[8192];
  int nst = 0;
  while (*c) {
    while (is_ws(*c)) c++;
    if (!*c) break;
    const char *s0 = c;
    if (*c == '#') {   // directive line
      while (*c && *c != '\n') c++;
      st[nst++] = (Stmt){ s0, (size_t)(c - s0), 0 };
      continue;
    }
    int depth = 0;
    for (; *c; c++) {
      if (*c == '{') depth++;
      else if (*c == '}') {
        depth--;
        if (depth == 0) {
          const char *q = c + 1;
          while (is_ws(*q)) q++;
          if (*q == ';') c = q;   // struct X { ... };
          c++;
          break;
        }
      } else if (*c == ';' && depth == 0) { c++; break; }
    }
    if (nst >= 8192) die("too many statements");
    st[nst++] = (Stmt){ s0, (size_t)(c - s0), 0 };
  }
  static Tok toks[65536];
  Str keep = {}, structs_src = {};
  static Decl decls[64];
  int next_attr_loc = 0, next_out_loc = 0;
  for (int i = 0; i < nst; i++) {
    Stmt *s = &st[i];
    if (s->p[0] == '#') {
      // keep #extension; drop #version (re-emitted) and #line
      if (!strncmp(s->p, "#extension", 10)) { s_addn(&keep, s->p, s->n); s_addc(&keep, '\n'); }
      continue;
    }
    int nt = tokenize(s->p, s->n, toks, 65536);
    if (!nt) continue;
    if (tok_is(toks[0], "precision")) continue;
    if (tok_is(toks[0], "struct") && nt > 2 && tok_is(toks[2], "{")) {
      StructDef *sd = &structs[nstructs++];
      *sd = (StructDef){};
      tok_str(toks[1], sd->name, sizeof sd->name);
      int k = 3;
      while (k < nt && !tok_is(toks[k], "}")) {
        while (tok_is(toks[k], "highp") || tok_is(toks[k], "mediump") || tok_is(toks[k], "lowp")) k++;
        char tn[64];
        tok_str(toks[k++], tn, sizeof tn);
        int sidx;
        TType ty = type_of(tn, &sidx);
        if (ty == T_NONE) die("struct %s: unknown member type %s", sd->name, tn);
        for (;;) {
          Member *m = &sd->m[sd->n++];
          *m = (Member){ .t = ty, .struct_idx = sidx };
          tok_str(toks[k++], m->name, sizeof m->name);
          if (tok_is(toks[k], "[")) { m->array = (int)strtol(toks[k + 1].p, nullptr, 10); k += 3; }
          if (tok_is(toks[k], ",")) { k++; continue; }
          if (tok_is(toks[k], ";")) { k++; break; }
          die("struct %s: bad member list", sd->name);
        }
      }
      s_addn(&structs_src, s->p, s->n);
      s_addc(&structs_src, '\n');
      continue;
    }
    bool is_u, is_in, is_out;
    int nd = parse_decl(toks, nt, &is_u, &is_in, &is_out, decls, 64);
    if (nd == 0) {
      s_addn(&keep, s->p, s->n);
      s_addc(&keep, '\n');
      continue;
    }
    for (int d = 0; d < nd; d++) {
      Decl *dc = &decls[d];
      if (is_u) {
        if (is_sampler(dc->t)) {
          int n = dc->array ? dc->array : 1;
          for (int e = 0; e < n; e++) {
            Samp *sp = &so->samp[so->nsamp];
            if (dc->array) snprintf(sp->name, sizeof sp->name, "%s_%d", dc->name, e);
            else snprintf(sp->name, sizeof sp->name, "%s", dc->name);
            sp->t = dc->t;
            sp->binding = so->nsamp++;
          }
        } else {
          Block *b = &so->ublock[is_object_uniform(dc->name) ? 1 : 0];
          int sz, al;
          std140(dc->t, dc->sidx, &sz, &al);
          if (dc->array) { al = round_up(al, 16); sz = round_up(sz, 16) * dc->array; }
          int off = round_up(b->size, al);
          add_leaves(b, dc->name, dc->t, dc->sidx, dc->array, off);
          b->size = off + sz;
          // remember the member declaration text for the block
          Leaf *marker = nullptr;
          (void)marker;
        }
      } else if (!frag && is_in) {
        Attr *a = &so->attr[so->nattr++];
        snprintf(a->name, sizeof a->name, "%s", dc->name);
        a->t = dc->t;
        a->nloc = loc_count(dc->t, dc->array);
        a->loc = next_attr_loc;
        next_attr_loc += a->nloc;
        if (dc->array) die("attribute arrays are not supported (%s)", dc->name);
      } else if ((!frag && is_out) || (frag && is_in)) {
        Var *v = &so->vary[so->nvary++];
        *v = (Var){ .t = dc->t, .sidx = dc->sidx, .array = dc->array, .flat = dc->flat, .nloc = loc_count(dc->t, dc->array) };
        snprintf(v->name, sizeof v->name, "%s", dc->name);
        snprintf(v->type_name, sizeof v->type_name, "%s", dc->type_name);
        if (!frag) {
          v->loc = next_out_loc;
          next_out_loc += v->nloc;
        } else {
          v->loc = -1;
          for (int k = 0; k < vs->nvary; k++)
            if (!strcmp(vs->vary[k].name, v->name)) v->loc = vs->vary[k].loc;
          // not written by the vertex stage: GL links as long as it is unused; it becomes a
          // private zero-initialized global (dropped at emit time when unused)
        }
      } else if (frag && is_out) {
        // pc_fragColor (or a GLSL3 material's own outputs): keep as written
        s_addn(&keep, s->p, s->n);
        s_addc(&keep, '\n');
        break;
      }
    }
  }
  // emit
  Str o = {};
  s_add(&o, "#version 450\n");
  s_add(&o, structs_src.p ? structs_src.p : "");
  // uniform blocks: members re-declared from the flattened top-level declarations, in order
  int set_u = frag ? 3 : 1, set_s = frag ? 2 : 0;
  for (int bi = 0; bi < 2; bi++) {
    if (!so->ublock[bi].nleaf) continue;
    s_printf(&o, "layout(set = %d, binding = %d, std140) uniform %s%s {\n", set_u, bi, frag ? "FragBlock" : "VertBlock", bi ? "Object" : "");
    // re-walk statements to print member declarations for this block
    for (int i = 0; i < nst; i++) {
      Stmt *s = &st[i];
      if (s->p[0] == '#') continue;
      int nt = tokenize(s->p, s->n, toks, 65536);
      if (!nt) continue;
      bool is_u, is_in, is_out;
      int nd = parse_decl(toks, nt, &is_u, &is_in, &is_out, decls, 64);
      if (!is_u) continue;
      for (int d = 0; d < nd; d++) {
        Decl *dc = &decls[d];
        if (is_sampler(dc->t) || (is_object_uniform(dc->name) ? 1 : 0) != bi) continue;
        if (dc->array) s_printf(&o, "\t%s %s[%d];\n", dc->type_name, dc->name, dc->array);
        else s_printf(&o, "\t%s %s;\n", dc->type_name, dc->name);
      }
    }
    s_add(&o, "};\n");
  }
  for (int i = 0; i < so->nsamp; i++) {
    const char *tn = "sampler2D";
    for (size_t k = 0; k < sizeof TYPE_NAMES / sizeof TYPE_NAMES[0]; k++)
      if (TYPE_NAMES[k].t == so->samp[i].t) tn = TYPE_NAMES[k].name;
    s_printf(&o, "layout(set = %d, binding = %d) uniform %s %s;\n", set_s, so->samp[i].binding, tn, so->samp[i].name);
  }
  for (int i = 0; i < so->nattr; i++) {
    const char *tn = "float";
    for (size_t k = 0; k < sizeof TYPE_NAMES / sizeof TYPE_NAMES[0]; k++)
      if (TYPE_NAMES[k].t == so->attr[i].t) tn = TYPE_NAMES[k].name;
    s_printf(&o, "layout(location = %d) in %s %s;\n", so->attr[i].loc, tn, so->attr[i].name);
  }
  for (int i = 0; i < so->nvary; i++) {
    const Var *v = &so->vary[i];
    if (v->loc < 0) {
      if (!keep.p || !strstr(keep.p, v->name)) continue;
      s_printf(&o, "%s %s", v->type_name, v->name);
      if (v->array) s_printf(&o, "[%d]", v->array);
      s_printf(&o, " = %s(0);\n", v->type_name);
      continue;
    }
    s_printf(&o, "layout(location = %d) %s%s %s %s", v->loc, v->flat ? "flat " : "", frag ? "in" : "out", v->type_name, v->name);
    if (v->array) s_printf(&o, "[%d]", v->array);
    s_add(&o, ";\n");
  }
  s_add(&o, keep.p ? keep.p : "");
  char *code = s_take(&o);
  // sampler arrays -> separate samplers
  for (int i = 0; i < so->nsamp; i++) {
    const char *nm = so->samp[i].name;
    const char *us = strrchr(nm, '_');
    if (us && isdigit((unsigned char)us[1]) && !strcmp(us + 1, "0")) {
      char base[64];
      snprintf(base, sizeof base, "%.*s", (int)(us - nm), nm);
      // only split names that were declared as arrays (their base name is not itself a sampler)
      bool plain = false;
      for (int k = 0; k < so->nsamp; k++) if (!strcmp(so->samp[k].name, base)) plain = true;
      if (!plain && strstr(code, base)) {
        char *n = split_sampler_array(code, base);
        // the declarations themselves were emitted already split; restore them
        free(code);
        code = n;
      }
    }
  }
  if (es100) {
    // GLSL ES 1.00 built-ins -> 4.50
    char *t1 = replace_ident(code, "texture2D", "texture");
    char *t2 = replace_ident(t1, "textureCube", "texture");
    free(code);
    free(t1);
    code = t2;
    if (frag && strstr(code, "gl_FragColor")) {
      char *t3 = replace_ident(code, "gl_FragColor", "od_FragColor");
      free(code);
      Str w = {};
      // after the #version line
      size_t k = strcspn(t3, "\n");
      if (!t3[k]) die("converted fragment shader has no #version line");
      s_printf(&w, "%.*s", (int)k + 1, t3);
      s_add(&w, "layout(location = 0) out vec4 od_FragColor;\n");
      s_add(&w, t3 + k + 1);
      free(t3);
      code = s_take(&w);
    }
  }
  char *a = replace_ident(code, "gl_VertexID", "gl_VertexIndex");
  char *b = replace_ident(a, "gl_InstanceID", "gl_InstanceIndex");
  free(code);
  free(a);
  if (!frag) {
    // The renderer draws every pass in OpenGL's memory orientation (row 0 = bottom), so that
    // texture v, render-target sampling, viewports and gl_FragCoord all behave as in WebGL:
    // SDL_GPU maps NDC +y to row 0, the extra flip here undoes that (pipelines use clockwise
    // front faces to match, and the final image is blitted to the screen flipped). Clip z comes
    // in WebGL form ([-w, w]) and is remapped to [0, w] as ANGLE does (same clip distances).
    char *c2 = replace_ident(b, "main", "od_main");
    free(b);
    Str w = {};
    s_add(&w, c2);
    s_add(&w, "void main() {\n\tod_main();\n\tgl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;\n\tgl_Position.y = -gl_Position.y;\n}\n");
    free(c2);
    b = s_take(&w);
  }
  so->code = (Str){};
  s_add(&so->code, b);
  free(b);
  free(keep.p);
  free(structs_src.p);
}

// ---- build ---------------------------------------------------------------------------------

static void ident_of(const char *name, char *out, size_t n) {
  size_t k = 0;
  for (const char *c = name; *c && k + 1 < n; c++) out[k++] = (char)(is_id(*c) ? toupper((unsigned char)*c) : '_');
  out[k] = 0;
}

typedef struct Variant { char *vs_es, *fs_es; StageOut *v, *f; char file[256]; } Variant;

int main(int argc, char **argv) {
  shader_root = getenv("SHADER_ROOT") ? getenv("SHADER_ROOT") : "shaders";
  if (getenv("GLSLANG")) glslang = getenv("GLSLANG");
  if (argc >= 6 && !strcmp(argv[1], "assemble")) {
    Prog *p = load_prog(argv[2], argv[3]);
    for (int t = 0; t < NTIERS; t++)
      if (!strcmp(TIERS[t], argv[3]) && !p->tier_used[t]) return 3;   // not built for this tier
    KV tv[16];
    int ntv = tier_vars(argv[3], tv);
    Assembled a = assemble(p, tv, ntv);
    write_text(argv[4], a.vs);
    write_text(argv[5], a.fs);
    return 0;
  }
  if (argc >= 5 && !strcmp(argv[1], "build")) {
    const char *outdir = argv[2], *gen_h = argv[3], *gen_c = argv[4];
    const char *tmpdir = getenv("SHADERGEN_TMP") ? getenv("SHADERGEN_TMP") : "build/pp";
    int nprog = argc - 5;
    Str h = {}, cc = {}, list = {};
    s_add(&h, "// Generated by tools/shadergen.c. Do not edit.\n#pragma once\n#include \"gfx/program_types.h\"\n\n");
    s_add(&h, "typedef enum ProgramId {\n");
    s_add(&cc, "// Generated by tools/shadergen.c. Do not edit.\n#include \"programs.h\"\n#include \"shaders.h\"\n\n");
    Str table = {};
    s_add(&table, "const ProgramInfo PROGRAMS[PROG_COUNT] = {\n");
    for (int pi = 0; pi < nprog; pi++) {
      Variant var[NTIERS] = {};
      int map[NTIERS];
      int nvar = 0;
      Prog *first = nullptr;
      for (int t = 0; t < NTIERS; t++) {
        Prog *p = load_prog(argv[5 + pi], TIERS[t]);
        if (!first) first = p;
        if (!p->tier_used[t]) { map[t] = -1; continue; }
        KV tv[16];
        int ntv = tier_vars(TIERS[t], tv);
        Assembled a = assemble(p, tv, ntv);
        int same = -1;
        for (int k = 0; k < nvar; k++)
          if (!strcmp(var[k].vs_es, a.vs) && !strcmp(var[k].fs_es, a.fs)) same = k;
        if (same >= 0) { map[t] = same; continue; }
        map[t] = nvar;
        Variant *v = &var[nvar++];
        v->vs_es = a.vs;
        v->fs_es = a.fs;
        snprintf(v->file, sizeof v->file, "%s%s%s", p->name, nvar > 1 ? "__" : "", nvar > 1 ? TIERS[t] : "");
      }
      char id[128];
      ident_of(first->name, id, sizeof id);
      s_printf(&h, "  PROG_%s,\n", id);
      // parameter signature (first built tier) for the runtime's material / program checks
      Str sig = {};
      for (int k = 0; k < first->nparams; k++) {
        if (!strcmp(first->params[k].k, "shaderType") || !strcmp(first->params[k].k, "shaderName")) continue;
        s_printf(&sig, ";%s", first->params[k].k);
      }
      s_add(&sig, ";");
      s_printf(&table, "  [PROG_%s] = { \"%s\", \"%s\", %d, { ", id, first->name, sig.p, nvar);
      free(sig.p);
      for (int t = 0; t < NTIERS; t++) s_printf(&table, "%d%s", map[t], t < NTIERS - 1 ? ", " : "");
      s_add(&table, " }, {\n");
      for (int k = 0; k < nvar; k++) {
        Variant *v = &var[k];
        char *pv = preprocess(v->vs_es, "vert", tmpdir);
        char *pf = preprocess(v->fs_es, "frag", tmpdir);
        v->v = calloc(1, sizeof(StageOut));
        v->f = calloc(1, sizeof(StageOut));
        convert_stage(pv, false, nullptr, v->v);
        convert_stage(pf, true, v->v, v->f);
        char path[1024];
        snprintf(path, sizeof path, "%s/%s.vert", outdir, v->file);
        write_text(path, v->v->code.p);
        snprintf(path, sizeof path, "%s/%s.frag", outdir, v->file);
        write_text(path, v->f->code.p);
        s_printf(&list, "%s/%s.vert\n%s/%s.frag\n", outdir, v->file, outdir, v->file);
        char vid[160];
        ident_of(v->file, vid, sizeof vid);
        // per-stage reflection tables
        for (int s = 0; s < 2; s++) {
          StageOut *so = s ? v->f : v->v;
          const char *sn = s ? "f" : "v";
          for (int bi = 0; bi < 2; bi++) {
            s_printf(&cc, "static const UniformLeaf %s_%s_u%d[] = {\n", vid, sn, bi);
            for (int l = 0; l < so->ublock[bi].nleaf; l++)
              s_printf(&cc, "  { \"%s\", %d, %s },\n", so->ublock[bi].leaf[l].name, so->ublock[bi].leaf[l].offset, leaf_code(so->ublock[bi].leaf[l].t));
            s_printf(&cc, "  { nullptr, 0, 0 }\n};\n");
          }
          s_printf(&cc, "static const SamplerSlot %s_%s_s[] = {\n", vid, sn);
          for (int l = 0; l < so->nsamp; l++) s_printf(&cc, "  { \"%s\", %d, %s },\n", so->samp[l].name, so->samp[l].binding, sampler_code(so->samp[l].t));
          s_printf(&cc, "  { nullptr, 0, 0 }\n};\n");
        }
        s_printf(&cc, "static const AttribSlot %s_attr[] = {\n", vid);
        for (int l = 0; l < v->v->nattr; l++) s_printf(&cc, "  { \"%s\", %d, %d, %s },\n", v->v->attr[l].name, v->v->attr[l].loc, v->v->attr[l].nloc, leaf_code(v->v->attr[l].t));
        s_printf(&cc, "  { nullptr, 0, 0, 0 }\n};\n");
        s_printf(&table, "    { SH_%s_VERT, SH_%s_FRAG, %s_attr,\n      { { %d, %s_v_u0 }, { %d, %s_v_u1 } }, %s_v_s,\n      { { %d, %s_f_u0 }, { %d, %s_f_u1 } }, %s_f_s },\n",
                 vid, vid, vid, round_up(v->v->ublock[0].size, 16), vid, round_up(v->v->ublock[1].size, 16), vid, vid,
                 round_up(v->f->ublock[0].size, 16), vid, round_up(v->f->ublock[1].size, 16), vid, vid);
      }
      s_add(&table, "  } },\n");
    }
    s_add(&h, "  PROG_COUNT\n} ProgramId;\n\nextern const ProgramInfo PROGRAMS[PROG_COUNT];\n");
    s_add(&table, "};\n");
    s_add(&cc, table.p);
    write_text(gen_h, h.p);
    write_text(gen_c, cc.p);
    char lp[1024];
    snprintf(lp, sizeof lp, "%s/list.txt", outdir);
    write_text(lp, list.p ? list.p : "");
    return 0;
  }
  fprintf(stderr, "usage:\n  shadergen assemble PROG TIER OUT.vert OUT.frag\n  shadergen build OUTDIR GEN_H GEN_C PROG...\n");
  return 2;
}
