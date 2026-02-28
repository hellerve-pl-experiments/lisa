#include "ast.h"
#include <stdlib.h>

static lisa_ast *alloc_node(lisa_ast_type type, int line) {
    lisa_ast *node = calloc(1, sizeof(lisa_ast));
    node->type = type;
    node->line = line;
    return node;
}

void lisa_ast_list_init(lisa_ast_list *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void lisa_ast_list_push(lisa_ast_list *list, lisa_ast *node) {
    if (list->count >= list->capacity) {
        list->capacity = list->capacity < 4 ? 4 : list->capacity * 2;
        list->items = realloc(list->items, sizeof(lisa_ast*) * (size_t)list->capacity);
    }
    list->items[list->count++] = node;
}

lisa_ast *lisa_ast_int(int64_t value, int line) {
    lisa_ast *node = alloc_node(AST_INT_LIT, line);
    node->as.int_val = value;
    return node;
}

lisa_ast *lisa_ast_double(double value, int line) {
    lisa_ast *node = alloc_node(AST_DOUBLE_LIT, line);
    node->as.double_val = value;
    return node;
}

lisa_ast *lisa_ast_string(const char *start, int length, int line) {
    lisa_ast *node = alloc_node(AST_STRING_LIT, line);
    node->as.string_val.start = start;
    node->as.string_val.length = length;
    return node;
}

lisa_ast *lisa_ast_bool(int value, int line) {
    lisa_ast *node = alloc_node(AST_BOOL_LIT, line);
    node->as.bool_val = value;
    return node;
}

lisa_ast *lisa_ast_nil(int line) {
    return alloc_node(AST_NIL_LIT, line);
}

lisa_ast *lisa_ast_symbol(const char *start, int length, int line) {
    lisa_ast *node = alloc_node(AST_SYMBOL, line);
    node->as.symbol.start = start;
    node->as.symbol.length = length;
    return node;
}

lisa_ast *lisa_ast_call(lisa_ast *callee, int line) {
    lisa_ast *node = alloc_node(AST_CALL, line);
    node->as.call.callee = callee;
    lisa_ast_list_init(&node->as.call.args);
    return node;
}

lisa_ast *lisa_ast_def(lisa_ast *name, lisa_ast *value, int line) {
    lisa_ast *node = alloc_node(AST_DEF, line);
    node->as.def.name = name;
    node->as.def.value = value;
    return node;
}

lisa_ast *lisa_ast_fn(int line) {
    lisa_ast *node = alloc_node(AST_FN, line);
    lisa_ast_list_init(&node->as.fn.params);
    lisa_ast_list_init(&node->as.fn.body);
    return node;
}

lisa_ast *lisa_ast_let(int line) {
    lisa_ast *node = alloc_node(AST_LET, line);
    lisa_ast_list_init(&node->as.let.bindings);
    lisa_ast_list_init(&node->as.let.body);
    return node;
}

lisa_ast *lisa_ast_if(lisa_ast *cond, lisa_ast *then_b, lisa_ast *else_b, int line) {
    lisa_ast *node = alloc_node(AST_IF, line);
    node->as.if_expr.cond = cond;
    node->as.if_expr.then_branch = then_b;
    node->as.if_expr.else_branch = else_b;
    return node;
}

lisa_ast *lisa_ast_do(int line) {
    lisa_ast *node = alloc_node(AST_DO, line);
    lisa_ast_list_init(&node->as.do_block.exprs);
    return node;
}

static void free_list(lisa_ast_list *list) {
    for (int i = 0; i < list->count; i++) {
        lisa_ast_free(list->items[i]);
    }
    free(list->items);
}

void lisa_ast_free(lisa_ast *node) {
    if (node == NULL) return;
    switch (node->type) {
    case AST_INT_LIT:
    case AST_DOUBLE_LIT:
    case AST_STRING_LIT:
    case AST_BOOL_LIT:
    case AST_NIL_LIT:
    case AST_SYMBOL:
        break;
    case AST_CALL:
        lisa_ast_free(node->as.call.callee);
        free_list(&node->as.call.args);
        break;
    case AST_DEF:
        lisa_ast_free(node->as.def.name);
        lisa_ast_free(node->as.def.value);
        break;
    case AST_FN:
        free_list(&node->as.fn.params);
        free_list(&node->as.fn.body);
        break;
    case AST_LET:
        free_list(&node->as.let.bindings);
        free_list(&node->as.let.body);
        break;
    case AST_IF:
        lisa_ast_free(node->as.if_expr.cond);
        lisa_ast_free(node->as.if_expr.then_branch);
        lisa_ast_free(node->as.if_expr.else_branch);
        break;
    case AST_DO:
        free_list(&node->as.do_block.exprs);
        break;
    }
    free(node);
}
