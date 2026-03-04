/* lexer.c -- J73 tokeniser
 * Turns MIL-STD-1589C source into tokens.
 * Case-insensitive, apostrophe idents, double-quote comments,
 * based integers (2B1010, 8O777, 16HFF). A relic, lovingly preserved. */

#include "lexer.h"
#include <ctype.h>

/* ---- Keyword table (sorted for binary search) ---- */

const sk_kw_t sk_kwtab[] = {
    {"A",         TOK_TY_A},
    {"ABS",       TOK_ABS},
    {"ABORT",     TOK_ABORT},
    {"AND",       TOK_AND},
    {"B",         TOK_TY_B},
    {"BEGIN",     TOK_BEGIN},
    {"BIT",       TOK_BIT},
    {"BITSIZE",   TOK_BITSIZE},
    {"BLOCK",     TOK_BLOCK},
    {"BY",        TOK_BY},
    {"BYTE",      TOK_BYTE},
    {"BYTESIZE",  TOK_BYTESIZE},
    {"C",         TOK_TY_C},
    {"CASE",      TOK_CASE},
    {"CHARONE",   TOK_CHARONE},
    {"CLOSE",     TOK_CLOSE},
    {"COML",      TOK_COML},
    {"COMPOOL",   TOK_COMPOOL},
    {"CONSTANT",  TOK_CONSTANT},
    {"COPY",      TOK_COPY},
    {"D",         TOK_TY_D},
    {"DEF",       TOK_DEF},
    {"DEFAULT",   TOK_DEFAULT},
    {"DEFINE",    TOK_DEFINE},
    {"DELETE",    TOK_DELETE},
    {"ELSE",      TOK_ELSE},
    {"END",       TOK_END},
    {"ENTRY",     TOK_ENTRY},
    {"EQV",       TOK_EQV},
    {"EXIT",      TOK_EXIT},
    {"F",         TOK_TY_F},
    {"FALLTHRU",  TOK_FALLTHRU},
    {"FIRST",     TOK_FIRST},
    {"FIXED",     TOK_FIXED},
    {"FLOAT",     TOK_FLOAT},
    {"FOR",       TOK_FOR},
    {"FORMAT",    TOK_FORMAT},
    {"FREE",      TOK_FREE},
    {"GOTO",      TOK_GOTO},
    {"H",         TOK_TY_H},
    {"HOLRONE",   TOK_HOLRONE},
    {"IF",        TOK_IF},
    {"INLINE",    TOK_INLINE},
    {"INSTANCE",  TOK_INSTANCE},
    {"IOERR",     TOK_IOERR},
    {"ITEM",      TOK_ITEM},
    {"LABEL",     TOK_LABEL},
    {"LAST",      TOK_LAST},
    {"LBOUND",    TOK_LBOUND},
    {"LIKE",      TOK_LIKE},
    {"LOC",       TOK_LOC},
    {"MOD",       TOK_MOD},
    {"NENT",      TOK_NENT},
    {"NEXT",      TOK_NEXT},
    {"NOPACK",    TOK_NOPACK},
    {"NOREF",     TOK_NOREF},
    {"NOT",       TOK_NOT},
    {"NULL",      TOK_NULL},
    {"NWDSEN",    TOK_NWDSEN},
    {"OPEN",      TOK_OPEN},
    {"OR",        TOK_OR},
    {"OVERLAY",   TOK_OVERLAY},
    {"PACK",      TOK_PACK},
    {"PARALLEL",  TOK_PARALLEL},
    {"POINTER",   TOK_POINTER},
    {"POS",       TOK_POS},
    {"PROC",      TOK_PROC},
    {"PROGRAM",   TOK_PROGRAM},
    {"READ",      TOK_READ},
    {"REF",       TOK_REF},
    {"RENT",      TOK_RENT},
    {"REP",       TOK_REP},
    {"RETURN",    TOK_RETURN},
    {"ROUND",     TOK_ROUND},
    {"S",         TOK_TY_S},
    {"SEQ",       TOK_SEQ},
    {"SGN",       TOK_SGN},
    {"SHIFTL",    TOK_SHIFTL},
    {"SHIFTR",    TOK_SHIFTR},
    {"SIGNED",    TOK_SIGNED},
    {"SIZE",      TOK_SIZE},
    {"SQRT",      TOK_SQRT},
    {"SREF",      TOK_SREF},
    {"START",     TOK_START},
    {"STATIC",    TOK_STATIC},
    {"STATUS",    TOK_STATUS},
    {"STOP",      TOK_STOP},
    {"TABLE",     TOK_TABLE},
    {"TERM",      TOK_TERM},
    {"THEN",      TOK_THEN},
    {"TRUNCATE",  TOK_TRUNCATE},
    {"TYPE",      TOK_TYPE},
    {"U",         TOK_TY_U},
    {"UBOUND",    TOK_UBOUND},
    {"UNSIGNED",  TOK_UNSIGNED},
    {"V",         TOK_V},
    {"WHILE",     TOK_WHILE},
    {"WORDSIZE",  TOK_WORDSIZE},
    {"WREF",      TOK_WREF},
    {"WRITE",     TOK_WRITE},
    {"XOR",       TOK_XOR},
    {"ZONE",      TOK_ZONE},
};

const int sk_nkw = (int)(sizeof(sk_kwtab) / sizeof(sk_kwtab[0]));

/* ---- Token names ---- */

static const char *tok_names[] = {
    [TOK_INT_LIT]  = "INT_LIT",
    [TOK_FLT_LIT]  = "FLT_LIT",
    [TOK_STR_LIT]  = "STR_LIT",
    [TOK_IDENT]    = "IDENT",
    [TOK_ASSIGN]   = ":=",
    [TOK_PLUS]     = "+",
    [TOK_MINUS]    = "-",
    [TOK_STAR]     = "*",
    [TOK_SLASH]    = "/",
    [TOK_POWER]    = "**",
    [TOK_EQ]       = "=",
    [TOK_NE]       = "<>",
    [TOK_LT]       = "<",
    [TOK_LE]       = "<=",
    [TOK_GT]       = ">",
    [TOK_GE]       = ">=",
    [TOK_AT]       = "@",
    [TOK_DOT]      = ".",
    [TOK_LPAREN]   = "(",
    [TOK_RPAREN]   = ")",
    [TOK_LBRACK]   = "[",
    [TOK_RBRACK]   = "]",
    [TOK_COMMA]    = ",",
    [TOK_SEMI]     = ";",
    [TOK_COLON]    = ":",
    [TOK_NEWLINE]  = "NL",
    [TOK_COMMENT]  = "COMMENT",
    [TOK_EOF]      = "EOF",
    [TOK_ERROR]    = "ERROR",
};

const char *tok_name(int type)
{
    if (type >= 0 && type < TOK_COUNT && tok_names[type])
        return tok_names[type];
    /* keywords: look up in table */
    for (int i = 0; i < sk_nkw; i++)
        if (sk_kwtab[i].type == (uint16_t)type)
            return sk_kwtab[i].name;
    return "???";
}

/* ---- Helpers ---- */

static inline int at_end(const lexer_t *L)
{
    return L->pos >= L->src_len;
}

static inline char peek(const lexer_t *L)
{
    return at_end(L) ? '\0' : L->src[L->pos];
}

static inline char peek2(const lexer_t *L)
{
    return (L->pos + 1 < L->src_len) ? L->src[L->pos + 1] : '\0';
}

static inline void next(lexer_t *L)
{
    if (!at_end(L)) L->pos++;
}

static void lex_err(lexer_t *L, const char *msg)
{
    if (L->num_errs >= SK_MAX_ERRORS) return;
    sk_err_t *e = &L->errors[L->num_errs++];
    e->loc.line   = L->line;
    e->loc.col    = (uint16_t)(L->pos - L->line_start);
    e->loc.offset = L->pos;
    e->code       = SK_ERR_LEX;
    snprintf(e->msg, sizeof(e->msg), "%s", msg);
}

static void emit(lexer_t *L, int type, uint32_t off, uint16_t len)
{
    if (L->num_toks >= L->max_toks) return;
    token_t *t = &L->tokens[L->num_toks++];
    t->type   = (uint16_t)type;
    t->offset = off;
    t->len    = len;
    t->line   = L->line;
    t->col    = (uint16_t)(off - L->line_start);
}

/* ---- Keyword lookup (binary search, case-insensitive) ---- */

static int kw_find(const char *name, int len)
{
    /* uppercase into stack buf for comparison */
    char buf[SK_MAX_IDENT];
    if (len >= SK_MAX_IDENT) len = SK_MAX_IDENT - 1;
    for (int i = 0; i < len; i++)
        buf[i] = (char)toupper((unsigned char)name[i]);
    buf[len] = '\0';

    int lo = 0, hi = sk_nkw - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(buf, sk_kwtab[mid].name);
        if (c == 0) return (int)sk_kwtab[mid].type;
        if (c < 0) hi = mid - 1;
        else        lo = mid + 1;
    }
    return -1;
}

/* ---- Number lexing ---- */

static void lex_num(lexer_t *L)
{
    uint32_t start = L->pos;

    /* eat leading digits */
    while (!at_end(L) && isdigit((unsigned char)peek(L)))
        next(L);

    /* check for based integer: digits followed by B/O/H */
    char c = (char)toupper((unsigned char)peek(L));
    if (c == 'B' || c == 'O' || c == 'H') {
        next(L); /* eat base indicator */
        while (!at_end(L) && isxdigit((unsigned char)peek(L)))
            next(L);
        emit(L, TOK_INT_LIT, start, (uint16_t)(L->pos - start));
        return;
    }

    /* check for float: digits followed by . or E */
    if (peek(L) == '.' && isdigit((unsigned char)peek2(L))) {
        next(L); /* eat . */
        while (!at_end(L) && isdigit((unsigned char)peek(L)))
            next(L);
    }

    c = (char)toupper((unsigned char)peek(L));
    if (c == 'E') {
        next(L); /* eat E */
        if (peek(L) == '+' || peek(L) == '-') next(L);
        while (!at_end(L) && isdigit((unsigned char)peek(L)))
            next(L);
        emit(L, TOK_FLT_LIT, start, (uint16_t)(L->pos - start));
        return;
    }

    /* was there a dot? then it's float */
    if (L->pos > start && L->src[L->pos - 1] != '.' &&
        memchr(L->src + start, '.', L->pos - start)) {
        emit(L, TOK_FLT_LIT, start, (uint16_t)(L->pos - start));
        return;
    }

    emit(L, TOK_INT_LIT, start, (uint16_t)(L->pos - start));
}

/* ---- Identifier / keyword lexing ---- */

static int is_ident_ch(char c)
{
    return isalnum((unsigned char)c) || c == '$' || c == '\'';
}

static void lex_ident(lexer_t *L)
{
    uint32_t start = L->pos;
    next(L); /* eat first char (alpha or $) */

    while (!at_end(L) && is_ident_ch(peek(L)))
        next(L);

    int len = (int)(L->pos - start);
    int kw = kw_find(L->src + start, len);
    if (kw >= 0)
        emit(L, kw, start, (uint16_t)len);
    else
        emit(L, TOK_IDENT, start, (uint16_t)len);
}

/* ---- String literal: 'text' ---- */

static void lex_str(lexer_t *L)
{
    uint32_t start = L->pos;
    next(L); /* eat opening ' */
    while (!at_end(L) && peek(L) != '\'')
        next(L);
    if (!at_end(L)) next(L); /* eat closing ' */
    else lex_err(L, "unterminated string");
    emit(L, TOK_STR_LIT, start, (uint16_t)(L->pos - start));
}

/* ---- Comment: "text" ---- */

static void lex_comment(lexer_t *L)
{
    uint32_t start = L->pos;
    next(L); /* eat opening " */
    while (!at_end(L) && peek(L) != '"')
        next(L);
    if (!at_end(L)) next(L); /* eat closing " */
    else lex_err(L, "unterminated comment");
    emit(L, TOK_COMMENT, start, (uint16_t)(L->pos - start));
}

/* ---- Main tokenise loop ---- */

void lexer_init(lexer_t *L, const char *src, uint32_t len,
                token_t *toks, uint32_t max)
{
    memset(L, 0, sizeof(*L));
    L->src      = src;
    L->src_len  = len;
    L->tokens   = toks;
    L->max_toks = max;
    L->line     = 1;
}

int lexer_run(lexer_t *L)
{
    while (!at_end(L)) {
        char c = peek(L);

        /* whitespace */
        if (c == ' ' || c == '\t' || c == '\r') {
            next(L);
            continue;
        }

        /* newline */
        if (c == '\n') {
            next(L);
            L->line++;
            L->line_start = L->pos;
            continue;
        }

        /* comment */
        if (c == '"') { lex_comment(L); continue; }

        /* string */
        if (c == '\'') { lex_str(L); continue; }

        /* number */
        if (isdigit((unsigned char)c)) { lex_num(L); continue; }

        /* ident or keyword */
        if (isalpha((unsigned char)c) || c == '$') { lex_ident(L); continue; }

        /* two-char operators */
        uint32_t start = L->pos;

        if (c == ':' && peek2(L) == '=') {
            next(L); next(L);
            emit(L, TOK_ASSIGN, start, 2);
            continue;
        }
        if (c == '*' && peek2(L) == '*') {
            next(L); next(L);
            emit(L, TOK_POWER, start, 2);
            continue;
        }
        if (c == '<' && peek2(L) == '>') {
            next(L); next(L);
            emit(L, TOK_NE, start, 2);
            continue;
        }
        if (c == '<' && peek2(L) == '=') {
            next(L); next(L);
            emit(L, TOK_LE, start, 2);
            continue;
        }
        if (c == '>' && peek2(L) == '=') {
            next(L); next(L);
            emit(L, TOK_GE, start, 2);
            continue;
        }

        /* single-char operators */
        next(L);
        switch (c) {
        case '+': emit(L, TOK_PLUS,   start, 1); break;
        case '-': emit(L, TOK_MINUS,  start, 1); break;
        case '*': emit(L, TOK_STAR,   start, 1); break;
        case '/': emit(L, TOK_SLASH,  start, 1); break;
        case '=': emit(L, TOK_EQ,     start, 1); break;
        case '<': emit(L, TOK_LT,     start, 1); break;
        case '>': emit(L, TOK_GT,     start, 1); break;
        case '@': emit(L, TOK_AT,     start, 1); break;
        case '.': emit(L, TOK_DOT,    start, 1); break;
        case '(': emit(L, TOK_LPAREN, start, 1); break;
        case ')': emit(L, TOK_RPAREN, start, 1); break;
        case '[': emit(L, TOK_LBRACK, start, 1); break;
        case ']': emit(L, TOK_RBRACK, start, 1); break;
        case ',': emit(L, TOK_COMMA,  start, 1); break;
        case ';': emit(L, TOK_SEMI,   start, 1); break;
        case ':': emit(L, TOK_COLON,  start, 1); break;
        default:
            lex_err(L, "unexpected character");
            emit(L, TOK_ERROR, start, 1);
            break;
        }
    }

    /* sentinel */
    emit(L, TOK_EOF, L->pos, 0);

    return L->num_errs ? SK_ERR_LEX : SK_OK;
}

int lexer_text(const lexer_t *L, const token_t *t,
               char *buf, int bufsz)
{
    int n = (int)t->len;
    if (n >= bufsz) n = bufsz - 1;
    memcpy(buf, L->src + t->offset, (size_t)n);
    buf[n] = '\0';
    return n;
}
