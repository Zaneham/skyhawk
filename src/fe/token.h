/* token.h -- J73 token types
 * 90 keywords for a language older than most of its users. */
#ifndef SKYHAWK_TOKEN_H
#define SKYHAWK_TOKEN_H

#include "skyhawk.h"

typedef enum {
    /* ---- Literals ---- */
    TOK_INT_LIT,        /* 42, 2B1010, 8O777, 16HFF */
    TOK_FLT_LIT,        /* 3.14, 1E10 */
    TOK_STR_LIT,        /* 'text' */
    TOK_IDENT,          /* NAME, CURRENT'WAYPOINT */

    /* ---- Operators ---- */
    TOK_ASSIGN,         /* := */
    TOK_PLUS,           /* + */
    TOK_MINUS,          /* - */
    TOK_STAR,           /* * */
    TOK_SLASH,          /* / */
    TOK_POWER,          /* ** */
    TOK_EQ,             /* = */
    TOK_NE,             /* <> */
    TOK_LT,             /* < */
    TOK_LE,             /* <= */
    TOK_GT,             /* > */
    TOK_GE,             /* >= */
    TOK_AT,             /* @ (deref) */
    TOK_DOT,            /* . (member) */

    /* ---- Delimiters ---- */
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACK,
    TOK_RBRACK,
    TOK_COMMA,
    TOK_SEMI,
    TOK_COLON,

    /* ---- Control flow ---- */
    TOK_ABORT,
    TOK_BEGIN,
    TOK_BY,
    TOK_CASE,
    TOK_DEFAULT,
    TOK_ELSE,
    TOK_END,
    TOK_EXIT,
    TOK_FALLTHRU,
    TOK_FOR,
    TOK_GOTO,
    TOK_IF,
    TOK_RETURN,
    TOK_STOP,
    TOK_TERM,
    TOK_THEN,
    TOK_WHILE,

    /* ---- Declarations ---- */
    TOK_BLOCK,
    TOK_COMPOOL,
    TOK_CONSTANT,
    TOK_DEF,
    TOK_DEFINE,
    TOK_ENTRY,
    TOK_INLINE,
    TOK_INSTANCE,
    TOK_ITEM,
    TOK_LABEL,
    TOK_LIKE,
    TOK_OVERLAY,
    TOK_PARALLEL,
    TOK_POS,
    TOK_PROC,
    TOK_PROGRAM,
    TOK_REF,
    TOK_RENT,
    TOK_REP,
    TOK_START,
    TOK_STATIC,
    TOK_TABLE,
    TOK_TYPE,
    TOK_ZONE,

    /* ---- Type keywords ---- */
    TOK_BIT,
    TOK_BYTE,
    TOK_CHARONE,
    TOK_FIXED,
    TOK_FLOAT,
    TOK_HOLRONE,
    TOK_POINTER,
    TOK_ROUND,
    TOK_SIGNED,
    TOK_STATUS,
    TOK_TRUNCATE,
    TOK_UNSIGNED,
    TOK_V,
    TOK_WORDSIZE,

    /* ---- Type indicators (single letter) ---- */
    TOK_TY_S,          /* S n (signed) */
    TOK_TY_U,          /* U n (unsigned) */
    TOK_TY_F,          /* F n (float) */
    TOK_TY_B,          /* B n (bit) */
    TOK_TY_C,          /* C n (character) */
    TOK_TY_H,          /* H n (hollerith) */
    TOK_TY_A,          /* A n (fixed-point) */
    TOK_TY_D,          /* D n (scale factor) */

    /* ---- Logical operators ---- */
    TOK_AND,
    TOK_OR,
    TOK_NOT,
    TOK_XOR,
    TOK_EQV,
    TOK_MOD,

    /* ---- Built-in functions ---- */
    TOK_ABS,
    TOK_BITSIZE,
    TOK_BYTESIZE,
    TOK_FIRST,
    TOK_LAST,
    TOK_LOC,
    TOK_LBOUND,
    TOK_UBOUND,
    TOK_NEXT,
    TOK_NWDSEN,
    TOK_SHIFTL,
    TOK_SHIFTR,
    TOK_SGN,
    TOK_SIZE,
    TOK_SQRT,
    TOK_NENT,

    /* ---- I/O ---- */
    TOK_CLOSE,
    TOK_FORMAT,
    TOK_FREE,
    TOK_OPEN,
    TOK_READ,
    TOK_WRITE,
    TOK_IOERR,
    TOK_NULL,

    /* ---- Directives ---- */
    TOK_COML,
    TOK_COPY,
    TOK_DELETE,
    TOK_NOPACK,
    TOK_NOREF,
    TOK_PACK,
    TOK_SEQ,
    TOK_SREF,
    TOK_WREF,

    /* ---- Special ---- */
    TOK_NEWLINE,
    TOK_COMMENT,
    TOK_EOF,
    TOK_ERROR,

    TOK_COUNT
} tok_type_t;

/* ---- Token ---- */

typedef struct {
    uint16_t type;      /* tok_type_t */
    uint16_t len;       /* source length */
    uint32_t offset;    /* offset into source buffer */
    uint32_t line;
    uint16_t col;
} token_t;

/* ---- Keyword table (sorted, for binary search) ---- */

typedef struct {
    const char *name;
    uint16_t    type;   /* tok_type_t */
} sk_kw_t;

extern const sk_kw_t sk_kwtab[];
extern const int     sk_nkw;

/* token name for debug printing */
const char *tok_name(int type);

#endif /* SKYHAWK_TOKEN_H */
