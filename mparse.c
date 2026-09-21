/*
** mparse.c - the lexer and the parser
**
** Text -> tokens -> a syntax tree (Node), one complete command at a time,
** following the bash grammar: lists, pipelines, ( ) { } if while until
** for case select, functions, (( )), [[ ]], here-documents, aliases.
** Words keep their quotes; mexpand.c takes them apart when the command
** runs. Nested $( ) is found by parsing it, so a ')' inside a case
** pattern or a quoted string cannot end it early.
*/

#include "mmc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*
** {==================================================================
** Arena: every node and string of one command, freed together
** ===================================================================
*/

typedef struct Chunk {
  struct Chunk *next;
  size_t used, size;
  /* data follows, aligned */
} Chunk;

typedef struct Arena {
  Chunk *head;
} Arena;

#define ALIGN	(sizeof(void *) > sizeof(long long) ? sizeof(void *) : sizeof(long long))


static void *a_alloc (Arena *a, size_t n) {
  Chunk *c = a->head;
  void *p;
  n = (n + ALIGN - 1) & ~(ALIGN - 1);
  if (c == NULL || c->used + n > c->size) {
    size_t size = n > 16384 ? n : 16384;
    size_t head = (sizeof(Chunk) + ALIGN - 1) & ~(ALIGN - 1);
    c = (Chunk *)xmalloc(head + size);
    c->next = a->head;
    c->used = head;
    c->size = head + size;
    a->head = c;
  }
  p = (char *)c + c->used;
  c->used += n;
  memset(p, 0, n);
  return p;
}


static char *a_strndup (Arena *a, const char *s, size_t n) {
  char *d = (char *)a_alloc(a, n + 1);
  memcpy(d, s, n);
  d[n] = '\0';
  return d;
}


static char *a_strdup (Arena *a, const char *s) {
  return a_strndup(a, s, strlen(s));
}


static Prog *prog_new (void) {
  Prog *p = (Prog *)xmalloc(sizeof(Prog));
  p->arena = (Arena *)xmalloc(sizeof(Arena));
  p->arena->head = NULL;
  p->refs = 1;
  return p;
}


void prog_ref (Prog *p) {
  if (p) p->refs++;
}


void prog_unref (Prog *p) {
  Chunk *c;
  if (p == NULL || --p->refs > 0) return;
  c = p->arena->head;
  while (c != NULL) {
    Chunk *next = c->next;
    free(c);
    c = next;
  }
  free(p->arena);
  free(p);
}

/* }================================================================== */


/*
** {==================================================================
** Tokens
** ===================================================================
*/

enum {
  T_WORD, T_NL, T_EOF, T_SEMI, T_AMP, T_AND, T_OR, T_PIPE, T_PIPEAMP,
  T_LPAREN, T_RPAREN, T_DSEMI, T_SEMIAMP, T_DSEMIAMP, T_REDIR,
  T_ERROR
};

typedef struct Tok {
  int type;
  char *text;	/* T_WORD: the raw word (arena) */
  size_t start, end;	/* offsets in the input */
  int line;
  int quoted;	/* the word has quotes or backslashes */
  int op, fd;	/* T_REDIR */
  char *fdvar;	/* T_REDIR: {name}> */
} Tok;

typedef struct Active {	/* an alias being expanded */
  char name[64];
  size_t end;
} Active;

struct Parser {
  Buf in;
  size_t pos;
  int line;
  char *name;
  Prog *prog;
  Tok tok;
  int have;	/* 'tok' holds a peeked token */
  Redir *pending[64];	/* here-docs waiting for their body */
  int npending;
  int error, incomplete, final;
  int check, errors;
  int cond;	/* lexing inside [[ ]] */
  Active act[MMC_ALIAS_DEPTH];
  int nact;
  int alias_next;	/* the last alias ended with a blank */
  int stop_rparen;	/* parsing the inside of $( ) */
  int depth;
};


static void syntax_error (Parser *p, const char *fmt, const char *arg) {
  if (p->error) return;
  p->error = 1;
  p->errors++;
  if (p->check) {
    fd_printf(2, "%s:%d: syntax error: ", p->name ? p->name : "mmc", p->tok.line ? p->tok.line : p->line);
    fd_printf(2, fmt, arg);
    fd_puts(2, "\n");
  }
  else {
    if (p->name != NULL && !sh_interactive)
      fd_printf(2, "%s: line %d: syntax error: ", p->name, p->tok.line ? p->tok.line : p->line);
    else fd_printf(2, "mmc: syntax error: ");
    fd_printf(2, fmt, arg);
    fd_puts(2, "\n");
  }
}


/* input ran out in the middle of something */
static void need_more (Parser *p, const char *what) {
  if (p->final) syntax_error(p, "unexpected end of file (%s)", what);
  else {
    p->incomplete = 1;
    p->error = 1;
  }
}


static int ch (Parser *p, size_t at) {
  return (at < p->in.len) ? (unsigned char)p->in.s[at] : '\0';
}


static int is_blank (int c) {
  return c == ' ' || c == '\t' || c == '\r';
}


static int is_meta (int c) {
  return c == '|' || c == '&' || c == ';' || c == '<' || c == '>' ||
         c == '(' || c == ')' || c == '\n' || is_blank(c) || c == '\0';
}


/* removes backslash-newline at 'at' (a line continuation) */
static int continuation (Parser *p, size_t at) {
  if (ch(p, at) == '\\' && ch(p, at + 1) == '\n') {
    memmove(p->in.s + at, p->in.s + at + 2, p->in.len - at - 2 + 1);
    p->in.len -= 2;
    p->line++;
    return 1;
  }
  return 0;
}


static void count_lines (Parser *p, size_t from, size_t to) {
  size_t i;
  for (i = from; i < to && i < p->in.len; i++)
    if (p->in.s[i] == '\n') p->line++;
}


/*
** {==================================================================
** Finding the end of $( ), ${ }, $(( )), ` `, quotes
** ===================================================================
*/

static size_t scan_dquote (Parser *p, size_t i);
static size_t scan_dollar (Parser *p, size_t i);
static Node *parse_compound_list (Parser *p);
static Parser *parser_sub (Parser *p, size_t at);
static void parser_free_sub (Parser *s);


/* i is just after the opening quote; returns the index after the closing one */
static size_t scan_squote (Parser *p, size_t i) {
  while (ch(p, i) != '\'' ) {
    if (ch(p, i) == '\0') {
      need_more(p, "matching '");
      return i;
    }
    i++;
  }
  return i + 1;
}


static size_t scan_backquote (Parser *p, size_t i) {
  for (;;) {
    int c = ch(p, i);
    if (c == '\0') {
      need_more(p, "matching `");
      return i;
    }
    if (c == '\\' && ch(p, i + 1) != '\0') i += 2;
    else if (c == '`') return i + 1;
    else i++;
  }
}


/* $'...': backslash escapes the quote */
static size_t scan_ansi (Parser *p, size_t i) {
  for (;;) {
    int c = ch(p, i);
    if (c == '\0') {
      need_more(p, "matching '");
      return i;
    }
    if (c == '\\' && ch(p, i + 1) != '\0') i += 2;
    else if (c == '\'') return i + 1;
    else i++;
  }
}


/* i is after "$(": parse the commands to find the ')' */
static size_t scan_cmdsubst (Parser *p, size_t i) {
  Parser *s = parser_sub(p, i);
  size_t end;
  parse_compound_list(s);
  if (!s->error) {
    Tok *t = &s->tok;
    if (!s->have) {
      syntax_error(s, "%s", "expected `)'");
    }
    else if (t->type != T_RPAREN) {
      if (t->type == T_EOF) need_more(s, "matching `)'");
      else syntax_error(s, "near unexpected token `%s'", t->text ? t->text : ";");
    }
  }
  /* the sub parser dropped "\<newline>" pairs from its copy: count them back */
  end = i + s->tok.end + ((p->in.len - i) - s->in.len);
  if (s->error) {
    if (s->incomplete) need_more(p, "matching `)'");
    else if (!p->error) {
      p->error = 1;
      p->errors++;
    }
  }
  /* the sub parser removed line continuations from its copy: keep ours */
  parser_free_sub(s);
  return end;
}


/* i is after "$((": the end of the arithmetic, or 0 if it is "$( (" */
static size_t scan_arith (Parser *p, size_t i) {
  int depth = 0;
  for (;;) {
    int c = ch(p, i);
    if (c == '\0') {
      need_more(p, "matching `))'");
      return i;
    }
    if (c == '\'') i = scan_squote(p, i + 1);
    else if (c == '"') i = scan_dquote(p, i + 1);
    else if (c == '`') i = scan_backquote(p, i + 1);
    else if (c == '$') i = scan_dollar(p, i);
    else if (c == '\\' && ch(p, i + 1) != '\0') i += 2;
    else if (c == '(') {
      depth++;
      i++;
    }
    else if (c == ')') {
      if (depth > 0) {
        depth--;
        i++;
      }
      else return (ch(p, i + 1) == ')') ? i + 2 : 0;
    }
    else i++;
    if (p->error) return i;
  }
}


/* i is after "${" */
static size_t scan_brace (Parser *p, size_t i) {
  int depth = 0;
  for (;;) {
    int c = ch(p, i);
    if (c == '\0') {
      need_more(p, "matching `}'");
      return i;
    }
    if (c == '\\' && ch(p, i + 1) != '\0') i += 2;
    else if (c == '\'') i = scan_squote(p, i + 1);
    else if (c == '"') i = scan_dquote(p, i + 1);
    else if (c == '`') i = scan_backquote(p, i + 1);
    else if (c == '$') i = scan_dollar(p, i);
    else if (c == '{') {
      depth++;
      i++;
    }
    else if (c == '}') {
      if (depth == 0) return i + 1;
      depth--;
      i++;
    }
    else i++;
    if (p->error) return i;
  }
}


/* i is at '$'; returns the index after what it introduces */
static size_t scan_dollar (Parser *p, size_t i) {
  int c = ch(p, i + 1);
  if (c == '(') {
    if (ch(p, i + 2) == '(') {
      size_t e = scan_arith(p, i + 3);
      if (e != 0 || p->error) return e;
    }
    return scan_cmdsubst(p, i + 2);
  }
  if (c == '{') return scan_brace(p, i + 2);
  if (c == '\'') return scan_ansi(p, i + 2);
  if (c == '"') return scan_dquote(p, i + 2);
  if (c == '[') {	/* $[ ... ]: old arithmetic */
    size_t j = i + 2;
    while (ch(p, j) != '\0' && ch(p, j) != ']') j++;
    return ch(p, j) ? j + 1 : j;
  }
  return i + 1;
}


static size_t scan_dquote (Parser *p, size_t i) {
  for (;;) {
    int c;
    continuation(p, i);
    c = ch(p, i);
    if (c == '\0') {
      need_more(p, "matching \"");
      return i;
    }
    if (c == '"') return i + 1;
    if (c == '\\' && ch(p, i + 1) != '\0') i += 2;
    else if (c == '`') i = scan_backquote(p, i + 1);
    else if (c == '$' && (ch(p, i + 1) == '"' || ch(p, i + 1) == '\'')) i++;	/* "$" */
    else if (c == '$') i = scan_dollar(p, i);
    else i++;
    if (p->error) return i;
  }
}


/* a balanced ( ... ) inside a word: extglob, a=( ... ), <( ... ) */
static size_t scan_parens (Parser *p, size_t i) {
  int depth = 1;
  for (;;) {
    int c;
    continuation(p, i);
    c = ch(p, i);
    if (c == '\0') {
      need_more(p, "matching `)'");
      return i;
    }
    if (c == '\\' && ch(p, i + 1) != '\0') i += 2;
    else if (c == '\'') i = scan_squote(p, i + 1);
    else if (c == '"') i = scan_dquote(p, i + 1);
    else if (c == '`') i = scan_backquote(p, i + 1);
    else if (c == '$') i = scan_dollar(p, i);
    else if (c == '#' && (i == 0 || is_meta(ch(p, i - 1)) || ch(p, i - 1) == '(')) {
      while (ch(p, i) != '\0' && ch(p, i) != '\n') i++;	/* a comment */
    }
    else if (c == '(') {
      depth++;
      i++;
    }
    else if (c == ')') {
      i++;
      if (--depth == 0) return i;
    }
    else i++;
    if (p->error) return i;
  }
}


/* process substitution <( ... ) inside a word: parse it like $( ) */
static size_t scan_procsubst (Parser *p, size_t i) {
  return scan_cmdsubst(p, i + 2);
}

/* }================================================================== */


/* a word from 'start'; returns its end */
static size_t scan_word (Parser *p, size_t start, int *quoted) {
  size_t i = start;
  *quoted = 0;
  for (;;) {
    int c;
    while (continuation(p, i))
      ;
    c = ch(p, i);
    if (c == '\0' || c == '\n' || is_blank(c)) break;
    if (p->cond && (c == '(' || c == ')') ) {
      if (i == start) break;
      /* x(...) in [[ ]] is an extglob pattern */
      if (c == '(' && strchr("?*+@!", ch(p, i - 1)) != NULL) {
        i = scan_parens(p, i + 1);
        continue;
      }
      break;
    }
    if (c == '(') {
      /* extglob ?( *( +( @( !( */
      if (i > start && strchr("?*+@!", ch(p, i - 1)) != NULL) {
        i = scan_parens(p, i + 1);
        continue;
      }
      /* NAME=( and NAME+=( : an array */
      if (i > start && ch(p, i - 1) == '=') {
        size_t k = start;
        while (k < i && (isalnum(ch(p, k)) || ch(p, k) == '_')) k++;
        if (k > start && !isdigit(ch(p, start)) &&
            (k == i - 1 || (k == i - 2 && ch(p, k) == '+'))) {
          i = scan_parens(p, i + 1);
          continue;
        }
      }
      break;
    }
    if ((c == '<' || c == '>') && ch(p, i + 1) == '(' && !p->cond) {
      i = scan_procsubst(p, i);
      continue;
    }
    if (c != '{' && c != '}' && is_meta(c)) {
      if (p->cond && (c == '<' || c == '>')) break;
      break;
    }
    if (c == '\\') {
      *quoted = 1;
      i += (ch(p, i + 1) != '\0') ? 2 : 1;
    }
    else if (c == '\'') {
      *quoted = 1;
      i = scan_squote(p, i + 1);
    }
    else if (c == '"') {
      *quoted = 1;
      i = scan_dquote(p, i + 1);
    }
    else if (c == '`') i = scan_backquote(p, i + 1);
    else if (c == '$') {
      if (ch(p, i + 1) == '\'' || ch(p, i + 1) == '"') *quoted = 1;
      i = scan_dollar(p, i);
    }
    else i++;
    if (p->error) return i;
  }
  return i;
}


/* the operator at i; 0 if none */
static int scan_operator (Parser *p, size_t i, Tok *t) {
  static const struct { const char *s; int type, op; } ops[] = {
    {"&>>", T_REDIR, R_BOTHAPP}, {"&>", T_REDIR, R_BOTH},
    {"&&", T_AND, 0}, {"&", T_AMP, 0},
    {"||", T_OR, 0}, {"|&", T_PIPEAMP, 0}, {"|", T_PIPE, 0},
    {";;&", T_DSEMIAMP, 0}, {";;", T_DSEMI, 0}, {";&", T_SEMIAMP, 0},
    {";", T_SEMI, 0}, {"(", T_LPAREN, 0}, {")", T_RPAREN, 0},
    {"<<<", T_REDIR, R_HERESTR}, {"<<-", T_REDIR, R_HEREDOC},
    {"<<", T_REDIR, R_HEREDOC}, {"<&", T_REDIR, R_DUPIN},
    {"<>", T_REDIR, R_RW}, {"<", T_REDIR, R_IN},
    {">>", T_REDIR, R_APPEND}, {">&", T_REDIR, R_DUPOUT},
    {">|", T_REDIR, R_CLOBBER}, {">", T_REDIR, R_OUT}
  };
  size_t k;
  for (k = 0; k < sizeof(ops) / sizeof(ops[0]); k++) {
    size_t n = strlen(ops[k].s);
    if (strncmp(p->in.s + i, ops[k].s, n) == 0 && i + n <= p->in.len) {
      if (p->cond && ops[k].type == T_REDIR) return 0;	/* < > compare in [[ ]] */
      t->type = ops[k].type;
      t->op = ops[k].op;
      t->fd = -1;
      t->text = a_strdup(p->prog->arena, ops[k].s);
      /* "<<-" strips tabs: remember it in fd for a moment */
      if (ops[k].op == R_HEREDOC && n == 3) t->fd = -2;
      return (int)n;
    }
  }
  return 0;
}


static void read_heredocs (Parser *p);


static Tok *lex (Parser *p) {
  Tok *t = &p->tok;
  size_t i;
  int n;
  if (p->have) return t;
  memset(t, 0, sizeof(*t));
  for (;;) {	/* blanks and comments */
    for (;;) {	/* a removed line continuation leaves pos where it is */
      if (continuation(p, p->pos)) continue;
      if (!is_blank(ch(p, p->pos))) break;
      p->pos++;
    }
    if (p->cond && ch(p, p->pos) == '\n') {	/* [[ ]] may span lines */
      p->pos++;
      p->line++;
      continue;
    }
    if (ch(p, p->pos) == '#') {
      while (ch(p, p->pos) != '\0' && ch(p, p->pos) != '\n') p->pos++;
    }
    break;
  }
  while (p->nact > 0 && p->pos >= p->act[p->nact - 1].end) p->nact--;
  i = p->pos;
  t->start = i;
  t->line = p->line;
  p->have = 1;
  if (ch(p, i) == '\0') {
    t->type = T_EOF;
    t->end = i;
    return t;
  }
  if (ch(p, i) == '\n') {
    t->type = T_NL;
    p->pos = t->end = i + 1;
    p->line++;
    if (p->npending > 0) read_heredocs(p);
    return t;
  }
  /* {name}> and 2> : a redirection with a file descriptor */
  if (isdigit(ch(p, i)) || ch(p, i) == '{') {
    size_t j = i;
    if (ch(p, i) == '{') {
      j++;
      while (isalnum(ch(p, j)) || ch(p, j) == '_') j++;
      if (ch(p, j) == '[' && j > i + 1) {	/* {arr[1]}>: an element */
        size_t k = j + 1;
        while (ch(p, k) != '\0' && ch(p, k) != ']' && ch(p, k) != '\n' && ch(p, k) != '}') k++;
        if (ch(p, k) == ']') j = k + 1;
      }
      if (ch(p, j) == '}' && j > i + 1 && (ch(p, j + 1) == '<' || ch(p, j + 1) == '>') &&
          !p->cond) {
        Tok op;
        memset(&op, 0, sizeof(op));
        if ((n = scan_operator(p, j + 1, &op)) > 0 && op.type == T_REDIR) {
          *t = op;
          t->start = i;
          t->line = p->line;
          t->fdvar = a_strndup(p->prog->arena, p->in.s + i + 1, j - i - 1);
          p->pos = t->end = j + 1 + (size_t)n;
          return t;
        }
      }
    }
    else {
      while (isdigit(ch(p, j))) j++;
      if ((ch(p, j) == '<' || ch(p, j) == '>') && !p->cond) {
        Tok op;
        memset(&op, 0, sizeof(op));
        if ((n = scan_operator(p, j, &op)) > 0 && op.type == T_REDIR &&
            op.op != R_BOTH && op.op != R_BOTHAPP) {
          int heredoc_dash = (op.fd == -2);
          *t = op;
          t->start = i;
          t->line = p->line;
          t->fd = atoi(p->in.s + i);
          if (heredoc_dash) t->fd = -2 - t->fd;	/* keep both facts */
          p->pos = t->end = j + (size_t)n;
          return t;
        }
      }
    }
  }
  if (!(ch(p, i) == '<' && ch(p, i + 1) == '(') &&
      !(ch(p, i) == '>' && ch(p, i + 1) == '(') &&
      (n = scan_operator(p, i, t)) > 0) {
    t->start = i;
    t->line = p->line;
    p->pos = t->end = i + (size_t)n;
    return t;
  }
  {
    size_t e = scan_word(p, i, &t->quoted);
    int line = p->line;
    if (p->error) {
      t->type = T_ERROR;
      return t;
    }
    t->type = T_WORD;
    t->start = i;
    t->end = e;
    t->line = line;
    t->text = a_strndup(p->prog->arena, p->in.s + i, e - i);
    count_lines(p, i, e);
    p->pos = e;
    if (e == i) {	/* a lone character nothing else took */
      t->text = a_strndup(p->prog->arena, p->in.s + i, 1);
      p->pos = t->end = i + 1;
    }
  }
  return t;
}


static void consume (Parser *p) {
  p->have = 0;
}


static int is_word (Tok *t, const char *kw) {
  return t->type == T_WORD && !t->quoted && strcmp(t->text, kw) == 0;
}


int parse_is_keyword (const char *w) {
  static const char *const kws[] = {
    "if", "then", "elif", "else", "fi", "while", "until", "do", "done",
    "for", "in", "case", "esac", "select", "function", "time", "{", "}",
    "!", "[[", "]]", "coproc", NULL
  };
  int i;
  for (i = 0; kws[i]; i++)
    if (strcmp(kws[i], w) == 0) return 1;
  return 0;
}

/* }================================================================== */


/*
** {==================================================================
** Here-documents
** ===================================================================
*/

/* the delimiter without its quotes; tells whether it had any */
static char *heredoc_delim (Parser *p, const char *raw, int *quoted) {
  Buf b;
  const char *s;
  *quoted = 0;
  buf_init(&b);
  for (s = raw; *s; s++) {
    if (*s == '\'' || *s == '"') *quoted = 1;
    else if (*s == '\\' && s[1] != '\0') {
      *quoted = 1;
      buf_putc(&b, *++s);
    }
    else if (*s == '$' && (s[1] == '\'' || s[1] == '"')) *quoted = 1;
    else buf_putc(&b, *s);
  }
  {
    char *r = a_strdup(p->prog->arena, b.s ? b.s : "");
    buf_free(&b);
    return r;
  }
}


static void read_heredocs (Parser *p) {
  int k;
  for (k = 0; k < p->npending; k++) {
    Redir *r = p->pending[k];
    int strip = r->fd <= -2;	/* <<- */
    Buf body;
    buf_init(&body);
    if (strip) r->fd = (r->fd == -2) ? -1 : -2 - r->fd;
    for (;;) {
      size_t ls = p->pos, le;
      const char *line;
      if (ch(p, ls) == '\0') {	/* end of input before the delimiter */
        if (!p->final) {
          p->incomplete = 1;
          p->error = 1;
          buf_free(&body);
          return;
        }
        fd_printf(2, "mmc: warning: here-document at line %d delimited by "
                     "end-of-file (wanted `%s')\n", p->line, r->word);
        break;
      }
      le = ls;
      while (ch(p, le) != '\0' && ch(p, le) != '\n') le++;
      line = p->in.s + ls;
      if (strip)
        while (*line == '\t') line++;
      p->pos = (ch(p, le) == '\n') ? le + 1 : le;
      p->line++;
      {
        size_t n = (size_t)(p->in.s + le - line);
        if (n > 0 && line[n - 1] == '\r') n--;	/* CRLF scripts */
        if (n == strlen(r->word) && strncmp(line, r->word, n) == 0) break;
        buf_putn(&body, line, (size_t)(p->in.s + le - line));
        buf_putc(&body, '\n');
      }
    }
    r->here = a_strdup(p->prog->arena, body.s ? body.s : "");
    buf_free(&body);
  }
  p->npending = 0;
}

/* }================================================================== */


/*
** {==================================================================
** Aliases: the alias text replaces the word in the input
** ===================================================================
*/

static int alias_active (Parser *p, const char *name) {
  int i;
  for (i = 0; i < p->nact; i++)
    if (strcmp(p->act[i].name, name) == 0) return 1;
  return 0;
}


/* the peeked token is a word in command position: expand it if it is an alias */
static int try_alias (Parser *p) {
  Tok *t = lex(p);
  const char *val;
  size_t vlen, wlen;
  int i;
  if (t->type != T_WORD || t->quoted || p->nact >= MMC_ALIAS_DEPTH) return 0;
  if (!O("expand_aliases") || p->check) return 0;
  if ((val = alias_get(t->text)) == NULL || alias_active(p, t->text)) return 0;
  if (strlen(t->text) >= sizeof(p->act[0].name)) return 0;
  vlen = strlen(val);
  wlen = t->end - t->start;
  /* replace the word by the value in the input */
  {
    Buf nb;
    buf_init(&nb);
    buf_putn(&nb, p->in.s, t->start);
    buf_putn(&nb, val, vlen);
    buf_putn(&nb, p->in.s + t->end, p->in.len - t->end);
    buf_free(&p->in);
    p->in = nb;
    if (p->in.s == NULL) buf_putc(&p->in, '\0'), p->in.len = 0;
  }
  for (i = 0; i < p->nact; i++)
    if (p->act[i].end > t->start) p->act[i].end = p->act[i].end + vlen - wlen;
  strcpy(p->act[p->nact].name, t->text);
  p->act[p->nact].end = t->start + vlen;
  p->nact++;
  p->alias_next = vlen > 0 && is_blank((unsigned char)val[vlen - 1]);
  p->pos = t->start;
  p->line = t->line;
  p->have = 0;
  return 1;
}

/* }================================================================== */


/*
** {==================================================================
** The grammar
** ===================================================================
*/

static Node *parse_command (Parser *p);
static Node *parse_and_or (Parser *p);
static Node *parse_pipeline (Parser *p);


static Node *new_node (Parser *p, int type, int line) {
  Node *n = (Node *)a_alloc(p->prog->arena, sizeof(Node));
  n->type = type;
  n->line = line;
  n->prog = p->prog;
  return n;
}


/* a growing array of pointers, copied into the arena at the end */
typedef struct PList {
  void **v;
  int n, cap;
} PList;


static void pl_push (PList *l, void *x) {
  if (l->n + 1 > l->cap) {
    l->cap = l->cap ? l->cap * 2 : 8;
    l->v = (void **)xrealloc(l->v, (size_t)l->cap * sizeof(void *));
  }
  l->v[l->n++] = x;
}


static void *pl_take (Parser *p, PList *l, size_t elem) {
  void **r = (void **)a_alloc(p->prog->arena, (size_t)(l->n + 1) * elem);
  if (l->n > 0) memcpy(r, l->v, (size_t)l->n * sizeof(void *));
  free(l->v);
  l->v = NULL;
  l->cap = 0;
  return r;
}


static const char *tok_name (Tok *t) {
  switch (t->type) {
    case T_EOF: return "end of file";
    case T_NL: return "newline";
    default: return t->text ? t->text : "?";
  }
}


static void unexpected (Parser *p) {
  Tok *t = lex(p);
  if (p->error) return;
  if (t->type == T_EOF) need_more(p, "unexpected end of file");
  else syntax_error(p, "near unexpected token `%s'", tok_name(t));
}


static void skip_newlines (Parser *p) {
  while (!p->error && lex(p)->type == T_NL) consume(p);
}


/* expects the keyword; reports what was found instead */
static int expect_word (Parser *p, const char *kw) {
  Tok *t = lex(p);
  if (p->error) return 0;
  if (is_word(t, kw)) {
    consume(p);
    return 1;
  }
  if (t->type == T_EOF) {
    char what[64];
    sprintf(what, "expected `%s'", kw);
    need_more(p, what);
  }
  else {
    char what[128];
    sprintf(what, "near unexpected token `%.60s' (expected `%s')", tok_name(t), kw);
    syntax_error(p, "%s", what);
  }
  return 0;
}


static int is_terminator (Parser *p) {
  static const char *const ends[] = {
    "then", "else", "elif", "fi", "do", "done", "esac", "}", NULL
  };
  Tok *t = lex(p);
  int i;
  if (t->type == T_EOF || t->type == T_RPAREN || t->type == T_DSEMI ||
      t->type == T_SEMIAMP || t->type == T_DSEMIAMP)
    return 1;
  if (t->type != T_WORD || t->quoted) return 0;
  for (i = 0; ends[i]; i++)
    if (strcmp(t->text, ends[i]) == 0) return 1;
  return 0;
}


/* commands separated by ; & or newlines, up to a closing keyword */
static Node *parse_compound_list (Parser *p) {
  PList items = {NULL, 0, 0};
  Node *n;
  int line = p->line;
  for (;;) {
    Node *item;
    Tok *t;
    skip_newlines(p);
    if (p->error || is_terminator(p)) break;
    item = parse_and_or(p);
    if (p->error || item == NULL) break;
    pl_push(&items, item);
    t = lex(p);
    if (t->type == T_SEMI || t->type == T_NL) consume(p);
    else if (t->type == T_AMP) {
      item->flags |= NF_BG;
      consume(p);
    }
    else break;
  }
  if (p->error) {
    free(items.v);
    return NULL;
  }
  if (items.n == 0) {	/* "if then" and friends */
    free(items.v);
    if (!p->stop_rparen || lex(p)->type != T_RPAREN) {
      unexpected(p);
      return NULL;
    }
    n = new_node(p, N_LIST, line);
    return n;
  }
  if (items.n == 1 && !(((Node *)items.v[0])->flags & NF_BG)) {
    n = (Node *)items.v[0];
    free(items.v);
    return n;
  }
  n = new_node(p, N_LIST, line);
  n->nkids = items.n;
  n->kids = (Node **)pl_take(p, &items, sizeof(Node *));
  return n;
}


static Redir *parse_redirect (Parser *p) {
  Tok *t = lex(p);
  Redir *r = (Redir *)a_alloc(p->prog->arena, sizeof(Redir));
  Tok *w;
  r->fd = t->fd;
  r->op = t->op;
  r->fdvar = t->fdvar;
  consume(p);
  w = lex(p);
  if (w->type != T_WORD) {
    if (!p->error) {
      if (w->type == T_EOF || w->type == T_NL) {
        if (w->type == T_EOF) need_more(p, "redirection needs a file name");
        else syntax_error(p, "near unexpected token `%s'", tok_name(w));
      }
      else syntax_error(p, "near unexpected token `%s'", tok_name(w));
    }
    return NULL;
  }
  if (r->op == R_HEREDOC) {
    if (p->npending >= (int)(sizeof(p->pending) / sizeof(p->pending[0]))) {
      syntax_error(p, "%s", "too many here-documents");
      return NULL;
    }
    r->word = heredoc_delim(p, w->text, &r->here_quoted);
    p->pending[p->npending++] = r;
  }
  else r->word = w->text;
  consume(p);
  return r;
}


static void add_redir (Redir **list, Redir *r) {
  while (*list != NULL) list = &(*list)->next;
  *list = r;
}


/* redirections after a compound command */
static void parse_redirects (Parser *p, Node *n) {
  while (!p->error && lex(p)->type == T_REDIR) {
    Redir *r = parse_redirect(p);
    if (r != NULL) add_redir(&n->redir, r);
  }
}


/* NAME=, NAME+=, NAME[sub]= at the start of the word */
static int looks_assign (const char *w) {
  const char *s = w;
  if (!(isalpha((unsigned char)*s) || *s == '_')) return 0;
  while (isalnum((unsigned char)*s) || *s == '_') s++;
  if (*s == '[') {
    int depth = 0;
    for (; *s; s++) {
      if (*s == '[') depth++;
      else if (*s == ']' && --depth == 0) break;
    }
    if (*s != ']') return 0;
    s++;
  }
  if (*s == '+') s++;
  return *s == '=';
}


static Node *parse_simple (Parser *p) {
  PList words = {NULL, 0, 0}, assigns = {NULL, 0, 0};
  Node *n = new_node(p, N_SIMPLE, lex(p)->line);
  size_t start = lex(p)->start;
  for (;;) {
    Tok *t = lex(p);
    if (p->error) break;
    if (t->type == T_REDIR) {
      Redir *r = parse_redirect(p);
      if (r != NULL) add_redir(&n->redir, r);
      continue;
    }
    if (t->type != T_WORD) break;
    if (words.n > 0 && p->alias_next) {	/* "alias sudo='sudo '" */
      p->alias_next = 0;
      if (try_alias(p)) continue;
    }
    if (words.n == 0 && looks_assign(t->text))
      pl_push(&assigns, t->text);
    else pl_push(&words, t->text);
    consume(p);
  }
  if (p->error) {
    free(words.v);
    free(assigns.v);
    return NULL;
  }
  n->nwords = words.n;
  n->words = (char **)pl_take(p, &words, sizeof(char *));
  n->nassigns = assigns.n;
  n->assigns = (char **)pl_take(p, &assigns, sizeof(char *));
  n->src = a_strndup(p->prog->arena, p->in.s + start,
                     (p->have ? p->tok.start : p->pos) - start);
  if (n->nwords == 0 && n->nassigns == 0 && n->redir == NULL) {
    unexpected(p);
    return NULL;
  }
  return n;
}


/* ( ... ) or (( ... )) */
static Node *parse_paren (Parser *p) {
  Tok *t = lex(p);
  size_t at = t->start;
  int line = t->line;
  Node *n;
  if (ch(p, at + 1) == '(') {	/* (( arithmetic )) */
    size_t save_pos = p->pos;
    int save_line = p->line;
    size_t e;
    consume(p);
    e = scan_arith(p, at + 2);
    if (p->error) return NULL;
    if (e != 0) {
      n = new_node(p, N_ARITH, line);
      n->str = a_strndup(p->prog->arena, p->in.s + at + 2, e - 2 - (at + 2));
      count_lines(p, at + 2, e);
      p->pos = e;
      return n;
    }
    p->pos = save_pos;	/* "( (" : two subshells */
    p->line = save_line;
  }
  else consume(p);
  {
    int save = p->stop_rparen;
    p->stop_rparen = 1;
    n = new_node(p, N_SUBSHELL, line);
    n->a = parse_compound_list(p);
    p->stop_rparen = save;
  }
  if (p->error) return NULL;
  if (lex(p)->type != T_RPAREN) {
    unexpected(p);
    return NULL;
  }
  consume(p);
  return n;
}


static Node *parse_group (Parser *p) {
  Node *n = new_node(p, N_GROUP, lex(p)->line);
  consume(p);	/* { */
  n->a = parse_compound_list(p);
  if (p->error || !expect_word(p, "}")) return NULL;
  return n;
}


static Node *parse_if (Parser *p) {
  Node *n = new_node(p, N_IF, lex(p)->line);
  consume(p);	/* if or elif */
  n->a = parse_compound_list(p);
  if (p->error || !expect_word(p, "then")) return NULL;
  n->b = parse_compound_list(p);
  if (p->error) return NULL;
  if (is_word(lex(p), "elif")) {
    n->c = parse_if(p);	/* the elif ends with our fi */
    return p->error ? NULL : n;
  }
  if (is_word(lex(p), "else")) {
    consume(p);
    n->c = parse_compound_list(p);
    if (p->error) return NULL;
  }
  if (!expect_word(p, "fi")) return NULL;
  return n;
}


/* do ... done, or { ... } */
static Node *parse_do (Parser *p) {
  Node *body;
  skip_newlines(p);
  if (p->error) return NULL;
  if (is_word(lex(p), "{")) return parse_group(p);
  if (!expect_word(p, "do")) return NULL;
  body = parse_compound_list(p);
  if (p->error || !expect_word(p, "done")) return NULL;
  return body;
}


static Node *parse_while (Parser *p, int type) {
  Node *n = new_node(p, type, lex(p)->line);
  consume(p);
  n->a = parse_compound_list(p);
  if (p->error || !expect_word(p, "do")) return NULL;
  n->b = parse_compound_list(p);
  if (p->error || !expect_word(p, "done")) return NULL;
  return n;
}


/* for NAME [in WORDS] ; do ... done   and   for (( ; ; )) */
static Node *parse_for (Parser *p, int type) {
  Tok *t;
  Node *n = new_node(p, type, lex(p)->line);
  consume(p);	/* for / select */
  t = lex(p);
  if (type == N_FOR && t->type == T_LPAREN && ch(p, t->start + 1) == '(') {
    size_t at = t->start, e;
    consume(p);
    e = scan_arith(p, at + 2);
    if (p->error) return NULL;
    if (e == 0) {
      syntax_error(p, "%s", "bad for (( )) loop");
      return NULL;
    }
    {	/* split at the two top level ';' */
      const char *s = p->in.s + at + 2;
      size_t len = e - 2 - (at + 2), i, k = 0, from = 0;
      int depth = 0;
      n->type = N_ARITHFOR;
      for (i = 0; i <= len; i++) {
        char c = (i < len) ? s[i] : ';';
        if (c == '(') depth++;
        else if (c == ')') depth--;
        else if (c == ';' && depth == 0 && k < 3) {
          n->arith[k++] = a_strndup(p->prog->arena, s + from, i - from);
          from = i + 1;
        }
      }
      if (k != 3) {
        syntax_error(p, "%s", "for (( )) needs two `;'");
        return NULL;
      }
    }
    count_lines(p, at, e);
    p->pos = e;
    if (lex(p)->type == T_SEMI) consume(p);
    n->b = parse_do(p);
    return p->error ? NULL : n;
  }
  if (t->type != T_WORD || !is_name(t->text)) {
    unexpected(p);
    return NULL;
  }
  n->str = t->text;
  consume(p);
  skip_newlines(p);
  t = lex(p);
  if (is_word(t, "in")) {
    PList words = {NULL, 0, 0};
    n->has_in = 1;
    consume(p);
    for (;;) {
      t = lex(p);
      if (p->error || t->type != T_WORD) break;
      pl_push(&words, t->text);
      consume(p);
    }
    n->nwords = words.n;
    n->words = (char **)pl_take(p, &words, sizeof(char *));
    if (p->error) return NULL;
    if (t->type == T_SEMI || t->type == T_NL) consume(p);
    else {
      unexpected(p);
      return NULL;
    }
  }
  else if (t->type == T_SEMI) consume(p);
  n->b = parse_do(p);
  return p->error ? NULL : n;
}


static Node *parse_case (Parser *p) {
  Node *n = new_node(p, N_CASE, lex(p)->line);
  CaseItem **tail = &n->items;
  Tok *t;
  consume(p);	/* case */
  t = lex(p);
  if (t->type != T_WORD) {
    unexpected(p);
    return NULL;
  }
  n->str = t->text;
  consume(p);
  skip_newlines(p);
  if (!expect_word(p, "in")) return NULL;
  for (;;) {
    PList pats = {NULL, 0, 0};
    CaseItem *it;
    skip_newlines(p);
    t = lex(p);
    if (p->error) return NULL;
    if (is_word(t, "esac")) {
      consume(p);
      break;
    }
    if (t->type == T_LPAREN) {
      consume(p);
      t = lex(p);
    }
    for (;;) {	/* pattern | pattern ... ) */
      t = lex(p);
      if (t->type != T_WORD) {
        free(pats.v);
        unexpected(p);
        return NULL;
      }
      pl_push(&pats, t->text);
      consume(p);
      t = lex(p);
      if (t->type == T_PIPE) {
        consume(p);
        continue;
      }
      if (t->type == T_RPAREN) {
        consume(p);
        break;
      }
      free(pats.v);
      unexpected(p);
      return NULL;
    }
    it = (CaseItem *)a_alloc(p->prog->arena, sizeof(CaseItem));
    it->npats = pats.n;
    it->pats = (char **)pl_take(p, &pats, sizeof(char *));
    skip_newlines(p);
    t = lex(p);
    if (!(t->type == T_DSEMI || t->type == T_SEMIAMP || t->type == T_DSEMIAMP ||
          is_word(t, "esac"))) {
      it->body = parse_compound_list(p);
      if (p->error) return NULL;
    }
    t = lex(p);
    if (t->type == T_DSEMI) consume(p);
    else if (t->type == T_SEMIAMP) {
      it->term = 1;
      consume(p);
    }
    else if (t->type == T_DSEMIAMP) {
      it->term = 2;
      consume(p);
    }
    else if (!is_word(t, "esac")) {
      unexpected(p);
      return NULL;
    }
    *tail = it;
    tail = &it->next;
  }
  return n;
}


/*
** [[ expression ]]
**   or: and ('||' and)*    and: not ('&&' not)*    not: '!' not | prim
**   prim: '(' or ')' | unary-op word | word binary-op word | word
*/
static Node *cond_or (Parser *p);


static Node *cond_word_node (Parser *p, int kind, const char *op, char *a, char *b) {
  Node *n = new_node(p, N_COND, p->line);
  n->kind = kind;
  n->str = op ? a_strdup(p->prog->arena, op) : NULL;
  n->words = (char **)a_alloc(p->prog->arena, 3 * sizeof(char *));
  n->words[0] = a;
  n->words[1] = b;
  n->nwords = b ? 2 : 1;
  return n;
}


static int is_cond_unary (const char *s) {
  return s[0] == '-' && s[1] != '\0' && s[2] == '\0' &&
         strchr("abcdefghklnoprstuvwxzGLNOS", s[1]) != NULL;
}


static int is_cond_binary (const char *s) {
  static const char *const ops[] = {
    "==", "=", "!=", "=~", "<", ">", "-eq", "-ne", "-lt", "-le", "-gt",
    "-ge", "-nt", "-ot", "-ef", NULL
  };
  int i;
  for (i = 0; ops[i]; i++)
    if (strcmp(s, ops[i]) == 0) return 1;
  return 0;
}


/* the right side of =~ : everything up to a blank, ( ) | allowed */
static char *cond_regex (Parser *p) {
  size_t i, start;
  int depth = 0;
  Tok *t = lex(p);	/* only to skip blanks */
  if (p->error) return NULL;
  start = i = t->start;
  p->have = 0;
  for (;;) {
    int c = ch(p, i);
    if (c == '\0' || c == '\n' || ((c == ' ' || c == '\t') && depth == 0)) break;
    if (c == '\\' && ch(p, i + 1) != '\0') i += 2;
    else if (c == '\'') i = scan_squote(p, i + 1);
    else if (c == '"') i = scan_dquote(p, i + 1);
    else if (c == '$') i = scan_dollar(p, i);
    else if (c == '(') {
      depth++;
      i++;
    }
    else if (c == ')') {
      if (depth == 0) break;
      depth--;
      i++;
    }
    else if (c == ']' && ch(p, i + 1) == ']' && depth == 0 &&
             (is_blank(ch(p, i + 2)) || ch(p, i + 2) == '\0' || ch(p, i + 2) == '\n' ||
              ch(p, i + 2) == ';'))
      break;
    else i++;
    if (p->error) return NULL;
  }
  p->pos = i;
  if (i == start) {
    syntax_error(p, "%s", "=~ needs a regular expression");
    return NULL;
  }
  return a_strndup(p->prog->arena, p->in.s + start, i - start);
}


static Node *cond_prim (Parser *p) {
  Tok *t = lex(p);
  char *a;
  if (p->error) return NULL;
  if (t->type == T_LPAREN) {
    Node *n;
    consume(p);
    n = cond_or(p);
    if (p->error) return NULL;
    if (lex(p)->type != T_RPAREN) {
      unexpected(p);
      return NULL;
    }
    consume(p);
    return n;
  }
  if (t->type != T_WORD || is_word(t, "]]")) {
    unexpected(p);
    return NULL;
  }
  if (!t->quoted && is_cond_unary(t->text)) {
    char *op = t->text;
    Tok *w;
    consume(p);
    w = lex(p);
    if (w->type == T_WORD && !is_word(w, "]]")) {
      Node *n = cond_word_node(p, C_UNARY, op, w->text, NULL);
      consume(p);
      return n;
    }
    return cond_word_node(p, C_WORD, NULL, op, NULL);	/* "-n" alone */
  }
  a = t->text;
  consume(p);
  t = lex(p);
  if (t->type == T_WORD && !t->quoted && is_cond_binary(t->text)) {
    char *op = t->text;
    consume(p);
    if (strcmp(op, "=~") == 0) {
      char *re = cond_regex(p);
      return re ? cond_word_node(p, C_BINARY, op, a, re) : NULL;
    }
    t = lex(p);
    if (t->type != T_WORD) {
      unexpected(p);
      return NULL;
    }
    consume(p);
    return cond_word_node(p, C_BINARY, op, a, t->text);
  }
  return cond_word_node(p, C_WORD, NULL, a, NULL);
}


static Node *cond_not (Parser *p) {
  Tok *t = lex(p);
  if (is_word(t, "!")) {
    Node *n = new_node(p, N_COND, t->line);
    consume(p);
    n->kind = C_NOT;
    n->a = cond_not(p);
    return p->error ? NULL : n;
  }
  return cond_prim(p);
}


static Node *cond_and (Parser *p) {
  Node *l = cond_not(p);
  while (!p->error && lex(p)->type == T_AND) {
    Node *n = new_node(p, N_COND, p->line);
    consume(p);
    n->kind = C_AND;
    n->a = l;
    n->b = cond_not(p);
    l = n;
  }
  return p->error ? NULL : l;
}


static Node *cond_or (Parser *p) {
  Node *l = cond_and(p);
  while (!p->error && lex(p)->type == T_OR) {
    Node *n = new_node(p, N_COND, p->line);
    consume(p);
    n->kind = C_OR;
    n->a = l;
    n->b = cond_and(p);
    l = n;
  }
  return p->error ? NULL : l;
}


static Node *parse_cond (Parser *p) {
  Node *n;
  int line = lex(p)->line;
  consume(p);	/* [[ */
  p->cond = 1;
  n = cond_or(p);
  if (!p->error && !is_word(lex(p), "]]")) unexpected(p);
  p->cond = 0;
  if (p->error) return NULL;
  consume(p);
  if (n != NULL) n->line = line;
  return n;
}


/* name ( ) body   or   function name [()] body */
static Node *parse_function (Parser *p, char *name, size_t start, int line) {
  Node *f = new_node(p, N_FUNC, line), *body;
  Tok *t;
  skip_newlines(p);
  t = lex(p);
  if (p->error) return NULL;
  f->str = name;
  if (t->type == T_WORD && !t->quoted &&
      (strcmp(t->text, "{") == 0 || strcmp(t->text, "if") == 0 ||
       strcmp(t->text, "while") == 0 || strcmp(t->text, "until") == 0 ||
       strcmp(t->text, "for") == 0 || strcmp(t->text, "case") == 0 ||
       strcmp(t->text, "[[") == 0 || strcmp(t->text, "select") == 0))
    body = parse_command(p);
  else if (t->type == T_LPAREN) body = parse_command(p);
  else {
    unexpected(p);
    return NULL;
  }
  if (p->error || body == NULL) return NULL;
  f->a = body;
  f->src = a_strndup(p->prog->arena, p->in.s + start,
                     (p->have ? p->tok.start : p->pos) - start);
  return f;
}


static Node *parse_command (Parser *p) {
  Tok *t;
  Node *n = NULL;
  size_t start;
  int line;
  if (++p->depth > 500) {
    syntax_error(p, "%s", "nested too deeply");
    return NULL;
  }
  while (try_alias(p))
    ;
  t = lex(p);
  start = t->start;
  line = t->line;
  if (p->error) {
    p->depth--;
    return NULL;
  }
  if (t->type == T_LPAREN) n = parse_paren(p);
  else if (t->type == T_WORD && !t->quoted) {
    const char *w = t->text;
    if (strcmp(w, "{") == 0) n = parse_group(p);
    else if (strcmp(w, "if") == 0) n = parse_if(p);
    else if (strcmp(w, "while") == 0) n = parse_while(p, N_WHILE);
    else if (strcmp(w, "until") == 0) n = parse_while(p, N_UNTIL);
    else if (strcmp(w, "for") == 0) n = parse_for(p, N_FOR);
    else if (strcmp(w, "select") == 0) n = parse_for(p, N_SELECT);
    else if (strcmp(w, "case") == 0) n = parse_case(p);
    else if (strcmp(w, "[[") == 0) n = parse_cond(p);
    else if (strcmp(w, "function") == 0) {
      Tok *nt;
      consume(p);
      nt = lex(p);
      if (nt->type != T_WORD) {
        unexpected(p);
        p->depth--;
        return NULL;
      }
      {
        char *name = nt->text;
        consume(p);
        if (lex(p)->type == T_LPAREN) {
          consume(p);
          if (lex(p)->type != T_RPAREN) {
            unexpected(p);
            p->depth--;
            return NULL;
          }
          consume(p);
        }
        n = parse_function(p, name, start, line);
      }
      if (n) parse_redirects(p, n->a);
      p->depth--;
      return p->error ? NULL : n;
    }
    else if (strcmp(w, "coproc") == 0) {
      consume(p);
      n = new_node(p, N_COPROC, line);
      if (lex(p)->type == T_WORD && is_name(lex(p)->text) && !parse_is_keyword(lex(p)->text)) {
        /* coproc NAME compound: a name only counts before a compound command */
        static const char *const compound[] = {"{", "(", "while", "until", "for", "if",
                                               "case", "select", "[[", "((", NULL};
        const char *after = p->in.s + p->pos;
        int k;
        while (*after == ' ' || *after == '\t') after++;
        for (k = 0; compound[k] != NULL; k++) {
          size_t cl = strlen(compound[k]);
          if (strncmp(after, compound[k], cl) == 0 &&
              (cl == 1 || compound[k][0] == '[' || compound[k][0] == '(' ||
               after[cl] == ' ' || after[cl] == '\t' || after[cl] == '\n' || after[cl] == ';'))
            break;
        }
        if (compound[k] != NULL) {
          n->str = a_strdup(p->prog->arena, lex(p)->text);
          consume(p);
        }
      }
      n->a = parse_command(p);
      p->depth--;
      return p->error ? NULL : n;
    }
    else if (strcmp(w, "then") == 0 || strcmp(w, "else") == 0 ||
             strcmp(w, "elif") == 0 || strcmp(w, "fi") == 0 ||
             strcmp(w, "do") == 0 || strcmp(w, "done") == 0 ||
             strcmp(w, "esac") == 0 || strcmp(w, "}") == 0 ||
             strcmp(w, "in") == 0 || strcmp(w, "]]") == 0) {
      unexpected(p);
      p->depth--;
      return NULL;
    }
  }
  if (n == NULL && !p->error) {
    if (t->type != T_WORD && t->type != T_REDIR) {
      unexpected(p);
      p->depth--;
      return NULL;
    }
    n = parse_simple(p);
    /* name ( ) { ...; } */
    if (n != NULL && n->nwords == 1 && n->nassigns == 0 && n->redir == NULL &&
        lex(p)->type == T_LPAREN) {
      consume(p);
      if (lex(p)->type != T_RPAREN) {
        unexpected(p);
        p->depth--;
        return NULL;
      }
      consume(p);
      n = parse_function(p, n->words[0], start, line);
      if (n) parse_redirects(p, n->a);
      p->depth--;
      return p->error ? NULL : n;
    }
    p->depth--;
    return p->error ? NULL : n;
  }
  if (n != NULL && !p->error) {
    parse_redirects(p, n);
    n->src = a_strndup(p->prog->arena, p->in.s + start,
                       (p->have ? p->tok.start : p->pos) - start);
  }
  p->depth--;
  return p->error ? NULL : n;
}


static Node *parse_pipeline (Parser *p) {
  PList stages = {NULL, 0, 0};
  Node *n;
  Tok *t = lex(p);
  int flags = 0, line = t->line;
  size_t start = t->start;
  if (is_word(t, "time")) {
    consume(p);
    flags |= NF_TIME;
    if (is_word(lex(p), "-p")) {
      consume(p);
      flags |= NF_TIMEP;
    }
    t = lex(p);
    if (t->type == T_NL || t->type == T_EOF || t->type == T_SEMI) {	/* "time" alone */
      n = new_node(p, N_PIPE, line);
      n->flags = flags;
      n->kids = (Node **)a_alloc(p->prog->arena, sizeof(Node *));
      return n;
    }
  }
  while (is_word(lex(p), "!")) {
    consume(p);
    flags ^= NF_NEGATE;
  }
  for (;;) {
    Node *c = parse_command(p);
    if (p->error || c == NULL) {
      free(stages.v);
      return NULL;
    }
    pl_push(&stages, c);
    t = lex(p);
    if (t->type == T_PIPE || t->type == T_PIPEAMP) {
      if (t->type == T_PIPEAMP) c->flags |= NF_ERRPIPE;
      consume(p);
      skip_newlines(p);
      if (p->error) {
        free(stages.v);
        return NULL;
      }
      continue;
    }
    break;
  }
  if (stages.n == 1 && flags == 0) {
    n = (Node *)stages.v[0];
    free(stages.v);
    return n;
  }
  n = new_node(p, N_PIPE, line);
  n->flags = flags;
  n->nkids = stages.n;
  n->kids = (Node **)pl_take(p, &stages, sizeof(Node *));
  n->src = a_strndup(p->prog->arena, p->in.s + start,
                     (p->have ? p->tok.start : p->pos) - start);
  return n;
}


static Node *parse_and_or (Parser *p) {
  size_t start = lex(p)->start;
  Node *l = parse_pipeline(p);
  while (!p->error && l != NULL) {
    Tok *t = lex(p);
    Node *n;
    int type;
    if (t->type == T_AND) type = N_AND;
    else if (t->type == T_OR) type = N_OR;
    else break;
    consume(p);
    skip_newlines(p);
    n = new_node(p, type, l->line);
    n->a = l;
    n->b = parse_pipeline(p);
    if (p->error || n->b == NULL) return NULL;
    l = n;
  }
  if (l != NULL && !p->error)
    l->src = a_strndup(p->prog->arena, p->in.s + start,
                       (p->have ? p->tok.start : p->pos) - start);
  return p->error ? NULL : l;
}

/* }================================================================== */


/*
** {==================================================================
** Public interface
** ===================================================================
*/

Parser *parse_new (const char *text, const char *name, int line0) {
  Parser *p = (Parser *)xmalloc(sizeof(Parser));
  memset(p, 0, sizeof(*p));
  buf_init(&p->in);
  buf_puts(&p->in, text);
  if (p->in.s == NULL) buf_putc(&p->in, '\0'), p->in.len = 0;
  p->line = line0 > 0 ? line0 : 1;
  p->name = name ? xstrdup(name) : NULL;
  p->final = 1;
  return p;
}


/* a parser over the rest of p's text, to find the end of $( ) */
static Parser *parser_sub (Parser *p, size_t at) {
  Parser *s = (Parser *)xmalloc(sizeof(Parser));
  memset(s, 0, sizeof(*s));
  buf_init(&s->in);
  buf_putn(&s->in, p->in.s + at, p->in.len - at);
  if (s->in.s == NULL) buf_putc(&s->in, '\0'), s->in.len = 0;
  s->line = p->line;
  s->name = p->name ? xstrdup(p->name) : NULL;
  s->final = p->final;
  s->check = 1;	/* no aliases; errors are reported by the owner */
  s->prog = prog_new();
  s->stop_rparen = 1;
  s->depth = p->depth;
  return s;
}


static void parser_free_sub (Parser *s) {
  prog_unref(s->prog);
  buf_free(&s->in);
  free(s->name);
  free(s);
}


void parse_free (Parser *p) {
  if (p == NULL) return;
  if (p->prog) prog_unref(p->prog);
  buf_free(&p->in);
  free(p->name);
  free(p);
}


void parse_set_check (Parser *p, int on) {
  p->check = on;
}


int parse_errors (Parser *p) {
  return p->errors;
}


int parse_line (Parser *p) {
  return p->line;
}


/* the next complete command (up to a newline at the top level) */
int parse_next (Parser *p, Node **out) {
  Node *n;
  Tok *t;
  *out = NULL;
  p->error = p->incomplete = 0;
  p->npending = 0;
  if (p->prog) prog_unref(p->prog);
  p->prog = prog_new();
  p->have = 0;
  for (;;) {	/* blank lines and ; alone are nothing */
    t = lex(p);
    if (t->type == T_NL) {
      consume(p);
      continue;
    }
    break;
  }
  if (p->error) return p->incomplete ? P_INCOMPLETE : P_ERROR;
  if (t->type == T_EOF) return P_EOF;
  {	/* a list: and_or ((;|&) and_or)* up to a newline */
    PList items = {NULL, 0, 0};
    for (;;) {
      Node *item = parse_and_or(p);
      if (p->error || item == NULL) break;
      pl_push(&items, item);
      t = lex(p);
      if (t->type == T_SEMI || t->type == T_AMP) {
        if (t->type == T_AMP) item->flags |= NF_BG;
        consume(p);
        t = lex(p);
        if (t->type == T_NL || t->type == T_EOF) break;
        continue;
      }
      break;
    }
    if (!p->error) {
      t = lex(p);
      if (t->type == T_NL) consume(p);	/* reads pending here-documents */
      else if (t->type != T_EOF) unexpected(p);
      if (!p->error && p->npending > 0) need_more(p, "here-document");
    }
    if (p->error) {
      free(items.v);
      return p->incomplete ? P_INCOMPLETE : P_ERROR;
    }
    if (items.n == 1 && !(((Node *)items.v[0])->flags & NF_BG)) {
      n = (Node *)items.v[0];
      free(items.v);
    }
    else {
      n = new_node(p, N_LIST, ((Node *)items.v[0])->line);
      n->nkids = items.n;
      n->kids = (Node **)pl_take(p, &items, sizeof(Node *));
    }
  }
  *out = n;
  return P_OK;
}


/* is this text a whole command, or does the prompt need more lines? */
int parse_is_complete (const char *text) {
  Parser *p = parse_new(text, NULL, 1);
  int r;
  Node *n;
  p->final = 0;
  p->check = 1;	/* quiet */
  do r = parse_next(p, &n); while (r == P_OK);
  parse_free(p);
  return r == P_EOF ? P_OK : r;
}


/* the end of the $( ${ $(( ` construct at s (s points at '$' or '`') */
size_t parse_skip_subst (const char *s, int kind) {
  Parser *p = parse_new(s, NULL, 1);
  size_t e;
  (void)kind;
  p->check = 1;
  p->prog = prog_new();
  if (s[0] == '`') e = scan_backquote(p, 1);
  else if (s[0] == '"') e = scan_dquote(p, 1);
  else if (s[0] == '\'') e = scan_squote(p, 1);
  else if (s[0] == '<' || s[0] == '>') e = scan_procsubst(p, 0);
  else if (s[0] == '(') e = scan_parens(p, 1);
  else e = scan_dollar(p, 0);
  if (p->error) e = strlen(s);
  /* the scan may have dropped line continuations from its copy */
  if (p->in.len != strlen(s)) e += strlen(s) - p->in.len;
  parse_free(p);
  return e;
}


/* the words inside a=( ... ): comments and newlines allowed */
char *parse_word_list (const char *text, Vec *out) {
  Parser *p = parse_new(text, NULL, 1);
  p->check = 1;
  p->prog = prog_new();
  for (;;) {
    Tok *t;
    while (is_blank(ch(p, p->pos)) || ch(p, p->pos) == '\n') p->pos++;
    p->have = 0;
    t = lex(p);
    if (p->error || t->type == T_EOF) break;
    if (t->type == T_NL) {
      consume(p);
      continue;
    }
    if (t->type != T_WORD) {
      char *err = xstrcat3("syntax error near `", tok_name(t), "'");
      parse_free(p);
      return err;
    }
    vec_push(out, xstrdup(t->text));
    consume(p);
  }
  {
    char *err = p->error ? xstrdup("unbalanced quotes or parentheses") : NULL;
    parse_free(p);
    return err;
  }
}

/* }================================================================== */
