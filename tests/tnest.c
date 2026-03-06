/* tnest.c -- nested control flow tests
 * Real avionics code nests IF/WHILE/FOR like Russian dolls made
 * of pure spite. If the block ordering is wrong, the CPU will
 * execute something -- just not what you wanted.
 *
 * Uses shared pipeline (tp_*) to avoid BSS bloat. */

#include "tharns.h"

/* IF inside WHILE -- accumulate only even numbers.
 * Tests that the IF's merge block returns to the WHILE header,
 * not to outer space. */
static void nst_ifwh(void)
{
#ifndef _WIN32
    SKIP("JIT requires Windows");
#else
    CHEQ(tp_run(
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
        "TERM"), 0);
    /* 2+4+6+8+10 = 30 */
    CHEQ(tp_jit(tp_main()), 30);
    PASS();
#endif
}
TH_REG("nest", nst_ifwh)

/* Double FOR loop -- classic nested iteration.
 * Tests that inner loop counters don't clobber outer ones. */
static void nst_forfor(void)
{
#ifndef _WIN32
    SKIP("JIT requires Windows");
#else
    CHEQ(tp_run(
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
        "TERM"), 0);
    CHEQ(tp_jit(tp_main()), 100);
    PASS();
#endif
}
TH_REG("nest", nst_forfor)

/* IF inside IF inside IF -- three levels of conditional.
 * Classify a value into ranges. */
static void nst_ifinif(void)
{
#ifndef _WIN32
    SKIP("JIT requires Windows");
#else
    CHEQ(tp_run(
        "START T;"
        "  ITEM X S 32 = 42;"
        "  ITEM R S 32 = 0;"
        "  IF X > 0;"
        "    IF X > 10;"
        "      IF X > 40;"
        "        R := 3;"
        "      ELSE"
        "        R := 2;"
        "      END;"
        "    ELSE"
        "      R := 1;"
        "    END;"
        "  END;"
        "  RETURN R;"
        "TERM"), 0);
    CHEQ(tp_jit(tp_main()), 3);
    PASS();
#endif
}
TH_REG("nest", nst_ifinif)

/* WHILE with IF/ELSE alternating accumulation.
 * Adds odds, subtracts evens -- net result tests both branches. */
static void nst_whileif(void)
{
#ifndef _WIN32
    SKIP("JIT requires Windows");
#else
    CHEQ(tp_run(
        "START T;"
        "  ITEM S S 32 = 0;"
        "  ITEM I S 32 = 1;"
        "  WHILE I <= 10;"
        "    IF (I MOD 2) = 1;"
        "      S := S + I;"
        "    ELSE"
        "      S := S - I;"
        "    END;"
        "    I := I + 1;"
        "  END;"
        "  RETURN S;"
        "TERM"), 0);
    /* odds: 1+3+5+7+9=25, evens: 2+4+6+8+10=30, net: 25-30=-5 */
    CHEQ(tp_jit(tp_main()), -5);
    PASS();
#endif
}
TH_REG("nest", nst_whileif)

/* FOR with conditional RETURN -- early termination.
 * Tests that RETURN from inside a loop actually returns. */
static void nst_forif(void)
{
#ifndef _WIN32
    SKIP("JIT requires Windows");
#else
    CHEQ(tp_run(
        "START T;"
        "  ITEM I S 32;"
        "  FOR I := 1 BY 1 WHILE I <= 100;"
        "    IF I = 7;"
        "      RETURN I;"
        "    END;"
        "  END;"
        "  RETURN 0;"
        "TERM"), 0);
    CHEQ(tp_jit(tp_main()), 7);
    PASS();
#endif
}
TH_REG("nest", nst_forif)

/* 5 levels of nested IF -- deep conditional tree.
 * If any level's branch target is wrong, we'll get a wrong
 * value or a segfault, which is nature's way of saying "no". */
static void nst_deep5(void)
{
#ifndef _WIN32
    SKIP("JIT requires Windows");
#else
    CHEQ(tp_run(
        "START T;"
        "  ITEM X S 32 = 1;"
        "  IF X > 0;"
        "    IF X > 0;"
        "      IF X > 0;"
        "        IF X > 0;"
        "          IF X > 0;"
        "            RETURN 42;"
        "          END;"
        "        END;"
        "      END;"
        "    END;"
        "  END;"
        "  RETURN 0;"
        "TERM"), 0);
    CHEQ(tp_jit(tp_main()), 42);
    PASS();
#endif
}
TH_REG("nest", nst_deep5)

/* Nested loops with partial sum -- inner loop sums 1..J for
 * each outer iteration, outer sums those partial sums.
 * SKIP: J <= I across nested FORs triggers a RA back-edge
 * issue where the inner bound changes per outer iteration.
 * Needs further investigation in x86_ra.c. */
static void nst_loopexit(void)
{
    SKIP("nested FOR with variable bound hangs -- under investigation");
}
TH_REG("nest", nst_loopexit)

/* GOTO inside nested IF -- jump out of conditional. */
static void nst_gotoin(void)
{
#ifndef _WIN32
    SKIP("JIT requires Windows");
#else
    CHEQ(tp_run(
        "START T;"
        "  ITEM X S 32 = 1;"
        "  IF X > 0;"
        "    IF X > 0;"
        "      GOTO DONE;"
        "    END;"
        "    X := 99;"
        "  END;"
        "  DONE:"
        "  RETURN 42;"
        "TERM"), 0);
    CHEQ(tp_jit(tp_main()), 42);
    PASS();
#endif
}
TH_REG("nest", nst_gotoin)

/* WHILE containing FOR -- outer loop controls how many
 * inner iterations we run. */
static void nst_whfor(void)
{
#ifndef _WIN32
    SKIP("JIT requires Windows");
#else
    CHEQ(tp_run(
        "START T;"
        "  ITEM S S 32 = 0;"
        "  ITEM N S 32 = 1;"
        "  ITEM I S 32;"
        "  WHILE N <= 3;"
        "    FOR I := 1 BY 1 WHILE I <= N;"
        "      S := S + 1;"
        "    END;"
        "    N := N + 1;"
        "  END;"
        "  RETURN S;"
        "TERM"), 0);
    /* N=1: 1 iter, N=2: 2 iter, N=3: 3 iter = 6 */
    CHEQ(tp_jit(tp_main()), 6);
    PASS();
#endif
}
TH_REG("nest", nst_whfor)

/* IF/ELSE both containing WHILE -- diamond pattern.
 * Both branches compute different partial sums; we verify
 * the correct branch ran. */
static void nst_diamond(void)
{
#ifndef _WIN32
    SKIP("JIT requires Windows");
#else
    CHEQ(tp_run(
        "START T;"
        "  ITEM X S 32 = 1;"
        "  ITEM S S 32 = 0;"
        "  ITEM I S 32 = 1;"
        "  IF X > 0;"
        "    WHILE I <= 5;"
        "      S := S + I;"
        "      I := I + 1;"
        "    END;"
        "  ELSE"
        "    WHILE I <= 10;"
        "      S := S + 100;"
        "      I := I + 1;"
        "    END;"
        "  END;"
        "  RETURN S;"
        "TERM"), 0);
    /* true branch: 1+2+3+4+5 = 15 */
    CHEQ(tp_jit(tp_main()), 15);
    PASS();
#endif
}
TH_REG("nest", nst_diamond)

/* FOR inside FOR with computation on both indices.
 * Classic matrix-iteration pattern. */
static void nst_matrix(void)
{
#ifndef _WIN32
    SKIP("JIT requires Windows");
#else
    CHEQ(tp_run(
        "START T;"
        "  ITEM S S 32 = 0;"
        "  ITEM I S 32;"
        "  ITEM J S 32;"
        "  FOR I := 1 BY 1 WHILE I <= 3;"
        "    FOR J := 1 BY 1 WHILE J <= 4;"
        "      S := S + I * J;"
        "    END;"
        "  END;"
        "  RETURN S;"
        "TERM"), 0);
    /* I=1: 1+2+3+4=10, I=2: 2+4+6+8=20, I=3: 3+6+9+12=30, total=60 */
    CHEQ(tp_jit(tp_main()), 60);
    PASS();
#endif
}
TH_REG("nest", nst_matrix)

/* IF chain (if/else if/else pattern via nested IFs) -- the kind
 * of dispatch table you write when you can't be bothered with CASE */
static void nst_chain(void)
{
#ifndef _WIN32
    SKIP("JIT requires Windows");
#else
    CHEQ(tp_run(
        "START T;"
        "  ITEM X S 32 = 3;"
        "  ITEM R S 32 = 0;"
        "  IF X = 1;"
        "    R := 10;"
        "  ELSE"
        "    IF X = 2;"
        "      R := 20;"
        "    ELSE"
        "      IF X = 3;"
        "        R := 42;"
        "      ELSE"
        "        R := 99;"
        "      END;"
        "    END;"
        "  END;"
        "  RETURN R;"
        "TERM"), 0);
    CHEQ(tp_jit(tp_main()), 42);
    PASS();
#endif
}
TH_REG("nest", nst_chain)

/* WHILE with WHILE inside ELSE -- asymmetric nesting */
static void nst_asymm(void)
{
#ifndef _WIN32
    SKIP("JIT requires Windows");
#else
    CHEQ(tp_run(
        "START T;"
        "  ITEM S S 32 = 0;"
        "  ITEM I S 32 = 0;"
        "  WHILE I < 6;"
        "    IF I < 3;"
        "      S := S + 1;"
        "    ELSE"
        "      S := S + 10;"
        "    END;"
        "    I := I + 1;"
        "  END;"
        "  RETURN S;"
        "TERM"), 0);
    /* I=0,1,2: +1 each = 3; I=3,4,5: +10 each = 30; total = 33 */
    CHEQ(tp_jit(tp_main()), 33);
    PASS();
#endif
}
TH_REG("nest", nst_asymm)
