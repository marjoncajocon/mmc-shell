/*
** cmore.c - more text tools: tac rev nl paste comm join fold expand
** unexpand split shuf column od xxd hexdump strings cmp fmt
**
** They follow GNU coreutils (od, fmt, join ...), util-linux (rev,
** column, hexdump), diffutils (cmp), binutils (strings) and vim's xxd:
** the same options, output and messages. Notes:
** - Comparisons (comm, join) are byte by byte, as in the C locale.
** - fold, expand and unexpand count a UTF-8 character as one character
**   (fold: its width) unless LC_ALL/LC_CTYPE/LANG say C; GNU 8.32
**   counts bytes. rev reverses characters the same way.
** - tac -r searches like GNU (the last match first), with ERE syntax.
** - fmt is GNU's optimal-fit line breaking, cost for cost (and its
**   5000 byte / 1000 word paragraph buffer, which shapes long ones).
** - od, xxd, hexdump, cmp and strings read bytes; od and hexdump take
**   numbers little-endian (od --endian=big swaps); od has no fL.
** - hexdump: the built-in formats (-b -c -C -d -o -x, default) and -e/-f
**   go through one small engine modelled on util-linux's.
** - shuf's randomness is its own (--random-source only seeds it).
** - strings: -e s and S (no 16/32-bit encodings), whole files (-a).
*/

#include "mmc.h"

#include <ctype.h>
#include <float.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*
** {==================================================================
** Shared helpers
** ===================================================================
*/

#define OPEN_PLAIN	0	/* "tool: name: No such file or directory" */
#define OPEN_FAILED	1	/* "tool: failed to open 'name' for reading: ..." */
#define OPEN_CANNOT	2	/* "tool: cannot open 'name' for reading: ..." */
#define OPEN_REV	3	/* "rev: cannot open name: ..." */

/* opens 'name' ("-": stdin) or says why not; dirmsg: printf format for
** a folder (NULL: "name: Is a directory") */
static int m_open (In *r, const char *tool, const char *name, int in, int err, int how,
                   const char *dirmsg) {
  char *native;
  OsStat st;
  int isdir;
  if (strcmp(name, "-") == 0) {
    in_init(r, in, 0);
    return 0;
  }
  native = path_to_native(name);
  isdir = os_stat(native, &st) == 0 && st.is_dir;
  free(native);
  if (!isdir && in_open(r, name, in) == 0) return 0;
  if (isdir) {
    char msg[1024];
    snprintf(msg, sizeof(msg), dirmsg ? dirmsg : "%s: Is a directory", name);
    tool_err(err, tool, "%s", msg);
  }
  else if (how == OPEN_FAILED) tool_err(err, tool, "failed to open '%s' for reading: %s", name, os_errmsg());
  else if (how == OPEN_CANNOT) tool_err(err, tool, "cannot open '%s' for reading: %s", name, os_errmsg());
  else if (how == OPEN_REV) tool_err(err, tool, "cannot open %s: %s", name, os_errmsg());
  else tool_err(err, tool, "%s: %s", name, os_errmsg());
  return -1;
}


/* all of the input into b */
static void slurp (In *r, Buf *b) {
  char chunk[65536];
  long n;
  while ((n = in_read(r, chunk, sizeof(chunk))) > 0 && !tool_stop()) buf_putn(b, chunk, (size_t)n);
}


/* a whole number option: 0 ok, -1 not a number (or < min) */
static int num_arg (const char *s, long long min, long long *v) {
  char *end;
  long long x;
  while (*s == ' ' || *s == '\t') s++;
  if (*s == '\0') return -1;
  x = strtoll(s, &end, 10);
  if (*end != '\0' || x < min) return -1;
  *v = x;
  return 0;
}


/* the bytes of the UTF-8 character at s (1 for a bad one) */
static int u8_len (const unsigned char *s, size_t n) {
  int len, k;
  if (s[0] < 0x80) return 1;
  if ((s[0] & 0xE0) == 0xC0 && s[0] >= 0xC2) len = 2;
  else if ((s[0] & 0xF0) == 0xE0) len = 3;
  else if ((s[0] & 0xF8) == 0xF0 && s[0] <= 0xF4) len = 4;
  else return 1;
  if ((size_t)len > n) return 1;
  for (k = 1; k < len; k++)
    if ((s[k] & 0xC0) != 0x80) return 1;
  return len;
}


static unsigned long u8_code (const unsigned char *s, int len) {
  unsigned long cp;
  int k;
  if (len == 1) return s[0];
  cp = s[0] & (len == 2 ? 0x1F : len == 3 ? 0x0F : 0x07);
  for (k = 1; k < len; k++) cp = (cp << 6) | (s[k] & 0x3F);
  return cp;
}


/* a stream of characters over several files, like GNU's next_file():
** what cannot be opened is reported and skipped */
typedef struct CStream {
  const char *tool;
  Vec *names;
  size_t next;
  int in, err, open, status, how;
  In r;
  unsigned char buf[65536];
  size_t pos, len;
} CStream;

static void cs_init (CStream *cs, const char *tool, Vec *names, int in, int err) {
  cs->tool = tool;
  cs->names = names;
  cs->next = 0;
  cs->in = in;
  cs->err = err;
  cs->open = 0;
  cs->status = 0;
  cs->how = OPEN_PLAIN;
  cs->pos = cs->len = 0;
}

static int cs_getc (CStream *cs) {
  for (;;) {
    long n;
    if (cs->pos < cs->len) return cs->buf[cs->pos++];
    if (cs->open) {
      n = in_read(&cs->r, (char *)cs->buf, sizeof(cs->buf));
      if (n > 0 && !tool_stop()) {
        cs->pos = 0;
        cs->len = (size_t)n;
        continue;
      }
      in_close(&cs->r);
      cs->open = 0;
    }
    if (cs->next >= cs->names->n || tool_stop()) return -1;
    if (m_open(&cs->r, cs->tool, cs->names->v[cs->next++], cs->in, cs->err, cs->how, NULL) == 0)
      cs->open = 1;
    else cs->status = 1;
  }
}

/* up to n bytes; fewer only at the very end */
static size_t cs_read (CStream *cs, unsigned char *dst, size_t n) {
  size_t got = 0;
  while (got < n) {
    size_t k;
    if (cs->pos == cs->len) {
      int c = cs_getc(cs);	/* refills, or moves to the next file */
      if (c < 0) break;
      dst[got++] = (unsigned char)c;
      continue;
    }
    k = cs->len - cs->pos;
    if (k > n - got) k = n - got;
    memcpy(dst + got, cs->buf + cs->pos, k);
    cs->pos += k;
    got += k;
  }
  return got;
}

static void cs_close (CStream *cs) {
  if (cs->open) in_close(&cs->r);
  cs->open = 0;
}


/* tab stops for expand and unexpand: "8", "4,8,12", "2 4", ",/8", "+4" */
typedef struct Tabs {
  long long size;	/* all at multiples of this, or 0 */
  long long *list;
  size_t n;
  long long extend, incr;	/* after the list: '/N' multiples, '+N' steps */
} Tabs;

static int tabs_add (Tabs *t, const char *tool, int err, const char *spec) {
  const char *p = spec;
  while (*p) {
    long long v = 0;
    int kind = 0;
    if (*p == ',' || *p == ' ' || *p == '\t') { p++; continue; }
    if (*p == '/' || *p == '+') kind = *p++;
    if (!isdigit((unsigned char)*p)) {
      tool_err(err, tool, "tab size contains invalid character(s): '%s'", p);
      return -1;
    }
    while (isdigit((unsigned char)*p)) {
      v = v * 10 + (*p - '0');
      if (v > 1000000000000LL) {
        tool_err(err, tool, "tab stop is too large '%s'", spec);
        return -1;
      }
      p++;
    }
    if (*p && *p != ',' && *p != ' ' && *p != '\t') {
      tool_err(err, tool, "tab size contains invalid character(s): '%s'", p);
      return -1;
    }
    if (kind) {
      const char *q = p;
      while (*q == ',' || *q == ' ' || *q == '\t') q++;
      if (*q) {
        tool_err(err, tool, "'%c' specifier only allowed with the last value", kind);
        return -1;
      }
      if (kind == '/') t->extend = v;
      else t->incr = v;
      continue;
    }
    if (v == 0) {
      tool_err(err, tool, "tab size cannot be 0");
      return -1;
    }
    if (t->n > 0 && v <= t->list[t->n - 1]) {
      tool_err(err, tool, "tab sizes must be ascending");
      return -1;
    }
    t->list = (long long *)xrealloc(t->list, (t->n + 1) * sizeof(long long));
    t->list[t->n++] = v;
  }
  return 0;
}

static void tabs_finish (Tabs *t) {
  if (t->n == 0 && !t->extend && !t->incr) t->size = 8;
  else if (t->n == 1 && !t->extend && !t->incr) t->size = t->list[0];
  else if (t->n == 0) t->size = t->extend ? t->extend : t->incr;	/* only "/N" or "+N" */
}

/* the next tab stop after 'col'; *last: none left */
static long long tabs_next (const Tabs *t, long long col, size_t *idx, int *last) {
  *last = 0;
  if (t->size) return col + (t->size - col % t->size);
  for (; *idx < t->n; (*idx)++)
    if (col < t->list[*idx]) return t->list[*idx];
  if (t->extend) return col + (t->extend - col % t->extend);
  if (t->incr) {
    long long end = t->list[t->n - 1];
    return col + (t->incr - (col - end) % t->incr);
  }
  *last = 1;
  return 0;
}

/* "-8" (old style) is "OPT 8"; returns a new argv */
static char **legacy_num (int argc, char **argv, int *nargc, char *opt) {
  char **v = (char **)xmalloc(((size_t)argc * 2 + 1) * sizeof(char *));
  int i, k = 0;
  v[k++] = argv[0];
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--") == 0) {
      for (; i < argc; i++) v[k++] = argv[i];
      break;
    }
    if (argv[i][0] == '-' && isdigit((unsigned char)argv[i][1])) {
      v[k++] = opt;
      v[k++] = argv[i] + 1;
    }
    else v[k++] = argv[i];
  }
  v[k] = NULL;
  *nargc = k;
  return v;
}

/* }================================================================== */


/*
** {==================================================================
** tac
** ===================================================================
*/

/* the separators, found from the end like GNU: each one is the last
** match that ends before the one after it. at[]: start, end pairs */
static size_t tac_find_plain (const char *s, size_t len, const char *sep, size_t slen,
                              size_t **at) {
  size_t n = 0, cap = 0, limit = len;
  while (limit >= slen) {
    size_t p = limit - slen + 1;
    int found = 0;
    while (p-- > 0) {
      if (s[p] == sep[0] && memcmp(s + p, sep, slen) == 0) {
        found = 1;
        break;
      }
    }
    if (!found) break;
    if (n + 2 > cap) {
      cap = cap ? cap * 2 : 256;
      *at = (size_t *)xrealloc(*at, cap * sizeof(size_t));
    }
    (*at)[n++] = p;
    (*at)[n++] = p + slen;
    limit = p;
  }
  return n / 2;
}

static size_t tac_find_regex (const char *s, size_t len, Regex *re, size_t **at) {
  size_t *starts = NULL, ns = 0, cap = 0, from = 0, n = 0, acap = 0, limit = len;
  size_t *m = (size_t *)xmalloc(2 * ((size_t)regex_nsub(re) + 1) * sizeof(size_t));
  /* every place a match can start, in order */
  while (from < len && regex_match(re, s, len, from, 0, m)) {
    if (ns == cap) {
      cap = cap ? cap * 2 : 256;
      starts = (size_t *)xrealloc(starts, cap * sizeof(size_t));
    }
    starts[ns++] = m[0];
    from = m[0] + 1;
    if (tool_stop()) break;
  }
  /* then from the end: the last start whose match fits before 'limit' */
  while (ns > 0) {
    size_t p = starts[--ns];
    if (p >= limit) continue;
    if (!regex_match(re, s, limit, p, 0, m) || m[0] != p || m[1] == m[0]) continue;
    if (2 * n + 2 > acap) {
      acap = acap ? acap * 2 : 256;
      *at = (size_t *)xrealloc(*at, acap * sizeof(size_t));
    }
    (*at)[2 * n] = m[0];
    (*at)[2 * n + 1] = m[1];
    n++;
    limit = p;
  }
  free(starts);
  free(m);
  return n;
}


int t_tac (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {{"before", 'b', 0}, {"regex", 'r', 0}, {"separator", 's', 1},
    {NULL, 0, 0}};
  Opts g;
  Out o;
  int c, before = 0, regex = 0, status = 0;
  const char *sep = "\n";
  Regex *re = NULL;
  size_t i;
  opts_init(&g, "tac", argc, argv, err);
  while ((c = opts_next(&g, "brs:", lo)) != 0) {
    switch (c) {
      case 'b': before = 1; break;
      case 'r': regex = 1; break;
      case 's': sep = g.arg; break;
      case OPT_HELP: opts_free(&g); return tool_help(out, "tac");
      default: opts_free(&g); return 1;
    }
  }
  if (regex) {
    char *msg = NULL;
    re = regex_new(sep, RE_EXTENDED, &msg);
    if (re == NULL) {
      tool_err(err, "tac", "%s", msg ? msg : "invalid regular expression");
      free(msg);
      opts_free(&g);
      return 1;
    }
  }
  if (g.ops.n == 0) vec_push(&g.ops, xstrdup("-"));
  out_init(&o, out);
  for (i = 0; i < g.ops.n && !o.failed && !tool_stop(); i++) {
    In r;
    Buf b;
    size_t *at = NULL, n, k, end;
    if (m_open(&r, "tac", g.ops.v[i], in, err, OPEN_FAILED, "%s: read error: Is a directory") != 0) {
      status = 1;
      continue;
    }
    buf_init(&b);
    slurp(&r, &b);
    in_close(&r);
    /* at[]: the separators from the last one back */
    if (re) n = tac_find_regex(b.s, b.len, re, &at);
    else n = *sep ? tac_find_plain(b.s, b.len, sep, strlen(sep), &at) : 0;
    end = b.len;
    for (k = 0; k <= n; k++) {	/* a record ends (-b: starts) with its separator */
      size_t from = k < n ? at[2 * k + (before ? 0 : 1)] : 0;
      out_putn(&o, b.s + from, end - from);
      end = from;
    }
    free(at);
    buf_free(&b);
  }
  out_flush(&o);
  if (re) regex_free(re);
  opts_free(&g);
  return status;
}

/* }================================================================== */


/*
** {==================================================================
** rev
** ===================================================================
*/

int t_rev (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {{"zero", '0', 0}, {NULL, 0, 0}};
  Opts g;
  Out o;
  int c, status = 0, utf8 = tool_utf8();
  char delim = '\n';
  Buf rb;
  size_t i;
  opts_init(&g, "rev", argc, argv, err);
  while ((c = opts_next(&g, "0", lo)) != 0) {
    if (c == '0') delim = '\0';
    else if (c == OPT_HELP) { opts_free(&g); return tool_help(out, "rev"); }
    else { opts_free(&g); return 1; }
  }
  if (g.ops.n == 0) vec_push(&g.ops, xstrdup("-"));
  out_init(&o, out);
  buf_init(&rb);
  for (i = 0; i < g.ops.n && !o.failed && !tool_stop(); i++) {
    In r;
    char *line;
    size_t len;
    int had;
    const char *name = g.ops.v[i];
    if (m_open(&r, "rev", name, in, err, OPEN_REV, "cannot open %s: Is a directory") != 0) {
      status = 1;
      continue;
    }
    while (in_line(&r, &line, &len, delim, &had)) {
      const unsigned char *s = (const unsigned char *)line;
      size_t k = len;
      rb.len = 0;
      if (!utf8) {
        while (k > 0) buf_putc(&rb, (char)s[--k]);
      }
      else {	/* whole characters, the last first */
        size_t p = 0;
        buf_putn(&rb, line, len);
        while (p < len) {
          int l = u8_len(s + p, len - p);
          memcpy(rb.s + (len - p - (size_t)l), s + p, (size_t)l);
          p += (size_t)l;
        }
      }
      out_putn(&o, rb.s, rb.len);
      if (had) out_putc(&o, delim);
      if (o.failed) break;
    }
    in_close(&r);
  }
  out_flush(&o);
  buf_free(&rb);
  opts_free(&g);
  return status;
}

/* }================================================================== */


/*
** {==================================================================
** nl
** ===================================================================
*/

typedef struct NlStyle {
  char type;	/* a t n p */
  Regex *re;
} NlStyle;

static int nl_style (NlStyle *s, const char *arg, const char *what, int err) {
  if ((arg[0] == 'a' || arg[0] == 't' || arg[0] == 'n') && arg[1] == '\0') {
    s->type = arg[0];
    return 0;
  }
  if (arg[0] == 'p') {
    char *msg = NULL;
    if (s->re) regex_free(s->re);
    s->re = regex_new(arg + 1, 0, &msg);
    if (s->re == NULL) {
      tool_err(err, "nl", "%s", msg ? msg : "invalid regular expression");
      free(msg);
      return -1;
    }
    s->type = 'p';
    return 0;
  }
  tool_err(err, "nl", "invalid %s numbering style: '%s'", what, arg);
  fd_printf(err, "Try 'nl --help' for more information.\n");
  return -1;
}


int t_nl (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {{"body-numbering", 'b', 1}, {"section-delimiter", 'd', 1},
    {"footer-numbering", 'f', 1}, {"header-numbering", 'h', 1}, {"line-increment", 'i', 1},
    {"join-blank-lines", 'l', 1}, {"number-format", 'n', 1}, {"no-renumber", 'p', 0},
    {"number-separator", 's', 1}, {"starting-line-number", 'v', 1}, {"number-width", 'w', 1},
    {NULL, 0, 0}};
  Opts g;
  Out o;
  NlStyle st[3];	/* header body footer */
  int c, status = 0, renumber = 1, cur = 1, k;
  long long start = 1, incr = 1, join = 1, width = 6, lineno, blanks = 0;
  const char *sep = "\t", *fmt = "rn";
  char del[3] = "\\:", hdr[7], bdy[5];
  size_t i, nolen, *m = NULL, nm = 2;
  char *noline;
  memset(st, 0, sizeof(st));
  st[0].type = 'n';
  st[1].type = 't';
  st[2].type = 'n';
  opts_init(&g, "nl", argc, argv, err);
  while ((c = opts_next(&g, "b:d:f:h:i:l:n:ps:v:w:", lo)) != 0) {
    switch (c) {
      case 'b': if (nl_style(&st[1], g.arg, "body", err)) goto bad; break;
      case 'h': if (nl_style(&st[0], g.arg, "header", err)) goto bad; break;
      case 'f': if (nl_style(&st[2], g.arg, "footer", err)) goto bad; break;
      case 'd':
        if (g.arg[0]) {
          del[0] = g.arg[0];
          del[1] = g.arg[1] ? g.arg[1] : ':';
        }
        break;
      case 'i':
        if (num_arg(g.arg, LLONG_MIN, &incr)) {
          tool_err(err, "nl", "invalid line number increment: '%s'", g.arg);
          goto bad;
        }
        break;
      case 'l':
        if (num_arg(g.arg, 1, &join)) {
          tool_err(err, "nl", "invalid line number of blank lines: '%s'", g.arg);
          goto bad;
        }
        break;
      case 'n':
        if (strcmp(g.arg, "ln") && strcmp(g.arg, "rn") && strcmp(g.arg, "rz")) {
          tool_err(err, "nl", "invalid line numbering format: '%s'", g.arg);
          fd_printf(err, "Try 'nl --help' for more information.\n");
          goto bad;
        }
        fmt = g.arg;
        break;
      case 'p': renumber = 0; break;
      case 's': sep = g.arg; break;
      case 'v':
        if (num_arg(g.arg, LLONG_MIN, &start)) {
          tool_err(err, "nl", "invalid starting line number: '%s'", g.arg);
          goto bad;
        }
        break;
      case 'w':
        if (num_arg(g.arg, LLONG_MIN, &width) || width < 1 || width > INT_MAX) {
          int range = num_arg(g.arg, LLONG_MIN, &width) == 0;
          tool_err(err, "nl", "invalid line number field width: '%s'%s", g.arg,
                   range ? ": Numerical result out of range" : "");
          goto bad;
        }
        break;
      case OPT_HELP: opts_free(&g); return tool_help(out, "nl");
      default: goto bad;
    }
  }
  snprintf(hdr, sizeof(hdr), "%s%s%s", del, del, del);
  snprintf(bdy, sizeof(bdy), "%s%s", del, del);
  nolen = (size_t)width + strlen(sep);	/* an unnumbered line: blanks instead */
  noline = (char *)xmalloc(nolen + 1);
  memset(noline, ' ', nolen);
  noline[nolen] = '\0';
  for (k = 0; k < 3; k++)
    if (st[k].re && 2 * ((size_t)regex_nsub(st[k].re) + 1) > nm) nm = 2 * ((size_t)regex_nsub(st[k].re) + 1);
  m = (size_t *)xmalloc(nm * sizeof(size_t));
  lineno = start;
  if (g.ops.n == 0) vec_push(&g.ops, xstrdup("-"));
  out_init(&o, out);
  for (i = 0; i < g.ops.n && !o.failed && !tool_stop(); i++) {
    In r;
    char *line;
    size_t len;
    if (m_open(&r, "nl", g.ops.v[i], in, err, OPEN_PLAIN, NULL) != 0) {
      status = 1;
      continue;
    }
    while (in_line(&r, &line, &len, '\n', NULL)) {
      int sec = -1, number;
      if (len >= 2 && memcmp(line, del, 2) == 0) {
        if (len == 6 && memcmp(line, hdr, 6) == 0) sec = 0;
        else if (len == 4 && memcmp(line, bdy, 4) == 0) sec = 1;
        else if (len == 2) sec = 2;
      }
      if (sec >= 0) {	/* a section starts: an empty line, maybe from 1 again */
        cur = sec;
        if (renumber) lineno = start;
        out_putc(&o, '\n');
        continue;
      }
      switch (st[cur].type) {
        case 'a':
          if (join > 1) {
            number = len > 0 || ++blanks == join;
            if (number) blanks = 0;
          }
          else number = 1;
          break;
        case 't': number = len > 0; break;
        case 'p': number = regex_match(st[cur].re, line, len, 0, 0, m); break;
        default: number = 0; break;
      }
      if (number) {
        if (fmt[0] == 'l') out_printf(&o, "%-*lld", (int)width, lineno);
        else if (fmt[1] == 'z') out_printf(&o, "%0*lld", (int)width, lineno);
        else out_printf(&o, "%*lld", (int)width, lineno);
        out_puts(&o, sep);
        lineno += incr;
      }
      else out_putn(&o, noline, nolen);
      out_putn(&o, line, len);
      out_putc(&o, '\n');
      if (o.failed) break;
    }
    in_close(&r);
  }
  out_flush(&o);
  free(noline);
  free(m);
  for (k = 0; k < 3; k++)
    if (st[k].re) regex_free(st[k].re);
  opts_free(&g);
  return status;
bad:
  for (k = 0; k < 3; k++)
    if (st[k].re) regex_free(st[k].re);
  opts_free(&g);
  return 1;
}

/* }================================================================== */


/*
** {==================================================================
** paste
** ===================================================================
*/

#define PASTE_NONE	(-1)	/* "\0" in the list: no delimiter */

/* -d LIST with \n \t \\ \0 ... -> delimiters, PASTE_NONE for "\0" */
static int paste_delims (const char *s, int **out, size_t *n, int err) {
  const char *all = s;
  size_t k = 0;
  int *d = (int *)xmalloc((strlen(s) + 1) * sizeof(int));
  for (; *s; s++) {
    if (*s != '\\') {
      d[k++] = (unsigned char)*s;
      continue;
    }
    s++;
    switch (*s) {
      case '\0':
        tool_err(err, "paste", "delimiter list ends with an unescaped backslash: %s", all);
        free(d);
        return -1;
      case '0': d[k++] = PASTE_NONE; break;
      case 'b': d[k++] = '\b'; break;
      case 'f': d[k++] = '\f'; break;
      case 'n': d[k++] = '\n'; break;
      case 'r': d[k++] = '\r'; break;
      case 't': d[k++] = '\t'; break;
      case 'v': d[k++] = '\v'; break;
      default: d[k++] = (unsigned char)*s; break;
    }
  }
  if (k == 0) d[k++] = PASTE_NONE;
  *out = d;
  *n = k;
  return 0;
}


int t_paste (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {{"delimiters", 'd', 1}, {"serial", 's', 0},
    {"zero-terminated", 'z', 0}, {NULL, 0, 0}};
  Opts g;
  Out o;
  int c, serial = 0, status = 0, *delims = NULL;
  char eol = '\n';
  const char *dlist = "\t";
  size_t nd, i, nf;
  opts_init(&g, "paste", argc, argv, err);
  while ((c = opts_next(&g, "d:sz", lo)) != 0) {
    switch (c) {
      case 'd': dlist = g.arg; break;
      case 's': serial = 1; break;
      case 'z': eol = '\0'; break;
      case OPT_HELP: opts_free(&g); return tool_help(out, "paste");
      default: opts_free(&g); return 1;
    }
  }
  if (paste_delims(dlist, &delims, &nd, err) != 0) {
    opts_free(&g);
    return 1;
  }
  if (g.ops.n == 0) vec_push(&g.ops, xstrdup("-"));
  nf = g.ops.n;
  out_init(&o, out);
  if (serial) {
    for (i = 0; i < nf && !o.failed && !tool_stop(); i++) {
      In r;
      char *line;
      size_t len, di = 0;
      int had, first = 1;
      if (m_open(&r, "paste", g.ops.v[i], in, err, OPEN_PLAIN, NULL) != 0) {
        status = 1;
        continue;
      }
      while (in_line(&r, &line, &len, eol, &had)) {
        if (!first) {	/* the line end before this line becomes a delimiter */
          if (delims[di] != PASTE_NONE) out_putc(&o, delims[di]);
          if (++di == nd) di = 0;
        }
        out_putn(&o, line, len);
        first = 0;
      }
      out_putc(&o, eol);
      in_close(&r);
    }
  }
  else {
    In *rs = (In *)xmalloc(nf * sizeof(In)), std;
    In **fp = (In **)xmalloc(nf * sizeof(In *));
    char *saved = (char *)xmalloc(nf + 1);
    size_t open_n = 0, stdin_used = 0;
    for (i = 0; i < nf; i++) {
      if (strcmp(g.ops.v[i], "-") == 0) {	/* every "-" shares stdin */
        if (!stdin_used++) in_init(&std, in, 0);
        fp[i] = &std;
      }
      else if (m_open(&rs[i], "paste", g.ops.v[i], in, err, OPEN_PLAIN, NULL) != 0) {
        size_t j;
        for (j = 0; j < i; j++)
          if (fp[j] != &std) in_close(fp[j]);
        if (stdin_used) in_close(&std);
        free(rs);
        free(fp);
        free(saved);
        free(delims);
        opts_free(&g);
        return 1;
      }
      else fp[i] = &rs[i];
      open_n++;
    }
    while (open_n > 0 && !o.failed && !tool_stop()) {
      size_t di = 0, nsaved = 0;
      for (i = 0; i < nf && open_n > 0; i++) {
        char *line;
        size_t len;
        int had, got = 0;
        if (fp[i] != NULL && in_line(fp[i], &line, &len, eol, &had)) {
          got = 1;
          if (nsaved) {	/* delimiters of files that ended, held back */
            out_putn(&o, saved, nsaved);
            nsaved = 0;
          }
          out_putn(&o, line, len);
        }
        if (!got) {
          if (fp[i] != NULL) {
            if (fp[i] != &std) in_close(fp[i]);
            fp[i] = NULL;
            open_n--;
          }
          if (i + 1 == nf) {
            if (open_n) {
              out_putn(&o, saved, nsaved);
              nsaved = 0;
              out_putc(&o, eol);
            }
          }
          else {
            if (delims[di] != PASTE_NONE) saved[nsaved++] = (char)delims[di];
            if (++di == nd) di = 0;
          }
        }
        else if (i + 1 == nf) out_putc(&o, eol);
        else {
          if (delims[di] != PASTE_NONE) out_putc(&o, delims[di]);
          if (++di == nd) di = 0;
        }
      }
    }
    for (i = 0; i < nf; i++)
      if (fp[i] != NULL && fp[i] != &std) in_close(fp[i]);
    if (stdin_used) in_close(&std);
    free(rs);
    free(fp);
    free(saved);
  }
  out_flush(&o);
  free(delims);
  opts_free(&g);
  return status;
}

/* }================================================================== */


/*
** {==================================================================
** comm
** ===================================================================
*/

static int bytes_cmp (const char *a, size_t al, const char *b, size_t bl) {
  int d = memcmp(a, b, al < bl ? al : bl);
  if (d) return d;
  return al < bl ? -1 : al > bl;
}


int t_comm (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {{"check-order", 1001, 0}, {"nocheck-order", 1002, 0},
    {"output-delimiter", 1003, 1}, {"total", 1004, 0}, {"zero-terminated", 'z', 0},
    {NULL, 0, 0}};
  Opts g;
  Out o;
  int c, show[4] = {0, 1, 1, 1}, check = 0, total = 0, k, warned[2] = {0, 0}, unpaired = 0;
  char eol = '\n';
  const char *dl = "\t";
  size_t dlen;
  In r[2];
  char *line[2];
  size_t len[2];
  int have[2];
  Buf cur[2];
  long long count[4] = {0, 0, 0, 0};
  opts_init(&g, "comm", argc, argv, err);
  while ((c = opts_next(&g, "123z", lo)) != 0) {
    switch (c) {
      case '1': case '2': case '3': show[c - '0'] = 0; break;
      case 'z': eol = '\0'; break;
      case 1001: check = 1; break;
      case 1002: check = -1; break;
      case 1003: dl = g.arg; break;
      case 1004: total = 1; break;
      case OPT_HELP: opts_free(&g); return tool_help(out, "comm");
      default: opts_free(&g); return 1;
    }
  }
  if (g.ops.n != 2) {
    if (g.ops.n == 0) tool_err(err, "comm", "missing operand");
    else if (g.ops.n == 1) tool_err(err, "comm", "missing operand after '%s'", g.ops.v[0]);
    else tool_err(err, "comm", "extra operand '%s'", g.ops.v[2]);
    fd_printf(err, "Try 'comm --help' for more information.\n");
    opts_free(&g);
    return 1;
  }
  dlen = *dl ? strlen(dl) : 1;	/* an empty delimiter is a NUL */
  for (k = 0; k < 2; k++) {
    if (m_open(&r[k], "comm", g.ops.v[k], in, err, OPEN_PLAIN, NULL) != 0) {
      if (k == 1) in_close(&r[0]);
      opts_free(&g);
      return 1;
    }
  }
  buf_init(&cur[0]);
  buf_init(&cur[1]);
  out_init(&o, out);
  for (k = 0; k < 2; k++) {
    have[k] = in_line(&r[k], &line[k], &len[k], eol, NULL);
    if (have[k]) buf_putn(&cur[k], line[k], len[k]);
  }
  while ((have[0] || have[1]) && !o.failed && !tool_stop()) {
    int order, col, fill[2];
    if (!have[0]) order = 1;
    else if (!have[1]) order = -1;
    else order = bytes_cmp(cur[0].s, cur[0].len, cur[1].s, cur[1].len);
    col = order == 0 ? 3 : order < 0 ? 1 : 2;
    if (order != 0) unpaired = 1;
    count[col]++;
    if (show[col]) {
      Buf *b = &cur[col == 2 ? 1 : 0];
      if (col >= 2 && show[1]) out_putn(&o, dl, dlen);
      if (col == 3 && show[2]) out_putn(&o, dl, dlen);
      out_putn(&o, b->s, b->len);
      out_putc(&o, eol);
    }
    fill[0] = order <= 0;
    fill[1] = order >= 0;
    for (k = 0; k < 2; k++) {
      if (!fill[k]) continue;
      have[k] = in_line(&r[k], &line[k], &len[k], eol, NULL);
      if (!have[k]) continue;
      /* the order: always with --check-order, by default once lines differ */
      if (check >= 0 && (check == 1 || unpaired) && !warned[k] &&
          bytes_cmp(cur[k].s, cur[k].len, line[k], len[k]) > 0) {
        out_flush(&o);
        tool_err(err, "comm", "file %d is not in sorted order", k + 1);
        warned[k] = 1;
        if (check == 1) {
          in_close(&r[0]);
          in_close(&r[1]);
          buf_free(&cur[0]);
          buf_free(&cur[1]);
          opts_free(&g);
          return 1;
        }
      }
      cur[k].len = 0;
      buf_putn(&cur[k], line[k], len[k]);
    }
  }
  if (total) {
    out_printf(&o, "%lld", count[1]);
    out_putn(&o, dl, dlen);
    out_printf(&o, "%lld", count[2]);
    out_putn(&o, dl, dlen);
    out_printf(&o, "%lld", count[3]);
    out_putn(&o, dl, dlen);
    out_puts(&o, "total");
    out_putc(&o, eol);
  }
  out_flush(&o);
  in_close(&r[0]);
  in_close(&r[1]);
  buf_free(&cur[0]);
  buf_free(&cur[1]);
  opts_free(&g);
  if (warned[0] || warned[1]) {
    tool_err(err, "comm", "input is not in sorted order");
    return 1;
  }
  return 0;
}

/* }================================================================== */


/*
** {==================================================================
** join
** ===================================================================
*/

typedef struct JLine {
  char *s;
  size_t len, nf, cap;
  size_t *fb, *fl;	/* the fields: start and length */
  int ref;
} JLine;

typedef struct JFile {
  In r;
  const char *name;
  long long lineno;
  JLine *prev;	/* for the order check */
  JLine **seq;	/* lines with the same key */
  size_t count, alloc;
  int warned;
} JFile;

typedef struct Join {
  JFile f[2];
  size_t jf[2];	/* the join fields, from 0 */
  int tab;	/* -1: blanks */
  int icase, check, unpaired, autoformat;
  size_t autocount[2];
  size_t *ofile, *ofield, on;	/* -o: file 0 is the join field */
  const char *empty;
  int pairs, un[2];
  char eol;
  Out *o;
  int err, fatal;
} Join;

static JLine j_blank;	/* no fields: the missing side of an unpaired line */

static void j_unref (JLine *l) {
  if (l == NULL || --l->ref > 0) return;
  free(l->s);
  free(l->fb);
  free(l->fl);
  free(l);
}

static void j_field (JLine *l, size_t b, size_t n) {
  if (l->nf == l->cap) {
    l->cap = l->cap ? l->cap * 2 : 8;
    l->fb = (size_t *)xrealloc(l->fb, l->cap * sizeof(size_t));
    l->fl = (size_t *)xrealloc(l->fl, l->cap * sizeof(size_t));
  }
  l->fb[l->nf] = b;
  l->fl[l->nf++] = n;
}

/* the fields of a line, as GNU's xfields() */
static void j_split (const Join *J, JLine *l) {
  size_t p = 0, lim = l->len;
  const char *s = l->s;
  if (lim == 0) return;
  if (J->tab >= 0 && J->tab != '\n') {
    const char *sep;
    while ((sep = (const char *)memchr(s + p, J->tab, lim - p)) != NULL) {
      j_field(l, p, (size_t)(sep - s) - p);
      p = (size_t)(sep - s) + 1;
    }
  }
  else if (J->tab < 0) {
#define JSEP(c)	((c) == ' ' || (c) == '\t' || (c) == '\n')
    while (JSEP(s[p]))
      if (++p == lim) return;
    do {
      size_t e = p + 1;
      while (e != lim && !JSEP(s[e])) e++;
      j_field(l, p, e - p);
      if (e == lim) return;
      for (p = e + 1; p != lim && JSEP(s[p]); p++) continue;
    } while (p != lim);
#undef JSEP
  }
  j_field(l, p, lim - p);
}

static int j_keycmp (const Join *J, const JLine *a, const JLine *b, size_t ja, size_t jb) {
  const char *pa = NULL, *pb = NULL;
  size_t la = 0, lb = 0, k, n;
  int d = 0;
  if (ja < a->nf) { pa = a->s + a->fb[ja]; la = a->fl[ja]; }
  if (jb < b->nf) { pb = b->s + b->fb[jb]; lb = b->fl[jb]; }
  if (la == 0) return lb == 0 ? 0 : -1;
  if (lb == 0) return 1;
  n = la < lb ? la : lb;
  if (J->icase) {
    for (k = 0; k < n && d == 0; k++)
      d = toupper((unsigned char)pa[k]) - toupper((unsigned char)pb[k]);
  }
  else d = memcmp(pa, pb, n);
  if (d) return d;
  return la < lb ? -1 : la != lb;
}

/* the next line of file w (0 or 1), checked for order; NULL at the end */
static JLine *j_getline (Join *J, int w) {
  JFile *f = &J->f[w];
  char *s;
  size_t len;
  JLine *l;
  if (J->fatal || !in_line(&f->r, &s, &len, J->eol, NULL)) return NULL;
  l = (JLine *)xmalloc(sizeof(JLine));
  memset(l, 0, sizeof(*l));
  l->s = xstrndup(s, len);
  l->len = len;
  l->ref = 1;
  f->lineno++;
  j_split(J, l);
  if (f->prev && J->check >= 0 && (J->check == 1 || J->unpaired) && !f->warned &&
      j_keycmp(J, f->prev, l, J->jf[w], J->jf[w]) > 0) {
    out_flush(J->o);
    tool_err(J->err, "join", "%s:%lld: is not sorted: %.*s", f->name, f->lineno, (int)len, l->s);
    f->warned = 1;
    if (J->check == 1) J->fatal = 1;
  }
  j_unref(f->prev);
  f->prev = l;
  l->ref++;
  return l;
}

static int j_getseq (Join *J, int w) {
  JFile *f = &J->f[w];
  JLine *l = j_getline(J, w);
  if (l == NULL) return 0;
  if (f->count == f->alloc) {
    f->alloc = f->alloc ? f->alloc * 2 : 16;
    f->seq = (JLine **)xrealloc(f->seq, f->alloc * sizeof(JLine *));
  }
  f->seq[f->count++] = l;
  return 1;
}

static int j_advance (Join *J, int w, int advance) {
  JFile *f = &J->f[w];
  if (advance) {
    size_t k;
    for (k = 0; k < f->count; k++) j_unref(f->seq[k]);
    f->count = 0;
  }
  return j_getseq(J, w);
}

static void j_prfield (Join *J, size_t n, const JLine *l) {
  if (n < l->nf && l->fl[n] > 0) out_putn(J->o, l->s + l->fb[n], l->fl[n]);
  else if (J->empty) out_puts(J->o, J->empty);
}

static void j_prfields (Join *J, const JLine *l, size_t jf, size_t autocount) {
  size_t i, n = J->autoformat ? autocount : l->nf;
  char sep = J->tab < 0 ? ' ' : (char)J->tab;
  for (i = 0; i < jf && i < n; i++) {
    out_putc(J->o, sep);
    j_prfield(J, i, l);
  }
  for (i = jf + 1; i < n; i++) {
    out_putc(J->o, sep);
    j_prfield(J, i, l);
  }
}

static void j_print (Join *J, const JLine *a, const JLine *b) {
  char sep = J->tab < 0 ? ' ' : (char)J->tab;
  if (J->fatal) return;	/* --check-order failed: GNU has exited */
  if (J->on > 0) {
    size_t k;
    for (k = 0; k < J->on; k++) {
      if (k > 0) out_putc(J->o, sep);
      if (J->ofile[k] == 0) {
        if (a == &j_blank) j_prfield(J, J->jf[1], b);
        else j_prfield(J, J->jf[0], a);
      }
      else j_prfield(J, J->ofield[k], J->ofile[k] == 1 ? a : b);
    }
  }
  else {
    if (a == &j_blank) j_prfield(J, J->jf[1], b);
    else j_prfield(J, J->jf[0], a);
    j_prfields(J, a, J->jf[0], J->autocount[0]);
    j_prfields(J, b, J->jf[1], J->autocount[1]);
  }
  out_putc(J->o, J->eol);
}

static int j_fieldnum (Join *J, const char *s, size_t *out) {
  long long v;
  if (num_arg(s, 1, &v) != 0) {
    tool_err(J->err, "join", "invalid field number: '%s'", s);
    return -1;
  }
  *out = (size_t)v - 1;
  return 0;
}


/* -o "1.2,2.3 0": 0 ok */
static int j_outlist (Join *J, const char *spec) {
  const char *p = spec;
  while (*p) {
    size_t file, field = 0;
    const char *q = p;
    size_t n;
    char tok[64];
    while (*q && *q != ',' && *q != ' ' && *q != '\t') q++;
    n = (size_t)(q - p);
    if (n == 0) { p++; continue; }
    snprintf(tok, sizeof(tok), "%.*s", (int)(n < 63 ? n : 63), p);
    if (tok[0] == '0' && tok[1] == '\0') file = 0;
    else if ((tok[0] == '1' || tok[0] == '2') && tok[1] == '.') {
      file = (size_t)(tok[0] - '0');
      if (j_fieldnum(J, tok + 2, &field)) return -1;
    }
    else {
      if (tok[0] == '0' || tok[0] == '1' || tok[0] == '2')
        tool_err(J->err, "join", "invalid field specifier: '%s'", tok);
      else tool_err(J->err, "join", "invalid file number in field spec: '%s'", tok);
      return -1;
    }
    J->ofile = (size_t *)xrealloc(J->ofile, (J->on + 1) * sizeof(size_t));
    J->ofield = (size_t *)xrealloc(J->ofield, (J->on + 1) * sizeof(size_t));
    J->ofile[J->on] = file;
    J->ofield[J->on++] = field;
    p = q;
  }
  return 0;
}


static void j_run (Join *J, int header) {
  JFile *f1 = &J->f[0], *f2 = &J->f[1];
  size_t i, j;
  j_getseq(J, 0);
  j_getseq(J, 1);
  if (J->autoformat) {
    J->autocount[0] = f1->count ? f1->seq[0]->nf : 0;
    J->autocount[1] = f2->count ? f2->seq[0]->nf : 0;
  }
  if (header && (f1->count || f2->count)) {	/* --header: the first lines */
    j_print(J, f1->count ? f1->seq[0] : &j_blank, f2->count ? f2->seq[0] : &j_blank);
    j_unref(f1->prev);
    j_unref(f2->prev);
    f1->prev = f2->prev = NULL;
    if (f1->count) j_advance(J, 0, 1);
    if (f2->count) j_advance(J, 1, 1);
  }
  while (f1->count && f2->count && !J->fatal && !tool_stop() && !J->o->failed) {
    int diff = j_keycmp(J, f1->seq[0], f2->seq[0], J->jf[0], J->jf[1]), eof1 = 0, eof2 = 0;
    size_t n1, n2;
    if (diff < 0) {
      if (J->un[0]) j_print(J, f1->seq[0], &j_blank);
      j_advance(J, 0, 1);
      J->unpaired = 1;
      continue;
    }
    if (diff > 0) {
      if (J->un[1]) j_print(J, &j_blank, f2->seq[0]);
      j_advance(J, 1, 1);
      J->unpaired = 1;
      continue;
    }
    /* the run of lines with this key in each file */
    do {
      if (!j_advance(J, 0, 0)) { eof1 = 1; break; }
    } while (!j_keycmp(J, f1->seq[f1->count - 1], f2->seq[0], J->jf[0], J->jf[1]));
    do {
      if (!j_advance(J, 1, 0)) { eof2 = 1; break; }
    } while (!j_keycmp(J, f1->seq[0], f2->seq[f2->count - 1], J->jf[0], J->jf[1]));
    n1 = eof1 ? f1->count : f1->count - 1;
    n2 = eof2 ? f2->count : f2->count - 1;
    if (J->pairs)
      for (i = 0; i < n1; i++)
        for (j = 0; j < n2; j++) j_print(J, f1->seq[i], f2->seq[j]);
    for (i = 0; i < n1; i++) j_unref(f1->seq[i]);
    if (!eof1) f1->seq[0] = f1->seq[f1->count - 1];
    f1->count = eof1 ? 0 : 1;
    for (j = 0; j < n2; j++) j_unref(f2->seq[j]);
    if (!eof2) f2->seq[0] = f2->seq[f2->count - 1];
    f2->count = eof2 ? 0 : 1;
  }
  /* the tails: printed with -a / -v, read for the order check anyway */
  {
    int checktail = J->check >= 0 && !(f1->warned && f2->warned);
    int w;
    for (w = 0; w < 2; w++) {
      JFile *f = &J->f[w], *other = &J->f[1 - w];
      JLine *l;
      if (J->fatal || !(J->un[w] || checktail) || f->count == 0) continue;
      if (J->un[w]) {
        if (w == 0) j_print(J, f->seq[0], &j_blank);
        else j_print(J, &j_blank, f->seq[0]);
      }
      if (other->count) J->unpaired = 1;
      while ((l = j_getline(J, w)) != NULL) {
        if (J->un[w]) {
          if (w == 0) j_print(J, l, &j_blank);
          else j_print(J, &j_blank, l);
        }
        j_unref(l);
        if (f->warned && !J->un[w]) break;
        if (tool_stop() || J->o->failed) break;
      }
    }
  }
}


int t_join (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {{"ignore-case", 'i', 0}, {"check-order", 1001, 0},
    {"nocheck-order", 1002, 0}, {"zero-terminated", 'z', 0}, {"header", 1003, 0},
    {NULL, 0, 0}};
  Opts g;
  Out o;
  Join J;
  int c, k, status = 0, header = 0;
  memset(&J, 0, sizeof(J));
  J.tab = -1;
  J.eol = '\n';
  J.pairs = 1;
  J.err = err;
  J.o = &o;
  opts_init(&g, "join", argc, argv, err);
  while ((c = opts_next(&g, "a:e:i1:2:j:o:t:v:z", lo)) != 0) {
    switch (c) {
      case 'a': case 'v':
        if (strcmp(g.arg, "1") && strcmp(g.arg, "2")) {
          tool_err(err, "join", "invalid field number: '%s'", g.arg);
          goto bad;
        }
        J.un[g.arg[0] - '1'] = 1;
        if (c == 'v') J.pairs = 0;
        break;
      case 'e': J.empty = g.arg; break;
      case 'i': J.icase = 1; break;
      case '1': if (j_fieldnum(&J, g.arg, &J.jf[0])) goto bad; break;
      case '2': if (j_fieldnum(&J, g.arg, &J.jf[1])) goto bad; break;
      case 'j':
        if (j_fieldnum(&J, g.arg, &J.jf[0])) goto bad;
        J.jf[1] = J.jf[0];
        break;
      case 'o':
        if (strcmp(g.arg, "auto") == 0) J.autoformat = 1;
        else if (j_outlist(&J, g.arg)) goto bad;
        break;
      case 't': {
        int t = (unsigned char)g.arg[0];
        if (t == 0) t = '\n';	/* '': the whole line is the key */
        if (g.arg[0] && g.arg[1]) {
          if (strcmp(g.arg, "\\0") == 0) t = 0;
          else {
            tool_err(err, "join", "multi-character tab '%s'", g.arg);
            goto bad;
          }
        }
        if (J.tab >= 0 && J.tab != t) {
          tool_err(err, "join", "incompatible tabs");
          goto bad;
        }
        J.tab = t;
        break;
      }
      case 'z': J.eol = '\0'; break;
      case 1001: J.check = 1; break;
      case 1002: J.check = -1; break;
      case 1003: header = 1; break;
      case OPT_HELP: opts_free(&g); return tool_help(out, "join");
      default: goto bad;
    }
  }
  if (g.ops.n != 2) {
    if (g.ops.n == 0) tool_err(err, "join", "missing operand");
    else if (g.ops.n == 1) tool_err(err, "join", "missing operand after '%s'", g.ops.v[0]);
    else tool_err(err, "join", "extra operand '%s'", g.ops.v[2]);
    fd_printf(err, "Try 'join --help' for more information.\n");
    goto bad;
  }
  if (strcmp(g.ops.v[0], "-") == 0 && strcmp(g.ops.v[1], "-") == 0) {
    tool_err(err, "join", "both files cannot be standard input");
    goto bad;
  }
  if (J.on > 0) J.autoformat = 0;
  for (k = 0; k < 2; k++) {
    J.f[k].name = g.ops.v[k];
    if (m_open(&J.f[k].r, "join", g.ops.v[k], in, err, OPEN_PLAIN, "read error: Is a directory") != 0) {
      if (k == 1) in_close(&J.f[0].r);
      goto bad;
    }
  }
  out_init(&o, out);
  j_run(&J, header);
  out_flush(&o);
  for (k = 0; k < 2; k++) {
    size_t i;
    for (i = 0; i < J.f[k].count; i++) j_unref(J.f[k].seq[i]);
    free(J.f[k].seq);
    j_unref(J.f[k].prev);
    in_close(&J.f[k].r);
  }
  if (J.fatal) status = 1;
  else if (J.f[0].warned || J.f[1].warned) {
    tool_err(err, "join", "input is not in sorted order");
    status = 1;
  }
  free(J.ofile);
  free(J.ofield);
  opts_free(&g);
  return status;
bad:
  free(J.ofile);
  free(J.ofield);
  opts_free(&g);
  return 1;
}

/* }================================================================== */


/*
** {==================================================================
** fold
** ===================================================================
*/

/* the column after unit u (1 byte, or one UTF-8 character) */
static size_t fold_adjust (size_t col, const unsigned char *u, int ulen, int bytes) {
  if (bytes) return col + (size_t)ulen;
  if (u[0] == '\b') return col > 0 ? col - 1 : 0;
  if (u[0] == '\r') return 0;
  if (u[0] == '\t') return col + 8 - col % 8;
  if (ulen > 1) return col + (size_t)uc_width(u8_code(u, ulen));
  return col + 1;
}

static size_t fold_cols (const unsigned char *s, size_t n, int bytes, int utf8) {
  size_t col = 0, i = 0;
  while (i < n) {
    int l = utf8 ? u8_len(s + i, n - i) : 1;
    col = fold_adjust(col, s + i, l, bytes);
    i += (size_t)l;
  }
  return col;
}


int t_fold (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {{"bytes", 'b', 0}, {"spaces", 's', 0}, {"width", 'w', 1},
    {NULL, 0, 0}};
  Opts g;
  Out o;
  int c, bytes = 0, spaces = 0, status = 0, nargc, utf8;
  long long w = 80;
  size_t i, width;
  char **v = legacy_num(argc, argv, &nargc, "-w");
  Buf lb;
  opts_init(&g, "fold", nargc, v, err);
  while ((c = opts_next(&g, "bsw:", lo)) != 0) {
    switch (c) {
      case 'b': bytes = 1; break;
      case 's': spaces = 1; break;
      case 'w':
        if (num_arg(g.arg, 1, &w) != 0) {
          tool_err(err, "fold", "invalid number of columns: '%s'%s", g.arg,
                   num_arg(g.arg, LLONG_MIN, &w) == 0 ? ": Numerical result out of range" : "");
          opts_free(&g);
          free(v);
          return 1;
        }
        break;
      case OPT_HELP: opts_free(&g); free(v); return tool_help(out, "fold");
      default: opts_free(&g); free(v); return 1;
    }
  }
  width = (size_t)w;
  utf8 = !bytes && tool_utf8();
  if (g.ops.n == 0) vec_push(&g.ops, xstrdup("-"));
  out_init(&o, out);
  buf_init(&lb);
  for (i = 0; i < g.ops.n && !o.failed && !tool_stop(); i++) {
    In r;
    char *line;
    size_t len;
    int had;
    if (m_open(&r, "fold", g.ops.v[i], in, err, OPEN_PLAIN, NULL) != 0) {
      status = 1;
      continue;
    }
    while (in_line(&r, &line, &len, '\n', &had) && !o.failed) {
      const unsigned char *s = (const unsigned char *)line;
      size_t col = 0, p = 0;
      lb.len = 0;
      while (p < len) {
        int ul = utf8 ? u8_len(s + p, len - p) : 1;
        for (;;) {	/* GNU's "rescan" */
          size_t ncol = fold_adjust(col, s + p, ul, bytes);
          if (ncol <= width) {
            col = ncol;
            buf_putn(&lb, (const char *)s + p, (size_t)ul);
            break;
          }
          if (spaces) {	/* break after the last blank, if there is one */
            size_t e = lb.len;
            while (e > 0 && lb.s[e - 1] != ' ' && lb.s[e - 1] != '\t') e--;
            if (e > 0) {
              out_putn(&o, lb.s, e);
              out_putc(&o, '\n');
              memmove(lb.s, lb.s + e, lb.len - e);
              lb.len -= e;
              col = fold_cols((const unsigned char *)lb.s, lb.len, bytes, utf8);
              continue;
            }
          }
          if (lb.len == 0) {
            col = ncol;
            buf_putn(&lb, (const char *)s + p, (size_t)ul);
            break;
          }
          out_putn(&o, lb.s, lb.len);
          out_putc(&o, '\n');
          lb.len = 0;
          col = 0;
        }
        p += (size_t)ul;
      }
      out_putn(&o, lb.s, lb.len);
      if (had) out_putc(&o, '\n');
    }
    in_close(&r);
  }
  out_flush(&o);
  buf_free(&lb);
  opts_free(&g);
  free(v);
  return status;
}

/* }================================================================== */


/*
** {==================================================================
** expand, unexpand
** ===================================================================
*/

int t_expand (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {{"initial", 'i', 0}, {"tabs", 't', 1}, {NULL, 0, 0}};
  Opts g;
  Out o;
  Tabs t;
  CStream *cs;
  int c, initial = 0, nargc, utf8 = tool_utf8(), status;
  char **v = legacy_num(argc, argv, &nargc, "-t");
  memset(&t, 0, sizeof(t));
  opts_init(&g, "expand", nargc, v, err);
  while ((c = opts_next(&g, "it:", lo)) != 0) {
    switch (c) {
      case 'i': initial = 1; break;
      case 't': if (tabs_add(&t, "expand", err, g.arg)) goto bad; break;
      case OPT_HELP: opts_free(&g); free(v); free(t.list); return tool_help(out, "expand");
      default: goto bad;
    }
  }
  tabs_finish(&t);
  if (g.ops.n == 0) vec_push(&g.ops, xstrdup("-"));
  cs = (CStream *)xmalloc(sizeof(CStream));
  cs_init(cs, "expand", &g.ops, in, err);
  out_init(&o, out);
  for (;;) {	/* each line */
    long long col = 0;
    size_t idx = 0;
    int convert = 1;
    do {
      c = cs_getc(cs);
      if (convert) {
        if (c == '\t') {
          int last;
          long long next = tabs_next(&t, col, &idx, &last);
          if (last) next = col + 1;
          while (++col < next) out_putc(&o, ' ');
          c = ' ';
        }
        else if (c == '\b') {
          col -= col > 0;
          idx -= idx > 0;
        }
        else if (!(utf8 && (c & 0xC0) == 0x80)) col++;
        convert &= !initial || c == ' ' || c == '\t';
      }
      if (c < 0) goto done;
      out_putc(&o, c);
    } while (c != '\n' && !o.failed);
    if (o.failed) break;
  }
done:
  out_flush(&o);
  cs_close(cs);
  status = cs->status;
  free(cs);
  free(t.list);
  opts_free(&g);
  free(v);
  return status;
bad:
  free(t.list);
  opts_free(&g);
  free(v);
  return 1;
}


int t_unexpand (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {{"all", 'a', 0}, {"first-only", 1001, 0}, {"tabs", 't', 1},
    {"\002", 1002, 1}, {NULL, 0, 0}};
  Opts g;
  Out o;
  Tabs t;
  CStream *cs;
  Buf pend;
  int c, all = 0, first_only = 0, had_t = 0, nargc, utf8 = tool_utf8(), status;
  char **v = legacy_num(argc, argv, &nargc, "--\002");	/* "-4": not -a */
  memset(&t, 0, sizeof(t));
  opts_init(&g, "unexpand", nargc, v, err);
  while ((c = opts_next(&g, "at:", lo)) != 0) {
    switch (c) {
      case 'a': all = 1; break;
      case 't':
        if (tabs_add(&t, "unexpand", err, g.arg)) goto bad;
        had_t = 1;
        break;
      case 1001: first_only = 1; break;
      case 1002: if (tabs_add(&t, "unexpand", err, g.arg)) goto bad; break;
      case OPT_HELP: opts_free(&g); free(v); free(t.list); return tool_help(out, "unexpand");
      default: goto bad;
    }
  }
  if (had_t) all = 1;	/* -t means -a, unless --first-only */
  if (first_only) all = 0;
  tabs_finish(&t);
  if (g.ops.n == 0) vec_push(&g.ops, xstrdup("-"));
  cs = (CStream *)xmalloc(sizeof(CStream));
  cs_init(cs, "unexpand", &g.ops, in, err);
  out_init(&o, out);
  buf_init(&pend);
  for (;;) {	/* each line: GNU's state machine */
    int convert = 1, one_blank_before_tab_stop = 0, prev_blank = 1;
    long long col = 0, next_tab = 0;
    size_t idx = 0;
    pend.len = 0;
    do {
      c = cs_getc(cs);
      if (convert) {
        int blank = c == ' ' || c == '\t';
        if (blank) {
          int last;
          next_tab = tabs_next(&t, col, &idx, &last);
          if (last) convert = 0;
          if (convert) {
            if (c == '\t') {
              col = next_tab;
              if (pend.len) pend.s[0] = '\t';
            }
            else {
              col++;
              if (!(prev_blank && col == next_tab)) {
                /* not yet known whether these blanks become a tab */
                if (col == next_tab) one_blank_before_tab_stop = 1;
                buf_putc(&pend, (char)c);
                prev_blank = 1;
                continue;
              }
              c = '\t';	/* the pending blanks become a tab */
              if (pend.len) pend.s[0] = '\t';
            }
            /* drop the pending blanks, unless one was just before a stop */
            pend.len = (size_t)one_blank_before_tab_stop;
          }
        }
        else if (c == '\b') {
          col -= col > 0;
          next_tab = col;
          idx -= idx > 0;
        }
        else if (!(utf8 && (c & 0xC0) == 0x80)) col++;
        if (pend.len) {
          if (pend.len > 1 && one_blank_before_tab_stop) pend.s[0] = '\t';
          out_putn(&o, pend.s, pend.len);
          pend.len = 0;
          one_blank_before_tab_stop = 0;
        }
        prev_blank = blank;
        convert &= all || blank;
      }
      if (c < 0) goto done;
      out_putc(&o, c);
    } while (c != '\n' && !o.failed);
    if (o.failed) break;
  }
done:
  out_flush(&o);
  cs_close(cs);
  status = cs->status;
  free(cs);
  buf_free(&pend);
  free(t.list);
  opts_free(&g);
  free(v);
  return status;
bad:
  free(t.list);
  opts_free(&g);
  free(v);
  return 1;
}

/* }================================================================== */


/*
** {==================================================================
** split
** ===================================================================
*/

typedef struct Split {
  const char *prefix, *addsfx, *alpha;	/* alpha: the suffix characters */
  Buf base;	/* prefix, grown by GNU's suffix widening */
  int *idx;	/* the suffix, as positions in alpha */
  size_t len;	/* suffix length */
  int autow;	/* widen the suffix when it runs out (no -a given) */
  int started, verbose, elide, fd, failed, err;
  Out *vo;	/* --verbose goes here */
  char *name;	/* the current file */
} Split;

/* the next output file name; -1: the suffixes ran out */
static int split_next_name (Split *sp) {
  size_t i, alen = strlen(sp->alpha);
  Buf b;
  if (!sp->started) sp->started = 1;
  else {
    i = sp->len;
    for (;;) {
      if (i == 0) {
        tool_err(sp->err, "split", "output file suffixes exhausted");
        return -1;
      }
      i--;
      sp->idx[i]++;
      if (sp->autow && i == 0 && (size_t)sp->idx[0] + 1 == alen) {
        /* "zz.." would come next: the prefix takes a 'z', the suffix grows */
        buf_putc(&sp->base, sp->alpha[alen - 1]);
        sp->len++;
        sp->idx = (int *)xrealloc(sp->idx, sp->len * sizeof(int));
        memset(sp->idx, 0, sp->len * sizeof(int));
        break;
      }
      if ((size_t)sp->idx[i] < alen) break;
      sp->idx[i] = 0;
    }
  }
  buf_init(&b);
  buf_putn(&b, sp->base.s, sp->base.len);
  for (i = 0; i < sp->len; i++) buf_putc(&b, sp->alpha[sp->idx[i]]);
  buf_puts(&b, sp->addsfx);
  free(sp->name);
  sp->name = buf_take(&b);
  return 0;
}

static void split_close (Split *sp) {
  if (sp->fd >= 0) os_close(sp->fd);
  sp->fd = -1;
}

/* a new output file (--verbose says so); -1: failed */
static int split_create (Split *sp, const char *name) {
  char *native;
  int fd;
  if (sp->verbose) {
    out_printf(sp->vo, "creating file '%s'\n", name);
    out_flush(sp->vo);
  }
  native = path_to_native(name);
  fd = os_open(native, OS_WRITE);
  free(native);
  if (fd < 0) {
    tool_err(sp->err, "split", "%s: %s", name, os_errmsg());
    sp->failed = 1;
  }
  return fd;
}

/* GNU's cwrite(): a new file first when 'newfile' */
static int split_write (Split *sp, int newfile, const char *p, size_t n) {
  if (sp->failed) return -1;
  if (newfile) {
    if (p == NULL && n == 0 && sp->elide) return 0;
    split_close(sp);
    if (split_next_name(sp) != 0) {
      sp->failed = 1;
      return -1;
    }
    if ((sp->fd = split_create(sp, sp->name)) < 0) return -1;
  }
  while (n > 0) {
    long w = os_write(sp->fd, p, n);
    if (w <= 0) {
      tool_err(sp->err, "split", "%s: %s", sp->name, os_errmsg());
      sp->failed = 1;
      return -1;
    }
    p += w;
    n -= (size_t)w;
  }
  return 0;
}

/* -l N: N lines a file */
static void split_lines (Split *sp, In *r, long long n, char eol) {
  char chunk[65536];
  long got;
  long long lines = 0;
  int newfile = 1;
  while (!sp->failed && !tool_stop() && (got = in_read(r, chunk, sizeof(chunk))) > 0) {
    size_t from = 0, k;
    for (k = 0; k < (size_t)got; k++) {
      if (chunk[k] != eol) continue;
      if (++lines == n) {
        split_write(sp, newfile, chunk + from, k + 1 - from);
        newfile = 1;
        lines = 0;
        from = k + 1;
      }
    }
    if (from < (size_t)got) {
      split_write(sp, newfile, chunk + from, (size_t)got - from);
      newfile = 0;
    }
  }
}

/* -b N: N bytes a file */
static void split_bytes (Split *sp, In *r, long long n) {
  char chunk[65536];
  long got;
  long long to_write = n;
  int newfile = 1;
  while (!sp->failed && !tool_stop() && (got = in_read(r, chunk, sizeof(chunk))) > 0) {
    const char *p = chunk;
    long long left = got;
    while (!sp->failed) {
      if (left < to_write) {
        if (left) {
          split_write(sp, newfile, p, (size_t)left);
          to_write -= left;
          newfile = 0;
        }
        break;
      }
      split_write(sp, newfile, p, (size_t)to_write);
      newfile = 1;
      p += to_write;
      left -= to_write;
      to_write = n;
    }
  }
}

/* -C N: whole lines, at most N bytes a file (GNU's line_bytes_split) */
static void split_line_bytes (Split *sp, In *r, long long nbytes, char eol) {
  char chunk[65536];
  long got;
  long long n_out = 0;
  Buf hold;
  int split_line = 0;
  buf_init(&hold);
  while (!sp->failed && !tool_stop() && (got = in_read(r, chunk, sizeof(chunk))) > 0) {
    long long n_left = got;
    char *sob = chunk;
    while (n_left && !sp->failed) {
      long long split_rest = 0;
      char *eoc = NULL, *eol_p = NULL, *q;
      if (nbytes - n_out - (long long)hold.len <= n_left) {	/* enough for a file */
        split_rest = nbytes - n_out - (long long)hold.len;
        eoc = sob + split_rest - 1;
        for (q = sob + split_rest; q > sob; q--)
          if (q[-1] == eol) { eol_p = q - 1; break; }
      }
      else {
        for (q = sob + n_left; q > sob; q--)
          if (q[-1] == eol) { eol_p = q - 1; break; }
      }
      if (hold.len && !(!eol_p && n_out)) {	/* what was held back */
        split_write(sp, n_out == 0, hold.s, hold.len);
        n_out += (long long)hold.len;
        hold.len = 0;
      }
      if (eol_p) {	/* up to the line end */
        long long w = eol_p - sob + 1;
        split_line = 1;
        split_write(sp, n_out == 0, sob, (size_t)w);
        n_out += w;
        n_left -= w;
        sob += w;
        if (eoc) split_rest -= w;
      }
      if (n_left && !split_line) {	/* no line end: break the line */
        long long w = eoc ? split_rest : n_left;
        split_write(sp, n_out == 0, sob, (size_t)w);
        n_out += w;
        n_left -= w;
        sob += w;
        if (eoc) split_rest -= w;
      }
      if ((eoc && split_rest) || (!eoc && n_left)) {	/* hold the rest */
        long long nb = eoc ? split_rest : n_left;
        buf_putn(&hold, sob, (size_t)nb);
        n_left -= nb;
        sob += nb;
      }
      if (eoc) {
        n_out = 0;
        split_line = 0;
      }
    }
  }
  if (hold.len && !sp->failed) split_write(sp, n_out == 0, hold.s, hold.len);
  buf_free(&hold);
}

/* -n l/N or l/K/N: N pieces at line ends (GNU's lines_chunk_split) */
static void split_lines_chunks (Split *sp, Out *o, const char *buf, long long size, long long k,
                                long long n, char eol) {
  long long chunk_size = size / n, chunk_no = 1, chunk_end = chunk_size - 1, n_written = 0;
  int newfile = 1, truncated = 0;
  const char *bp = buf, *eob = buf + size;
  if (k > 1) {
    long long start = (k - 1) * chunk_size - 1;
    bp = buf + start;
    n_written = start;
    chunk_no = k - 1;
    chunk_end = chunk_no * chunk_size - 1;
  }
  while (bp != eob && !sp->failed) {
    long long n_read = eob - bp, skip = chunk_end - n_written, w;
    const char *e;
    int next = 0;
    if (skip < 0) skip = 0;
    if (skip > n_read) skip = n_read;
    e = (const char *)memchr(bp + skip, eol, (size_t)(n_read - skip));
    if (e != NULL) {
      e++;
      next = 1;
    }
    else e = eob;
    w = e - bp;
    if (k == chunk_no) out_putn(o, bp, (size_t)w);
    else if (k == 0) split_write(sp, newfile, bp, (size_t)w);
    n_written += w;
    bp += w;
    newfile = next;
    while (next || chunk_end <= n_written - 1) {	/* a long line may skip chunks */
      if (!next && bp == eob) {
        truncated = 1;
        break;
      }
      chunk_no++;
      if (k && chunk_no > k) return;
      if (chunk_no == n) chunk_end = size - 1;
      else chunk_end += chunk_size;
      if (chunk_end <= n_written - 1) {
        if (k == 0) split_write(sp, 1, NULL, 0);
      }
      else next = 0;
    }
  }
  if (truncated) chunk_no++;
  while (k == 0 && chunk_no++ <= n && !sp->failed) split_write(sp, 1, NULL, 0);
}


int t_split (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {{"suffix-length", 'a', 1}, {"additional-suffix", 1001, 1},
    {"bytes", 'b', 1}, {"line-bytes", 'C', 1}, {"numeric-suffixes", 1002, 2},
    {"hex-suffixes", 1003, 2}, {"elide-empty-files", 'e', 0}, {"lines", 'l', 1},
    {"number", 'n', 1}, {"separator", 't', 1}, {"verbose", 1004, 0}, {NULL, 0, 0}};
  Opts g;
  Out o;
  Split sp;
  In r;
  int c, mode = 0, status = 0, suffix_given = 0;
  long long count = 1000, k = 0, start = 0, a = 2;
  char eol = '\n', ctype = 0;
  const char *in_name = "-";
  memset(&sp, 0, sizeof(sp));
  sp.prefix = "x";
  sp.addsfx = "";
  sp.alpha = "abcdefghijklmnopqrstuvwxyz";
  sp.fd = -1;
  sp.err = err;
  sp.autow = 1;
  opts_init(&g, "split", argc, argv, err);
  while ((c = opts_next(&g, "a:b:C:del:n:t:x", lo)) != 0) {
    int newmode = 0;
    switch (c) {
      case 'a':
        if (num_arg(g.arg, 0, &a) != 0 || a > 1000) {
          tool_err(err, "split", "invalid suffix length: '%s'", g.arg);
          goto bad;
        }
        suffix_given = 1;
        break;
      case 1001:
        if (strchr(g.arg, '/')) {
          tool_err(err, "split", "invalid suffix '%s', contains directory separator", g.arg);
          goto bad;
        }
        sp.addsfx = g.arg;
        break;
      case 'b': case 'C': case 'l': {
        const char *what = c == 'l' ? "lines" : "bytes";
        long long v;
        newmode = c;
        if ((c == 'l' ? num_arg(g.arg, 0, &v) : parse_size(g.arg, &v)) != 0) {
          tool_err(err, "split", "invalid number of %s: '%s'", what, g.arg);
          goto bad;
        }
        if (v == 0) {
          tool_err(err, "split", "invalid number of %s: '%s': Numerical result out of range", what, g.arg);
          goto bad;
        }
        count = v;
        break;
      }
      case 'n': {
        const char *s = g.arg, *slash;
        long long v;
        char tmp[64];
        newmode = 'n';
        ctype = 0;
        k = 0;
        if ((s[0] == 'l' || s[0] == 'r') && s[1] == '/') {
          ctype = s[0];
          s += 2;
        }
        slash = strchr(s, '/');
        if (slash) {
          snprintf(tmp, sizeof(tmp), "%.*s", (int)(slash - s), s);
          if (num_arg(tmp, 0, &k) != 0) {
            tool_err(err, "split", "invalid chunk number: '%s'", tmp);
            goto bad;
          }
          s = slash + 1;
        }
        if (num_arg(s, 0, &v) != 0) {
          tool_err(err, "split", "invalid number of chunks: '%s'", s);
          goto bad;
        }
        if (v == 0) {
          tool_err(err, "split", "invalid number of chunks: '%s': Numerical result out of range", s);
          goto bad;
        }
        if (slash && (k == 0 || k > v)) {
          tool_err(err, "split", "invalid chunk number: '%.*s'%s", (int)(slash - g.arg - (ctype ? 2 : 0)),
                   g.arg + (ctype ? 2 : 0), ": Numerical result out of range");
          goto bad;
        }
        count = v;
        break;
      }
      case 'd': case 1002:
        sp.alpha = "0123456789";
        if (c == 1002 && g.arg) {
          if (num_arg(g.arg, 0, &start) != 0) {
            tool_err(err, "split", "invalid start value for numerical suffix: '%s'", g.arg);
            goto bad;
          }
        }
        break;
      case 'x': case 1003:
        sp.alpha = "0123456789abcdef";
        if (c == 1003 && g.arg) {
          char *end;
          start = strtoll(g.arg, &end, 16);
          if (*end || !*g.arg || start < 0) {
            tool_err(err, "split", "invalid start value for hexadecimal suffix: '%s'", g.arg);
            goto bad;
          }
        }
        break;
      case 'e': sp.elide = 1; break;
      case 't':
        if (g.arg[0] && g.arg[1]) {
          if (strcmp(g.arg, "\\0") == 0) eol = '\0';
          else {
            tool_err(err, "split", "multi-character separator '%s'", g.arg);
            goto bad;
          }
        }
        else if (!g.arg[0]) {
          tool_err(err, "split", "empty record separator");
          goto bad;
        }
        else eol = g.arg[0];
        break;
      case 1004: sp.verbose = 1; break;
      case OPT_HELP: opts_free(&g); return tool_help(out, "split");
      default: goto bad;
    }
    if (newmode) {
      if (mode && mode != newmode) {
        tool_err(err, "split", "cannot split in more than one way");
        fd_printf(err, "Try 'split --help' for more information.\n");
        goto bad;
      }
      mode = newmode;
    }
  }
  if (mode == 0) mode = 'l';
  if (g.ops.n > 2) {
    tool_err(err, "split", "extra operand '%s'", g.ops.v[2]);
    fd_printf(err, "Try 'split --help' for more information.\n");
    goto bad;
  }
  if (g.ops.n >= 1) in_name = g.ops.v[0];
  if (g.ops.n == 2) sp.prefix = g.ops.v[1];
  if (mode == 'n' || start > 0) sp.autow = 0;
  if (suffix_given) sp.autow = 0;
  if (mode == 'n') {	/* long enough for every piece */
    long long end = count + start, need = 0, alen = (long long)strlen(sp.alpha);
    do {
      need++;
      end /= alen;
    } while (end);
    if (suffix_given && a < need) {
      tool_err(err, "split", "the suffix length needs to be at least %lld", need);
      goto bad;
    }
    if (!suffix_given && a < need) a = need;
  }
  if (a == 0) a = 2;
  sp.len = (size_t)a;
  sp.idx = (int *)xmalloc(sp.len * sizeof(int));
  memset(sp.idx, 0, sp.len * sizeof(int));
  if (start > 0) {	/* --numeric-suffixes=FROM */
    long long v = start, alen = (long long)strlen(sp.alpha);
    size_t i = sp.len;
    while (i > 0) {
      sp.idx[--i] = (int)(v % alen);
      v /= alen;
    }
    if (v > 0) {
      tool_err(err, "split", "numerical suffix start value is too large for the suffix length");
      free(sp.idx);
      goto bad;
    }
  }
  buf_init(&sp.base);
  buf_puts(&sp.base, sp.prefix);
  if (m_open(&r, "split", in_name, in, err, OPEN_CANNOT, NULL) != 0) {
    free(sp.idx);
    buf_free(&sp.base);
    opts_free(&g);
    return 1;
  }
  out_init(&o, out);
  sp.vo = &o;
  if (mode == 'l') split_lines(&sp, &r, count, eol);
  else if (mode == 'b') split_bytes(&sp, &r, count);
  else if (mode == 'C') split_line_bytes(&sp, &r, count, eol);
  else if (ctype == 'r') {	/* round robin: line i to file i % N */
    long long i = 0, f, made = 0;
    int *fds = NULL;
    char **names = NULL, *line;
    size_t len;
    int had;
    if (k == 0) {	/* the N names; the files exist from the start, unless -e */
      fds = (int *)xmalloc((size_t)count * sizeof(int));
      names = (char **)xmalloc((size_t)count * sizeof(char *));
      for (made = 0; made < count; made++) {
        if (split_next_name(&sp) != 0) {
          sp.failed = 1;
          break;
        }
        names[made] = xstrdup(sp.name);
        fds[made] = sp.elide ? -1 : split_create(&sp, names[made]);
        if (sp.failed) {
          made++;
          break;
        }
      }
    }
    while (!sp.failed && !tool_stop() && in_line(&r, &line, &len, eol, &had)) {
      long long to = i++ % count;
      if (k == 0) {
        if (fds[to] < 0 && (fds[to] = split_create(&sp, names[to])) < 0) break;
        sp.fd = fds[to];
        split_write(&sp, 0, line, len);
        split_write(&sp, 0, &eol, 1);
        sp.fd = -1;
      }
      else if (to == k - 1) {
        out_putn(&o, line, len);
        out_putc(&o, eol);
      }
    }
    for (f = 0; f < made; f++) {
      if (fds[f] >= 0) os_close(fds[f]);
      free(names[f]);
    }
    free(fds);
    free(names);
  }
  else {	/* N pieces of the whole input */
    long long here = os_seek(r.fd, 0, 1), end = here >= 0 ? os_seek(r.fd, 0, 2) : -1;
    Buf all;
    if (end < 0) {
      tool_err(err, "split", "%s: cannot determine file size", in_name);
      status = 1;
    }
    else {
      os_seek(r.fd, here, 0);
      buf_init(&all);
      slurp(&r, &all);
      if (ctype == 'l') split_lines_chunks(&sp, &o, all.s, (long long)all.len, k, count, eol);
      else if (k > 0) {	/* only piece K, to stdout */
        long long size = (long long)all.len, csize = size / count;
        long long from = (k - 1) * csize, to = k == count ? size : k * csize;
        out_putn(&o, all.s + from, (size_t)(to - from));
      }
      else {	/* N pieces of size/N (at least 1), the last takes the rest */
        long long csize = (long long)all.len / count, left = (long long)all.len, opened = 0;
        const char *p = all.s;
        if (csize < 1) csize = 1;
        while (left > 0 && opened < count && !sp.failed) {
          long long w = opened + 1 == count ? left : (csize < left ? csize : left);
          split_write(&sp, 1, p, (size_t)w);
          opened++;
          p += w;
          left -= w;
        }
        while (opened++ < count && !sp.failed) split_write(&sp, 1, NULL, 0);
      }
      buf_free(&all);
    }
  }
  split_close(&sp);
  out_flush(&o);
  in_close(&r);
  if (sp.failed) status = 1;
  free(sp.idx);
  free(sp.name);
  buf_free(&sp.base);
  opts_free(&g);
  return status;
bad:
  opts_free(&g);
  return 1;
}

/* }================================================================== */


/*
** {==================================================================
** shuf
** ===================================================================
*/

typedef struct Rng {
  unsigned long long s;
} Rng;

static unsigned long long rng_next (Rng *r) {	/* xorshift64* */
  r->s ^= r->s >> 12;
  r->s ^= r->s << 25;
  r->s ^= r->s >> 27;
  return r->s * 2685821657736338717ULL;
}

/* uniform in [0, n) */
static unsigned long long rng_below (Rng *r, unsigned long long n) {
  unsigned long long lim, x;
  if (n <= 1) return 0;
  lim = ~0ULL - (~0ULL % n);
  do x = rng_next(r);
  while (x >= lim);
  return x % n;
}

/* where shuf -i keeps the swapped places of a big range */
typedef struct SwapMap {
  unsigned long long *key, *val;
  size_t cap, n;
} SwapMap;

static unsigned long long *swap_slot (SwapMap *m, unsigned long long k, int add) {
  size_t i;
  if (add && (m->n + 1) * 2 > m->cap) {
    SwapMap old = *m;
    size_t j;
    m->cap = m->cap ? m->cap * 2 : 1024;
    m->key = (unsigned long long *)xmalloc(m->cap * sizeof(unsigned long long));
    m->val = (unsigned long long *)xmalloc(m->cap * sizeof(unsigned long long));
    for (j = 0; j < m->cap; j++) m->key[j] = ~0ULL;
    m->n = 0;
    for (j = 0; j < old.cap; j++)
      if (old.key[j] != ~0ULL) *swap_slot(m, old.key[j], 1) = old.val[j];
    free(old.key);
    free(old.val);
  }
  if (m->cap == 0) return NULL;
  i = (size_t)((k * 11400714819323198485ULL) >> 20) & (m->cap - 1);
  while (m->key[i] != ~0ULL && m->key[i] != k) i = (i + 1) & (m->cap - 1);
  if (m->key[i] == ~0ULL) {
    if (!add) return NULL;
    m->key[i] = k;
    m->val[i] = k;
    m->n++;
  }
  return &m->val[i];
}

static unsigned long long swap_get (SwapMap *m, unsigned long long k) {
  unsigned long long *v = swap_slot(m, k, 0);
  return v ? *v : k;
}


int t_shuf (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {{"echo", 'e', 0}, {"input-range", 'i', 1}, {"head-count", 'n', 1},
    {"output", 'o', 1}, {"random-source", 1001, 1}, {"repeat", 'r', 0},
    {"zero-terminated", 'z', 0}, {NULL, 0, 0}};
  Opts g;
  Out o;
  Rng rng;
  int c, echo = 0, range = 0, repeat = 0, status = 0, ofd = -1;
  long long head = -1;
  unsigned long long lo_v = 0, hi_v = 0;
  char eol = '\n';
  const char *outname = NULL, *source = NULL;
  Vec lines;
  size_t i;
  vec_init(&lines);
  opts_init(&g, "shuf", argc, argv, err);
  while ((c = opts_next(&g, "ei:n:o:rz", lo)) != 0) {
    switch (c) {
      case 'e': echo = 1; break;
      case 'i': {
        char *end, part[64];
        const char *dash = strchr(g.arg, '-'), *hs = g.arg;
        unsigned long long a = 0, b;
        if (range) {
          tool_err(err, "shuf", "multiple -i options specified");
          goto bad;
        }
        if (dash) {	/* each side, then the range */
          snprintf(part, sizeof(part), "%.*s", (int)(dash - g.arg), g.arg);
          a = strtoull(part, &end, 10);
          if (!isdigit((unsigned char)part[0]) || *end) {
            tool_err(err, "shuf", "invalid input range: '%s'", part);
            goto bad;
          }
          hs = dash + 1;
        }
        b = strtoull(hs, &end, 10);
        if (!isdigit((unsigned char)hs[0]) || *end) {
          tool_err(err, "shuf", "invalid input range: '%s'", hs);
          goto bad;
        }
        if (!dash || (a > b && a != b + 1)) {
          tool_err(err, "shuf", "invalid input range: '%s'", g.arg);
          goto bad;
        }
        lo_v = a;
        hi_v = b;
        range = 1;
        break;
      }
      case 'n': {
        long long v;
        if (num_arg(g.arg, 0, &v) != 0) {
          tool_err(err, "shuf", "invalid line count: '%s'", g.arg);
          goto bad;
        }
        if (head < 0 || v < head) head = v;
        break;
      }
      case 'o': outname = g.arg; break;
      case 1001: source = g.arg; break;
      case 'r': repeat = 1; break;
      case 'z': eol = '\0'; break;
      case OPT_HELP: opts_free(&g); return tool_help(out, "shuf");
      default: goto bad;
    }
  }
  if (echo && range) {
    tool_err(err, "shuf", "cannot combine -e and -i options");
    fd_printf(err, "Try 'shuf --help' for more information.\n");
    goto bad;
  }
  if ((range && g.ops.n > 0) || (!echo && !range && g.ops.n > 1)) {
    tool_err(err, "shuf", "extra operand '%s'", g.ops.v[range ? 0 : 1]);
    fd_printf(err, "Try 'shuf --help' for more information.\n");
    goto bad;
  }
  /* the seed: the clock, or --random-source's bytes */
  rng.s = (unsigned long long)os_now_us() ^ ((unsigned long long)os_getpid() << 32) ^ 0x9E3779B97F4A7C15ULL;
  if (source) {
    In r;
    char chunk[4096];
    long n, j;
    if (m_open(&r, "shuf", source, in, err, OPEN_PLAIN, NULL) != 0) goto bad;
    rng.s = 0x9E3779B97F4A7C15ULL;
    n = in_read(&r, chunk, sizeof(chunk));
    for (j = 0; j < n; j++) rng.s = (rng.s ^ (unsigned char)chunk[j]) * 1099511628211ULL;
    in_close(&r);
  }
  if (rng.s == 0) rng.s = 1;
  for (i = 0; i < 4; i++) rng_next(&rng);
  if (echo) {
    for (i = 0; i < g.ops.n; i++) vec_push(&lines, xstrdup(g.ops.v[i]));
  }
  else if (!range) {	/* the input lines */
    In r;
    char *line;
    size_t len;
    if (m_open(&r, "shuf", g.ops.n ? g.ops.v[0] : "-", in, err, OPEN_PLAIN, "read error: Is a directory") != 0)
      goto bad;
    while (in_line(&r, &line, &len, eol, NULL)) vec_push(&lines, xstrndup(line, len));
    in_close(&r);
  }
  if (outname) {
    char *native = path_to_native(outname);
    ofd = os_open(native, OS_WRITE);
    free(native);
    if (ofd < 0) {
      tool_err(err, "shuf", "%s: %s", outname, os_errmsg());
      goto bad;
    }
  }
  out_init(&o, ofd >= 0 ? ofd : out);
  if (range) {
    unsigned long long size = hi_v - lo_v + 1, n, j;	/* lo = hi + 1: empty */
    if (lo_v == hi_v + 1) size = 0;
    if (repeat) {
      if (size == 0 && head != 0) {
        tool_err(err, "shuf", "no lines to repeat");
        status = 1;
      }
      else
        for (j = 0; (head < 0 || j < (unsigned long long)head) && !o.failed && !tool_stop(); j++) {
          out_printf(&o, "%llu", lo_v + rng_below(&rng, size));
          out_putc(&o, eol);
        }
    }
    else {	/* Fisher-Yates over a sparse map of the swapped places */
      SwapMap m;
      memset(&m, 0, sizeof(m));
      n = head >= 0 && (unsigned long long)head < size ? (unsigned long long)head : size;
      for (j = 0; j < n && !o.failed && !tool_stop(); j++) {
        unsigned long long r = j + rng_below(&rng, size - j);
        unsigned long long vj = swap_get(&m, j), vr = swap_get(&m, r);
        *swap_slot(&m, r, 1) = vj;
        out_printf(&o, "%llu", lo_v + vr);
        out_putc(&o, eol);
      }
      free(m.key);
      free(m.val);
    }
  }
  else if (repeat) {
    long long j;
    if (lines.n == 0 && head != 0) {
      tool_err(err, "shuf", "no lines to repeat");
      status = 1;
    }
    else
      for (j = 0; (head < 0 || j < head) && !o.failed && !tool_stop(); j++) {
        out_puts(&o, lines.v[rng_below(&rng, lines.n)]);
        out_putc(&o, eol);
      }
  }
  else {
    size_t n = head >= 0 && (size_t)head < lines.n ? (size_t)head : lines.n;
    for (i = 0; i < n && !o.failed; i++) {
      size_t r = i + (size_t)rng_below(&rng, lines.n - i);
      char *t = lines.v[i];
      lines.v[i] = lines.v[r];
      lines.v[r] = t;
      out_puts(&o, lines.v[i]);
      out_putc(&o, eol);
    }
  }
  out_flush(&o);
  if (ofd >= 0) os_close(ofd);
  vec_free(&lines);
  opts_free(&g);
  return status;
bad:
  vec_free(&lines);
  opts_free(&g);
  return 1;
}

/* }================================================================== */


/*
** {==================================================================
** column
** ===================================================================
*/

/* the columns a cell takes: control characters (tab, CR ...) take none */
static size_t col_width (const char *s, size_t n, int utf8) {
  size_t i, w = 0;
  if (utf8) return (size_t)tool_utf8_cols(s, n);
  for (i = 0; i < n; i++)
    if ((unsigned char)s[i] >= 32 && (unsigned char)s[i] != 127) w++;
  return w;
}

static int col_blank_line (const char *s, size_t n) {
  size_t i;
  for (i = 0; i < n; i++)
    if (!isspace((unsigned char)s[i])) return 0;
  return 1;
}


int t_column (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {{"table", 't', 0}, {"separator", 's', 1},
    {"output-separator", 'o', 1}, {"output-width", 'c', 1}, {"columns", 'c', 1},
    {"fillrows", 'x', 0}, {"keep-empty-lines", 'L', 0}, {NULL, 0, 0}};
  Opts g;
  Out o;
  int c, table = 0, rows = 0, keep = 0, status = 0, utf8 = tool_utf8();
  long long width = -1;
  const char *isep = NULL, *osep = "  ";
  Vec ents;	/* fill modes: the lines; table: the lines too */
  size_t i;
  vec_init(&ents);
  opts_init(&g, "column", argc, argv, err);
  while ((c = opts_next(&g, "ts:o:c:xL", lo)) != 0) {
    switch (c) {
      case 't': table = 1; break;
      case 's': isep = g.arg; break;
      case 'o': osep = g.arg; break;
      case 'c':
        if (num_arg(g.arg, 0, &width) != 0) {
          tool_err(err, "column", "invalid columns argument: '%s'", g.arg);
          opts_free(&g);
          return 1;
        }
        break;
      case 'x': rows = 1; break;
      case 'L': keep = 1; break;
      case OPT_HELP: opts_free(&g); return tool_help(out, "column");
      default: opts_free(&g); return 1;
    }
  }
  if (width < 0) {	/* the terminal's width, else $COLUMNS, else 80 */
    const char *cv = var_get("COLUMNS");
    long long v;
    width = 80;
    if (os_is_tty(out) && os_term_cols() > 0) width = os_term_cols();
    else if (cv && (var_flags("COLUMNS") & V_EXPORT) && num_arg(cv, 1, &v) == 0) width = v;
  }
  if (g.ops.n == 0) vec_push(&g.ops, xstrdup("-"));
  for (i = 0; i < g.ops.n && !tool_stop(); i++) {
    In r;
    char *line;
    size_t len;
    if (m_open(&r, "column", g.ops.v[i], in, err, OPEN_PLAIN, "read failed: Is a directory") != 0) {
      status = 1;
      continue;
    }
    while (in_line(&r, &line, &len, '\n', NULL)) {
      if (col_blank_line(line, len)) {
        if (!keep) continue;
        len = 0;
      }
      vec_push(&ents, xstrndup(line, len));
    }
    in_close(&r);
  }
  out_init(&o, out);
  if (table) {
    const char *seps = isep ? isep : " \t";
    int greedy = isep == NULL;
    Vec cells;	/* row after row; NULL ends a row */
    size_t *wid = NULL, ncol = 0, k;
    vec_init(&cells);
    for (i = 0; i < ents.n; i++) {
      char *s = ents.v[i];
      size_t nc = 0;
      if (greedy) {
        for (;;) {
          size_t e;
          s += strspn(s, seps);
          if (*s == '\0') break;
          e = strcspn(s, seps);
          vec_push(&cells, xstrndup(s, e));
          nc++;
          s += e;
        }
      }
      else if (*s != '\0' || !keep) {
        for (;;) {
          size_t e = *seps ? strcspn(s, seps) : strlen(s);
          vec_push(&cells, xstrndup(s, e));
          nc++;
          if (s[e] == '\0') break;
          s += e + 1;
        }
      }
      vec_push(&cells, NULL);
      if (nc > ncol) {
        wid = (size_t *)xrealloc(wid, nc * sizeof(size_t));
        while (ncol < nc) wid[ncol++] = 0;
      }
    }
    /* the widths, then the rows: every column but the last padded */
    for (i = 0, k = 0; i < cells.n; i++) {
      if (cells.v[i] == NULL) { k = 0; continue; }
      if (col_width(cells.v[i], strlen(cells.v[i]), utf8) > wid[k]) wid[k] = col_width(cells.v[i], strlen(cells.v[i]), utf8);
      k++;
    }
    for (i = 0; i < cells.n && !o.failed;) {
      for (k = 0; k < ncol; k++) {
        const char *cell = cells.v[i] ? cells.v[i] : "";
        if (cells.v[i]) i++;
        out_puts(&o, cell);
        if (k + 1 < ncol) {
          size_t w = col_width(cell, strlen(cell), utf8);
          for (; w < wid[k]; w++) out_putc(&o, ' ');
          out_puts(&o, osep);
        }
      }
      while (cells.v[i] != NULL) i++;	/* the row's end */
      i++;
      out_putc(&o, '\n');
    }
    for (i = 0; i < cells.n; i++) free(cells.v[i]);
    free(cells.v);
    free(wid);
  }
  else if (ents.n > 0) {
    size_t maxw = 0, ncols, nrows, row, col, n = ents.n;
    size_t *w = (size_t *)xmalloc(n * sizeof(size_t));
    for (i = 0; i < n; i++) {
      w[i] = col_width(ents.v[i], strlen(ents.v[i]), utf8);
      if (w[i] > maxw) maxw = w[i];
    }
    maxw = (maxw + 8) & ~(size_t)7;	/* to the next tab stop */
    ncols = (size_t)width / maxw;
    if (ncols == 0) ncols = 1;
    if (!rows) {	/* down the columns first */
      nrows = n / ncols + (n % ncols != 0);
      for (row = 0; row < nrows && !o.failed; row++) {
        size_t base = row, chcnt = 0, endcol = maxw;
        for (col = 0; col < ncols; col++) {
          out_puts(&o, ents.v[base]);
          chcnt += w[base];
          if ((base += nrows) >= n) break;
          while (chcnt < endcol) {
            out_putc(&o, '\t');
            chcnt = (chcnt + 8) & ~(size_t)7;
          }
          endcol += maxw;
        }
        out_putc(&o, '\n');
      }
    }
    else {	/* -x: along the rows */
      size_t chcnt = 0, endcol = maxw;
      for (i = 0, col = 0; i < n && !o.failed; i++) {
        if (col > 0) {
          while (chcnt < endcol) {
            out_putc(&o, '\t');
            chcnt = (chcnt + 8) & ~(size_t)7;
          }
          endcol += maxw;
        }
        out_puts(&o, ents.v[i]);
        chcnt += w[i];
        if (++col == ncols) {
          out_putc(&o, '\n');
          chcnt = col = 0;
          endcol = maxw;
        }
      }
      if (col > 0) out_putc(&o, '\n');
    }
    free(w);
  }
  out_flush(&o);
  vec_free(&ents);
  opts_free(&g);
  return status;
}

/* }================================================================== */


/*
** {==================================================================
** od
** ===================================================================
*/

typedef struct OdSpec {
  char kind;	/* a c d o u x f */
  int size, width, pad, z;
} OdSpec;

static const char *const od_names[33] = {
  "nul", "soh", "stx", "etx", "eot", "enq", "ack", "bel", "bs", "ht", "nl", "vt", "ff", "cr",
  "so", "si", "dle", "dc1", "dc2", "dc3", "dc4", "nak", "syn", "etb", "can", "em", "sub", "esc",
  "fs", "gs", "rs", "us", "sp"
};

/* an od number: 10, 0x10, 010, with b k m (and K M G ...) suffixes */
static int od_num (const char *s, long long *v) {
  char *end;
  unsigned long long x;
  long long mul = 1;
  if (!isdigit((unsigned char)*s)) return -1;
  x = strtoull(s, &end, 0);
  if (*end) {	/* b: 512; K M G ...: 1024s; KB MB: 1000s */
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "1%s", end);
    if (strcmp(end, "b") == 0) mul = 512;
    else if (strlen(end) > 3 || parse_size(tmp, &mul) != 0) return -1;
  }
  *v = (long long)x * mul;
  return 0;
}

/* -t TYPE (can hold several: "x1z", "d2u1") */
static int od_types (const char *s, OdSpec **specs, size_t *n, int err) {
  const char *all = s;
  while (*s) {
    OdSpec sp;
    memset(&sp, 0, sizeof(sp));
    sp.kind = *s;
    switch (*s++) {
      case 'a': case 'c': sp.size = 1; break;
      case 'd': case 'o': case 'u': case 'x':
        if (*s == 'C') { sp.size = 1; s++; }
        else if (*s == 'S') { sp.size = 2; s++; }
        else if (*s == 'I') { sp.size = 4; s++; }
        else if (*s == 'L') { sp.size = 8; s++; }
        else if (isdigit((unsigned char)*s)) {
          sp.size = (int)strtol(s, (char **)&s, 10);
          if (sp.size != 1 && sp.size != 2 && sp.size != 4 && sp.size != 8) {
            tool_err(err, "od", "invalid type string '%s';\nthis system doesn't provide a %d-byte integral type",
                     all, sp.size);
            return -1;
          }
        }
        else sp.size = 4;
        break;
      case 'f':
        if (*s == 'F') { sp.size = 4; s++; }
        else if (*s == 'D') { sp.size = 8; s++; }
        else if (*s == 'L') {
          tool_err(err, "od", "invalid type string '%s';\nthis system doesn't provide a %d-byte floating point type", all, 16);
          return -1;
        }
        else if (isdigit((unsigned char)*s)) {
          sp.size = (int)strtol(s, (char **)&s, 10);
          if (sp.size != 4 && sp.size != 8) {
            tool_err(err, "od", "invalid type string '%s';\nthis system doesn't provide a %d-byte floating point type",
                     all, sp.size);
            return -1;
          }
        }
        else sp.size = 8;
        break;
      default:
        tool_err(err, "od", "invalid character '%c' in type string '%s'", s[-1], all);
        return -1;
    }
    switch (sp.kind) {
      case 'a': case 'c': sp.width = 3; break;
      case 'd': sp.width = sp.size == 1 ? 4 : sp.size == 2 ? 6 : sp.size == 4 ? 11 : 20; break;
      case 'o': sp.width = sp.size == 1 ? 3 : sp.size == 2 ? 6 : sp.size == 4 ? 11 : 22; break;
      case 'u': sp.width = sp.size == 1 ? 3 : sp.size == 2 ? 5 : sp.size == 4 ? 10 : 20; break;
      case 'x': sp.width = 2 * sp.size; break;
      case 'f': sp.width = sp.size == 4 ? 15 : 24; break;
    }
    if (*s == 'z') {
      sp.z = 1;
      s++;
    }
    *specs = (OdSpec *)xrealloc(*specs, (*n + 1) * sizeof(OdSpec));
    (*specs)[(*n)++] = sp;
  }
  return 0;
}

static unsigned long long od_get (const unsigned char *p, int size, int big) {
  unsigned long long v = 0;
  int k;
  for (k = 0; k < size; k++) v |= (unsigned long long)p[big ? size - 1 - k : k] << (8 * k);
  return v;
}

/* the shortest text that reads back as the same number, like ftoastr */
static void od_float (char *buf, size_t n, double x, int is_float) {
  int prec = is_float ? FLT_DIG : DBL_DIG;
  double ax = x < 0 ? -x : x;
  if (ax != 0 && ax < (is_float ? FLT_MIN : DBL_MIN)) prec = 1;
  for (;; prec++) {
    snprintf(buf, n, "%.*g", prec, x);
    if (prec >= (is_float ? 9 : 17)) break;
    if (is_float ? (float)strtod(buf, NULL) == (float)x : strtod(buf, NULL) == x) break;
  }
}

/* one line of one type (GNU's print functions: the pad is spread out) */
static void od_line (Out *o, const OdSpec *sp, const unsigned char *block, size_t per_block,
                     size_t nbytes, int big) {
  size_t fields = per_block / (size_t)sp->size, blank = (per_block - nbytes) / (size_t)sp->size, i;
  int pad_rem = sp->pad;
  const unsigned char *p = block;
  char buf[64];
  for (i = fields; blank < i; i--) {
    int next_pad = (int)((long long)sp->pad * (long long)(i - 1) / (long long)fields);
    int w = pad_rem - next_pad + sp->width;
    unsigned long long v = od_get(p, sp->size, big);
    switch (sp->kind) {
      case 'a': {
        int m = p[0] & 0x7f;
        if (m == 127) out_printf(o, "%*s", w, "del");
        else if (m <= 040) out_printf(o, "%*s", w, od_names[m]);
        else out_printf(o, "%*c", w, m);
        break;
      }
      case 'c': {
        const char *s;
        switch (p[0]) {
          case '\0': s = "\\0"; break;
          case '\a': s = "\\a"; break;
          case '\b': s = "\\b"; break;
          case '\f': s = "\\f"; break;
          case '\n': s = "\\n"; break;
          case '\r': s = "\\r"; break;
          case '\t': s = "\\t"; break;
          case '\v': s = "\\v"; break;
          default:
            if (p[0] >= 32 && p[0] < 127) snprintf(buf, sizeof(buf), "%c", p[0]);
            else snprintf(buf, sizeof(buf), "%03o", p[0]);
            s = buf;
        }
        out_printf(o, "%*s", w, s);
        break;
      }
      case 'd': {
        long long sv;
        if (sp->size == 8) sv = (long long)v;
        else {
          unsigned long long sign = 1ULL << (8 * sp->size - 1);
          sv = (v & sign) ? (long long)(v | ~((sign << 1) - 1)) : (long long)v;
        }
        out_printf(o, "%*lld", w, sv);
        break;
      }
      case 'u': out_printf(o, "%*llu", w, v); break;
      case 'o': out_printf(o, "%*.*llo", w, sp->width, v); break;
      case 'x': out_printf(o, "%*.*llx", w, sp->width, v); break;
      case 'f':
        if (sp->size == 4) {
          float f;
          unsigned long v32 = (unsigned long)v;
          unsigned char b4[4];
          b4[0] = (unsigned char)v32; b4[1] = (unsigned char)(v32 >> 8);
          b4[2] = (unsigned char)(v32 >> 16); b4[3] = (unsigned char)(v32 >> 24);
          {	/* the bits into a float, in this machine's order */
            unsigned long one = 1;
            if (*(unsigned char *)&one == 1) memcpy(&f, b4, 4);
            else {
              unsigned char r4[4];
              r4[0] = b4[3]; r4[1] = b4[2]; r4[2] = b4[1]; r4[3] = b4[0];
              memcpy(&f, r4, 4);
            }
          }
          od_float(buf, sizeof(buf), (double)f, 1);
        }
        else {
          double d;
          unsigned char b8[8];
          int k;
          unsigned long one = 1;
          for (k = 0; k < 8; k++) b8[*(unsigned char *)&one == 1 ? k : 7 - k] = (unsigned char)(v >> (8 * k));
          memcpy(&d, b8, 8);
          od_float(buf, sizeof(buf), d, 0);
        }
        out_printf(o, "%*s", w, buf);
        break;
    }
    p += sp->size;
    pad_rem = next_pad;
  }
  if (sp->z) {	/* the bytes as text, after padding to the full width */
    int padw = (int)((long long)sp->pad * (long long)blank / (long long)fields);
    out_printf(o, "%*s", (int)blank * sp->width + padw, "");
    out_puts(o, "  >");
    for (i = 0; i < nbytes; i++) out_putc(o, block[i] >= 32 && block[i] < 127 ? block[i] : '.');
    out_putc(o, '<');
  }
}

static void od_address (Out *o, char radix, long long a) {
  if (radix == 'o') out_printf(o, "%07llo", a);
  else if (radix == 'd') out_printf(o, "%07lld", a);
  else if (radix == 'x') out_printf(o, "%06llx", a);
}


int t_od (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {{"address-radix", 'A', 1}, {"skip-bytes", 'j', 1},
    {"read-bytes", 'N', 1}, {"format", 't', 1}, {"output-duplicates", 'v', 0},
    {"width", 'w', 2}, {"endian", 1001, 1}, {"traditional", 1002, 0}, {NULL, 0, 0}};
  Opts g;
  Out o;
  OdSpec *specs = NULL;
  size_t nspecs = 0, i, per_block = 16, lcm = 1;
  int c, verbose = 0, big = 0, status = 0, first = 1, prev_equal = 0;
  char radix = 'o';
  long long skip = 0, limit = -1, offset;
  CStream *cs;
  unsigned char *block, *prev;
  char **av = (char **)xmalloc(((size_t)argc + 1) * sizeof(char *));
  Vec made;
  vec_init(&made);
  for (i = 0; i < (size_t)argc; i++) {	/* -w, -w16: an optional number (as --width[=N]) */
    av[i] = argv[i];
    if (i > 0 && strncmp(argv[i], "-w", 2) == 0) {
      char *w = xstrcat3("--width", argv[i][2] ? "=" : "", argv[i] + 2);
      vec_push(&made, w);
      av[i] = w;
    }
    if (strcmp(argv[i], "--") == 0) {
      for (i++; i < (size_t)argc; i++) av[i] = argv[i];
      break;
    }
  }
  av[argc] = NULL;
  opts_init(&g, "od", argc, av, err);
  while ((c = opts_next(&g, "A:j:N:t:vabcdfilosx", lo)) != 0) {
    const char *trad = NULL;
    switch (c) {
      case 'A':
        if (strlen(g.arg) != 1 || !strchr("doxn", g.arg[0])) {
          tool_err(err, "od", "invalid output address radix '%s'; it must be one character from [doxn]", g.arg);
          goto bad;
        }
        radix = g.arg[0];
        break;
      case 'j':
        if (od_num(g.arg, &skip) != 0) {
          tool_err(err, "od", "invalid -j argument '%s'", g.arg);
          goto bad;
        }
        break;
      case 'N':
        if (od_num(g.arg, &limit) != 0) {
          tool_err(err, "od", "invalid -N argument '%s'", g.arg);
          goto bad;
        }
        break;
      case 't': if (od_types(g.arg, &specs, &nspecs, err)) goto bad; break;
      case 'v': verbose = 1; break;
      case 'w': {
        long long w = 32;
        if (g.arg && (num_arg(g.arg, 1, &w) != 0)) {
          tool_err(err, "od", "invalid -w argument '%s'", g.arg);
          goto bad;
        }
        per_block = (size_t)w;
        break;
      }
      case 1001:
        if (strcmp(g.arg, "big") == 0) big = 1;
        else if (strcmp(g.arg, "little") == 0) big = 0;
        else {
          tool_err(err, "od", "invalid argument '%s' for '--endian'", g.arg);
          fd_printf(err, "Valid arguments are:\n  - 'big'\n  - 'little'\nTry 'od --help' for more information.\n");
          goto bad;
        }
        break;
      case 1002: break;
      case 'a': trad = "a"; break;
      case 'b': trad = "o1"; break;
      case 'c': trad = "c"; break;
      case 'd': trad = "u2"; break;
      case 'f': trad = "fF"; break;
      case 'i': trad = "dI"; break;
      case 'l': trad = "dL"; break;
      case 'o': trad = "o2"; break;
      case 's': trad = "d2"; break;
      case 'x': trad = "x2"; break;
      case OPT_HELP: opts_free(&g); free(specs); free(av); vec_free(&made); return tool_help(out, "od");
      default: goto bad;
    }
    if (trad && od_types(trad, &specs, &nspecs, err)) goto bad;
  }
  if (nspecs == 0) od_types("o2", &specs, &nspecs, err);
  for (i = 0; i < nspecs; i++) {	/* the block must hold whole items of every type */
    size_t a = lcm, b = (size_t)specs[i].size;
    while (b) { size_t t = a % b; a = b; b = t; }
    lcm = lcm / a * (size_t)specs[i].size;
  }
  if (per_block % lcm != 0) {
    tool_err(err, "od", "warning: invalid width %zu; using %d instead", per_block, (int)lcm);
    per_block = lcm;
  }
  {	/* the widths: every type's line as wide as the widest */
    int wpb = 0;
    for (i = 0; i < nspecs; i++) {
      int f = (int)(per_block / (size_t)specs[i].size), bw = (specs[i].width + 1) * f;
      if (bw > wpb) wpb = bw;
    }
    for (i = 0; i < nspecs; i++) {
      int f = (int)(per_block / (size_t)specs[i].size);
      specs[i].pad = wpb - specs[i].width * f;
    }
  }
  if (g.ops.n == 0) vec_push(&g.ops, xstrdup("-"));
  cs = (CStream *)xmalloc(sizeof(CStream));
  cs_init(cs, "od", &g.ops, in, err);
  block = (unsigned char *)xmalloc(per_block + 16);
  prev = (unsigned char *)xmalloc(per_block + 16);
  out_init(&o, out);
  /* -j: skip, seeking where the file allows it */
  offset = 0;
  while (offset < skip && !tool_stop()) {
    unsigned char tmp[65536];
    long long want = skip - offset;
    size_t got = cs_read(cs, tmp, want < (long long)sizeof(tmp) ? (size_t)want : sizeof(tmp));
    if (got == 0) break;
    offset += (long long)got;
  }
  if (offset < skip) {
    out_flush(&o);
    tool_err(err, "od", "cannot skip past end of combined input");
    status = 1;
  }
  else {
    size_t n = 0;
    for (;;) {	/* full blocks */
      size_t want = per_block;
      if (limit >= 0 && offset - skip + (long long)want > limit) want = (size_t)(limit - (offset - skip));
      n = want ? cs_read(cs, block, want) : 0;
      if (n < per_block || tool_stop() || o.failed) break;
      if (!verbose && !first && memcmp(prev, block, per_block) == 0) {
        if (!prev_equal) out_puts(&o, "*\n");
        prev_equal = 1;
      }
      else {
        prev_equal = 0;
        for (i = 0; i < nspecs; i++) {
          if (i == 0) od_address(&o, radix, offset);
          else if (radix != 'n') out_printf(&o, "%*s", radix == 'x' ? 6 : 7, "");
          od_line(&o, &specs[i], block, per_block, per_block, big);
          out_putc(&o, '\n');
        }
      }
      first = 0;
      memcpy(prev, block, per_block);
      offset += (long long)n;
    }
    if (n > 0) {	/* the last, short block: zero filled */
      memset(block + n, 0, per_block + 16 - n);
      for (i = 0; i < nspecs; i++) {
        if (i == 0) od_address(&o, radix, offset);
        else if (radix != 'n') out_printf(&o, "%*s", radix == 'x' ? 6 : 7, "");
        od_line(&o, &specs[i], block, per_block, n, big);
        out_putc(&o, '\n');
      }
      offset += (long long)n;
    }
    if (radix != 'n') {
      od_address(&o, radix, offset);
      out_putc(&o, '\n');
    }
  }
  out_flush(&o);
  cs_close(cs);
  if (cs->status) status = 1;
  free(cs);
  free(block);
  free(prev);
  free(specs);
  opts_free(&g);
  free(av);
  vec_free(&made);
  return status;
bad:
  free(specs);
  opts_free(&g);
  free(av);
  vec_free(&made);
  return 1;
}

/* }================================================================== */


/*
** {==================================================================
** xxd
** ===================================================================
*/

static int xxd_usage (int err) {
  fd_printf(err, "Usage:\n       xxd [options] [infile [outfile]]\n    or\n"
            "       xxd -r [-s [-]offset] [-c cols] [-ps] [infile [outfile]]\n"
            "Options: -a -b -C -c cols -d -E -e -g bytes -i -l len -n name -o off -p -r -s [+][-]seek -u -v\n");
  return 1;
}

/* the value of -c 8, -c8, -cols 8 ...; NULL: usage */
static const char *xxd_val (char **argv, int *i, const char *pp, const char *longrest) {
  if (pp[2] && strncmp(longrest, pp + 2, strlen(longrest)) != 0) return pp + 2;
  if (argv[*i + 1] == NULL) return NULL;
  return argv[++*i];
}

static int xxd_hexval (int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* xxd -r: as vim's huntype() */
static int xxd_revert (In *r, int ofd, int cols, int plain, long long base_off, int err) {
  Out o;
  int c, n1 = -1, n2 = 0, n3, ign_garb = 1, p = cols, seekable;
  long long have_off = 0, want_off = 0;
  char chunk[65536];
  long got = 0, pos = 0;
  out_init(&o, ofd);
  seekable = os_seek(ofd, 0, 1) >= 0;
#define XGETC()	(pos < got ? (unsigned char)chunk[pos++] : \
                 ((got = in_read(r, chunk, sizeof(chunk))) > 0 ? (pos = 1, (unsigned char)chunk[0]) : -1))
  while ((c = XGETC()) >= 0) {
    if (c == '\r') continue;
    if (plain && (c == ' ' || c == '\n' || c == '\t')) continue;
    n3 = n2;
    n2 = n1;
    n1 = xxd_hexval(c);
    if (n1 == -1 && ign_garb) continue;
    ign_garb = 0;
    if (!plain && p >= cols) {	/* the address */
      if (n1 < 0) {
        p = 0;
        continue;
      }
      want_off = (want_off << 4) | n1;
      continue;
    }
    if (base_off + want_off != have_off) {
      out_flush(&o);
      if (seekable && os_seek(ofd, base_off + want_off - have_off, 1) >= 0) have_off = base_off + want_off;
      if (base_off + want_off < have_off) {
        out_flush(&o);
        fd_printf(err, "xxd: Sorry, cannot seek backwards.\n");
        return 5;
      }
      for (; have_off < base_off + want_off; have_off++) out_putc(&o, 0);
    }
    if (n2 >= 0 && n1 >= 0) {
      out_putc(&o, (n2 << 4) | n1);
      have_off++;
      want_off++;
      n1 = -1;
      if (!plain && ++p >= cols)	/* the rest of the line is the text column */
        while (c != '\n' && c >= 0) c = XGETC();
    }
    else if (n1 < 0 && n2 < 0 && n3 < 0)	/* garbage: skip the line */
      while (c != '\n' && c >= 0) c = XGETC();
    if (c == '\n') {
      if (!plain) want_off = 0;
      p = cols;
      ign_garb = 1;
    }
    if (c < 0) break;
  }
#undef XGETC
  out_flush(&o);
  return 0;
}

/* vim's xxdline(): -a squeezes runs of zero lines to '*' */
static void xxd_line (Out *o, const char *l, int nz, char *z, int *zero_seen) {
  if (!nz && *zero_seen == 1) strcpy(z, l);
  if (nz || !(*zero_seen)++) {
    if (nz) {
      if (nz < 0) (*zero_seen)--;
      if (*zero_seen == 2) out_puts(o, z);
      if (*zero_seen > 2) out_puts(o, "*\n");
    }
    if (nz >= 0 || *zero_seen > 0) out_puts(o, l);
    if (nz) *zero_seen = 0;
  }
}


int t_xxd (int argc, char **argv, int in, int out, int err) {
  enum { X_NORMAL, X_PLAIN, X_INCLUDE, X_BITS, X_LITTLE };
  int i, type = X_NORMAL, autoskip = 0, capital = 0, decimal = 0, revert = 0, cols = 0, colsgiven = 0;
  int octs = -1, relseek = 0, negseek = 0, ofd = out, own_out = 0, status = 0;
  long long seekoff = 0, length = -1, displayoff = 0;
  const char *hexx = "0123456789abcdef", *varname = NULL, *iname = NULL, *oname = NULL;
  In r;
  Out o;
  for (i = 1; i < argc; i++) {
    const char *pp = argv[i], *v;
    if (pp[0] == '-' && pp[1] == '-' && pp[2]) pp++;
    if (strncmp(pp, "-a", 2) == 0) autoskip = !autoskip;
    else if (strncmp(pp, "-b", 2) == 0) type = X_BITS;
    else if (strncmp(pp, "-e", 2) == 0) type = X_LITTLE;
    else if (strncmp(pp, "-u", 2) == 0) hexx = "0123456789ABCDEF";
    else if (strncmp(pp, "-p", 2) == 0) type = X_PLAIN;
    else if (strncmp(pp, "-i", 2) == 0) type = X_INCLUDE;
    else if (strncmp(pp, "-C", 2) == 0) capital = 1;
    else if (strncmp(pp, "-d", 2) == 0) decimal = 1;
    else if (strncmp(pp, "-r", 2) == 0) revert++;
    else if (strncmp(pp, "-E", 2) == 0) ;
    else if (strncmp(pp, "-v", 2) == 0) {
      fd_printf(err, "xxd 2024-01-01 by Juergen Weigert et al. (mmc)\n");
      return 0;
    }
    else if (strcmp(pp, "-h") == 0 || strcmp(pp, "-help") == 0) return tool_help(out, "xxd");
    else if (strncmp(pp, "-c", 2) == 0) {
      if (pp[2] && strncmp("apitalize", pp + 2, 9) == 0) capital = 1;
      else {
        if ((v = xxd_val(argv, &i, pp, "ols")) == NULL) return xxd_usage(err);
        cols = (int)strtol(v, NULL, 0);
        colsgiven = 1;
      }
    }
    else if (strncmp(pp, "-g", 2) == 0) {
      if ((v = xxd_val(argv, &i, pp, "roup")) == NULL) return xxd_usage(err);
      octs = (int)strtol(v, NULL, 0);
    }
    else if (strncmp(pp, "-o", 2) == 0) {
      if ((v = xxd_val(argv, &i, pp, "ffset")) == NULL) return xxd_usage(err);
      displayoff = (long long)strtoul(v, NULL, 0);
    }
    else if (strncmp(pp, "-s", 2) == 0) {
      relseek = negseek = 0;
      if (pp[2] && strncmp("kip", pp + 2, 3) && strncmp("eek", pp + 2, 3)) v = pp + 2;
      else {
        if (argv[i + 1] == NULL) return xxd_usage(err);
        v = argv[++i];
      }
      if (*v == '+') { relseek = 1; v++; }
      if (*v == '-') { negseek = 1; v++; }
      seekoff = strtol(v, NULL, 0);
    }
    else if (strncmp(pp, "-l", 2) == 0) {
      if ((v = xxd_val(argv, &i, pp, "en")) == NULL) return xxd_usage(err);
      length = strtol(v, NULL, 0);
    }
    else if (strncmp(pp, "-n", 2) == 0) {
      if ((v = xxd_val(argv, &i, pp, "ame")) == NULL) return xxd_usage(err);
      varname = v;
    }
    else if (strncmp(pp, "-R", 2) == 0) {
      if (!pp[2]) {
        if (argv[i + 1] == NULL) return xxd_usage(err);
        i++;
      }
    }
    else if (strcmp(pp, "--") == 0) {
      i++;
      break;
    }
    else if (pp[0] == '-' && pp[1]) return xxd_usage(err);
    else break;
  }
  if (!colsgiven || (!cols && type != X_PLAIN))
    cols = type == X_PLAIN ? 30 : type == X_INCLUDE ? 12 : type == X_BITS ? 6 : 16;
  if (octs < 0) octs = type == X_BITS ? 1 : type == X_NORMAL ? 2 : type == X_LITTLE ? 4 : 0;
  if ((type == X_PLAIN && cols < 0) || (type != X_PLAIN && cols < 1) ||
      ((type == X_NORMAL || type == X_BITS || type == X_LITTLE) && cols > 256)) {
    fd_printf(err, "xxd: invalid number of columns (max. %d).\n", 256);
    return 1;
  }
  if (octs < 1 || octs > cols) octs = cols;
  else if (type == X_LITTLE && (octs & (octs - 1))) {
    fd_printf(err, "xxd: number of octets per group must be a power of 2 with -e.\n");
    return 1;
  }
  if (argc - i > 2) return xxd_usage(err);
  if (i < argc && strcmp(argv[i], "-") != 0) iname = argv[i];
  if (i + 1 < argc && strcmp(argv[i + 1], "-") != 0) oname = argv[i + 1];
  if (iname == NULL) in_init(&r, in, 0);
  else {
    char *native = path_to_native(iname);
    OsStat st;
    int isdir = os_stat(native, &st) == 0 && st.is_dir;
    free(native);
    if (isdir || in_open(&r, iname, in) != 0) {
      fd_printf(err, "xxd: %s: %s\n", iname, isdir ? "Is a directory" : os_errmsg());
      return 2;
    }
  }
  if (oname) {
    char *native = path_to_native(oname);
    OsStat st;
    /* -r patches an existing file in place */
    ofd = os_open(native, revert && os_stat(native, &st) == 0 ? OS_RDWR : OS_WRITE);
    free(native);
    if (ofd < 0) {
      fd_printf(err, "xxd: %s: %s\n", oname, os_errmsg());
      in_close(&r);
      return 3;
    }
    own_out = 1;
  }
  if (revert) {
    if (type != X_NORMAL && type != X_PLAIN) {
      fd_printf(err, "xxd: Sorry, cannot revert this type of hexdump\n");
      status = 1;
    }
    else status = xxd_revert(&r, ofd, cols, type == X_PLAIN, negseek ? -seekoff : seekoff, err);
    in_close(&r);
    if (own_out) os_close(ofd);
    return status;
  }
  /* -s: seek (or read over) */
  if (seekoff || negseek || !relseek) {
    long long cur = os_seek(r.fd, 0, 1), e = -1;
    if (cur >= 0) {
      if (relseek) e = os_seek(r.fd, negseek ? cur - seekoff : cur + seekoff, 0);
      else if (negseek) {
        long long end = os_seek(r.fd, 0, 2);
        e = end >= 0 && end - seekoff >= 0 ? os_seek(r.fd, end - seekoff, 0) : -1;
      }
      else e = os_seek(r.fd, seekoff, 0);
    }
    if (e < 0 && negseek) {
      fd_printf(err, "xxd: Sorry, cannot seek.\n");
      in_close(&r);
      if (own_out) os_close(ofd);
      return 4;
    }
    if (e >= 0) seekoff = e;
    else {
      long long s = seekoff;
      char tmp[4096];
      while (s > 0) {
        long n = in_read(&r, tmp, s < (long long)sizeof(tmp) ? (size_t)s : sizeof(tmp));
        if (n <= 0) {
          fd_printf(err, "xxd: Sorry, cannot seek.\n");
          in_close(&r);
          if (own_out) os_close(ofd);
          return 4;
        }
        s -= n;
      }
    }
  }
  out_init(&o, ofd);
  if (type == X_INCLUDE) {
    long long p = 0;
    int ch;
    char name[1024];
    unsigned char byte;
    if (iname || varname) {	/* "unsigned char file_txt[] = {" */
      const char *src = varname ? varname : iname;
      size_t k = 0;
      if (!varname && isdigit((unsigned char)src[0])) { name[k++] = '_'; name[k++] = '_'; }
      for (; *src && k < sizeof(name) - 1; src++) {
        if (varname) name[k++] = capital ? (char)toupper((unsigned char)*src) : *src;
        else name[k++] = isalnum((unsigned char)*src) ? (capital ? (char)toupper((unsigned char)*src) : *src) : '_';
      }
      name[k] = '\0';
      if (iname) out_printf(&o, "unsigned char %s[] = {\n", name);
    }
    while ((length < 0 || p < length) && in_read(&r, (char *)&byte, 1) == 1) {
      ch = byte;
      out_printf(&o, hexx[10] == 'a' ? "%s0x%02x" : "%s0X%02X",
                 (p % cols) ? ", " : (!p ? "  " : ",\n  "), ch);
      p++;
    }
    if (p) out_puts(&o, "\n");
    if (iname) {
      out_printf(&o, "};\nunsigned int %s_%s = %lld;\n", name, capital ? "LEN" : "len", p);
    }
  }
  else if (type == X_PLAIN) {
    long long n = 0;
    int p = cols;
    unsigned char chunk[65536];
    long got = 0, k = 0;
    for (;;) {
      if (length >= 0 && n >= length) break;
      if (k == got) {
        got = in_read(&r, (char *)chunk, sizeof(chunk));
        k = 0;
        if (got <= 0) break;
      }
      out_putc(&o, hexx[chunk[k] >> 4]);
      out_putc(&o, hexx[chunk[k] & 15]);
      k++;
      n++;
      if (cols > 0 && !--p) {
        out_putc(&o, '\n');
        p = cols;
      }
      if (o.failed) break;
    }
    if (cols == 0 || p < cols) out_putc(&o, '\n');
  }
  else {	/* the classic dump, laid out as vim's xxd does */
    char l[4096], z[4096];
    int p = 0, c = 0, addrlen = 9, nonzero = 0, zero_seen = 0;
    int grplen = type == X_BITS ? 8 * octs + 1 : 2 * octs + 1;
    long long n = 0;
    unsigned char chunk[65536];
    long got = 0, k = 0;
    z[0] = '\0';
    for (;;) {
      int e, x;
      if (length >= 0 && n >= length) break;
      if (k == got) {
        got = in_read(&r, (char *)chunk, sizeof(chunk));
        k = 0;
        if (got <= 0) break;
      }
      e = chunk[k++];
      if (p == 0) {
        addrlen = snprintf(l, sizeof(l), decimal ? "%08lld:" : "%08llx:", n + seekoff + displayoff);
        for (c = addrlen; c < (int)sizeof(l) - 1; c++) l[c] = ' ';
      }
      x = type == X_LITTLE ? p ^ (octs - 1) : p;
      c = addrlen + 1 + (grplen * x) / octs;
      if (type == X_BITS) {
        int b;
        for (b = 7; b >= 0; b--) l[c++] = (e & (1 << b)) ? '1' : '0';
      }
      else {
        l[c] = hexx[(e >> 4) & 15];
        l[++c] = hexx[e & 15];
      }
      if (e) nonzero++;
      c = addrlen + 3 + (grplen * cols - 1) / octs + p;
      l[c++] = (e > 31 && e < 127) ? (char)e : '.';
      n++;
      if (++p == cols) {
        l[c] = '\n';
        l[++c] = '\0';
        xxd_line(&o, l, autoskip ? nonzero : 1, z, &zero_seen);
        nonzero = 0;
        p = 0;
      }
      if (o.failed || tool_stop()) break;
    }
    if (p) {
      l[c] = '\n';
      l[++c] = '\0';
      xxd_line(&o, l, 1, z, &zero_seen);
    }
    else if (autoskip) xxd_line(&o, l, -1, z, &zero_seen);
  }
  out_flush(&o);
  in_close(&r);
  if (own_out) os_close(ofd);
  return status;
}

/* }================================================================== */


/*
** {==================================================================
** hexdump (util-linux): the format engine for the built-in formats
** and -e
** ===================================================================
*/

enum { HF_ADDRESS = 1, HF_BPAD, HF_C, HF_CHAR, HF_DBL, HF_INT, HF_P, HF_STR, HF_TEXT, HF_U, HF_UINT };

typedef struct HdPr {	/* one conversion, with the text before it */
  char *fmt;	/* a printf format for this piece */
  int flags, bcnt;
  char conv;	/* the printf conversion letter in fmt */
  size_t conv_at;	/* where the conversion is in fmt */
  int nospace;	/* the last repetition drops fmt's final blank */
} HdPr;

typedef struct HdFu {	/* "8/2 "%04x "" */
  int reps, bcnt, setrep, ignore;
  HdPr *pr;
  size_t npr;
} HdFu;

typedef struct HdFs {	/* one -e format string */
  HdFu *fu;
  size_t nfu;
  int bcnt;
} HdFs;

typedef struct Hexdump {
  HdFs *fs;
  size_t nfs;
  HdFu *endfu;
  int blocksize, err;
} Hexdump;

static int hd_escape (char **p) {	/* \n \t ... inside a format */
  char c = *++(*p);
  switch (c) {
    case 'a': return '\a';
    case 'b': return '\b';
    case 'f': return '\f';
    case 'n': return '\n';
    case 'r': return '\r';
    case 't': return '\t';
    case 'v': return '\v';
    case '0': return '\0';
    case '\0': (*p)--; return '\\';
    default: return c;
  }
}

/* one -e string: units of [iter][/count] "format" */
static int hd_add (Hexdump *h, const char *fmt) {
  HdFs fs;
  const char *p = fmt;
  memset(&fs, 0, sizeof(fs));
  for (;;) {
    HdFu fu;
    Buf text;
    const char *q;
    while (isspace((unsigned char)*p)) p++;
    if (!*p) break;
    memset(&fu, 0, sizeof(fu));
    fu.reps = 1;
    if (isdigit((unsigned char)*p)) {
      fu.reps = atoi(p);
      fu.setrep = 1;
      while (isdigit((unsigned char)*p)) p++;
      while (isspace((unsigned char)*p)) p++;
      if (*p != '/') {
        while (isspace((unsigned char)*p)) p++;
      }
    }
    if (*p == '/') {
      p++;
      while (isspace((unsigned char)*p)) p++;
      if (!isdigit((unsigned char)*p)) {
        tool_err(h->err, "hexdump", "bad byte count for conversion character %s", p);
        return -1;
      }
      fu.bcnt = atoi(p);
      while (isdigit((unsigned char)*p)) p++;
      while (isspace((unsigned char)*p)) p++;
    }
    if (*p != '"') {
      tool_err(h->err, "hexdump", "bad format {%s}", fmt);
      return -1;
    }
    q = ++p;
    while (*p && *p != '"') {
      if (*p == '\\' && p[1]) p++;
      p++;
    }
    if (*p != '"') {
      tool_err(h->err, "hexdump", "bad format {%s}", fmt);
      return -1;
    }
    /* the format text with escapes done */
    buf_init(&text);
    {
      char *raw = xstrndup(q, (size_t)(p - q)), *s;
      for (s = raw; *s; s++) buf_putc(&text, *s == '\\' ? (char)hd_escape(&s) : *s);
      free(raw);
    }
    p++;
    /* split it into conversions, each with the text before it */
    {
      size_t i = 0, start = 0;
      while (i <= text.len) {
        HdPr pr;
        Buf f;
        size_t k;
        memset(&pr, 0, sizeof(pr));
        while (i < text.len && text.s[i] != '%') i++;
        if (i >= text.len) {	/* only text left */
          if (start < text.len) {
            pr.flags = HF_TEXT;
            pr.fmt = xstrndup(text.s + start, text.len - start);
            fu.pr = (HdPr *)xrealloc(fu.pr, (fu.npr + 1) * sizeof(HdPr));
            fu.pr[fu.npr++] = pr;
          }
          break;
        }
        if (text.s[i + 1] == '%') {	/* "%%" is text */
          i += 2;
          continue;
        }
        buf_init(&f);
        buf_putn(&f, text.s + start, i - start);
        buf_putc(&f, '%');
        k = i + 1;
        while (k < text.len && strchr(" -0+#", text.s[k])) buf_putc(&f, text.s[k++]);
        while (k < text.len && (isdigit((unsigned char)text.s[k]) || text.s[k] == '.')) buf_putc(&f, text.s[k++]);
        if (k >= text.len) {
          tool_err(h->err, "hexdump", "bad conversion character %%%s", text.s + i + 1);
          buf_free(&f);
          buf_free(&text);
          return -1;
        }
        switch (text.s[k]) {
          case 'c': pr.flags = HF_CHAR; pr.bcnt = 1; pr.conv = 'c'; break;
          case 'd': case 'i': pr.flags = HF_INT; pr.conv = 'd'; pr.bcnt = fu.bcnt ? fu.bcnt : 4; break;
          case 'o': case 'u': case 'x': case 'X':
            pr.flags = HF_UINT; pr.conv = text.s[k]; pr.bcnt = fu.bcnt ? fu.bcnt : 4; break;
          case 'e': case 'E': case 'f': case 'g': case 'G':
            pr.flags = HF_DBL; pr.conv = text.s[k]; pr.bcnt = fu.bcnt ? fu.bcnt : 8; break;
          case 's': pr.flags = HF_STR; pr.conv = 's'; pr.bcnt = fu.bcnt; break;
          case '_':
            k++;
            switch (k < text.len ? text.s[k] : 0) {
              case 'A': case 'a':
                pr.flags = HF_ADDRESS;
                if (text.s[k] == 'A') fu.ignore = 1;	/* printed once, at the end */
                k++;
                if (k >= text.len || !strchr("dox", text.s[k])) {
                  tool_err(h->err, "hexdump", "bad conversion character %%_%c", text.s[k - 1]);
                  buf_free(&f);
                  buf_free(&text);
                  return -1;
                }
                pr.conv = text.s[k];
                break;
              case 'c': pr.flags = HF_C; pr.conv = 'c'; pr.bcnt = 1; break;
              case 'p': pr.flags = HF_P; pr.conv = 'c'; pr.bcnt = 1; break;
              case 'u': pr.flags = HF_U; pr.conv = 'c'; pr.bcnt = 1; break;
              default:
                tool_err(h->err, "hexdump", "bad conversion character %%_%c", k < text.len ? text.s[k] : ' ');
                buf_free(&f);
                buf_free(&text);
                return -1;
            }
            break;
          default:
            tool_err(h->err, "hexdump", "bad conversion character %%%c", text.s[k]);
            buf_free(&f);
            buf_free(&text);
            return -1;
        }
        if ((pr.flags == HF_INT || pr.flags == HF_UINT) && pr.bcnt != 1 && pr.bcnt != 2 &&
            pr.bcnt != 4 && pr.bcnt != 8) {
          tool_err(h->err, "hexdump", "bad byte count for conversion character %c", pr.conv);
          buf_free(&f);
          buf_free(&text);
          return -1;
        }
        if (pr.flags == HF_DBL && pr.bcnt != 4 && pr.bcnt != 8) {
          tool_err(h->err, "hexdump", "bad byte count for conversion character %c", pr.conv);
          buf_free(&f);
          buf_free(&text);
          return -1;
        }
        pr.conv_at = f.len;
        if (pr.flags == HF_INT || pr.flags == HF_UINT || pr.flags == HF_ADDRESS) buf_puts(&f, "ll");
        buf_putc(&f, pr.conv);
        pr.fmt = buf_take(&f);
        fu.pr = (HdPr *)xrealloc(fu.pr, (fu.npr + 1) * sizeof(HdPr));
        fu.pr[fu.npr++] = pr;
        i = k + 1;
        start = i;
      }
    }
    buf_free(&text);
    {	/* the unit's byte count: what its conversions use */
      size_t k;
      int sum = 0;
      for (k = 0; k < fu.npr; k++) sum += fu.pr[k].bcnt;
      fu.bcnt = sum;
    }
    fs.fu = (HdFu *)xrealloc(fs.fu, (fs.nfu + 1) * sizeof(HdFu));
    fs.fu[fs.nfu++] = fu;
  }
  {
    size_t k;
    fs.bcnt = 0;
    for (k = 0; k < fs.nfu; k++) fs.bcnt += fs.fu[k].reps * fs.fu[k].bcnt;
  }
  h->fs = (HdFs *)xrealloc(h->fs, (h->nfs + 1) * sizeof(HdFs));
  h->fs[h->nfs++] = fs;
  return 0;
}

/* after all formats: the block size, repeats and the "%_A" unit */
static void hd_finish (Hexdump *h) {
  size_t i, k, j;
  h->blocksize = 0;
  for (i = 0; i < h->nfs; i++)
    if (h->fs[i].bcnt > h->blocksize) h->blocksize = h->fs[i].bcnt;
  for (i = 0; i < h->nfs; i++) {
    HdFs *fs = &h->fs[i];
    for (k = 0; k < fs->nfu; k++) {
      HdFu *fu = &fs->fu[k];
      for (j = 0; j < fu->npr; j++)
        if (fu->pr[j].flags == HF_ADDRESS && fu->ignore) h->endfu = fu;
      /* the last unit repeats to fill the block */
      if (k + 1 == fs->nfu && fs->bcnt < h->blocksize && !fu->setrep && fu->bcnt)
        fu->reps += (h->blocksize - fs->bcnt) / fu->bcnt;
      if (fu->reps > 1 && fu->npr > 0) {
        HdPr *pr = &fu->pr[fu->npr - 1];
        size_t n = strlen(pr->fmt);
        if (n > 0 && isspace((unsigned char)pr->fmt[n - 1])) pr->nospace = 1;
      }
    }
  }
}

static void hd_bpad (HdPr *pr) {	/* past the end: blanks of the same width */
  Buf f;
  const char *p = pr->fmt;
  size_t at = pr->conv_at;
  const char *pct = p + at;
  while (pct > p && *pct != '%') pct--;
  buf_init(&f);
  buf_putn(&f, p, (size_t)(pct - p) + 1);
  pct++;
  while (*pct && strchr(" -0+#", *pct)) pct++;
  while (pct < p + at) buf_putc(&f, *pct++);
  /* precision makes no sense for "%s" of "" here, but GNU keeps it */
  buf_putc(&f, 's');
  free(pr->fmt);
  pr->fmt = buf_take(&f);
  pr->flags = HF_BPAD;
}

static void hd_print (Out *o, HdPr *pr, const unsigned char *bp, long long address, int last) {
  char tmpfmt[256];
  const char *fmt = pr->fmt;
  if (last && pr->nospace) {
    snprintf(tmpfmt, sizeof(tmpfmt), "%.*s", (int)strlen(pr->fmt) - 1, pr->fmt);
    fmt = tmpfmt;
  }
  switch (pr->flags) {
    case HF_ADDRESS: out_printf(o, fmt, address); break;
    case HF_BPAD: out_printf(o, fmt, ""); break;
    case HF_TEXT: {	/* no conversions: "%%" is a '%' */
      const char *s;
      for (s = fmt; *s; s++) {
        if (s[0] == '%' && s[1] == '%') s++;
        out_putc(o, *s);
      }
      break;
    }
    case HF_CHAR: out_printf(o, fmt, bp[0]); break;
    case HF_P: out_printf(o, fmt, bp[0] >= 32 && bp[0] < 127 ? bp[0] : '.'); break;
    case HF_C: case HF_U: {
      static const char *const names[32] = {
        "nul", "soh", "stx", "etx", "eot", "enq", "ack", "bel", "bs", "ht", "lf", "vt", "ff", "cr",
        "so", "si", "dle", "dc1", "dc2", "dc3", "dc4", "nak", "syn", "etb", "can", "em", "sub", "esc",
        "fs", "gs", "rs", "us"
      };
      const char *str = NULL;
      char buf[16], sfmt[256];
      if (pr->flags == HF_C) {
        switch (bp[0]) {
          case '\0': str = "\\0"; break;
          case '\a': str = "\\a"; break;
          case '\b': str = "\\b"; break;
          case '\f': str = "\\f"; break;
          case '\n': str = "\\n"; break;
          case '\r': str = "\\r"; break;
          case '\t': str = "\\t"; break;
          case '\v': str = "\\v"; break;
          default:
            if (!(bp[0] >= 32 && bp[0] < 127)) {
              snprintf(buf, sizeof(buf), "%03o", bp[0]);
              str = buf;
            }
        }
      }
      else {
        if (bp[0] < 32) str = names[bp[0]];
        else if (bp[0] == 127) str = "del";
        else if (bp[0] >= 128) {
          snprintf(buf, sizeof(buf), "%x", bp[0]);
          str = buf;
        }
      }
      if (str == NULL) out_printf(o, fmt, bp[0]);
      else {	/* the same format, as a string */
        snprintf(sfmt, sizeof(sfmt), "%s", fmt);
        sfmt[pr->conv_at] = 's';
        out_printf(o, sfmt, str);
      }
      break;
    }
    case HF_INT: case HF_UINT: {
      unsigned long long v = od_get(bp, pr->bcnt, 0);
      if (pr->flags == HF_INT) {	/* sign-extended, passed as signed */
        long long sv;
        if (pr->bcnt < 8) {
          unsigned long long sign = 1ULL << (8 * pr->bcnt - 1);
          sv = (v & sign) ? -(long long)((~v & (sign - 1)) + 1) : (long long)v;
        }
        else sv = (long long)v;
        out_printf(o, fmt, sv);
      }
      else out_printf(o, fmt, v);
      break;
    }
    case HF_DBL: {
      double d;
      if (pr->bcnt == 4) {
        float f;
        memcpy(&f, bp, 4);
        d = f;
      }
      else memcpy(&d, bp, 8);
      out_printf(o, fmt, d);
      break;
    }
    case HF_STR: {
      char sbuf[1024];
      size_t n = pr->bcnt > 0 && pr->bcnt < (int)sizeof(sbuf) ? (size_t)pr->bcnt : 0;
      memcpy(sbuf, bp, n);
      sbuf[n] = '\0';
      out_printf(o, fmt, sbuf);
      break;
    }
  }
}


int t_hexdump (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {{"one-byte-octal", 'b', 0}, {"one-byte-char", 'c', 0},
    {"canonical", 'C', 0}, {"two-bytes-decimal", 'd', 0}, {"two-bytes-octal", 'o', 0},
    {"two-bytes-hex", 'x', 0}, {"format", 'e', 1}, {"format-file", 'f', 1}, {"length", 'n', 1},
    {"skip", 's', 1}, {"no-squeezing", 'v', 0}, {NULL, 0, 0}};
  static const char *const deflt[2] = {"\"%07.7_Ax\n\"", "\"%07.7_ax \" 8/2 \"%04x \" \"\\n\""};
  Opts g;
  Out o;
  Hexdump h;
  CStream *cs;
  int c, status = 0, squeeze = 1, any = 0, vflag = 0;	/* vflag: 0 first, 1 wait, 2 dup */
  long long length = -1, skip = 0, address, eaddress = -1;
  unsigned char *curp, *savp;
  size_t i, k, j;
  memset(&h, 0, sizeof(h));
  h.err = err;
  opts_init(&g, "hexdump", argc, argv, err);
  while ((c = opts_next(&g, "bcCde:f:n:os:vx", lo)) != 0) {
    const char *f1 = NULL;
    switch (c) {
      case 'b': f1 = "\"%07.7_ax \" 16/1 \"%03o \" \"\\n\""; break;
      case 'c': f1 = "\"%07.7_ax \" 16/1 \"%3_c \" \"\\n\""; break;
      case 'C':
        if (hd_add(&h, "\"%08.8_Ax\n\"") || hd_add(&h, "\"%08.8_ax  \" 8/1 \"%02x \" \"  \" 8/1 \"%02x \" ") ||
            hd_add(&h, "\"  |\" 16/1 \"%_p\" \"|\\n\"")) goto bad;
        any = 1;
        break;
      case 'd': f1 = "\"%07.7_ax \" 8/2 \"  %05u \" \"\\n\""; break;
      case 'o': f1 = "\"%07.7_ax \" 8/2 \" %06o \" \"\\n\""; break;
      case 'x': f1 = "\"%07.7_ax \" 8/2 \"   %04x \" \"\\n\""; break;
      case 'e': if (hd_add(&h, g.arg)) goto bad; any = 1; break;
      case 'f': {
        In r;
        char *line;
        size_t len;
        if (m_open(&r, "hexdump", g.arg, in, err, OPEN_PLAIN, NULL) != 0) goto bad;
        while (in_line(&r, &line, &len, '\n', NULL)) {
          char *s = line;
          while (isspace((unsigned char)*s)) s++;
          if (*s && *s != '#' && hd_add(&h, s)) {
            in_close(&r);
            goto bad;
          }
        }
        in_close(&r);
        any = 1;
        break;
      }
      case 'n':
        if (od_num(g.arg, &length) != 0) {
          tool_err(err, "hexdump", "failed to parse length '%s'", g.arg);
          goto bad;
        }
        break;
      case 's':
        if (od_num(g.arg, &skip) != 0) {
          tool_err(err, "hexdump", "failed to parse offset '%s'", g.arg);
          goto bad;
        }
        break;
      case 'v': squeeze = 0; break;
      case OPT_HELP: opts_free(&g); return tool_help(out, "hexdump");
      default: goto bad;
    }
    if (f1) {
      if (hd_add(&h, "\"%07.7_Ax\n\"") || hd_add(&h, f1)) goto bad;
      any = 1;
    }
  }
  if (!any && (hd_add(&h, deflt[0]) || hd_add(&h, deflt[1]))) goto bad;
  hd_finish(&h);
  if (h.blocksize <= 0) h.blocksize = 1;
  if (g.ops.n == 0) vec_push(&g.ops, xstrdup("-"));
  cs = (CStream *)xmalloc(sizeof(CStream));
  cs_init(cs, "hexdump", &g.ops, in, err);
  curp = (unsigned char *)xmalloc((size_t)h.blocksize + 8);
  savp = (unsigned char *)xmalloc((size_t)h.blocksize + 8);
  memset(savp, 0, (size_t)h.blocksize + 8);
  out_init(&o, out);
  /* -s */
  address = 0;
  while (address < skip) {
    unsigned char tmp[65536];
    long long want = skip - address;
    size_t got = cs_read(cs, tmp, want < (long long)sizeof(tmp) ? (size_t)want : sizeof(tmp));
    if (got == 0) break;
    address += (long long)got;
  }
  /* the blocks, as util-linux's get() and display() */
  for (;;) {
    size_t need = (size_t)h.blocksize, nread = 0;
    int have = 0;
    for (;;) {
      size_t want = need, n;
      if (length == 0) break;
      if (length > 0 && (long long)want > length) want = (size_t)length;
      n = cs_read(cs, curp + nread, want);
      if (length > 0) length -= (long long)n;
      if (n == 0) break;
      nread += n;
      need -= n;
      if (need == 0) {
        if (!squeeze || vflag == 0 || memcmp(curp, savp, (size_t)h.blocksize) != 0) {
          if (vflag == 2 || vflag == 0) vflag = 1;
          have = 1;
          break;
        }
        if (vflag == 1) out_puts(&o, "*\n");
        vflag = 2;
        address += h.blocksize;
        need = (size_t)h.blocksize;
        nread = 0;
      }
      if (length == 0 && need > 0) break;
    }
    if (!have) {	/* the end: a short block, or nothing */
      if (need == (size_t)h.blocksize) break;
      memset(curp + nread, 0, need);
      eaddress = address + (long long)nread;
    }
    for (i = 0; i < h.nfs && !o.failed; i++) {
      long long a = address;
      const unsigned char *bp = curp;
      HdFs *fs = &h.fs[i];
      for (k = 0; k < fs->nfu; k++) {
        HdFu *fu = &fs->fu[k];
        int cnt;
        if (fu->ignore) break;
        for (cnt = fu->reps; cnt > 0; cnt--) {
          for (j = 0; j < fu->npr; j++) {
            HdPr *pr = &fu->pr[j];
            if (eaddress >= 0 && a >= eaddress && pr->flags != HF_TEXT && pr->flags != HF_BPAD) hd_bpad(pr);
            hd_print(&o, pr, bp, a, cnt == 1);
            a += pr->bcnt;
            bp += pr->bcnt;
          }
        }
      }
    }
    if (!have || tool_stop()) break;
    {
      unsigned char *t = curp;
      curp = savp;
      savp = t;
    }
    address += h.blocksize;
  }
  if (h.endfu) {	/* the final address */
    if (eaddress < 0) eaddress = address;
    if (eaddress > 0 || address > 0) {
      for (j = 0; j < h.endfu->npr; j++) {
        HdPr *pr = &h.endfu->pr[j];
        if (pr->flags == HF_ADDRESS) out_printf(&o, pr->fmt, eaddress);
        else if (pr->flags == HF_TEXT) hd_print(&o, pr, NULL, 0, 0);
      }
    }
  }
  out_flush(&o);
  cs_close(cs);
  if (cs->status) status = 1;
  free(cs);
  free(curp);
  free(savp);
  for (i = 0; i < h.nfs; i++) {
    for (k = 0; k < h.fs[i].nfu; k++) {
      for (j = 0; j < h.fs[i].fu[k].npr; j++) free(h.fs[i].fu[k].pr[j].fmt);
      free(h.fs[i].fu[k].pr);
    }
    free(h.fs[i].fu);
  }
  free(h.fs);
  opts_free(&g);
  return status;
bad:
  opts_free(&g);
  return 1;
}

/* }================================================================== */


/*
** {==================================================================
** strings
** ===================================================================
*/

int t_strings (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {{"all", 'a', 0}, {"bytes", 'n', 1}, {"radix", 't', 1},
    {"encoding", 'e', 1}, {"print-file-name", 'f', 0}, {"include-all-whitespace", 'w', 0},
    {"output-separator", 's', 1}, {"data", 'd', 0}, {NULL, 0, 0}};
  Opts g;
  Out o;
  int c, status = 0, names = 0, allws = 0, eight = 0, nargc;
  long long minlen = 4;
  char radix = 0;
  const char *sep = "\n";
  char **v = legacy_num(argc, argv, &nargc, "-n");
  size_t i;
  Buf cur;
  opts_init(&g, "strings", nargc, v, err);
  while ((c = opts_next(&g, "adfn:ot:e:ws:", lo)) != 0) {
    switch (c) {
      case 'a': case 'd': break;
      case 'f': names = 1; break;
      case 'n':
        if (num_arg(g.arg, LLONG_MIN, &minlen) != 0) {
          tool_err(err, "strings", "invalid integer argument %s", g.arg);
          goto bad;
        }
        if (minlen < 1) {
          tool_err(err, "strings", "minimum string length is too small: %s", g.arg);
          goto bad;
        }
        break;
      case 'o': radix = 'o'; break;
      case 't':
        if (strlen(g.arg) != 1 || !strchr("odx", g.arg[0])) {
          tool_err(err, "strings", "invalid radix");
          goto bad;
        }
        radix = g.arg[0];
        break;
      case 'e':
        if (strcmp(g.arg, "s") == 0) eight = 0;
        else if (strcmp(g.arg, "S") == 0) eight = 1;
        else {
          tool_err(err, "strings", "invalid encoding");	/* b l B L: not done */
          goto bad;
        }
        break;
      case 'w': allws = 1; break;
      case 's': sep = g.arg; break;
      case OPT_HELP: opts_free(&g); free(v); return tool_help(out, "strings");
      default: goto bad;
    }
  }
  if (g.ops.n == 0) vec_push(&g.ops, xstrdup("-"));
  out_init(&o, out);
  buf_init(&cur);
  for (i = 0; i < g.ops.n && !o.failed && !tool_stop(); i++) {
    In r;
    const char *name = g.ops.v[i];
    unsigned char chunk[65536];
    long got;
    long long off = 0, start = 0;
    int printing = 0;
    if (strcmp(name, "-") == 0) in_init(&r, in, 0);
    else {
      char *native = path_to_native(name);
      OsStat st;
      int isdir = os_stat(native, &st) == 0 && st.is_dir;
      free(native);
      if (isdir) {
        tool_err(err, "strings", "Warning: '%s' is a directory", name);
        status = 1;
        continue;
      }
      if (in_open(&r, name, in) != 0) {
        tool_err(err, "strings", "'%s': No such file", name);
        status = 1;
        continue;
      }
    }
    cur.len = 0;
    while ((got = in_read(&r, (char *)chunk, sizeof(chunk))) > 0 && !o.failed) {
      long k;
      for (k = 0; k < got; k++, off++) {
        int ch = chunk[k];
        int ok = ch == '\t' || (ch >= 32 && ch < 127) || (eight && ch >= 128) ||
                 (allws && (ch == '\n' || ch == '\r' || ch == '\v' || ch == '\f'));
        if (!ok) {
          if (printing) out_puts(&o, sep);
          printing = 0;
          cur.len = 0;
          continue;
        }
        if (printing) {
          out_putc(&o, ch);
          continue;
        }
        if (cur.len == 0) start = off;
        buf_putc(&cur, (char)ch);
        if ((long long)cur.len >= minlen) {	/* long enough: out it goes */
          if (names) out_printf(&o, "%s: ", strcmp(name, "-") == 0 ? "{standard input}" : name);
          if (radix == 'o') out_printf(&o, "%7llo ", start);
          else if (radix == 'd') out_printf(&o, "%7lld ", start);
          else if (radix == 'x') out_printf(&o, "%7llx ", start);
          out_putn(&o, cur.s, cur.len);
          cur.len = 0;
          printing = 1;
        }
      }
    }
    if (printing) out_puts(&o, sep);
    in_close(&r);
  }
  out_flush(&o);
  buf_free(&cur);
  opts_free(&g);
  free(v);
  return status;
bad:
  opts_free(&g);
  free(v);
  return 1;
}

/* }================================================================== */


/*
** {==================================================================
** cmp
** ===================================================================
*/

/* cat -v style, as cmp -b shows a byte */
static void cmp_char (char *buf, unsigned c) {
  char *p = buf;
  if (!(c >= 32 && c < 127)) {
    if (c >= 128) {
      *p++ = 'M';
      *p++ = '-';
      c -= 128;
    }
    if (c < 32) {
      *p++ = '^';
      c += 64;
    }
    else if (c == 127) {
      *p++ = '^';
      c = '?';
    }
  }
  *p++ = (char)c;
  *p = '\0';
}

/* SKIP for -i and the operands: 10, 1K, 2MB, 0x10 */
static int cmp_skip (const char *s, long long *v) {
  return od_num(s, v);
}


int t_cmp (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {{"print-bytes", 'b', 0}, {"ignore-initial", 'i', 1},
    {"verbose", 'l', 0}, {"bytes", 'n', 1}, {"silent", 's', 0}, {"quiet", 's', 0},
    {NULL, 0, 0}};
  Opts g;
  Out o;
  int c, bytes = 0, mode = 0, k, status = 0, lflag = 0, sflag = 0, last = -1;	/* mode: 0 first difference, 'l' all, 's' status */
  long long limit = -1, skip[2] = {0, 0}, pos, line = 1;
  const char *name[2];
  In r[2];
  unsigned char b0[65536], b1[65536];
  size_t n0 = 0, n1 = 0, p0 = 0, p1 = 0;
  int eof0 = 0, eof1 = 0, width = 1;
  opts_init(&g, "cmp", argc, argv, err);
  while ((c = opts_next(&g, "bi:ln:s", lo)) != 0) {
    switch (c) {
      case 'b': bytes = 1; break;
      case 'i': {
        const char *colon = strchr(g.arg, ':');
        char tmp[64];
        if (colon) {
          snprintf(tmp, sizeof(tmp), "%.*s", (int)(colon - g.arg), g.arg);
          if (cmp_skip(tmp, &skip[0]) || cmp_skip(colon + 1, &skip[1])) goto badskip;
        }
        else {
          if (cmp_skip(g.arg, &skip[0])) goto badskip;
          skip[1] = skip[0];
        }
        break;
      badskip:
        tool_err(err, "cmp", "invalid --ignore-initial value '%s'", g.arg);
        fd_printf(err, "cmp: Try 'cmp --help' for more information.\n");
        opts_free(&g);
        return 2;
      }
      case 'l': mode = 'l'; lflag = 1; break;
      case 'n':
        if (od_num(g.arg, &limit) != 0) {
          tool_err(err, "cmp", "invalid --bytes value '%s'", g.arg);
          fd_printf(err, "cmp: Try 'cmp --help' for more information.\n");
          opts_free(&g);
          return 2;
        }
        break;
      case 's': mode = 's'; sflag = 1; break;
      case OPT_HELP: opts_free(&g); return tool_help(out, "cmp");
      default:
        opts_free(&g);
        return 2;
    }
  }
  if (lflag && sflag) {
    tool_err(err, "cmp", "options -l and -s are incompatible");
    fd_printf(err, "cmp: Try 'cmp --help' for more information.\n");
    opts_free(&g);
    return 2;
  }
  if (g.ops.n == 0) {
    tool_err(err, "cmp", "missing operand after 'cmp'");
    fd_printf(err, "cmp: Try 'cmp --help' for more information.\n");
    opts_free(&g);
    return 2;
  }
  for (k = 2; k < (int)g.ops.n && k < 4; k++)
    if (cmp_skip(g.ops.v[k], &skip[k - 2])) {
      tool_err(err, "cmp", "invalid --ignore-initial value '%s'", g.ops.v[k]);
      fd_printf(err, "cmp: Try 'cmp --help' for more information.\n");
      opts_free(&g);
      return 2;
    }
  if (g.ops.n > 4) {
    tool_err(err, "cmp", "extra operand '%s'", g.ops.v[4]);
    fd_printf(err, "cmp: Try 'cmp --help' for more information.\n");
    opts_free(&g);
    return 2;
  }
  name[0] = g.ops.v[0];
  name[1] = g.ops.n > 1 ? g.ops.v[1] : "-";
  for (k = 0; k < 2; k++) {
    if (m_open(&r[k], "cmp", name[k], in, err, OPEN_PLAIN, NULL) != 0) {
      if (k == 1) in_close(&r[0]);
      opts_free(&g);
      return 2;
    }
  }
  if (strcmp(name[0], name[1]) == 0 && skip[0] == skip[1] && strcmp(name[0], "-") != 0) {
    in_close(&r[0]);	/* the same file */
    in_close(&r[1]);
    opts_free(&g);
    return 0;
  }
  /* skip, and for -l the width of the biggest offset */
  {
    long long maxb = limit >= 0 ? limit : 9223372036854775807LL;
    for (k = 0; k < 2; k++) {
      long long here = os_seek(r[k].fd, 0, 1), end = here >= 0 ? os_seek(r[k].fd, 0, 2) : -1;
      if (end >= 0) {
        long long to = here + skip[k] < end ? here + skip[k] : end;
        os_seek(r[k].fd, to, 0);
        if (end - to < maxb) maxb = end - to;
      }
      else {
        long long left = skip[k];
        while (left > 0) {
          long n = in_read(&r[k], (char *)b0, left < (long long)sizeof(b0) ? (size_t)left : sizeof(b0));
          if (n <= 0) break;
          left -= n;
        }
      }
    }
    while ((maxb /= 10) != 0) width++;
  }
  out_init(&o, out);
  for (pos = 0; limit < 0 || pos < limit; pos++) {
    int a, b;
    if (p0 == n0 && !eof0) {
      long n = in_read(&r[0], (char *)b0, sizeof(b0));
      if (n <= 0) eof0 = 1;
      else { n0 = (size_t)n; p0 = 0; }
    }
    if (p1 == n1 && !eof1) {
      long n = in_read(&r[1], (char *)b1, sizeof(b1));
      if (n <= 0) eof1 = 1;
      else { n1 = (size_t)n; p1 = 0; }
    }
    a = p0 < n0 ? b0[p0] : -1;
    b = p1 < n1 ? b1[p1] : -1;
    if (a < 0 || b < 0) {
      if (a < 0 && b < 0) break;	/* both ended together */
      if (mode != 's') {	/* one file is shorter */
        int f = a < 0 ? 0 : 1;
        out_flush(&o);
        if (pos == 0) fd_printf(err, "cmp: EOF on '%s' which is empty\n", name[f]);
        else if (mode == 'l') fd_printf(err, "cmp: EOF on '%s' after byte %lld\n", name[f], pos);
        else if (last == '\n') fd_printf(err, "cmp: EOF on '%s' after byte %lld, line %lld\n", name[f], pos, line - 1);
        else fd_printf(err, "cmp: EOF on '%s' after byte %lld, in line %lld\n", name[f], pos, line);
      }
      status = 1;
      break;
    }
    p0++;
    p1++;
    if (a != b) {
      status = 1;
      if (mode == 's') break;
      if (mode == 0) {
        if (bytes) {
          char ca[8], cb[8];
          cmp_char(ca, (unsigned)a);
          cmp_char(cb, (unsigned)b);
          out_printf(&o, "%s %s differ: byte %lld, line %lld is %3o %s %3o %s\n", name[0], name[1],
                     pos + 1, line, a, ca, b, cb);
        }
        else out_printf(&o, "%s %s differ: char %lld, line %lld\n", name[0], name[1], pos + 1, line);
        break;
      }
      if (bytes) {
        char ca[8], cb[8];
        cmp_char(ca, (unsigned)a);
        cmp_char(cb, (unsigned)b);
        out_printf(&o, "%*lld %3o %-4s %3o %s\n", width, pos + 1, a, ca, b, cb);
      }
      else out_printf(&o, "%*lld %3o %3o\n", width, pos + 1, a, b);
    }
    if (a == '\n') line++;
    last = a;
    if (o.failed || tool_stop()) break;
  }
  out_flush(&o);
  in_close(&r[0]);
  in_close(&r[1]);
  opts_free(&g);
  return status;
}

/* }================================================================== */


/*
** {==================================================================
** fmt: GNU's paragraph filler (the costs and the breaking are its own)
** ===================================================================
*/

#define FMT_MAXWORDS	1000
#define FMT_MAXCHARS	5000
#define FMT_TABWIDTH	8
#define FMT_DEF_INDENT	3
#define FMT_LEEWAY	7

typedef long long FCost;
#define FMT_EQUIV(n)	((FCost)(n) * (FCost)(n))
#define FMT_SHORT_COST(n)	FMT_EQUIV((n) * 10)
#define FMT_RAGGED_COST(n)	(FMT_SHORT_COST(n) / 2)
#define FMT_LINE_COST	FMT_EQUIV(70)
#define FMT_WIDOW_COST(n)	(FMT_EQUIV(200) / ((n) + 2))
#define FMT_ORPHAN_COST(n)	(FMT_EQUIV(150) / ((n) + 2))
#define FMT_SENTENCE_BONUS	FMT_EQUIV(50)
#define FMT_NOBREAK_COST	FMT_EQUIV(600)
#define FMT_PAREN_BONUS	FMT_EQUIV(40)
#define FMT_PUNCT_BONUS	FMT_EQUIV(40)
#define FMT_LINE_CREDIT	FMT_EQUIV(3)
#define FMT_MAXCOST	LLONG_MAX

typedef struct FWord {
  size_t text;	/* offset in parabuf */
  int length, space, line_length;
  unsigned paren:1, period:1, punct:1, final:1;
  FCost best_cost;
  int next_break;	/* index of a word */
} FWord;

typedef struct Fmt {
  int split, uniform, crown, tagged;
  int max_width, goal_width;
  const char *prefix;
  int prefix_full_length, prefix_lead_space, prefix_length;
  int tabs, prefix_indent, first_indent, other_indent;
  int next_char, next_prefix_indent, last_line_length, in_column, out_column;
  char parabuf[FMT_MAXCHARS];
  size_t wptr;
  FWord word[FMT_MAXWORDS];
  int word_limit;
  CStream *cs;
  Out *o;
} Fmt;

#define FGETC(F)	cs_getc((F)->cs)

static int fmt_space (Fmt *F, int c) {	/* GNU's get_space */
  for (;;) {
    if (c == ' ') F->in_column++;
    else if (c == '\t') {
      F->tabs = 1;
      F->in_column = (F->in_column / FMT_TABWIDTH + 1) * FMT_TABWIDTH;
    }
    else return c;
    c = FGETC(F);
  }
}

static int fmt_prefix (Fmt *F) {	/* GNU's get_prefix */
  int c;
  F->in_column = 0;
  c = fmt_space(F, FGETC(F));
  if (F->prefix_length == 0)
    F->next_prefix_indent = F->prefix_lead_space < F->in_column ? F->prefix_lead_space : F->in_column;
  else {
    const char *p;
    F->next_prefix_indent = F->in_column;
    for (p = F->prefix; *p != '\0'; p++) {
      if (c != (unsigned char)*p) return c;
      F->in_column++;
      c = FGETC(F);
    }
    c = fmt_space(F, c);
  }
  return c;
}

static void fmt_put_space (Fmt *F, int space) {
  int space_target = F->out_column + space;
  if (F->tabs) {
    int tab_target = space_target / FMT_TABWIDTH * FMT_TABWIDTH;
    if (F->out_column + 1 < tab_target)
      while (F->out_column < tab_target) {
        out_putc(F->o, '\t');
        F->out_column = (F->out_column / FMT_TABWIDTH + 1) * FMT_TABWIDTH;
      }
  }
  while (F->out_column < space_target) {
    out_putc(F->o, ' ');
    F->out_column++;
  }
}

/* a line that has no prefix or no words: copied */
static int fmt_copy_rest (Fmt *F, int c) {
  const char *s;
  F->out_column = 0;
  if (F->in_column > F->next_prefix_indent || (c != '\n' && c != -1)) {
    fmt_put_space(F, F->next_prefix_indent);
    for (s = F->prefix; F->out_column != F->in_column && *s; F->out_column++) out_putc(F->o, *s++);
    if (c != -1 && c != '\n') fmt_put_space(F, F->in_column - F->out_column);
    if (c == -1 && F->in_column >= F->next_prefix_indent + F->prefix_length) out_putc(F->o, '\n');
  }
  while (c != '\n' && c != -1) {
    out_putc(F->o, c);
    c = FGETC(F);
  }
  return c;
}

static int fmt_same_para (Fmt *F, int c) {
  return F->next_prefix_indent == F->prefix_indent &&
         F->in_column >= F->next_prefix_indent + F->prefix_full_length && c != '\n' && c != -1;
}

static void fmt_other_indent (Fmt *F, int same) {
  if (F->split) F->other_indent = F->first_indent;
  else if (F->crown) F->other_indent = same ? F->in_column : F->first_indent;
  else if (F->tagged) {
    if (same && F->in_column != F->first_indent) F->other_indent = F->in_column;
    else if (F->other_indent == F->first_indent)
      F->other_indent = F->first_indent == 0 ? FMT_DEF_INDENT : 0;
  }
  else F->other_indent = F->first_indent;
}

/* GNU uses strchr() here, so a NUL byte counts as all of them */
static void fmt_check_punct (Fmt *F, FWord *w) {
  const char *start = F->parabuf + w->text, *finish = start + (w->length - 1);
  unsigned char fin = (unsigned char)*finish;
  w->paren = strchr("(['`\"", *start) != NULL;
  w->punct = ispunct(fin) != 0;
  while (start < finish && strchr(")]'\"", *finish) != NULL) finish--;
  w->period = strchr(".?!", *finish) != NULL;
}

static FCost fmt_base_cost (Fmt *F, int t) {
  FCost cost = FMT_LINE_COST;
  FWord *w = F->word;
  if (t > 0) {
    if (w[t - 1].period) {
      if (w[t - 1].final) cost -= FMT_SENTENCE_BONUS;
      else cost += FMT_NOBREAK_COST;
    }
    else if (w[t - 1].punct) cost -= FMT_PUNCT_BONUS;
    else if (t > 1 && w[t - 2].final) cost += FMT_WIDOW_COST(w[t - 1].length);
  }
  if (w[t].paren) cost -= FMT_PAREN_BONUS;
  else if (w[t].final) cost += FMT_ORPHAN_COST(w[t].length);
  return cost;
}

static FCost fmt_line_cost (Fmt *F, int next, int len) {
  int n;
  FCost cost;
  if (next == F->word_limit) return 0;
  n = F->goal_width - len;
  cost = FMT_SHORT_COST(n);
  if (F->word[next].next_break != F->word_limit) {
    n = len - F->word[next].line_length;
    cost += FMT_RAGGED_COST(n);
  }
  return cost;
}

static void fmt_paragraph (Fmt *F) {	/* the best breaks, from the end */
  FWord *word = F->word;
  int start, w, len, saved_length;
  FCost wcost, best;
  word[F->word_limit].best_cost = 0;
  saved_length = word[F->word_limit].length;
  word[F->word_limit].length = F->max_width;	/* sentinel */
  for (start = F->word_limit - 1; start >= 0; start--) {
    best = FMT_MAXCOST;
    len = start == 0 ? F->first_indent : F->other_indent;
    w = start;
    len += word[w].length;
    do {
      w++;
      wcost = fmt_line_cost(F, w, len) + word[w].best_cost;
      if (start == 0 && F->last_line_length > 0)	/* after a flush: like the line before */
        wcost += FMT_RAGGED_COST(len - F->last_line_length);
      if (wcost < best) {
        best = wcost;
        word[start].next_break = w;
        word[start].line_length = len;
      }
      if (w == F->word_limit) break;
      len += word[w - 1].space + word[w].length;
    } while (len < F->max_width);
    word[start].best_cost = best + fmt_base_cost(F, start);
  }
  word[F->word_limit].length = saved_length;
}

static void fmt_put_line (Fmt *F, int w, int indent) {
  int endline;
  F->out_column = 0;
  fmt_put_space(F, F->prefix_indent);
  out_puts(F->o, F->prefix);
  F->out_column += F->prefix_length;
  fmt_put_space(F, indent - F->out_column);
  endline = F->word[w].next_break - 1;
  for (; w != endline; w++) {
    out_putn(F->o, F->parabuf + F->word[w].text, (size_t)F->word[w].length);
    F->out_column += F->word[w].length;
    fmt_put_space(F, F->word[w].space);
  }
  out_putn(F->o, F->parabuf + F->word[w].text, (size_t)F->word[w].length);
  F->out_column += F->word[w].length;
  F->last_line_length = F->out_column;
  out_putc(F->o, '\n');
}

static void fmt_put_paragraph (Fmt *F, int finish) {
  int w;
  fmt_put_line(F, 0, F->first_indent);
  for (w = F->word[0].next_break; w != finish; w = F->word[w].next_break) fmt_put_line(F, w, F->other_indent);
}

/* the buffers are full: print most of the paragraph, keep the rest */
static void fmt_flush (Fmt *F) {
  int split_point, w, k;
  FCost best_break;
  size_t shift;
  if (F->word_limit == 0) {
    out_putn(F->o, F->parabuf, F->wptr);
    F->wptr = 0;
    return;
  }
  fmt_paragraph(F);
  split_point = F->word_limit;
  best_break = FMT_MAXCOST;
  for (w = F->word[0].next_break; w != F->word_limit; w = F->word[w].next_break) {
    FCost d = F->word[w].best_cost - F->word[F->word[w].next_break].best_cost;
    if (d < best_break) {
      split_point = w;
      best_break = d;
    }
    if (best_break <= FMT_MAXCOST - FMT_LINE_CREDIT) best_break += FMT_LINE_CREDIT;
  }
  fmt_put_paragraph(F, split_point);
  shift = F->word[split_point].text;
  memmove(F->parabuf, F->parabuf + shift, F->wptr - shift);
  F->wptr -= shift;
  for (w = split_point; w <= F->word_limit; w++) F->word[w].text -= shift;
  for (k = 0; k <= F->word_limit - split_point; k++) {
    F->word[k] = F->word[split_point + k];
    F->word[k].next_break -= split_point;
  }
  F->word_limit -= split_point;
}

static int fmt_get_line (Fmt *F, int c) {	/* the words of a line */
  do {
    FWord *w = &F->word[F->word_limit];
    int start;
    w->text = F->wptr;
    do {
      if (F->wptr == FMT_MAXCHARS) {
        fmt_other_indent(F, 1);
        fmt_flush(F);
        w = &F->word[F->word_limit];
      }
      F->parabuf[F->wptr++] = (char)c;
      c = FGETC(F);
    } while (c != -1 && !isspace(c));
    w->length = (int)(F->wptr - w->text);
    F->in_column += w->length;
    fmt_check_punct(F, w);
    start = F->in_column;
    c = fmt_space(F, c);
    w->space = F->in_column - start;
    w->final = c == -1 || (w->period && (c == '\n' || w->space > 1));
    if (c == '\n' || c == -1 || F->uniform) w->space = w->final ? 2 : 1;
    if (F->word_limit == FMT_MAXWORDS - 2) {
      fmt_other_indent(F, 1);
      fmt_flush(F);
    }
    F->word_limit++;
  } while (c != '\n' && c != -1);
  return fmt_prefix(F);
}

static int fmt_get_paragraph (Fmt *F) {
  int c;
  F->last_line_length = 0;
  c = F->next_char;
  /* blank lines and lines without the prefix are copied */
  while (c == '\n' || c == -1 || F->next_prefix_indent < F->prefix_lead_space ||
         F->in_column < F->next_prefix_indent + F->prefix_full_length) {
    c = fmt_copy_rest(F, c);
    if (c == -1) {
      F->next_char = -1;
      return 0;
    }
    out_putc(F->o, '\n');
    c = fmt_prefix(F);
  }
  F->prefix_indent = F->next_prefix_indent;
  F->first_indent = F->in_column;
  F->wptr = 0;
  F->word_limit = 0;
  c = fmt_get_line(F, c);
  fmt_other_indent(F, fmt_same_para(F, c));
  if (F->split) {
    /* one line a paragraph */
  }
  else if (F->crown) {
    if (fmt_same_para(F, c)) {
      do c = fmt_get_line(F, c);
      while (fmt_same_para(F, c) && F->in_column == F->other_indent);
    }
  }
  else if (F->tagged) {
    if (fmt_same_para(F, c) && F->in_column != F->first_indent) {
      do c = fmt_get_line(F, c);
      while (fmt_same_para(F, c) && F->in_column == F->other_indent);
    }
  }
  else {
    while (fmt_same_para(F, c) && F->in_column == F->other_indent) c = fmt_get_line(F, c);
  }
  F->word[F->word_limit - 1].period = 1;
  F->word[F->word_limit - 1].final = 1;
  F->next_char = c;
  return 1;
}


int t_fmt (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {{"crown-margin", 'c', 0}, {"prefix", 'p', 1}, {"split-only", 's', 0},
    {"tagged-paragraph", 't', 0}, {"uniform-spacing", 'u', 0}, {"width", 'w', 1}, {"goal", 'g', 1},
    {NULL, 0, 0}};
  Opts g;
  Out o;
  Fmt *F;
  int c, nargc, status = 0, goal_given = 0, width_given = 0;
  long long v;
  size_t i;
  char **av = legacy_num(argc, argv, &nargc, "-w");
  char *prefix = xstrdup("");
  F = (Fmt *)xmalloc(sizeof(Fmt));
  memset(F, 0, sizeof(*F));
  F->max_width = 75;
  opts_init(&g, "fmt", nargc, av, err);
  while ((c = opts_next(&g, "cp:stuw:g:", lo)) != 0) {
    switch (c) {
      case 'c': F->crown = 1; break;
      case 's': F->split = 1; break;
      case 't': F->tagged = 1; break;
      case 'u': F->uniform = 1; break;
      case 'p':
        free(prefix);
        prefix = xstrdup(g.arg);
        break;
      case 'w':
        if (num_arg(g.arg, 0, &v) != 0 || v > 2500) {
          tool_err(err, "fmt", "invalid width: '%s'", g.arg);
          goto bad;
        }
        F->max_width = (int)v;
        width_given = 1;
        break;
      case 'g':
        if (num_arg(g.arg, 0, &v) != 0 || v > 2500) {
          tool_err(err, "fmt", "invalid width: '%s'", g.arg);
          goto bad;
        }
        F->goal_width = (int)v;
        goal_given = 1;
        break;
      case OPT_HELP: opts_free(&g); free(av); free(prefix); free(F); return tool_help(out, "fmt");
      default: goto bad;
    }
  }
  if (goal_given) {
    if (!width_given) F->max_width = F->goal_width + 10;
    if (F->goal_width > F->max_width) {
      tool_err(err, "fmt", "invalid width: '%d'", F->goal_width);	/* GNU: goal > width */
      goto bad;
    }
  }
  else F->goal_width = (int)((long long)F->max_width * (2 * (100 - FMT_LEEWAY) + 1) / 200);
  {	/* the prefix: its leading blanks counted, trailing ones dropped */
    char *p = prefix, *s;
    while (*p == ' ') {
      F->prefix_lead_space++;
      p++;
    }
    F->prefix = p;
    F->prefix_full_length = (int)strlen(p);
    s = p + F->prefix_full_length;
    while (s > p && s[-1] == ' ') s--;
    *s = '\0';
    F->prefix_length = (int)(s - p);
  }
  if (g.ops.n == 0) vec_push(&g.ops, xstrdup("-"));
  out_init(&o, out);
  F->o = &o;
  for (i = 0; i < g.ops.n && !o.failed && !tool_stop(); i++) {
    Vec one;
    CStream *cs = (CStream *)xmalloc(sizeof(CStream));
    vec_init(&one);
    vec_push(&one, xstrdup(g.ops.v[i]));
    cs_init(cs, "fmt", &one, in, err);
    cs->how = OPEN_CANNOT;
    F->cs = cs;
    F->tabs = 0;
    F->other_indent = 0;
    F->next_char = fmt_prefix(F);
    if (!cs->status) {
      while (fmt_get_paragraph(F) && !o.failed && !tool_stop()) {
        fmt_paragraph(F);
        fmt_put_paragraph(F, F->word_limit);
      }
    }
    cs_close(cs);
    if (cs->status) status = 1;
    free(cs);
    vec_free(&one);
  }
  out_flush(&o);
  opts_free(&g);
  free(av);
  free(prefix);
  free(F);
  return status;
bad:
  opts_free(&g);
  free(av);
  free(prefix);
  free(F);
  return 1;
}

/* }================================================================== */
