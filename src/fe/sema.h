/* sema.h -- J73 semantic analysis
 * The bit where we discover that the parser was only the beginning
 * of our problems. Like landing the plane and then finding customs.
 * "Your types aren't in order, sir. Step aside." */
#ifndef SKYHAWK_SEMA_H
#define SKYHAWK_SEMA_H

#include "ast.h"
#include "parser.h"

/* ---- Limits ---- */

#define SM_MAX_TYPES   2048
#define SM_MAX_SYMS    2048
#define SM_MAX_SCOPES  128
#define SM_MAX_TBLDF   128
#define SM_MAX_FIELDS  32
#define SM_MAX_STDEF   128
#define SM_MAX_STVALS  64
#define SM_MAX_PARAMS  1024
#define SM_MAX_LABELS  256

/* ---- Interned type ---- */

typedef enum {
    JT_VOID,        /* nothing here, move along */
    JT_SIGNED,      /* S n */
    JT_UNSIGN,      /* U n */
    JT_FLOAT,       /* F n */
    JT_BIT,         /* B n */
    JT_CHAR,        /* C n -- characters, not personality */
    JT_HOLLER,      /* H n -- hollerith, for the distinguished */
    JT_FIXED,       /* A n D s -- fixed-point, for the masochistic */
    JT_STATUS,      /* STATUS(...) -- enum's eccentric uncle */
    JT_PTR,         /* POINTER(type) */
    JT_TABLE,       /* TABLE -- the whole blessed row */
    JT_ARRAY,       /* dimensioned thing */
    JT_PROC,        /* procedure/function */
    JT_ERROR,       /* type-checking gave up */
    JT_COUNT
} jt_kind_t;

/* 16 bytes, interned in pool. Each unique type lives once,
 * like a particularly inflexible hotel booking system. */
typedef struct {
    uint8_t   kind;     /* jt_kind_t */
    uint8_t   pad;
    uint16_t  width;    /* bit width */
    int16_t   scale;    /* fixed-point D value, 0 otherwise */
    uint16_t  n_extra;  /* param count / field count / status val count */
    uint32_t  inner;    /* PTR->pointee, ARRAY->element, PROC->ret type */
    uint32_t  extra;    /* tbldef idx / stdef idx / param_pool start */
} jtype_t;

/* ---- Symbol table ---- */

typedef enum {
    SYM_VAR,
    SYM_PROC,
    SYM_PARAM,
    SYM_TYPE,
    SYM_CONST,
    SYM_TABLE,
    SYM_LABEL,
    SYM_CPOOL
} sym_kind_t;

typedef struct {
    char      name[SK_MAX_IDENT];
    uint32_t  type;       /* jtype index */
    uint32_t  ast_nd;     /* defining AST node */
    uint8_t   kind;       /* sym_kind_t */
    uint8_t   scope;      /* scope depth */
    uint16_t  flags;      /* NF_STATIC, NF_CONST, etc */
    int64_t   cval;       /* constant value (SYM_CONST) */
} sema_sym_t;

/* ---- TABLE field definition ---- */

typedef struct {
    char      name[SK_MAX_IDENT];
    uint32_t  jtype;      /* field's jtype index */
    uint32_t  ast_nd;     /* ITEM node */
} sm_fld_t;

typedef struct {
    sm_fld_t  flds[SM_MAX_FIELDS];
    int       n_flds;
    uint32_t  ast_nd;     /* TABLE node */
    int32_t   lo_dim;     /* lower dimension bound */
    int32_t   hi_dim;     /* upper dimension bound */
} sm_tbldf_t;

/* ---- STATUS value definition ---- */

typedef struct {
    char      vals[SM_MAX_STVALS][SK_MAX_IDENT];
    int       n_vals;
} sm_stdef_t;

/* ---- FORMAT definitions ---- */

#define SM_MAX_FMTS   32
#define SM_MAX_FSPEC  16

typedef struct {
    uint8_t  kind;    /* 0=I, 1=F, 2=A, 3=X, 4=/, 5=literal */
    uint16_t width;
    uint16_t decim;
    uint16_t str;     /* literal string tok idx */
} sm_fspec_t;

typedef struct {
    char       name[SK_MAX_IDENT];
    sm_fspec_t specs[SM_MAX_FSPEC];
    int        n_spec;
} sm_fmt_t;

/* ---- Label tracking ---- */

typedef struct {
    char      name[SK_MAX_IDENT];
    uint32_t  ast_nd;
    int       defined;
    int       used;
} sm_label_t;

/* ---- Main context (~3MB, must be static) ----
 * Putting this on the stack would be like parking a 747 in a
 * disabled spot. Technically possible, spiritually wrong. */

typedef struct {
    /* input */
    const parser_t  *P;
    ast_node_t      *nodes;  /* mutable alias -- for call-vs-index rewrite */
    const token_t   *toks;
    uint32_t         n_toks;
    const char      *src;
    uint32_t         src_len;
    uint32_t         n_nodes;
    uint32_t         root;

    /* type pool */
    jtype_t    types[SM_MAX_TYPES];
    int        n_types;

    /* symbol table */
    sema_sym_t syms[SM_MAX_SYMS];
    int        n_syms;
    int        scp_stk[SM_MAX_SCOPES];
    int        scp_dep;

    /* TABLE definitions */
    sm_tbldf_t tbldef[SM_MAX_TBLDF];
    int        n_tbldf;

    /* STATUS definitions */
    sm_stdef_t stdef[SM_MAX_STDEF];
    int        n_stdef;

    /* PROC parameter pool */
    uint32_t   prm_pool[SM_MAX_PARAMS];
    int        n_params;

    /* labels */
    sm_label_t labels[SM_MAX_LABELS];
    int        n_labels;

    /* FORMAT definitions */
    sm_fmt_t   fmts[SM_MAX_FMTS];
    int        n_fmts;

    /* node -> type side table */
    uint32_t   nd_types[SK_MAX_NODES];

    /* current proc return type (while checking proc body) */
    uint32_t   cur_ret;

    /* errors */
    sk_err_t   errors[SK_MAX_ERRORS];
    int        n_errs;
} sema_ctx_t;

/* ---- Public API ---- */

void sema_init(sema_ctx_t *S, const parser_t *P);
int  sema_run(sema_ctx_t *S);
void sema_dump(const sema_ctx_t *S);
int  jt_str(const sema_ctx_t *S, uint32_t tidx, char *buf, int sz);

#endif /* SKYHAWK_SEMA_H */
