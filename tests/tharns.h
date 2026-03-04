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

#endif /* THARNS_H */
