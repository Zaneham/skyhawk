/* x86_emit.c -- JIR to x86-64 code generation
 * The dumbest possible backend: every value on the stack,
 * no register allocation, no optimisation, no apologies.
 * Like filling out a tax return in triplicate — correct
 * but nobody's idea of a good time. */

#include "x86.h"
#include <stdio.h>
#include <string.h>

/* ---- Register Allocation Map ----
 * Per-instruction phys reg assignment, or -1 for spilled.
 * 64KB static — don't even think about the stack. */

static int8_t g_rmap[JIR_MAX_INST];

/* callee-saved mask and caller-saved mask for current function */
static uint16_t g_csmask;   /* callee-saved regs used */
static uint16_t g_csrmsk;   /* caller-saved regs used */

/* ---- Raw Emission ---- */

static void eb(x86_mod_t *X, uint8_t b)
{
    if (X->codelen < X86_CODE_MAX)
        X->code[X->codelen++] = b;
}

static void eu32(x86_mod_t *X, uint32_t v)
{
    eb(X, (uint8_t)(v));
    eb(X, (uint8_t)(v >> 8));
    eb(X, (uint8_t)(v >> 16));
    eb(X, (uint8_t)(v >> 24));
}

static void ei32(x86_mod_t *X, int32_t v)
{
    eu32(X, (uint32_t)v);
}

static void eu64(x86_mod_t *X, uint64_t v)
{
    eu32(X, (uint32_t)v);
    eu32(X, (uint32_t)(v >> 32));
}

/* ---- REX + ModRM ---- */

/* REX.W prefix for 64-bit operand size.
 * r = reg field (0-15), b = r/m field (0-15). */
static void e_rex_w(x86_mod_t *X, int r, int b)
{
    uint8_t rex = 0x48;
    if (r >= 8) rex |= 0x04;  /* REX.R */
    if (b >= 8) rex |= 0x01;  /* REX.B */
    eb(X, rex);
}

/* ModRM byte. mod=0b11 for reg-reg, mod=0b10 for [reg+disp32]. */
static void e_modrm(x86_mod_t *X, int mod, int reg, int rm)
{
    eb(X, (uint8_t)(((mod & 3) << 6) | ((reg & 7) << 3) | (rm & 7)));
}

/* ---- Stack ↔ Register (the hot path of despair) ---- */

/* MOV r64, [RBP + off]  —  always disp32 because life is short */
static void e_ld_stk(x86_mod_t *X, int reg, int32_t off)
{
    e_rex_w(X, reg, R_RBP);
    eb(X, 0x8B);
    e_modrm(X, 2, reg, R_RBP);  /* mod=10, r/m=RBP → [RBP+disp32] */
    ei32(X, off);
}

/* MOV [RBP + off], r64 */
static void e_st_stk(x86_mod_t *X, int reg, int32_t off)
{
    e_rex_w(X, reg, R_RBP);
    eb(X, 0x89);
    e_modrm(X, 2, reg, R_RBP);
    ei32(X, off);
}

/* LEA r64, [RBP + off]  —  address of stack slot, not its value */
static void e_lea_stk(x86_mod_t *X, int reg, int32_t off)
{
    e_rex_w(X, reg, R_RBP);
    eb(X, 0x8D);
    e_modrm(X, 2, reg, R_RBP);
    ei32(X, off);
}

/* MOVSD xmm, [RBP + off]  —  F2 REX.W? 0F 10 /r */
static void e_fld_stk(x86_mod_t *X, int xmm, int32_t off)
{
    eb(X, 0xF2);
    /* REX only needed if xmm>=8 or RBP needs REX.B (it doesn't) */
    if (xmm >= 8) eb(X, (uint8_t)(0x44));
    eb(X, 0x0F); eb(X, 0x10);
    e_modrm(X, 2, xmm, R_RBP);
    ei32(X, off);
}

/* MOVSD [RBP + off], xmm  —  F2 0F 11 /r */
static void e_fst_stk(x86_mod_t *X, int xmm, int32_t off)
{
    eb(X, 0xF2);
    if (xmm >= 8) eb(X, (uint8_t)(0x44));
    eb(X, 0x0F); eb(X, 0x11);
    e_modrm(X, 2, xmm, R_RBP);
    ei32(X, off);
}

/* ---- Register ↔ Register ALU ---- */

/* ADD dst, src  (64-bit) */
static void e_add_rr(x86_mod_t *X, int d, int s)
{
    e_rex_w(X, s, d);
    eb(X, 0x01);
    e_modrm(X, 3, s, d);
}

/* SUB dst, src */
static void e_sub_rr(x86_mod_t *X, int d, int s)
{
    e_rex_w(X, s, d);
    eb(X, 0x29);
    e_modrm(X, 3, s, d);
}

/* IMUL dst, src */
static void e_imul_rr(x86_mod_t *X, int d, int s)
{
    e_rex_w(X, d, s);
    eb(X, 0x0F); eb(X, 0xAF);
    e_modrm(X, 3, d, s);
}

/* AND dst, src */
static void e_and_rr(x86_mod_t *X, int d, int s)
{
    e_rex_w(X, s, d);
    eb(X, 0x21);
    e_modrm(X, 3, s, d);
}

/* OR dst, src */
static void e_or_rr(x86_mod_t *X, int d, int s)
{
    e_rex_w(X, s, d);
    eb(X, 0x09);
    e_modrm(X, 3, s, d);
}

/* XOR dst, src */
static void e_xor_rr(x86_mod_t *X, int d, int s)
{
    e_rex_w(X, s, d);
    eb(X, 0x31);
    e_modrm(X, 3, s, d);
}

/* CMP dst, src */
static void e_cmp_rr(x86_mod_t *X, int d, int s)
{
    e_rex_w(X, s, d);
    eb(X, 0x39);
    e_modrm(X, 3, s, d);
}

/* TEST r, r */
static void e_test_rr(x86_mod_t *X, int r, int s)
{
    e_rex_w(X, s, r);
    eb(X, 0x85);
    e_modrm(X, 3, s, r);
}

/* NEG r */
static void e_neg_r(x86_mod_t *X, int r)
{
    e_rex_w(X, 0, r);
    eb(X, 0xF7);
    e_modrm(X, 3, 3, r);  /* /3 = NEG */
}

/* NOT r */
static void e_not_r(x86_mod_t *X, int r)
{
    e_rex_w(X, 0, r);
    eb(X, 0xF7);
    e_modrm(X, 3, 2, r);  /* /2 = NOT */
}

/* SHL r, CL */
static void e_shl_cl(x86_mod_t *X, int r)
{
    e_rex_w(X, 0, r);
    eb(X, 0xD3);
    e_modrm(X, 3, 4, r);  /* /4 = SHL */
}

/* SHR r, CL (logical) */
static void e_shr_cl(x86_mod_t *X, int r)
{
    e_rex_w(X, 0, r);
    eb(X, 0xD3);
    e_modrm(X, 3, 5, r);  /* /5 = SHR */
}

/* SAR r, CL (arithmetic) */
static void e_sar_cl(x86_mod_t *X, int r)
{
    e_rex_w(X, 0, r);
    eb(X, 0xD3);
    e_modrm(X, 3, 7, r);  /* /7 = SAR */
}

/* MOV r64, r64 — elides if d==s, because even the CPU
 * finds self-referential moves philosophically troubling */
static void e_mov_rr(x86_mod_t *X, int d, int s)
{
    if (d == s) return;
    e_rex_w(X, d, s);
    eb(X, 0x8B);
    e_modrm(X, 3, d, s);
}

/* MOVSD xmm, xmm — F2 0F 10 /r. Elides if d==s. */
static void e_msd_rr(x86_mod_t *X, int d, int s)
{
    if (d == s) return;
    eb(X, 0xF2);
    if (d >= 8 || s >= 8) {
        uint8_t rex = 0x40;
        if (d >= 8) rex |= 0x04;
        if (s >= 8) rex |= 0x01;
        eb(X, rex);
    }
    eb(X, 0x0F); eb(X, 0x10);
    e_modrm(X, 3, d, s);
}

/* CQO — sign-extend RAX into RDX:RAX */
static void e_cqo(x86_mod_t *X)
{
    eb(X, 0x48); eb(X, 0x99);
}

/* IDIV r64 — RDX:RAX / r → RAX=quot, RDX=rem */
static void e_idiv_r(x86_mod_t *X, int r)
{
    e_rex_w(X, 0, r);
    eb(X, 0xF7);
    e_modrm(X, 3, 7, r);  /* /7 = IDIV */
}

/* SETcc r8 (byte register, assumes low 8 of GPR) */
static void e_setcc(x86_mod_t *X, int cc, int r)
{
    if (r >= 8) eb(X, (uint8_t)(0x41));  /* REX.B */
    else eb(X, 0x40);  /* REX prefix for uniform byte reg access */
    eb(X, 0x0F);
    eb(X, (uint8_t)(0x90 + cc));
    e_modrm(X, 3, 0, r);
}

/* MOVZX r64, r8 */
static void e_movzx8(x86_mod_t *X, int d, int s)
{
    e_rex_w(X, d, s);
    eb(X, 0x0F); eb(X, 0xB6);
    e_modrm(X, 3, d, s);
}

/* ---- Immediate loads ---- */

/* MOV r64, imm64 (REX.W + B8+rd) — the full monty, 10 bytes */
static void e_mov_ri(x86_mod_t *X, int r, uint64_t imm)
{
    e_rex_w(X, 0, r);
    eb(X, (uint8_t)(0xB8 + (r & 7)));
    eu64(X, imm);
}

/* MOV r64, imm32 sign-extended (REX.W C7 /0) — 7 bytes */
static void e_mov_ri32(x86_mod_t *X, int r, int32_t imm)
{
    e_rex_w(X, 0, r);
    eb(X, 0xC7);
    e_modrm(X, 3, 0, r);
    ei32(X, imm);
}

/* ---- Control Flow ---- */

/* JMP rel32 — writes placeholder, returns offset of rel32 field */
static uint32_t e_jmp32(x86_mod_t *X)
{
    eb(X, 0xE9);
    uint32_t off = X->codelen;
    ei32(X, 0);
    return off;
}

/* Jcc rel32 — 0F 8x rel32. Returns offset of rel32 field */
static uint32_t e_jcc32(x86_mod_t *X, int cc)
{
    eb(X, 0x0F);
    eb(X, (uint8_t)(0x80 + cc));
    uint32_t off = X->codelen;
    ei32(X, 0);
    return off;
}

/* CALL rel32 — returns offset of rel32 field */
static uint32_t e_call32(x86_mod_t *X)
{
    eb(X, 0xE8);
    uint32_t off = X->codelen;
    ei32(X, 0);
    return off;
}

/* RET */
static void e_ret(x86_mod_t *X)
{
    eb(X, 0xC3);
}

/* PUSH r64 */
static void e_push(x86_mod_t *X, int r)
{
    if (r >= 8) eb(X, (uint8_t)(0x41));
    eb(X, (uint8_t)(0x50 + (r & 7)));
}

/* POP r64 */
static void e_pop(x86_mod_t *X, int r)
{
    if (r >= 8) eb(X, (uint8_t)(0x41));
    eb(X, (uint8_t)(0x58 + (r & 7)));
}

/* SUB RSP, imm32 */
static void e_sub_rsp(x86_mod_t *X, int32_t v)
{
    e_rex_w(X, 0, R_RSP);
    eb(X, 0x81);
    e_modrm(X, 3, 5, R_RSP);  /* /5 = SUB */
    ei32(X, v);
}

/* ADD RSP, imm32 */
static void e_add_rsp(x86_mod_t *X, int32_t v)
{
    e_rex_w(X, 0, R_RSP);
    eb(X, 0x81);
    e_modrm(X, 3, 0, R_RSP);  /* /0 = ADD */
    ei32(X, v);
}

/* MOV RBP, RSP */
static void e_mov_bp_sp(x86_mod_t *X)
{
    e_rex_w(X, R_RBP, R_RSP);
    eb(X, 0x8B);
    e_modrm(X, 3, R_RBP, R_RSP);
}

/* MOV RSP, RBP */
static void e_mov_sp_bp(x86_mod_t *X)
{
    e_rex_w(X, R_RSP, R_RBP);
    eb(X, 0x8B);
    e_modrm(X, 3, R_RSP, R_RBP);
}

/* ---- SSE (F64) operations ---- */

/* generic SSE F2 0F op xmm, xmm */
static void e_sse_rr(x86_mod_t *X, uint8_t op, int d, int s)
{
    eb(X, 0xF2);
    /* need REX if either xmm >= 8 (rare but safe) */
    if (d >= 8 || s >= 8) {
        uint8_t rex = 0x40;
        if (d >= 8) rex |= 0x04;
        if (s >= 8) rex |= 0x01;
        eb(X, rex);
    }
    eb(X, 0x0F); eb(X, op);
    e_modrm(X, 3, d, s);
}

static void e_addsd(x86_mod_t *X, int d, int s)  { e_sse_rr(X, 0x58, d, s); }
static void e_subsd(x86_mod_t *X, int d, int s)  { e_sse_rr(X, 0x5C, d, s); }
static void e_mulsd(x86_mod_t *X, int d, int s)  { e_sse_rr(X, 0x59, d, s); }
static void e_divsd(x86_mod_t *X, int d, int s)  { e_sse_rr(X, 0x5E, d, s); }

/* UCOMISD xmm, xmm — 66 0F 2E /r */
static void e_ucomisd(x86_mod_t *X, int d, int s)
{
    eb(X, 0x66);
    if (d >= 8 || s >= 8) {
        uint8_t rex = 0x40;
        if (d >= 8) rex |= 0x04;
        if (s >= 8) rex |= 0x01;
        eb(X, rex);
    }
    eb(X, 0x0F); eb(X, 0x2E);
    e_modrm(X, 3, d, s);
}

/* CVTSI2SD xmm, r64 — F2 REX.W 0F 2A /r */
static void e_cvtsi2sd(x86_mod_t *X, int xmm, int gpr)
{
    eb(X, 0xF2);
    e_rex_w(X, xmm, gpr);
    eb(X, 0x0F); eb(X, 0x2A);
    e_modrm(X, 3, xmm, gpr);
}

/* CVTTSD2SI r64, xmm — F2 REX.W 0F 2C /r */
static void e_cvttsd2si(x86_mod_t *X, int gpr, int xmm)
{
    eb(X, 0xF2);
    e_rex_w(X, gpr, xmm);
    eb(X, 0x0F); eb(X, 0x2C);
    e_modrm(X, 3, gpr, xmm);
}

/* XORPD xmm, xmm — 66 0F 57 /r (zero a register) */
static void e_xorpd(x86_mod_t *X, int d, int s)
{
    eb(X, 0x66);
    if (d >= 8 || s >= 8) {
        uint8_t rex = 0x40;
        if (d >= 8) rex |= 0x04;
        if (s >= 8) rex |= 0x01;
        eb(X, rex);
    }
    eb(X, 0x0F); eb(X, 0x57);
    e_modrm(X, 3, d, s);
}

/* ---- .rdata Helpers ---- */

/* Add a NUL-terminated string to .rdata, return its offset.
 * Dedup by content — no point storing "hello" twice when
 * the linker's already hoarding bytes like a librarian. */
static uint32_t add_rd(x86_mod_t *X, const char *s, int len)
{
    uint32_t need = (uint32_t)len + 1;
    /* dedup */
    for (uint32_t i = 0; i + need <= X->rdlen; i++) {
        if (memcmp(X->rdata + i, s, (size_t)len) == 0 &&
            X->rdata[i + (uint32_t)len] == 0)
            return i;
    }
    if (X->rdlen + need > X86_RDATA_MAX) return 0;
    uint32_t off = X->rdlen;
    memcpy(X->rdata + off, s, (size_t)len);
    X->rdata[off + (uint32_t)len] = 0;
    X->rdlen += need;
    return off;
}

/* LEA reg, [RIP + disp32] — loads address of .rdata string.
 * The disp32 is a placeholder; the linker resolves it via
 * IMAGE_REL_AMD64_REL32 against the .rdata section symbol. */
static void e_lea_rd(x86_mod_t *X, int reg, uint32_t rd_off)
{
    e_rex_w(X, reg, 0);
    eb(X, 0x8D);
    e_modrm(X, 0, reg, 5); /* mod=00, r/m=101 = [RIP+disp32] */
    uint32_t fixpos = X->codelen;
    ei32(X, 0); /* placeholder — linker fills with S+A-(P+4) */
    /* record rdata fixup */
    if (X->n_rdfx < X86_RDFX_MAX) {
        X->rdfx[X->n_rdfx].off    = fixpos;
        X->rdfx[X->n_rdfx].rd_off = rd_off;
        X->n_rdfx++;
    }
}

/* ---- Operand Resolution ---- */

/* Is this JIR type a float? */
static int is_flt(const x86_mod_t *X, uint32_t type)
{
    const sema_ctx_t *S = X->J->S;
    if (type == 0 || type >= (uint32_t)S->n_types) return 0;
    return S->types[type].kind == JT_FLOAT;
}

/* Is this JIR type unsigned? */
static int is_uns(const x86_mod_t *X, uint32_t type)
{
    const sema_ctx_t *S = X->J->S;
    if (type == 0 || type >= (uint32_t)S->n_types) return 0;
    return S->types[type].kind == JT_UNSIGN ||
           S->types[type].kind == JT_BIT;
}

/* Slot offset for JIR instruction */
static int32_t slotof(const x86_mod_t *X, uint32_t ii)
{
    if (ii >= JIR_MAX_INST) return -8;
    return X->slots[ii];
}

/* Load JIR operand into GPR */
static void ld_val(x86_mod_t *X, int reg, uint32_t op)
{
    if (JIR_IS_C(op)) {
        uint32_t ci = JIR_C_IDX(op);
        if (ci < X->J->n_consts) {
            int64_t v = X->J->consts[ci].iv;
            if (v >= -2147483647LL - 1 && v <= 2147483647LL)
                e_mov_ri32(X, reg, (int32_t)v);
            else
                e_mov_ri(X, reg, (uint64_t)v);
        } else {
            e_mov_ri32(X, reg, 0);
        }
    } else if (op < JIR_MAX_INST && g_rmap[op] >= 0) {
        e_mov_rr(X, reg, g_rmap[op]);
    } else {
        e_ld_stk(X, reg, slotof(X, op));
    }
}

/* Load JIR operand into XMM via scratch slot.
 * Float constants: mov ieee bits to RAX, store to scratch, movsd. */
static void ld_fval(x86_mod_t *X, int xmm, uint32_t op, int32_t scratch)
{
    if (JIR_IS_C(op)) {
        uint32_t ci = JIR_C_IDX(op);
        int64_t bits = 0;
        if (ci < X->J->n_consts)
            bits = X->J->consts[ci].iv;
        e_mov_ri(X, R_RAX, (uint64_t)bits);
        e_st_stk(X, R_RAX, scratch);
        e_fld_stk(X, xmm, scratch);
    } else if (op < JIR_MAX_INST && g_rmap[op] >= 0) {
        e_msd_rr(X, xmm, g_rmap[op]);
    } else {
        e_fld_stk(X, xmm, slotof(X, op));
    }
}

/* Store GPR to JIR instruction's slot */
static void st_val(x86_mod_t *X, uint32_t ii, int reg)
{
    if (ii < JIR_MAX_INST && g_rmap[ii] >= 0)
        e_mov_rr(X, g_rmap[ii], reg);
    else
        e_st_stk(X, reg, slotof(X, ii));
}

/* Store XMM to JIR instruction's slot */
static void st_fval(x86_mod_t *X, uint32_t ii, int xmm)
{
    if (ii < JIR_MAX_INST && g_rmap[ii] >= 0)
        e_msd_rr(X, g_rmap[ii], xmm);
    else
        e_fst_stk(X, xmm, slotof(X, ii));
}

/* ---- Predicate → CC mapping ---- */

static int pred_cc(int pred)
{
    switch (pred) {
    case JP_EQ: return CC_E;
    case JP_NE: return CC_NE;
    case JP_LT: return CC_L;
    case JP_LE: return CC_LE;
    case JP_GT: return CC_G;
    case JP_GE: return CC_GE;
    default:    return CC_E;
    }
}

/* Float comparisons use unsigned CCs (UCOMISD sets CF/ZF) */
static int fpred_cc(int pred)
{
    switch (pred) {
    case JP_EQ: return CC_E;
    case JP_NE: return CC_NE;
    case JP_LT: return CC_B;
    case JP_LE: return CC_BE;
    case JP_GT: return CC_A;
    case JP_GE: return CC_AE;
    default:    return CC_E;
    }
}

/* ---- PHI Predecessor Tracking ----
 * Built per-function so the backend knows which block came
 * from where, like a genealogy chart for control flow. */

#define X86_MAXP 8

static uint16_t xp_nprd[JIR_MAX_BLKS];
static uint16_t xp_pred[JIR_MAX_BLKS][X86_MAXP];

static void xp_cfg(const jir_mod_t *J, uint32_t fb, uint32_t nb)
{
    for (uint32_t i = fb; i < fb + nb; i++)
        xp_nprd[i] = 0;

    for (uint32_t bi = fb; bi < fb + nb && bi < J->n_blks; bi++) {
        const jir_blk_t *b = &J->blks[bi];
        if (b->n_inst == 0) continue;
        const jir_inst_t *I = &J->insts[b->first + b->n_inst - 1];

        if (I->op == JIR_BR) {
            uint32_t t = I->ops[0];
            if (t >= fb && t < fb + nb && xp_nprd[t] < X86_MAXP)
                xp_pred[t][xp_nprd[t]++] = (uint16_t)bi;
        } else if (I->op == JIR_BR_COND) {
            uint32_t tt = I->ops[1], tf = I->ops[2];
            if (tt >= fb && tt < fb + nb && xp_nprd[tt] < X86_MAXP)
                xp_pred[tt][xp_nprd[tt]++] = (uint16_t)bi;
            if (tf >= fb && tf < fb + nb && xp_nprd[tf] < X86_MAXP)
                xp_pred[tf][xp_nprd[tf]++] = (uint16_t)bi;
        }
    }
}

/* does block 'bi' start with PHI instructions? */
static int has_phi(const jir_mod_t *J, uint32_t bi)
{
    if (bi >= J->n_blks) return 0;
    const jir_blk_t *b = &J->blks[bi];
    if (b->n_inst == 0) return 0;
    return J->insts[b->first].op == JIR_PHI;
}

/* find predecessor index of 'from' in block 'to' */
static int fnd_pid(uint32_t from, uint32_t to)
{
    for (int p = 0; p < xp_nprd[to]; p++) {
        if (xp_pred[to][p] == (uint16_t)from)
            return p;
    }
    return -1;
}

/* emit PHI parallel copies: for each PHI in 'to', copy the
 * operand at pred_idx(from) into the PHI's stack slot.
 * Sequential copies — swap case deferred until someone
 * actually writes a program gnarly enough to trigger it. */
static void phi_cpy(x86_mod_t *X, uint32_t from, uint32_t to,
                    int32_t scratch)
{
    if (!has_phi(X->J, to)) return;
    int pidx = fnd_pid(from, to);
    if (pidx < 0) return;

    const jir_blk_t *b = &X->J->blks[to];
    for (uint32_t ii = b->first;
         ii < b->first + b->n_inst && ii < X->J->n_inst; ii++) {
        const jir_inst_t *I = &X->J->insts[ii];
        if (I->op != JIR_PHI) break;

        uint32_t src_op = (pidx < 4) ? I->ops[pidx] : 0;

        if (is_flt(X, I->type)) {
            ld_fval(X, R_XMM0, src_op, scratch);
            st_fval(X, ii, R_XMM0);
        } else {
            ld_val(X, R_RAX, src_op);
            st_val(X, ii, R_RAX);
        }
    }
}

/* ---- Prologue / Epilogue ---- */

static void em_prol(x86_mod_t *X, int32_t frmsz)
{
    e_push(X, R_RBP);
    e_mov_bp_sp(X);
    if (frmsz > 0)
        e_sub_rsp(X, frmsz);
}

/* callee-saved GPR pool indices (in push order):
 * RBX=2, RSI=3, RDI=4, R12=5, R13=6, R14=7, R15=8 */
static const int8_t CS_GPRS[] = {
    R_RBX, R_RSI, R_RDI, R_R12, R_R13, R_R14, R_R15
};
#define N_CS_GPRS 7

static void em_epil(x86_mod_t *X)
{
    e_mov_sp_bp(X);
    e_pop(X, R_RBP);
    /* pop callee-saved in reverse push order */
    for (int i = N_CS_GPRS - 1; i >= 0; i--) {
        int bit = 2 + i;  /* pool indices 2-8 */
        if (g_csmask & (uint16_t)(1u << bit))
            e_pop(X, CS_GPRS[i]);
    }
    e_ret(X);
}

/* ---- TABLE Helpers ----
 * Extracting table geometry from a maze of AST nodes,
 * type pools, and definition arrays. Like archaeology
 * but with more pointer arithmetic. */

/* row size for dumbest-backend: 8 bytes per field, no packing */
static int tbl_row(const x86_mod_t *X, uint32_t ty)
{
    const sema_ctx_t *S = X->J->S;
    if (ty == 0 || ty >= (uint32_t)S->n_types) return 8;
    int nf = (int)S->types[ty].n_extra;
    return (nf < 1 ? 1 : nf) * 8;
}

/* number of entries: hi - lo + 1 from tbldef */
static int tbl_cnt(const x86_mod_t *X, uint32_t ty)
{
    const sema_ctx_t *S = X->J->S;
    if (ty == 0 || ty >= (uint32_t)S->n_types) return 1;
    uint32_t tdi = S->types[ty].extra;
    if (tdi >= (uint32_t)S->n_tbldf) return 1;
    int n = S->tbldef[tdi].hi_dim - S->tbldef[tdi].lo_dim + 1;
    return n < 1 ? 1 : n;
}

/* lower dimension bound from tbldef */
static int64_t tbl_lo(const x86_mod_t *X, uint32_t ty)
{
    const sema_ctx_t *S = X->J->S;
    if (ty == 0 || ty >= (uint32_t)S->n_types) return 0;
    uint32_t tdi = S->types[ty].extra;
    if (tdi >= (uint32_t)S->n_tbldf) return 0;
    return S->tbldef[tdi].lo_dim;
}

/* ---- Caller-saved save/restore around CALL ----
 * GPR only (R10, R11) via PUSH/POP. XMM caller-save
 * deferred to Phase 12 — no test currently needs floats
 * surviving across a procedure call, and we're not here
 * to solve problems that haven't happened yet. */

static void sv_call(x86_mod_t *X)
{
    int n = 0;
    /* R10 = pool idx 0, R11 = pool idx 1 */
    if (g_csrmsk & (uint16_t)(1u << 0)) { e_push(X, R_R10); n++; }
    if (g_csrmsk & (uint16_t)(1u << 1)) { e_push(X, R_R11); n++; }
    /* alignment pad: odd pushes → SUB RSP, 8.
     * NOT push RAX — rs_call runs after CALL returns,
     * and RAX holds the return value. Popping into RAX
     * would be like stamping "VOID" on your own pay cheque. */
    if (n & 1) e_sub_rsp(X, 8);
}

static void rs_call(x86_mod_t *X)
{
    int n = 0;
    if (g_csrmsk & (uint16_t)(1u << 0)) n++;
    if (g_csrmsk & (uint16_t)(1u << 1)) n++;
    /* reverse order: alignment pad first, then regs */
    if (n & 1) e_add_rsp(X, 8);
    if (g_csrmsk & (uint16_t)(1u << 1)) e_pop(X, R_R11);
    if (g_csrmsk & (uint16_t)(1u << 0)) e_pop(X, R_R10);
}

/* ---- Per-Instruction Emission ---- */

static void em_inst(x86_mod_t *X, uint32_t ii, int32_t scratch,
                    uint32_t cur_blk)
{
    const jir_inst_t *I = &X->J->insts[ii];
    uint16_t op = I->op;

    switch (op) {

    /* ---- Integer Arithmetic ---- */

    case JIR_ADD:
        ld_val(X, R_RAX, I->ops[0]);
        ld_val(X, R_RCX, I->ops[1]);
        e_add_rr(X, R_RAX, R_RCX);
        st_val(X, ii, R_RAX);
        break;

    case JIR_SUB:
        ld_val(X, R_RAX, I->ops[0]);
        ld_val(X, R_RCX, I->ops[1]);
        e_sub_rr(X, R_RAX, R_RCX);
        st_val(X, ii, R_RAX);
        break;

    case JIR_MUL:
        ld_val(X, R_RAX, I->ops[0]);
        ld_val(X, R_RCX, I->ops[1]);
        e_imul_rr(X, R_RAX, R_RCX);
        st_val(X, ii, R_RAX);
        break;

    case JIR_DIV:
        ld_val(X, R_RAX, I->ops[0]);
        e_cqo(X);
        ld_val(X, R_RCX, I->ops[1]);
        e_idiv_r(X, R_RCX);
        st_val(X, ii, R_RAX);  /* quotient */
        break;

    case JIR_MOD:
        ld_val(X, R_RAX, I->ops[0]);
        e_cqo(X);
        ld_val(X, R_RCX, I->ops[1]);
        e_idiv_r(X, R_RCX);
        st_val(X, ii, R_RDX);  /* remainder */
        break;

    case JIR_NEG:
        ld_val(X, R_RAX, I->ops[0]);
        e_neg_r(X, R_RAX);
        st_val(X, ii, R_RAX);
        break;

    /* ---- Bitwise ---- */

    case JIR_AND:
        ld_val(X, R_RAX, I->ops[0]);
        ld_val(X, R_RCX, I->ops[1]);
        e_and_rr(X, R_RAX, R_RCX);
        st_val(X, ii, R_RAX);
        break;

    case JIR_OR:
        ld_val(X, R_RAX, I->ops[0]);
        ld_val(X, R_RCX, I->ops[1]);
        e_or_rr(X, R_RAX, R_RCX);
        st_val(X, ii, R_RAX);
        break;

    case JIR_XOR:
        ld_val(X, R_RAX, I->ops[0]);
        ld_val(X, R_RCX, I->ops[1]);
        e_xor_rr(X, R_RAX, R_RCX);
        st_val(X, ii, R_RAX);
        break;

    case JIR_NOT:
        ld_val(X, R_RAX, I->ops[0]);
        e_not_r(X, R_RAX);
        st_val(X, ii, R_RAX);
        break;

    case JIR_SHL:
        ld_val(X, R_RAX, I->ops[0]);
        ld_val(X, R_RCX, I->ops[1]);
        e_shl_cl(X, R_RAX);
        st_val(X, ii, R_RAX);
        break;

    case JIR_SHR:
        ld_val(X, R_RAX, I->ops[0]);
        ld_val(X, R_RCX, I->ops[1]);
        if (is_uns(X, I->type))
            e_shr_cl(X, R_RAX);
        else
            e_sar_cl(X, R_RAX);
        st_val(X, ii, R_RAX);
        break;

    /* ---- Integer Compare ---- */

    case JIR_ICMP:
        ld_val(X, R_RAX, I->ops[0]);
        ld_val(X, R_RCX, I->ops[1]);
        e_cmp_rr(X, R_RAX, R_RCX);
        e_setcc(X, pred_cc(I->subop), R_RAX);
        e_movzx8(X, R_RAX, R_RAX);
        st_val(X, ii, R_RAX);
        break;

    /* ---- Float Arithmetic ---- */

    case JIR_FADD:
        ld_fval(X, R_XMM0, I->ops[0], scratch);
        ld_fval(X, R_XMM1, I->ops[1], scratch);
        e_addsd(X, R_XMM0, R_XMM1);
        st_fval(X, ii, R_XMM0);
        break;

    case JIR_FSUB:
        ld_fval(X, R_XMM0, I->ops[0], scratch);
        ld_fval(X, R_XMM1, I->ops[1], scratch);
        e_subsd(X, R_XMM0, R_XMM1);
        st_fval(X, ii, R_XMM0);
        break;

    case JIR_FMUL:
        ld_fval(X, R_XMM0, I->ops[0], scratch);
        ld_fval(X, R_XMM1, I->ops[1], scratch);
        e_mulsd(X, R_XMM0, R_XMM1);
        st_fval(X, ii, R_XMM0);
        break;

    case JIR_FDIV:
        ld_fval(X, R_XMM0, I->ops[0], scratch);
        ld_fval(X, R_XMM1, I->ops[1], scratch);
        e_divsd(X, R_XMM0, R_XMM1);
        st_fval(X, ii, R_XMM0);
        break;

    case JIR_FNEG:
        /* 0.0 - x */
        e_xorpd(X, R_XMM1, R_XMM1);
        ld_fval(X, R_XMM0, I->ops[0], scratch);
        e_subsd(X, R_XMM1, R_XMM0);
        st_fval(X, ii, R_XMM1);
        break;

    /* ---- Float Compare ---- */

    case JIR_FCMP:
        ld_fval(X, R_XMM0, I->ops[0], scratch);
        ld_fval(X, R_XMM1, I->ops[1], scratch);
        e_ucomisd(X, R_XMM0, R_XMM1);
        e_setcc(X, fpred_cc(I->subop), R_RAX);
        e_movzx8(X, R_RAX, R_RAX);
        st_val(X, ii, R_RAX);
        break;

    /* ---- Memory ---- */

    case JIR_ALLOCA:
        /* no code — slot already assigned during frame setup */
        break;

    case JIR_LOAD: {
        /* if operand is an ALLOCA, load directly from its slot */
        uint32_t addr = I->ops[0];
        if (!JIR_IS_C(addr) && addr < X->J->n_inst &&
            X->J->insts[addr].op == JIR_ALLOCA) {
            if (is_flt(X, I->type)) {
                e_fld_stk(X, R_XMM0, slotof(X, addr));
                st_fval(X, ii, R_XMM0);
            } else {
                e_ld_stk(X, R_RAX, slotof(X, addr));
                st_val(X, ii, R_RAX);
            }
        } else {
            /* general pointer load */
            ld_val(X, R_RAX, addr);
            /* MOV RAX, [RAX] */
            e_rex_w(X, R_RAX, R_RAX);
            eb(X, 0x8B);
            e_modrm(X, 0, R_RAX, R_RAX);
            st_val(X, ii, R_RAX);
        }
        break;
    }

    case JIR_STORE: {
        /* ops[0] = value, ops[1] = address */
        uint32_t val  = I->ops[0];
        uint32_t addr = I->ops[1];
        if (!JIR_IS_C(addr) && addr < X->J->n_inst &&
            X->J->insts[addr].op == JIR_ALLOCA) {
            /* store directly into alloca slot */
            uint32_t aty = X->J->insts[addr].type;
            if (is_flt(X, aty)) {
                ld_fval(X, R_XMM0, val, scratch);
                e_fst_stk(X, R_XMM0, slotof(X, addr));
            } else {
                ld_val(X, R_RAX, val);
                e_st_stk(X, R_RAX, slotof(X, addr));
            }
        } else {
            /* general pointer store */
            ld_val(X, R_RCX, addr);
            ld_val(X, R_RAX, val);
            /* MOV [RCX], RAX */
            e_rex_w(X, R_RAX, R_RCX);
            eb(X, 0x89);
            e_modrm(X, 0, R_RAX, R_RCX);
        }
        break;
    }

    case JIR_GEP: {
        /* Phase 6: TABLE address arithmetic.
         * 8-bytes-per-field layout because this backend already
         * puts everything on the stack like luggage at a carousel
         * that only handles oversized items. */
        uint32_t base = I->ops[0];
        int is_alloc = !JIR_IS_C(base) && base < X->J->n_inst &&
                       X->J->insts[base].op == JIR_ALLOCA;

        if (I->n_ops >= 2) {
            /* INDEX GEP: base + (index - lo) * row_size */
            uint32_t tty = I->type; /* TABLE type */
            int row = tbl_row(X, tty);
            int64_t lo = tbl_lo(X, tty);

            if (is_alloc)
                e_lea_stk(X, R_RAX, slotof(X, base));
            else
                ld_val(X, R_RAX, base);

            ld_val(X, R_RCX, I->ops[1]);
            if (lo != 0) {
                e_mov_ri32(X, R_RDX, (int32_t)lo);
                e_sub_rr(X, R_RCX, R_RDX);
            }
            e_mov_ri32(X, R_RDX, (int32_t)row);
            e_imul_rr(X, R_RCX, R_RDX);
            e_add_rr(X, R_RAX, R_RCX);
        } else {
            /* MEMBER GEP: base + field_idx * 8 */
            if (is_alloc)
                e_lea_stk(X, R_RAX, slotof(X, base));
            else
                ld_val(X, R_RAX, base);

            int32_t foff = (int32_t)I->subop * 8;
            if (foff != 0) {
                e_mov_ri32(X, R_RCX, foff);
                e_add_rr(X, R_RAX, R_RCX);
            }
        }
        st_val(X, ii, R_RAX);
        break;
    }

    /* ---- Control Flow ---- */

    case JIR_BR: {
        uint32_t tgt = I->ops[0];
        phi_cpy(X, cur_blk, tgt, scratch);
        uint32_t off = e_jmp32(X);
        if (X->n_fix < X86_FIX_MAX) {
            X->fix[X->n_fix].off = off;
            X->fix[X->n_fix].blk = tgt;
            X->n_fix++;
        }
        break;
    }

    case JIR_BR_COND: {
        /* ops[0] = cond, ops[1] = true_blk, ops[2] = false_blk */
        uint32_t tb = I->ops[1], fbb = I->ops[2];
        int tp = has_phi(X->J, tb);
        int fp = has_phi(X->J, fbb);

        ld_val(X, R_RAX, I->ops[0]);
        e_test_rr(X, R_RAX, R_RAX);

        if (!tp && !fp) {
            /* neither target has PHIs — original code */
            uint32_t off_t = e_jcc32(X, CC_NE);
            if (X->n_fix < X86_FIX_MAX) {
                X->fix[X->n_fix].off = off_t;
                X->fix[X->n_fix].blk = tb;
                X->n_fix++;
            }
            uint32_t off_f = e_jmp32(X);
            if (X->n_fix < X86_FIX_MAX) {
                X->fix[X->n_fix].off = off_f;
                X->fix[X->n_fix].blk = fbb;
                X->n_fix++;
            }
        } else if (tp && !fp) {
            /* only true has PHIs: JE false; phi_cpy(true); JMP true */
            uint32_t off_f = e_jcc32(X, CC_E);
            if (X->n_fix < X86_FIX_MAX) {
                X->fix[X->n_fix].off = off_f;
                X->fix[X->n_fix].blk = fbb;
                X->n_fix++;
            }
            phi_cpy(X, cur_blk, tb, scratch);
            uint32_t off_t = e_jmp32(X);
            if (X->n_fix < X86_FIX_MAX) {
                X->fix[X->n_fix].off = off_t;
                X->fix[X->n_fix].blk = tb;
                X->n_fix++;
            }
        } else if (!tp && fp) {
            /* only false has PHIs: JNE true; phi_cpy(false); JMP false */
            uint32_t off_t = e_jcc32(X, CC_NE);
            if (X->n_fix < X86_FIX_MAX) {
                X->fix[X->n_fix].off = off_t;
                X->fix[X->n_fix].blk = tb;
                X->n_fix++;
            }
            phi_cpy(X, cur_blk, fbb, scratch);
            uint32_t off_f = e_jmp32(X);
            if (X->n_fix < X86_FIX_MAX) {
                X->fix[X->n_fix].off = off_f;
                X->fix[X->n_fix].blk = fbb;
                X->n_fix++;
            }
        } else {
            /* both have PHIs: JE skip; phi_cpy(true); JMP true; skip: phi_cpy(false); JMP false */
            uint32_t skip_off = e_jcc32(X, CC_E);
            phi_cpy(X, cur_blk, tb, scratch);
            uint32_t off_t = e_jmp32(X);
            if (X->n_fix < X86_FIX_MAX) {
                X->fix[X->n_fix].off = off_t;
                X->fix[X->n_fix].blk = tb;
                X->n_fix++;
            }
            /* patch skip JE to here */
            {
                uint32_t here = X->codelen;
                uint32_t src = skip_off + 4;
                int32_t rel = (int32_t)(here - src);
                X->code[skip_off + 0] = (uint8_t)(rel);
                X->code[skip_off + 1] = (uint8_t)(rel >> 8);
                X->code[skip_off + 2] = (uint8_t)(rel >> 16);
                X->code[skip_off + 3] = (uint8_t)(rel >> 24);
            }
            phi_cpy(X, cur_blk, fbb, scratch);
            uint32_t off_f = e_jmp32(X);
            if (X->n_fix < X86_FIX_MAX) {
                X->fix[X->n_fix].off = off_f;
                X->fix[X->n_fix].blk = fbb;
                X->n_fix++;
            }
        }
        break;
    }

    case JIR_RET:
        if (I->n_ops > 0) {
            if (is_flt(X, I->type))
                ld_fval(X, R_XMM0, I->ops[0], scratch);
            else
                ld_val(X, R_RAX, I->ops[0]);
        }
        em_epil(X);
        break;

    case JIR_CALL: {
        /* Win64: first 4 args in RCX, RDX, R8, R9.
         * Shadow space = 32 bytes. Return in RAX. */
        static const int argregs[] = { R_RCX, R_RDX, R_R8, R_R9 };
        uint32_t callee;
        uint32_t arg_base;
        int narg;

        /* save caller-saved allocatable regs */
        sv_call(X);

        if (I->n_ops == 0xFF) {
            /* overflow encoding: ops[0]=extra start, ops[1]=count */
            uint32_t estart = I->ops[0];
            uint32_t ecount = I->ops[1];
            callee = (estart < X->J->n_extra) ?
                     X->J->extra[estart] : JIR_MK_C(0);
            arg_base = estart + 1;
            narg = (int)ecount - 1;
            (void)arg_base; /* args loaded from extra[] */

            /* load up to 4 args from extra pool */
            for (int a = 0; a < narg && a < 4; a++) {
                uint32_t aop = X->J->extra[estart + 1 + (uint32_t)a];
                ld_val(X, argregs[a], aop);
            }
        } else {
            /* inline: ops[0]=callee, ops[1..]=args */
            callee = I->ops[0];
            narg = I->n_ops - 1;

            /* load args into ABI regs */
            for (int a = 0; a < narg && a < 4; a++)
                ld_val(X, argregs[a], I->ops[1 + a]);
        }

        /* shadow space */
        e_sub_rsp(X, 32);

        /* CALL rel32 */
        uint32_t coff = e_call32(X);

        /* resolve callee function index */
        uint32_t fn_idx = 0;
        if (JIR_IS_C(callee))
            fn_idx = JIR_C_IDX(callee);

        if (X->n_cfx < X86_CFX_MAX) {
            X->cfx[X->n_cfx].off = coff;
            X->cfx[X->n_cfx].fn  = fn_idx;
            X->n_cfx++;
        }

        /* reclaim shadow */
        e_add_rsp(X, 32);

        /* restore caller-saved allocatable regs */
        rs_call(X);

        /* store result */
        st_val(X, ii, R_RAX);
        break;
    }

    case JIR_XCALL: {
        /* Win64 external call. Integer args: RCX, RDX, R8, R9.
         * Float args: XMM0-XMM3. Shadow space = 32 bytes.
         * Like JIR_CALL but we can't resolve the address yet —
         * the linker will handle that, assuming it's in a good mood. */
        static const int xareg[] = { R_RCX, R_RDX, R_R8, R_R9 };

        sv_call(X);

        /* ops[0] = xfunc index (const-tagged), ops[1..3] = args */
        uint32_t xfn_ref = I->ops[0];
        int narg = I->n_ops - 1;

        /* load args — type-check for float vs int ABI */
        for (int a = 0; a < narg && a < 4; a++) {
            uint32_t aop = I->ops[1 + a];
            /* check if arg is float-typed. XCALL args can be:
             *   - a JIR inst (check its type)
             *   - a JC_FLT constant */
            int aflt = 0;
            if (JIR_IS_C(aop)) {
                uint32_t ci = JIR_C_IDX(aop);
                if (ci < X->J->n_consts &&
                    X->J->consts[ci].kind == JC_FLT)
                    aflt = 1;
            } else if (aop < X->J->n_inst) {
                aflt = is_flt(X, X->J->insts[aop].type);
            }

            if (aflt) {
                ld_fval(X, a, aop, scratch); /* XMM0-XMM3 */
            } else {
                ld_val(X, xareg[a], aop);
            }
        }

        /* shadow space */
        e_sub_rsp(X, 32);

        /* CALL rel32 — placeholder, linker resolves */
        uint32_t coff = e_call32(X);

        /* record external fixup — deref const pool to get xfunc idx */
        uint32_t xfi = 0;
        if (JIR_IS_C(xfn_ref)) {
            uint32_t ci = JIR_C_IDX(xfn_ref);
            if (ci < X->J->n_consts)
                xfi = (uint32_t)X->J->consts[ci].iv;
        }

        if (X->n_xfx < X86_XFX_MAX) {
            X->xfx[X->n_xfx].off = coff;
            X->xfx[X->n_xfx].xfn = xfi;
            X->n_xfx++;
        }

        /* reclaim shadow */
        e_add_rsp(X, 32);

        rs_call(X);

        /* store return value — check if caller expects float */
        if (is_flt(X, I->type))
            st_fval(X, ii, R_XMM0);
        else
            st_val(X, ii, R_RAX);
        break;
    }

    /* ---- Conversions ---- */

    case JIR_SITOFP:
        ld_val(X, R_RAX, I->ops[0]);
        e_cvtsi2sd(X, R_XMM0, R_RAX);
        st_fval(X, ii, R_XMM0);
        break;

    case JIR_FPTOSI:
        ld_fval(X, R_XMM0, I->ops[0], scratch);
        e_cvttsd2si(X, R_RAX, R_XMM0);
        st_val(X, ii, R_RAX);
        break;

    case JIR_SEXT:
    case JIR_ZEXT:
    case JIR_TRUNC:
        /* all values are 8-byte slots, so these are mostly no-ops */
        ld_val(X, R_RAX, I->ops[0]);
        st_val(X, ii, R_RAX);
        break;

    case JIR_FPEXT:
    case JIR_FPTRUNC:
        /* F32↔F64 — treat everything as F64 for now */
        ld_fval(X, R_XMM0, I->ops[0], scratch);
        st_fval(X, ii, R_XMM0);
        break;

    case JIR_PHI:
    case JIR_NOP:
        /* nothing to emit — PHI resolution is a Phase 7 problem */
        break;

    default:
        break;
    }
}

/* ---- Function Emission ---- */

static void em_func(x86_mod_t *X, uint32_t fi)
{
    const jir_func_t *f = &X->J->funcs[fi];
    uint32_t fb = f->first_blk;
    uint32_t nb = f->n_blks;

    /* ---- Pass 1: assign stack slots ---- */
    int n_slots = 0;
    for (uint32_t bi = fb; bi < fb + nb && bi < X->J->n_blks; bi++) {
        const jir_blk_t *b = &X->J->blks[bi];
        for (uint32_t ii = b->first;
             ii < b->first + b->n_inst && ii < X->J->n_inst; ii++) {
            const jir_inst_t *I = &X->J->insts[ii];
            /* every value-producing instruction gets a slot */
            int has_val = I->op != JIR_STORE && I->op != JIR_BR &&
                          I->op != JIR_BR_COND && I->op != JIR_RET &&
                          I->op != JIR_NOP;
            if (has_val) {
                /* TABLE alloca: allocate row*entries bytes */
                if (I->op == JIR_ALLOCA && I->type > 0 &&
                    I->type < (uint32_t)X->J->S->n_types &&
                    X->J->S->types[I->type].kind == JT_TABLE) {
                    int sz = tbl_row(X, I->type) * tbl_cnt(X, I->type);
                    int ns = (sz + 7) / 8;
                    if (ns < 1) ns = 1;
                    n_slots += ns;
                } else {
                    n_slots++;
                }
                X->slots[ii] = -(n_slots * 8);
            }
        }
    }

    /* +1 slot for float scratch space */
    n_slots++;
    int32_t scratch = -(n_slots * 8);

    /* frame size: round up to 16-byte alignment */
    int32_t frmsz = n_slots * 8;
    if (frmsz & 0xF) frmsz = (frmsz + 15) & ~0xF;

    /* ---- Build predecessor lists for PHI resolution ---- */
    xp_cfg(X->J, fb, nb);

    /* ---- Register allocation ---- */
    memset(g_rmap, -1, sizeof(g_rmap));
    x86_ra(X->J, fi, g_rmap);
    g_csmask = x86_rcs();
    g_csrmsk = x86_rcsr();

    /* ---- Count callee-saved pushes for frame alignment ----
     * Win64: RSP_entry ≡ 8 (mod 16) after CALL pushed return addr.
     * We push n_cs callee-saved + 1 RBP = (n_cs + 1) pushes.
     * RSP after pushes ≡ 8 - (n_cs + 1)*8 (mod 16).
     *   n_cs even → RSP ≡ 0 → need frmsz ≡ 0 (mod 16) [already is]
     *   n_cs odd  → RSP ≡ 8 → need frmsz ≡ 8 (mod 16) [add 8] */
    {
        int n_cs = 0;
        for (int i = 0; i < N_CS_GPRS; i++) {
            int bit = 2 + i;
            if (g_csmask & (uint16_t)(1u << bit)) n_cs++;
        }
        if (n_cs & 1)
            frmsz += 8;
    }

    /* ---- Callee-saved pushes (before PUSH RBP) ---- */
    for (int i = 0; i < N_CS_GPRS; i++) {
        int bit = 2 + i;
        if (g_csmask & (uint16_t)(1u << bit))
            e_push(X, CS_GPRS[i]);
    }

    /* ---- Prologue ---- */
    em_prol(X, frmsz);

    /* ---- Win64: store register args into param alloca slots ----
     * Parameters are the first n_params ALLOCAs in the entry block. */
    {
        static const int argregs[] = { R_RCX, R_RDX, R_R8, R_R9 };
        uint32_t eb_idx = fb;
        if (eb_idx < X->J->n_blks) {
            const jir_blk_t *eb = &X->J->blks[eb_idx];
            int pi = 0;
            for (uint32_t ii = eb->first;
                 ii < eb->first + eb->n_inst && ii < X->J->n_inst &&
                 pi < (int)f->n_params && pi < 4; ii++) {
                if (X->J->insts[ii].op == JIR_ALLOCA) {
                    e_st_stk(X, argregs[pi], slotof(X, ii));
                    pi++;
                }
            }
        }
    }

    /* ---- Block walk ---- */
    for (uint32_t bi = fb; bi < fb + nb && bi < X->J->n_blks; bi++) {
        X->blk_off[bi] = X->codelen;
        const jir_blk_t *b = &X->J->blks[bi];
        for (uint32_t ii = b->first;
             ii < b->first + b->n_inst && ii < X->J->n_inst; ii++) {
            em_inst(X, ii, scratch, bi);
        }
    }
}

/* ---- Fixup Application ---- */

static void fixups(x86_mod_t *X)
{
    /* branch fixups */
    for (int i = 0; i < X->n_fix; i++) {
        uint32_t off = X->fix[i].off;
        uint32_t blk = X->fix[i].blk;
        if (blk >= X->J->n_blks) continue;
        uint32_t tgt = X->blk_off[blk];
        uint32_t src = off + 4;
        int32_t rel = (int32_t)(tgt - src);
        X->code[off + 0] = (uint8_t)(rel);
        X->code[off + 1] = (uint8_t)(rel >> 8);
        X->code[off + 2] = (uint8_t)(rel >> 16);
        X->code[off + 3] = (uint8_t)(rel >> 24);
    }

    /* call fixups */
    for (int i = 0; i < X->n_cfx; i++) {
        uint32_t off = X->cfx[i].off;
        uint32_t fn  = X->cfx[i].fn;
        if (fn >= X->n_funcs) continue;
        uint32_t tgt = X->fn_off[fn];
        uint32_t src = off + 4;
        int32_t rel = (int32_t)(tgt - src);
        X->code[off + 0] = (uint8_t)(rel);
        X->code[off + 1] = (uint8_t)(rel >> 8);
        X->code[off + 2] = (uint8_t)(rel >> 16);
        X->code[off + 3] = (uint8_t)(rel >> 24);
    }
}

/* ---- Public API ---- */

void x86_init(x86_mod_t *X, const jir_mod_t *J)
{
    memset(X, 0, sizeof(*X));
    X->J = J;
}

int x86_emit(x86_mod_t *X)
{
    const jir_mod_t *J = X->J;

    for (uint32_t fi = 0; fi < J->n_funcs; fi++) {
        X->fn_off[fi] = X->codelen;
        em_func(X, fi);
    }
    X->n_funcs = J->n_funcs;

    fixups(X);

    return X->n_errs > 0 ? SK_ERR_CODEGEN : SK_OK;
}
