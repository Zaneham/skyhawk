/* skrt.h -- Skyhawk J73 runtime library
 * The thinnest possible wrapper between a 1970s
 * aerospace language and a modern C library.
 * Compiled separately, linked by the user. */

#ifndef SKYHAWK_SKRT_H
#define SKYHAWK_SKRT_H

#include <stdint.h>

/* ---- Print to stdout ---- */

void sk_prtI(int64_t v);           /* integer */
void sk_prtF(double v);            /* float */
void sk_prtS(const char *s);       /* string */
void sk_prtC(int64_t ch);          /* character */
void sk_prtN(void);                /* newline */

/* ---- Formatted print ---- */

void sk_pfmI(int64_t v, int64_t w);          /* integer, width w */
void sk_pfmF(double v, int64_t w, int64_t d);/* float, width w.d */
void sk_pfmS(const char *s, int64_t w);      /* string, width w */

/* ---- Read from stdin ---- */

int64_t sk_rdI(void);
double  sk_rdF(void);

/* ---- File I/O (handle-based, table of 16 FILE*) ---- */

int64_t sk_fopn(const char *path, int64_t mode);
void    sk_fcls(int64_t h);
void    sk_fwrI(int64_t h, int64_t v);
void    sk_fwrF(int64_t h, double v);
void    sk_fwrS(int64_t h, const char *s);
int64_t sk_frdI(int64_t h);
double  sk_frdF(int64_t h);

/* ---- Arithmetic ---- */

int64_t sk_powi(int64_t b, int64_t e);
double  sk_powf(double b, double e);
int64_t sk_absi(int64_t v);

/* ---- Process ---- */

void sk_halt(int64_t code);

#endif /* SKYHAWK_SKRT_H */
