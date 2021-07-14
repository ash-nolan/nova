// Copyright 2021 The Nova Project Authors
// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "nova.h"

struct resolver {
    struct module* module;
    struct tir_function* current_function; // NULL if not in a function.
    struct symbol_table* current_symbol_table;
    // Current offset of rbp for stack allocated data. Initialized to zero at
    // the start of function resolution.
    int current_rbp_offset;
};
static struct resolver*
resolver_new(struct module* module);
static void
resolver_del(struct resolver* self);
// Returns true if resolution being performed in the global scope.
static bool
resolver_is_global(struct resolver const* self);
// Reserve global or local space for an object of the provided type.
// Returns the address for the object.
static struct address
resolver_reserve_space(
    struct resolver* self, char const* name, struct type const* type);

static struct symbol const*
resolve_decl(struct resolver* resolver, struct ast_decl const* decl);
static struct symbol const*
resolve_decl_variable(
    struct resolver* resolver,
    struct ast_decl const* decl,
    struct tir_expr const** /*optional*/ lhs,
    struct tir_expr const** /*optional*/ rhs);
static struct symbol const*
resolve_decl_constant(struct resolver* resolver, struct ast_decl const* decl);
static struct symbol const*
resolve_decl_function(struct resolver* resolver, struct ast_decl const* decl);

static struct tir_stmt const* // optional
resolve_stmt(struct resolver* resolver, struct ast_stmt const* stmt);
static struct tir_stmt const* // optional
resolve_stmt_decl(struct resolver* resolver, struct ast_stmt const* stmt);
static struct tir_stmt const*
resolve_stmt_if(struct resolver* resolver, struct ast_stmt const* stmt);
static struct tir_stmt const*
resolve_stmt_for_range(struct resolver* resolver, struct ast_stmt const* stmt);
static struct tir_stmt const*
resolve_stmt_for_expr(struct resolver* resolver, struct ast_stmt const* stmt);
static struct tir_stmt const*
resolve_stmt_dump(struct resolver* resolver, struct ast_stmt const* stmt);
static struct tir_stmt const*
resolve_stmt_return(struct resolver* resolver, struct ast_stmt const* stmt);
static struct tir_stmt const*
resolve_stmt_assign(struct resolver* resolver, struct ast_stmt const* stmt);
static struct tir_stmt const*
resolve_stmt_expr(struct resolver* resolver, struct ast_stmt const* stmt);

static struct tir_expr const*
resolve_expr(struct resolver* resolver, struct ast_expr const* expr);
static struct tir_expr const*
resolve_expr_identifier(struct resolver* resolver, struct ast_expr const* expr);
static struct tir_expr const*
resolve_expr_boolean(struct resolver* resolver, struct ast_expr const* expr);
static struct tir_expr const*
resolve_expr_integer(struct resolver* resolver, struct ast_expr const* expr);
static struct tir_expr const*
resolve_expr_array(struct resolver* resolver, struct ast_expr const* expr);
static struct tir_expr const*
resolve_expr_syscall(struct resolver* resolver, struct ast_expr const* expr);
static struct tir_expr const*
resolve_expr_call(struct resolver* resolver, struct ast_expr const* expr);
static struct tir_expr const*
resolve_expr_index(struct resolver* resolver, struct ast_expr const* expr);
static struct tir_expr const*
resolve_expr_unary(struct resolver* resolver, struct ast_expr const* expr);
static struct tir_expr const*
resolve_expr_unary_logical(
    struct resolver* resolver,
    struct token const* op,
    enum uop_kind uop,
    struct tir_expr const* rhs);
static struct tir_expr const*
resolve_expr_unary_arithmetic(
    struct resolver* resolver,
    struct token const* op,
    enum uop_kind uop,
    struct tir_expr const* rhs);
static struct tir_expr const*
resolve_expr_unary_dereference(
    struct resolver* resolver,
    struct token const* op,
    struct tir_expr const* rhs);
static struct tir_expr const*
resolve_expr_unary_addressof(
    struct resolver* resolver,
    struct token const* op,
    struct tir_expr const* rhs);
static struct tir_expr const*
resolve_expr_binary(struct resolver* resolver, struct ast_expr const* expr);
static struct tir_expr const*
resolve_expr_binary_logical(
    struct resolver* resolver,
    struct token const* op,
    enum bop_kind bop,
    struct tir_expr const* lhs,
    struct tir_expr const* rhs);
static struct tir_expr const*
resolve_expr_binary_compare(
    struct resolver* resolver,
    struct token const* op,
    enum bop_kind bop,
    struct tir_expr const* lhs,
    struct tir_expr const* rhs);
static struct tir_expr const*
resolve_expr_binary_arithmetic(
    struct resolver* resolver,
    struct token const* op,
    enum bop_kind bop,
    struct tir_expr const* lhs,
    struct tir_expr const* rhs);

static struct tir_block const*
resolve_block(
    struct resolver* resolver,
    struct symbol_table* symbol_table,
    struct ast_block const* block);

static struct type const*
resolve_typespec(
    struct resolver* resolver, struct ast_typespec const* typespec);

static struct resolver*
resolver_new(struct module* module)
{
    assert(module != NULL);

    struct resolver* const self = autil_xalloc(NULL, sizeof(*self));
    memset(self, 0x00, sizeof(*self));
    self->module = module;
    self->current_function = NULL;
    self->current_symbol_table = context()->global_symbol_table;
    self->current_rbp_offset = 0x0;
    return self;
}

static void
resolver_del(struct resolver* self)
{
    assert(self != NULL);

    memset(self, 0x00, sizeof(*self));
    autil_xalloc(self, AUTIL_XALLOC_FREE);
}

static bool
resolver_is_global(struct resolver const* self)
{
    assert(self != NULL);

    return self->current_function == NULL;
}

static struct address
resolver_reserve_space(
    struct resolver* self, char const* name, struct type const* type)
{
    assert(self != NULL);
    assert(name != NULL);
    assert(type != NULL);

    if (resolver_is_global(self)) {
        return address_init_global(name);
    }

    self->current_rbp_offset -= (int)ceil8z(type->size);
    if (self->current_rbp_offset < self->current_function->local_stack_offset) {
        self->current_function->local_stack_offset = self->current_rbp_offset;
    }
    return address_init_local(self->current_rbp_offset);
}

static struct symbol const*
resolve_decl(struct resolver* resolver, struct ast_decl const* decl)
{
    assert(resolver != NULL);
    assert(decl != NULL);
    trace(resolver->module->path, NO_LINE, "%s", __func__);

    switch (decl->kind) {
    case AST_DECL_VARIABLE: {
        return resolve_decl_variable(resolver, decl, NULL, NULL);
    }
    case AST_DECL_CONSTANT: {
        return resolve_decl_constant(resolver, decl);
    }
    case AST_DECL_FUNCTION: {
        return resolve_decl_function(resolver, decl);
    }
    }

    UNREACHABLE();
}

static struct symbol const*
resolve_decl_variable(
    struct resolver* resolver,
    struct ast_decl const* decl,
    struct tir_expr const** lhs,
    struct tir_expr const** rhs)
{
    assert(resolver != NULL);
    assert(decl != NULL);
    assert(decl->kind == AST_DECL_VARIABLE);

    struct tir_expr const* const expr =
        resolve_expr(resolver, decl->data.variable.expr);

    // Global variables have their initial values computed at compile-time, but
    // local variables have their value calculated/assigned at runtime when the
    // value is placed on the stack.
    bool const is_global = resolver->current_symbol_table->parent == NULL;
    struct value* value = NULL;
    if (is_global) {
        struct evaluator* const evaluator =
            evaluator_new(resolver->current_symbol_table);
        value = eval_expr(evaluator, expr);
        value_freeze(value, context()->freezer);
        evaluator_del(evaluator);
    }

    struct type const* const type =
        resolve_typespec(resolver, decl->data.variable.typespec);
    if (expr->type != type) {
        fatal(
            decl->location->path,
            decl->location->line,
            "illegal type conversion from `%s` to `%s`",
            expr->type->name,
            type->name);
    }

    struct address* const address =
        address_new(resolver_reserve_space(resolver, decl->name, type));
    autil_freezer_register(context()->freezer, address);

    struct symbol* const symbol =
        symbol_new_variable(decl->location, decl->name, type, address, value);
    autil_freezer_register(context()->freezer, symbol);

    symbol_table_insert(resolver->current_symbol_table, symbol);
    if (lhs != NULL) {
        struct tir_expr* const identifier = tir_expr_new_identifier(
            decl->data.variable.identifier->location, symbol);
        autil_freezer_register(context()->freezer, identifier);
        *lhs = identifier;
    }
    if (rhs != NULL) {
        *rhs = expr;
    }
    return symbol;
}

static struct symbol const*
resolve_decl_constant(struct resolver* resolver, struct ast_decl const* decl)
{
    assert(resolver != NULL);
    assert(decl != NULL);
    assert(decl->kind == AST_DECL_CONSTANT);

    struct tir_expr const* const expr =
        resolve_expr(resolver, decl->data.constant.expr);

    // Constants (globals and locals) have their values computed at compile-time
    // and therefore must always be added to the symbol table with an evaluated
    // value.
    struct evaluator* const evaluator =
        evaluator_new(resolver->current_symbol_table);
    struct value* const value = eval_expr(evaluator, expr);
    value_freeze(value, context()->freezer);
    evaluator_del(evaluator);

    struct type const* const type =
        resolve_typespec(resolver, decl->data.constant.typespec);
    if (expr->type != type) {
        fatal(
            decl->location->path,
            decl->location->line,
            "illegal type conversion from `%s` to `%s`",
            expr->type->name,
            type->name);
    }

    struct address* const address =
        address_new(resolver_reserve_space(resolver, decl->name, type));
    autil_freezer_register(context()->freezer, address);

    struct symbol* const symbol =
        symbol_new_constant(decl->location, decl->name, type, address, value);
    autil_freezer_register(context()->freezer, symbol);

    symbol_table_insert(resolver->current_symbol_table, symbol);
    return symbol;
}

static struct symbol const*
resolve_decl_function(struct resolver* resolver, struct ast_decl const* decl)
{
    assert(resolver != NULL);
    assert(decl != NULL);
    assert(decl->kind == AST_DECL_FUNCTION);

    autil_sbuf(struct ast_parameter const* const) const parameters =
        decl->data.function.parameters;

    // Create the type corresponding to the function.
    struct type const** parameter_types = NULL;
    autil_sbuf_resize(parameter_types, autil_sbuf_count(parameters));
    for (size_t i = 0; i < autil_sbuf_count(parameters); ++i) {
        parameter_types[i] =
            resolve_typespec(resolver, parameters[i]->typespec);
    }
    autil_sbuf_freeze(parameter_types, context()->freezer);

    struct type const* return_type =
        resolve_typespec(resolver, decl->data.function.return_typespec);

    struct type const* type =
        type_unique_function(parameter_types, return_type);

    // Create a new incomplete function, a value that evaluates to that
    // function, and the address of that function/value.
    struct tir_function* const function =
        tir_function_new(decl->data.function.identifier->name, type);
    autil_freezer_register(context()->freezer, function);

    struct value* const value = value_new_function(function);
    value_freeze(value, context()->freezer);

    struct address* const address =
        address_new(address_init_global(decl->name));
    autil_freezer_register(context()->freezer, address);

    // Add the function/value to the symbol table now so that recursive
    // functions may reference themselves.
    struct symbol* const function_symbol =
        symbol_new_function(decl->location, decl->name, type, address, value);
    autil_freezer_register(context()->freezer, function_symbol);
    symbol_table_insert(resolver->current_symbol_table, function_symbol);

    // Executing a call instruction pushes the return address (0x8 bytes) onto
    // the stack. Inside the function the prelude saves the previous value of
    // rbp (0x8 bytes) by pushing it on the stack. So in total there are 0x8 +
    // 0x8 = 0x10 bytes between the current rbp (saved from the stack pointer)
    // and the region of the stack containing function parameters.
    // XXX: Currently the compiler assumes 0x8 byte stack alignment and does
    // *NOT* pad the stack to be 0x10 byte-aligned as required by some ABIs.
    int rbp_offset = 0x10; // Saved rbp + return address.
    // Resolve the function's parameters in order from lowest->highest on the
    // stack (i.e. right to left), adjusting the rbp_offset for each parameter
    // along the way.
    autil_sbuf(struct symbol const*) symbol_parameters = NULL;
    autil_sbuf_resize(symbol_parameters, autil_sbuf_count(parameters));
    for (size_t i = autil_sbuf_count(parameters); i--;) {
        struct source_location const* const location = parameters[i]->location;
        char const* const name = parameters[i]->identifier->name;
        struct type const* const type =
            resolve_typespec(resolver, parameters[i]->typespec);
        struct address* const address =
            address_new(address_init_local(rbp_offset));
        autil_freezer_register(context()->freezer, address);

        rbp_offset += (int)ceil8z(type->size);
        struct symbol* const symbol =
            symbol_new_variable(location, name, type, address, NULL);
        autil_freezer_register(context()->freezer, symbol);

        symbol_parameters[i] = symbol;
    }
    autil_sbuf_freeze(symbol_parameters, context()->freezer);
    function->symbol_parameters = symbol_parameters;

    // Add the function's parameters to its outermost symbol table in order
    // from left to right so that any error message about duplicate parameter
    // symbols will list the left-most symbol as the first of the two symbols
    // added to the table.
    struct symbol_table* const symbol_table =
        symbol_table_new(context()->global_symbol_table);
    // The function references, but does not own, its outermost symbol table.
    function->symbol_table = symbol_table;
    for (size_t i = 0; i < autil_sbuf_count(parameters); ++i) {
        symbol_table_insert(symbol_table, function->symbol_parameters[i]);
    }

    // Add the function's return value to it's outermost symbol table.
    struct address* const return_value_address =
        address_new(address_init_local(rbp_offset));
    autil_freezer_register(context()->freezer, return_value_address);
    struct symbol* const return_value_symbol = symbol_new_variable(
        decl->data.function.return_typespec->location,
        context()->interned.return_,
        return_type,
        return_value_address,
        NULL);
    autil_freezer_register(context()->freezer, return_value_symbol);
    symbol_table_insert(symbol_table, return_value_symbol);
    function->symbol_return = return_value_symbol;

    // Complete the function.
    assert(resolver->current_function == NULL);
    assert(resolver->current_symbol_table == context()->global_symbol_table);
    assert(resolver->current_rbp_offset == 0x0);
    resolver->current_function = function;
    function->body =
        resolve_block(resolver, symbol_table, decl->data.function.body);
    resolver->current_function = NULL;
    assert(resolver->current_symbol_table == context()->global_symbol_table);
    assert(resolver->current_rbp_offset == 0x0);

    // Freeze the symbol table now that the function has been completed and no
    // new symbols will be added.
    symbol_table_freeze(symbol_table, context()->freezer);

    return function_symbol;
}

static struct tir_stmt const*
resolve_stmt(struct resolver* resolver, struct ast_stmt const* stmt)
{
    assert(resolver != NULL);
    assert(!resolver_is_global(resolver));
    assert(stmt != NULL);
    trace(resolver->module->path, NO_LINE, "%s", __func__);

    switch (stmt->kind) {
    case AST_STMT_DECL: {
        return resolve_stmt_decl(resolver, stmt);
    }
    case AST_STMT_IF: {
        return resolve_stmt_if(resolver, stmt);
    }
    case AST_STMT_FOR_RANGE: {
        return resolve_stmt_for_range(resolver, stmt);
    }
    case AST_STMT_FOR_EXPR: {
        return resolve_stmt_for_expr(resolver, stmt);
    }
    case AST_STMT_DUMP: {
        return resolve_stmt_dump(resolver, stmt);
    }
    case AST_STMT_RETURN: {
        return resolve_stmt_return(resolver, stmt);
    }
    case AST_STMT_ASSIGN: {
        return resolve_stmt_assign(resolver, stmt);
    }
    case AST_STMT_EXPR: {
        return resolve_stmt_expr(resolver, stmt);
    }
    }

    UNREACHABLE();
    return NULL;
}

static struct tir_stmt const*
resolve_stmt_decl(struct resolver* resolver, struct ast_stmt const* stmt)
{
    assert(resolver != NULL);
    assert(!resolver_is_global(resolver));
    assert(stmt != NULL);
    assert(stmt->kind == AST_STMT_DECL);
    trace(resolver->module->path, NO_LINE, "%s", __func__);

    struct ast_decl const* const decl = stmt->data.decl;
    switch (decl->kind) {
    case AST_DECL_VARIABLE: {
        struct tir_expr const* lhs = NULL;
        struct tir_expr const* rhs = NULL;
        resolve_decl_variable(resolver, decl, &lhs, &rhs);
        struct tir_stmt* const resolved =
            tir_stmt_new_assign(stmt->location, lhs, rhs);

        autil_freezer_register(context()->freezer, resolved);
        return resolved;
    }
    case AST_DECL_CONSTANT: {
        resolve_decl_constant(resolver, decl);
        return NULL;
    }
    case AST_DECL_FUNCTION: {
        fatal(
            stmt->location->path,
            stmt->location->line,
            "nested function declaration");
        return NULL;
    }
    }

    UNREACHABLE();
    return NULL;
}

static struct tir_stmt const*
resolve_stmt_if(struct resolver* resolver, struct ast_stmt const* stmt)
{
    assert(resolver != NULL);
    assert(!resolver_is_global(resolver));
    assert(stmt != NULL);
    assert(stmt->kind == AST_STMT_IF);
    trace(resolver->module->path, NO_LINE, "%s", __func__);

    autil_sbuf(struct ast_conditional const* const) const conditionals =
        stmt->data.if_.conditionals;
    autil_sbuf(struct tir_conditional const*) resolved_conditionals = NULL;
    autil_sbuf_resize(resolved_conditionals, autil_sbuf_count(conditionals));
    for (size_t i = 0; i < autil_sbuf_count(conditionals); ++i) {
        assert(
            (conditionals[i]->condition != NULL)
            || (i == (autil_sbuf_count(conditionals) - 1)));

        struct tir_expr const* condition = NULL;
        if (conditionals[i]->condition != NULL) {
            condition = resolve_expr(resolver, conditionals[i]->condition);
            if (condition->type->kind != TYPE_BOOL) {
                fatal(
                    condition->location->path,
                    condition->location->line,
                    "illegal condition with non-boolean type `%s`",
                    condition->type->name);
            }
        }

        struct symbol_table* const symbol_table =
            symbol_table_new(resolver->current_symbol_table);
        struct tir_block const* const block =
            resolve_block(resolver, symbol_table, conditionals[i]->body);
        // Freeze the symbol table now that the block has been resolved and no
        // new symbols will be added.
        symbol_table_freeze(symbol_table, context()->freezer);

        struct tir_conditional* const resolved_conditional =
            tir_conditional_new(conditionals[i]->location, condition, block);
        autil_freezer_register(context()->freezer, resolved_conditional);
        resolved_conditionals[i] = resolved_conditional;
    }

    autil_sbuf_freeze(resolved_conditionals, context()->freezer);
    struct tir_stmt* const resolved = tir_stmt_new_if(resolved_conditionals);

    autil_freezer_register(context()->freezer, resolved);
    return resolved;
}

static struct tir_stmt const*
resolve_stmt_for_range(struct resolver* resolver, struct ast_stmt const* stmt)
{
    assert(resolver != NULL);
    assert(!resolver_is_global(resolver));
    assert(stmt != NULL);
    assert(stmt->kind == AST_STMT_FOR_RANGE);
    trace(resolver->module->path, NO_LINE, "%s", __func__);

    struct tir_expr const* const begin =
        resolve_expr(resolver, stmt->data.for_range.begin);
    if (begin->type != context()->builtin.usize) {
        fatal(
            begin->location->path,
            begin->location->line,
            "illegal range-begin-expression with non-usize type `%s`",
            begin->type->name);
    }

    struct tir_expr const* const end =
        resolve_expr(resolver, stmt->data.for_range.end);
    if (end->type != context()->builtin.usize) {
        fatal(
            end->location->path,
            end->location->line,
            "illegal range-end-expression with non-usize type `%s`",
            end->type->name);
    }

    int const save_rbp_offset = resolver->current_rbp_offset;
    struct source_location const* const loop_var_location =
        stmt->data.for_range.identifier->location;
    char const* const loop_var_name = stmt->data.for_range.identifier->name;
    struct type const* const loop_var_type = context()->builtin.usize;
    struct address* const loop_var_address = address_new(
        resolver_reserve_space(resolver, loop_var_name, loop_var_type));
    autil_freezer_register(context()->freezer, loop_var_address);
    struct symbol* const loop_var_symbol = symbol_new_variable(
        loop_var_location,
        loop_var_name,
        loop_var_type,
        loop_var_address,
        NULL);
    autil_freezer_register(context()->freezer, loop_var_symbol);

    struct symbol_table* const symbol_table =
        symbol_table_new(resolver->current_symbol_table);
    symbol_table_insert(symbol_table, loop_var_symbol);
    struct tir_block const* const body =
        resolve_block(resolver, symbol_table, stmt->data.for_range.body);
    // Freeze the symbol table now that the block has been resolved and no new
    // symbols will be added.
    symbol_table_freeze(symbol_table, context()->freezer);
    resolver->current_rbp_offset = save_rbp_offset;

    struct tir_stmt* const resolved = tir_stmt_new_for_range(
        stmt->location, loop_var_symbol, begin, end, body);
    autil_freezer_register(context()->freezer, resolved);
    return resolved;
}

static struct tir_stmt const*
resolve_stmt_for_expr(struct resolver* resolver, struct ast_stmt const* stmt)
{
    assert(resolver != NULL);
    assert(!resolver_is_global(resolver));
    assert(stmt != NULL);
    assert(stmt->kind == AST_STMT_FOR_EXPR);
    trace(resolver->module->path, NO_LINE, "%s", __func__);

    struct tir_expr const* const expr =
        resolve_expr(resolver, stmt->data.for_expr.expr);
    if (expr->type->kind != TYPE_BOOL) {
        fatal(
            expr->location->path,
            expr->location->line,
            "illegal condition with non-boolean type `%s`",
            expr->type->name);
    }

    struct symbol_table* const symbol_table =
        symbol_table_new(resolver->current_symbol_table);
    struct tir_block const* const body =
        resolve_block(resolver, symbol_table, stmt->data.for_expr.body);
    // Freeze the symbol table now that the block has been resolved and no new
    // symbols will be added.
    symbol_table_freeze(symbol_table, context()->freezer);

    struct tir_stmt* const resolved =
        tir_stmt_new_for_expr(stmt->location, expr, body);

    autil_freezer_register(context()->freezer, resolved);
    return resolved;
}

static struct tir_stmt const*
resolve_stmt_dump(struct resolver* resolver, struct ast_stmt const* stmt)
{
    assert(resolver != NULL);
    assert(!resolver_is_global(resolver));
    assert(stmt != NULL);
    assert(stmt->kind == AST_STMT_DUMP);
    trace(resolver->module->path, NO_LINE, "%s", __func__);

    struct tir_expr const* expr = resolve_expr(resolver, stmt->data.dump.expr);
    struct tir_stmt* const resolved = tir_stmt_new_dump(stmt->location, expr);

    autil_freezer_register(context()->freezer, resolved);
    return resolved;
}

static struct tir_stmt const*
resolve_stmt_return(struct resolver* resolver, struct ast_stmt const* stmt)
{
    assert(resolver != NULL);
    assert(!resolver_is_global(resolver));
    assert(stmt != NULL);
    assert(stmt->kind == AST_STMT_RETURN);
    trace(resolver->module->path, NO_LINE, "%s", __func__);

    struct type const* const return_type =
        resolver->current_function->type->data.function.return_type;
    struct tir_expr const* expr = NULL;
    if (stmt->data.return_.expr != NULL) {
        expr = resolve_expr(resolver, stmt->data.return_.expr);
        if (expr->type != return_type) {
            fatal(
                expr->location->path,
                expr->location->line,
                "illegal type conversion from `%s` to `%s`",
                expr->type->name,
                return_type->name);
        }
    }
    else {
        if (context()->builtin.void_ != return_type) {
            fatal(
                stmt->location->path,
                stmt->location->line,
                "illegal return statement in function with non-void return type");
        }
    }

    struct tir_stmt* const resolved = tir_stmt_new_return(stmt->location, expr);

    autil_freezer_register(context()->freezer, resolved);
    return resolved;
}

static struct tir_stmt const*
resolve_stmt_assign(struct resolver* resolver, struct ast_stmt const* stmt)
{
    assert(resolver != NULL);
    assert(!resolver_is_global(resolver));
    assert(stmt != NULL);
    assert(stmt->kind == AST_STMT_ASSIGN);

    struct tir_expr const* const lhs =
        resolve_expr(resolver, stmt->data.assign.lhs);
    struct tir_expr const* const rhs =
        resolve_expr(resolver, stmt->data.assign.rhs);
    // TODO: Rather than query if lhs is an lvalue, perhaps there could function
    // `validate_expr_is_lvalue` in resolve.c which traverses the expression
    // tree and emits an error with more context about *why* a specific
    // expression is not an lvalue. Currently it's up to the user to figure out
    // *why* lhs is not an lvalue, and better information could ease debugging.
    if (!tir_expr_is_lvalue(lhs)) {
        fatal(
            lhs->location->path,
            lhs->location->line,
            "left hand side of assignment statement is not an lvalue");
    }
    struct tir_stmt* const resolved =
        tir_stmt_new_assign(stmt->location, lhs, rhs);

    autil_freezer_register(context()->freezer, resolved);
    return resolved;
}

static struct tir_stmt const*
resolve_stmt_expr(struct resolver* resolver, struct ast_stmt const* stmt)
{
    assert(resolver != NULL);
    assert(!resolver_is_global(resolver));
    assert(stmt != NULL);
    assert(stmt->kind == AST_STMT_EXPR);
    trace(resolver->module->path, NO_LINE, "%s", __func__);

    struct tir_expr const* const expr = resolve_expr(resolver, stmt->data.expr);
    struct tir_stmt* const resolved = tir_stmt_new_expr(stmt->location, expr);

    autil_freezer_register(context()->freezer, resolved);
    return resolved;
}

static struct tir_expr const*
resolve_expr(struct resolver* resolver, struct ast_expr const* expr)
{
    assert(resolver != NULL);
    assert(expr != NULL);
    trace(resolver->module->path, NO_LINE, "%s", __func__);

    switch (expr->kind) {
    case AST_EXPR_IDENTIFIER: {
        return resolve_expr_identifier(resolver, expr);
    }
    case AST_EXPR_BOOLEAN: {
        return resolve_expr_boolean(resolver, expr);
    }
    case AST_EXPR_INTEGER: {
        return resolve_expr_integer(resolver, expr);
    }
    case AST_EXPR_ARRAY: {
        return resolve_expr_array(resolver, expr);
    }
    case AST_EXPR_GROUPED: {
        return resolve_expr(resolver, expr->data.grouped.expr);
    }
    case AST_EXPR_SYSCALL: {
        return resolve_expr_syscall(resolver, expr);
    }
    case AST_EXPR_CALL: {
        return resolve_expr_call(resolver, expr);
    }
    case AST_EXPR_INDEX: {
        return resolve_expr_index(resolver, expr);
    }
    case AST_EXPR_UNARY: {
        return resolve_expr_unary(resolver, expr);
    }
    case AST_EXPR_BINARY: {
        return resolve_expr_binary(resolver, expr);
    }
    }

    UNREACHABLE();
    return NULL;
}

static struct tir_expr const*
resolve_expr_identifier(struct resolver* resolver, struct ast_expr const* expr)
{
    assert(resolver != NULL);
    assert(expr != NULL);
    assert(expr->kind == AST_EXPR_IDENTIFIER);
    trace(resolver->module->path, NO_LINE, "%s", __func__);

    char const* const name = expr->data.identifier->name;
    struct symbol const* const symbol =
        symbol_table_lookup(resolver->current_symbol_table, name);
    if (symbol == NULL) {
        fatal(
            expr->location->path,
            expr->location->line,
            "use of undeclared identifier `%s`",
            name);
    }
    if (symbol->kind == SYMBOL_TYPE) {
        fatal(
            expr->location->path,
            expr->location->line,
            "use of type `%s` as an expression",
            name);
    }
    struct tir_expr* const resolved =
        tir_expr_new_identifier(expr->location, symbol);

    autil_freezer_register(context()->freezer, resolved);
    return resolved;
}

static struct tir_expr const*
resolve_expr_boolean(struct resolver* resolver, struct ast_expr const* expr)
{
    assert(resolver != NULL);
    assert(expr != NULL);
    assert(expr->kind == AST_EXPR_BOOLEAN);
    trace(resolver->module->path, NO_LINE, "%s", __func__);

    bool const value = expr->data.boolean->value;
    struct tir_expr* const resolved =
        tir_expr_new_boolean(expr->location, value);

    autil_freezer_register(context()->freezer, resolved);
    return resolved;
}

static struct type const*
integer_literal_suffix_to_type(
    struct source_location const* location, char const* suffix)
{
    assert(suffix != NULL);

    if (suffix == context()->interned.empty) {
        fatal(
            location->path,
            location->line,
            "integer literal has no suffix",
            suffix);
    }

    if (suffix == context()->interned.u) {
        return context()->builtin.usize;
    }
    if (suffix == context()->interned.s) {
        return context()->builtin.ssize;
    }

    fatal(
        location->path,
        location->line,
        "unknown integer literal suffix `%s`",
        suffix);
    return NULL;
}

static struct tir_expr const*
resolve_expr_integer(struct resolver* resolver, struct ast_expr const* expr)
{
    assert(resolver != NULL);
    assert(expr != NULL);
    assert(expr->kind == AST_EXPR_INTEGER);
    trace(resolver->module->path, NO_LINE, "%s", __func__);

    struct ast_integer const* const ast_integer = expr->data.integer;
    struct autil_bigint const* const value = ast_integer->value;
    struct type const* const type = integer_literal_suffix_to_type(
        ast_integer->location, ast_integer->suffix);

    assert(type != NULL);
    struct tir_expr* const resolved =
        tir_expr_new_integer(expr->location, type, value);

    autil_freezer_register(context()->freezer, resolved);
    return resolved;
}

static struct tir_expr const*
resolve_expr_array(struct resolver* resolver, struct ast_expr const* expr)
{
    assert(resolver != NULL);
    assert(expr != NULL);
    assert(expr->kind == AST_EXPR_ARRAY);
    trace(resolver->module->path, NO_LINE, "%s", __func__);

    struct type const* const type =
        resolve_typespec(resolver, expr->data.array.typespec);

    autil_sbuf(struct ast_expr const* const) elements =
        expr->data.array.elements;
    autil_sbuf(struct tir_expr const*) resolved_elements = NULL;
    for (size_t i = 0; i < autil_sbuf_count(elements); ++i) {
        autil_sbuf_push(resolved_elements, resolve_expr(resolver, elements[i]));
    }
    autil_sbuf_freeze(resolved_elements, context()->freezer);

    if (type->data.array.count != autil_sbuf_count(resolved_elements)) {
        fatal(
            expr->location->path,
            expr->location->line,
            "array of type `%s` created with %zu elements (expected %zu)",
            type->name,
            autil_sbuf_count(resolved_elements),
            type->data.array.count);
    }

    struct tir_expr* const resolved =
        tir_expr_new_array(expr->location, type, resolved_elements);

    autil_freezer_register(context()->freezer, resolved);
    return resolved;
}

static struct tir_expr const*
resolve_expr_syscall(struct resolver* resolver, struct ast_expr const* expr)
{
    assert(resolver != NULL);
    assert(expr != NULL);
    assert(expr->kind == AST_EXPR_SYSCALL);
    trace(resolver->module->path, NO_LINE, "%s", __func__);

    autil_sbuf(struct ast_expr const* const) arguments =
        expr->data.syscall.arguments;
    size_t const arguments_count = autil_sbuf_count(arguments);

    // Sanity-check assert. The parser should have reported a fatal error if
    // fewer than SYSCALL_ARGUMENTS_MIN were provided.
    assert(arguments_count >= SYSCALL_ARGUMENTS_MIN);

    if (arguments_count > SYSCALL_ARGUMENTS_MAX) {
        fatal(
            expr->location->path,
            expr->location->line,
            "%zu syscall arguments provided (maximum %zu allowed)",
            arguments_count,
            SYSCALL_ARGUMENTS_MAX);
    }

    autil_sbuf(struct tir_expr const*) exprs = NULL;
    for (size_t i = 0; i < autil_sbuf_count(arguments); ++i) {
        autil_sbuf_push(exprs, resolve_expr(resolver, arguments[i]));
    }
    autil_sbuf_freeze(exprs, context()->freezer);
    struct tir_expr* const resolved =
        tir_expr_new_syscall(expr->location, exprs);

    autil_freezer_register(context()->freezer, resolved);
    return resolved;
}

static struct tir_expr const*
resolve_expr_call(struct resolver* resolver, struct ast_expr const* expr)
{
    assert(resolver != NULL);
    assert(expr != NULL);
    assert(expr->kind == AST_EXPR_CALL);
    trace(resolver->module->path, NO_LINE, "%s", __func__);

    struct tir_expr const* const function =
        resolve_expr(resolver, expr->data.call.func);
    if (function->type->kind != TYPE_FUNCTION) {
        fatal(
            expr->location->path,
            expr->location->line,
            "non-callable type `%s` used in function call expression",
            function->type->name);
    }

    autil_sbuf(struct tir_expr const*) arguments = NULL;
    for (size_t i = 0; i < autil_sbuf_count(expr->data.call.arguments); ++i) {
        autil_sbuf_push(
            arguments, resolve_expr(resolver, expr->data.call.arguments[i]));
    }
    autil_sbuf_freeze(arguments, context()->freezer);

    autil_sbuf(struct type const* const) const parameter_types =
        function->type->data.function.parameter_types;
    if (autil_sbuf_count(arguments) != autil_sbuf_count(parameter_types)) {
        fatal(
            expr->location->path,
            expr->location->line,
            "function with type `%s` expects %zu argument(s) (%zu provided)",
            function->type->name,
            autil_sbuf_count(parameter_types),
            autil_sbuf_count(arguments));
    }

    // Type-check function arguments.
    for (size_t i = 0; i < autil_sbuf_count(parameter_types); ++i) {
        struct type const* const expected = parameter_types[i];
        struct type const* const received = arguments[i]->type;
        if (expected == received) {
            continue;
        }
        fatal(
            arguments[i]->location->path,
            arguments[i]->location->line,
            "expected argument of type `%s` (received `%s`)",
            expected->name,
            received->name);
    }

    struct tir_expr* const resolved =
        tir_expr_new_call(expr->location, function, arguments);

    autil_freezer_register(context()->freezer, resolved);
    return resolved;
}

static struct tir_expr const*
resolve_expr_index(struct resolver* resolver, struct ast_expr const* expr)
{
    assert(resolver != NULL);
    assert(expr != NULL);
    assert(expr->kind == AST_EXPR_INDEX);
    trace(resolver->module->path, NO_LINE, "%s", __func__);

    struct tir_expr const* const lhs =
        resolve_expr(resolver, expr->data.index.lhs);
    if (lhs->type->kind != TYPE_ARRAY) {
        fatal(
            lhs->location->path,
            lhs->location->line,
            "illegal index operation with left-hand-side of type `%s`",
            lhs->type->name);
    }

    struct tir_expr const* const idx =
        resolve_expr(resolver, expr->data.index.idx);
    if (idx->type->kind != TYPE_USIZE) {
        fatal(
            idx->location->path,
            idx->location->line,
            "illegal index operation with index of non-usize type `%s`",
            idx->type->name);
    }

    struct tir_expr* const resolved =
        tir_expr_new_index(expr->location, lhs, idx);

    autil_freezer_register(context()->freezer, resolved);
    return resolved;
}

static struct tir_expr const*
resolve_expr_unary(struct resolver* resolver, struct ast_expr const* expr)
{
    assert(resolver != NULL);
    assert(expr != NULL);
    assert(expr->kind == AST_EXPR_UNARY);
    trace(resolver->module->path, NO_LINE, "%s", __func__);

    struct tir_expr const* const rhs =
        resolve_expr(resolver, expr->data.unary.rhs);
    struct token const* const op = expr->data.unary.op;
    switch (op->kind) {
    case TOKEN_NOT: {
        return resolve_expr_unary_logical(resolver, op, UOP_NOT, rhs);
    }
    case TOKEN_PLUS: {
        return resolve_expr_unary_arithmetic(resolver, op, UOP_POS, rhs);
    }
    case TOKEN_DASH: {
        return resolve_expr_unary_arithmetic(resolver, op, UOP_NEG, rhs);
    }
    case TOKEN_STAR: {
        return resolve_expr_unary_dereference(resolver, op, rhs);
    }
    case TOKEN_AMPERSAND: {
        return resolve_expr_unary_addressof(resolver, op, rhs);
    }
    default: {
        UNREACHABLE();
    }
    }

    UNREACHABLE();
    return NULL;
}

static struct tir_expr const*
resolve_expr_unary_logical(
    struct resolver* resolver,
    struct token const* op,
    enum uop_kind uop,
    struct tir_expr const* rhs)
{
    assert(resolver != NULL);
    assert(op != NULL);
    assert(rhs != NULL);
    trace(resolver->module->path, NO_LINE, "%s", __func__);

    if (rhs->type->kind != TYPE_BOOL) {
        fatal(
            op->location.path,
            op->location.line,
            "invalid argument of type `%s` in unary `%s` expression",
            rhs->type->name,
            token_kind_to_cstr(op->kind));
    }

    struct tir_expr* const resolved =
        tir_expr_new_unary(&op->location, rhs->type, uop, rhs);

    autil_freezer_register(context()->freezer, resolved);
    return resolved;
}

static struct tir_expr const*
resolve_expr_unary_arithmetic(
    struct resolver* resolver,
    struct token const* op,
    enum uop_kind uop,
    struct tir_expr const* rhs)
{
    assert(resolver != NULL);
    assert(op != NULL);
    assert(rhs != NULL);
    trace(resolver->module->path, NO_LINE, "%s", __func__);

    if (!type_is_integer(rhs->type)) {
        fatal(
            op->location.path,
            op->location.line,
            "invalid argument of type `%s` in unary `%s` expression",
            rhs->type->name,
            token_kind_to_cstr(op->kind));
    }

    struct tir_expr* const resolved =
        tir_expr_new_unary(&op->location, rhs->type, uop, rhs);

    autil_freezer_register(context()->freezer, resolved);
    return resolved;
}

static struct tir_expr const*
resolve_expr_unary_dereference(
    struct resolver* resolver,
    struct token const* op,
    struct tir_expr const* rhs)
{
    assert(resolver != NULL);
    assert(op != NULL);
    assert(op->kind == TOKEN_STAR);
    assert(rhs != NULL);
    trace(resolver->module->path, NO_LINE, "%s", __func__);

    if (rhs->type->kind != TYPE_POINTER) {
        fatal(
            rhs->location->path,
            rhs->location->line,
            "cannot dereference non-pointer type `%s`",
            rhs->type->name);
    }
    struct tir_expr* const resolved = tir_expr_new_unary(
        &op->location, rhs->type->data.pointer.base, UOP_DEREFERENCE, rhs);

    autil_freezer_register(context()->freezer, resolved);
    return resolved;
}

static struct tir_expr const*
resolve_expr_unary_addressof(
    struct resolver* resolver,
    struct token const* op,
    struct tir_expr const* rhs)
{
    assert(resolver != NULL);
    assert(op != NULL);
    assert(op->kind == TOKEN_AMPERSAND);
    assert(rhs != NULL);
    trace(resolver->module->path, NO_LINE, "%s", __func__);

    if (!tir_expr_is_lvalue(rhs)) {
        fatal(
            rhs->location->path,
            rhs->location->line,
            "cannot take the address of a non-lvalue");
    }
    struct tir_expr* const resolved = tir_expr_new_unary(
        &op->location, type_unique_pointer(rhs->type), UOP_ADDRESSOF, rhs);

    autil_freezer_register(context()->freezer, resolved);
    return resolved;
}

static struct tir_expr const*
resolve_expr_binary(struct resolver* resolver, struct ast_expr const* expr)
{
    assert(resolver != NULL);
    assert(expr != NULL);
    assert(expr->kind == AST_EXPR_BINARY);
    trace(resolver->module->path, NO_LINE, "%s", __func__);

    struct tir_expr const* const lhs =
        resolve_expr(resolver, expr->data.binary.lhs);
    struct tir_expr const* const rhs =
        resolve_expr(resolver, expr->data.binary.rhs);
    struct token const* const op = expr->data.binary.op;
    switch (op->kind) {
    case TOKEN_OR: {
        return resolve_expr_binary_logical(resolver, op, BOP_OR, lhs, rhs);
    }
    case TOKEN_AND: {
        return resolve_expr_binary_logical(resolver, op, BOP_AND, lhs, rhs);
    }
    case TOKEN_EQ: {
        return resolve_expr_binary_compare(resolver, op, BOP_EQ, lhs, rhs);
    }
    case TOKEN_NE: {
        return resolve_expr_binary_compare(resolver, op, BOP_NE, lhs, rhs);
    }
    case TOKEN_LE: {
        return resolve_expr_binary_compare(resolver, op, BOP_LE, lhs, rhs);
    }
    case TOKEN_LT: {
        return resolve_expr_binary_compare(resolver, op, BOP_LT, lhs, rhs);
    }
    case TOKEN_GE: {
        return resolve_expr_binary_compare(resolver, op, BOP_GE, lhs, rhs);
    }
    case TOKEN_GT: {
        return resolve_expr_binary_compare(resolver, op, BOP_GT, lhs, rhs);
    }
    case TOKEN_PLUS: {
        return resolve_expr_binary_arithmetic(resolver, op, BOP_ADD, lhs, rhs);
    }
    case TOKEN_DASH: {
        return resolve_expr_binary_arithmetic(resolver, op, BOP_SUB, lhs, rhs);
    }
    case TOKEN_STAR: {
        return resolve_expr_binary_arithmetic(resolver, op, BOP_MUL, lhs, rhs);
    }
    case TOKEN_FSLASH: {
        return resolve_expr_binary_arithmetic(resolver, op, BOP_DIV, lhs, rhs);
    }
    default: {
        UNREACHABLE();
    }
    }

    UNREACHABLE();
    return NULL;
}

static struct tir_expr const*
resolve_expr_binary_logical(
    struct resolver* resolver,
    struct token const* op,
    enum bop_kind bop,
    struct tir_expr const* lhs,
    struct tir_expr const* rhs)
{
    assert(resolver != NULL);
    assert(op != NULL);
    assert(lhs != NULL);
    assert(rhs != NULL);
    trace(resolver->module->path, NO_LINE, "%s", __func__);

    bool const valid = lhs->type->kind == TYPE_BOOL
        && rhs->type->kind == TYPE_BOOL && lhs->type == rhs->type;
    if (!valid) {
        fatal(
            op->location.path,
            op->location.line,
            "invalid arguments of types `%s` and `%s` in binary `%s` expression",
            lhs->type->name,
            rhs->type->name,
            token_kind_to_cstr(op->kind));
    }

    struct type const* const type = context()->builtin.bool_;
    struct tir_expr* const resolved =
        tir_expr_new_binary(&op->location, type, bop, lhs, rhs);

    autil_freezer_register(context()->freezer, resolved);
    return resolved;
}

static struct tir_expr const*
resolve_expr_binary_compare(
    struct resolver* resolver,
    struct token const* op,
    enum bop_kind bop,
    struct tir_expr const* lhs,
    struct tir_expr const* rhs)
{
    assert(resolver != NULL);
    assert(op != NULL);
    assert(lhs != NULL);
    assert(rhs != NULL);
    trace(resolver->module->path, NO_LINE, "%s", __func__);

    bool const valid = lhs->type == rhs->type;
    if (!valid) {
        fatal(
            op->location.path,
            op->location.line,
            "invalid arguments of types `%s` and `%s` in binary `%s` expression",
            lhs->type->name,
            rhs->type->name,
            token_kind_to_cstr(op->kind));
    }

    struct type const* const type = context()->builtin.bool_;
    struct tir_expr* const resolved =
        tir_expr_new_binary(&op->location, type, bop, lhs, rhs);

    autil_freezer_register(context()->freezer, resolved);
    return resolved;
}

static struct tir_expr const*
resolve_expr_binary_arithmetic(
    struct resolver* resolver,
    struct token const* op,
    enum bop_kind bop,
    struct tir_expr const* lhs,
    struct tir_expr const* rhs)
{
    assert(resolver != NULL);
    assert(op != NULL);
    assert(lhs != NULL);
    assert(rhs != NULL);
    trace(resolver->module->path, NO_LINE, "%s", __func__);

    bool const valid = type_is_integer(lhs->type) && type_is_integer(rhs->type)
        && lhs->type == rhs->type;
    if (!valid) {
        fatal(
            op->location.path,
            op->location.line,
            "invalid arguments of types `%s` and `%s` in binary `%s` expression",
            lhs->type->name,
            rhs->type->name,
            token_kind_to_cstr(op->kind));
    }

    struct type const* const type = lhs->type; // Arbitrarily use lhs.
    struct tir_expr* const resolved =
        tir_expr_new_binary(&op->location, type, bop, lhs, rhs);

    autil_freezer_register(context()->freezer, resolved);
    return resolved;
}

static struct tir_block const*
resolve_block(
    struct resolver* resolver,
    struct symbol_table* symbol_table,
    struct ast_block const* block)
{
    assert(resolver->current_function != NULL);
    assert(symbol_table != NULL);

    struct symbol_table* const save_symbol_table =
        resolver->current_symbol_table;
    resolver->current_symbol_table = symbol_table;
    int const save_rbp_offset = resolver->current_rbp_offset;

    autil_sbuf(struct tir_stmt const*) stmts = NULL;
    for (size_t i = 0; i < autil_sbuf_count(block->stmts); ++i) {
        struct tir_stmt const* const resolved_stmt =
            resolve_stmt(resolver, block->stmts[i]);
        if (resolved_stmt != NULL) {
            autil_sbuf_push(stmts, resolved_stmt);
        }
    }
    autil_sbuf_freeze(stmts, context()->freezer);

    struct tir_block* const resolved =
        tir_block_new(block->location, symbol_table, stmts);

    autil_freezer_register(context()->freezer, resolved);
    resolver->current_symbol_table = save_symbol_table;
    resolver->current_rbp_offset = save_rbp_offset;
    return resolved;
}

static struct type const*
resolve_typespec(struct resolver* resolver, struct ast_typespec const* typespec)
{
    assert(resolver != NULL);
    assert(typespec != NULL);
    trace(resolver->module->path, NO_LINE, "%s", __func__);

    switch (typespec->kind) {
    case TYPESPEC_IDENTIFIER: {
        char const* const name = typespec->data.identifier->name;
        struct symbol const* const symbol =
            symbol_table_lookup(resolver->current_symbol_table, name);

        if (symbol == NULL) {
            fatal(
                typespec->location->path,
                typespec->location->line,
                "use of undeclared identifier `%s`",
                name);
        }
        if (symbol->kind != SYMBOL_TYPE) {
            fatal(
                typespec->location->path,
                typespec->location->line,
                "identifier `%s` is not a type",
                name);
        }

        return symbol->type;
    }
    case TYPESPEC_FUNCTION: {
        autil_sbuf(struct ast_typespec const* const) const parameter_typespecs =
            typespec->data.function.parameter_typespecs;

        autil_sbuf(struct type const*) parameter_types = NULL;
        autil_sbuf_resize(
            parameter_types, autil_sbuf_count(parameter_typespecs));
        for (size_t i = 0; i < autil_sbuf_count(parameter_typespecs); ++i) {
            parameter_types[i] =
                resolve_typespec(resolver, parameter_typespecs[i]);
        }
        autil_sbuf_freeze(parameter_types, context()->freezer);

        struct type const* const return_type =
            resolve_typespec(resolver, typespec->data.function.return_typespec);

        return type_unique_function(parameter_types, return_type);
    }
    case TYPESPEC_POINTER: {
        struct type const* const base =
            resolve_typespec(resolver, typespec->data.pointer.base);
        return type_unique_pointer(base);
    }
    case TYPESPEC_ARRAY: {
        struct tir_expr const* const count_expr =
            resolve_expr(resolver, typespec->data.array.count);
        if (count_expr->type != context()->builtin.usize) {
            fatal(
                count_expr->location->path,
                count_expr->location->line,
                "illegal array count with non-usize type `%s`",
                count_expr->type->name);
        }

        struct evaluator* const evaluator =
            evaluator_new(resolver->current_symbol_table);
        struct value* const count_value = eval_expr(evaluator, count_expr);
        evaluator_del(evaluator);

        assert(count_value->type == context()->builtin.usize);
        size_t count = 0u;
        if (bigint_to_uz(&count, count_value->data.integer)) {
            char* const cstr =
                autil_bigint_to_new_cstr(count_value->data.integer, NULL);
            fatal(
                count_expr->location->path,
                count_expr->location->line,
                "array count too large (received %s)",
                cstr);
        }
        value_del(count_value);

        struct type const* const base =
            resolve_typespec(resolver, typespec->data.array.base);
        return type_unique_array(count, base);
    }
    }

    UNREACHABLE();
    return NULL;
}

void
resolve(struct module* module)
{
    assert(module != NULL);
    trace(module->path, NO_LINE, "%s", __func__);

    struct resolver* const resolver = resolver_new(module);
    for (size_t i = 0; i < autil_sbuf_count(module->ordered); ++i) {
        resolve_decl(resolver, module->ordered[i]);
    }
    resolver_del(resolver);
}
