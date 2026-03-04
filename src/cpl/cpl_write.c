/* cpl_write.c -- COMPOOL binary serializer
 * Takes the sema's carefully curated declarations and packs them
 * into a .cpl file with the tenderness of a postal worker on
 * a Friday afternoon. Everything arrives, nothing is fragile. */

#include "cpl.h"
#include <stdio.h>
#include <string.h>

/* ---- String table builder ---- */

static char  g_strs[CPL_MAXSTR];
static int   g_slen;

static void str_init(void)
{
    g_strs[0] = '\0';  /* offset 0 = empty sentinel */
    g_slen = 1;
}

static uint32_t str_add(const char *s)
{
    int len = (int)strlen(s);
    if (len == 0) return 0;

    /* dedup: linear scan, N is small */
    for (int i = 1; i < g_slen; ) {
        if (strcmp(&g_strs[i], s) == 0)
            return (uint32_t)i;
        i += (int)strlen(&g_strs[i]) + 1;
    }

    if (g_slen + len + 1 > CPL_MAXSTR) return 0;
    uint32_t off = (uint32_t)g_slen;
    memcpy(&g_strs[g_slen], s, (size_t)(len + 1));
    g_slen += len + 1;
    return off;
}

/* ---- Little-endian writers ---- */

static void wr8(FILE *fp, uint8_t v) { fwrite(&v, 1, 1, fp); }
static void wr16(FILE *fp, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    fwrite(b, 1, 2, fp);
}
static void wr32(FILE *fp, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8),
                     (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    fwrite(b, 1, 4, fp);
}
static void wri16(FILE *fp, int16_t v) { wr16(fp, (uint16_t)v); }
static void wri32(FILE *fp, int32_t v) { wr32(fp, (uint32_t)v); }
static void wr64(FILE *fp, int64_t v)
{
    wr32(fp, (uint32_t)(uint64_t)v);
    wr32(fp, (uint32_t)((uint64_t)v >> 32));
}

/* ---- Uppercase (local copy — sema's is static) ---- */

static void up(char *s)
{
    for (; *s; s++)
        if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 32);
}

/* ---- Main Writer ---- */

int cpl_write(const sema_ctx_t *S, const char *path)
{
    /* count exportable symbols */
    int n_syms = 0;
    for (int i = 0; i < S->n_syms; i++) {
        int k = S->syms[i].kind;
        if (k == SYM_VAR || k == SYM_CONST ||
            k == SYM_TYPE || k == SYM_TABLE)
            n_syms++;
    }

    /* build string table */
    str_init();

    /* symbol names */
    for (int i = 0; i < S->n_syms; i++) {
        int k = S->syms[i].kind;
        if (k == SYM_VAR || k == SYM_CONST ||
            k == SYM_TYPE || k == SYM_TABLE)
            str_add(S->syms[i].name);
    }

    /* STATUS value names */
    for (int i = 0; i < S->n_stdef; i++)
        for (int j = 0; j < S->stdef[i].n_vals; j++) {
            char tmp[SK_MAX_IDENT];
            snprintf(tmp, SK_MAX_IDENT, "%s", S->stdef[i].vals[j]);
            up(tmp);
            str_add(tmp);
        }

    /* TABLE field names */
    for (int i = 0; i < S->n_tbldf; i++)
        for (int j = 0; j < S->tbldef[i].n_flds; j++)
            str_add(S->tbldef[i].flds[j].name);

    /* open output */
    FILE *fp = fopen(path, "wb");
    if (!fp) return SK_ERR_IO;

    /* ---- Header ---- */
    wr32(fp, CPL_MAGIC);
    wr16(fp, CPL_VER);
    wr16(fp, 0);                          /* flags */
    wr16(fp, (uint16_t)(S->n_types > 0 ? S->n_types - 1 : 0));  /* skip type 0 */
    wr16(fp, (uint16_t)S->n_stdef);
    wr16(fp, (uint16_t)S->n_tbldf);
    wr16(fp, (uint16_t)n_syms);
    wr32(fp, (uint32_t)g_slen);
    wr32(fp, 0);                          /* reserved */

    /* ---- String table ---- */
    fwrite(g_strs, 1, (size_t)g_slen, fp);

    /* ---- Type pool (skip index 0 = void sentinel) ---- */
    for (int i = 1; i < S->n_types; i++) {
        const jtype_t *t = &S->types[i];
        wr8(fp, t->kind);
        wr8(fp, 0);
        wr16(fp, t->width);
        wri16(fp, t->scale);
        wr16(fp, t->n_extra);
        wr32(fp, t->inner);
        wr32(fp, t->extra);
    }

    /* ---- STATUS defs ---- */
    for (int i = 0; i < S->n_stdef; i++) {
        const sm_stdef_t *sd = &S->stdef[i];
        wr16(fp, (uint16_t)sd->n_vals);
        wr16(fp, 0);  /* reserved */
        for (int j = 0; j < sd->n_vals; j++) {
            char tmp[SK_MAX_IDENT];
            snprintf(tmp, SK_MAX_IDENT, "%s", sd->vals[j]);
            up(tmp);
            wr32(fp, str_add(tmp));
        }
    }

    /* ---- TABLE defs ---- */
    for (int i = 0; i < S->n_tbldf; i++) {
        const sm_tbldf_t *td = &S->tbldef[i];

        /* compute layout for serialization */
        /* find the TABLE type that references this tbldef */
        uint32_t tty = 0;
        for (int t = 1; t < S->n_types; t++) {
            if (S->types[t].kind == JT_TABLE &&
                S->types[t].extra == (uint32_t)i)
                { tty = (uint32_t)t; break; }
        }

        static lay_tbl_t lay;
        memset(&lay, 0, sizeof(lay));
        if (tty != 0) lay_tabl(&lay, S, tty);

        wr16(fp, (uint16_t)td->n_flds);
        wr16(fp, lay.wordsz);
        wr32(fp, lay.total_bytes);
        wri32(fp, td->lo_dim);
        wri32(fp, td->hi_dim);

        for (int j = 0; j < td->n_flds; j++) {
            wr32(fp, str_add(td->flds[j].name));
            wr32(fp, td->flds[j].jtype);
            if (j < lay.n_flds) {
                wr32(fp, lay.flds[j].byte_off);
                wr16(fp, lay.flds[j].bit_off);
                wr16(fp, lay.flds[j].bit_wid);
            } else {
                wr32(fp, 0); wr16(fp, 0); wr16(fp, 0);
            }
        }
    }

    /* ---- Symbols ---- */
    for (int i = 0; i < S->n_syms; i++) {
        const sema_sym_t *sy = &S->syms[i];
        int k = sy->kind;
        if (k != SYM_VAR && k != SYM_CONST &&
            k != SYM_TYPE && k != SYM_TABLE)
            continue;
        wr32(fp, str_add(sy->name));
        wr32(fp, sy->type);
        wr8(fp, sy->kind);
        wr8(fp, 0);
        wr16(fp, sy->flags);
        wr64(fp, sy->cval);
    }

    fclose(fp);
    return SK_OK;
}
