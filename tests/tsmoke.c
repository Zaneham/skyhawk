/* tsmoke.c -- lexer smoke tests
 * If these fail, something has gone properly wrong. */

#include "tharns.h"
#include "../src/skyhawk.h"
#include "../src/fe/token.h"
#include "../src/fe/lexer.h"

/* ---- Helpers ---- */

#define MAX_TT 256

static token_t  tt_buf[MAX_TT];
static lexer_t  tt_lex;

static int lex(const char *src)
{
    lexer_init(&tt_lex, src, (uint32_t)strlen(src),
               tt_buf, MAX_TT);
    return lexer_run(&tt_lex);
}

static int ntoks(void) { return (int)tt_lex.num_toks; }
static int ttype(int i) { return (int)tt_buf[i].type; }

static const char *ttext(int i)
{
    static char buf[256];
    lexer_text(&tt_lex, &tt_buf[i], buf, (int)sizeof(buf));
    return buf;
}

/* ---- Empty input ---- */

static void smk_empty(void)
{
    CHEQ(lex(""), SK_OK);
    CHEQ(ntoks(), 1);
    CHEQ(ttype(0), TOK_EOF);
    PASS();
}
TH_REG("smoke", smk_empty)

/* ---- Integer literals ---- */

static void smk_int(void)
{
    CHEQ(lex("42"), SK_OK);
    CHEQ(ntoks(), 2);
    CHEQ(ttype(0), TOK_INT_LIT);
    CHSTR(ttext(0), "42");
    PASS();
}
TH_REG("smoke", smk_int)

/* ---- Based integers ---- */

static void smk_based(void)
{
    CHEQ(lex("2B1010 8O777 16HFF"), SK_OK);
    CHEQ(ttype(0), TOK_INT_LIT);
    CHSTR(ttext(0), "2B1010");
    CHEQ(ttype(1), TOK_INT_LIT);
    CHSTR(ttext(1), "8O777");
    CHEQ(ttype(2), TOK_INT_LIT);
    CHSTR(ttext(2), "16HFF");
    PASS();
}
TH_REG("smoke", smk_based)

/* ---- Float literal ---- */

static void smk_float(void)
{
    CHEQ(lex("3.14 1E10 2.5E-3"), SK_OK);
    CHEQ(ttype(0), TOK_FLT_LIT);
    CHSTR(ttext(0), "3.14");
    CHEQ(ttype(1), TOK_FLT_LIT);
    CHSTR(ttext(1), "1E10");
    CHEQ(ttype(2), TOK_FLT_LIT);
    CHSTR(ttext(2), "2.5E-3");
    PASS();
}
TH_REG("smoke", smk_float)

/* ---- String literal ---- */

static void smk_str(void)
{
    CHEQ(lex("'HELLO WORLD'"), SK_OK);
    CHEQ(ttype(0), TOK_STR_LIT);
    CHSTR(ttext(0), "'HELLO WORLD'");
    PASS();
}
TH_REG("smoke", smk_str)

/* ---- Comment ---- */

static void smk_cmt(void)
{
    CHEQ(lex("\"this is a comment\""), SK_OK);
    CHEQ(ttype(0), TOK_COMMENT);
    PASS();
}
TH_REG("smoke", smk_cmt)

/* ---- Keywords ---- */

static void smk_kw(void)
{
    CHEQ(lex("PROGRAM BEGIN END"), SK_OK);
    CHEQ(ttype(0), TOK_PROGRAM);
    CHEQ(ttype(1), TOK_BEGIN);
    CHEQ(ttype(2), TOK_END);
    PASS();
}
TH_REG("smoke", smk_kw)

/* ---- Case insensitive ---- */

static void smk_icase(void)
{
    CHEQ(lex("program Program PROGRAM"), SK_OK);
    CHEQ(ttype(0), TOK_PROGRAM);
    CHEQ(ttype(1), TOK_PROGRAM);
    CHEQ(ttype(2), TOK_PROGRAM);
    PASS();
}
TH_REG("smoke", smk_icase)

/* ---- Identifiers ---- */

static void smk_ident(void)
{
    CHEQ(lex("FOO BAR42 $SYS"), SK_OK);
    CHEQ(ttype(0), TOK_IDENT);
    CHSTR(ttext(0), "FOO");
    CHEQ(ttype(1), TOK_IDENT);
    CHSTR(ttext(1), "BAR42");
    CHEQ(ttype(2), TOK_IDENT);
    CHSTR(ttext(2), "$SYS");
    PASS();
}
TH_REG("smoke", smk_ident)

/* ---- Apostrophe identifier (qualified name) ---- */

static void smk_apost(void)
{
    CHEQ(lex("CURRENT'WAYPOINT"), SK_OK);
    CHEQ(ntoks(), 2); /* ident + EOF */
    CHEQ(ttype(0), TOK_IDENT);
    CHSTR(ttext(0), "CURRENT'WAYPOINT");
    PASS();
}
TH_REG("smoke", smk_apost)

/* ---- Operators ---- */

static void smk_ops(void)
{
    CHEQ(lex(":= + - * / ** = <> < <= > >="), SK_OK);
    CHEQ(ttype(0), TOK_ASSIGN);
    CHEQ(ttype(1), TOK_PLUS);
    CHEQ(ttype(2), TOK_MINUS);
    CHEQ(ttype(3), TOK_STAR);
    CHEQ(ttype(4), TOK_SLASH);
    CHEQ(ttype(5), TOK_POWER);
    CHEQ(ttype(6), TOK_EQ);
    CHEQ(ttype(7), TOK_NE);
    CHEQ(ttype(8), TOK_LT);
    CHEQ(ttype(9), TOK_LE);
    CHEQ(ttype(10), TOK_GT);
    CHEQ(ttype(11), TOK_GE);
    PASS();
}
TH_REG("smoke", smk_ops)

/* ---- Delimiters ---- */

static void smk_delim(void)
{
    CHEQ(lex("( ) [ ] , ; : @ ."), SK_OK);
    CHEQ(ttype(0), TOK_LPAREN);
    CHEQ(ttype(1), TOK_RPAREN);
    CHEQ(ttype(2), TOK_LBRACK);
    CHEQ(ttype(3), TOK_RBRACK);
    CHEQ(ttype(4), TOK_COMMA);
    CHEQ(ttype(5), TOK_SEMI);
    CHEQ(ttype(6), TOK_COLON);
    CHEQ(ttype(7), TOK_AT);
    CHEQ(ttype(8), TOK_DOT);
    PASS();
}
TH_REG("smoke", smk_delim)

/* ---- Type indicators ---- */

static void smk_tyind(void)
{
    CHEQ(lex("S U F B C H A D"), SK_OK);
    CHEQ(ttype(0), TOK_TY_S);
    CHEQ(ttype(1), TOK_TY_U);
    CHEQ(ttype(2), TOK_TY_F);
    CHEQ(ttype(3), TOK_TY_B);
    CHEQ(ttype(4), TOK_TY_C);
    CHEQ(ttype(5), TOK_TY_H);
    CHEQ(ttype(6), TOK_TY_A);
    CHEQ(ttype(7), TOK_TY_D);
    PASS();
}
TH_REG("smoke", smk_tyind)

/* ---- Line/column tracking ---- */

static void smk_loc(void)
{
    CHEQ(lex("X\nY\nZ"), SK_OK);
    CHEQ(tt_buf[0].line, 1);
    CHEQ(tt_buf[1].line, 2);
    CHEQ(tt_buf[2].line, 3);
    PASS();
}
TH_REG("smoke", smk_loc)

/* ---- ITEM declaration snippet ---- */

static void smk_item(void)
{
    CHEQ(lex("ITEM SPEED S 32;"), SK_OK);
    CHEQ(ttype(0), TOK_ITEM);
    CHEQ(ttype(1), TOK_IDENT);
    CHSTR(ttext(1), "SPEED");
    CHEQ(ttype(2), TOK_TY_S);
    CHEQ(ttype(3), TOK_INT_LIT);
    CHEQ(ttype(4), TOK_SEMI);
    PASS();
}
TH_REG("smoke", smk_item)

/* ---- PROC declaration snippet ---- */

static void smk_proc(void)
{
    CHEQ(lex("PROC CALC(X, Y);"), SK_OK);
    CHEQ(ttype(0), TOK_PROC);
    CHEQ(ttype(1), TOK_IDENT);
    CHSTR(ttext(1), "CALC");
    CHEQ(ttype(2), TOK_LPAREN);
    CHEQ(ttype(3), TOK_IDENT);
    CHEQ(ttype(4), TOK_COMMA);
    CHEQ(ttype(5), TOK_IDENT);
    CHEQ(ttype(6), TOK_RPAREN);
    CHEQ(ttype(7), TOK_SEMI);
    PASS();
}
TH_REG("smoke", smk_proc)

/* ---- TABLE with TYPE ---- */

static void smk_table(void)
{
    CHEQ(lex("TABLE NAV'DATA(10);"), SK_OK);
    CHEQ(ttype(0), TOK_TABLE);
    CHEQ(ttype(1), TOK_IDENT);
    CHSTR(ttext(1), "NAV'DATA");
    CHEQ(ttype(2), TOK_LPAREN);
    CHEQ(ttype(3), TOK_INT_LIT);
    CHEQ(ttype(4), TOK_RPAREN);
    CHEQ(ttype(5), TOK_SEMI);
    PASS();
}
TH_REG("smoke", smk_table)

/* ---- Logical operators ---- */

static void smk_logic(void)
{
    CHEQ(lex("AND OR NOT XOR EQV MOD"), SK_OK);
    CHEQ(ttype(0), TOK_AND);
    CHEQ(ttype(1), TOK_OR);
    CHEQ(ttype(2), TOK_NOT);
    CHEQ(ttype(3), TOK_XOR);
    CHEQ(ttype(4), TOK_EQV);
    CHEQ(ttype(5), TOK_MOD);
    PASS();
}
TH_REG("smoke", smk_logic)

/* ---- Unterminated string error ---- */

static void smk_errstr(void)
{
    CHEQ(lex("'oops"), SK_ERR_LEX);
    CHECK(tt_lex.num_errs > 0);
    PASS();
}
TH_REG("smoke", smk_errstr)

/* ---- Unterminated comment error ---- */

static void smk_errcmt(void)
{
    CHEQ(lex("\"oops"), SK_ERR_LEX);
    CHECK(tt_lex.num_errs > 0);
    PASS();
}
TH_REG("smoke", smk_errcmt)

/* ---- CLI: --lex mode ---- */

static void smk_cli(void)
{
    char buf[TH_BUFSZ]; /* plenty for token dump */
    int rc = th_run(SK_BIN " --lex tests/fixtures/hello.jov", buf, TH_BUFSZ);
    CHEQ(rc, 0);
    /* should contain PROGRAM keyword token */
    CHECK(strstr(buf, "START") != NULL);
    /* should contain HELLO identifier */
    CHECK(strstr(buf, "HELLO") != NULL);
    PASS();
}
TH_REG("smoke", smk_cli)
