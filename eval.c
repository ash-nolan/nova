// Copyright 2021 The Nova Project Authors
// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "nova.h"

struct evaluator {
    struct symbol_table const* symbol_table;
};

struct evaluator*
evaluator_new(struct symbol_table const* symbol_table)
{
    assert(symbol_table != NULL);

    struct evaluator* const self = autil_xalloc(NULL, sizeof(*self));
    memset(self, 0x00, sizeof(*self));
    self->symbol_table = symbol_table;

    return self;
}
void
evaluator_del(struct evaluator* self)
{
    assert(self != NULL);

    memset(self, 0x00, sizeof(*self));
    autil_xalloc(self, AUTIL_XALLOC_FREE);
}

struct value*
eval_expr(struct evaluator* evaluator, struct tir_expr const* expr)
{
    assert(evaluator != NULL);
    assert(expr != NULL);

    switch (expr->kind) {
    case TIR_EXPR_IDENTIFIER: {
        assert(expr->data.identifier->value != NULL);
        return value_clone(expr->data.identifier->value);
    }
    case TIR_EXPR_BOOLEAN: {
        return value_new_boolean(expr->data.boolean);
    }
    case TIR_EXPR_INTEGER: {
        struct autil_bigint const* const integer = expr->data.integer;
        return value_new_integer(expr->type, autil_bigint_new(integer));
    }
    case TIR_EXPR_ARRAY: {
        autil_sbuf(struct tir_expr const* const) elements =
            expr->data.array.elements;
        autil_sbuf(struct value*) evaled_elements = NULL;
        for (size_t i = 0; i < autil_sbuf_count(elements); ++i) {
            autil_sbuf_push(evaled_elements, eval_expr(evaluator, elements[i]));
        }
        return value_new_array(expr->type, evaled_elements);
    }
    case TIR_EXPR_SYSCALL: {
        fatal(
            expr->location->path,
            expr->location->line,
            "constant expression contains system call");
    }
    case TIR_EXPR_CALL: {
        fatal(
            expr->location->path,
            expr->location->line,
            "constant expression contains function call");
    }
    case TIR_EXPR_INDEX: {
        struct value* const lhs = eval_expr(evaluator, expr->data.index.lhs);
        struct value* const idx = eval_expr(evaluator, expr->data.index.idx);
        assert(lhs->type->kind == TYPE_ARRAY);
        assert(idx->type->kind == TYPE_USIZE);

        size_t idx_uz = 0u;
        if (bigint_to_uz(&idx_uz, idx->data.integer)) {
            char* const cstr =
                autil_bigint_to_new_cstr(idx->data.integer, NULL);
            fatal(
                expr->data.index.idx->location->path,
                expr->data.index.idx->location->line,
                "index out-of-range (received %s)",
                cstr);
        }

        if (idx_uz >= lhs->type->data.array.count) {
            char* const cstr =
                autil_bigint_to_new_cstr(idx->data.integer, NULL);
            fatal(
                expr->data.index.idx->location->path,
                expr->data.index.idx->location->line,
                "index out-of-bounds (array count is %zu, received %s)",
                lhs->type->data.array.count,
                cstr);
        }

        struct value* const res = value_clone(lhs->data.array.elements[idx_uz]);
        value_del(lhs);
        value_del(idx);
        return res;
    }
    case TIR_EXPR_UNARY: {
        struct value* const rhs = eval_expr(evaluator, expr->data.unary.rhs);
        switch (expr->data.unary.op) {
        case UOP_NOT: {
            assert(rhs->type->kind == TYPE_BOOL);
            rhs->data.boolean = !rhs->data.boolean;
            return rhs;
        }
        case UOP_POS: {
            assert(type_is_integer(rhs->type));
            return rhs;
        }
        case UOP_NEG: {
            assert(type_is_integer(rhs->type));
            autil_bigint_neg(rhs->data.integer, rhs->data.integer);
            return rhs;
        }
        case UOP_DEREFERENCE: {
            fatal(
                expr->location->path,
                expr->location->line,
                "dereference operator not-supported in compile-time expressions");
        }
        case UOP_ADDRESSOF: {
            fatal(
                expr->location->path,
                expr->location->line,
                "addressof operator not-supported in compile-time expressions");
        }
        default:
            UNREACHABLE();
        }
    }
    case TIR_EXPR_BINARY: {
        struct value* const lhs = eval_expr(evaluator, expr->data.binary.lhs);
        struct value* const rhs = eval_expr(evaluator, expr->data.binary.rhs);
        struct value* res = NULL;
        switch (expr->data.binary.op) {
        case BOP_OR: {
            assert(lhs->type->kind == TYPE_BOOL);
            assert(rhs->type->kind == TYPE_BOOL);
            res = value_new_boolean(lhs->data.boolean || rhs->data.boolean);
            break;
        }
        case BOP_AND: {
            assert(lhs->type->kind == TYPE_BOOL);
            assert(rhs->type->kind == TYPE_BOOL);
            res = value_new_boolean(lhs->data.boolean && rhs->data.boolean);
            break;
        }
        case BOP_EQ: {
            res = value_new_boolean(value_eq(lhs, rhs));
            break;
        }
        case BOP_NE: {
            res = value_new_boolean(!value_eq(lhs, rhs));
            break;
        }
        case BOP_LE: {
            res = value_new_boolean(value_lt(lhs, rhs) || value_eq(lhs, rhs));
            break;
        }
        case BOP_LT: {
            res = value_new_boolean(value_lt(lhs, rhs));
            break;
        }
        case BOP_GE: {
            res = value_new_boolean(value_gt(lhs, rhs) || value_eq(lhs, rhs));
            break;
        }
        case BOP_GT: {
            res = value_new_boolean(value_gt(lhs, rhs));
            break;
        }
        case BOP_ADD: {
            assert(type_is_integer(lhs->type));
            assert(type_is_integer(rhs->type));
            struct autil_bigint* const r = autil_bigint_new(AUTIL_BIGINT_ZERO);
            autil_bigint_add(r, lhs->data.integer, rhs->data.integer);
            res = value_new_integer(expr->type, r);
            break;
        }
        case BOP_SUB: {
            assert(type_is_integer(lhs->type));
            assert(type_is_integer(rhs->type));
            struct autil_bigint* const r = autil_bigint_new(AUTIL_BIGINT_ZERO);
            autil_bigint_sub(r, lhs->data.integer, rhs->data.integer);
            res = value_new_integer(expr->type, r);
            break;
        }
        case BOP_MUL: {
            assert(type_is_integer(lhs->type));
            assert(type_is_integer(rhs->type));
            struct autil_bigint* const r = autil_bigint_new(AUTIL_BIGINT_ZERO);
            autil_bigint_mul(r, lhs->data.integer, rhs->data.integer);
            res = value_new_integer(expr->type, r);
            break;
        }
        case BOP_DIV: {
            assert(type_is_integer(lhs->type));
            assert(type_is_integer(rhs->type));
            struct autil_bigint* const r = autil_bigint_new(AUTIL_BIGINT_ZERO);
            autil_bigint_divrem(r, NULL, lhs->data.integer, rhs->data.integer);
            res = value_new_integer(expr->type, r);
            break;
        }
        case BOP_BITAND: {
            assert(
                lhs->type->kind == TYPE_BOOL || lhs->type->kind == TYPE_BYTE
                || type_is_integer(lhs->type));
            assert(
                rhs->type->kind == TYPE_BOOL || rhs->type->kind == TYPE_BYTE
                || type_is_integer(rhs->type));
            assert(lhs->type->kind == rhs->type->kind);
            struct type const* const type = lhs->type;

            if (type->kind == TYPE_BOOL) {
                res = value_new_boolean(lhs->data.boolean && rhs->data.boolean);
                break;
            }

            if (type->kind == TYPE_BYTE) {
                res = value_new_byte(lhs->data.byte & rhs->data.byte);
                break;
            }

            assert(type_is_integer(type));
            bool is_signed = type_is_sinteger(type);
            size_t bit_count = type->size * 8u;
            struct autil_bitarr* const lhs_bits = autil_bitarr_new(bit_count);
            struct autil_bitarr* const rhs_bits = autil_bitarr_new(bit_count);
            struct autil_bitarr* const res_bits = autil_bitarr_new(bit_count);
            if (bigint_to_bitarr(lhs_bits, lhs->data.integer)) {
                UNREACHABLE();
            }
            if (bigint_to_bitarr(rhs_bits, rhs->data.integer)) {
                UNREACHABLE();
            }

            for (size_t i = 0; i < bit_count; ++i) {
                int const bit = autil_bitarr_get(lhs_bits, i)
                    && autil_bitarr_get(rhs_bits, i);
                autil_bitarr_set(res_bits, i, bit);
            }
            autil_bitarr_del(lhs_bits);
            autil_bitarr_del(rhs_bits);

            struct autil_bigint* const res_bigint =
                autil_bigint_new(AUTIL_BIGINT_ZERO);
            bitarr_to_bigint(res_bigint, res_bits, is_signed);
            autil_bitarr_del(res_bits);

            res = value_new_integer(type, res_bigint);
            break;
        }
        default:
            UNREACHABLE();
        }
        value_del(lhs);
        value_del(rhs);
        assert(res != NULL);
        return res;
    }
    }

    UNREACHABLE();
    return NULL;
}
