/* trv.c -- RISC-V 64 codegen tests
 * Where we discover if our RISC-V encoding is merely
 * aspirational or actually functional, by running it
 * under QEMU like a test pilot with a parachute. */

#include "tharns.h"
#include "../src/skyhawk.h"
#include "../src/fe/token.h"
#include "../src/fe/lexer.h"
#include "../src/fe/ast.h"
#include "../src/fe/parser.h"
#include "../src/fe/sema.h"
#include "../src/ir/jir.h"
#include "../src/rv/rv.h"

/* ---- Pipeline contexts (static, too large for the stack) ---- */

#define RT_MAXTOK  4096
#define RT_MAXND   8192

static token_t    rt_toks[RT_MAXTOK];
static ast_node_t rt_nds[RT_MAXND];
static lexer_t    rt_lex;
static parser_t   rt_par;
static sema_ctx_t rt_sem;
static jir_mod_t  rt_jir;
static rv_mod_t   rt_rv;

/* ---- Full pipeline: lex -> parse -> sema -> jir -> rv ---- */

static int run_rv(const char *src)
{
    uint32_t len = (uint32_t)strlen(src);
    lexer_init(&rt_lex, src, len, rt_toks, RT_MAXTOK);
    if (lexer_run(&rt_lex) != SK_OK) return -1;
    parser_init(&rt_par, rt_toks, rt_lex.num_toks,
                src, len, rt_nds, RT_MAXND);
    if (parser_run(&rt_par) != SK_OK) return -2;
    sema_init(&rt_sem, &rt_par);
    if (sema_run(&rt_sem) != SK_OK) return -3;
    jir_init(&rt_jir, &rt_sem);
    if (jir_lower(&rt_jir) != SK_OK) return -4;
    jir_m2r(&rt_jir);
    rv_init(&rt_rv, &rt_jir);
    if (rv_emit(&rt_rv) != SK_OK) return -5;
    return 0;
}

/* ---- QEMU execution ----
 * Compile through the full pipeline, write standalone ELF,
 * run under qemu-riscv64, check the exit code.
 * If QEMU isn't available, skip gracefully. */

static int qemu_ok = -1; /* -1=unknown, 0=no, 1=yes */

static void qemu_chk(void)
{
    if (qemu_ok >= 0) return;
    char buf[256];
    int rc = th_run("qemu-riscv64 --version", buf, 256);
    qemu_ok = (rc >= 0 && strstr(buf, "QEMU") != NULL) ? 1 : 0;
}

static int64_t qemu_run(const char *src)
{
    qemu_chk();
    if (!qemu_ok) return -88888;

    if (run_rv(src) != 0) return -99999;

    const char *elf = "tests/fixtures/_rv_tmp.elf";
    if (rv_exec(&rt_rv, elf) != SK_OK) return -99998;

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "qemu-riscv64 %s", elf);
    char obuf[256];
    int rc = th_run(cmd, obuf, 256);
    return (int64_t)rc;
}

/* ---- Encoding test: verify we produce non-zero code ---- */

static void rv_enc(void)
{
    CHEQ(run_rv("START T; RETURN 42; TERM"), 0);
    CHECK(rt_rv.codelen > 0);
    CHECK(rt_rv.n_funcs > 0);
    PASS();
}
TH_REG("rv", rv_enc)

/* ---- QEMU execution tests ---- */

static void rv_ret42(void)
{
    int64_t rc = qemu_run("START T; RETURN 42; TERM");
    if (rc == -88888) SKIP("qemu-riscv64 not found");
    CHEQ(rc, 42);
    PASS();
}
TH_REG("rv", rv_ret42)

static void rv_arith(void)
{
    int64_t rc = qemu_run(
        "START T;"
        "  ITEM X S 32 = 10;"
        "  ITEM Y S 32 = 32;"
        "  RETURN X + Y;"
        "TERM");
    if (rc == -88888) SKIP("qemu-riscv64 not found");
    CHEQ(rc, 42);
    PASS();
}
TH_REG("rv", rv_arith)

static void rv_sub(void)
{
    int64_t rc = qemu_run("START T; RETURN 50 - 8; TERM");
    if (rc == -88888) SKIP("qemu-riscv64 not found");
    CHEQ(rc, 42);
    PASS();
}
TH_REG("rv", rv_sub)

static void rv_mul(void)
{
    int64_t rc = qemu_run(
        "START T;"
        "  ITEM A S 32 = 6;"
        "  ITEM B S 32 = 7;"
        "  RETURN A * B;"
        "TERM");
    if (rc == -88888) SKIP("qemu-riscv64 not found");
    CHEQ(rc, 42);
    PASS();
}
TH_REG("rv", rv_mul)

static void rv_div(void)
{
    int64_t rc = qemu_run("START T; RETURN 84 / 2; TERM");
    if (rc == -88888) SKIP("qemu-riscv64 not found");
    CHEQ(rc, 42);
    PASS();
}
TH_REG("rv", rv_div)

static void rv_cmp(void)
{
    int64_t rc = qemu_run(
        "START T;"
        "  IF 5 > 3;"
        "    RETURN 1;"
        "  ELSE"
        "    RETURN 0;"
        "  END;"
        "TERM");
    if (rc == -88888) SKIP("qemu-riscv64 not found");
    CHEQ(rc, 1);
    PASS();
}
TH_REG("rv", rv_cmp)

static void rv_loop(void)
{
    int64_t rc = qemu_run(
        "START T;"
        "  ITEM S S 32 = 0;"
        "  ITEM I S 32;"
        "  FOR I := 1 BY 1 WHILE I <= 10;"
        "    S := S + I;"
        "  END;"
        "  RETURN S;"
        "TERM");
    if (rc == -88888) SKIP("qemu-riscv64 not found");
    CHEQ(rc, 55);
    PASS();
}
TH_REG("rv", rv_loop)

static void rv_call(void)
{
    int64_t rc = qemu_run(
        "START T;"
        "  PROC DBL(N) S 32;"
        "  BEGIN"
        "    RETURN N + N;"
        "  END;"
        "  RETURN DBL(21);"
        "TERM");
    if (rc == -88888) SKIP("qemu-riscv64 not found");
    CHEQ(rc, 42);
    PASS();
}
TH_REG("rv", rv_call)

static void rv_neg(void)
{
    /* Exit codes are 0-255 unsigned on Linux, so test that
     * negation produces the right value by negating then adding.
     * -10 + 52 = 42. */
    int64_t rc = qemu_run(
        "START T;"
        "  ITEM X S 32 = 10;"
        "  ITEM Y S 32 = -X;"
        "  RETURN Y + 52;"
        "TERM");
    if (rc == -88888) SKIP("qemu-riscv64 not found");
    CHEQ(rc, 42);
    PASS();
}
TH_REG("rv", rv_neg)

static void rv_bits(void)
{
    int64_t rc = qemu_run(
        "START T;"
        "  ITEM X B 32 = 255;"
        "  ITEM Y B 32 = 15;"
        "  RETURN X AND Y;"
        "TERM");
    if (rc == -88888) SKIP("qemu-riscv64 not found");
    CHEQ(rc, 15);
    PASS();
}
TH_REG("rv", rv_bits)

/* ---- ELF output test ---- */

static void rv_elf_out(void)
{
    CHEQ(run_rv("START T; RETURN 42; TERM"), 0);
    const char *path = "tests/fixtures/rv_test.o";
    CHEQ(rv_elf(&rt_rv, path), SK_OK);

    FILE *fp = fopen(path, "rb");
    CHECK(fp != NULL);

    /* verify ELF magic */
    uint8_t hdr[16];
    CHEQ((int)fread(hdr, 1, 16, fp), 16);
    fclose(fp);

    CHEQ(hdr[0], 0x7F);
    CHEQ(hdr[1], 'E');
    CHEQ(hdr[2], 'L');
    CHEQ(hdr[3], 'F');
    CHEQ(hdr[4], 2);  /* ELFCLASS64 */
    CHEQ(hdr[5], 1);  /* ELFDATA2LSB */

    PASS();
}
TH_REG("rv", rv_elf_out)

/* ---- RV Parity Tests ----
 * Closing the gap between x86 and RV coverage.
 * If x86 can do it, RV should too — otherwise it's not
 * a backend, it's a suggestion. */

/* Float add → int return (FADD + FPTOSI on RV) */
static void rv_fadd(void)
{
    int64_t rc = qemu_run(
        "START T;"
        "  ITEM X F 64 = 3.0;"
        "  ITEM Y F 64 = 14.0;"
        "  ITEM R F 64;"
        "  R := X + Y;"
        "  RETURN R;"
        "TERM");
    if (rc == -88888) SKIP("qemu-riscv64 not found");
    CHEQ(rc, 17);
    PASS();
}
TH_REG("rv", rv_fadd)

/* Float multiply → int return (FMUL + FPTOSI) */
static void rv_fmul(void)
{
    int64_t rc = qemu_run(
        "START T;"
        "  ITEM X F 64 = 6.0;"
        "  ITEM Y F 64 = 7.0;"
        "  ITEM R F 64;"
        "  R := X * Y;"
        "  RETURN R;"
        "TERM");
    if (rc == -88888) SKIP("qemu-riscv64 not found");
    CHEQ(rc, 42);
    PASS();
}
TH_REG("rv", rv_fmul)

/* IF inside WHILE — accumulate evens (nested control flow) */
static void rv_nest(void)
{
    int64_t rc = qemu_run(
        "START T;"
        "  ITEM S S 32 = 0;"
        "  ITEM I S 32 = 1;"
        "  WHILE I <= 10;"
        "    IF (I MOD 2) = 0;"
        "      S := S + I;"
        "    END;"
        "    I := I + 1;"
        "  END;"
        "  RETURN S;"
        "TERM");
    if (rc == -88888) SKIP("qemu-riscv64 not found");
    CHEQ(rc, 30);
    PASS();
}
TH_REG("rv", rv_nest)

/* Double FOR loop — nested iteration */
static void rv_forfor(void)
{
    int64_t rc = qemu_run(
        "START T;"
        "  ITEM S S 32 = 0;"
        "  ITEM I S 32;"
        "  ITEM J S 32;"
        "  FOR I := 1 BY 1 WHILE I <= 10;"
        "    FOR J := 1 BY 1 WHILE J <= 10;"
        "      S := S + 1;"
        "    END;"
        "  END;"
        "  RETURN S;"
        "TERM");
    if (rc == -88888) SKIP("qemu-riscv64 not found");
    CHEQ(rc, 100);
    PASS();
}
TH_REG("rv", rv_forfor)

/* 15 variables — register pressure (RV has more GPRs than x86
 * but we should still test that the allocator handles it) */
static void rv_15var(void)
{
    int64_t rc = qemu_run(
        "START T;"
        "  ITEM A S 32 = 1;"
        "  ITEM B S 32 = 2;"
        "  ITEM C S 32 = 3;"
        "  ITEM D S 32 = 4;"
        "  ITEM E S 32 = 5;"
        "  ITEM F S 32 = 6;"
        "  ITEM G S 32 = 7;"
        "  ITEM H S 32 = 8;"
        "  ITEM I S 32 = 9;"
        "  ITEM J S 32 = 10;"
        "  ITEM K S 32 = 11;"
        "  ITEM L S 32 = 12;"
        "  ITEM M S 32 = 13;"
        "  ITEM N S 32 = 14;"
        "  ITEM O S 32 = 15;"
        "  RETURN A+B+C+D+E+F+G+H+I+J+K+L+M+N+O;"
        "TERM");
    if (rc == -88888) SKIP("qemu-riscv64 not found");
    /* 1+2+...+15 = 120, but exit codes are 0-255 unsigned */
    CHEQ(rc, 120);
    PASS();
}
TH_REG("rv", rv_15var)

/* PROC calling PROC — multi-level call */
static void rv_proc2(void)
{
    int64_t rc = qemu_run(
        "START T;"
        "  PROC ADD1(N) S 32;"
        "  BEGIN"
        "    RETURN N + 1;"
        "  END;"
        "  PROC ADD2(N) S 32;"
        "  BEGIN"
        "    RETURN ADD1(N) + 1;"
        "  END;"
        "  RETURN ADD2(40);"
        "TERM");
    if (rc == -88888) SKIP("qemu-riscv64 not found");
    CHEQ(rc, 42);
    PASS();
}
TH_REG("rv", rv_proc2)

/* TABLE read/write on RV */
static void rv_tbl(void)
{
    int64_t rc = qemu_run(
        "START T;"
        "  TABLE TBL(0:3);"
        "  BEGIN"
        "    ITEM V S 32;"
        "  END"
        "  TBL(0).V := 10;"
        "  TBL(1).V := 20;"
        "  TBL(2).V := 42;"
        "  RETURN TBL(2).V;"
        "TERM");
    if (rc == -88888) SKIP("qemu-riscv64 not found");
    CHEQ(rc, 42);
    PASS();
}
TH_REG("rv", rv_tbl)

/* MOD operation on RV */
static void rv_modulo(void)
{
    int64_t rc = qemu_run(
        "START T;"
        "  RETURN 47 MOD 5;"
        "TERM");
    if (rc == -88888) SKIP("qemu-riscv64 not found");
    CHEQ(rc, 2);
    PASS();
}
TH_REG("rv", rv_modulo)

/* GOTO/LABEL on RV */
static void rv_goto(void)
{
    int64_t rc = qemu_run(
        "START T;"
        "  ITEM X S 32 = 0;"
        "  GOTO SKIP;"
        "  X := 99;"
        "  SKIP:"
        "  RETURN 42;"
        "TERM");
    if (rc == -88888) SKIP("qemu-riscv64 not found");
    CHEQ(rc, 42);
    PASS();
}
TH_REG("rv", rv_goto)

/* ---- RA diagnostic ---- */

static void rv_rachk(void)
{
    CHEQ(run_rv(
        "START T;"
        "  ITEM A S 32 = 1;"
        "  ITEM B S 32 = 2;"
        "  ITEM C S 32 = 3;"
        "  RETURN A + B + C;"
        "TERM"), 0);

    static int8_t rmap[JIR_MAX_INST];
    memset(rmap, -1, sizeof(rmap));

    uint32_t mi = rt_jir.n_funcs - 1;
    rv_ra(&rt_jir, mi, rmap);

    int n_reg = 0;
    jir_func_t *f = &rt_jir.funcs[mi];
    uint32_t fb = f->first_blk;
    for (uint32_t bi = fb; bi < fb + f->n_blks; bi++) {
        const jir_blk_t *b = &rt_jir.blks[bi];
        for (uint32_t ii = b->first; ii < b->first + b->n_inst; ii++) {
            if (rmap[ii] >= 0) n_reg++;
        }
    }
    CHECK(n_reg > 0);
    PASS();
}
TH_REG("rv", rv_rachk)
