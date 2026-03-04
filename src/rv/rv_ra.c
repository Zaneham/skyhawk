/* rv_ra.c -- Linear scan register allocator for RV64
 * Same Poletto/Sarkar algorithm as x86_ra.c, just with
 * different GPR/FPR pools. Like moving to a new flat
 * with the same furniture, arranged differently. */

#include "rv.h"
#include <string.h>

/* ---- Limits ---- */

#define RA_MAX_IV   4096
#define RA_MAXP     8

/* ---- Interval ---- */

typedef struct {
    uint16_t iidx;
    uint16_t start;
    uint16_t end;
    int8_t   reg;
    int8_t   cls;       /* 0=GPR, 1=FPR */
} ra_iv_t;

/* ---- Static Pools ---- */

static uint16_t g_pp[JIR_MAX_INST];
static ra_iv_t  g_iv[RA_MAX_IV];
static int      g_niv;
static int      g_act[32];
static int      g_nact;
static int8_t   g_gfr[18];     /* free GPR stack */
static int      g_ngfr;
static int8_t   g_ffr[22];     /* free FPR stack */
static int      g_nffr;
static uint16_t g_csave;       /* callee-saved regs used */
static uint16_t g_csrm;        /* caller-saved regs used */

/* ---- Register Pools ----
 * Caller-saved first, callee-saved second.
 * t0 and t1 are reserved as scratch for ld_val/st_val.
 * s0 reserved as frame pointer. a0-a7 reserved for args/return. */

static const int8_t GPR_POOL[] = {
    RV_T2, RV_T3, RV_T4, RV_T5, RV_T6,           /* 5 caller-saved */
    RV_T0, RV_T1,                                   /* 2 more temps (fragile) */
    RV_S1, RV_S2, RV_S3, RV_S4, RV_S5,             /* 11 callee-saved */
    RV_S6, RV_S7, RV_S8, RV_S9, RV_S10, RV_S11
};
#define N_GPR_POOL 18

/* FPR pool: ft2-ft11 caller-saved, fs0-fs11 callee-saved.
 * ft0-ft1 reserved as float scratch. fa0-fa7 reserved for args. */
static const int8_t FPR_POOL[] = {
    RV_FT2, RV_FT3, RV_FT4, RV_FT5, RV_FT6, RV_FT7,  /* 6 caller-saved */
    RV_FT8, RV_FT9, RV_FT10, RV_FT11,                   /* 4 more temps */
    RV_FS0, RV_FS1, RV_FS2, RV_FS3, RV_FS4, RV_FS5,     /* 12 callee-saved */
    RV_FS6, RV_FS7, RV_FS8, RV_FS9, RV_FS10, RV_FS11
};
#define N_FPR_POOL 22

/* ---- Callee-saved classification ---- */

static int is_csav(int8_t reg, int cls)
{
    if (cls == 0) {
        return reg == RV_S1 || (reg >= RV_S2 && reg <= RV_S11);
    }
    return reg >= RV_FS0 && reg <= RV_FS11;
}

static int csav_bit(int8_t reg, int cls)
{
    if (cls == 0) {
        for (int i = 0; i < N_GPR_POOL; i++)
            if (GPR_POOL[i] == reg) return i;
    } else {
        for (int i = 0; i < N_FPR_POOL; i++)
            if (FPR_POOL[i] == reg) return i;
    }
    return 0;
}

/* ---- Type check ---- */

static int ra_isflt(const jir_mod_t *J, uint32_t type)
{
    const sema_ctx_t *S = J->S;
    if (type == 0 || type >= (uint32_t)S->n_types) return 0;
    return S->types[type].kind == JT_FLOAT;
}

/* ---- cmp_pp: assign program points ---- */

static uint16_t cmp_pp(const jir_mod_t *J, uint32_t fb, uint32_t nb)
{
    uint16_t pp = 1;
    for (uint32_t bi = fb; bi < fb + nb && bi < J->n_blks; bi++) {
        const jir_blk_t *b = &J->blks[bi];
        for (uint32_t ii = b->first;
             ii < b->first + b->n_inst && ii < J->n_inst; ii++) {
            g_pp[ii] = pp++;
            if (pp == 0) pp = 0xFFFF;
        }
    }
    return pp;
}

/* ---- cmp_iv: compute live intervals ---- */

static void cmp_iv(const jir_mod_t *J, uint32_t fb, uint32_t nb,
                   uint16_t *nprd, uint16_t pred[][RA_MAXP])
{
    g_niv = 0;

    static int16_t imap[JIR_MAX_INST];
    memset(imap, -1, sizeof(imap));

    /* Pass 1: create intervals for value-producing instructions */
    for (uint32_t bi = fb; bi < fb + nb && bi < J->n_blks; bi++) {
        const jir_blk_t *b = &J->blks[bi];
        for (uint32_t ii = b->first;
             ii < b->first + b->n_inst && ii < J->n_inst; ii++) {
            const jir_inst_t *I = &J->insts[ii];

            if (I->op == JIR_STORE || I->op == JIR_BR ||
                I->op == JIR_BR_COND || I->op == JIR_RET ||
                I->op == JIR_NOP || I->op == JIR_ALLOCA)
                continue;

            if (g_niv >= RA_MAX_IV) return;

            ra_iv_t *iv = &g_iv[g_niv];
            iv->iidx  = (uint16_t)ii;
            iv->start = g_pp[ii];
            iv->end   = g_pp[ii];
            iv->reg   = -1;
            iv->cls   = ra_isflt(J, I->type) ? 1 : 0;

            imap[ii] = (int16_t)g_niv;
            g_niv++;
        }
    }

    /* Pass 2: extend intervals based on uses */
    for (uint32_t bi = fb; bi < fb + nb && bi < J->n_blks; bi++) {
        const jir_blk_t *b = &J->blks[bi];
        for (uint32_t ii = b->first;
             ii < b->first + b->n_inst && ii < J->n_inst; ii++) {
            const jir_inst_t *I = &J->insts[ii];
            uint16_t use_pp = g_pp[ii];

            int nops = I->n_ops;
            if (nops == 0xFF) continue;

            for (int k = 0; k < nops; k++) {
                uint32_t op = I->ops[k];
                if (JIR_IS_C(op)) continue;
                if (I->op == JIR_BR && k == 0) continue;
                if (I->op == JIR_BR_COND && k >= 1) continue;
                if (op >= JIR_MAX_INST) continue;
                int16_t idx = imap[op];
                if (idx < 0) continue;
                if (use_pp > g_iv[idx].end)
                    g_iv[idx].end = use_pp;
            }

            /* PHI operands: extend to end of predecessor block */
            if (I->op == JIR_PHI) {
                for (int k = 0; k < nops && k < (int)nprd[bi]; k++) {
                    uint32_t op = I->ops[k];
                    if (JIR_IS_C(op)) continue;
                    if (op >= JIR_MAX_INST) continue;
                    int16_t idx = imap[op];
                    if (idx < 0) continue;
                    uint32_t pbi = pred[bi][k];
                    if (pbi >= J->n_blks) continue;
                    const jir_blk_t *pb = &J->blks[pbi];
                    if (pb->n_inst == 0) continue;
                    uint32_t term = pb->first + pb->n_inst - 1;
                    uint16_t tpp = g_pp[term];
                    if (tpp > g_iv[idx].end)
                        g_iv[idx].end = tpp;
                }
            }

            /* CALL/XCALL overflow operands */
            if ((I->op == JIR_CALL || I->op == JIR_XCALL) &&
                I->n_ops == 0xFF) {
                uint32_t estart = I->ops[0];
                uint32_t ecount = I->ops[1];
                for (uint32_t e = 0; e < ecount &&
                     estart + e < J->n_extra; e++) {
                    uint32_t op = J->extra[estart + e];
                    if (JIR_IS_C(op)) continue;
                    if (op >= JIR_MAX_INST) continue;
                    int16_t idx = imap[op];
                    if (idx < 0) continue;
                    if (use_pp > g_iv[idx].end)
                        g_iv[idx].end = use_pp;
                }
            }
        }
    }
}

/* ---- srt_iv: insertion sort by ascending start ---- */

static void srt_iv(void)
{
    for (int i = 1; i < g_niv; i++) {
        ra_iv_t tmp = g_iv[i];
        int j = i - 1;
        for (; j >= 0 && g_iv[j].start > tmp.start; j--)
            g_iv[j + 1] = g_iv[j];
        g_iv[j + 1] = tmp;
    }
}

/* ---- exp_old: expire dead intervals ---- */

static void exp_old(uint16_t point)
{
    int w = 0;
    for (int i = 0; i < g_nact; i++) {
        ra_iv_t *iv = &g_iv[g_act[i]];
        if (iv->end < point) {
            if (iv->cls == 0) {
                if (g_ngfr < N_GPR_POOL)
                    g_gfr[g_ngfr++] = iv->reg;
            } else {
                if (g_nffr < N_FPR_POOL)
                    g_ffr[g_nffr++] = iv->reg;
            }
        } else {
            g_act[w++] = g_act[i];
        }
    }
    g_nact = w;
}

/* ---- act_ins: insert into active list sorted by end ---- */

static void act_ins(int idx)
{
    if (g_nact >= 32) return;
    int pos = g_nact;
    for (int i = g_nact - 1; i >= 0; i--) {
        if (g_iv[g_act[i]].end > g_iv[idx].end) {
            g_act[i + 1] = g_act[i];
            pos = i;
        } else {
            break;
        }
    }
    g_act[pos] = idx;
    g_nact++;
}

/* ---- spl_at: spill at interval with furthest end ---- */

static void spl_at(int idx)
{
    ra_iv_t *cur = &g_iv[idx];
    int victim = -1;
    uint16_t vend = 0;
    for (int i = 0; i < g_nact; i++) {
        ra_iv_t *a = &g_iv[g_act[i]];
        if (a->cls != cur->cls) continue;
        if (a->end > vend) {
            vend = a->end;
            victim = i;
        }
    }

    if (victim >= 0 && vend > cur->end) {
        ra_iv_t *v = &g_iv[g_act[victim]];
        cur->reg = v->reg;
        v->reg = -1;
        for (int i = victim; i < g_nact - 1; i++)
            g_act[i] = g_act[i + 1];
        g_nact--;
        act_ins(idx);
    }
}

/* ---- ra_scan: the main linear scan loop ---- */

static void ra_scan(void)
{
    g_ngfr = 0;
    for (int i = N_GPR_POOL - 1; i >= 0; i--)
        g_gfr[g_ngfr++] = GPR_POOL[i];

    g_nffr = 0;
    for (int i = N_FPR_POOL - 1; i >= 0; i--)
        g_ffr[g_nffr++] = FPR_POOL[i];

    g_nact  = 0;
    g_csave = 0;
    g_csrm  = 0;

    for (int i = 0; i < g_niv; i++) {
        ra_iv_t *iv = &g_iv[i];
        exp_old(iv->start);

        int *nfr;
        int8_t *frs;
        int pool_sz;
        if (iv->cls == 0) {
            nfr = &g_ngfr;
            frs = g_gfr;
            pool_sz = N_GPR_POOL;
        } else {
            nfr = &g_nffr;
            frs = g_ffr;
            pool_sz = N_FPR_POOL;
        }

        (void)pool_sz;

        if (*nfr > 0) {
            iv->reg = frs[--(*nfr)];
            act_ins(i);

            if (is_csav(iv->reg, iv->cls))
                g_csave |= (uint16_t)(1u << csav_bit(iv->reg, iv->cls));
            else
                g_csrm |= (uint16_t)(1u << csav_bit(iv->reg, iv->cls));
        } else {
            spl_at(i);
            if (iv->reg >= 0) {
                if (is_csav(iv->reg, iv->cls))
                    g_csave |= (uint16_t)(1u << csav_bit(iv->reg, iv->cls));
                else
                    g_csrm |= (uint16_t)(1u << csav_bit(iv->reg, iv->cls));
            }
        }
    }
}

/* ---- Public API ---- */

void rv_ra(const jir_mod_t *J, uint32_t fi, int8_t *rmap)
{
    const jir_func_t *f = &J->funcs[fi];
    uint32_t fb = f->first_blk;
    uint32_t nb = f->n_blks;

    static uint16_t nprd[JIR_MAX_BLKS];
    static uint16_t pred[JIR_MAX_BLKS][RA_MAXP];

    for (uint32_t i = fb; i < fb + nb; i++)
        nprd[i] = 0;

    for (uint32_t bi = fb; bi < fb + nb && bi < J->n_blks; bi++) {
        const jir_blk_t *b = &J->blks[bi];
        if (b->n_inst == 0) continue;
        const jir_inst_t *I = &J->insts[b->first + b->n_inst - 1];

        if (I->op == JIR_BR) {
            uint32_t t = I->ops[0];
            if (t >= fb && t < fb + nb && nprd[t] < RA_MAXP)
                pred[t][nprd[t]++] = (uint16_t)bi;
        } else if (I->op == JIR_BR_COND) {
            uint32_t tt = I->ops[1], tf = I->ops[2];
            if (tt >= fb && tt < fb + nb && nprd[tt] < RA_MAXP)
                pred[tt][nprd[tt]++] = (uint16_t)bi;
            if (tf >= fb && tf < fb + nb && nprd[tf] < RA_MAXP)
                pred[tf][nprd[tf]++] = (uint16_t)bi;
        }
    }

    memset(g_pp, 0, sizeof(g_pp));
    g_niv = 0;

    cmp_pp(J, fb, nb);
    cmp_iv(J, fb, nb, nprd, pred);
    srt_iv();
    ra_scan();

    for (int i = 0; i < g_niv; i++) {
        ra_iv_t *iv = &g_iv[i];
        if (iv->reg >= 0)
            rmap[iv->iidx] = iv->reg;
    }
}

uint16_t rv_rcs(void)
{
    return g_csave;
}

uint16_t rv_rcsr(void)
{
    return g_csrm;
}
