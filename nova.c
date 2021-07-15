// Copyright 2021 The Nova Project Authors
// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h> /* pid_t */
#include <sys/wait.h> /* wait* */
#include <unistd.h> /* exec*, isatty, fork */

#include "nova.h"

// clang-format off
#define ANSI_ESC_DEFAULT "\x1b[0m"
#define ANSI_ESC_BOLD    "\x1b[1m"

#define ANSI_ESC_RED     "\x1b[31m"
#define ANSI_ESC_YELLOW  "\x1b[33m"
#define ANSI_ESC_MAGENTA "\x1b[35m"
#define ANSI_ESC_CYAN    "\x1b[36m"

#define ANSI_MSG_TRACE ANSI_ESC_BOLD ANSI_ESC_MAGENTA
#define ANSI_MSG_DEBUG ANSI_ESC_BOLD ANSI_ESC_YELLOW
#define ANSI_MSG_ERROR ANSI_ESC_BOLD ANSI_ESC_RED
// clang-format on

static void
messagev_(
    char const* path,
    size_t line,
    char const* level_text,
    char const* level_ansi,
    char const* fmt,
    va_list args)
{
    assert(path != NO_PATH || line == NO_LINE);
    assert(level_text != NULL);
    assert(level_ansi != NULL);
    assert(fmt != NULL);

    bool const is_tty = isatty(STDERR_FILENO);

    if (path != NO_PATH) {
        fprintf(stderr, "[");

        char const* const path_ansi_beg = is_tty ? ANSI_ESC_CYAN : "";
        char const* const path_ansi_end = is_tty ? ANSI_ESC_DEFAULT : "";
        fprintf(stderr, "%s%s%s", path_ansi_beg, path, path_ansi_end);

        if (line != NO_LINE) {
            char const* const line_ansi_beg = is_tty ? ANSI_ESC_CYAN : "";
            char const* const line_ansi_end = is_tty ? ANSI_ESC_DEFAULT : "";
            fprintf(stderr, ":%s%zu%s", line_ansi_beg, line, line_ansi_end);
        }

        fprintf(stderr, "] ");
    }

    char const* const level_ansi_beg = is_tty ? level_ansi : "";
    char const* const level_ansi_end = is_tty ? ANSI_ESC_DEFAULT : "";
    fprintf(stderr, "%s%s:%s ", level_ansi_beg, level_text, level_ansi_end);

    vfprintf(stderr, fmt, args);
    fputs("\n", stderr);
}

void
trace(char const* path, size_t line, char const* fmt, ...)
{
#if ENABLE_TRACE != 0
    va_list args;
    va_start(args, fmt);
    messagev_(path, line, "trace", ANSI_MSG_TRACE, fmt, args);
    va_end(args);
#else
    (void)path;
    (void)line;
    (void)fmt;
    (void)messagev_;
#endif
}

void
debug(char const* path, size_t line, char const* fmt, ...)
{
#if ENABLE_DEBUG != 0
    va_list args;
    va_start(args, fmt);
    messagev_(path, line, "debug", ANSI_MSG_DEBUG, fmt, args);
    va_end(args);
#else
    (void)path;
    (void)line;
    (void)fmt;
    (void)messagev_;
#endif
}

void
error(char const* path, size_t line, char const* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    messagev_(path, line, "error", ANSI_MSG_ERROR, fmt, args);
    va_end(args);
}

void
fatal(char const* path, size_t line, char const* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    messagev_(path, line, "error", ANSI_MSG_ERROR, fmt, args);
    va_end(args);

    exit(EXIT_FAILURE);
}

void
todo(char const* file, int line, char const* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[%s:%d] TODO: ", file, line);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fputs("\n", stderr);
    exit(EXIT_FAILURE);
}

void
unreachable(char const* file, int line)
{
    fprintf(stderr, "[%s:%d] Unreachable!\n", file, line);
    exit(EXIT_FAILURE);
}

// TODO: Check if ((x + 7) & (-8)) is portable. Since it relies on two's
// complement it might not be strictly conforming for signed integers.
int
ceil8i(int x)
{
    while (x % 8 != 0) {
        x += 1;
    }
    return x;
}

size_t
ceil8z(size_t x)
{
    while (x % 8u != 0u) {
        x += 1u;
    }
    return x;
}

int
spawnvpw(char const* path, char const* const* argv)
{
    assert(path != NULL);
    assert(argv != NULL);
    assert(argv[0] != NULL);

    pid_t const pid = fork();
    if (pid == -1) {
        fatal(
            NO_PATH,
            NO_LINE,
            "failed to fork with error '%s'",
            strerror(errno));
    }

    if (pid == 0) {
        // The POSIX 2017 rational section for the exec family of functions
        // notes that the neither the argv vector's elements nor the characters
        // within those elements are modified. The parameter declaration `char
        // *const argv[]` was chosen to allow for for historical compatibility.
        char* const* argv_ = (char* const*)argv;
        if (execvp(path, argv_) == -1) {
            fatal(
                NO_PATH,
                NO_LINE,
                "failed to execvp '%s' with error '%s'",
                path,
                strerror(errno));
        }
    }

    int status = 0;
    waitpid(pid, &status, 0);
    return status;
}

int
bigint_to_uz(size_t* res, struct autil_bigint const* bigint)
{
    assert(bigint != NULL);

    if (autil_bigint_cmp(bigint, AUTIL_BIGINT_ZERO) < 0) {
        return -1;
    }

    char* const cstr = autil_bigint_to_new_cstr(bigint, NULL);
    errno = 0;
    uintmax_t umax = strtoumax(cstr, NULL, 0);
    int const err = errno; // save errno
    autil_xalloc(cstr, AUTIL_XALLOC_FREE);
    assert(err == 0 || err == ERANGE);
    if (err == ERANGE) {
        return -1;
    }

    if (umax > SIZE_MAX) {
        return -1;
    }
    *res = (size_t)umax;
    return 0;
}

void
xspawnvpw(char const* path, char const* const* argv)
{
    assert(path != NULL);
    assert(argv != NULL);
    assert(argv[0] != NULL);

    if (spawnvpw(path, argv) != 0) {
        exit(EXIT_FAILURE);
    }
}

static char*
read_source(char const* path)
{
    void* source = NULL;
    size_t source_size = 0;
    if (autil_file_read(path, &source, &source_size)) {
        fatal(
            path,
            NO_LINE,
            "failed to read source with error '%s'",
            strerror(errno));
    }

    // NUL terminate.
    source = autil_xalloc(source, source_size + 1);
    ((char*)source)[source_size] = '\0';

    return source;
}

struct module*
module_new(char const* path)
{
    assert(path != NULL);

    struct module* const self = autil_xalloc(NULL, sizeof(*self));
    memset(self, 0x00, sizeof(*self));

    self->path = autil_sipool_intern_cstr(context()->sipool, path);

    char* const source = read_source(self->path);
    autil_freezer_register(context()->freezer, source);
    self->source = source;

    return self;
}

void
module_del(struct module* self)
{
    assert(self != NULL);

    autil_sbuf_fini(self->ordered);
    autil_xalloc(memset(self, 0x00, sizeof(*self)), AUTIL_XALLOC_FREE);
}

static struct context s_context = {0};

void
context_init(void)
{
    s_context.freezer = autil_freezer_new();

    s_context.sipool = autil_sipool_new();
#define INTERN_STR_LITERAL(str_literal)                                        \
    autil_sipool_intern_cstr(context()->sipool, str_literal)
    s_context.interned.empty = INTERN_STR_LITERAL("");
    s_context.interned.builtin = INTERN_STR_LITERAL("builtin");
    s_context.interned.return_ = INTERN_STR_LITERAL("return");
    s_context.interned.void_ = INTERN_STR_LITERAL("void");
    s_context.interned.bool_ = INTERN_STR_LITERAL("bool");
    s_context.interned.byte = INTERN_STR_LITERAL("byte");
    s_context.interned.usize = INTERN_STR_LITERAL("usize");
    s_context.interned.ssize = INTERN_STR_LITERAL("ssize");
    s_context.interned.u = INTERN_STR_LITERAL("u");
    s_context.interned.s = INTERN_STR_LITERAL("s");
#undef INTERN_STR_LITERAL

    s_context.global_symbol_table = symbol_table_new(NULL);
    s_context.module = NULL;

    s_context.builtin.location = (struct source_location){
        s_context.interned.builtin,
        NO_LINE,
    };
    struct type* const type_void = type_new_void();
    struct type* const type_bool = type_new_bool();
    struct type* const type_byte = type_new_byte();
    struct type* const type_usize = type_new_usize();
    struct type* const type_ssize = type_new_ssize();
    autil_freezer_register(context()->freezer, type_void);
    autil_freezer_register(context()->freezer, type_bool);
    autil_freezer_register(context()->freezer, type_byte);
    autil_freezer_register(context()->freezer, type_usize);
    autil_freezer_register(context()->freezer, type_ssize);
    struct symbol* const symbol_void =
        symbol_new_type(&s_context.builtin.location, type_void);
    struct symbol* const symbol_bool =
        symbol_new_type(&s_context.builtin.location, type_bool);
    struct symbol* const symbol_byte =
        symbol_new_type(&s_context.builtin.location, type_byte);
    struct symbol* const symbol_usize =
        symbol_new_type(&s_context.builtin.location, type_usize);
    struct symbol* const symbol_ssize =
        symbol_new_type(&s_context.builtin.location, type_ssize);
    autil_freezer_register(context()->freezer, symbol_void);
    autil_freezer_register(context()->freezer, symbol_bool);
    autil_freezer_register(context()->freezer, symbol_byte);
    autil_freezer_register(context()->freezer, symbol_usize);
    autil_freezer_register(context()->freezer, symbol_ssize);
    symbol_table_insert(s_context.global_symbol_table, symbol_void);
    symbol_table_insert(s_context.global_symbol_table, symbol_bool);
    symbol_table_insert(s_context.global_symbol_table, symbol_byte);
    symbol_table_insert(s_context.global_symbol_table, symbol_usize);
    symbol_table_insert(s_context.global_symbol_table, symbol_ssize);
    s_context.builtin.void_ = type_void;
    s_context.builtin.bool_ = type_bool;
    s_context.builtin.byte = type_byte;
    s_context.builtin.usize = type_usize;
    s_context.builtin.ssize = type_ssize;
}

void
context_fini(void)
{
    struct context* const self = &s_context;

    autil_sipool_del(self->sipool);

    autil_freezer_del(self->freezer);

    autil_sbuf_fini(s_context.global_symbol_table->symbols);
    autil_xalloc(s_context.global_symbol_table, AUTIL_XALLOC_FREE);

    module_del(self->module);

    memset(self, 0x00, sizeof(*self));
}

struct context const*
context(void)
{
    return &s_context;
}

void
load_module(char const* path)
{
    assert(s_context.module == NULL);

    struct module* const module = module_new(path);
    s_context.module = module;
    parse(module);
    order(module);
    resolve(module);
}

#define AUTIL_IMPLEMENTATION
#include <autil/autil.h>
