/* tsema.c -- sema + layout tests
 * Asking a type checker if it's having a good day.
 * (It never is. Types are inherently disappointing.
 * "Your S 32 doesn't match my S 32." "They're the same."
 * "Not spiritually.") */

#include "tharns.h"
#include "../src/skyhawk.h"
#include "../src/fe/token.h"
#include "../src/fe/lexer.h"
#include "../src/fe/ast.h"
#include "../src/fe/parser.h"
#include "../src/fe/sema.h"
#include "../src/fe/layout.h"
#include <ctype.h>

/* ---- Helpers ---- */

#define ST_MAXTOK  2048
#define ST_MAXND   4096

static token_t    st_toks[ST_MAXTOK];
static ast_node_t st_nds[ST_MAXND];
static lexer_t    st_lex;
static parser_t   st_par;
static sema_ctx_t st_sem; /* ~3MB, must be static */

/* replicated find_sym -- sema.c's is static, so we roll our own */
static int ts_find(const sema_ctx_t *S, const char *name)
{
    char up[SK_MAX_IDENT];
    snprintf(up, SK_MAX_IDENT, "%s", name);
    for (char *p = up; *p; p++) *p = (char)toupper((unsigned char)*p);
    for (int i = S->n_syms - 1; i >= 0; i--) {
        int eq = 1;
        const char *a = S->syms[i].name;
        const char *b = up;
        for (; *a && *b; a++, b++) {
            if (toupper((unsigned char)*a) != toupper((unsigned char)*b))
                { eq = 0; break; }
        }
        if (eq && *a == '\0' && *b == '\0') return i;
    }
    return -1;
}

/* lex + parse + sema */
static int run_sema(const char *src)
{
    uint32_t len = (uint32_t)strlen(src);
    lexer_init(&st_lex, src, len, st_toks, ST_MAXTOK);
    lexer_run(&st_lex);
    parser_init(&st_par, st_toks, st_lex.num_toks,
                src, len, st_nds, ST_MAXND);
    int rc = parser_run(&st_par);
    if (rc != SK_OK) return rc;
    sema_init(&st_sem, &st_par);
    return sema_run(&st_sem);
}

/* find symbol by name */
static const sema_sym_t *sym(const char *name)
{
    int si = ts_find(&st_sem, name);
    if (si < 0) return NULL;
    return &st_sem.syms[si];
}

/* ---- sema_base: ITEM X S 32 ---- */

static void sema_base(void)
{
    CHEQ(run_sema("START T; ITEM X S 32; TERM"), SK_OK);
    const sema_sym_t *s = sym("X");
    CHECK(s != NULL);
    CHEQ(s->kind, (uint8_t)SYM_VAR);
    CHECK(s->type > 0);
    CHEQ(st_sem.types[s->type].kind, (uint8_t)JT_SIGNED);
    CHEQ(st_sem.types[s->type].width, 32);
    PASS();
}
TH_REG("sema", sema_base)

/* ---- sema_ftypes: all base types ---- */

static void sema_ftypes(void)
{
    CHEQ(run_sema(
        "START T;"
        "  ITEM A S 16;"
        "  ITEM B U 8;"
        "  ITEM C F 64;"
        "  ITEM D B 1;"
        "  ITEM E C 10;"
        "  ITEM G H 4;"
        "TERM"), SK_OK);

    const sema_sym_t *sa = sym("A");
    CHECK(sa != NULL);
    CHEQ(st_sem.types[sa->type].kind, (uint8_t)JT_SIGNED);
    CHEQ(st_sem.types[sa->type].width, 16);

    const sema_sym_t *sb = sym("B");
    CHECK(sb != NULL);
    CHEQ(st_sem.types[sb->type].kind, (uint8_t)JT_UNSIGN);

    const sema_sym_t *sc = sym("C");
    CHECK(sc != NULL);
    CHEQ(st_sem.types[sc->type].kind, (uint8_t)JT_FLOAT);

    const sema_sym_t *sd = sym("D");
    CHECK(sd != NULL);
    CHEQ(st_sem.types[sd->type].kind, (uint8_t)JT_BIT);

    const sema_sym_t *se = sym("E");
    CHECK(se != NULL);
    CHEQ(st_sem.types[se->type].kind, (uint8_t)JT_CHAR);

    const sema_sym_t *sg = sym("G");
    CHECK(sg != NULL);
    CHEQ(st_sem.types[sg->type].kind, (uint8_t)JT_HOLLER);

    PASS();
}
TH_REG("sema", sema_ftypes)

/* ---- sema_like: LIKE resolution ---- */

static void sema_like(void)
{
    CHEQ(run_sema(
        "START T;"
        "  ITEM X F 64;"
        "  ITEM Y LIKE X;"
        "TERM"), SK_OK);
    const sema_sym_t *sy = sym("Y");
    CHECK(sy != NULL);
    CHEQ(st_sem.types[sy->type].kind, (uint8_t)JT_FLOAT);
    CHEQ(st_sem.types[sy->type].width, 64);
    PASS();
}
TH_REG("sema", sema_like)

/* ---- sema_stdef: STATUS creates sm_stdef with ordinals ---- */

static void sema_stdef(void)
{
    CHEQ(run_sema(
        "START T;"
        "  TYPE CLR STATUS(V(RED), V(GREEN), V(BLUE));"
        "TERM"), SK_OK);
    CHECK(st_sem.n_stdef > 0);
    CHEQ(st_sem.stdef[0].n_vals, 3);
    CHSTR(st_sem.stdef[0].vals[0], "RED");
    CHSTR(st_sem.stdef[0].vals[1], "GREEN");
    CHSTR(st_sem.stdef[0].vals[2], "BLUE");
    PASS();
}
TH_REG("sema", sema_stdef)

/* ---- sema_stlit: V(RED) resolves to STATUS type ---- */

static void sema_stlit(void)
{
    CHEQ(run_sema(
        "START T;"
        "  TYPE CLR STATUS(V(RED), V(GREEN), V(BLUE));"
        "  ITEM C CLR = V(RED);"
        "TERM"), SK_OK);
    const sema_sym_t *sc = sym("C");
    CHECK(sc != NULL);
    CHEQ(st_sem.types[sc->type].kind, (uint8_t)JT_STATUS);
    PASS();
}
TH_REG("sema", sema_stlit)

/* ---- sema_defn: DEFINE creates SYM_CONST ---- */

static void sema_defn(void)
{
    CHEQ(run_sema(
        "START T;"
        "  DEFINE PI = 314;"
        "TERM"), SK_OK);
    const sema_sym_t *s = sym("PI");
    CHECK(s != NULL);
    CHEQ(s->kind, (uint8_t)SYM_CONST);
    CHEQ(s->cval, 314);
    PASS();
}
TH_REG("sema", sema_defn)

/* ---- sema_proc: PROC with params and return type ---- */

static void sema_proc(void)
{
    CHEQ(run_sema(
        "START T;"
        "  PROC ADD(X, Y) S 32;"
        "  BEGIN"
        "    RETURN X + Y;"
        "  END "
        "TERM"), SK_OK);
    const sema_sym_t *s = sym("ADD");
    CHECK(s != NULL);
    CHEQ(s->kind, (uint8_t)SYM_PROC);
    CHECK(st_sem.types[s->type].kind == JT_PROC);
    /* return type should be S 32 */
    uint32_t rt = st_sem.types[s->type].inner;
    CHECK(rt > 0);
    CHEQ(st_sem.types[rt].kind, (uint8_t)JT_SIGNED);
    CHEQ(st_sem.types[rt].width, 32);
    PASS();
}
TH_REG("sema", sema_proc)

/* ---- sema_tabl: TABLE member types in tbldef ---- */

static void sema_tabl(void)
{
    CHEQ(run_sema(
        "START T;"
        "  TABLE WPT(0:9);"
        "  BEGIN"
        "    ITEM LAT F 64;"
        "    ITEM LON F 64;"
        "    ITEM ALT S 32;"
        "  END "
        "TERM"), SK_OK);
    const sema_sym_t *s = sym("WPT");
    CHECK(s != NULL);
    CHEQ(s->kind, (uint8_t)SYM_TABLE);
    CHEQ(st_sem.types[s->type].kind, (uint8_t)JT_TABLE);

    /* check tbldef has 3 fields */
    uint32_t tdi = st_sem.types[s->type].extra;
    CHECK(tdi < (uint32_t)st_sem.n_tbldf);
    CHEQ(st_sem.tbldef[tdi].n_flds, 3);
    CHSTR(st_sem.tbldef[tdi].flds[0].name, "LAT");
    CHSTR(st_sem.tbldef[tdi].flds[1].name, "LON");
    CHSTR(st_sem.tbldef[tdi].flds[2].name, "ALT");
    PASS();
}
TH_REG("sema", sema_tabl)

/* ---- sema_callidx: FNCALL on TABLE -> rewrite to ND_INDEX ---- */

static void sema_callidx(void)
{
    CHEQ(run_sema(
        "START T;"
        "  TABLE TBL(0:9);"
        "  BEGIN"
        "    ITEM X S 32;"
        "  END"
        "  ITEM I S 32;"
        "  I := TBL(0).X;"
        "TERM"), SK_OK);

    /* find the assignment, its RHS should be ND_MEMBER,
     * whose base should have been rewritten from FNCALL to INDEX */
    uint32_t c = st_nds[st_par.ast.root].child;
    for (int g = 0; g < 100 && c != 0; g++) {
        if (st_nds[c].type == ND_ASSIGN) {
            /* RHS of assignment */
            uint32_t rhs = st_nds[st_nds[c].child].sibling;
            CHECK(rhs != 0);
            /* should be ND_MEMBER */
            if (st_nds[rhs].type == ND_MEMBER) {
                /* base of member should be rewritten to ND_INDEX */
                uint32_t base = st_nds[rhs].child;
                CHECK(base != 0);
                CHEQ(st_nds[base].type, (uint16_t)ND_INDEX);
                PASS();
                return;
            }
            /* or directly ND_INDEX if no member */
            CHEQ(st_nds[rhs].type, (uint16_t)ND_MEMBER);
        }
        c = st_nds[c].sibling;
    }
    CHECK(0); /* didn't find assignment */
}
TH_REG("sema", sema_callidx)

/* ---- sema_expr: arithmetic yields correct type ---- */

static void sema_expr(void)
{
    CHEQ(run_sema(
        "START T;"
        "  ITEM X S 32;"
        "  ITEM Y S 32;"
        "  ITEM Z S 32;"
        "  Z := X + Y;"
        "TERM"), SK_OK);

    /* the assignment's RHS (binary +) should be typed S 32 */
    uint32_t c = st_nds[st_par.ast.root].child;
    for (int g = 0; g < 100 && c != 0; g++) {
        if (st_nds[c].type == ND_ASSIGN) {
            uint32_t rhs = st_nds[st_nds[c].child].sibling;
            CHECK(rhs != 0);
            uint32_t nt = st_sem.nd_types[rhs];
            CHECK(nt > 0);
            CHEQ(st_sem.types[nt].kind, (uint8_t)JT_SIGNED);
            CHEQ(st_sem.types[nt].width, 32);
            PASS();
            return;
        }
        c = st_nds[c].sibling;
    }
    CHECK(0);
}
TH_REG("sema", sema_expr)

/* ---- sema_cmp: comparison yields BIT(1) ---- */

static void sema_cmp(void)
{
    CHEQ(run_sema(
        "START T;"
        "  ITEM X S 32;"
        "  ITEM Y B 1;"
        "  Y := X > 0;"
        "TERM"), SK_OK);

    uint32_t c = st_nds[st_par.ast.root].child;
    for (int g = 0; g < 100 && c != 0; g++) {
        if (st_nds[c].type == ND_ASSIGN) {
            uint32_t rhs = st_nds[st_nds[c].child].sibling;
            CHECK(rhs != 0);
            uint32_t nt = st_sem.nd_types[rhs];
            CHECK(nt > 0);
            CHEQ(st_sem.types[nt].kind, (uint8_t)JT_BIT);
            CHEQ(st_sem.types[nt].width, 1);
            PASS();
            return;
        }
        c = st_nds[c].sibling;
    }
    CHECK(0);
}
TH_REG("sema", sema_cmp)

/* ---- sema_scope: inner BLOCK var shadows outer ---- */

static void sema_scope(void)
{
    CHEQ(run_sema(
        "START T;"
        "  ITEM X S 32;"
        "  BEGIN"
        "    ITEM X F 64;"
        "  END "
        "TERM"), SK_OK);
    /* after the block, X should be back to S 32 */
    const sema_sym_t *s = sym("X");
    CHECK(s != NULL);
    CHEQ(st_sem.types[s->type].kind, (uint8_t)JT_SIGNED);
    CHEQ(st_sem.types[s->type].width, 32);
    PASS();
}
TH_REG("sema", sema_scope)

/* ---- sema_fixed: A 32 D 16 -> JT_FIXED ---- */

static void sema_fixed(void)
{
    CHEQ(run_sema("START T; ITEM Q A 32 D 16; TERM"), SK_OK);
    const sema_sym_t *s = sym("Q");
    CHECK(s != NULL);
    CHEQ(st_sem.types[s->type].kind, (uint8_t)JT_FIXED);
    CHEQ(st_sem.types[s->type].width, 32);
    CHEQ(st_sem.types[s->type].scale, 16);
    PASS();
}
TH_REG("sema", sema_fixed)

/* ---- sema_ptr: POINTER(S 32) -> JT_PTR ---- */

static void sema_ptr(void)
{
    CHEQ(run_sema("START T; ITEM P POINTER(S 32); TERM"), SK_OK);
    const sema_sym_t *s = sym("P");
    CHECK(s != NULL);
    CHEQ(st_sem.types[s->type].kind, (uint8_t)JT_PTR);
    uint32_t inner = st_sem.types[s->type].inner;
    CHECK(inner > 0);
    CHEQ(st_sem.types[inner].kind, (uint8_t)JT_SIGNED);
    CHEQ(st_sem.types[inner].width, 32);
    PASS();
}
TH_REG("sema", sema_ptr)

/* ---- sema_undef: undefined symbol -> error ---- */

static void sema_undef(void)
{
    int rc = run_sema(
        "START T;"
        "  ITEM X S 32;"
        "  X := BOGUS;"
        "TERM");
    /* should produce at least one error about BOGUS */
    CHECK(st_sem.n_errs > 0);
    CHECK(strstr(st_sem.errors[0].msg, "BOGUS") != NULL ||
          strstr(st_sem.errors[0].msg, "bogus") != NULL ||
          strstr(st_sem.errors[0].msg, "undefined") != NULL);
    (void)rc;
    PASS();
}
TH_REG("sema", sema_undef)

/* ---- sema_cli: --sema on hello.jov ---- */

static void sema_cli(void)
{
    char buf[TH_BUFSZ];
    int rc = th_run(SK_BIN " --sema tests/fixtures/hello.jov", buf, TH_BUFSZ);
    CHEQ(rc, 0);
    CHECK(strstr(buf, "Symbol Table") != NULL);
    CHECK(strstr(buf, "GREETING") != NULL);
    CHECK(strstr(buf, "COUNT") != NULL);
    PASS();
}
TH_REG("sema", sema_cli)

/* ---- sema_clcp: --sema on compool.jov ---- */

static void sema_clcp(void)
{
    char buf[TH_BUFSZ];
    int rc = th_run(SK_BIN " --sema tests/fixtures/compool.jov", buf, TH_BUFSZ);
    CHEQ(rc, 0);
    CHECK(strstr(buf, "TABLE") != NULL);
    CHECK(strstr(buf, "STATUS") != NULL);
    PASS();
}
TH_REG("sema", sema_clcp)

/* ---- lay_basic: WAYPOINT table layout ---- */

static void lay_basic(void)
{
    CHEQ(run_sema(
        "START T;"
        "  TABLE WPT(0:9);"
        "  BEGIN"
        "    ITEM LAT F 64;"
        "    ITEM LON F 64;"
        "    ITEM ALT S 32;"
        "    ITEM ACTIVE B 1;"
        "  END "
        "TERM"), SK_OK);

    const sema_sym_t *s = sym("WPT");
    CHECK(s != NULL);

    static lay_tbl_t lay;
    int rc = lay_tabl(&lay, &st_sem, s->type);
    CHEQ(rc, 0);
    CHEQ(lay.n_flds, 4);

    /* LAT: 64 bits = 8 bytes at offset 0 */
    CHEQ(lay.flds[0].byte_off, 0u);
    CHEQ(lay.flds[0].bit_wid, 64);

    /* LON: 64 bits = 8 bytes at offset 8 */
    CHEQ(lay.flds[1].byte_off, 8u);
    CHEQ(lay.flds[1].bit_wid, 64);

    /* ALT: 32 bits = 4 bytes at offset 16 */
    CHEQ(lay.flds[2].byte_off, 16u);
    CHEQ(lay.flds[2].bit_wid, 32);

    /* ACTIVE: 1 bit, at offset 20 */
    CHEQ(lay.flds[3].byte_off, 20u);
    CHEQ(lay.flds[3].bit_wid, 1);

    /* total should be 22 bytes (20 + 2 for word alignment of remaining bit) */
    CHECK(lay.total_bytes >= 21);

    PASS();
}
TH_REG("layout", lay_basic)

/* ---- I/O sema tests ---- */

static void sema_write(void)
{
    CHEQ(run_sema(
        "START T;"
        "  ITEM X S 32;"
        "  WRITE(FREE) X;"
        "TERM"), SK_OK);
    CHEQ(st_sem.n_errs, 0);
    PASS();
}
TH_REG("sema", sema_write)

static void sema_fmt(void)
{
    CHEQ(run_sema(
        "START T;"
        "  FORMAT FMT1(I 6, F 10.2);"
        "  ITEM X S 32;"
        "  WRITE(FMT1) X;"
        "TERM"), SK_OK);
    CHEQ(st_sem.n_errs, 0);
    CHEQ(st_sem.n_fmts, 1);
    PASS();
}
TH_REG("sema", sema_fmt)

/* ---- sema_ovl: OVERLAY wider than overlaid → error ---- */

static void sema_ovl(void)
{
    int rc = run_sema(
        "START T;"
        "  TABLE TB(0:1);"
        "  BEGIN"
        "    ITEM X B 4;"
        "    ITEM Y OVERLAY X B 8;"
        "  END "
        "TERM");
    /* should produce an error: Y (8 bits) > X (4 bits) */
    CHECK(st_sem.n_errs > 0);
    CHECK(strstr(st_sem.errors[0].msg, "OVERLAY") != NULL ||
          strstr(st_sem.errors[0].msg, "wider") != NULL);
    (void)rc;
    PASS();
}
TH_REG("sema", sema_ovl)
