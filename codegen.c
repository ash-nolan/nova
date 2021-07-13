// Copyright 2021 The Nova Project Authors
// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "nova.h"

static struct autil_string* out = NULL;
static struct tir_function const* current_function = NULL;
static size_t unique_id = 0; // Used for generating unique names and labels.

static void
append(char const* fmt, ...);
static void
appendln(char const* fmt, ...);
static void
appendli(char const* fmt, ...);
static void
appendch(char ch);

static char*
address_to_new_cstr(struct address const* address);

// All push_* functions align rsp to an 8-byte boundary.
static void
push(size_t size);
static void
push_value(struct value const* value);
static void
push_address(struct address const* address);
static void
push_at_address(size_t size, struct address const* address);
// The pop function will round size up to an 8-byte boundary to match the push_*
// functions so that one push/pop pair will restore the stack to it's previous
// state.
static void
pop(size_t size);

static void
append(char const* fmt, ...)
{
    assert(out != NULL);
    assert(fmt != NULL);

    va_list args;
    va_start(args, fmt);
    autil_string_append_vfmt(out, fmt, args);
    va_end(args);
}

static void
appendln(char const* fmt, ...)
{
    assert(out != NULL);
    assert(fmt != NULL);

    va_list args;
    va_start(args, fmt);
    autil_string_append_vfmt(out, fmt, args);
    va_end(args);

    autil_string_append_cstr(out, "\n");
}

static void
appendli(char const* fmt, ...)
{
    assert(out != NULL);
    assert(fmt != NULL);

    autil_string_append_cstr(out, "    ");

    va_list args;
    va_start(args, fmt);
    autil_string_append_vfmt(out, fmt, args);
    va_end(args);

    autil_string_append_cstr(out, "\n");
}

static void
appendch(char ch)
{
    assert(out != NULL);

    autil_string_append(out, &ch, 1u);
}

static char*
address_to_new_cstr(struct address const* address)
{
    assert(address != NULL);

    switch (address->kind) {
    case ADDRESS_GLOBAL:
        return autil_cstr_new_cstr(address->data.global.name);
    case ADDRESS_LOCAL:
        return autil_cstr_new_fmt("rbp + %d", address->data.local.rbp_offset);
    }

    UNREACHABLE();
}

static void
push(size_t size)
{
    if (size == 0) {
        return;
    }

    appendli("sub rsp, %#zx", ceil8z(size));
}

static void
push_value(struct value const* value)
{
    assert(value != NULL);

    if (value->type->kind == TYPE_FUNCTION) {
        struct address const address =
            address_init_global(value->data.function->name);
        push_address(&address);
        return;
    }

    autil_sbuf(uint8_t) const bytes = value_to_new_bytes(value);
    push(autil_sbuf_count(bytes));
    for (size_t i = 0; i < autil_sbuf_count(bytes); ++i) {
        appendli("mov byte [rsp + %#zx], %#x", i, (unsigned)bytes[i]);
    }
    autil_sbuf_fini(bytes);
}

static void
push_address(struct address const* address)
{
    assert(address != NULL);

    // TODO: address_to_new_cstr generated the instruction:
    //      mov rax, rbp + 16
    // which results in an `invalid register size` error from NASM. Something
    // should be done in order to unify the way addresses are handled / loaded
    // so that address_to_new_cstr (or similar) could be used here. Or maybe
    // this is fine and really we should just handle it as we do now.
    switch (address->kind) {
    case ADDRESS_GLOBAL: {
        appendli("push %s", address->data.global.name);
        break;
    }
    case ADDRESS_LOCAL: {
        appendli("mov rax, rbp");
        appendli("add rax, %d", address->data.local.rbp_offset);
        appendli("push rax");
        break;
    }
    }
}

static void
push_at_address(size_t size, struct address const* address)
{
    assert(address != NULL);

    push(size);

    char* const addr = address_to_new_cstr(address);
    // TODO: Add unit tests for signed and unsigned integers with size 1, 2, 4,
    // 8, and >8 to make sure that this cascade of mov operations on 8, then 4,
    // then 2, then 1 byte objects behaves correctly for all cases.
    size_t cur = 0u;
    while ((size - cur) >= 8u) {
        appendli("mov rax, [%s + %#zu]", addr, cur);
        appendli("mov [rsp + %#zx], rax", cur);
        cur += 8u;
    }
    if ((size - cur) >= 4u) {
        appendli("mov eax, [%s + %#zu]", addr, cur);
        appendli("mov [rsp + %#zx], eax", cur);
        cur += 4u;
        assert((size - cur) < 4u);
    }
    if ((size - cur) >= 2u) {
        appendli("mov ax, [%s + %#zu]", addr, cur);
        appendli("mov [rsp + %#zx], ax", cur);
        cur += 2u;
        assert((size - cur) < 2u);
    }
    if ((size - cur) == 1u) {
        appendli("mov al, [%s + %#zu]", addr, cur);
        appendli("mov [rsp + %#zx], al", cur);
    }
    // NOTE: In relation to the above TODO, if the cascade logic does not work,
    // then fall back to the dumb-memcpy version that it replaced (shown below).
    /*
    for (size_t i = 0; i < size; ++i) {
        appendli("mov al, [%s + %#zu]", addr, i);
        appendli("mov [rsp + %#zx], al", i);
    }
    */

    autil_xalloc(addr, AUTIL_XALLOC_FREE);
}

static void
pop(size_t size)
{
    if (size == 0) {
        return;
    }

    appendli("add rsp, %#zx", ceil8z(size));
}

static void
codegen_global_variables(void);
static void
codegen_global_functions(void);
static void
codegen_core(void);

static void
codegen_global_variable(struct symbol const* symbol);
static void
codegen_global_function(struct symbol const* symbol);

static void
codegen_stmt(struct tir_stmt const* stmt);
static void
codegen_stmt_if(struct tir_stmt const* stmt);
static void
codegen_stmt_for_range(struct tir_stmt const* stmt);
static void
codegen_stmt_for_expr(struct tir_stmt const* stmt);
static void
codegen_stmt_dump(struct tir_stmt const* stmt);
static void
codegen_stmt_return(struct tir_stmt const* stmt);
static void
codegen_stmt_assign(struct tir_stmt const* stmt);
static void
codegen_stmt_expr(struct tir_stmt const* stmt);

static void
codegen_rvalue(struct tir_expr const* expr);
static void
codegen_rvalue_identifier(struct tir_expr const* expr);
static void
codegen_rvalue_boolean(struct tir_expr const* expr);
static void
codegen_rvalue_integer(struct tir_expr const* expr);
static void
codegen_rvalue_array(struct tir_expr const* expr);
static void
codegen_rvalue_syscall(struct tir_expr const* expr);
static void
codegen_rvalue_call(struct tir_expr const* expr);
static void
codegen_rvalue_index(struct tir_expr const* expr);
static void
codegen_rvalue_unary(struct tir_expr const* expr);
static void
codegen_rvalue_binary(struct tir_expr const* expr);
static void
codegen_lvalue(struct tir_expr const* expr);

static void
codegen_global_variables(void)
{
    trace(NO_PATH, NO_LINE, "%s", __func__);

    appendln("; GLOBAL VARIABLES");
    appendln("section .data");
    struct symbol const* const* const symbols =
        context()->global_symbol_table->symbols;
    for (size_t i = 0; i < autil_sbuf_count(symbols); ++i) {
        struct symbol const* const symbol = symbols[i];
        if (symbol->kind != SYMBOL_VARIABLE) {
            continue;
        }
        codegen_global_variable(symbol);
    }
}

static void
codegen_global_functions(void)
{
    trace(NO_PATH, NO_LINE, "%s", __func__);

    appendln("; GLOBAL FUNCTIONS");
    appendln("section .text");
    struct symbol const* const* const symbols =
        context()->global_symbol_table->symbols;
    for (size_t i = 0; i < autil_sbuf_count(symbols); ++i) {
        struct symbol const* const symbol = symbols[i];
        if (symbol->kind != SYMBOL_FUNCTION) {
            continue;
        }
        codegen_global_function(symbol);
    }
}

static void
codegen_core(void)
{
    trace(NO_PATH, NO_LINE, "%s", __func__);

    appendln("; BUILTIN DUMP SUBROUTINE");
    appendln("section .text");
    appendln("global dump");
    appendln("dump:");
    appendln("    push rbp");
    appendln("    mov rbp, rsp");
    appendch('\n');
    appendln("    mov r15, [rbp + 0x10] ; r15 = size");
    appendch('\n');
    appendln("    cmp r15, 0");
    appendln("    jne .setup");
    appendln("    mov rax, 1 ; SYS_WRITE");
    appendln("    mov rdi, 2 ; STDERR_FILENO");
    appendln("    mov rsi, __dump_nl");
    appendln("    mov rdx, 1");
    appendln("    syscall");
    appendln("    mov rsp, rbp");
    appendln("    pop rbp");
    appendln("    ret");
    appendch('\n');
    appendln(".setup:");
    appendln("    mov r14, r15 ; r14 = size * 3");
    appendln("    imul r14, 3");
    appendln("    sub rsp, r14 ; buf = rsp = alloca(size * 3)");
    appendch('\n');
    appendln("    mov r11, rsp ; ptr = r11 = buf");
    appendln("    mov r12, rbp ; cur = r12 = &obj");
    appendln("    add r12, 0x18");
    appendln("    mov r13, r12 ; end = r13 = &obj + size");
    appendln("    add r13, r15");
    appendch('\n');
    appendln(".loop:");
    appendln("    cmp r12, r13 ; while (cur != end)");
    appendln("    je .write");
    appendch('\n');
    appendln("    mov rax, [r12] ; repr = rax = dump_lookup_table + *cur * 2");
    appendln("    and rax, 0xFF");
    appendln("    imul rax, 2");
    appendln("    add rax, __dump_lookup_table");
    appendch('\n');
    appendln("    mov bl, [rax + 0] ; *ptr = repr[0]");
    appendln("    mov [r11], bl");
    appendln("    inc r11 ; ptr += 1");
    appendln("    mov bl, [rax + 1] ; *ptr = repr[1]");
    appendln("    mov [r11], bl");
    appendln("    inc r11 ; ptr += 1");
    appendln("    mov bl, 0x20 ; *ptr = ' '");
    appendln("    mov byte [r11], bl");
    appendln("    inc r11 ; ptr += 1");
    appendch('\n');
    appendln("    inc r12 ; cur += 1");
    appendln("    jmp .loop");
    appendch('\n');
    appendln(".write:");
    appendln("    dec r11 ; ptr -= 1");
    appendln("    mov byte [r11], 0x0A ; *ptr = '\\n'");
    appendch('\n');
    appendln("    ; write(STDERR_FILENO, buf, size * 3)");
    appendln("    mov rax, 1 ; SYS_WRITE");
    appendln("    mov rdi, 2 ; STDERR_FILENO");
    appendln("    mov rsi, rsp");
    appendln("    mov rdx, r14");
    appendln("    syscall");
    appendch('\n');
    appendln("    mov rsp, rbp");
    appendln("    pop rbp");
    appendln("    ret");
    appendch('\n');
    appendln("section .rodata");
    appendln("__dump_nl: db 0x0A");
    appendln("__dump_lookup_table: db \\");
    appendln("    '00', '01', '02', '03', '04', '05', '06', '07', \\");
    appendln("    '08', '09', '0A', '0B', '0C', '0D', '0E', '0F', \\");
    appendln("    '10', '11', '12', '13', '14', '15', '16', '17', \\");
    appendln("    '18', '19', '1A', '1B', '1C', '1D', '1E', '1F', \\");
    appendln("    '20', '21', '22', '23', '24', '25', '26', '27', \\");
    appendln("    '28', '29', '2A', '2B', '2C', '2D', '2E', '2F', \\");
    appendln("    '30', '31', '32', '33', '34', '35', '36', '37', \\");
    appendln("    '38', '39', '3A', '3B', '3C', '3D', '3E', '3F', \\");
    appendln("    '40', '41', '42', '43', '44', '45', '46', '47', \\");
    appendln("    '48', '49', '4A', '4B', '4C', '4D', '4E', '4F', \\");
    appendln("    '50', '51', '52', '53', '54', '55', '56', '57', \\");
    appendln("    '58', '59', '5A', '5B', '5C', '5D', '5E', '5F', \\");
    appendln("    '60', '61', '62', '63', '64', '65', '66', '67', \\");
    appendln("    '68', '69', '6A', '6B', '6C', '6D', '6E', '6F', \\");
    appendln("    '70', '71', '72', '73', '74', '75', '76', '77', \\");
    appendln("    '78', '79', '7A', '7B', '7C', '7D', '7E', '7F', \\");
    appendln("    '80', '81', '82', '83', '84', '85', '86', '87', \\");
    appendln("    '88', '89', '8A', '8B', '8C', '8D', '8E', '8F', \\");
    appendln("    '90', '91', '92', '93', '94', '95', '96', '97', \\");
    appendln("    '98', '99', '9A', '9B', '9C', '9D', '9E', '9F', \\");
    appendln("    'A0', 'A1', 'A2', 'A3', 'A4', 'A5', 'A6', 'A7', \\");
    appendln("    'A8', 'A9', 'AA', 'AB', 'AC', 'AD', 'AE', 'AF', \\");
    appendln("    'B0', 'B1', 'B2', 'B3', 'B4', 'B5', 'B6', 'B7', \\");
    appendln("    'B8', 'B9', 'BA', 'BB', 'BC', 'BD', 'BE', 'BF', \\");
    appendln("    'C0', 'C1', 'C2', 'C3', 'C4', 'C5', 'C6', 'C7', \\");
    appendln("    'C8', 'C9', 'CA', 'CB', 'CC', 'CD', 'CE', 'CF', \\");
    appendln("    'D0', 'D1', 'D2', 'D3', 'D4', 'D5', 'D6', 'D7', \\");
    appendln("    'D8', 'D9', 'DA', 'DB', 'DC', 'DD', 'DE', 'DF', \\");
    appendln("    'E0', 'E1', 'E2', 'E3', 'E4', 'E5', 'E6', 'E7', \\");
    appendln("    'E8', 'E9', 'EA', 'EB', 'EC', 'ED', 'EE', 'EF', \\");
    appendln("    'F0', 'F1', 'F2', 'F3', 'F4', 'F5', 'F6', 'F7', \\");
    appendln("    'F8', 'F9', 'FA', 'FB', 'FC', 'FD', 'FE', 'FF'");
    appendch('\n');

    appendln("; PROGRAM ENTRY POINT");
    appendln("section .text");
    appendln("global _start");
    appendln("_start:");
    appendli("call main");
    appendli("mov rax, 60 ; exit");
    appendli("mov rdi, 0 ; EXIT_SUCCESS");
    appendli("syscall");
}

static void
codegen_global_variable(struct symbol const* symbol)
{
    assert(symbol != NULL);
    assert(symbol->kind == SYMBOL_VARIABLE);
    assert(symbol->value != NULL);
    trace(NO_PATH, NO_LINE, "%s (%s)", __func__, symbol->name);

    struct value const* const value = symbol->value;
    struct type const* const type = value->type;
    if (type->size == 0) {
        return;
    }
    if (type->kind == TYPE_FUNCTION) {
        appendln("%s: dq %s", symbol->name, symbol->value->data.function->name);
        return;
    }

    autil_sbuf(uint8_t) const bytes = value_to_new_bytes(value);
    append("%s: db", symbol->name);
    for (size_t i = 0; i < autil_sbuf_count(bytes); ++i) {
        if (i != 0) {
            appendch(',');
        }
        append(" %#x", (unsigned)bytes[i]);
    }
    appendch('\n');
    autil_sbuf_fini(bytes);
    return;

    switch (type->kind) {
    case TYPE_VOID:
        UNREACHABLE();
    case TYPE_BOOL: {
        assert(value->type->kind == TYPE_BOOL);
        char const* const value_cstr = value->data.boolean ? "0x01" : "0x00";
        appendln("%s: db %s", symbol->name, value_cstr);
        break;
    }
    case TYPE_USIZE: /* fallthrough */
    case TYPE_SSIZE: {
        // TODO: Use a goto to jump to the correct label based on the ARCH's
        // natural word size. Currently this is hard-coded for x64 as dq data.
        assert(type_is_integer(value->type));
        char* const value_cstr =
            autil_bigint_to_new_cstr(value->data.integer, NULL);
        appendln("%s: dq %s", symbol->name, value_cstr);
        autil_xalloc(value_cstr, AUTIL_XALLOC_FREE);
        break;
    }
    case TYPE_FUNCTION: {
        appendln("%s: dq %s", symbol->name, symbol->value->data.function->name);
        break;
    }
    default:
        UNREACHABLE();
    }
}

static void
codegen_global_function(struct symbol const* symbol)
{
    assert(symbol != NULL);
    assert(symbol->kind == SYMBOL_FUNCTION);
    assert(symbol->value != NULL);
    trace(NO_PATH, NO_LINE, "%s (%s)", __func__, symbol->name);

    assert(symbol->value->type->kind == TYPE_FUNCTION);
    struct tir_function const* const function = symbol->value->data.function;

    appendln("global %s", function->name);
    appendln("%s:", function->name);
    appendli("; PROLOGUE");
    // Save previous frame pointer.
    // With this push, the stack should now be 16-byte aligned.
    appendli("push rbp");
    // Move stack pointer into current frame pointer.
    appendli("mov rbp, rsp");
    // Adjust the stack pointer to make space for locals.
    assert(function->local_stack_offset <= 0);
    appendli("add rsp, %d", function->local_stack_offset);

    assert(current_function == NULL);
    current_function = function;
    for (size_t i = 0; i < autil_sbuf_count(function->body->stmts); ++i) {
        struct tir_stmt const* const stmt = function->body->stmts[i];
        codegen_stmt(stmt);
    }
    current_function = NULL;

    appendli("; END-OF-FUNCTION");
    if (function->type->data.function.return_type == context()->builtin.void_) {
        appendli("; EPILOGUE (implicit-return)");
        appendli("mov rsp, rbp");
        appendli("pop rbp");
        appendli("ret");
    }
    else {
        appendli("; Segfault if no return statement occured.");
        appendli("mov r15, [0x0000000000000000]");
    }
    appendch('\n');
}

static void
codegen_stmt(struct tir_stmt const* stmt)
{
    assert(stmt != NULL);
    trace(NO_PATH, NO_LINE, "%s", __func__);

    appendli(
        "; [%s:%zu] statement", stmt->location->path, stmt->location->line);
    switch (stmt->kind) {
    case TIR_STMT_IF: {
        codegen_stmt_if(stmt);
        return;
    }
    case TIR_STMT_FOR_RANGE: {
        codegen_stmt_for_range(stmt);
        return;
    }
    case TIR_STMT_FOR_EXPR: {
        codegen_stmt_for_expr(stmt);
        return;
    }
    case TIR_STMT_DUMP: {
        codegen_stmt_dump(stmt);
        return;
    }
    case TIR_STMT_RETURN: {
        codegen_stmt_return(stmt);
        return;
    }
    case TIR_STMT_ASSIGN: {
        codegen_stmt_assign(stmt);
        return;
    }
    case TIR_STMT_EXPR: {
        codegen_stmt_expr(stmt);
        return;
    }
    }

    UNREACHABLE();
}

static void
codegen_stmt_if(struct tir_stmt const* stmt)
{
    assert(stmt != NULL);
    assert(stmt->kind == TIR_STMT_IF);
    trace(NO_PATH, NO_LINE, "%s", __func__);

    size_t const stmt_id = unique_id++;
    autil_sbuf(struct tir_conditional const* const) const conditionals =
        stmt->data.if_.conditionals;
    appendln(".l%zu_stmt_if_bgn:", stmt_id);
    for (size_t i = 0; i < autil_sbuf_count(conditionals); ++i) {
        bool const is_last = i == (autil_sbuf_count(conditionals) - 1);

        if (conditionals[i]->condition != NULL) {
            appendln(".l%zu_stmt_if_%zu_condition:", stmt_id, i);
            assert(conditionals[i]->condition->type->kind == TYPE_BOOL);
            codegen_rvalue(conditionals[i]->condition);
            appendli("pop rax");
            appendli("mov rbx, 0x00");
            appendli("cmp rax, rbx");
            if (is_last) {
                appendli("je .l%zu_stmt_if_end", stmt_id);
            }
            else {
                appendli("je .l%zu_stmt_if_%zu_condition", stmt_id, i + 1);
            }
        }
        else {
            appendln(".l%zu_stmt_if_%zu_condition:", stmt_id, i);
            appendli("; else condition (always true)");
        }

        appendln(".l%zu_stmt_if_%zu_body:", stmt_id, i);
        autil_sbuf(struct tir_stmt const* const) const stmts =
            conditionals[i]->body->stmts;
        for (size_t i = 0; i < autil_sbuf_count(stmts); ++i) {
            codegen_stmt(stmts[i]);
        }
        appendli("jmp .l%zu_stmt_if_end", stmt_id);
    }
    appendln(".l%zu_stmt_if_end:", stmt_id);
}

static void
codegen_stmt_for_range(struct tir_stmt const* stmt)
{
    assert(stmt != NULL);
    assert(stmt->kind == TIR_STMT_FOR_RANGE);
    trace(NO_PATH, NO_LINE, "%s", __func__);

    assert(
        stmt->data.for_range.loop_variable->type == context()->builtin.usize);
    assert(stmt->data.for_range.begin->type == context()->builtin.usize);
    assert(stmt->data.for_range.end->type == context()->builtin.usize);
    assert(stmt->data.for_range.loop_variable->address->kind == ADDRESS_LOCAL);
    size_t const stmt_id = unique_id++;
    appendln(".l%zu_stmt_for_range_bgn:", stmt_id);
    push_address(stmt->data.for_range.loop_variable->address);
    codegen_rvalue(stmt->data.for_range.begin);
    appendli("pop rbx"); // begin
    appendli("pop rax"); // addr of loop variable
    appendli("mov [rax], rbx");
    appendln(".l%zu_stmt_for_range_condition:", stmt_id);
    push_at_address(
        stmt->data.for_range.loop_variable->type->size,
        stmt->data.for_range.loop_variable->address);
    codegen_rvalue(stmt->data.for_range.end);
    appendli("pop rbx"); // end
    appendli("pop rax"); // loop variable
    appendli("cmp rax, rbx");
    appendli("je .l%zu_stmt_for_range_end", stmt_id);
    appendln(".l%zu_stmt_for_range_body:", stmt_id);
    autil_sbuf(struct tir_stmt const* const) const stmts =
        stmt->data.for_range.body->stmts;
    for (size_t i = 0; i < autil_sbuf_count(stmts); ++i) {
        codegen_stmt(stmts[i]);
    }
    appendli(
        "inc qword [rbp + %d]",
        stmt->data.for_range.loop_variable->address->data.local.rbp_offset);
    appendli("jmp .l%zu_stmt_for_range_condition", stmt_id);
    appendln(".l%zu_stmt_for_range_end:", stmt_id);
}

static void
codegen_stmt_for_expr(struct tir_stmt const* stmt)
{
    assert(stmt != NULL);
    assert(stmt->kind == TIR_STMT_FOR_EXPR);
    trace(NO_PATH, NO_LINE, "%s", __func__);

    size_t const stmt_id = unique_id++;
    appendln(".l%zu_stmt_for_expr_bgn:", stmt_id);
    appendln(".l%zu_stmt_for_expr_condition:", stmt_id);
    assert(stmt->data.for_expr.expr->type->kind == TYPE_BOOL);
    codegen_rvalue(stmt->data.for_expr.expr);
    appendli("pop rax");
    appendli("mov rbx, 0x00");
    appendli("cmp rax, rbx");
    appendli("je .l%zu_stmt_for_expr_end", stmt_id);
    appendln(".l%zu_stmt_for_expr_body:", stmt_id);
    autil_sbuf(struct tir_stmt const* const) const stmts =
        stmt->data.for_expr.body->stmts;
    for (size_t i = 0; i < autil_sbuf_count(stmts); ++i) {
        codegen_stmt(stmts[i]);
    }
    appendli("jmp .l%zu_stmt_for_expr_condition", stmt_id);
    appendln(".l%zu_stmt_for_expr_end:", stmt_id);
}

static void
codegen_stmt_dump(struct tir_stmt const* stmt)
{
    assert(stmt != NULL);
    assert(stmt->kind == TIR_STMT_DUMP);
    trace(NO_PATH, NO_LINE, "%s", __func__);

    codegen_rvalue(stmt->data.expr);
    appendli("push %#zx", stmt->data.expr->type->size);
    appendli("call dump");
    appendli("pop rax", stmt->data.expr->type->size);
    pop(stmt->data.expr->type->size);
}

static void
codegen_stmt_return(struct tir_stmt const* stmt)
{
    assert(stmt != NULL);
    assert(stmt->kind == TIR_STMT_RETURN);
    trace(NO_PATH, NO_LINE, "%s", __func__);

    if (stmt->data.return_.expr != NULL) {
        // Compute result.
        codegen_rvalue(stmt->data.return_.expr);

        // Store in return address.
        struct symbol const* const return_symbol = symbol_table_lookup(
            current_function->symbol_table, context()->interned.return_);
        appendli("pop rax");
        assert(return_symbol->address->kind == ADDRESS_LOCAL);
        char* const addr = address_to_new_cstr(return_symbol->address);
        appendli("mov [%s], rax", addr);
        autil_xalloc(addr, AUTIL_XALLOC_FREE);
    }

    appendli("; EPILOGUE");
    // Restore stack pointer.
    appendli("mov rsp, rbp");
    // Restore previous frame pointer.
    appendli("pop rbp");
    // Return control to the calling routine.
    appendli("ret");
}

static void
codegen_stmt_assign(struct tir_stmt const* stmt)
{
    assert(stmt != NULL);
    assert(stmt->kind == TIR_STMT_ASSIGN);
    trace(NO_PATH, NO_LINE, "%s", __func__);

    codegen_rvalue(stmt->data.assign.rhs);
    codegen_lvalue(stmt->data.assign.lhs);

    appendli("pop rbx");
    size_t const size = stmt->data.assign.rhs->type->size;
    // TODO: Add unit tests for signed and unsigned integers with size 1, 2, 4,
    // 8, and >8 to make sure that this cascade of mov operations on 8, then 4,
    // then 2, then 1 byte objects behaves correctly for all cases.
    size_t cur = 0u;
    while ((size - cur) >= 8u) {
        appendli("mov rax, [rsp + %#zu]", cur);
        appendli("mov [rbx + %#zx], rax", cur);
        cur += 8u;
    }
    if ((size - cur) >= 4u) {
        appendli("mov eax, [rsp + %#zu]", cur);
        appendli("mov [rbx + %#zx], eax", cur);
        cur += 4u;
        assert((size - cur) < 4u);
    }
    if ((size - cur) >= 2u) {
        appendli("mov ax, [rsp + %#zu]", cur);
        appendli("mov [rbx + %#zx], ax", cur);
        cur += 2u;
        assert((size - cur) < 2u);
    }
    if ((size - cur) == 1u) {
        appendli("mov al, [rsp + %#zu]", cur);
        appendli("mov [rbx + %#zx], al", cur);
    }
    // NOTE: In relation to the above TODO, if the cascade logic does not work,
    // then fall back to the dumb-memcpy version that it replaced (shown below).
    /*
    for (size_t i = 0; i < size; ++i) {
        appendli("mov al, [rsp + %#zu]", i);
        appendli("mov [rbx + %#zx], al", i);
    }
    */
    pop(size);
}

static void
codegen_stmt_expr(struct tir_stmt const* stmt)
{
    assert(stmt != NULL);
    assert(stmt->kind == TIR_STMT_EXPR);
    trace(NO_PATH, NO_LINE, "%s", __func__);

    codegen_rvalue(stmt->data.expr);
    // Remove the (unused) result from the stack.
    pop(stmt->data.expr->type->size);
}

static void
codegen_rvalue(struct tir_expr const* expr)
{
    assert(expr != NULL);
    trace(NO_PATH, NO_LINE, "%s", __func__);

    switch (expr->kind) {
    case TIR_EXPR_IDENTIFIER: {
        codegen_rvalue_identifier(expr);
        return;
    }
    case TIR_EXPR_BOOLEAN: {
        codegen_rvalue_boolean(expr);
        return;
    }
    case TIR_EXPR_INTEGER: {
        codegen_rvalue_integer(expr);
        return;
    }
    case TIR_EXPR_ARRAY: {
        codegen_rvalue_array(expr);
        return;
    }
    case TIR_EXPR_SYSCALL: {
        codegen_rvalue_syscall(expr);
        return;
    }
    case TIR_EXPR_CALL: {
        codegen_rvalue_call(expr);
        return;
    }
    case TIR_EXPR_INDEX: {
        codegen_rvalue_index(expr);
        return;
    }
    case TIR_EXPR_UNARY: {
        codegen_rvalue_unary(expr);
        return;
    }
    case TIR_EXPR_BINARY: {
        codegen_rvalue_binary(expr);
        return;
    }
    }

    UNREACHABLE();
}

static void
codegen_rvalue_identifier(struct tir_expr const* expr)
{
    assert(expr != NULL);
    assert(expr->kind == TIR_EXPR_IDENTIFIER);
    trace(NO_PATH, NO_LINE, "%s", __func__);

    struct symbol const* const symbol = expr->data.identifier;
    switch (symbol->kind) {
    case SYMBOL_TYPE: {
        UNREACHABLE();
    }
    case SYMBOL_VARIABLE: {
        push_at_address(symbol->type->size, symbol->address);
        return;
    }
    case SYMBOL_CONSTANT: {
        push_value(symbol->value);
        return;
    }
    case SYMBOL_FUNCTION: {
        push_address(symbol->address);
        return;
    }
    }

    UNREACHABLE();
}

static void
codegen_rvalue_boolean(struct tir_expr const* expr)
{
    assert(expr != NULL);
    assert(expr->kind == TIR_EXPR_BOOLEAN);
    trace(NO_PATH, NO_LINE, "%s", __func__);

    appendli("mov rax, %s", expr->data.boolean ? "0x01" : "0x00");
    appendli("push rax");
}

static void
codegen_rvalue_integer(struct tir_expr const* expr)
{
    assert(expr != NULL);
    assert(expr->kind == TIR_EXPR_INTEGER);
    trace(NO_PATH, NO_LINE, "%s", __func__);

    char* const cstr = autil_bigint_to_new_cstr(expr->data.integer, NULL);

    assert(expr->type->size == 8);
    appendli("mov rax, %s", cstr);
    appendli("push rax");

    autil_xalloc(cstr, AUTIL_XALLOC_FREE);
}

static void
codegen_rvalue_array(struct tir_expr const* expr)
{
    assert(expr != NULL);
    assert(expr->kind == TIR_EXPR_ARRAY);
    assert(expr->type->kind == TYPE_ARRAY);
    trace(NO_PATH, NO_LINE, "%s", __func__);

    // Make space for the array.
    push(expr->type->size);

    // One by one evaluate the rvalues for the elements of the array. Each
    // element will be at the top of the stack after being evaluated, so the
    // element is manually memcpy-ed into the correct position on the stack.
    // This process feels like it would be somewhat slow, but unfortunately it
    // seems necessary in order to keep the left-to-right evaluation order of
    // array elements. Additionally pushing/popping to and from the stack uses
    // 8-byte alignment, but arrays may have element alignment that does not
    // cleanly match the stack alignment (e.g. [count]bool).
    autil_sbuf(struct tir_expr const* const) const elements =
        expr->data.array.elements;
    struct type const* const element_type = expr->type->data.array.base;
    size_t const element_size = element_type->size;
    // TODO: This loop is manually unrolled here, but should probably be turned
    // into an actual asm loop for elements with trivial initialization.
    // Basically we should account for the scenario where a brainfuck
    // interpreter allocates an array 30000 zeroed bytes, which would cause the
    // equivalent of memcpy(&my_array[index], &my_zero, 0x8) inline thousands of
    // times.
    for (size_t i = 0; i < autil_sbuf_count(elements); ++i) {
        assert(elements[i]->type == element_type);
        codegen_rvalue(elements[i]);

        // TODO: Replace this byte-by-byte memcpy with a cascading memcpy. Or
        // perhaps we should add some `copy` function that takes a source and a
        // destination address and does this for us? Maybe use it for assign
        // stmts as well?
        appendli("mov rbx, rsp");
        appendli("add rbx, %zu", ceil8z(element_size)); // array start
        appendli("add rbx, %zu", element_size * i); // array index
        for (size_t ii = 0; ii < element_size; ++ii) {
            appendli("mov al, [rsp + %#zu]", ii);
            appendli("mov [rbx + %#zx], al", ii);
        }

        pop(element_size);
    }
}

static void
codegen_rvalue_syscall(struct tir_expr const* expr)
{
    assert(expr != NULL);
    assert(expr->kind == TIR_EXPR_SYSCALL);
    trace(NO_PATH, NO_LINE, "%s", __func__);

    struct tir_expr const* const* const exprs = expr->data.syscall.exprs;
    size_t const count = autil_sbuf_count(exprs);
    for (size_t i = 0; i < count; ++i) {
        codegen_rvalue(exprs[i]);
    }

    assert(count != 0);
    assert(count <= 7);
    if (count == 7) {
        appendli("pop r9 ; syscall parameter 6");
    }
    if (count >= 6) {
        appendli("pop r8 ; syscall parameter 5");
    }
    if (count >= 5) {
        appendli("pop r10 ; syscall parameter 4");
    }
    if (count >= 4) {
        appendli("pop rdx ; syscall parameter 3");
    }
    if (count >= 3) {
        appendli("pop rsi ; syscall parameter 2");
    }
    if (count >= 2) {
        appendli("pop rdi ; syscall parameter 1");
    }
    if (count >= 1) {
        appendli("pop rax ; syscall number");
    }
    appendli("syscall");
    appendli("push rax ; syscall result");
}

static void
codegen_rvalue_call(struct tir_expr const* expr)
{
    assert(expr != NULL);
    assert(expr->kind == TIR_EXPR_CALL);
    trace(NO_PATH, NO_LINE, "%s", __func__);

    // Push space for return value.
    struct type const* function_type = expr->data.call.function->type;
    assert(function_type->kind == TYPE_FUNCTION);
    struct type const* return_type = function_type->data.function.return_type;
    if (return_type->size != 0) {
        assert(return_type->size <= 8);
        appendli("mov rax, 0");
        appendli("push rax");
    }
    // Evaluate & push arguments from left to right.
    struct tir_expr const* const* const arguments = expr->data.call.arguments;
    for (size_t i = 0; i < autil_sbuf_count(arguments); ++i) {
        codegen_rvalue(arguments[i]);
    }

    // Load the function pointer and call the function.
    codegen_rvalue(expr->data.call.function);
    appendli("pop rax");
    appendli("call rax");

    // Pop arguments from right to left, leaving the return value as the top
    // element on the stack (for return values with non-zero size).
    for (size_t i = autil_sbuf_count(arguments); i--;) {
        appendli("pop rax");
    }
}

static void
codegen_rvalue_index(struct tir_expr const* expr)
{
    assert(expr != NULL);
    assert(expr->kind == TIR_EXPR_INDEX);
    assert(expr->data.index.lhs->type->kind == TYPE_ARRAY);
    assert(expr->data.index.idx->type->kind == TYPE_USIZE);
    trace(NO_PATH, NO_LINE, "%s", __func__);

    struct type const* const lhs_type = expr->data.index.lhs->type;
    struct type const* const element_type = lhs_type->data.array.base;

    // Push space for result.
    assert(expr->type == element_type);
    push(expr->type->size);

    if (tir_expr_is_lvalue(expr->data.index.lhs)) {
        // Array expression is an lvalue. Compute the address of the of the
        // indexed element and copy from that address into the result.
        codegen_lvalue(expr->data.index.lhs);
        codegen_rvalue(expr->data.index.idx);
        // rax := source
        // rsp := destination
        // After calculating the source address the stack pointer will point to
        // the result since space for the result space was pushed onto the
        // stack.
        appendli("pop rax"); // index
        appendli("mov rbx, %zu", element_type->size);
        appendli("mul rbx"); // index * sizeof(element_type)
        appendli("pop rbx"); // start
        appendli("add rax, rbx"); // start + index * sizeof(element_type)
        // copy
        for (size_t i = 0; i < element_type->size; ++i) {
            appendli("mov cl, [rax + %#zu]", i);
            appendli("mov [rsp + %#zx], cl", i);
        }

        return;
    }

    // Array expression is an rvalue. Generate the rvalue array and rvalue
    // index. Then copy indexed element into the result.
    codegen_rvalue(expr->data.index.lhs);
    codegen_rvalue(expr->data.index.idx);
    // rax := source
    appendli("pop rax"); // index
    appendli("mov rbx, %zu", element_type->size);
    appendli("mul rbx"); // index * sizeof(element_type)
    appendli("add rax, rsp"); // start + index * sizeof(element_type)
    // rbx := destination
    appendli("mov rbx, %zu", lhs_type->size); // sizeof(array)
    appendli("add rbx, rsp", lhs_type); // start  + sizeof(array)
    // copy
    for (size_t i = 0; i < element_type->size; ++i) {
        appendli("mov cl, [rax + %#zu]", i);
        appendli("mov [rbx + %#zx], cl", i);
    }

    // Pop array rvalue.
    pop(lhs_type->size);
}

static void
codegen_rvalue_unary(struct tir_expr const* expr)
{
    assert(expr != NULL);
    assert(expr->kind == TIR_EXPR_UNARY);
    trace(NO_PATH, NO_LINE, "%s", __func__);

    codegen_rvalue(expr->data.unary.rhs);
    switch (expr->data.unary.op) {
    case UOP_NOT: {
        appendli("pop rax");
        appendli("mov rbx, 0");
        appendli("cmp rax, rbx");
        appendli("setz al");
        appendli("push rax");
    }
    case UOP_POS: {
        /* nothing */
        return;
    }
    case UOP_NEG: {
        appendli("pop rax");
        appendli("neg rax");
        appendli("push rax");
        return;
    }
    }

    UNREACHABLE();
}

static void
codegen_rvalue_binary(struct tir_expr const* expr)
{
    assert(expr != NULL);
    assert(expr->kind == TIR_EXPR_BINARY);
    trace(NO_PATH, NO_LINE, "%s", __func__);

    switch (expr->data.binary.op) {
    case BOP_OR: {
        assert(expr->data.binary.lhs->type->kind == TYPE_BOOL);
        assert(expr->data.binary.rhs->type->kind == TYPE_BOOL);
        assert(expr->data.binary.lhs->type->size == 1u);
        assert(expr->data.binary.rhs->type->size == 1u);
        size_t const id = unique_id++;

        // TODO: Remove redundant jumps.
        appendln(".l%zu_binary_or_lhs:", id);
        codegen_rvalue(expr->data.binary.lhs);
        appendli("pop rax");
        appendli("mov rbx, 0x00");
        appendli("cmp rax, rbx");
        appendli("jne .l%zu_binary_or_true", id);
        appendli("jmp .l%zu_binary_or_rhs", id);

        appendln(".l%zu_binary_or_rhs:", id);
        codegen_rvalue(expr->data.binary.rhs);
        appendli("pop rax");
        appendli("mov rbx, 0x00");
        appendli("cmp rax, rbx");
        appendli("jne .l%zu_binary_or_true", id);
        appendli("jmp .l%zu_binary_or_end", id);

        appendln(".l%zu_binary_or_true:", id);
        appendli("push 0x01");
        appendli("jmp .l%zu_binary_or_end", id);

        appendln(".l%zu_binary_or_false:", id);
        appendli("push 0x00");
        appendli("jmp .l%zu_binary_or_end", id);

        appendln(".l%zu_binary_or_end:", id);
        return;
    }
    case BOP_AND: {
        assert(expr->data.binary.lhs->type->kind == TYPE_BOOL);
        assert(expr->data.binary.rhs->type->kind == TYPE_BOOL);
        assert(expr->data.binary.lhs->type->size == 1u);
        assert(expr->data.binary.rhs->type->size == 1u);
        size_t const id = unique_id++;

        // TODO: Remove redundant jumps.
        appendln(".l%zu_binary_and_lhs:", id);
        codegen_rvalue(expr->data.binary.lhs);
        appendli("pop rax");
        appendli("mov rbx, 0x00");
        appendli("cmp rax, rbx");
        appendli("jne .l%zu_binary_and_rhs", id);
        appendli("jmp .l%zu_binary_and_false", id);

        appendln(".l%zu_binary_and_rhs:", id);
        codegen_rvalue(expr->data.binary.rhs);
        appendli("pop rax");
        appendli("mov rbx, 0x00");
        appendli("cmp rax, rbx");
        appendli("jne .l%zu_binary_and_true", id);
        appendli("jmp .l%zu_binary_and_false", id);

        appendln(".l%zu_binary_and_true:", id);
        appendli("push 0x01");
        appendli("jmp .l%zu_binary_and_end", id);

        appendln(".l%zu_binary_and_false:", id);
        appendli("push 0x00");
        appendli("jmp .l%zu_binary_and_end", id);

        appendln(".l%zu_binary_and_end:", id);
        return;
    }
    case BOP_EQ: {
    bop_eq:
        assert(expr->data.binary.lhs->type->size <= 8u);
        assert(expr->data.binary.rhs->type->size <= 8u);
        codegen_rvalue(expr->data.binary.lhs);
        codegen_rvalue(expr->data.binary.rhs);
        appendli("pop rbx");
        appendli("pop rax");
        appendli("mov rcx, 0"); // result (default false)
        appendli("mov rdx, 1"); // register holding true
        appendli("cmp rax, rbx");
        appendli("cmove rcx, rdx");
        appendli("push rcx");
        return;
    }
    case BOP_NE: {
        assert(expr->data.binary.lhs->type->size <= 8u);
        assert(expr->data.binary.rhs->type->size <= 8u);
        codegen_rvalue(expr->data.binary.lhs);
        codegen_rvalue(expr->data.binary.rhs);
        appendli("pop rbx");
        appendli("pop rax");
        appendli("mov rcx, 0"); // result (default false)
        appendli("mov rdx, 1"); // register holding true
        appendli("cmp rax, rbx");
        appendli("cmovne rcx, rdx");
        appendli("push rcx");
        return;
    }
    case BOP_LE: {
        assert(expr->data.binary.lhs->type->size <= 8u);
        assert(expr->data.binary.rhs->type->size <= 8u);
        codegen_rvalue(expr->data.binary.lhs);
        codegen_rvalue(expr->data.binary.rhs);
        if (expr->data.binary.lhs->type->kind == TYPE_FUNCTION) {
            assert(expr->data.binary.rhs->type->kind == TYPE_FUNCTION);
            goto bop_eq;
        }
        appendli("pop rbx");
        appendli("pop rax");
        appendli("mov rcx, 0"); // result (default false)
        appendli("mov rdx, 1"); // register holding true
        appendli("cmp rax, rbx");
        appendli("cmovle rcx, rdx");
        appendli("push rcx");
        return;
    }
    case BOP_LT: {
        assert(expr->data.binary.lhs->type->size <= 8u);
        assert(expr->data.binary.rhs->type->size <= 8u);
        codegen_rvalue(expr->data.binary.lhs);
        codegen_rvalue(expr->data.binary.rhs);
        if (expr->data.binary.lhs->type->kind == TYPE_FUNCTION) {
            assert(expr->data.binary.rhs->type->kind == TYPE_FUNCTION);
            appendli("pop rax");
            appendli("pop rax");
            appendli("push 0"); // func < func == false
            return;
        }
        appendli("pop rbx");
        appendli("pop rax");
        appendli("mov rcx, 0"); // result (default false)
        appendli("mov rdx, 1"); // register holding true
        appendli("cmp rax, rbx");
        appendli("cmovl rcx, rdx");
        appendli("push rcx");
        return;
    }
    case BOP_GE: {
        assert(expr->data.binary.lhs->type->size <= 8u);
        assert(expr->data.binary.rhs->type->size <= 8u);
        codegen_rvalue(expr->data.binary.lhs);
        codegen_rvalue(expr->data.binary.rhs);
        if (expr->data.binary.lhs->type->kind == TYPE_FUNCTION) {
            assert(expr->data.binary.rhs->type->kind == TYPE_FUNCTION);
            goto bop_eq;
        }
        appendli("pop rbx");
        appendli("pop rax");
        appendli("mov rcx, 0"); // result (default false)
        appendli("mov rdx, 1"); // register holding true
        appendli("cmp rax, rbx");
        appendli("cmovge rcx, rdx");
        appendli("push rcx");
        return;
    }
    case BOP_GT: {
        assert(expr->data.binary.lhs->type->size <= 8u);
        assert(expr->data.binary.rhs->type->size <= 8u);
        codegen_rvalue(expr->data.binary.lhs);
        codegen_rvalue(expr->data.binary.rhs);
        if (expr->data.binary.lhs->type->kind == TYPE_FUNCTION) {
            assert(expr->data.binary.rhs->type->kind == TYPE_FUNCTION);
            appendli("pop rax");
            appendli("pop rax");
            appendli("push 0"); // func > func == false
            return;
        }
        appendli("pop rbx");
        appendli("pop rax");
        appendli("mov rcx, 0"); // result (default false)
        appendli("mov rdx, 1"); // register holding true
        appendli("cmp rax, rbx");
        appendli("cmovg rcx, rdx");
        appendli("push rcx");
        return;
    }
    case BOP_ADD: {
        assert(expr->data.binary.lhs->type->size == 8u);
        assert(expr->data.binary.rhs->type->size == 8u);
        codegen_rvalue(expr->data.binary.lhs);
        codegen_rvalue(expr->data.binary.rhs);
        appendli("pop rbx");
        appendli("pop rax");
        appendli("add rax, rbx");
        appendli("push rax");
        return;
    }
    case BOP_SUB: {
        assert(expr->data.binary.lhs->type->size == 8u);
        assert(expr->data.binary.rhs->type->size == 8u);
        codegen_rvalue(expr->data.binary.lhs);
        codegen_rvalue(expr->data.binary.rhs);
        appendli("pop rbx");
        appendli("pop rax");
        appendli("sub rax, rbx");
        appendli("push rax");
        return;
    }
    // TODO: Check mul vs imul. Does sign matter at all?
    case BOP_MUL: {
        assert(expr->data.binary.lhs->type->size == 8u);
        assert(expr->data.binary.rhs->type->size == 8u);
        codegen_rvalue(expr->data.binary.lhs);
        codegen_rvalue(expr->data.binary.rhs);
        appendli("pop rbx");
        appendli("pop rax");
        appendli("mul rbx");
        appendli("push rax");
        return;
    }
    // TODO: Check div vs idiv. Does sign matter at all?
    case BOP_DIV: {
        assert(expr->data.binary.lhs->type->size == 8u);
        assert(expr->data.binary.rhs->type->size == 8u);
        codegen_rvalue(expr->data.binary.lhs);
        codegen_rvalue(expr->data.binary.rhs);
        appendli("pop rbx");
        appendli("pop rax");
        appendli("div rbx");
        appendli("push rax");
        return;
    }
    }

    UNREACHABLE();
}

static void
codegen_lvalue(struct tir_expr const* expr)
{
    assert(expr != NULL);
    trace(NO_PATH, NO_LINE, "%s", __func__);

    switch (expr->kind) {
    case TIR_EXPR_IDENTIFIER: {
        push_address(expr->data.identifier->address);
        return;
    }
    case TIR_EXPR_INDEX: {
        codegen_lvalue(expr->data.index.lhs);
        codegen_rvalue(expr->data.index.idx);
        struct type const* const lhs_type = expr->data.index.lhs->type;
        struct type const* const element_type = lhs_type->data.array.base;
        appendli("pop rax"); // index
        appendli("mov rbx, %zu", element_type->size); // sizeof(element_type)
        appendli("mul rbx"); // index * sizeof(element_type)
        appendli("pop rbx"); // start
        appendli("add rax, rbx"); // start + index * sizeof(element_type)
        appendli("push rax");
        return;
    }
    case TIR_EXPR_BOOLEAN: /* fallthrough */
    case TIR_EXPR_INTEGER: /* fallthrough */
    case TIR_EXPR_ARRAY: /* fallthrough */
    case TIR_EXPR_SYSCALL: /* fallthrough */
    case TIR_EXPR_CALL: /* fallthrough */
    case TIR_EXPR_UNARY: /* fallthrough */
    case TIR_EXPR_BINARY: {
        UNREACHABLE();
    }
    }

    UNREACHABLE();
}

void
codegen(void)
{
    trace(NO_PATH, NO_LINE, "%s", __func__);

    out = autil_string_new(NULL, 0u);

    codegen_global_variables();
    appendch('\n');
    codegen_global_functions();
    appendch('\n');
    codegen_core();

    autil_file_write("a.asm", autil_string_start(out), autil_string_count(out));
    autil_string_del(out);

    // clang-format off
    char const* const nasm_argv[] = {
        "nasm", "-w+error=all", "-f", "elf64", "-O0", "-g", "-F", "dwarf",
        "a.asm", (char const*)NULL
    };
    // clang-format on
    xspawnvpw("nasm", nasm_argv);

    char const* const ld_argv[] = {"ld", "a.o", (char const*)NULL};
    xspawnvpw("ld", ld_argv);
}
