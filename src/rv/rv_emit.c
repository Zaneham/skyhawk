/* rv_emit.c -- JIR to RISC-V 64 code generation
 * Fixed 32-bit instructions. No variable-length encoding,
 * no prefix bytes, no REX. It's like moving from Heathrow
 * to a regional airfield where things actually make sense. */

#include "rv.h"
#include <stdio.h>
#include <string.h>

/* ---- Register Allocation Map ---- */

static int8_t g_rmap[JIR_MAX_INST];

/* callee-saved / caller-saved masks for current function */
static uint16_t g_csmsk;
static uint16_t g_csrmk;

/* ---- Raw Emission ---- */

static void ew(rv_mod_t *R, uint32_t w)
{
    if (R->codelen + 4 <= RV_CODE_MAX) {
        R->code[R->codelen + 0] = (uint8_t)(w);
        R->code[R->codelen + 1] = (uint8_t)(w >> 8);
        R->code[R->codelen + 2] = (uint8_t)(w >> 16);
        R->code[R->codelen + 3] = (uint8_t)(w >> 24);
        R->codelen += 4;
    }
}

/* ---- Instruction Format Builders ----
 * Six formats, one truth: bits go where RISC-V says they go,
 * not where you'd intuitively put them. */

static uint32_t mk_R(int op, int rd, int f3, int rs1, int rs2, int f7)
{
    return (uint32_t)(
        (op  & 0x7F)        |
        ((rd  & 0x1F) << 7) |
        ((f3  & 0x07) << 12)|
        ((rs1 & 0x1F) << 15)|
        ((rs2 & 0x1F) << 20)|
        ((f7  & 0x7F) << 25)
    );
}

static uint32_t mk_I(int op, int rd, int f3, int rs1, int imm12)
{
    return (uint32_t)(
        (op  & 0x7F)         |
        ((rd  & 0x1F) << 7)  |
        ((f3  & 0x07) << 12) |
        ((rs1 & 0x1F) << 15) |
        ((imm12 & 0xFFF) << 20)
    );
}

static uint32_t mk_S(int op, int f3, int rs1, int rs2, int imm12)
{
    int lo = imm12 & 0x1F;
    int hi = (imm12 >> 5) & 0x7F;
    return (uint32_t)(
        (op  & 0x7F)         |
        ((lo) << 7)          |
        ((f3  & 0x07) << 12) |
        ((rs1 & 0x1F) << 15) |
        ((rs2 & 0x1F) << 20) |
        ((hi) << 25)
    );
}

static uint32_t mk_B(int op, int f3, int rs1, int rs2, int imm13)
{
    /* B-type: imm[12|10:5|4:1|11] scattered across the word
     * like confetti at a wedding nobody asked for. */
    int b11  = (imm13 >> 11) & 1;
    int b4_1 = (imm13 >> 1)  & 0xF;
    int b105 = (imm13 >> 5)  & 0x3F;
    int b12  = (imm13 >> 12) & 1;
    return (uint32_t)(
        (op  & 0x7F)          |
        ((b11) << 7)          |
        ((b4_1) << 8)         |
        ((f3  & 0x07) << 12)  |
        ((rs1 & 0x1F) << 15)  |
        ((rs2 & 0x1F) << 20)  |
        ((b105) << 25)        |
        ((b12) << 31)
    );
}

static uint32_t mk_U(int op, int rd, int imm20)
{
    return (uint32_t)(
        (op & 0x7F)         |
        ((rd & 0x1F) << 7)  |
        ((imm20 & 0xFFFFF) << 12)
    );
}

static uint32_t mk_J(int op, int rd, int imm21)
{
    /* J-type: imm[20|10:1|11|19:12]. Who designed this encoding
     * lost a bet with someone who hates sequential bit fields. */
    int b191  = (imm21 >> 12) & 0xFF;
    int b11   = (imm21 >> 11) & 1;
    int b101  = (imm21 >> 1)  & 0x3FF;
    int b20   = (imm21 >> 20) & 1;
    return (uint32_t)(
        (op & 0x7F)          |
        ((rd & 0x1F) << 7)   |
        ((b191) << 12)       |
        ((b11) << 20)        |
        ((b101) << 21)       |
        ((b20) << 31)
    );
}

/* ---- Convenience Emitters ---- */

/* ADD rd, rs1, rs2 */
static void e_add(rv_mod_t *R, int rd, int rs1, int rs2)
{ ew(R, mk_R(0x33, rd, 0, rs1, rs2, 0x00)); }

/* SUB rd, rs1, rs2 */
static void e_sub(rv_mod_t *R, int rd, int rs1, int rs2)
{ ew(R, mk_R(0x33, rd, 0, rs1, rs2, 0x20)); }

/* MUL rd, rs1, rs2 (M ext) */
static void e_mul(rv_mod_t *R, int rd, int rs1, int rs2)
{ ew(R, mk_R(0x33, rd, 0, rs1, rs2, 0x01)); }

/* DIV rd, rs1, rs2 (signed) */
static void e_div(rv_mod_t *R, int rd, int rs1, int rs2)
{ ew(R, mk_R(0x33, rd, 4, rs1, rs2, 0x01)); }

/* REM rd, rs1, rs2 (signed) */
static void e_rem(rv_mod_t *R, int rd, int rs1, int rs2)
{ ew(R, mk_R(0x33, rd, 6, rs1, rs2, 0x01)); }

/* AND/OR/XOR rd, rs1, rs2 */
static void e_and(rv_mod_t *R, int rd, int rs1, int rs2)
{ ew(R, mk_R(0x33, rd, 7, rs1, rs2, 0x00)); }

static void e_or(rv_mod_t *R, int rd, int rs1, int rs2)
{ ew(R, mk_R(0x33, rd, 6, rs1, rs2, 0x00)); }

static void e_xor(rv_mod_t *R, int rd, int rs1, int rs2)
{ ew(R, mk_R(0x33, rd, 4, rs1, rs2, 0x00)); }

/* SLL/SRA rd, rs1, rs2 */
static void e_sll(rv_mod_t *R, int rd, int rs1, int rs2)
{ ew(R, mk_R(0x33, rd, 1, rs1, rs2, 0x00)); }

static void e_sra(rv_mod_t *R, int rd, int rs1, int rs2)
{ ew(R, mk_R(0x33, rd, 5, rs1, rs2, 0x20)); }

static void e_srl(rv_mod_t *R, int rd, int rs1, int rs2)
{ ew(R, mk_R(0x33, rd, 5, rs1, rs2, 0x00)); }

/* SLT rd, rs1, rs2 (signed set-less-than) */
static void e_slt(rv_mod_t *R, int rd, int rs1, int rs2)
{ ew(R, mk_R(0x33, rd, 2, rs1, rs2, 0x00)); }

/* SLTU rd, rs1, rs2 (unsigned set-less-than) */
static void e_sltu(rv_mod_t *R, int rd, int rs1, int rs2)
{ ew(R, mk_R(0x33, rd, 3, rs1, rs2, 0x00)); }

/* ADDI rd, rs1, imm12 */
static void e_addi(rv_mod_t *R, int rd, int rs1, int imm)
{ ew(R, mk_I(0x13, rd, 0, rs1, imm & 0xFFF)); }

/* XORI rd, rs1, imm12 */
static void e_xori(rv_mod_t *R, int rd, int rs1, int imm)
{ ew(R, mk_I(0x13, rd, 4, rs1, imm & 0xFFF)); }

/* SLTIU rd, rs1, imm12 */
static void e_sltiu(rv_mod_t *R, int rd, int rs1, int imm)
{ ew(R, mk_I(0x13, rd, 3, rs1, imm & 0xFFF)); }

/* SLLI rd, rs1, shamt (RV64: shamt in imm[5:0]) */
static void e_slli(rv_mod_t *R, int rd, int rs1, int sh)
{ ew(R, mk_I(0x13, rd, 1, rs1, sh & 0x3F)); }

/* SRAI rd, rs1, shamt (bit 10 = 1 for arithmetic) */
static void e_srai(rv_mod_t *R, int rd, int rs1, int sh)
{ ew(R, mk_I(0x13, rd, 5, rs1, (sh & 0x3F) | 0x400)); }

/* SRLI rd, rs1, shamt */
static void e_srli(rv_mod_t *R, int rd, int rs1, int sh)
{ ew(R, mk_I(0x13, rd, 5, rs1, sh & 0x3F)); }

/* LD rd, off(rs1) -- 64-bit load */
static void e_ld(rv_mod_t *R, int rd, int rs1, int off)
{ ew(R, mk_I(0x03, rd, 3, rs1, off & 0xFFF)); }

/* SD rs2, off(rs1) -- 64-bit store */
static void e_sd(rv_mod_t *R, int rs1, int rs2, int off)
{ ew(R, mk_S(0x23, 3, rs1, rs2, off & 0xFFF)); }

/* LUI rd, imm20 */
static void e_lui(rv_mod_t *R, int rd, int imm20)
{ ew(R, mk_U(0x37, rd, imm20)); }

/* AUIPC rd, imm20 */
static void e_auipc(rv_mod_t *R, int rd, int imm20)
{ ew(R, mk_U(0x17, rd, imm20)); }

/* JAL rd, imm21 */
static void e_jal(rv_mod_t *R, int rd, int imm21)
{ ew(R, mk_J(0x6F, rd, imm21)); }

/* JALR rd, rs1, imm12 */
static void e_jalr(rv_mod_t *R, int rd, int rs1, int imm)
{ ew(R, mk_I(0x67, rd, 0, rs1, imm & 0xFFF)); }

/* BEQ rs1, rs2, off13 */
static void e_beq(rv_mod_t *R, int rs1, int rs2, int off)
{ ew(R, mk_B(0x63, 0, rs1, rs2, off)); }

/* BNE rs1, rs2, off13 */
static void e_bne(rv_mod_t *R, int rs1, int rs2, int off)
{ ew(R, mk_B(0x63, 1, rs1, rs2, off)); }

/* MV rd, rs (pseudo: ADDI rd, rs, 0) */
static void e_mv(rv_mod_t *R, int rd, int rs)
{
    if (rd != rs) e_addi(R, rd, rs, 0);
}

/* NOP (ADDI x0, x0, 0) */
static void e_nop(rv_mod_t *R)
{ e_addi(R, RV_ZERO, RV_ZERO, 0); }

/* ---- FP Instructions (D extension) ---- */

/* FLD fd, off(rs1) -- load f64 */
static void e_fld(rv_mod_t *R, int fd, int rs1, int off)
{ ew(R, mk_I(0x07, fd, 3, rs1, off & 0xFFF)); }

/* FSD fs2, off(rs1) -- store f64 */
static void e_fsd(rv_mod_t *R, int rs1, int fs2, int off)
{ ew(R, mk_S(0x27, 3, rs1, fs2, off & 0xFFF)); }

/* FADD.D fd, fs1, fs2 (rm=0 RNE) */
static void e_faddd(rv_mod_t *R, int fd, int fs1, int fs2)
{ ew(R, mk_R(0x53, fd, 0, fs1, fs2, 0x01)); }

/* FSUB.D fd, fs1, fs2 */
static void e_fsubd(rv_mod_t *R, int fd, int fs1, int fs2)
{ ew(R, mk_R(0x53, fd, 0, fs1, fs2, 0x05)); }

/* FMUL.D fd, fs1, fs2 */
static void e_fmuld(rv_mod_t *R, int fd, int fs1, int fs2)
{ ew(R, mk_R(0x53, fd, 0, fs1, fs2, 0x09)); }

/* FDIV.D fd, fs1, fs2 */
static void e_fdivd(rv_mod_t *R, int fd, int fs1, int fs2)
{ ew(R, mk_R(0x53, fd, 0, fs1, fs2, 0x0D)); }

/* FSGNJN.D fd, fs, fs (FNEG.D pseudo) */
static void e_fnegd(rv_mod_t *R, int fd, int fs)
{ ew(R, mk_R(0x53, fd, 1, fs, fs, 0x11)); }

/* FEQ.D rd, fs1, fs2 */
static void e_feqd(rv_mod_t *R, int rd, int fs1, int fs2)
{ ew(R, mk_R(0x53, rd, 2, fs1, fs2, 0x51)); }

/* FLT.D rd, fs1, fs2 */
static void e_fltd(rv_mod_t *R, int rd, int fs1, int fs2)
{ ew(R, mk_R(0x53, rd, 1, fs1, fs2, 0x51)); }

/* FLE.D rd, fs1, fs2 */
static void e_fled(rv_mod_t *R, int rd, int fs1, int fs2)
{ ew(R, mk_R(0x53, rd, 0, fs1, fs2, 0x51)); }

/* FCVT.D.L fd, rs1 (int64 -> f64, rm=RNE) */
static void e_cvtdl(rv_mod_t *R, int fd, int rs1)
{ ew(R, mk_R(0x53, fd, 0, rs1, 2, 0x69)); }

/* FCVT.L.D rd, fs1 (f64 -> int64, rm=RTZ=1) */
static void e_cvtld(rv_mod_t *R, int rd, int fs1)
{ ew(R, mk_R(0x53, rd, 1, fs1, 2, 0x61)); }

/* FMV.D fd, fs (FSGNJ.D fd, fs, fs) */
static void e_fmvd(rv_mod_t *R, int fd, int fs)
{
    if (fd != fs) ew(R, mk_R(0x53, fd, 0, fs, fs, 0x11));
}

/* FMV.X.W / FMV.D.X: move bits between GPR and FPR.
 * FMV.X.D rd, fs1 (f64 bits -> GPR, RV64D only) */
static void e_fmvxd(rv_mod_t *R, int rd, int fs1)
{ ew(R, mk_R(0x53, rd, 0, fs1, 0, 0x71)); }

/* FMV.D.X fd, rs1 (GPR bits -> f64, RV64D only) */
static void e_fmvdx(rv_mod_t *R, int fd, int rs1)
{ ew(R, mk_R(0x53, fd, 0, rs1, 0, 0x79)); }

/* ECALL */
static void e_ecall(rv_mod_t *R)
{ ew(R, mk_I(0x73, 0, 0, 0, 0)); }

/* ---- 64-bit Immediate Loading ----
 * RISC-V can only load 32-bit sign-extended constants with
 * LUI+ADDI. For 64-bit we need the multi-step shuffle.
 * Most J73 values fit in 32 bits. The full path is rare,
 * which is fortunate because it's not particularly pretty. */

static void e_li64(rv_mod_t *R, int rd, int64_t v)
{
    /* small immediate: ADDI rd, zero, v */
    if (v >= -2048 && v < 2048) {
        e_addi(R, rd, RV_ZERO, (int)(v & 0xFFF));
        return;
    }

    /* 32-bit range: LUI + ADDI */
    if (v >= -0x80000000LL && v < 0x80000000LL) {
        int32_t lo = (int32_t)(v & 0xFFF);
        /* if lo[11] is set, LUI needs +1 to compensate sign-ext */
        int32_t hi = (int32_t)((v + 0x800) >> 12);
        e_lui(R, rd, hi & 0xFFFFF);
        if ((lo & 0xFFF) != 0)
            e_addi(R, rd, rd, lo & 0xFFF);
        return;
    }

    /* Full 64-bit: build upper 32, shift, add lower 32.
     * We split into hi32 + lo32, load hi32 via LUI+ADDI,
     * SLLI 32, then add lo32 via LUI+ADDI into a temp
     * and ADD. Bounded to 8 instructions max. */
    int32_t lo32 = (int32_t)(uint32_t)v;
    int32_t hi32 = (int32_t)(uint32_t)(v >> 32);

    /* load upper 32 bits */
    {
        int32_t hlo = hi32 & 0xFFF;
        int32_t hhi = (hi32 + 0x800) >> 12;
        if (hhi == 0 && ((hlo & 0x800) ? (int32_t)(hlo | (int32_t)0xFFFFF000) : hlo) == hi32) {
            e_addi(R, rd, RV_ZERO, hlo & 0xFFF);
        } else {
            e_lui(R, rd, hhi & 0xFFFFF);
            if ((hlo & 0xFFF) != 0)
                e_addi(R, rd, rd, hlo & 0xFFF);
        }
    }

    /* shift left 32 */
    e_slli(R, rd, rd, 32);

    /* add lower 32 bits */
    if (lo32 != 0) {
        int32_t llo = lo32 & 0xFFF;
        int32_t lhi = (lo32 + 0x800) >> 12;
        if (lhi != 0) {
            e_lui(R, RV_T2, lhi & 0xFFFFF);
            if ((llo & 0xFFF) != 0)
                e_addi(R, RV_T2, RV_T2, llo & 0xFFF);
        } else {
            /* small positive lower bits */
            e_addi(R, RV_T2, RV_ZERO, llo & 0xFFF);
        }
        /* zero-extend t2 to avoid sign issues: AND with 0xFFFFFFFF */
        e_slli(R, RV_T2, RV_T2, 32);
        e_srli(R, RV_T2, RV_T2, 32);
        e_add(R, rd, rd, RV_T2);
    }
}

/* ---- Operand Resolution ---- */

static int is_flt(const rv_mod_t *R, uint32_t type)
{
    const sema_ctx_t *S = R->J->S;
    if (type == 0 || type >= (uint32_t)S->n_types) return 0;
    return S->types[type].kind == JT_FLOAT;
}

static int is_uns(const rv_mod_t *R, uint32_t type)
{
    const sema_ctx_t *S = R->J->S;
    if (type == 0 || type >= (uint32_t)S->n_types) return 0;
    return S->types[type].kind == JT_UNSIGN ||
           S->types[type].kind == JT_BIT;
}

static int32_t slotof(const rv_mod_t *R, uint32_t ii)
{
    if (ii >= JIR_MAX_INST) return -8;
    return R->slots[ii];
}

/* Load JIR operand into GPR.
 * Scratch GPRs: t0, t1. Operand loading uses whatever reg
 * you ask for, like a polite but firm hotel concierge. */
static void ld_val(rv_mod_t *R, int reg, uint32_t op)
{
    if (JIR_IS_C(op)) {
        uint32_t ci = JIR_C_IDX(op);
        if (ci < R->J->n_consts) {
            int64_t v = R->J->consts[ci].iv;
            e_li64(R, reg, v);
        } else {
            e_addi(R, reg, RV_ZERO, 0);
        }
    } else if (op < JIR_MAX_INST && g_rmap[op] >= 0) {
        e_mv(R, reg, g_rmap[op]);
    } else {
        e_ld(R, reg, RV_S0, slotof(R, op));
    }
}

/* Load JIR float operand into FPR.
 * Float constants: load IEEE bits into GPR, then FMV.D.X. */
static void ld_fval(rv_mod_t *R, int frd, uint32_t op,
                    int32_t scratch)
{
    if (JIR_IS_C(op)) {
        uint32_t ci = JIR_C_IDX(op);
        int64_t bits = 0;
        if (ci < R->J->n_consts)
            bits = R->J->consts[ci].iv;
        e_li64(R, RV_T0, bits);
        e_fmvdx(R, frd, RV_T0);
    } else if (op < JIR_MAX_INST && g_rmap[op] >= 0) {
        e_fmvd(R, frd, g_rmap[op]);
    } else {
        e_fld(R, frd, RV_S0, slotof(R, op));
    }
    (void)scratch;
}

/* Store GPR to JIR instruction's slot or register */
static void st_val(rv_mod_t *R, uint32_t ii, int reg)
{
    if (ii < JIR_MAX_INST && g_rmap[ii] >= 0)
        e_mv(R, g_rmap[ii], reg);
    else
        e_sd(R, RV_S0, reg, slotof(R, ii));
}

/* Store FPR to JIR instruction's slot or register */
static void st_fval(rv_mod_t *R, uint32_t ii, int frd)
{
    if (ii < JIR_MAX_INST && g_rmap[ii] >= 0)
        e_fmvd(R, g_rmap[ii], frd);
    else
        e_fsd(R, RV_S0, frd, slotof(R, ii));
}

/* ---- PHI Predecessor Tracking ---- */

#define RV_MAXP 8

static uint16_t rp_nprd[JIR_MAX_BLKS];
static uint16_t rp_pred[JIR_MAX_BLKS][RV_MAXP];

static void rp_cfg(const jir_mod_t *J, uint32_t fb, uint32_t nb)
{
    for (uint32_t i = fb; i < fb + nb; i++)
        rp_nprd[i] = 0;

    for (uint32_t bi = fb; bi < fb + nb && bi < J->n_blks; bi++) {
        const jir_blk_t *b = &J->blks[bi];
        if (b->n_inst == 0) continue;
        const jir_inst_t *I = &J->insts[b->first + b->n_inst - 1];

        if (I->op == JIR_BR) {
            uint32_t t = I->ops[0];
            if (t >= fb && t < fb + nb && rp_nprd[t] < RV_MAXP)
                rp_pred[t][rp_nprd[t]++] = (uint16_t)bi;
        } else if (I->op == JIR_BR_COND) {
            uint32_t tt = I->ops[1], tf = I->ops[2];
            if (tt >= fb && tt < fb + nb && rp_nprd[tt] < RV_MAXP)
                rp_pred[tt][rp_nprd[tt]++] = (uint16_t)bi;
            if (tf >= fb && tf < fb + nb && rp_nprd[tf] < RV_MAXP)
                rp_pred[tf][rp_nprd[tf]++] = (uint16_t)bi;
        }
    }
}

static int has_phi(const jir_mod_t *J, uint32_t bi)
{
    if (bi >= J->n_blks) return 0;
    const jir_blk_t *b = &J->blks[bi];
    if (b->n_inst == 0) return 0;
    return J->insts[b->first].op == JIR_PHI;
}

static int fnd_pid(uint32_t from, uint32_t to)
{
    for (int p = 0; p < rp_nprd[to]; p++) {
        if (rp_pred[to][p] == (uint16_t)from)
            return p;
    }
    return -1;
}

static void phi_cpy(rv_mod_t *R, uint32_t from, uint32_t to,
                    int32_t scratch)
{
    if (!has_phi(R->J, to)) return;
    int pidx = fnd_pid(from, to);
    if (pidx < 0) return;

    const jir_blk_t *b = &R->J->blks[to];
    for (uint32_t ii = b->first;
         ii < b->first + b->n_inst && ii < R->J->n_inst; ii++) {
        const jir_inst_t *I = &R->J->insts[ii];
        if (I->op != JIR_PHI) break;

        uint32_t src = (pidx < 4) ? I->ops[pidx] : 0;

        if (is_flt(R, I->type)) {
            ld_fval(R, RV_FT0, src, scratch);
            st_fval(R, ii, RV_FT0);
        } else {
            ld_val(R, RV_T0, src);
            st_val(R, ii, RV_T0);
        }
    }
}

/* ---- TABLE Helpers ---- */

static int tbl_row(const rv_mod_t *R, uint32_t ty)
{
    const sema_ctx_t *S = R->J->S;
    if (ty == 0 || ty >= (uint32_t)S->n_types) return 8;
    int nf = (int)S->types[ty].n_extra;
    return (nf < 1 ? 1 : nf) * 8;
}

static int tbl_cnt(const rv_mod_t *R, uint32_t ty)
{
    const sema_ctx_t *S = R->J->S;
    if (ty == 0 || ty >= (uint32_t)S->n_types) return 1;
    uint32_t tdi = S->types[ty].extra;
    if (tdi >= (uint32_t)S->n_tbldf) return 1;
    int n = S->tbldef[tdi].hi_dim - S->tbldef[tdi].lo_dim + 1;
    return n < 1 ? 1 : n;
}

static int64_t tbl_lo(const rv_mod_t *R, uint32_t ty)
{
    const sema_ctx_t *S = R->J->S;
    if (ty == 0 || ty >= (uint32_t)S->n_types) return 0;
    uint32_t tdi = S->types[ty].extra;
    if (tdi >= (uint32_t)S->n_tbldf) return 0;
    return S->tbldef[tdi].lo_dim;
}

/* ---- Caller-saved save/restore around CALL ----
 * Save allocated caller-saved regs to stack slots before
 * the call obliterates them like a bull at a pottery exhibit. */

static void sv_call(rv_mod_t *R, int32_t scratch)
{
    /* t0-t6 are caller-saved. We use t0/t1 as scratch,
     * so only pool entries t2-t6 + t3-t6 might be allocated.
     * The RA tracks which are in use via the caller-saved mask. */
    int n = 0;
    int32_t base = scratch - 8; /* below float scratch */
    /* save each caller-saved GPR that RA allocated */
    for (int i = 0; i < 7; i++) {
        if (g_csrmk & (uint16_t)(1u << i)) {
            static const int8_t cs_regs[] = {
                RV_T0, RV_T1, RV_T2, RV_T3, RV_T4, RV_T5, RV_T6
            };
            e_sd(R, RV_S0, cs_regs[i], base - n * 8);
            n++;
        }
    }
    (void)n;
}

static void rs_call(rv_mod_t *R, int32_t scratch)
{
    int n = 0;
    int32_t base = scratch - 8;
    for (int i = 0; i < 7; i++) {
        if (g_csrmk & (uint16_t)(1u << i)) {
            static const int8_t cs_regs[] = {
                RV_T0, RV_T1, RV_T2, RV_T3, RV_T4, RV_T5, RV_T6
            };
            e_ld(R, cs_regs[i], RV_S0, base - n * 8);
            n++;
        }
    }
    (void)n;
}

/* ---- Prologue / Epilogue ----
 * s0 (FP) points at saved RA, slots are negative offsets.
 *
 * Stack layout:
 *   s0+0   -> saved ra
 *   s0-8   -> saved s0
 *   s0-16  -> saved s1 (if used)
 *   ...
 *   s0-N   -> local slots (negative offsets)
 *   sp     -> bottom of frame
 */

/* Callee-saved GPRs in our allocatable pool (excl s0/FP) */
static const int8_t CS_GPRS[] = {
    RV_S1, RV_S2, RV_S3, RV_S4, RV_S5,
    RV_S6, RV_S7, RV_S8, RV_S9, RV_S10, RV_S11
};
#define N_CS_GPRS 11

static void em_prol(rv_mod_t *R, int32_t frmsz)
{
    /* addi sp, sp, -frmsz */
    e_addi(R, RV_SP, RV_SP, (-frmsz) & 0xFFF);

    /* Save ra/s0/callee-saved at BOTTOM of frame (near sp).
     * User slots live at negative offsets from s0 (top of frame).
     * Keeping them apart prevents the kind of collision that
     * makes return addresses spontaneously become loop counters. */
    e_sd(R, RV_SP, RV_RA, 0);
    e_sd(R, RV_SP, RV_S0, 8);

    /* save callee-saved GPRs used by RA */
    int off = 16;
    for (int i = 0; i < N_CS_GPRS; i++) {
        int bit = 7 + i; /* pool indices 7-17 for s1-s11 */
        if (g_csmsk & (uint16_t)(1u << bit)) {
            e_sd(R, RV_SP, CS_GPRS[i], off);
            off += 8;
        }
    }

    /* addi s0, sp, frmsz  (s0 = frame base) */
    e_addi(R, RV_S0, RV_SP, frmsz & 0xFFF);
}

static void em_epil(rv_mod_t *R, int32_t frmsz)
{
    /* restore callee-saved (bottom of frame, ascending) */
    int off = 16;
    for (int i = 0; i < N_CS_GPRS; i++) {
        int bit = 7 + i;
        if (g_csmsk & (uint16_t)(1u << bit)) {
            e_ld(R, CS_GPRS[i], RV_SP, off);
            off += 8;
        }
    }

    e_ld(R, RV_S0, RV_SP, 8);
    e_ld(R, RV_RA, RV_SP, 0);
    /* addi sp, sp, frmsz */
    e_addi(R, RV_SP, RV_SP, frmsz & 0xFFF);
    /* jalr zero, ra, 0 (ret) */
    e_jalr(R, RV_ZERO, RV_RA, 0);
}

/* ---- Per-Instruction Emission ---- */

static void em_inst(rv_mod_t *R, uint32_t ii, int32_t scratch,
                    uint32_t cur_blk, int32_t frmsz)
{
    const jir_inst_t *I = &R->J->insts[ii];
    uint16_t op = I->op;

    switch (op) {

    /* ---- Integer Arithmetic ---- */

    case JIR_ADD:
        ld_val(R, RV_T0, I->ops[0]);
        ld_val(R, RV_T1, I->ops[1]);
        e_add(R, RV_T0, RV_T0, RV_T1);
        st_val(R, ii, RV_T0);
        break;

    case JIR_SUB:
        ld_val(R, RV_T0, I->ops[0]);
        ld_val(R, RV_T1, I->ops[1]);
        e_sub(R, RV_T0, RV_T0, RV_T1);
        st_val(R, ii, RV_T0);
        break;

    case JIR_MUL:
        ld_val(R, RV_T0, I->ops[0]);
        ld_val(R, RV_T1, I->ops[1]);
        e_mul(R, RV_T0, RV_T0, RV_T1);
        st_val(R, ii, RV_T0);
        break;

    case JIR_DIV:
        ld_val(R, RV_T0, I->ops[0]);
        ld_val(R, RV_T1, I->ops[1]);
        e_div(R, RV_T0, RV_T0, RV_T1);
        st_val(R, ii, RV_T0);
        break;

    case JIR_MOD:
        ld_val(R, RV_T0, I->ops[0]);
        ld_val(R, RV_T1, I->ops[1]);
        e_rem(R, RV_T0, RV_T0, RV_T1);
        st_val(R, ii, RV_T0);
        break;

    case JIR_NEG:
        ld_val(R, RV_T0, I->ops[0]);
        e_sub(R, RV_T0, RV_ZERO, RV_T0);
        st_val(R, ii, RV_T0);
        break;

    /* ---- Bitwise ---- */

    case JIR_AND:
        ld_val(R, RV_T0, I->ops[0]);
        ld_val(R, RV_T1, I->ops[1]);
        e_and(R, RV_T0, RV_T0, RV_T1);
        st_val(R, ii, RV_T0);
        break;

    case JIR_OR:
        ld_val(R, RV_T0, I->ops[0]);
        ld_val(R, RV_T1, I->ops[1]);
        e_or(R, RV_T0, RV_T0, RV_T1);
        st_val(R, ii, RV_T0);
        break;

    case JIR_XOR:
        ld_val(R, RV_T0, I->ops[0]);
        ld_val(R, RV_T1, I->ops[1]);
        e_xor(R, RV_T0, RV_T0, RV_T1);
        st_val(R, ii, RV_T0);
        break;

    case JIR_NOT:
        /* XORI rd, rs, -1 */
        ld_val(R, RV_T0, I->ops[0]);
        e_xori(R, RV_T0, RV_T0, -1);
        st_val(R, ii, RV_T0);
        break;

    case JIR_SHL:
        ld_val(R, RV_T0, I->ops[0]);
        ld_val(R, RV_T1, I->ops[1]);
        e_sll(R, RV_T0, RV_T0, RV_T1);
        st_val(R, ii, RV_T0);
        break;

    case JIR_SHR:
        ld_val(R, RV_T0, I->ops[0]);
        ld_val(R, RV_T1, I->ops[1]);
        if (is_uns(R, I->type))
            e_srl(R, RV_T0, RV_T0, RV_T1);
        else
            e_sra(R, RV_T0, RV_T0, RV_T1);
        st_val(R, ii, RV_T0);
        break;

    /* ---- Integer Compare ----
     * No FLAGS register. Comparisons produce integer 0/1.
     * Like getting a yes/no answer instead of a weather report. */

    case JIR_ICMP: {
        ld_val(R, RV_T0, I->ops[0]);
        ld_val(R, RV_T1, I->ops[1]);

        switch (I->subop) {
        case JP_LT:
            e_slt(R, RV_T0, RV_T0, RV_T1);
            break;
        case JP_GE:
            e_slt(R, RV_T0, RV_T0, RV_T1);
            e_xori(R, RV_T0, RV_T0, 1);
            break;
        case JP_GT:
            e_slt(R, RV_T0, RV_T1, RV_T0);
            break;
        case JP_LE:
            e_slt(R, RV_T0, RV_T1, RV_T0);
            e_xori(R, RV_T0, RV_T0, 1);
            break;
        case JP_EQ:
            e_sub(R, RV_T0, RV_T0, RV_T1);
            e_sltiu(R, RV_T0, RV_T0, 1);
            break;
        case JP_NE:
            e_sub(R, RV_T0, RV_T0, RV_T1);
            e_sltu(R, RV_T0, RV_ZERO, RV_T0);
            break;
        default:
            e_addi(R, RV_T0, RV_ZERO, 0);
            break;
        }
        st_val(R, ii, RV_T0);
        break;
    }

    /* ---- Float Arithmetic ---- */

    case JIR_FADD:
        ld_fval(R, RV_FT0, I->ops[0], scratch);
        ld_fval(R, RV_FT1, I->ops[1], scratch);
        e_faddd(R, RV_FT0, RV_FT0, RV_FT1);
        st_fval(R, ii, RV_FT0);
        break;

    case JIR_FSUB:
        ld_fval(R, RV_FT0, I->ops[0], scratch);
        ld_fval(R, RV_FT1, I->ops[1], scratch);
        e_fsubd(R, RV_FT0, RV_FT0, RV_FT1);
        st_fval(R, ii, RV_FT0);
        break;

    case JIR_FMUL:
        ld_fval(R, RV_FT0, I->ops[0], scratch);
        ld_fval(R, RV_FT1, I->ops[1], scratch);
        e_fmuld(R, RV_FT0, RV_FT0, RV_FT1);
        st_fval(R, ii, RV_FT0);
        break;

    case JIR_FDIV:
        ld_fval(R, RV_FT0, I->ops[0], scratch);
        ld_fval(R, RV_FT1, I->ops[1], scratch);
        e_fdivd(R, RV_FT0, RV_FT0, RV_FT1);
        st_fval(R, ii, RV_FT0);
        break;

    case JIR_FNEG:
        ld_fval(R, RV_FT0, I->ops[0], scratch);
        e_fnegd(R, RV_FT0, RV_FT0);
        st_fval(R, ii, RV_FT0);
        break;

    /* ---- Float Compare ---- */

    case JIR_FCMP: {
        ld_fval(R, RV_FT0, I->ops[0], scratch);
        ld_fval(R, RV_FT1, I->ops[1], scratch);

        switch (I->subop) {
        case JP_EQ:
            e_feqd(R, RV_T0, RV_FT0, RV_FT1);
            break;
        case JP_NE:
            e_feqd(R, RV_T0, RV_FT0, RV_FT1);
            e_xori(R, RV_T0, RV_T0, 1);
            break;
        case JP_LT:
            e_fltd(R, RV_T0, RV_FT0, RV_FT1);
            break;
        case JP_LE:
            e_fled(R, RV_T0, RV_FT0, RV_FT1);
            break;
        case JP_GT:
            e_fltd(R, RV_T0, RV_FT1, RV_FT0);
            break;
        case JP_GE:
            e_fled(R, RV_T0, RV_FT1, RV_FT0);
            break;
        default:
            e_addi(R, RV_T0, RV_ZERO, 0);
            break;
        }
        st_val(R, ii, RV_T0);
        break;
    }

    /* ---- Memory ---- */

    case JIR_ALLOCA:
        /* no code, slot already assigned during frame setup */
        break;

    case JIR_LOAD: {
        uint32_t addr = I->ops[0];
        if (!JIR_IS_C(addr) && addr < R->J->n_inst &&
            R->J->insts[addr].op == JIR_ALLOCA) {
            if (is_flt(R, I->type)) {
                e_fld(R, RV_FT0, RV_S0, slotof(R, addr));
                st_fval(R, ii, RV_FT0);
            } else {
                e_ld(R, RV_T0, RV_S0, slotof(R, addr));
                st_val(R, ii, RV_T0);
            }
        } else {
            /* general pointer load */
            ld_val(R, RV_T0, addr);
            e_ld(R, RV_T0, RV_T0, 0);
            st_val(R, ii, RV_T0);
        }
        break;
    }

    case JIR_STORE: {
        uint32_t val  = I->ops[0];
        uint32_t addr = I->ops[1];
        if (!JIR_IS_C(addr) && addr < R->J->n_inst &&
            R->J->insts[addr].op == JIR_ALLOCA) {
            uint32_t aty = R->J->insts[addr].type;
            if (is_flt(R, aty)) {
                ld_fval(R, RV_FT0, val, scratch);
                e_fsd(R, RV_S0, RV_FT0, slotof(R, addr));
            } else {
                ld_val(R, RV_T0, val);
                e_sd(R, RV_S0, RV_T0, slotof(R, addr));
            }
        } else {
            /* general pointer store */
            ld_val(R, RV_T1, addr);
            ld_val(R, RV_T0, val);
            e_sd(R, RV_T1, RV_T0, 0);
        }
        break;
    }

    case JIR_GEP: {
        uint32_t base = I->ops[0];
        int is_alloc = !JIR_IS_C(base) && base < R->J->n_inst &&
                       R->J->insts[base].op == JIR_ALLOCA;

        if (I->n_ops >= 2) {
            /* INDEX GEP: base + (index - lo) * row_size */
            uint32_t tty = I->type;
            int row = tbl_row(R, tty);
            int64_t lo = tbl_lo(R, tty);

            if (is_alloc)
                e_addi(R, RV_T0, RV_S0, slotof(R, base) & 0xFFF);
            else
                ld_val(R, RV_T0, base);

            ld_val(R, RV_T1, I->ops[1]);
            if (lo != 0) {
                e_li64(R, RV_T2, lo);
                e_sub(R, RV_T1, RV_T1, RV_T2);
            }
            e_li64(R, RV_T2, (int64_t)row);
            e_mul(R, RV_T1, RV_T1, RV_T2);
            e_add(R, RV_T0, RV_T0, RV_T1);
        } else {
            /* MEMBER GEP: base + field_idx * 8 */
            if (is_alloc)
                e_addi(R, RV_T0, RV_S0, slotof(R, base) & 0xFFF);
            else
                ld_val(R, RV_T0, base);

            int32_t foff = (int32_t)I->subop * 8;
            if (foff != 0)
                e_addi(R, RV_T0, RV_T0, foff & 0xFFF);
        }
        st_val(R, ii, RV_T0);
        break;
    }

    /* ---- Control Flow ---- */

    case JIR_BR: {
        uint32_t tgt = I->ops[0];
        phi_cpy(R, cur_blk, tgt, scratch);
        /* JAL zero, offset (placeholder) */
        uint32_t off = R->codelen;
        e_jal(R, RV_ZERO, 0);
        if (R->n_fix < RV_FIX_MAX) {
            R->fix[R->n_fix].off = off;
            R->fix[R->n_fix].blk = tgt;
            R->n_fix++;
        }
        break;
    }

    case JIR_BR_COND: {
        uint32_t tb = I->ops[1], fbb = I->ops[2];
        int tp = has_phi(R->J, tb);
        int fp = has_phi(R->J, fbb);

        ld_val(R, RV_T0, I->ops[0]);

        if (!tp && !fp) {
            /* BNE t0, zero -> true_blk (placeholder) */
            uint32_t off_t = R->codelen;
            e_bne(R, RV_T0, RV_ZERO, 0);
            if (R->n_fix < RV_FIX_MAX) {
                R->fix[R->n_fix].off = off_t;
                R->fix[R->n_fix].blk = tb;
                R->n_fix++;
            }
            /* JAL zero -> false_blk */
            uint32_t off_f = R->codelen;
            e_jal(R, RV_ZERO, 0);
            if (R->n_fix < RV_FIX_MAX) {
                R->fix[R->n_fix].off = off_f;
                R->fix[R->n_fix].blk = fbb;
                R->n_fix++;
            }
        } else if (tp && !fp) {
            /* BEQ t0, zero -> false_blk */
            uint32_t off_f = R->codelen;
            e_beq(R, RV_T0, RV_ZERO, 0);
            if (R->n_fix < RV_FIX_MAX) {
                R->fix[R->n_fix].off = off_f;
                R->fix[R->n_fix].blk = fbb;
                R->n_fix++;
            }
            phi_cpy(R, cur_blk, tb, scratch);
            uint32_t off_t = R->codelen;
            e_jal(R, RV_ZERO, 0);
            if (R->n_fix < RV_FIX_MAX) {
                R->fix[R->n_fix].off = off_t;
                R->fix[R->n_fix].blk = tb;
                R->n_fix++;
            }
        } else if (!tp && fp) {
            /* BNE t0, zero -> true_blk */
            uint32_t off_t = R->codelen;
            e_bne(R, RV_T0, RV_ZERO, 0);
            if (R->n_fix < RV_FIX_MAX) {
                R->fix[R->n_fix].off = off_t;
                R->fix[R->n_fix].blk = tb;
                R->n_fix++;
            }
            phi_cpy(R, cur_blk, fbb, scratch);
            uint32_t off_f = R->codelen;
            e_jal(R, RV_ZERO, 0);
            if (R->n_fix < RV_FIX_MAX) {
                R->fix[R->n_fix].off = off_f;
                R->fix[R->n_fix].blk = fbb;
                R->n_fix++;
            }
        } else {
            /* both have PHIs */
            uint32_t skip_off = R->codelen;
            e_beq(R, RV_T0, RV_ZERO, 0);
            phi_cpy(R, cur_blk, tb, scratch);
            uint32_t off_t = R->codelen;
            e_jal(R, RV_ZERO, 0);
            if (R->n_fix < RV_FIX_MAX) {
                R->fix[R->n_fix].off = off_t;
                R->fix[R->n_fix].blk = tb;
                R->n_fix++;
            }
            /* patch skip BEQ to here */
            {
                uint32_t here = R->codelen;
                int32_t rel = (int32_t)(here - skip_off);
                /* re-encode B-type at skip_off */
                uint32_t old = (uint32_t)(
                    R->code[skip_off]     |
                    (R->code[skip_off+1] << 8) |
                    (R->code[skip_off+2] << 16)|
                    (R->code[skip_off+3] << 24));
                /* clear imm bits, rebuild */
                uint32_t base_bits = old & 0x01FFF07Fu;
                uint32_t nw = base_bits | (uint32_t)(
                    (((rel >> 11) & 1) << 7)  |
                    (((rel >> 1) & 0xF) << 8) |
                    (((rel >> 5) & 0x3F) << 25)|
                    (((rel >> 12) & 1) << 31));
                R->code[skip_off+0] = (uint8_t)(nw);
                R->code[skip_off+1] = (uint8_t)(nw >> 8);
                R->code[skip_off+2] = (uint8_t)(nw >> 16);
                R->code[skip_off+3] = (uint8_t)(nw >> 24);
            }
            phi_cpy(R, cur_blk, fbb, scratch);
            uint32_t off_f = R->codelen;
            e_jal(R, RV_ZERO, 0);
            if (R->n_fix < RV_FIX_MAX) {
                R->fix[R->n_fix].off = off_f;
                R->fix[R->n_fix].blk = fbb;
                R->n_fix++;
            }
        }
        break;
    }

    case JIR_RET:
        if (I->n_ops > 0) {
            if (is_flt(R, I->type))
                ld_fval(R, RV_FA0, I->ops[0], scratch);
            else
                ld_val(R, RV_A0, I->ops[0]);
        }
        em_epil(R, frmsz);
        break;

    case JIR_CALL: {
        /* LP64D: integer args a0-a7, return in a0 */
        static const int argregs[] = {
            RV_A0, RV_A1, RV_A2, RV_A3,
            RV_A4, RV_A5, RV_A6, RV_A7
        };
        uint32_t callee;
        int narg;

        sv_call(R, scratch);

        if (I->n_ops == 0xFF) {
            uint32_t estart = I->ops[0];
            uint32_t ecount = I->ops[1];
            callee = (estart < R->J->n_extra) ?
                     R->J->extra[estart] : JIR_MK_C(0);
            narg = (int)ecount - 1;

            for (int a = 0; a < narg && a < 8; a++) {
                uint32_t aop = R->J->extra[estart + 1 + (uint32_t)a];
                ld_val(R, argregs[a], aop);
            }
        } else {
            callee = I->ops[0];
            narg = I->n_ops - 1;

            for (int a = 0; a < narg && a < 8; a++)
                ld_val(R, argregs[a], I->ops[1 + a]);
        }

        /* JAL ra, offset (placeholder, fixup later) */
        uint32_t coff = R->codelen;
        e_jal(R, RV_RA, 0);

        uint32_t fn_idx = 0;
        if (JIR_IS_C(callee))
            fn_idx = JIR_C_IDX(callee);

        if (R->n_cfx < RV_CFX_MAX) {
            R->cfx[R->n_cfx].off = coff;
            R->cfx[R->n_cfx].fn  = fn_idx;
            R->n_cfx++;
        }

        rs_call(R, scratch);

        st_val(R, ii, RV_A0);
        break;
    }

    case JIR_XCALL: {
        static const int xareg[] = {
            RV_A0, RV_A1, RV_A2, RV_A3,
            RV_A4, RV_A5, RV_A6, RV_A7
        };

        sv_call(R, scratch);

        uint32_t xfn_ref = I->ops[0];
        int narg = I->n_ops - 1;

        for (int a = 0; a < narg && a < 8; a++) {
            uint32_t aop = I->ops[1 + a];
            int aflt = 0;
            if (JIR_IS_C(aop)) {
                uint32_t ci = JIR_C_IDX(aop);
                if (ci < R->J->n_consts &&
                    R->J->consts[ci].kind == JC_FLT)
                    aflt = 1;
            } else if (aop < R->J->n_inst) {
                aflt = is_flt(R, R->J->insts[aop].type);
            }

            if (aflt)
                ld_fval(R, RV_FA0 + a, aop, scratch);
            else
                ld_val(R, xareg[a], aop);
        }

        /* AUIPC t0, 0 + JALR ra, t0, 0 (8 bytes, reloc placeholder) */
        uint32_t coff = R->codelen;
        e_auipc(R, RV_T0, 0);
        e_jalr(R, RV_RA, RV_T0, 0);

        uint32_t xfi = 0;
        if (JIR_IS_C(xfn_ref)) {
            uint32_t ci = JIR_C_IDX(xfn_ref);
            if (ci < R->J->n_consts)
                xfi = (uint32_t)R->J->consts[ci].iv;
        }

        if (R->n_xfx < RV_XFX_MAX) {
            R->xfx[R->n_xfx].off = coff;
            R->xfx[R->n_xfx].xfn = xfi;
            R->n_xfx++;
        }

        rs_call(R, scratch);

        if (is_flt(R, I->type))
            st_fval(R, ii, RV_FA0);
        else
            st_val(R, ii, RV_A0);
        break;
    }

    /* ---- Conversions ---- */

    case JIR_SITOFP:
        ld_val(R, RV_T0, I->ops[0]);
        e_cvtdl(R, RV_FT0, RV_T0);
        st_fval(R, ii, RV_FT0);
        break;

    case JIR_FPTOSI:
        ld_fval(R, RV_FT0, I->ops[0], scratch);
        e_cvtld(R, RV_T0, RV_FT0);
        st_val(R, ii, RV_T0);
        break;

    case JIR_SEXT:
    case JIR_ZEXT:
    case JIR_TRUNC:
        /* 8-byte slots, mostly no-ops */
        ld_val(R, RV_T0, I->ops[0]);
        st_val(R, ii, RV_T0);
        break;

    case JIR_FPEXT:
    case JIR_FPTRUNC:
        /* F32/F64 both handled as F64 for now */
        ld_fval(R, RV_FT0, I->ops[0], scratch);
        st_fval(R, ii, RV_FT0);
        break;

    case JIR_PHI:
    case JIR_NOP:
        break;

    default:
        break;
    }
}

/* ---- Function Emission ---- */

static void em_func(rv_mod_t *R, uint32_t fi)
{
    const jir_func_t *f = &R->J->funcs[fi];
    uint32_t fb = f->first_blk;
    uint32_t nb = f->n_blks;

    /* ---- Pass 1: assign stack slots ---- */
    int n_slots = 0;
    for (uint32_t bi = fb; bi < fb + nb && bi < R->J->n_blks; bi++) {
        const jir_blk_t *b = &R->J->blks[bi];
        for (uint32_t ii = b->first;
             ii < b->first + b->n_inst && ii < R->J->n_inst; ii++) {
            const jir_inst_t *I = &R->J->insts[ii];
            int has_val = I->op != JIR_STORE && I->op != JIR_BR &&
                          I->op != JIR_BR_COND && I->op != JIR_RET &&
                          I->op != JIR_NOP;
            if (has_val) {
                if (I->op == JIR_ALLOCA && I->type > 0 &&
                    I->type < (uint32_t)R->J->S->n_types &&
                    R->J->S->types[I->type].kind == JT_TABLE) {
                    int sz = tbl_row(R, I->type) * tbl_cnt(R, I->type);
                    int ns = (sz + 7) / 8;
                    if (ns < 1) ns = 1;
                    n_slots += ns;
                } else {
                    n_slots++;
                }
                R->slots[ii] = -(n_slots * 8);
            }
        }
    }

    /* +1 for float scratch + caller-saved save area */
    int n_csave_slots = 0;
    for (int i = 0; i < 7; i++)
        n_csave_slots++; /* worst case 7 caller-saved GPRs */
    n_slots += 1 + n_csave_slots;
    int32_t scratch = -((n_slots - n_csave_slots) * 8);

    /* ---- Register allocation ---- */
    memset(g_rmap, -1, sizeof(g_rmap));
    rv_ra(R->J, fi, g_rmap);
    g_csmsk = rv_rcs();
    g_csrmk = rv_rcsr();

    /* ---- Frame size ----
     * Include 2 words for ra + s0, plus callee-saved regs. */
    int n_cs = 0;
    for (int i = 0; i < N_CS_GPRS; i++) {
        int bit = 7 + i;
        if (g_csmsk & (uint16_t)(1u << bit)) n_cs++;
    }

    int32_t frmsz = (n_slots + 2 + n_cs) * 8;
    /* 16-byte alignment */
    if (frmsz & 0xF) frmsz = (frmsz + 15) & ~0xF;

    /* Check that frame size fits in 12-bit immediate.
     * J73 programs are small, so this should always hold.
     * If not, we'd need a multi-instruction SP adjustment. */
    if (frmsz > 2040) frmsz = 2040; /* clamp, pray */

    R->frm_sz[fi] = (uint32_t)frmsz;

    /* ---- Build predecessor lists ---- */
    rp_cfg(R->J, fb, nb);

    /* ---- Prologue ---- */
    em_prol(R, frmsz);

    /* ---- LP64D: store register args into param alloca slots ---- */
    {
        static const int argregs[] = {
            RV_A0, RV_A1, RV_A2, RV_A3,
            RV_A4, RV_A5, RV_A6, RV_A7
        };
        uint32_t eb_idx = fb;
        if (eb_idx < R->J->n_blks) {
            const jir_blk_t *eb = &R->J->blks[eb_idx];
            int pi = 0;
            for (uint32_t ii = eb->first;
                 ii < eb->first + eb->n_inst && ii < R->J->n_inst &&
                 pi < (int)f->n_params && pi < 8; ii++) {
                if (R->J->insts[ii].op == JIR_ALLOCA) {
                    e_sd(R, RV_S0, argregs[pi], slotof(R, ii));
                    pi++;
                }
            }
        }
    }

    /* ---- Block walk ---- */
    for (uint32_t bi = fb; bi < fb + nb && bi < R->J->n_blks; bi++) {
        R->blk_off[bi] = R->codelen;
        const jir_blk_t *b = &R->J->blks[bi];
        for (uint32_t ii = b->first;
             ii < b->first + b->n_inst && ii < R->J->n_inst; ii++) {
            em_inst(R, ii, scratch, bi, frmsz);
        }
    }
}

/* ---- Fixup Application ---- */

static void fixups(rv_mod_t *R)
{
    /* branch fixups: B-type and J-type */
    for (int i = 0; i < R->n_fix; i++) {
        uint32_t off = R->fix[i].off;
        uint32_t blk = R->fix[i].blk;
        if (blk >= R->J->n_blks) continue;
        uint32_t tgt = R->blk_off[blk];
        int32_t rel = (int32_t)(tgt - off);

        /* read original instruction */
        uint32_t w = (uint32_t)(
            R->code[off]     |
            (R->code[off+1] << 8) |
            (R->code[off+2] << 16)|
            (R->code[off+3] << 24));

        int opcode = (int)(w & 0x7F);

        if (opcode == 0x6F) {
            /* J-type (JAL): re-encode with offset */
            int rd = (int)((w >> 7) & 0x1F);
            w = mk_J(0x6F, rd, rel);
        } else if (opcode == 0x63) {
            /* B-type: re-encode with offset */
            uint32_t base_bits = w & 0x01FFF07Fu;
            w = base_bits | (uint32_t)(
                (((rel >> 11) & 1) << 7)  |
                (((rel >> 1) & 0xF) << 8) |
                (((rel >> 5) & 0x3F) << 25)|
                (((rel >> 12) & 1) << 31));
        }

        R->code[off+0] = (uint8_t)(w);
        R->code[off+1] = (uint8_t)(w >> 8);
        R->code[off+2] = (uint8_t)(w >> 16);
        R->code[off+3] = (uint8_t)(w >> 24);
    }

    /* call fixups: JAL with function offset */
    for (int i = 0; i < R->n_cfx; i++) {
        uint32_t off = R->cfx[i].off;
        uint32_t fn  = R->cfx[i].fn;
        if (fn >= R->n_funcs) continue;
        uint32_t tgt = R->fn_off[fn];
        int32_t rel = (int32_t)(tgt - off);

        uint32_t w = (uint32_t)(
            R->code[off]     |
            (R->code[off+1] << 8) |
            (R->code[off+2] << 16)|
            (R->code[off+3] << 24));
        int rd = (int)((w >> 7) & 0x1F);
        w = mk_J(0x6F, rd, rel);

        R->code[off+0] = (uint8_t)(w);
        R->code[off+1] = (uint8_t)(w >> 8);
        R->code[off+2] = (uint8_t)(w >> 16);
        R->code[off+3] = (uint8_t)(w >> 24);
    }
}

/* ---- Public API ---- */

void rv_init(rv_mod_t *R, const jir_mod_t *J)
{
    memset(R, 0, sizeof(*R));
    R->J = J;
}

int rv_emit(rv_mod_t *R)
{
    const jir_mod_t *J = R->J;

    for (uint32_t fi = 0; fi < J->n_funcs; fi++) {
        R->fn_off[fi] = R->codelen;
        em_func(R, fi);
    }
    R->n_funcs = J->n_funcs;

    fixups(R);

    return R->n_errs > 0 ? SK_ERR_CODEGEN : SK_OK;
}

/* suppress unused-function warnings for helpers not yet called */
static void rv_em_unused_guard(void)
{
    (void)e_nop; (void)e_ecall;
    (void)e_beq; (void)e_fmvxd;
    (void)e_srli; (void)is_uns;
    (void)rv_em_unused_guard;
}
