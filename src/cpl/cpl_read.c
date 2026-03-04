/* cpl_read.c -- COMPOOL binary deserializer
 * Reconstitutes declarations from a .cpl file with the quiet
 * efficiency of a customs officer who's seen it all before.
 * "Types? Symbols? TABLE layouts? Right this way, sir." */

#include "cpl.h"
#include <stdio.h>
#include <string.h>

/* ---- Little-endian readers ---- */

static uint8_t rd8(const uint8_t **p)
{
    uint8_t v = **p; (*p)++;
    return v;
}

static uint16_t rd16(const uint8_t **p)
{
    uint16_t v = (uint16_t)((*p)[0] | ((*p)[1] << 8));
    *p += 2;
    return v;
}

static int16_t rdi16(const uint8_t **p) { return (int16_t)rd16(p); }

static uint32_t rd32(const uint8_t **p)
{
    uint32_t v = (uint32_t)(*p)[0] | ((uint32_t)(*p)[1] << 8) |
                 ((uint32_t)(*p)[2] << 16) | ((uint32_t)(*p)[3] << 24);
    *p += 4;
    return v;
}

static int32_t rdi32(const uint8_t **p) { return (int32_t)rd32(p); }

static int64_t rd64(const uint8_t **p)
{
    uint32_t lo = rd32(p);
    uint32_t hi = rd32(p);
    return (int64_t)((uint64_t)lo | ((uint64_t)hi << 32));
}

/* ---- Uppercase ---- */

static void up(char *s)
{
    for (; *s; s++)
        if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 32);
}

/* ---- Type interning (local — mirrors sema's jt_intern) ---- */

static uint32_t intern(sema_ctx_t *S, int kind, int width,
                       int scale, int n_extra,
                       uint32_t inner, uint32_t extra)
{
    for (int i = 1; i < S->n_types; i++) {
        const jtype_t *t = &S->types[i];
        if (t->kind == (uint8_t)kind && t->width == (uint16_t)width &&
            t->scale == (int16_t)scale && t->n_extra == (uint16_t)n_extra &&
            t->inner == inner && t->extra == extra)
            return (uint32_t)i;
    }
    if (S->n_types >= SM_MAX_TYPES) return 0;
    int idx = S->n_types++;
    jtype_t *t = &S->types[idx];
    t->kind    = (uint8_t)kind;
    t->pad     = 0;
    t->width   = (uint16_t)width;
    t->scale   = (int16_t)scale;
    t->n_extra = (uint16_t)n_extra;
    t->inner   = inner;
    t->extra   = extra;
    return (uint32_t)idx;
}

/* ---- Main Reader ---- */

int cpl_read(sema_ctx_t *S, const char *path)
{
    /* slurp whole file (static — too large for the stack) */
    static uint8_t fbuf[CPL_MAXFILE];
    FILE *fp = fopen(path, "rb");
    if (!fp) return SK_ERR_IO;
    size_t flen = fread(fbuf, 1, CPL_MAXFILE, fp);
    fclose(fp);
    if (flen < 24) return SK_ERR_IO;

    const uint8_t *p = fbuf;
    const uint8_t *end = fbuf + flen;

    /* ---- Header ---- */
    uint32_t magic = rd32(&p);
    if (magic != CPL_MAGIC) return SK_ERR_IO;
    uint16_t ver = rd16(&p);
    if (ver != CPL_VER) return SK_ERR_IO;
    rd16(&p); /* flags */
    uint16_t n_types = rd16(&p);
    uint16_t n_stdef = rd16(&p);
    uint16_t n_tbldf = rd16(&p);
    uint16_t n_syms  = rd16(&p);
    uint32_t str_len = rd32(&p);
    rd32(&p); /* reserved */

    /* ---- String table ---- */
    if (p + str_len > end) return SK_ERR_IO;
    const char *strs = (const char *)p;
    p += str_len;

    /* helper: safe string fetch from strtab */
    #define SGET(off) ((off) < str_len ? &strs[(off)] : "")

    /* ---- Type pool ----
     * Types reference stdef/tbldef by index. Those indices are
     * relative to the .cpl file. We need to remap them after
     * importing STATUS and TABLE defs. Two-pass: first read raw
     * types, then fix up after stdef/tbldf import. */

    /* record starting indices for remapping */
    int base_stdef = S->n_stdef;
    int base_tbldf = S->n_tbldf;

    /* raw type data — read but don't intern yet (static — 32KB) */
    static cpl_type_t raw_types[SM_MAX_TYPES];
    if (n_types > SM_MAX_TYPES) n_types = (uint16_t)SM_MAX_TYPES;
    for (int i = 0; i < n_types; i++) {
        if (p + 16 > end) return SK_ERR_IO;
        raw_types[i].kind    = rd8(&p);
        raw_types[i].pad     = rd8(&p);
        raw_types[i].width   = rd16(&p);
        raw_types[i].scale   = rdi16(&p);
        raw_types[i].n_extra = rd16(&p);
        raw_types[i].inner   = rd32(&p);
        raw_types[i].extra   = rd32(&p);
    }

    /* ---- STATUS defs ---- */
    for (int i = 0; i < n_stdef; i++) {
        if (p + 4 > end) return SK_ERR_IO;
        uint16_t nv = rd16(&p);
        rd16(&p); /* reserved */

        if (S->n_stdef >= SM_MAX_STDEF) { p += (size_t)nv * 4; continue; }
        sm_stdef_t *sd = &S->stdef[S->n_stdef++];
        sd->n_vals = 0;
        for (int j = 0; j < nv && j < SM_MAX_STVALS; j++) {
            if (p + 4 > end) return SK_ERR_IO;
            uint32_t soff = rd32(&p);
            snprintf(sd->vals[sd->n_vals], SK_MAX_IDENT, "%s", SGET(soff));
            up(sd->vals[sd->n_vals]);
            sd->n_vals++;
        }
        /* skip overflow vals we can't store */
        for (int j = SM_MAX_STVALS; j < nv; j++) {
            if (p + 4 > end) return SK_ERR_IO;
            rd32(&p);
        }
    }

    /* ---- TABLE defs ---- */
    for (int i = 0; i < n_tbldf; i++) {
        if (p + 12 > end) return SK_ERR_IO;
        uint16_t nf   = rd16(&p);
        rd16(&p); /* wordsz — informational, not stored */
        rd32(&p); /* total_bytes — informational */
        int32_t lo    = rdi32(&p);
        int32_t hi    = rdi32(&p);

        if (S->n_tbldf >= SM_MAX_TBLDF) {
            p += (size_t)nf * 20;
            continue;
        }
        sm_tbldf_t *td = &S->tbldef[S->n_tbldf++];
        td->n_flds = 0;
        td->ast_nd = 0;  /* no AST backing — imported */
        td->lo_dim = lo;
        td->hi_dim = hi;

        for (int j = 0; j < nf; j++) {
            if (p + 20 > end) return SK_ERR_IO;
            uint32_t noff = rd32(&p);
            uint32_t fty  = rd32(&p);
            rd32(&p); /* byte_off — layout, skip */
            rd16(&p); /* bit_off */
            rd16(&p); /* bit_wid */

            if (j < SM_MAX_FIELDS) {
                sm_fld_t *f = &td->flds[td->n_flds++];
                snprintf(f->name, SK_MAX_IDENT, "%s", SGET(noff));
                up(f->name);
                f->jtype  = fty;   /* raw index — remapped below */
                f->ast_nd = 0;
            }
        }
    }

    /* ---- Type import with remapping ----
     * File type index i (1-based in sema) maps to remap[i].
     * Type 0 is always void sentinel → maps to 0.
     * File types are 0-based (type 0 in file = sema type 1). */

    static uint32_t remap[SM_MAX_TYPES];
    memset(remap, 0, sizeof(remap));

    for (int i = 0; i < n_types; i++) {
        cpl_type_t *rt = &raw_types[i];
        uint32_t inner = rt->inner;
        uint32_t extra = rt->extra;

        /* remap inner type reference */
        if (inner > 0 && inner <= (uint32_t)n_types)
            inner = remap[inner];

        /* remap extra for STATUS and TABLE types */
        if (rt->kind == JT_STATUS)
            extra = (uint32_t)(base_stdef + (int)extra);
        else if (rt->kind == JT_TABLE)
            extra = (uint32_t)(base_tbldf + (int)extra);

        remap[i + 1] = intern(S, rt->kind, rt->width,
                              rt->scale, rt->n_extra,
                              inner, extra);
    }

    /* fixup TABLE field type references */
    for (int i = base_tbldf; i < S->n_tbldf; i++) {
        sm_tbldf_t *td = &S->tbldef[i];
        for (int j = 0; j < td->n_flds; j++) {
            uint32_t old = td->flds[j].jtype;
            if (old > 0 && old <= (uint32_t)n_types)
                td->flds[j].jtype = remap[old];
        }
    }

    /* ---- Symbols ---- */
    for (int i = 0; i < n_syms; i++) {
        if (p + 20 > end) return SK_ERR_IO;
        uint32_t noff = rd32(&p);
        uint32_t type = rd32(&p);
        uint8_t  kind = rd8(&p);
        rd8(&p); /* pad */
        uint16_t flags = rd16(&p);
        int64_t  cval  = rd64(&p);

        /* remap type */
        if (type > 0 && type <= (uint32_t)n_types)
            type = remap[type];

        if (S->n_syms >= SM_MAX_SYMS) continue;
        sema_sym_t *sy = &S->syms[S->n_syms++];
        memset(sy, 0, sizeof(*sy));
        snprintf(sy->name, SK_MAX_IDENT, "%s", SGET(noff));
        up(sy->name);
        sy->type   = type;
        sy->ast_nd = 0;
        sy->kind   = kind;
        sy->scope  = 0;
        sy->flags  = flags;
        sy->cval   = cval;
    }

    #undef SGET
    return SK_OK;
}
