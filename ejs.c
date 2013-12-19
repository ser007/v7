// Copyright (c) 2004-2013 Sergey Lyubka <valenok@gmail.com>
// Copyright (c) 2013 Cesanta Software Limited
// All rights reserved
//
// This library is dual-licensed: you can redistribute it and/or modify
// it under the terms of the GNU General Public License version 2 as
// published by the Free Software Foundation. For the terms of this
// license, see <http://www.gnu.org/licenses/>.
//
// You are free to use this library under the terms of the GNU General
// Public License, but WITHOUT ANY WARRANTY; without even the implied
// warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
// See the GNU General Public License for more details.
//
// Alternatively, you can license this library under a commercial
// license, as set out in <http://cesanta.com/products.html>.

#include "ejs.h"

#include <sys/stat.h>
#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Linked list interface
struct ll { struct ll *prev, *next; };
#define LINKED_LIST_INIT(N)  ((N)->next = (N)->prev = (N))
#define LINKED_LIST_ENTRY(P,T,N)  ((T *)((char *)(P) - offsetof(T, N)))
#define LINKED_LIST_IS_EMPTY(N)  ((N)->next == (N))
#define LINKED_LIST_FOREACH(H,N,T) \
  for (N = (H)->next, T = (N)->next; N != (H); N = (T), T = (N)->next)
#define LINKED_LIST_ADD_TO_FRONT(H,N) do { ((H)->next)->prev = (N); \
  (N)->next = ((H)->next);  (N)->prev = (H); (H)->next = (N); } while (0)
#define LINKED_LIST_ADD_TO_TAIL(H,N) do { ((H)->prev)->next = (N); \
  (N)->prev = ((H)->prev); (N)->next = (H); (H)->prev = (N); } while (0)
#define LINKED_LIST_REMOVE(N) do { ((N)->next)->prev = ((N)->prev); \
  ((N)->prev)->next = ((N)->next); LINKED_LIST_INIT(N); } while (0)

#ifdef ENABLE_DBG
#define DBG(x) do { printf("%-20s ", __func__); printf x; putchar('\n'); \
  fflush(stdout); } while(0)
#else
#define DBG(x)
#endif

#define ARRAY_SIZE(array) (sizeof(array) / sizeof(array[0]))

// a string.
struct str {
  char *buf;        // Pointer to buffer with string data
  int len;          // String length
  int buf_size;     // Buffer size, should be equal or larger then len
};
#define EMPTY_STR { NULL, 0, 0 }

#if 0
struct tok {
  //struct ll link;           // Linkage in expression
  struct str str;           // Points to the source code
  int value;                // Token value, one of the TOK_*
  int line_no;              // Line number
  //struct tok *left;
  //struct tok *right;
  //struct tok *parent;
};

// Long tokens that are > 1 character in length
enum {
  TOK_VAR, TOK_EQUAL, TOK_TYPE_EQUAL, TOK_FUNCTION,
  TOK_IF, TOK_ELSE, TOK_ADD_EQUAL, TOK_SUB_EQUAL, TOK_MULTIPLY_EQUAL,
  TOK_DIVIDE_EQUAL, TOK_NULL, TOK_NOT, TOK_AND, TOK_OR, TOK_UNDEFINED,
  TOK_IDENTIFIER, TOK_INTEGER, TOK_FLOAT, TOK_STRING,
  TOK_END
};
#endif

// Variable types
enum { TYPE_OBJ, TYPE_INT, TYPE_DBL, TYPE_STR, TYPE_FUNC };

struct var {
  struct ll link;
  char *name;
  unsigned char type;
  union { struct str s; long i; double d; };
};

struct ejs {
  const char *source_code;  // Pointer to the source code string
  const char *cursor;       // Current parsing position
  int line_no;              // Line number
  const char *tok;          // Parsed terminal token (ident, number, string)
  int tok_len;

  struct ll symbol_table;
  jmp_buf exception_env;    // Exception environment
  char error_msg[100];      // Error message placeholder
};

static void parse_expression(struct ejs *ejs);  // Forward declaration

static void die(struct ejs *vm, const char *fmt, ...) {
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(vm->error_msg, sizeof(vm->error_msg), fmt, ap);
  va_end(ap);
  vm->error_msg[sizeof(vm->error_msg) - 1] = '\0';  // If vsnprintf fails
  DBG(("%s", vm->error_msg));

  longjmp(vm->exception_env, 1);
}

struct ejs *ejs_create(void) {
  struct ejs *ejs = NULL;

  if ((ejs = (struct ejs *) calloc(1, sizeof(*ejs))) != NULL) {
    LINKED_LIST_INIT(&ejs->symbol_table);
  }

  return ejs;
}

void ejs_destroy(struct ejs **ejs) {
  if (ejs && *ejs) {
    free(*ejs);
    *ejs = NULL;
  }
}

static int char_class(const char *s) {
  // Character classes for source code tokenization.
  // 0 means invalid character, 1: delimiters, 2: digits,
  // 3: hex digits, 4: letters
  static const unsigned char tab[256] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 0, 0, 0, //   0-15
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //  16-31
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //  32-47   !"#$%&'()*+,-./
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, //  48-62  0122456789:;<=>?
    1, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, //  63-79  @ABCDEFGHIJKLMNO
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 1, 1, 1, 1, 1, //  80-95  PQRSTUVWXYZ[\]^_
    1, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, //  96-111 `abcdefghijklmno
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 1, 1, 1, 1, 0, // 114-147 pqrstuvwzyz{|}~
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
  };
  return tab[* (unsigned char *) s];
}
static int is_alpha(const char *s) { return char_class(s) > 2;  };
static int is_alnum(const char *s) { return char_class(s) > 1;  };
static int is_digit(const char *s) { return char_class(s) == 2; };
static int is_space(const char *s) {
  return *s == ' ' || *s == '\t' || *s == '\r' || *s == '\n';
};

#if 0
// Return a pointer to the next token, or NULL
static const char *skip_to_delimiter(const char *s, int len) {
  if (s == NULL || len <= 0) return NULL;
  for (; len > 0; s++, len--) {
    if (char_class(s) == 0) return NULL;
    if (char_class(s) == 1) break;
  }
  return s;
}

// Return a pointer to the next token, or NULL
static const char *skip_to_next_token(const char *s) {
  for (; s != NULL && *s != '\0'; s++) {
    if (DELIM(s) == 0) return NULL;
    if (DELIM(s) == 1) break;
  }
  return s;
}

static int match_long_token(const char *str, int len, int *value) {
  static const struct { const char *s; int len; int value; } toks[] = {
    { "var",      3, TOK_VAR },
    { "==",       2, TOK_EQUAL },
    { "===",      2, TOK_TYPE_EQUAL },
    { "if",       2, TOK_IF },
    { "else",     4, TOK_ELSE },
    { "function", 8, TOK_FUNCTION },
    { "V",        0, TOK_IDENTIFIER },
    { "D",        0, TOK_INTEGER },
    { NULL,      -1, -1 },
  };
  int i;

  //DBG(("%s: [%.*s]", __func__, len, str));
  for (i = 0; toks[i].s != NULL; i++) {
    if (toks[i].len > 0 && len == toks[i].len && !memcmp(toks[i].s, str, len)) {
      *value = toks[i].value;
      return len;
    } else if (toks[i].s[0] == 'V' && (str[0] == '_' || is_alpha(str))) {
      int k = 1;
      while (k < len && (str[k] == '_' || is_alnum(str + k))) k++;
      *value = TOK_IDENTIFIER;
      return k;
    } else if (toks[i].s[0] == 'D' && is_digit(str)) {
      int k = 1;
      while (k < len && is_digit(str + k)) k++;
      *value = TOK_INTEGER;
      return k;
    }
  }

  *value = TOK_END;
  return 0;
}

static int resize_str(struct str *str, int increment) {
  char *p;
  if (str->buf_size < 0 || increment < 0) return 0;
  if (increment == 0) return 1;
  p = (char *) realloc(str->buf, str->buf_size + increment);
  if (p == NULL) return 0;
  str->buf = p;
  str->buf_size += increment;
  return 1;
}

// Parse the buffer, link all tokens into the list head.
// Return total number of tokens, including last TOK_END
static struct tok *tokenize(const char *s, int len) {
  const char *p, *eof = s + len;
  int num_tokens = 0, line_no = 1, increment = 1000 * sizeof(struct tok);
  struct str str = EMPTY_STR;
  struct tok *tok = NULL;

  //for (; (p = skip(s, eof - s)) != NULL; s = p + 1) {
  while (s != NULL && s <= eof) {
    if (s[0] == '\n') line_no++;
    if (s[0] == ' ' || s[0] == '\t' || s[0] == '\r' || s[0] == '\n') {
      s++;
      continue;
    }

    // Resize tokens array if necessary. We keep it in a string.
    assert(str.len <= str.buf_size);
    if (str.len >= str.buf_size && !resize_str(&str, increment)) {
      free(str.buf);
      return NULL;
    }

    // Initialize token
    tok = (struct tok *) (str.buf + num_tokens * sizeof(*tok));
    tok->str.buf = (char *) s;
    tok->line_no = line_no;
    num_tokens++;
    str.len = num_tokens * sizeof(*tok);

    // Find where next token starts
    p = skip_to_delimiter(s, eof - s);
    //DBG(("skip(%.*s) -> [%.*s]", eof - s, s, p ? p - s : eof - s, s));

    if (p == s) {
      tok->str.len = 1;
      tok->value = *s++;
    } else {
      tok->str.len = match_long_token(s, p ? p - s : eof - s, &tok->value);
      s += tok->str.len;
    }
    //DBG(("[%.*s] -> %d", tok->str.len, tok->str.buf, tok->value));
    if (tok->value == TOK_END) break;
  }

  return (struct tok *) str.buf;
}

static int get_next_token(const char *s) {
  const char *next = skip_to_next_token(s);
}
#endif

#define EXPECT(ejs, cond) do { if (!(cond)) \
  die((ejs), "[%.*s]: %s", 10, (ejs)->cursor, #cond); } while (0)
#define IS(ejs, ch) (*(ejs)->cursor == (ch))

static void skip_whitespaces_and_comments(struct ejs *ejs) {
  const char *s = ejs->cursor;
  if (is_space(s)) {
    while (*s != '\0' && is_space(s)) {
      if (*s == '\n') ejs->line_no++;
      s++;
    }
    if (s[0] == '/' && s[1] == '/') {
      s += 2;
      while (*s != '\0' && *s != '\n') s++;
    }
  }
  ejs->cursor = s;
}

static void match(struct ejs *ejs, int ch) {
  EXPECT(ejs, *ejs->cursor++ == ch);
  skip_whitespaces_and_comments(ejs);
}

static int test_and_skip(struct ejs *ejs, const char *kw, int kwlen) {
  if (memcmp(ejs->cursor, kw, kwlen) == 0) {
    ejs->cursor += kwlen;
    skip_whitespaces_and_comments(ejs);
    return 1;
  }
  return 0;
}

static int parse_num(struct ejs *ejs) {
  int result = 0;

  EXPECT(ejs, is_digit(ejs->cursor));
  ejs->tok = ejs->cursor;
  while (is_digit(ejs->cursor)) {
    result *= 10;
    result += *ejs->cursor++ - '0';
  }
  ejs->tok_len = ejs->cursor - ejs->tok;
  skip_whitespaces_and_comments(ejs);

  return result;
}

static void parse_identifier(struct ejs *ejs) {
  EXPECT(ejs, is_alpha(ejs->cursor) || *ejs->cursor == '_');
  ejs->tok = ejs->cursor;
  ejs->cursor++;
  while (is_alnum(ejs->cursor) || *ejs->cursor == '_') ejs->cursor++;
  ejs->tok_len = ejs->cursor - ejs->tok;
  skip_whitespaces_and_comments(ejs);
}

static void parse_function_call(struct ejs *ejs) {
  match(ejs, '(');
  while (*ejs->cursor != ')') {
    parse_expression(ejs);
    if (*ejs->cursor == ',') match(ejs, ',');
  }
  match(ejs, ')');
}

static void parse_factor(struct ejs *ejs) {
  if (*ejs->cursor == '(') {
    match(ejs, '(');
    parse_expression(ejs);
    match(ejs, ')');
  } else if (is_alpha(ejs->cursor)) {
    parse_identifier(ejs);
    if (*ejs->cursor == '(') {
      parse_function_call(ejs);
    }
  } else {
    parse_num(ejs);
  }
}

static void parse_term(struct ejs *ejs) {
  parse_factor(ejs);
  while (*ejs->cursor == '*' || *ejs->cursor == '/') {
    match(ejs, *ejs->cursor);
    parse_factor(ejs);
  }
}

static void parse_expression(struct ejs *ejs) {
  parse_term(ejs);
  while (*ejs->cursor == '-' || *ejs->cursor == '+') {
    match(ejs, *ejs->cursor);
    parse_term(ejs);
  }
}

static void parse_declaration(struct ejs *ejs) {
  do {
    parse_identifier(ejs);
    match(ejs, '=');
    parse_expression(ejs);
  } while (test_and_skip(ejs, ",", 1));
}

static void parse_assignment(struct ejs *ejs) {
  parse_identifier(ejs);
  match(ejs, '=');
  parse_expression(ejs);
}

static void parse_statement(struct ejs *ejs) {
  if (test_and_skip(ejs, "var", 3)) {
    parse_declaration(ejs);
  } else if (is_alpha(ejs->cursor)) {
    parse_assignment(ejs);
  } else {
    parse_expression(ejs);
  }
  match(ejs, ';');
}

//                              GRAMMAR
//
//  code        =   { statement } ;
//  statement   =   declaration | assignment | expression ";"
//  declaration =   "var" assignment [ "," {assignment} ] ";"
//  assignment  =   identifier "=" expression
//  expression  =   term { add_op term }
//  term        =   factor { mul_op factor }
//  factor      =   number | string | call | "(" expression ")" | identifier
//  call        =   identifier "(" { expression} ")"
//  mul_op      =   "*" | "/"
//  add_op      =   "+" | "-"
//  assign_op   =   "=" | "+=" | "-=" | "*=" | "/=" | "%=" | "^="
//  identifier  =   letter { letter | digit }
//  number      =   [ "-" ] { digit }
int ejs_exec(struct ejs *ejs, const char *source_code) {
  ejs->source_code = ejs->cursor = source_code;
  skip_whitespaces_and_comments(ejs);
  if (setjmp(ejs->exception_env) != 0) return 0;  // Catches exception
  while (*ejs->cursor != '\0') {
    parse_statement(ejs);
  }
  return 1;
}
