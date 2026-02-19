#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
// platform-specific chdir
#ifdef _MSC_VER
#include <direct.h>
#define strdup _strdup
#else
#include <unistd.h>
#endif

#include "lexer.h"
#include "parser.h"
#include "interpreter.h"
#include "builtins.h"
#include "extensions.h"

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

static int is_extension_arg(const char* arg) {
    if (!arg) return 0;
    return ends_with_case_insensitive(arg, ".dll") ||
           ends_with_case_insensitive(arg, ".so") ||
           ends_with_case_insensitive(arg, ".dylib") ||
           ends_with_case_insensitive(arg, ".prex");
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
    if (!path) return strdup("program");
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
        return strdup("program");
    }
    return out;
}

static char* path_join2(const char* a, const char* b) {
    if (!a || a[0] == '\0') return b ? strdup(b) : NULL;
    if (!b || b[0] == '\0') return strdup(a);
    size_t la = strlen(a);
    size_t lb = strlen(b);
    int need_sep = (a[la - 1] != '/' && a[la - 1] != '\\');
    char* out = malloc(la + (size_t)need_sep + lb + 1);
    if (!out) return NULL;
    memcpy(out, a, la);
    size_t p = la;
    if (need_sep) out[p++] = '/';
    memcpy(out + p, b, lb);
    out[p + lb] = '\0';
    return out;
}

static int load_extension_input(const char* arg, char** err_out) {
    if (ends_with_case_insensitive(arg, ".prex")) {
        return extensions_load_prex_file(arg, err_out);
    }
    return extensions_load_library(arg, NULL, err_out);
}

static int buf_append(char** buf, size_t* len, size_t* cap, const char* s) {
    if (!buf || !len || !cap || !s) return -1;
    size_t add = strlen(s);
    if (*len + add + 1 > *cap) {
        size_t new_cap = (*cap == 0) ? 256 : *cap;
        while (*len + add + 1 > new_cap) new_cap *= 2;
        char* next = realloc(*buf, new_cap);
        if (!next) return -1;
        *buf = next;
        *cap = new_cap;
    }
    memcpy(*buf + *len, s, add);
    *len += add;
    (*buf)[*len] = '\0';
    return 0;
}

static char* read_line_dynamic(FILE* in) {
    if (!in) return NULL;
    char* line = NULL;
    size_t len = 0;
    size_t cap = 0;
    char chunk[512];

    while (fgets(chunk, sizeof(chunk), in)) {
        if (buf_append(&line, &len, &cap, chunk) != 0) {
            free(line);
            return NULL;
        }
        size_t n = strlen(chunk);
        if (n > 0 && chunk[n - 1] == '\n') break;
    }

    if (len == 0 && feof(in)) {
        free(line);
        return NULL;
    }

    if (!line) {
        line = strdup("");
    }
    return line;
}

static int is_exit_meta_command(const char* text) {
    if (!text) return 0;
    const char* p = text;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (strncmp(p, ".exit", 5) != 0) return 0;
    p += 5;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return *p == '\0';
}

static void repl_update_line_state(const char* line, int* brace_depth, int* line_continuation) {
    if (!line || !brace_depth || !line_continuation) return;

    int in_single = 0;
    int in_double = 0;
    int escaped = 0;
    size_t comment_pos = strlen(line);

    for (size_t i = 0; line[i] != '\0'; i++) {
        char c = line[i];
        if (escaped) {
            escaped = 0;
            continue;
        }
        if (in_single) {
            if (c == '\\') escaped = 1;
            else if (c == '\'') in_single = 0;
            continue;
        }
        if (in_double) {
            if (c == '\\') escaped = 1;
            else if (c == '"') in_double = 0;
            continue;
        }

        if (c == '!') {
            comment_pos = i;
            break;
        }
        if (c == '\'') {
            in_single = 1;
            continue;
        }
        if (c == '"') {
            in_double = 1;
            continue;
        }
        if (c == '{') {
            (*brace_depth)++;
        } else if (c == '}' && *brace_depth > 0) {
            (*brace_depth)--;
        }
    }

    size_t end = comment_pos;
    while (end > 0) {
        char c = line[end - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            end--;
            continue;
        }
        break;
    }
    *line_continuation = (end > 0 && line[end - 1] == '^') ? 1 : 0;
}

static int run_repl(void) {
    Interpreter interp;
    interpreter_init(&interp, "<repl>");

    char* entry = NULL;
    size_t entry_len = 0;
    size_t entry_cap = 0;
    int brace_depth = 0;
    int line_continuation = 0;
    fprintf(stdout, "\x1b[38;2;153;221;255mPrefix REPL. Enter statements, blank line to run buffer.\033[0m\n");

    for (;;) {
        int in_continuation = (brace_depth > 0) || line_continuation;
        fputs(in_continuation ? "\x1b[38;2;153;221;255m..>\033[0m " : "\x1b[38;2;153;221;255m>>>\033[0m ", stdout);
        fflush(stdout);

        char* line = read_line_dynamic(stdin);
        int eof = (line == NULL);

        if (!eof) {
            if (buf_append(&entry, &entry_len, &entry_cap, line) != 0) {
                free(line);
                free(entry);
                interpreter_destroy(&interp);
                fprintf(stderr, "Out of memory\n");
                return PREFIX_ERROR_MEMORY;
            }
            repl_update_line_state(line, &brace_depth, &line_continuation);
            free(line);
        }

        if (!eof && ((brace_depth > 0) || line_continuation)) {
            continue;
        }

        if (entry_len == 0) {
            if (eof) break;
            continue;
        }

        if (is_exit_meta_command(entry)) {
            break;
        }

        Lexer lex;
        lexer_init(&lex, entry, "<repl>");

        Parser parser;
        parser_init(&parser, &lex);
        Stmt* program = parser_parse(&parser);

        if (!parser.had_error) {
            ExecResult res = exec_program_in_env(&interp, program, interp.global_env);
            if (res.status == EXEC_ERROR) {
                fprintf(stderr, "Runtime error: %s at %d:%d\n",
                        res.error ? res.error : "error",
                        res.error_line,
                        res.error_column);
            }
        }

        entry_len = 0;
        if (entry) entry[0] = '\0';
        brace_depth = 0;
        line_continuation = 0;

        if (eof) break;
    }

    free(entry);
    interpreter_destroy(&interp);
    return PREFIX_SUCCESS;
}

int main(int argc, char** argv) {
    const char* path = NULL;
    int verbose_flag = 0;
    int explicit_ext_count = 0;

    builtins_reset_dynamic();
    builtins_set_argv(argc, argv);

    char cwd_buf[4096];
    const char* cwd = NULL;
#ifdef _MSC_VER
    if (_getcwd(cwd_buf, (int)sizeof(cwd_buf))) cwd = cwd_buf;
#else
    if (getcwd(cwd_buf, sizeof(cwd_buf))) cwd = cwd_buf;
#endif
    char* exe_dir = path_dirname_dup((argc > 0) ? argv[0] : NULL);
    extensions_set_runtime_dirs(exe_dir ? exe_dir : ".", cwd ? cwd : ".");
    free(exe_dir);

    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];

        if (strcmp(arg, "-verbose") == 0) {
            verbose_flag = 1;
            continue;
        }

        if (is_extension_arg(arg)) {
            char* err = NULL;
            if (load_extension_input(arg, &err) != 0) {
                fprintf(stderr, "%s\n", err ? err : "Failed to load extension");
                free(err);
                extensions_shutdown();
                builtins_reset_dynamic();
                return PREFIX_ERROR_IO;
            }
            free(err);
            explicit_ext_count++;
            continue;
        }

        if (!path) {
            path = arg;
            continue;
        }

        fprintf(stderr, "Unexpected argument '%s'\n", arg);
        extensions_shutdown();
        builtins_reset_dynamic();
        return PREFIX_ERROR_IO;
    }

    if (explicit_ext_count == 0) {
        char* err = NULL;
        int loaded_any = 0;

        if (extensions_load_prex_if_exists(".prex", &loaded_any, &err) != 0) {
            fprintf(stderr, "%s\n", err ? err : "Failed to load .prex");
            free(err);
            extensions_shutdown();
            builtins_reset_dynamic();
            return PREFIX_ERROR_IO;
        }
        free(err);

        if (path) {
            char* prog_dir = path_dirname_dup(path);
            char* base = path_basename_no_ext_dup(path);
            if (!prog_dir || !base) {
                free(prog_dir);
                free(base);
                extensions_shutdown();
                builtins_reset_dynamic();
                fprintf(stderr, "Out of memory\n");
                return PREFIX_ERROR_MEMORY;
            }

            size_t base_len = strlen(base);
            char* base_prex_name = malloc(base_len + strlen(".prex") + 1);
            if (!base_prex_name) {
                free(prog_dir);
                free(base);
                extensions_shutdown();
                builtins_reset_dynamic();
                fprintf(stderr, "Out of memory\n");
                return PREFIX_ERROR_MEMORY;
            }
            memcpy(base_prex_name, base, base_len);
            memcpy(base_prex_name + base_len, ".prex", strlen(".prex") + 1);

            char* prex_in_prog_dir = path_join2(prog_dir, ".prex");
            char* base_prex = path_join2(prog_dir, base_prex_name);

            if (prex_in_prog_dir) {
                err = NULL;
                loaded_any = 0;
                if (extensions_load_prex_if_exists(prex_in_prog_dir, &loaded_any, &err) != 0) {
                    fprintf(stderr, "%s\n", err ? err : "Failed to load program directory .prex");
                    free(err);
                    free(prog_dir);
                    free(base);
                    free(prex_in_prog_dir);
                    free(base_prex_name);
                    free(base_prex);
                    extensions_shutdown();
                    builtins_reset_dynamic();
                    return PREFIX_ERROR_IO;
                }
                free(err);
            }

            if (base_prex) {
                err = NULL;
                loaded_any = 0;
                if (extensions_load_prex_if_exists(base_prex, &loaded_any, &err) != 0) {
                    fprintf(stderr, "%s\n", err ? err : "Failed to load program basename .prex");
                    free(err);
                    free(prog_dir);
                    free(base);
                    free(prex_in_prog_dir);
                    free(base_prex_name);
                    free(base_prex);
                    extensions_shutdown();
                    builtins_reset_dynamic();
                    return PREFIX_ERROR_IO;
                }
                free(err);
            }

            free(prog_dir);
            free(base);
            free(prex_in_prog_dir);
            free(base_prex_name);
            free(base_prex);
        }
    }

    if (!path) {
        int repl_rc = run_repl();
        extensions_shutdown();
        builtins_reset_dynamic();
        return repl_rc;
    }

    (void)verbose_flag;

    char* src = NULL;
    char* source_label = NULL;
    /* Canonicalize the provided program path now so it's correct even if
       the process changes cwd below. This prevents relative paths like
       "./tests/test2.pre" from resolving incorrectly after chdir. */
#if defined(_MSC_VER)
    if (path) source_label = _fullpath(NULL, path, 0);
#else
    if (path) source_label = realpath(path, NULL);
#endif
    if (!source_label && path) source_label = strdup(path);

    FILE* f = fopen(path, "rb");
        if (!f) {
            fprintf(stderr, "Failed to open '%s'\n", path);
            extensions_shutdown();
            builtins_reset_dynamic();
            return PREFIX_ERROR_IO;
        }
        if (fseek(f, 0, SEEK_END) != 0) {
            fclose(f);
            fprintf(stderr, "Failed to read '%s'\n", path);
            extensions_shutdown();
            builtins_reset_dynamic();
            return PREFIX_ERROR_IO;
        }
        long sz = ftell(f);
        if (sz < 0) sz = 0;
        rewind(f);

        src = malloc((size_t)sz + 1);
        if (!src) {
            fclose(f);
            fprintf(stderr, "Out of memory\n");
            extensions_shutdown();
            builtins_reset_dynamic();
            return PREFIX_ERROR_MEMORY;
        }
        size_t r = fread(src, 1, (size_t)sz, f);
        src[r] = '\0';
        fclose(f);

    Lexer lex;
    lexer_init(&lex, src, source_label);

    Parser parser;
    parser_init(&parser, &lex);

    Stmt* program = parser_parse(&parser);
    if (parser.had_error) {
        free(src);
        extensions_shutdown();
        builtins_reset_dynamic();
        return PREFIX_ERROR_SYNTAX;
    }

    // Change working directory to the directory containing the script
    // so relative READFILE/WRITEFILE operate relative to the script.
    if (path) {
        char* dir = strdup(path);
        char* last_slash = NULL;
        for (char* p = dir; *p; p++) if (*p == '/' || *p == '\\') last_slash = p;
        if (last_slash) {
            *last_slash = '\0';
#ifdef _MSC_VER
            _chdir(dir);
#else
            chdir(dir);
#endif
        }
        free(dir);
    }

    ExecResult res = exec_program(program, source_label);
    if (res.status == EXEC_ERROR) {
        fprintf(stderr, "Runtime error: %s at %d:%d\n", res.error ? res.error : "error", res.error_line, res.error_column);
        free(src);
        extensions_shutdown();
        builtins_reset_dynamic();
        return PREFIX_ERROR_RUNTIME;
    }

    free(src);
    if (source_label) free(source_label);
    extensions_shutdown();
    builtins_reset_dynamic();
    return PREFIX_SUCCESS;
}
