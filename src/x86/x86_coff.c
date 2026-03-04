/* x86_coff.c -- PE-COFF object file writer
 * Translates our lovingly crafted x86 bytes into a format
 * that link.exe will grudgingly accept. Think of it as
 * filling out customs forms for machine code. */

#include "x86.h"
#include "../skyhawk.h"

#include <stdio.h>
#include <string.h>

/* ---- Byte writers (LE, same dance as cpl_write.c) ---- */

static void wr8(FILE *fp, uint8_t v)
{
    fwrite(&v, 1, 1, fp);
}

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

static void wr_pad(FILE *fp, int n)
{
    while (n-- > 0) wr8(fp, 0);
}

/* ---- String table ---- */

static char     g_stab[OBJ_MAXSTR];
static uint32_t g_slen;

static void stb_init(void)
{
    g_slen = 4; /* 4-byte size prefix lives at [0..3] */
}

/* Add string to table, return offset. 0 on overflow. */
static uint32_t stb_add(const char *s)
{
    uint32_t len = (uint32_t)strlen(s) + 1; /* include NUL */
    if (g_slen + len > OBJ_MAXSTR) return 0;
    uint32_t off = g_slen;
    memcpy(g_stab + off, s, len);
    g_slen += len;
    return off;
}

/* ---- COFF structure writers ---- */

/* IMAGE_FILE_HEADER (20 bytes) */
static void wr_fhdr(FILE *fp, uint16_t nsec,
                    uint32_t sym_off, uint32_t n_sym)
{
    wr16(fp, COFF_AMD64);       /* Machine */
    wr16(fp, nsec);             /* NumberOfSections */
    wr32(fp, 0);                /* TimeDateStamp (reproducible) */
    wr32(fp, sym_off);          /* PointerToSymbolTable */
    wr32(fp, n_sym);            /* NumberOfSymbols */
    wr16(fp, 0);                /* SizeOfOptionalHeader */
    wr16(fp, 0);                /* Characteristics */
}

/* IMAGE_SECTION_HEADER (40 bytes) */
static void wr_shdr(FILE *fp, const char *name, uint32_t raw_sz,
                    uint32_t raw_off, uint32_t reloc_off,
                    uint16_t n_reloc, uint32_t flags)
{
    char nm[8];
    memset(nm, 0, 8);
    size_t nlen = strlen(name);
    if (nlen > 8) nlen = 8;
    memcpy(nm, name, nlen);
    fwrite(nm, 1, 8, fp);

    wr32(fp, 0);                /* VirtualSize */
    wr32(fp, 0);                /* VirtualAddress */
    wr32(fp, raw_sz);           /* SizeOfRawData */
    wr32(fp, raw_off);          /* PointerToRawData */
    wr32(fp, reloc_off);        /* PointerToRelocations */
    wr32(fp, 0);                /* PointerToLinenumbers */
    wr16(fp, n_reloc);          /* NumberOfRelocations */
    wr16(fp, 0);                /* NumberOfLinenumbers */
    wr32(fp, flags);            /* Characteristics */
}

/* IMAGE_RELOCATION (10 bytes) */
static void wr_reloc(FILE *fp, uint32_t vaddr,
                     uint32_t sym_idx, uint16_t type)
{
    wr32(fp, vaddr);
    wr32(fp, sym_idx);
    wr16(fp, type);
}

/* IMAGE_SYMBOL (18 bytes).
 * name <=8 chars: inline zero-padded.
 * name >8 chars: 4 bytes zero + 4 bytes strtab offset. */
static void wr_sym(FILE *fp, const char *name,
                   uint32_t val, uint16_t sec,
                   uint16_t type, uint8_t cls)
{
    size_t nlen = strlen(name);
    if (nlen <= 8) {
        /* inline name, zero-padded to 8 bytes */
        char buf[8];
        memset(buf, 0, 8);
        memcpy(buf, name, nlen);
        fwrite(buf, 1, 8, fp);
    } else {
        /* long name: zeroes + string table offset */
        uint32_t off = stb_add(name);
        wr32(fp, 0);
        wr32(fp, off);
    }
    wr32(fp, val);              /* Value */
    wr16(fp, sec);              /* SectionNumber */
    wr16(fp, type);             /* Type */
    wr8(fp, cls);               /* StorageClass */
    wr8(fp, 0);                 /* NumberOfAuxSymbols */
}

/* String table: 4-byte size prefix + string data. */
static void wr_stab(FILE *fp)
{
    /* Write size (includes the 4-byte prefix itself) */
    wr32(fp, g_slen);
    /* Write string data after the prefix */
    if (g_slen > 4)
        fwrite(g_stab + 4, 1, g_slen - 4, fp);
}

/* ---- Public API ---- */

int x86_coff(const x86_mod_t *X, const char *path)
{
    const jir_mod_t *J = X->J;
    int n_xfx  = X->n_xfx;
    int n_rdfx = X->n_rdfx;
    int has_rd = (X->rdlen > 0);
    uint16_t nsec = has_rd ? 2 : 1;

    /* total .text relocs = external calls + rdata refs */
    int n_trel = n_xfx + n_rdfx;

    /* sym count: section syms + internal funcs + external funcs.
     * .rdata gets its own section sym when present. */
    uint32_t n_sym = (uint32_t)nsec + X->n_funcs + J->n_xfn;

    /* .rdata section sym index (for rdata reloc targets) */
    uint32_t rd_sym = has_rd ? 1 : 0; /* 0=.text, 1=.rdata */

    stb_init();

    /* Pre-scan: add long names to string table */
    for (uint32_t fi = 0; fi < X->n_funcs; fi++) {
        const char *fn = J->strs + J->funcs[fi].name;
        if (strlen(fn) > 8)
            stb_add(fn);
    }
    for (uint32_t xi = 0; xi < J->n_xfn; xi++) {
        const char *xn = J->strs + J->xfuncs[xi].name;
        if (strlen(xn) > 8)
            stb_add(xn);
    }

    /* Layout (2 sections):
     *   0                  FILE_HEADER          (20)
     *   20                 SECTION .text         (40)
     *   60                 SECTION .rdata        (40)  [if has_rd]
     *   HDR_END            .text raw data        (CL)
     *   HDR_END+CL         .text relocations     (10 * n_trel)
     *   HDR_END+CL+TRL     .rdata raw data       (rdlen)
     *   HDR_END+CL+TRL+RD  symbol table
     *   ...                string table
     */
    uint32_t CL  = X->codelen;
    uint32_t RD  = has_rd ? X->rdlen : 0;
    uint32_t HDR = 20 + 40 * (uint32_t)nsec;
    uint32_t TRL = (uint32_t)n_trel * 10;

    uint32_t text_raw  = HDR;
    uint32_t text_reloc= (n_trel > 0) ? text_raw + CL : 0;
    uint32_t rd_raw    = text_raw + CL + TRL;
    uint32_t sym_off   = rd_raw + RD;

    FILE *fp = fopen(path, "wb");
    if (!fp) return SK_ERR_IO;

    /* 1. File header */
    wr_fhdr(fp, nsec, sym_off, n_sym);

    /* 2. Section headers */
    wr_shdr(fp, ".text", CL, text_raw, text_reloc,
            (uint16_t)n_trel, COFF_SCN_TEXT);

    if (has_rd)
        wr_shdr(fp, ".rdata", RD, rd_raw, 0, 0, COFF_SCN_RDATA);

    /* 3. .text raw data */
    fwrite(X->code, 1, CL, fp);

    /* 4. .text relocations */
    /* external call fixups — target = extern sym */
    for (int i = 0; i < n_xfx; i++) {
        uint32_t si = (uint32_t)nsec + X->n_funcs + X->xfx[i].xfn;
        wr_reloc(fp, X->xfx[i].off, si, COFF_REL32);
    }
    /* rdata reference fixups — target = .rdata section sym.
     * Write the rdata offset as addend at the rel32 site. */
    for (int i = 0; i < n_rdfx; i++) {
        /* patch the addend into .text code (already written),
         * so we need to fseek back. Instead, we pre-wrote 0
         * and rely on the linker: the initial 4 bytes at the
         * rel32 site serve as the addend A in S+A-(P+4). */
        /* first, write addend into the code buffer copy */
        uint32_t site = X->rdfx[i].off;
        if (site + 4 <= CL) {
            /* fseek to the raw data position and patch */
            long pos = (long)(text_raw + site);
            fseek(fp, pos, SEEK_SET);
            wr32(fp, X->rdfx[i].rd_off);
            fseek(fp, 0, SEEK_END);
        }
        wr_reloc(fp, site, rd_sym, COFF_REL32);
    }

    /* 5. .rdata raw data */
    if (has_rd)
        fwrite(X->rdata, 1, RD, fp);

    /* 6. Symbol table */
    /* .text section symbol */
    wr_sym(fp, ".text", 0, 1, 0, COFF_SYM_STAT);

    /* .rdata section symbol */
    if (has_rd)
        wr_sym(fp, ".rdata", 0, 2, 0, COFF_SYM_STAT);

    /* Internal function symbols (section 1) */
    for (uint32_t fi = 0; fi < X->n_funcs; fi++) {
        const char *fn = J->strs + J->funcs[fi].name;
        wr_sym(fp, fn, X->fn_off[fi], 1,
               COFF_DTYPE_FN, COFF_SYM_EXT);
    }

    /* External function symbols (section 0 = UNDEF) */
    for (uint32_t xi = 0; xi < J->n_xfn; xi++) {
        const char *xn = J->strs + J->xfuncs[xi].name;
        wr_sym(fp, xn, 0, 0, COFF_DTYPE_FN, COFF_SYM_EXT);
    }

    /* 7. String table */
    wr_stab(fp);

    fclose(fp);

    (void)wr_pad;
    return SK_OK;
}
