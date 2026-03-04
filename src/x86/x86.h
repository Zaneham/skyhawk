/* x86.h -- x86-64 backend definitions
 * Register names, condition codes, and a struct large enough
 * to park a light aircraft in. Keep it static or suffer. */
#ifndef SKYHAWK_X86_H
#define SKYHAWK_X86_H

#include "../ir/jir.h"

/* ---- GPR indices (ModRM encoding) ---- */

#define R_RAX  0
#define R_RCX  1
#define R_RDX  2
#define R_RBX  3
#define R_RSP  4
#define R_RBP  5
#define R_RSI  6
#define R_RDI  7
#define R_R8   8
#define R_R9   9
#define R_R10 10
#define R_R11 11
#define R_R12 12
#define R_R13 13
#define R_R14 14
#define R_R15 15

/* ---- XMM indices ---- */

#define R_XMM0  0
#define R_XMM1  1
#define R_XMM2  2
#define R_XMM3  3
#define R_XMM4  4
#define R_XMM5  5
#define R_XMM6  6
#define R_XMM7  7

/* ---- Condition codes (Jcc / SETcc low nibble) ---- */

#define CC_O   0x00
#define CC_NO  0x01
#define CC_B   0x02   /* unsigned < */
#define CC_AE  0x03   /* unsigned >= */
#define CC_E   0x04   /* == */
#define CC_NE  0x05   /* != */
#define CC_BE  0x06   /* unsigned <= */
#define CC_A   0x07   /* unsigned > */
#define CC_S   0x08
#define CC_NS  0x09
#define CC_L   0x0C   /* signed < */
#define CC_GE  0x0D   /* signed >= */
#define CC_LE  0x0E   /* signed <= */
#define CC_G   0x0F   /* signed > */

/* ---- PE-COFF constants ---- */

#define COFF_AMD64      0x8664
#define COFF_SCN_TEXT   0x60000020  /* CODE|EXEC|READ */
#define COFF_SCN_RDATA  0x40000040  /* INITIALIZED_DATA|READ */
#define COFF_SYM_EXT    2           /* IMAGE_SYM_CLASS_EXTERNAL */
#define COFF_SYM_STAT   3           /* IMAGE_SYM_CLASS_STATIC */
#define COFF_DTYPE_FN   0x20        /* IMAGE_SYM_DTYPE_FUNCTION */
#define COFF_REL32      0x04        /* IMAGE_REL_AMD64_REL32 */
#define OBJ_MAXSYM      256
#define OBJ_MAXSTR      8192

/* ---- Code buffer limits ---- */

#define X86_CODE_MAX  (256 * 1024)
#define X86_FIX_MAX   4096
#define X86_CFX_MAX   1024
#define X86_XFX_MAX   256
#define X86_RDATA_MAX (16 * 1024)
#define X86_RDFX_MAX  256

/* ---- Module (~300KB, must be static) ---- */

typedef struct {
    const jir_mod_t *J;

    uint8_t   code[X86_CODE_MAX];
    uint32_t  codelen;

    int32_t   slots[JIR_MAX_INST];     /* JIR inst → RBP offset */
    uint32_t  blk_off[JIR_MAX_BLKS];   /* block → code offset */

    struct { uint32_t off; uint32_t blk; } fix[X86_FIX_MAX];
    int       n_fix;

    struct { uint32_t off; uint32_t fn; }  cfx[X86_CFX_MAX];
    int       n_cfx;

    struct { uint32_t off; uint32_t xfn; } xfx[X86_XFX_MAX];
    int       n_xfx;

    uint8_t   rdata[X86_RDATA_MAX];    /* .rdata contents */
    uint32_t  rdlen;

    struct { uint32_t off; uint32_t rd_off; } rdfx[X86_RDFX_MAX];
    int       n_rdfx;

    uint32_t  fn_off[JIR_MAX_FUNCS];
    uint32_t  n_funcs;

    sk_err_t  errors[SK_MAX_ERRORS];
    int       n_errs;
} x86_mod_t;

/* ---- Public API ---- */

void x86_init(x86_mod_t *X, const jir_mod_t *J);
int  x86_emit(x86_mod_t *X);
int  x86_coff(const x86_mod_t *X, const char *path);

/* ---- Register Allocator ---- */

void     x86_ra(const jir_mod_t *J, uint32_t fi, int8_t *rmap);
uint16_t x86_rcs(void);     /* callee-saved regs used (bitmask) */
uint16_t x86_rcsr(void);    /* caller-saved regs used (bitmask) */
int8_t   x86_gpr_pool(int i);  /* allocatable GPR at index i */
int8_t   x86_xmm_pool(int i);  /* allocatable XMM at index i */

#endif /* SKYHAWK_X86_H */
