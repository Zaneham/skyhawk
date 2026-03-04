/* jir_lower.c -- typed AST to JIR SSA lowering
 * Turns a tree of good intentions into a flat array of instructions.
 * Every local gets an alloca because mem2reg hasn't been invited yet. */

#include "jir.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

/* ---- Lowering Context ---- */

typedef struct {
    jir_mod_t        *M;
    const sema_ctx_t *S;
    uint32_t          cur_blk;
    uint32_t          cur_fn;

    /* locals: name -> alloca inst */
    struct { char nm[SK_MAX_IDENT]; uint32_t alloc; uint32_t type; } loc[JIR_MAX_LOCAL];
    int n_loc;

    /* label -> block map */
    struct { char nm[SK_MAX_IDENT]; uint32_t blk; } lbl[JIR_MAX_LABEL];
    int n_lbl;

    /* loop exit stack */
    uint32_t lp_exit[JIR_MAX_LOOP];
    int lp_dep;

    uint32_t ret_ty;
} lower_t;

/* file-scope so it doesn't blow the stack */
static lower_t L_;

/* ---- AST helpers ---- */

static inline ast_node_t *ND(const lower_t *L, uint32_t i)
{
    if (i == 0 || i >= L->S->n_nodes) return &L->S->nodes[0];
    return &L->S->nodes[i];
}

static void nd_txt(const lower_t *L, uint32_t ti, char *buf, int sz)
{
    if (ti >= L->S->n_toks) { buf[0] = '\0'; return; }
    const token_t *t = &L->S->toks[ti];
    int n = (int)t->len;
    if (n >= sz) n = sz - 1;
    memcpy(buf, L->S->src + t->offset, (size_t)n);
    buf[n] = '\0';
}

static uint32_t nd_line(const lower_t *L, uint32_t nd)
{
    const ast_node_t *n = ND(L, nd);
    if (n->tok < L->S->n_toks)
        return L->S->toks[n->tok].line;
    return 0;
}

static void ucase(char *s)
{
    for (; *s; s++) *s = (char)toupper((unsigned char)*s);
}

static int ci_eq(const char *a, const char *b)
{
    for (;;) {
        char ca = (char)toupper((unsigned char)*a);
        char cb = (char)toupper((unsigned char)*b);
        if (ca != cb) return 0;
        if (ca == '\0') return 1;
        a++; b++;
    }
}

/* ---- IR emit helpers ---- */

static uint32_t emit(lower_t *L, int op, uint32_t type,
                     int nops, int subop)
{
    jir_mod_t *M = L->M;
    if (M->n_inst >= JIR_MAX_INST) return 0;
    uint32_t idx = M->n_inst++;
    jir_inst_t *I = &M->insts[idx];
    memset(I, 0, sizeof(*I));
    I->op    = (uint16_t)op;
    I->n_ops = (uint8_t)nops;
    I->subop = (uint8_t)subop;
    I->type  = type;
    if (L->cur_blk < M->n_blks)
        M->blks[L->cur_blk].n_inst++;
    return idx;
}

static void setop(lower_t *L, uint32_t inst, int slot, uint32_t val)
{
    if (inst < L->M->n_inst && slot < 4)
        L->M->insts[inst].ops[slot] = val;
}

static void setln(lower_t *L, uint32_t inst, uint32_t ln)
{
    if (inst < L->M->n_inst)
        L->M->insts[inst].line = ln;
}

/* constant pool */
static uint32_t mk_ci(lower_t *L, int64_t v)
{
    jir_mod_t *M = L->M;
    /* dedup */
    for (uint32_t i = 0; i < M->n_consts; i++) {
        if (M->consts[i].kind == JC_INT && M->consts[i].iv == v)
            return JIR_MK_C(i);
    }
    if (M->n_consts >= JIR_MAX_CONST) return JIR_MK_C(0);
    uint32_t idx = M->n_consts++;
    M->consts[idx].kind = JC_INT;
    M->consts[idx].iv   = v;
    return JIR_MK_C(idx);
}

static uint32_t mk_cf(lower_t *L, double v)
{
    jir_mod_t *M = L->M;
    int64_t bits;
    memcpy(&bits, &v, 8);
    for (uint32_t i = 0; i < M->n_consts; i++) {
        if (M->consts[i].kind == JC_FLT && M->consts[i].iv == bits)
            return JIR_MK_C(i);
    }
    if (M->n_consts >= JIR_MAX_CONST) return JIR_MK_C(0);
    uint32_t idx = M->n_consts++;
    M->consts[idx].kind = JC_FLT;
    M->consts[idx].iv   = bits;
    return JIR_MK_C(idx);
}

static uint32_t mk_str(lower_t *L, const char *s, int len)
{
    jir_mod_t *M = L->M;
    if (M->str_len + (uint32_t)len + 1 > JIR_MAX_STRS) return 0;
    uint32_t off = M->str_len;
    memcpy(M->strs + off, s, (size_t)len);
    M->strs[off + (uint32_t)len] = '\0';
    M->str_len += (uint32_t)len + 1;
    return off;
}

static uint32_t add_str(lower_t *L, const char *s)
{
    return mk_str(L, s, (int)strlen(s));
}

/* blocks */
static uint32_t new_blk(lower_t *L, const char *name)
{
    jir_mod_t *M = L->M;
    if (M->n_blks >= JIR_MAX_BLKS) return 0;
    uint32_t idx = M->n_blks++;
    jir_blk_t *b = &M->blks[idx];
    b->first  = M->n_inst;
    b->n_inst = 0;
    b->name   = add_str(L, name);
    return idx;
}

static void set_blk(lower_t *L, uint32_t idx)
{
    L->cur_blk = idx;
    if (idx < L->M->n_blks)
        L->M->blks[idx].first = L->M->n_inst;
}

/* check if current block has a terminator */
static int blk_term(const lower_t *L)
{
    const jir_mod_t *M = L->M;
    if (L->cur_blk >= M->n_blks) return 0;
    const jir_blk_t *b = &M->blks[L->cur_blk];
    if (b->n_inst == 0) return 0;
    uint16_t last_op = M->insts[b->first + b->n_inst - 1].op;
    return last_op == JIR_BR || last_op == JIR_BR_COND || last_op == JIR_RET;
}

/* locals */
static void add_loc(lower_t *L, const char *nm, uint32_t alloc, uint32_t ty)
{
    if (L->n_loc >= JIR_MAX_LOCAL) return;
    snprintf(L->loc[L->n_loc].nm, SK_MAX_IDENT, "%s", nm);
    ucase(L->loc[L->n_loc].nm);
    L->loc[L->n_loc].alloc = alloc;
    L->loc[L->n_loc].type  = ty;
    L->n_loc++;
}

static int find_loc(const lower_t *L, const char *nm)
{
    char up[SK_MAX_IDENT];
    snprintf(up, SK_MAX_IDENT, "%s", nm);
    ucase(up);
    for (int i = L->n_loc - 1; i >= 0; i--) {
        if (ci_eq(L->loc[i].nm, up)) return i;
    }
    return -1;
}

/* label -> block */
static uint32_t map_lbl(lower_t *L, const char *nm)
{
    char up[SK_MAX_IDENT];
    snprintf(up, SK_MAX_IDENT, "%s", nm);
    ucase(up);
    for (int i = 0; i < L->n_lbl; i++) {
        if (ci_eq(L->lbl[i].nm, up)) return L->lbl[i].blk;
    }
    if (L->n_lbl >= JIR_MAX_LABEL) return 0;
    char bname[SK_MAX_IDENT + 8];
    snprintf(bname, sizeof(bname), "lbl.%.60s", up);
    uint32_t blk = new_blk(L, bname);
    snprintf(L->lbl[L->n_lbl].nm, SK_MAX_IDENT, "%s", up);
    L->lbl[L->n_lbl].blk = blk;
    L->n_lbl++;
    return blk;
}

/* ---- Forward decls ---- */

static uint32_t low_expr(lower_t *L, uint32_t nd);
static uint32_t low_lhs(lower_t *L, uint32_t nd);
static void low_stmt(lower_t *L, uint32_t nd);
static void low_decl(lower_t *L, uint32_t nd);

/* ---- External call helper ---- */

/* emit JIR_XCALL to xfunc by name, with 0-4 args.
 * returns the call instruction index (for result extraction). */
static uint32_t em_xc(lower_t *L, const char *fn,
                         uint32_t rtype, int narg, uint32_t *args)
{
    uint32_t xi = jir_xfn(L->M, fn);
    uint32_t xref = mk_ci(L, (int64_t)xi);

    int nops = (narg < 3) ? narg + 1 : 4;
    uint32_t call = emit(L, JIR_XCALL, rtype, nops, 0);
    setop(L, call, 0, xref);
    for (int i = 0; i < narg && i < 3; i++)
        setop(L, call, i + 1, args[i]);

    if (narg > 3) {
        jir_mod_t *M = L->M;
        M->insts[call].n_ops = 0xFF;
        M->insts[call].ops[3] = M->n_extra;
        for (int i = 0; i < narg && M->n_extra < JIR_MAX_EXTRA; i++)
            M->extra[M->n_extra++] = args[i];
    }
    return call;
}

/* ---- Expression Lowering ---- */

static uint32_t low_expr(lower_t *L, uint32_t nd)
{
    ast_node_t *n = ND(L, nd);
    if (nd == 0 || n->type == 0) return mk_ci(L, 0);

    uint32_t ln = nd_line(L, nd);
    uint32_t nty = L->S->nd_types[nd]; /* sema-annotated type */

    switch (n->type) {
    case ND_INTLIT:
        return mk_ci(L, n->val);

    case ND_FLTLIT: {
        char buf[64];
        nd_txt(L, n->tok, buf, (int)sizeof(buf));
        double fv = strtod(buf, NULL);
        return mk_cf(L, fv);
    }

    case ND_STRLIT: {
        char buf[SK_MAX_IDENT];
        nd_txt(L, n->tok, buf, (int)sizeof(buf));
        /* strip quotes */
        int slen = (int)strlen(buf);
        if (slen >= 2) {
            uint32_t soff = mk_str(L, buf + 1, slen - 2);
            return mk_ci(L, (int64_t)soff);
        }
        return mk_ci(L, 0);
    }

    case ND_STATUSLIT: {
        /* look up ordinal from stdef */
        char vn[SK_MAX_IDENT];
        nd_txt(L, n->tok2 ? n->tok2 : n->tok, vn, (int)sizeof(vn));
        ucase(vn);
        for (int i = 0; i < L->S->n_stdef; i++) {
            for (int j = 0; j < L->S->stdef[i].n_vals; j++) {
                if (ci_eq(L->S->stdef[i].vals[j], vn))
                    return mk_ci(L, (int64_t)j);
            }
        }
        return mk_ci(L, 0);
    }

    case ND_IDENT: {
        char nm[SK_MAX_IDENT];
        nd_txt(L, n->tok2 ? n->tok2 : n->tok, nm, (int)sizeof(nm));
        int li = find_loc(L, nm);
        if (li >= 0) {
            uint32_t ty = L->loc[li].type;
            /* tables: return alloca address, not a load */
            if (ty > 0 && ty < (uint32_t)L->S->n_types &&
                L->S->types[ty].kind == JT_TABLE)
                return L->loc[li].alloc;
            uint32_t ld = emit(L, JIR_LOAD, ty, 1, 0);
            setop(L, ld, 0, L->loc[li].alloc);
            setln(L, ld, ln);
            return ld;
        }
        /* might be a DEFINE constant from sema */
        int si = -1;
        { /* replicate sema find_sym */
            char up[SK_MAX_IDENT];
            snprintf(up, SK_MAX_IDENT, "%s", nm);
            ucase(up);
            for (int k = L->S->n_syms - 1; k >= 0; k--) {
                if (ci_eq(L->S->syms[k].name, up)) { si = k; break; }
            }
        }
        if (si >= 0 && L->S->syms[si].kind == SYM_CONST)
            return mk_ci(L, L->S->syms[si].cval);
        return mk_ci(L, 0); /* shouldn't happen after sema */
    }

    case ND_BINARY: {
        uint32_t lv = low_expr(L, n->child);
        uint32_t rv = low_expr(L, ND(L, n->child)->sibling);

        int is_flt = nty > 0 && nty < (uint32_t)L->S->n_types &&
                     L->S->types[nty].kind == JT_FLOAT;
        int is_bit = nty > 0 && nty < (uint32_t)L->S->n_types &&
                     L->S->types[nty].kind == JT_BIT;
        int is_fix = nty > 0 && nty < (uint32_t)L->S->n_types &&
                     L->S->types[nty].kind == JT_FIXED;

        /* ---- Fixed-point scaling ----
         * A n D s stores (value * 2^s) in n bits. Arithmetic
         * needs scale alignment, like converting currencies
         * except the exchange rate is always a power of two. */
        if (is_fix && n->aux >= OP_ADD && n->aux <= OP_DIV) {
            uint32_t lt = L->S->nd_types[n->child];
            uint32_t rt = L->S->nd_types[ND(L, n->child)->sibling];
            int ls = (lt > 0 && lt < (uint32_t)L->S->n_types)
                     ? (int)L->S->types[lt].scale : 0;
            int rs = (rt > 0 && rt < (uint32_t)L->S->n_types)
                     ? (int)L->S->types[rt].scale : 0;
            int ts = (int)L->S->types[nty].scale;

            if (n->aux == OP_ADD || n->aux == OP_SUB) {
                /* align to max scale, then add/sub */
                int ms = ls > rs ? ls : rs;
                if (ls < ms) {
                    uint32_t sh = mk_ci(L, (int64_t)(ms - ls));
                    uint32_t shl = emit(L, JIR_SHL, nty, 2, 0);
                    setop(L, shl, 0, lv); setop(L, shl, 1, sh);
                    setln(L, shl, ln);
                    lv = shl;
                }
                if (rs < ms) {
                    uint32_t sh = mk_ci(L, (int64_t)(ms - rs));
                    uint32_t shl = emit(L, JIR_SHL, nty, 2, 0);
                    setop(L, shl, 0, rv); setop(L, shl, 1, sh);
                    setln(L, shl, ln);
                    rv = shl;
                }
                int aop = (n->aux == OP_ADD) ? JIR_ADD : JIR_SUB;
                uint32_t res = emit(L, aop, nty, 2, 0);
                setop(L, res, 0, lv); setop(L, res, 1, rv);
                setln(L, res, ln);
                /* adjust from aligned scale to target scale */
                if (ms != ts) {
                    int d = ms - ts;
                    uint32_t sh = mk_ci(L, (int64_t)(d > 0 ? d : -d));
                    int sop = d > 0 ? JIR_SHR : JIR_SHL;
                    uint32_t adj = emit(L, sop, nty, 2, 0);
                    setop(L, adj, 0, res); setop(L, adj, 1, sh);
                    setln(L, adj, ln);
                    res = adj;
                }
                return res;
            }
            if (n->aux == OP_MUL) {
                /* raw product scale = ls + rs, shift to target */
                uint32_t res = emit(L, JIR_MUL, nty, 2, 0);
                setop(L, res, 0, lv); setop(L, res, 1, rv);
                setln(L, res, ln);
                int ps = ls + rs;
                if (ps != ts) {
                    int d = ps - ts;
                    uint32_t sh = mk_ci(L, (int64_t)(d > 0 ? d : -d));
                    int sop = d > 0 ? JIR_SHR : JIR_SHL;
                    uint32_t adj = emit(L, sop, nty, 2, 0);
                    setop(L, adj, 0, res); setop(L, adj, 1, sh);
                    setln(L, adj, ln);
                    res = adj;
                }
                return res;
            }
            if (n->aux == OP_DIV) {
                /* pre-shift numerator by target scale, then divide */
                uint32_t nlv = lv;
                if (ts > 0) {
                    uint32_t sh = mk_ci(L, (int64_t)ts);
                    uint32_t shl = emit(L, JIR_SHL, nty, 2, 0);
                    setop(L, shl, 0, lv); setop(L, shl, 1, sh);
                    setln(L, shl, ln);
                    nlv = shl;
                }
                uint32_t res = emit(L, JIR_DIV, nty, 2, 0);
                setop(L, res, 0, nlv); setop(L, res, 1, rv);
                setln(L, res, ln);
                return res;
            }
        }

        /* fixed-point comparisons: align scales then ICMP */
        if (is_fix && n->aux >= OP_EQ && n->aux <= OP_GE) {
            uint32_t lt = L->S->nd_types[n->child];
            uint32_t rt = L->S->nd_types[ND(L, n->child)->sibling];
            int ls = (lt > 0 && lt < (uint32_t)L->S->n_types)
                     ? (int)L->S->types[lt].scale : 0;
            int rs = (rt > 0 && rt < (uint32_t)L->S->n_types)
                     ? (int)L->S->types[rt].scale : 0;
            int ms = ls > rs ? ls : rs;
            if (ls < ms) {
                uint32_t sh = mk_ci(L, (int64_t)(ms - ls));
                uint32_t shl = emit(L, JIR_SHL, nty, 2, 0);
                setop(L, shl, 0, lv); setop(L, shl, 1, sh);
                setln(L, shl, ln);
                lv = shl;
            }
            if (rs < ms) {
                uint32_t sh = mk_ci(L, (int64_t)(ms - rs));
                uint32_t shl = emit(L, JIR_SHL, nty, 2, 0);
                setop(L, shl, 0, rv); setop(L, shl, 1, sh);
                setln(L, shl, ln);
                rv = shl;
            }
            int pred = JP_EQ;
            switch (n->aux) {
            case OP_EQ: pred = JP_EQ; break;
            case OP_NE: pred = JP_NE; break;
            case OP_LT: pred = JP_LT; break;
            case OP_LE: pred = JP_LE; break;
            case OP_GT: pred = JP_GT; break;
            case OP_GE: pred = JP_GE; break;
            default: break;
            }
            uint32_t cmp = emit(L, JIR_ICMP, nty, 2, pred);
            setop(L, cmp, 0, lv); setop(L, cmp, 1, rv);
            setln(L, cmp, ln);
            return cmp;
        }

        int op;
        int sub = 0;
        switch (n->aux) {
        case OP_ADD: op = is_flt ? JIR_FADD : JIR_ADD; break;
        case OP_SUB: op = is_flt ? JIR_FSUB : JIR_SUB; break;
        case OP_MUL: op = is_flt ? JIR_FMUL : JIR_MUL; break;
        case OP_DIV: op = is_flt ? JIR_FDIV : JIR_DIV; break;
        case OP_MOD: op = JIR_MOD; break;
        case OP_POW: {
            /* exponentiation via runtime call — can't inline a
             * variable exponent without turning the compiler into
             * a research project on loop unrolling */
            uint32_t a[2] = { lv, rv };
            uint32_t xc = em_xc(L, is_flt ? "sk_powf" : "sk_powi",
                                nty, 2, a);
            setln(L, xc, ln);
            return xc;
        }

        case OP_EQ:  op = is_flt ? JIR_FCMP : JIR_ICMP; sub = JP_EQ; break;
        case OP_NE:  op = is_flt ? JIR_FCMP : JIR_ICMP; sub = JP_NE; break;
        case OP_LT:  op = is_flt ? JIR_FCMP : JIR_ICMP; sub = JP_LT; break;
        case OP_LE:  op = is_flt ? JIR_FCMP : JIR_ICMP; sub = JP_LE; break;
        case OP_GT:  op = is_flt ? JIR_FCMP : JIR_ICMP; sub = JP_GT; break;
        case OP_GE:  op = is_flt ? JIR_FCMP : JIR_ICMP; sub = JP_GE; break;

        case OP_AND: op = JIR_AND; break;
        case OP_OR:  op = JIR_OR; break;
        case OP_XOR: op = JIR_XOR; break;
        case OP_EQV: op = JIR_XOR; break; /* EQV = NOT XOR, close enough for now */
        default:     op = JIR_ADD; break;
        }

        /* comparisons produce BIT(1), not the operand type */
        uint32_t rty = (is_bit || (n->aux >= OP_EQ && n->aux <= OP_GE)) ? nty : nty;

        uint32_t inst = emit(L, op, rty, 2, sub);
        setop(L, inst, 0, lv);
        setop(L, inst, 1, rv);
        setln(L, inst, ln);
        return inst;
    }

    case ND_UNARY: {
        uint32_t ov = low_expr(L, n->child);
        int is_flt = nty > 0 && nty < (uint32_t)L->S->n_types &&
                     L->S->types[nty].kind == JT_FLOAT;
        if (n->aux == OP_NEG || n->aux == OP_SUB) {
            int op = is_flt ? JIR_FNEG : JIR_NEG;
            uint32_t inst = emit(L, op, nty, 1, 0);
            setop(L, inst, 0, ov);
            setln(L, inst, ln);
            return inst;
        }
        if (n->aux == OP_NOT) {
            uint32_t inst = emit(L, JIR_NOT, nty, 1, 0);
            setop(L, inst, 0, ov);
            setln(L, inst, ln);
            return inst;
        }
        return ov; /* OP_POS is identity */
    }

    case ND_FNCALL: {
        /* callee is first child (ND_IDENT), args are siblings */
        uint32_t callee_nd = n->child;
        char cnm[SK_MAX_IDENT];
        if (callee_nd != 0 && ND(L, callee_nd)->type == ND_IDENT)
            nd_txt(L, ND(L, callee_nd)->tok2 ? ND(L, callee_nd)->tok2
                                              : ND(L, callee_nd)->tok,
                   cnm, (int)sizeof(cnm));
        else
            nd_txt(L, n->tok2 ? n->tok2 : n->tok, cnm, (int)sizeof(cnm));

        /* collect args */
        uint32_t args[16];
        int narg = 0;
        uint32_t a = (callee_nd != 0) ? ND(L, callee_nd)->sibling : 0;
        for (int g = 0; g < 16 && a != 0; g++) {
            args[narg++] = low_expr(L, a);
            a = ND(L, a)->sibling;
        }

        ucase(cnm);

        /* ---- Built-in function dispatch ----
         * J73's idea of a standard library: five functions and
         * a stern pamphlet about determinism. */
        if (ci_eq(cnm, "SHIFTL") && narg >= 2) {
            uint32_t inst = emit(L, JIR_SHL, nty, 2, 0);
            setop(L, inst, 0, args[0]);
            setop(L, inst, 1, args[1]);
            setln(L, inst, ln);
            return inst;
        }
        if (ci_eq(cnm, "SHIFTR") && narg >= 2) {
            uint32_t inst = emit(L, JIR_SHR, nty, 2, 0);
            setop(L, inst, 0, args[0]);
            setop(L, inst, 1, args[1]);
            setln(L, inst, ln);
            return inst;
        }
        if (ci_eq(cnm, "ABS") && narg >= 1) {
            /* inline ABS: (x < 0) ? -x : x */
            uint32_t zero = mk_ci(L, 0);
            uint32_t cmp = emit(L, JIR_ICMP, nty, 2, JP_LT);
            setop(L, cmp, 0, args[0]); setop(L, cmp, 1, zero);
            setln(L, cmp, ln);
            uint32_t neg = emit(L, JIR_NEG, nty, 1, 0);
            setop(L, neg, 0, args[0]);
            setln(L, neg, ln);
            /* use XOR trick: mask = x >> 63, (x ^ mask) - mask */
            /* simpler: emit XCALL to sk_absi or use sub(0, x) */
            /* for now just emit neg and let backend select.
             * TODO: proper select when we have JIR_SELECT */
            uint32_t xa[1] = { args[0] };
            uint32_t xc = em_xc(L, "sk_absi", nty, 1, xa);
            setln(L, xc, ln);
            return xc;
        }
        if (ci_eq(cnm, "BITSIZE") && narg >= 1) {
            /* constant fold to arg's type width */
            uint32_t arg_nd = (callee_nd != 0) ?
                ND(L, callee_nd)->sibling : 0;
            uint32_t at = (arg_nd != 0) ?
                L->S->nd_types[arg_nd] : 0;
            int bw = 0;
            if (at > 0 && at < (uint32_t)L->S->n_types)
                bw = (int)L->S->types[at].width;
            return mk_ci(L, (int64_t)bw);
        }
        if (ci_eq(cnm, "BYTESIZE") && narg >= 1) {
            uint32_t arg_nd = (callee_nd != 0) ?
                ND(L, callee_nd)->sibling : 0;
            uint32_t at = (arg_nd != 0) ?
                L->S->nd_types[arg_nd] : 0;
            int bw = 0;
            if (at > 0 && at < (uint32_t)L->S->n_types)
                bw = (int)L->S->types[at].width;
            return mk_ci(L, (int64_t)((bw + 7) / 8));
        }

        /* find callee function index */
        uint32_t cref = mk_ci(L, 0);
        for (uint32_t i = 0; i < L->M->n_funcs; i++) {
            uint32_t fnoff = L->M->funcs[i].name;
            if (fnoff < L->M->str_len && ci_eq(L->M->strs + fnoff, cnm)) {
                cref = JIR_MK_C((uint32_t)i); /* reuse const encoding for func ref */
                break;
            }
        }

        /* emit CALL: ops[0] = callee, ops[1..3] = first 3 args */
        /* overflow for >3 args goes to extra pool */
        uint32_t call = emit(L, JIR_CALL, nty, (narg < 4) ? narg + 1 : 4, 0);
        setop(L, call, 0, cref);
        for (int i = 0; i < narg && i < 3; i++)
            setop(L, call, i + 1, args[i]);

        if (narg > 3) {
            /* overflow */
            jir_mod_t *M = L->M;
            L->M->insts[call].n_ops = 0xFF;
            if (M->n_extra + (uint32_t)narg + 1 <= JIR_MAX_EXTRA) {
                L->M->insts[call].ops[0] = M->n_extra;
                L->M->insts[call].ops[1] = (uint32_t)narg + 1;
                M->extra[M->n_extra++] = cref;
                for (int i = 0; i < narg; i++)
                    M->extra[M->n_extra++] = args[i];
            }
        }
        setln(L, call, ln);
        return call;
    }

    case ND_INDEX: {
        /* TABLE(I) → GEP on table base */
        uint32_t base = low_expr(L, n->child);
        uint32_t idx_v = mk_ci(L, 0);
        uint32_t ai = ND(L, n->child)->sibling;
        if (ai != 0) idx_v = low_expr(L, ai);

        uint32_t gep = emit(L, JIR_GEP, nty, 2, 0);
        setop(L, gep, 0, base);
        setop(L, gep, 1, idx_v);
        setln(L, gep, ln);
        return gep;
    }

    case ND_MEMBER: {
        /* expr.FIELD → GEP with field index in subop.
         * POS fields get bit extraction after the load,
         * like customs inspecting only the relevant luggage. */
        uint32_t base = low_expr(L, n->child);
        char fn[SK_MAX_IDENT];
        nd_txt(L, n->tok2 ? n->tok2 : n->tok, fn, (int)sizeof(fn));
        ucase(fn);

        /* find field index + POS info from tbldef */
        uint8_t fld_idx = 0;
        uint16_t pos_boff = 0;   /* bit offset within word */
        uint16_t pos_bwid = 0;   /* bit width */
        int has_pos = 0;
        uint32_t base_ty = L->S->nd_types[n->child];
        if (base_ty > 0 && base_ty < (uint32_t)L->S->n_types &&
            L->S->types[base_ty].kind == JT_TABLE) {
            uint32_t tdi = L->S->types[base_ty].extra;
            if (tdi < (uint32_t)L->S->n_tbldf) {
                for (int i = 0; i < L->S->tbldef[tdi].n_flds; i++) {
                    if (ci_eq(L->S->tbldef[tdi].flds[i].name, fn)) {
                        fld_idx = (uint8_t)i;
                        /* check for POS flag */
                        uint32_t fnd = L->S->tbldef[tdi].flds[i].ast_nd;
                        if (fnd != 0 && fnd < L->S->n_nodes &&
                            (L->S->nodes[fnd].flags & NF_POS)) {
                            has_pos = 1;
                            int64_t bp = L->S->nodes[fnd].val;
                            pos_boff = (uint16_t)(bp % 8);
                            const jtype_t *ft = &L->S->types[
                                L->S->tbldef[tdi].flds[i].jtype];
                            pos_bwid = ft->width;
                        }
                        break;
                    }
                }
            }
        }

        uint32_t gep = emit(L, JIR_GEP, nty, 1, fld_idx);
        setop(L, gep, 0, base);
        setln(L, gep, ln);

        uint32_t ld = emit(L, JIR_LOAD, nty, 1, 0);
        setop(L, ld, 0, gep);
        setln(L, ld, ln);

        /* POS bit field extraction: SHR by bit_off, AND mask */
        if (has_pos && (pos_boff > 0 || pos_bwid < 64)) {
            uint32_t val = ld;
            if (pos_boff > 0) {
                uint32_t sh = mk_ci(L, (int64_t)pos_boff);
                uint32_t shr = emit(L, JIR_SHR, nty, 2, 0);
                setop(L, shr, 0, val); setop(L, shr, 1, sh);
                setln(L, shr, ln);
                val = shr;
            }
            if (pos_bwid < 64) {
                int64_t mask = ((int64_t)1 << pos_bwid) - 1;
                uint32_t mc = mk_ci(L, mask);
                uint32_t msk = emit(L, JIR_AND, nty, 2, 0);
                setop(L, msk, 0, val); setop(L, msk, 1, mc);
                setln(L, msk, ln);
                val = msk;
            }
            return val;
        }
        return ld;
    }

    case ND_DEREF: {
        uint32_t ptr = low_expr(L, n->child);
        uint32_t ld = emit(L, JIR_LOAD, nty, 1, 0);
        setop(L, ld, 0, ptr);
        setln(L, ld, ln);
        return ld;
    }

    case ND_ADDROF: {
        /* LOC(x) → return the alloca directly */
        return low_lhs(L, n->child);
    }

    default:
        return mk_ci(L, 0);
    }
}

/* ---- LHS Lowering (address for stores) ---- */

static uint32_t low_lhs(lower_t *L, uint32_t nd)
{
    ast_node_t *n = ND(L, nd);
    if (nd == 0 || n->type == 0) return mk_ci(L, 0);

    switch (n->type) {
    case ND_IDENT: {
        char nm[SK_MAX_IDENT];
        nd_txt(L, n->tok2 ? n->tok2 : n->tok, nm, (int)sizeof(nm));
        int li = find_loc(L, nm);
        if (li >= 0) return L->loc[li].alloc;
        return mk_ci(L, 0);
    }

    case ND_MEMBER: {
        uint32_t base = low_lhs(L, n->child);
        char fn[SK_MAX_IDENT];
        nd_txt(L, n->tok2 ? n->tok2 : n->tok, fn, (int)sizeof(fn));
        ucase(fn);
        uint8_t fld_idx = 0;
        uint32_t base_ty = L->S->nd_types[n->child];
        if (base_ty > 0 && base_ty < (uint32_t)L->S->n_types &&
            L->S->types[base_ty].kind == JT_TABLE) {
            uint32_t tdi = L->S->types[base_ty].extra;
            if (tdi < (uint32_t)L->S->n_tbldf) {
                for (int i = 0; i < L->S->tbldef[tdi].n_flds; i++) {
                    if (ci_eq(L->S->tbldef[tdi].flds[i].name, fn))
                        { fld_idx = (uint8_t)i; break; }
                }
            }
        }
        uint32_t nty = L->S->nd_types[nd];
        uint32_t gep = emit(L, JIR_GEP, nty, 1, fld_idx);
        setop(L, gep, 0, base);
        setln(L, gep, nd_line(L, nd));
        return gep;
    }

    case ND_INDEX: {
        uint32_t base = low_lhs(L, n->child);
        uint32_t idx_v = mk_ci(L, 0);
        uint32_t ai = ND(L, n->child)->sibling;
        if (ai != 0) idx_v = low_expr(L, ai);
        uint32_t nty = L->S->nd_types[nd];
        uint32_t gep = emit(L, JIR_GEP, nty, 2, 0);
        setop(L, gep, 0, base);
        setop(L, gep, 1, idx_v);
        setln(L, gep, nd_line(L, nd));
        return gep;
    }

    case ND_DEREF: {
        return low_expr(L, n->child);
    }

    default:
        return low_expr(L, nd);
    }
}

/* ---- I/O Lowering ----
 * Translating JOVIAL I/O to C runtime calls. The 1970s meet
 * the 1990s, grudgingly. Like a NASA engineer ordering a latte. */

/* dispatch one WRITE item to the correct sk_prt* function */
static void low_prt(lower_t *L, uint32_t val, uint32_t nty, uint32_t ln)
{
    const jtype_t *jt = &L->S->types[nty];
    uint32_t a[1] = { val };

    if (jt->kind == JT_FLOAT) {
        em_xc(L, "sk_prtF", 0, 1, a);
    } else if (jt->kind == JT_CHAR || jt->kind == JT_HOLLER) {
        em_xc(L, "sk_prtS", 0, 1, a);
    } else {
        /* signed, unsigned, bit, status, etc → integer */
        em_xc(L, "sk_prtI", 0, 1, a);
    }
    (void)ln;
}

/* dispatch one formatted WRITE item */
static void low_pfm(lower_t *L, uint32_t val, uint32_t nty,
                     const sm_fspec_t *fs)
{
    const jtype_t *jt = &L->S->types[nty];
    uint32_t w = mk_ci(L, (int64_t)fs->width);

    if (fs->kind == 1 && jt->kind == JT_FLOAT) {
        /* F w.d */
        uint32_t d = mk_ci(L, (int64_t)fs->decim);
        uint32_t a[3] = { val, w, d };
        em_xc(L, "sk_pfmF", 0, 3, a);
    } else if (fs->kind == 2) {
        /* A w (string) */
        uint32_t a[2] = { val, w };
        em_xc(L, "sk_pfmS", 0, 2, a);
    } else {
        /* I w (integer) */
        uint32_t a[2] = { val, w };
        em_xc(L, "sk_pfmI", 0, 2, a);
    }
}

/* WRITE(file, fmt) items; */
static void low_wrt(lower_t *L, uint32_t nd)
{
    ast_node_t *n = ND(L, nd);
    uint32_t ln = nd_line(L, nd);

    /* walk children: [file, fmt, item1, item2, ...] */
    uint32_t c = n->child;
    uint32_t file_nd = c;                         /* child 0: file */
    uint32_t fmt_nd  = ND(L, file_nd)->sibling;   /* child 1: format */

    /* check if format is FREE (ND_NULL sentinel) */
    int is_free = (ND(L, fmt_nd)->type == ND_NULL);

    /* look up FORMAT definition if named */
    const sm_fmt_t *fmt = NULL;
    if (!is_free && ND(L, fmt_nd)->type == ND_IDENT) {
        char fn[SK_MAX_IDENT];
        nd_txt(L, ND(L, fmt_nd)->tok, fn, (int)sizeof(fn));
        ucase(fn);
        for (int i = 0; i < L->S->n_fmts; i++) {
            char up[SK_MAX_IDENT];
            snprintf(up, SK_MAX_IDENT, "%s", L->S->fmts[i].name);
            ucase(up);
            if (ci_eq(up, fn)) { fmt = &L->S->fmts[i]; break; }
        }
    }

    /* emit calls for each item */
    uint32_t item = ND(L, fmt_nd)->sibling;
    int si = 0; /* spec index for formatted output */
    for (int g = 0; g < 256 && item != 0; g++) {
        uint32_t val  = low_expr(L, item);
        uint32_t nty  = L->S->nd_types[item];

        if (fmt && si < fmt->n_spec) {
            low_pfm(L, val, nty, &fmt->specs[si]);
            si++;
        } else {
            /* FREE format: print with separator */
            if (g > 0 && is_free) {
                uint32_t sp = mk_ci(L, (int64_t)add_str(L, " "));
                uint32_t a[1] = { sp };
                em_xc(L, "sk_prtS", 0, 1, a);
            }
            low_prt(L, val, nty, ln);
        }
        item = ND(L, item)->sibling;
    }

    /* trailing newline */
    em_xc(L, "sk_prtN", 0, 0, NULL);
}

/* READ(file, fmt) items; */
static void low_rd(lower_t *L, uint32_t nd)
{
    ast_node_t *n = ND(L, nd);
    uint32_t ln = nd_line(L, nd);

    uint32_t c = n->child;
    uint32_t fmt_nd = ND(L, c)->sibling;
    uint32_t item = ND(L, fmt_nd)->sibling;

    for (int g = 0; g < 256 && item != 0; g++) {
        uint32_t addr = low_lhs(L, item);
        uint32_t nty  = L->S->nd_types[item];
        const jtype_t *jt = &L->S->types[nty];

        uint32_t rv;
        if (jt->kind == JT_FLOAT) {
            rv = em_xc(L, "sk_rdF", nty, 0, NULL);
        } else {
            rv = em_xc(L, "sk_rdI", nty, 0, NULL);
        }

        uint32_t st = emit(L, JIR_STORE, 0, 2, 0);
        setop(L, st, 0, rv);
        setop(L, st, 1, addr);
        setln(L, st, ln);

        item = ND(L, item)->sibling;
    }
}

/* OPEN(path, mode) */
static void low_opn(lower_t *L, uint32_t nd)
{
    ast_node_t *n = ND(L, nd);
    uint32_t path = low_expr(L, n->child);
    uint32_t mode = mk_ci(L, 0);
    if (ND(L, n->child)->sibling != 0)
        mode = low_expr(L, ND(L, n->child)->sibling);
    uint32_t a[2] = { path, mode };
    em_xc(L, "sk_fopn", 0, 2, a);
}

/* CLOSE(handle) */
static void low_cls(lower_t *L, uint32_t nd)
{
    ast_node_t *n = ND(L, nd);
    uint32_t h = low_expr(L, n->child);
    uint32_t a[1] = { h };
    em_xc(L, "sk_fcls", 0, 1, a);
}

/* ---- Statement Lowering ---- */

static void low_stmt(lower_t *L, uint32_t nd)
{
    ast_node_t *n = ND(L, nd);
    if (nd == 0 || n->type == 0) return;
    uint32_t ln = nd_line(L, nd);

    switch (n->type) {
    case ND_ASSIGN: {
        uint32_t addr = low_lhs(L, n->child);
        uint32_t val  = low_expr(L, ND(L, n->child)->sibling);
        uint32_t st = emit(L, JIR_STORE, 0, 2, 0);
        setop(L, st, 0, val);
        setop(L, st, 1, addr);
        setln(L, st, ln);
        break;
    }

    case ND_IF: {
        uint32_t cond = low_expr(L, n->child);
        uint32_t then_b = new_blk(L, "if.then");
        uint32_t else_b = new_blk(L, "if.else");
        uint32_t merg_b = new_blk(L, "if.end");

        /* does this IF have an else block? */
        uint32_t tb_nd = ND(L, n->child)->sibling;
        uint32_t eb_nd = (tb_nd != 0) ? ND(L, tb_nd)->sibling : 0;

        uint32_t bc = emit(L, JIR_BR_COND, 0, 3, 0);
        setop(L, bc, 0, cond);
        setop(L, bc, 1, then_b);
        setop(L, bc, 2, (eb_nd != 0) ? else_b : merg_b);
        setln(L, bc, ln);

        /* then block */
        set_blk(L, then_b);
        if (tb_nd != 0) low_stmt(L, tb_nd);
        if (!blk_term(L)) {
            uint32_t br = emit(L, JIR_BR, 0, 1, 0);
            setop(L, br, 0, merg_b);
        }

        /* else block */
        if (eb_nd != 0) {
            set_blk(L, else_b);
            low_stmt(L, eb_nd);
            if (!blk_term(L)) {
                uint32_t br = emit(L, JIR_BR, 0, 1, 0);
                setop(L, br, 0, merg_b);
            }
        }

        set_blk(L, merg_b);
        break;
    }

    case ND_WHILE: {
        uint32_t hdr_b  = new_blk(L, "wh.hdr");
        uint32_t body_b = new_blk(L, "wh.body");
        uint32_t exit_b = new_blk(L, "wh.exit");

        /* branch to header */
        if (!blk_term(L)) {
            uint32_t br = emit(L, JIR_BR, 0, 1, 0);
            setop(L, br, 0, hdr_b);
        }

        /* header: evaluate condition */
        set_blk(L, hdr_b);
        uint32_t cond = low_expr(L, n->child);
        uint32_t bc = emit(L, JIR_BR_COND, 0, 3, 0);
        setop(L, bc, 0, cond);
        setop(L, bc, 1, body_b);
        setop(L, bc, 2, exit_b);
        setln(L, bc, ln);

        /* body */
        set_blk(L, body_b);
        if (L->lp_dep < JIR_MAX_LOOP)
            L->lp_exit[L->lp_dep++] = exit_b;
        uint32_t c = ND(L, n->child)->sibling;
        for (int g = 0; g < 65536 && c != 0; g++) {
            low_stmt(L, c);
            c = ND(L, c)->sibling;
        }
        if (L->lp_dep > 0) L->lp_dep--;
        if (!blk_term(L)) {
            uint32_t br = emit(L, JIR_BR, 0, 1, 0);
            setop(L, br, 0, hdr_b);
        }

        set_blk(L, exit_b);
        break;
    }

    case ND_FOR: {
        /* tok2 has loop var. children: start, step, condition, body... */
        char lv[SK_MAX_IDENT];
        nd_txt(L, n->tok2 ? n->tok2 : n->tok, lv, (int)sizeof(lv));
        int li = find_loc(L, lv);

        /* init: store start value */
        uint32_t c = n->child;
        if (c != 0 && li >= 0) {
            uint32_t sv = low_expr(L, c);
            uint32_t st = emit(L, JIR_STORE, 0, 2, 0);
            setop(L, st, 0, sv);
            setop(L, st, 1, L->loc[li].alloc);
            setln(L, st, ln);
            c = ND(L, c)->sibling; /* advance to step */
        }

        uint32_t step_nd = c;
        c = (c != 0) ? ND(L, c)->sibling : 0; /* advance to condition */
        uint32_t cond_nd = c;
        c = (c != 0) ? ND(L, c)->sibling : 0; /* advance to body */

        uint32_t hdr_b  = new_blk(L, "for.hdr");
        uint32_t body_b = new_blk(L, "for.body");
        uint32_t step_b = new_blk(L, "for.step");
        uint32_t exit_b = new_blk(L, "for.exit");

        if (!blk_term(L)) {
            uint32_t br = emit(L, JIR_BR, 0, 1, 0);
            setop(L, br, 0, hdr_b);
        }

        /* header: condition */
        set_blk(L, hdr_b);
        if (cond_nd != 0) {
            uint32_t cv = low_expr(L, cond_nd);
            uint32_t bc = emit(L, JIR_BR_COND, 0, 3, 0);
            setop(L, bc, 0, cv);
            setop(L, bc, 1, body_b);
            setop(L, bc, 2, exit_b);
            setln(L, bc, ln);
        } else {
            uint32_t br = emit(L, JIR_BR, 0, 1, 0);
            setop(L, br, 0, body_b);
        }

        /* body */
        set_blk(L, body_b);
        if (L->lp_dep < JIR_MAX_LOOP)
            L->lp_exit[L->lp_dep++] = exit_b;
        for (int g = 0; g < 65536 && c != 0; g++) {
            low_stmt(L, c);
            c = ND(L, c)->sibling;
        }
        if (L->lp_dep > 0) L->lp_dep--;
        if (!blk_term(L)) {
            uint32_t br = emit(L, JIR_BR, 0, 1, 0);
            setop(L, br, 0, step_b);
        }

        /* step: increment + back to header */
        set_blk(L, step_b);
        if (step_nd != 0 && li >= 0) {
            uint32_t cur = emit(L, JIR_LOAD, L->loc[li].type, 1, 0);
            setop(L, cur, 0, L->loc[li].alloc);
            uint32_t sv = low_expr(L, step_nd);
            uint32_t add = emit(L, JIR_ADD, L->loc[li].type, 2, 0);
            setop(L, add, 0, cur);
            setop(L, add, 1, sv);
            uint32_t st = emit(L, JIR_STORE, 0, 2, 0);
            setop(L, st, 0, add);
            setop(L, st, 1, L->loc[li].alloc);
        }
        {
            uint32_t br = emit(L, JIR_BR, 0, 1, 0);
            setop(L, br, 0, hdr_b);
        }

        set_blk(L, exit_b);
        break;
    }

    case ND_CASE: {
        /* lower selector, chain of icmp + br_cond for each branch */
        uint32_t sel = low_expr(L, n->child);
        uint32_t end_b = new_blk(L, "case.end");

        uint32_t c = ND(L, n->child)->sibling;
        for (int g = 0; g < 256 && c != 0; g++) {
            ast_node_t *br = ND(L, c);
            if (br->type == ND_CSBRANCH) {
                uint32_t body_b = new_blk(L, "case.br");
                uint32_t next_b = new_blk(L, "case.nxt");

                /* V() value is first child of branch */
                uint32_t vnd = br->child;
                if (vnd != 0 && ND(L, vnd)->type == ND_STATUSLIT) {
                    uint32_t vv = low_expr(L, vnd);
                    uint32_t cmp = emit(L, JIR_ICMP, 0, 2, JP_EQ);
                    setop(L, cmp, 0, sel);
                    setop(L, cmp, 1, vv);
                    setln(L, cmp, ln);

                    uint32_t bc = emit(L, JIR_BR_COND, 0, 3, 0);
                    setop(L, bc, 0, cmp);
                    setop(L, bc, 1, body_b);
                    setop(L, bc, 2, next_b);
                }

                set_blk(L, body_b);
                /* body stmts are siblings of the V() value */
                uint32_t bs = (vnd != 0) ? ND(L, vnd)->sibling : 0;
                for (int g2 = 0; g2 < 1024 && bs != 0; g2++) {
                    low_stmt(L, bs);
                    bs = ND(L, bs)->sibling;
                }
                if (!blk_term(L)) {
                    uint32_t jmp = emit(L, JIR_BR, 0, 1, 0);
                    setop(L, jmp, 0, end_b);
                }

                set_blk(L, next_b);
            } else if (br->type == ND_DEFAULT) {
                /* default: just lower body inline */
                uint32_t bs = br->child;
                for (int g2 = 0; g2 < 1024 && bs != 0; g2++) {
                    low_stmt(L, bs);
                    bs = ND(L, bs)->sibling;
                }
            }
            c = br->sibling;
        }

        if (!blk_term(L)) {
            uint32_t br = emit(L, JIR_BR, 0, 1, 0);
            setop(L, br, 0, end_b);
        }
        set_blk(L, end_b);
        break;
    }

    case ND_RETURN: {
        if (n->child != 0) {
            uint32_t rv = low_expr(L, n->child);
            uint32_t ret = emit(L, JIR_RET, L->ret_ty, 1, 0);
            setop(L, ret, 0, rv);
            setln(L, ret, ln);
        } else {
            uint32_t ret = emit(L, JIR_RET, 0, 0, 0);
            setln(L, ret, ln);
        }
        break;
    }

    case ND_GOTO: {
        char lab[SK_MAX_IDENT];
        nd_txt(L, n->tok2 ? n->tok2 : n->tok, lab, (int)sizeof(lab));
        uint32_t tgt = map_lbl(L, lab);
        uint32_t br = emit(L, JIR_BR, 0, 1, 0);
        setop(L, br, 0, tgt);
        setln(L, br, ln);
        break;
    }

    case ND_LABEL: {
        char lab[SK_MAX_IDENT];
        nd_txt(L, n->tok2 ? n->tok2 : n->tok, lab, (int)sizeof(lab));
        uint32_t tgt = map_lbl(L, lab);
        /* terminate current block, switch to label block */
        if (!blk_term(L)) {
            uint32_t br = emit(L, JIR_BR, 0, 1, 0);
            setop(L, br, 0, tgt);
        }
        set_blk(L, tgt);
        break;
    }

    case ND_STMTBLK: {
        uint32_t c = n->child;
        for (int g = 0; g < 65536 && c != 0; g++) {
            ast_node_t *ch = ND(L, c);
            if (ch->type == ND_ITEM || ch->type == ND_TABLE ||
                ch->type == ND_DEFINE || ch->type == ND_TYPEDEF ||
                ch->type == ND_FORMAT)
                low_decl(L, c);
            else
                low_stmt(L, c);
            c = ch->sibling;
        }
        break;
    }

    case ND_EXIT: {
        if (L->lp_dep > 0) {
            uint32_t br = emit(L, JIR_BR, 0, 1, 0);
            setop(L, br, 0, L->lp_exit[L->lp_dep - 1]);
            setln(L, br, ln);
        }
        break;
    }

    case ND_ABORT: {
        uint32_t a[1] = { mk_ci(L, 1) };
        em_xc(L, "sk_halt", 0, 1, a);
        setln(L, emit(L, JIR_RET, 0, 0, 0), ln);
        break;
    }
    case ND_STOP: {
        uint32_t a[1] = { mk_ci(L, 0) };
        em_xc(L, "sk_halt", 0, 1, a);
        setln(L, emit(L, JIR_RET, 0, 0, 0), ln);
        break;
    }

    case ND_WRITE:  low_wrt(L, nd); break;
    case ND_READ:   low_rd(L, nd);  break;
    case ND_OPENF:  low_opn(L, nd); break;
    case ND_CLOSEF: low_cls(L, nd); break;

    case ND_CALL: {
        low_expr(L, n->child);
        break;
    }

    case ND_FNCALL: {
        low_expr(L, nd);
        break;
    }

    case ND_ITEM:
    case ND_TABLE:
    case ND_DEFINE:
    case ND_TYPEDEF:
        low_decl(L, nd);
        break;

    case ND_NULL:
        break;

    default:
        break;
    }
}

/* ---- Declaration Lowering ---- */

static void low_decl(lower_t *L, uint32_t nd)
{
    ast_node_t *n = ND(L, nd);
    if (nd == 0 || n->type == 0) return;
    uint32_t ln = nd_line(L, nd);

    switch (n->type) {
    case ND_ITEM: {
        char nm[SK_MAX_IDENT];
        nd_txt(L, n->tok2 ? n->tok2 : n->tok, nm, (int)sizeof(nm));
        uint32_t ty = L->S->nd_types[nd];

        /* emit alloca */
        uint32_t al = emit(L, JIR_ALLOCA, ty, 0, 0);
        setln(L, al, ln);
        add_loc(L, nm, al, ty);

        /* check for initializer (skip typespec child) */
        uint32_t c = n->child;
        if (c != 0 && ND(L, c)->type == ND_TYPESPEC)
            c = ND(L, c)->sibling;
        if (c != 0) {
            uint32_t iv = low_expr(L, c);
            uint32_t st = emit(L, JIR_STORE, 0, 2, 0);
            setop(L, st, 0, iv);
            setop(L, st, 1, al);
            setln(L, st, ln);
        }
        break;
    }

    case ND_TABLE: {
        char nm[SK_MAX_IDENT];
        nd_txt(L, n->tok2 ? n->tok2 : n->tok, nm, (int)sizeof(nm));
        uint32_t ty = L->S->nd_types[nd];
        uint32_t al = emit(L, JIR_ALLOCA, ty, 0, 0);
        setln(L, al, ln);
        add_loc(L, nm, al, ty);
        break;
    }

    case ND_DEFINE:
    case ND_TYPEDEF:
    case ND_FORMAT:
        /* nothing: sema handled these */
        break;

    default:
        break;
    }
}

/* ---- Function Lowering ---- */

static void low_func(lower_t *L, uint32_t nd)
{
    ast_node_t *n = ND(L, nd);
    char nm[SK_MAX_IDENT];
    nd_txt(L, n->tok2 ? n->tok2 : n->tok, nm, (int)sizeof(nm));
    ucase(nm);

    jir_mod_t *M = L->M;
    if (M->n_funcs >= JIR_MAX_FUNCS) return;

    uint32_t fi = M->n_funcs++;
    jir_func_t *f = &M->funcs[fi];
    f->name    = add_str(L, nm);
    f->sema_nd = nd;
    f->n_blks  = 0;
    f->n_inst  = 0;

    /* find return type from sema */
    uint32_t sty = L->S->nd_types[nd];
    if (sty > 0 && sty < (uint32_t)L->S->n_types &&
        L->S->types[sty].kind == JT_PROC)
        f->ret_type = L->S->types[sty].inner;
    else
        f->ret_type = 0;

    L->cur_fn = fi;
    L->ret_ty = f->ret_type;
    int saved_nloc = L->n_loc;
    int saved_nlbl = L->n_lbl;

    /* entry block */
    f->first_blk = M->n_blks;
    uint32_t entry = new_blk(L, "entry");
    set_blk(L, entry);

    /* count + emit params */
    uint16_t pcnt = 0;
    uint32_t c = n->child;
    for (int g = 0; g < 1024 && c != 0; g++) {
        ast_node_t *ch = ND(L, c);
        if (ch->type == ND_PARAM) {
            char pn[SK_MAX_IDENT];
            nd_txt(L, ch->tok2 ? ch->tok2 : ch->tok, pn, (int)sizeof(pn));
            /* params default to S 32 (sema binds actual types via body ITEMs) */
            uint32_t pty = L->S->nd_types[c];
            if (pty == 0) {
                /* look for ITEM in body with same name */
                uint32_t bc = n->child;
                for (int g2 = 0; g2 < 1024 && bc != 0; g2++) {
                    ast_node_t *bn = ND(L, bc);
                    if (bn->type == ND_ITEM) {
                        char in[SK_MAX_IDENT];
                        nd_txt(L, bn->tok2 ? bn->tok2 : bn->tok,
                               in, (int)sizeof(in));
                        if (ci_eq(in, pn)) {
                            pty = L->S->nd_types[bc];
                            break;
                        }
                    }
                    bc = bn->sibling;
                }
            }
            uint32_t al = emit(L, JIR_ALLOCA, pty, 0, 0);
            setln(L, al, nd_line(L, c));
            add_loc(L, pn, al, pty);
            pcnt++;
        }
        c = ch->sibling;
    }
    f->n_params = pcnt;

    /* body: declarations then statements */
    c = n->child;
    for (int g = 0; g < 65536 && c != 0; g++) {
        ast_node_t *ch = ND(L, c);
        if (ch->type != ND_PARAM && ch->type != ND_TYPESPEC) {
            if (ch->type == ND_ITEM || ch->type == ND_TABLE ||
                ch->type == ND_DEFINE || ch->type == ND_TYPEDEF)
                low_decl(L, c);
            else
                low_stmt(L, c);
        }
        c = ch->sibling;
    }

    /* ensure trailing ret */
    if (!blk_term(L)) {
        uint32_t ret = emit(L, JIR_RET, 0, 0, 0);
        (void)ret;
    }

    /* count blocks + instructions belonging to this function */
    f->n_blks = (uint16_t)(M->n_blks - f->first_blk);
    f->n_inst = 0;
    for (uint32_t i = f->first_blk; i < M->n_blks; i++)
        f->n_inst += M->blks[i].n_inst;

    /* restore locals/labels */
    L->n_loc = saved_nloc;
    L->n_lbl = saved_nlbl;
}

/* ---- Top-level lowering ---- */

void jir_init(jir_mod_t *M, const sema_ctx_t *S)
{
    memset(M, 0, sizeof(*M));
    M->S = S;
    M->str_len = 1; /* offset 0 = empty string sentinel */
    M->strs[0] = '\0';
}

int jir_lower(jir_mod_t *M)
{
    lower_t *L = &L_;
    memset(L, 0, sizeof(*L));
    L->M = M;
    L->S = M->S;

    ast_node_t *root = &M->S->nodes[M->S->root];

    /* standalone COMPOOL — empty module, validation only */
    if (root->type == ND_COMPOOL) {
        if (M->n_funcs >= JIR_MAX_FUNCS) return SK_ERR_SEMA;
        uint32_t fi = M->n_funcs++;
        jir_func_t *mf = &M->funcs[fi];
        mf->name = add_str(L, "__CPL__");
        mf->ret_type = 0;
        mf->n_params = 0;
        mf->first_blk = M->n_blks;
        L->cur_fn = fi;
        uint32_t entry = new_blk(L, "entry");
        set_blk(L, entry);
        emit(L, JIR_RET, 0, 0, 0);
        mf->n_blks = 1;
        mf->n_inst = 1;
        return M->n_errs > 0 ? SK_ERR_IR : SK_OK;
    }

    /* first pass: lower PROCs (including inside COMPOOLs) */
    uint32_t c = root->child;
    for (int g = 0; g < 65536 && c != 0; g++) {
        if (ND(L, c)->type == ND_PROC)
            low_func(L, c);
        else if (ND(L, c)->type == ND_COMPOOL) {
            uint32_t cc = ND(L, c)->child;
            for (int g2 = 0; g2 < 65536 && cc != 0; g2++) {
                if (ND(L, cc)->type == ND_PROC)
                    low_func(L, cc);
                cc = ND(L, cc)->sibling;
            }
        }
        c = ND(L, c)->sibling;
    }

    /* second pass: global decls + stmts → implicit "main" function */
    if (M->n_funcs >= JIR_MAX_FUNCS) return SK_ERR_SEMA;
    uint32_t fi = M->n_funcs++;
    jir_func_t *mf = &M->funcs[fi];

    /* program entry = "main" so the C runtime can find it.
     * Your JOVIAL program name is purely decorative at this point,
     * like a monogram on a spacecraft's ejection seat. */
    mf->name     = add_str(L, "main");
    mf->ret_type = 0;
    mf->n_params = 0;
    mf->sema_nd  = M->S->root;
    mf->first_blk = M->n_blks;

    L->cur_fn = fi;
    L->ret_ty = 0;
    L->n_loc  = 0;
    L->n_lbl  = 0;
    L->lp_dep = 0;

    uint32_t entry = new_blk(L, "entry");
    set_blk(L, entry);

    c = root->child;
    for (int g = 0; g < 65536 && c != 0; g++) {
        ast_node_t *ch = ND(L, c);
        if (ch->type != ND_PROC) {
            if (ch->type == ND_ITEM || ch->type == ND_TABLE ||
                ch->type == ND_DEFINE || ch->type == ND_TYPEDEF ||
                ch->type == ND_FORMAT)
                low_decl(L, c);
            else if (ch->type == ND_COMPOOL) {
                uint32_t cc = ch->child;
                for (int g2 = 0; g2 < 65536 && cc != 0; g2++) {
                    ast_node_t *cd = ND(L, cc);
                    if (cd->type == ND_ITEM || cd->type == ND_TABLE ||
                        cd->type == ND_DEFINE || cd->type == ND_TYPEDEF ||
                        cd->type == ND_FORMAT)
                        low_decl(L, cc);
                    cc = cd->sibling;
                }
            } else
                low_stmt(L, c);
        }
        c = ch->sibling;
    }

    if (!blk_term(L)) {
        /* return 0 from main — successful landing */
        uint32_t rv = mk_ci(L, 0);
        uint32_t ret = emit(L, JIR_RET, 0, 1, 0);
        setop(L, ret, 0, rv);
    }

    mf->n_blks = (uint16_t)(M->n_blks - mf->first_blk);
    mf->n_inst = 0;
    for (uint32_t i = mf->first_blk; i < M->n_blks; i++)
        mf->n_inst += M->blks[i].n_inst;

    return M->n_errs > 0 ? SK_ERR_SEMA : SK_OK;
}

/* ---- External Function Registry ---- */

uint32_t jir_xfn(jir_mod_t *M, const char *name)
{
    /* dedup by name */
    for (uint32_t i = 0; i < M->n_xfn; i++) {
        uint32_t off = M->xfuncs[i].name;
        if (off < M->str_len && strcmp(M->strs + off, name) == 0)
            return i;
    }
    if (M->n_xfn >= JIR_MAX_XFUNC) return 0;
    uint32_t idx = M->n_xfn++;
    /* add name to string pool */
    uint32_t slen = (uint32_t)strlen(name);
    if (M->str_len + slen + 1 > JIR_MAX_STRS) return 0;
    uint32_t soff = M->str_len;
    memcpy(M->strs + soff, name, slen + 1);
    M->str_len += slen + 1;
    M->xfuncs[idx].name = soff;
    return idx;
}

/* ---- Opcode Names ---- */

static const char *op_names[] = {
    "nop",
    "add","sub","mul","div","mod","neg",
    "fadd","fsub","fmul","fdiv","fneg",
    "and","or","xor","not","shl","shr",
    "icmp","fcmp",
    "alloca","load","store","gep",
    "br","br_cond","ret","call","xcall",
    "sext","zext","trunc",
    "sitofp","fptosi","fpext","fptrunc",
    "phi"
};

const char *jir_opnm(int op)
{
    if (op >= 0 && op < JIR_OP_COUNT) return op_names[op];
    return "???";
}

static const char *pred_nm[] = { "eq","ne","lt","le","gt","ge" };

/* ---- Dump ---- */

static void fmt_op(const jir_mod_t *M, uint32_t v, char *buf, int sz)
{
    if (JIR_IS_C(v)) {
        uint32_t ci = JIR_C_IDX(v);
        if (ci < M->n_consts) {
            const jir_const_t *c = &M->consts[ci];
            if (c->kind == JC_INT)
                snprintf(buf, (size_t)sz, "$%lld", (long long)c->iv);
            else if (c->kind == JC_FLT) {
                double fv;
                memcpy(&fv, &c->iv, 8);
                snprintf(buf, (size_t)sz, "$%.6g", fv);
            } else
                snprintf(buf, (size_t)sz, "$str(%u)", (unsigned)c->iv);
        } else {
            snprintf(buf, (size_t)sz, "$?%u", ci);
        }
    } else {
        snprintf(buf, (size_t)sz, "%%%u", v);
    }
}

void jir_dump(const jir_mod_t *M)
{
    char tbuf[64], obuf[4][32];

    /* constant pool */
    if (M->n_consts > 0) {
        printf("; constants:\n");
        for (uint32_t i = 0; i < M->n_consts; i++) {
            const jir_const_t *c = &M->consts[i];
            if (c->kind == JC_INT)
                printf(";  $%u = %lld\n", i, (long long)c->iv);
            else if (c->kind == JC_FLT) {
                double fv;
                memcpy(&fv, &c->iv, 8);
                printf(";  $%u = %.6g\n", i, fv);
            }
        }
        printf("\n");
    }

    for (uint32_t fi = 0; fi < M->n_funcs; fi++) {
        const jir_func_t *f = &M->funcs[fi];
        const char *fname = (f->name < M->str_len) ?
                            M->strs + f->name : "?";

        /* return type */
        if (f->ret_type > 0 && f->ret_type < (uint32_t)M->S->n_types)
            jt_str(M->S, f->ret_type, tbuf, (int)sizeof(tbuf));
        else
            snprintf(tbuf, sizeof(tbuf), "void");

        printf("func @%s(%u params) -> %s {\n", fname, f->n_params, tbuf);

        for (uint32_t bi = f->first_blk;
             bi < f->first_blk + f->n_blks && bi < M->n_blks; bi++) {
            const jir_blk_t *b = &M->blks[bi];
            const char *bname = (b->name < M->str_len) ?
                                M->strs + b->name : "?";
            printf("bb%u:  ; %s\n", bi, bname);

            for (uint32_t ii = b->first; ii < b->first + b->n_inst &&
                 ii < M->n_inst; ii++) {
                const jir_inst_t *I = &M->insts[ii];

                /* format operands */
                int nops = (I->n_ops == 0xFF) ? 0 : I->n_ops;
                for (int j = 0; j < nops && j < 4; j++)
                    fmt_op(M, I->ops[j], obuf[j], 32);

                /* type string */
                if (I->type > 0 && I->type < (uint32_t)M->S->n_types)
                    jt_str(M->S, I->type, tbuf, (int)sizeof(tbuf));
                else
                    tbuf[0] = '\0';

                printf("  ");

                /* instructions that produce values get %N = */
                int has_val = I->op != JIR_STORE && I->op != JIR_BR &&
                              I->op != JIR_BR_COND && I->op != JIR_RET &&
                              I->op != JIR_NOP;
                if (has_val)
                    printf("%%%u = ", ii);

                printf("%s", jir_opnm(I->op));

                /* predicate for CMP */
                if ((I->op == JIR_ICMP || I->op == JIR_FCMP) &&
                    I->subop < 6)
                    printf(" %s", pred_nm[I->subop]);

                /* type */
                if (tbuf[0]) printf(" %s", tbuf);

                /* operands */
                if (I->op == JIR_BR) {
                    printf(" bb%u", I->ops[0]);
                } else if (I->op == JIR_BR_COND) {
                    printf(" %s, bb%u, bb%u", obuf[0], I->ops[1], I->ops[2]);
                } else if (I->op == JIR_PHI) {
                    for (int j = 0; j < nops; j++)
                        printf("%s%s", j ? ", " : " ", obuf[j]);
                } else if (I->op == JIR_XCALL && nops >= 1 &&
                           JIR_IS_C(I->ops[0])) {
                    /* deref const pool to get xfunc idx */
                    uint32_t ci = JIR_C_IDX(I->ops[0]);
                    uint32_t xi = (ci < M->n_consts)
                                  ? (uint32_t)M->consts[ci].iv : 0;
                    const char *xn = (xi < M->n_xfn &&
                                      M->xfuncs[xi].name < M->str_len)
                                     ? M->strs + M->xfuncs[xi].name : "?";
                    printf(" @%s", xn);
                    for (int j = 1; j < nops; j++)
                        printf(", %s", obuf[j]);
                } else if (I->op == JIR_GEP && I->subop > 0) {
                    printf(" %s .%u", obuf[0], I->subop);
                } else {
                    for (int j = 0; j < nops; j++)
                        printf("%s%s", j ? ", " : " ", obuf[j]);
                }

                if (I->line > 0)
                    printf("  ; L%u", I->line);
                printf("\n");
            }
        }
        printf("}\n\n");
    }
}
