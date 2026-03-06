/* tavion.c -- avionics-flavoured JOVIAL programs
 * Because if you're going to test a compiler for a language that flew
 * in the F-16, you might as well write code that sounds like it could
 * have started a small war. None of this code is classified. Probably.
 *
 * Uses shared pipeline (tp_*) to avoid BSS bloat. */

#include "tharns.h"

/* ---- Avionics Tests ----
 * Programs that feel like they could have flown in something with
 * wings and an ejection seat. The compiler had better get these
 * right, because at Mach 2 there are no second chances. */

/* Deep Thought computes 42 via unnecessarily complex arithmetic.
 * Because the answer to life, the universe, and everything should
 * require at least three operations to compute. Douglas Adams
 * would have wanted it this way. */
static void av_answer(void)
{
#ifndef _WIN32
    SKIP("JIT requires Windows");
#else
    CHEQ(tp_run(
        "START DEEP'THOUGHT;"
        "  ITEM X S 32 = 6;"
        "  ITEM Y S 32 = 9;"
        "  ITEM Z S 32;"
        "  Z := X * Y;"
        "  Z := Z - 12;"
        "  RETURN Z;"
        "TERM"), 0);
    /* 6*9=54, 54-12=42 (in base 13, 6*9=42, but we're in base 10) */
    CHEQ(tp_jit(tp_main()), 42);
    PASS();
#endif
}
TH_REG("avionics", av_answer)

/* Piecewise linear throttle curve -- the F-16's throttle was
 * piecewise linear because the alternative was trusting a
 * polynomial to behave itself at Mach 2, and polynomials at
 * high speed are about as trustworthy as a fox in a henhouse.
 * Three segments: idle (0-30), mil (30-80), AB (80-100). */
static void av_throttle(void)
{
#ifndef _WIN32
    SKIP("JIT requires Windows");
#else
    CHEQ(tp_run(
        "START THROTTLE;"
        "  ITEM PLA S 32 = 60;"
        "  ITEM THRUST S 32;"
        "  IF PLA < 30;"
        "    THRUST := PLA * 2;"
        "  ELSE"
        "    IF PLA < 80;"
        "      THRUST := 60 + (PLA - 30);"
        "    ELSE"
        "      THRUST := 110 + (PLA - 80) * 3;"
        "    END;"
        "  END;"
        "  RETURN THRUST;"
        "TERM"), 0);
    /* PLA=60: mil segment, 60 + (60-30) = 90 */
    CHEQ(tp_jit(tp_main()), 90);
    PASS();
#endif
}
TH_REG("avionics", av_throttle)

/* Waypoint distance -- simplified integer Pythagoras.
 * Real navigation uses haversine on a WGS84 ellipsoid;
 * this version uses dx*dx + dy*dy because we're testing
 * a compiler, not launching a satellite. */
static void av_waypoint(void)
{
#ifndef _WIN32
    SKIP("JIT requires Windows");
#else
    CHEQ(tp_run(
        "START NAV;"
        "  ITEM X1 S 32 = 3;"
        "  ITEM Y1 S 32 = 0;"
        "  ITEM X2 S 32 = 0;"
        "  ITEM Y2 S 32 = 4;"
        "  ITEM DX S 32;"
        "  ITEM DY S 32;"
        "  DX := X2 - X1;"
        "  DY := Y2 - Y1;"
        "  RETURN DX * DX + DY * DY;"
        "TERM"), 0);
    /* (-3)^2 + 4^2 = 9 + 16 = 25 (which is 5^2, a 3-4-5 triangle) */
    CHEQ(tp_jit(tp_main()), 25);
    PASS();
#endif
}
TH_REG("avionics", av_waypoint)

/* Radar TABLE lookup -- find a target code, return its value.
 * The classic avionics pattern: scan a table, match on key,
 * return the associated value. If not found, return -1
 * because radar contacts that vanish are never good news. */
static void av_lookup(void)
{
#ifndef _WIN32
    SKIP("JIT requires Windows");
#else
    CHEQ(tp_run(
        "START RADAR;"
        "  TABLE TGTS(0:3);"
        "  BEGIN"
        "    ITEM CODE S 32;"
        "    ITEM RNG  S 32;"
        "  END"
        "  TGTS(0).CODE := 100; TGTS(0).RNG := 50;"
        "  TGTS(1).CODE := 200; TGTS(1).RNG := 75;"
        "  TGTS(2).CODE := 300; TGTS(2).RNG := 42;"
        "  TGTS(3).CODE := 400; TGTS(3).RNG := 90;"
        "  ITEM I S 32;"
        "  ITEM RESULT S 32 = -1;"
        "  FOR I := 0 BY 1 WHILE I <= 3;"
        "    IF TGTS(I).CODE = 300;"
        "      RESULT := TGTS(I).RNG;"
        "    END;"
        "  END;"
        "  RETURN RESULT;"
        "TERM"), 0);
    CHEQ(tp_jit(tp_main()), 42);
    PASS();
#endif
}
TH_REG("avionics", av_lookup)

/* XOR-based checksum over a data TABLE.
 * Not CRC-32, but the principle is the same: fold data into
 * a single value. All items are B 32 for type-consistent XOR. */
static void av_checksum(void)
{
#ifndef _WIN32
    SKIP("JIT requires Windows");
#else
    CHEQ(tp_run(
        "START COMMS;"
        "  TABLE DATA(0:4);"
        "  BEGIN"
        "    ITEM V B 32;"
        "  END"
        "  DATA(0).V := 170;"
        "  DATA(1).V := 85;"
        "  DATA(2).V := 255;"
        "  DATA(3).V := 0;"
        "  DATA(4).V := 42;"
        "  ITEM CRC B 32 = 0;"
        "  ITEM I S 32;"
        "  FOR I := 0 BY 1 WHILE I <= 4;"
        "    CRC := CRC XOR DATA(I).V;"
        "  END;"
        "  RETURN CRC;"
        "TERM"), 0);
    /* 0 ^ 170 = 170, ^ 85 = 255, ^ 255 = 0, ^ 0 = 0, ^ 42 = 42 */
    CHEQ(tp_jit(tp_main()), 42);
    PASS();
#endif
}
TH_REG("avionics", av_checksum)

/* Altitude fence -- clamp altitude to min/max bounds.
 * In real avionics, this is the difference between a
 * controlled descent and an uncontrolled one. */
static void av_altitude(void)
{
#ifndef _WIN32
    SKIP("JIT requires Windows");
#else
    CHEQ(tp_run(
        "START ALT'FENCE;"
        "  ITEM ALT S 32 = 55000;"
        "  ITEM FLOOR S 32 = 500;"
        "  ITEM CEIL S 32 = 50000;"
        "  IF ALT < FLOOR;"
        "    ALT := FLOOR;"
        "  END;"
        "  IF ALT > CEIL;"
        "    ALT := CEIL;"
        "  END;"
        "  RETURN ALT;"
        "TERM"), 0);
    /* 55000 > 50000, so clamped to 50000 */
    CHEQ(tp_jit(tp_main()), 50000);
    PASS();
#endif
}
TH_REG("avionics", av_altitude)

/* Launch countdown with abort condition.
 * Count from 10 to 0. If we hit the abort value (5),
 * return 0 (aborted). Otherwise return final count.
 * Uses > comparison for abort check since = works in IF context. */
static void av_countdown(void)
{
#ifndef _WIN32
    SKIP("JIT requires Windows");
#else
    CHEQ(tp_run(
        "START LAUNCH;"
        "  ITEM T S 32 = 10;"
        "  ITEM ABRT S 32 = 5;"
        "  WHILE T > 0;"
        "    IF T = ABRT;"
        "      RETURN 0;"
        "    END;"
        "    T := T - 1;"
        "  END;"
        "  RETURN T;"
        "TERM"), 0);
    /* Countdown hits 5, aborts, returns 0 */
    CHEQ(tp_jit(tp_main()), 0);
    PASS();
#endif
}
TH_REG("avionics", av_countdown)

/* Caution panel -- accumulate warning flags via bitwise OR.
 * Each warning sets a different bit. The final word tells
 * maintenance what went wrong, which is usually everything. */
static void av_bitflag(void)
{
#ifndef _WIN32
    SKIP("JIT requires Windows");
#else
    CHEQ(tp_run(
        "START CAUTION;"
        "  ITEM FLAGS B 32 = 0;"
        "  ITEM ENG'FIRE B 32 = 1;"
        "  ITEM HYD'LOW  B 32 = 2;"
        "  ITEM OIL'PRESS B 32 = 8;"
        "  FLAGS := FLAGS OR ENG'FIRE;"
        "  FLAGS := FLAGS OR HYD'LOW;"
        "  FLAGS := FLAGS OR OIL'PRESS;"
        "  RETURN FLAGS;"
        "TERM"), 0);
    /* 1 | 2 | 8 = 11 */
    CHEQ(tp_jit(tp_main()), 11);
    PASS();
#endif
}
TH_REG("avionics", av_bitflag)

/* Unit conversion: nautical miles to feet.
 * 1 NM = 6076 feet, give or take a few inches that
 * the navigator is too busy to worry about. */
static void av_convert(void)
{
#ifndef _WIN32
    SKIP("JIT requires Windows");
#else
    CHEQ(tp_run(
        "START CONVERT;"
        "  ITEM NM S 32 = 3;"
        "  ITEM FT'PER'NM S 32 = 6076;"
        "  RETURN NM * FT'PER'NM;"
        "TERM"), 0);
    CHEQ(tp_jit(tp_main()), 18228);
    PASS();
#endif
}
TH_REG("avionics", av_convert)

/* Bubble sort a 5-element TABLE -- pure J73.
 * Uses temp variable for J+1 index since TABLE expressions
 * need to be precomputed. If the sort works, return 1. */
static void av_bubble(void)
{
#ifndef _WIN32
    SKIP("JIT requires Windows");
#else
    CHEQ(tp_run(
        "START BSORT;"
        "  TABLE A(0:4);"
        "  BEGIN"
        "    ITEM V S 32;"
        "  END"
        "  A(0).V := 5; A(1).V := 3; A(2).V := 1; A(3).V := 4; A(4).V := 2;"
        "  ITEM I S 32;"
        "  ITEM J S 32;"
        "  ITEM K S 32;"
        "  ITEM TMP S 32;"
        "  FOR I := 0 BY 1 WHILE I <= 3;"
        "    FOR J := 0 BY 1 WHILE J <= 3;"
        "      K := J + 1;"
        "      IF A(J).V > A(K).V;"
        "        TMP := A(J).V;"
        "        A(J).V := A(K).V;"
        "        A(K).V := TMP;"
        "      END;"
        "    END;"
        "  END;"
        "  IF A(0).V = 1;"
        "    IF A(4).V = 5;"
        "      RETURN 1;"
        "    END;"
        "  END;"
        "  RETURN 0;"
        "TERM"), 0);
    CHEQ(tp_jit(tp_main()), 1);
    PASS();
#endif
}
TH_REG("avionics", av_bubble)

/* Ken Thompson's "Trusting Trust" test -- compile arithmetic
 * that the compiler MUST get right. If this returns wrong,
 * either the compiler is broken or it's been compromised.
 * We trust it. For now. */
static void av_trust(void)
{
#ifndef _WIN32
    SKIP("JIT requires Windows");
#else
    CHEQ(tp_run(
        "START TRUST;"
        "  ITEM A S 32 = 2;"
        "  ITEM B S 32 = 3;"
        "  ITEM C S 32 = 7;"
        "  RETURN A * B * C;"
        "TERM"), 0);
    CHEQ(tp_jit(tp_main()), 42);
    PASS();
#endif
}
TH_REG("avionics", av_trust)

/* The A-4K Skyhawk -- the flagship test. Multi-PROC avionics
 * program: compute heading, check bounds, apply correction.
 * Named after the aircraft that gave this compiler its name,
 * because every compiler deserves a plane. */
static void av_skyhawk(void)
{
#ifndef _WIN32
    SKIP("JIT requires Windows");
#else
    CHEQ(tp_run(
        "START A4K;"
        "  PROC CLAMP(X, LO, HI) S 32;"
        "  BEGIN"
        "    IF X < LO;"
        "      RETURN LO;"
        "    END;"
        "    IF X > HI;"
        "      RETURN HI;"
        "    END;"
        "    RETURN X;"
        "  END;"
        "  PROC CORRECT(HDG, ERR) S 32;"
        "  BEGIN"
        "    RETURN HDG + ERR;"
        "  END;"
        "  ITEM RAW'HDG S 32 = 350;"
        "  ITEM DRIFT S 32 = 15;"
        "  ITEM ADJ S 32;"
        "  ADJ := CORRECT(RAW'HDG, DRIFT);"
        "  IF ADJ > 360;"
        "    ADJ := ADJ - 360;"
        "  END;"
        "  ADJ := CLAMP(ADJ, 0, 359);"
        "  RETURN ADJ;"
        "TERM"), 0);
    /* 350 + 15 = 365, > 360 so 365-360 = 5, clamped to [0,359] = 5 */
    CHEQ(tp_jit(tp_main()), 5);
    PASS();
#endif
}
TH_REG("avionics", av_skyhawk)

/* STATUS type state machine (SAFE -> ARMED -> LOCKED).
 * The weapons system FSM: three states, two transitions.
 * Getting this wrong has consequences that go beyond a
 * failing test case. */
static void av_state(void)
{
#ifndef _WIN32
    SKIP("JIT requires Windows");
#else
    CHEQ(tp_run(
        "START WEAPONS;"
        "  TYPE STATE STATUS(V(SAFE), V(ARMED), V(LOCKED));"
        "  ITEM S STATE = V(SAFE);"
        "  S := V(ARMED);"
        "  S := V(LOCKED);"
        "  RETURN S;"
        "TERM"), 0);
    /* SAFE=0, ARMED=1, LOCKED=2 */
    CHEQ(tp_jit(tp_main()), 2);
    PASS();
#endif
}
TH_REG("avionics", av_state)
