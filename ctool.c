/*
** ctool.c - what every tool uses: buffered input and output, GNU style
** options, messages, sizes. And the smallest tools: basename, dirname,
** seq, sleep.
**
** The tools (c*.c) are fallbacks: they run only when no program of that
** name is found in PATH, so on Linux and macOS the system's own grep or
** cp still answers, and a Windows PC without git-bash gets these. They
** behave like the GNU ones: same options, same messages, same output.
** They write to the descriptors they are given, check tool_stop() in
** their loops (Ctrl-C) and never exit the shell.
*/

#include "mmc.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*
** {==================================================================
** Output
** ===================================================================
*/

void out_init (Out *o, int fd) {
  o->fd = fd;
  o->failed = 0;
  o->n = 0;
  o->line = os_is_tty(fd);
}


int out_flush (Out *o) {
  if (o->n > 0 && !o->failed && os_write(o->fd, o->buf, o->n) < 0) o->failed = 1;
  o->n = 0;
  return o->failed ? -1 : 0;
}


void out_putn (Out *o, const char *s, size_t n) {
  if (o->failed) return;
  if (o->n + n > sizeof(o->buf)) {
    out_flush(o);
    if (n >= sizeof(o->buf)) {	/* big: straight through */
      if (!o->failed && os_write(o->fd, s, n) < 0) o->failed = 1;
      return;
    }
  }
  memcpy(o->buf + o->n, s, n);
  o->n += n;
  if (o->line && memchr(s, '\n', n) != NULL) out_flush(o);
}


void out_puts (Out *o, const char *s) {
  out_putn(o, s, strlen(s));
}


void out_putc (Out *o, int c) {
  char ch = (char)c;
  if (o->n < sizeof(o->buf) && !o->failed) {
    o->buf[o->n++] = ch;
    if (o->line && ch == '\n') out_flush(o);
  }
  else out_putn(o, &ch, 1);
}


void out_printf (Out *o, const char *fmt, ...) {
  char small[512];
  va_list ap;
  int n;
  va_start(ap, fmt);
  n = vsnprintf(small, sizeof(small), fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if ((size_t)n < sizeof(small)) out_putn(o, small, (size_t)n);
  else {
    char *big = (char *)xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    out_putn(o, big, (size_t)n);
    free(big);
  }
}

/* }================================================================== */


/*
** {==================================================================
** Input
** ===================================================================
*/

void in_init (In *r, int fd, int own) {
  r->fd = fd;
  r->own = own;
  r->eof = 0;
  r->cap = 65536;
  r->buf = (char *)xmalloc(r->cap);
  r->start = r->end = 0;
}


int in_open (In *r, const char *arg, int stdin_fd) {
  char *native;
  int fd;
  if (arg == NULL || strcmp(arg, "-") == 0) {
    in_init(r, stdin_fd, 0);
    return 0;
  }
  native = path_to_native(arg);
  fd = os_open(native, OS_READ);
  if (fd >= 0) {	/* a folder opens on some systems, but reads fail */
    OsStat st;
    if (os_stat(native, &st) == 0 && st.is_dir) {
      os_close(fd);
      fd = -1;
    }
  }
  free(native);
  if (fd < 0) return -1;
  in_init(r, fd, 1);
  return 0;
}


void in_close (In *r) {
  if (r->own && r->fd >= 0) os_close(r->fd);
  free(r->buf);
  r->buf = NULL;
  r->fd = -1;
}


/* more data after what is buffered; 0 at the end */
static int in_fill (In *r) {
  long n;
  if (r->eof) return 0;
  if (r->start > 0) {
    memmove(r->buf, r->buf + r->start, r->end - r->start);
    r->end -= r->start;
    r->start = 0;
  }
  if (r->end + 1 >= r->cap) {
    r->cap *= 2;
    r->buf = (char *)xrealloc(r->buf, r->cap);
  }
  n = os_read(r->fd, r->buf + r->end, r->cap - r->end - 1);
  if (n <= 0) {
    r->eof = 1;
    return 0;
  }
  r->end += (size_t)n;
  return 1;
}


int in_line (In *r, char **line, size_t *len, int delim, int *had_delim) {
  size_t scanned = 0;
  for (;;) {
    char *p = (char *)memchr(r->buf + r->start + scanned, delim, r->end - r->start - scanned);
    if (p != NULL) {
      *line = r->buf + r->start;
      *len = (size_t)(p - *line);
      *p = '\0';
      r->start += *len + 1;
      if (had_delim) *had_delim = 1;
      return 1;
    }
    scanned = r->end - r->start;
    if (tool_stop() || !in_fill(r)) break;
  }
  if (r->end == r->start) return 0;
  *line = r->buf + r->start;	/* the last line, without its delimiter */
  *len = r->end - r->start;
  r->buf[r->end] = '\0';
  r->start = r->end;
  if (had_delim) *had_delim = 0;
  return 1;
}


long in_read (In *r, char *dst, size_t n) {
  if (r->end > r->start) {
    size_t k = r->end - r->start;
    if (k > n) k = n;
    memcpy(dst, r->buf + r->start, k);
    r->start += k;
    return (long)k;
  }
  if (r->eof) return 0;
  {
    long got = os_read(r->fd, dst, n);
    if (got <= 0) r->eof = 1;
    return got < 0 ? 0 : got;
  }
}

/* }================================================================== */


/*
** {==================================================================
** Options
** ===================================================================
*/

void opts_init (Opts *g, const char *tool, int argc, char **argv, int err) {
  g->tool = tool;
  g->argc = argc;
  g->argv = argv;
  g->i = 1;
  g->err = err;
  g->no_permute = 0;
  g->cluster = NULL;
  g->arg = NULL;
  vec_init(&g->ops);
}


void opts_free (Opts *g) {
  vec_free(&g->ops);
}


static int opts_bad (Opts *g) {
  fd_printf(g->err, "Try '%s --help' for more information.\n", g->tool);
  return OPT_BAD;
}


int opts_next (Opts *g, const char *spec, const LongOpt *lo) {
  g->arg = NULL;
  for (;;) {
    const char *a;
    if (g->cluster != NULL && *g->cluster != '\0') {
      char c = *g->cluster++;
      const char *s = (c != ':') ? strchr(spec, c) : NULL;
      if (s == NULL) {
        tool_err(g->err, g->tool, "invalid option -- '%c'", c);
        return opts_bad(g);
      }
      if (s[1] == ':') {
        if (*g->cluster != '\0') g->arg = g->cluster;
        else if (g->i < g->argc) g->arg = g->argv[g->i++];
        else {
          tool_err(g->err, g->tool, "option requires an argument -- '%c'", c);
          return opts_bad(g);
        }
        g->cluster = NULL;
      }
      return (unsigned char)c;
    }
    g->cluster = NULL;
    if (g->i >= g->argc) return 0;
    a = g->argv[g->i];
    if (strcmp(a, "--") == 0) {
      for (g->i++; g->i < g->argc; g->i++) vec_push(&g->ops, xstrdup(g->argv[g->i]));
      return 0;
    }
    if (a[0] == '-' && a[1] == '-') {	/* --name, --name=value */
      const char *name = a + 2, *eq = strchr(name, '=');
      size_t n = eq ? (size_t)(eq - name) : strlen(name);
      const LongOpt *hit = NULL, *o;
      int count = 0;
      g->i++;
      for (o = lo; o && o->name; o++) {
        if (strncmp(o->name, name, n) != 0) continue;
        if (o->name[n] == '\0') {	/* exact */
          hit = o;
          count = 1;
          break;
        }
        if (hit == NULL || hit->key != o->key) count++;
        hit = o;
      }
      if (hit == NULL && strncmp("help", name, n) == 0 && n > 0 && eq == NULL) return OPT_HELP;
      if (hit == NULL) {
        tool_err(g->err, g->tool, "unrecognized option '%s'", a);
        return opts_bad(g);
      }
      if (count > 1) {
        tool_err(g->err, g->tool, "option '%.*s' is ambiguous", (int)(n + 2), a);
        return opts_bad(g);
      }
      if (hit->arg == 0 && eq != NULL) {
        tool_err(g->err, g->tool, "option '--%s' doesn't allow an argument", hit->name);
        return opts_bad(g);
      }
      if (eq != NULL) g->arg = eq + 1;
      else if (hit->arg == 1) {
        if (g->i >= g->argc) {
          tool_err(g->err, g->tool, "option '--%s' requires an argument", hit->name);
          return opts_bad(g);
        }
        g->arg = g->argv[g->i++];
      }
      return hit->key;
    }
    if (a[0] == '-' && a[1] != '\0') {
      g->cluster = a + 1;
      g->i++;
      continue;
    }
    /* an operand */
    if (g->no_permute) {
      for (; g->i < g->argc; g->i++) vec_push(&g->ops, xstrdup(g->argv[g->i]));
      return 0;
    }
    vec_push(&g->ops, xstrdup(a));
    g->i++;
  }
}

/* }================================================================== */


/*
** {==================================================================
** Messages and small helpers
** ===================================================================
*/

void tool_err (int err, const char *tool, const char *fmt, ...) {
  char msg[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  fd_printf(err, "%s: %s\n", tool, msg);
}


int tool_help (int out, const char *tool) {
  const Builtin *b = builtin_find(tool, 1);
  if (b != NULL && b->help) fd_printf(out, "usage: %s\n", b->help);
  fd_printf(out, "(mmc's own %s: the GNU options people use most)\n", tool);
  return 0;
}


/* is the text UTF-8? Yes, unless LC_ALL / LC_CTYPE / LANG say C or POSIX */
int tool_utf8 (void) {
  const char *v = var_get("LC_ALL");
  if (v == NULL || !*v) v = var_get("LC_CTYPE");
  if (v == NULL || !*v) v = var_get("LANG");
  if (v == NULL || !*v) return 1;
  return !(strcmp(v, "C") == 0 || strcmp(v, "POSIX") == 0);
}


int tool_stop (void) {
  return os_interrupted;
}


int tool_ask (int in, int err, const char *fmt, ...) {
  char msg[2048], c;
  va_list ap;
  int yes = -1;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  fd_puts(err, msg);
  for (;;) {	/* the first letter of the line says it */
    long n = os_read(in, &c, 1);
    if (n <= 0) break;
    if (c == '\n') break;
    if (yes < 0 && c != ' ' && c != '\t' && c != '\r') yes = (c == 'y' || c == 'Y');
  }
  return yes > 0;
}


char *tool_join (const char *dir, const char *name) {
  size_t n = strlen(dir);
  if (n == 0) return xstrdup(name);
  if (dir[n - 1] == '/' || dir[n - 1] == '\\') return xstrcat3(dir, name, "");
  return xstrcat3(dir, "/", name);
}


const char *tool_base (const char *path) {
  const char *b = path, *p;
  for (p = path; *p; p++)
    if ((*p == '/' || *p == '\\') && p[1] != '\0' && p[1] != '/' && p[1] != '\\') b = p + 1;
  return b;
}


/* a pattern as the user typed it (\x is a literal x) -> marked for pat_match */
char *glob_mark (const char *pat) {
  Buf b;
  buf_init(&b);
  for (; *pat; pat++) {
    if (*pat == '\\' && pat[1] != '\0') {
      buf_putc(&b, QMARK);
      buf_putc(&b, *++pat);
    }
    else buf_putc(&b, *pat);
  }
  return buf_take(&b);
}


int is_dot_or_dotdot (const char *name) {
  return name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}


/* like GNU -h: 1023, 1.0K, 1.5K, 10K, 2.3M; rounded up */
void human_size (char *out, unsigned long long v, int si) {
  static const char units[] = "KMGTPE";
  double base = si ? 1000.0 : 1024.0, x = (double)v;
  int u = -1;
  if (x < base) {
    sprintf(out, "%llu", v);
    return;
  }
  while (x >= base && u < 5) {
    x /= base;
    u++;
  }
  if (x < 10.0) {
    double r = (double)(long long)(x * 10.0);
    if (r < x * 10.0) r += 1.0;
    if (r >= 100.0) sprintf(out, "%d%c", 10, si && u == 0 ? 'k' : units[u]);
    else sprintf(out, "%.1f%c", r / 10.0, si && u == 0 ? 'k' : units[u]);
  }
  else {
    long long r = (long long)x;
    if ((double)r < x) r++;
    if (r >= (long long)base && u < 5) sprintf(out, "1.0%c", units[u + 1]);
    else sprintf(out, "%lld%c", r, si && u == 0 ? 'k' : units[u]);
  }
}


int parse_size (const char *s, long long *out) {
  char *end;
  long long v, mul = 1;
  if (!isdigit((unsigned char)*s)) return -1;
  v = strtoll(s, &end, 10);
  if (*end) {
    const char *sfx = "KMGTPE", *p;
    char c = (char)toupper((unsigned char)*end);
    if (c == 'B' && end[1] == '\0') mul = 512;
    else if ((p = strchr(sfx, c)) != NULL) {
      int k, pow = (int)(p - sfx) + 1;
      long long base = 1024;
      if (end[1] == 'B' && end[2] == '\0') base = 1000;	/* KB MB: powers of 1000 */
      else if (end[1] == 'i' && end[2] == 'B' && end[3] == '\0') base = 1024;
      else if (end[1] != '\0') return -1;
      for (k = 0; k < pow; k++) mul *= base;
    }
    else return -1;
  }
  *out = v * mul;
  return 0;
}


int tool_utf8_cols (const char *s, size_t n) {
  size_t i = 0;
  int cols = 0;
  while (i < n) {
    unsigned char c = (unsigned char)s[i];
    unsigned long cp;
    int len = 1, k;
    if (c < 0x80) cp = c;
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
    else { cp = '?'; len = 1; }
    if (i + (size_t)len > n) len = 1;
    for (k = 1; k < len; k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
    cols += cp < 0x20 ? 0 : uc_width(cp);
    i += (size_t)len;
  }
  return cols;
}

/* }================================================================== */


/*
** {==================================================================
** basename, dirname, seq, sleep
** ===================================================================
*/

/* basename of one path, GNU rules: trailing slashes go, "/" stays */
static char *base_of (const char *p) {
  size_t n = strlen(p), e;
  const char *b;
  while (n > 1 && p[n - 1] == '/') n--;
  if (n == 1 && p[0] == '/') return xstrdup("/");
  e = n;
  while (n > 0 && p[n - 1] != '/') n--;
  b = p + n;
  return xstrndup(b, e - n);
}


int t_basename (int argc, char **argv, int in, int out, int err) {
  Opts g;
  LongOpt lo[] = {{"multiple", 'a', 0}, {"suffix", 's', 1}, {"zero", 'z', 0}, {NULL, 0, 0}};
  int c, multiple = 0, zero = 0;
  const char *suffix = NULL;
  size_t i;
  Out o;
  (void)in;
  opts_init(&g, "basename", argc, argv, err);
  while ((c = opts_next(&g, "as:z", lo)) != 0) {
    if (c == 'a') multiple = 1;
    else if (c == 's') {
      suffix = g.arg;
      multiple = 1;
    }
    else if (c == 'z') zero = 1;
    else if (c == OPT_HELP) { opts_free(&g); return tool_help(out, "basename"); }
    else { opts_free(&g); return 1; }
  }
  if (g.ops.n == 0) {
    tool_err(err, "basename", "missing operand");
    opts_free(&g);
    return 1;
  }
  if (!multiple && g.ops.n > 2) {
    tool_err(err, "basename", "extra operand '%s'", g.ops.v[2]);
    opts_free(&g);
    return 1;
  }
  if (!multiple && g.ops.n == 2) suffix = g.ops.v[1];
  out_init(&o, out);
  for (i = 0; i < (multiple ? g.ops.n : 1); i++) {
    char *b = base_of(g.ops.v[i]);
    size_t bl = strlen(b), sl = suffix ? strlen(suffix) : 0;
    if (sl > 0 && bl > sl && strcmp(b + bl - sl, suffix) == 0) b[bl - sl] = '\0';
    out_puts(&o, b);
    out_putc(&o, zero ? '\0' : '\n');
    free(b);
  }
  out_flush(&o);
  opts_free(&g);
  return 0;
}


int t_dirname (int argc, char **argv, int in, int out, int err) {
  Opts g;
  LongOpt lo[] = {{"zero", 'z', 0}, {NULL, 0, 0}};
  int c, zero = 0;
  size_t i;
  Out o;
  (void)in;
  opts_init(&g, "dirname", argc, argv, err);
  while ((c = opts_next(&g, "z", lo)) != 0) {
    if (c == 'z') zero = 1;
    else if (c == OPT_HELP) { opts_free(&g); return tool_help(out, "dirname"); }
    else { opts_free(&g); return 1; }
  }
  if (g.ops.n == 0) {
    tool_err(err, "dirname", "missing operand");
    opts_free(&g);
    return 1;
  }
  out_init(&o, out);
  for (i = 0; i < g.ops.n; i++) {
    const char *p = g.ops.v[i];
    size_t n = strlen(p);
    while (n > 1 && p[n - 1] == '/') n--;	/* trailing slashes */
    while (n > 0 && p[n - 1] != '/') n--;	/* the last part */
    while (n > 1 && p[n - 1] == '/') n--;	/* slashes before it */
    if (n == 0) out_putc(&o, '.');
    else out_putn(&o, p, n);
    out_putc(&o, zero ? '\0' : '\n');
  }
  out_flush(&o);
  opts_free(&g);
  return 0;
}


/* seq [-w] [-s SEP] [-f FMT] [FIRST [INCR]] LAST */
static int seq_decimals (const char *s) {
  const char *d = strchr(s, '.');
  if (d == NULL || strpbrk(s, "eE")) return 0;
  return (int)strspn(d + 1, "0123456789");
}


int t_seq (int argc, char **argv, int in, int out, int err) {
  Opts g;
  LongOpt lo[] = {{"separator", 's', 1}, {"format", 'f', 1}, {"equal-width", 'w', 0},
                  {NULL, 0, 0}};
  int c, width = 0, prec = 0, k;
  const char *sep = "\n", *fmt = NULL;
  double first = 1, incr = 1, last, v;
  long long i;
  char *end;
  Out o;
  (void)in;
  /* negative numbers are operands, not options */
  opts_init(&g, "seq", argc, argv, err);
  for (k = 1; k < argc; k++) {
    const char *a = argv[k];
    if (a[0] == '-' && (isdigit((unsigned char)a[1]) || a[1] == '.')) break;
    if (strcmp(a, "--") == 0) break;
  }
  g.argc = k;
  while ((c = opts_next(&g, "s:f:w", lo)) != 0) {
    if (c == 's') sep = g.arg;
    else if (c == 'f') fmt = g.arg;
    else if (c == 'w') width = 1;
    else if (c == OPT_HELP) { opts_free(&g); return tool_help(out, "seq"); }
    else { opts_free(&g); return 1; }
  }
  for (; k < argc; k++)
    if (strcmp(argv[k], "--") != 0) vec_push(&g.ops, xstrdup(argv[k]));
  if (g.ops.n < 1 || g.ops.n > 3) {
    tool_err(err, "seq", g.ops.n ? "extra operand '%s'" : "missing operand",
             g.ops.n ? g.ops.v[3] : "");
    opts_free(&g);
    return 1;
  }
  for (k = 0; k < (int)g.ops.n; k++) {
    strtod(g.ops.v[k], &end);
    if (*end || g.ops.v[k][0] == '\0') {
      tool_err(err, "seq", "invalid floating point argument: '%s'", g.ops.v[k]);
      opts_free(&g);
      return 1;
    }
    /* the decimals of FIRST and INCREMENT, like GNU; LAST does not count */
    if (k != (int)g.ops.n - 1 && seq_decimals(g.ops.v[k]) > prec) prec = seq_decimals(g.ops.v[k]);
  }
  last = strtod(g.ops.v[g.ops.n - 1], NULL);
  if (g.ops.n >= 2) first = strtod(g.ops.v[0], NULL);
  if (g.ops.n == 3) incr = strtod(g.ops.v[1], NULL);
  if (incr == 0) {
    tool_err(err, "seq", "invalid Zero increment value: '%s'", g.ops.v[1]);
    opts_free(&g);
    return 1;
  }
  out_init(&o, out);
  {
    int wid = 0;
    if (width) {	/* as wide as the widest end */
      char a[64], b[64];
      snprintf(a, sizeof(a), "%.*f", prec, first);
      snprintf(b, sizeof(b), "%.*f", prec, last);
      wid = (int)(strlen(a) > strlen(b) ? strlen(a) : strlen(b));
    }
    for (i = 0;; i++) {
      v = first + (double)i * incr;
      if ((incr > 0 && v > last + 1e-9 * (incr > 0 ? incr : -incr)) ||
          (incr < 0 && v < last - 1e-9 * -incr)) break;
      if (i > 0) out_puts(&o, sep);
      if (fmt) out_printf(&o, fmt, v);
      else if (width) out_printf(&o, "%0*.*f", wid, prec, v);
      else if (prec == 0 && v > -1e15 && v < 1e15) out_printf(&o, "%lld", (long long)(v < 0 ? v - 0.5 : v + 0.5));
      else out_printf(&o, "%.*f", prec, v);
      if (o.failed || tool_stop()) break;
    }
    if (i > 0) out_putc(&o, '\n');
  }
  out_flush(&o);
  opts_free(&g);
  return 0;
}


/* sleep 1  sleep 0.5  sleep 2m  sleep 1h 30m */
int t_sleep (int argc, char **argv, int in, int out, int err) {
  double total = 0;
  long long until;
  int i;
  (void)in;
  if (argc < 2) {
    tool_err(err, "sleep", "missing operand");
    return 1;
  }
  if (strcmp(argv[1], "--help") == 0) return tool_help(out, "sleep");
  for (i = 1; i < argc; i++) {
    char *end;
    double v = strtod(argv[i], &end);
    if (end == argv[i] || v < 0 || (end[0] && (end[1] || !strchr("smhd", end[0])))) {
      tool_err(err, "sleep", "invalid time interval '%s'", argv[i]);
      return 1;
    }
    if (*end == 'm') v *= 60;
    else if (*end == 'h') v *= 3600;
    else if (*end == 'd') v *= 86400;
    total += v;
  }
  until = os_now_us() + (long long)(total * 1e6);
  while (!tool_stop()) {
    long long left = until - os_now_us();
    if (left <= 0) break;
    os_sleep_ms(left > 100000 ? 100 : (int)((left + 999) / 1000));
  }
  return tool_stop() ? 130 : 0;
}

/* }================================================================== */
