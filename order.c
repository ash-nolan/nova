// Copyright 2021 The Nova Project Authors
// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "nova.h"

// Top-Level Declaration
struct tldecl {
    enum {
        TLDECL_UNORDERED,
        TLDECL_ORDERING,
        TLDECL_ORDERED,
    } state;
    struct ast_decl const* decl;
};

struct orderer {
    struct module* module;
    struct {
        // Mapping from each top-level declaration name to its corresponding
        // tldecl structure. Initialized & populated in orderer_new.
#define TLDECL_MAP_KEY_TYPE char const*
#define TLDECL_MAP_VAL_TYPE struct tldecl
#define TLDECL_MAP_CMP_FUNC autil_cstr_vpcmp
        struct autil_map* map; // Populated in orderer_new.
        // Names of each top-level declaration in the order seen by the parser.
        // Initialized & populated in orderer_new.
        autil_sbuf(char const*) declaration_order;
        // Names of each top-level declaration, topologically sorted such that
        // any declaration with index k does not depend on any declaration with
        // index k+n for all n. Initialized as an empty sbuf in orderer_new and
        // populated during the order phase.
        autil_sbuf(char const*) topological_order;
    } tldecls;
};
static struct orderer*
orderer_new(struct module* module);
static void
orderer_del(struct orderer* self);
static void
orderer_tldecl_insert(struct orderer* orderer, struct ast_decl const* decl);
static struct tldecl*
orderer_tldecl_lookup(struct orderer* orderer, char const* name);

static void
order_tldecl(struct orderer* orderer, struct tldecl* tldecl);
static void
order_decl(struct orderer* orderer, struct ast_decl const* decl);
static void
order_expr(struct orderer* orderer, struct ast_expr const* expr);
static void
order_parameter(struct orderer* orderer, struct ast_parameter const* parameter);
static void
order_typespec(struct orderer* orderer, struct ast_typespec const* typespec);
static void
order_identifier(
    struct orderer* orderer, struct ast_identifier const* identifier);
static void
order_name(struct orderer* orderer, char const* name);

static struct orderer*
orderer_new(struct module* module)
{
    assert(module != NULL);

    struct orderer* const self = autil_xalloc(NULL, sizeof(*self));
    memset(self, 0x00, sizeof(*self));
    self->module = module;
    self->tldecls.map = autil_map_new(
        sizeof(TLDECL_MAP_KEY_TYPE),
        sizeof(TLDECL_MAP_VAL_TYPE),
        TLDECL_MAP_CMP_FUNC);
    for (size_t i = 0; i < autil_sbuf_count(module->ast->decls); ++i) {
        orderer_tldecl_insert(self, module->ast->decls[i]);
    }
    return self;
}

static void
orderer_del(struct orderer* self)
{
    assert(self != NULL);

    autil_map_del(self->tldecls.map);
    autil_sbuf_fini(self->tldecls.declaration_order);
    autil_sbuf_fini(self->tldecls.topological_order);

    memset(self, 0x00, sizeof(*self));
    autil_xalloc(self, AUTIL_XALLOC_FREE);
}

static void
orderer_tldecl_insert(struct orderer* orderer, struct ast_decl const* decl)
{
    assert(orderer != NULL);
    assert(decl != NULL);

    struct tldecl const tldecl = {TLDECL_UNORDERED, decl};

    autil_sbuf_push(orderer->tldecls.declaration_order, decl->name);
    struct tldecl const* existing =
        autil_map_lookup_const(orderer->tldecls.map, &decl->name);
    if (existing != NULL) {
        fatal(
            decl->location->path,
            decl->location->line,
            "redeclaration of `%s` previously declared at [%s:%zu]",
            existing->decl->name,
            existing->decl->location->path,
            existing->decl->location->line);
    }

    autil_map_insert(orderer->tldecls.map, &decl->name, &tldecl, NULL, NULL);
}

static struct tldecl*
orderer_tldecl_lookup(struct orderer* orderer, char const* name)
{
    assert(orderer != NULL);
    assert(name != NULL);

    return autil_map_lookup(orderer->tldecls.map, &name);
}

static void
order_tldecl(struct orderer* orderer, struct tldecl* tldecl)
{
    assert(orderer != NULL);
    assert(tldecl != NULL);

    if (tldecl->state == TLDECL_ORDERED) {
        // Top-level declaration is already ordered.
        return;
    }
    if (tldecl->state == TLDECL_ORDERING) {
        // Top-level declaration is currently in the process of being ordered.
        fatal(
            tldecl->decl->location->path,
            tldecl->decl->location->line,
            "circular dependency created by declaration of `%s`",
            tldecl->decl->name);
    }

    trace(
        orderer->module->path,
        NO_LINE,
        "[%s] ordering unordered top-level declaration `%s`",
        __func__,
        tldecl->decl->name);
    // Change the state from UNORDERED to ORDERING so that cyclic dependencies
    // will be detected if another attempt is made to order this declaration.
    assert(tldecl->state == TLDECL_UNORDERED);
    tldecl->state = TLDECL_ORDERING;

    order_decl(orderer, tldecl->decl);
    tldecl->state = TLDECL_ORDERED;
    autil_sbuf_push(orderer->tldecls.topological_order, tldecl->decl->name);
}

static void
order_decl(struct orderer* orderer, struct ast_decl const* decl)
{
    switch (decl->kind) {
    case AST_DECL_VARIABLE: {
        order_typespec(orderer, decl->data.variable.typespec);
        order_expr(orderer, decl->data.variable.expr);
        return;
    }
    case AST_DECL_CONSTANT: {
        order_typespec(orderer, decl->data.constant.typespec);
        order_expr(orderer, decl->data.constant.expr);
        return;
    }
    case AST_DECL_FUNCTION: {
        struct ast_parameter const* const* const parameters =
            decl->data.function.parameters;
        for (size_t i = 0; i < autil_sbuf_count(parameters); ++i) {
            order_parameter(orderer, parameters[i]);
        }
        order_typespec(orderer, decl->data.function.return_typespec);
        return;
    }
    }

    UNREACHABLE();
}

static void
order_expr(struct orderer* orderer, struct ast_expr const* expr)
{
    assert(orderer != NULL);
    assert(expr != NULL);

    switch (expr->kind) {
    case AST_EXPR_IDENTIFIER: {
        order_identifier(orderer, expr->data.identifier);
        return;
    }
    case AST_EXPR_BOOLEAN: /* fallthrough */
    case AST_EXPR_INTEGER:
        return;
    case AST_EXPR_ARRAY: {
        autil_sbuf(struct ast_expr const* const) const elements =
            expr->data.array.elements;
        for (size_t i = 0; i < autil_sbuf_count(elements); ++i) {
            order_expr(orderer, elements[i]);
        }
        return;
    }
    case AST_EXPR_GROUPED: {
        order_expr(orderer, expr->data.grouped.expr);
        return;
    }
    case AST_EXPR_SYSCALL: {
        autil_sbuf(struct ast_expr const* const) const arguments =
            expr->data.syscall.arguments;
        for (size_t i = 0; i < autil_sbuf_count(arguments); ++i) {
            order_expr(orderer, arguments[i]);
        }
        return;
    }
    case AST_EXPR_CALL: {
        autil_sbuf(struct ast_expr const* const) const arguments =
            expr->data.call.arguments;
        for (size_t i = 0; i < autil_sbuf_count(arguments); ++i) {
            order_expr(orderer, arguments[i]);
        }
        return;
    }
    case AST_EXPR_INDEX: {
        order_expr(orderer, expr->data.index.lhs);
        order_expr(orderer, expr->data.index.idx);
    }
    case AST_EXPR_UNARY: {
        order_expr(orderer, expr->data.unary.rhs);
        return;
    }
    case AST_EXPR_BINARY: {
        order_expr(orderer, expr->data.binary.lhs);
        order_expr(orderer, expr->data.binary.rhs);
        return;
    }
    }

    UNREACHABLE();
}

static void
order_parameter(struct orderer* orderer, struct ast_parameter const* parameter)
{
    assert(orderer != NULL);
    assert(parameter != NULL);

    order_typespec(orderer, parameter->typespec);
}

static void
order_typespec(struct orderer* orderer, struct ast_typespec const* typespec)
{
    assert(orderer != NULL);
    assert(typespec != NULL);

    switch (typespec->kind) {
    case TYPESPEC_IDENTIFIER: {
        order_identifier(orderer, typespec->data.identifier);
        return;
    }
    case TYPESPEC_FUNCTION: {
        autil_sbuf(struct ast_typespec const* const) const parameter_typespecs =
            typespec->data.function.parameter_typespecs;
        for (size_t i = 0; i < autil_sbuf_count(parameter_typespecs); ++i) {
            order_typespec(orderer, parameter_typespecs[i]);
        }

        order_typespec(orderer, typespec->data.function.return_typespec);
        return;
    }
    case TYPESPEC_POINTER: {
        order_typespec(orderer, typespec->data.pointer.base);
        return;
    }
    case TYPESPEC_ARRAY: {
        order_expr(orderer, typespec->data.array.count);
        order_typespec(orderer, typespec->data.array.base);
        return;
    }
    }

    UNREACHABLE();
}

static void
order_identifier(
    struct orderer* orderer, struct ast_identifier const* identifier)
{
    assert(orderer != NULL);
    assert(identifier != NULL);

    order_name(orderer, identifier->name);
}

static void
order_name(struct orderer* orderer, char const* name)
{
    assert(orderer != NULL);
    assert(name != NULL);

    struct tldecl* const tldecl = orderer_tldecl_lookup(orderer, name);
    if (tldecl == NULL) {
        // Top-level declaration with the provided name does not exist. Assume
        // that it is a builtin and allow future stages of semantic analysis to
        // raise an unknown identifier error if necessary.
        trace(
            orderer->module->path,
            NO_LINE,
            "[%s] skipping unknown identifier `%s`",
            __func__,
            name);
        return;
    }

    order_tldecl(orderer, tldecl);
}

void
order(struct module* module)
{
    assert(module != NULL);

    struct orderer* const orderer = orderer_new(module);
    size_t const decl_count = autil_map_count(orderer->tldecls.map);

    assert(decl_count == autil_sbuf_count(orderer->tldecls.declaration_order));
    debug(orderer->module->path, NO_LINE, "[%s] Declaration order:", __func__);
    for (size_t i = 0; i < decl_count; ++i) {
        char const* const name = orderer->tldecls.declaration_order[i];
        debug(
            orderer->module->path, NO_LINE, "[%s] (%zu) %s", __func__, i, name);
    }

    for (size_t i = 0; i < decl_count; ++i) {
        char const* const name = orderer->tldecls.declaration_order[i];
        order_tldecl(orderer, orderer_tldecl_lookup(orderer, name));
    }

    assert(decl_count == autil_sbuf_count(orderer->tldecls.topological_order));
    debug(orderer->module->path, NO_LINE, "[%s] Topological order:", __func__);
    for (size_t i = 0; i < decl_count; ++i) {
        char const* const name = orderer->tldecls.topological_order[i];
        debug(
            orderer->module->path, NO_LINE, "[%s] (%zu) %s", __func__, i, name);
        struct ast_decl const* const decl =
            orderer_tldecl_lookup(orderer, name)->decl;
        autil_sbuf_push(module->ordered, decl);
    }

    orderer_del(orderer);
}
