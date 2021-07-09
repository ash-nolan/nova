// Copyright 2021 The Nova Project Authors
// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <string.h>

#include "nova.h"

static enum token_kind const KEYWORDS_FIRST = TOKEN_TRUE;
static enum token_kind const KEYWORDS_LAST = TOKEN_SYSCALL;
static enum token_kind const SIGILS_FIRST = TOKEN_EQ;
static enum token_kind const SIGILS_LAST = TOKEN_SEMICOLON;
static struct autil_vstr token_kind_vstrs[TOKEN_EOF + 1u] = {
    // Keywords
    [TOKEN_TRUE] = AUTIL_VSTR_INIT_STR_LITERAL("true"),
    [TOKEN_FALSE] = AUTIL_VSTR_INIT_STR_LITERAL("false"),
    [TOKEN_NOT] = AUTIL_VSTR_INIT_STR_LITERAL("not"),
    [TOKEN_OR] = AUTIL_VSTR_INIT_STR_LITERAL("or"),
    [TOKEN_AND] = AUTIL_VSTR_INIT_STR_LITERAL("and"),
    [TOKEN_VAR] = AUTIL_VSTR_INIT_STR_LITERAL("var"),
    [TOKEN_CONST] = AUTIL_VSTR_INIT_STR_LITERAL("const"),
    [TOKEN_FUNC] = AUTIL_VSTR_INIT_STR_LITERAL("func"),
    [TOKEN_DUMP] = AUTIL_VSTR_INIT_STR_LITERAL("dump"),
    [TOKEN_RETURN] = AUTIL_VSTR_INIT_STR_LITERAL("return"),
    [TOKEN_IF] = AUTIL_VSTR_INIT_STR_LITERAL("if"),
    [TOKEN_ELIF] = AUTIL_VSTR_INIT_STR_LITERAL("elif"),
    [TOKEN_ELSE] = AUTIL_VSTR_INIT_STR_LITERAL("else"),
    [TOKEN_FOR] = AUTIL_VSTR_INIT_STR_LITERAL("for"),
    [TOKEN_IN] = AUTIL_VSTR_INIT_STR_LITERAL("in"),
    [TOKEN_SYSCALL] = AUTIL_VSTR_INIT_STR_LITERAL("syscall"),
    // Sigils
    [TOKEN_EQ] = AUTIL_VSTR_INIT_STR_LITERAL("=="),
    [TOKEN_NE] = AUTIL_VSTR_INIT_STR_LITERAL("!="),
    [TOKEN_LE] = AUTIL_VSTR_INIT_STR_LITERAL("<="),
    [TOKEN_LT] = AUTIL_VSTR_INIT_STR_LITERAL("<"),
    [TOKEN_GE] = AUTIL_VSTR_INIT_STR_LITERAL(">="),
    [TOKEN_GT] = AUTIL_VSTR_INIT_STR_LITERAL(">"),
    [TOKEN_ASSIGN] = AUTIL_VSTR_INIT_STR_LITERAL("="),
    [TOKEN_PLUS] = AUTIL_VSTR_INIT_STR_LITERAL("+"),
    [TOKEN_DASH] = AUTIL_VSTR_INIT_STR_LITERAL("-"),
    [TOKEN_STAR] = AUTIL_VSTR_INIT_STR_LITERAL("*"),
    [TOKEN_FSLASH] = AUTIL_VSTR_INIT_STR_LITERAL("/"),
    [TOKEN_LPAREN] = AUTIL_VSTR_INIT_STR_LITERAL("("),
    [TOKEN_RPAREN] = AUTIL_VSTR_INIT_STR_LITERAL(")"),
    [TOKEN_LBRACE] = AUTIL_VSTR_INIT_STR_LITERAL("{"),
    [TOKEN_RBRACE] = AUTIL_VSTR_INIT_STR_LITERAL("}"),
    [TOKEN_COMMA] = AUTIL_VSTR_INIT_STR_LITERAL(","),
    [TOKEN_COLON] = AUTIL_VSTR_INIT_STR_LITERAL(":"),
    [TOKEN_SEMICOLON] = AUTIL_VSTR_INIT_STR_LITERAL(";"),
    // Identifiers and Non-Keyword Literals
    [TOKEN_IDENTIFIER] = AUTIL_VSTR_INIT_STR_LITERAL("identifier"),
    [TOKEN_INTEGER] = AUTIL_VSTR_INIT_STR_LITERAL("integer"),
    // Meta
    [TOKEN_EOF] = AUTIL_VSTR_INIT_STR_LITERAL("end-of-file"),
};

char const*
token_kind_to_cstr(enum token_kind kind)
{
    assert((size_t)kind <= TOKEN_EOF);
    return token_kind_vstrs[(size_t)kind].start;
}

char*
token_to_new_cstr(struct token const* token)
{
    assert(token != NULL);

    if (token->kind == TOKEN_IDENTIFIER) {
        return autil_cstr_new_fmt(
            "identifier(%.*s)", (int)token->count, token->start);
    }
    if (token->kind == TOKEN_INTEGER) {
        return autil_cstr_new_fmt(
            "integer(%.*s)", (int)token->count, token->start);
    }

    return autil_cstr_new_cstr(token_kind_to_cstr(token->kind));
}

struct lexer {
    struct module* module;
    char const* current;
    size_t current_line;

    // Starting location of the token parsed by lexer_next_token, set at the
    // beginning of that function and read during the eventual token_new call.
    struct source_location next_token_location;
};

struct lexer*
lexer_new(struct module* module)
{
    assert(module != NULL);

    struct lexer* const self = autil_xalloc(NULL, sizeof(*self));
    memset(self, 0x00, sizeof(*self));

    self->module = module;
    self->current = module->source;
    self->current_line = 1;

    return self;
}

void
lexer_del(struct lexer* self)
{
    assert(self != NULL);

    memset(self, 0x00, sizeof(*self));
    autil_xalloc(self, AUTIL_XALLOC_FREE);
}

static struct token*
token_new(
    char const* start,
    size_t count,
    struct source_location location,
    enum token_kind kind)
{
    assert(start != NULL || count == 0);
    assert(count != 0 || kind == TOKEN_EOF);

    struct token* const self = autil_xalloc(NULL, sizeof(*self));
    memset(self, 0x00, sizeof(*self));
    self->kind = kind;
    self->start = start;
    self->count = count;
    self->location = location;

    autil_freezer_register(context()->freezer, self);
    return self;
}

static struct token*
token_new_identifier(
    char const* start, size_t count, struct source_location location)
{
    assert(start != NULL && count != 0);

    struct token* const token =
        token_new(start, count, location, TOKEN_IDENTIFIER);
    return token;
}

static struct token*
token_new_integer(
    char const* start,
    size_t count,
    struct source_location location,
    struct autil_vstr number,
    struct autil_vstr suffix)
{
    assert(start != NULL && count != 0);
    assert(number.start != NULL && number.count != 0);
    assert(suffix.start != NULL || suffix.count == 0);

    struct token* const token =
        token_new(start, count, location, TOKEN_INTEGER);
    token->data.integer.number = number;
    token->data.integer.suffix = suffix;
    return token;
}

static size_t
skip_whitespace(struct lexer* self)
{
    assert(self != NULL);

    size_t count = 0;
    while (autil_isspace(self->current[count])) {
        self->current_line += self->current[count] == '\n';
        count += 1;
    }
    self->current += count;
    return count;
}

static size_t
skip_comment(struct lexer* self)
{
    assert(self != NULL);

    if (*self->current != '#') {
        return 0;
    }

    size_t count = 0;
    while (self->current[count] != '\0' && self->current[count] != '\n') {
        count += 1;
    }
    self->current += count + AUTIL_STR_LITERAL_COUNT("\n");
    self->current_line += AUTIL_STR_LITERAL_COUNT("\n");
    return count;
}

static void
skip_whitespace_and_comments(struct lexer* self)
{
    assert(self != NULL);

    while (autil_isspace(*self->current) || (*self->current == '#')) {
        skip_whitespace(self);
        skip_comment(self);
    }
}

static struct token*
lex_keyword_or_identifier(struct lexer* self)
{
    assert(self != NULL);
    assert(autil_isalpha(*self->current) || *self->current == '_');

    char const* const start = self->current;
    while (autil_isalnum(*self->current) || *self->current == '_') {
        self->current += 1;
    }
    size_t const count = (size_t)(self->current - start);
    struct autil_vstr const symbol = {start, count};

    for (int i = (int)KEYWORDS_FIRST; i <= (int)KEYWORDS_LAST; ++i) {
        struct autil_vstr const* const keyword = &token_kind_vstrs[i];
        if (autil_vstr_cmp(&symbol, keyword) == 0) {
            return token_new(
                start, count, self->next_token_location, (enum token_kind)i);
        }
    }

    return token_new_identifier(start, count, self->next_token_location);
}

static struct token*
lex_integer(struct lexer* self)
{
    assert(self != NULL);
    assert(autil_isdigit(*self->current));

    // Prefix
    char const* const number_start = self->current;
    int (*radix_isdigit)(int c) = autil_isdigit;
    if (autil_cstr_starts_with(self->current, "0b")) {
        self->current += AUTIL_STR_LITERAL_COUNT("0b");
        radix_isdigit = autil_isbdigit;
    }
    else if (autil_cstr_starts_with(self->current, "0o")) {
        self->current += AUTIL_STR_LITERAL_COUNT("0o");
        radix_isdigit = autil_isodigit;
    }
    else if (autil_cstr_starts_with(self->current, "0x")) {
        self->current += AUTIL_STR_LITERAL_COUNT("0x");
        radix_isdigit = autil_isxdigit;
    }

    // Digits
    if (!radix_isdigit(*self->current)) {
        fatal(
            self->module->path,
            self->current_line,
            "integer literal has no digits");
    }
    while (radix_isdigit(*self->current)) {
        self->current += 1;
    }
    size_t const number_count = (size_t)(self->current - number_start);

    // Suffix
    char const* const suffix_start = self->current;
    while (autil_isalnum(*self->current)) {
        self->current += 1;
    }
    size_t const suffix_count = (size_t)(self->current - suffix_start);

    char const* const start = number_start;
    size_t const count = (size_t)(self->current - start);
    struct autil_vstr const number = {number_start, number_count};
    struct autil_vstr const suffix = {suffix_start, suffix_count};
    struct token* const token = token_new_integer(
        start, count, self->next_token_location, number, suffix);

    return token;
}

static struct token*
lex_sigil(struct lexer* self)
{
    assert(self != NULL);
    assert(autil_ispunct(*self->current));

    for (int i = (int)SIGILS_FIRST; i <= (int)SIGILS_LAST; ++i) {
        char const* const sigil_start = token_kind_vstrs[i].start;
        size_t const sigil_count = token_kind_vstrs[i].count;
        if (autil_cstr_starts_with(self->current, sigil_start)) {
            self->current += sigil_count;
            return token_new(
                sigil_start,
                sigil_count,
                self->next_token_location,
                (enum token_kind)i);
        }
    }

    char const* const start = self->current;
    size_t count = 0;
    while (autil_ispunct(start[count]) && start[count] != '#') {
        count += 1;
    }
    fatal(
        self->module->path,
        self->current_line,
        "invalid token `%.*s`",
        (int)count,
        start);
    return NULL;
}

struct token const*
lexer_next_token(struct lexer* self)
{
    assert(self != NULL);

    skip_whitespace_and_comments(self);
    self->next_token_location = (struct source_location){
        self->module->path,
        self->current_line,
    };

    char const ch = *self->current;
    if (autil_isalpha(ch) || ch == '_') {
        return lex_keyword_or_identifier(self);
    }
    if (autil_isdigit(ch)) {
        return lex_integer(self);
    }
    if (autil_ispunct(ch)) {
        return lex_sigil(self);
    }
    if (ch == '\0') {
        return token_new(
            self->current, 1u, self->next_token_location, TOKEN_EOF);
    }

    fatal(self->module->path, self->current_line, "invalid token");
    return NULL;
}
