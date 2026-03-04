/* rv.h -- RISC-V 64 backend definitions
 * RV64IMFD: where every instruction is 32 bits wide
 * and the encoding makes more sense than x86's fever dream. */
#ifndef SKYHAWK_RV_H
#define SKYHAWK_RV_H

#include "../ir/jir.h"

/* ---- GPR indices (x0-x31, ABI names) ---- */

#define RV_ZERO  0    /* hardwired zero, nature's /dev/null */
#define RV_RA    1    /* return address */
#define RV_SP    2    /* stack pointer */
#define RV_GP    3    /* global pointer */
#define RV_TP    4    /* thread pointer */
#define RV_T0    5    /* temporaries (caller-saved) */
#define RV_T1    6
#define RV_T2    7
#define RV_S0    8    /* frame pointer / callee-saved */
#define RV_S1    9
#define RV_A0   10    /* args / return value */
#define RV_A1   11
#define RV_A2   12
#define RV_A3   13
#define RV_A4   14
#define RV_A5   15
#define RV_A6   16
#define RV_A7   17
#define RV_S2   18    /* callee-saved */
#define RV_S3   19
#define RV_S4   20
#define RV_S5   21
#define RV_S6   22
#define RV_S7   23
#define RV_S8   24
#define RV_S9   25
#define RV_S10  26
#define RV_S11  27
#define RV_T3   28    /* more caller-saved temps */
#define RV_T4   29
#define RV_T5   30
#define RV_T6   31

/* ---- FPR indices (f0-f31, ABI names) ---- */

#define RV_FT0   0    /* float temporaries */
#define RV_FT1   1
#define RV_FT2   2
#define RV_FT3   3
#define RV_FT4   4
#define RV_FT5   5
#define RV_FT6   6
#define RV_FT7   7
#define RV_FS0   8    /* float callee-saved */
#define RV_FS1   9
#define RV_FA0  10    /* float args / return */
#define RV_FA1  11
#define RV_FA2  12
#define RV_FA3  13
#define RV_FA4  14
#define RV_FA5  15
#define RV_FA6  16
#define RV_FA7  17
#define RV_FS2  18    /* float callee-saved */
#define RV_FS3  19
#define RV_FS4  20
#define RV_FS5  21
#define RV_FS6  22
#define RV_FS7  23
#define RV_FS8  24
#define RV_FS9  25
#define RV_FS10 26
#define RV_FS11 27
#define RV_FT8  28    /* float temporaries */
#define RV_FT9  29
#define RV_FT10 30
#define RV_FT11 31

/* ---- Code buffer limits ---- */

#define RV_CODE_MAX  (256 * 1024)
#define RV_FIX_MAX   4096
#define RV_CFX_MAX   1024
#define RV_XFX_MAX   256
#define RV_RDATA_MAX (16 * 1024)

/* ---- Module (~300KB, must be static) ---- */

typedef struct {
    const jir_mod_t *J;

    uint8_t   code[RV_CODE_MAX];
    uint32_t  codelen;

    int32_t   slots[JIR_MAX_INST];     /* JIR inst -> s0 offset */
    uint32_t  blk_off[JIR_MAX_BLKS];   /* block -> code offset */

    struct { uint32_t off; uint32_t blk; } fix[RV_FIX_MAX];
    int       n_fix;

    struct { uint32_t off; uint32_t fn; }  cfx[RV_CFX_MAX];
    int       n_cfx;

    struct { uint32_t off; uint32_t xfn; } xfx[RV_XFX_MAX];
    int       n_xfx;

    uint8_t   rdata[RV_RDATA_MAX];    /* .rdata contents */
    uint32_t  rdlen;

    uint32_t  fn_off[JIR_MAX_FUNCS];
    uint32_t  n_funcs;
    uint32_t  frm_sz[JIR_MAX_FUNCS];  /* frame size per func */

    sk_err_t  errors[SK_MAX_ERRORS];
    int       n_errs;
} rv_mod_t;

/* ---- Public API ---- */

void rv_init(rv_mod_t *R, const jir_mod_t *J);
int  rv_emit(rv_mod_t *R);
int  rv_elf(const rv_mod_t *R, const char *path);
int  rv_exec(const rv_mod_t *R, const char *path);

/* ---- Register Allocator ---- */

void     rv_ra(const jir_mod_t *J, uint32_t fi, int8_t *rmap);
uint16_t rv_rcs(void);     /* callee-saved bitmask */
uint16_t rv_rcsr(void);    /* caller-saved bitmask */

#endif /* SKYHAWK_RV_H */
