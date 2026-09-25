/*
** ctext.c - the text tools: head tail tee cut sort uniq wc tr
*/

#include "mmc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* "-5" is "-n 5", "+5" is "-n +5" (old style); returns a new argv */
static char **legacy_count (int argc, char **argv, int plus_ok, int *nargc) {
  char **v = (char **)xmalloc(((size_t)argc + 2) * sizeof(char *));
  int i, k = 0;
  v[k++] = argv[0];
  for (i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (i == 1 && ((a[0] == '-' && isdigit((unsigned char)a[1])) ||
                   (plus_ok && a[0] == '+' && isdigit((unsigned char)a[1])))) {
      size_t n = strspn(a + 1, "0123456789");
      static char buf[64];
      const char *rest = a + 1 + n;
      int bytes = *rest == 'c';
      if (*rest == 'c' || *rest == 'l') rest++;
      if (*rest == '\0' || strcmp(rest, "f") == 0) {
        snprintf(buf, sizeof(buf), "%s%.*s", a[0] == '+' ? "+" : "", (int)n, a + 1);
        v[k++] = bytes ? "-c" : "-n";
        v[k++] = buf;
        if (*rest == 'f') v[k++] = "-f";
        continue;
      }
    }
    v[k++] = argv[i];
  }
  v[k] = NULL;
  *nargc = k;
  return v;
}


/* a count for head/tail: 10, 10k, 5M; *plus: had '+', *neg: had '-' */
static int count_arg (const char *tool, int err, const char *s, long long *v, int *plus, int *neg) {
  *plus = *neg = 0;
  if (*s == '+') { *plus = 1; s++; }
  else if (*s == '-') { *neg = 1; s++; }
  if (parse_size(s, v) != 0) {
    tool_err(err, tool, "invalid number of lines: '%s'", s);
    return -1;
  }
  return 0;
}


static int open_or_complain (In *r, const char *tool, const char *name, int in, int err) {
  if (in_open(r, name, in) == 0) return 0;
  {
    char *native = path_to_native(name);
    OsStat st;
    if (os_stat(native, &st) == 0 && st.is_dir) tool_err(err, tool, "error reading '%s': Is a directory", name);
    else tool_err(err, tool, "cannot open '%s' for reading: %s", name, os_errmsg());
    free(native);
  }
  return -1;
}


/*
** {==================================================================
** head
** ===================================================================
*/

int t_head (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {{"lines", 'n', 1}, {"bytes", 'c', 1}, {"quiet", 'q', 0},
    {"silent", 'q', 0}, {"verbose", 'v', 0}, {"zero-terminated", 'z', 0}, {NULL, 0, 0}};
  Opts g;
  Out o;
  int c, nargc, bytes = 0, quiet = 0, verbose = 0, plus, neg = 0, status = 0;
  long long count = 10;
  char delim = '\n';
  char **v = legacy_count(argc, argv, 0, &nargc);
  size_t i;
  opts_init(&g, "head", nargc, v, err);
  while ((c = opts_next(&g, "n:c:qvz", lo)) != 0) {
    switch (c) {
      case 'n': case 'c':
        bytes = c == 'c';
        if (count_arg("head", err, g.arg, &count, &plus, &neg) != 0) goto bad;
        break;
      case 'q': quiet = 1; verbose = 0; break;
      case 'v': verbose = 1; quiet = 0; break;
      case 'z': delim = '\0'; break;
      case OPT_HELP: opts_free(&g); free(v); return tool_help(out, "head");
      default: goto bad;
    }
  }
  if (g.ops.n == 0) vec_push(&g.ops, xstrdup("-"));
  out_init(&o, out);
  for (i = 0; i < g.ops.n && !o.failed && !tool_stop(); i++) {
    In r;
    const char *name = g.ops.v[i];
    if (open_or_complain(&r, "head", name, in, err) != 0) {
      status = 1;
      continue;
    }
    if (verbose || (!quiet && g.ops.n > 1))
      out_printf(&o, "%s==> %s <==\n", i > 0 ? "\n" : "", strcmp(name, "-") == 0 ? "standard input" : name);
    if (bytes && !neg) {
      char chunk[65536];
      long long left = count;
      while (left > 0) {
        long n = in_read(&r, chunk, left < (long long)sizeof(chunk) ? (size_t)left : sizeof(chunk));
        if (n <= 0) break;
        out_putn(&o, chunk, (size_t)n);
        left -= n;
      }
    }
    else if (bytes) {	/* all but the last N bytes */
      Buf all;
      char chunk[65536];
      long n;
      buf_init(&all);
      while ((n = in_read(&r, chunk, sizeof(chunk))) > 0) buf_putn(&all, chunk, (size_t)n);
      if ((long long)all.len > count) out_putn(&o, all.s, all.len - (size_t)count);
      buf_free(&all);
    }
    else if (!neg) {
      long long k = 0;
      char *line;
      size_t len;
      int had;
      while (k < count && in_line(&r, &line, &len, delim, &had)) {
        out_putn(&o, line, len);
        if (had) out_putc(&o, delim);
        k++;
      }
    }
    else {	/* all but the last N lines: keep N back */
      char **ring = count > 0 ? (char **)xmalloc((size_t)count * sizeof(char *)) : NULL;
      size_t *lens = count > 0 ? (size_t *)xmalloc((size_t)count * sizeof(size_t)) : NULL;
      long long k = 0, j;
      char *line;
      size_t len;
      int had;
      while (in_line(&r, &line, &len, delim, &had)) {
        if (count == 0) {
          out_putn(&o, line, len);
          if (had) out_putc(&o, delim);
          continue;
        }
        if (k >= count) {
          size_t at = (size_t)(k % count);
          out_putn(&o, ring[at], lens[at]);
          out_putc(&o, delim);
          free(ring[at]);
        }
        ring[k % count] = xstrndup(line, len);
        lens[k % count] = len;
        k++;
      }
      for (j = k > count ? k - count : 0; j < k; j++) free(ring[j % count]);
      free(ring);
      free(lens);
    }
    in_close(&r);
  }
  out_flush(&o);
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
** tail
** ===================================================================
*/

/* the last 'count' lines (or bytes) of a file we can seek in */
static int tail_seek (Out *o, int fd, long long count, int bytes, char delim) {
  long long size = os_seek(fd, 0, 2), pos, from = 0;
  char buf[65536];
  if (size < 0) return -1;
  if (bytes) from = size > count ? size - count : 0;
  else {
    long long found = 0;
    int first = 1;
    pos = size;
    while (pos > 0 && from == 0) {
      long long chunk = pos > (long long)sizeof(buf) ? (long long)sizeof(buf) : pos;
      long n, k;
      pos -= chunk;
      if (os_seek(fd, pos, 0) < 0) return -1;
      n = os_read(fd, buf, (size_t)chunk);
      if (n <= 0) return -1;
      for (k = n - 1; k >= 0; k--) {
        if (buf[k] != delim) {
          first = 0;
          continue;
        }
        if (first) {	/* the newline that ends the last line */
          first = 0;
          continue;
        }
        if (++found == count) {
          from = pos + k + 1;
          break;
        }
      }
    }
    if (count == 0) from = size;
  }
  if (os_seek(fd, from, 0) < 0) return -1;
  for (;;) {
    long n = os_read(fd, buf, sizeof(buf));
    if (n <= 0) break;
    out_putn(o, buf, (size_t)n);
  }
  return 0;
}


int t_tail (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {{"lines", 'n', 1}, {"bytes", 'c', 1}, {"quiet", 'q', 0},
    {"silent", 'q', 0}, {"verbose", 'v', 0}, {"follow", 'f', 2}, {"retry", 1001, 0},
    {"sleep-interval", 's', 1}, {"pid", 1002, 1}, {"zero-terminated", 'z', 0}, {NULL, 0, 0}};
  Opts g;
  Out o;
  int c, nargc, bytes = 0, quiet = 0, verbose = 0, plus = 0, neg, follow = 0, retry = 0;
  int status = 0, last_shown = -1;
  long long count = 10, pid = 0;
  double interval = 1.0;
  char delim = '\n';
  char **v = legacy_count(argc, argv, 1, &nargc);
  size_t i;
  int *fds;
  long long *sizes;
  opts_init(&g, "tail", nargc, v, err);
  while ((c = opts_next(&g, "n:c:qvfFs:z", lo)) != 0) {
    switch (c) {
      case 'n': case 'c':
        bytes = c == 'c';
        if (count_arg("tail", err, g.arg, &count, &plus, &neg) != 0) goto bad;
        break;
      case 'q': quiet = 1; verbose = 0; break;
      case 'v': verbose = 1; quiet = 0; break;
      case 'f': follow = 1; break;
      case 'F': follow = 1; retry = 1; break;
      case 1001: retry = 1; break;
      case 's': interval = atof(g.arg); break;
      case 1002: pid = atoll(g.arg); break;
      case 'z': delim = '\0'; break;
      case OPT_HELP: opts_free(&g); free(v); return tool_help(out, "tail");
      default: goto bad;
    }
  }
  if (g.ops.n == 0) vec_push(&g.ops, xstrdup("-"));
  out_init(&o, out);
  fds = (int *)xmalloc(g.ops.n * sizeof(int));
  sizes = (long long *)xmalloc(g.ops.n * sizeof(long long));
  for (i = 0; i < g.ops.n && !o.failed && !tool_stop(); i++) {
    const char *name = g.ops.v[i];
    int is_stdin = strcmp(name, "-") == 0, fd;
    fds[i] = -1;
    sizes[i] = 0;
    if (is_stdin) fd = in;
    else {
      char *native = path_to_native(name);
      OsStat st;
      fd = os_open(native, OS_READ);
      if (fd >= 0 && os_stat(native, &st) == 0 && st.is_dir) {
        os_close(fd);
        fd = -1;
        tool_err(err, "tail", "error reading '%s': Is a directory", name);
        free(native);
        status = 1;
        continue;
      }
      free(native);
      if (fd < 0) {
        tool_err(err, "tail", "cannot open '%s' for reading: %s", name, os_errmsg());
        status = 1;
        continue;
      }
    }
    if (verbose || (!quiet && g.ops.n > 1)) {
      out_printf(&o, "%s==> %s <==\n", last_shown >= 0 ? "\n" : "", is_stdin ? "standard input" : name);
      last_shown = (int)i;
    }
    if (!plus && tail_seek(&o, fd, count, bytes, delim) == 0) {
      /* done by seeking */
    }
    else {
      In r;
      in_init(&r, fd, 0);
      if (plus) {	/* from line (byte) N on */
        long long k = 1;
        if (bytes) {
          char chunk[65536];
          long n;
          long long skip = count > 0 ? count - 1 : 0;
          while (skip > 0 && (n = in_read(&r, chunk, skip < (long long)sizeof(chunk) ? (size_t)skip : sizeof(chunk))) > 0)
            skip -= n;
          while ((n = in_read(&r, chunk, sizeof(chunk))) > 0) out_putn(&o, chunk, (size_t)n);
        }
        else {
          char *line;
          size_t len;
          int had;
          while (in_line(&r, &line, &len, delim, &had)) {
            if (k++ >= count) {
              out_putn(&o, line, len);
              if (had) out_putc(&o, delim);
            }
            if (o.failed) break;
          }
        }
      }
      else if (bytes) {
        char *ring = count > 0 ? (char *)xmalloc((size_t)count) : NULL;
        long long total = 0, k;
        char ch;
        while (count > 0 && in_read(&r, &ch, 1) == 1) ring[total++ % count] = ch;
        for (k = total > count ? total - count : 0; k < total; k++) out_putc(&o, ring[k % count]);
        free(ring);
      }
      else {
        char **ring = count > 0 ? (char **)xmalloc((size_t)count * sizeof(char *)) : NULL;
        int *hads = count > 0 ? (int *)xmalloc((size_t)count * sizeof(int)) : NULL;
        long long k = 0, j;
        char *line;
        size_t len;
        int had;
        while (in_line(&r, &line, &len, delim, &had)) {
          if (count == 0) continue;
          if (k >= count) free(ring[k % count]);
          ring[k % count] = xstrndup(line, len);
          hads[k % count] = had;
          k++;
        }
        for (j = k > count ? k - count : 0; j < k; j++) {
          out_puts(&o, ring[j % count]);
          if (hads[j % count]) out_putc(&o, delim);
          free(ring[j % count]);
        }
        free(ring);
        free(hads);
      }
      /* keep what came after the lines we buffered: follow reads the fd */
      r.fd = -1;
      in_close(&r);
    }
    fds[i] = fd;
    if (!is_stdin) {
      long long pos = os_seek(fd, 0, 1);
      sizes[i] = pos > 0 ? pos : 0;
    }
  }
  out_flush(&o);
  if (follow && !o.failed) {	/* -f: wait for more, until Ctrl-C */
    char buf[65536];
    while (!tool_stop() && !o.failed) {
      int any = 0;
      for (i = 0; i < g.ops.n; i++) {
        long n;
        if (fds[i] < 0 && !retry) continue;
        if (retry && strcmp(g.ops.v[i], "-") != 0) {	/* -F: truncated, replaced? */
          char *native = path_to_native(g.ops.v[i]);
          OsStat st;
          if (os_stat(native, &st) == 0) {
            if (fds[i] < 0) {
              fds[i] = os_open(native, OS_READ);
              sizes[i] = 0;
            }
            else if (st.size < sizes[i]) {
              tool_err(err, "tail", "%s: file truncated", g.ops.v[i]);
              os_seek(fds[i], 0, 0);
              sizes[i] = 0;
            }
          }
          free(native);
          if (fds[i] < 0) continue;
        }
        while ((n = os_read(fds[i], buf, sizeof(buf))) > 0) {
          if (g.ops.n > 1 && !quiet && last_shown != (int)i) {
            out_printf(&o, "\n==> %s <==\n", g.ops.v[i]);
            last_shown = (int)i;
          }
          out_putn(&o, buf, (size_t)n);
          sizes[i] += n;
          any = 1;
        }
      }
      out_flush(&o);
      if (pid > 0 && os_kill((long)pid, 0) != 0) break;
      if (!any) {
        int ms = (int)(interval * 1000);
        int waited;
        for (waited = 0; waited < ms && !tool_stop(); waited += 50) os_sleep_ms(50);
      }
    }
  }
  for (i = 0; i < g.ops.n; i++)
    if (fds[i] >= 0 && fds[i] != in) os_close(fds[i]);
  free(fds);
  free(sizes);
  out_flush(&o);
  opts_free(&g);
  free(v);
  return tool_stop() && follow ? 130 : status;
bad:
  opts_free(&g);
  free(v);
  return 1;
}

/* }================================================================== */


/*
** {==================================================================
** tee
** ===================================================================
*/

int t_tee (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {{"append", 'a', 0}, {"ignore-interrupts", 'i', 0},
                               {"output-error", 'p', 2}, {NULL, 0, 0}};
  Opts g;
  int c, append = 0, status = 0, *fds, ok_out = 1;
  size_t i, nf = 0;
  char buf[65536];
  long n;
  opts_init(&g, "tee", argc, argv, err);
  while ((c = opts_next(&g, "aip", lo)) != 0) {
    if (c == 'a') append = 1;
    else if (c == 'i' || c == 'p') continue;
    else if (c == OPT_HELP) { opts_free(&g); return tool_help(out, "tee"); }
    else { opts_free(&g); return 1; }
  }
  fds = (int *)xmalloc((g.ops.n + 1) * sizeof(int));
  for (i = 0; i < g.ops.n; i++) {
    char *native = path_to_native(g.ops.v[i]);
    int fd = strcmp(g.ops.v[i], "-") == 0 ? os_dup(out) : os_open(native, append ? OS_APPEND : OS_WRITE);
    free(native);
    if (fd < 0) {
      tool_err(err, "tee", "%s: %s", g.ops.v[i], os_errmsg());
      status = 1;
      continue;
    }
    fds[nf++] = fd;
  }
  while ((n = os_read(in, buf, sizeof(buf))) > 0) {
    if (ok_out && os_write(out, buf, (size_t)n) < 0) ok_out = 0;
    for (i = 0; i < nf; i++)
      if (fds[i] >= 0 && os_write(fds[i], buf, (size_t)n) < 0) {
        tool_err(err, "tee", "%s: %s", g.ops.v[i], os_errmsg());
        os_close(fds[i]);
        fds[i] = -1;
        status = 1;
      }
    if (tool_stop()) break;
  }
  for (i = 0; i < nf; i++)
    if (fds[i] >= 0) os_close(fds[i]);
  free(fds);
  opts_free(&g);
  return status;
}

/* }================================================================== */


/*
** {==================================================================
** cut
** ===================================================================
*/

typedef struct Ranges {
  long long *lo, *hi;	/* 1-based, inclusive; hi -1: to the end */
  size_t n;
} Ranges;


static int parse_list (const char *s, Ranges *r, int err, int fields) {
  const char *p = s;
  r->lo = r->hi = NULL;
  r->n = 0;
  while (*p) {
    long long lo = 1, hi;
    char *end;
    if (isdigit((unsigned char)*p)) {
      lo = strtoll(p, &end, 10);
      p = end;
      if (lo == 0) {
        tool_err(err, "cut", fields ? "fields are numbered from 1" :
                 "byte/character positions are numbered from 1");
        fd_printf(err, "Try 'cut --help' for more information.\n");
        return -1;
      }
    }
    else if (*p != '-') goto bad;
    hi = lo;
    if (*p == '-') {
      p++;
      if (isdigit((unsigned char)*p)) {
        hi = strtoll(p, &end, 10);
        p = end;
        if (hi < lo) {
          tool_err(err, "cut", "invalid decreasing range");
          return -1;
        }
      }
      else hi = -1;
    }
    r->lo = (long long *)xrealloc(r->lo, (r->n + 1) * sizeof(long long));
    r->hi = (long long *)xrealloc(r->hi, (r->n + 1) * sizeof(long long));
    r->lo[r->n] = lo;
    r->hi[r->n] = hi;
    r->n++;
    if (*p == ',' || *p == ' ') p++;
    else if (*p) goto bad;
  }
  if (r->n > 0) return 0;
bad:
  tool_err(err, "cut", "invalid byte, character or field list");
  fd_printf(err, "Try 'cut --help' for more information.\n");
  return -1;
}


static int in_ranges (const Ranges *r, long long k, int complement) {
  size_t i;
  for (i = 0; i < r->n; i++)
    if (k >= r->lo[i] && (r->hi[i] < 0 || k <= r->hi[i])) return !complement;
  return complement;
}


int t_cut (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {{"bytes", 'b', 1}, {"characters", 'c', 1}, {"fields", 'f', 1},
    {"delimiter", 'd', 1}, {"only-delimited", 's', 0}, {"complement", 1001, 0},
    {"output-delimiter", 1002, 1}, {"zero-terminated", 'z', 0}, {NULL, 0, 0}};
  Opts g;
  Out o;
  Ranges r;
  int c, mode = 0, only = 0, complement = 0, status = 0;
  const char *list = NULL, *delim = "\t", *odelim = NULL;
  char eol = '\n';
  size_t i, dl;
  int utf8 = tool_utf8();
  opts_init(&g, "cut", argc, argv, err);
  while ((c = opts_next(&g, "b:c:f:d:snz", lo)) != 0) {
    switch (c) {
      case 'b': case 'c': case 'f':
        if (mode) {
          tool_err(err, "cut", "only one type of list may be specified");
          opts_free(&g);
          return 1;
        }
        mode = c;
        list = g.arg;
        break;
      case 'd':
        if (utf8_len(g.arg) != (int)strlen(g.arg) && strlen(g.arg) != 0) {
          tool_err(err, "cut", "the delimiter must be a single character");
          opts_free(&g);
          return 1;
        }
        delim = g.arg;
        break;
      case 's': only = 1; break;
      case 'n': break;
      case 'z': eol = '\0'; break;
      case 1001: complement = 1; break;
      case 1002: odelim = g.arg; break;
      case OPT_HELP: opts_free(&g); return tool_help(out, "cut");
      default: opts_free(&g); return 1;
    }
  }
  if (!mode) {
    tool_err(err, "cut", "you must specify a list of bytes, characters, or fields");
    fd_printf(err, "Try 'cut --help' for more information.\n");
    opts_free(&g);
    return 1;
  }
  if (parse_list(list, &r, err, mode == 'f') != 0) {
    opts_free(&g);
    return 1;
  }
  if (*delim == '\0') delim = "\n";
  dl = strlen(delim);
  if (odelim == NULL) odelim = mode == 'f' ? delim : NULL;
  if (g.ops.n == 0) vec_push(&g.ops, xstrdup("-"));
  out_init(&o, out);
  for (i = 0; i < g.ops.n && !o.failed; i++) {
    In rd;
    char *line;
    size_t len;
    int had;
    if (open_or_complain(&rd, "cut", g.ops.v[i], in, err) != 0) {
      status = 1;
      continue;
    }
    while (in_line(&rd, &line, &len, eol, &had) && !o.failed) {
      if (mode == 'f') {
        const char *p = line, *e = line + len, *q;
        long long k = 1;
        int first = 1;
        if (dl > len || strstr(line, delim) == NULL) {
          if (!only) {
            out_putn(&o, line, len);
            out_putc(&o, eol);
          }
          continue;
        }
        for (;;) {
          q = p;
          while (q + dl <= e && memcmp(q, delim, dl) != 0) q++;
          if (q + dl > e) q = e;
          if (in_ranges(&r, k, complement)) {
            if (!first) out_puts(&o, odelim);
            out_putn(&o, p, (size_t)(q - p));
            first = 0;
          }
          if (q >= e) break;
          p = q + dl;
          k++;
        }
        out_putc(&o, eol);
      }
      else {
        size_t pos = 0;
        long long idx = 1;
        int in_sel = 0, first = 1;
        while (pos < len) {
          int cl = (mode == 'c' && utf8) ? utf8_len(line + pos) : 1;
          int sel;
          if (cl < 1 || pos + (size_t)cl > len) cl = 1;
          sel = in_ranges(&r, idx, complement);
          if (sel) {
            if (!in_sel && !first && odelim) out_puts(&o, odelim);
            out_putn(&o, line + pos, (size_t)cl);
            first = 0;
          }
          in_sel = sel;
          pos += (size_t)cl;
          idx++;
        }
        out_putc(&o, eol);
      }
    }
    in_close(&rd);
  }
  out_flush(&o);
  free(r.lo);
  free(r.hi);
  opts_free(&g);
  return status;
}

/* }================================================================== */


/*
** {==================================================================
** sort
** ===================================================================
*/

enum {
  K_NUM = 1, K_GEN = 2, K_HUMAN = 4, K_MONTH = 8, K_VERSION = 16, K_RANDOM = 32,
  K_FOLD = 64, K_DICT = 128, K_NOPRINT = 256, K_REV = 512, K_BLANK_S = 1024,
  K_BLANK_E = 2048
};

typedef struct SKey {
  int f1, c1, f2, c2;	/* fields and characters, 1-based; f2 0: to the end */
  int flags;
} SKey;

typedef struct SLine {
  const char *s;
  size_t len;
} SLine;

typedef struct Sort {
  SKey *keys;
  int nkeys;
  int gflags;	/* the options for keys without their own */
  int tab;	/* -t, -1: blanks */
  int stable, unique;
  unsigned seed;
} Sort;

static Sort *g_sort;


static int key_flag (char c) {
  switch (c) {
    case 'n': return K_NUM;
    case 'g': return K_GEN;
    case 'h': return K_HUMAN;
    case 'M': return K_MONTH;
    case 'V': return K_VERSION;
    case 'R': return K_RANDOM;
    case 'f': return K_FOLD;
    case 'd': return K_DICT;
    case 'i': return K_NOPRINT;
    case 'r': return K_REV;
    case 'b': return K_BLANK_S | K_BLANK_E;
  }
  return 0;
}


/* F[.C][opts][,F[.C][opts]] */
static int parse_key (const char *s, SKey *k, int err) {
  char *end;
  memset(k, 0, sizeof(*k));
  k->f1 = (int)strtol(s, &end, 10);
  if (end == s || k->f1 < 1) goto bad;
  s = end;
  if (*s == '.') {
    k->c1 = (int)strtol(s + 1, &end, 10);
    if (k->c1 < 1) goto bad;
    s = end;
  }
  for (; *s && *s != ','; s++) {
    int f = key_flag(*s);
    if (!f) goto bad;
    k->flags |= f == (K_BLANK_S | K_BLANK_E) ? K_BLANK_S : f;
  }
  if (*s == ',') {
    s++;
    k->f2 = (int)strtol(s, &end, 10);
    if (end == s || k->f2 < 1) goto bad;
    s = end;
    if (*s == '.') {
      k->c2 = (int)strtol(s + 1, &end, 10);
      s = end;
    }
    for (; *s; s++) {
      int f = key_flag(*s);
      if (!f) goto bad;
      k->flags |= f == (K_BLANK_S | K_BLANK_E) ? K_BLANK_E : f;
    }
  }
  return 0;
bad:
  tool_err(err, "sort", "invalid key specification");
  return -1;
}


static int is_blank (char c) {
  return c == ' ' || c == '\t';
}


/* where field f (1-based) starts; blanks before it belong to it (no -t) */
static const char *field_start (const Sort *so, const char *s, const char *e, int f) {
  const char *p = s;
  int k;
  for (k = 1; k < f && p < e; k++) {
    if (so->tab >= 0) {
      while (p < e && *p != (char)so->tab) p++;
      if (p < e) p++;
    }
    else {
      while (p < e && is_blank(*p)) p++;
      while (p < e && !is_blank(*p)) p++;
    }
  }
  return p;
}


static void key_span (const Sort *so, const SKey *k, const SLine *l, const char **ks, const char **ke) {
  const char *s = l->s, *e = l->s + l->len, *p, *q;
  p = field_start(so, s, e, k->f1);
  if (k->flags & K_BLANK_S)
    while (p < e && is_blank(*p)) p++;
  if (k->c1 > 1) {
    const char *lim = e;
    int n = k->c1 - 1;
    while (n-- > 0 && p < lim) p++;
  }
  if (k->f2 == 0) q = e;
  else {
    q = field_start(so, s, e, k->f2);
    if (k->c2 == 0) {	/* to the end of field f2 */
      if (so->tab >= 0) {
        while (q < e && *q != (char)so->tab) q++;
      }
      else {
        while (q < e && is_blank(*q)) q++;
        while (q < e && !is_blank(*q)) q++;
      }
    }
    else {
      int n = k->c2;
      if (k->flags & K_BLANK_E)
        while (q < e && is_blank(*q)) q++;
      while (n-- > 0 && q < e) q++;
    }
  }
  if (q < p) q = p;
  *ks = p;
  *ke = q;
}


static int month_of (const char *s, const char *e) {
  static const char *const m[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG",
                                  "SEP", "OCT", "NOV", "DEC"};
  int i;
  while (s < e && is_blank(*s)) s++;
  if (e - s < 3) return 0;
  for (i = 0; i < 12; i++)
    if (toupper((unsigned char)s[0]) == m[i][0] && toupper((unsigned char)s[1]) == m[i][1] &&
        toupper((unsigned char)s[2]) == m[i][2])
      return i + 1;
  return 0;
}


/* -n: compare the leading numbers as text, so any length works */
static int num_cmp (const char *a, const char *ae, const char *b, const char *be) {
  int na = 0, nb = 0, sign;
  const char *ai, *bi, *af, *bf;
  size_t la, lb;
  while (a < ae && is_blank(*a)) a++;
  while (b < be && is_blank(*b)) b++;
  if (a < ae && *a == '-') { na = 1; a++; }
  if (b < be && *b == '-') { nb = 1; b++; }
  ai = a;
  while (a < ae && isdigit((unsigned char)*a)) a++;
  bi = b;
  while (b < be && isdigit((unsigned char)*b)) b++;
  af = a;
  bf = b;
  /* no digits at all: zero, whatever the sign */
  {
    int za = (af == ai) && !(a < ae && *a == '.' && a + 1 < ae && isdigit((unsigned char)a[1]));
    int zb = (bf == bi) && !(b < be && *b == '.' && b + 1 < be && isdigit((unsigned char)b[1]));
    const char *p;
    if (!za) { za = 1; for (p = ai; p < af; p++) if (*p != '0') za = 0; }
    if (za && a < ae && *a == '.') { for (p = a + 1; p < ae && isdigit((unsigned char)*p); p++) if (*p != '0') za = 0; }
    if (!zb) { zb = 1; for (p = bi; p < bf; p++) if (*p != '0') zb = 0; }
    if (zb && b < be && *b == '.') { for (p = b + 1; p < be && isdigit((unsigned char)*p); p++) if (*p != '0') zb = 0; }
    if (za) na = 0;
    if (zb) nb = 0;
    if (za && zb) return 0;
  }
  if (na != nb) return na ? -1 : 1;
  sign = na ? -1 : 1;
  while (ai < af && *ai == '0') ai++;
  while (bi < bf && *bi == '0') bi++;
  la = (size_t)(af - ai);
  lb = (size_t)(bf - bi);
  if (la != lb) return la < lb ? -sign : sign;
  if (la > 0) {
    int r = memcmp(ai, bi, la);
    if (r) return r < 0 ? -sign : sign;
  }
  /* the fractions */
  a = af;
  b = bf;
  if (a < ae && *a == '.') a++;
  else a = ae;
  if (b < be && *b == '.') b++;
  else b = be;
  for (;;) {
    int da = (a < ae && isdigit((unsigned char)*a)) ? *a : '0';
    int db = (b < be && isdigit((unsigned char)*b)) ? *b : '0';
    int more_a = a < ae && isdigit((unsigned char)*a), more_b = b < be && isdigit((unsigned char)*b);
    if (!more_a && !more_b) return 0;
    if (da != db) return da < db ? -sign : sign;
    if (more_a) a++;
    if (more_b) b++;
  }
}


static double human_val (const char *s, const char *e, int *unit) {
  char buf[64], *end;
  size_t n = (size_t)(e - s);
  double v;
  static const char *const units = "KMGTPEZY";
  const char *u;
  if (n >= sizeof(buf)) n = sizeof(buf) - 1;
  memcpy(buf, s, n);
  buf[n] = '\0';
  v = strtod(buf, &end);
  *unit = 0;
  if (*end && (u = strchr(units, toupper((unsigned char)*end))) != NULL) *unit = (int)(u - units) + 1;
  return v;
}


static unsigned hash_bytes (const char *s, size_t n, unsigned seed) {
  unsigned h = 2166136261u ^ seed;
  size_t i;
  for (i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 16777619u;
  return h;
}


static int text_cmp (const char *a, const char *ae, const char *b, const char *be, int flags) {
  if (!(flags & (K_FOLD | K_DICT | K_NOPRINT))) {
    size_t la = (size_t)(ae - a), lb = (size_t)(be - b);
    int r = memcmp(a, b, la < lb ? la : lb);
    if (r) return r;
    return la < lb ? -1 : la > lb ? 1 : 0;
  }
  for (;;) {
    int ca, cb;
    if (flags & K_DICT) {
      while (a < ae && !(isalnum((unsigned char)*a) || is_blank(*a))) a++;
      while (b < be && !(isalnum((unsigned char)*b) || is_blank(*b))) b++;
    }
    if (flags & K_NOPRINT) {
      while (a < ae && !isprint((unsigned char)*a)) a++;
      while (b < be && !isprint((unsigned char)*b)) b++;
    }
    if (a >= ae || b >= be) return (a < ae) - (b < be);
    ca = (unsigned char)*a++;
    cb = (unsigned char)*b++;
    if (flags & K_FOLD) {
      ca = toupper(ca);
      cb = toupper(cb);
    }
    if (ca != cb) return ca - cb;
  }
}


static int key_cmp (const Sort *so, int flags, const char *a, const char *ae, const char *b,
                    const char *be) {
  int r = 0;
  if (flags & K_NUM) r = num_cmp(a, ae, b, be);
  else if (flags & K_GEN) {
    char ba[64], bb[64];
    size_t la = (size_t)(ae - a) < 63 ? (size_t)(ae - a) : 63, lb = (size_t)(be - b) < 63 ? (size_t)(be - b) : 63;
    double x, y;
    char *ea, *eb;
    memcpy(ba, a, la);
    ba[la] = '\0';
    memcpy(bb, b, lb);
    bb[lb] = '\0';
    x = strtod(ba, &ea);
    y = strtod(bb, &eb);
    if (ea == ba && eb == bb) r = 0;
    else if (ea == ba) r = -1;
    else if (eb == bb) r = 1;
    else r = x < y ? -1 : x > y ? 1 : 0;
  }
  else if (flags & K_HUMAN) {
    int ua, ub;
    double x = human_val(a, ae, &ua), y = human_val(b, be, &ub);
    if ((x < 0) != (y < 0)) r = x < 0 ? -1 : 1;
    else if (ua != ub) r = (ua < ub ? -1 : 1) * (x < 0 ? -1 : 1);
    else r = x < y ? -1 : x > y ? 1 : 0;
  }
  else if (flags & K_MONTH) {
    int ma = month_of(a, ae), mb = month_of(b, be);
    r = ma - mb;
  }
  else if (flags & K_VERSION) {
    char *x = xstrndup(a, (size_t)(ae - a)), *y = xstrndup(b, (size_t)(be - b));
    const char *p = x, *q = y;
    r = 0;
    while (*p && *q && !r) {
      if (isdigit((unsigned char)*p) && isdigit((unsigned char)*q)) {
        unsigned long long u = strtoull(p, (char **)&p, 10), v = strtoull(q, (char **)&q, 10);
        r = u < v ? -1 : u > v ? 1 : 0;
      }
      else {
        r = (unsigned char)*p - (unsigned char)*q;
        p++;
        q++;
      }
    }
    if (!r) r = (*p != 0) - (*q != 0);
    free(x);
    free(y);
  }
  else if (flags & K_RANDOM) {
    unsigned ha = hash_bytes(a, (size_t)(ae - a), so->seed), hb = hash_bytes(b, (size_t)(be - b), so->seed);
    r = ha < hb ? -1 : ha > hb ? 1 : 0;
  }
  else r = text_cmp(a, ae, b, be, flags);
  return (flags & K_REV) ? -r : r;
}


static int keys_cmp (const Sort *so, const SLine *x, const SLine *y) {
  int i, r;
  if (so->nkeys == 0) {
    const char *a = x->s, *ae = x->s + x->len, *b = y->s, *be = y->s + y->len;
    if (so->gflags & K_BLANK_S) {
      while (a < ae && is_blank(*a)) a++;
      while (b < be && is_blank(*b)) b++;
    }
    return key_cmp(so, so->gflags, a, ae, b, be);
  }
  for (i = 0; i < so->nkeys; i++) {
    const SKey *k = &so->keys[i];
    const char *a, *ae, *b, *be;
    int flags = k->flags;
    if ((flags & ~(K_BLANK_S | K_BLANK_E)) == 0) flags |= so->gflags;
    key_span(so, k, x, &a, &ae);
    key_span(so, k, y, &b, &be);
    if ((r = key_cmp(so, flags, a, ae, b, be)) != 0) return r;
  }
  return 0;
}


static int line_cmp (const SLine *x, const SLine *y) {
  const Sort *so = g_sort;
  int r = keys_cmp(so, x, y);
  if (r || so->stable || so->unique) return r;
  if (so->nkeys == 0 && !(so->gflags & ~K_REV)) return 0;	/* it was the whole line already */
  r = text_cmp(x->s, x->s + x->len, y->s, y->s + y->len, 0);	/* last resort */
  return (so->gflags & K_REV) ? -r : r;
}


static void merge_sort (SLine *v, SLine *tmp, size_t n) {
  size_t mid, i, j, k;
  if (n < 2) return;
  if (n < 12) {	/* insertion sort */
    for (i = 1; i < n; i++) {
      SLine t = v[i];
      for (j = i; j > 0 && line_cmp(&v[j - 1], &t) > 0; j--) v[j] = v[j - 1];
      v[j] = t;
    }
    return;
  }
  mid = n / 2;
  merge_sort(v, tmp, mid);
  merge_sort(v + mid, tmp, n - mid);
  if (line_cmp(&v[mid - 1], &v[mid]) <= 0) return;
  memcpy(tmp, v, mid * sizeof(SLine));
  i = 0;
  j = mid;
  k = 0;
  while (i < mid && j < n) v[k++] = line_cmp(&tmp[i], &v[j]) <= 0 ? tmp[i++] : v[j++];
  while (i < mid) v[k++] = tmp[i++];
}


int t_sort (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {
    {"numeric-sort", 'n', 0}, {"general-numeric-sort", 'g', 0}, {"human-numeric-sort", 'h', 0},
    {"month-sort", 'M', 0}, {"version-sort", 'V', 0}, {"random-sort", 'R', 0},
    {"ignore-case", 'f', 0}, {"dictionary-order", 'd', 0}, {"ignore-nonprinting", 'i', 0},
    {"reverse", 'r', 0}, {"ignore-leading-blanks", 'b', 0}, {"key", 'k', 1},
    {"field-separator", 't', 1}, {"unique", 'u', 0}, {"stable", 's', 0}, {"output", 'o', 1},
    {"check", 'c', 2}, {"merge", 'm', 0}, {"zero-terminated", 'z', 0}, {"parallel", 1001, 1},
    {"buffer-size", 'S', 1}, {"temporary-directory", 'T', 1}, {"sort", 1002, 1},
    {"random-source", 1001, 1}, {NULL, 0, 0}};
  Sort so;
  Opts g;
  Out o;
  int c, check = 0, status = 0;
  const char *outfile = NULL;
  char eol = '\n';
  Buf data;
  SLine *lines = NULL;
  size_t nl = 0, cap = 0, i;
  size_t *starts = NULL;
  memset(&so, 0, sizeof(so));
  so.tab = -1;
  so.seed = (unsigned)os_now_us();
  opts_init(&g, "sort", argc, argv, err);
  while ((c = opts_next(&g, "nghMVRfdirbk:t:uso:cCmzS:T:", lo)) != 0) {
    switch (c) {
      case 'k': {
        SKey k;
        if (parse_key(g.arg, &k, err) != 0) goto bad;
        so.keys = (SKey *)xrealloc(so.keys, (size_t)(so.nkeys + 1) * sizeof(SKey));
        so.keys[so.nkeys++] = k;
        break;
      }
      case 't':
        if (strlen(g.arg) != 1 && strcmp(g.arg, "\\0") != 0) {
          tool_err(err, "sort", "multi-character tab '%s'", g.arg);
          goto bad;
        }
        so.tab = strcmp(g.arg, "\\0") == 0 ? 0 : (unsigned char)g.arg[0];
        break;
      case 'u': so.unique = 1; break;
      case 's': so.stable = 1; break;
      case 'o': outfile = g.arg; break;
      case 'c': check = (g.arg && (strcmp(g.arg, "quiet") == 0 || strcmp(g.arg, "silent") == 0)) ? 2 : 1; break;
      case 'C': check = 2; break;
      case 'm': case 'S': case 'T': case 1001: break;
      case 'z': eol = '\0'; break;
      case 1002:
        so.gflags |= strcmp(g.arg, "numeric") == 0 ? K_NUM : strcmp(g.arg, "general-numeric") == 0 ? K_GEN :
                     strcmp(g.arg, "human-numeric") == 0 ? K_HUMAN : strcmp(g.arg, "month") == 0 ? K_MONTH :
                     strcmp(g.arg, "version") == 0 ? K_VERSION : strcmp(g.arg, "random") == 0 ? K_RANDOM : 0;
        break;
      case OPT_HELP: opts_free(&g); free(so.keys); return tool_help(out, "sort");
      default:
        if (c > 0 && c < 256 && key_flag((char)c)) {
          so.gflags |= key_flag((char)c) == (K_BLANK_S | K_BLANK_E) ? K_BLANK_S | K_BLANK_E : key_flag((char)c);
          break;
        }
        goto bad;
    }
  }
  /* key options apply to keys that have none of their own */
  if (g.ops.n == 0) vec_push(&g.ops, xstrdup("-"));
  buf_init(&data);
  for (i = 0; i < g.ops.n; i++) {
    In r;
    char chunk[65536];
    long n;
    if (in_open(&r, g.ops.v[i], in) != 0) {
      tool_err(err, "sort", "cannot read: %s: %s", g.ops.v[i], os_errmsg());
      buf_free(&data);
      goto bad;
    }
    while ((n = in_read(&r, chunk, sizeof(chunk))) > 0) buf_putn(&data, chunk, (size_t)n);
    if (data.len > 0 && data.s[data.len - 1] != eol) buf_putc(&data, eol);
    in_close(&r);
    if (tool_stop()) break;
  }
  /* the lines, pointing into data */
  {
    size_t pos = 0;
    while (pos < data.len) {
      char *e = (char *)memchr(data.s + pos, eol, data.len - pos);
      size_t len = e ? (size_t)(e - (data.s + pos)) : data.len - pos;
      if (nl == cap) {
        cap = cap ? cap * 2 : 1024;
        lines = (SLine *)xrealloc(lines, cap * sizeof(SLine));
        starts = (size_t *)xrealloc(starts, cap * sizeof(size_t));
      }
      starts[nl] = pos;
      lines[nl].len = len;
      nl++;
      pos += len + 1;
    }
    for (i = 0; i < nl; i++) lines[i].s = data.s + starts[i];
    free(starts);
  }
  g_sort = &so;
  if (check) {
    for (i = 1; i < nl; i++) {
      int r = line_cmp(&lines[i - 1], &lines[i]);
      if (r > 0 || (so.unique && r == 0)) {
        if (check == 1)
          fd_printf(err, "sort: %s:%lu: disorder: %.*s\n", strcmp(g.ops.v[0], "-") == 0 ? "-" : g.ops.v[0],
                    (unsigned long)(i + 1), (int)lines[i].len, lines[i].s);
        status = 1;
        break;
      }
    }
    free(lines);
    buf_free(&data);
    opts_free(&g);
    free(so.keys);
    return status;
  }
  {
    SLine *tmp = (SLine *)xmalloc((nl / 2 + 1) * sizeof(SLine));
    merge_sort(lines, tmp, nl);
    free(tmp);
  }
  {
    int fd = out;
    if (outfile != NULL && strcmp(outfile, "-") != 0) {
      char *native = path_to_native(outfile);
      fd = os_open(native, OS_WRITE);
      free(native);
      if (fd < 0) {
        tool_err(err, "sort", "open failed: %s: %s", outfile, os_errmsg());
        free(lines);
        buf_free(&data);
        goto bad;
      }
    }
    out_init(&o, fd);
    for (i = 0; i < nl && !o.failed; i++) {
      if (so.unique && i > 0 && keys_cmp(&so, &lines[i - 1], &lines[i]) == 0) continue;
      out_putn(&o, lines[i].s, lines[i].len);
      out_putc(&o, eol);
    }
    out_flush(&o);
    if (fd != out) os_close(fd);
  }
  free(lines);
  buf_free(&data);
  opts_free(&g);
  free(so.keys);
  return status;
bad:
  opts_free(&g);
  free(so.keys);
  return 2;
}

/* }================================================================== */


/*
** {==================================================================
** uniq
** ===================================================================
*/

static const char *uniq_key (const char *s, size_t len, int skip_f, int skip_c, size_t *klen) {
  const char *p = s, *e = s + len;
  int k;
  for (k = 0; k < skip_f; k++) {
    while (p < e && is_blank(*p)) p++;
    while (p < e && !is_blank(*p)) p++;
  }
  for (k = 0; k < skip_c && p < e; k++) p++;
  *klen = (size_t)(e - p);
  return p;
}


int t_uniq (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {{"count", 'c', 0}, {"repeated", 'd', 0},
    {"all-repeated", 'D', 2}, {"unique", 'u', 0}, {"ignore-case", 'i', 0},
    {"skip-fields", 'f', 1}, {"skip-chars", 's', 1}, {"check-chars", 'w', 1},
    {"zero-terminated", 'z', 0}, {"group", 1001, 2}, {NULL, 0, 0}};
  Opts g;
  Out o;
  In r;
  int c, count = 0, only_dup = 0, all_dup = 0, only_uniq = 0, icase = 0, skip_f = 0, skip_c = 0;
  long long width = -1;
  char eol = '\n';
  char *prev = NULL;
  size_t prev_len = 0;
  long long run = 0;
  int outfd = out, status = 0;
  char *line;
  size_t len;
  int had;
  opts_init(&g, "uniq", argc, argv, err);
  while ((c = opts_next(&g, "cdDuif:s:w:z", lo)) != 0) {
    switch (c) {
      case 'c': count = 1; break;
      case 'd': only_dup = 1; break;
      case 'D': case 1001: all_dup = 1; break;
      case 'u': only_uniq = 1; break;
      case 'i': icase = 1; break;
      case 'f': skip_f = atoi(g.arg); break;
      case 's': skip_c = atoi(g.arg); break;
      case 'w': width = atoll(g.arg); break;
      case 'z': eol = '\0'; break;
      case OPT_HELP: opts_free(&g); return tool_help(out, "uniq");
      default: opts_free(&g); return 1;
    }
  }
  if (g.ops.n > 2) {
    tool_err(err, "uniq", "extra operand '%s'", g.ops.v[2]);
    opts_free(&g);
    return 1;
  }
  if (open_or_complain(&r, "uniq", g.ops.n > 0 ? g.ops.v[0] : "-", in, err) != 0) {
    opts_free(&g);
    return 1;
  }
  if (g.ops.n == 2 && strcmp(g.ops.v[1], "-") != 0) {
    char *native = path_to_native(g.ops.v[1]);
    outfd = os_open(native, OS_WRITE);
    free(native);
    if (outfd < 0) {
      tool_err(err, "uniq", "%s: %s", g.ops.v[1], os_errmsg());
      in_close(&r);
      opts_free(&g);
      return 1;
    }
  }
  out_init(&o, outfd);
  for (;;) {
    int more = in_line(&r, &line, &len, eol, &had);
    int same = 0;
    if (more && prev != NULL) {
      size_t ka, kb;
      const char *a = uniq_key(prev, prev_len, skip_f, skip_c, &ka);
      const char *b = uniq_key(line, len, skip_f, skip_c, &kb);
      if (width >= 0) {
        if ((long long)ka > width) ka = (size_t)width;
        if ((long long)kb > width) kb = (size_t)width;
      }
      same = ka == kb && (icase ? m_strnicmp(a, b, ka) == 0 : memcmp(a, b, ka) == 0);
    }
    if (prev != NULL && (!more || !same)) {	/* the run of prev ends */
      int show = (only_dup && run > 1) || (only_uniq && run == 1) || (!only_dup && !only_uniq && !all_dup);
      if (all_dup) show = 0;
      if (show) {
        if (count) out_printf(&o, "%7lld ", run);
        out_putn(&o, prev, prev_len);
        out_putc(&o, eol);
      }
    }
    if (!more) break;
    if (all_dup) {	/* -D: every line of a repeated run */
      /* printed when we know the run is repeated: keep it simple, look back */
    }
    if (prev != NULL && same) {
      run++;
      if (all_dup) {
        if (run == 2) {
          out_putn(&o, prev, prev_len);
          out_putc(&o, eol);
        }
        out_putn(&o, line, len);
        out_putc(&o, eol);
      }
    }
    else {
      free(prev);
      prev = xstrndup(line, len);
      prev_len = len;
      run = 1;
    }
    if (o.failed) break;
  }
  free(prev);
  out_flush(&o);
  in_close(&r);
  if (outfd != out) os_close(outfd);
  opts_free(&g);
  return status;
}

/* }================================================================== */


/*
** {==================================================================
** wc
** ===================================================================
*/

typedef struct WcCount {
  long long lines, words, bytes, chars, maxw;
} WcCount;


static void wc_count (In *r, WcCount *w) {
  char buf[65536];
  long n;
  int in_word = 0, col = 0, utf8 = tool_utf8();
  memset(w, 0, sizeof(*w));
  while ((n = in_read(r, buf, sizeof(buf))) > 0) {
    long i;
    w->bytes += n;
    for (i = 0; i < n; i++) {
      unsigned char c = (unsigned char)buf[i];
      if ((c & 0xC0) != 0x80 || !utf8) w->chars++;
      if (c == '\n') {
        w->lines++;
        if (col > w->maxw) w->maxw = col;
        col = 0;
      }
      else if (c == '\t') col = (col / 8 + 1) * 8;
      else if (c == '\r' || c == '\f') {
        if (c == '\r') col = 0;
      }
      else if ((c & 0xC0) != 0x80 && c >= 32) col++;
      if (c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '\f' || c == '\v') in_word = 0;
      else if (!in_word && (c >= 32 || c >= 0x80)) {
        in_word = 1;
        w->words++;
      }
    }
    if (tool_stop()) break;
  }
  if (col > w->maxw) w->maxw = col;
}


int t_wc (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {{"lines", 'l', 0}, {"words", 'w', 0}, {"bytes", 'c', 0},
    {"chars", 'm', 0}, {"max-line-length", 'L', 0}, {"total", 1001, 1}, {NULL, 0, 0}};
  Opts g;
  Out o;
  int c, fl = 0, fw = 0, fc = 0, fm = 0, fL = 0, status = 0, width = 1, only_stdin;
  WcCount *counts, total;
  int *ok;
  size_t i, n;
  const char *total_when = "auto";
  opts_init(&g, "wc", argc, argv, err);
  while ((c = opts_next(&g, "lwcmL", lo)) != 0) {
    switch (c) {
      case 'l': fl = 1; break;
      case 'w': fw = 1; break;
      case 'c': fc = 1; break;
      case 'm': fm = 1; break;
      case 'L': fL = 1; break;
      case 1001: total_when = g.arg; break;
      case OPT_HELP: opts_free(&g); return tool_help(out, "wc");
      default: opts_free(&g); return 1;
    }
  }
  if (!fl && !fw && !fc && !fm && !fL) fl = fw = fc = 1;
  only_stdin = g.ops.n == 0;
  if (only_stdin) vec_push(&g.ops, xstrdup("-"));
  n = g.ops.n;
  counts = (WcCount *)xmalloc(n * sizeof(WcCount));
  ok = (int *)xmalloc(n * sizeof(int));
  memset(&total, 0, sizeof(total));
  for (i = 0; i < n; i++) {
    In r;
    ok[i] = 0;
    if (in_open(&r, g.ops.v[i], in) != 0) {
      char *native = path_to_native(g.ops.v[i]);
      OsStat st;
      if (os_stat(native, &st) == 0 && st.is_dir) tool_err(err, "wc", "%s: Is a directory", g.ops.v[i]);
      else tool_err(err, "wc", "%s: %s", g.ops.v[i], os_errmsg());
      free(native);
      status = 1;
      continue;
    }
    wc_count(&r, &counts[i]);
    in_close(&r);
    ok[i] = 1;
    total.lines += counts[i].lines;
    total.words += counts[i].words;
    total.bytes += counts[i].bytes;
    total.chars += counts[i].chars;
    if (counts[i].maxw > total.maxw) total.maxw = counts[i].maxw;
  }
  /* column width: like GNU, from the biggest number; 7 for a pipe */
  if (fl + fw + fc + fm + fL > 1 || n > 1) {
    char tmp[32];
    long long big = total.bytes > total.chars ? total.bytes : total.chars;
    if (total.lines > big) big = total.lines;
    if (total.words > big) big = total.words;
    if (total.maxw > big) big = total.maxw;
    width = (int)strlen(ll_to_str(big, tmp));
    if (only_stdin && width < 7) width = 7;
  }
  out_init(&o, out);
  for (i = 0; i <= n; i++) {
    WcCount *w;
    const char *name;
    int first = 1;
    if (i == n) {
      int show = strcmp(total_when, "always") == 0 || (strcmp(total_when, "auto") == 0 && n > 1);
      if (!show) break;
      w = &total;
      name = "total";
    }
    else {
      if (!ok[i] || strcmp(total_when, "only") == 0) continue;
      w = &counts[i];
      name = only_stdin ? NULL : g.ops.v[i];
      if (name && strcmp(name, "-") == 0) name = n > 1 ? "-" : NULL;
      if (name && strcmp(g.ops.v[i], "-") == 0 && n == 1) name = NULL;
    }
    if (fl) { out_printf(&o, "%*lld", width, w->lines); first = 0; }
    if (fw) { out_printf(&o, "%s%*lld", first ? "" : " ", width, w->words); first = 0; }
    if (fm) { out_printf(&o, "%s%*lld", first ? "" : " ", width, w->chars); first = 0; }
    if (fc) { out_printf(&o, "%s%*lld", first ? "" : " ", width, w->bytes); first = 0; }
    if (fL) { out_printf(&o, "%s%*lld", first ? "" : " ", width, w->maxw); first = 0; }
    if (name) out_printf(&o, " %s", name);
    out_putc(&o, '\n');
  }
  out_flush(&o);
  free(counts);
  free(ok);
  opts_free(&g);
  return status;
}

/* }================================================================== */


/*
** {==================================================================
** tr
** ===================================================================
*/

/* a tr set as bytes, in order: a-z, [:class:], [c*n], \n \NNN */
static int tr_expand (const char *s, Buf *outb, int err, int is_set2, int *has_class_upper,
                      int *has_class_lower) {
  const unsigned char *p = (const unsigned char *)s;
  while (*p) {
    int c;
    if (p[0] == '[' && p[1] == ':') {
      const char *e = strstr((const char *)p + 2, ":]");
      char name[16];
      size_t n;
      int k;
      if (e && (n = (size_t)(e - (const char *)p - 2)) < sizeof(name)) {
        memcpy(name, p + 2, n);
        name[n] = '\0';
        if (strcmp(name, "upper") == 0 && has_class_upper) *has_class_upper = (int)outb->len + 1;
        if (strcmp(name, "lower") == 0 && has_class_lower) *has_class_lower = (int)outb->len + 1;
        for (k = 0; k < 256; k++) {
          int in;
          if (strcmp(name, "alpha") == 0) in = isalpha(k);
          else if (strcmp(name, "digit") == 0) in = isdigit(k);
          else if (strcmp(name, "alnum") == 0) in = isalnum(k);
          else if (strcmp(name, "upper") == 0) in = isupper(k);
          else if (strcmp(name, "lower") == 0) in = islower(k);
          else if (strcmp(name, "space") == 0) in = isspace(k);
          else if (strcmp(name, "blank") == 0) in = k == ' ' || k == '\t';
          else if (strcmp(name, "punct") == 0) in = ispunct(k);
          else if (strcmp(name, "print") == 0) in = isprint(k);
          else if (strcmp(name, "graph") == 0) in = isgraph(k);
          else if (strcmp(name, "cntrl") == 0) in = iscntrl(k);
          else if (strcmp(name, "xdigit") == 0) in = isxdigit(k);
          else {
            tool_err(err, "tr", "invalid character class '%s'", name);
            return -1;
          }
          if (in && k < 128) buf_putc(outb, (char)k);
        }
        p = (const unsigned char *)e + 2;
        continue;
      }
    }
    if (p[0] == '[' && p[1] == '=' && p[2] && p[3] == '=' && p[4] == ']') {
      buf_putc(outb, (char)p[2]);
      p += 5;
      continue;
    }
    if (is_set2 && p[0] == '[' && p[1] && p[2] == '*') {	/* [c*n] */
      const unsigned char *q = p + 3;
      long n = 0;
      int k;
      while (isdigit(*q)) n = n * 10 + (*q++ - '0');
      if (*q == ']') {
        if (n == 0) n = -1;	/* fill: marked, expanded later */
        if (n < 0) {
          buf_putc(outb, '\0');
          buf_putc(outb, (char)p[1]);
          buf_putc(outb, '\x01');
        }
        else
          for (k = 0; k < n; k++) buf_putc(outb, (char)p[1]);
        p = q + 1;
        continue;
      }
    }
    if (*p == '\\' && p[1]) {
      p++;
      switch (*p) {
        case 'n': c = '\n'; p++; break;
        case 't': c = '\t'; p++; break;
        case 'r': c = '\r'; p++; break;
        case 'a': c = '\a'; p++; break;
        case 'b': c = '\b'; p++; break;
        case 'f': c = '\f'; p++; break;
        case 'v': c = '\v'; p++; break;
        case '\\': c = '\\'; p++; break;
        default:
          if (*p >= '0' && *p <= '7') {
            int k;
            c = 0;
            for (k = 0; k < 3 && *p >= '0' && *p <= '7'; k++) c = c * 8 + (*p++ - '0');
          }
          else c = *p++;
      }
    }
    else c = *p++;
    if (*p == '-' && p[1]) {	/* a range */
      int hi;
      p++;
      if (*p == '\\' && p[1]) {
        p++;
        if (*p >= '0' && *p <= '7') {
          int k;
          hi = 0;
          for (k = 0; k < 3 && *p >= '0' && *p <= '7'; k++) hi = hi * 8 + (*p++ - '0');
        }
        else hi = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p, p++;
      }
      else hi = *p++;
      if (hi < c) {
        tool_err(err, "tr", "range-endpoints of '%c-%c' are in reverse collating sequence order", c, hi);
        return -1;
      }
      for (; c <= hi; c++) buf_putc(outb, (char)c);
      continue;
    }
    if (c == 0) {	/* a real NUL: escaped so it is not the fill mark */
      buf_putc(outb, '\0');
      buf_putc(outb, '\0');
      buf_putc(outb, '\x02');
      continue;
    }
    buf_putc(outb, (char)c);
  }
  return 0;
}


/* resolves the NUL markers of tr_expand: fill ([c*]) to 'want' length */
static void tr_finish (Buf *b, size_t want) {
  Buf r;
  size_t i, fixed = 0;
  for (i = 0; i < b->len; i++) {
    if (b->s[i] == '\0' && i + 2 < b->len) {
      if (b->s[i + 2] == '\x02') fixed++;
      i += 2;
    }
    else fixed++;
  }
  buf_init(&r);
  for (i = 0; i < b->len; i++) {
    if (b->s[i] == '\0' && i + 2 < b->len) {
      if (b->s[i + 2] == '\x02') buf_putc(&r, '\0');
      else {
        size_t k;
        for (k = fixed; k < want; k++) buf_putc(&r, b->s[i + 1]);
        fixed = want;
      }
      i += 2;
    }
    else buf_putc(&r, b->s[i]);
  }
  buf_free(b);
  *b = r;
}


int t_tr (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {{"delete", 'd', 0}, {"squeeze-repeats", 's', 0},
    {"complement", 'c', 0}, {"truncate-set1", 't', 0}, {NULL, 0, 0}};
  Opts g;
  Out o;
  int c, del = 0, squeeze = 0, comp = 0, trunc = 0, k;
  Buf s1, s2;
  unsigned char map[256], in1[256], in2[256];
  char buf[65536];
  long n;
  int last = -1;
  opts_init(&g, "tr", argc, argv, err);
  while ((c = opts_next(&g, "dscCt", lo)) != 0) {
    switch (c) {
      case 'd': del = 1; break;
      case 's': squeeze = 1; break;
      case 'c': case 'C': comp = 1; break;
      case 't': trunc = 1; break;
      case OPT_HELP: opts_free(&g); return tool_help(out, "tr");
      default: opts_free(&g); return 1;
    }
  }
  if (g.ops.n == 0 || (g.ops.n == 1 && !del && !squeeze)) {
    tool_err(err, "tr", g.ops.n == 0 ? "missing operand" : "missing operand after '%s'",
             g.ops.n ? g.ops.v[0] : "");
    fd_printf(err, "Two strings must be given when translating.\n");
    opts_free(&g);
    return 1;
  }
  if (g.ops.n > 2 || (g.ops.n == 2 && del && !squeeze)) {
    tool_err(err, "tr", "extra operand '%s'", g.ops.v[g.ops.n - 1]);
    opts_free(&g);
    return 1;
  }
  buf_init(&s1);
  buf_init(&s2);
  if (tr_expand(g.ops.v[0], &s1, err, 0, NULL, NULL) != 0 ||
      (g.ops.n > 1 && tr_expand(g.ops.v[1], &s2, err, 1, NULL, NULL) != 0)) {
    buf_free(&s1);
    buf_free(&s2);
    opts_free(&g);
    return 1;
  }
  tr_finish(&s1, 0);
  memset(in1, 0, sizeof(in1));
  for (k = 0; k < (int)s1.len; k++) in1[(unsigned char)s1.s[k]] = 1;
  if (comp) {	/* the complement, in byte order */
    Buf cs;
    buf_init(&cs);
    for (k = 0; k < 256; k++)
      if (!in1[k]) buf_putc(&cs, (char)k);
    buf_free(&s1);
    s1 = cs;
    for (k = 0; k < 256; k++) in1[k] = !in1[k];
  }
  tr_finish(&s2, s1.len);
  memset(in2, 0, sizeof(in2));
  for (k = 0; k < (int)s2.len; k++) in2[(unsigned char)s2.s[k]] = 1;
  for (k = 0; k < 256; k++) map[k] = (unsigned char)k;
  if (!del && g.ops.n > 1) {
    size_t n1 = s1.len;
    if (trunc && s2.len < n1) n1 = s2.len;
    if (s2.len == 0 && n1 > 0) {
      tool_err(err, "tr", "when not truncating set1, string2 must be non-empty");
      buf_free(&s1);
      buf_free(&s2);
      opts_free(&g);
      return 1;
    }
    for (k = 0; k < (int)n1; k++) {
      unsigned char to = (unsigned char)s2.s[(size_t)k < s2.len ? (size_t)k : s2.len - 1];
      map[(unsigned char)s1.s[k]] = to;
    }
  }
  out_init(&o, out);
  while ((n = os_read(in, buf, sizeof(buf))) > 0 && !o.failed) {
    long i;
    for (i = 0; i < n; i++) {
      unsigned char ch = (unsigned char)buf[i];
      if (del && in1[ch]) continue;
      if (!del) ch = map[ch];
      if (squeeze) {
        const unsigned char *sq = (del || g.ops.n > 1) ? in2 : in1;
        if (sq[ch] && last == ch) continue;
      }
      last = ch;
      out_putc(&o, ch);
    }
    if (tool_stop()) break;
  }
  out_flush(&o);
  buf_free(&s1);
  buf_free(&s2);
  opts_free(&g);
  return 0;
}

/* }================================================================== */
