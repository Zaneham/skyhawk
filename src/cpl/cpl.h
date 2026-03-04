/* cpl.h -- COMPOOL binary format
 * The ABI contract between compilation units, serialised into
 * a file so compact it makes a telegram look verbose.
 * Two modules reading the same .cpl agree on every byte offset
 * and bit position. That's the deal. No haggling. */
#ifndef SKYHAWK_CPL_H
#define SKYHAWK_CPL_H

#include "../fe/sema.h"
#include "../fe/layout.h"

/* ---- Magic & Limits ---- */

#define CPL_MAGIC   0x43504C31u  /* "CPL1" */
#define CPL_VER     1
#define CPL_MAXSTR  32768
#define CPL_MAXFILE (256 * 1024)

/* ---- On-disk Records ----
 * All little-endian, no struct padding.
 * Packed via explicit byte layout in read/write. */

/* Header (24 bytes) */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint16_t n_types;
    uint16_t n_stdef;
    uint16_t n_tbldf;
    uint16_t n_syms;
    uint32_t str_len;
    uint32_t reserved;
} cpl_hdr_t;

/* Type record (16 bytes) — mirrors jtype_t layout */
typedef struct {
    uint8_t  kind;
    uint8_t  pad;
    uint16_t width;
    int16_t  scale;
    uint16_t n_extra;
    uint32_t inner;
    uint32_t extra;
} cpl_type_t;

/* Field record (20 bytes) — pre-computed layout */
typedef struct {
    uint32_t name_off;
    uint32_t jtype;
    uint32_t byte_off;
    uint16_t bit_off;
    uint16_t bit_wid;
} cpl_fld_t;

/* Symbol record (20 bytes) */
typedef struct {
    uint32_t name_off;
    uint32_t type;
    uint8_t  kind;
    uint8_t  pad;
    uint16_t flags;
    int64_t  cval;
} cpl_sym_t;

/* ---- Public API ---- */

int cpl_write(const sema_ctx_t *S, const char *path);
int cpl_read(sema_ctx_t *S, const char *path);

#endif /* SKYHAWK_CPL_H */
