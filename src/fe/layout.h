/* layout.h -- J73 TABLE memory layout
 * Where bits go to live in neat little rows,
 * like passengers on a particularly bureaucratic airline. */
#ifndef SKYHAWK_LAYOUT_H
#define SKYHAWK_LAYOUT_H

#include "sema.h"

#define LY_MAX_FLDS  32

/* ---- Field layout result ---- */

typedef struct {
    char      name[SK_MAX_IDENT];
    uint32_t  jtype;       /* field's jtype index */
    uint32_t  byte_off;    /* byte offset in entry */
    uint16_t  bit_off;     /* bit offset within word */
    uint16_t  bit_wid;     /* field width in bits */
} lay_fld_t;

typedef struct {
    lay_fld_t flds[LY_MAX_FLDS];
    int       n_flds;
    uint32_t  total_bytes;
    uint16_t  wordsz;      /* 16 bits default */
} lay_tbl_t;

/* ---- Public API ---- */

int  lay_tabl(lay_tbl_t *L, const sema_ctx_t *S, uint32_t tbl_type_idx);
void lay_dump(const lay_tbl_t *L);

#endif /* SKYHAWK_LAYOUT_H */
