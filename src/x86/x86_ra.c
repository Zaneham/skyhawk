/* x86_ra.c -- Poletto/Sarkar linear scan register allocator
 * The art of giving values somewhere to live that isn't
 * a draughty stack slot. Like council housing for SSA values,
 * except the eviction policy is actually documented. */

#include "x86.h"
#include <string.h>

/* ---- Limits ---- */

#define RA_MAX_IV   4096
#define RA_MAXP     8       /* max predecessors per block */

/* ---- Interval ---- */

typedef struct {
    uint16_t iidx;      /* JIR inst index */
    uint16_t start;     /* program point of def */
    uint16_t end;       /* program point of last use */
    int8_t   reg;       /* phys reg or -1 = spilled */
    int8_t   cls;       /* 0=GPR, 1=XMM */
} ra_iv_t;

/* ---- Static Pools ---- */

static uint16_t g_pp[JIR_MAX_INST];     /* inst → program point */
static ra_iv_t  g_iv[RA_MAX_IV];
static int      g_niv;
static int      g_act[16];              /* active list: indices into g_iv */
static int      g_nact;
static int8_t   g_gfr[9];              /* free GPR stack */
static int      g_ngfr;
static int8_t   g_xfr[6];              /* free XMM stack */
static int      g_nxfr;
static uint16_t g_csave;               /* bitmask: callee-saved regs used */

/* caller-saved allocatable bitmask (for call save/restore) */
static uint16_t g_csrm;

/* ---- Register Pools ----
 * Allocatable GPR: R10, R11, RBX, RSI, RDI, R12, R13, R14, R15
 * Caller-saved first so they get grabbed before callee-saved.
 * Like choosing the cheap wine before raiding the cellar. */

static const int8_t GPR_POOL[] = {
    R_R10, R_R11, R_RBX, R_RSI, R_RDI, R_R12, R_R13, R_R14, R_R15
};
#define N_GPR_POOL 9

/* Allocatable XMM: XMM2-XMM7 */
static const int8_t XMM_POOL[] = {
    R_XMM2, R_XMM3, R_XMM4, R_XMM5, R_XMM6, R_XMM7
};
#define N_XMM_POOL 6

/* ---- Callee-saved classification ----
 * Win64 callee-saved GPRs in our pool: RBX, RSI, RDI, R12-R15
 * Win64 callee-saved XMMs in our pool: XMM6, XMM7 */

static int is_csav(int8_t reg, int cls)
{
    if (cls == 0) {
        return reg == R_RBX || reg == R_RSI || reg == R_RDI ||
               reg == R_R12 || reg == R_R13 || reg == R_R14 ||
               reg == R_R15;
    }
    /* XMM6, XMM7 callee-saved on Win64 */
    return reg == R_XMM6 || reg == R_XMM7;
}

/* bit index for callee-saved mask: GPR 0-8, XMM 9-14 */
static int csav_bit(int8_t reg, int cls)
{
    if (cls == 0) {
        for (int i = 0; i < N_GPR_POOL; i++)
            if (GPR_POOL[i] == reg) return i;
    } else {
        for (int i = 0; i < N_XMM_POOL; i++)
            if (XMM_POOL[i] == reg) return 9 + i;
    }
    return 0;
}

/* ---- is_flt: duplicate from emit (RA needs it too) ---- */

static int ra_isflt(const jir_mod_t *J, uint32_t type)
{
    const sema_ctx_t *S = J->S;
    if (type == 0 || type >= (uint32_t)S->n_types) return 0;
    return S->types[type].kind == JT_FLOAT;
}

/* ---- cmp_pp: assign program points ---- */

static uint16_t cmp_pp(const jir_mod_t *J, uint32_t fb, uint32_t nb)
{
    uint16_t pp = 1;   /* start at 1 so 0 means "unmapped" */
    for (uint32_t bi = fb; bi < fb + nb && bi < J->n_blks; bi++) {
        const jir_blk_t *b = &J->blks[bi];
        for (uint32_t ii = b->first;
             ii < b->first + b->n_inst && ii < J->n_inst; ii++) {
            g_pp[ii] = pp++;
            if (pp == 0) pp = 0xFFFF;  /* clamp -- shouldn't happen */
        }
    }
    return pp;  /* next unused PP */
}

/* ---- cmp_iv: compute live intervals ---- */

static void cmp_iv(const jir_mod_t *J, uint32_t fb, uint32_t nb,
                   uint16_t *nprd, uint16_t pred[][RA_MAXP],
                   uint16_t max_pp)
{
    g_niv = 0;

    /* map inst index → iv index, -1 = no interval */
    static int16_t imap[JIR_MAX_INST];
    memset(imap, -1, sizeof(imap));

    /* Pass 1: create intervals for value-producing instructions */
    for (uint32_t bi = fb; bi < fb + nb && bi < J->n_blks; bi++) {
        const jir_blk_t *b = &J->blks[bi];
        for (uint32_t ii = b->first;
             ii < b->first + b->n_inst && ii < J->n_inst; ii++) {
            const jir_inst_t *I = &J->insts[ii];

            /* skip non-value-producing + address-taken ALLOCAs.
             * ALLOCA yields a stack address (not a register value),
             * and the emitter never st_val's into it. Giving it a
             * register is like issuing a boarding pass for a bus. */
            if (I->op == JIR_STORE || I->op == JIR_BR ||
                I->op == JIR_BR_COND || I->op == JIR_RET ||
                I->op == JIR_NOP || I->op == JIR_ALLOCA)
                continue;

            if (g_niv >= RA_MAX_IV) return;   /* bail silently */

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

            /* walk inline operands */
            int nops = I->n_ops;
            if (nops == 0xFF) continue;  /* overflow -- skip for now */

            for (int k = 0; k < nops; k++) {
                uint32_t op = I->ops[k];
                if (JIR_IS_C(op)) continue;

                /* branch targets are block indices, not values */
                if (I->op == JIR_BR && k == 0) continue;
                if (I->op == JIR_BR_COND && k >= 1) continue;

                if (op >= JIR_MAX_INST) continue;
                int16_t idx = imap[op];
                if (idx < 0) continue;
                if (use_pp > g_iv[idx].end)
                    g_iv[idx].end = use_pp;
            }

            /* PHI operands: extend to end of predecessor block.
             * Back-edge (pred terminates before def): value is
             * loop-carried, extend to function end.
             * Loop header (any pred has higher block index):
             * extend ALL phi results to function end -- otherwise
             * outer loop counters get clobbered by inner loops. */
            if (I->op == JIR_PHI) {
                int is_lhdr = 0;
                for (int k = 0; k < nops && k < (int)nprd[bi]; k++) {
                    uint32_t pbi = pred[bi][k];
                    if (pbi > bi) is_lhdr = 1;

                    uint32_t op = I->ops[k];
                    if (JIR_IS_C(op)) continue;
                    if (op >= JIR_MAX_INST) continue;
                    int16_t idx = imap[op];
                    if (idx < 0) continue;

                    /* find last inst (terminator) of predecessor */
                    if (pbi >= J->n_blks) continue;
                    const jir_blk_t *pb = &J->blks[pbi];
                    if (pb->n_inst == 0) continue;
                    uint32_t term = pb->first + pb->n_inst - 1;
                    uint16_t tpp = g_pp[term];
                    if (tpp > g_iv[idx].end)
                        g_iv[idx].end = tpp;
                    /* back-edge: pred terminates before value is
                     * defined → value is loop-carried. Reserve the
                     * register from the loop header to function end
                     * so it's not reused in the loop body. */
                    if (tpp < g_iv[idx].start) {
                        g_iv[idx].start = g_pp[b->first];
                        g_iv[idx].end = max_pp;
                    }
                }
                /* loop header: extend PHI result to function end
                 * so the register survives the entire loop body */
                if (is_lhdr) {
                    int16_t pidx = imap[ii];
                    if (pidx >= 0)
                        g_iv[pidx].end = max_pp;
                }
            }

            /* CALL/XCALL overflow operands */
            if ((I->op == JIR_CALL || I->op == JIR_XCALL) &&
                I->n_ops == 0xFF) {
                uint32_t estart = I->ops[0];
                uint32_t ecount = I->ops[1];
                for (uint32_t e = 0; e < ecount && estart + e < J->n_extra; e++) {
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
            /* expired -- return register to free pool */
            if (iv->cls == 0) {
                if (g_ngfr < N_GPR_POOL)
                    g_gfr[g_ngfr++] = iv->reg;
            } else {
                if (g_nxfr < N_XMM_POOL)
                    g_xfr[g_nxfr++] = iv->reg;
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
    if (g_nact >= 16) return;  /* overflow guard */
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

    /* find active interval of same class with latest end */
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
        /* steal victim's register */
        ra_iv_t *v = &g_iv[g_act[victim]];
        cur->reg = v->reg;
        v->reg = -1;  /* spill victim */

        /* remove victim from active */
        for (int i = victim; i < g_nact - 1; i++)
            g_act[i] = g_act[i + 1];
        g_nact--;

        /* insert current into active */
        act_ins(idx);
    }
    /* else: spill current (reg stays -1) */
}

/* ---- ra_scan: the main linear scan loop ---- */

static void ra_scan(void)
{
    /* init free pools -- push in reverse so first pops first */
    g_ngfr = 0;
    for (int i = N_GPR_POOL - 1; i >= 0; i--)
        g_gfr[g_ngfr++] = GPR_POOL[i];

    g_nxfr = 0;
    for (int i = N_XMM_POOL - 1; i >= 0; i--)
        g_xfr[g_nxfr++] = XMM_POOL[i];

    g_nact  = 0;
    g_csave = 0;
    g_csrm  = 0;

    for (int i = 0; i < g_niv; i++) {
        ra_iv_t *iv = &g_iv[i];
        exp_old(iv->start);

        int *nfr;
        int8_t *frs;
        if (iv->cls == 0) {
            nfr = &g_ngfr;
            frs = g_gfr;
        } else {
            nfr = &g_nxfr;
            frs = g_xfr;
        }

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

void x86_ra(const jir_mod_t *J, uint32_t fi, int8_t *rmap)
{
    const jir_func_t *f = &J->funcs[fi];
    uint32_t fb = f->first_blk;
    uint32_t nb = f->n_blks;


    /* build predecessor lists (same as xp_cfg in x86_emit.c) */
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

    /* run pipeline */
    memset(g_pp, 0, sizeof(g_pp));
    g_niv = 0;

    uint16_t mpp = cmp_pp(J, fb, nb);
    if (mpp > 0) mpp--;  /* last used program point */
    cmp_iv(J, fb, nb, nprd, pred, mpp);
    srt_iv();
    ra_scan();

    /* write results into rmap */
    for (int i = 0; i < g_niv; i++) {
        ra_iv_t *iv = &g_iv[i];
        if (iv->reg >= 0)
            rmap[iv->iidx] = iv->reg;
    }
}

uint16_t x86_rcs(void)
{
    return g_csave;
}

uint16_t x86_rcsr(void)
{
    return g_csrm;
}

/* ---- Pool queries (for emit callee-save logic) ---- */

int8_t x86_gpr_pool(int i)
{
    if (i < 0 || i >= N_GPR_POOL) return -1;
    return GPR_POOL[i];
}

int8_t x86_xmm_pool(int i)
{
    if (i < 0 || i >= N_XMM_POOL) return -1;
    return XMM_POOL[i];
}
