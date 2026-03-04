/* main.c -- Skyhawk J73 compiler driver
 * Everything starts here, and so do most problems. */

#include "skyhawk.h"
#include "fe/token.h"
#include "fe/lexer.h"
#include "fe/ast.h"
#include "fe/parser.h"
#include "fe/sema.h"
#include "ir/jir.h"
#include "x86/x86.h"
#include "rv/rv.h"
#include "cpl/cpl.h"

/* ---- Modes ---- */

#define MODE_LEX   1
#define MODE_PARSE 2
#define MODE_SEMA  3
#define MODE_IR    4
#define MODE_COMP  5

/* ---- File I/O ---- */

static int read_src(const char *path, char *buf, uint32_t max)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "skyhawk: cannot open '%s'\n", path);
        return -1;
    }
    int n = (int)fread(buf, 1, max - 1, fp);
    fclose(fp);
    if (n < 0) n = 0;
    buf[n] = '\0';
    return n;
}

/* ---- Token dump ---- */

static void dump_toks(const lexer_t *L)
{
    char buf[256];
    for (uint32_t i = 0; i < L->num_toks; i++) {
        const token_t *t = &L->tokens[i];
        lexer_text(L, t, buf, (int)sizeof(buf));
        printf("%4u:%-3u  %-12s  %s\n",
               t->line, t->col, tok_name(t->type), buf);
    }
}

/* ---- Error dump ---- */

static void dump_errs(const sk_err_t *errs, int n, const char *path)
{
    for (int i = 0; i < n; i++)
        fprintf(stderr, "%s:%u:%u: %s\n",
                path, errs[i].loc.line, errs[i].loc.col,
                errs[i].msg);
}

/* ---- Usage ---- */

static void usage(void)
{
    fprintf(stderr,
        "usage: skyhawk [options] <file.jov>\n"
        "\n"
        "options:\n"
        "  --lex          dump tokens\n"
        "  --parse        dump AST (not yet)\n"
        "  --sema         dump typed AST (not yet)\n"
        "  --ir           dump JIR SSA\n"
        "  --rv           target RISC-V 64 (ELF output)\n"
        "  --cpl <file>   import COMPOOL binary before compilation\n"
        "  -o <out>       output object file\n"
    );
}

/* ---- Main ---- */

int main(int argc, char *argv[])
{
    int mode = MODE_COMP;
    int use_rv = 0;
    const char *infile  = NULL;
    const char *outfile = NULL;
    const char *cpls[16];
    int n_cpls = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--lex") == 0)
            mode = MODE_LEX;
        else if (strcmp(argv[i], "--parse") == 0)
            mode = MODE_PARSE;
        else if (strcmp(argv[i], "--sema") == 0)
            mode = MODE_SEMA;
        else if (strcmp(argv[i], "--ir") == 0)
            mode = MODE_IR;
        else if (strcmp(argv[i], "--rv") == 0)
            use_rv = 1;
        else if (strcmp(argv[i], "--cpl") == 0 && i + 1 < argc) {
            if (n_cpls < 16) cpls[n_cpls++] = argv[++i];
            else { fprintf(stderr, "skyhawk: too many --cpl\n"); return 1; }
        }
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            outfile = argv[++i];
        else if (argv[i][0] == '-') {
            fprintf(stderr, "skyhawk: unknown flag '%s'\n", argv[i]);
            usage();
            return 1;
        } else {
            if (infile) {
                fprintf(stderr, "skyhawk: multiple input files\n");
                return 1;
            }
            infile = argv[i];
        }
    }

    if (!infile) {
        usage();
        return 1;
    }

    /* outfile used after codegen */

    /* ---- Read source ---- */

    static char src[SK_MAX_SRC];
    int slen = read_src(infile, src, SK_MAX_SRC);
    if (slen < 0) return 1;

    /* ---- Lex ---- */

    static token_t toks[SK_MAX_TOKS];
    static lexer_t lex;
    lexer_init(&lex, src, (uint32_t)slen, toks, SK_MAX_TOKS);
    int rc = lexer_run(&lex);

    if (lex.num_errs > 0)
        dump_errs(lex.errors, lex.num_errs, infile);

    if (mode == MODE_LEX) {
        dump_toks(&lex);
        return rc != SK_OK ? 1 : 0;
    }

    if (rc != SK_OK) {
        fprintf(stderr, "skyhawk: lexer failed with %d error(s)\n",
                lex.num_errs);
        return 1;
    }

    /* ---- Parse ---- */

    static ast_node_t nodes[SK_MAX_NODES];
    static parser_t par;
    parser_init(&par, toks, lex.num_toks, src, (uint32_t)slen,
                nodes, SK_MAX_NODES);
    rc = parser_run(&par);

    if (par.num_errs > 0)
        dump_errs(par.errors, par.num_errs, infile);

    if (mode == MODE_PARSE) {
        ast_dump(&par);
        return rc != SK_OK ? 1 : 0;
    }

    if (rc != SK_OK) {
        fprintf(stderr, "skyhawk: parser failed with %d error(s)\n",
                par.num_errs);
        return 1;
    }
    /* ---- Sema ---- */

    static sema_ctx_t sem;
    sema_init(&sem, &par);

    /* import COMPOOLs before sema — declarations become visible */
    for (int ci = 0; ci < n_cpls; ci++) {
        rc = cpl_read(&sem, cpls[ci]);
        if (rc != SK_OK) {
            fprintf(stderr, "skyhawk: cannot import '%s'\n", cpls[ci]);
            return 1;
        }
    }

    rc = sema_run(&sem);

    if (sem.n_errs > 0)
        dump_errs(sem.errors, sem.n_errs, infile);

    if (mode == MODE_SEMA) {
        sema_dump(&sem);
        return rc != SK_OK ? 1 : 0;
    }

    if (rc != SK_OK) {
        fprintf(stderr, "skyhawk: sema failed with %d error(s)\n",
                sem.n_errs);
        return 1;
    }

    /* standalone COMPOOL → emit .cpl binary */
    if (par.ast.root < par.ast.n_nodes &&
        par.ast.nodes[par.ast.root].type == ND_COMPOOL &&
        mode == MODE_COMP) {
        /* derive output path: foo.jov → foo.cpl */
        char cpath[SK_MAX_PATH];
        snprintf(cpath, SK_MAX_PATH, "%s", infile);
        char *dot = strrchr(cpath, '.');
        if (dot) snprintf(dot, (size_t)(SK_MAX_PATH - (dot - cpath)), ".cpl");
        else snprintf(cpath + strlen(cpath),
                      (size_t)(SK_MAX_PATH - strlen(cpath)), ".cpl");

        rc = cpl_write(&sem, cpath);
        if (rc != SK_OK) {
            fprintf(stderr, "skyhawk: failed to write '%s'\n", cpath);
            return 1;
        }
        printf("skyhawk: %s written\n", cpath);
        return 0;
    }
    /* ---- JIR ---- */

    static jir_mod_t jir;
    jir_init(&jir, &sem);
    rc = jir_lower(&jir);

    if (jir.n_errs > 0)
        dump_errs(jir.errors, jir.n_errs, infile);

    if (rc != SK_OK) {
        fprintf(stderr, "skyhawk: IR lowering failed with %d error(s)\n",
                jir.n_errs);
        return 1;
    }

    /* mem2reg: promote scalar allocas to SSA */
    jir_m2r(&jir);

    if (mode == MODE_IR) {
        jir_dump(&jir);
        return 0;
    }

    /* ---- Codegen ---- */

    if (use_rv) {
        static rv_mod_t rv;
        rv_init(&rv, &jir);
        rc = rv_emit(&rv);
        if (rv.n_errs > 0)
            dump_errs(rv.errors, rv.n_errs, infile);
        if (rc != SK_OK) {
            fprintf(stderr, "skyhawk: rv codegen failed\n");
            return 1;
        }
        if (outfile) {
            rc = rv_elf(&rv, outfile);
            if (rc != SK_OK) {
                fprintf(stderr, "skyhawk: failed to write '%s'\n", outfile);
                return 1;
            }
            printf("skyhawk: %s written (%u bytes rv64)\n",
                   outfile, rv.codelen);
            return 0;
        }
        printf("skyhawk: %u bytes generated (rv64)\n", rv.codelen);
        return 0;
    }

    static x86_mod_t x86;
    x86_init(&x86, &jir);
    rc = x86_emit(&x86);

    if (x86.n_errs > 0)
        dump_errs(x86.errors, x86.n_errs, infile);

    if (rc != SK_OK) {
        fprintf(stderr, "skyhawk: codegen failed\n");
        return 1;
    }

    if (outfile) {
        rc = x86_coff(&x86, outfile);
        if (rc != SK_OK) {
            fprintf(stderr, "skyhawk: failed to write '%s'\n", outfile);
            return 1;
        }
        printf("skyhawk: %s written (%u bytes code)\n",
               outfile, x86.codelen);
        return 0;
    }

    printf("skyhawk: %u bytes generated\n", x86.codelen);
    return 0;
}
