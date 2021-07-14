// Copyright 2021 The Nova Project Authors
// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <string.h>
#include "nova.h"

struct ast_module*
ast_module_new(struct ast_decl const* const* decls)
{
    struct ast_module* const self = autil_xalloc(NULL, sizeof(*self));
    memset(self, 0x00, sizeof(*self));
    self->decls = decls;
    return self;
}

struct ast_decl*
ast_decl_new_variable(
    struct source_location const* location,
    struct ast_identifier const* identifier,
    struct ast_typespec const* typespec,
    struct ast_expr const* expr)
{
    assert(location != NULL);
    assert(identifier != NULL);
    assert(typespec != NULL);
    assert(expr != NULL);

    struct ast_decl* const self = autil_xalloc(NULL, sizeof(*self));
    memset(self, 0x00, sizeof(*self));
    self->kind = AST_DECL_VARIABLE;
    self->location = location;
    self->name = identifier->name;
    self->data.variable.identifier = identifier;
    self->data.variable.typespec = typespec;
    self->data.variable.expr = expr;
    return self;
}

struct ast_decl*
ast_decl_new_constant(
    struct source_location const* location,
    struct ast_identifier const* identifier,
    struct ast_typespec const* typespec,
    struct ast_expr const* expr)
{
    assert(location != NULL);
    assert(identifier != NULL);
    assert(typespec != NULL);
    assert(expr != NULL);

    struct ast_decl* const self = autil_xalloc(NULL, sizeof(*self));
    memset(self, 0x00, sizeof(*self));
    self->kind = AST_DECL_CONSTANT;
    self->location = location;
    self->name = identifier->name;
    self->data.constant.identifier = identifier;
    self->data.constant.typespec = typespec;
    self->data.constant.expr = expr;
    return self;
}

struct ast_decl*
ast_decl_new_func(
    struct source_location const* location,
    struct ast_identifier const* identifier,
    struct ast_parameter const* const* parameters,
    struct ast_typespec const* return_typespec,
    struct ast_block const* body)
{
    assert(location != NULL);
    assert(identifier != NULL);
    assert(return_typespec != NULL);
    assert(body != NULL);

    struct ast_decl* const self = autil_xalloc(NULL, sizeof(*self));
    memset(self, 0x00, sizeof(*self));
    self->kind = AST_DECL_FUNCTION;
    self->location = location;
    self->name = identifier->name;
    self->data.function.identifier = identifier;
    self->data.function.parameters = parameters;
    self->data.function.return_typespec = return_typespec;
    self->data.function.body = body;
    return self;
}

static struct ast_stmt*
ast_stmt_new(struct source_location const* location, enum ast_stmt_kind kind)
{
    struct ast_stmt* const self = autil_xalloc(NULL, sizeof(*self));
    memset(self, 0x00, sizeof(*self));
    self->location = location;
    self->kind = kind;
    return self;
}

struct ast_stmt*
ast_stmt_new_if(struct ast_conditional const* const* conditionals)
{
    assert(autil_sbuf_count(conditionals) > 0u);

    struct source_location const* const location = conditionals[0]->location;
    struct ast_stmt* const self = ast_stmt_new(location, AST_STMT_IF);
    self->data.if_.conditionals = conditionals;
    return self;
}

struct ast_stmt*
ast_stmt_new_for_range(
    struct source_location const* location,
    struct ast_identifier const* identifier,
    struct ast_expr const* begin,
    struct ast_expr const* end,
    struct ast_block const* body)
{
    assert(location != NULL);
    assert(identifier != NULL);
    assert(begin != NULL);
    assert(end != NULL);
    assert(body != NULL);

    struct ast_stmt* const self = ast_stmt_new(location, AST_STMT_FOR_RANGE);
    self->data.for_range.identifier = identifier;
    self->data.for_range.begin = begin;
    self->data.for_range.end = end;
    self->data.for_range.body = body;
    return self;
}

struct ast_stmt*
ast_stmt_new_for_expr(
    struct source_location const* location,
    struct ast_expr const* expr,
    struct ast_block const* body)
{
    assert(location != NULL);
    assert(expr != NULL);
    assert(body != NULL);

    struct ast_stmt* const self = ast_stmt_new(location, AST_STMT_FOR_EXPR);
    self->data.for_expr.expr = expr;
    self->data.for_expr.body = body;
    return self;
}

struct ast_stmt*
ast_stmt_new_decl(struct ast_decl const* decl)
{
    assert(decl != NULL);

    struct ast_stmt* const self = ast_stmt_new(decl->location, AST_STMT_DECL);
    self->data.decl = decl;
    return self;
}

struct ast_stmt*
ast_stmt_new_dump(
    struct source_location const* location, struct ast_expr const* expr)
{
    assert(location != NULL);
    assert(expr != NULL);

    struct ast_stmt* const self = ast_stmt_new(location, AST_STMT_DUMP);
    self->data.dump.expr = expr;
    return self;
}

struct ast_stmt*
ast_stmt_new_return(
    struct source_location const* location, struct ast_expr const* expr)
{
    assert(location != NULL);

    struct ast_stmt* const self = ast_stmt_new(location, AST_STMT_RETURN);
    self->data.return_.expr = expr;
    return self;
}

struct ast_stmt*
ast_stmt_new_assign(
    struct source_location const* location,
    struct ast_expr const* lhs,
    struct ast_expr const* rhs)
{
    assert(location != NULL);
    assert(lhs != NULL);
    assert(rhs != NULL);

    struct ast_stmt* const self = ast_stmt_new(location, AST_STMT_ASSIGN);
    self->data.assign.lhs = lhs;
    self->data.assign.rhs = rhs;
    return self;
}

struct ast_stmt*
ast_stmt_new_expr(struct ast_expr const* expr)
{
    assert(expr != NULL);

    struct ast_stmt* const self = ast_stmt_new(expr->location, AST_STMT_EXPR);
    self->data.expr = expr;
    return self;
}

static struct ast_expr*
ast_expr_new(struct source_location const* location, enum ast_expr_kind kind)
{
    struct ast_expr* const self = autil_xalloc(NULL, sizeof(*self));
    memset(self, 0x00, sizeof(*self));
    self->location = location;
    self->kind = kind;
    return self;
}

struct ast_expr*
ast_expr_new_identifier(struct ast_identifier const* identifier)
{
    assert(identifier != NULL);

    struct ast_expr* const self =
        ast_expr_new(identifier->location, AST_EXPR_IDENTIFIER);
    self->data.identifier = identifier;
    return self;
}

struct ast_expr*
ast_expr_new_boolean(struct ast_boolean const* boolean)
{
    assert(boolean != NULL);

    struct ast_expr* const self =
        ast_expr_new(boolean->location, AST_EXPR_BOOLEAN);
    self->data.boolean = boolean;
    return self;
}

struct ast_expr*
ast_expr_new_integer(struct ast_integer const* integer)
{
    assert(integer != NULL);

    struct ast_expr* const self =
        ast_expr_new(integer->location, AST_EXPR_INTEGER);
    self->data.integer = integer;
    return self;
}

struct ast_expr*
ast_expr_new_array(
    struct source_location const* location,
    struct ast_typespec const* typespec,
    struct ast_expr const* const* elements)
{
    struct ast_expr* const self = ast_expr_new(location, AST_EXPR_ARRAY);
    self->data.array.typespec = typespec;
    self->data.array.elements = elements;
    return self;
}

struct ast_expr*
ast_expr_new_grouped(
    struct source_location const* location, struct ast_expr const* expr)
{
    assert(expr != NULL);

    struct ast_expr* const self = ast_expr_new(location, AST_EXPR_GROUPED);
    self->data.grouped.expr = expr;
    return self;
}

struct ast_expr*
ast_expr_new_syscall(
    struct source_location const* location,
    struct ast_expr const* const* arguments)
{
    struct ast_expr* const self = ast_expr_new(location, AST_EXPR_SYSCALL);
    self->data.syscall.arguments = arguments;
    return self;
}

struct ast_expr*
ast_expr_new_call(
    struct ast_expr const* func, struct ast_expr const* const* arguments)
{
    assert(func != NULL);

    struct ast_expr* const self = ast_expr_new(func->location, AST_EXPR_CALL);
    self->data.call.func = func;
    self->data.call.arguments = arguments;
    return self;
}

struct ast_expr*
ast_expr_new_index(
    struct source_location const* location,
    struct ast_expr const* lhs,
    struct ast_expr const* idx)
{
    assert(location != NULL);
    assert(lhs != NULL);
    assert(idx != NULL);

    struct ast_expr* const self = ast_expr_new(location, AST_EXPR_INDEX);
    self->data.index.lhs = lhs;
    self->data.index.idx = idx;
    return self;
}

struct ast_expr*
ast_expr_new_unary(struct token const* op, struct ast_expr const* rhs)
{
    assert(op != NULL);
    assert(rhs != NULL);

    struct ast_expr* const self = ast_expr_new(&op->location, AST_EXPR_UNARY);
    self->data.unary.op = op;
    self->data.unary.rhs = rhs;
    return self;
}

struct ast_expr*
ast_expr_new_binary(
    struct token const* op,
    struct ast_expr const* lhs,
    struct ast_expr const* rhs)
{
    assert(op != NULL);
    assert(lhs != NULL);
    assert(rhs != NULL);

    struct ast_expr* const self = ast_expr_new(&op->location, AST_EXPR_BINARY);
    self->data.binary.op = op;
    self->data.binary.lhs = lhs;
    self->data.binary.rhs = rhs;
    return self;
}

struct ast_conditional*
ast_conditional_new(
    struct source_location const* location,
    struct ast_expr const* condition,
    struct ast_block const* body)
{
    assert(location != NULL);
    assert(body != NULL);

    struct ast_conditional* const self = autil_xalloc(NULL, sizeof(*self));
    memset(self, 0x00, sizeof(*self));
    self->location = location;
    self->condition = condition;
    self->body = body;
    return self;
}

struct ast_block*
ast_block_new(
    struct source_location const* location, struct ast_stmt const* const* stmts)
{
    assert(location != NULL);

    struct ast_block* const self = autil_xalloc(NULL, sizeof(*self));
    memset(self, 0x00, sizeof(*self));
    self->location = location;
    self->stmts = stmts;
    return self;
}

struct ast_parameter*
ast_parameter_new(
    struct ast_identifier const* identifier,
    struct ast_typespec const* typespec)
{
    assert(identifier != NULL);
    assert(typespec != NULL);

    struct ast_parameter* const self = autil_xalloc(NULL, sizeof(*self));
    memset(self, 0x00, sizeof(*self));
    self->location = identifier->location;
    self->identifier = identifier;
    self->typespec = typespec;
    return self;
}

static struct ast_typespec*
ast_typespec_new(
    struct source_location const* location, enum typespec_kind kind)
{
    assert(location != NULL);

    struct ast_typespec* const self = autil_xalloc(NULL, sizeof(*self));
    memset(self, 0x00, sizeof(*self));
    self->location = location;
    self->kind = kind;
    return self;
}

struct ast_typespec*
ast_typespec_new_identifier(struct ast_identifier const* identifier)
{
    assert(identifier != NULL);

    struct ast_typespec* const self =
        ast_typespec_new(identifier->location, TYPESPEC_IDENTIFIER);
    self->data.identifier = identifier;
    return self;
}

struct ast_typespec*
ast_typespec_new_function(
    struct source_location const* location,
    struct ast_typespec const* const* parameter_typespecs,
    struct ast_typespec const* return_typespec)
{
    assert(location != NULL);
    assert(return_typespec != NULL);

    struct ast_typespec* const self =
        ast_typespec_new(location, TYPESPEC_FUNCTION);
    self->data.function.parameter_typespecs = parameter_typespecs;
    self->data.function.return_typespec = return_typespec;
    return self;
}

struct ast_typespec*
ast_typespec_new_pointer(
    struct source_location const* location, struct ast_typespec const* base)
{
    assert(location != NULL);
    assert(base != NULL);

    struct ast_typespec* const self =
        ast_typespec_new(location, TYPESPEC_POINTER);
    self->data.pointer.base = base;
    return self;
}

struct ast_typespec*
ast_typespec_new_array(
    struct source_location const* location,
    struct ast_expr const* count,
    struct ast_typespec const* base)
{
    assert(location != NULL);
    assert(count != NULL);
    assert(base != NULL);

    struct ast_typespec* const self =
        ast_typespec_new(location, TYPESPEC_ARRAY);
    self->data.array.count = count;
    self->data.array.base = base;
    return self;
}

struct ast_identifier*
ast_identifier_new(struct source_location const* location, char const* name)
{
    assert(location != NULL);
    assert(name != NULL);

    struct ast_identifier* const self = autil_xalloc(NULL, sizeof(*self));
    memset(self, 0x00, sizeof(*self));
    self->location = location;
    self->name = name;
    return self;
}

struct ast_boolean*
ast_boolean_new(struct source_location const* location, bool value)
{
    assert(location != NULL);

    struct ast_boolean* const self = autil_xalloc(NULL, sizeof(*self));
    memset(self, 0x00, sizeof(*self));
    self->location = location;
    self->value = value;
    return self;
}

struct ast_integer*
ast_integer_new(
    struct source_location const* location,
    struct autil_bigint const* value,
    char const* suffix)
{
    assert(location != NULL);
    assert(suffix != NULL);

    struct ast_integer* const self = autil_xalloc(NULL, sizeof(*self));
    memset(self, 0x00, sizeof(*self));
    self->location = location;
    self->value = value;
    self->suffix = suffix;
    return self;
}
