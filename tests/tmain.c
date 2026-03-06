/* tmain.c -- Skyhawk test runner
 * Judges your code silently. Like a customs officer at Auckland airport. */

#include "tharns.h"

#ifdef _WIN32
#include <windows.h>
#endif

/* ---- Storage ---- */

tcase_t th_list[TH_MAXTS];
int th_cnt = 0;
int npass  = 0;
int nfail  = 0;
int nskip  = 0;

/* ---- Shared Pipeline Context ----
 * One copy of the pipeline statics, shared by tfloat, tnest,
 * tstress, and tavion. Six copies of a 6 MB struct exceeds
 * MinGW's BSS tolerance and corrupts adjacent memory. */

token_t    tp_toks[TP_MAXTOK];
ast_node_t tp_nds[TP_MAXND];
lexer_t    tp_lex;
parser_t   tp_par;
sema_ctx_t tp_sem;
jir_mod_t  tp_jir;
x86_mod_t  tp_x86;

int tp_run(const char *src)
{
    uint32_t len = (uint32_t)strlen(src);
    lexer_init(&tp_lex, src, len, tp_toks, TP_MAXTOK);
    if (lexer_run(&tp_lex) != SK_OK) return -1;
    parser_init(&tp_par, tp_toks, tp_lex.num_toks,
                src, len, tp_nds, TP_MAXND);
    if (parser_run(&tp_par) != SK_OK) return -2;
    sema_init(&tp_sem, &tp_par);
    if (sema_run(&tp_sem) != SK_OK) return -3;
    jir_init(&tp_jir, &tp_sem);
    if (jir_lower(&tp_jir) != SK_OK) return -4;
    jir_m2r(&tp_jir);
    x86_init(&tp_x86, &tp_jir);
    if (x86_emit(&tp_x86) != SK_OK) return -5;
    return 0;
}

typedef int64_t (*tp_fn_t)(void);

int64_t tp_jit(uint32_t fn_idx)
{
#ifdef _WIN32
    uint32_t len = tp_x86.codelen;
    void *mem = VirtualAlloc(NULL, len,
                             MEM_COMMIT | MEM_RESERVE,
                             PAGE_EXECUTE_READWRITE);
    if (!mem) return -99999;
    memcpy(mem, tp_x86.code, len);
    uint32_t off = tp_x86.fn_off[fn_idx];
    tp_fn_t fn;
    {
        void *addr = (uint8_t *)mem + off;
        memcpy(&fn, &addr, sizeof(fn));
    }
    int64_t result = fn();
    VirtualFree(mem, 0, MEM_RELEASE);
    return result;
#else
    (void)fn_idx;
    return -99999;
#endif
}

uint32_t tp_main(void)
{
    if (tp_jir.n_funcs == 0) return 0;
    return tp_jir.n_funcs - 1;
}

int tp_fndop(int op, uint32_t from)
{
    for (uint32_t i = from; i < tp_jir.n_inst; i++)
        if (tp_jir.insts[i].op == (uint16_t)op)
            return (int)i;
    return -1;
}

int tp_cntop(int op)
{
    int c = 0;
    for (uint32_t i = 0; i < tp_jir.n_inst; i++)
        if (tp_jir.insts[i].op == (uint16_t)op) c++;
    return c;
}

/* ---- Utilities ---- */

int th_run(const char *cmd, char *obuf, int osz)
{
    char full[TH_BUFSZ];
    snprintf(full, TH_BUFSZ, "%s 2>&1", cmd);
    FILE *fp = popen(full, "r");
    if (!fp) { obuf[0] = '\0'; return -1; }
    int n = (int)fread(obuf, 1, (size_t)(osz - 1), fp);
    if (n < 0) n = 0;
    obuf[n] = '\0';
    int rc = pclose(fp);
#ifndef _WIN32
    if (rc != -1 && (rc & 0xFF) == 0)
        rc = (rc >> 8) & 0xFF;
#endif
    return rc;
}

int th_exist(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    fclose(fp);
    return 1;
}

/* ---- Category Ordering ---- */

static const char *cat_order[] = {
    "smoke", "sema", "layout", "ir", "types", "compool",
    "codegen", "nest", "stress", "avionics", "encode", "rv", NULL
};

static int cat_idx(const char *cat)
{
    for (int i = 0; cat_order[i]; i++)
        if (strcmp(cat, cat_order[i]) == 0) return i;
    return 99;
}

/* ---- Display ---- */

static void prt_res(const char *tname, const char *tag)
{
    int nlen = (int)strlen(tname);
    int dots = 30 - nlen;
    if (dots < 3) dots = 3;
    printf("  %s ", tname);
    for (int i = 0; i < dots; i++) putchar('.');
    printf(" %s\n", tag);
}

static void run_one(tcase_t *tc)
{
    int wp = npass, wf = nfail, ws = nskip;

    tc->func();

    if (nfail > wf)       prt_res(tc->tname, "FAIL");
    else if (nskip > ws)  prt_res(tc->tname, "SKIP");
    else if (npass > wp)  prt_res(tc->tname, "PASS");
    else {
        npass++;
        prt_res(tc->tname, "PASS");
    }
}

/* ---- Main ---- */

int main(int argc, char *argv[])
{
    const char *fcat  = NULL;
    const char *ftest = NULL;
    int list = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--cat") == 0 && i + 1 < argc)
            fcat = argv[++i];
        else if (strcmp(argv[i], "--test") == 0 && i + 1 < argc)
            ftest = argv[++i];
        else if (strcmp(argv[i], "--list") == 0)
            list = 1;
    }

    if (list) {
        for (int ci = 0; cat_order[ci]; ci++) {
            int hdr = 0;
            for (int i = 0; i < th_cnt; i++) {
                if (strcmp(th_list[i].tcats, cat_order[ci]) != 0) continue;
                if (!hdr) { printf("[%s]\n", cat_order[ci]); hdr = 1; }
                printf("  %s\n", th_list[i].tname);
            }
        }
        return 0;
    }

    printf("Skyhawk Test Suite\n");
    printf("====================\n");

    for (int ci = 0; cat_order[ci]; ci++) {
        if (fcat && strcmp(fcat, cat_order[ci]) != 0)
            continue;

        int hdr = 0;
        for (int i = 0; i < th_cnt; i++) {
            if (strcmp(th_list[i].tcats, cat_order[ci]) != 0) continue;
            if (ftest && strcmp(ftest, th_list[i].tname) != 0) continue;
            if (!hdr) { printf("[%s]\n", cat_order[ci]); hdr = 1; }
            run_one(&th_list[i]);
        }
    }

    /* orphans */
    for (int i = 0; i < th_cnt; i++) {
        if (cat_idx(th_list[i].tcats) < 99) continue;
        if (fcat) continue;
        if (ftest && strcmp(ftest, th_list[i].tname) != 0) continue;
        run_one(&th_list[i]);
    }

    int total = npass + nfail + nskip;
    printf("====================\n");
    printf("%d tests: %d passed, %d failed, %d skipped\n",
           total, npass, nfail, nskip);

    (void)argc;
    return nfail > 0 ? 1 : 0;
}
