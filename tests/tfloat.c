/* tfloat.c -- floating-point codegen tests
 * Where we discover whether IEEE 754 is a standard or merely
 * a polite suggestion. These tests verify that the float pipeline
 * (FADD/FSUB/FMUL/FDIV/FNEG/FCMP/SITOFP/FPTOSI) generates
 * correct opcodes all the way through x86 emit.
 *
 * NOTE: JIT float execution is limited because Win64 ABI returns
 * floats in XMM0 but our JIT harness reads RAX. Tests that need
 * execution use integer branches (IF X > Y; RETURN int; END).
 * Full float JIT will land when FPTOSI on assignment is wired up.
 *
 * Uses shared pipeline (tp_*) to avoid BSS bloat -- six copies
 * of a 6 MB struct is how you corrupt stdout on MinGW. */

#include "tharns.h"

/* FADD -- the gateway drug to float codegen */
static void flt_add(void)
{
    CHEQ(tp_run(
        "START T;"
        "  ITEM X F 64 = 3.0;"
        "  ITEM Y F 64 = 14.0;"
        "  ITEM R F 64;"
        "  R := X + Y;"
        "TERM"), 0);
    CHECK(tp_fndop(JIR_FADD, 0) >= 0);
    CHECK(tp_x86.codelen > 0);
    PASS();
}
TH_REG("codegen", flt_add)

/* FSUB: subtraction, the arithmetic of accountants and grudges */
static void flt_sub(void)
{
    CHEQ(tp_run(
        "START T;"
        "  ITEM X F 64 = 50.5;"
        "  ITEM Y F 64 = 8.5;"
        "  ITEM R F 64;"
        "  R := X - Y;"
        "TERM"), 0);
    CHECK(tp_fndop(JIR_FSUB, 0) >= 0);
    CHECK(tp_x86.codelen > 0);
    PASS();
}
TH_REG("codegen", flt_sub)

/* FMUL: multiplication, where rounding errors go to breed */
static void flt_mul(void)
{
    CHEQ(tp_run(
        "START T;"
        "  ITEM X F 64 = 6.5;"
        "  ITEM Y F 64 = 7.0;"
        "  ITEM R F 64;"
        "  R := X * Y;"
        "TERM"), 0);
    CHECK(tp_fndop(JIR_FMUL, 0) >= 0);
    CHECK(tp_x86.codelen > 0);
    PASS();
}
TH_REG("codegen", flt_mul)

/* FDIV: division, where we pray the denominator isn't zero */
static void flt_div(void)
{
    CHEQ(tp_run(
        "START T;"
        "  ITEM X F 64 = 84.0;"
        "  ITEM Y F 64 = 2.0;"
        "  ITEM R F 64;"
        "  R := X / Y;"
        "TERM"), 0);
    CHECK(tp_fndop(JIR_FDIV, 0) >= 0);
    CHECK(tp_x86.codelen > 0);
    PASS();
}
TH_REG("codegen", flt_div)

/* FNEG: negation -- the existential crisis of a floating-point number */
static void flt_neg(void)
{
    CHEQ(tp_run(
        "START T;"
        "  ITEM X F 64 = 42.0;"
        "  ITEM Y F 64;"
        "  Y := -X;"
        "TERM"), 0);
    CHECK(tp_fndop(JIR_FNEG, 0) >= 0);
    CHECK(tp_x86.codelen > 0);
    PASS();
}
TH_REG("codegen", flt_neg)

/* FCMP via JIT -- the one float test we can execute, because
 * IF/ELSE returns integers (which travel in RAX). */
static void flt_cmp(void)
{
#ifndef _WIN32
    SKIP("JIT requires Windows");
#else
    CHEQ(tp_run(
        "START T;"
        "  ITEM X F 64 = 3.14;"
        "  ITEM Y F 64 = 2.71;"
        "  IF X > Y;"
        "    RETURN 1;"
        "  ELSE"
        "    RETURN 0;"
        "  END;"
        "TERM"), 0);
    CHEQ(tp_jit(tp_main()), 1);
    PASS();
#endif
}
TH_REG("codegen", flt_cmp)

/* Chained float ops -- if intermediate storage is wrong, the
 * chain breaks and opcodes vanish */
static void flt_chain(void)
{
    CHEQ(tp_run(
        "START T;"
        "  ITEM X F 64 = 2.0;"
        "  X := X * 3.0;"
        "  X := X + 1.0;"
        "  X := X * 6.0;"
        "TERM"), 0);
    /* should have 2 FMUL and 1 FADD */
    CHECK(tp_cntop(JIR_FMUL) >= 2);
    CHECK(tp_cntop(JIR_FADD) >= 1);
    CHECK(tp_x86.codelen > 0);
    PASS();
}
TH_REG("codegen", flt_chain)

/* Float in a loop -- where FADD meets the back-edge and they
 * have to learn to share registers like adults */
static void flt_loop(void)
{
    CHEQ(tp_run(
        "START T;"
        "  ITEM X F 64 = 0.0;"
        "  ITEM I S 32;"
        "  FOR I := 1 BY 1 WHILE I <= 10;"
        "    X := X + 1.0;"
        "  END;"
        "TERM"), 0);
    CHECK(tp_fndop(JIR_FADD, 0) >= 0);
    CHECK(tp_x86.codelen > 0);
    PASS();
}
TH_REG("codegen", flt_loop)

/* All float arith ops in one program -- the full Monte */
static void flt_allops(void)
{
    CHEQ(tp_run(
        "START T;"
        "  ITEM A F 64 = 10.0;"
        "  ITEM B F 64 = 3.0;"
        "  ITEM R F 64;"
        "  R := A + B;"
        "  R := R - 1.0;"
        "  R := R * 2.0;"
        "  R := R / 3.0;"
        "  ITEM N F 64;"
        "  N := -R;"
        "TERM"), 0);
    CHECK(tp_fndop(JIR_FADD, 0) >= 0);
    CHECK(tp_fndop(JIR_FSUB, 0) >= 0);
    CHECK(tp_fndop(JIR_FMUL, 0) >= 0);
    CHECK(tp_fndop(JIR_FDIV, 0) >= 0);
    CHECK(tp_fndop(JIR_FNEG, 0) >= 0);
    CHECK(tp_x86.codelen > 0);
    PASS();
}
TH_REG("codegen", flt_allops)
