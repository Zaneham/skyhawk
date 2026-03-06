/* tharns.h -- Skyhawk test harness
 * Borrowed from BarraCUDA, which borrowed it from a dream
 * about a testing framework that didn't need a PhD to operate. */
#ifndef THARNS_H
#define THARNS_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef void (*tfunc_t)(void);

typedef struct {
    const char *tname;
    const char *tcats;
    tfunc_t     func;
} tcase_t;

#define TH_MAXTS 256
#define TH_BUFSZ 2048

extern tcase_t th_list[];
extern int th_cnt;
extern int npass, nfail, nskip;

/* ---- Self-Registration ---- */

#define TH_REG(cat, fn) \
    __attribute__((constructor)) static void reg_##fn(void) { \
        if (th_cnt < TH_MAXTS) \
            th_list[th_cnt++] = (tcase_t){#fn, cat, fn}; \
    }

/* ---- Assertions ---- */

#define CHECK(x) do { if (!(x)) { \
    printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
    nfail++; return; } } while(0)

#define CHEQ(a, b)   CHECK((a) == (b))
#define CHNE(a, b)   CHECK((a) != (b))
#define CHSTR(a, b)  CHECK(strcmp((a),(b)) == 0)
#define PASS()       do { npass++; } while(0)
#define SKIP(r)      do { nskip++; printf("  SKIP: %s\n", r); return; } while(0)

/* ---- Hex check with diagnostic ---- */

#define CHEQX(a, b) do { \
    unsigned _a = (unsigned)(a), _b = (unsigned)(b); \
    if (_a != _b) { \
        printf("  FAIL %s:%d: 0x%08X != 0x%08X\n", __FILE__, __LINE__, _a, _b); \
        nfail++; return; \
    } } while(0)

/* ---- Binary Path ---- */

#ifdef _WIN32
#define SK_BIN ".\\skyhawk.exe"
#else
#define SK_BIN "./skyhawk"
#endif

/* ---- Utilities ---- */

int th_run(const char *cmd, char *obuf, int osz);
int th_exist(const char *path);

/* ---- Shared Pipeline Context ----
 * One 6 MB pipeline context shared by all new test files.
 * Because having six identical copies of a 6 MB struct is
 * the kind of thing that makes MinGW's BSS segment weep,
 * and 36 MB of uninitialised globals turns out to be the
 * precise amount needed to corrupt your stdout buffer. */

#include "../src/skyhawk.h"
#include "../src/fe/token.h"
#include "../src/fe/lexer.h"
#include "../src/fe/ast.h"
#include "../src/fe/parser.h"
#include "../src/fe/sema.h"
#include "../src/ir/jir.h"
#include "../src/x86/x86.h"

#define TP_MAXTOK  4096
#define TP_MAXND   8192

extern token_t    tp_toks[];
extern ast_node_t tp_nds[];
extern lexer_t    tp_lex;
extern parser_t   tp_par;
extern sema_ctx_t tp_sem;
extern jir_mod_t  tp_jir;
extern x86_mod_t  tp_x86;

int     tp_run(const char *src);
int64_t tp_jit(uint32_t fn_idx);
uint32_t tp_main(void);
int     tp_fndop(int op, uint32_t from);
int     tp_cntop(int op);

#endif /* THARNS_H */
