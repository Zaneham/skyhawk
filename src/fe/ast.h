/* ast.h -- J73 abstract syntax tree
 * Left-child / right-sibling in a flat array.
 * No malloc, no recursion, no regrets. Well, some regrets. */
#ifndef SKYHAWK_AST_H
#define SKYHAWK_AST_H

#include "skyhawk.h"

/* ---- Node types ---- */

typedef enum {
    /* ---- Program ---- */
    ND_PROG,            /* START name ; ... TERM */

    /* ---- Declarations ---- */
    ND_ITEM,            /* ITEM name type [= init] */
    ND_TABLE,           /* TABLE name(dims) ; entries */
    ND_PROC,            /* PROC name(params) type ; body */
    ND_BLOCK,           /* BLOCK [name] ; decls */
    ND_COMPOOL,         /* COMPOOL name ; decls */
    ND_DEFINE,          /* DEFINE name = expr */
    ND_TYPEDEF,         /* TYPE name typespec */
    ND_PARAM,           /* procedure parameter (name only) */

    /* ---- Statements ---- */
    ND_ASSIGN,          /* target := value */
    ND_CALL,            /* bare expression as statement */
    ND_IF,              /* IF cond ; then [ELSE else] END */
    ND_CASE,            /* CASE sel ; branches END */
    ND_CSBRANCH,        /* V(x),V(y) : stmts [FALLTHRU] */
    ND_DEFAULT,         /* DEFAULT : stmts */
    ND_WHILE,           /* WHILE cond ; body END */
    ND_FOR,             /* FOR var := start BY step WHILE cond ; body */
    ND_GOTO,            /* GOTO label */
    ND_LABEL,           /* label : */
    ND_RETURN,          /* RETURN [expr] */
    ND_EXIT,            /* EXIT */
    ND_ABORT,           /* ABORT */
    ND_STOP,            /* STOP */
    ND_WRITE,           /* WRITE(file, fmt) items */
    ND_READ,            /* READ(file, fmt) items */
    ND_OPENF,           /* OPEN(file [, mode]) */
    ND_CLOSEF,          /* CLOSE(file) */
    ND_STMTBLK,        /* BEGIN stmts END */
    ND_NULL,            /* empty statement */

    /* ---- Expressions ---- */
    ND_INTLIT,          /* 42, 2B1010 */
    ND_FLTLIT,          /* 3.14 */
    ND_STRLIT,          /* 'text' */
    ND_STATUSLIT,       /* V(NAME) */
    ND_IDENT,           /* FOO, CURRENT'WAYPOINT */
    ND_BINARY,          /* left op right */
    ND_UNARY,           /* op operand */
    ND_FNCALL,          /* name(args [: outargs]) */
    ND_INDEX,           /* expr(indices) */
    ND_MEMBER,          /* expr.field */
    ND_DEREF,           /* @expr */
    ND_ADDROF,          /* LOC(expr) */

    /* ---- I/O declarations ---- */
    ND_FORMAT,          /* FORMAT name(specs) */
    ND_FMTSP,           /* single format specifier */

    /* ---- Type specifications ---- */
    ND_TYPESPEC,        /* type with base/size/scale */
    ND_STATUSVAL,       /* single V(name) in STATUS type def */
    ND_DIMPAIR,         /* lower:upper bound pair */

    ND_COUNT
} nd_type_t;

/* ---- Operator tags (stored in node.aux) ---- */

typedef enum {
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_POW,
    OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE,
    OP_AND, OP_OR, OP_XOR, OP_EQV, OP_NOT,
    OP_NEG, OP_POS,
    OP_COUNT
} op_type_t;

/* ---- Base type tags (stored in node.aux for ND_TYPESPEC) ---- */

typedef enum {
    BT_VOID,
    BT_SIGNED,      /* S n */
    BT_UNSIGNED,    /* U n */
    BT_FLOAT,       /* F n */
    BT_BIT,         /* B n */
    BT_CHAR,        /* C n */
    BT_HOLLER,      /* H n */
    BT_FIXED,       /* A n D s */
    BT_STATUS,      /* STATUS(...) */
    BT_POINTER,     /* POINTER(type) */
    BT_TYPEREF,     /* named type reference */
    BT_COUNT
} base_type_t;

/* ---- Item flags (stored in node.flags) ---- */

#define NF_STATIC    0x0001
#define NF_CONST     0x0002
#define NF_PARALLEL  0x0004
#define NF_INLINE    0x0008
#define NF_RENT      0x0010
#define NF_ENTRY     0x0020
#define NF_OVERLAY   0x0040
#define NF_POS       0x0080
#define NF_LIKE      0x0100
#define NF_FALLTHRU  0x0200
#define NF_OUTPUT    0x0400  /* output parameter */

/* ---- AST Node ---- */

/* 32 bytes. Flat array, indexed. child/sibling are indices.
 * 0 = no child / no sibling (sentinel). */

typedef struct {
    uint16_t type;      /* nd_type_t */
    uint16_t flags;     /* NF_* bitfield */
    int16_t  aux;       /* op_type_t, base_type_t, or scale factor */
    uint16_t aux2;      /* bit width for typespec, wordsize for table */
    uint32_t child;     /* first child index */
    uint32_t sibling;   /* next sibling index */
    uint32_t tok;       /* token index (for source location + text) */
    uint32_t tok2;      /* secondary token (name, label target, etc) */
    int64_t  val;       /* integer literal value */
} ast_node_t;

/* ---- AST storage ---- */

typedef struct {
    ast_node_t *nodes;
    uint32_t    n_nodes;
    uint32_t    max_nodes;
    uint32_t    root;       /* index of ND_PROG */
} ast_t;

/* node name for debug printing */
const char *nd_name(int type);
const char *op_name(int op);

#endif /* SKYHAWK_AST_H */
