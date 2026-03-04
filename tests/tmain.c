/* tmain.c -- Skyhawk test runner
 * Judges your code silently. Like a customs officer at Auckland airport. */

#include "tharns.h"

/* ---- Storage ---- */

tcase_t th_list[TH_MAXTS];
int th_cnt = 0;
int npass  = 0;
int nfail  = 0;
int nskip  = 0;

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
    "codegen", "encode", "rv", NULL
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
