/* parser.c -- J73 recursive descent parser
 * MIL-STD-1589C, interpreted loosely and with considerable optimism.
 * Left-child/right-sibling AST in a flat array. No recursion in the
 * tree walk, but the parser itself recurses because expressions are
 * fundamentally a recursive concept and anyone who says otherwise
 * is selling something. */

#include "parser.h"
#include <ctype.h>
#include <stdlib.h>

/* ---- Name tables ---- */

static const char *nd_names[] = {
    [ND_PROG]     = "PROG",
    [ND_ITEM]     = "ITEM",
    [ND_TABLE]    = "TABLE",
    [ND_PROC]     = "PROC",
    [ND_BLOCK]    = "BLOCK",
    [ND_COMPOOL]  = "COMPOOL",
    [ND_DEFINE]   = "DEFINE",
    [ND_TYPEDEF]  = "TYPEDEF",
    [ND_PARAM]    = "PARAM",
    [ND_ASSIGN]   = "ASSIGN",
    [ND_CALL]     = "CALL",
    [ND_IF]       = "IF",
    [ND_CASE]     = "CASE",
    [ND_CSBRANCH] = "CSBRANCH",
    [ND_DEFAULT]  = "DEFAULT",
    [ND_WHILE]    = "WHILE",
    [ND_FOR]      = "FOR",
    [ND_GOTO]     = "GOTO",
    [ND_LABEL]    = "LABEL",
    [ND_RETURN]   = "RETURN",
    [ND_EXIT]     = "EXIT",
    [ND_ABORT]    = "ABORT",
    [ND_STOP]     = "STOP",
    [ND_WRITE]    = "WRITE",
    [ND_READ]     = "READ",
    [ND_OPENF]    = "OPENF",
    [ND_CLOSEF]   = "CLOSEF",
    [ND_STMTBLK]  = "STMTBLK",
    [ND_NULL]     = "NULL",
    [ND_INTLIT]   = "INTLIT",
    [ND_FLTLIT]   = "FLTLIT",
    [ND_STRLIT]   = "STRLIT",
    [ND_STATUSLIT]= "STATUSLIT",
    [ND_IDENT]    = "IDENT",
    [ND_BINARY]   = "BINARY",
    [ND_UNARY]    = "UNARY",
    [ND_FNCALL]   = "FNCALL",
    [ND_INDEX]    = "INDEX",
    [ND_MEMBER]   = "MEMBER",
    [ND_DEREF]    = "DEREF",
    [ND_ADDROF]   = "ADDROF",
    [ND_FORMAT]   = "FORMAT",
    [ND_FMTSP]    = "FMTSP",
    [ND_TYPESPEC]  = "TYPESPEC",
    [ND_STATUSVAL] = "STATUSVAL",
    [ND_DIMPAIR]  = "DIMPAIR",
};

static const char *op_names[] = {
    [OP_ADD] = "+",   [OP_SUB] = "-",   [OP_MUL] = "*",
    [OP_DIV] = "/",   [OP_MOD] = "MOD", [OP_POW] = "**",
    [OP_EQ]  = "=",   [OP_NE]  = "<>",  [OP_LT]  = "<",
    [OP_LE]  = "<=",  [OP_GT]  = ">",   [OP_GE]  = ">=",
    [OP_AND] = "AND", [OP_OR]  = "OR",  [OP_XOR] = "XOR",
    [OP_EQV] = "EQV", [OP_NOT] = "NOT",
    [OP_NEG] = "NEG", [OP_POS] = "POS",
};

const char *nd_name(int type)
{
    if (type >= 0 && type < ND_COUNT && nd_names[type])
        return nd_names[type];
    return "???";
}

const char *op_name(int op)
{
    if (op >= 0 && op < OP_COUNT && op_names[op])
        return op_names[op];
    return "???";
}

/* ---- Token helpers ---- */

static inline int at_end(const parser_t *P)
{
    return P->pos >= P->n_toks ||
           P->toks[P->pos].type == TOK_EOF;
}

static inline int cur(const parser_t *P)
{
    if (P->pos >= P->n_toks) return TOK_EOF;
    return (int)P->toks[P->pos].type;
}

static inline const token_t *curtok(const parser_t *P)
{
    return &P->toks[P->pos < P->n_toks ? P->pos : P->n_toks - 1];
}

static inline int peek(const parser_t *P, int off)
{
    uint32_t i = P->pos + (uint32_t)off;
    if (i >= P->n_toks) return TOK_EOF;
    return (int)P->toks[i].type;
}

static inline void advance(parser_t *P)
{
    if (P->pos < P->n_toks) P->pos++;
}

/* skip comments and newlines -- they're noise at this stage */
static void skip_ws(parser_t *P)
{
    while (!at_end(P) &&
           (cur(P) == TOK_COMMENT || cur(P) == TOK_NEWLINE))
        advance(P);
}

/* ---- Error reporting ---- */

static void perr(parser_t *P, const char *msg)
{
    if (P->num_errs >= SK_MAX_ERRORS) return;
    const token_t *t = curtok(P);
    sk_err_t *e = &P->errors[P->num_errs++];
    e->loc.line   = t->line;
    e->loc.col    = t->col;
    e->loc.offset = t->offset;
    e->code       = SK_ERR_PARSE;
    snprintf(e->msg, sizeof(e->msg), "%s", msg);
}

/* J73's single-letter type indicators (A, B, C, etc.) are also
 * valid identifiers in most contexts. The language designers
 * presumably thought this was fine. They were wrong, but here
 * we are. */
static int is_name(int t)
{
    return t == TOK_IDENT ||
           t == TOK_TY_S  || t == TOK_TY_U || t == TOK_TY_F ||
           t == TOK_TY_B  || t == TOK_TY_C || t == TOK_TY_H ||
           t == TOK_TY_A  || t == TOK_TY_D || t == TOK_V;
}

/* consume current token if it's a name-like token */
static int eat_name(parser_t *P)
{
    skip_ws(P);
    if (is_name(cur(P))) { advance(P); return 1; }
    if (cur(P) == TOK_IDENT) { advance(P); return 1; }
    perr(P, "expected identifier");
    return 0;
}

/* skip to next semicolon or known sync point -- error recovery */
static void sync(parser_t *P)
{
    int guard = 1000;
    while (!at_end(P) && --guard > 0) {
        int t = cur(P);
        if (t == TOK_SEMI || t == TOK_END || t == TOK_TERM ||
            t == TOK_EOF)
            break;
        advance(P);
    }
    if (cur(P) == TOK_SEMI) advance(P);
}

/* ---- Token text extraction ---- */

static void tok_text(const parser_t *P, uint32_t ti, char *buf, int sz)
{
    if (ti >= P->n_toks) { buf[0] = '\0'; return; }
    const token_t *t = &P->toks[ti];
    int n = (int)t->len;
    if (n >= sz) n = sz - 1;
    memcpy(buf, P->src + t->offset, (size_t)n);
    buf[n] = '\0';
}

/* ---- Expect / consume ---- */

static int expect(parser_t *P, int type)
{
    skip_ws(P);
    if (cur(P) == type) {
        advance(P);
        return 1;
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "expected %s, got %s",
             tok_name(type), tok_name(cur(P)));
    perr(P, msg);
    return 0;
}

/* consume if present, return 1; else 0 */
static int match(parser_t *P, int type)
{
    skip_ws(P);
    if (cur(P) == type) { advance(P); return 1; }
    return 0;
}

/* ---- Node allocation ---- */

static uint32_t mknode(parser_t *P, int type, uint32_t tok)
{
    ast_t *a = &P->ast;
    if (a->n_nodes >= a->max_nodes) return 0;
    uint32_t idx = a->n_nodes++;
    ast_node_t *n = &a->nodes[idx];
    memset(n, 0, sizeof(*n));
    n->type = (uint16_t)type;
    n->tok  = tok;
    return idx;
}

/* append child to parent's child list */
static void add_child(parser_t *P, uint32_t parent, uint32_t child)
{
    if (parent == 0 || child == 0) return;
    ast_node_t *p = &P->ast.nodes[parent];
    if (p->child == 0) {
        p->child = child;
    } else {
        /* walk to last sibling */
        uint32_t s = p->child;
        int guard = (int)P->ast.max_nodes;
        while (P->ast.nodes[s].sibling != 0 && --guard > 0)
            s = P->ast.nodes[s].sibling;
        P->ast.nodes[s].sibling = child;
    }
}

/* ---- Forward declarations ---- */

static uint32_t p_expr(parser_t *P);
static uint32_t p_decl(parser_t *P);
static uint32_t p_stmt(parser_t *P);
static uint32_t p_tspec(parser_t *P);

/* ---- Declaration start check ---- */

static int is_decl(const parser_t *P)
{
    int t = cur(P);
    return t == TOK_ITEM    || t == TOK_TABLE   ||
           t == TOK_PROC    || t == TOK_BLOCK   ||
           t == TOK_COMPOOL || t == TOK_DEFINE  ||
           t == TOK_TYPE    || t == TOK_FORMAT;
}

/* ---- Integer parsing ---- */

static int64_t parse_int(const parser_t *P, uint32_t ti)
{
    char buf[64];
    tok_text(P, ti, buf, (int)sizeof(buf));
    int len = (int)strlen(buf);

    /* based integer: digits + base_char + hex_digits */
    for (int i = 0; i < len; i++) {
        char c = (char)toupper((unsigned char)buf[i]);
        if (c == 'B' && i > 0) {
            buf[i] = '\0';
            int base = atoi(buf);
            if (base == 2) return strtoll(buf + i + 1, NULL, 2);
            return strtoll(buf + i + 1, NULL, base);
        }
        if (c == 'O' && i > 0) {
            return strtoll(buf + i + 1, NULL, 8);
        }
        if (c == 'H' && i > 0) {
            return strtoll(buf + i + 1, NULL, 16);
        }
    }
    return strtoll(buf, NULL, 10);
}

/* ---- Type specification ---- */

static uint32_t p_tspec(parser_t *P)
{
    skip_ws(P);
    uint32_t ti = P->pos;
    int t = cur(P);

    /* STATUS ( V(name), ... ) */
    if (t == TOK_STATUS) {
        advance(P);
        uint32_t nd = mknode(P, ND_TYPESPEC, ti);
        P->ast.nodes[nd].aux = (int16_t)BT_STATUS;
        if (match(P, TOK_LPAREN)) {
            while (!at_end(P) && cur(P) != TOK_RPAREN) {
                expect(P, TOK_V);
                expect(P, TOK_LPAREN);
                skip_ws(P);
                uint32_t sv = mknode(P, ND_STATUSVAL, P->pos);
                eat_name(P);
                add_child(P, nd, sv);
                expect(P, TOK_RPAREN);
                if (!match(P, TOK_COMMA)) break;
            }
            expect(P, TOK_RPAREN);
        }
        return nd;
    }

    /* POINTER ( typespec ) */
    if (t == TOK_POINTER) {
        advance(P);
        uint32_t nd = mknode(P, ND_TYPESPEC, ti);
        P->ast.nodes[nd].aux = (int16_t)BT_POINTER;
        if (match(P, TOK_LPAREN)) {
            uint32_t inner = p_tspec(P);
            add_child(P, nd, inner);
            expect(P, TOK_RPAREN);
        }
        return nd;
    }

    /* single-letter type indicators + full names */
    int bt = -1;
    if      (t == TOK_TY_S || t == TOK_SIGNED)   bt = BT_SIGNED;
    else if (t == TOK_TY_U || t == TOK_UNSIGNED)  bt = BT_UNSIGNED;
    else if (t == TOK_TY_F || t == TOK_FLOAT)     bt = BT_FLOAT;
    else if (t == TOK_TY_B || t == TOK_BIT)       bt = BT_BIT;
    else if (t == TOK_TY_C || t == TOK_CHARONE)   bt = BT_CHAR;
    else if (t == TOK_TY_H || t == TOK_HOLRONE)   bt = BT_HOLLER;
    else if (t == TOK_TY_A || t == TOK_FIXED)     bt = BT_FIXED;

    if (bt >= 0) {
        advance(P);
        uint32_t nd = mknode(P, ND_TYPESPEC, ti);
        P->ast.nodes[nd].aux = (int16_t)bt;

        /* optional size: integer literal */
        skip_ws(P);
        if (cur(P) == TOK_INT_LIT) {
            P->ast.nodes[nd].aux2 = (uint16_t)parse_int(P, P->pos);
            advance(P);
        }

        /* fixed-point scale factor: D n */
        if (bt == BT_FIXED) {
            skip_ws(P);
            if (cur(P) == TOK_TY_D) {
                advance(P);
                skip_ws(P);
                if (cur(P) == TOK_INT_LIT) {
                    /* scale in aux (overwrite base type? no, use val) */
                    /* store scale as negative aux2 high bits? no.
                     * just shove it in val on the node. */
                    P->ast.nodes[nd].val = parse_int(P, P->pos);
                    advance(P);
                }
            }
        }
        return nd;
    }

    /* named type reference (LIKE target, or TYPE name usage) */
    if (t == TOK_IDENT) {
        advance(P);
        uint32_t nd = mknode(P, ND_TYPESPEC, ti);
        P->ast.nodes[nd].aux = (int16_t)BT_TYPEREF;
        return nd;
    }

    perr(P, "expected type specification");
    return 0;
}

/* ---- Expressions ---- */

static uint32_t p_primary(parser_t *P)
{
    skip_ws(P);
    uint32_t ti = P->pos;
    int t = cur(P);

    /* integer literal */
    if (t == TOK_INT_LIT) {
        advance(P);
        uint32_t nd = mknode(P, ND_INTLIT, ti);
        P->ast.nodes[nd].val = parse_int(P, ti);
        return nd;
    }

    /* float literal */
    if (t == TOK_FLT_LIT) {
        advance(P);
        return mknode(P, ND_FLTLIT, ti);
    }

    /* string literal */
    if (t == TOK_STR_LIT) {
        advance(P);
        return mknode(P, ND_STRLIT, ti);
    }

    /* status literal: V ( name ) */
    if (t == TOK_V) {
        advance(P);
        expect(P, TOK_LPAREN);
        skip_ws(P);
        uint32_t nd = mknode(P, ND_STATUSLIT, P->pos);
        eat_name(P);
        expect(P, TOK_RPAREN);
        return nd;
    }

    /* LOC(expr) -- address-of */
    if (t == TOK_LOC) {
        advance(P);
        expect(P, TOK_LPAREN);
        uint32_t nd = mknode(P, ND_ADDROF, ti);
        uint32_t inner = p_expr(P);
        add_child(P, nd, inner);
        expect(P, TOK_RPAREN);
        return nd;
    }

    /* parenthesized expression */
    if (t == TOK_LPAREN) {
        advance(P);
        uint32_t nd = p_expr(P);
        expect(P, TOK_RPAREN);
        return nd;
    }

    /* identifier */
    if (t == TOK_IDENT) {
        advance(P);
        return mknode(P, ND_IDENT, ti);
    }

    /* builtins that look like function calls */
    if (t == TOK_ABS   || t == TOK_SGN   || t == TOK_SQRT  ||
        t == TOK_SIZE  || t == TOK_NWDSEN || t == TOK_FIRST ||
        t == TOK_LAST  || t == TOK_LBOUND || t == TOK_UBOUND||
        t == TOK_NEXT  || t == TOK_NENT   || t == TOK_SHIFTL||
        t == TOK_SHIFTR|| t == TOK_BITSIZE|| t == TOK_BYTESIZE) {
        advance(P);
        return mknode(P, ND_IDENT, ti);
    }

    /* single-letter type keywords used as identifiers in expressions.
     * naming your variable A was a bold choice but the spec allows it. */
    if (is_name(t)) {
        advance(P);
        return mknode(P, ND_IDENT, ti);
    }

    perr(P, "expected expression");
    return 0;
}

static uint32_t p_postfix(parser_t *P)
{
    uint32_t nd = p_primary(P);
    int guard = 1000;

    while (!at_end(P) && --guard > 0) {
        skip_ws(P);

        /* call / index: expr ( args ) */
        if (cur(P) == TOK_LPAREN) {
            uint32_t ti = P->pos;
            advance(P);

            int is_ident = (P->ast.nodes[nd].type == ND_IDENT);
            uint32_t call = mknode(P, is_ident ? ND_FNCALL : ND_INDEX, ti);
            P->ast.nodes[call].tok2 = P->ast.nodes[nd].tok;
            add_child(P, call, nd);

            /* parse args */
            skip_ws(P);
            int in_output = 0;
            while (!at_end(P) && cur(P) != TOK_RPAREN) {
                /* colon switches to output args */
                if (cur(P) == TOK_COLON && !in_output) {
                    advance(P);
                    in_output = 1;
                    continue;
                }
                uint32_t arg = p_expr(P);
                if (in_output && arg != 0)
                    P->ast.nodes[arg].flags |= NF_OUTPUT;
                add_child(P, call, arg);
                if (!match(P, TOK_COMMA) && cur(P) != TOK_COLON)
                    break;
            }
            expect(P, TOK_RPAREN);
            nd = call;
            continue;
        }

        /* member access: expr . ident */
        if (cur(P) == TOK_DOT) {
            uint32_t ti = P->pos;
            advance(P);
            skip_ws(P);
            uint32_t mem = mknode(P, ND_MEMBER, ti);
            P->ast.nodes[mem].tok2 = P->pos;
            add_child(P, mem, nd);
            eat_name(P);
            nd = mem;
            continue;
        }

        break;
    }
    return nd;
}

static uint32_t p_unary(parser_t *P)
{
    skip_ws(P);
    uint32_t ti = P->pos;
    int t = cur(P);

    if (t == TOK_MINUS) {
        advance(P);
        uint32_t nd = mknode(P, ND_UNARY, ti);
        P->ast.nodes[nd].aux = (int16_t)OP_NEG;
        add_child(P, nd, p_unary(P));
        return nd;
    }
    if (t == TOK_PLUS) {
        advance(P);
        uint32_t nd = mknode(P, ND_UNARY, ti);
        P->ast.nodes[nd].aux = (int16_t)OP_POS;
        add_child(P, nd, p_unary(P));
        return nd;
    }
    if (t == TOK_AT) {
        advance(P);
        uint32_t nd = mknode(P, ND_DEREF, ti);
        add_child(P, nd, p_unary(P));
        return nd;
    }
    return p_postfix(P);
}

static uint32_t p_power(parser_t *P)
{
    uint32_t left = p_unary(P);
    skip_ws(P);
    if (cur(P) == TOK_POWER) {
        uint32_t ti = P->pos;
        advance(P);
        uint32_t nd = mknode(P, ND_BINARY, ti);
        P->ast.nodes[nd].aux = (int16_t)OP_POW;
        add_child(P, nd, left);
        add_child(P, nd, p_power(P)); /* right-assoc */
        return nd;
    }
    return left;
}

static uint32_t p_mul(parser_t *P)
{
    uint32_t left = p_power(P);
    int guard = 10000;
    while (!at_end(P) && --guard > 0) {
        skip_ws(P);
        int t = cur(P);
        int op = -1;
        if      (t == TOK_STAR)  op = OP_MUL;
        else if (t == TOK_SLASH) op = OP_DIV;
        else if (t == TOK_MOD)   op = OP_MOD;
        else break;
        uint32_t ti = P->pos;
        advance(P);
        uint32_t nd = mknode(P, ND_BINARY, ti);
        P->ast.nodes[nd].aux = (int16_t)op;
        add_child(P, nd, left);
        add_child(P, nd, p_power(P));
        left = nd;
    }
    return left;
}

static uint32_t p_add(parser_t *P)
{
    uint32_t left = p_mul(P);
    int guard = 10000;
    while (!at_end(P) && --guard > 0) {
        skip_ws(P);
        int t = cur(P);
        int op = -1;
        if      (t == TOK_PLUS)  op = OP_ADD;
        else if (t == TOK_MINUS) op = OP_SUB;
        else break;
        uint32_t ti = P->pos;
        advance(P);
        uint32_t nd = mknode(P, ND_BINARY, ti);
        P->ast.nodes[nd].aux = (int16_t)op;
        add_child(P, nd, left);
        add_child(P, nd, p_mul(P));
        left = nd;
    }
    return left;
}

static uint32_t p_rel(parser_t *P)
{
    uint32_t left = p_add(P);
    skip_ws(P);
    int t = cur(P);
    int op = -1;
    if      (t == TOK_EQ) op = OP_EQ;
    else if (t == TOK_NE) op = OP_NE;
    else if (t == TOK_LT) op = OP_LT;
    else if (t == TOK_LE) op = OP_LE;
    else if (t == TOK_GT) op = OP_GT;
    else if (t == TOK_GE) op = OP_GE;
    else return left;

    uint32_t ti = P->pos;
    advance(P);
    uint32_t nd = mknode(P, ND_BINARY, ti);
    P->ast.nodes[nd].aux = (int16_t)op;
    add_child(P, nd, left);
    add_child(P, nd, p_add(P));
    return nd; /* non-associative: one comparison only */
}

static uint32_t p_not(parser_t *P)
{
    skip_ws(P);
    if (cur(P) == TOK_NOT) {
        uint32_t ti = P->pos;
        advance(P);
        uint32_t nd = mknode(P, ND_UNARY, ti);
        P->ast.nodes[nd].aux = (int16_t)OP_NOT;
        add_child(P, nd, p_not(P));
        return nd;
    }
    return p_rel(P);
}

static uint32_t p_and(parser_t *P)
{
    uint32_t left = p_not(P);
    int guard = 10000;
    while (!at_end(P) && --guard > 0) {
        skip_ws(P);
        if (cur(P) != TOK_AND) break;
        uint32_t ti = P->pos;
        advance(P);
        uint32_t nd = mknode(P, ND_BINARY, ti);
        P->ast.nodes[nd].aux = (int16_t)OP_AND;
        add_child(P, nd, left);
        add_child(P, nd, p_not(P));
        left = nd;
    }
    return left;
}

static uint32_t p_xor(parser_t *P)
{
    uint32_t left = p_and(P);
    int guard = 10000;
    while (!at_end(P) && --guard > 0) {
        skip_ws(P);
        if (cur(P) != TOK_XOR) break;
        uint32_t ti = P->pos;
        advance(P);
        uint32_t nd = mknode(P, ND_BINARY, ti);
        P->ast.nodes[nd].aux = (int16_t)OP_XOR;
        add_child(P, nd, left);
        add_child(P, nd, p_and(P));
        left = nd;
    }
    return left;
}

static uint32_t p_or(parser_t *P)
{
    uint32_t left = p_xor(P);
    int guard = 10000;
    while (!at_end(P) && --guard > 0) {
        skip_ws(P);
        int t = cur(P);
        int op = -1;
        if      (t == TOK_OR)  op = OP_OR;
        else if (t == TOK_EQV) op = OP_EQV;
        else break;
        uint32_t ti = P->pos;
        advance(P);
        uint32_t nd = mknode(P, ND_BINARY, ti);
        P->ast.nodes[nd].aux = (int16_t)op;
        add_child(P, nd, left);
        add_child(P, nd, p_xor(P));
        left = nd;
    }
    return left;
}

static uint32_t p_expr(parser_t *P)
{
    return p_or(P);
}

/* ---- Declarations ---- */

/* DEFINE name = expr ; */
static uint32_t p_define(parser_t *P)
{
    uint32_t ti = P->pos;
    expect(P, TOK_DEFINE);
    skip_ws(P);
    uint32_t nd = mknode(P, ND_DEFINE, ti);
    P->ast.nodes[nd].tok2 = P->pos; /* name token */
    eat_name(P);
    expect(P, TOK_EQ);
    add_child(P, nd, p_expr(P));
    expect(P, TOK_SEMI);
    return nd;
}

/* TYPE name typespec ; */
static uint32_t p_typedef(parser_t *P)
{
    uint32_t ti = P->pos;
    expect(P, TOK_TYPE);
    skip_ws(P);
    uint32_t nd = mknode(P, ND_TYPEDEF, ti);
    P->ast.nodes[nd].tok2 = P->pos; /* name token */
    eat_name(P);
    uint32_t ts = p_tspec(P);
    add_child(P, nd, ts);
    expect(P, TOK_SEMI);
    return nd;
}

/* ITEM name [modifiers] type [= init] ; */
static uint32_t p_item(parser_t *P)
{
    uint32_t ti = P->pos;
    expect(P, TOK_ITEM);
    skip_ws(P);

    uint32_t nd = mknode(P, ND_ITEM, ti);
    P->ast.nodes[nd].tok2 = P->pos; /* name token */
    eat_name(P);

    /* modifiers: STATIC, CONSTANT, PARALLEL in any order */
    int guard = 20;
    while (!at_end(P) && --guard > 0) {
        skip_ws(P);
        if      (match(P, TOK_STATIC))   P->ast.nodes[nd].flags |= NF_STATIC;
        else if (match(P, TOK_CONSTANT)) P->ast.nodes[nd].flags |= NF_CONST;
        else if (match(P, TOK_PARALLEL)) P->ast.nodes[nd].flags |= NF_PARALLEL;
        else break;
    }

    /* OVERLAY target */
    skip_ws(P);
    if (match(P, TOK_OVERLAY)) {
        P->ast.nodes[nd].flags |= NF_OVERLAY;
        skip_ws(P);
        /* store overlay target as a child ident node */
        uint32_t ov = mknode(P, ND_IDENT, P->pos);
        eat_name(P);
        add_child(P, nd, ov);
    }

    /* POS(target, offset) */
    skip_ws(P);
    if (match(P, TOK_POS)) {
        P->ast.nodes[nd].flags |= NF_POS;
        expect(P, TOK_LPAREN);
        skip_ws(P);
        uint32_t pt = mknode(P, ND_IDENT, P->pos);
        eat_name(P);
        add_child(P, nd, pt);
        expect(P, TOK_COMMA);
        skip_ws(P);
        /* bit offset stored in val */
        if (cur(P) == TOK_INT_LIT)
            P->ast.nodes[nd].val = parse_int(P, P->pos);
        expect(P, TOK_INT_LIT);
        expect(P, TOK_RPAREN);
    }

    /* LIKE source */
    skip_ws(P);
    if (match(P, TOK_LIKE)) {
        P->ast.nodes[nd].flags |= NF_LIKE;
        skip_ws(P);
        uint32_t lk = mknode(P, ND_IDENT, P->pos);
        eat_name(P);
        add_child(P, nd, lk);
    } else {
        /* concrete type spec */
        uint32_t ts = p_tspec(P);
        add_child(P, nd, ts);
    }

    /* optional initializer: = expr */
    skip_ws(P);
    if (match(P, TOK_EQ)) {
        uint32_t init = p_expr(P);
        add_child(P, nd, init);
    }

    expect(P, TOK_SEMI);
    return nd;
}

/* TABLE name(dims) [PARALLEL] [WORDSIZE n] ; BEGIN items END */
static uint32_t p_table(parser_t *P)
{
    uint32_t ti = P->pos;
    expect(P, TOK_TABLE);
    skip_ws(P);

    uint32_t nd = mknode(P, ND_TABLE, ti);
    P->ast.nodes[nd].tok2 = P->pos; /* name token */
    eat_name(P);

    /* dimensions: ( lower:upper , ... ) */
    skip_ws(P);
    if (match(P, TOK_LPAREN)) {
        int guard = 32;
        while (!at_end(P) && cur(P) != TOK_RPAREN && --guard > 0) {
            uint32_t dp = mknode(P, ND_DIMPAIR, P->pos);
            uint32_t lo = p_expr(P);
            add_child(P, dp, lo);
            expect(P, TOK_COLON);
            uint32_t hi = p_expr(P);
            add_child(P, dp, hi);
            add_child(P, nd, dp);
            if (!match(P, TOK_COMMA)) break;
        }
        expect(P, TOK_RPAREN);
    }

    /* modifiers */
    int guard = 10;
    while (!at_end(P) && --guard > 0) {
        skip_ws(P);
        if (match(P, TOK_PARALLEL)) {
            P->ast.nodes[nd].flags |= NF_PARALLEL;
        } else if (cur(P) == TOK_WORDSIZE) {
            advance(P);
            skip_ws(P);
            if (cur(P) == TOK_INT_LIT) {
                P->ast.nodes[nd].aux2 = (uint16_t)parse_int(P, P->pos);
                advance(P);
            }
        } else {
            break;
        }
    }

    expect(P, TOK_SEMI);

    /* body: BEGIN { ITEM ... }* END */
    expect(P, TOK_BEGIN);
    guard = 10000;
    while (!at_end(P) && cur(P) != TOK_END && --guard > 0) {
        skip_ws(P);
        if (cur(P) == TOK_END) break;
        uint32_t item = p_item(P);
        add_child(P, nd, item);
    }
    expect(P, TOK_END);

    return nd;
}

/* PROC name(params [:outparams]) [RENT] [INLINE] [type] ; BEGIN locals stmts END */
static uint32_t p_proc(parser_t *P)
{
    uint32_t ti = P->pos;
    expect(P, TOK_PROC);
    skip_ws(P);

    uint32_t nd = mknode(P, ND_PROC, ti);
    P->ast.nodes[nd].tok2 = P->pos; /* name token */
    eat_name(P);

    /* parameters */
    skip_ws(P);
    if (match(P, TOK_LPAREN)) {
        int in_output = 0;
        int guard = 256;
        while (!at_end(P) && cur(P) != TOK_RPAREN && --guard > 0) {
            skip_ws(P);
            if (cur(P) == TOK_COLON && !in_output) {
                advance(P);
                in_output = 1;
                continue;
            }
            uint32_t pm = mknode(P, ND_PARAM, P->pos);
            if (in_output) P->ast.nodes[pm].flags |= NF_OUTPUT;
            eat_name(P);
            add_child(P, nd, pm);
            /* comma separates within group, colon between groups */
            if (!match(P, TOK_COMMA) && cur(P) != TOK_COLON)
                break;
        }
        expect(P, TOK_RPAREN);
    }

    /* modifiers */
    int guard = 10;
    while (!at_end(P) && --guard > 0) {
        skip_ws(P);
        if      (match(P, TOK_RENT))   P->ast.nodes[nd].flags |= NF_RENT;
        else if (match(P, TOK_INLINE)) P->ast.nodes[nd].flags |= NF_INLINE;
        else if (match(P, TOK_ENTRY))  P->ast.nodes[nd].flags |= NF_ENTRY;
        else break;
    }

    /* optional return type */
    skip_ws(P);
    if (cur(P) != TOK_SEMI) {
        uint32_t rt = p_tspec(P);
        add_child(P, nd, rt);
    }

    expect(P, TOK_SEMI);

    /* body: BEGIN [locals] [stmts] END */
    expect(P, TOK_BEGIN);

    /* locals: greedily consume declarations */
    guard = 10000;
    while (!at_end(P) && --guard > 0) {
        skip_ws(P);
        if (!is_decl(P)) break;
        uint32_t d = p_decl(P);
        add_child(P, nd, d);
    }

    /* statements */
    guard = 10000;
    while (!at_end(P) && cur(P) != TOK_END && --guard > 0) {
        skip_ws(P);
        if (cur(P) == TOK_END) break;
        uint32_t s = p_stmt(P);
        add_child(P, nd, s);
    }
    expect(P, TOK_END);

    return nd;
}

/* BLOCK [name] ; BEGIN decls END */
static uint32_t p_block(parser_t *P)
{
    uint32_t ti = P->pos;
    expect(P, TOK_BLOCK);
    skip_ws(P);

    uint32_t nd = mknode(P, ND_BLOCK, ti);

    /* optional name */
    if (cur(P) == TOK_IDENT) {
        P->ast.nodes[nd].tok2 = P->pos;
        advance(P);
    }

    expect(P, TOK_SEMI);
    expect(P, TOK_BEGIN);

    int guard = 10000;
    while (!at_end(P) && cur(P) != TOK_END && --guard > 0) {
        skip_ws(P);
        if (cur(P) == TOK_END) break;
        uint32_t d = p_decl(P);
        add_child(P, nd, d);
    }
    expect(P, TOK_END);

    return nd;
}

/* COMPOOL name ; BEGIN decls END */
static uint32_t p_compool(parser_t *P)
{
    uint32_t ti = P->pos;
    expect(P, TOK_COMPOOL);
    skip_ws(P);

    uint32_t nd = mknode(P, ND_COMPOOL, ti);
    P->ast.nodes[nd].tok2 = P->pos;
    eat_name(P);
    expect(P, TOK_SEMI);
    expect(P, TOK_BEGIN);

    int guard = 10000;
    while (!at_end(P) && cur(P) != TOK_END && --guard > 0) {
        skip_ws(P);
        if (cur(P) == TOK_END) break;
        uint32_t d = p_decl(P);
        add_child(P, nd, d);
    }
    expect(P, TOK_END);

    return nd;
}

/* FORMAT name ( specs ) ;
 * Where format specs are I/F/A/X width designators, '/' for newline,
 * or string literals. The 1970s idea of printf, minus the fun. */
static uint32_t p_fmt(parser_t *P)
{
    uint32_t ti = P->pos;
    expect(P, TOK_FORMAT);
    skip_ws(P);

    uint32_t nd = mknode(P, ND_FORMAT, ti);
    P->ast.nodes[nd].tok2 = P->pos; /* name token */
    eat_name(P);
    expect(P, TOK_LPAREN);

    /* format specifiers: I w, F w.d, A w, X w, /, 'literal' */
    int guard = 64;
    while (!at_end(P) && cur(P) != TOK_RPAREN && --guard > 0) {
        skip_ws(P);
        uint32_t sp = mknode(P, ND_FMTSP, P->pos);

        if (cur(P) == TOK_SLASH) {
            /* '/' = newline specifier */
            P->ast.nodes[sp].aux = 4; /* kind=/ */
            advance(P);
        } else if (cur(P) == TOK_STR_LIT) {
            /* literal string in format */
            P->ast.nodes[sp].aux = 5; /* kind=literal */
            P->ast.nodes[sp].tok2 = P->pos;
            advance(P);
        } else if (cur(P) == TOK_TY_A || cur(P) == TOK_TY_F ||
                   cur(P) == TOK_TY_S || cur(P) == TOK_TY_U ||
                   cur(P) == TOK_TY_B || cur(P) == TOK_TY_C ||
                   cur(P) == TOK_TY_H || cur(P) == TOK_IDENT) {
            /* I w, F w.d, A w, X w -- single-letter spec.
             * These overlap with type indicators, because J73. */
            char buf[8];
            tok_text(P, P->pos, buf, (int)sizeof(buf));
            char ch = (char)toupper((unsigned char)buf[0]);
            if      (ch == 'I') P->ast.nodes[sp].aux = 0;
            else if (ch == 'F') P->ast.nodes[sp].aux = 1;
            else if (ch == 'A') P->ast.nodes[sp].aux = 2;
            else if (ch == 'X') P->ast.nodes[sp].aux = 3;
            else    P->ast.nodes[sp].aux = 0; /* default I */
            advance(P);
            skip_ws(P);
            /* width.dec as FLT_LIT -- the lexer helpfully merges 10.2
             * into a float literal. We split it back apart here,
             * which is the compiler equivalent of unscrambling an egg. */
            if (cur(P) == TOK_FLT_LIT) {
                char fb[32];
                tok_text(P, P->pos, fb, (int)sizeof(fb));
                char *dot = strchr(fb, '.');
                if (dot) {
                    *dot = '\0';
                    P->ast.nodes[sp].aux2 = (uint16_t)atoi(fb);
                    P->ast.nodes[sp].val  = atoi(dot + 1);
                }
                advance(P);
            } else if (cur(P) == TOK_INT_LIT) {
                P->ast.nodes[sp].aux2 = (uint16_t)parse_int(P, P->pos);
                advance(P);
                /* decimal places: .d (for F format) */
                if (match(P, TOK_DOT)) {
                    skip_ws(P);
                    if (cur(P) == TOK_INT_LIT) {
                        P->ast.nodes[sp].val = parse_int(P, P->pos);
                        advance(P);
                    }
                }
            }
        } else {
            perr(P, "expected format specifier");
            advance(P);
        }
        add_child(P, nd, sp);
        if (!match(P, TOK_COMMA)) break;
    }
    expect(P, TOK_RPAREN);
    expect(P, TOK_SEMI);
    return nd;
}

static uint32_t p_decl(parser_t *P)
{
    skip_ws(P);
    int t = cur(P);
    if (t == TOK_ITEM)    return p_item(P);
    if (t == TOK_TABLE)   return p_table(P);
    if (t == TOK_PROC)    return p_proc(P);
    if (t == TOK_BLOCK)   return p_block(P);
    if (t == TOK_COMPOOL) return p_compool(P);
    if (t == TOK_DEFINE)  return p_define(P);
    if (t == TOK_TYPE)    return p_typedef(P);
    if (t == TOK_FORMAT)  return p_fmt(P);
    perr(P, "expected declaration");
    advance(P); /* skip the offending token */
    return 0;
}

/* ---- Statements ---- */

/* BEGIN decls+stmts END
 * J73 allows declarations anywhere inside a BEGIN/END block,
 * because the 1970s had different ideas about scope. */
static uint32_t p_stmtblk(parser_t *P)
{
    uint32_t ti = P->pos;
    expect(P, TOK_BEGIN);
    uint32_t nd = mknode(P, ND_STMTBLK, ti);

    int guard = 10000;
    while (!at_end(P) && cur(P) != TOK_END && --guard > 0) {
        skip_ws(P);
        if (cur(P) == TOK_END) break;
        if (is_decl(P)) {
            uint32_t d = p_decl(P);
            add_child(P, nd, d);
        } else {
            uint32_t s = p_stmt(P);
            add_child(P, nd, s);
        }
    }
    expect(P, TOK_END);
    return nd;
}

/* IF cond ; stmts [ELSE stmts] END */
static uint32_t p_if(parser_t *P)
{
    uint32_t ti = P->pos;
    expect(P, TOK_IF);

    uint32_t nd = mknode(P, ND_IF, ti);
    uint32_t cond = p_expr(P);
    add_child(P, nd, cond);
    expect(P, TOK_SEMI);

    /* then body: until ELSE or END */
    uint32_t then_blk = mknode(P, ND_STMTBLK, P->pos);
    add_child(P, nd, then_blk);
    int guard = 10000;
    while (!at_end(P) && cur(P) != TOK_ELSE && cur(P) != TOK_END && --guard > 0) {
        skip_ws(P);
        if (cur(P) == TOK_ELSE || cur(P) == TOK_END) break;
        uint32_t s = p_stmt(P);
        add_child(P, then_blk, s);
    }

    /* optional ELSE */
    if (match(P, TOK_ELSE)) {
        uint32_t else_blk = mknode(P, ND_STMTBLK, P->pos);
        add_child(P, nd, else_blk);
        guard = 10000;
        while (!at_end(P) && cur(P) != TOK_END && --guard > 0) {
            skip_ws(P);
            if (cur(P) == TOK_END) break;
            uint32_t s = p_stmt(P);
            add_child(P, else_blk, s);
        }
    }

    expect(P, TOK_END);
    return nd;
}

/* WHILE cond ; stmts END */
static uint32_t p_while(parser_t *P)
{
    uint32_t ti = P->pos;
    expect(P, TOK_WHILE);

    uint32_t nd = mknode(P, ND_WHILE, ti);
    uint32_t cond = p_expr(P);
    add_child(P, nd, cond);
    expect(P, TOK_SEMI);

    int guard = 10000;
    while (!at_end(P) && cur(P) != TOK_END && --guard > 0) {
        skip_ws(P);
        if (cur(P) == TOK_END) break;
        uint32_t s = p_stmt(P);
        add_child(P, nd, s);
    }
    expect(P, TOK_END);
    return nd;
}

/* FOR var := start BY step WHILE cond ; stmts END */
static uint32_t p_for(parser_t *P)
{
    uint32_t ti = P->pos;
    expect(P, TOK_FOR);
    skip_ws(P);

    uint32_t nd = mknode(P, ND_FOR, ti);
    P->ast.nodes[nd].tok2 = P->pos; /* loop variable */
    eat_name(P);
    expect(P, TOK_ASSIGN);

    /* start expression */
    uint32_t start = p_expr(P);
    add_child(P, nd, start);

    /* BY step */
    expect(P, TOK_BY);
    uint32_t step = p_expr(P);
    add_child(P, nd, step);

    /* WHILE condition */
    expect(P, TOK_WHILE);
    uint32_t cond = p_expr(P);
    add_child(P, nd, cond);

    expect(P, TOK_SEMI);

    /* body */
    int guard = 10000;
    while (!at_end(P) && cur(P) != TOK_END && --guard > 0) {
        skip_ws(P);
        if (cur(P) == TOK_END) break;
        uint32_t s = p_stmt(P);
        add_child(P, nd, s);
    }
    expect(P, TOK_END);
    return nd;
}

/* CASE selector ; BEGIN branches END */
static uint32_t p_case(parser_t *P)
{
    uint32_t ti = P->pos;
    expect(P, TOK_CASE);

    uint32_t nd = mknode(P, ND_CASE, ti);
    uint32_t sel = p_expr(P);
    add_child(P, nd, sel);
    expect(P, TOK_SEMI);
    expect(P, TOK_BEGIN);

    int guard = 10000;
    while (!at_end(P) && cur(P) != TOK_END && --guard > 0) {
        skip_ws(P);
        if (cur(P) == TOK_END) break;

        /* DEFAULT : stmts */
        if (cur(P) == TOK_DEFAULT) {
            uint32_t dti = P->pos;
            advance(P);
            expect(P, TOK_COLON);
            uint32_t db = mknode(P, ND_DEFAULT, dti);
            add_child(P, nd, db);
            int g2 = 10000;
            while (!at_end(P) && cur(P) != TOK_END && --g2 > 0) {
                skip_ws(P);
                if (cur(P) == TOK_END) break;
                uint32_t s = p_stmt(P);
                add_child(P, db, s);
            }
            continue;
        }

        /* V(x), V(y) : stmts [FALLTHRU ;] */
        uint32_t br = mknode(P, ND_CSBRANCH, P->pos);
        add_child(P, nd, br);

        /* parse value labels: V(name), ... */
        int g2 = 64;
        while (!at_end(P) && --g2 > 0) {
            skip_ws(P);
            if (cur(P) != TOK_V) break;
            uint32_t sv = mknode(P, ND_STATUSLIT, P->pos);
            advance(P); /* V */
            expect(P, TOK_LPAREN);
            skip_ws(P);
            P->ast.nodes[sv].tok2 = P->pos;
            eat_name(P);
            expect(P, TOK_RPAREN);
            add_child(P, br, sv);
            if (!match(P, TOK_COMMA)) break;
        }

        expect(P, TOK_COLON);

        /* branch body */
        g2 = 10000;
        while (!at_end(P) && --g2 > 0) {
            skip_ws(P);
            int t2 = cur(P);
            if (t2 == TOK_V || t2 == TOK_DEFAULT || t2 == TOK_END)
                break;
            if (t2 == TOK_FALLTHRU) {
                P->ast.nodes[br].flags |= NF_FALLTHRU;
                advance(P);
                expect(P, TOK_SEMI);
                break;
            }
            uint32_t s = p_stmt(P);
            add_child(P, br, s);
        }
    }
    expect(P, TOK_END);
    return nd;
}

/* ---- I/O Statements ----
 * JOVIAL I/O: like printf if printf had been designed by a
 * committee of aerospace engineers in 1973. Which it wasn't,
 * but only because printf got lucky. */

/* WRITE ( [file ,] fmt ) items ;
 * Children: [file-expr | ND_NULL], [fmt-ident | ND_NULL for FREE], items...
 * The ND_NULL sentinel means "stdout" or "FREE format" respectively. */
static uint32_t p_write(parser_t *P)
{
    uint32_t ti = P->pos;
    expect(P, TOK_WRITE);
    uint32_t nd = mknode(P, ND_WRITE, ti);
    expect(P, TOK_LPAREN);

    /* parse (file, fmt) or (fmt) -- FREE is a keyword, ident is named format */
    skip_ws(P);
    if (cur(P) == TOK_FREE) {
        /* WRITE(FREE) -- no file, free format */
        advance(P);
        add_child(P, nd, mknode(P, ND_NULL, ti)); /* no file */
        add_child(P, nd, mknode(P, ND_NULL, ti)); /* FREE sentinel */
    } else {
        /* could be WRITE(expr, FREE), WRITE(expr, fmt), or WRITE(fmt) */
        uint32_t first = p_expr(P);
        skip_ws(P);
        if (match(P, TOK_COMMA)) {
            /* two-arg: file, fmt */
            add_child(P, nd, first);
            skip_ws(P);
            if (cur(P) == TOK_FREE) {
                advance(P);
                add_child(P, nd, mknode(P, ND_NULL, ti));
            } else {
                add_child(P, nd, p_expr(P));
            }
        } else {
            /* single arg: format name, no file */
            add_child(P, nd, mknode(P, ND_NULL, ti));
            add_child(P, nd, first);
        }
    }
    expect(P, TOK_RPAREN);

    /* io-list: expr, expr, ... ; */
    skip_ws(P);
    int guard = 256;
    while (!at_end(P) && cur(P) != TOK_SEMI && --guard > 0) {
        add_child(P, nd, p_expr(P));
        if (!match(P, TOK_COMMA)) break;
    }
    expect(P, TOK_SEMI);
    return nd;
}

/* READ ( [file ,] fmt ) items ; */
static uint32_t p_read(parser_t *P)
{
    uint32_t ti = P->pos;
    expect(P, TOK_READ);
    uint32_t nd = mknode(P, ND_READ, ti);
    expect(P, TOK_LPAREN);

    skip_ws(P);
    if (cur(P) == TOK_FREE) {
        advance(P);
        add_child(P, nd, mknode(P, ND_NULL, ti));
        add_child(P, nd, mknode(P, ND_NULL, ti));
    } else {
        uint32_t first = p_expr(P);
        skip_ws(P);
        if (match(P, TOK_COMMA)) {
            add_child(P, nd, first);
            skip_ws(P);
            if (cur(P) == TOK_FREE) {
                advance(P);
                add_child(P, nd, mknode(P, ND_NULL, ti));
            } else {
                add_child(P, nd, p_expr(P));
            }
        } else {
            add_child(P, nd, mknode(P, ND_NULL, ti));
            add_child(P, nd, first);
        }
    }
    expect(P, TOK_RPAREN);

    skip_ws(P);
    int guard = 256;
    while (!at_end(P) && cur(P) != TOK_SEMI && --guard > 0) {
        add_child(P, nd, p_expr(P));
        if (!match(P, TOK_COMMA)) break;
    }
    expect(P, TOK_SEMI);
    return nd;
}

/* OPEN ( expr [, expr] ) ; */
static uint32_t p_open(parser_t *P)
{
    uint32_t ti = P->pos;
    expect(P, TOK_OPEN);
    uint32_t nd = mknode(P, ND_OPENF, ti);
    expect(P, TOK_LPAREN);
    add_child(P, nd, p_expr(P));
    if (match(P, TOK_COMMA))
        add_child(P, nd, p_expr(P));
    expect(P, TOK_RPAREN);
    expect(P, TOK_SEMI);
    return nd;
}

/* CLOSE ( expr ) ; */
static uint32_t p_close(parser_t *P)
{
    uint32_t ti = P->pos;
    expect(P, TOK_CLOSE);
    uint32_t nd = mknode(P, ND_CLOSEF, ti);
    expect(P, TOK_LPAREN);
    add_child(P, nd, p_expr(P));
    expect(P, TOK_RPAREN);
    expect(P, TOK_SEMI);
    return nd;
}

static uint32_t p_stmt(parser_t *P)
{
    skip_ws(P);
    uint32_t ti = P->pos;
    int t = cur(P);

    /* empty statement */
    if (t == TOK_SEMI) {
        advance(P);
        return mknode(P, ND_NULL, ti);
    }

    /* block */
    if (t == TOK_BEGIN) return p_stmtblk(P);

    /* control flow */
    if (t == TOK_IF)    return p_if(P);
    if (t == TOK_CASE)  return p_case(P);
    if (t == TOK_WHILE) return p_while(P);
    if (t == TOK_FOR)   return p_for(P);

    /* I/O */
    if (t == TOK_WRITE) return p_write(P);
    if (t == TOK_READ)  return p_read(P);
    if (t == TOK_OPEN)  return p_open(P);
    if (t == TOK_CLOSE) return p_close(P);

    if (t == TOK_GOTO) {
        advance(P);
        skip_ws(P);
        uint32_t nd = mknode(P, ND_GOTO, ti);
        P->ast.nodes[nd].tok2 = P->pos;
        eat_name(P);
        expect(P, TOK_SEMI);
        return nd;
    }
    if (t == TOK_RETURN) {
        advance(P);
        uint32_t nd = mknode(P, ND_RETURN, ti);
        skip_ws(P);
        if (cur(P) != TOK_SEMI) {
            uint32_t val = p_expr(P);
            add_child(P, nd, val);
        }
        expect(P, TOK_SEMI);
        return nd;
    }
    if (t == TOK_EXIT) {
        advance(P);
        expect(P, TOK_SEMI);
        return mknode(P, ND_EXIT, ti);
    }
    if (t == TOK_ABORT) {
        advance(P);
        expect(P, TOK_SEMI);
        return mknode(P, ND_ABORT, ti);
    }
    if (t == TOK_STOP) {
        advance(P);
        expect(P, TOK_SEMI);
        return mknode(P, ND_STOP, ti);
    }

    /* expression-based: assignment, call, or label */
    uint32_t expr = p_expr(P);

    /* error recovery: if expression parsing failed, skip to semicolon */
    if (expr == 0) {
        sync(P);
        return mknode(P, ND_NULL, ti);
    }

    skip_ws(P);

    /* label: ident followed by : (not :=) */
    if (P->ast.nodes[expr].type == ND_IDENT &&
        cur(P) == TOK_COLON &&
        peek(P, 1) != TOK_EQ) {
        advance(P); /* eat : */
        uint32_t nd = mknode(P, ND_LABEL, ti);
        P->ast.nodes[nd].tok2 = P->ast.nodes[expr].tok;
        return nd;
    }

    /* assignment: expr := expr */
    if (cur(P) == TOK_ASSIGN) {
        advance(P);
        uint32_t nd = mknode(P, ND_ASSIGN, ti);
        add_child(P, nd, expr);
        add_child(P, nd, p_expr(P));
        expect(P, TOK_SEMI);
        return nd;
    }

    /* bare expression as call/statement */
    uint32_t nd = mknode(P, ND_CALL, ti);
    add_child(P, nd, expr);
    expect(P, TOK_SEMI);
    return nd;
}

/* ---- Top-level: START name ; body TERM ---- */

static uint32_t p_program(parser_t *P)
{
    skip_ws(P);
    uint32_t ti = P->pos;

    /* START or PROGRAM */
    if (cur(P) == TOK_START || cur(P) == TOK_PROGRAM) {
        advance(P);
    } else {
        perr(P, "expected START or PROGRAM");
    }

    skip_ws(P);
    uint32_t nd = mknode(P, ND_PROG, ti);
    P->ast.nodes[nd].tok2 = P->pos; /* program name */
    eat_name(P);
    expect(P, TOK_SEMI);

    /* declarations and statements until TERM or EOF */
    int guard = 100000;
    while (!at_end(P) && cur(P) != TOK_TERM && --guard > 0) {
        skip_ws(P);
        if (at_end(P) || cur(P) == TOK_TERM) break;

        if (is_decl(P)) {
            uint32_t d = p_decl(P);
            add_child(P, nd, d);
        } else {
            uint32_t s = p_stmt(P);
            add_child(P, nd, s);
        }
    }

    match(P, TOK_TERM);
    P->ast.root = nd;
    return nd;
}

/* COMPOOL as top-level (no START/TERM wrapper) */
static uint32_t p_toplevel(parser_t *P)
{
    skip_ws(P);

    /* standalone COMPOOL file */
    if (cur(P) == TOK_COMPOOL)
        return p_compool(P);

    /* normal program */
    return p_program(P);
}

/* ---- Public API ---- */

void parser_init(parser_t *P, const token_t *toks, uint32_t n_toks,
                 const char *src, uint32_t src_len,
                 ast_node_t *nodes, uint32_t max_nodes)
{
    memset(P, 0, sizeof(*P));
    P->toks    = toks;
    P->n_toks  = n_toks;
    P->src     = src;
    P->src_len = src_len;
    P->ast.nodes     = nodes;
    P->ast.max_nodes = max_nodes;
    /* reserve index 0 as sentinel */
    P->ast.n_nodes = 1;
    memset(&nodes[0], 0, sizeof(nodes[0]));
}

int parser_run(parser_t *P)
{
    P->ast.root = p_toplevel(P);
    return P->num_errs ? SK_ERR_PARSE : SK_OK;
}

/* ---- AST dump (iterative with explicit stack) ---- */

void ast_dump(const parser_t *P)
{
    if (P->ast.root == 0) {
        printf("(empty AST)\n");
        return;
    }

    /* explicit stack: (node_index, depth) */
    typedef struct { uint32_t idx; int dep; } dstk_t;
    dstk_t stk[SK_MAX_DEPTH];
    int sp = 0;
    stk[sp].idx = P->ast.root;
    stk[sp].dep = 0;
    sp++;

    char buf[128];
    int guard = (int)P->ast.max_nodes * 2;

    while (sp > 0 && --guard > 0) {
        sp--;
        uint32_t idx = stk[sp].idx;
        int dep      = stk[sp].dep;

        if (idx == 0 || idx >= P->ast.n_nodes) continue;
        const ast_node_t *n = &P->ast.nodes[idx];

        /* indent */
        for (int i = 0; i < dep; i++) printf("  ");

        /* node type */
        printf("%s", nd_name(n->type));

        /* token text */
        if (n->tok2 != 0) {
            tok_text(P, n->tok2, buf, (int)sizeof(buf));
            printf(" '%s'", buf);
        } else if (n->tok != 0) {
            tok_text(P, n->tok, buf, (int)sizeof(buf));
            printf(" '%s'", buf);
        }

        /* operator */
        if (n->type == ND_BINARY || n->type == ND_UNARY)
            printf(" [%s]", op_name(n->aux));

        /* typespec */
        if (n->type == ND_TYPESPEC) {
            static const char *bt[] = {
                "void","S","U","F","B","C","H","A",
                "STATUS","POINTER","ref"
            };
            int b = n->aux;
            if (b >= 0 && b < BT_COUNT)
                printf(" %s", bt[b]);
            if (n->aux2) printf(" %u", n->aux2);
            if (n->val)  printf(" D%lld", (long long)n->val);
        }

        /* int literal value */
        if (n->type == ND_INTLIT)
            printf(" =%lld", (long long)n->val);

        /* flags */
        if (n->flags) {
            printf(" {");
            if (n->flags & NF_STATIC)   printf("static ");
            if (n->flags & NF_CONST)    printf("const ");
            if (n->flags & NF_PARALLEL) printf("parallel ");
            if (n->flags & NF_INLINE)   printf("inline ");
            if (n->flags & NF_RENT)     printf("rent ");
            if (n->flags & NF_OVERLAY)  printf("overlay ");
            if (n->flags & NF_POS)      printf("pos ");
            if (n->flags & NF_LIKE)     printf("like ");
            if (n->flags & NF_FALLTHRU) printf("fallthru ");
            if (n->flags & NF_OUTPUT)   printf("output ");
            printf("}");
        }

        printf("\n");

        /* push sibling first (processed after children) */
        if (n->sibling != 0 && sp < SK_MAX_DEPTH)
            stk[sp++] = (dstk_t){n->sibling, dep};

        /* push child (processed next) */
        if (n->child != 0 && sp < SK_MAX_DEPTH)
            stk[sp++] = (dstk_t){n->child, dep + 1};
    }
}
