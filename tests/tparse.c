/* tparse.c -- parser smoke tests
 * Feeding J73 to a C parser and hoping for the best.
 * Not unlike feeding a cat to a vet. */

#include "tharns.h"
#include "../src/skyhawk.h"
#include "../src/fe/token.h"
#include "../src/fe/lexer.h"
#include "../src/fe/ast.h"
#include "../src/fe/parser.h"

/* ---- Helpers ---- */

#define PT_MAXTOK  1024
#define PT_MAXND   2048

static token_t    pt_toks[PT_MAXTOK];
static ast_node_t pt_nds[PT_MAXND];
static lexer_t    pt_lex;
static parser_t   pt_par;

static int parse(const char *src)
{
    uint32_t len = (uint32_t)strlen(src);
    lexer_init(&pt_lex, src, len, pt_toks, PT_MAXTOK);
    lexer_run(&pt_lex);
    parser_init(&pt_par, pt_toks, pt_lex.num_toks,
                src, len, pt_nds, PT_MAXND);
    return parser_run(&pt_par);
}

static ast_node_t *root(void)
{
    return &pt_nds[pt_par.ast.root];
}

/* returns sentinel (index 0, type 0) if no child/sibling.
 * never NULL, so gcc's null-deref analyzer stays quiet. */
static ast_node_t *child(ast_node_t *n)
{
    return &pt_nds[n->child];
}

static ast_node_t *sib(ast_node_t *n)
{
    return &pt_nds[n->sibling];
}

/* ---- Minimal program ---- */

static void par_empty(void)
{
    CHEQ(parse("START FOO; TERM"), SK_OK);
    CHECK(root()->type == ND_PROG);
    CHECK(child(root())->type == 0); /* no children */
    PASS();
}
TH_REG("smoke", par_empty)

/* ---- ITEM declaration ---- */

static void par_item(void)
{
    CHEQ(parse("START T; ITEM X S 32; TERM"), SK_OK);
    ast_node_t *r = root();
    ast_node_t *c = child(r);
    CHECK(c->type != 0);
    CHEQ(c->type, (uint16_t)ND_ITEM);
    /* typespec child */
    ast_node_t *ts = child(c);
    CHECK(ts->type != 0);
    CHEQ(ts->type, (uint16_t)ND_TYPESPEC);
    CHEQ(ts->aux, (int16_t)BT_SIGNED);
    CHEQ(ts->aux2, 32);
    PASS();
}
TH_REG("smoke", par_item)

/* ---- ITEM with initializer ---- */

static void par_init(void)
{
    CHEQ(parse("START T; ITEM N S 16 = 42; TERM"), SK_OK);
    ast_node_t *item = child(root());
    CHECK(item->type != 0);
    /* first child: typespec, sibling: init value */
    ast_node_t *ts = child(item);
    CHECK(ts->type != 0);
    ast_node_t *init = sib(ts);
    CHECK(init->type != 0);
    CHEQ(init->type, (uint16_t)ND_INTLIT);
    CHEQ(init->val, 42);
    PASS();
}
TH_REG("smoke", par_init)

/* ---- DEFINE ---- */

static void par_defn(void)
{
    CHEQ(parse("START T; DEFINE PI = 314; TERM"), SK_OK);
    ast_node_t *d = child(root());
    CHECK(d->type != 0);
    CHEQ(d->type, (uint16_t)ND_DEFINE);
    ast_node_t *val = child(d);
    CHECK(val->type != 0);
    CHEQ(val->type, (uint16_t)ND_INTLIT);
    CHEQ(val->val, 314);
    PASS();
}
TH_REG("smoke", par_defn)

/* ---- PROC with params ---- */

static void par_proc(void)
{
    CHEQ(parse("START T; PROC ADD(X, Y) S 32; BEGIN RETURN X + Y; END TERM"), SK_OK);
    ast_node_t *p = child(root());
    CHECK(p->type != 0);
    CHEQ(p->type, (uint16_t)ND_PROC);
    /* first child: param X */
    ast_node_t *px = child(p);
    CHECK(px->type != 0);
    CHEQ(px->type, (uint16_t)ND_PARAM);
    /* second child: param Y */
    ast_node_t *py = sib(px);
    CHECK(py->type != 0);
    CHEQ(py->type, (uint16_t)ND_PARAM);
    /* third child: return type */
    ast_node_t *rt = sib(py);
    CHECK(rt->type != 0);
    CHEQ(rt->type, (uint16_t)ND_TYPESPEC);
    PASS();
}
TH_REG("smoke", par_proc)

/* ---- Output params ---- */

static void par_outpm(void)
{
    CHEQ(parse("START T; PROC SW(A, B : A, B); BEGIN END TERM"), SK_OK);
    ast_node_t *p = child(root());
    ast_node_t *c = child(p);
    /* skip input params A, B */
    CHECK(c->type != 0); /* A */
    CHEQ(c->flags & NF_OUTPUT, 0);
    c = sib(c);
    CHECK(c->type != 0); /* B */
    CHEQ(c->flags & NF_OUTPUT, 0);
    /* output params */
    c = sib(c);
    CHECK(c->type != 0); /* A out */
    CHECK(c->flags & NF_OUTPUT);
    c = sib(c);
    CHECK(c->type != 0); /* B out */
    CHECK(c->flags & NF_OUTPUT);
    PASS();
}
TH_REG("smoke", par_outpm)

/* ---- TABLE ---- */

static void par_table(void)
{
    CHEQ(parse("START T; TABLE TBL(0:9); BEGIN ITEM X F 32; END TERM"), SK_OK);
    ast_node_t *t = child(root());
    CHECK(t->type != 0);
    CHEQ(t->type, (uint16_t)ND_TABLE);
    /* first child: dimpair */
    ast_node_t *dp = child(t);
    CHECK(dp->type != 0);
    CHEQ(dp->type, (uint16_t)ND_DIMPAIR);
    /* second child: item */
    ast_node_t *item = sib(dp);
    CHECK(item->type != 0);
    CHEQ(item->type, (uint16_t)ND_ITEM);
    PASS();
}
TH_REG("smoke", par_table)

/* ---- IF/ELSE ---- */

static void par_if(void)
{
    CHEQ(parse("START T; IF X = 1; Y := 2; ELSE Y := 3; END TERM"), SK_OK);
    ast_node_t *f = child(root());
    CHECK(f->type != 0);
    CHEQ(f->type, (uint16_t)ND_IF);
    /* first child: condition */
    ast_node_t *cond = child(f);
    CHECK(cond->type != 0);
    CHEQ(cond->type, (uint16_t)ND_BINARY);
    /* then block */
    ast_node_t *tb = sib(cond);
    CHECK(tb->type != 0);
    CHEQ(tb->type, (uint16_t)ND_STMTBLK);
    /* else block */
    ast_node_t *eb = sib(tb);
    CHECK(eb->type != 0);
    CHEQ(eb->type, (uint16_t)ND_STMTBLK);
    PASS();
}
TH_REG("smoke", par_if)

/* ---- WHILE ---- */

static void par_while(void)
{
    CHEQ(parse("START T; WHILE X > 0; X := X - 1; END TERM"), SK_OK);
    ast_node_t *w = child(root());
    CHECK(w->type != 0);
    CHEQ(w->type, (uint16_t)ND_WHILE);
    PASS();
}
TH_REG("smoke", par_while)

/* ---- FOR ---- */

static void par_for(void)
{
    CHEQ(parse("START T; FOR I := 1 BY 1 WHILE I <= 10; END TERM"), SK_OK);
    ast_node_t *f = child(root());
    CHECK(f->type != 0);
    CHEQ(f->type, (uint16_t)ND_FOR);
    /* children: start, step, condition */
    ast_node_t *st = child(f);
    CHECK(st->type != 0);
    CHEQ(st->type, (uint16_t)ND_INTLIT);
    PASS();
}
TH_REG("smoke", par_for)

/* ---- CASE ---- */

static void par_case(void)
{
    CHEQ(parse(
        "START T;"
        "  CASE X;"
        "  BEGIN"
        "    V(RED) : Y := 1;"
        "    V(BLUE) : Y := 2;"
        "    DEFAULT : Y := 0;"
        "  END "
        "TERM"), SK_OK);
    ast_node_t *c = child(root());
    CHECK(c->type != 0);
    CHEQ(c->type, (uint16_t)ND_CASE);
    PASS();
}
TH_REG("smoke", par_case)

/* ---- COMPOOL (standalone) ---- */

static void par_cpool(void)
{
    CHEQ(parse(
        "COMPOOL CP;"
        "BEGIN"
        "  ITEM X S 32;"
        "  ITEM Y F 64;"
        "END"), SK_OK);
    CHECK(root()->type == ND_COMPOOL);
    ast_node_t *x = child(root());
    CHECK(x->type != 0);
    CHEQ(x->type, (uint16_t)ND_ITEM);
    ast_node_t *y = sib(x);
    CHECK(y->type != 0);
    CHEQ(y->type, (uint16_t)ND_ITEM);
    PASS();
}
TH_REG("smoke", par_cpool)

/* ---- Expression precedence ---- */

static void par_prec(void)
{
    /* 1 + 2 * 3 should be 1 + (2 * 3), not (1 + 2) * 3 */
    CHEQ(parse("START T; X := 1 + 2 * 3; TERM"), SK_OK);
    ast_node_t *asgn = child(root());
    CHECK(asgn->type != 0);
    CHEQ(asgn->type, (uint16_t)ND_ASSIGN);
    /* rhs should be ND_BINARY [+] */
    ast_node_t *lhs = child(asgn);
    ast_node_t *rhs = sib(lhs);
    CHECK(rhs->type != 0);
    CHEQ(rhs->type, (uint16_t)ND_BINARY);
    CHEQ(rhs->aux, (int16_t)OP_ADD);
    /* right child of + should be * */
    ast_node_t *plus_l = child(rhs);
    ast_node_t *plus_r = sib(plus_l);
    CHECK(plus_r->type != 0);
    CHEQ(plus_r->type, (uint16_t)ND_BINARY);
    CHEQ(plus_r->aux, (int16_t)OP_MUL);
    PASS();
}
TH_REG("smoke", par_prec)

/* ---- Member access + table index ---- */

static void par_memb(void)
{
    CHEQ(parse("START T; X := TBL(I).FIELD; TERM"), SK_OK);
    ast_node_t *asgn = child(root());
    ast_node_t *lhs = child(asgn);
    ast_node_t *rhs = sib(lhs);
    CHECK(rhs->type != 0);
    CHEQ(rhs->type, (uint16_t)ND_MEMBER);
    ast_node_t *base = child(rhs);
    CHECK(base->type != 0);
    CHEQ(base->type, (uint16_t)ND_FNCALL);
    PASS();
}
TH_REG("smoke", par_memb)

/* ---- STATUS type + V() literals ---- */

static void par_status(void)
{
    CHEQ(parse(
        "START T;"
        "  TYPE CLR STATUS(V(RED), V(GREEN));"
        "  ITEM C CLR = V(RED);"
        "TERM"), SK_OK);
    ast_node_t *td = child(root());
    CHECK(td->type != 0);
    CHEQ(td->type, (uint16_t)ND_TYPEDEF);
    ast_node_t *ts = child(td);
    CHECK(ts->type != 0);
    CHEQ(ts->aux, (int16_t)BT_STATUS);
    PASS();
}
TH_REG("smoke", par_status)

/* ---- Fixed-point type: A n D s ---- */

static void par_fixed(void)
{
    CHEQ(parse("START T; ITEM Q A 32 D 16; TERM"), SK_OK);
    ast_node_t *item = child(root());
    ast_node_t *ts = child(item);
    CHECK(ts->type != 0);
    CHEQ(ts->aux, (int16_t)BT_FIXED);
    CHEQ(ts->aux2, 32);
    CHEQ(ts->val, 16); /* scale factor */
    PASS();
}
TH_REG("smoke", par_fixed)

/* ---- CLI: --parse mode ---- */

static void par_cli(void)
{
    char buf[TH_BUFSZ];
    int rc = th_run(SK_BIN " --parse tests/fixtures/hello.jov", buf, TH_BUFSZ);
    CHEQ(rc, 0);
    CHECK(strstr(buf, "PROG") != NULL);
    CHECK(strstr(buf, "ITEM") != NULL);
    CHECK(strstr(buf, "FOR") != NULL);
    PASS();
}
TH_REG("smoke", par_cli)

/* ---- CLI: COMPOOL fixture ---- */

static void par_cpfi(void)
{
    char buf[TH_BUFSZ];
    int rc = th_run(SK_BIN " --parse tests/fixtures/compool.jov", buf, TH_BUFSZ);
    CHEQ(rc, 0);
    CHECK(strstr(buf, "COMPOOL") != NULL);
    CHECK(strstr(buf, "TABLE") != NULL);
    CHECK(strstr(buf, "STATUS") != NULL);
    PASS();
}
TH_REG("smoke", par_cpfi)

/* ---- Deref and LOC ---- */

static void par_deref(void)
{
    CHEQ(parse("START T; X := @P; TERM"), SK_OK);
    ast_node_t *asgn = child(root());
    ast_node_t *lhs = child(asgn);
    ast_node_t *rhs = sib(lhs);
    CHECK(rhs->type != 0);
    CHEQ(rhs->type, (uint16_t)ND_DEREF);
    PASS();
}
TH_REG("smoke", par_deref)

/* ---- GOTO and labels ---- */

static void par_goto(void)
{
    CHEQ(parse("START T; GOTO DONE; DONE: TERM"), SK_OK);
    ast_node_t *g = child(root());
    CHECK(g->type != 0);
    CHEQ(g->type, (uint16_t)ND_GOTO);
    ast_node_t *l = sib(g);
    CHECK(l->type != 0);
    CHEQ(l->type, (uint16_t)ND_LABEL);
    PASS();
}
TH_REG("smoke", par_goto)

/* ---- I/O statements ---- */

static void par_write(void)
{
    CHEQ(parse("START T; WRITE(FREE) 42; TERM"), SK_OK);
    ast_node_t *w = child(root());
    CHECK(w->type != 0);
    CHEQ(w->type, (uint16_t)ND_WRITE);
    /* children: file(NULL), fmt(NULL), 42 */
    ast_node_t *f = child(w);
    CHEQ(f->type, (uint16_t)ND_NULL); /* no file */
    ast_node_t *fmt = sib(f);
    CHEQ(fmt->type, (uint16_t)ND_NULL); /* FREE */
    ast_node_t *val = sib(fmt);
    CHEQ(val->type, (uint16_t)ND_INTLIT);
    PASS();
}
TH_REG("smoke", par_write)

static void par_read(void)
{
    CHEQ(parse("START T; ITEM X S 32; READ(FREE) X; TERM"), SK_OK);
    ast_node_t *c = child(root());
    ast_node_t *rd = sib(c); /* skip ITEM */
    CHECK(rd->type != 0);
    CHEQ(rd->type, (uint16_t)ND_READ);
    PASS();
}
TH_REG("smoke", par_read)

static void par_open(void)
{
    CHEQ(parse("START T; OPEN('FILE', 1); TERM"), SK_OK);
    ast_node_t *o = child(root());
    CHECK(o->type != 0);
    CHEQ(o->type, (uint16_t)ND_OPENF);
    /* two children: path and mode */
    ast_node_t *p = child(o);
    CHEQ(p->type, (uint16_t)ND_STRLIT);
    ast_node_t *m = sib(p);
    CHEQ(m->type, (uint16_t)ND_INTLIT);
    PASS();
}
TH_REG("smoke", par_open)

static void par_close(void)
{
    CHEQ(parse("START T; CLOSE(3); TERM"), SK_OK);
    ast_node_t *c = child(root());
    CHECK(c->type != 0);
    CHEQ(c->type, (uint16_t)ND_CLOSEF);
    PASS();
}
TH_REG("smoke", par_close)

static void par_fmt(void)
{
    CHEQ(parse("START T; FORMAT FMT1(I 6, F 10.2); TERM"), SK_OK);
    ast_node_t *f = child(root());
    CHECK(f->type != 0);
    CHEQ(f->type, (uint16_t)ND_FORMAT);
    /* two FMTSP children */
    ast_node_t *s1 = child(f);
    CHEQ(s1->type, (uint16_t)ND_FMTSP);
    CHEQ(s1->aux, (int16_t)0); /* kind I */
    CHEQ(s1->aux2, (uint16_t)6);
    ast_node_t *s2 = sib(s1);
    CHEQ(s2->type, (uint16_t)ND_FMTSP);
    CHEQ(s2->aux, (int16_t)1); /* kind F */
    CHEQ(s2->aux2, (uint16_t)10);
    CHEQ((int)s2->val, 2);  /* decimal places */
    PASS();
}
TH_REG("smoke", par_fmt)
