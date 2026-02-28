#include "compiler.h"
#include "chunk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool had_error = false;

static void compile_error(int line, const char *message) {
    fprintf(stderr, "[line %d] Compile error: %s\n", line, message);
    had_error = true;
}

/* --- Compiler state --- */

static void init_compiler(lisa_compiler *c, lisa_compiler *enclosing,
                          lisa_function_type type, lisa_gc *gc, const char *name) {
    c->enclosing = enclosing;
    c->type = type;
    c->gc = gc;
    c->local_count = 0;
    c->scope_depth = 0;
    c->function = lisa_new_function(gc);

    if (name != NULL) {
        c->function->name = lisa_copy_string(gc, name, (int)strlen(name));
    }

    /* Reserve slot 0 for the function itself */
    lisa_local *local = &c->locals[c->local_count++];
    local->depth = 0;
    local->is_captured = false;
    local->name = "";
    local->name_length = 0;
}

static lisa_chunk *current_chunk(lisa_compiler *c) {
    return &c->function->chunk;
}

/* --- Emit helpers --- */

static void emit_byte(lisa_compiler *c, uint8_t byte, int line) {
    lisa_chunk_write(current_chunk(c), byte, line);
}

static void emit_bytes(lisa_compiler *c, uint8_t a, uint8_t b, int line) {
    emit_byte(c, a, line);
    emit_byte(c, b, line);
}


static uint8_t make_constant(lisa_compiler *c, lisa_value value, int line) {
    int idx = lisa_chunk_add_constant(current_chunk(c), value);
    if (idx > 255) {
        compile_error(line, "Too many constants in one chunk.");
        return 0;
    }
    return (uint8_t)idx;
}

static void emit_constant(lisa_compiler *c, lisa_value value, int line) {
    emit_bytes(c, OP_CONSTANT, make_constant(c, value, line), line);
}

static int emit_jump(lisa_compiler *c, uint8_t instruction, int line) {
    emit_byte(c, instruction, line);
    emit_byte(c, 0xFF, line);
    emit_byte(c, 0xFF, line);
    return current_chunk(c)->count - 2;
}

static void patch_jump(lisa_compiler *c, int offset, int line) {
    int jump = current_chunk(c)->count - offset - 2;
    if (jump > 65535) {
        compile_error(line, "Jump too large.");
        return;
    }
    current_chunk(c)->code[offset] = (uint8_t)(jump & 0xFF);
    current_chunk(c)->code[offset + 1] = (uint8_t)((jump >> 8) & 0xFF);
}

/* --- Scope management --- */

static void begin_scope(lisa_compiler *c) {
    c->scope_depth++;
}


static void add_local(lisa_compiler *c, const char *name, int length, int line) {
    if (c->local_count >= 256) {
        compile_error(line, "Too many local variables in function.");
        return;
    }
    lisa_local *local = &c->locals[c->local_count++];
    local->name = name;
    local->name_length = length;
    local->depth = c->scope_depth;
    local->is_captured = false;
}

static int resolve_local(lisa_compiler *c, const char *name, int length) {
    for (int i = c->local_count - 1; i >= 0; i--) {
        lisa_local *local = &c->locals[i];
        if (local->name_length == length &&
            memcmp(local->name, name, (size_t)length) == 0) {
            return i;
        }
    }
    return -1;
}

static int add_upvalue(lisa_compiler *c, uint8_t index, bool is_local, int line) {
    int upvalue_count = c->function->upvalue_count;
    /* Check if we already have this upvalue */
    for (int i = 0; i < upvalue_count; i++) {
        if (c->upvalues[i].index == index && c->upvalues[i].is_local == is_local) {
            return i;
        }
    }
    if (upvalue_count >= 256) {
        compile_error(line, "Too many closure variables.");
        return 0;
    }
    c->upvalues[upvalue_count].is_local = is_local;
    c->upvalues[upvalue_count].index = index;
    return c->function->upvalue_count++;
}

static int resolve_upvalue(lisa_compiler *c, const char *name, int length, int line) {
    if (c->enclosing == NULL) return -1;

    int local = resolve_local(c->enclosing, name, length);
    if (local != -1) {
        c->enclosing->locals[local].is_captured = true;
        return add_upvalue(c, (uint8_t)local, true, line);
    }

    int upvalue = resolve_upvalue(c->enclosing, name, length, line);
    if (upvalue != -1) {
        return add_upvalue(c, (uint8_t)upvalue, false, line);
    }

    return -1;
}

/* --- Compilation --- */

static void compile_expr(lisa_compiler *c, lisa_ast *node, bool tail);

static uint8_t identifier_constant(lisa_compiler *c, const char *name, int length) {
    lisa_obj_string *str = lisa_copy_string(c->gc, name, length);
    return make_constant(c, LISA_OBJ(str), 0);
}

static void compile_symbol(lisa_compiler *c, lisa_ast *node) {
    const char *name = node->as.symbol.start;
    int length = node->as.symbol.length;
    int line = node->line;

    int local = resolve_local(c, name, length);
    if (local != -1) {
        emit_bytes(c, OP_GET_LOCAL, (uint8_t)local, line);
        return;
    }

    int upvalue = resolve_upvalue(c, name, length, line);
    if (upvalue != -1) {
        emit_bytes(c, OP_GET_UPVALUE, (uint8_t)upvalue, line);
        return;
    }

    /* Global */
    uint8_t idx = identifier_constant(c, name, length);
    emit_bytes(c, OP_GET_GLOBAL, idx, line);
}

static void compile_def(lisa_compiler *c, lisa_ast *node) {
    const char *name = node->as.def.name->as.symbol.start;
    int length = node->as.def.name->as.symbol.length;
    uint8_t global = identifier_constant(c, name, length);

    compile_expr(c, node->as.def.value, false);
    emit_bytes(c, OP_DEF_GLOBAL, global, node->line);
    /* def is an expression that produces nil */
    emit_byte(c, OP_NIL, node->line);
}

/* Check if a symbol AST matches a given string */
static bool sym_eq(lisa_ast *node, const char *s) {
    if (node->type != AST_SYMBOL) return false;
    int len = (int)strlen(s);
    return node->as.symbol.length == len &&
           memcmp(node->as.symbol.start, s, (size_t)len) == 0;
}

static void compile_call(lisa_compiler *c, lisa_ast *node, bool tail) {
    lisa_ast *callee = node->as.call.callee;
    lisa_ast_list *args = &node->as.call.args;
    int line = node->line;

    /* Built-in operators: compile to dedicated opcodes */
    if (callee->type == AST_SYMBOL) {
        /* Binary arithmetic */
        if (args->count == 2) {
            if (sym_eq(callee, "+")) {
                compile_expr(c, args->items[0], false);
                compile_expr(c, args->items[1], false);
                emit_byte(c, OP_ADD, line);
                return;
            }
            if (sym_eq(callee, "-")) {
                compile_expr(c, args->items[0], false);
                compile_expr(c, args->items[1], false);
                emit_byte(c, OP_SUB, line);
                return;
            }
            if (sym_eq(callee, "*")) {
                compile_expr(c, args->items[0], false);
                compile_expr(c, args->items[1], false);
                emit_byte(c, OP_MUL, line);
                return;
            }
            if (sym_eq(callee, "/")) {
                compile_expr(c, args->items[0], false);
                compile_expr(c, args->items[1], false);
                emit_byte(c, OP_DIV, line);
                return;
            }
            if (sym_eq(callee, "%") || sym_eq(callee, "mod")) {
                compile_expr(c, args->items[0], false);
                compile_expr(c, args->items[1], false);
                emit_byte(c, OP_MOD, line);
                return;
            }
            /* Comparisons */
            if (sym_eq(callee, "=") || sym_eq(callee, "==")) {
                compile_expr(c, args->items[0], false);
                compile_expr(c, args->items[1], false);
                emit_byte(c, OP_EQUAL, line);
                return;
            }
            if (sym_eq(callee, "!=") || sym_eq(callee, "not=")) {
                compile_expr(c, args->items[0], false);
                compile_expr(c, args->items[1], false);
                emit_byte(c, OP_NOT_EQUAL, line);
                return;
            }
            if (sym_eq(callee, "<")) {
                compile_expr(c, args->items[0], false);
                compile_expr(c, args->items[1], false);
                emit_byte(c, OP_LESS, line);
                return;
            }
            if (sym_eq(callee, "<=")) {
                compile_expr(c, args->items[0], false);
                compile_expr(c, args->items[1], false);
                emit_byte(c, OP_LESS_EQUAL, line);
                return;
            }
            if (sym_eq(callee, ">")) {
                compile_expr(c, args->items[0], false);
                compile_expr(c, args->items[1], false);
                emit_byte(c, OP_GREATER, line);
                return;
            }
            if (sym_eq(callee, ">=")) {
                compile_expr(c, args->items[0], false);
                compile_expr(c, args->items[1], false);
                emit_byte(c, OP_GREATER_EQUAL, line);
                return;
            }
            /* List: cons */
            if (sym_eq(callee, "cons")) {
                compile_expr(c, args->items[0], false);
                compile_expr(c, args->items[1], false);
                emit_byte(c, OP_CONS, line);
                return;
            }
        }

        /* Unary ops */
        if (args->count == 1) {
            if (sym_eq(callee, "-")) {
                compile_expr(c, args->items[0], false);
                emit_byte(c, OP_NEGATE, line);
                return;
            }
            if (sym_eq(callee, "not")) {
                compile_expr(c, args->items[0], false);
                emit_byte(c, OP_NOT, line);
                return;
            }
            if (sym_eq(callee, "car") || sym_eq(callee, "first")) {
                compile_expr(c, args->items[0], false);
                emit_byte(c, OP_CAR, line);
                return;
            }
            if (sym_eq(callee, "cdr") || sym_eq(callee, "rest")) {
                compile_expr(c, args->items[0], false);
                emit_byte(c, OP_CDR, line);
                return;
            }
        }

        /* println: special opcode */
        if (sym_eq(callee, "println")) {
            for (int i = 0; i < args->count; i++) {
                compile_expr(c, args->items[i], false);
            }
            emit_bytes(c, OP_PRINTLN, (uint8_t)args->count, line);
            return;
        }

        /* list: build a list from N elements */
        if (sym_eq(callee, "list")) {
            for (int i = 0; i < args->count; i++) {
                compile_expr(c, args->items[i], false);
            }
            emit_bytes(c, OP_LIST, (uint8_t)args->count, line);
            return;
        }
    }

    /* General function call */
    compile_expr(c, callee, false);
    for (int i = 0; i < args->count; i++) {
        compile_expr(c, args->items[i], false);
    }
    emit_bytes(c, tail ? OP_TAIL_CALL : OP_CALL, (uint8_t)args->count, line);
}

static void compile_fn(lisa_compiler *c, lisa_ast *node) {
    int line = node->line;
    lisa_compiler fn_compiler;
    init_compiler(&fn_compiler, c, TYPE_FUNCTION, c->gc, NULL);
    begin_scope(&fn_compiler);

    /* Parameters */
    lisa_ast_list *params = &node->as.fn.params;
    fn_compiler.function->arity = params->count;
    for (int i = 0; i < params->count; i++) {
        lisa_ast *param = params->items[i];
        add_local(&fn_compiler, param->as.symbol.start, param->as.symbol.length, line);
    }

    /* Body */
    lisa_ast_list *body = &node->as.fn.body;
    for (int i = 0; i < body->count; i++) {
        bool is_last = (i == body->count - 1);
        compile_expr(&fn_compiler, body->items[i], is_last);
        if (!is_last) {
            emit_byte(&fn_compiler, OP_POP, body->items[i]->line);
        }
    }

    /* Last expression is the return value */
    emit_byte(&fn_compiler, OP_RETURN, line);

    lisa_obj_function *fn = fn_compiler.function;

    /* Emit closure instruction in the enclosing compiler */
    uint8_t idx = make_constant(c, LISA_OBJ(fn), line);
    emit_bytes(c, OP_CLOSURE, idx, line);

    for (int i = 0; i < fn->upvalue_count; i++) {
        emit_byte(c, fn_compiler.upvalues[i].is_local ? 1 : 0, line);
        emit_byte(c, fn_compiler.upvalues[i].index, line);
    }
}

static void compile_let(lisa_compiler *c, lisa_ast *node) {
    int line = node->line;
    begin_scope(c);

    lisa_ast_list *bindings = &node->as.let.bindings;
    for (int i = 0; i < bindings->count; i += 2) {
        lisa_ast *name = bindings->items[i];
        lisa_ast *val = bindings->items[i + 1];
        compile_expr(c, val, false);
        add_local(c, name->as.symbol.start, name->as.symbol.length, line);
    }

    lisa_ast_list *body = &node->as.let.body;
    for (int i = 0; i < body->count; i++) {
        compile_expr(c, body->items[i], false);
        if (i < body->count - 1) {
            emit_byte(c, OP_POP, body->items[i]->line);
        }
    }

    /* Stack: [..., local0, local1, ..., localN-1, body_result]
     * We want: [..., body_result]
     *
     * SET_LOCAL overwrites first let-local with body_result (no pop).
     * Then POP N times removes: body_result copy on top + N-1 remaining locals.
     * Final stack: [..., body_result_in_first_slot]. */
    int local_count_before = c->local_count;
    c->scope_depth--;
    while (c->local_count > 0 &&
           c->locals[c->local_count - 1].depth > c->scope_depth) {
        c->local_count--;
    }
    int locals_to_pop = local_count_before - c->local_count;
    int first_let_slot = c->local_count;

    /* Restore state so we can emit from the right local indices */
    c->scope_depth++;
    c->local_count = local_count_before;

    if (locals_to_pop > 0) {
        emit_bytes(c, OP_SET_LOCAL, (uint8_t)first_let_slot, line);

        for (int i = c->local_count - 1; i >= first_let_slot; i--) {
            if (c->locals[i].is_captured) {
                emit_byte(c, OP_CLOSE_UPVALUE, line);
            } else {
                emit_byte(c, OP_POP, line);
            }
        }
    }

    c->scope_depth--;
    c->local_count = first_let_slot;
}

static void compile_if(lisa_compiler *c, lisa_ast *node, bool tail) {
    int line = node->line;
    compile_expr(c, node->as.if_expr.cond, false);

    int then_jump = emit_jump(c, OP_JUMP_IF_FALSE, line);

    compile_expr(c, node->as.if_expr.then_branch, tail);
    int else_jump = emit_jump(c, OP_JUMP, line);

    patch_jump(c, then_jump, line);

    if (node->as.if_expr.else_branch != NULL) {
        compile_expr(c, node->as.if_expr.else_branch, tail);
    } else {
        emit_byte(c, OP_NIL, line);
    }

    patch_jump(c, else_jump, line);
}

static void compile_do(lisa_compiler *c, lisa_ast *node, bool tail) {
    lisa_ast_list *exprs = &node->as.do_block.exprs;
    for (int i = 0; i < exprs->count; i++) {
        bool is_last = (i == exprs->count - 1);
        compile_expr(c, exprs->items[i], is_last ? tail : false);
        if (!is_last) {
            emit_byte(c, OP_POP, exprs->items[i]->line);
        }
    }
}

static void compile_string_literal(lisa_compiler *c, lisa_ast *node) {
    /* Process escape sequences */
    const char *src = node->as.string_val.start;
    int src_len = node->as.string_val.length;
    char *buf = malloc((size_t)src_len + 1);
    int dst = 0;
    for (int i = 0; i < src_len; i++) {
        if (src[i] == '\\' && i + 1 < src_len) {
            i++;
            switch (src[i]) {
            case 'n': buf[dst++] = '\n'; break;
            case 't': buf[dst++] = '\t'; break;
            case 'r': buf[dst++] = '\r'; break;
            case '\\': buf[dst++] = '\\'; break;
            case '"': buf[dst++] = '"'; break;
            default: buf[dst++] = '\\'; buf[dst++] = src[i]; break;
            }
        } else {
            buf[dst++] = src[i];
        }
    }
    lisa_obj_string *str = lisa_copy_string(c->gc, buf, dst);
    free(buf);
    emit_constant(c, LISA_OBJ(str), node->line);
}

static void compile_expr(lisa_compiler *c, lisa_ast *node, bool tail) {
    switch (node->type) {
    case AST_INT_LIT:
        emit_constant(c, LISA_INT(node->as.int_val), node->line);
        break;
    case AST_DOUBLE_LIT:
        emit_constant(c, lisa_double(node->as.double_val), node->line);
        break;
    case AST_STRING_LIT:
        compile_string_literal(c, node);
        break;
    case AST_BOOL_LIT:
        emit_byte(c, node->as.bool_val ? OP_TRUE : OP_FALSE, node->line);
        break;
    case AST_NIL_LIT:
        emit_byte(c, OP_NIL, node->line);
        break;
    case AST_SYMBOL:
        compile_symbol(c, node);
        break;
    case AST_CALL:
        compile_call(c, node, tail);
        break;
    case AST_DEF:
        compile_def(c, node);
        break;
    case AST_FN:
        compile_fn(c, node);
        break;
    case AST_LET:
        compile_let(c, node);
        break;
    case AST_IF:
        compile_if(c, node, tail);
        break;
    case AST_DO:
        compile_do(c, node, tail);
        break;
    }
}

lisa_obj_function *lisa_compile(lisa_gc *gc, lisa_ast **exprs, int count) {
    had_error = false;
    lisa_compiler c;
    init_compiler(&c, NULL, TYPE_SCRIPT, gc, NULL);

    for (int i = 0; i < count; i++) {
        bool is_last = (i == count - 1);
        compile_expr(&c, exprs[i], is_last);
        if (!is_last) {
            emit_byte(&c, OP_POP, exprs[i]->line);
        }
    }

    if (count == 0) {
        emit_byte(&c, OP_NIL, 1);
    }

    emit_byte(&c, OP_RETURN, count > 0 ? exprs[count-1]->line : 1);

    if (had_error) return NULL;
    return c.function;
}
