/*
** marith.c - shell arithmetic: $(( )), (( )), let, array subscripts
**
** 64-bit integers, the operators of C with bash's precedence, ** for
** powers, assignments (= += -= ...), ++ and --, and numbers written as
** 0x1F, 017 (octal) or base#digits. A variable holding an expression is
** evaluated in turn, like bash does. $ expansions have already been done
** by the caller.
*/

#include "mmc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


typedef struct Ar {
  const char *s;	/* the whole expression, for messages */
  const char *p;
  int noeval;	/* inside the side of && || ?: that is skipped */
  int error;
  int depth;
} Ar;

/* an lvalue: a variable, maybe an array element */
typedef struct LVal {
  char name[256];
  int is_elem;
  char *key;	/* subscript text (associative) */
  long long idx;
} LVal;

static long long comma (Ar *a);
static long long assign (Ar *a);


static void ar_error (Ar *a, const char *what) {
  if (a->error) return;
  a->error = 1;
  sh_error("%s: %s (error token is \"%s\")", a->s, what, *a->p ? a->p : "");
}


static void skip (Ar *a) {
  while (*a->p == ' ' || *a->p == '\t' || *a->p == '\n' || *a->p == '\r') a->p++;
}


static int accept (Ar *a, const char *op) {
  size_t n = strlen(op);
  skip(a);
  if (strncmp(a->p, op, n) != 0) return 0;
  /* do not take "<" out of "<<" or "<=", "&" out of "&&" ... */
  if (n == 1 && strchr("<>&|*=!", op[0]) && a->p[1] == op[0] && op[0] != '!') return 0;
  if (n == 1 && strchr("<>!=+-*/%&^|", op[0]) && a->p[1] == '=' &&
      !(op[0] == '<' && 0))
    return 0;
  if (n == 2 && (strcmp(op, "<<") == 0 || strcmp(op, ">>") == 0) && a->p[2] == '=')
    return 0;
  if (n == 1 && (op[0] == '+' || op[0] == '-') && a->p[1] == op[0]) return 0;
  if (n == 2 && strcmp(op, "**") == 0 && a->p[2] == '=') return 0;
  a->p += n;
  return 1;
}


/* 0x1F, 017, 2#101, 36#zz, 123 */
static long long number (Ar *a) {
  const char *p = a->p;
  long long v = 0;
  int base = 10;
  if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
    base = 16;
    p += 2;
  }
  else if (p[0] == '0' && isdigit((unsigned char)p[1])) {
    base = 8;
    p++;
  }
  else {
    const char *q = p;
    while (isdigit((unsigned char)*q)) q++;
    if (*q == '#') {
      base = atoi(p);
      p = q + 1;
      if (base < 2 || base > 64) {
        ar_error(a, "invalid arithmetic base");
        return 0;
      }
    }
  }
  if (!isalnum((unsigned char)*p) && *p != '@' && *p != '_') {
    if (base != 10 || p == a->p) {
      ar_error(a, "invalid number");
      return 0;
    }
  }
  while (isalnum((unsigned char)*p) || *p == '@' || *p == '_') {
    int d;
    char c = *p;
    if (isdigit((unsigned char)c)) d = c - '0';
    else if (base <= 36 && isalpha((unsigned char)c)) d = tolower((unsigned char)c) - 'a' + 10;
    else if (islower((unsigned char)c)) d = c - 'a' + 10;
    else if (isupper((unsigned char)c)) d = c - 'A' + 36;
    else if (c == '@') d = 62;
    else d = 63;
    if (d >= base) {
      a->p = p;
      ar_error(a, "value too great for base");
      return 0;
    }
    v = v * base + d;
    p++;
  }
  a->p = p;
  return v;
}


/* the value of a variable: its text is an expression itself */
static long long var_value (Ar *a, const char *text) {
  long long v = 0;
  if (text == NULL || *text == '\0') return 0;
  if (!(text[0] == '0' && text[1] != '\0') && str_to_ll(text, &v) == 0) return v;
  if (a->depth > 64) {
    ar_error(a, "expression recursion level exceeded");
    return 0;
  }
  {
    Ar sub;
    sub.s = text;
    sub.p = text;
    sub.noeval = a->noeval;
    sub.error = 0;
    sub.depth = a->depth + 1;
    v = comma(&sub);
    skip(&sub);
    if (!sub.error && *sub.p != '\0') {
      sub.error = 1;
      sh_error("%s: syntax error in expression (error token is \"%s\")", text, sub.p);
    }
    if (sub.error) a->error = 1;
  }
  return v;
}


static long long lval_get (Ar *a, LVal *lv) {
  if (lv->is_elem) {
    int flags = var_flags(lv->name);
    if (flags >= 0 && (flags & V_ASSOC)) return var_value(a, var_akget(lv->name, lv->key));
    return var_value(a, var_aget(lv->name, lv->idx));
  }
  return var_value(a, var_get(lv->name));
}


static void lval_set (Ar *a, LVal *lv, long long v) {
  char num[24];
  if (a->noeval) return;
  ll_to_str(v, num);
  if (lv->is_elem) {
    int flags = var_flags(lv->name);
    if (flags >= 0 && (flags & V_ASSOC)) var_akset(lv->name, lv->key, num);
    else var_aset(lv->name, lv->idx, num);
  }
  else if (var_set(lv->name, num) != 0) a->error = 1;
}


/* name or name[subscript] at a->p */
static int read_lval (Ar *a, LVal *lv) {
  const char *p = a->p;
  size_t n = 0;
  memset(lv, 0, sizeof(*lv));
  while ((isalnum((unsigned char)*p) || *p == '_') && n < sizeof(lv->name) - 1)
    lv->name[n++] = *p++;
  lv->name[n] = '\0';
  a->p = p;
  if (*p == '[') {
    const char *start = p + 1;
    int depth = 1;
    p++;
    while (*p && depth > 0) {
      if (*p == '[') depth++;
      else if (*p == ']') depth--;
      if (depth > 0) p++;
    }
    if (*p != ']') {
      ar_error(a, "missing `]'");
      return -1;
    }
    lv->is_elem = 1;
    lv->key = xstrndup(start, (size_t)(p - start));
    {
      int flags = var_flags(lv->name);
      if (!(flags >= 0 && (flags & V_ASSOC))) {
        Ar sub;
        sub.s = lv->key;
        sub.p = lv->key;
        sub.noeval = a->noeval;
        sub.error = 0;
        sub.depth = a->depth + 1;
        lv->idx = comma(&sub);
        if (sub.error) a->error = 1;
      }
    }
    a->p = p + 1;
  }
  return 0;
}


static long long primary (Ar *a) {
  skip(a);
  if (a->error) return 0;
  if (*a->p == '(') {
    long long v;
    a->p++;
    v = comma(a);
    skip(a);
    if (*a->p != ')') {
      ar_error(a, "missing `)'");
      return 0;
    }
    a->p++;
    return v;
  }
  if (isdigit((unsigned char)*a->p)) return number(a);
  if (isalpha((unsigned char)*a->p) || *a->p == '_') {
    LVal lv;
    long long v;
    if (read_lval(a, &lv) != 0) return 0;
    skip(a);
    if (strncmp(a->p, "++", 2) == 0 || strncmp(a->p, "--", 2) == 0) {	/* x++ */
      int inc = a->p[0] == '+';
      a->p += 2;
      v = lval_get(a, &lv);
      lval_set(a, &lv, inc ? v + 1 : v - 1);
    }
    else v = lval_get(a, &lv);
    free(lv.key);
    return v;
  }
  if (*a->p == '\0') ar_error(a, "syntax error: operand expected");
  else ar_error(a, "syntax error: operand expected");
  return 0;
}


static long long unary (Ar *a) {
  skip(a);
  if (strncmp(a->p, "++", 2) == 0 || strncmp(a->p, "--", 2) == 0) {
    int inc = a->p[0] == '+';
    LVal lv;
    long long v;
    a->p += 2;
    skip(a);
    if (!(isalpha((unsigned char)*a->p) || *a->p == '_')) {
      ar_error(a, "syntax error: variable expected after ++/--");
      return 0;
    }
    if (read_lval(a, &lv) != 0) return 0;
    v = lval_get(a, &lv) + (inc ? 1 : -1);
    lval_set(a, &lv, v);
    free(lv.key);
    return v;
  }
  if (*a->p == '!') {
    a->p++;
    return !unary(a);
  }
  if (*a->p == '~') {
    a->p++;
    return ~unary(a);
  }
  if (*a->p == '-') {
    a->p++;
    return (long long)(0ULL - (unsigned long long)unary(a));
  }
  if (*a->p == '+') {
    a->p++;
    return unary(a);
  }
  return primary(a);
}


static long long power (Ar *a) {
  long long base = unary(a);
  if (accept(a, "**")) {
    long long e = power(a), r = 1;
    if (e < 0) {
      ar_error(a, "exponent less than 0");
      return 0;
    }
    while (e-- > 0) r *= base;
    return r;
  }
  return base;
}


static long long mul (Ar *a) {
  long long v = power(a);
  for (;;) {
    int op;
    skip(a);
    if (a->p[0] == '*' && a->p[1] != '*' && a->p[1] != '=') op = '*';
    else if (a->p[0] == '/' && a->p[1] != '=') op = '/';
    else if (a->p[0] == '%' && a->p[1] != '=') op = '%';
    else return v;
    a->p++;
    {
      long long r = power(a);
      if (op == '*') v = (long long)((unsigned long long)v * (unsigned long long)r);
      else if (r == 0) {
        if (!a->noeval) {
          ar_error(a, "division by 0");
          return 0;
        }
        v = 0;
      }
      else if (r == -1) v = (op == '/') ? (long long)(0ULL - (unsigned long long)v) : 0;
      else v = (op == '/') ? v / r : v % r;
    }
  }
}


static long long add (Ar *a) {
  long long v = mul(a);
  for (;;) {
    skip(a);
    if (a->p[0] == '+' && a->p[1] != '+' && a->p[1] != '=') {
      a->p++;
      v = (long long)((unsigned long long)v + (unsigned long long)mul(a));
    }
    else if (a->p[0] == '-' && a->p[1] != '-' && a->p[1] != '=') {
      a->p++;
      v = (long long)((unsigned long long)v - (unsigned long long)mul(a));
    }
    else return v;
  }
}


static long long shift (Ar *a) {
  long long v = add(a);
  for (;;) {
    skip(a);
    if (a->p[0] == '<' && a->p[1] == '<' && a->p[2] != '=') {
      a->p += 2;
      v = (long long)((unsigned long long)v << (add(a) & 63));
    }
    else if (a->p[0] == '>' && a->p[1] == '>' && a->p[2] != '=') {
      a->p += 2;
      v = v >> (add(a) & 63);
    }
    else return v;
  }
}


static long long rel (Ar *a) {
  long long v = shift(a);
  for (;;) {
    skip(a);
    if (a->p[0] == '<' && a->p[1] == '=') {
      a->p += 2;
      v = v <= shift(a);
    }
    else if (a->p[0] == '>' && a->p[1] == '=') {
      a->p += 2;
      v = v >= shift(a);
    }
    else if (a->p[0] == '<' && a->p[1] != '<') {
      a->p++;
      v = v < shift(a);
    }
    else if (a->p[0] == '>' && a->p[1] != '>') {
      a->p++;
      v = v > shift(a);
    }
    else return v;
  }
}


static long long eq (Ar *a) {
  long long v = rel(a);
  for (;;) {
    skip(a);
    if (a->p[0] == '=' && a->p[1] == '=') {
      a->p += 2;
      v = v == rel(a);
    }
    else if (a->p[0] == '!' && a->p[1] == '=') {
      a->p += 2;
      v = v != rel(a);
    }
    else return v;
  }
}


static long long band (Ar *a) {
  long long v = eq(a);
  for (;;) {
    skip(a);
    if (a->p[0] == '&' && a->p[1] != '&' && a->p[1] != '=') {
      a->p++;
      v &= eq(a);
    }
    else return v;
  }
}


static long long bxor (Ar *a) {
  long long v = band(a);
  for (;;) {
    skip(a);
    if (a->p[0] == '^' && a->p[1] != '=') {
      a->p++;
      v ^= band(a);
    }
    else return v;
  }
}


static long long bor (Ar *a) {
  long long v = bxor(a);
  for (;;) {
    skip(a);
    if (a->p[0] == '|' && a->p[1] != '|' && a->p[1] != '=') {
      a->p++;
      v |= bxor(a);
    }
    else return v;
  }
}


static long long land (Ar *a) {
  long long v = bor(a);
  for (;;) {
    skip(a);
    if (a->p[0] == '&' && a->p[1] == '&') {
      long long r;
      a->p += 2;
      if (!v) a->noeval++;
      r = bor(a);
      if (!v) a->noeval--;
      v = v && r;
    }
    else return v;
  }
}


static long long lor (Ar *a) {
  long long v = land(a);
  for (;;) {
    skip(a);
    if (a->p[0] == '|' && a->p[1] == '|') {
      long long r;
      a->p += 2;
      if (v) a->noeval++;
      r = land(a);
      if (v) a->noeval--;
      v = v || r;
    }
    else return v;
  }
}


static long long cond (Ar *a) {
  long long c = lor(a);
  skip(a);
  if (*a->p == '?') {
    long long t, f;
    a->p++;
    if (!c) a->noeval++;
    t = comma(a);
    if (!c) a->noeval--;
    skip(a);
    if (*a->p != ':') {
      ar_error(a, "`:' expected for conditional expression");
      return 0;
    }
    a->p++;
    if (c) a->noeval++;
    f = cond(a);
    if (c) a->noeval--;
    return c ? t : f;
  }
  return c;
}


/* is there an assignment operator after the lvalue at p? */
static const char *assign_op (const char *p, int *len) {
  static const char *const ops[] = {
    "<<=", ">>=", "**=", "*=", "/=", "%=", "+=", "-=", "&=", "^=", "|=", "=", NULL
  };
  int i;
  while (*p == ' ' || *p == '\t') p++;
  for (i = 0; ops[i]; i++) {
    size_t n = strlen(ops[i]);
    if (strncmp(p, ops[i], n) == 0) {
      if (strcmp(ops[i], "=") == 0 && p[1] == '=') return NULL;	/* == */
      *len = (int)n;
      return ops[i];
    }
  }
  return NULL;
}


static long long assign (Ar *a) {
  const char *save;
  skip(a);
  save = a->p;
  if (isalpha((unsigned char)*a->p) || *a->p == '_') {
    LVal lv;
    const char *op;
    int len;
    if (read_lval(a, &lv) != 0) return 0;
    if ((op = assign_op(a->p, &len)) != NULL) {
      long long v, old = 0;
      skip(a);
      a->p += len;
      v = assign(a);
      if (strcmp(op, "=") != 0) {
        old = lval_get(a, &lv);
        switch (op[0]) {
          case '+': v = old + v; break;
          case '-': v = old - v; break;
          case '*':
            if (op[1] == '*') {
              long long r = 1, e = v;
              while (e-- > 0) r *= old;
              v = r;
            }
            else v = old * v;
            break;
          case '/':
          case '%':
            if (v == 0) {
              ar_error(a, "division by 0");
              free(lv.key);
              return 0;
            }
            v = (op[0] == '/') ? old / v : old % v;
            break;
          case '&': v = old & v; break;
          case '^': v = old ^ v; break;
          case '|': v = old | v; break;
          case '<': v = (long long)((unsigned long long)old << (v & 63)); break;
          case '>': v = old >> (v & 63); break;
        }
      }
      if (!a->error) lval_set(a, &lv, v);
      free(lv.key);
      return v;
    }
    free(lv.key);
    a->p = save;
  }
  return cond(a);
}


static long long comma (Ar *a) {
  long long v = assign(a);
  while (!a->error) {
    skip(a);
    if (*a->p != ',') break;
    a->p++;
    v = assign(a);
  }
  return v;
}


int arith_eval (const char *expr, long long *out) {
  Ar a;
  a.s = expr;
  a.p = expr;
  a.noeval = 0;
  a.error = 0;
  a.depth = 0;
  skip(&a);
  if (*a.p == '\0') {	/* $(( )) is 0 */
    *out = 0;
    return 0;
  }
  *out = comma(&a);
  skip(&a);
  if (!a.error && *a.p != '\0') {
    a.error = 1;
    sh_error("%s: syntax error in expression (error token is \"%s\")", expr, a.p);
  }
  if (a.error) {
    *out = 0;
    return -1;
  }
  return 0;
}
