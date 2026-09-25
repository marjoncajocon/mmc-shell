/*
** cgrep.c - grep (and egrep, fgrep): print the lines that match
**
**   grep [-EFGiwxvcloqsnhHbrRz] [-e pat] [-f file] [-m N] [-A N -B N -C N]
**        [--color] [--include=glob --exclude=glob --exclude-dir=glob]
**        pattern [file...]
** All the patterns (-e, -f, lines of the pattern) become one expression
** with alternatives, so -o and --color see the leftmost-longest match.
*/

#include "mmc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


typedef struct Grep {
  Regex *re;
  int invert, count, list, list_not, only, quiet, silent, number, byte_off;
  int with_name, recursive, deref, null_name, zero, binary_text, skip_binary;
  int color, max_count, before, after, line_buffered, match_all;
  const char *label;
  Vec include, exclude, exclude_dir;
  int status;	/* 1 until something matches */
  int err_status;
  int in, err;
  int stop;	/* -q matched: stop everything */
  Out *o;
} Grep;

#define C_MATCH	"\033[01;31m\033[K"
#define C_NAME	"\033[35m\033[K"
#define C_NUM	"\033[32m\033[K"
#define C_SEP	"\033[36m\033[K"
#define C_END	"\033[m\033[K"


/* BRE text of a fixed string: the special characters escaped */
static void fixed_to_bre (Buf *b, const char *s, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    if (strchr("\\.[]*^$", s[i])) buf_putc(b, '\\');
    buf_putc(b, s[i]);
  }
}


static void put_prefix (Grep *g, const char *name, long long lineno, long long off, char sep) {
  Out *o = g->o;
  if (g->with_name) {
    if (g->color) out_printf(o, C_NAME "%s" C_END, name);
    else out_puts(o, name);
    if (g->null_name) out_putc(o, '\0');
    else if (g->color) out_printf(o, C_SEP "%c" C_END, sep);
    else out_putc(o, sep);
  }
  if (g->number) {
    if (g->color) out_printf(o, C_NUM "%lld" C_END C_SEP "%c" C_END, lineno, sep);
    else out_printf(o, "%lld%c", lineno, sep);
  }
  if (g->byte_off) {
    if (g->color) out_printf(o, C_NUM "%lld" C_END C_SEP "%c" C_END, off, sep);
    else out_printf(o, "%lld%c", off, sep);
  }
}


/* does the line match? (-v not applied) */
static int line_matches (Grep *g, const char *line, size_t len) {
  if (g->match_all) return 1;
  return regex_match(g->re, line, len, 0, 0, NULL);
}


static void put_line (Grep *g, const char *name, const char *line, size_t len,
                      long long lineno, long long off, char sep, int selected) {
  Out *o = g->o;
  char eol = g->zero ? '\0' : '\n';
  if (g->only) {	/* each match on its own line */
    size_t m[2 * 10], start = 0;
    size_t *mm = m;
    size_t need = 2 * ((size_t)regex_nsub(g->re) + 1);
    if (!selected || g->invert || g->match_all) return;
    if (need > 20) mm = (size_t *)xmalloc(need * sizeof(size_t));
    while (start <= len && regex_match(g->re, line, len, start, 0, mm)) {
      if (mm[1] > mm[0]) {
        put_prefix(g, name, lineno, off + (long long)mm[0], sep);
        if (g->color) out_puts(o, C_MATCH);
        out_putn(o, line + mm[0], mm[1] - mm[0]);
        if (g->color) out_puts(o, C_END);
        out_putc(o, eol);
        start = mm[1];
      }
      else start = mm[1] + 1;
    }
    if (mm != m) free(mm);
    return;
  }
  put_prefix(g, name, lineno, off, sep);
  if (g->color && selected && !g->invert && !g->match_all) {
    size_t m[2 * 10], start = 0, done = 0;
    size_t *mm = m;
    size_t need = 2 * ((size_t)regex_nsub(g->re) + 1);
    if (need > 20) mm = (size_t *)xmalloc(need * sizeof(size_t));
    while (start <= len && regex_match(g->re, line, len, start, 0, mm)) {
      if (mm[1] > mm[0]) {
        out_putn(o, line + done, mm[0] - done);
        out_puts(o, C_MATCH);
        out_putn(o, line + mm[0], mm[1] - mm[0]);
        out_puts(o, C_END);
        done = start = mm[1];
      }
      else start = mm[1] + 1;
    }
    out_putn(o, line + done, len - done);
    if (mm != m) free(mm);
  }
  else out_putn(o, line, len);
  out_putc(o, eol);
}


typedef struct Held {	/* a line kept for -B context */
  char *s;
  size_t len;
  long long lineno, off;
} Held;


/* one file (or stdin); name is what is shown */
static void grep_fd (Grep *g, int fd, const char *name) {
  In r;
  char *line;
  size_t len;
  int had;
  long long lineno = 0, off = 0, selected_n = 0, last_printed = 0;
  int after_left = 0, binary = 0, any_printed = 0, checked_binary = 0;
  Held *ring = NULL;
  int ring_n = 0, ring_at = 0;
  char eol = g->zero ? '\0' : '\n';
  if (g->before > 0) ring = (Held *)xmalloc((size_t)g->before * sizeof(Held));
  in_init(&r, fd, 0);
  while (!g->stop && !g->o->failed && in_line(&r, &line, &len, eol, &had)) {
    int sel;
    long long this_off = off;
    lineno++;
    off += (long long)len + (had ? 1 : 0);
    if (!checked_binary) {	/* a NUL in the first block: a binary file */
      checked_binary = 1;
      if (!g->binary_text && !g->zero &&
          (memchr(line, '\0', len) != NULL || memchr(r.buf + r.start, '\0', r.end - r.start) != NULL))
        binary = 1;
      if (binary && g->skip_binary) break;
    }
    sel = line_matches(g, line, len) != g->invert;
    if (sel) {
      selected_n++;
      g->status = 0;
      if (g->quiet) {
        g->stop = 1;
        break;
      }
      if (g->list || g->list_not) break;
      if (!g->count) {
        if (binary) {
          out_flush(g->o);
          fd_printf(g->o->fd, "Binary file %s matches\n", name);
          break;
        }
        /* context: a "--" between groups that are not next to each other */
        if ((g->before > 0 || g->after > 0) && any_printed &&
            lineno - (long long)ring_n > last_printed + 1) {
          if (g->color) out_puts(g->o, C_SEP "--" C_END "\n");
          else out_puts(g->o, "--\n");
        }
        {
          int k;
          for (k = 0; k < ring_n; k++) {
            Held *h = &ring[(ring_at + k) % g->before];
            put_line(g, name, h->s, h->len, h->lineno, h->off, '-', 0);
            free(h->s);
          }
          ring_n = ring_at = 0;
        }
        put_line(g, name, line, len, lineno, this_off, ':', 1);
        any_printed = 1;
        last_printed = lineno;
        after_left = g->after;
      }
      if (g->max_count >= 0 && selected_n >= g->max_count) {
        /* -m: the after-context still comes */
        while (after_left > 0 && !g->count && in_line(&r, &line, &len, eol, &had)) {
          lineno++;
          if (line_matches(g, line, len) != g->invert) break;
          put_line(g, name, line, len, lineno, off, '-', 0);
          off += (long long)len + (had ? 1 : 0);
          last_printed = lineno;
          after_left--;
        }
        break;
      }
    }
    else if (!g->count && !g->list && !g->list_not && !binary) {
      if (after_left > 0) {
        put_line(g, name, line, len, lineno, this_off, '-', 0);
        last_printed = lineno;
        after_left--;
      }
      else if (g->before > 0) {
        Held *h;
        if (ring_n == g->before) {
          free(ring[ring_at].s);
          ring_at = (ring_at + 1) % g->before;
          ring_n--;
        }
        h = &ring[(ring_at + ring_n) % g->before];
        h->s = xstrndup(line, len);
        h->len = len;
        h->lineno = lineno;
        h->off = this_off;
        ring_n++;
      }
    }
    if (g->line_buffered) out_flush(g->o);
  }
  while (ring_n > 0) {
    free(ring[ring_at].s);
    ring_at = (ring_at + 1) % (g->before ? g->before : 1);
    ring_n--;
  }
  free(ring);
  r.fd = -1;
  in_close(&r);
  if (g->count) {
    if (g->with_name) {
      if (g->color) out_printf(g->o, C_NAME "%s" C_END C_SEP ":" C_END, name);
      else out_printf(g->o, "%s:", name);
    }
    out_printf(g->o, "%lld\n", selected_n);
  }
  if (g->list && selected_n > 0) {
    if (g->color) out_printf(g->o, C_NAME "%s" C_END, name);
    else out_puts(g->o, name);
    out_putc(g->o, g->null_name ? '\0' : '\n');
  }
  if (g->list_not && selected_n == 0) {
    out_puts(g->o, name);
    out_putc(g->o, g->null_name ? '\0' : '\n');
  }
}


static int glob_any (const Vec *v, const char *name) {
  size_t i;
  for (i = 0; i < v->n; i++)
    if (pat_match(v->v[i], name, 0)) return 1;
  return 0;
}


static void grep_path (Grep *g, const char *disp, const char *native, int top) {
  OsStat st;
  int fd;
  if (g->stop || tool_stop()) return;
  if (((g->deref || top) ? os_stat(native, &st) : os_lstat(native, &st)) != 0) {
    if (!g->silent) {
      out_flush(g->o);
      tool_err(g->err, "grep", "%s: %s", disp, os_errmsg());
    }
    g->err_status = 1;
    return;
  }
  if (st.is_link && !top && !g->deref) return;	/* -r: links met on the way are skipped */
  if (st.is_dir) {
    Vec names;
    size_t i;
    if (!g->recursive) {
      if (!g->silent) {
        out_flush(g->o);
        tool_err(g->err, "grep", "%s: Is a directory", disp);
      }
      g->err_status = 1;
      return;
    }
    if (!top && glob_any(&g->exclude_dir, tool_base(disp))) return;
    vec_init(&names);
    if (os_listdir(native, &names) != 0) {
      if (!g->silent) {
        out_flush(g->o);
        tool_err(g->err, "grep", "%s: %s", disp, os_errmsg());
      }
      g->err_status = 1;
    }
    vec_sort(&names);
    for (i = 0; i < names.n && !g->stop; i++) {
      char *cd = strcmp(disp, ".") == 0 && top && g->recursive == 2 ? xstrdup(names.v[i]) :
                 tool_join(disp, names.v[i]);
      char *cn = path_join(native, names.v[i]);
      grep_path(g, cd, cn, 0);
      free(cd);
      free(cn);
    }
    vec_free(&names);
    return;
  }
  if (!top || g->recursive) {
    const char *base = tool_base(disp);
    if (g->include.n > 0 && !glob_any(&g->include, base)) return;
    if (glob_any(&g->exclude, base)) return;
  }
  fd = os_open(native, OS_READ);
  if (fd < 0) {
    if (!g->silent) {
      out_flush(g->o);
      tool_err(g->err, "grep", "%s: %s", disp, os_errmsg());
    }
    g->err_status = 1;
    return;
  }
  grep_fd(g, fd, disp);
  os_close(fd);
}


static int grep_main (int argc, char **argv, int in, int out, int err, int mode) {
  static const LongOpt lo[] = {
    {"extended-regexp", 'E', 0}, {"fixed-strings", 'F', 0}, {"basic-regexp", 'G', 0},
    {"perl-regexp", 'P', 0}, {"regexp", 'e', 1}, {"file", 'f', 1}, {"ignore-case", 'i', 0},
    {"no-ignore-case", 1001, 0}, {"word-regexp", 'w', 0}, {"line-regexp", 'x', 0},
    {"invert-match", 'v', 0}, {"count", 'c', 0}, {"files-with-matches", 'l', 0},
    {"files-without-match", 'L', 0}, {"only-matching", 'o', 0}, {"quiet", 'q', 0},
    {"silent", 'q', 0}, {"no-messages", 's', 0}, {"line-number", 'n', 0},
    {"with-filename", 'H', 0}, {"no-filename", 'h', 0}, {"byte-offset", 'b', 0},
    {"recursive", 'r', 0}, {"dereference-recursive", 'R', 0}, {"include", 1002, 1},
    {"exclude", 1003, 1}, {"exclude-dir", 1004, 1}, {"after-context", 'A', 1},
    {"before-context", 'B', 1}, {"context", 'C', 1}, {"max-count", 'm', 1},
    {"color", 1005, 2}, {"colour", 1005, 2}, {"text", 'a', 0}, {"null", 'Z', 0},
    {"null-data", 'z', 0}, {"label", 1006, 1}, {"line-buffered", 1007, 0},
    {"binary-files", 1008, 1}, {"initial-tab", 'T', 0}, {"binary", 'U', 0},
    {"devices", 'D', 1}, {"directories", 'd', 1}, {NULL, 0, 0}};
  Grep g;
  Opts op;
  Out o;
  Vec pats;
  int c, have_pat = 0, icase = 0, words = 0, whole = 0, color_when = 0, no_name = -1, k;
  const char *tool = mode == 'E' ? "egrep" : mode == 'F' ? "fgrep" : "grep";
  size_t i;
  char *emsg = NULL;
  Buf all;
  int nargc = argc;
  char **nargv = argv;
  char ctx[32];
  memset(&g, 0, sizeof(g));
  g.status = 1;
  g.max_count = -1;
  g.in = in;
  g.err = err;
  vec_init(&g.include);
  vec_init(&g.exclude);
  vec_init(&g.exclude_dir);
  vec_init(&pats);
  /* -5 is -C 5 */
  opts_init(&op, "grep", nargc, nargv, err);
  while ((c = opts_next(&op, "EFGPe:f:iywxvclLoqsnHhbrRA:B:C:m:aZzTUD:d:0123456789", lo)) != 0) {
    switch (c) {
      case 'E': case 'P': mode = 'E'; break;
      case 'F': mode = 'F'; break;
      case 'G': mode = 'G'; break;
      case 'e': vec_push(&pats, xstrdup(op.arg)); have_pat = 1; break;
      case 'f': {
        char *native = path_to_native(op.arg), *text;
        size_t len;
        text = strcmp(op.arg, "-") == 0 ? NULL : read_file(native, &len);
        free(native);
        if (text == NULL) {
          tool_err(err, tool, "%s: %s", op.arg, os_errmsg());
          goto bad;
        }
        {
          char *p = text, *nl;
          while (*p) {
            nl = strchr(p, '\n');
            if (nl) *nl = '\0';
            if (nl && nl > p && nl[-1] == '\r') nl[-1] = '\0';
            vec_push(&pats, xstrdup(p));
            if (!nl) break;
            p = nl + 1;
          }
        }
        free(text);
        have_pat = 1;
        break;
      }
      case 'i': case 'y': icase = 1; break;
      case 1001: icase = 0; break;
      case 'w': words = 1; break;
      case 'x': whole = 1; break;
      case 'v': g.invert = 1; break;
      case 'c': g.count = 1; break;
      case 'l': g.list = 1; g.list_not = 0; break;
      case 'L': g.list_not = 1; g.list = 0; break;
      case 'o': g.only = 1; break;
      case 'q': g.quiet = 1; break;
      case 's': g.silent = 1; break;
      case 'n': g.number = 1; break;
      case 'H': no_name = 0; break;
      case 'h': no_name = 1; break;
      case 'b': g.byte_off = 1; break;
      case 'r': g.recursive = 1; break;
      case 'R': g.recursive = 1; g.deref = 1; break;
      case 'd':
        if (strcmp(op.arg, "recurse") == 0) g.recursive = 1;
        break;
      case 1002: vec_push(&g.include, glob_mark(op.arg)); break;
      case 1003: vec_push(&g.exclude, glob_mark(op.arg)); break;
      case 1004: vec_push(&g.exclude_dir, glob_mark(op.arg)); break;
      case 'A': g.after = atoi(op.arg); break;
      case 'B': g.before = atoi(op.arg); break;
      case 'C': g.after = g.before = atoi(op.arg); break;
      case 'm': g.max_count = atoi(op.arg); break;
      case 1005:
        if (op.arg == NULL || strcmp(op.arg, "always") == 0 || strcmp(op.arg, "yes") == 0 ||
            strcmp(op.arg, "force") == 0) color_when = 1;
        else if (strcmp(op.arg, "auto") == 0 || strcmp(op.arg, "tty") == 0 ||
                 strcmp(op.arg, "if-tty") == 0) color_when = 2;
        else color_when = 0;
        break;
      case 'a': g.binary_text = 1; break;
      case 1008:
        if (strcmp(op.arg, "text") == 0) g.binary_text = 1;
        else if (strcmp(op.arg, "without-match") == 0) g.skip_binary = 1;
        break;
      case 'Z': g.null_name = 1; break;
      case 'z': g.zero = 1; break;
      case 1006: g.label = op.arg; break;
      case 1007: g.line_buffered = 1; break;
      case 'T': case 'U': case 'D': break;
      case OPT_HELP: opts_free(&op); vec_free(&pats); return tool_help(out, "grep");
      default:
        if (c >= '0' && c <= '9') {	/* -5: context 5 (digits may follow) */
          int n = c - '0';
          while (op.cluster && isdigit((unsigned char)*op.cluster)) n = n * 10 + (*op.cluster++ - '0');
          g.after = g.before = n;
          break;
        }
        goto bad;
    }
  }
  if (!have_pat) {
    if (op.ops.n == 0) {
      fd_printf(err, "Usage: %s [OPTION]... PATTERNS [FILE]...\n", tool);
      fd_printf(err, "Try '%s --help' for more information.\n", tool);
      goto bad;
    }
    vec_push(&pats, xstrdup(op.ops.v[0]));
    free(op.ops.v[0]);
    memmove(op.ops.v, op.ops.v + 1, op.ops.n * sizeof(char *));
    op.ops.n--;
  }
  /* one expression: the patterns (and the lines in them) as alternatives */
  buf_init(&all);
  {
    int first = 1;
    for (i = 0; i < pats.n; i++) {
      const char *p = pats.v[i];
      for (;;) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        if (!first) buf_puts(&all, mode == 'E' ? "|" : "\\|");
        first = 0;
        if (mode == 'F') fixed_to_bre(&all, p, n);
        else buf_putn(&all, p, n);
        if (!nl) break;
        p = nl + 1;
      }
    }
  }
  g.re = regex_new_n(all.s ? all.s : "", all.len,
                     (mode == 'E' ? RE_EXTENDED : 0) | (icase ? RE_ICASE : 0) |
                     (words ? RE_WORDS : 0) | (whole ? RE_WHOLE : 0), &emsg);
  buf_free(&all);
  if (g.re == NULL) {
    tool_err(err, tool, "%s", emsg ? emsg : "invalid regular expression");
    free(emsg);
    goto bad;
  }
  out_init(&o, out);
  g.o = &o;
  g.color = color_when == 1 || (color_when == 2 && os_is_tty(out));
  if (g.recursive && op.ops.n == 0) {
    vec_push(&op.ops, xstrdup("."));
    g.recursive = 2;	/* names without "./" */
  }
  g.with_name = no_name >= 0 ? !no_name : (op.ops.n > 1 || g.recursive);
  if (op.ops.n == 0) vec_push(&op.ops, xstrdup("-"));
  (void)ctx;
  for (i = 0; i < op.ops.n && !g.stop && !tool_stop(); i++) {
    const char *name = op.ops.v[i];
    if (strcmp(name, "-") == 0) grep_fd(&g, in, g.label ? g.label : "(standard input)");
    else {
      char *native = path_to_native(name);
      grep_path(&g, name, native, 1);
      free(native);
    }
  }
  out_flush(&o);
  regex_free(g.re);
  vec_free(&pats);
  vec_free(&g.include);
  vec_free(&g.exclude);
  vec_free(&g.exclude_dir);
  opts_free(&op);
  (void)k;
  if (g.quiet && g.status == 0) return 0;
  return g.err_status && !(g.silent && g.status == 0) ? 2 : g.status;
bad:
  vec_free(&pats);
  vec_free(&g.include);
  vec_free(&g.exclude);
  vec_free(&g.exclude_dir);
  opts_free(&op);
  return 2;
}


int t_grep (int argc, char **argv, int in, int out, int err) {
  return grep_main(argc, argv, in, out, err, 'G');
}


int t_egrep (int argc, char **argv, int in, int out, int err) {
  return grep_main(argc, argv, in, out, err, 'E');
}


int t_fgrep (int argc, char **argv, int in, int out, int err) {
  return grep_main(argc, argv, in, out, err, 'F');
}
