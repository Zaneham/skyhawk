/* jir_mem2reg.c -- promote scalar allocas to SSA values
 * Cytron et al. PHI insertion + Cooper/Harvey/Kennedy dominators.
 * Every local was an ALLOCA because the lowerer didn't trust anyone.
 * Now we promote the well-behaved ones to proper SSA values,
 * like releasing prisoners with good conduct records. */

#include "jir.h"
#include <string.h>
#include <stdio.h>

/* ---- Limits ---- */

#define M2R_MAXP    8       /* max predecessors per block */
#define M2R_MAXV    128     /* max promotable variables */
#define M2R_MAXPHI  1024    /* max PHI nodes across function */
#define M2R_STKSZ   256     /* rename stack depth per variable */
#define M2R_DFMAX   (JIR_MAX_BLKS * 8)

/* ---- PHI descriptor ---- */

typedef struct {
    uint32_t blk;               /* block index */
    uint16_t var;               /* promotable variable index */
    uint32_t ops[M2R_MAXP];     /* operand per predecessor */
    uint8_t  n_ops;             /* filled operand count */
    uint32_t dst;               /* assigned inst index after rewrite */
} phi_t;

/* ---- Static pools (file-scope, ~1.5 MB) ---- */

/* CFG predecessors */
static uint16_t g_nprd[JIR_MAX_BLKS];
static uint16_t g_pred[JIR_MAX_BLKS][M2R_MAXP];

/* dominator tree */
static int32_t  g_idom[JIR_MAX_BLKS];

/* dominance frontiers — flat buffer + per-block offset/count */
static uint16_t g_dfbuf[M2R_DFMAX];
static uint16_t g_dfoff[JIR_MAX_BLKS];
static uint16_t g_dfcnt[JIR_MAX_BLKS];

/* promotable variables */
static uint32_t g_pvar[M2R_MAXV];      /* alloca inst index */
static int      g_nvar;

/* PHI nodes */
static phi_t    g_phi[M2R_MAXPHI];
static int      g_nphi;

/* rename stacks */
static uint32_t g_rstk[M2R_MAXV][M2R_STKSZ];
static int      g_rtop[M2R_MAXV];

/* rewrite workspace */
static jir_inst_t g_tmp[JIR_MAX_INST];
static uint32_t g_rmap[JIR_MAX_INST];  /* old idx → new idx */
static uint8_t  g_dead[JIR_MAX_INST];
static uint32_t g_repl[JIR_MAX_INST];  /* dead load → replacement value */

/* reverse postorder */
static uint16_t g_rpo[JIR_MAX_BLKS];
static int      g_nrpo;

/* mk_rpo workspace */
static uint8_t  g_vis[JIR_MAX_BLKS];
static uint16_t g_dstk[JIR_MAX_BLKS];
static uint8_t  g_dph[JIR_MAX_BLKS];

/* ins_phi workspace */
static uint8_t  g_hphi[JIR_MAX_BLKS];
static uint8_t  g_inwl[JIR_MAX_BLKS];
static uint16_t g_wl[JIR_MAX_BLKS];

/* var_of: inst → promo var index (-1 if none) */
static int16_t  g_vof[JIR_MAX_INST];

/* ---- mk_cfg: build predecessor lists ---- */

static void mk_cfg(const jir_mod_t *M, uint32_t fb, uint32_t nb)
{
    for (uint32_t i = fb; i < fb + nb; i++)
        g_nprd[i] = 0;

    for (uint32_t bi = fb; bi < fb + nb && bi < M->n_blks; bi++) {
        const jir_blk_t *b = &M->blks[bi];
        uint32_t last = b->first + b->n_inst;
        if (b->n_inst == 0) continue;
        const jir_inst_t *I = &M->insts[last - 1];

        if (I->op == JIR_BR) {
            uint32_t tgt = I->ops[0];
            if (tgt >= fb && tgt < fb + nb &&
                g_nprd[tgt] < M2R_MAXP)
                g_pred[tgt][g_nprd[tgt]++] = (uint16_t)bi;
        } else if (I->op == JIR_BR_COND) {
            uint32_t tt = I->ops[1];
            uint32_t tf = I->ops[2];
            if (tt >= fb && tt < fb + nb &&
                g_nprd[tt] < M2R_MAXP)
                g_pred[tt][g_nprd[tt]++] = (uint16_t)bi;
            if (tf >= fb && tf < fb + nb &&
                g_nprd[tf] < M2R_MAXP)
                g_pred[tf][g_nprd[tf]++] = (uint16_t)bi;
        }
    }
}

/* ---- Reverse postorder via iterative DFS ---- */

static void mk_rpo(uint32_t fb, uint32_t nb,
                    const jir_mod_t *M)
{
    int sp = 0;

    memset(g_vis, 0, sizeof(uint8_t) * (fb + nb));
    g_nrpo = 0;

    g_dstk[sp] = (uint16_t)fb;
    g_dph[sp] = 0;
    g_vis[fb] = 1;
    sp++;

    while (sp > 0) {
        sp--;
        uint16_t cur = g_dstk[sp];
        uint8_t  ph  = g_dph[sp];

        if (ph == 1) {
            if (g_nrpo < JIR_MAX_BLKS)
                g_rpo[g_nrpo++] = cur;
            continue;
        }

        /* re-push for post-visit */
        g_dph[sp] = 1;
        g_dstk[sp] = cur;
        sp++;

        const jir_blk_t *b = &M->blks[cur];
        if (b->n_inst == 0) continue;
        const jir_inst_t *I = &M->insts[b->first + b->n_inst - 1];

        uint32_t succs[2];
        int nsuc = 0;
        if (I->op == JIR_BR) {
            succs[nsuc++] = I->ops[0];
        } else if (I->op == JIR_BR_COND) {
            succs[nsuc++] = I->ops[1];
            succs[nsuc++] = I->ops[2];
        }

        for (int i = nsuc - 1; i >= 0; i--) {
            uint32_t s = succs[i];
            if (s >= fb && s < fb + nb && !g_vis[s] &&
                sp < JIR_MAX_BLKS) {
                g_vis[s] = 1;
                g_dstk[sp] = (uint16_t)s;
                g_dph[sp] = 0;
                sp++;
            }
        }
    }

    /* reverse to get actual RPO */
    for (int i = 0; i < g_nrpo / 2; i++) {
        uint16_t tmp = g_rpo[i];
        g_rpo[i] = g_rpo[g_nrpo - 1 - i];
        g_rpo[g_nrpo - 1 - i] = tmp;
    }
}

/* ---- intersect (for dominator computation) ---- */

static uint16_t g_rpo_num[JIR_MAX_BLKS]; /* block → RPO index */

static int32_t isect(int32_t a, int32_t b)
{
    for (int g = 0; g < 65536 && a != b; g++) {
        while (a > b && a >= 0) a = g_idom[a];
        while (b > a && b >= 0) b = g_idom[b];
    }
    return a;
}

/* ---- cmp_dom: iterative dominator tree (Cooper/Harvey/Kennedy) ---- */

static void cmp_dom(uint32_t fb, uint32_t nb)
{
    for (uint32_t i = fb; i < fb + nb; i++)
        g_idom[i] = -1;

    /* RPO numbering for intersect ordering */
    for (int i = 0; i < g_nrpo; i++)
        g_rpo_num[g_rpo[i]] = (uint16_t)i;

    g_idom[fb] = (int32_t)fb; /* entry dominates itself */

    for (int iter = 0; iter < 64; iter++) {
        int changed = 0;
        for (int ri = 0; ri < g_nrpo; ri++) {
            uint32_t bi = g_rpo[ri];
            if (bi == fb) continue;

            /* find first processed predecessor */
            int32_t new_idom = -1;
            for (int p = 0; p < g_nprd[bi]; p++) {
                uint32_t pr = g_pred[bi][p];
                if (g_idom[pr] >= 0) {
                    new_idom = (int32_t)pr;
                    break;
                }
            }
            if (new_idom < 0) continue;

            /* intersect with remaining processed preds */
            for (int p = 0; p < g_nprd[bi]; p++) {
                uint32_t pr = g_pred[bi][p];
                if ((int32_t)pr == new_idom) continue;
                if (g_idom[pr] >= 0) {
                    /* use RPO numbers for intersect ordering */
                    int32_t a = (int32_t)g_rpo_num[(uint32_t)new_idom];
                    int32_t b = (int32_t)g_rpo_num[pr];
                    for (int g = 0; g < 65536 && a != b; g++) {
                        while (a > b) {
                            uint32_t bn = g_rpo[(uint32_t)a];
                            a = (int32_t)g_rpo_num[(uint32_t)g_idom[bn]];
                        }
                        while (b > a) {
                            uint32_t bn = g_rpo[(uint32_t)b];
                            b = (int32_t)g_rpo_num[(uint32_t)g_idom[bn]];
                        }
                    }
                    new_idom = (int32_t)g_rpo[(uint32_t)a];
                }
            }

            if (g_idom[bi] != new_idom) {
                g_idom[bi] = new_idom;
                changed = 1;
            }
        }
        if (!changed) break;
    }
}

/* ---- cmp_df: dominance frontiers ---- */

static void cmp_df(uint32_t fb, uint32_t nb)
{
    uint32_t total = 0;
    for (uint32_t i = fb; i < fb + nb; i++) {
        g_dfoff[i] = (uint16_t)total;
        g_dfcnt[i] = 0;
    }

    for (uint32_t bi = fb; bi < fb + nb; bi++) {
        if (g_nprd[bi] < 2) continue;
        for (int p = 0; p < g_nprd[bi]; p++) {
            uint32_t runner = g_pred[bi][p];
            for (int g = 0; g < 4096 && runner != (uint32_t)g_idom[bi] &&
                 runner >= fb; g++) {
                /* add bi to DF[runner] if not already there */
                uint16_t off = g_dfoff[runner];
                uint16_t cnt = g_dfcnt[runner];
                int dup = 0;
                for (int k = 0; k < cnt; k++) {
                    if (g_dfbuf[off + k] == (uint16_t)bi) { dup = 1; break; }
                }
                if (!dup && total < M2R_DFMAX) {
                    /* append to flat buffer — but we need to ensure
                     * each block's entries are contiguous. Since we're
                     * building incrementally, store at end and repack. */
                    g_dfbuf[total] = (uint16_t)bi;
                    if (g_dfcnt[runner] == 0)
                        g_dfoff[runner] = (uint16_t)total;
                    g_dfcnt[runner]++;
                    total++;
                }
                if (runner == (uint32_t)g_idom[runner]) break;
                runner = (uint32_t)g_idom[runner];
            }
        }
    }
}

/* ---- fnd_prm: find promotable allocas ---- */

static void fnd_prm(const jir_mod_t *M, uint32_t fb, uint32_t nb,
                    uint16_t n_params)
{
    g_nvar = 0;

    /* scan for ALLOCAs in function range */
    uint32_t fi = M->blks[fb].first;
    uint32_t li = fi;
    for (uint32_t bi = fb; bi < fb + nb && bi < M->n_blks; bi++)
        li = M->blks[bi].first + M->blks[bi].n_inst;

    for (uint32_t ii = fi; ii < li && ii < M->n_inst; ii++)
        g_vof[ii] = -1;

    /* identify param ALLOCAs: first n_params ALLOCAs in entry block */
    uint8_t is_param[JIR_MAX_LOCAL];
    memset(is_param, 0, sizeof(is_param));
    {
        const jir_blk_t *eb = &M->blks[fb];
        int pcnt = 0;
        for (uint32_t ii = eb->first;
             ii < eb->first + eb->n_inst && ii < M->n_inst; ii++) {
            if (M->insts[ii].op == JIR_ALLOCA) {
                if (pcnt < (int)n_params)
                    is_param[ii - fi] = 1;
                pcnt++;
            }
        }
    }

    for (uint32_t ii = fi; ii < li && ii < M->n_inst; ii++) {
        const jir_inst_t *I = &M->insts[ii];
        if (I->op != JIR_ALLOCA) continue;

        /* skip param ALLOCAs — backend stores args into them */
        if ((ii - fi) < JIR_MAX_LOCAL && is_param[ii - fi])
            continue;

        /* skip TABLE allocas — not scalar */
        if (I->type > 0 && I->type < (uint32_t)M->S->n_types &&
            M->S->types[I->type].kind == JT_TABLE)
            continue;

        /* check all uses: only LOAD(ops[0]) and STORE(ops[1]) allowed */
        int ok = 1;
        for (uint32_t j = fi; j < li && j < M->n_inst; j++) {
            const jir_inst_t *U = &M->insts[j];
            for (int s = 0; s < 4 && s < U->n_ops; s++) {
                if (U->ops[s] == ii && !JIR_IS_C(U->ops[s])) {
                    if (U->op == JIR_LOAD && s == 0) continue;
                    if (U->op == JIR_STORE && s == 1) continue;
                    ok = 0;
                    break;
                }
            }
            if (!ok) break;
        }

        if (ok && g_nvar < M2R_MAXV) {
            int vi = g_nvar++;
            g_pvar[vi] = ii;
            g_vof[ii] = (int16_t)vi;
        }
    }

    /* build reverse map for loads/stores */
    for (uint32_t ii = fi; ii < li && ii < M->n_inst; ii++) {
        const jir_inst_t *I = &M->insts[ii];
        if (I->op == JIR_LOAD && !JIR_IS_C(I->ops[0]) && g_vof[I->ops[0]] >= 0)
            g_vof[ii] = g_vof[I->ops[0]];
        if (I->op == JIR_STORE && !JIR_IS_C(I->ops[1]) && g_vof[I->ops[1]] >= 0)
            g_vof[ii] = g_vof[I->ops[1]];
    }
}

/* ---- blk_of: find which block an instruction belongs to ---- */

static uint32_t blk_of(const jir_mod_t *M, uint32_t ii,
                        uint32_t fb, uint32_t nb)
{
    for (uint32_t bi = fb; bi < fb + nb && bi < M->n_blks; bi++) {
        const jir_blk_t *b = &M->blks[bi];
        if (ii >= b->first && ii < b->first + b->n_inst)
            return bi;
    }
    return fb;
}

/* ---- ins_phi: place PHI nodes via iterated dominance frontier ---- */

static void ins_phi(const jir_mod_t *M, uint32_t fb, uint32_t nb)
{
    g_nphi = 0;
    int wl_n;

    /* first inst, last inst of function */
    uint32_t fi = M->blks[fb].first;
    uint32_t li = fi;
    for (uint32_t bi = fb; bi < fb + nb && bi < M->n_blks; bi++)
        li = M->blks[bi].first + M->blks[bi].n_inst;

    for (int v = 0; v < g_nvar; v++) {
        memset(g_hphi, 0, sizeof(uint8_t) * (fb + nb));
        memset(g_inwl, 0, sizeof(uint8_t) * (fb + nb));
        wl_n = 0;

        /* seed worklist with blocks containing stores to this var */
        for (uint32_t ii = fi; ii < li && ii < M->n_inst; ii++) {
            const jir_inst_t *I = &M->insts[ii];
            if (I->op == JIR_STORE && !JIR_IS_C(I->ops[1]) &&
                g_vof[I->ops[1]] == (int16_t)v) {
                uint32_t b = blk_of(M, ii, fb, nb);
                if (!g_inwl[b]) {
                    g_inwl[b] = 1;
                    g_wl[wl_n++] = (uint16_t)b;
                }
            }
        }

        /* iterated DF */
        for (int wi = 0; wi < wl_n && wi < JIR_MAX_BLKS; wi++) {
            uint32_t b = g_wl[wi];
            for (int d = 0; d < g_dfcnt[b]; d++) {
                uint32_t df = g_dfbuf[g_dfoff[b] + d];
                if (g_hphi[df]) continue;
                if (g_nprd[df] > M2R_MAXP) continue;

                g_hphi[df] = 1;
                if (g_nphi < M2R_MAXPHI) {
                    phi_t *p = &g_phi[g_nphi++];
                    p->blk   = df;
                    p->var   = (uint16_t)v;
                    p->n_ops = (uint8_t)g_nprd[df];
                    p->dst   = 0;
                    for (int k = 0; k < M2R_MAXP; k++)
                        p->ops[k] = 0;
                }

                if (!g_inwl[df]) {
                    g_inwl[df] = 1;
                    if (wl_n < JIR_MAX_BLKS)
                        g_wl[wl_n++] = (uint16_t)df;
                }
            }
        }
    }
}

/* ---- ren_var: rename variables (iterative domtree DFS) ---- */

/* fake IDs for PHI destinations before rewrite assigns real indices */
#define FAKE_BASE JIR_MAX_INST

static uint32_t g_fseq; /* fake ID sequence */

/* domtree children — built on demand */
static uint16_t g_dch[JIR_MAX_BLKS][8]; /* children of each block */
static uint8_t  g_ndch[JIR_MAX_BLKS];

static void bld_dch(uint32_t fb, uint32_t nb)
{
    for (uint32_t i = fb; i < fb + nb; i++)
        g_ndch[i] = 0;

    for (uint32_t bi = fb; bi < fb + nb; bi++) {
        if (bi == fb) continue;
        int32_t par = g_idom[bi];
        if (par >= 0 && par != (int32_t)bi &&
            g_ndch[(uint32_t)par] < 8)
            g_dch[(uint32_t)par][g_ndch[(uint32_t)par]++] = (uint16_t)bi;
    }
}

/* find PHI index for (blk, var), or -1 */
static int fnd_phi(uint32_t blk, int var)
{
    for (int i = 0; i < g_nphi; i++) {
        if (g_phi[i].blk == blk && g_phi[i].var == (uint16_t)var)
            return i;
    }
    return -1;
}

static void ren_var(jir_mod_t *M, uint32_t fb, uint32_t nb)
{
    uint32_t fi = M->blks[fb].first;
    uint32_t li = fi;
    for (uint32_t bi = fb; bi < fb + nb && bi < M->n_blks; bi++)
        li = M->blks[bi].first + M->blks[bi].n_inst;

    /* clear */
    for (int v = 0; v < g_nvar; v++)
        g_rtop[v] = 0;
    memset(g_dead, 0, sizeof(uint8_t) * li);
    for (uint32_t i = fi; i < li; i++)
        g_repl[i] = 0;
    g_fseq = 0;

    /* zero constant for uninitialised reads */
    uint32_t zero_c = 0;
    for (uint32_t i = 0; i < M->n_consts; i++) {
        if (M->consts[i].kind == JC_INT && M->consts[i].iv == 0) {
            zero_c = JIR_MK_C(i);
            break;
        }
    }
    if (!JIR_IS_C(zero_c)) {
        /* create a zero constant */
        if (M->n_consts < JIR_MAX_CONST) {
            uint32_t ci = M->n_consts++;
            M->consts[ci].kind = JC_INT;
            M->consts[ci].iv = 0;
            zero_c = JIR_MK_C(ci);
        }
    }

    /* iterative domtree DFS */
    static struct { uint16_t blk; uint8_t ph; int svtop[M2R_MAXV]; } rstk[JIR_MAX_BLKS];
    int sp = 0;

    rstk[0].blk = (uint16_t)fb;
    rstk[0].ph  = 0;
    sp = 1;

    while (sp > 0) {
        sp--;
        uint16_t bi = rstk[sp].blk;
        uint8_t  ph = rstk[sp].ph;

        if (ph == 1) {
            for (int v = 0; v < g_nvar; v++)
                g_rtop[v] = rstk[sp].svtop[v];
            continue;
        }

        /* save stack tops for backtrack */
        for (int v = 0; v < g_nvar; v++)
            rstk[sp].svtop[v] = g_rtop[v];
        rstk[sp].ph = 1;
        sp++;

        /* process PHIs at this block */
        for (int pi = 0; pi < g_nphi; pi++) {
            if (g_phi[pi].blk != bi) continue;
            int v = g_phi[pi].var;
            uint32_t fid = FAKE_BASE + g_fseq++;
            if (g_rtop[v] < M2R_STKSZ)
                g_rstk[v][g_rtop[v]++] = fid;
        }

        /* walk instructions in this block */
        const jir_blk_t *b = &M->blks[bi];
        for (uint32_t ii = b->first;
             ii < b->first + b->n_inst && ii < M->n_inst; ii++) {
            jir_inst_t *I = &M->insts[ii];

            /* promoted ALLOCA → dead */
            if (I->op == JIR_ALLOCA && g_vof[ii] >= 0) {
                g_dead[ii] = 1;
                continue;
            }

            /* STORE to promoted alloca → dead, push value */
            if (I->op == JIR_STORE && !JIR_IS_C(I->ops[1]) &&
                g_vof[I->ops[1]] >= 0) {
                int v = g_vof[I->ops[1]];
                g_dead[ii] = 1;
                if (g_rtop[v] < M2R_STKSZ)
                    g_rstk[v][g_rtop[v]++] = I->ops[0];
                continue;
            }

            /* LOAD from promoted alloca → dead, replace */
            if (I->op == JIR_LOAD && !JIR_IS_C(I->ops[0]) &&
                g_vof[I->ops[0]] >= 0) {
                int v = g_vof[I->ops[0]];
                g_dead[ii] = 1;
                g_repl[ii] = (g_rtop[v] > 0) ? g_rstk[v][g_rtop[v] - 1]
                                               : zero_c;
                continue;
            }
        }

        /* fill successor PHI operands */
        for (uint32_t ii = b->first;
             ii < b->first + b->n_inst && ii < M->n_inst; ii++) {
            const jir_inst_t *I = &M->insts[ii];
            if (I->op != JIR_BR && I->op != JIR_BR_COND) continue;

            uint32_t succs[2];
            int nsuc = 0;
            if (I->op == JIR_BR) {
                succs[nsuc++] = I->ops[0];
            } else {
                succs[nsuc++] = I->ops[1];
                succs[nsuc++] = I->ops[2];
            }

            for (int si = 0; si < nsuc; si++) {
                uint32_t sb = succs[si];
                /* find pred index of bi in sb */
                int pidx = -1;
                for (int p = 0; p < g_nprd[sb]; p++) {
                    if (g_pred[sb][p] == bi) { pidx = p; break; }
                }
                if (pidx < 0) continue;

                /* fill all PHIs in successor block */
                for (int pi = 0; pi < g_nphi; pi++) {
                    if (g_phi[pi].blk != sb) continue;
                    int v = g_phi[pi].var;
                    uint32_t val = (g_rtop[v] > 0) ? g_rstk[v][g_rtop[v] - 1]
                                                     : zero_c;
                    if (pidx < M2R_MAXP)
                        g_phi[pi].ops[pidx] = val;
                }
            }
        }

        /* push domtree children (reverse order for correct DFS) */
        for (int c = (int)g_ndch[bi] - 1; c >= 0; c--) {
            if (sp < JIR_MAX_BLKS) {
                rstk[sp].blk = g_dch[bi][c];
                rstk[sp].ph  = 0;
                sp++;
            }
        }
    }
}

/* ---- remap: resolve an operand through replacement chain ---- */

static uint32_t remap(uint32_t v, uint32_t fi, uint32_t li)
{
    if (JIR_IS_C(v)) return v;

    /* chase replacement chain for dead loads */
    for (int g = 0; g < 64; g++) {
        if (v >= fi && v < li && g_dead[v] && g_repl[v] != 0) {
            v = g_repl[v];
            continue;
        }
        break;
    }

    if (JIR_IS_C(v)) return v;

    /* fake PHI ID → look up remapped phi dst */
    if (v >= FAKE_BASE) {
        /* find which phi this fake ID belongs to */
        uint32_t seq = v - FAKE_BASE;
        /* scan phis in order — seq matches emission order */
        uint32_t cur = 0;
        for (int i = 0; i < g_nphi; i++) {
            if (cur == seq) {
                return g_rmap[g_phi[i].dst];
            }
            cur++;
        }
        return v; /* shouldn't happen */
    }

    /* normal instruction → remapped index */
    if (v >= fi && v < li)
        return g_rmap[v];
    return v;
}

/* ---- rewrite: rebuild instruction array with PHIs, dead removal ---- */

static void rewrite(jir_mod_t *M, uint32_t fb, uint32_t nb)
{
    uint32_t fi = M->blks[fb].first;
    uint32_t li = fi;
    for (uint32_t bi = fb; bi < fb + nb && bi < M->n_blks; bi++)
        li = M->blks[bi].first + M->blks[bi].n_inst;

    /* copy original instructions to temp */
    uint32_t n_orig = li - fi;
    memcpy(g_tmp + fi, M->insts + fi, sizeof(jir_inst_t) * n_orig);

    /* clear rmap */
    for (uint32_t i = fi; i < li; i++)
        g_rmap[i] = i;

    /* first pass: assign PHI destinations (fake inst indices for rmap) */
    for (int i = 0; i < g_nphi; i++)
        g_phi[i].dst = fi + (uint32_t)i; /* temporary — overwritten below */

    /* rebuild: walk blocks, emit PHIs first then surviving insts */
    uint32_t wp = fi; /* write pointer */

    for (uint32_t bi = fb; bi < fb + nb && bi < M->n_blks; bi++) {
        uint32_t old_first = M->blks[bi].first;
        uint32_t old_count = M->blks[bi].n_inst;
        M->blks[bi].first = wp;
        uint32_t count = 0;

        /* emit PHIs for this block */
        for (int pi = 0; pi < g_nphi; pi++) {
            if (g_phi[pi].blk != bi) continue;
            if (wp >= JIR_MAX_INST) break;

            g_phi[pi].dst = wp; /* real index now */
            jir_inst_t *P = &M->insts[wp];
            memset(P, 0, sizeof(*P));
            P->op = JIR_PHI;
            P->n_ops = g_phi[pi].n_ops;
            P->type = g_tmp[g_pvar[g_phi[pi].var]].type;
            /* operands filled after remap pass below */
            for (int k = 0; k < P->n_ops && k < 4; k++)
                P->ops[k] = g_phi[pi].ops[k];
            g_rmap[wp] = wp; /* identity for PHIs */
            wp++;
            count++;
        }

        /* copy surviving non-dead instructions */
        for (uint32_t ii = old_first; ii < old_first + old_count; ii++) {
            if (g_dead[ii]) continue;
            if (wp >= JIR_MAX_INST) break;

            g_rmap[ii] = wp;
            M->insts[wp] = g_tmp[ii];
            wp++;
            count++;
        }

        M->blks[bi].n_inst = count;
    }

    /* fill gap between compacted end and original end with NOPs.
     * Shifting subsequent functions would break their internal
     * operand references — easier to leave NOP padding. */
    uint32_t last_bi = fb + nb - 1;
    if (last_bi < M->n_blks) {
        uint32_t new_end = M->blks[last_bi].first + M->blks[last_bi].n_inst;
        for (uint32_t i = new_end; i < li; i++) {
            memset(&M->insts[i], 0, sizeof(jir_inst_t));
            M->insts[i].op = JIR_NOP;
        }
    }

    /* remap operands (second pass — all rmap entries now valid) */
    for (uint32_t bi = fb; bi < fb + nb && bi < M->n_blks; bi++) {
        const jir_blk_t *b = &M->blks[bi];
        for (uint32_t ii = b->first;
             ii < b->first + b->n_inst && ii < M->n_inst; ii++) {
            jir_inst_t *I = &M->insts[ii];

            if (I->op == JIR_PHI) {
                /* remap PHI operands */
                for (int k = 0; k < I->n_ops && k < 4; k++)
                    I->ops[k] = remap(I->ops[k], fi, li);
                continue;
            }

            if (I->op == JIR_BR) {
                /* block refs — no remap needed (blocks unchanged) */
                continue;
            }
            if (I->op == JIR_BR_COND) {
                I->ops[0] = remap(I->ops[0], fi, li);
                /* ops[1], ops[2] are block refs */
                continue;
            }

            int nops = (I->n_ops == 0xFF) ? 0 : I->n_ops;
            for (int k = 0; k < nops && k < 4; k++)
                I->ops[k] = remap(I->ops[k], fi, li);
        }
    }
}

/* ---- Entry point ---- */

int jir_m2r(jir_mod_t *M)
{
    for (uint32_t fi = 0; fi < M->n_funcs; fi++) {
        jir_func_t *f = &M->funcs[fi];
        uint32_t fb = f->first_blk;
        uint32_t nb = f->n_blks;

        if (nb == 0) continue;

        /* 1. build CFG */
        mk_cfg(M, fb, nb);

        /* 2. find promotable allocas */
        fnd_prm(M, fb, nb, f->n_params);
        if (g_nvar == 0) continue; /* nothing to promote */

        /* 3. reverse postorder */
        mk_rpo(fb, nb, M);

        /* 4. compute dominators */
        cmp_dom(fb, nb);

        /* 5. dominance frontiers */
        cmp_df(fb, nb);

        /* 6. insert PHI nodes */
        ins_phi(M, fb, nb);

        /* 7. build domtree children */
        bld_dch(fb, nb);

        /* 8. rename variables */
        ren_var(M, fb, nb);

        /* 9. rewrite instruction array */
        rewrite(M, fb, nb);

        /* update function inst count */
        f->n_inst = 0;
        for (uint32_t bi = fb; bi < fb + nb && bi < M->n_blks; bi++)
            f->n_inst += M->blks[bi].n_inst;
    }

    return SK_OK;
}
