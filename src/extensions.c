#include "extensions.h"
#include "builtins.h"
#include "prefix_extension.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
typedef HMODULE DynLibHandle;
#else
#include <dlfcn.h>
typedef void *DynLibHandle;
#endif

typedef struct ExtensionExposure {
    char *key;
    struct ExtensionExposure *next;
} ExtensionExposure;

typedef struct RegisteredOperator {
    char *name;
    prefix_operator_fn fn;
    int flags;
    struct RegisteredOperator *next;
} RegisteredOperator;

typedef struct LoadedExtension {
    char *canonical_path;
    DynLibHandle handle;
    prefix_extension_init_fn init_fn;
    RegisteredOperator *ops;
    ExtensionExposure *exposures;
    struct LoadedExtension *next;
} LoadedExtension;

static LoadedExtension *g_loaded = NULL;
static char *g_interpreter_dir = NULL;
static char *g_cwd_dir = NULL;
static const char *g_loading_extension_name = NULL;
static LoadedExtension *g_current_loading_extension = NULL;

// Registered event handlers and periodic hooks
typedef struct EventHandler {
    char *event_name;
    prefix_event_fn fn;
    char *owner;
    struct EventHandler *next;
} EventHandler;

typedef struct PeriodicHook {
    int n;
    prefix_event_fn fn;
    char *owner;
    struct PeriodicHook *next;
} PeriodicHook;

static EventHandler *g_event_handlers = NULL;
static PeriodicHook *g_periodic_hooks = NULL;
static prefix_repl_fn g_repl_handler = NULL;

static void set_error(char **error_out, const char *msg) {
    if (!error_out) {
        return;
    }
    free(*error_out);
    *error_out = msg ? strdup(msg) : NULL;
}

static void set_errorf(char **error_out, const char *prefix, const char *value) {
    if (!error_out) {
        return;
    }
    size_t a = prefix ? strlen(prefix) : 0;
    size_t b = value ? strlen(value) : 0;
    char *out = malloc(a + b + 1);
    if (!out) {
        return;
    }
    if (prefix) {
        memcpy(out, prefix, a);
    }
    if (value) {
        memcpy(out + a, value, b);
    }
    out[a + b] = '\0';
    free(*error_out);
    *error_out = out;
}

static int path_is_absolute(const char *path) {
    if (!path || path[0] == '\0') {
        return 0;
    }
#ifdef _WIN32
    if ((isalpha((unsigned char)path[0]) && path[1] == ':') || (path[0] == '\\' && path[1] == '\\') || path[0] == '/' ||
        path[0] == '\\') {
        return 1;
    }
#else
    if (path[0] == '/')
        return 1;
#endif
    return 0;
}

static int file_exists_regular(const char *path) {
    struct stat st;
    if (!path) {
        return 0;
    }
    if (stat(path, &st) != 0) {
        return 0;
    }
    return (st.st_mode & S_IFMT) == S_IFREG;
}

static const char *platform_dynlib_suffix(void) {
#ifdef _WIN32
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}

static int has_dynlib_suffix(const char *path) {
    if (!path) {
        return 0;
    }
    size_t plen = strlen(path);
    return (plen >= 4 && prefix_stricmp(path + plen - 4, ".dll") == 0) ||
           (plen >= 3 && prefix_stricmp(path + plen - 3, ".so") == 0) ||
           (plen >= 6 && prefix_stricmp(path + plen - 6, ".dylib") == 0);
}

static char *path_join2(const char *a, const char *b) {
    if (!a || a[0] == '\0') {
        return b ? strdup(b) : NULL;
    }
    if (!b || b[0] == '\0') {
        return strdup(a);
    }

    size_t la = strlen(a);
    size_t lb = strlen(b);
    int need_sep = (la > 0 && a[la - 1] != '/' && a[la - 1] != '\\');

    char *out = malloc(la + (size_t)need_sep + lb + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, a, la);
    size_t p = la;
    if (need_sep) {
        out[p++] = '/';
    }
    memcpy(out + p, b, lb);
    out[p + lb] = '\0';
    return out;
}

static char *path_basename_no_ext_dup(const char *path) {
    if (!path) {
        return strdup("extension");
    }
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    char *out = strdup(base);
    if (!out) {
        return NULL;
    }
    char *dot = strrchr(out, '.');
    if (dot) {
        *dot = '\0';
    }
    if (out[0] == '\0') {
        free(out);
        return strdup("extension");
    }
    return out;
}

static char *resolve_extension_path(const char *input, const char *base_dir) {
    if (!input || input[0] == '\0') {
        return NULL;
    }

    if (path_is_absolute(input) && file_exists_regular(input)) {
        return prefix_fullpath_dup(input);
    }

    if (file_exists_regular(input)) {
        return prefix_fullpath_dup(input);
    }

    if (base_dir && base_dir[0] != '\0') {
        char *p = path_join2(base_dir, input);
        if (p && file_exists_regular(p)) {
            char *c = prefix_fullpath_dup(p);
            free(p);
            return c;
        }
        free(p);
    }

    if (g_cwd_dir && g_cwd_dir[0] != '\0') {
        char *p = path_join2(g_cwd_dir, input);
        if (p && file_exists_regular(p)) {
            char *c = prefix_fullpath_dup(p);
            free(p);
            return c;
        }
        free(p);
    }

    if (g_interpreter_dir && g_interpreter_dir[0] != '\0') {
        const char *ext_roots[] = {"ext/std", "ext/usr"};
        for (size_t i = 0; i < sizeof(ext_roots) / sizeof(ext_roots[0]); i++) {
            char *ext_dir = path_join2(g_interpreter_dir, ext_roots[i]);
            if (!ext_dir) {
                continue;
            }
            char *p = path_join2(ext_dir, input);
            free(ext_dir);
            if (p && file_exists_regular(p)) {
                char *c = prefix_fullpath_dup(p);
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
        const char *lib_roots[] = {"lib/std", "lib/usr"};
        for (size_t i = 0; i < sizeof(lib_roots) / sizeof(lib_roots[0]); i++) {
            char *lib_dir = path_join2(g_interpreter_dir, lib_roots[i]);
            if (!lib_dir) {
                continue;
            }

            char *p1 = path_join2(lib_dir, input);
            if (p1 && file_exists_regular(p1)) {
                char *c = prefix_fullpath_dup(p1);
                free(p1);
                free(lib_dir);
                return c;
            }
            free(p1);

            char *base = path_basename_no_ext_dup(input);
            char *subdir = path_join2(lib_dir, base);
            char *p2 = path_join2(subdir, input);
            if (p2 && file_exists_regular(p2)) {
                char *c = prefix_fullpath_dup(p2);
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

static char *normalize_extension_spec_path(const char *spec) {
    if (!spec) {
        return NULL;
    }
    size_t n = strlen(spec);
    char *out = malloc(n + 2);
    if (!out) {
        return NULL;
    }

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

static char *extension_name_from_spec(const char *spec) {
    if (!spec || spec[0] == '\0') {
        return strdup("extension");
    }

    char *norm = normalize_extension_spec_path(spec);
    if (!norm) {
        return NULL;
    }

    if (has_dynlib_suffix(norm)) {
        char *dot = strrchr(norm, '.');
        if (dot) {
            *dot = '\0';
        }
    }

    size_t len = strlen(norm);
    if (len >= 5 && strcmp(norm + len - 5, "/init") == 0) {
        norm[len - 5] = '\0';
    }

    char *name = path_basename_no_ext_dup(norm);
    free(norm);
    return name;
}

static char *resolve_extension_spec_path(const char *spec, const char *base_dir) {
    if (!spec || spec[0] == '\0') {
        return NULL;
    }

    if (has_dynlib_suffix(spec)) {
        return resolve_extension_path(spec, base_dir);
    }

    char *norm = normalize_extension_spec_path(spec);
    if (!norm) {
        return NULL;
    }

    const char *suffix = platform_dynlib_suffix();
    size_t len = strlen(norm);

    size_t n1 = len + strlen(suffix) + 1;
    char *c1 = malloc(n1);
    if (!c1) {
        free(norm);
        return NULL;
    }
    snprintf(c1, n1, "%s%s", norm, suffix);
    char *resolved = resolve_extension_path(c1, base_dir);
    free(c1);
    if (resolved) {
        free(norm);
        return resolved;
    }

    size_t n2 = len + strlen("/init") + strlen(suffix) + 1;
    char *c2 = malloc(n2);
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

static char *exposure_key_for(const char *ext_name) {
    const char *ext = (ext_name && ext_name[0] != '\0') ? ext_name : "extension";
    return strdup(ext);
}

static int exposure_exists(LoadedExtension *le, const char *key) {
    if (!le || !key) {
        return 0;
    }
    for (ExtensionExposure *e = le->exposures; e; e = e->next) {
        if (e->key && strcmp(e->key, key) == 0) {
            return 1;
        }
    }
    return 0;
}

static int exposure_add(LoadedExtension *le, const char *key) {
    if (!le || !key) {
        return -1;
    }
    ExtensionExposure *ex = calloc(1, sizeof(ExtensionExposure));
    if (!ex) {
        return -1;
    }
    ex->key = strdup(key);
    if (!ex->key) {
        free(ex);
        return -1;
    }
    ex->next = le->exposures;
    le->exposures = ex;
    return 0;
}

static LoadedExtension *loaded_find_by_path(const char *canonical_path) {
    if (!canonical_path) {
        return NULL;
    }
    for (LoadedExtension *e = g_loaded; e; e = e->next) {
        if (e->canonical_path && strcmp(e->canonical_path, canonical_path) == 0) {
            return e;
        }
    }
    return NULL;
}

static int ctx_register_operator(const char *name, prefix_operator_fn fn, int flags) {
    if (!name || name[0] == '\0' || !fn) {
        return -1;
    }

    /* Record the operator for the currently-loading extension so we can
       expose aliases later when the same extension is imported into other
       module scopes without reinitializing the library. */
    if (g_current_loading_extension) {
        RegisteredOperator *ro = calloc(1, sizeof(RegisteredOperator));
        if (ro) {
            ro->name = strdup(name);
            if (ro->name) {
                ro->fn = fn;
                ro->flags = flags;
                ro->next = g_current_loading_extension->ops;
                g_current_loading_extension->ops = ro;
            } else {
                free(ro);
            }
        }
    }

    char *final_name = NULL;
    if ((flags & PREFIX_EXTENSION_MODULE_RESTRICTED) != 0 && g_loading_extension_name &&
        g_loading_extension_name[0] != '\0') {
        const char *ext_name = g_loading_extension_name;

        size_t a = strlen(ext_name);
        size_t b = strlen(name);
        size_t total = a + 1 + b + 1;
        final_name = malloc(total);
        if (!final_name) {
            return -1;
        }

        size_t p = 0;
        memcpy(final_name + p, ext_name, a);
        p += a;
        final_name[p++] = '.';
        memcpy(final_name + p, name, b);
        p += b;
        final_name[p] = '\0';
    }

    const char *reg_name = final_name ? final_name : name;
    int rc = builtins_register_operator(reg_name, (BuiltinImplFn)fn, 0, -1, NULL, 0);
    free(final_name);
    return rc;
}

static int ctx_register_periodic_hook(int n, prefix_event_fn fn) {
    if (n <= 0 || !fn) {
        return -1;
    }
    PeriodicHook *h = calloc(1, sizeof(PeriodicHook));
    if (!h) {
        return -1;
    }
    h->n = n;
    h->fn = fn;
    h->owner = g_loading_extension_name ? strdup(g_loading_extension_name) : strdup("extension");
    if (!h->owner) {
        free(h);
        return -1;
    }
    h->next = g_periodic_hooks;
    g_periodic_hooks = h;
    return 0;
}

static int ctx_register_event_handler(const char *event_name, prefix_event_fn fn) {
    if (!event_name || !fn) {
        return -1;
    }
    EventHandler *h = calloc(1, sizeof(EventHandler));
    if (!h) {
        return -1;
    }
    h->event_name = strdup(event_name);
    if (!h->event_name) {
        free(h);
        return -1;
    }
    h->fn = fn;
    h->owner = g_loading_extension_name ? strdup(g_loading_extension_name) : strdup("extension");
    if (!h->owner) {
        free(h->event_name);
        free(h);
        return -1;
    }
    h->next = g_event_handlers;
    g_event_handlers = h;
    return 0;
}

static int ctx_register_repl_handler(prefix_repl_fn repl_fn) {
    if (!repl_fn) {
        return -1;
    }
    g_repl_handler = repl_fn;
    return 0;
}

static DynLibHandle dyn_open_library(const char *path) {
#ifdef _WIN32
    return LoadLibraryExA(path, NULL, 0);
#else
    return dlopen(path, RTLD_NOW);
#endif
}

static const char *dyn_last_error(void) {
#ifdef _WIN32
    static char buf[256];
    DWORD code = GetLastError();
    snprintf(buf, sizeof(buf), "win32 error %lu", (unsigned long)code);
    return buf;
#else
    const char *e = dlerror();
    return e ? e : "dlopen/dlsym failed";
#endif
}

static void *dyn_find_symbol(DynLibHandle h, const char *name) {
#ifdef _WIN32
    return (void *)GetProcAddress(h, name);
#else
    return dlsym(h, name);
#endif
}

static void dyn_close_library(DynLibHandle h) {
#ifdef _WIN32
    if (h) {
        FreeLibrary(h);
    }
#else
    if (h)
        dlclose(h);
#endif
}

void extensions_set_runtime_dirs(const char *interpreter_dir, const char *cwd_dir) {
    free(g_interpreter_dir);
    g_interpreter_dir = interpreter_dir ? strdup(interpreter_dir) : NULL;
    free(g_cwd_dir);
    g_cwd_dir = cwd_dir ? strdup(cwd_dir) : NULL;
}

int extensions_fire_event(Interpreter *interp, const char *event_name) {
    if (!event_name) {
        return 0;
    }
    int invoked = 0;
    for (EventHandler *h = g_event_handlers; h; h = h->next) {
        if (h->event_name && strcmp(h->event_name, event_name) == 0) {
            if (h->fn) {
                h->fn(interp, event_name);
            }
            invoked++;
        }
    }
    return invoked;
}

void extensions_run_periodic_hooks(Interpreter *interp) {
    if (!interp) {
        return;
    }
    if (!g_periodic_hooks) {
        return;
    }
    int step_index = interp->trace_next_step_index - 1;
    if (step_index < 0) {
        return;
    }
    for (PeriodicHook *p = g_periodic_hooks; p; p = p->next) {
        if (p->n > 0 && (step_index % p->n) == 0) {
            if (p->fn) {
                p->fn(interp, "periodic");
            }
        }
    }
}

int extensions_call_repl_handler(void) {
    if (!g_repl_handler) {
        return -1;
    }
    return g_repl_handler();
}

static int extension_register_exposure(LoadedExtension *le, const char *ext_name, const char *scope_name,
                                       char **error_out) {
    if (!le || !le->init_fn) {
        set_error(error_out, "Internal extension loader error");
        return -1;
    }

    char *base_key = exposure_key_for(ext_name);
    if (!base_key) {
        set_error(error_out, "Out of memory");
        return -1;
    }

    char *scope_key = NULL;
    if (scope_name && scope_name[0] != '\0') {
        size_t s = strlen(scope_name);
        size_t e = strlen(ext_name);
        /* use ':' as an internal separator to form a unique exposure key */
        scope_key = malloc(s + 1 + e + 1);
        if (!scope_key) {
            free(base_key);
            set_error(error_out, "Out of memory");
            return -1;
        }
        memcpy(scope_key, scope_name, s);
        scope_key[s] = ':';
        memcpy(scope_key + s + 1, ext_name, e);
        scope_key[s + 1 + e] = '\0';
    }

    int base_exists = exposure_exists(le, base_key);
    int scope_exists = scope_key ? exposure_exists(le, scope_key) : 0;
    if (base_exists && (!scope_key || scope_exists)) {
        free(base_key);
        free(scope_key);
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

    /* If this is the first time the extension is exposed (base), run its
       init function so it can register operators. Otherwise, the operators
       should already be recorded in le->ops and we only need to create
       scope-qualified aliases. */
    if (!base_exists) {
        g_current_loading_extension = le;
        g_loading_extension_name = ext_name;
        le->init_fn(&ctx);
        g_current_loading_extension = NULL;
        g_loading_extension_name = NULL;

        if (exposure_add(le, base_key) != 0) {
            free(base_key);
            free(scope_key);
            set_error(error_out, "Out of memory");
            return -1;
        }
    }

    /* If a scope was provided and we haven't yet exposed the extension under
       that scope, create aliases for module-restricted operators. */
    if (scope_key && !scope_exists) {
        for (RegisteredOperator *ro = le->ops; ro; ro = ro->next) {
            if ((ro->flags & PREFIX_EXTENSION_MODULE_RESTRICTED) != 0) {
                size_t s = strlen(scope_name);
                size_t e = strlen(ext_name);
                size_t n = strlen(ro->name);
                size_t total = s + 1 + e + 1 + n + 1;
                char *alias = malloc(total);
                if (!alias) {
                    continue;
                }
                size_t p = 0;
                memcpy(alias + p, scope_name, s);
                p += s;
                alias[p++] = '.';
                memcpy(alias + p, ext_name, e);
                p += e;
                alias[p++] = '.';
                memcpy(alias + p, ro->name, n);
                p += n;
                alias[p] = '\0';
                /* ignore registration errors for aliases */
                builtins_register_operator(alias, (BuiltinImplFn)ro->fn, 0, -1, NULL, 0);
                free(alias);
            }
        }

        if (exposure_add(le, scope_key) != 0) {
            free(base_key);
            free(scope_key);
            set_error(error_out, "Out of memory");
            return -1;
        }
    }

    free(base_key);
    free(scope_key);
    return 0;
}

static int extensions_load_resolved(const char *resolved, const char *ext_name, const char *scope_name,
                                    char **error_out) {
    if (!resolved || !ext_name || ext_name[0] == '\0') {
        set_error(error_out, "Invalid extension load request");
        return -1;
    }

    LoadedExtension *existing = loaded_find_by_path(resolved);
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

    LoadedExtension *le = calloc(1, sizeof(LoadedExtension));
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

int extensions_load_library(const char *path, const char *base_dir, char **error_out) {
    if (!path || path[0] == '\0') {
        set_error(error_out, "Empty extension path");
        return -1;
    }

    char *resolved = resolve_extension_path(path, base_dir);
    if (!resolved) {
        set_errorf(error_out, "Extension not found: ", path);
        return -1;
    }

    char *ext_name = path_basename_no_ext_dup(resolved);
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

int extensions_load_named(const char *spec, const char *base_dir, const char *scope_name, char **loaded_name_out,
                          char **error_out) {
    if (loaded_name_out) {
        *loaded_name_out = NULL;
    }

    if (!spec || spec[0] == '\0') {
        set_error(error_out, "EXTEND: empty extension specifier");
        return -1;
    }

    if (has_dynlib_suffix(spec)) {
        set_errorf(error_out, "EXTEND: explicit extension suffix rejected: ", spec);
        return -1;
    }

    char *ext_name = extension_name_from_spec(spec);
    if (!ext_name || ext_name[0] == '\0') {
        free(ext_name);
        set_error(error_out, "EXTEND: invalid extension name");
        return -1;
    }

    char *resolved = resolve_extension_spec_path(spec, base_dir);
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

int extensions_expose_named(const char *ext_name, const char *scope_name, char **error_out) {
    if (!ext_name || ext_name[0] == '\0') {
        set_error(error_out, "extensions_expose_named: empty extension name");
        return -1;
    }

    for (LoadedExtension *le = g_loaded; le; le = le->next) {
        if (exposure_exists(le, ext_name)) {
            return extension_register_exposure(le, ext_name, scope_name, error_out);
        }
    }

    set_errorf(error_out, "Extension not loaded: ", ext_name);
    return -1;
}

void extensions_shutdown(void) {
    LoadedExtension *e = g_loaded;
    while (e) {
        LoadedExtension *next = e->next;

        ExtensionExposure *ex = e->exposures;
        while (ex) {
            ExtensionExposure *ex_next = ex->next;
            free(ex->key);
            free(ex);
            ex = ex_next;
        }

        RegisteredOperator *ro = e->ops;
        while (ro) {
            RegisteredOperator *rn = ro->next;
            free(ro->name);
            free(ro);
            ro = rn;
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

    // Free registered event handlers
    EventHandler *eh = g_event_handlers;
    while (eh) {
        EventHandler *en = eh->next;
        free(eh->event_name);
        free(eh->owner);
        free(eh);
        eh = en;
    }
    g_event_handlers = NULL;

    // Free periodic hooks
    PeriodicHook *ph = g_periodic_hooks;
    while (ph) {
        PeriodicHook *pn = ph->next;
        free(ph->owner);
        free(ph);
        ph = pn;
    }
    g_periodic_hooks = NULL;

    // Repl handler reset
    g_repl_handler = NULL;
}
