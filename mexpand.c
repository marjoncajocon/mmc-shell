/*
** mexpand.c - word expansion and globbing
**
** A raw word goes through: tilde, quotes, $variables, then globbing.
** Characters that came from quotes or variables must not glob, so they
** are written with a QMARK byte in front ("marked") until the end.
*/

#include "mmc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static int is_glob_char (int c) {
  return c == '*' || c == '?' || c == '[';
}


static int is_name_start (int c) {
  return isalpha(c) || c == '_';
}


static int is_name_char (int c) {
  return isalnum(c) || c == '_';
}


/* index of '=' when the word looks like NAME=value, else 0 */
size_t word_assign_pos (const char *w) {
  size_t i;
  if (!is_name_start((unsigned char)w[0])) return 0;
  for (i = 1; is_name_char((unsigned char)w[i]); i++)
    ;
  return (w[i] == '=') ? i : 0;
}


static void put_lit (Buf *b, char c) {
  if (is_glob_char(c) || c == QMARK) buf_putc(b, QMARK);
  buf_putc(b, c);
}


static void put_lits (Buf *b, const char *s) {
  for (; *s; s++) put_lit(b, *s);
}


static int has_glob (const char *marked) {
  for (; *marked; marked++) {
    if (*marked == QMARK && marked[1] != '\0') marked++;
    else if (is_glob_char(*marked)) return 1;
  }
  return 0;
}


static char *unmark (const char *marked) {
  Buf b;
  buf_init(&b);
  for (; *marked; marked++) {
    if (*marked == QMARK && marked[1] != '\0') marked++;
    buf_putc(&b, *marked);
  }
  return buf_take(&b);
}


static void put_var (Buf *b, const char *name, size_t n) {
  char *key = xstrndup(name, n);
  char *val = os_getenv(key);
  if (val) put_lits(b, val);
  free(key);
  free(val);
}


/* expands the '$' at s[0]; returns how many bytes were used */
static size_t expand_dollar (const char *s, Buf *b) {
  char num[32];
  size_t i;
  int c = (unsigned char)s[1];
  if (c == '?') {
    sprintf(num, "%d", sh_status);
    put_lits(b, num);
    return 2;
  }
  if (c == '$') {
    sprintf(num, "%ld", os_getpid());
    put_lits(b, num);
    return 2;
  }
  if (c == '#') {
    sprintf(num, "%lu", (unsigned long)(sh_args.n > 0 ? sh_args.n - 1 : 0));
    put_lits(b, num);
    return 2;
  }
  if (c == '@' || c == '*') {
    for (i = 1; i < sh_args.n; i++) {
      if (i > 1) put_lit(b, ' ');
      put_lits(b, sh_args.v[i]);
    }
    return 2;
  }
  if (isdigit(c)) {
    size_t k = (size_t)(c - '0');
    if (k < sh_args.n) put_lits(b, sh_args.v[k]);
    else if (k == 0) put_lits(b, MMC_NAME);
    return 2;
  }
  if (c == '{') {	/* ${NAME} and ${NAME:-default} */
    const char *close = strchr(s + 2, '}');
    const char *name = s + 2;
    if (close == NULL || !is_name_start((unsigned char)name[0])) {
      put_lit(b, '$');
      return 1;
    }
    for (i = 0; is_name_char((unsigned char)name[i]); i++)
      ;
    if (name + i == close) put_var(b, name, i);
    else if (name[i] == ':' && name[i + 1] == '-') {
      char *key = xstrndup(name, i);
      char *val = os_getenv(key);
      if (val && val[0]) put_lits(b, val);
      else {
        char *def = xstrndup(name + i + 2, (size_t)(close - (name + i + 2)));
        char *exp = expand_str(def);
        put_lits(b, exp);
        free(def);
        free(exp);
      }
      free(key);
      free(val);
    }
    return (size_t)(close + 1 - s);
  }
  if (is_name_start(c)) {
    for (i = 1; is_name_char((unsigned char)s[i]); i++)
      ;
    put_var(b, s + 1, i - 1);
    return i;
  }
  put_lit(b, '$');
  return 1;
}


static size_t expand_tilde (const char *s, Buf *b) {
  if (s[0] == '~' && (s[1] == '\0' || s[1] == '/' || s[1] == ':')) {
    char *home = os_getenv("HOME");
    if (home) {
      put_lits(b, home);
      free(home);
      return 1;
    }
  }
  return 0;
}


/*
** Outside quotes a backslash only escapes characters that are special
** to the shell, so Windows paths such as C:\Users keep working.
*/
static int is_escapable (int c) {
  return c != '\0' && strchr(" \t\"'$;&|<>#*?[]~()", c) != NULL;
}


/* returns the marked expansion; '*quoted' tells if quotes were seen */
static char *expand_raw (const char *raw, int *quoted) {
  Buf b;
  size_t i = 0;
  size_t eq = word_assign_pos(raw);
  buf_init(&b);
  *quoted = 0;
  i += expand_tilde(raw, &b);
  while (raw[i] != '\0') {
    char c = raw[i];
    if (c == '\'') {
      *quoted = 1;
      for (i++; raw[i] != '\0' && raw[i] != '\''; i++) put_lit(&b, raw[i]);
      if (raw[i] == '\'') i++;
    }
    else if (c == '"') {
      *quoted = 1;
      i++;
      while (raw[i] != '\0' && raw[i] != '"') {
        if (raw[i] == '\\' && raw[i + 1] != '\0' &&
            strchr("\"$\\`", raw[i + 1]) != NULL) {
          put_lit(&b, raw[i + 1]);
          i += 2;
        }
        else if (raw[i] == '$') i += expand_dollar(raw + i, &b);
        else put_lit(&b, raw[i++]);
      }
      if (raw[i] == '"') i++;
    }
    else if (c == '\\' && is_escapable((unsigned char)raw[i + 1])) {
      put_lit(&b, raw[i + 1]);
      i += 2;
    }
    else if (c == '$') i += expand_dollar(raw + i, &b);
    else {
      buf_putc(&b, c);
      i++;
      /* NAME=~/x and PATH=~/a:~/b */
      if (eq != 0 && (c == ':' || i == eq + 1)) i += expand_tilde(raw + i, &b);
    }
  }
  return buf_take(&b);
}


/*
** {==================================================================
** Globbing
** ===================================================================
*/

static int same_char (int a, int b) {
#ifdef _WIN32
  return tolower(a) == tolower(b);
#else
  return a == b;
#endif
}


static int glob_match (const char *p, const char *s) {
  while (*p != '\0') {
    if (*p == QMARK && p[1] != '\0') {
      if (!same_char((unsigned char)p[1], (unsigned char)*s)) return 0;
      p += 2;
      s++;
    }
    else if (*p == '*') {
      while (*p == '*') p++;
      if (*p == '\0') return 1;
      for (;; s++) {
        if (glob_match(p, s)) return 1;
        if (*s == '\0') return 0;
      }
    }
    else if (*p == '?') {
      if (*s == '\0') return 0;
      p++;
      s++;
      while (((unsigned char)*s & 0xC0) == 0x80) s++;	/* UTF-8 tail */
    }
    else if (*p == '[' && strchr(p + 2, ']') != NULL) {
      int neg = 0, hit = 0;
      if (*s == '\0') return 0;
      p++;
      if (*p == '!' || *p == '^') {
        neg = 1;
        p++;
      }
      do {
        int lo = (unsigned char)*p++;
        int hi = lo;
        if (*p == '-' && p[1] != ']' && p[1] != '\0') {
          hi = (unsigned char)p[1];
          p += 2;
        }
        if ((unsigned char)*s >= lo && (unsigned char)*s <= hi) hit = 1;
        if (same_char(lo, (unsigned char)*s)) hit = 1;
      } while (*p != ']' && *p != '\0');
      if (*p == ']') p++;
      if (hit == neg) return 0;
      s++;
    }
    else {
      if (!same_char((unsigned char)*p, (unsigned char)*s)) return 0;
      p++;
      s++;
    }
  }
  return *s == '\0';
}


static int exists (const char *posix, int want_dir) {
  OsStat st;
  char *native = path_to_native(posix);
  os_stat(native, &st);
  free(native);
  return st.exists && (!want_dir || st.is_dir);
}


/* 'prefix' is literal text already matched; 'pat' is still marked */
static void glob_rec (const char *prefix, const char *pat, Vec *out) {
  const char *slash = strchr(pat, '/');
  size_t n = slash ? (size_t)(slash - pat) : strlen(pat);
  char *comp = xstrndup(pat, n);
  const char *rest = slash;
  while (rest && *rest == '/') rest++;
  if (rest && *rest == '\0') rest = NULL;
  if (!has_glob(comp)) {
    char *lit = unmark(comp);
    char *next = xstrcat3(prefix, lit, slash ? "/" : "");
    if (rest) glob_rec(next, rest, out);
    else if (exists(next, 0)) vec_push(out, xstrdup(next));
    free(lit);
    free(next);
  }
  else {
    Vec names;
    size_t i;
    char *dir = path_to_native(prefix[0] ? prefix : ".");
    vec_init(&names);
    os_listdir(dir, &names);
    vec_sort(&names);
    for (i = 0; i < names.n; i++) {
      const char *name = names.v[i];
      char *next;
      if (name[0] == '.' && comp[0] != '.') continue;	/* hidden */
      if (!glob_match(comp, name)) continue;
      next = xstrcat3(prefix, name, slash ? "/" : "");
      if (rest) {
        if (exists(next, 1)) glob_rec(next, rest, out);
        free(next);
      }
      else if (slash && !exists(next, 1)) free(next);
      else vec_push(out, next);
    }
    free(dir);
    vec_free(&names);
  }
  free(comp);
}

/* }================================================================== */


/* expands one raw word into zero or more words; returns how many */
int expand_word (const char *raw, Vec *out) {
  int quoted;
  size_t before = out->n;
  char *marked = expand_raw(raw, &quoted);
  if (has_glob(marked)) glob_rec("", marked, out);
  if (out->n == before && (marked[0] != '\0' || quoted))
    vec_push(out, unmark(marked));	/* no glob, or nothing matched */
  free(marked);
  return (int)(out->n - before);
}


/* expansion without globbing: redirect targets, assignments */
char *expand_str (const char *raw) {
  int quoted;
  char *marked = expand_raw(raw, &quoted);
  char *r = unmark(marked);
  free(marked);
  return r;
}
