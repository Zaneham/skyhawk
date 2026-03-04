/* rv_elf.c -- ELF64 object file writer for RV64
 * Two output modes: relocatable .o (for proper linking)
 * and standalone executable (for QEMU testing, because
 * sometimes you just want to run the bloody thing). */

#include "rv.h"
#include "../skyhawk.h"

#include <stdio.h>
#include <string.h>

/* ---- Instruction Encoding (minimal, for _start stub) ---- */

static uint32_t elf_mkI(int op, int rd, int f3, int rs1, int imm12)
{
    return (uint32_t)(
        (op  & 0x7F)         |
        ((rd  & 0x1F) << 7)  |
        ((f3  & 0x07) << 12) |
        ((rs1 & 0x1F) << 15) |
        ((imm12 & 0xFFF) << 20)
    );
}

static uint32_t elf_mkJ(int op, int rd, int imm21)
{
    int b191  = (imm21 >> 12) & 0xFF;
    int b11   = (imm21 >> 11) & 1;
    int b101  = (imm21 >> 1)  & 0x3FF;
    int b20   = (imm21 >> 20) & 1;
    return (uint32_t)(
        (op & 0x7F)          |
        ((rd & 0x1F) << 7)   |
        ((b191) << 12)       |
        ((b11) << 20)        |
        ((b101) << 21)       |
        ((b20) << 31)
    );
}

/* ---- ELF Constants ---- */

#define EI_NIDENT   16
#define ET_REL      1
#define ET_EXEC     2
#define EM_RISCV    0xF3
#define EV_CURRENT  1
#define ELFCLASS64  2
#define ELFDATA2LSB 1
#define ELFOSABI_NONE 0

/* e_flags for RV64 */
#define EF_RV_DABI  0x0004    /* double-precision float ABI */

/* section types */
#define SHT_NULL     0
#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHT_RELA     4

/* section flags */
#define SHF_WRITE    0x1
#define SHF_ALLOC    0x2
#define SHF_EXEC     0x4

/* symbol binding/type */
#define STB_LOCAL    0
#define STB_GLOBAL   1
#define STT_NOTYPE   0
#define STT_FUNC     2
#define STT_SECTION  3

/* relocation types */
#define R_RV_CALL    19   /* R_RISCV_CALL_PLT */

/* program header type */
#define PT_LOAD      1

/* program header flags */
#define PF_X 1
#define PF_W 2
#define PF_R 4

/* ---- Byte writers (LE) ---- */

static void wr8(FILE *fp, uint8_t v)
{ fwrite(&v, 1, 1, fp); }

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

static void wr64(FILE *fp, uint64_t v)
{
    wr32(fp, (uint32_t)v);
    wr32(fp, (uint32_t)(v >> 32));
}

static void wr_pad(FILE *fp, int n)
{
    while (n-- > 0) wr8(fp, 0);
}

/* ---- String table ---- */

#define ELF_MAXSTR 4096

static char     g_stab[ELF_MAXSTR];
static uint32_t g_slen;

static void stb_init(void)
{
    g_slen = 1; /* [0] = NUL (empty string) */
    g_stab[0] = '\0';
}

static uint32_t stb_add(const char *s)
{
    uint32_t len = (uint32_t)strlen(s) + 1;
    if (g_slen + len > ELF_MAXSTR) return 0;
    uint32_t off = g_slen;
    memcpy(g_stab + off, s, len);
    g_slen += len;
    return off;
}

/* ---- ELF64 Header ---- */

static void wr_ehdr(FILE *fp, uint16_t type, uint64_t entry,
                    uint64_t phoff, uint64_t shoff,
                    uint16_t phnum, uint16_t shnum, uint16_t shstr)
{
    /* e_ident */
    wr8(fp, 0x7F); wr8(fp, 'E'); wr8(fp, 'L'); wr8(fp, 'F');
    wr8(fp, ELFCLASS64);
    wr8(fp, ELFDATA2LSB);
    wr8(fp, EV_CURRENT);
    wr8(fp, ELFOSABI_NONE);
    wr_pad(fp, 8); /* padding */

    wr16(fp, type);               /* e_type */
    wr16(fp, EM_RISCV);           /* e_machine */
    wr32(fp, EV_CURRENT);         /* e_version */
    wr64(fp, entry);              /* e_entry */
    wr64(fp, phoff);              /* e_phoff */
    wr64(fp, shoff);              /* e_shoff */
    wr32(fp, EF_RV_DABI);         /* e_flags */
    wr16(fp, 64);                 /* e_ehsize */
    wr16(fp, phnum ? 56 : 0);    /* e_phentsize */
    wr16(fp, phnum);              /* e_phnum */
    wr16(fp, 64);                 /* e_shentsize */
    wr16(fp, shnum);              /* e_shnum */
    wr16(fp, shstr);              /* e_shstrndx */
}

/* ---- Section Header ---- */

static void wr_shdr(FILE *fp, uint32_t name, uint32_t type,
                    uint64_t flags, uint64_t addr, uint64_t off,
                    uint64_t size, uint32_t link, uint32_t info,
                    uint64_t align, uint64_t entsize)
{
    wr32(fp, name);
    wr32(fp, type);
    wr64(fp, flags);
    wr64(fp, addr);
    wr64(fp, off);
    wr64(fp, size);
    wr32(fp, link);
    wr32(fp, info);
    wr64(fp, align);
    wr64(fp, entsize);
}

/* ---- Program Header ---- */

static void wr_phdr(FILE *fp, uint32_t type, uint32_t flags,
                    uint64_t off, uint64_t vaddr, uint64_t paddr,
                    uint64_t filesz, uint64_t memsz, uint64_t align)
{
    wr32(fp, type);
    wr32(fp, flags);
    wr64(fp, off);
    wr64(fp, vaddr);
    wr64(fp, paddr);
    wr64(fp, filesz);
    wr64(fp, memsz);
    wr64(fp, align);
}

/* ---- Symbol Table Entry (24 bytes) ---- */

static void wr_sym(FILE *fp, uint32_t name, uint8_t info,
                   uint8_t other, uint16_t shndx,
                   uint64_t value, uint64_t size)
{
    wr32(fp, name);
    wr8(fp, info);
    wr8(fp, other);
    wr16(fp, shndx);
    wr64(fp, value);
    wr64(fp, size);
}

/* ---- Rela Entry (24 bytes) ---- */

static void wr_rela(FILE *fp, uint64_t off, uint32_t sym,
                    uint32_t type, int64_t addend)
{
    wr64(fp, off);
    wr64(fp, ((uint64_t)sym << 32) | type);
    wr64(fp, (uint64_t)addend);
}

/* ---- rv_elf: Relocatable Object (.o) ----
 * Layout:
 *   ELF header (64)
 *   .text data
 *   .rela.text data
 *   .symtab data
 *   .strtab data
 *   .shstrtab data
 *   section headers (6 * 64)
 */

int rv_elf(const rv_mod_t *R, const char *path)
{
    const jir_mod_t *J = R->J;
    uint32_t CL = R->codelen;
    int n_xfx = R->n_xfx;

    /* string tables */
    stb_init();

    /* shstrtab: section names */
    char shstrtab[256];
    uint32_t shsl = 1; /* [0] = NUL */
    shstrtab[0] = '\0';

    /* helper to add to shstrtab */
    #define SHSTR(s) do { \
        uint32_t _l = (uint32_t)strlen(s) + 1; \
        if (shsl + _l < sizeof(shstrtab)) { \
            memcpy(shstrtab + shsl, s, _l); \
            shsl += _l; \
        } \
    } while(0)

    uint32_t nm_text  = shsl; SHSTR(".text");
    uint32_t nm_rela  = shsl; SHSTR(".rela.text");
    uint32_t nm_sym   = shsl; SHSTR(".symtab");
    uint32_t nm_str   = shsl; SHSTR(".strtab");
    uint32_t nm_shstr = shsl; SHSTR(".shstrtab");

    /* strtab: symbol names */
    for (uint32_t fi = 0; fi < R->n_funcs; fi++)
        stb_add(J->strs + J->funcs[fi].name);
    for (uint32_t xi = 0; xi < J->n_xfn; xi++)
        stb_add(J->strs + J->xfuncs[xi].name);

    /* symbol count: NULL + .text section + funcs + externs */
    uint32_t n_local = 2; /* NULL + .text */
    uint32_t n_global = R->n_funcs + J->n_xfn;
    uint32_t n_sym = n_local + n_global;

    /* rela entries */
    uint32_t n_rela = (uint32_t)n_xfx;

    /* layout */
    uint64_t off_text = 64;
    uint64_t off_rela = off_text + CL;
    uint64_t off_symt = off_rela + n_rela * 24;
    uint64_t off_strt = off_symt + n_sym * 24;
    uint64_t off_shst = off_strt + g_slen;
    uint64_t off_shdr = off_shst + shsl;

    /* align section headers to 8 */
    if (off_shdr & 7) off_shdr = (off_shdr + 7) & ~(uint64_t)7;

    FILE *fp = fopen(path, "wb");
    if (!fp) return SK_ERR_IO;

    /* 1. ELF header */
    wr_ehdr(fp, ET_REL, 0, 0, off_shdr, 0, 6, 5);

    /* 2. .text data */
    fwrite(R->code, 1, CL, fp);

    /* 3. .rela.text entries */
    for (int i = 0; i < n_xfx; i++) {
        uint32_t si = n_local + R->n_funcs + R->xfx[i].xfn;
        wr_rela(fp, R->xfx[i].off, si, R_RV_CALL, 0);
    }

    /* 4. .symtab */
    /* NULL symbol */
    wr_sym(fp, 0, 0, 0, 0, 0, 0);
    /* .text section symbol */
    wr_sym(fp, 0, (uint8_t)((STB_LOCAL << 4) | STT_SECTION),
           0, 1, 0, 0);

    /* re-init strtab for name lookup */
    stb_init();

    /* function symbols */
    for (uint32_t fi = 0; fi < R->n_funcs; fi++) {
        const char *nm = J->strs + J->funcs[fi].name;
        uint32_t noff = stb_add(nm);
        wr_sym(fp, noff,
               (uint8_t)((STB_GLOBAL << 4) | STT_FUNC),
               0, 1, R->fn_off[fi], 0);
    }

    /* external function symbols (SHN_UNDEF = 0) */
    for (uint32_t xi = 0; xi < J->n_xfn; xi++) {
        const char *nm = J->strs + J->xfuncs[xi].name;
        uint32_t noff = stb_add(nm);
        wr_sym(fp, noff,
               (uint8_t)((STB_GLOBAL << 4) | STT_FUNC),
               0, 0, 0, 0);
    }

    /* 5. .strtab data */
    fwrite(g_stab, 1, g_slen, fp);

    /* 6. .shstrtab data */
    fwrite(shstrtab, 1, shsl, fp);

    /* pad to alignment */
    {
        long cur = ftell(fp);
        while (cur < (long)off_shdr) { wr8(fp, 0); cur++; }
    }

    /* 7. Section headers */
    /* [0] NULL */
    wr_shdr(fp, 0, SHT_NULL, 0, 0, 0, 0, 0, 0, 0, 0);

    /* [1] .text */
    wr_shdr(fp, nm_text, SHT_PROGBITS,
            SHF_ALLOC | SHF_EXEC, 0, off_text, CL,
            0, 0, 4, 0);

    /* [2] .rela.text */
    wr_shdr(fp, nm_rela, SHT_RELA, 0, 0, off_rela,
            n_rela * 24, 3, 1, 8, 24);

    /* [3] .symtab */
    wr_shdr(fp, nm_sym, SHT_SYMTAB, 0, 0, off_symt,
            n_sym * 24, 4, n_local, 8, 24);

    /* [4] .strtab */
    wr_shdr(fp, nm_str, SHT_STRTAB, 0, 0, off_strt,
            g_slen, 0, 0, 1, 0);

    /* [5] .shstrtab */
    wr_shdr(fp, nm_shstr, SHT_STRTAB, 0, 0, off_shst,
            shsl, 0, 0, 1, 0);

    fclose(fp);

    (void)wr_pad; (void)wr_phdr;
    return SK_OK;
}

/* ---- rv_exec: Standalone ELF for QEMU ----
 * Minimal ET_EXEC with a _start that calls main, then ecall(exit).
 * One LOAD segment. No dynamic linking, no shared libraries,
 * no nonsense. Just machine code and a syscall.
 *
 * Layout:
 *   ELF header (64)
 *   Program header (56)
 *   [padding to 0x78]
 *   _start stub (28 bytes)
 *   User code (codelen bytes)
 *   [end]
 */

int rv_exec(const rv_mod_t *R, const char *path)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) return SK_ERR_IO;

    /* _start stub: call main, then exit with a0 as code */
    uint8_t stub[28];
    uint32_t stub_len = 0;

    /* Find main function (last one, the implicit entry) */
    uint32_t main_off = 0;
    if (R->n_funcs > 0)
        main_off = R->fn_off[R->n_funcs - 1];

    /* The stub will be placed right after the ELF+PH headers.
     * Total header size: 64 (ehdr) + 56 (phdr) = 120 = 0x78.
     * Code starts at vaddr + 0x78. Stub is at offset 0.
     * main_off is relative to start of user code.
     * Stub occupies stub_len bytes, so main is at stub_len + main_off. */

    /* jal ra, main_off + stub_len (from stub start) */
    /* we'll patch this after we know stub_len */
    memset(stub, 0, sizeof(stub));

    /* instruction 0: JAL ra, <main>  (4 bytes, patched below) */
    stub_len = 0;
    stub_len += 4;

    /* instruction 1: mv a0, a0 (result already in a0) -- NOP */
    /* Actually, the main function already puts return value in a0 */

    /* instruction 2: li a7, 93 (exit syscall) = ADDI a7, zero, 93 */
    {
        uint32_t w = elf_mkI(0x13, RV_A7, 0, RV_ZERO, 93);
        stub[stub_len+0] = (uint8_t)(w);
        stub[stub_len+1] = (uint8_t)(w >> 8);
        stub[stub_len+2] = (uint8_t)(w >> 16);
        stub[stub_len+3] = (uint8_t)(w >> 24);
        stub_len += 4;
    }

    /* instruction 3: ecall */
    {
        uint32_t w = elf_mkI(0x73, 0, 0, 0, 0);
        stub[stub_len+0] = (uint8_t)(w);
        stub[stub_len+1] = (uint8_t)(w >> 8);
        stub[stub_len+2] = (uint8_t)(w >> 16);
        stub[stub_len+3] = (uint8_t)(w >> 24);
        stub_len += 4;
    }

    /* patch instruction 0: JAL ra, target
     * target = stub_len + main_off (bytes from stub[0]) */
    {
        int32_t rel = (int32_t)(stub_len + main_off);
        uint32_t w = elf_mkJ(0x6F, RV_RA, rel);
        stub[0] = (uint8_t)(w);
        stub[1] = (uint8_t)(w >> 8);
        stub[2] = (uint8_t)(w >> 16);
        stub[3] = (uint8_t)(w >> 24);
    }

    uint64_t vaddr = 0x10000;
    uint64_t hdr_sz = 64 + 56; /* ehdr + 1 phdr */
    uint64_t code_start = hdr_sz;
    uint64_t total_code = stub_len + R->codelen;
    uint64_t total_file = hdr_sz + total_code;
    uint64_t entry = vaddr + code_start; /* _start */

    /* 1. ELF header */
    wr_ehdr(fp, ET_EXEC, entry, 64, 0, 1, 0, 0);

    /* 2. Program header (LOAD segment: entire file mapped) */
    wr_phdr(fp, PT_LOAD, PF_R | PF_X,
            0, vaddr, vaddr,
            total_file, total_file, 0x1000);

    /* 3. Stub + user code */
    fwrite(stub, 1, stub_len, fp);
    fwrite(R->code, 1, R->codelen, fp);

    fclose(fp);
    return SK_OK;
}
