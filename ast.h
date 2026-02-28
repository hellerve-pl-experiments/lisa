#ifndef LISA_AST_H
#define LISA_AST_H

#include <stdint.h>

typedef enum {
    AST_INT_LIT,
    AST_DOUBLE_LIT,
    AST_STRING_LIT,
    AST_BOOL_LIT,
    AST_NIL_LIT,
    AST_SYMBOL,
    AST_CALL,       /* (op args...) */
    AST_DEF,        /* (def name value) */
    AST_FN,         /* (fn [params] body...) */
    AST_LET,        /* (let [bindings] body...) */
    AST_IF,         /* (if cond then else?) */
    AST_DO,         /* (do exprs...) */
} lisa_ast_type;

typedef struct lisa_ast lisa_ast;

typedef struct {
    lisa_ast **items;
    int count;
    int capacity;
} lisa_ast_list;

struct lisa_ast {
    lisa_ast_type type;
    int line;
    union {
        int64_t int_val;
        double double_val;
        struct { const char *start; int length; } string_val;
        int bool_val;
        struct { const char *start; int length; } symbol;
        struct { lisa_ast *callee; lisa_ast_list args; } call;
        struct { lisa_ast *name; lisa_ast *value; } def;
        struct { lisa_ast_list params; lisa_ast_list body; } fn;
        struct { lisa_ast_list bindings; lisa_ast_list body; } let;
        struct { lisa_ast *cond; lisa_ast *then_branch; lisa_ast *else_branch; } if_expr;
        struct { lisa_ast_list exprs; } do_block;
    } as;
};

lisa_ast *lisa_ast_int(int64_t value, int line);
lisa_ast *lisa_ast_double(double value, int line);
lisa_ast *lisa_ast_string(const char *start, int length, int line);
lisa_ast *lisa_ast_bool(int value, int line);
lisa_ast *lisa_ast_nil(int line);
lisa_ast *lisa_ast_symbol(const char *start, int length, int line);
lisa_ast *lisa_ast_call(lisa_ast *callee, int line);
lisa_ast *lisa_ast_def(lisa_ast *name, lisa_ast *value, int line);
lisa_ast *lisa_ast_fn(int line);
lisa_ast *lisa_ast_let(int line);
lisa_ast *lisa_ast_if(lisa_ast *cond, lisa_ast *then_b, lisa_ast *else_b, int line);
lisa_ast *lisa_ast_do(int line);

void lisa_ast_list_init(lisa_ast_list *list);
void lisa_ast_list_push(lisa_ast_list *list, lisa_ast *node);

void lisa_ast_free(lisa_ast *node);

#endif
