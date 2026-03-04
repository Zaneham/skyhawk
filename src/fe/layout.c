/* layout.c -- J73 TABLE memory layout computation
 * MIL-STD-1589C said "word-aligned" and left the rest as an exercise
 * for the reader. Fifty years later, here we are, arranging bits
 * into bytes like a very pedantic bricklayer.
 *
 * Default wordsize = 16 bits (J73 standard). Items >= wordsize get
 * word-aligned. Smaller items pack within the current word.
 * NF_OVERLAY fields share the offset of the previous field.
 * NF_POS fields get an explicit bit position. If none of that
 * makes sense, congratulations: you're normal. */

#include "layout.h"
#include <stdio.h>
#include <string.h>

/* ---- Bit width of a type ----
 * "How wide is your type?" sounds like a dating app question
 * but here it determines memory layout. Less romantic. */

static int fld_bits(const sema_ctx_t *S, uint32_t tidx)
{
    if (tidx == 0 || tidx >= (uint32_t)S->n_types)
        return 0;
    const jtype_t *t = &S->types[tidx];

    switch (t->kind) {
    case JT_SIGNED:
    case JT_UNSIGN:
    case JT_FLOAT:
    case JT_BIT:
    case JT_FIXED:
        return (int)t->width;

    case JT_CHAR:
        /* C n means n characters, each 8 bits. Not code points,
         * not UTF-anything, just bytes. It was the 70s. */
        return (int)t->width * 8;

    case JT_HOLLER:
        /* H n means n characters, each 8 bits (same as CHAR for layout) */
        return (int)t->width * 8;

    case JT_STATUS:
        /* STATUS ordinals: minimum 8 bits because nobody needs
         * to distinguish RED from BLUE with fewer than 8 bits
         * of conviction */
        if (t->n_extra <= 2) return 8;
        if (t->n_extra <= 4) return 8;
        if (t->n_extra <= 16) return 8;
        if (t->n_extra <= 256) return 8;
        return 16;

    case JT_PTR:
        return 32; /* pointer width -- architecture-dependent, default 32 */

    default:
        return 0;
    }
}

/* ---- Compute TABLE field layout ----
 * Tetris, but with alignment constraints and no music. */

int lay_tabl(lay_tbl_t *L, const sema_ctx_t *S, uint32_t tbl_type_idx)
{
    memset(L, 0, sizeof(*L));
    L->wordsz = 16; /* J73 default word size */

    if (tbl_type_idx == 0 || tbl_type_idx >= (uint32_t)S->n_types)
        return -1;
    const jtype_t *tt = &S->types[tbl_type_idx];
    if (tt->kind != JT_TABLE) return -1;

    uint32_t tdi = tt->extra;
    if (tdi >= (uint32_t)S->n_tbldf) return -1;
    const sm_tbldf_t *td = &S->tbldef[tdi];

    /* check for explicit wordsize in the AST (TABLE's aux2 field) */
    if (td->ast_nd != 0 && td->ast_nd < S->n_nodes) {
        uint16_t aw = S->nodes[td->ast_nd].aux2;
        if (aw > 0) L->wordsz = aw;
    }

    uint32_t cur_byte = 0;   /* current byte offset */
    int      cur_bit  = 0;   /* current bit offset within word */
    int      ws       = (int)L->wordsz;

    for (int i = 0; i < td->n_flds && i < LY_MAX_FLDS; i++) {
        const sm_fld_t *sf = &td->flds[i];
        lay_fld_t *lf = &L->flds[L->n_flds++];

        snprintf(lf->name, SK_MAX_IDENT, "%s", sf->name);
        lf->jtype   = sf->jtype;
        lf->bit_wid = (uint16_t)fld_bits(S, sf->jtype);

        /* check NF_OVERLAY and NF_POS from the AST node */
        uint16_t aflags = 0;
        if (sf->ast_nd != 0 && sf->ast_nd < S->n_nodes)
            aflags = S->nodes[sf->ast_nd].flags;

        if (aflags & NF_OVERLAY) {
            /* overlay: share offset of previous field */
            if (i > 0) {
                lf->byte_off = L->flds[i - 1].byte_off;
                lf->bit_off  = L->flds[i - 1].bit_off;
            } else {
                lf->byte_off = 0;
                lf->bit_off  = 0;
            }
            continue;
        }

        if (aflags & NF_POS) {
            /* explicit bit position from AST val field */
            int64_t bpos = 0;
            if (sf->ast_nd != 0 && sf->ast_nd < S->n_nodes)
                bpos = S->nodes[sf->ast_nd].val;
            lf->byte_off = (uint32_t)(bpos / 8);
            lf->bit_off  = (uint16_t)(bpos % 8);
            /* advance cursor past this field */
            int end_bit = (int)bpos + (int)lf->bit_wid;
            cur_byte = (uint32_t)((end_bit + 7) / 8);
            cur_bit  = end_bit % ws;
            continue;
        }

        /* normal packing: items >= wordsize get word-aligned */
        int bw = (int)lf->bit_wid;
        if (bw >= ws) {
            /* word-align: advance to next word boundary */
            if (cur_bit != 0) {
                cur_byte += (uint32_t)((ws + 7) / 8);
                cur_bit = 0;
            }
            /* also byte-align for multi-byte fields */
            lf->byte_off = cur_byte;
            lf->bit_off  = 0;
            cur_byte += (uint32_t)((bw + 7) / 8);
            cur_bit = 0;
        } else {
            /* small item: pack within current word */
            if (cur_bit + bw > ws) {
                /* doesn't fit in current word -- advance */
                cur_byte += (uint32_t)((ws + 7) / 8);
                cur_bit = 0;
            }
            lf->byte_off = cur_byte + (uint32_t)(cur_bit / 8);
            lf->bit_off  = (uint16_t)(cur_bit % 8);
            cur_bit += bw;
        }
    }

    /* total bytes: round up to word boundary */
    if (cur_bit != 0)
        cur_byte += (uint32_t)((ws + 7) / 8);
    L->total_bytes = cur_byte;

    return 0;
}

/* ---- Print field layout table ---- */

void lay_dump(const lay_tbl_t *L)
{
    printf("TABLE Layout (%u bytes, wordsz=%u):\n",
           L->total_bytes, L->wordsz);
    printf("  %-20s  %8s  %6s  %6s\n",
           "Field", "ByteOff", "BitOff", "BitWid");
    printf("  %-20s  %8s  %6s  %6s\n",
           "--------------------", "--------", "------", "------");
    for (int i = 0; i < L->n_flds; i++) {
        const lay_fld_t *f = &L->flds[i];
        printf("  %-20s  %8u  %6u  %6u\n",
               f->name, f->byte_off, f->bit_off, f->bit_wid);
    }
}
