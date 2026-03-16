#include "extensions.h"
#include "prefix_extension.h"
#include "builtins.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

#ifdef _MSC_VER
#define strdup _strdup
#endif

#ifdef _WIN32
#include <windows.h>
typedef HMODULE DynLibHandle;
#else
#include <dlfcn.h>
typedef void* DynLibHandle;
#endif

typedef struct ExtensionExposure {
    char* key;
    struct ExtensionExposure* next;
} ExtensionExposure;

typedef struct LoadedExtension {
    char* canonical_path;
    DynLibHandle handle;
    prefix_extension_init_fn init_fn;
    ExtensionExposure* exposures;
    struct LoadedExtension* next;
} LoadedExtension;

static LoadedExtension* g_loaded = NULL;
static char* g_interpreter_dir = NULL;
static char* g_cwd_dir = NULL;
static const char* g_loading_extension_name = NULL;
static const char* g_loading_scope_name = NULL;

static void set_error(char** error_out, const char* msg) {
    if (!error_out) return;
    free(*error_out);
    *error_out = msg ? strdup(msg) : NULL;
}

static void set_errorf(char** error_out, const char* prefix, const char* value) {
    if (!error_out) return;
    size_t a = prefix ? strlen(prefix) : 0;
    size_t b = value ? strlen(value) : 0;
    char* out = malloc(a + b + 1);
    if (!out) return;
    if (prefix) memcpy(out, prefix, a);
    if (value) memcpy(out + a, value, b);
    out[a + b] = '\0';
    free(*error_out);
    *error_out = out;
}

static int path_is_absolute(const char* path) {
    if (!path || path[0] == '\0') return 0;
#ifdef _WIN32
    if ((isalpha((unsigned char)path[0]) && path[1] == ':') ||
        (path[0] == '\\' && path[1] == '\\') ||
        path[0] == '/' || path[0] == '\\') {
        return 1;
    }
#else
    if (path[0] == '/') return 1;
#endif
    return 0;
}

static int file_exists_regular(const char* path) {
    struct stat st;
    if (!path) return 0;
    if (stat(path, &st) != 0) return 0;
    return (st.st_mode & S_IFMT) == S_IFREG;
}

static int ends_with_case_insensitive(const char* s, const char* suffix) {
    if (!s || !suffix) return 0;
    size_t ls = strlen(s);
    size_t lf = strlen(suffix);
    if (lf > ls) return 0;
    const char* tail = s + (ls - lf);
    for (size_t i = 0; i < lf; i++) {
        unsigned char a = (unsigned char)tail[i];
        unsigned char b = (unsigned char)suffix[i];
        if ((unsigned char)tolower(a) != (unsigned char)tolower(b)) return 0;
    }
    return 1;
}

static const char* platform_dynlib_suffix(void) {
#ifdef _WIN32
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}

static int has_dynlib_suffix(const char* path) {
    return ends_with_case_insensitive(path, ".dll") ||
           ends_with_case_insensitive(path, ".so") ||
           ends_with_case_insensitive(path, ".dylib");
}

static char* path_join2(const char* a, const char* b) {
    if (!a || a[0] == '\0') return b ? strdup(b) : NULL;
    if (!b || b[0] == '\0') return strdup(a);

    size_t la = strlen(a);
    size_t lb = strlen(b);
    int need_sep = (la > 0 && a[la - 1] != '/' && a[la - 1] != '\\');

    char* out = malloc(la + (size_t)need_sep + lb + 1);
    if (!out) return NULL;
    memcpy(out, a, la);
    size_t p = la;
    if (need_sep) out[p++] = '/';
    memcpy(out + p, b, lb);
    out[p + lb] = '\0';
    return out;
}

static char* path_basename_no_ext_dup(const char* path) {
    if (!path) return strdup("extension");
    const char* base = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    char* out = strdup(base);
    if (!out) return NULL;
    char* dot = strrchr(out, '.');
    if (dot) *dot = '\0';
    if (out[0] == '\0') {
        free(out);
        return strdup("extension");
    }
    return out;
}

static char* canonicalize_existing_path(const char* path) {
    if (!path || path[0] == '\0') return NULL;
#ifdef _WIN32
    char full[_MAX_PATH];
    if (_fullpath(full, path, _MAX_PATH)) return strdup(full);
#else
    char full[4096];
    if (realpath(path, full)) return strdup(full);
#endif
    return strdup(path);
}

static char* resolve_extension_path(const char* input, const char* base_dir) {
    if (!input || input[0] == '\0') return NULL;

    if (path_is_absolute(input) && file_exists_regular(input)) {
        return canonicalize_existing_path(input);
    }

    if (file_exists_regular(input)) {
        return canonicalize_existing_path(input);
    }

    if (base_dir && base_dir[0] != '\0') {
        char* p = path_join2(base_dir, input);
        if (p && file_exists_regular(p)) {
            char* c = canonicalize_existing_path(p);
            free(p);
            return c;
        }
        free(p);
    }

    if (g_cwd_dir && g_cwd_dir[0] != '\0') {
        char* p = path_join2(g_cwd_dir, input);
        if (p && file_exists_regular(p)) {
            char* c = canonicalize_existing_path(p);
            free(p);
            return c;
        }
        free(p);
    }

    if (g_interpreter_dir && g_interpreter_dir[0] != '\0') {
        const char* ext_roots[] = { "ext/std", "ext/usr" };
        for (size_t i = 0; i < sizeof(ext_roots) / sizeof(ext_roots[0]); i++) {
            char* ext_dir = path_join2(g_interpreter_dir, ext_roots[i]);
            if (!ext_dir) continue;
            char* p = path_join2(ext_dir, input);
            free(ext_dir);
            if (p && file_exists_regular(p)) {
                char* c = canonicalize_existing_path(p);
                free(p);
                return c;
            }
            free(p);
        }
    }

    /* Also check the interpreter's bundled and user module directories for
       extensions. This allows pointer files that list a bare filename (for
       example, "image.dll") to resolve to lib/std/<name>/<file>,
       lib/usr/<name>/<file>, or their root-level equivalents. */
    if (g_interpreter_dir && g_interpreter_dir[0] != '\0') {
        const char* lib_roots[] = { "lib/std", "lib/usr" };
        for (size_t i = 0; i < sizeof(lib_roots) / sizeof(lib_roots[0]); i++) {
            char* lib_dir = path_join2(g_interpreter_dir, lib_roots[i]);
            if (!lib_dir) continue;

            char* p1 = path_join2(lib_dir, input);
            if (p1 && file_exists_regular(p1)) {
                char* c = canonicalize_existing_path(p1);
                free(p1);
                free(lib_dir);
                return c;
            }
            free(p1);

            char* base = path_basename_no_ext_dup(input);
            char* subdir = path_join2(lib_dir, base);
            char* p2 = path_join2(subdir, input);
            if (p2 && file_exists_regular(p2)) {
                char* c = canonicalize_existing_path(p2);
                free(p2);
                free(subdir);
                free(base);
                free(lib_dir);
                return c;
            }
            free(p2);
            free(subdir);
            free(base);
            free(lib_dir);
        }
    }

    return NULL;
}

static char* normalize_extension_spec_path(const char* spec) {
    if (!spec) return NULL;
    size_t n = strlen(spec);
    char* out = malloc(n + 2);
    if (!out) return NULL;

    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        if (spec[i] == '.' && spec[i + 1] == '.') {
            out[w++] = '/';
            i++;
            continue;
        }
        out[w++] = spec[i];
    }
    out[w] = '\0';
    return out;
}

static char* extension_name_from_spec(const char* spec) {
    if (!spec || spec[0] == '\0') return strdup("extension");

    char* norm = normalize_extension_spec_path(spec);
    if (!norm) return NULL;

    if (has_dynlib_suffix(norm)) {
        char* dot = strrchr(norm, '.');
        if (dot) *dot = '\0';
    }

    size_t len = strlen(norm);
    if (len >= 5 && strcmp(norm + len - 5, "/init") == 0) {
        norm[len - 5] = '\0';
    }

    char* name = path_basename_no_ext_dup(norm);
    free(norm);
    return name;
}

static char* resolve_extension_spec_path(const char* spec, const char* base_dir) {
    if (!spec || spec[0] == '\0') return NULL;

    if (has_dynlib_suffix(spec)) {
        return resolve_extension_path(spec, base_dir);
    }

    char* norm = normalize_extension_spec_path(spec);
    if (!norm) return NULL;

    const char* suffix = platform_dynlib_suffix();
    size_t len = strlen(norm);

    size_t n1 = len + strlen(suffix) + 1;
    char* c1 = malloc(n1);
    if (!c1) {
        free(norm);
        return NULL;
    }
    snprintf(c1, n1, "%s%s", norm, suffix);
    char* resolved = resolve_extension_path(c1, base_dir);
    free(c1);
    if (resolved) {
        free(norm);
        return resolved;
    }

    size_t n2 = len + strlen("/init") + strlen(suffix) + 1;
    char* c2 = malloc(n2);
    if (!c2) {
        free(norm);
        return NULL;
    }
    snprintf(c2, n2, "%s/init%s", norm, suffix);
    resolved = resolve_extension_path(c2, base_dir);
    free(c2);
    free(norm);
    return resolved;
}

static char* exposure_key_for(const char* scope_name, const char* ext_name) {
    const char* scope = (scope_name && scope_name[0] != '\0') ? scope_name : "";
    const char* ext = (ext_name && ext_name[0] != '\0') ? ext_name : "extension";
    size_t n = strlen(scope) + 1 + strlen(ext) + 1;
    char* out = malloc(n);
    if (!out) return NULL;
    snprintf(out, n, "%s|%s", scope, ext);
    return out;
}

static int exposure_exists(LoadedExtension* le, const char* key) {
    if (!le || !key) return 0;
    for (ExtensionExposure* e = le->exposures; e; e = e->next) {
        if (e->key && strcmp(e->key, key) == 0) return 1;
    }
    return 0;
}

static int exposure_add(LoadedExtension* le, const char* key) {
    if (!le || !key) return -1;
    ExtensionExposure* ex = calloc(1, sizeof(ExtensionExposure));
    if (!ex) return -1;
    ex->key = strdup(key);
    if (!ex->key) {
        free(ex);
        return -1;
    }
    ex->next = le->exposures;
    le->exposures = ex;
    return 0;
}

static LoadedExtension* loaded_find_by_path(const char* canonical_path) {
    if (!canonical_path) return NULL;
    for (LoadedExtension* e = g_loaded; e; e = e->next) {
        if (e->canonical_path && strcmp(e->canonical_path, canonical_path) == 0) {
            return e;
        }
    }
    return NULL;
}

static int ctx_register_operator(const char* name, prefix_operator_fn fn, int asmodule) {
    if (!name || name[0] == '\0' || !fn) return -1;

    char* final_name = NULL;
    if ((asmodule & PREFIX_EXTENSION_ASMODULE) != 0 && g_loading_extension_name && g_loading_extension_name[0] != '\0') {
        const char* ext_name = g_loading_extension_name;
        const char* scope_name = (g_loading_scope_name && g_loading_scope_name[0] != '\0') ? g_loading_scope_name : NULL;

        int collapse_scope = 0;
        if (scope_name && strcmp(scope_name, ext_name) == 0) {
            collapse_scope = 1;
        }

        size_t a = strlen(ext_name);
        size_t s = (scope_name && !collapse_scope) ? strlen(scope_name) : 0;
        size_t b = strlen(name);
        size_t total = a + 1 + b + 1;
        if (s > 0) total += s + 1;
        final_name = malloc(total);
        if (!final_name) return -1;

        size_t p = 0;
        if (s > 0) {
            memcpy(final_name + p, scope_name, s);
            p += s;
            final_name[p++] = '.';
        }
        memcpy(final_name + p, ext_name, a);
        p += a;
        final_name[p++] = '.';
        memcpy(final_name + p, name, b);
        p += b;
        final_name[p] = '\0';
    }

    const char* reg_name = final_name ? final_name : name;
    int rc = builtins_register_operator(reg_name, (BuiltinImplFn)fn, 0, -1, NULL, 0);
    free(final_name);
    return rc;
}

static int ctx_register_periodic_hook(int n, prefix_event_fn fn) {
    (void)n;
    (void)fn;
    return 0;
}

static int ctx_register_event_handler(const char* event_name, prefix_event_fn fn) {
    (void)event_name;
    (void)fn;
    return 0;
}

static int ctx_register_repl_handler(prefix_repl_fn repl_fn) {
    (void)repl_fn;
    return 0;
}

static DynLibHandle dyn_open_library(const char* path) {
#ifdef _WIN32
    return LoadLibraryExA(path, NULL, 0);
#else
    return dlopen(path, RTLD_NOW);
#endif
}

static const char* dyn_last_error(void) {
#ifdef _WIN32
    static char buf[256];
    DWORD code = GetLastError();
    snprintf(buf, sizeof(buf), "win32 error %lu", (unsigned long)code);
    return buf;
#else
    const char* e = dlerror();
    return e ? e : "dlopen/dlsym failed";
#endif
}

static void* dyn_find_symbol(DynLibHandle h, const char* name) {
#ifdef _WIN32
    return (void*)GetProcAddress(h, name);
#else
    return dlsym(h, name);
#endif
}

static void dyn_close_library(DynLibHandle h) {
#ifdef _WIN32
    if (h) FreeLibrary(h);
#else
    if (h) dlclose(h);
#endif
}

void extensions_set_runtime_dirs(const char* interpreter_dir, const char* cwd_dir) {
    free(g_interpreter_dir);
    g_interpreter_dir = interpreter_dir ? strdup(interpreter_dir) : NULL;
    free(g_cwd_dir);
    g_cwd_dir = cwd_dir ? strdup(cwd_dir) : NULL;
}

static int extension_register_exposure(LoadedExtension* le,
                                       const char* ext_name,
                                       const char* scope_name,
                                       char** error_out) {
    if (!le || !le->init_fn) {
        set_error(error_out, "Internal extension loader error");
        return -1;
    }

    char* key = exposure_key_for(scope_name, ext_name);
    if (!key) {
        set_error(error_out, "Out of memory");
        return -1;
    }

    if (exposure_exists(le, key)) {
        free(key);
        return 0;
    }

    prefix_ext_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.api_version = PREFIX_EXTENSION_API_VERSION;
    ctx.extension_name = ext_name;
    ctx.register_operator = ctx_register_operator;
    ctx.register_periodic_hook = ctx_register_periodic_hook;
    ctx.register_event_handler = ctx_register_event_handler;
    ctx.register_repl_handler = ctx_register_repl_handler;

    g_loading_extension_name = ext_name;
    g_loading_scope_name = (scope_name && scope_name[0] != '\0') ? scope_name : NULL;
    le->init_fn(&ctx);
    g_loading_extension_name = NULL;
    g_loading_scope_name = NULL;

    if (exposure_add(le, key) != 0) {
        free(key);
        set_error(error_out, "Out of memory");
        return -1;
    }

    free(key);
    return 0;
}

static int extensions_load_resolved(const char* resolved,
                                    const char* ext_name,
                                    const char* scope_name,
                                    char** error_out) {
    if (!resolved || !ext_name || ext_name[0] == '\0') {
        set_error(error_out, "Invalid extension load request");
        return -1;
    }

    LoadedExtension* existing = loaded_find_by_path(resolved);
    if (existing) {
        return extension_register_exposure(existing, ext_name, scope_name, error_out);
    }

    DynLibHandle handle = dyn_open_library(resolved);
    if (!handle) {
        set_errorf(error_out, "Failed to load extension library: ", dyn_last_error());
        return -1;
    }

    prefix_extension_init_fn init_fn = (prefix_extension_init_fn)dyn_find_symbol(handle, "prefix_extension_init");
    if (!init_fn) {
        set_error(error_out, "Extension missing required symbol: prefix_extension_init");
        dyn_close_library(handle);
        return -1;
    }

    LoadedExtension* le = calloc(1, sizeof(LoadedExtension));
    if (!le) {
        set_error(error_out, "Out of memory");
        dyn_close_library(handle);
        return -1;
    }

    le->canonical_path = strdup(resolved);
    le->handle = handle;
    le->init_fn = init_fn;
    le->exposures = NULL;
    le->next = g_loaded;
    g_loaded = le;

    if (!le->canonical_path) {
        set_error(error_out, "Out of memory");
        g_loaded = le->next;
        dyn_close_library(handle);
        free(le);
        return -1;
    }

    if (extension_register_exposure(le, ext_name, scope_name, error_out) != 0) {
        g_loaded = le->next;
        dyn_close_library(handle);
        free(le->canonical_path);
        free(le);
        return -1;
    }

    return 0;
}

int extensions_load_library(const char* path, const char* base_dir, char** error_out) {
    if (!path || path[0] == '\0') {
        set_error(error_out, "Empty extension path");
        return -1;
    }

    char* resolved = resolve_extension_path(path, base_dir);
    if (!resolved) {
        set_errorf(error_out, "Extension not found: ", path);
        return -1;
    }

    char* ext_name = path_basename_no_ext_dup(resolved);
    if (!ext_name) {
        set_error(error_out, "Out of memory");
        free(resolved);
        return -1;
    }

    int rc = extensions_load_resolved(resolved, ext_name, NULL, error_out);
    free(ext_name);
    free(resolved);
    return rc;
}

int extensions_load_named(const char* spec,
                          const char* base_dir,
                          const char* scope_name,
                          char** loaded_name_out,
                          char** error_out) {
    if (loaded_name_out) *loaded_name_out = NULL;

    if (!spec || spec[0] == '\0') {
        set_error(error_out, "EXTEND: empty extension specifier");
        return -1;
    }

    char* ext_name = extension_name_from_spec(spec);
    if (!ext_name || ext_name[0] == '\0') {
        free(ext_name);
        set_error(error_out, "EXTEND: invalid extension name");
        return -1;
    }

    char* resolved = resolve_extension_spec_path(spec, base_dir);
    if (!resolved) {
        set_errorf(error_out, "Extension not found: ", spec);
        free(ext_name);
        return -1;
    }

    int rc = extensions_load_resolved(resolved, ext_name, scope_name, error_out);
    if (rc == 0 && loaded_name_out) {
        *loaded_name_out = strdup(ext_name);
        if (!*loaded_name_out) {
            set_error(error_out, "Out of memory");
            rc = -1;
        }
    }

    free(resolved);
    free(ext_name);
    return rc;
}

void extensions_shutdown(void) {
    LoadedExtension* e = g_loaded;
    while (e) {
        LoadedExtension* next = e->next;

        ExtensionExposure* ex = e->exposures;
        while (ex) {
            ExtensionExposure* ex_next = ex->next;
            free(ex->key);
            free(ex);
            ex = ex_next;
        }

        dyn_close_library(e->handle);
        free(e->canonical_path);
        free(e);
        e = next;
    }
    g_loaded = NULL;

    free(g_interpreter_dir);
    g_interpreter_dir = NULL;
    free(g_cwd_dir);
    g_cwd_dir = NULL;

    g_loading_extension_name = NULL;
    g_loading_scope_name = NULL;
}
