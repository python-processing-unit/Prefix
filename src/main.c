#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// platform-specific chdir
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

#include "builtins.h"
#include "extensions.h"
#include "interpreter.h"
#include "lexer.h"
#include "parser.h"

static int is_extension_arg(const char *arg) {
    if (!arg) {
        return 0;
    }
    size_t alen = strlen(arg);
    return (alen >= 4 && prefix_stricmp(arg + alen - 4, ".dll") == 0) ||
           (alen >= 3 && prefix_stricmp(arg + alen - 3, ".so") == 0) ||
           (alen >= 6 && prefix_stricmp(arg + alen - 6, ".dylib") == 0);
}

static char *path_dirname_dup(const char *path) {
    if (!path || path[0] == '\0') {
        return strdup(".");
    }
    const char *last = NULL;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            last = p;
        }
    }
    if (!last) {
        return strdup(".");
    }
    size_t n = (size_t)(last - path);
    if (n == 0) {
        n = 1;
    }
    char *out = malloc(n + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, path, n);
    out[n] = '\0';
    return out;
}

static int buf_append(char **buf, size_t *len, size_t *cap, const char *s) {
    if (!buf || !len || !cap || !s) {
        return -1;
    }
    size_t add = strlen(s);
    if (*len + add + 1 > *cap) {
        size_t new_cap = (*cap == 0) ? 256 : *cap;
        while (*len + add + 1 > new_cap) {
            new_cap *= 2;
        }
        char *next = realloc(*buf, new_cap);
        if (!next) {
            return -1;
        }
        *buf = next;
        *cap = new_cap;
    }
    memcpy(*buf + *len, s, add);
    *len += add;
    (*buf)[*len] = '\0';
    return 0;
}

static int is_exit_meta_command(const char *text) {
    if (!text) {
        return 0;
    }
    const char *p = text;
    p += strspn(p, " \t\r\n");
    if (strncmp(p, ".exit", 5) != 0) {
        return 0;
    }
    p += 5;
    p += strspn(p, " \t\r\n");
    return *p == '\0';
}

static void repl_update_line_state(const char *line, int *brace_depth, int *line_continuation) {
    if (!line || !brace_depth || !line_continuation) {
        return;
    }

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
            if (c == '\\') {
                escaped = 1;
            } else if (c == '\'') {
                in_single = 0;
            }
            continue;
        }
        if (in_double) {
            if (c == '\\') {
                escaped = 1;
            } else if (c == '"') {
                in_double = 0;
            }
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

static int run_repl(int verbose, int private_flag) {
    Interpreter interp;
    interpreter_init(&interp, "<repl>", verbose != 0, private_flag != 0);

    char *entry = NULL;
    size_t entry_len = 0;
    size_t entry_cap = 0;
    int brace_depth = 0;
    int line_continuation = 0;
    fprintf(stdout, "\x1b[38;2;153;221;255mPrefix REPL. Enter statements, blank line to run buffer.\033[0m\n");

    for (;;) {
        int in_continuation = (brace_depth > 0) || line_continuation;
        fputs(in_continuation ? "\x1b[38;2;153;221;255m..>\033[0m " : "\x1b[38;2;153;221;255m>>>\033[0m ", stdout);
        fflush(stdout);

        char *line = NULL;
        {
            size_t rllen = 0;
            size_t rlcap = 0;
            char chunk[512];
            while (fgets(chunk, sizeof(chunk), stdin)) {
                if (buf_append(&line, &rllen, &rlcap, chunk) != 0) {
                    free(line);
                    line = NULL;
                    break;
                }
                size_t n = strlen(chunk);
                if (n > 0 && chunk[n - 1] == '\n') {
                    break;
                }
            }
            if (rllen == 0 && feof(stdin)) {
                free(line);
                line = NULL;
            } else if (!line) {
                line = strdup("");
            }
        }
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
            if (eof) {
                break;
            }
            continue;
        }

        if (is_exit_meta_command(entry)) {
            break;
        }

        Lexer lex;
        lexer_init(&lex, entry, "<repl>");

        Parser parser;
        parser_init(&parser, &lex);
        Stmt *program = parser_parse(&parser);

        if (!parser.had_error) {
            ExecResult res = exec_program_in_env(&interp, program, interp.global_env);
            if (res.status == EXEC_ERROR) {
                fprintf(stderr, "%s\n", res.error ? res.error : "RuntimeError");
                if (res.error) {
                    free(res.error);
                }
                interpreter_reset_traceback(&interp, interp.global_env);
            }
        }

        entry_len = 0;
        if (entry) {
            entry[0] = '\0';
        }
        brace_depth = 0;
        line_continuation = 0;

        if (eof) {
            break;
        }
    }

    free(entry);
    interpreter_destroy(&interp);
    return PREFIX_SUCCESS;
}

int main(int argc, char **argv) {
    const char *path = NULL;
    int source_mode = 0;
    char *source_text = NULL;
    int verbose_flag = 0;
    int private_flag = 0;

    builtins_reset_dynamic();
    builtins_set_argv(argc, argv);

    char cwd_buf[4096];
    const char *cwd = NULL;
#ifdef _WIN32
    if (_getcwd(cwd_buf, sizeof(cwd_buf))) {
#else
    if (getcwd(cwd_buf, sizeof(cwd_buf))) {
#endif
        cwd = cwd_buf;
    }
    char *exe_dir = path_dirname_dup((argc > 0) ? argv[0] : NULL);
    extensions_set_runtime_dirs(exe_dir ? exe_dir : ".", cwd ? cwd : ".");
    free(exe_dir);

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "-verbose") == 0) {
            verbose_flag = 1;
            continue;
        }

        if (strcmp(arg, "-private") == 0) {
            private_flag = 1;
            continue;
        }

        if (strcmp(arg, "-source") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing argument for -source\n");
                extensions_shutdown();
                builtins_reset_dynamic();
                return PREFIX_ERROR_IO;
            }
            source_mode = 1;
            source_text = strdup(argv[++i]);
            if (!source_text) {
                extensions_shutdown();
                builtins_reset_dynamic();
                fprintf(stderr, "Out of memory\n");
                return PREFIX_ERROR_MEMORY;
            }
            continue;
        }

        if (is_extension_arg(arg)) {
            char *err = NULL;
            if (extensions_load_library(arg, NULL, &err) != 0) {
                fprintf(stderr, "%s\n", err ? err : "Failed to load extension");
                free(err);
                extensions_shutdown();
                builtins_reset_dynamic();
                return PREFIX_ERROR_IO;
            }
            free(err);
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

    if (!path && !source_mode) {
        int repl_rc = extensions_call_repl_handler();
        if (repl_rc >= 0) {
            extensions_shutdown();
            builtins_reset_dynamic();
            return repl_rc;
        }
        repl_rc = run_repl(verbose_flag, private_flag);
        extensions_shutdown();
        builtins_reset_dynamic();
        return repl_rc;
    }

    char *src = NULL;
    char *source_label = NULL;

    if (source_mode) {
        /* Per SPECIFICATION: when running with -source the primary
           program's module name should be "<string>" (not "<source>"). */
        source_label = strdup("<string>");
        src = strdup(source_text ? source_text : "");
        if (!source_label || !src) {
            free(source_label);
            free(src);
            extensions_shutdown();
            builtins_reset_dynamic();
            fprintf(stderr, "Out of memory\n");
            return PREFIX_ERROR_MEMORY;
        }
    } else {
        /* Canonicalize the provided program path now so it's correct even if
           the process changes cwd below. This prevents relative paths like
           "./tests/test2.pre" from resolving incorrectly after chdir. */
        if (path) {
            source_label = prefix_fullpath_dup(path);
        }
        if (!source_label && path) {
            source_label = strdup(path);
        }

        FILE *f = fopen(path, "rb");
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
        if (sz < 0) {
            sz = 0;
        }
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
    }

    Lexer lex;
    lexer_init(&lex, src, source_label);

    Parser parser;
    parser_init(&parser, &lex);

    Stmt *program = parser_parse(&parser);
    if (parser.had_error) {
        free(src);
        if (source_text) {
            free(source_text);
        }
        extensions_shutdown();
        builtins_reset_dynamic();
        return PREFIX_ERROR_SYNTAX;
    }

    // Change working directory to the directory containing the script
    // so relative READFILE/WRITEFILE operate relative to the script.
    if (path) {
        char *dir = strdup(path);
        char *last_slash = NULL;
        for (char *p = dir; *p; p++) {
            if (*p == '/' || *p == '\\') {
                last_slash = p;
            }
        }
        if (last_slash) {
            *last_slash = '\0';
#ifdef _WIN32
            _chdir(dir);
#else
            chdir(dir);
#endif
        }
        free(dir);
    }

    Interpreter interp;
    interpreter_init(&interp, source_label, verbose_flag != 0, private_flag != 0);
    ExecResult res = exec_program_in_env(&interp, program, interp.global_env);
    interpreter_destroy(&interp);
    if (res.status == EXEC_ERROR) {
        fprintf(stderr, "%s\n", res.error ? res.error : "RuntimeError");
        if (res.error) {
            free(res.error);
        }
        free(src);
        if (source_label) {
            free(source_label);
        }
        if (source_text) {
            free(source_text);
        }
        extensions_shutdown();
        builtins_reset_dynamic();
        return PREFIX_ERROR_RUNTIME;
    }

    free(src);
    if (source_label) {
        free(source_label);
    }
    if (source_text) {
        free(source_text);
    }
    extensions_shutdown();
    builtins_reset_dynamic();
    return PREFIX_SUCCESS;
}
