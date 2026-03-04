/* jir.h -- J73 intermediate representation
 * SSA in flat arrays. Every value is an instruction index,
 * every instruction is 32 bytes, and every programmer who
 * reads this will have opinions about the operand encoding. */
#ifndef SKYHAWK_JIR_H
#define SKYHAWK_JIR_H

#include "../fe/sema.h"
#include "../fe/layout.h"

/* ---- Limits ---- */

#define JIR_MAX_INST   (1 << 16)   /* 65536 instructions */
#define JIR_MAX_BLKS   (1 << 12)   /* 4096 basic blocks */
#define JIR_MAX_FUNCS  256
#define JIR_MAX_CONST  4096
#define JIR_MAX_EXTRA  4096         /* overflow operands */
#define JIR_MAX_STRS   (64 * 1024)
#define JIR_MAX_LOCAL  256
#define JIR_MAX_LABEL  256
#define JIR_MAX_LOOP   64

/* ---- Operand Encoding ---- */

#define JIR_CONST_BIT  0x80000000u
#define JIR_MK_C(i)   (JIR_CONST_BIT | (uint32_t)(i))
#define JIR_IS_C(v)    ((v) & JIR_CONST_BIT)
#define JIR_C_IDX(v)   ((v) & ~JIR_CONST_BIT)

/* ---- Opcodes ---- */

typedef enum {
    JIR_NOP,
    /* integer arith */
    JIR_ADD, JIR_SUB, JIR_MUL, JIR_DIV, JIR_MOD, JIR_NEG,
    /* float arith */
    JIR_FADD, JIR_FSUB, JIR_FMUL, JIR_FDIV, JIR_FNEG,
    /* bitwise */
    JIR_AND, JIR_OR, JIR_XOR, JIR_NOT, JIR_SHL, JIR_SHR,
    /* compare (subop = predicate) */
    JIR_ICMP, JIR_FCMP,
    /* memory */
    JIR_ALLOCA, JIR_LOAD, JIR_STORE, JIR_GEP,
    /* control */
    JIR_BR, JIR_BR_COND, JIR_RET, JIR_CALL, JIR_XCALL,
    /* conversion */
    JIR_SEXT, JIR_ZEXT, JIR_TRUNC,
    JIR_SITOFP, JIR_FPTOSI, JIR_FPEXT, JIR_FPTRUNC,
    /* SSA */
    JIR_PHI,
    JIR_OP_COUNT
} jir_op_t;

/* compare predicates (stored in subop) */
typedef enum {
    JP_EQ, JP_NE, JP_LT, JP_LE, JP_GT, JP_GE
} jir_pred_t;

/* ---- Instruction (32 bytes) ---- */

typedef struct {
    uint16_t op;        /* jir_op_t */
    uint8_t  n_ops;     /* 0-4 inline, 0xFF = overflow */
    uint8_t  subop;     /* predicate, field idx, etc */
    uint32_t type;      /* result jtype index */
    uint32_t ops[4];    /* operands (val refs or const refs) */
    uint32_t line;      /* source line */
    uint32_t pad;
} jir_inst_t;

/* ---- Constant Pool ---- */

typedef enum { JC_INT, JC_FLT, JC_STR } jc_kind_t;

typedef struct {
    uint8_t  kind;      /* jc_kind_t */
    uint8_t  pad[3];
    int64_t  iv;        /* JC_INT: value. JC_FLT: bits. JC_STR: soff in low 32 */
} jir_const_t;  /* 16 bytes (union-free for simplicity) */

/* ---- Basic Block (12 bytes) ---- */

typedef struct {
    uint32_t first;     /* first inst index */
    uint32_t n_inst;    /* instruction count */
    uint32_t name;      /* string pool offset */
} jir_blk_t;

/* ---- Function (28 bytes) ---- */

typedef struct {
    uint32_t name;      /* string pool offset */
    uint32_t ret_type;  /* jtype index */
    uint32_t first_blk; /* first block index */
    uint16_t n_blks;
    uint16_t n_params;
    uint32_t n_inst;    /* total insts across all blocks */
    uint32_t sema_nd;   /* source AST node */
} jir_func_t;

/* ---- External Function Registry ---- */

#define JIR_MAX_XFUNC  64

typedef struct {
    uint32_t name;      /* strs[] offset */
} jir_xfn_t;

/* ---- Module (~2.2MB, must be static) ---- */

typedef struct {
    const sema_ctx_t *S;

    jir_inst_t  insts[JIR_MAX_INST];
    uint32_t    n_inst;

    jir_blk_t   blks[JIR_MAX_BLKS];
    uint32_t    n_blks;

    jir_func_t  funcs[JIR_MAX_FUNCS];
    uint32_t    n_funcs;

    jir_const_t consts[JIR_MAX_CONST];
    uint32_t    n_consts;

    uint32_t    extra[JIR_MAX_EXTRA];
    uint32_t    n_extra;

    char        strs[JIR_MAX_STRS];
    uint32_t    str_len;

    jir_xfn_t   xfuncs[JIR_MAX_XFUNC];
    uint32_t    n_xfn;

    sk_err_t    errors[SK_MAX_ERRORS];
    int         n_errs;
} jir_mod_t;

/* ---- Public API ---- */

void        jir_init(jir_mod_t *M, const sema_ctx_t *S);
int         jir_lower(jir_mod_t *M);
int         jir_m2r(jir_mod_t *M);
void        jir_dump(const jir_mod_t *M);
const char *jir_opnm(int op);
uint32_t    jir_xfn(jir_mod_t *M, const char *name);

#endif /* SKYHAWK_JIR_H */
