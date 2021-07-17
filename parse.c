// Copyright 2021 The Nova Project Authors
// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>

#include "nova.h"

struct parser {
    struct module* module;
    struct lexer* lexer;

    struct token const* current_token;
    struct token const* peek_token;
};
static struct parser*
parser_new(struct module* module, struct lexer* lexer);
static void
parser_del(struct parser* self);
static struct token const*
advance_token(struct parser* parser);
static bool
check_current(struct parser const* parser, enum token_kind kind);
static bool
check_peek(struct parser const* parser, enum token_kind kind);
static struct token const*
expect_current(struct parser* parser, enum token_kind kind);

static struct ast_module const*
parse_module(struct parser* parser);

static struct ast_decl const*
parse_decl(struct parser* parser);
static struct ast_decl const*
parse_decl_variable(struct parser* parser);
static struct ast_decl const*
parse_decl_constant(struct parser* parser);
static struct ast_decl const*
parse_decl_function(struct parser* parser);

static struct ast_stmt const*
parse_stmt(struct parser* parser);
static struct ast_stmt const*
parse_stmt_if(struct parser* parser);
static struct ast_stmt const*
parse_stmt_for(struct parser* parser);
static struct ast_stmt const*
parse_stmt_dump(struct parser* parser);
static struct ast_stmt const*
parse_stmt_return(struct parser* parser);
static struct ast_stmt const*
parse_stmt_decl(struct parser* parser);

enum precedence {
    PRECEDENCE_LOWEST,
    PRECEDENCE_OR, // or
    PRECEDENCE_AND, // and
    PRECEDENCE_COMPARE, // ==
    PRECEDENCE_SUM, // + -
    PRECEDENCE_PRODUCT, // * /
    PRECEDENCE_PREFIX, // +x OR -x OR *x OR &x
    PRECEDENCE_POSTFIX, // foo(bar, 123) OR foo[42]
};
// Returns the precedence corresponding to provided token or PRECEDENCE_LOWEST
// if no precedence mapping exists.
static enum precedence
token_kind_precedence(enum token_kind kind);
static enum precedence
current_precedence(struct parser* parser);

// Parse function for a null denotation.
typedef struct ast_expr const* (*parse_nud_fn)(struct parser*);
// Parse function for a left denotation.
typedef struct ast_expr const* (*parse_led_fn)(
    struct parser*, struct ast_expr const*);
// Returns NULL if no function is associated with the provided token kind.
static parse_nud_fn
token_kind_nud(enum token_kind kind);
// Returns NULL if no function is associated with the provided token kind.
static parse_led_fn
token_kind_led(enum token_kind kind);

static struct ast_expr const*
parse_expr(struct parser* parser);
static struct ast_expr const*
parse_expr_identifier(struct parser* parser);
static struct ast_expr const*
parse_expr_boolean(struct parser* parser);
static struct ast_expr const*
parse_expr_integer(struct parser* parser);
static struct ast_expr const*
parse_expr_lparen(struct parser* parser);
static struct ast_expr const*
parse_expr_syscall(struct parser* parser);
static struct ast_expr const*
parse_expr_led_call(struct parser* parser, struct ast_expr const* lhs);
static struct ast_expr const*
parse_expr_led_index(struct parser* parser, struct ast_expr const* lhs);
static struct ast_expr const*
parse_expr_nud_unary(struct parser* parser);
static struct ast_expr const*
parse_expr_led_binary(struct parser* parser, struct ast_expr const* lhs);

static struct ast_block const*
parse_block(struct parser* parser);

static struct ast_parameter const* const*
parse_parameter_list(struct parser* parser);
static struct ast_parameter const*
parse_parameter(struct parser* parser);

static struct ast_typespec const*
parse_typespec(struct parser* parser);
static struct ast_typespec const*
parse_typespec_identifier(struct parser* parser);
static struct ast_typespec const*
parse_typespec_function(struct parser* parser);
static struct ast_typespec const*
parse_typespec_pointer(struct parser* parser);
static struct ast_typespec const*
parse_typespec_array(struct parser* parser);

static struct ast_identifier const*
parse_identifier(struct parser* parser);

static struct ast_boolean const*
parse_boolean(struct parser* parser);

static struct ast_integer const*
parse_integer(struct parser* parser);

static struct parser*
parser_new(struct module* module, struct lexer* lexer)
{
    assert(module != NULL);
    assert(lexer != NULL);

    struct parser* const self = autil_xalloc(NULL, sizeof(*self));
    self->module = module;
    self->lexer = lexer;
    self->current_token = NULL;
    advance_token(self);
    advance_token(self);

    assert(self->current_token != NULL);
    assert(self->peek_token != NULL);
    return self;
}

static void
parser_del(struct parser* self)
{
    assert(self != NULL);

    memset(self, 0x00, sizeof(*self));
    autil_xalloc(self, AUTIL_XALLOC_FREE);
}

static struct token const*
advance_token(struct parser* parser)
{
    assert(parser != NULL);

    struct token const* const current_token = parser->current_token;
    parser->current_token = parser->peek_token;
    parser->peek_token = lexer_next_token(parser->lexer);
    return current_token;
}

static bool
check_current(struct parser const* parser, enum token_kind kind)
{
    assert(parser != NULL);

    return parser->current_token->kind == kind;
}

static bool
check_peek(struct parser const* parser, enum token_kind kind)
{
    assert(parser != NULL);

    return parser->peek_token->kind == kind;
}

static struct token const*
expect_current(struct parser* parser, enum token_kind kind)
{
    assert(parser != NULL);

    if (!check_current(parser, kind)) {
        char const* const expected = token_kind_to_cstr(kind);
        char* const found = token_to_new_cstr(parser->current_token);
        fatal(
            parser->current_token->location.path,
            parser->current_token->location.line,
            "expected `%s`, found `%s`",
            expected,
            found);
    }
    return advance_token(parser);
}

static struct ast_module const*
parse_module(struct parser* parser)
{
    assert(parser != NULL);
    trace(parser->module->path, NO_LINE, "%s", __func__);

    autil_sbuf(struct ast_decl const*) decls = NULL;
    while (!check_current(parser, TOKEN_EOF)) {
        autil_sbuf_push(decls, parse_decl(parser));
    }

    autil_sbuf_freeze(decls, context()->freezer);
    struct ast_module* const product = ast_module_new(decls);

    autil_freezer_register(context()->freezer, product);
    return product;
}

static struct ast_decl const*
parse_decl(struct parser* parser)
{
    assert(parser != NULL);
    trace(parser->module->path, NO_LINE, "%s", __func__);

    if (check_current(parser, TOKEN_VAR)) {
        return parse_decl_variable(parser);
    }

    if (check_current(parser, TOKEN_CONST)) {
        return parse_decl_constant(parser);
    }

    if (check_current(parser, TOKEN_FUNC)) {
        return parse_decl_function(parser);
    }

    fatal(
        parser->current_token->location.path,
        parser->current_token->location.line,
        "expected declaration, found `%s`",
        token_to_new_cstr(parser->current_token));
    return NULL;
}

static struct ast_decl const*
parse_decl_variable(struct parser* parser)
{
    assert(parser != NULL);
    trace(parser->module->path, NO_LINE, "%s", __func__);

    struct source_location const* const location =
        &expect_current(parser, TOKEN_VAR)->location;
    struct ast_identifier const* const identifier = parse_identifier(parser);
    expect_current(parser, TOKEN_COLON);
    struct ast_typespec const* const typespec = parse_typespec(parser);
    expect_current(parser, TOKEN_ASSIGN);
    struct ast_expr const* const expr = parse_expr(parser);
    expect_current(parser, TOKEN_SEMICOLON);

    struct ast_decl* const product =
        ast_decl_new_variable(location, identifier, typespec, expr);

    autil_freezer_register(context()->freezer, product);
    return product;
}

static struct ast_decl const*
parse_decl_constant(struct parser* parser)
{
    assert(parser != NULL);
    trace(parser->module->path, NO_LINE, "%s", __func__);

    struct source_location const* const location =
        &expect_current(parser, TOKEN_CONST)->location;
    struct ast_identifier const* const identifier = parse_identifier(parser);
    expect_current(parser, TOKEN_COLON);
    struct ast_typespec const* const typespec = parse_typespec(parser);
    expect_current(parser, TOKEN_ASSIGN);
    struct ast_expr const* const expr = parse_expr(parser);
    expect_current(parser, TOKEN_SEMICOLON);

    struct ast_decl* const product =
        ast_decl_new_constant(location, identifier, typespec, expr);

    autil_freezer_register(context()->freezer, product);
    return product;
}

static struct ast_decl const*
parse_decl_function(struct parser* parser)
{
    assert(parser != NULL);
    trace(parser->module->path, NO_LINE, "%s", __func__);

    struct source_location const* const location =
        &expect_current(parser, TOKEN_FUNC)->location;
    struct ast_identifier const* const identifier = parse_identifier(parser);
    expect_current(parser, TOKEN_LPAREN);
    autil_sbuf(struct ast_parameter const* const) parameters =
        parse_parameter_list(parser);
    expect_current(parser, TOKEN_RPAREN);
    struct ast_typespec const* const return_typespec = parse_typespec(parser);
    struct ast_block const* const body = parse_block(parser);

    struct ast_decl* const product = ast_decl_new_func(
        location, identifier, parameters, return_typespec, body);

    autil_freezer_register(context()->freezer, product);
    return product;
}

static struct ast_stmt const*
parse_stmt(struct parser* parser)
{
    assert(parser != NULL);
    trace(parser->module->path, NO_LINE, "%s", __func__);

    if (check_current(parser, TOKEN_VAR) || check_current(parser, TOKEN_CONST)
        || check_current(parser, TOKEN_FUNC)) {
        return parse_stmt_decl(parser);
    }

    if (check_current(parser, TOKEN_IF)) {
        return parse_stmt_if(parser);
    }

    if (check_current(parser, TOKEN_FOR)) {
        return parse_stmt_for(parser);
    }

    if (check_current(parser, TOKEN_DUMP)) {
        return parse_stmt_dump(parser);
    }

    if (check_current(parser, TOKEN_RETURN)) {
        return parse_stmt_return(parser);
    }

    struct ast_expr const* const expr = parse_expr(parser);
    if (check_current(parser, TOKEN_ASSIGN)) {
        // <stmt-assign>
        struct source_location const* const location =
            &expect_current(parser, TOKEN_ASSIGN)->location;
        struct ast_expr const* const rhs = parse_expr(parser);
        expect_current(parser, TOKEN_SEMICOLON);

        struct ast_stmt* const product =
            ast_stmt_new_assign(location, expr, rhs);
        autil_freezer_register(context()->freezer, product);
        return product;
    }

    // <stmt-expr>
    expect_current(parser, TOKEN_SEMICOLON);
    struct ast_stmt* const product = ast_stmt_new_expr(expr);
    autil_freezer_register(context()->freezer, product);
    return product;
}

static struct ast_stmt const*
parse_stmt_if(struct parser* parser)
{
    assert(parser != NULL);
    assert(check_current(parser, TOKEN_IF));
    trace(parser->module->path, NO_LINE, "%s", __func__);

    autil_sbuf(struct ast_conditional const*) conditionals = NULL;

    struct source_location const* location =
        &expect_current(parser, TOKEN_IF)->location;
    struct ast_expr const* condition = parse_expr(parser);
    struct ast_block const* body = parse_block(parser);
    struct ast_conditional* conditional =
        ast_conditional_new(location, condition, body);
    autil_freezer_register(context()->freezer, conditional);
    autil_sbuf_push(conditionals, conditional);

    while (check_current(parser, TOKEN_ELIF)) {
        location = &advance_token(parser)->location;
        condition = parse_expr(parser);
        body = parse_block(parser);
        conditional = ast_conditional_new(location, condition, body);
        autil_freezer_register(context()->freezer, conditional);
        autil_sbuf_push(conditionals, conditional);
    }

    if (check_current(parser, TOKEN_ELSE)) {
        location = &advance_token(parser)->location;
        body = parse_block(parser);
        conditional = ast_conditional_new(location, NULL, body);
        autil_freezer_register(context()->freezer, conditional);
        autil_sbuf_push(conditionals, conditional);
    }

    autil_sbuf_freeze(conditionals, context()->freezer);
    struct ast_stmt* const product = ast_stmt_new_if(conditionals);

    autil_freezer_register(context()->freezer, product);
    return product;
}

static struct ast_stmt const*
parse_stmt_for(struct parser* parser)
{
    assert(parser != NULL);
    assert(check_current(parser, TOKEN_FOR));
    trace(parser->module->path, NO_LINE, "%s", __func__);

    struct source_location const* location =
        &expect_current(parser, TOKEN_FOR)->location;

    // <stmt-for-range>
    if (check_current(parser, TOKEN_IDENTIFIER)
        && check_peek(parser, TOKEN_IN)) {
        struct ast_identifier const* const identifier =
            parse_identifier(parser);
        expect_current(parser, TOKEN_IN);
        struct ast_expr const* const begin = parse_expr(parser);
        expect_current(parser, TOKEN_COLON);
        struct ast_expr const* const end = parse_expr(parser);
        struct ast_block const* const body = parse_block(parser);

        struct ast_stmt* const product =
            ast_stmt_new_for_range(location, identifier, begin, end, body);

        autil_freezer_register(context()->freezer, product);
        return product;
    }

    // <stmt-for-expr>
    struct ast_expr const* const expr = parse_expr(parser);
    struct ast_block const* const body = parse_block(parser);

    struct ast_stmt* const product =
        ast_stmt_new_for_expr(location, expr, body);

    autil_freezer_register(context()->freezer, product);
    return product;
}

static struct ast_stmt const*
parse_stmt_dump(struct parser* parser)
{
    assert(parser != NULL);
    assert(check_current(parser, TOKEN_DUMP));
    trace(parser->module->path, NO_LINE, "%s", __func__);

    struct source_location const* const location =
        &expect_current(parser, TOKEN_DUMP)->location;
    struct ast_expr const* const expr = parse_expr(parser);
    expect_current(parser, TOKEN_SEMICOLON);

    struct ast_stmt* const product = ast_stmt_new_dump(location, expr);

    autil_freezer_register(context()->freezer, product);
    return product;
}

static struct ast_stmt const*
parse_stmt_return(struct parser* parser)
{
    assert(parser != NULL);
    assert(check_current(parser, TOKEN_RETURN));
    trace(parser->module->path, NO_LINE, "%s", __func__);

    struct source_location const* const location =
        &expect_current(parser, TOKEN_RETURN)->location;

    struct ast_expr const* expr = NULL;
    if (!check_current(parser, TOKEN_SEMICOLON)) {
        expr = parse_expr(parser);
    }

    expect_current(parser, TOKEN_SEMICOLON);
    struct ast_stmt* const product = ast_stmt_new_return(location, expr);

    autil_freezer_register(context()->freezer, product);
    return product;
}

static struct ast_stmt const*
parse_stmt_decl(struct parser* parser)
{
    assert(parser != NULL);
    trace(parser->module->path, NO_LINE, "%s", __func__);

    struct ast_decl const* const decl = parse_decl(parser);
    struct ast_stmt* const product = ast_stmt_new_decl(decl);

    autil_freezer_register(context()->freezer, product);
    return product;
}

static enum precedence
token_kind_precedence(enum token_kind kind)
{
    switch (kind) {
    case TOKEN_OR:
        return PRECEDENCE_OR;
    case TOKEN_AND:
        return PRECEDENCE_AND;
    case TOKEN_EQ: /* fallthrough */
    case TOKEN_NE: /* fallthrough */
    case TOKEN_LE: /* fallthrough */
    case TOKEN_LT: /* fallthrough */
    case TOKEN_GE: /* fallthrough */
    case TOKEN_GT:
        return PRECEDENCE_COMPARE;
    case TOKEN_PLUS: /* fallthrough */
    case TOKEN_DASH:
        return PRECEDENCE_SUM;
    case TOKEN_STAR: /* fallthrough */
    case TOKEN_FSLASH: /* fallthrough */
    case TOKEN_AMPERSAND:
        return PRECEDENCE_PRODUCT;
    case TOKEN_LPAREN: /* fallthrough */
    case TOKEN_LBRACKET:
        return PRECEDENCE_POSTFIX;
    default:
        break;
    }

    return PRECEDENCE_LOWEST;
}

static enum precedence
current_precedence(struct parser* parser)
{
    assert(parser != NULL);

    return token_kind_precedence(parser->current_token->kind);
}

static parse_nud_fn
token_kind_nud(enum token_kind kind)
{
    switch (kind) {
    case TOKEN_IDENTIFIER:
        return parse_expr_identifier;
    case TOKEN_TRUE: /* fallthrough */
    case TOKEN_FALSE:
        return parse_expr_boolean;
    case TOKEN_INTEGER:
        return parse_expr_integer;
    case TOKEN_LPAREN:
        return parse_expr_lparen;
    case TOKEN_SYSCALL:
        return parse_expr_syscall;
    case TOKEN_NOT: /* fallthrough */
    case TOKEN_PLUS: /* fallthrough */
    case TOKEN_DASH: /* fallthrough */
    case TOKEN_STAR: /* fallthrough */
    case TOKEN_AMPERSAND:
        return parse_expr_nud_unary;
    default:
        break;
    }

    return NULL;
}

static parse_led_fn
token_kind_led(enum token_kind kind)
{
    trace(NO_PATH, NO_LINE, "%s (token kind => %d)", __func__, (int)kind);
    switch (kind) {
    case TOKEN_LPAREN:
        return parse_expr_led_call;
    case TOKEN_LBRACKET:
        return parse_expr_led_index;
    case TOKEN_OR: /* fallthrough */
    case TOKEN_AND: /* fallthrough */
    case TOKEN_EQ: /* fallthrough */
    case TOKEN_NE: /* fallthrough */
    case TOKEN_LE: /* fallthrough */
    case TOKEN_LT: /* fallthrough */
    case TOKEN_GE: /* fallthrough */
    case TOKEN_GT: /* fallthrough */
    case TOKEN_PLUS: /* fallthrough */
    case TOKEN_DASH: /* fallthrough */
    case TOKEN_STAR: /* fallthrough */
    case TOKEN_FSLASH: /* fallthrough */
    case TOKEN_AMPERSAND:
        return parse_expr_led_binary;
    default:
        break;
    }

    return NULL;
}

static struct ast_expr const*
parse_expr_precedence(struct parser* parser, enum precedence precedence)
{
    assert(parser != NULL);
    trace(
        parser->module->path,
        NO_LINE,
        "%s (precedence => %d)",
        __func__,
        (int)precedence);

    struct token const* const nud_token = parser->current_token;
    parse_nud_fn const parse_nud = token_kind_nud(nud_token->kind);
    if (parse_nud == NULL) {
        fatal(
            nud_token->location.path,
            nud_token->location.line,
            "unrecognized prefix token `%s` in expression",
            token_kind_to_cstr(nud_token->kind));
    }

    struct ast_expr const* expr = parse_nud(parser);
    while (precedence < current_precedence(parser)) {
        parse_led_fn const parse_led =
            token_kind_led(parser->current_token->kind);
        if (parse_led == NULL) {
            return expr;
        }
        expr = parse_led(parser, expr);
    }

    return expr;
}

static struct ast_expr const*
parse_expr(struct parser* parser)
{
    assert(parser != NULL);
    trace(parser->module->path, NO_LINE, "%s", __func__);

    struct ast_expr const* const product =
        parse_expr_precedence(parser, PRECEDENCE_LOWEST);

    return product;
}

static struct ast_expr const*
parse_expr_identifier(struct parser* parser)
{
    assert(parser != NULL);
    trace(parser->module->path, NO_LINE, "%s", __func__);

    struct ast_identifier const* const identifier = parse_identifier(parser);
    struct ast_expr* const product = ast_expr_new_identifier(identifier);

    autil_freezer_register(context()->freezer, product);
    return product;
}

static struct ast_expr const*
parse_expr_boolean(struct parser* parser)
{
    assert(parser != NULL);
    trace(parser->module->path, NO_LINE, "%s", __func__);

    struct ast_boolean const* const boolean = parse_boolean(parser);
    struct ast_expr* const product = ast_expr_new_boolean(boolean);

    autil_freezer_register(context()->freezer, product);
    return product;
}

static struct ast_expr const*
parse_expr_integer(struct parser* parser)
{
    assert(parser != NULL);
    trace(parser->module->path, NO_LINE, "%s", __func__);

    struct ast_integer const* const integer = parse_integer(parser);
    struct ast_expr* const product = ast_expr_new_integer(integer);

    autil_freezer_register(context()->freezer, product);
    return product;
}

static struct ast_expr const*
parse_expr_lparen(struct parser* parser)
{
    assert(parser != NULL);
    trace(parser->module->path, NO_LINE, "%s", __func__);

    struct source_location const* const location =
        &expect_current(parser, TOKEN_LPAREN)->location;

    if (check_current(parser, TOKEN_COLON)) {
        // <expr-array>
        expect_current(parser, TOKEN_COLON);
        struct ast_typespec const* const typespec = parse_typespec(parser);
        expect_current(parser, TOKEN_RPAREN);

        expect_current(parser, TOKEN_LBRACKET);
        autil_sbuf(struct ast_expr const*) elements = NULL;
        while (!check_current(parser, TOKEN_RBRACKET)) {
            if (autil_sbuf_count(elements) != 0u) {
                expect_current(parser, TOKEN_COMMA);
            }
            autil_sbuf_push(elements, parse_expr(parser));
        }
        autil_sbuf_freeze(elements, context()->freezer);
        expect_current(parser, TOKEN_RBRACKET);

        struct ast_expr* const product =
            ast_expr_new_array(location, typespec, elements);

        autil_freezer_register(context()->freezer, product);
        return product;
    }

    // <expr-grouped>
    struct ast_expr const* const expr = parse_expr(parser);
    expect_current(parser, TOKEN_RPAREN);
    struct ast_expr* const product = ast_expr_new_grouped(location, expr);

    autil_freezer_register(context()->freezer, product);
    return product;
}

static struct ast_expr const*
parse_expr_syscall(struct parser* parser)
{
    assert(parser != NULL);
    trace(parser->module->path, NO_LINE, "%s", __func__);

    struct source_location const* const location =
        &expect_current(parser, TOKEN_SYSCALL)->location;
    expect_current(parser, TOKEN_LPAREN);
    autil_sbuf(struct ast_expr const*) exprs = NULL;
    autil_sbuf_push(exprs, parse_expr(parser));
    while (!check_current(parser, TOKEN_RPAREN)) {
        expect_current(parser, TOKEN_COMMA);
        autil_sbuf_push(exprs, parse_expr(parser));
    }
    autil_sbuf_freeze(exprs, context()->freezer);
    expect_current(parser, TOKEN_RPAREN);

    struct ast_expr* const product = ast_expr_new_syscall(location, exprs);

    autil_freezer_register(context()->freezer, product);
    return product;
}

static struct ast_expr const*
parse_expr_led_call(struct parser* parser, struct ast_expr const* lhs)
{
    assert(parser != NULL);
    assert(lhs != NULL);
    trace(parser->module->path, NO_LINE, "%s", __func__);

    expect_current(parser, TOKEN_LPAREN);
    autil_sbuf(struct ast_expr const*) args = NULL;
    while (!check_current(parser, TOKEN_RPAREN)) {
        if (autil_sbuf_count(args) != 0) {
            expect_current(parser, TOKEN_COMMA);
        }
        autil_sbuf_push(args, parse_expr(parser));
    }
    autil_sbuf_freeze(args, context()->freezer);
    expect_current(parser, TOKEN_RPAREN);
    struct ast_expr* const product = ast_expr_new_call(lhs, args);

    autil_freezer_register(context()->freezer, product);
    return product;
}

static struct ast_expr const*
parse_expr_led_index(struct parser* parser, struct ast_expr const* lhs)
{
    assert(parser != NULL);
    assert(lhs != NULL);
    trace(parser->module->path, NO_LINE, "%s", __func__);

    struct source_location const* const location =
        &expect_current(parser, TOKEN_LBRACKET)->location;
    struct ast_expr const* const idx = parse_expr(parser);
    expect_current(parser, TOKEN_RBRACKET);
    struct ast_expr* const product = ast_expr_new_index(location, lhs, idx);

    autil_freezer_register(context()->freezer, product);
    return product;
}

static struct ast_expr const*
parse_expr_nud_unary(struct parser* parser)
{
    assert(parser != NULL);
    trace(parser->module->path, NO_LINE, "%s", __func__);

    struct token const* const op = advance_token(parser);
    struct ast_expr const* const rhs =
        parse_expr_precedence(parser, PRECEDENCE_PREFIX);
    struct ast_expr* const product = ast_expr_new_unary(op, rhs);

    autil_freezer_register(context()->freezer, product);
    return product;
}

static struct ast_expr const*
parse_expr_led_binary(struct parser* parser, struct ast_expr const* lhs)
{
    assert(parser != NULL);
    assert(lhs != NULL);
    trace(parser->module->path, NO_LINE, "%s", __func__);

    struct token const* const op = advance_token(parser);
    struct ast_expr const* const rhs =
        parse_expr_precedence(parser, token_kind_precedence(op->kind));
    struct ast_expr* const product = ast_expr_new_binary(op, lhs, rhs);

    autil_freezer_register(context()->freezer, product);
    return product;
}

static struct ast_block const*
parse_block(struct parser* parser)
{
    assert(parser != NULL);
    trace(parser->module->path, NO_LINE, "%s", __func__);

    struct source_location const* const location =
        &expect_current(parser, TOKEN_LBRACE)->location;

    autil_sbuf(struct ast_stmt const*) stmts = NULL;
    while (!check_current(parser, TOKEN_RBRACE)) {
        autil_sbuf_push(stmts, parse_stmt(parser));
    }
    autil_sbuf_freeze(stmts, context()->freezer);
    expect_current(parser, TOKEN_RBRACE);

    struct ast_block* const product = ast_block_new(location, stmts);

    autil_freezer_register(context()->freezer, product);
    return product;
}

static struct ast_parameter const* const*
parse_parameter_list(struct parser* parser)
{
    assert(parser != NULL);
    trace(parser->module->path, NO_LINE, "%s", __func__);

    autil_sbuf(struct ast_parameter const*) parameters = NULL;
    if (!check_current(parser, TOKEN_IDENTIFIER)) {
        return parameters;
    }

    autil_sbuf_push(parameters, parse_parameter(parser));
    while (check_current(parser, TOKEN_COMMA)) {
        advance_token(parser);
        autil_sbuf_push(parameters, parse_parameter(parser));
    }

    autil_sbuf_freeze(parameters, context()->freezer);
    return parameters;
}

static struct ast_parameter const*
parse_parameter(struct parser* parser)
{
    assert(parser != NULL);
    assert(check_current(parser, TOKEN_IDENTIFIER));
    trace(parser->module->path, NO_LINE, "%s", __func__);

    struct ast_identifier const* identifier = parse_identifier(parser);
    expect_current(parser, TOKEN_COLON);
    struct ast_typespec const* typespec = parse_typespec(parser);
    struct ast_parameter* const product =
        ast_parameter_new(identifier, typespec);

    autil_freezer_register(context()->freezer, product);
    return product;
}

static struct ast_typespec const*
parse_typespec(struct parser* parser)
{
    assert(parser != NULL);
    trace(parser->module->path, NO_LINE, "%s", __func__);

    if (check_current(parser, TOKEN_IDENTIFIER)) {
        return parse_typespec_identifier(parser);
    }

    if (check_current(parser, TOKEN_FUNC)) {
        return parse_typespec_function(parser);
    }

    if (check_current(parser, TOKEN_STAR)) {
        return parse_typespec_pointer(parser);
    }

    if (check_current(parser, TOKEN_LBRACKET)) {
        return parse_typespec_array(parser);
    }

    fatal(
        parser->current_token->location.path,
        parser->current_token->location.line,
        "expected type specifier, found `%s`",
        token_to_new_cstr(parser->current_token));
    return NULL;
}

static struct ast_typespec const*
parse_typespec_identifier(struct parser* parser)
{
    assert(parser != NULL);
    assert(check_current(parser, TOKEN_IDENTIFIER));
    trace(parser->module->path, NO_LINE, "%s", __func__);

    struct ast_identifier const* identifier = parse_identifier(parser);
    struct ast_typespec* const product =
        ast_typespec_new_identifier(identifier);

    autil_freezer_register(context()->freezer, product);
    return product;
}

static struct ast_typespec const*
parse_typespec_function(struct parser* parser)
{
    assert(parser != NULL);
    assert(check_current(parser, TOKEN_FUNC));
    trace(parser->module->path, NO_LINE, "%s", __func__);

    struct source_location const* const location =
        &expect_current(parser, TOKEN_FUNC)->location;

    expect_current(parser, TOKEN_LPAREN);
    autil_sbuf(struct ast_typespec const*) parameter_typespecs = NULL;
    if (!check_current(parser, TOKEN_RPAREN)) {
        autil_sbuf_push(parameter_typespecs, parse_typespec(parser));
        while (check_current(parser, TOKEN_COMMA)) {
            advance_token(parser);
            autil_sbuf_push(parameter_typespecs, parse_typespec(parser));
        }
    }
    expect_current(parser, TOKEN_RPAREN);
    autil_sbuf_freeze(parameter_typespecs, context()->freezer);

    struct ast_typespec const* const return_typespec = parse_typespec(parser);

    struct ast_typespec* const product = ast_typespec_new_function(
        location, parameter_typespecs, return_typespec);

    autil_freezer_register(context()->freezer, product);
    return product;
}

static struct ast_typespec const*
parse_typespec_pointer(struct parser* parser)
{
    assert(parser != NULL);
    assert(check_current(parser, TOKEN_STAR));
    trace(parser->module->path, NO_LINE, "%s", __func__);

    struct source_location const* const location =
        &expect_current(parser, TOKEN_STAR)->location;

    struct ast_typespec const* const base = parse_typespec(parser);

    struct ast_typespec* const product =
        ast_typespec_new_pointer(location, base);

    autil_freezer_register(context()->freezer, product);
    return product;
}

static struct ast_typespec const*
parse_typespec_array(struct parser* parser)
{
    assert(parser != NULL);
    assert(check_current(parser, TOKEN_LBRACKET));
    trace(parser->module->path, NO_LINE, "%s", __func__);

    struct source_location const* const location =
        &expect_current(parser, TOKEN_LBRACKET)->location;
    struct ast_expr const* const count = parse_expr(parser);
    expect_current(parser, TOKEN_RBRACKET);
    struct ast_typespec const* const base = parse_typespec(parser);

    struct ast_typespec* const product =
        ast_typespec_new_array(location, count, base);

    autil_freezer_register(context()->freezer, product);
    return product;
}

static struct ast_identifier const*
parse_identifier(struct parser* parser)
{
    assert(parser != NULL);
    trace(parser->module->path, NO_LINE, "%s", __func__);

    struct token const* const token = expect_current(parser, TOKEN_IDENTIFIER);
    struct source_location const* location = &token->location;
    char const* const name =
        autil_sipool_intern(context()->sipool, token->start, token->count);
    struct ast_identifier* const product = ast_identifier_new(location, name);

    autil_freezer_register(context()->freezer, product);
    return product;
}

static struct ast_boolean const*
parse_boolean(struct parser* parser)
{
    assert(parser != NULL);
    trace(parser->module->path, NO_LINE, "%s", __func__);

    struct token const* const token = advance_token(parser);
    assert(token->kind == TOKEN_TRUE || token->kind == TOKEN_FALSE);
    struct source_location const* const location = &token->location;
    bool const value = token->kind == TOKEN_TRUE;
    struct ast_boolean* const product = ast_boolean_new(location, value);

    autil_freezer_register(context()->freezer, product);
    return product;
}

static struct ast_integer const*
parse_integer(struct parser* parser)
{
    assert(parser != NULL);
    trace(parser->module->path, NO_LINE, "%s", __func__);

    struct token const* const token = expect_current(parser, TOKEN_INTEGER);
    struct source_location const* const location = &token->location;
    struct autil_bigint* const value = autil_bigint_new_text(
        token->data.integer.number.start, token->data.integer.number.count);
    autil_bigint_freeze(value, context()->freezer);
    char const* const suffix = autil_sipool_intern(
        context()->sipool,
        token->data.integer.suffix.start,
        token->data.integer.suffix.count);
    struct ast_integer* const product =
        ast_integer_new(location, value, suffix);

    autil_freezer_register(context()->freezer, product);
    return product;
}

void
parse(struct module* module)
{
    assert(module != NULL);
    assert(module->ast == NULL);

    struct lexer* const lexer = lexer_new(module);
    struct parser* const parser = parser_new(module, lexer);

    module->ast = parse_module(parser);

    lexer_del(lexer);
    parser_del(parser);
}
