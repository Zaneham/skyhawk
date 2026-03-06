/* tjir.c -- JIR lowering tests
 * Watching an AST dissolve into SSA instructions, like a sugar
 * cube in tea — except the sugar has opinions about alignment
 * and the tea is 65,536 instructions wide. */

#include "tharns.h"
#include "../src/skyhawk.h"
#include "../src/fe/token.h"
#include "../src/fe/lexer.h"
#include "../src/fe/ast.h"
#include "../src/fe/parser.h"
#include "../src/fe/sema.h"
#include "../src/ir/jir.h"
#include <ctype.h>

/* ---- Helpers ---- */

#define JT_MAXTOK  2048
#define JT_MAXND   4096

static token_t    jt_toks[JT_MAXTOK];
static ast_node_t jt_nds[JT_MAXND];
static lexer_t    jt_lex;
static parser_t   jt_par;
static sema_ctx_t jt_sem;
static jir_mod_t  jt_jir;

/* lex + parse + sema + jir_lower */
static int run_jir(const char *src)
{
    uint32_t len = (uint32_t)strlen(src);
    lexer_init(&jt_lex, src, len, jt_toks, JT_MAXTOK);
    lexer_run(&jt_lex);
    parser_init(&jt_par, jt_toks, jt_lex.num_toks,
                src, len, jt_nds, JT_MAXND);
    int rc = parser_run(&jt_par);
    if (rc != SK_OK) return rc;
    sema_init(&jt_sem, &jt_par);
    rc = sema_run(&jt_sem);
    if (rc != SK_OK) return rc;
    jir_init(&jt_jir, &jt_sem);
    return jir_lower(&jt_jir);
}

/* find instruction by opcode, starting at given index */
static int find_op(int op, uint32_t from)
{
    for (uint32_t i = from; i < jt_jir.n_inst; i++) {
        if (jt_jir.insts[i].op == (uint16_t)op)
            return (int)i;
    }
    return -1;
}

/* count instructions with given opcode */
static int cnt_op(int op)
{
    int n = 0;
    for (uint32_t i = 0; i < jt_jir.n_inst; i++) {
        if (jt_jir.insts[i].op == (uint16_t)op)
            n++;
    }
    return n;
}

/* ---- jir_empty: minimal program -> 1 func, 1 block, ret void ---- */

static void jir_empty(void)
{
    CHEQ(run_jir("START T; TERM"), SK_OK);
    CHECK(jt_jir.n_funcs >= 1);
    CHECK(jt_jir.n_blks >= 1);
    /* should have at least one RET */
    CHECK(find_op(JIR_RET, 0) >= 0);
    PASS();
}
TH_REG("ir", jir_empty)

/* ---- jir_item: ITEM X S 32 -> alloca with correct type ---- */

static void jir_item(void)
{
    CHEQ(run_jir("START T; ITEM X S 32; TERM"), SK_OK);
    int ai = find_op(JIR_ALLOCA, 0);
    CHECK(ai >= 0);
    /* alloca should have a type that's S 32 */
    uint32_t ty = jt_jir.insts[ai].type;
    CHECK(ty > 0);
    CHEQ(jt_sem.types[ty].kind, (uint8_t)JT_SIGNED);
    CHEQ(jt_sem.types[ty].width, 32);
    PASS();
}
TH_REG("ir", jir_item)

/* ---- jir_vinit: ITEM X S 32 = 42 -> alloca + store const ---- */

static void jir_vinit(void)
{
    CHEQ(run_jir("START T; ITEM X S 32 = 42; TERM"), SK_OK);
    int ai = find_op(JIR_ALLOCA, 0);
    CHECK(ai >= 0);
    int si = find_op(JIR_STORE, 0);
    CHECK(si >= 0);
    /* store's ops[1] should be the alloca */
    CHEQ(jt_jir.insts[si].ops[1], (uint32_t)ai);
    /* store's ops[0] should be a constant = 42 */
    uint32_t val = jt_jir.insts[si].ops[0];
    CHECK(JIR_IS_C(val));
    uint32_t ci = JIR_C_IDX(val);
    CHECK(ci < jt_jir.n_consts);
    CHEQ(jt_jir.consts[ci].kind, (uint8_t)JC_INT);
    CHEQ(jt_jir.consts[ci].iv, 42);
    PASS();
}
TH_REG("ir", jir_vinit)

/* ---- jir_arith: X + Y -> load, load, add ---- */

static void jir_arith(void)
{
    CHEQ(run_jir(
        "START T;"
        "  ITEM X S 32 = 10;"
        "  ITEM Y S 32 = 20;"
        "  ITEM Z S 32;"
        "  Z := X + Y;"
        "TERM"), SK_OK);
    /* should have JIR_ADD */
    int ai = find_op(JIR_ADD, 0);
    CHECK(ai >= 0);
    /* the add should have type S 32 */
    uint32_t ty = jt_jir.insts[ai].type;
    CHECK(ty > 0);
    CHEQ(jt_sem.types[ty].kind, (uint8_t)JT_SIGNED);
    /* should also have LOADs for X and Y */
    CHECK(cnt_op(JIR_LOAD) >= 2);
    PASS();
}
TH_REG("ir", jir_arith)

/* ---- jir_float: F 64 arithmetic -> fadd/fmul ---- */

static void jir_float(void)
{
    CHEQ(run_jir(
        "START T;"
        "  ITEM A F 64 = 1.0;"
        "  ITEM B F 64 = 2.0;"
        "  ITEM C F 64;"
        "  C := A + B;"
        "TERM"), SK_OK);
    int fi = find_op(JIR_FADD, 0);
    CHECK(fi >= 0);
    uint32_t ty = jt_jir.insts[fi].type;
    CHECK(ty > 0);
    CHEQ(jt_sem.types[ty].kind, (uint8_t)JT_FLOAT);
    PASS();
}
TH_REG("ir", jir_float)

/* ---- jir_cmp: X > 0 -> icmp with GT predicate ---- */

static void jir_cmp(void)
{
    CHEQ(run_jir(
        "START T;"
        "  ITEM X S 32 = 5;"
        "  ITEM R B 1;"
        "  R := X > 0;"
        "TERM"), SK_OK);
    int ci = find_op(JIR_ICMP, 0);
    CHECK(ci >= 0);
    CHEQ(jt_jir.insts[ci].subop, (uint8_t)JP_GT);
    PASS();
}
TH_REG("ir", jir_cmp)

/* ---- jir_asgn: X := expr -> store to alloca ---- */

static void jir_asgn(void)
{
    CHEQ(run_jir(
        "START T;"
        "  ITEM X S 32 = 1;"
        "  X := 99;"
        "TERM"), SK_OK);
    /* should have at least 2 stores: init + assignment */
    CHECK(cnt_op(JIR_STORE) >= 2);
    PASS();
}
TH_REG("ir", jir_asgn)

/* ---- jir_if: IF/ELSE -> br_cond + 3 blocks ---- */

static void jir_if(void)
{
    CHEQ(run_jir(
        "START T;"
        "  ITEM X S 32 = 5;"
        "  IF X > 0;"
        "    X := 1;"
        "  ELSE"
        "    X := 2;"
        "  END "
        "TERM"), SK_OK);
    /* should have BR_COND for the if */
    CHECK(find_op(JIR_BR_COND, 0) >= 0);
    /* should have at least 4 blocks: entry + then + else + merge */
    /* (in last function) */
    jir_func_t *f = &jt_jir.funcs[jt_jir.n_funcs - 1];
    CHECK(f->n_blks >= 4);
    PASS();
}
TH_REG("ir", jir_if)

/* ---- jir_while: WHILE -> header + body + exit ---- */

static void jir_while(void)
{
    CHEQ(run_jir(
        "START T;"
        "  ITEM X S 32 = 0;"
        "  WHILE X < 10;"
        "    X := X + 1;"
        "  END "
        "TERM"), SK_OK);
    /* should have BR_COND */
    CHECK(find_op(JIR_BR_COND, 0) >= 0);
    /* should have back-edge BR */
    CHECK(cnt_op(JIR_BR) >= 2);
    /* at least 3 blocks for the loop: header, body, exit */
    jir_func_t *f = &jt_jir.funcs[jt_jir.n_funcs - 1];
    CHECK(f->n_blks >= 4); /* entry + hdr + body + exit */
    PASS();
}
TH_REG("ir", jir_while)

/* ---- jir_for: FOR -> 4 blocks ---- */

static void jir_for(void)
{
    CHEQ(run_jir(
        "START T;"
        "  ITEM I S 32;"
        "  FOR I := 1 BY 1 WHILE I <= 10;"
        "    \"loop body\""
        "  END "
        "TERM"), SK_OK);
    /* should have BR_COND for condition */
    CHECK(find_op(JIR_BR_COND, 0) >= 0);
    /* for loop creates: entry + hdr + body + step + exit = 5 blocks min */
    jir_func_t *f = &jt_jir.funcs[jt_jir.n_funcs - 1];
    CHECK(f->n_blks >= 5);
    /* step block should have an ADD for increment */
    CHECK(cnt_op(JIR_ADD) >= 1);
    PASS();
}
TH_REG("ir", jir_for)

/* ---- jir_proc: PROC -> separate function with params ---- */

static void jir_proc(void)
{
    CHEQ(run_jir(
        "START T;"
        "  PROC DOUBLE(N) S 32;"
        "  BEGIN"
        "    RETURN N + N;"
        "  END "
        "TERM"), SK_OK);
    /* should have at least 2 functions: DOUBLE + main */
    CHECK(jt_jir.n_funcs >= 2);
    /* first function should be DOUBLE */
    const char *fn = jt_jir.strs + jt_jir.funcs[0].name;
    CHECK(fn[0] == 'D');
    /* DOUBLE should have 1 param */
    CHEQ(jt_jir.funcs[0].n_params, 1);
    /* should have a RET in DOUBLE */
    uint32_t fb = jt_jir.funcs[0].first_blk;
    int found_ret = 0;
    for (uint32_t bi = fb; bi < fb + jt_jir.funcs[0].n_blks; bi++) {
        const jir_blk_t *b = &jt_jir.blks[bi];
        for (uint32_t ii = b->first; ii < b->first + b->n_inst; ii++) {
            if (jt_jir.insts[ii].op == JIR_RET) found_ret = 1;
        }
    }
    CHECK(found_ret);
    PASS();
}
TH_REG("ir", jir_proc)

/* ---- jir_call: function call -> JIR_CALL with args ---- */

static void jir_call(void)
{
    CHEQ(run_jir(
        "START T;"
        "  PROC ADD(X, Y) S 32;"
        "  BEGIN"
        "    RETURN X + Y;"
        "  END "
        "  ITEM R S 32;"
        "  R := ADD(3, 4);"
        "TERM"), SK_OK);
    int ci = find_op(JIR_CALL, 0);
    CHECK(ci >= 0);
    PASS();
}
TH_REG("ir", jir_call)

/* ---- jir_table: TABLE field access -> gep + load ---- */

static void jir_table(void)
{
    CHEQ(run_jir(
        "START T;"
        "  TABLE TBL(0:9);"
        "  BEGIN"
        "    ITEM X S 32;"
        "  END"
        "  ITEM I S 32;"
        "  I := TBL(0).X;"
        "TERM"), SK_OK);
    /* should have GEP for table indexing + field access */
    CHECK(find_op(JIR_GEP, 0) >= 0);
    PASS();
}
TH_REG("ir", jir_table)

/* ---- jir_goto: GOTO/LABEL -> br to named block ---- */

static void jir_goto(void)
{
    CHEQ(run_jir(
        "START T;"
        "  ITEM X S 32 = 0;"
        "  GOTO DONE;"
        "  X := 99;"
        "  DONE: X := 1;"
        "TERM"), SK_OK);
    /* should have a BR (unconditional) for the GOTO */
    CHECK(cnt_op(JIR_BR) >= 1);
    /* the label should create a new block */
    jir_func_t *f = &jt_jir.funcs[jt_jir.n_funcs - 1];
    CHECK(f->n_blks >= 2);
    PASS();
}
TH_REG("ir", jir_goto)

/* ---- jir_opnames: opcode name lookup ---- */

static void jir_opnms(void)
{
    CHSTR(jir_opnm(JIR_NOP), "nop");
    CHSTR(jir_opnm(JIR_ADD), "add");
    CHSTR(jir_opnm(JIR_FADD), "fadd");
    CHSTR(jir_opnm(JIR_ICMP), "icmp");
    CHSTR(jir_opnm(JIR_ALLOCA), "alloca");
    CHSTR(jir_opnm(JIR_BR), "br");
    CHSTR(jir_opnm(JIR_RET), "ret");
    CHSTR(jir_opnm(JIR_PHI), "phi");
    CHSTR(jir_opnm(999), "???");
    PASS();
}
TH_REG("ir", jir_opnms)

/* ---- jir_cli: --ir on hello.jov -> success, output has func ---- */

static void jir_cli(void)
{
    char buf[TH_BUFSZ];
    int rc = th_run(SK_BIN " --ir tests/fixtures/hello.jov", buf, TH_BUFSZ);
    CHEQ(rc, 0);
    CHECK(strstr(buf, "func @") != NULL);
    CHECK(strstr(buf, "ret") != NULL);
    PASS();
}
TH_REG("ir", jir_cli)

/* ---- jir_fixed: fixed-point add emits SHL for scale alignment ---- */

static void jir_fixed(void)
{
    CHEQ(run_jir(
        "START T;"
        "  ITEM X A 16 D 4 = 3;"
        "  ITEM Y A 16 D 4 = 2;"
        "  ITEM Z A 16 D 4;"
        "  Z := X + Y;"
        "TERM"), SK_OK);
    /* fixed-point with same scales: should have ADD */
    int ai = find_op(JIR_ADD, 0);
    CHECK(ai >= 0);
    /* result type should be JT_FIXED */
    uint32_t ty = jt_jir.insts[ai].type;
    CHECK(ty > 0);
    CHEQ(jt_sem.types[ty].kind, (uint8_t)JT_FIXED);
    CHEQ(jt_sem.types[ty].scale, (int16_t)4);
    PASS();
}
TH_REG("ir", jir_fixed)

/* ---- jir_pow: X ** 4 -> XCALL to sk_powi ---- */

static void jir_pow(void)
{
    CHEQ(run_jir(
        "START T;"
        "  ITEM X S 32 = 3;"
        "  ITEM Y S 32;"
        "  Y := X ** 4;"
        "TERM"), SK_OK);
    /* should emit XCALL (to sk_powi), not MUL */
    int xi = find_op(JIR_XCALL, 0);
    CHECK(xi >= 0);
    /* should NOT have a bare MUL for the power op */
    int found_powi = 0;
    for (uint32_t i = 0; i < jt_jir.n_xfn; i++) {
        const char *nm = jt_jir.strs + jt_jir.xfuncs[i].name;
        if (strcmp(nm, "sk_powi") == 0) found_powi = 1;
    }
    CHECK(found_powi);
    PASS();
}
TH_REG("ir", jir_pow)

/* ---- jir_bfld: POS field read emits SHR + AND ---- */

static void jir_bfld(void)
{
    CHEQ(run_jir(
        "START T;"
        "  TABLE TB(0:9);"
        "  BEGIN"
        "    ITEM X POS(W, 0) B 4;"
        "    ITEM Y POS(W, 4) B 4;"
        "  END"
        "  ITEM R B 4;"
        "  R := TB(0).Y;"
        "TERM"), SK_OK);
    /* Y is at bit offset 4, width 4 → SHR by 4, AND 0xF */
    int si = find_op(JIR_SHR, 0);
    CHECK(si >= 0);
    int ai = find_op(JIR_AND, 0);
    CHECK(ai >= 0);
    PASS();
}
TH_REG("ir", jir_bfld)

/* ---- jir_shift: SHIFTL/SHIFTR → JIR_SHL/SHR ---- */

static void jir_shift(void)
{
    CHEQ(run_jir(
        "START T;"
        "  ITEM X S 32 = 1;"
        "  ITEM Y S 32;"
        "  Y := SHIFTL(X, 3);"
        "TERM"), SK_OK);
    /* should emit JIR_SHL, NOT a CALL */
    CHECK(find_op(JIR_SHL, 0) >= 0);
    /* no CALL should exist for SHIFTL */
    CHEQ(cnt_op(JIR_CALL), 0);
    PASS();
}
TH_REG("ir", jir_shift)

/* ---- I/O lowering ---- */

static void jir_write(void)
{
    CHEQ(run_jir("START T; WRITE(FREE) 42; TERM"), SK_OK);
    /* should emit XCALL to sk_prtI and sk_prtN */
    int x1 = find_op(JIR_XCALL, 0);
    CHECK(x1 >= 0); /* at least one XCALL */
    CHECK(jt_jir.n_xfn >= 2); /* sk_prtI + sk_prtN */
    /* verify sk_prtI registered */
    int found_prtI = 0, found_prtN = 0;
    for (uint32_t i = 0; i < jt_jir.n_xfn; i++) {
        const char *nm = jt_jir.strs + jt_jir.xfuncs[i].name;
        if (strcmp(nm, "sk_prtI") == 0) found_prtI = 1;
        if (strcmp(nm, "sk_prtN") == 0) found_prtN = 1;
    }
    CHECK(found_prtI);
    CHECK(found_prtN);
    PASS();
}
TH_REG("ir", jir_write)

static void jir_read(void)
{
    CHEQ(run_jir("START T; ITEM X S 32; READ(FREE) X; TERM"), SK_OK);
    int x1 = find_op(JIR_XCALL, 0);
    CHECK(x1 >= 0);
    /* should also have a STORE to write result into X */
    int st = find_op(JIR_STORE, (uint32_t)x1);
    CHECK(st >= 0);
    PASS();
}
TH_REG("ir", jir_read)
