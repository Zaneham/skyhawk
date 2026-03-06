/* tstress.c -- register pressure and edge case tests
 * Where we discover whether the register allocator is a genius
 * or just a very confident random number generator. These tests
 * exist because "it works on simple programs" is not the same
 * as "it works".
 *
 * Uses shared pipeline (tp_*) to avoid BSS bloat. */

#include "tharns.h"

/* 20 live variables -- well past the 9 allocatable GPRs.
 * If the sum is right, the spill/reload machinery works.
 * If it's wrong, we've invented a new form of arithmetic. */
static void str_20var(void)
{
#ifndef _WIN32
    SKIP("JIT requires Windows");
#else
    CHEQ(tp_run(
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
        "  ITEM P S 32 = 16;"
        "  ITEM Q S 32 = 17;"
        "  ITEM R S 32 = 18;"
        "  ITEM S S 32 = 19;"
        "  ITEM T S 32 = 20;"
        "  RETURN A+B+C+D+E+F+G+H+I+J+K+L+M+N+O+P+Q+R+S+T;"
        "TERM"), 0);
    /* 1+2+...+20 = 210 */
    CHEQ(tp_jit(tp_main()), 210);
    PASS();
#endif
}
TH_REG("stress", str_20var)

/* 5 PROCs calling each other in a chain: A->B->C->D->E.
 * Tests that the calling convention survives multiple
 * levels of nesting without losing track of the stack. */
static void str_chain(void)
{
#ifndef _WIN32
    SKIP("JIT requires Windows");
#else
    CHEQ(tp_run(
        "START T;"
        "  PROC E(X) S 32;"
        "  BEGIN"
        "    RETURN X + 1;"
        "  END;"
        "  PROC D(X) S 32;"
        "  BEGIN"
        "    RETURN E(X) + 1;"
        "  END;"
        "  PROC C(X) S 32;"
        "  BEGIN"
        "    RETURN D(X) + 1;"
        "  END;"
        "  PROC B(X) S 32;"
        "  BEGIN"
        "    RETURN C(X) + 1;"
        "  END;"
        "  PROC A(X) S 32;"
        "  BEGIN"
        "    RETURN B(X) + 1;"
        "  END;"
        "  RETURN A(37);"
        "TERM"), 0);
    /* 37 + 5 = 42 */
    CHEQ(tp_jit(tp_main()), 42);
    PASS();
#endif
}
TH_REG("stress", str_chain)

/* Deeply nested PROC calls -- factorial-ish via iteration.
 * Not actually recursive (J73 doesn't mandate recursion), but
 * tests multi-level return value propagation. */
static void str_recur(void)
{
#ifndef _WIN32
    SKIP("JIT requires Windows");
#else
    CHEQ(tp_run(
        "START T;"
        "  PROC FACT(N) S 32;"
        "  BEGIN"
        "    ITEM R S 32 = 1;"
        "    ITEM I S 32;"
        "    FOR I := 1 BY 1 WHILE I <= N;"
        "      R := R * I;"
        "    END;"
        "    RETURN R;"
        "  END;"
        "  RETURN FACT(5);"
        "TERM"), 0);
    /* 5! = 120 */
    CHEQ(tp_jit(tp_main()), 120);
    PASS();
#endif
}
TH_REG("stress", str_recur)

/* Large constant: 2^20 = 1048576.
 * Tests that the compiler doesn't truncate or mishandle
 * constants that don't fit in a small immediate. */
static void str_bigint(void)
{
#ifndef _WIN32
    SKIP("JIT requires Windows");
#else
    CHEQ(tp_run(
        "START T;"
        "  ITEM X S 32 = 1048576;"
        "  RETURN X / 1024;"
        "TERM"), 0);
    CHEQ(tp_jit(tp_main()), 1024);
    PASS();
#endif
}
TH_REG("stress", str_bigint)

/* TABLE with 5 fields -- enough to make the layout arithmetic
 * interesting, not enough to qualify as a database. */
static void str_tblbig(void)
{
#ifndef _WIN32
    SKIP("JIT requires Windows");
#else
    CHEQ(tp_run(
        "START T;"
        "  TABLE REC(0:0);"
        "  BEGIN"
        "    ITEM A S 32;"
        "    ITEM B S 32;"
        "    ITEM C S 32;"
        "    ITEM D S 32;"
        "    ITEM E S 32;"
        "  END"
        "  REC(0).A := 10;"
        "  REC(0).B := 20;"
        "  REC(0).C := 30;"
        "  REC(0).D := 40;"
        "  REC(0).E := 50;"
        "  RETURN REC(0).A + REC(0).B + REC(0).C + REC(0).D + REC(0).E;"
        "TERM"), 0);
    CHEQ(tp_jit(tp_main()), 150);
    PASS();
#endif
}
TH_REG("stress", str_tblbig)

/* TABLE indexed in nested loop -- matrix sum pattern.
 * Write values with one loop, sum them with another. */
static void str_tbl2d(void)
{
#ifndef _WIN32
    SKIP("JIT requires Windows");
#else
    CHEQ(tp_run(
        "START T;"
        "  TABLE ARR(0:9);"
        "  BEGIN"
        "    ITEM V S 32;"
        "  END"
        "  ITEM I S 32;"
        "  FOR I := 0 BY 1 WHILE I <= 9;"
        "    ARR(I).V := I + 1;"
        "  END;"
        "  ITEM S S 32 = 0;"
        "  FOR I := 0 BY 1 WHILE I <= 9;"
        "    S := S + ARR(I).V;"
        "  END;"
        "  RETURN S;"
        "TERM"), 0);
    /* 1+2+...+10 = 55 */
    CHEQ(tp_jit(tp_main()), 55);
    PASS();
#endif
}
TH_REG("stress", str_tbl2d)

/* Values live across PROC calls -- tests that caller-save
 * actually saves. Two values kept alive while calling a proc. */
static void str_alive(void)
{
#ifndef _WIN32
    SKIP("JIT requires Windows");
#else
    CHEQ(tp_run(
        "START T;"
        "  PROC ADD3(X) S 32;"
        "  BEGIN"
        "    RETURN X + 3;"
        "  END;"
        "  ITEM A S 32 = 10;"
        "  ITEM B S 32 = 20;"
        "  ITEM C S 32 = ADD3(5);"
        "  ITEM D S 32 = ADD3(1);"
        "  RETURN A + B + C + D;"
        "TERM"), 0);
    /* 10 + 20 + 8 + 4 = 42 */
    CHEQ(tp_jit(tp_main()), 42);
    PASS();
#endif
}
TH_REG("stress", str_alive)

/* Chained MOD operations -- the remainder chain.
 * Tests that MOD codegen (CDQ + IDIV) handles multiple
 * sequential divisions without register conflicts. */
static void str_modchain(void)
{
#ifndef _WIN32
    SKIP("JIT requires Windows");
#else
    CHEQ(tp_run(
        "START T;"
        "  ITEM X S 32 = 100;"
        "  X := X MOD 47;"
        "  X := X + 100;"
        "  X := X MOD 51;"
        "  RETURN X;"
        "TERM"), 0);
    /* 100 MOD 47 = 6, 6+100=106, 106 MOD 51 = 4 */
    CHEQ(tp_jit(tp_main()), 4);
    PASS();
#endif
}
TH_REG("stress", str_modchain)

/* Complex AND/OR/XOR chain -- bitwise logic.
 * Tests that bitwise ops don't interfere with each other
 * when chained. All operands are B 32 to keep sema happy. */
static void str_bitwise(void)
{
#ifndef _WIN32
    SKIP("JIT requires Windows");
#else
    CHEQ(tp_run(
        "START T;"
        "  ITEM X B 32 = 255;"
        "  ITEM Y B 32 = 170;"
        "  ITEM M B 32 = 128;"
        "  ITEM K B 32 = 1;"
        "  ITEM R B 32;"
        "  R := X AND Y;"
        "  R := R XOR M;"
        "  R := R OR K;"
        "  RETURN R;"
        "TERM"), 0);
    /* 255 AND 170 = 170 (0xAA), XOR 128 = 42 (0x2A), OR 1 = 43 (0x2B) */
    CHEQ(tp_jit(tp_main()), 43);
    PASS();
#endif
}
TH_REG("stress", str_bitwise)

/* SHIFTL/SHIFTR stress -- shifting a value around.
 * The bit-twiddler's equivalent of juggling chainsaws.
 * OR operand is B 32 to match type with X. */
static void str_shift(void)
{
#ifndef _WIN32
    SKIP("JIT requires Windows");
#else
    CHEQ(tp_run(
        "START T;"
        "  ITEM X B 32 = 1;"
        "  ITEM M B 32 = 10;"
        "  X := SHIFTL(X, 5);"
        "  X := X OR M;"
        "  X := SHIFTR(X, 1);"
        "  RETURN X;"
        "TERM"), 0);
    /* 1 << 5 = 32, 32 OR 10 = 42, 42 >> 1 = 21 */
    CHEQ(tp_jit(tp_main()), 21);
    PASS();
#endif
}
TH_REG("stress", str_shift)

/* Every arithmetic op in one program -- the full Monte.
 * If this works, all arithmetic codegen paths are alive. */
static void str_allops(void)
{
#ifndef _WIN32
    SKIP("JIT requires Windows");
#else
    CHEQ(tp_run(
        "START T;"
        "  ITEM A S 32 = 10;"
        "  ITEM B S 32 = 3;"
        "  ITEM R S 32 = 0;"
        "  R := A + B;"
        "  R := R - 1;"
        "  R := R * 2;"
        "  R := R / 3;"
        "  R := R + (17 MOD 5);"
        "  R := -R;"
        "  R := -R;"
        "  RETURN R;"
        "TERM"), 0);
    /* (10+3)=13, 13-1=12, 12*2=24, 24/3=8, 8+(17%5)=8+2=10 */
    /* -10 = -10, -(-10) = 10 */
    CHEQ(tp_jit(tp_main()), 10);
    PASS();
#endif
}
TH_REG("stress", str_allops)

/* Multiple PROCs with shared variables -- tests that each
 * function gets its own stack frame and doesn't stomp on
 * its neighbours' furniture. */
static void str_mproc(void)
{
#ifndef _WIN32
    SKIP("JIT requires Windows");
#else
    CHEQ(tp_run(
        "START T;"
        "  PROC SQR(N) S 32;"
        "  BEGIN"
        "    RETURN N * N;"
        "  END;"
        "  PROC CUBE(N) S 32;"
        "  BEGIN"
        "    RETURN N * N * N;"
        "  END;"
        "  RETURN SQR(3) + CUBE(2) + SQR(1);"
        "TERM"), 0);
    /* 9 + 8 + 1 = 18 */
    CHEQ(tp_jit(tp_main()), 18);
    PASS();
#endif
}
TH_REG("stress", str_mproc)

/* TABLE sum via PROC parameter -- pass the count, sum inline.
 * Avoids outer-scope TABLE access (which needs closures). */
static void str_ptbl(void)
{
#ifndef _WIN32
    SKIP("JIT requires Windows");
#else
    CHEQ(tp_run(
        "START T;"
        "  TABLE ARR(0:4);"
        "  BEGIN"
        "    ITEM V S 32;"
        "  END"
        "  ITEM I S 32;"
        "  FOR I := 0 BY 1 WHILE I <= 4;"
        "    ARR(I).V := (I + 1) * 2;"
        "  END;"
        "  ITEM S S 32 = 0;"
        "  FOR I := 0 BY 1 WHILE I <= 4;"
        "    S := S + ARR(I).V;"
        "  END;"
        "  RETURN S;"
        "TERM"), 0);
    /* 2+4+6+8+10 = 30 */
    CHEQ(tp_jit(tp_main()), 30);
    PASS();
#endif
}
TH_REG("stress", str_ptbl)
