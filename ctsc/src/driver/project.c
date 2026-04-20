#include "ctsc/project.h"

#include "ctsc/json.h"
#include "ctsc/parser.h"
#include "ctsc/emitter.h"
#include "ctsc/buffer.h"
#include "ctsc/arena.h"
#include "ctsc/utf8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#  include <direct.h>
#  define CTSC_PATH_SEP '\\'
#else
#  include <dirent.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#  define CTSC_PATH_SEP '/'
#  define _strdup strdup
#  define _mkdir(p) mkdir((p), 0755)
#  define _getcwd  getcwd
#endif

/* ================================================================== */
/*  Path helpers                                                      */
/* ================================================================== */

static void str_copy(char* dst, size_t cap, const char* s) {
    size_t n = strlen(s);
    if (n >= cap) n = cap - 1;
    memcpy(dst, s, n);
    dst[n] = '\0';
}

static void path_join(char* dst, size_t cap, const char* a, const char* b) {
    if (!a || !*a) { str_copy(dst, cap, b); return; }
    size_t na = strlen(a);
    bool has_sep = na > 0 && (a[na - 1] == '/' || a[na - 1] == '\\');
    snprintf(dst, cap, "%s%s%s", a, has_sep ? "" : "/", b);
}

static void normalize_slashes(char* s) {
    for (; *s; ++s) if (*s == '\\') *s = '/';
}

/* Collapse "//", "/./" and remove any leading "./" in a forward-slash path.
 * Does not resolve ".." (not needed for tsconfig paths). */
static void normalize_path(char* s) {
    normalize_slashes(s);
    /* Strip leading "./" segments. */
    while (s[0] == '.' && s[1] == '/') memmove(s, s + 2, strlen(s + 2) + 1);
    /* Collapse "//" and "/./" in-place. */
    size_t r = 0, w = 0;
    size_t n = strlen(s);
    while (r < n) {
        if (s[r] == '/' && r + 1 < n && s[r + 1] == '/') { r++; continue; }
        if (s[r] == '/' && r + 2 < n && s[r + 1] == '.' && s[r + 2] == '/') { r += 2; continue; }
        if (s[r] == '/' && r + 1 == n - 1 && s[r + 1] == '.') {
            /* trailing "/." */ r++; continue;
        }
        s[w++] = s[r++];
    }
    s[w] = '\0';
}

static void path_dirname(const char* path, char* out, size_t cap) {
    size_t n = strlen(path);
    size_t cut = n;
    while (cut > 0 && path[cut - 1] != '/' && path[cut - 1] != '\\') cut--;
    if (cut == 0) { str_copy(out, cap, "."); return; }
    if (cut >= cap) cut = cap - 1;
    memcpy(out, path, cut - (cut > 0 ? 1 : 0));
    out[cut > 0 ? cut - 1 : 0] = '\0';
}

/* Make `path` absolute relative to `base` if it is relative. `base` must be
 * absolute (or cwd-relative already). Result is written to `out`. */
static void path_resolve(const char* base, const char* path, char* out, size_t cap) {
    if (!path || !*path) { str_copy(out, cap, base); return; }
    bool absolute =
        path[0] == '/' || path[0] == '\\'
#ifdef _WIN32
        || (path[0] && path[1] == ':')
#endif
        ;
    if (absolute) { str_copy(out, cap, path); return; }
    path_join(out, cap, base, path);
}

/* Create `path` and all missing parent directories. Returns 0 on success. */
static int ensure_dir(const char* path) {
    char tmp[1024];
    str_copy(tmp, sizeof(tmp), path);
    normalize_slashes(tmp);
    size_t n = strlen(tmp);
    if (n == 0) return 0;
    for (size_t i = 1; i <= n; ++i) {
        if (tmp[i] == '/' || tmp[i] == '\0') {
            char saved = tmp[i];
            tmp[i] = '\0';
#ifdef _WIN32
            _mkdir(tmp);
#else
            mkdir(tmp, 0755);
#endif
            tmp[i] = saved;
        }
    }
    return 0;
}

/* Remove files + subdirs recursively. Best-effort. */
static void rm_rf(const char* path) {
#ifdef _WIN32
    char pat[1024];
    snprintf(pat, sizeof(pat), "%s\\*", path);
    WIN32_FIND_DATAA d;
    HANDLE h = FindFirstFileA(pat, &d);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (strcmp(d.cFileName, ".") == 0 || strcmp(d.cFileName, "..") == 0) continue;
        char child[1024];
        snprintf(child, sizeof(child), "%s\\%s", path, d.cFileName);
        if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) rm_rf(child);
        else DeleteFileA(child);
    } while (FindNextFileA(h, &d));
    FindClose(h);
    RemoveDirectoryA(path);
#else
    DIR* dir = opendir(path);
    if (!dir) return;
    struct dirent* e;
    while ((e = readdir(dir))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char child[1024];
        snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        struct stat st;
        if (lstat(child, &st) == 0) {
            if (S_ISDIR(st.st_mode)) rm_rf(child);
            else unlink(child);
        }
    }
    closedir(dir);
    rmdir(path);
#endif
}

/* ================================================================== */
/*  Glob matcher                                                      */
/* ================================================================== */
/* Supports:
 *   `**`  - zero or more characters, including `/`
 *   `*`   - zero or more characters except `/`
 *   literal bytes otherwise.
 * Paths and patterns must be normalized to forward slashes.
 */
static bool glob_match(const char* pat, const char* s) {
    while (*pat) {
        if (pat[0] == '*' && pat[1] == '*') {
            pat += 2;
            if (*pat == '/') pat++;
            if (*pat == '\0') return true;
            while (*s) {
                if (glob_match(pat, s)) return true;
                s++;
            }
            return glob_match(pat, s);
        } else if (*pat == '*') {
            pat++;
            if (*pat == '\0') {
                while (*s) { if (*s == '/') return false; s++; }
                return true;
            }
            while (*s && *s != '/') {
                if (glob_match(pat, s)) return true;
                s++;
            }
            return glob_match(pat, s);
        } else if (*pat == *s) {
            pat++; s++;
        } else {
            return false;
        }
    }
    return *s == '\0';
}

/* ================================================================== */
/*  Source file list                                                  */
/* ================================================================== */

typedef struct {
    char** items;
    size_t len;
    size_t cap;
} StrVec;

static void strvec_push(StrVec* v, const char* s) {
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 16;
        v->items = (char**)realloc(v->items, v->cap * sizeof(char*));
    }
    v->items[v->len++] = _strdup(s);
}

static void strvec_free(StrVec* v) {
    for (size_t i = 0; i < v->len; ++i) free(v->items[i]);
    free(v->items);
    v->items = NULL; v->len = v->cap = 0;
}

static int cmp_cstr(const void* a, const void* b) {
    const char* const* pa = (const char* const*)a;
    const char* const* pb = (const char* const*)b;
    return strcmp(*pa, *pb);
}

/* Walk `dir` (absolute), collect all files. `rel_prefix` is the path relative
 * to the project root, with forward slashes. */
static void walk_collect(const char* dir, const char* rel_prefix, StrVec* out_abs, StrVec* out_rel) {
#ifdef _WIN32
    char pat[1024];
    snprintf(pat, sizeof(pat), "%s\\*", dir);
    WIN32_FIND_DATAA d;
    HANDLE h = FindFirstFileA(pat, &d);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(d.cFileName, ".") || !strcmp(d.cFileName, "..")) continue;
        char abs[1024];
        snprintf(abs, sizeof(abs), "%s\\%s", dir, d.cFileName);
        char rel[1024];
        if (rel_prefix && rel_prefix[0]) snprintf(rel, sizeof(rel), "%s/%s", rel_prefix, d.cFileName);
        else                             snprintf(rel, sizeof(rel), "%s", d.cFileName);
        if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            walk_collect(abs, rel, out_abs, out_rel);
        } else {
            normalize_slashes(abs);
            strvec_push(out_abs, abs);
            strvec_push(out_rel, rel);
        }
    } while (FindNextFileA(h, &d));
    FindClose(h);
#else
    DIR* di = opendir(dir);
    if (!di) return;
    struct dirent* e;
    while ((e = readdir(di))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char abs[1024], rel[1024];
        snprintf(abs, sizeof(abs), "%s/%s", dir, e->d_name);
        if (rel_prefix && rel_prefix[0]) snprintf(rel, sizeof(rel), "%s/%s", rel_prefix, e->d_name);
        else                             snprintf(rel, sizeof(rel), "%s", e->d_name);
        struct stat st;
        if (stat(abs, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) walk_collect(abs, rel, out_abs, out_rel);
        else { strvec_push(out_abs, abs); strvec_push(out_rel, rel); }
    }
    closedir(di);
#endif
}

/* ================================================================== */
/*  tsconfig reading                                                  */
/* ================================================================== */

typedef struct {
    char            tsconfig_path[1024]; /* absolute path to tsconfig.json */
    char            project_dir[1024];   /* absolute dir containing tsconfig */
    char            root_dir[1024];      /* absolute */
    char            out_dir[1024];       /* absolute */
    StrVec          include_globs;
    StrVec          exclude_globs;
    StrVec          files_list;          /* explicit `files` from tsconfig, relative to project_dir */
    bool            has_include;
    bool            has_exclude;
    bool            has_files;
    /* Merged JSON root (after extends resolution). Kept so we can be freed at the end. */
    CtscJsonValue*  root;
} TsConfig;

static void tsconfig_free(TsConfig* c) {
    strvec_free(&c->include_globs);
    strvec_free(&c->exclude_globs);
    strvec_free(&c->files_list);
    if (c->root) { ctsc_json_free(c->root); free(c->root); c->root = NULL; }
}

/* Deep clone a JSON value. Caller owns the returned *out memory. */
static void json_clone(const CtscJsonValue* src, CtscJsonValue* out) {
    out->kind = src->kind;
    switch (src->kind) {
        case CTSC_JSON_NULL:   break;
        case CTSC_JSON_BOOL:   out->u.b = src->u.b; break;
        case CTSC_JSON_NUMBER: out->u.n = src->u.n; break;
        case CTSC_JSON_STRING:
            out->u.s.data = _strdup(src->u.s.data);
            out->u.s.len  = src->u.s.len;
            break;
        case CTSC_JSON_ARRAY: {
            out->u.arr.len = src->u.arr.len;
            out->u.arr.items = (CtscJsonValue*)calloc(src->u.arr.len, sizeof(CtscJsonValue));
            for (size_t i = 0; i < src->u.arr.len; ++i) json_clone(&src->u.arr.items[i], &out->u.arr.items[i]);
            break;
        }
        case CTSC_JSON_OBJECT: {
            out->u.obj.len = src->u.obj.len;
            out->u.obj.members = (CtscJsonMember*)calloc(src->u.obj.len, sizeof(CtscJsonMember));
            for (size_t i = 0; i < src->u.obj.len; ++i) {
                out->u.obj.members[i].key = _strdup(src->u.obj.members[i].key);
                json_clone(&src->u.obj.members[i].value, &out->u.obj.members[i].value);
            }
            break;
        }
    }
}

/* Deep-merge `ext` into `base` (values in `ext` win, nested objects merge
 * recursively; arrays get replaced). Both must be objects. */
static void json_merge(CtscJsonValue* base, const CtscJsonValue* ext) {
    if (!base || !ext) return;
    if (base->kind != CTSC_JSON_OBJECT || ext->kind != CTSC_JSON_OBJECT) return;
    for (size_t i = 0; i < ext->u.obj.len; ++i) {
        const char* k = ext->u.obj.members[i].key;
        const CtscJsonValue* v = &ext->u.obj.members[i].value;
        ptrdiff_t found = -1;
        for (size_t j = 0; j < base->u.obj.len; ++j) {
            if (strcmp(base->u.obj.members[j].key, k) == 0) { found = (ptrdiff_t)j; break; }
        }
        if (found < 0) {
            base->u.obj.members = (CtscJsonMember*)realloc(
                base->u.obj.members,
                (base->u.obj.len + 1) * sizeof(CtscJsonMember));
            CtscJsonMember* m = &base->u.obj.members[base->u.obj.len++];
            m->key = _strdup(k);
            json_clone(v, &m->value);
        } else {
            CtscJsonValue* cur = &base->u.obj.members[found].value;
            if (cur->kind == CTSC_JSON_OBJECT && v->kind == CTSC_JSON_OBJECT) {
                json_merge(cur, v);
            } else {
                ctsc_json_free(cur);
                json_clone(v, cur);
            }
        }
    }
}

/* Read a JSON file into memory. Returns a malloc'd null-terminated buffer and
 * sets *out_len. Caller frees. */
static char* read_file_all(const char* path, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char* buf = (char*)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t r = fread(buf, 1, (size_t)n, f);
    buf[r] = '\0';
    fclose(f);
    if (out_len) *out_len = r;
    return buf;
}

/* Parse tsconfig at `path`, following extends up to one level. Returns a
 * freshly-allocated CtscJsonValue (object), or NULL on failure. */
static CtscJsonValue* load_tsconfig_with_extends(const char* path) {
    size_t n = 0;
    char* txt = read_file_all(path, &n);
    if (!txt) { fprintf(stderr, "ctsc: cannot read '%s'\n", path); return NULL; }
    char err[256];
    CtscJsonValue* cfg = ctsc_json_parse(txt, n, err, sizeof(err));
    free(txt);
    if (!cfg) { fprintf(stderr, "ctsc: %s: %s\n", path, err); return NULL; }
    const CtscJsonValue* ext = ctsc_json_obj_get(cfg, "extends");
    if (ext && ext->kind == CTSC_JSON_STRING) {
        char base_dir[1024];
        path_dirname(path, base_dir, sizeof(base_dir));
        char ext_path[1024];
        path_resolve(base_dir, ext->u.s.data, ext_path, sizeof(ext_path));
        /* tsconfig allows omitting ".json" extension in extends */
        FILE* tst = fopen(ext_path, "rb");
        if (!tst) {
            char with_ext[1024];
            snprintf(with_ext, sizeof(with_ext), "%s.json", ext_path);
            tst = fopen(with_ext, "rb");
            if (tst) str_copy(ext_path, sizeof(ext_path), with_ext);
        }
        if (!tst) { fprintf(stderr, "ctsc: cannot resolve extends '%s'\n", ext->u.s.data); }
        else { fclose(tst); }
        CtscJsonValue* base = load_tsconfig_with_extends(ext_path);
        if (base) {
            json_merge(base, cfg);
            ctsc_json_free(cfg);
            free(cfg);
            return base;
        }
        /* Fall through: use cfg as-is if extends cannot be loaded */
    }
    return cfg;
}

static void collect_string_array(const CtscJsonValue* arr, StrVec* out) {
    if (!arr || arr->kind != CTSC_JSON_ARRAY) return;
    for (size_t i = 0; i < arr->u.arr.len; ++i) {
        const char* s = ctsc_json_as_cstr(&arr->u.arr.items[i]);
        if (s) strvec_push(out, s);
    }
}

static int load_tsconfig(const char* path, TsConfig* out) {
    memset(out, 0, sizeof(*out));

    /* resolve absolute tsconfig path */
    char cwd[1024];
#ifdef _WIN32
    _getcwd(cwd, sizeof(cwd));
#else
    getcwd(cwd, sizeof(cwd));
#endif
    path_resolve(cwd, path, out->tsconfig_path, sizeof(out->tsconfig_path));

    /* If it's a directory, append tsconfig.json */
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(out->tsconfig_path);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        char p[1024];
        snprintf(p, sizeof(p), "%s\\tsconfig.json", out->tsconfig_path);
        str_copy(out->tsconfig_path, sizeof(out->tsconfig_path), p);
    }
#else
    struct stat st;
    if (stat(out->tsconfig_path, &st) == 0 && S_ISDIR(st.st_mode)) {
        char p[1024];
        snprintf(p, sizeof(p), "%s/tsconfig.json", out->tsconfig_path);
        str_copy(out->tsconfig_path, sizeof(out->tsconfig_path), p);
    }
#endif

    normalize_path(out->tsconfig_path);
    path_dirname(out->tsconfig_path, out->project_dir, sizeof(out->project_dir));
    normalize_path(out->project_dir);

    out->root = load_tsconfig_with_extends(out->tsconfig_path);
    if (!out->root) return 1;

    const CtscJsonValue* co = ctsc_json_obj_get(out->root, "compilerOptions");
    const char* root_dir = co ? ctsc_json_as_cstr(ctsc_json_obj_get(co, "rootDir")) : NULL;
    const char* out_dir  = co ? ctsc_json_as_cstr(ctsc_json_obj_get(co, "outDir")) : NULL;

    path_resolve(out->project_dir, root_dir ? root_dir : "./src", out->root_dir, sizeof(out->root_dir));
    path_resolve(out->project_dir, out_dir  ? out_dir  : "./dist", out->out_dir,  sizeof(out->out_dir));
    normalize_path(out->root_dir);
    normalize_path(out->out_dir);

    const CtscJsonValue* inc = ctsc_json_obj_get(out->root, "include");
    const CtscJsonValue* exc = ctsc_json_obj_get(out->root, "exclude");
    const CtscJsonValue* fls = ctsc_json_obj_get(out->root, "files");
    if (inc) { out->has_include = true; collect_string_array(inc, &out->include_globs); }
    if (exc) { out->has_exclude = true; collect_string_array(exc, &out->exclude_globs); }
    if (fls) { out->has_files   = true; collect_string_array(fls, &out->files_list); }

    /* tsc semantics: when `files` is explicit AND non-empty, it is the
     * authoritative list; include/exclude are ignored for file selection.
     * When only `include` is explicit, it defines the set; exclude filters
     * from it. When neither is present, compile everything under project_dir
     * matching *.ts/*.tsx. */
    if (!out->has_files && (!out->has_include || out->include_globs.len == 0)) {
        strvec_push(&out->include_globs, "**/*.ts");
        strvec_push(&out->include_globs, "**/*.tsx");
    }

    if (!out->has_exclude) {
        strvec_push(&out->exclude_globs, "node_modules/**");
    }

    return 0;
}

/* Decide if `rel` (path relative to project_dir, forward slashes) should be
 * compiled: must match one include glob AND no exclude glob; must end in
 * .ts/.tsx and not .d.ts. */
static bool include_file(const TsConfig* c, const char* rel) {
    size_t n = strlen(rel);
    bool ts_ext = (n > 3 && strcmp(rel + n - 3, ".ts") == 0)
               || (n > 4 && strcmp(rel + n - 4, ".tsx") == 0);
    if (!ts_ext) return false;
    if (n > 5 && strcmp(rel + n - 5, ".d.ts") == 0) return false;

    bool matched = false;
    for (size_t i = 0; i < c->include_globs.len; ++i) {
        if (glob_match(c->include_globs.items[i], rel)) { matched = true; break; }
    }
    if (!matched) return false;

    for (size_t i = 0; i < c->exclude_globs.len; ++i) {
        if (glob_match(c->exclude_globs.items[i], rel)) return false;
    }
    return true;
}

/* ================================================================== */
/*  Emit pipeline                                                     */
/* ================================================================== */

static int emit_one(const char* src_path, const char* dest_path) {
    size_t src_len = 0;
    char* src = ctsc_read_file(src_path, &src_len);
    if (!src) { fprintf(stderr, "ctsc: cannot read '%s'\n", src_path); return 1; }

    CtscArena a; ctsc_arena_init(&a, 64 * 1024);
    CtscParseResult r = ctsc_parse(src, src_len, &a);

    CtscUtf16Buf u16; ctsc_utf16_init(&u16);
    ctsc_utf16_from_utf8(&u16, src, src_len);

    CtscBuffer out; ctsc_buf_init(&out);
    ctsc_emit_js(r.sourceFile, &u16, &out);

    /* Ensure output directory exists */
    char dest_dir[1024];
    path_dirname(dest_path, dest_dir, sizeof(dest_dir));
    ensure_dir(dest_dir);

    FILE* f = fopen(dest_path, "wb");
    if (!f) {
        fprintf(stderr, "ctsc: cannot write '%s'\n", dest_path);
        ctsc_buf_free(&out); ctsc_utf16_free(&u16); ctsc_arena_free(&a); free(src);
        return 1;
    }
    fwrite(out.data, 1, out.len, f);
    fclose(f);

    ctsc_buf_free(&out);
    ctsc_utf16_free(&u16);
    ctsc_arena_free(&a);
    free(src);
    return 0;
}

/* ================================================================== */
/*  Driver                                                            */
/* ================================================================== */

int ctsc_run_project(const CtscProjectOptions* opts) {
    if (!opts || !opts->tsconfig_path) { fprintf(stderr, "ctsc: --project needs a path\n"); return 2; }

    TsConfig cfg;
    if (load_tsconfig(opts->tsconfig_path, &cfg) != 0) return 1;

    if (opts->verbose) {
        fprintf(stderr, "[ctsc] tsconfig=%s\n", cfg.tsconfig_path);
        fprintf(stderr, "[ctsc] rootDir=%s\n", cfg.root_dir);
        fprintf(stderr, "[ctsc] outDir=%s\n", cfg.out_dir);
    }

    /* Clear outDir and create fresh. */
    rm_rf(cfg.out_dir);
    ensure_dir(cfg.out_dir);

    if (opts->write_package_json) {
        char pj[1024];
        snprintf(pj, sizeof(pj), "%s/package.json", cfg.out_dir);
        FILE* f = fopen(pj, "wb");
        if (f) { fputs("{\"type\":\"module\"}\n", f); fclose(f); }
    }

    /* Collect source files. If tsconfig has an explicit `files` list, use it
     * verbatim (relative to project_dir). Otherwise walk project_dir and
     * apply include/exclude globs. */
    StrVec abs_all; StrVec rel_all;
    memset(&abs_all, 0, sizeof(abs_all));
    memset(&rel_all, 0, sizeof(rel_all));

    StrVec abs_src; StrVec rel_src;
    memset(&abs_src, 0, sizeof(abs_src));
    memset(&rel_src, 0, sizeof(rel_src));

    if (cfg.has_files && cfg.files_list.len > 0) {
        for (size_t i = 0; i < cfg.files_list.len; ++i) {
            char abs[1024];
            path_resolve(cfg.project_dir, cfg.files_list.items[i], abs, sizeof(abs));
            normalize_path(abs);
            char* rel = cfg.files_list.items[i];
            /* Skip .d.ts just like parseJsonConfigFileContent would. */
            size_t rl = strlen(rel);
            if (rl > 5 && strcmp(rel + rl - 5, ".d.ts") == 0) continue;
            strvec_push(&abs_src, abs);
            strvec_push(&rel_src, rel);
        }
    } else {
        walk_collect(cfg.project_dir, "", &abs_all, &rel_all);
        for (size_t i = 0; i < rel_all.len; ++i) {
            if (include_file(&cfg, rel_all.items[i])) {
                strvec_push(&abs_src, abs_all.items[i]);
                strvec_push(&rel_src, rel_all.items[i]);
            }
        }
    }

    /* Sort for deterministic order. */
    /* Note: rel_src and abs_src are parallel; to sort together, sort indices. */
    size_t m = rel_src.len;
    size_t* idx = (size_t*)malloc(m * sizeof(size_t));
    for (size_t i = 0; i < m; ++i) idx[i] = i;
    /* simple insertion sort for small arrays */
    for (size_t i = 1; i < m; ++i) {
        size_t k = idx[i], j = i;
        while (j > 0 && strcmp(rel_src.items[idx[j - 1]], rel_src.items[k]) > 0) { idx[j] = idx[j - 1]; j--; }
        idx[j] = k;
    }

    int rc = 0;
    for (size_t ii = 0; ii < m; ++ii) {
        size_t i = idx[ii];
        const char* abs_s = abs_src.items[i];
        const char* rel_s = rel_src.items[i];

        /* Compute out path: rel_s starts under project_dir. Strip the rootDir
         * prefix (relative to project_dir) to get the path inside outDir. */
        char root_rel[1024];
        /* root_dir absolute - project_dir absolute = root_rel */
        size_t plen = strlen(cfg.project_dir);
        if (strncmp(cfg.root_dir, cfg.project_dir, plen) == 0
            && (cfg.root_dir[plen] == '/' || cfg.root_dir[plen] == '\0')) {
            const char* r = cfg.root_dir + plen;
            if (*r == '/') r++;
            str_copy(root_rel, sizeof(root_rel), r);
        } else {
            str_copy(root_rel, sizeof(root_rel), "");
        }

        const char* under_root = rel_s;
        size_t rr = strlen(root_rel);
        if (rr > 0 && strncmp(rel_s, root_rel, rr) == 0 && rel_s[rr] == '/') {
            under_root = rel_s + rr + 1;
        }

        /* Build dest path: outDir/under_root replacing .ts(x) with .js */
        char dest[1024];
        snprintf(dest, sizeof(dest), "%s/%s", cfg.out_dir, under_root);
        size_t dl = strlen(dest);
        if (dl > 4 && strcmp(dest + dl - 4, ".tsx") == 0) { strcpy(dest + dl - 4, ".js"); }
        else if (dl > 3 && strcmp(dest + dl - 3, ".ts") == 0) { strcpy(dest + dl - 3, ".js"); }

        if (opts->verbose) fprintf(stderr, "[ctsc] emit %s -> %s\n", rel_s, dest);

        int r = emit_one(abs_s, dest);
        if (r != 0) { rc = r; }
    }

    free(idx);
    strvec_free(&abs_all); strvec_free(&rel_all);
    strvec_free(&abs_src); strvec_free(&rel_src);
    tsconfig_free(&cfg);
    return rc;
}
