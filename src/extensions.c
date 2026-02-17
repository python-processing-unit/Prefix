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

typedef struct LoadedExtension {
    char* canonical_path;
    char* name;
    DynLibHandle handle;
    struct LoadedExtension* next;
} LoadedExtension;

static LoadedExtension* g_loaded = NULL;
static char* g_interpreter_dir = NULL;
static char* g_cwd_dir = NULL;
static const char* g_loading_extension_name = NULL;

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

static char* path_dirname_dup(const char* path) {
    if (!path || path[0] == '\0') return strdup(".");
    const char* last = NULL;
    for (const char* p = path; *p; p++) {
        if (*p == '/' || *p == '\\') last = p;
    }
    if (!last) return strdup(".");
    size_t n = (size_t)(last - path);
    if (n == 0) n = 1;
    char* out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, path, n);
    out[n] = '\0';
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
        char* ext_dir = path_join2(g_interpreter_dir, "ext");
        char* p = path_join2(ext_dir, input);
        free(ext_dir);
        if (p && file_exists_regular(p)) {
            char* c = canonicalize_existing_path(p);
            free(p);
            return c;
        }
        free(p);
    }

    /* Also check the interpreter's lib/ directory for extensions. This
       allows pointer files that list a bare filename (e.g. "image.dll") to
       resolve to lib/<name>/<file> or lib/<file>. */
    if (g_interpreter_dir && g_interpreter_dir[0] != '\0') {
        char* lib_dir = path_join2(g_interpreter_dir, "lib");
        if (lib_dir) {
            char* p1 = path_join2(lib_dir, input);
            if (p1 && file_exists_regular(p1)) {
                char* c = canonicalize_existing_path(p1);
                free(p1);
                free(lib_dir);
                return c;
            }
            free(p1);

            /* Try lib/<basename_no_ext>/<input> e.g. lib/image/image.dll */
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

static int ctx_register_operator(const char* name, prefix_operator_fn fn, int asmodule) {
    if (!name || name[0] == '\0' || !fn) return -1;

    char* final_name = NULL;
    if ((asmodule & PREFIX_EXTENSION_ASMODULE) != 0 && g_loading_extension_name && g_loading_extension_name[0] != '\0') {
        size_t a = strlen(g_loading_extension_name);
        size_t b = strlen(name);
        final_name = malloc(a + 1 + b + 1);
        if (!final_name) return -1;
        memcpy(final_name, g_loading_extension_name, a);
        final_name[a] = '.';
        memcpy(final_name + a + 1, name, b);
        final_name[a + 1 + b] = '\0';
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

    for (LoadedExtension* e = g_loaded; e; e = e->next) {
        if (strcmp(e->canonical_path, resolved) == 0) {
            free(resolved);
            return 0;
        }
    }

    DynLibHandle handle = dyn_open_library(resolved);
    if (!handle) {
        set_errorf(error_out, "Failed to load extension library: ", dyn_last_error());
        free(resolved);
        return -1;
    }

    prefix_extension_init_fn init_fn = (prefix_extension_init_fn)dyn_find_symbol(handle, "prefix_extension_init");
    if (!init_fn) {
        set_error(error_out, "Extension missing required symbol: prefix_extension_init");
        dyn_close_library(handle);
        free(resolved);
        return -1;
    }

    char* ext_name = path_basename_no_ext_dup(resolved);
    if (!ext_name) {
        set_error(error_out, "Out of memory");
        dyn_close_library(handle);
        free(resolved);
        return -1;
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
    init_fn(&ctx);
    g_loading_extension_name = NULL;

    LoadedExtension* le = calloc(1, sizeof(LoadedExtension));
    if (!le) {
        set_error(error_out, "Out of memory");
        dyn_close_library(handle);
        free(ext_name);
        free(resolved);
        return -1;
    }

    le->canonical_path = resolved;
    le->name = ext_name;
    le->handle = handle;
    le->next = g_loaded;
    g_loaded = le;

    return 0;
}

static char* trim_in_place(char* s) {
    if (!s) return s;
    while (*s && isspace((unsigned char)*s)) s++;
    char* end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) {
        end--;
        *end = '\0';
    }
    return s;
}

int extensions_load_prex_file(const char* prex_path, char** error_out) {
    if (!prex_path || prex_path[0] == '\0') {
        set_error(error_out, "Empty .prex path");
        return -1;
    }

    FILE* f = fopen(prex_path, "rb");
    if (!f) {
        set_errorf(error_out, "Failed to open .prex file: ", prex_path);
        return -1;
    }

    char* base_dir = path_dirname_dup(prex_path);
    if (!base_dir) base_dir = strdup(".");

    char line[4096];
    int line_no = 0;
    while (fgets(line, sizeof(line), f)) {
        line_no++;
        char* t = trim_in_place(line);
        if (t[0] == '\0' || t[0] == '!') continue;

        char* err = NULL;
        int load_result = -1;
        
        size_t tl = strlen(t);
        if (tl >= 5 && (strcmp(t + tl - 5, ".prex") == 0)) {
            char* resolved_prex = resolve_extension_path(t, base_dir);
            if (resolved_prex) {
                load_result = extensions_load_prex_file(resolved_prex, &err);
                free(resolved_prex);
            } else {
                set_errorf(&err, "Extension not found: ", t);
                load_result = -1;
            }
        } else {
            load_result = extensions_load_library(t, base_dir, &err);
        }
        
        if (load_result != 0) {
            if (err) {
                size_t n = strlen(err) + 64;
                char* out = malloc(n);
                if (out) {
                    snprintf(out, n, "%s (from %s:%d)", err, prex_path, line_no);
                    set_error(error_out, out);
                    free(out);
                } else {
                    set_error(error_out, err);
                }
                free(err);
            }
            free(base_dir);
            fclose(f);
            return -1;
        }
    }

    free(base_dir);
    fclose(f);
    return 0;
}

int extensions_load_prex_if_exists(const char* prex_path, int* loaded_any, char** error_out) {
    if (loaded_any) *loaded_any = 0;
    if (!prex_path || prex_path[0] == '\0') return 0;
    if (!file_exists_regular(prex_path)) return 0;
    if (loaded_any) *loaded_any = 1;
    return extensions_load_prex_file(prex_path, error_out);
}

void extensions_shutdown(void) {
    LoadedExtension* e = g_loaded;
    while (e) {
        LoadedExtension* next = e->next;
        dyn_close_library(e->handle);
        free(e->canonical_path);
        free(e->name);
        free(e);
        e = next;
    }
    g_loaded = NULL;

    free(g_interpreter_dir);
    g_interpreter_dir = NULL;
    free(g_cwd_dir);
    g_cwd_dir = NULL;

    g_loading_extension_name = NULL;
}
