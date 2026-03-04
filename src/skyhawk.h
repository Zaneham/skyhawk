/* skyhawk.h -- master header
 * Limits, errors, and the quiet confidence of fixed buffers. */
#ifndef SKYHAWK_H
#define SKYHAWK_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- Limits ---- */

#define SK_MAX_SRC      (4 * 1024 * 1024)
#define SK_MAX_TOKS     (1 << 20)
#define SK_MAX_NODES    (1 << 18)
#define SK_MAX_IDENT    128
#define SK_MAX_ERRORS   64
#define SK_MAX_PATH     512
#define SK_MAX_DEPTH    256

/* ---- Error codes ---- */

#define SK_OK            0
#define SK_ERR_IO       -1
#define SK_ERR_LEX      -2
#define SK_ERR_PARSE    -3
#define SK_ERR_SEMA     -4
#define SK_ERR_OVERFLOW -5
#define SK_ERR_IR       -6
#define SK_ERR_CODEGEN  -7

/* ---- Source location ---- */

typedef struct {
    uint32_t line;
    uint16_t col;
    uint32_t offset;
} sk_loc_t;

/* ---- Error record ---- */

typedef struct {
    sk_loc_t loc;
    char     msg[256];
    int      code;
} sk_err_t;

/* ---- Item flags ---- */

#define SK_STATIC    0x0001
#define SK_CONSTANT  0x0002
#define SK_PARALLEL  0x0004
#define SK_INLINE    0x0008
#define SK_RENT      0x0010
#define SK_ENTRY     0x0020

#endif /* SKYHAWK_H */
