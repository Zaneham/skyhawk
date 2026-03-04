/* tx86.c -- x86-64 codegen JIT tests
 * Where we discover whether our instruction encoding
 * is merely wrong or catastrophically wrong.
 * Spoiler: the CPU does not grade on a curve. */

#include "tharns.h"
#include "../src/skyhawk.h"
#include "../src/fe/token.h"
#include "../src/fe/lexer.h"
#include "../src/fe/ast.h"
#include "../src/fe/parser.h"
#include "../src/fe/sema.h"
#include "../src/ir/jir.h"
#include "../src/x86/x86.h"

#ifdef _WIN32
#include <windows.h>
#endif

/* ---- Pipeline contexts (static — too large for the stack) ---- */

#define XT_MAXTOK  4096
#define XT_MAXND   8192

static token_t    xt_toks[XT_MAXTOK];
static ast_node_t xt_nds[XT_MAXND];
static lexer_t    xt_lex;
static parser_t   xt_par;
static sema_ctx_t xt_sem;
static jir_mod_t  xt_jir;
static x86_mod_t  xt_x86;

/* ---- Full pipeline: lex → parse → sema → jir → x86 ---- */

static int run_x86(const char *src)
{
    uint32_t len = (uint32_t)strlen(src);
    lexer_init(&xt_lex, src, len, xt_toks, XT_MAXTOK);
    if (lexer_run(&xt_lex) != SK_OK) return -1;
    parser_init(&xt_par, xt_toks, xt_lex.num_toks,
                src, len, xt_nds, XT_MAXND);
    if (parser_run(&xt_par) != SK_OK) return -2;
    sema_init(&xt_sem, &xt_par);
    if (sema_run(&xt_sem) != SK_OK) return -3;
    jir_init(&xt_jir, &xt_sem);
    if (jir_lower(&xt_jir) != SK_OK) return -4;
    jir_m2r(&xt_jir);
    x86_init(&xt_x86, &xt_jir);
    if (x86_emit(&xt_x86) != SK_OK) return -5;
    return 0;
}

/* ---- JIT: alloc executable memory, copy, call, free ---- */

typedef int64_t (*jit_fn_t)(void);

static int64_t jit_run(uint32_t fn_idx)
{
#ifdef _WIN32
    uint32_t len = xt_x86.codelen;
    void *mem = VirtualAlloc(NULL, len,
                             MEM_COMMIT | MEM_RESERVE,
                             PAGE_EXECUTE_READWRITE);
    if (!mem) return -99999;

    memcpy(mem, xt_x86.code, len);

    uint32_t off = xt_x86.fn_off[fn_idx];
    jit_fn_t fn;
    /* memcpy avoids pedantic void*→fnptr cast warning */
    {
        void *addr = (uint8_t *)mem + off;
        memcpy(&fn, &addr, sizeof(fn));
    }
    int64_t result = fn();

    VirtualFree(mem, 0, MEM_RELEASE);
    return result;
#else
    /* POSIX JIT not wired yet — tests only run on Windows */
    (void)fn_idx;
    return -99999;
#endif
}

/* Helper: find "main" function (last one, the implicit entry) */
static uint32_t find_main(void)
{
    if (xt_jir.n_funcs == 0) return 0;
    return xt_jir.n_funcs - 1;
}

/* ---- Tests ---- */

static void x86_ret42(void)
{
    CHEQ(run_x86("START T; RETURN 42; TERM"), 0);
    CHEQ(jit_run(find_main()), 42);
    PASS();
}
TH_REG("codegen", x86_ret42)

static void x86_add(void)
{
    CHEQ(run_x86(
        "START T;"
        "  ITEM X S 32 = 10;"
        "  ITEM Y S 32 = 20;"
        "  RETURN X + Y;"
        "TERM"), 0);
    CHEQ(jit_run(find_main()), 30);
    PASS();
}
TH_REG("codegen", x86_add)

static void x86_sub(void)
{
    CHEQ(run_x86("START T; RETURN 50 - 8; TERM"), 0);
    CHEQ(jit_run(find_main()), 42);
    PASS();
}
TH_REG("codegen", x86_sub)

static void x86_mul(void)
{
    CHEQ(run_x86(
        "START T;"
        "  ITEM A S 32 = 6;"
        "  ITEM B S 32 = 7;"
        "  RETURN A * B;"
        "TERM"), 0);
    CHEQ(jit_run(find_main()), 42);
    PASS();
}
TH_REG("codegen", x86_mul)

static void x86_div(void)
{
    CHEQ(run_x86("START T; RETURN 84 / 2; TERM"), 0);
    CHEQ(jit_run(find_main()), 42);
    PASS();
}
TH_REG("codegen", x86_div)

static void x86_mod(void)
{
    CHEQ(run_x86("START T; RETURN 47 MOD 5; TERM"), 0);
    CHEQ(jit_run(find_main()), 2);
    PASS();
}
TH_REG("codegen", x86_mod)

static void x86_neg(void)
{
    CHEQ(run_x86(
        "START T;"
        "  ITEM X S 32 = 42;"
        "  RETURN -X;"
        "TERM"), 0);
    CHEQ(jit_run(find_main()), -42);
    PASS();
}
TH_REG("codegen", x86_neg)

static void x86_icmp(void)
{
    CHEQ(run_x86(
        "START T;"
        "  IF 5 > 3;"
        "    RETURN 1;"
        "  ELSE"
        "    RETURN 0;"
        "  END;"
        "TERM"), 0);
    CHEQ(jit_run(find_main()), 1);
    PASS();
}
TH_REG("codegen", x86_icmp)

static void x86_while(void)
{
    CHEQ(run_x86(
        "START T;"
        "  ITEM I S 32 = 0;"
        "  WHILE I < 10;"
        "    I := I + 1;"
        "  END;"
        "  RETURN I;"
        "TERM"), 0);
    CHEQ(jit_run(find_main()), 10);
    PASS();
}
TH_REG("codegen", x86_while)

static void x86_for(void)
{
    CHEQ(run_x86(
        "START T;"
        "  ITEM S S 32 = 0;"
        "  ITEM I S 32;"
        "  FOR I := 1 BY 1 WHILE I <= 5;"
        "    S := S + I;"
        "  END;"
        "  RETURN S;"
        "TERM"), 0);
    CHEQ(jit_run(find_main()), 15);
    PASS();
}
TH_REG("codegen", x86_for)

static void x86_bits(void)
{
    CHEQ(run_x86(
        "START T;"
        "  ITEM X B 32 = 255;"
        "  ITEM Y B 32 = 15;"
        "  RETURN X AND Y;"
        "TERM"), 0);
    CHEQ(jit_run(find_main()), 15);
    PASS();
}
TH_REG("codegen", x86_bits)

static void x86_proc(void)
{
    CHEQ(run_x86(
        "START T;"
        "  PROC DBL(N) S 32;"
        "  BEGIN"
        "    RETURN N + N;"
        "  END;"
        "  RETURN DBL(21);"
        "TERM"), 0);
    CHEQ(jit_run(find_main()), 42);
    PASS();
}
TH_REG("codegen", x86_proc)

/* ---- TABLE Tests ----
 * Where we prove that tables actually work and aren't just
 * elaborate suggestions that the compiler politely ignores. */

static void x86_tbl_rw(void)
{
    CHEQ(run_x86(
        "START T;"
        "  TABLE TBL(0:3);"
        "  BEGIN"
        "    ITEM X S 32;"
        "  END"
        "  TBL(0).X := 42;"
        "  RETURN TBL(0).X;"
        "TERM"), 0);
    CHEQ(jit_run(find_main()), 42);
    PASS();
}
TH_REG("codegen", x86_tbl_rw)

static void x86_tbl_idx(void)
{
    CHEQ(run_x86(
        "START T;"
        "  TABLE TBL(0:9);"
        "  BEGIN"
        "    ITEM V S 32;"
        "  END"
        "  TBL(0).V := 10;"
        "  TBL(3).V := 77;"
        "  TBL(9).V := 99;"
        "  RETURN TBL(3).V;"
        "TERM"), 0);
    CHEQ(jit_run(find_main()), 77);
    PASS();
}
TH_REG("codegen", x86_tbl_idx)

static void x86_tbl_mf(void)
{
    CHEQ(run_x86(
        "START T;"
        "  TABLE REC(0:1);"
        "  BEGIN"
        "    ITEM A S 32;"
        "    ITEM B S 32;"
        "  END"
        "  REC(0).A := 10;"
        "  REC(0).B := 20;"
        "  REC(1).A := 30;"
        "  REC(1).B := 40;"
        "  RETURN REC(0).A + REC(1).B;"
        "TERM"), 0);
    CHEQ(jit_run(find_main()), 50);
    PASS();
}
TH_REG("codegen", x86_tbl_mf)

static void x86_tbl_loop(void)
{
    CHEQ(run_x86(
        "START T;"
        "  TABLE ARR(0:4);"
        "  BEGIN"
        "    ITEM V S 32;"
        "  END"
        "  ITEM I S 32;"
        "  ITEM S S 32 = 0;"
        "  FOR I := 0 BY 1 WHILE I <= 4;"
        "    ARR(I).V := I * 10;"
        "  END;"
        "  FOR I := 0 BY 1 WHILE I <= 4;"
        "    S := S + ARR(I).V;"
        "  END;"
        "  RETURN S;"
        "TERM"), 0);
    /* 0+10+20+30+40 = 100 */
    CHEQ(jit_run(find_main()), 100);
    PASS();
}
TH_REG("codegen", x86_tbl_loop)

/* ---- Register Pressure Tests ----
 * Where we discover whether the allocator actually spills
 * or just wishes really hard and hopes for the best. */

static void x86_rpres(void)
{
    /* 10 live integer variables — exceeds 9 allocatable GPRs,
     * so at least one must spill. If the result is right,
     * the spill/reload machinery works. */
    CHEQ(run_x86(
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
        "  RETURN A + B + C + D + E + F + G + H + I + J;"
        "TERM"), 0);
    CHEQ(jit_run(find_main()), 55);
    PASS();
}
TH_REG("codegen", x86_rpres)

static void x86_rcall(void)
{
    /* Value live across a procedure call — exercises
     * caller-save around CALL. If 10 comes back wrong,
     * R10 got clobbered and nobody noticed. */
    CHEQ(run_x86(
        "START T;"
        "  PROC IDENT(N) S 32;"
        "  BEGIN"
        "    RETURN N;"
        "  END;"
        "  ITEM X S 32 = 10;"
        "  ITEM Y S 32 = IDENT(32);"
        "  RETURN X + Y;"
        "TERM"), 0);
    CHEQ(jit_run(find_main()), 42);
    PASS();
}
TH_REG("codegen", x86_rcall)

/* ---- COMPOOL Tests ----
 * Proving that shared data declarations survive the journey
 * from COMPOOL into the enclosing scope, like luggage that
 * actually arrives at the right carousel. */

static void cpl_item(void)
{
    CHEQ(run_x86(
        "START T;"
        "  COMPOOL CPL;"
        "  BEGIN"
        "    ITEM MAGIC S 32 = 42;"
        "  END"
        "  RETURN MAGIC;"
        "TERM"), 0);
    CHEQ(jit_run(find_main()), 42);
    PASS();
}
TH_REG("compool", cpl_item)

static void cpl_const(void)
{
    CHEQ(run_x86(
        "START T;"
        "  COMPOOL DEFS;"
        "  BEGIN"
        "    DEFINE MAX = 99;"
        "  END"
        "  RETURN MAX;"
        "TERM"), 0);
    CHEQ(jit_run(find_main()), 99);
    PASS();
}
TH_REG("compool", cpl_const)

static void cpl_status(void)
{
    CHEQ(run_x86(
        "START T;"
        "  COMPOOL HDG'POOL;"
        "  BEGIN"
        "    TYPE DIR STATUS(V(NORTH), V(SOUTH), V(EAST), V(WEST));"
        "    ITEM HDG DIR = V(SOUTH);"
        "  END"
        "  RETURN HDG;"
        "TERM"), 0);
    CHEQ(jit_run(find_main()), 1);
    PASS();
}
TH_REG("compool", cpl_status)

static void cpl_multi(void)
{
    CHEQ(run_x86(
        "START T;"
        "  COMPOOL NUMS;"
        "  BEGIN"
        "    ITEM A S 32 = 10;"
        "    ITEM B S 32 = 20;"
        "    ITEM C S 32 = 3;"
        "  END"
        "  RETURN (A + B) * C;"
        "TERM"), 0);
    CHEQ(jit_run(find_main()), 90);
    PASS();
}
TH_REG("compool", cpl_multi)

static void cpl_cli(void)
{
    char buf[TH_BUFSZ];
    int rc = th_run(SK_BIN " --ir tests/fixtures/compool.jov", buf, TH_BUFSZ);
    CHEQ(rc, 0);
    CHECK(strstr(buf, "__CPL__") != NULL);
    CHECK(strstr(buf, "ret") != NULL);
    PASS();
}
TH_REG("compool", cpl_cli)

/* ---- Binary COMPOOL (.cpl) Tests ----
 * The ABI contract: serialise it, deserialise it, and pray
 * that both ends of the telephone agree on what was said. */

static void cpl_write(void)
{
    /* compile minicpl.jov → minicpl.cpl, check magic */
    char buf[TH_BUFSZ];
    int rc = th_run(SK_BIN " tests/fixtures/minicpl.jov", buf, TH_BUFSZ);
    CHEQ(rc, 0);
    CHECK(strstr(buf, ".cpl written") != NULL);

    /* read first 4 bytes — should be "CPL1" (LE: 0x31,0x4C,0x50,0x43) */
    FILE *fp = fopen("tests/fixtures/minicpl.cpl", "rb");
    CHECK(fp != NULL);
    uint8_t hdr[4] = {0};
    size_t n = fread(hdr, 1, 4, fp);
    fclose(fp);
    CHEQ((int)n, 4);
    CHEQ(hdr[0], 0x31); /* '1' */
    CHEQ(hdr[1], 0x4C); /* 'L' */
    CHEQ(hdr[2], 0x50); /* 'P' */
    CHEQ(hdr[3], 0x43); /* 'C' */
    PASS();
}
TH_REG("compool", cpl_write)

static void cpl_imprt(void)
{
    /* compile minicpl.jov to .cpl first */
    char buf[TH_BUFSZ];
    th_run(SK_BIN " tests/fixtures/minicpl.jov", buf, TH_BUFSZ);

    /* import .cpl and compile a program that uses MAGIC + LIMIT */
    int rc = th_run(
        SK_BIN " --cpl tests/fixtures/minicpl.cpl"
               " tests/fixtures/usecpl.jov",
        buf, TH_BUFSZ);
    CHEQ(rc, 0);
    CHECK(strstr(buf, "bytes generated") != NULL);
    PASS();
}
TH_REG("compool", cpl_imprt)

static void cpl_irdmp(void)
{
    /* compile minicpl.jov to .cpl, then --ir with import */
    char buf[TH_BUFSZ];
    th_run(SK_BIN " tests/fixtures/minicpl.jov", buf, TH_BUFSZ);

    int rc = th_run(
        SK_BIN " --ir --cpl tests/fixtures/minicpl.cpl"
               " tests/fixtures/usecpl.jov",
        buf, TH_BUFSZ);
    CHEQ(rc, 0);
    /* IR should show the add of MAGIC(42) + LIMIT(99) */
    CHECK(strstr(buf, "add") != NULL);
    CHECK(strstr(buf, "ret") != NULL);
    PASS();
}
TH_REG("compool", cpl_irdmp)

static void cpl_bigcp(void)
{
    /* full compool.jov (NAV pool with TABLE, STATUS, TYPE) → .cpl */
    char buf[TH_BUFSZ];
    int rc = th_run(SK_BIN " tests/fixtures/compool.jov", buf, TH_BUFSZ);
    CHEQ(rc, 0);
    CHECK(strstr(buf, ".cpl written") != NULL);
    CHECK(th_exist("tests/fixtures/compool.cpl"));
    PASS();
}
TH_REG("compool", cpl_bigcp)

/* ---- PE-COFF object file tests ---- */

static void t_coff(void)
{
    /* compile a simple program, write .obj, read back and verify */
    CHEQ(run_x86("START T; RETURN 42; TERM"), 0);
    const char *path = "tests/fixtures/test.obj";
    CHEQ(x86_coff(&xt_x86, path), SK_OK);

    FILE *fp = fopen(path, "rb");
    CHECK(fp != NULL);

    /* read file into buffer */
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    CHECK(sz > 60); /* at minimum: 20 hdr + 40 shdr + code */

    uint8_t hdr[60];
    CHEQ((int)fread(hdr, 1, 60, fp), 60);
    fclose(fp);

    /* IMAGE_FILE_HEADER checks */
    uint16_t mach = (uint16_t)(hdr[0] | (hdr[1] << 8));
    CHEQ(mach, 0x8664);

    uint16_t nsec = (uint16_t)(hdr[2] | (hdr[3] << 8));
    CHEQ(nsec, 1);

    /* section header: SizeOfRawData at offset 36 (20+16) */
    uint32_t rawsz = (uint32_t)(hdr[36] | (hdr[37] << 8) |
                     (hdr[38] << 16) | (hdr[39] << 24));
    CHEQ(rawsz, xt_x86.codelen);

    /* NumberOfSymbols at offset 12 */
    uint32_t nsym = (uint32_t)(hdr[12] | (hdr[13] << 8) |
                    (hdr[14] << 16) | (hdr[15] << 24));
    CHEQ(nsym, xt_x86.n_funcs + 1);

    PASS();
}
TH_REG("coff", t_coff)

/* ---- XCALL infrastructure test ----
 * Verify that JIR_XCALL creates proper code and COFF
 * records an UNDEF symbol + relocation, without actually
 * linking against anything. It's a trust exercise. */

static void t_xcall(void)
{
    /* Build a JIR module by hand with one XCALL */
    CHEQ(run_x86("START T; RETURN 42; TERM"), 0);

    /* manually inject an XCALL into the JIR module.
     * We register an external func, emit XCALL in x86. */
    uint32_t xi = jir_xfn(&xt_jir, "sk_prtI");
    CHEQ(xi, 0); /* first external func */
    CHEQ(xt_jir.n_xfn, 1);

    /* verify dedup */
    uint32_t xi2 = jir_xfn(&xt_jir, "sk_prtI");
    CHEQ(xi2, 0);
    CHEQ(xt_jir.n_xfn, 1); /* still 1 */

    /* add a second */
    uint32_t xi3 = jir_xfn(&xt_jir, "sk_prtF");
    CHEQ(xi3, 1);
    CHEQ(xt_jir.n_xfn, 2);

    /* verify name in string pool */
    const char *nm = xt_jir.strs + xt_jir.xfuncs[0].name;
    CHECK(strcmp(nm, "sk_prtI") == 0);

    PASS();
}
TH_REG("coff", t_xcall)

static void t_xcoff(void)
{
    /* compile, manually add an xfunc + fake xfx entry, write .obj,
     * verify UNDEF symbol + relocation present */
    CHEQ(run_x86("START T; RETURN 42; TERM"), 0);

    /* register external func */
    jir_xfn(&xt_jir, "sk_halt");

    /* fake an xfx entry (normally x86_emit does this) */
    xt_x86.xfx[0].off = 10; /* fake offset */
    xt_x86.xfx[0].xfn = 0;
    xt_x86.n_xfx = 1;

    const char *path = "tests/fixtures/xcall.obj";
    CHEQ(x86_coff(&xt_x86, path), SK_OK);

    /* read back and verify */
    FILE *fp = fopen(path, "rb");
    CHECK(fp != NULL);
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    static uint8_t buf[4096];
    CHECK(sz > 0 && sz < (long)sizeof(buf));
    CHEQ((int)fread(buf, 1, (size_t)sz, fp), (int)sz);
    fclose(fp);

    /* NumberOfSymbols should be 1(.text) + n_funcs + 1(extern) */
    uint32_t nsym = (uint32_t)(buf[12] | (buf[13] << 8) |
                    (buf[14] << 16) | (buf[15] << 24));
    CHEQ(nsym, xt_x86.n_funcs + 1 + xt_jir.n_xfn);

    /* .text NumberOfRelocations at section header offset 32 (20+12) = hdr[52..53] */
    uint16_t nrel = (uint16_t)(buf[52] | (buf[53] << 8));
    CHEQ(nrel, 1);

    /* find symbol table, look for UNDEF sym (section=0) */
    uint32_t sym_off = (uint32_t)(buf[8] | (buf[9] << 8) |
                       (buf[10] << 16) | (buf[11] << 24));
    /* last sym is the extern: section number at sym+12 should be 0 */
    uint32_t xs = sym_off + 18 * (nsym - 1);
    CHECK(xs + 18 <= (uint32_t)sz);
    uint16_t sec = (uint16_t)(buf[xs + 12] | (buf[xs + 13] << 8));
    CHEQ(sec, 0); /* UNDEF */

    /* check relocation entry (after .text raw data) */
    uint32_t roff = 60 + xt_x86.codelen;
    CHECK(roff + 10 <= (uint32_t)sz);
    uint32_t rel_vaddr = (uint32_t)(buf[roff] | (buf[roff+1] << 8) |
                         (buf[roff+2] << 16) | (buf[roff+3] << 24));
    CHEQ(rel_vaddr, 10); /* our fake offset */
    uint16_t rel_type = (uint16_t)(buf[roff+8] | (buf[roff+9] << 8));
    CHEQ(rel_type, COFF_REL32);

    /* clean up the injected state for next tests */
    xt_x86.n_xfx = 0;

    PASS();
}
TH_REG("coff", t_xcoff)

static void x86_cfmt(void)
{
    /* multi-function program: verify both symbols present */
    const char *src =
        "START MF;"
        "  PROC ADD2(N) S 32;"
        "  BEGIN"
        "    RETURN N + N;"
        "  END;"
        "  RETURN ADD2(21);"
        "TERM";
    CHEQ(run_x86(src), 0);
    CHECK(xt_x86.n_funcs >= 2);

    const char *path = "tests/fixtures/multi.obj";
    CHEQ(x86_coff(&xt_x86, path), SK_OK);

    FILE *fp = fopen(path, "rb");
    CHECK(fp != NULL);

    /* read entire file */
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    static uint8_t buf[4096];
    CHECK(sz > 0 && sz < (long)sizeof(buf));
    CHEQ((int)fread(buf, 1, (size_t)sz, fp), (int)sz);
    fclose(fp);

    /* symbol table starts at offset stored in hdr[8..11] */
    uint32_t sym_off = (uint32_t)(buf[8] | (buf[9] << 8) |
                       (buf[10] << 16) | (buf[11] << 24));
    uint32_t nsym = (uint32_t)(buf[12] | (buf[13] << 8) |
                    (buf[14] << 16) | (buf[15] << 24));
    CHEQ(nsym, xt_x86.n_funcs + 1);

    /* second symbol (index 1) should be ADD2 — check name */
    uint32_t s1 = sym_off + 18; /* skip .text sym */
    CHECK(s1 + 18 <= (uint32_t)sz);
    CHECK(memcmp(buf + s1, "ADD2", 4) == 0);

    /* third symbol (index 2) value should match fn_off[1] */
    uint32_t s2 = sym_off + 36;
    CHECK(s2 + 18 <= (uint32_t)sz);
    uint32_t val = (uint32_t)(buf[s2 + 8] | (buf[s2 + 9] << 8) |
                   (buf[s2 + 10] << 16) | (buf[s2 + 11] << 24));
    CHEQ(val, xt_x86.fn_off[find_main()]);

    PASS();
}
TH_REG("coff", x86_cfmt)

/* ---- I/O COFF test: compile WRITE(FREE) 42, verify ext syms ---- */

static void t_iocoff(void)
{
    CHEQ(run_x86("START T; WRITE(FREE) 42; TERM"), SK_OK);
    const char *path = "tests/fixtures/io_test.obj";
    int rc = x86_coff(&xt_x86, path);
    CHEQ(rc, SK_OK);

    /* verify external symbols for sk_prtI and sk_prtN exist */
    CHECK(xt_jir.n_xfn >= 2);
    int found_prtI = 0, found_prtN = 0;
    for (uint32_t i = 0; i < xt_jir.n_xfn; i++) {
        const char *nm = xt_jir.strs + xt_jir.xfuncs[i].name;
        if (strcmp(nm, "sk_prtI") == 0) found_prtI = 1;
        if (strcmp(nm, "sk_prtN") == 0) found_prtN = 1;
    }
    CHECK(found_prtI);
    CHECK(found_prtN);

    /* verify .obj file exists and has non-trivial size */
    FILE *fp = fopen(path, "rb");
    CHECK(fp != NULL);
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fclose(fp);
    CHECK(sz > 100); /* has file header + sections + symbols */

    PASS();
}
TH_REG("coff", t_iocoff)

/* ---- RA diagnostic: verify allocator assigns registers ---- */

static void x86_rachk(void)
{
    CHEQ(run_x86(
        "START T;"
        "  ITEM A S 32 = 1;"
        "  ITEM B S 32 = 2;"
        "  ITEM C S 32 = 3;"
        "  RETURN A + B + C;"
        "TERM"), 0);
    CHEQ(jit_run(find_main()), 6);

    /* run RA on the main function and check that at least one
     * instruction got a physical register assigned — proof that
     * the allocator did something besides stare blankly. */
    static int8_t rmap[JIR_MAX_INST];
    memset(rmap, -1, sizeof(rmap));
    x86_ra(&xt_jir, find_main(), rmap);

    int n_reg = 0;
    jir_func_t *f = &xt_jir.funcs[find_main()];
    uint32_t fb = f->first_blk;
    for (uint32_t bi = fb; bi < fb + f->n_blks; bi++) {
        const jir_blk_t *b = &xt_jir.blks[bi];
        for (uint32_t ii = b->first; ii < b->first + b->n_inst; ii++) {
            if (rmap[ii] >= 0) n_reg++;
        }
    }
    CHECK(n_reg > 0);
    PASS();
}
TH_REG("codegen", x86_rachk)
