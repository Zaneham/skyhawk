/* parser.h -- J73 recursive descent parser interface
 * Consumes tokens, produces disappointment and occasionally an AST. */
#ifndef SKYHAWK_PARSER_H
#define SKYHAWK_PARSER_H

#include "token.h"
#include "ast.h"

typedef struct {
    const token_t *toks;
    uint32_t       n_toks;
    uint32_t       pos;

    /* source text (for extracting token strings) */
    const char    *src;
    uint32_t       src_len;

    /* output AST */
    ast_t          ast;

    sk_err_t       errors[SK_MAX_ERRORS];
    int            num_errs;
} parser_t;

void parser_init(parser_t *P, const token_t *toks, uint32_t n_toks,
                 const char *src, uint32_t src_len,
                 ast_node_t *nodes, uint32_t max_nodes);

int  parser_run(parser_t *P);

/* debug: print AST to stdout */
void ast_dump(const parser_t *P);

#endif /* SKYHAWK_PARSER_H */
