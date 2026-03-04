/* skrt.c -- Skyhawk J73 runtime library
 * Thin wrappers around C stdio, because even the most
 * deterministic avionic code occasionally needs to tell
 * someone what it's been up to. Keep it simple — nobody
 * wants their runtime larger than their compiler. */

#include "skrt.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

/* ---- File handle table ----
 * 0=stdin, 1=stdout, 2=stderr, 3-15 user files.
 * Like a boarding gate for I/O — 16 gates, no refunds. */

#define SK_MAXFH 16

static FILE *g_fhtab[SK_MAXFH];
static int   g_fhinit;

static void fh_init(void)
{
    if (g_fhinit) return;
    g_fhtab[0] = stdin;
    g_fhtab[1] = stdout;
    g_fhtab[2] = stderr;
    g_fhinit = 1;
}

static FILE *fh_get(int64_t h)
{
    fh_init();
    if (h < 0 || h >= SK_MAXFH) return stdout;
    return g_fhtab[h] ? g_fhtab[h] : stdout;
}

/* ---- Print to stdout ---- */

void sk_prtI(int64_t v)   { printf("%lld", (long long)v); }
void sk_prtF(double v)    { printf("%g", v); }
void sk_prtS(const char *s) { if (s) printf("%s", s); }
void sk_prtC(int64_t ch)  { putchar((int)(ch & 0x7F)); }
void sk_prtN(void)        { putchar('\n'); }

/* ---- Formatted print ---- */

void sk_pfmI(int64_t v, int64_t w)
{
    printf("%*lld", (int)w, (long long)v);
}

void sk_pfmF(double v, int64_t w, int64_t d)
{
    printf("%*.*f", (int)w, (int)d, v);
}

void sk_pfmS(const char *s, int64_t w)
{
    if (s) printf("%-*s", (int)w, s);
}

/* ---- Read from stdin ---- */

int64_t sk_rdI(void)
{
    long long v = 0;
    if (scanf("%lld", &v) != 1) v = 0;
    return (int64_t)v;
}

double sk_rdF(void)
{
    double v = 0.0;
    if (scanf("%lf", &v) != 1) v = 0.0;
    return v;
}

/* ---- File I/O ---- */

int64_t sk_fopn(const char *path, int64_t mode)
{
    fh_init();
    /* find free slot */
    int slot = -1;
    for (int i = 3; i < SK_MAXFH; i++) {
        if (!g_fhtab[i]) { slot = i; break; }
    }
    if (slot < 0) return -1;

    const char *m = (mode == 1) ? "rb" : (mode == 2) ? "wb" : "rb";
    FILE *fp = fopen(path, m);
    if (!fp) return -1;
    g_fhtab[slot] = fp;
    return (int64_t)slot;
}

void sk_fcls(int64_t h)
{
    fh_init();
    if (h < 3 || h >= SK_MAXFH) return;
    if (g_fhtab[h]) {
        fclose(g_fhtab[h]);
        g_fhtab[h] = NULL;
    }
}

void sk_fwrI(int64_t h, int64_t v)
{
    fprintf(fh_get(h), "%lld", (long long)v);
}

void sk_fwrF(int64_t h, double v)
{
    fprintf(fh_get(h), "%g", v);
}

void sk_fwrS(int64_t h, const char *s)
{
    if (s) fprintf(fh_get(h), "%s", s);
}

int64_t sk_frdI(int64_t h)
{
    long long v = 0;
    if (fscanf(fh_get(h), "%lld", &v) != 1) v = 0;
    return (int64_t)v;
}

double sk_frdF(int64_t h)
{
    double v = 0.0;
    if (fscanf(fh_get(h), "%lf", &v) != 1) v = 0.0;
    return v;
}

/* ---- Arithmetic ----
 * Because 3**4 shouldn't require a maths degree,
 * but implementing it from scratch sort of does. */

int64_t sk_powi(int64_t b, int64_t e)
{
    if (e < 0) return 0;     /* integer truncation — spec-compliant */
    if (e == 0) return 1;
    int64_t r = 1;
    for (int i = 0; i < 63 && e > 0; i++) {
        if (e & 1) r *= b;
        b *= b;
        e >>= 1;
    }
    return r;
}

double sk_powf(double b, double e)
{
    return pow(b, e);
}

int64_t sk_absi(int64_t v)
{
    return v < 0 ? -v : v;
}

/* ---- Process ---- */

void sk_halt(int64_t code)
{
    exit((int)code);
}
