/* sema.c -- J73 semantic analysis
 * Two passes over an AST, like re-reading your own code the next morning
 * and discovering you were a different person when you wrote it.
 *
 * Pass 1: collect declarations (items, tables, procs, defines, types)
 * Pass 2: check procedure bodies, expressions, assignments
 *
 * The type pool interns every distinct type once. The symbol table is
 * a flat array with scope snapshots. No malloc. No hash tables. No mercy. */

#include "sema.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>

/* ---- Helpers ---- */

/* bounds-checked node access. index 0 is sentinel (type=0).
 * If you ask for node 0 you get the void. Fair warning. */
static inline ast_node_t *ND(sema_ctx_t *S, uint32_t i)
{
    if (i == 0 || i >= S->n_nodes) return &S->nodes[0];
    return &S->nodes[i];
}

/* extract token text into buf. same pattern as parser's tok_text. */
static void nd_text(const sema_ctx_t *S, uint32_t ti, char *buf, int sz)
{
    if (ti >= S->n_toks) { buf[0] = '\0'; return; }
    const token_t *t = &S->toks[ti];
    int n = (int)t->len;
    if (n >= sz) n = sz - 1;
    memcpy(buf, S->src + t->offset, (size_t)n);
    buf[n] = '\0';
}

/* accumulate an error with source location from node's token */
static void sm_err(sema_ctx_t *S, uint32_t nd, const char *fmt, ...)
{
    if (S->n_errs >= SK_MAX_ERRORS) return;
    sk_err_t *e = &S->errors[S->n_errs++];
    e->code = SK_ERR_SEMA;

    /* location from node's token */
    const ast_node_t *n = ND(S, nd);
    if (n->tok < S->n_toks) {
        const token_t *t = &S->toks[n->tok];
        e->loc.line   = t->line;
        e->loc.col    = t->col;
        e->loc.offset = t->offset;
    } else {
        e->loc.line = 0; e->loc.col = 0; e->loc.offset = 0;
    }

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->msg, sizeof(e->msg), fmt, ap);
    va_end(ap);
}

/* annotate: nd_types[nd] = type, return type */
static uint32_t annot(sema_ctx_t *S, uint32_t nd, uint32_t type)
{
    if (nd > 0 && nd < (uint32_t)SK_MAX_NODES)
        S->nd_types[nd] = type;
    return type;
}

/* case-insensitive compare (J73 is case-insensitive) */
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

/* upper-case a string in place */
static void ucase(char *s)
{
    for (; *s; s++) *s = (char)toupper((unsigned char)*s);
}

/* ---- Type Pool ----
 * Every distinct type exists exactly once, like a Platonic ideal
 * but with more uint16_t fields and less philosophical rigour. */

/* intern a type: linear dedup. O(n) per insert, but n <= 2048
 * and we're compiling JOVIAL, not training a neural network. */
static uint32_t jt_intern(sema_ctx_t *S, int kind, int width,
                          int scale, int n_extra,
                          uint32_t inner, uint32_t extra)
{
    /* search for existing match */
    for (int i = 1; i < S->n_types; i++) {
        const jtype_t *t = &S->types[i];
        if (t->kind == (uint8_t)kind && t->width == (uint16_t)width &&
            t->scale == (int16_t)scale && t->n_extra == (uint16_t)n_extra &&
            t->inner == inner && t->extra == extra)
            return (uint32_t)i;
    }
    if (S->n_types >= SM_MAX_TYPES) return 0; /* overflow -> sentinel */
    int idx = S->n_types++;
    jtype_t *t = &S->types[idx];
    t->kind    = (uint8_t)kind;
    t->pad     = 0;
    t->width   = (uint16_t)width;
    t->scale   = (int16_t)scale;
    t->n_extra = (uint16_t)n_extra;
    t->inner   = inner;
    t->extra   = extra;
    return (uint32_t)idx;
}

/* convenience constructors */
static uint32_t jt_void(sema_ctx_t *S)
    { return jt_intern(S, JT_VOID, 0, 0, 0, 0, 0); }
static uint32_t jt_s(sema_ctx_t *S, int w)
    { return jt_intern(S, JT_SIGNED, w, 0, 0, 0, 0); }
static uint32_t jt_u(sema_ctx_t *S, int w)
    { return jt_intern(S, JT_UNSIGN, w, 0, 0, 0, 0); }
static uint32_t jt_f(sema_ctx_t *S, int w)
    { return jt_intern(S, JT_FLOAT, w, 0, 0, 0, 0); }
static uint32_t jt_b(sema_ctx_t *S, int w)
    { return jt_intern(S, JT_BIT, w, 0, 0, 0, 0); }
static uint32_t jt_c(sema_ctx_t *S, int w)
    { return jt_intern(S, JT_CHAR, w, 0, 0, 0, 0); }
static uint32_t jt_h(sema_ctx_t *S, int w)
    { return jt_intern(S, JT_HOLLER, w, 0, 0, 0, 0); }
static uint32_t jt_a(sema_ctx_t *S, int w, int sc)
    { return jt_intern(S, JT_FIXED, w, sc, 0, 0, 0); }
static uint32_t jt_ptr(sema_ctx_t *S, uint32_t inner)
    { return jt_intern(S, JT_PTR, 0, 0, 0, inner, 0); }
static uint32_t jt_err(sema_ctx_t *S)
    { return jt_intern(S, JT_ERROR, 0, 0, 0, 0, 0); }

/* queries */
static inline int is_err(const sema_ctx_t *S, uint32_t t)
    { return t == 0 || t >= (uint32_t)S->n_types || S->types[t].kind == JT_ERROR; }
static inline int is_num(const sema_ctx_t *S, uint32_t t)
    { uint8_t k = S->types[t].kind;
      return k==JT_SIGNED||k==JT_UNSIGN||k==JT_FLOAT||k==JT_FIXED; }
static inline int is_int(const sema_ctx_t *S, uint32_t t)
    { uint8_t k = S->types[t].kind; return k==JT_SIGNED||k==JT_UNSIGN; }
static inline int is_flt(const sema_ctx_t *S, uint32_t t)
    { return S->types[t].kind == JT_FLOAT; }
static inline int is_fix(const sema_ctx_t *S, uint32_t t)
    { return S->types[t].kind == JT_FIXED; }
static inline int is_bit(const sema_ctx_t *S, uint32_t t)
    { return S->types[t].kind == JT_BIT; }
static inline int is_chr(const sema_ctx_t *S, uint32_t t)
    { return S->types[t].kind == JT_CHAR; }
static inline int is_ptr(const sema_ctx_t *S, uint32_t t)
    { return S->types[t].kind == JT_PTR; }
static inline int is_tbl(const sema_ctx_t *S, uint32_t t)
    { return S->types[t].kind == JT_TABLE; }
static inline int is_sts(const sema_ctx_t *S, uint32_t t)
    { return S->types[t].kind == JT_STATUS; }
static inline int is_prc(const sema_ctx_t *S, uint32_t t)
    { return S->types[t].kind == JT_PROC; }

/* format type as human-readable string */
int jt_str(const sema_ctx_t *S, uint32_t tidx, char *buf, int sz)
{
    if (tidx == 0 || tidx >= (uint32_t)S->n_types) {
        snprintf(buf, (size_t)sz, "<invalid>");
        return -1;
    }
    const jtype_t *t = &S->types[tidx];
    switch (t->kind) {
    case JT_VOID:   snprintf(buf, (size_t)sz, "VOID"); break;
    case JT_SIGNED:  snprintf(buf, (size_t)sz, "S %u", t->width); break;
    case JT_UNSIGN:  snprintf(buf, (size_t)sz, "U %u", t->width); break;
    case JT_FLOAT:   snprintf(buf, (size_t)sz, "F %u", t->width); break;
    case JT_BIT:     snprintf(buf, (size_t)sz, "B %u", t->width); break;
    case JT_CHAR:    snprintf(buf, (size_t)sz, "C %u", t->width); break;
    case JT_HOLLER:  snprintf(buf, (size_t)sz, "H %u", t->width); break;
    case JT_FIXED:   snprintf(buf, (size_t)sz, "A %u D %d", t->width, t->scale); break;
    case JT_STATUS:  snprintf(buf, (size_t)sz, "STATUS(%u vals)", t->n_extra); break;
    case JT_PTR: {
        char ibuf[64];
        jt_str(S, t->inner, ibuf, (int)sizeof(ibuf));
        snprintf(buf, (size_t)sz, "PTR(%s)", ibuf);
        break;
    }
    case JT_TABLE:   snprintf(buf, (size_t)sz, "TABLE(%u flds)", t->n_extra); break;
    case JT_ARRAY: {
        char ibuf[64];
        jt_str(S, t->inner, ibuf, (int)sizeof(ibuf));
        snprintf(buf, (size_t)sz, "ARRAY(%s)", ibuf);
        break;
    }
    case JT_PROC: {
        char ibuf[64];
        jt_str(S, t->inner, ibuf, (int)sizeof(ibuf));
        snprintf(buf, (size_t)sz, "PROC(%u params)->%s", t->n_extra, ibuf);
        break;
    }
    case JT_ERROR:   snprintf(buf, (size_t)sz, "ERROR"); break;
    default:         snprintf(buf, (size_t)sz, "?%d", t->kind); break;
    }
    return 0;
}

/* ---- Symbol Table ----
 * A flat array with scope snapshots. No trees, no hashing, just
 * a linear scan backwards. Like looking for your keys by starting
 * at the pub and retracing your steps. */

static void push_scp(sema_ctx_t *S)
{
    if (S->scp_dep >= SM_MAX_SCOPES) return;
    S->scp_stk[S->scp_dep++] = S->n_syms;
}

static void pop_scp(sema_ctx_t *S)
{
    if (S->scp_dep <= 0) return;
    S->n_syms = S->scp_stk[--S->scp_dep];
}

static int add_sym(sema_ctx_t *S, const char *name, uint32_t type,
                   uint32_t ast_nd, int kind, uint16_t flags)
{
    if (S->n_syms >= SM_MAX_SYMS) return -1;
    sema_sym_t *sy = &S->syms[S->n_syms++];
    memset(sy, 0, sizeof(*sy));
    snprintf(sy->name, SK_MAX_IDENT, "%s", name);
    ucase(sy->name);
    sy->type   = type;
    sy->ast_nd = ast_nd;
    sy->kind   = (uint8_t)kind;
    sy->scope  = (uint8_t)S->scp_dep;
    sy->flags  = flags;
    sy->cval   = 0;
    return S->n_syms - 1;
}

static int add_csym(sema_ctx_t *S, const char *name, uint32_t type,
                    uint32_t ast_nd, int64_t cval)
{
    int idx = add_sym(S, name, type, ast_nd, SYM_CONST, NF_CONST);
    if (idx >= 0) S->syms[idx].cval = cval;
    return idx;
}

/* backward linear search -- most recent (innermost scope) wins */
static int find_sym(const sema_ctx_t *S, const char *name)
{
    /* upper-case the query on a tiny local buf */
    char up[SK_MAX_IDENT];
    snprintf(up, SK_MAX_IDENT, "%s", name);
    ucase(up);
    for (int i = S->n_syms - 1; i >= 0; i--) {
        if (ci_eq(S->syms[i].name, up)) return i;
    }
    return -1;
}

/* ---- Type Resolution ----
 * Turning the parser's vague gestures at types into
 * actual interned type indices. The bureaucracy continues. */

/* resolve ND_TYPESPEC to jtype index */
static uint32_t res_type(sema_ctx_t *S, uint32_t nd)
{
    const ast_node_t *n = ND(S, nd);
    if (n->type != ND_TYPESPEC) return jt_err(S);

    int bt    = n->aux;   /* base_type_t */
    int width = (int)n->aux2;
    int scale = (int)n->val;

    switch (bt) {
    case BT_VOID:     return jt_void(S);
    case BT_SIGNED:   return jt_s(S, width);
    case BT_UNSIGNED: return jt_u(S, width);
    case BT_FLOAT:    return jt_f(S, width);
    case BT_BIT:      return jt_b(S, width);
    case BT_CHAR:     return jt_c(S, width);
    case BT_HOLLER:   return jt_h(S, width);
    case BT_FIXED:    return jt_a(S, width, scale);

    case BT_STATUS: {
        /* create STATUS def from ND_STATUSVAL children */
        if (S->n_stdef >= SM_MAX_STDEF) {
            sm_err(S, nd, "STATUS definition overflow");
            return jt_err(S);
        }
        int si = S->n_stdef++;
        sm_stdef_t *sd = &S->stdef[si];
        sd->n_vals = 0;

        uint32_t c = n->child;
        for (int guard = 0; guard < SM_MAX_STVALS && c != 0; guard++) {
            const ast_node_t *sv = ND(S, c);
            if (sv->type == ND_STATUSVAL && sd->n_vals < SM_MAX_STVALS) {
                char vn[SK_MAX_IDENT];
                nd_text(S, sv->tok2 ? sv->tok2 : sv->tok, vn,
                        (int)sizeof(vn));
                ucase(vn);
                snprintf(sd->vals[sd->n_vals], SK_MAX_IDENT, "%s", vn);
                sd->n_vals++;
            }
            c = sv->sibling;
        }
        return jt_intern(S, JT_STATUS, 0, 0, sd->n_vals,
                         0, (uint32_t)si);
    }

    case BT_POINTER: {
        /* child typespec is the pointee */
        uint32_t inner = jt_void(S);
        if (n->child != 0)
            inner = res_type(S, n->child);
        return jt_ptr(S, inner);
    }

    case BT_TYPEREF: {
        /* named type reference -- look up in symbol table */
        char tn[SK_MAX_IDENT];
        nd_text(S, n->tok2 ? n->tok2 : n->tok, tn, (int)sizeof(tn));
        int si = find_sym(S, tn);
        if (si < 0) {
            sm_err(S, nd, "unknown type '%s'", tn);
            return jt_err(S);
        }
        if (S->syms[si].kind == SYM_TYPE || S->syms[si].kind == SYM_TABLE)
            return S->syms[si].type;
        /* STATUS type used as ITEM type (e.g. ITEM X HEADING) */
        return S->syms[si].type;
    }

    default:
        sm_err(S, nd, "unknown base type %d", bt);
        return jt_err(S);
    }
}

/* resolve LIKE: copy the source symbol's type.
 * The parser stores LIKE X as: ND_ITEM {NF_LIKE} -> child ND_IDENT 'X' */
static uint32_t res_like(sema_ctx_t *S, uint32_t nd)
{
    const ast_node_t *n = ND(S, nd);
    char ln[SK_MAX_IDENT];

    /* walk children looking for an ND_IDENT -- that's the LIKE source */
    uint32_t c = n->child;
    for (int g = 0; g < 32 && c != 0; g++) {
        const ast_node_t *ch = ND(S, c);
        if (ch->type == ND_IDENT) {
            nd_text(S, ch->tok2 ? ch->tok2 : ch->tok, ln,
                    (int)sizeof(ln));
            int si = find_sym(S, ln);
            if (si < 0) {
                sm_err(S, nd, "LIKE target '%s' not found", ln);
                return jt_err(S);
            }
            return S->syms[si].type;
        }
        c = ch->sibling;
    }

    /* fallback: try tok2 */
    nd_text(S, n->tok2 ? n->tok2 : n->tok, ln, (int)sizeof(ln));
    int si = find_sym(S, ln);
    if (si < 0) {
        sm_err(S, nd, "LIKE target '%s' not found", ln);
        return jt_err(S);
    }
    return S->syms[si].type;
}

/* ---- Constant Evaluator ----
 * A tiny interpreter that runs at compile time, because 1970s
 * language designers thought "just fold it" was obvious. */

/* evaluate compile-time constant expressions (DEFINE values, dim bounds) */
static int64_t eval_const(sema_ctx_t *S, uint32_t nd, int *ok)
{
    const ast_node_t *n = ND(S, nd);
    *ok = 1;

    switch (n->type) {
    case ND_INTLIT:
        return n->val;

    case ND_UNARY:
        if (n->aux == OP_NEG) {
            int sub_ok;
            int64_t v = eval_const(S, n->child, &sub_ok);
            *ok = sub_ok;
            return -v;
        }
        if (n->aux == OP_POS) {
            return eval_const(S, n->child, ok);
        }
        break;

    case ND_BINARY: {
        int lok, rok;
        int64_t lv = eval_const(S, n->child, &lok);
        int64_t rv = eval_const(S, ND(S, n->child)->sibling, &rok);
        *ok = lok && rok;
        if (!*ok) return 0;
        switch (n->aux) {
        case OP_ADD: return lv + rv;
        case OP_SUB: return lv - rv;
        case OP_MUL: return lv * rv;
        case OP_DIV: return rv != 0 ? lv / rv : 0;
        case OP_MOD: return rv != 0 ? lv % rv : 0;
        default: break;
        }
        break;
    }

    case ND_IDENT: {
        char nm[SK_MAX_IDENT];
        nd_text(S, n->tok2 ? n->tok2 : n->tok, nm, (int)sizeof(nm));
        int si = find_sym(S, nm);
        if (si >= 0 && S->syms[si].kind == SYM_CONST) {
            return S->syms[si].cval;
        }
        break;
    }

    default: break;
    }

    *ok = 0;
    sm_err(S, nd, "not a constant expression");
    return 0;
}

/* ---- Forward declarations ---- */

static uint32_t chk_expr(sema_ctx_t *S, uint32_t nd);
static void chk_stmt(sema_ctx_t *S, uint32_t nd);
static void col_decl(sema_ctx_t *S, uint32_t nd);

/* ---- Pass 1: Collect Declarations ----
 * Walk the AST like a census taker, writing down everyone's name
 * and type before asking them any difficult questions. */

/* collect ITEM declaration */
static void col_item(sema_ctx_t *S, uint32_t nd)
{
    ast_node_t *n = ND(S, nd);
    char nm[SK_MAX_IDENT];
    nd_text(S, n->tok2 ? n->tok2 : n->tok, nm, (int)sizeof(nm));

    uint32_t jt;
    if (n->flags & NF_LIKE) {
        jt = res_like(S, nd);
    } else {
        /* first child should be ND_TYPESPEC */
        uint32_t tc = n->child;
        if (tc != 0 && ND(S, tc)->type == ND_TYPESPEC) {
            jt = res_type(S, tc);
        } else {
            sm_err(S, nd, "ITEM '%s' has no type", nm);
            jt = jt_err(S);
        }
    }

    annot(S, nd, jt);

    if (n->flags & NF_CONST) {
        /* constant item -- evaluate initializer */
        uint32_t c = n->child;
        /* skip typespec */
        if (c != 0 && ND(S, c)->type == ND_TYPESPEC)
            c = ND(S, c)->sibling;
        if (c != 0) {
            int ok;
            int64_t cv = eval_const(S, c, &ok);
            if (ok) add_csym(S, nm, jt, nd, cv);
            else    add_sym(S, nm, jt, nd, SYM_CONST, n->flags);
        } else {
            add_sym(S, nm, jt, nd, SYM_CONST, n->flags);
        }
    } else {
        add_sym(S, nm, jt, nd, SYM_VAR, n->flags);
    }
}

/* collect TABLE declaration */
static void col_tabl(sema_ctx_t *S, uint32_t nd)
{
    ast_node_t *n = ND(S, nd);
    char nm[SK_MAX_IDENT];
    nd_text(S, n->tok2 ? n->tok2 : n->tok, nm, (int)sizeof(nm));

    if (S->n_tbldf >= SM_MAX_TBLDF) {
        sm_err(S, nd, "TABLE definition overflow");
        return;
    }
    int ti = S->n_tbldf++;
    sm_tbldf_t *td = &S->tbldef[ti];
    td->n_flds = 0;
    td->ast_nd = nd;
    td->lo_dim = 0;
    td->hi_dim = 0;

    /* walk children: DIMPAIR(s) then ITEM(s) */
    uint32_t c = n->child;
    for (int guard = 0; guard < 1024 && c != 0; guard++) {
        ast_node_t *ch = ND(S, c);
        if (ch->type == ND_DIMPAIR) {
            int ok_lo = 0, ok_hi = 0;
            uint32_t lo_nd = ch->child;
            uint32_t hi_nd = lo_nd ? ND(S, lo_nd)->sibling : 0;
            if (lo_nd) td->lo_dim = (int32_t)eval_const(S, lo_nd, &ok_lo);
            if (hi_nd) td->hi_dim = (int32_t)eval_const(S, hi_nd, &ok_hi);
            c = ch->sibling;
            continue;
        }
        if (ch->type == ND_ITEM && td->n_flds < SM_MAX_FIELDS) {
            /* resolve field type */
            char fn[SK_MAX_IDENT];
            nd_text(S, ch->tok2 ? ch->tok2 : ch->tok, fn,
                    (int)sizeof(fn));

            uint32_t fjt;
            if (ch->flags & NF_LIKE) {
                fjt = res_like(S, c);
            } else {
                /* type spec is first child, unless OVERLAY/POS
                 * shoved an ident in front of it */
                uint32_t ftc = ch->child;
                if (ftc != 0 && ND(S, ftc)->type != ND_TYPESPEC)
                    ftc = ND(S, ftc)->sibling;
                if (ftc != 0 && ND(S, ftc)->type == ND_TYPESPEC)
                    fjt = res_type(S, ftc);
                else
                    fjt = jt_err(S);
            }
            annot(S, c, fjt);

            sm_fld_t *f = &td->flds[td->n_flds++];
            snprintf(f->name, SK_MAX_IDENT, "%s", fn);
            ucase(f->name);
            f->jtype  = fjt;
            f->ast_nd = c;

            /* OVERLAY width check: overlay must not exceed overlaid */
            if ((ch->flags & NF_OVERLAY) && td->n_flds >= 2) {
                uint16_t ow = S->types[fjt].width;
                uint16_t pw = S->types[
                    td->flds[td->n_flds - 2].jtype].width;
                if (ow > pw)
                    sm_err(S, c, "OVERLAY field '%s' (%u bits)"
                           " wider than overlaid field (%u bits)",
                           fn, ow, pw);
            }
        }
        c = ch->sibling;
    }

    /* create TABLE jtype: n_extra = field count, extra = tbldef index */
    uint32_t jt = jt_intern(S, JT_TABLE, 0, 0, td->n_flds, 0, (uint32_t)ti);
    annot(S, nd, jt);
    add_sym(S, nm, jt, nd, SYM_TABLE, n->flags);
}

/* collect PROC declaration */
static void col_proc(sema_ctx_t *S, uint32_t nd)
{
    ast_node_t *n = ND(S, nd);
    char nm[SK_MAX_IDENT];
    nd_text(S, n->tok2 ? n->tok2 : n->tok, nm, (int)sizeof(nm));

    /* walk children: ND_PARAM(s), optional ND_TYPESPEC, then body */
    int pstart = S->n_params;
    int pcnt   = 0;
    uint32_t ret_type = jt_void(S);

    uint32_t c = n->child;
    for (int guard = 0; guard < 1024 && c != 0; guard++) {
        ast_node_t *ch = ND(S, c);
        if (ch->type == ND_PARAM) {
            /* params are name-only at declaration; types bound in Pass 2 */
            if (S->n_params < SM_MAX_PARAMS) {
                S->prm_pool[S->n_params++] = 0; /* placeholder */
                pcnt++;
            }
        } else if (ch->type == ND_TYPESPEC) {
            ret_type = res_type(S, c);
        }
        /* skip body nodes (ND_STMTBLK, statements) -- Pass 2 handles those */
        c = ch->sibling;
    }

    uint32_t jt = jt_intern(S, JT_PROC, 0, 0, pcnt, ret_type,
                            (uint32_t)pstart);
    annot(S, nd, jt);
    add_sym(S, nm, jt, nd, SYM_PROC, n->flags);
}

/* collect DEFINE */
static void col_defn(sema_ctx_t *S, uint32_t nd)
{
    ast_node_t *n = ND(S, nd);
    char nm[SK_MAX_IDENT];
    nd_text(S, n->tok2 ? n->tok2 : n->tok, nm, (int)sizeof(nm));

    int64_t cv = 0;
    int ok = 0;
    if (n->child != 0)
        cv = eval_const(S, n->child, &ok);

    uint32_t jt = jt_s(S, 32); /* DEFINE values are signed 32-bit by default */
    annot(S, nd, jt);
    if (ok)
        add_csym(S, nm, jt, nd, cv);
    else
        add_sym(S, nm, jt, nd, SYM_CONST, 0);
}

/* collect TYPE definition */
static void col_tdef(sema_ctx_t *S, uint32_t nd)
{
    ast_node_t *n = ND(S, nd);
    char nm[SK_MAX_IDENT];
    nd_text(S, n->tok2 ? n->tok2 : n->tok, nm, (int)sizeof(nm));

    /* child is the typespec */
    uint32_t jt = jt_err(S);
    if (n->child != 0)
        jt = res_type(S, n->child);

    annot(S, nd, jt);
    add_sym(S, nm, jt, nd, SYM_TYPE, 0);
}

/* collect FORMAT declaration into fmts[] */
static void col_fmt(sema_ctx_t *S, uint32_t nd)
{
    ast_node_t *n = ND(S, nd);
    if (S->n_fmts >= SM_MAX_FMTS) return;

    sm_fmt_t *f = &S->fmts[S->n_fmts++];
    memset(f, 0, sizeof(*f));
    nd_text(S, n->tok2 ? n->tok2 : n->tok, f->name, SK_MAX_IDENT);

    /* walk ND_FMTSP children */
    uint32_t c = n->child;
    for (int g = 0; g < 256 && c != 0; g++) {
        ast_node_t *sp = ND(S, c);
        if (sp->type == ND_FMTSP && f->n_spec < SM_MAX_FSPEC) {
            sm_fspec_t *fs = &f->specs[f->n_spec++];
            fs->kind  = (uint8_t)sp->aux;
            fs->width = sp->aux2;
            fs->decim = (uint16_t)sp->val;
            fs->str   = (uint16_t)sp->tok2;
        }
        c = sp->sibling;
    }
}

/* main Pass 1 dispatcher */
static void col_decl(sema_ctx_t *S, uint32_t nd)
{
    ast_node_t *n = ND(S, nd);

    switch (n->type) {
    case ND_ITEM:    col_item(S, nd); break;
    case ND_TABLE:   col_tabl(S, nd); break;
    case ND_PROC:    col_proc(S, nd); break;
    case ND_DEFINE:  col_defn(S, nd); break;
    case ND_TYPEDEF: col_tdef(S, nd); break;
    case ND_FORMAT:  col_fmt(S, nd);  break;

    case ND_BLOCK: {
        push_scp(S);
        uint32_t c = n->child;
        for (int g = 0; g < 65536 && c != 0; g++) {
            col_decl(S, c);
            c = ND(S, c)->sibling;
        }
        pop_scp(S);
        break;
    }

    case ND_COMPOOL: {
        char nm[SK_MAX_IDENT];
        nd_text(S, n->tok2 ? n->tok2 : n->tok, nm, (int)sizeof(nm));
        add_sym(S, nm, jt_void(S), nd, SYM_CPOOL, 0);
        /* no scope push — inline COMPOOL exports to enclosing scope */
        uint32_t c = n->child;
        for (int g = 0; g < 65536 && c != 0; g++) {
            col_decl(S, c);
            c = ND(S, c)->sibling;
        }
        break;
    }

    default:
        /* non-declaration nodes: skip in Pass 1 */
        break;
    }
}

/* ---- Pass 2: Expression Type Checking ----
 * Now we actually read the forms everyone filled in during Pass 1
 * and discover half of them are illegible. */

/* JOVIAL arithmetic conversions -- MIL-STD-1589C section 4.2
 * basically C's usual arithmetic conversions if C had been
 * designed by committee at a defence contractor. Which it was. */
static uint32_t arith_cv(sema_ctx_t *S, uint32_t lt, uint32_t rt, int op)
{
    /* error poison */
    if (is_err(S, lt) || is_err(S, rt)) return jt_err(S);

    /* comparison operators -> BIT(1) */
    if (op >= OP_EQ && op <= OP_GE) return jt_b(S, 1);

    /* logical operators: both must be BIT */
    if (op == OP_AND || op == OP_OR || op == OP_XOR || op == OP_EQV) {
        if (!is_bit(S, lt) || !is_bit(S, rt)) {
            return jt_err(S);
        }
        int mw = S->types[lt].width;
        if (S->types[rt].width > (uint16_t)mw)
            mw = (int)S->types[rt].width;
        return jt_b(S, mw);
    }

    /* float promotion: either float -> float(max width) */
    if (is_flt(S, lt) || is_flt(S, rt)) {
        int mw = 64; /* default float width */
        if (is_flt(S, lt) && S->types[lt].width > 0)
            mw = (int)S->types[lt].width;
        if (is_flt(S, rt) && (int)S->types[rt].width > mw)
            mw = (int)S->types[rt].width;
        return jt_f(S, mw);
    }

    /* fixed-point: either fixed -> fixed(max width, handle scale) */
    if (is_fix(S, lt) || is_fix(S, rt)) {
        int mw = (int)S->types[lt].width;
        int ms = (int)S->types[lt].scale;
        if ((int)S->types[rt].width > mw) mw = (int)S->types[rt].width;
        if ((int)S->types[rt].scale > ms) ms = (int)S->types[rt].scale;
        return jt_a(S, mw, ms);
    }

    /* integer arithmetic */
    if (is_int(S, lt) && is_int(S, rt)) {
        int lw = (int)S->types[lt].width;
        int rw = (int)S->types[rt].width;
        int mw = lw > rw ? lw : rw;

        /* S+S->S, U+U->U, S+U->S(max+1 clamped 64) */
        int ls = S->types[lt].kind == JT_SIGNED;
        int rs = S->types[rt].kind == JT_SIGNED;
        if (ls && rs) return jt_s(S, mw);
        if (!ls && !rs) return jt_u(S, mw);
        /* mixed: signed wins, widen */
        mw = mw + 1;
        if (mw > 64) mw = 64;
        return jt_s(S, mw);
    }

    /* numeric but mismatched (e.g. BIT + S) -- promote to signed */
    if (is_num(S, lt) || is_num(S, rt))
        return jt_s(S, 32);

    return jt_err(S);
}

/* check expression, return jtype, annotate nd_types */
static uint32_t chk_expr(sema_ctx_t *S, uint32_t nd)
{
    ast_node_t *n = ND(S, nd);
    if (nd == 0 || n->type == 0) return jt_err(S);

    switch (n->type) {
    case ND_INTLIT:
        return annot(S, nd, jt_s(S, 32));

    case ND_FLTLIT:
        return annot(S, nd, jt_f(S, 64));

    case ND_STRLIT: {
        /* string length = token length minus the two quote chars */
        char buf[SK_MAX_IDENT];
        nd_text(S, n->tok, buf, (int)sizeof(buf));
        int slen = (int)strlen(buf);
        if (slen >= 2) slen -= 2; /* remove quotes */
        return annot(S, nd, jt_c(S, slen));
    }

    case ND_STATUSLIT: {
        /* V(NAME) -- search all STATUS defs for matching value */
        char vn[SK_MAX_IDENT];
        nd_text(S, n->tok2 ? n->tok2 : n->tok, vn, (int)sizeof(vn));
        ucase(vn);

        for (int i = 0; i < S->n_stdef; i++) {
            for (int j = 0; j < S->stdef[i].n_vals; j++) {
                if (ci_eq(S->stdef[i].vals[j], vn)) {
                    /* find the STATUS type that references this stdef */
                    for (int k = 1; k < S->n_types; k++) {
                        if (S->types[k].kind == JT_STATUS &&
                            S->types[k].extra == (uint32_t)i)
                            return annot(S, nd, (uint32_t)k);
                    }
                }
            }
        }
        sm_err(S, nd, "unknown status value '%s'", vn);
        return annot(S, nd, jt_err(S));
    }

    case ND_IDENT: {
        char nm[SK_MAX_IDENT];
        nd_text(S, n->tok2 ? n->tok2 : n->tok, nm, (int)sizeof(nm));
        int si = find_sym(S, nm);
        if (si < 0) {
            sm_err(S, nd, "undefined symbol '%s'", nm);
            return annot(S, nd, jt_err(S));
        }
        return annot(S, nd, S->syms[si].type);
    }

    case ND_BINARY: {
        uint32_t lt = chk_expr(S, n->child);
        uint32_t rt = chk_expr(S, ND(S, n->child)->sibling);
        uint32_t res = arith_cv(S, lt, rt, n->aux);
        if (is_err(S, res) && !is_err(S, lt) && !is_err(S, rt))
            sm_err(S, nd, "type mismatch in binary expression");
        return annot(S, nd, res);
    }

    case ND_UNARY: {
        uint32_t ot = chk_expr(S, n->child);
        if (n->aux == OP_NOT) {
            if (!is_bit(S, ot) && !is_err(S, ot))
                sm_err(S, nd, "NOT requires BIT operand");
            return annot(S, nd, ot);
        }
        /* NEG, POS -- same type */
        return annot(S, nd, ot);
    }

    case ND_FNCALL: {
        /* call-vs-index disambiguation: the crown jewel of J73 parsing.
         * TBL(I) looks exactly like FUNC(I). We ask the symbol table
         * which one it is, like asking the bartender if the bloke at
         * table 5 is a regular or just lost. */
        char nm[SK_MAX_IDENT];
        /* callee name from first child (should be ND_IDENT) or tok/tok2 */
        uint32_t callee = n->child;
        if (callee != 0 && ND(S, callee)->type == ND_IDENT) {
            nd_text(S, ND(S, callee)->tok2 ? ND(S, callee)->tok2
                                           : ND(S, callee)->tok,
                    nm, (int)sizeof(nm));
        } else {
            nd_text(S, n->tok2 ? n->tok2 : n->tok, nm, (int)sizeof(nm));
        }

        int si = find_sym(S, nm);
        if (si < 0) {
            /* check for J73 built-in functions before crying */
            char up[SK_MAX_IDENT];
            snprintf(up, SK_MAX_IDENT, "%s", nm);
            ucase(up);
            int is_bi = ci_eq(up, "SHIFTL") || ci_eq(up, "SHIFTR") ||
                        ci_eq(up, "ABS") || ci_eq(up, "BITSIZE") ||
                        ci_eq(up, "BYTESIZE");
            if (!is_bi) {
                sm_err(S, nd, "undefined '%s' in call/index", nm);
            }
            /* type-check args either way */
            uint32_t a = (callee != 0) ? ND(S, callee)->sibling : 0;
            uint32_t arg1_ty = 0;
            for (int g = 0; g < 256 && a != 0; g++) {
                uint32_t at = chk_expr(S, a);
                if (g == 0) arg1_ty = at;
                a = ND(S, a)->sibling;
            }
            if (is_bi) {
                /* SHIFTL/SHIFTR/ABS: result type = first arg type.
                 * BITSIZE/BYTESIZE: result = S 32. */
                if (ci_eq(up, "BITSIZE") || ci_eq(up, "BYTESIZE"))
                    return annot(S, nd, jt_intern(S, JT_SIGNED, 32,
                                                  0, 0, 0, 0));
                if (arg1_ty > 0)
                    return annot(S, nd, arg1_ty);
            }
            return annot(S, nd, jt_err(S));
        }

        if (S->syms[si].kind == SYM_TABLE) {
            /* rewrite FNCALL -> INDEX. The AST is mutable. */
            n->type = (uint16_t)ND_INDEX;
            /* type-check index arguments */
            uint32_t a = (callee != 0) ? ND(S, callee)->sibling : 0;
            for (int g = 0; g < 256 && a != 0; g++) {
                chk_expr(S, a);
                a = ND(S, a)->sibling;
            }
            /* result type: TABLE's row type */
            return annot(S, nd, S->syms[si].type);
        }

        if (S->syms[si].kind == SYM_PROC) {
            /* type-check arguments */
            uint32_t a = (callee != 0) ? ND(S, callee)->sibling : 0;
            for (int g = 0; g < 256 && a != 0; g++) {
                chk_expr(S, a);
                a = ND(S, a)->sibling;
            }
            /* return type is inner of PROC type */
            uint32_t pt = S->syms[si].type;
            if (is_prc(S, pt))
                return annot(S, nd, S->types[pt].inner);
            return annot(S, nd, pt);
        }

        /* symbol exists but is not callable/indexable */
        uint32_t a = (callee != 0) ? ND(S, callee)->sibling : 0;
        for (int g = 0; g < 256 && a != 0; g++) {
            chk_expr(S, a);
            a = ND(S, a)->sibling;
        }
        return annot(S, nd, S->syms[si].type);
    }

    case ND_INDEX: {
        /* already rewritten, or explicit index */
        uint32_t bt = chk_expr(S, n->child);
        /* index args */
        uint32_t a = ND(S, n->child)->sibling;
        for (int g = 0; g < 256 && a != 0; g++) {
            chk_expr(S, a);
            a = ND(S, a)->sibling;
        }
        if (is_tbl(S, bt))
            return annot(S, nd, bt);
        return annot(S, nd, bt);
    }

    case ND_MEMBER: {
        /* expr.field -- base must be TABLE, look up field in tbldef */
        uint32_t bt = chk_expr(S, n->child);
        char fn[SK_MAX_IDENT];
        nd_text(S, n->tok2 ? n->tok2 : n->tok, fn, (int)sizeof(fn));
        ucase(fn);

        if (is_tbl(S, bt)) {
            uint32_t tdi = S->types[bt].extra;
            if (tdi < (uint32_t)S->n_tbldf) {
                const sm_tbldf_t *td = &S->tbldef[tdi];
                for (int i = 0; i < td->n_flds; i++) {
                    if (ci_eq(td->flds[i].name, fn))
                        return annot(S, nd, td->flds[i].jtype);
                }
                sm_err(S, nd, "TABLE has no field '%s'", fn);
            }
            return annot(S, nd, jt_err(S));
        }
        if (!is_err(S, bt))
            sm_err(S, nd, "member access on non-TABLE type");
        return annot(S, nd, jt_err(S));
    }

    case ND_DEREF: {
        uint32_t ot = chk_expr(S, n->child);
        if (is_ptr(S, ot))
            return annot(S, nd, S->types[ot].inner);
        if (!is_err(S, ot))
            sm_err(S, nd, "dereference of non-POINTER");
        return annot(S, nd, jt_err(S));
    }

    case ND_ADDROF: {
        uint32_t ot = chk_expr(S, n->child);
        return annot(S, nd, jt_ptr(S, ot));
    }

    case ND_CALL: {
        /* bare call as expression -- check child */
        return chk_expr(S, n->child);
    }

    default:
        /* unknown expression node */
        return annot(S, nd, jt_err(S));
    }
}

/* ---- Pass 2: Statement Checking ----
 * Where assignments learn they've been typing incompatibly,
 * FOR loops discover their variables don't exist, and GOTO
 * targets are held to account for once in their lives. */

static void chk_stmt(sema_ctx_t *S, uint32_t nd)
{
    ast_node_t *n = ND(S, nd);
    if (nd == 0 || n->type == 0) return;

    switch (n->type) {
    case ND_ASSIGN: {
        /* LHS := RHS */
        uint32_t lt = chk_expr(S, n->child);
        uint32_t rt = chk_expr(S, ND(S, n->child)->sibling);
        annot(S, nd, lt);
        /* basic compatibility: both numeric, or same kind, or error */
        if (!is_err(S, lt) && !is_err(S, rt)) {
            int lk = S->types[lt].kind;
            int rk = S->types[rt].kind;
            /* numeric <-> numeric is fine */
            if (is_num(S, lt) && is_num(S, rt)) break;
            /* same kind is fine */
            if (lk == rk) break;
            /* BIT <- BIT is fine */
            if (is_bit(S, lt) && is_bit(S, rt)) break;
            /* BIT <- integer is fine (bit pattern) */
            if (is_bit(S, lt) && is_int(S, rt)) break;
            /* STATUS <- STATUS is fine */
            if (is_sts(S, lt) && is_sts(S, rt)) break;
            /* CHAR <- CHAR is fine */
            if (is_chr(S, lt) && is_chr(S, rt)) break;
            /* otherwise warn */
            sm_err(S, nd, "assignment type mismatch");
        }
        break;
    }

    case ND_IF: {
        /* child: condition, sibling: then-block, sibling: else-block */
        uint32_t cnd = chk_expr(S, n->child);
        (void)cnd; /* condition can be any boolean-ish value */
        uint32_t tb = ND(S, n->child)->sibling;
        if (tb != 0) chk_stmt(S, tb);
        uint32_t eb = ND(S, tb)->sibling;
        if (eb != 0) chk_stmt(S, eb);
        break;
    }

    case ND_WHILE: {
        /* child: condition, sibling: body stmts */
        chk_expr(S, n->child);
        uint32_t c = ND(S, n->child)->sibling;
        for (int g = 0; g < 65536 && c != 0; g++) {
            chk_stmt(S, c);
            c = ND(S, c)->sibling;
        }
        break;
    }

    case ND_FOR: {
        /* tok2 has loop variable name. children: start, step, condition,
         * then body stmts */
        char lv[SK_MAX_IDENT];
        nd_text(S, n->tok2 ? n->tok2 : n->tok, lv, (int)sizeof(lv));
        int si = find_sym(S, lv);
        if (si < 0)
            sm_err(S, nd, "FOR loop variable '%s' not found", lv);

        /* check start/step/condition children */
        uint32_t c = n->child;
        for (int g = 0; g < 3 && c != 0; g++) {
            chk_expr(S, c);
            c = ND(S, c)->sibling;
        }
        /* remaining children are body statements */
        for (int g = 0; g < 65536 && c != 0; g++) {
            chk_stmt(S, c);
            c = ND(S, c)->sibling;
        }
        break;
    }

    case ND_CASE: {
        /* child: selector expr, then ND_CSBRANCH / ND_DEFAULT children */
        chk_expr(S, n->child);
        uint32_t c = ND(S, n->child)->sibling;
        for (int g = 0; g < 1024 && c != 0; g++) {
            ast_node_t *br = ND(S, c);
            if (br->type == ND_CSBRANCH || br->type == ND_DEFAULT) {
                /* walk branch body statements */
                uint32_t bc = br->child;
                for (int g2 = 0; g2 < 1024 && bc != 0; g2++) {
                    chk_stmt(S, bc);
                    bc = ND(S, bc)->sibling;
                }
            }
            c = br->sibling;
        }
        break;
    }

    case ND_RETURN: {
        if (n->child != 0) {
            uint32_t rt = chk_expr(S, n->child);
            annot(S, nd, rt);
        } else {
            annot(S, nd, jt_void(S));
        }
        break;
    }

    case ND_GOTO: {
        char lab[SK_MAX_IDENT];
        nd_text(S, n->tok2 ? n->tok2 : n->tok, lab, (int)sizeof(lab));
        ucase(lab);
        /* mark label as used */
        int found = 0;
        for (int i = 0; i < S->n_labels; i++) {
            if (ci_eq(S->labels[i].name, lab)) {
                S->labels[i].used = 1;
                found = 1;
                break;
            }
        }
        if (!found && S->n_labels < SM_MAX_LABELS) {
            sm_label_t *l = &S->labels[S->n_labels++];
            snprintf(l->name, SK_MAX_IDENT, "%s", lab);
            l->ast_nd  = nd;
            l->defined = 0;
            l->used    = 1;
        }
        break;
    }

    case ND_LABEL: {
        char lab[SK_MAX_IDENT];
        nd_text(S, n->tok2 ? n->tok2 : n->tok, lab, (int)sizeof(lab));
        ucase(lab);
        /* mark label as defined */
        int found = 0;
        for (int i = 0; i < S->n_labels; i++) {
            if (ci_eq(S->labels[i].name, lab)) {
                S->labels[i].defined = 1;
                found = 1;
                break;
            }
        }
        if (!found && S->n_labels < SM_MAX_LABELS) {
            sm_label_t *l = &S->labels[S->n_labels++];
            snprintf(l->name, SK_MAX_IDENT, "%s", lab);
            l->ast_nd  = nd;
            l->defined = 1;
            l->used    = 0;
        }
        break;
    }

    case ND_STMTBLK: {
        push_scp(S);
        uint32_t c = n->child;
        for (int g = 0; g < 65536 && c != 0; g++) {
            ast_node_t *ch = ND(S, c);
            /* declarations inside a stmt block */
            if (ch->type == ND_ITEM || ch->type == ND_TABLE ||
                ch->type == ND_DEFINE || ch->type == ND_TYPEDEF ||
                ch->type == ND_FORMAT)
                col_decl(S, c);
            else
                chk_stmt(S, c);
            c = ch->sibling;
        }
        pop_scp(S);
        break;
    }

    case ND_CALL: {
        /* bare call/expression as statement */
        chk_expr(S, n->child);
        break;
    }

    case ND_EXIT:
    case ND_ABORT:
    case ND_STOP:
    case ND_NULL:
        /* nothing to check, these are the easy ones */
        break;

    /* I/O -- type-check the items, trust the rest */
    case ND_WRITE: {
        /* children: file, fmt, item1, item2, ... */
        uint32_t c = n->child;
        int idx = 0;
        for (int g = 0; g < 256 && c != 0; g++) {
            ast_node_t *ch = ND(S, c);
            if (idx >= 2 && ch->type != ND_NULL)
                chk_expr(S, c);
            c = ch->sibling;
            idx++;
        }
        break;
    }
    case ND_READ: {
        uint32_t c = n->child;
        int idx = 0;
        for (int g = 0; g < 256 && c != 0; g++) {
            ast_node_t *ch = ND(S, c);
            if (idx >= 2 && ch->type != ND_NULL)
                chk_expr(S, c);
            c = ch->sibling;
            idx++;
        }
        break;
    }
    case ND_OPENF:
    case ND_CLOSEF: {
        uint32_t c = n->child;
        for (int g = 0; g < 8 && c != 0; g++) {
            chk_expr(S, c);
            c = ND(S, c)->sibling;
        }
        break;
    }

    /* declarations handled in Pass 1 or inline in STMTBLK */
    case ND_ITEM:
    case ND_TABLE:
    case ND_DEFINE:
    case ND_TYPEDEF:
    case ND_FORMAT:
        col_decl(S, nd);
        break;

    /* expression as statement (e.g. function call) */
    case ND_FNCALL:
        chk_expr(S, nd);
        break;

    default:
        /* unexpected node in statement position -- soldier on */
        break;
    }
}

/* ---- Pass 2: Check Proc Bodies ----
 * J73 procs: the parameters arrive nameless and pick up their
 * types from ITEM declarations inside the body. Like a costume
 * party where you find out who you are when you get there. */

static void chk_proc(sema_ctx_t *S, uint32_t nd)
{
    ast_node_t *n = ND(S, nd);
    char nm[SK_MAX_IDENT];
    nd_text(S, n->tok2 ? n->tok2 : n->tok, nm, (int)sizeof(nm));

    int si = find_sym(S, nm);
    if (si < 0) return; /* shouldn't happen after Pass 1 */

    uint32_t pt = S->syms[si].type;
    S->cur_ret = is_prc(S, pt) ? S->types[pt].inner : jt_void(S);

    push_scp(S);

    /* bind parameter names as symbols.
     * J73 convention: params are name-only at PROC decl.
     * Their types come from ITEM decls inside the body.
     * For now, add params as untyped (JT_VOID), then the body's
     * ITEM decls will shadow them with proper types. */
    uint32_t c = n->child;
    for (int g = 0; g < 1024 && c != 0; g++) {
        ast_node_t *ch = ND(S, c);
        if (ch->type == ND_PARAM) {
            char pn[SK_MAX_IDENT];
            nd_text(S, ch->tok2 ? ch->tok2 : ch->tok, pn,
                    (int)sizeof(pn));
            add_sym(S, pn, jt_s(S, 32), c, SYM_PARAM, ch->flags);
        }
        c = ch->sibling;
    }

    /* walk body: find declarations first, then statements.
     * The body is either a ND_STMTBLK or loose statements after
     * the param/typespec children. */
    c = n->child;
    for (int g = 0; g < 1024 && c != 0; g++) {
        ast_node_t *ch = ND(S, c);
        if (ch->type != ND_PARAM && ch->type != ND_TYPESPEC) {
            /* this is a body node */
            if (ch->type == ND_ITEM || ch->type == ND_TABLE ||
                ch->type == ND_DEFINE || ch->type == ND_TYPEDEF ||
                ch->type == ND_FORMAT)
                col_decl(S, c);
            else
                chk_stmt(S, c);
        }
        c = ch->sibling;
    }

    pop_scp(S);
}

/* ---- Initialisation / Run / Dump ----
 * The public API: init, run, and dump. Like a washing machine
 * but for type errors instead of socks. */

void sema_init(sema_ctx_t *S, const parser_t *P)
{
    memset(S, 0, sizeof(*S));
    S->P       = P;
    S->nodes   = P->ast.nodes;   /* mutable alias */
    S->toks    = P->toks;
    S->n_toks  = P->n_toks;
    S->src     = P->src;
    S->src_len = P->src_len;
    S->n_nodes = P->ast.n_nodes;
    S->root    = P->ast.root;

    /* slot 0 is sentinel (JT_VOID-ish) */
    S->n_types = 1;
    memset(&S->types[0], 0, sizeof(jtype_t));
}

int sema_run(sema_ctx_t *S)
{
    if (S->root == 0) return SK_ERR_SEMA;

    /* ---- Pass 1: collect all declarations ---- */
    ast_node_t *rn = ND(S, S->root);
    uint32_t c = rn->child;
    for (int g = 0; g < 65536 && c != 0; g++) {
        col_decl(S, c);
        c = ND(S, c)->sibling;
    }

    /* ---- Pass 2: check proc bodies ---- */
    c = rn->child;
    for (int g = 0; g < 65536 && c != 0; g++) {
        if (ND(S, c)->type == ND_PROC)
            chk_proc(S, c);
        c = ND(S, c)->sibling;
    }

    /* ---- Pass 2b: check global statements ---- */
    c = rn->child;
    for (int g = 0; g < 65536 && c != 0; g++) {
        ast_node_t *ch = ND(S, c);
        if (ch->type != ND_ITEM && ch->type != ND_TABLE &&
            ch->type != ND_PROC && ch->type != ND_DEFINE &&
            ch->type != ND_TYPEDEF && ch->type != ND_COMPOOL &&
            ch->type != ND_BLOCK && ch->type != ND_FORMAT)
            chk_stmt(S, c);
        c = ch->sibling;
    }

    /* ---- Validate labels ---- */
    for (int i = 0; i < S->n_labels; i++) {
        if (S->labels[i].used && !S->labels[i].defined)
            sm_err(S, S->labels[i].ast_nd,
                   "GOTO target '%s' never defined", S->labels[i].name);
    }

    return S->n_errs > 0 ? SK_ERR_SEMA : SK_OK;
}

/* dump typed AST + symbol table to stdout */
void sema_dump(const sema_ctx_t *S)
{
    /* ---- Symbol table ---- */
    printf("=== Symbol Table (%d symbols) ===\n", S->n_syms);
    static const char *sk_nm[] = {
        "VAR","PROC","PARAM","TYPE","CONST","TABLE","LABEL","CPOOL"
    };
    for (int i = 0; i < S->n_syms; i++) {
        const sema_sym_t *sy = &S->syms[i];
        char tbuf[128];
        jt_str(S, sy->type, tbuf, (int)sizeof(tbuf));
        printf("  %-20s  %-6s  scope=%d  %s",
               sy->name,
               sy->kind < 8 ? sk_nm[sy->kind] : "?",
               sy->scope, tbuf);
        if (sy->kind == SYM_CONST)
            printf("  = %lld", (long long)sy->cval);
        printf("\n");
    }

    /* ---- Type pool ---- */
    printf("\n=== Type Pool (%d types) ===\n", S->n_types);
    for (int i = 1; i < S->n_types; i++) {
        char tbuf[128];
        jt_str(S, (uint32_t)i, tbuf, (int)sizeof(tbuf));
        printf("  [%3d] %s\n", i, tbuf);
    }

    /* ---- TABLE definitions ---- */
    if (S->n_tbldf > 0) {
        printf("\n=== TABLE Definitions (%d) ===\n", S->n_tbldf);
        for (int i = 0; i < S->n_tbldf; i++) {
            const sm_tbldf_t *td = &S->tbldef[i];
            printf("  TABLE #%d (%d fields):\n", i, td->n_flds);
            for (int j = 0; j < td->n_flds; j++) {
                char tbuf[128];
                jt_str(S, td->flds[j].jtype, tbuf, (int)sizeof(tbuf));
                printf("    %-20s  %s\n", td->flds[j].name, tbuf);
            }
        }
    }

    /* ---- STATUS definitions ---- */
    if (S->n_stdef > 0) {
        printf("\n=== STATUS Definitions (%d) ===\n", S->n_stdef);
        for (int i = 0; i < S->n_stdef; i++) {
            const sm_stdef_t *sd = &S->stdef[i];
            printf("  STATUS #%d: ", i);
            for (int j = 0; j < sd->n_vals; j++) {
                if (j > 0) printf(", ");
                printf("%s", sd->vals[j]);
            }
            printf("\n");
        }
    }

    /* ---- Typed AST (iterative walk) ---- */
    printf("\n=== Typed AST ===\n");
    uint32_t stk[SK_MAX_DEPTH];
    int dep[SK_MAX_DEPTH];
    int sp = 0;
    if (S->root != 0) {
        stk[sp] = S->root;
        dep[sp] = 0;
        sp++;
    }
    int iters = 0;
    while (sp > 0 && iters < 100000) {
        iters++;
        sp--;
        uint32_t ni = stk[sp];
        int d = dep[sp];
        const ast_node_t *n = &S->nodes[ni];

        /* indent */
        for (int i = 0; i < d; i++) printf("  ");
        printf("%s", nd_name(n->type));

        /* name from tok2 or tok */
        char buf[SK_MAX_IDENT];
        if (n->tok2 != 0) {
            nd_text(S, n->tok2, buf, (int)sizeof(buf));
            printf(" '%s'", buf);
        } else if (n->tok != 0) {
            nd_text(S, n->tok, buf, (int)sizeof(buf));
            printf(" '%s'", buf);
        }

        /* type annotation */
        uint32_t nt = S->nd_types[ni];
        if (nt != 0 && nt < (uint32_t)S->n_types) {
            char tbuf[128];
            jt_str(S, nt, tbuf, (int)sizeof(tbuf));
            printf("  : %s", tbuf);
        }

        printf("\n");

        /* push sibling first (processed later), then child (processed next) */
        if (n->sibling != 0 && sp < SK_MAX_DEPTH) {
            stk[sp] = n->sibling;
            dep[sp] = d;
            sp++;
        }
        if (n->child != 0 && sp < SK_MAX_DEPTH) {
            stk[sp] = n->child;
            dep[sp] = d + 1;
            sp++;
        }
    }
}
