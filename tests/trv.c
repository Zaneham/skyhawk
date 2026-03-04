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
