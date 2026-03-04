/* lexer.h -- J73 tokeniser interface */
#ifndef SKYHAWK_LEXER_H
#define SKYHAWK_LEXER_H

#include "token.h"

typedef struct {
    const char *src;
    uint32_t    src_len;
    uint32_t    pos;
    uint32_t    line;
    uint32_t    line_start;

    token_t    *tokens;
    uint32_t    num_toks;
    uint32_t    max_toks;

    sk_err_t    errors[SK_MAX_ERRORS];
    int         num_errs;
} lexer_t;

void lexer_init(lexer_t *L, const char *src, uint32_t len,
                token_t *toks, uint32_t max);

int  lexer_run(lexer_t *L);

int  lexer_text(const lexer_t *L, const token_t *t,
                char *buf, int bufsz);

#endif /* SKYHAWK_LEXER_H */
