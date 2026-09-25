/*
** csys.c - the system tools: chmod du df file uname hostname whoami id ps
** watch cal
*/

#include "mmc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*
** {==================================================================
** chmod
** ===================================================================
*/

typedef struct Chmod {
  const char *spec;
  unsigned ref_mode;
  int use_ref, recursive, verbose, changes, quiet, status;
  Out *o;
  int err;
} Chmod;


static void chmod_one (Chmod *c, const char *disp, const char *native, int top) {
  OsStat st;
  unsigned mode;
  char a[11], b[11];
  if (tool_stop()) return;
  if ((top ? os_stat(native, &st) : os_lstat(native, &st)) != 0) {
    if (!c->quiet) {
      out_flush(c->o);
      tool_err(c->err, "chmod", "cannot access '%s': %s", disp, os_errmsg());
    }
    c->status = 1;
    return;
  }
  if (st.is_link) return;	/* -R does not follow links */
  if (c->use_ref) mode = c->ref_mode;
  else if (parse_mode(c->spec, st.mode, st.is_dir, &mode) != 0) {
    tool_err(c->err, "chmod", "invalid mode: '%s'", c->spec);
    c->status = 1;
    return;
  }
  if (os_chmod(native, mode) != 0) {
    if (!c->quiet) {
      out_flush(c->o);
      tool_err(c->err, "chmod", "changing permissions of '%s': %s", disp, os_errmsg());
    }
    c->status = 1;
  }
  else if (c->verbose || c->changes) {
    OsStat now;
    unsigned nm = mode;
    if (os_stat(native, &now) == 0) nm = now.mode;
    mode_string(a, &st);
    now = st;
    now.mode = nm;
    mode_string(b, &now);
    if ((st.mode & 07777) != (nm & 07777))
      out_printf(c->o, "mode of '%s' changed from %04o (%s) to %04o (%s)\n", disp,
                 st.mode & 07777, a + 1, nm & 07777, b + 1);
    else if (c->verbose)
      out_printf(c->o, "mode of '%s' retained as %04o (%s)\n", disp, st.mode & 07777, a + 1);
  }
  if (c->recursive && st.is_dir) {
    Vec names;
    size_t i;
    vec_init(&names);
    if (os_listdir(native, &names) != 0) {
      if (!c->quiet) {
        out_flush(c->o);
        tool_err(c->err, "chmod", "cannot read directory '%s': %s", disp, os_errmsg());
      }
      c->status = 1;
    }
    for (i = 0; i < names.n; i++) {
      char *cd = tool_join(disp, names.v[i]), *cn = path_join(native, names.v[i]);
      chmod_one(c, cd, cn, 0);
      free(cd);
      free(cn);
    }
    vec_free(&names);
  }
}


int t_chmod (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {{"recursive", 'R', 0}, {"verbose", 'v', 0}, {"changes", 'c', 0},
    {"silent", 'f', 0}, {"quiet", 'f', 0}, {"reference", 1001, 1}, {"no-preserve-root", 1002, 0},
    {"preserve-root", 1003, 0}, {NULL, 0, 0}};
  Chmod c;
  Opts g;
  Out o;
  int ch, i, ac = 0;
  size_t k, first;
  char **av = (char **)xmalloc(((size_t)argc + 1) * sizeof(char *));
  const char *dash_mode = NULL;
  (void)in;
  memset(&c, 0, sizeof(c));
  c.err = err;
  /* "chmod -w file": -w is a mode, not an option */
  for (i = 0; i < argc; i++) {
    if (i > 0 && dash_mode == NULL && argv[i][0] == '-' && argv[i][1] &&
        strspn(argv[i] + 1, "rwxXstugoa+-=,01234567") == strlen(argv[i] + 1) &&
        strspn(argv[i] + 1, "rwxXst") > 0) {
      dash_mode = argv[i];
      continue;
    }
    av[ac++] = argv[i];
  }
  av[ac] = NULL;
  opts_init(&g, "chmod", ac, av, err);
  while ((ch = opts_next(&g, "Rvcf", lo)) != 0) {
    switch (ch) {
      case 'R': c.recursive = 1; break;
      case 'v': c.verbose = 1; break;
      case 'c': c.changes = 1; break;
      case 'f': c.quiet = 1; break;
      case 1001: {
        char *native = path_to_native(g.arg);
        OsStat st;
        if (os_stat(native, &st) != 0) {
          tool_err(err, "chmod", "failed to get attributes of '%s': %s", g.arg, os_errmsg());
          free(native);
          opts_free(&g);
          free(av);
          return 1;
        }
        free(native);
        c.use_ref = 1;
        c.ref_mode = st.mode & 07777;
        break;
      }
      case 1002: case 1003: break;
      case OPT_HELP: opts_free(&g); free(av); return tool_help(out, "chmod");
      default:
        opts_free(&g);
        free(av);
        return 1;
    }
  }
  first = 0;
  if (dash_mode) c.spec = dash_mode;
  else if (!c.use_ref) {
    if (g.ops.n == 0) {
      tool_err(err, "chmod", "missing operand");
      opts_free(&g);
      free(av);
      return 1;
    }
    c.spec = g.ops.v[0];
    first = 1;
  }
  if (!c.use_ref) {
    unsigned probe;
    if (parse_mode(c.spec, 0644, 0, &probe) != 0) {
      tool_err(err, "chmod", "invalid mode: '%s'", c.spec);
      fd_printf(err, "Try 'chmod --help' for more information.\n");
      opts_free(&g);
      free(av);
      return 1;
    }
  }
  if (g.ops.n <= first) {
    tool_err(err, "chmod", "missing operand after '%s'", c.spec ? c.spec : "");
    opts_free(&g);
    free(av);
    return 1;
  }
  out_init(&o, out);
  c.o = &o;
  for (k = first; k < g.ops.n; k++) {
    char *native = path_to_native(g.ops.v[k]);
    chmod_one(&c, g.ops.v[k], native, 1);
    free(native);
  }
  out_flush(&o);
  opts_free(&g);
  free(av);
  return c.status;
}

/* }================================================================== */


/*
** {==================================================================
** du
** ===================================================================
*/

typedef struct Du {
  int all, summarize, human, si, bytes, apparent, total, deref, status, null;
  long long unit;	/* the size shown is divided by this, rounded up */
  int maxdepth;
  Vec exclude;
  Out *o;
  int err;
  unsigned long long *seen;	/* dev, ino of files with more links */
  size_t nseen;
} Du;


static void du_put (Du *d, long long size, const char *disp) {
  char buf[32];
  if (d->human) human_size(buf, (unsigned long long)size, d->si);
  else sprintf(buf, "%lld", (size + d->unit - 1) / d->unit);
  out_printf(d->o, "%s\t%s%c", buf, disp, d->null ? '\0' : '\n');
}


static long long du_walk (Du *d, const char *disp, const char *native, int depth) {
  OsStat st;
  long long size;
  size_t i;
  if (tool_stop()) return 0;
  if (((d->deref || depth == 0) ? os_stat(native, &st) : os_lstat(native, &st)) != 0) {
    out_flush(d->o);
    tool_err(d->err, "du", "cannot access '%s': %s", disp, os_errmsg());
    d->status = 1;
    return 0;
  }
  if (d->exclude.n > 0 && depth > 0) {
    for (i = 0; i < d->exclude.n; i++)
      if (pat_match(d->exclude.v[i], tool_base(disp), 0)) return 0;
  }
  if (st.nlink > 1 && !st.is_dir && st.ino != 0) {	/* a hard link counts once */
    for (i = 0; i < d->nseen; i++)
      if (d->seen[2 * i] == st.dev && d->seen[2 * i + 1] == st.ino) return 0;
    d->seen = (unsigned long long *)xrealloc(d->seen, (d->nseen + 1) * 2 * sizeof(unsigned long long));
    d->seen[2 * d->nseen] = st.dev;
    d->seen[2 * d->nseen + 1] = st.ino;
    d->nseen++;
  }
  size = d->apparent ? st.size : st.blocks * 512;
  if (st.is_dir && !st.is_link) {
    Vec names;
    vec_init(&names);
    if (os_listdir(native, &names) != 0) {
      out_flush(d->o);
      tool_err(d->err, "du", "cannot read directory '%s': %s", disp, os_errmsg());
      d->status = 1;
    }
    for (i = 0; i < names.n && !tool_stop(); i++) {
      char *cd = tool_join(disp, names.v[i]), *cn = path_join(native, names.v[i]);
      size += du_walk(d, cd, cn, depth + 1);
      free(cd);
      free(cn);
    }
    vec_free(&names);
    if (!d->summarize && (d->maxdepth < 0 || depth <= d->maxdepth)) du_put(d, size, disp);
  }
  else if ((d->all && !d->summarize && (d->maxdepth < 0 || depth <= d->maxdepth)) || depth == 0)
    du_put(d, size, disp);
  return size;
}


int t_du (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {{"all", 'a', 0}, {"summarize", 's', 0}, {"human-readable", 'h', 0},
    {"si", 1001, 0}, {"bytes", 'b', 0}, {"apparent-size", 1002, 0}, {"total", 'c', 0},
    {"max-depth", 'd', 1}, {"dereference", 'L', 0}, {"exclude", 1003, 1},
    {"null", '0', 0}, {"block-size", 'B', 1}, {"one-file-system", 'x', 0}, {NULL, 0, 0}};
  Du d;
  Opts g;
  Out o;
  int c;
  size_t i;
  long long total = 0;
  (void)in;
  memset(&d, 0, sizeof(d));
  d.maxdepth = -1;
  d.unit = 1024;
  d.err = err;
  vec_init(&d.exclude);
  opts_init(&g, "du", argc, argv, err);
  while ((c = opts_next(&g, "ashbcd:LkmB:x0SHD", lo)) != 0) {
    switch (c) {
      case 'a': d.all = 1; break;
      case 's': d.summarize = 1; break;
      case 'h': d.human = 1; break;
      case 1001: d.human = d.si = 1; break;
      case 'b': d.apparent = 1; d.unit = 1; break;
      case 1002: d.apparent = 1; break;
      case 'c': d.total = 1; break;
      case 'd': d.maxdepth = atoi(g.arg); break;
      case 'L': d.deref = 1; break;
      case 'k': d.unit = 1024; break;
      case 'm': d.unit = 1048576; break;
      case 'B':
        if (parse_size(g.arg, &d.unit) != 0 || d.unit <= 0) {
          tool_err(err, "du", "invalid block size '%s'", g.arg);
          opts_free(&g);
          vec_free(&d.exclude);
          return 1;
        }
        break;
      case '0': d.null = 1; break;
      case 1003: vec_push(&d.exclude, glob_mark(g.arg)); break;
      case 'x': case 'S': case 'H': case 'D': break;
      case OPT_HELP: opts_free(&g); vec_free(&d.exclude); return tool_help(out, "du");
      default: opts_free(&g); vec_free(&d.exclude); return 1;
    }
  }
  if (d.summarize && d.all) {
    tool_err(err, "du", "cannot both summarize and show all entries");
    opts_free(&g);
    vec_free(&d.exclude);
    return 1;
  }
  if (g.ops.n == 0) vec_push(&g.ops, xstrdup("."));
  out_init(&o, out);
  d.o = &o;
  for (i = 0; i < g.ops.n; i++) {
    char *native = path_to_native(g.ops.v[i]);
    long long s = du_walk(&d, g.ops.v[i], native, 0);
    OsStat st;
    if (d.summarize && os_stat(native, &st) == 0 && st.is_dir) du_put(&d, s, g.ops.v[i]);
    total += s;
    free(native);
  }
  if (d.total) du_put(&d, total, "total");
  out_flush(&o);
  opts_free(&g);
  vec_free(&d.exclude);
  free(d.seen);
  return tool_stop() ? 130 : d.status;
}

/* }================================================================== */


/*
** {==================================================================
** df
** ===================================================================
*/

typedef struct DfRow {
  char *dev, *mnt, *type;	/* mnt as shown */
  char *native;
  unsigned long long total, avail, free_;
} DfRow;


static int df_skip_type (const char *t) {
  static const char *const pseudo[] = {"proc", "sysfs", "devpts", "cgroup", "cgroup2", "pstore",
    "securityfs", "debugfs", "tracefs", "configfs", "fusectl", "mqueue", "hugetlbfs", "bpf",
    "binfmt_misc", "autofs", "rpc_pipefs", "nsfs", "devfs", "autofs", "efivarfs", "selinuxfs", NULL};
  int i;
  for (i = 0; pseudo[i]; i++)
    if (strcmp(t, pseudo[i]) == 0) return 1;
  return 0;
}


int t_df (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {{"human-readable", 'h', 0}, {"si", 'H', 0}, {"all", 'a', 0},
    {"print-type", 'T', 0}, {"portability", 'P', 0}, {"block-size", 'B', 1}, {"local", 'l', 0},
    {"total", 1001, 0}, {"type", 't', 1}, {"exclude-type", 'x', 1}, {"inodes", 'i', 0}, {NULL, 0, 0}};
  Opts g;
  Out o;
  int c, human = 0, si = 0, all = 0, types = 0, status = 0, want_total = 0;
  long long unit = 1024;
  const char *only_type = NULL, *ex_type = NULL;
  Vec mounts;
  DfRow *rows = NULL;
  size_t nrows = 0, i, k;
  int wdev = 10, wsize = 9;
  unsigned long long tt = 0, ta = 0, tf = 0;
  (void)in;
  opts_init(&g, "df", argc, argv, err);
  while ((c = opts_next(&g, "hHaTPB:lkmt:x:i", lo)) != 0) {
    switch (c) {
      case 'h': human = 1; si = 0; break;
      case 'H': human = si = 1; break;
      case 'a': all = 1; break;
      case 'T': types = 1; break;
      case 'k': unit = 1024; break;
      case 'm': unit = 1048576; break;
      case 'B':
        if (parse_size(g.arg, &unit) != 0 || unit <= 0) {
          tool_err(err, "df", "invalid block size '%s'", g.arg);
          opts_free(&g);
          return 1;
        }
        break;
      case 't': only_type = g.arg; break;
      case 'x': ex_type = g.arg; break;
      case 1001: want_total = 1; break;
      case 'P': case 'l': case 'i': break;
      case OPT_HELP: opts_free(&g); return tool_help(out, "df");
      default: opts_free(&g); return 1;
    }
  }
  vec_init(&mounts);
  os_mounts(&mounts);
  for (i = 0; i < mounts.n; i++) {
    char *f1 = mounts.v[i], *f2 = strchr(f1, '\t'), *f3;
    DfRow r;
    if (f2 == NULL || (f3 = strchr(f2 + 1, '\t')) == NULL) continue;
    *f2++ = '\0';
    *f3++ = '\0';
    memset(&r, 0, sizeof(r));
    r.dev = xstrdup(f1);
    r.native = xstrdup(f2);
#ifdef _WIN32
    r.mnt = path_to_drive(f2);
    {
      size_t n = strlen(r.mnt);
      while (n > 2 && r.mnt[n - 1] == '/') r.mnt[--n] = '\0';
    }
#else
    r.mnt = xstrdup(f2);
#endif
    r.type = xstrdup(f3);
    if (os_diskfree(r.native, &r.total, &r.avail, &r.free_) != 0) r.total = r.avail = r.free_ = 0;
    rows = (DfRow *)xrealloc(rows, (nrows + 1) * sizeof(DfRow));
    rows[nrows++] = r;
  }
  vec_free(&mounts);
  out_init(&o, out);
  /* which rows */
  {
    int *show = (int *)xmalloc((nrows + 1) * sizeof(int));
    for (i = 0; i < nrows; i++) {
      show[i] = all || (rows[i].total > 0 && !df_skip_type(rows[i].type));
      if (only_type && strcmp(rows[i].type, only_type) != 0) show[i] = 0;
      if (ex_type && strcmp(rows[i].type, ex_type) == 0) show[i] = 0;
    }
    if (g.ops.n > 0) {	/* only the file systems of the files named */
      int *want = (int *)xmalloc((nrows + 1) * sizeof(int));
      for (i = 0; i < nrows; i++) want[i] = 0;
      for (k = 0; k < g.ops.n; k++) {
        char *native = path_to_native(g.ops.v[k]), *real;
        size_t best = 0, bestlen = 0;
        int found = 0;
        OsStat st;
        if (os_stat(native, &st) != 0) {
          tool_err(err, "df", "%s: %s", g.ops.v[k], os_errmsg());
          status = 1;
          free(native);
          continue;
        }
        real = os_realpath(native);
        if (real == NULL) real = xstrdup(native);
        for (i = 0; i < nrows; i++) {	/* the longest mount point that holds it */
          size_t n = strlen(rows[i].native);
          while (n > 1 && path_is_sep(rows[i].native[n - 1])) n--;
          if (m_fnncmp(real, rows[i].native, n) == 0 &&
              (real[n] == '\0' || path_is_sep(real[n]) || n == 1 || path_is_sep(rows[i].native[n])) && n >= bestlen) {
            best = i;
            bestlen = n;
            found = 1;
          }
        }
        if (found) want[best] = 1;
        free(real);
        free(native);
      }
      for (i = 0; i < nrows; i++) show[i] = want[i];
      free(want);
    }
    for (i = 0; i < nrows; i++) {
      if (!show[i]) continue;
      if ((int)strlen(rows[i].dev) > wdev) wdev = (int)strlen(rows[i].dev);
    }
    if (human) wsize = 5;
    {
      const char *sizecol = human ? "Size" : unit == 1024 ? "1K-blocks" : unit == 1048576 ? "1M-blocks" : "Blocks";
      out_printf(&o, "%-*s ", wdev, "Filesystem");
      if (types) out_printf(&o, "%-8s ", "Type");
      out_printf(&o, "%*s %*s %*s Use%% Mounted on\n", wsize, sizecol, wsize, "Used",
                 wsize, human ? "Avail" : "Available");
    }
    for (i = 0; i < nrows; i++) {
      DfRow *r = &rows[i];
      unsigned long long used = r->total >= r->free_ ? r->total - r->free_ : 0;
      char a[32], b[32], cc[32], pct[8];
      if (!show[i]) continue;
      tt += r->total;
      ta += r->avail;
      tf += r->free_;
      if (human) {
        human_size(a, r->total, si);
        human_size(b, used, si);
        human_size(cc, r->avail, si);
      }
      else {
        sprintf(a, "%llu", (r->total + (unsigned long long)unit - 1) / (unsigned long long)unit);
        sprintf(b, "%llu", (used + (unsigned long long)unit - 1) / (unsigned long long)unit);
        sprintf(cc, "%llu", r->avail / (unsigned long long)unit);
      }
      if (used + r->avail == 0) strcpy(pct, "-");
      else sprintf(pct, "%llu%%", (used * 100 + used + r->avail - 1) / (used + r->avail));
      out_printf(&o, "%-*s ", wdev, r->dev);
      if (types) out_printf(&o, "%-8s ", r->type);
      out_printf(&o, "%*s %*s %*s %4s %s\n", wsize, a, wsize, b, wsize, cc, pct, r->mnt);
    }
    if (want_total) {
      unsigned long long used = tt >= tf ? tt - tf : 0;
      char a[32], b[32], cc[32], pct[8];
      if (human) {
        human_size(a, tt, si);
        human_size(b, used, si);
        human_size(cc, ta, si);
      }
      else {
        sprintf(a, "%llu", tt / (unsigned long long)unit);
        sprintf(b, "%llu", used / (unsigned long long)unit);
        sprintf(cc, "%llu", ta / (unsigned long long)unit);
      }
      if (used + ta == 0) strcpy(pct, "-");
      else sprintf(pct, "%llu%%", (used * 100 + used + ta - 1) / (used + ta));
      out_printf(&o, "%-*s ", wdev, "total");
      if (types) out_printf(&o, "%-8s ", "-");
      out_printf(&o, "%*s %*s %*s %4s -\n", wsize, a, wsize, b, wsize, cc, pct);
    }
    free(show);
  }
  for (i = 0; i < nrows; i++) {
    free(rows[i].dev);
    free(rows[i].mnt);
    free(rows[i].type);
    free(rows[i].native);
  }
  free(rows);
  out_flush(&o);
  opts_free(&g);
  return status;
}

/* }================================================================== */


/*
** {==================================================================
** file: what a file holds, from its first bytes
** ===================================================================
*/

static unsigned get16le (const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static unsigned long get32le (const unsigned char *p) {
  return (unsigned long)p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}
static unsigned long get32be (const unsigned char *p) {
  return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) | (unsigned long)p[3];
}


static const char *elf_machine (unsigned m) {
  switch (m) {
    case 3: return "Intel 80386";
    case 40: return "ARM";
    case 62: return "x86-64";
    case 183: return "ARM aarch64";
    case 243: return "UCB RISC-V";
    case 8: return "MIPS";
    case 20: return "PowerPC";
    case 21: return "64-bit PowerPC";
  }
  return "unknown arch";
}


/* a description of the file's data (and its MIME type) */
static void describe (const unsigned char *b, size_t n, const char *name, Buf *d, const char **mime) {
  size_t i;
  int ascii = 1, utf8 = 1, crlf = 0, lf = 0, cr = 0, nul = 0, lines_long = 0, col = 0;
  *mime = "application/octet-stream";
  if (n == 0) {
    buf_puts(d, "empty");
    *mime = "inode/x-empty";
    return;
  }
  if (n >= 4 && b[0] == 0x7f && b[1] == 'E' && b[2] == 'L' && b[3] == 'F' && n >= 20) {
    int bits = b[4] == 2 ? 64 : 32, le = b[5] == 1;
    unsigned type = le ? get16le(b + 16) : ((unsigned)b[16] << 8 | b[17]);
    unsigned mach = le ? get16le(b + 18) : ((unsigned)b[18] << 8 | b[19]);
    buf_printf(d, "ELF %d-bit %s %s, %s", bits, le ? "LSB" : "MSB",
               type == 2 ? "executable" : type == 3 ? "pie executable" : type == 1 ? "relocatable" :
               type == 4 ? "core file" : "unknown type", elf_machine(mach));
    *mime = type == 1 ? "application/x-object" : type == 3 ? "application/x-pie-executable" : "application/x-executable";
    return;
  }
  if (n >= 64 && b[0] == 'M' && b[1] == 'Z') {
    unsigned long pe = get32le(b + 60);
    if (pe + 24 < n && b[pe] == 'P' && b[pe + 1] == 'E' && b[pe + 2] == 0 && b[pe + 3] == 0) {
      unsigned mach = get16le(b + pe + 4), chars = get16le(b + pe + 22);
      unsigned magic = pe + 26 < n ? get16le(b + pe + 24) : 0;
      unsigned subsys = 0;
      size_t so = pe + 24 + 68;
      if (so + 2 <= n) subsys = get16le(b + so);
      buf_printf(d, "%s executable (%s) %s, for MS Windows",
                 magic == 0x20b ? "PE32+" : "PE32",
                 (chars & 0x2000) ? "DLL" : subsys == 2 ? "GUI" : subsys == 3 ? "console" : "native",
                 mach == 0x8664 ? "x86-64" : mach == 0x14c ? "Intel 80386" : mach == 0xaa64 ? "Aarch64" :
                 mach == 0x1c4 ? "ARMv7 Thumb" : "unknown");
      *mime = (chars & 0x2000) ? "application/x-dosexec" : "application/vnd.microsoft.portable-executable";
      return;
    }
    buf_puts(d, "MS-DOS executable");
    *mime = "application/x-dosexec";
    return;
  }
  if (n >= 4 && (get32be(b) == 0xfeedfaceUL || get32be(b) == 0xfeedfacfUL || get32le(b) == 0xfeedfaceUL ||
                 get32le(b) == 0xfeedfacfUL)) {
    buf_puts(d, get32le(b) == 0xfeedfacfUL ? "Mach-O 64-bit executable" : "Mach-O executable");
    *mime = "application/x-mach-binary";
    return;
  }
  if (n >= 4 && get32be(b) == 0xcafebabeUL) {
    buf_puts(d, "Mach-O universal binary");
    *mime = "application/x-mach-binary";
    return;
  }
  if (n >= 24 && memcmp(b, "\x89PNG\r\n\x1a\n", 8) == 0) {
    buf_printf(d, "PNG image data, %lu x %lu, %d-bit%s", get32be(b + 16), get32be(b + 20), b[24],
               b[25] == 6 ? "/color RGBA, non-interlaced" : b[25] == 2 ? "/color RGB, non-interlaced" :
               b[25] == 0 ? " grayscale, non-interlaced" : b[25] == 3 ? " colormap, non-interlaced" : "");
    *mime = "image/png";
    return;
  }
  if (n >= 3 && b[0] == 0xff && b[1] == 0xd8 && b[2] == 0xff) {
    buf_puts(d, "JPEG image data");
    *mime = "image/jpeg";
    return;
  }
  if (n >= 10 && (memcmp(b, "GIF87a", 6) == 0 || memcmp(b, "GIF89a", 6) == 0)) {
    buf_printf(d, "GIF image data, version %.3s, %u x %u", b + 3, get16le(b + 6), get16le(b + 8));
    *mime = "image/gif";
    return;
  }
  if (n >= 12 && memcmp(b, "RIFF", 4) == 0 && memcmp(b + 8, "WEBP", 4) == 0) {
    buf_puts(d, "RIFF (little-endian) data, Web/P image");
    *mime = "image/webp";
    return;
  }
  if (n >= 12 && memcmp(b, "RIFF", 4) == 0 && memcmp(b + 8, "WAVE", 4) == 0) {
    buf_puts(d, "RIFF (little-endian) data, WAVE audio");
    *mime = "audio/x-wav";
    return;
  }
  if (n >= 2 && b[0] == 'B' && b[1] == 'M' && n >= 26) {
    buf_printf(d, "PC bitmap, Windows 3.x format, %ld x %ld", (long)get32le(b + 18), (long)get32le(b + 22));
    *mime = "image/bmp";
    return;
  }
  if (n >= 4 && b[0] == 0 && b[1] == 0 && b[2] == 1 && b[3] == 0) {
    buf_puts(d, "MS Windows icon resource");
    *mime = "image/vnd.microsoft.icon";
    return;
  }
  if (n >= 5 && memcmp(b, "%PDF-", 5) == 0) {
    size_t k = 5;
    buf_puts(d, "PDF document, version ");
    while (k < n && k < 10 && (isdigit(b[k]) || b[k] == '.')) buf_putc(d, (char)b[k++]);
    *mime = "application/pdf";
    return;
  }
  if (n >= 4 && b[0] == 'P' && b[1] == 'K' && b[2] == 3 && b[3] == 4) {
    const char *ext = strrchr(name, '.');
    if (ext && (m_stricmp(ext, ".jar") == 0)) buf_puts(d, "Java archive data (JAR)");
    else if (ext && m_stricmp(ext, ".docx") == 0) buf_puts(d, "Microsoft Word 2007+");
    else if (ext && m_stricmp(ext, ".xlsx") == 0) buf_puts(d, "Microsoft Excel 2007+");
    else if (ext && m_stricmp(ext, ".pptx") == 0) buf_puts(d, "Microsoft PowerPoint 2007+");
    else if (ext && m_stricmp(ext, ".apk") == 0) buf_puts(d, "Android package (APK)");
    else buf_printf(d, "Zip archive data, at least v%d.%d to extract", get16le(b + 4) / 10, get16le(b + 4) % 10);
    *mime = "application/zip";
    return;
  }
  if (n >= 4 && b[0] == 'P' && b[1] == 'K' && b[2] == 5 && b[3] == 6) {
    buf_puts(d, "Zip archive data (empty)");
    *mime = "application/zip";
    return;
  }
  if (n >= 3 && b[0] == 0x1f && b[1] == 0x8b) {
    buf_printf(d, "gzip compressed data%s", b[2] == 8 ? ", deflate" : "");
    if (n >= 10 && (b[3] & 8)) {	/* the original name */
      size_t k = 10;
      if (b[3] & 4) k += 2 + get16le(b + 10);
      buf_puts(d, ", was \"");
      while (k < n && b[k]) buf_putc(d, (char)b[k++]);
      buf_putc(d, '"');
    }
    *mime = "application/gzip";
    return;
  }
  if (n >= 3 && b[0] == 'B' && b[1] == 'Z' && b[2] == 'h') {
    buf_puts(d, "bzip2 compressed data");
    *mime = "application/x-bzip2";
    return;
  }
  if (n >= 6 && memcmp(b, "\xfd" "7zXZ\0", 6) == 0) {
    buf_puts(d, "XZ compressed data");
    *mime = "application/x-xz";
    return;
  }
  if (n >= 4 && get32le(b) == 0xFD2FB528UL) {
    buf_puts(d, "Zstandard compressed data");
    *mime = "application/zstd";
    return;
  }
  if (n >= 6 && memcmp(b, "7z\xbc\xaf\x27\x1c", 6) == 0) {
    buf_puts(d, "7-zip archive data");
    *mime = "application/x-7z-compressed";
    return;
  }
  if (n >= 4 && memcmp(b, "Rar!", 4) == 0) {
    buf_puts(d, "RAR archive data");
    *mime = "application/x-rar";
    return;
  }
  if (n >= 16 && memcmp(b, "SQLite format 3", 15) == 0) {
    buf_puts(d, "SQLite 3.x database");
    *mime = "application/vnd.sqlite3";
    return;
  }
  if (n >= 262 && memcmp(b + 257, "ustar", 5) == 0) {
    buf_puts(d, "POSIX tar archive");
    *mime = "application/x-tar";
    return;
  }
  if (n >= 4 && (memcmp(b, "\0\1\0\0", 4) == 0 || memcmp(b, "OTTO", 4) == 0 || memcmp(b, "true", 4) == 0)) {
    buf_puts(d, memcmp(b, "OTTO", 4) == 0 ? "OpenType font data" : "TrueType Font data");
    *mime = "font/sfnt";
    return;
  }
  if (n >= 4 && memcmp(b, "wOFF", 4) == 0) { buf_puts(d, "Web Open Font Format"); *mime = "font/woff"; return; }
  if (n >= 4 && memcmp(b, "wOF2", 4) == 0) { buf_puts(d, "Web Open Font Format (Version 2)"); *mime = "font/woff2"; return; }
  if (n >= 8 && memcmp(b + 4, "ftyp", 4) == 0) {
    buf_puts(d, "ISO Media");
    *mime = "video/mp4";
    return;
  }
  if (n >= 3 && (memcmp(b, "ID3", 3) == 0 || (b[0] == 0xff && (b[1] & 0xe0) == 0xe0))) {
    buf_puts(d, "Audio file with ID3 version 2");
    if (memcmp(b, "ID3", 3) != 0) {
      d->len = 0;
      buf_puts(d, "MPEG ADTS, layer III");
    }
    *mime = "audio/mpeg";
    return;
  }
  if (n >= 4 && memcmp(b, "OggS", 4) == 0) { buf_puts(d, "Ogg data"); *mime = "audio/ogg"; return; }
  if (n >= 4 && memcmp(b, "fLaC", 4) == 0) { buf_puts(d, "FLAC audio bitstream data"); *mime = "audio/flac"; return; }
  if (n >= 4 && memcmp(b, "\0asm", 4) == 0) { buf_puts(d, "WebAssembly (wasm) binary module"); *mime = "application/wasm"; return; }
  if (n >= 4 && get32be(b) == 0xcafebabeUL) { buf_puts(d, "compiled Java class data"); return; }
  /* text? */
  for (i = 0; i < n; i++) {
    unsigned char c = b[i];
    if (c == 0) nul = 1;
    if (c == '\n') {
      if (i > 0 && b[i - 1] == '\r') crlf = 1;
      else lf = 1;
      if (col > 300) lines_long = 1;
      col = 0;
      continue;
    }
    col++;
    if (c == '\r' && (i + 1 >= n || b[i + 1] != '\n')) cr = 1;
    if (c >= 0x80) {
      ascii = 0;
      if (utf8) {
        int l = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0, k;
        if (l == 0) utf8 = 0;
        else {
          for (k = 1; k < l; k++)
            if (i + (size_t)k >= n) break;
            else if ((b[i + (size_t)k] & 0xC0) != 0x80) utf8 = 0;
          i += (size_t)l - 1;
        }
      }
    }
    else if (c < 32 && c != '\t' && c != '\r' && c != '\f' && c != '\b' && c != 27) ascii = utf8 = 0;
  }
  if (nul || (!ascii && !utf8)) {
    if (n >= 2 && ((b[0] == 0xff && b[1] == 0xfe) || (b[0] == 0xfe && b[1] == 0xff))) {
      buf_puts(d, "Unicode text, UTF-16, ");
      buf_puts(d, b[0] == 0xff ? "little-endian text" : "big-endian text");
      *mime = "text/plain";
      return;
    }
    if (!nul) {
      buf_puts(d, "ISO-8859 text");
      *mime = "text/plain";
    }
    else buf_puts(d, "data");
    return;
  }
  *mime = "text/plain";
  {
    const char *kind = NULL;
    if (n > 2 && b[0] == '#' && b[1] == '!') {
      char interp[128];
      size_t k = 2, j = 0;
      const char *base;
      while (k < n && b[k] == ' ') k++;
      while (k < n && b[k] != '\n' && b[k] != ' ' && j < sizeof(interp) - 1) interp[j++] = (char)b[k++];
      interp[j] = '\0';
      base = tool_base(interp);
      if (strcmp(base, "env") == 0) {	/* #!/usr/bin/env python3 */
        j = 0;
        while (k < n && b[k] == ' ') k++;
        while (k < n && b[k] != '\n' && b[k] != ' ' && j < sizeof(interp) - 1) interp[j++] = (char)b[k++];
        interp[j] = '\0';
        base = interp;
      }
      if (strcmp(base, "sh") == 0) kind = "POSIX shell script";
      else if (strcmp(base, "bash") == 0) kind = "Bourne-Again shell script";
      else if (strcmp(base, "mmc") == 0 || strcmp(base, "mmc-shell") == 0) kind = "mmc shell script";
      else if (strncmp(base, "python", 6) == 0) kind = "Python script";
      else if (strcmp(base, "perl") == 0) kind = "Perl script";
      else if (strcmp(base, "node") == 0) kind = "Node.js script";
      else if (strcmp(base, "ruby") == 0) kind = "Ruby script";
      else if (strcmp(base, "zsh") == 0) kind = "Paul Falstad's zsh script";
      else if (strcmp(base, "awk") == 0 || strcmp(base, "gawk") == 0) kind = "awk or perl script";
      else {
        buf_printf(d, "a %s script", base);
        kind = "";
      }
      *mime = "text/x-shellscript";
    }
    else if (n >= 5 && (memcmp(b, "<?xml", 5) == 0)) kind = "XML 1.0 document";
    else if (n >= 9 && (m_strnicmp((const char *)b, "<!doctype", 9) == 0 || m_strnicmp((const char *)b, "<html", 5) == 0)) {
      kind = "HTML document";
      *mime = "text/html";
    }
    else if (n > 0 && (b[0] == '{' || b[0] == '[') && strstr(name, ".json")) {
      kind = "JSON text data";
      *mime = "application/json";
    }
    else {
      const char *ext = strrchr(name, '.');
      if (ext && (strcmp(ext, ".c") == 0 || strcmp(ext, ".h") == 0)) kind = "C source";
      else if (ext && (strcmp(ext, ".cpp") == 0 || strcmp(ext, ".cc") == 0 || strcmp(ext, ".hpp") == 0)) kind = "C++ source";
      else if (ext && strcmp(ext, ".py") == 0) kind = "Python script";
    }
    if (kind && strcmp(kind, "JSON text data") == 0) buf_puts(d, kind);
    else if (kind == NULL || kind[0]) {
      if (kind) buf_printf(d, "%s, ", kind);
      if (n >= 3 && b[0] == 0xef && b[1] == 0xbb && b[2] == 0xbf) buf_puts(d, "Unicode text, UTF-8 (with BOM) text");
      else buf_puts(d, ascii ? "ASCII text" : "Unicode text, UTF-8 text");
    }
    if (kind && !kind[0]) buf_puts(d, ", ASCII text");
    if (strstr(d->s, "script") && !strstr(d->s, "executable")) {
      /* GNU adds "executable" for scripts: said by the caller when the x bit is on */
    }
    if (lines_long) buf_puts(d, ", with very long lines");
    if (crlf && !lf) buf_puts(d, ", with CRLF line terminators");
    else if (crlf && lf) buf_puts(d, ", with CRLF, LF line terminators");
    else if (cr && !lf) buf_puts(d, ", with CR line terminators");
    else if (!lf && !crlf && n > 0) buf_puts(d, ", with no line terminators");
  }
}


int t_file (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {{"brief", 'b', 0}, {"mime", 'i', 0}, {"mime-type", 1001, 0},
    {"dereference", 'L', 0}, {"no-dereference", 'h', 0}, {"separator", 'F', 1},
    {"print0", '0', 0}, {"no-pad", 'N', 0}, {NULL, 0, 0}};
  Opts g;
  Out o;
  int c, brief = 0, mime = 0, deref = 0, nopad = 0, width = 0;
  const char *sep = ":";
  size_t i;
  (void)in;
  opts_init(&g, "file", argc, argv, err);
  while ((c = opts_next(&g, "biLhF:0Nz", lo)) != 0) {
    switch (c) {
      case 'b': brief = 1; break;
      case 'i': mime = 2; break;
      case 1001: mime = 1; break;
      case 'L': deref = 1; break;
      case 'h': deref = 0; break;
      case 'F': sep = g.arg; break;
      case 'N': nopad = 1; break;
      case '0': case 'z': break;
      case OPT_HELP: opts_free(&g); return tool_help(out, "file");
      default: opts_free(&g); return 1;
    }
  }
  if (g.ops.n == 0) {
    fd_printf(err, "Usage: file [-bchikLlNnprsSvzZ0] [--apple] [--extension] [--mime-encoding]\n"
                   "            [--mime-type] [-e <testname>] [-F <separator>]  [-f <namefile>]\n"
                   "            [-m <magicfiles>] [-P <parameter=value>] <file> ...\n");
    opts_free(&g);
    return 1;
  }
  for (i = 0; i < g.ops.n; i++) {
    int w = (int)strlen(g.ops.v[i]) + (int)strlen(sep);
    if (w > width) width = w;
  }
  out_init(&o, out);
  for (i = 0; i < g.ops.n; i++) {
    const char *name = g.ops.v[i];
    char *native = path_to_native(name);
    OsStat st;
    Buf d;
    const char *mt = "application/octet-stream";
    buf_init(&d);
    if ((deref ? os_stat(native, &st) : os_lstat(native, &st)) != 0) {
      buf_printf(&d, "cannot open `%s' (%s)", name, os_errmsg());
      mt = "cannot open";
    }
    else if (st.is_link) {
      char *t = os_readlink(native);
      buf_printf(&d, "symbolic link to %s", t ? t : "?");
      free(t);
      mt = "inode/symlink";
    }
    else if (st.is_dir) {
      buf_puts(&d, "directory");
      mt = "inode/directory";
    }
    else if (st.is_chr) { buf_puts(&d, "character special"); mt = "inode/chardevice"; }
    else if (st.is_blk) { buf_puts(&d, "block special"); mt = "inode/blockdevice"; }
    else if (st.is_fifo) { buf_puts(&d, "fifo (named pipe)"); mt = "inode/fifo"; }
    else if (st.is_sock) { buf_puts(&d, "socket"); mt = "inode/socket"; }
    else {
      int fd = os_open(native, OS_READ);
      if (fd < 0) {
        buf_printf(&d, "regular file, no read permission");
      }
      else {
        unsigned char head[8192];
        long n = 0, got;
        while (n < (long)sizeof(head) && (got = os_read(fd, head + n, sizeof(head) - (size_t)n)) > 0) n += got;
        os_close(fd);
        describe(head, n > 0 ? (size_t)n : 0, name, &d, &mt);
        if ((st.mode & 0111) && strstr(d.s, "script") && !strstr(d.s, "executable")) {
          char *comma = strstr(d.s, "text");
          if (comma) {	/* "..., ASCII text executable" */
            Buf x;
            size_t at = (size_t)(comma - d.s) + 4;
            buf_init(&x);
            buf_putn(&x, d.s, at);
            buf_puts(&x, " executable");
            buf_puts(&x, d.s + at);
            buf_free(&d);
            d = x;
          }
        }
      }
    }
    if (!brief) {
      out_printf(&o, "%s%s ", name, sep);
      if (!nopad) {
        int k;
        for (k = (int)(strlen(name) + strlen(sep)); k < width; k++) out_putc(&o, ' ');
      }
    }
    if (mime) {
      out_puts(&o, mt);
      if (mime == 2) {
        const char *cs = strncmp(mt, "text/", 5) == 0 || strstr(mt, "json") ?
                         (d.s && strstr(d.s, "UTF-8") ? "utf-8" : d.s && strstr(d.s, "ASCII") ? "us-ascii" : "unknown-8bit") :
                         strcmp(mt, "inode/x-empty") == 0 ? "binary" : strncmp(mt, "inode/", 6) == 0 ? "binary" : "binary";
        out_printf(&o, "; charset=%s", cs);
      }
    }
    else out_puts(&o, d.s ? d.s : "");
    out_putc(&o, '\n');
    buf_free(&d);
    free(native);
  }
  out_flush(&o);
  opts_free(&g);
  return 0;
}

/* }================================================================== */


/*
** {==================================================================
** uname, hostname, whoami, id
** ===================================================================
*/

int t_uname (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {{"all", 'a', 0}, {"kernel-name", 's', 0}, {"nodename", 'n', 0},
    {"kernel-release", 'r', 0}, {"kernel-version", 'v', 0}, {"machine", 'm', 0},
    {"processor", 'p', 0}, {"hardware-platform", 'i', 0}, {"operating-system", 'o', 0},
    {NULL, 0, 0}};
  Opts g;
  OsUname u;
  int c, s = 0, n = 0, r = 0, v = 0, m = 0, p = 0, i = 0, o = 0, all = 0, first = 1;
  char *host;
  Buf b;
  (void)in;
  opts_init(&g, "uname", argc, argv, err);
  while ((c = opts_next(&g, "asnrvmpio", lo)) != 0) {
    switch (c) {
      case 'a': all = s = n = r = v = m = o = 1; break;
      case 's': s = 1; break;
      case 'n': n = 1; break;
      case 'r': r = 1; break;
      case 'v': v = 1; break;
      case 'm': m = 1; break;
      case 'p': p = 1; break;
      case 'i': i = 1; break;
      case 'o': o = 1; break;
      case OPT_HELP: opts_free(&g); return tool_help(out, "uname");
      default: opts_free(&g); return 1;
    }
  }
  if (g.ops.n > 0) {
    tool_err(err, "uname", "extra operand '%s'", g.ops.v[0]);
    opts_free(&g);
    return 1;
  }
  if (!(s || n || r || v || m || p || i || o)) s = 1;
  os_uname(&u);
  host = os_hostname();
  buf_init(&b);
#define PUT(x) do { if (!first) buf_putc(&b, ' '); buf_puts(&b, x); first = 0; } while (0)
  if (s) PUT(u.sysname);
  if (n) PUT(host);
  if (r) PUT(u.release);
  if (v) PUT(u.version);
  if (m) PUT(u.machine);
  if (p && !all) PUT(strcmp(u.machine, "x86_64") == 0 || strcmp(u.machine, "aarch64") == 0 ? u.machine : "unknown");
  if (i && !all) PUT(strcmp(u.machine, "x86_64") == 0 || strcmp(u.machine, "aarch64") == 0 ? u.machine : "unknown");
  if (o) PUT(u.os);
#undef PUT
  buf_putc(&b, '\n');
  fd_puts(out, b.s);
  buf_free(&b);
  free(host);
  opts_free(&g);
  return 0;
}


int t_hostname (int argc, char **argv, int in, int out, int err) {
  char *h = os_hostname();
  int i, shortn = 0;
  (void)in;
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--short") == 0) shortn = 1;
    else if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--fqdn") == 0 || strcmp(argv[i], "--long") == 0 ||
             strcmp(argv[i], "-A") == 0) {
    }
    else if (strcmp(argv[i], "--help") == 0) {
      free(h);
      return tool_help(out, "hostname");
    }
    else if (argv[i][0] == '-') {
      tool_err(err, "hostname", "invalid option -- '%s'", argv[i] + 1);
      free(h);
      return 1;
    }
    else {
      tool_err(err, "hostname", "you must be root to change the host name");
      free(h);
      return 1;
    }
  }
  if (shortn) h[strcspn(h, ".")] = '\0';
  fd_printf(out, "%s\n", h);
  free(h);
  return 0;
}


int t_whoami (int argc, char **argv, int in, int out, int err) {
  char *u;
  (void)in;
  if (argc > 1 && strcmp(argv[1], "--help") == 0) return tool_help(out, "whoami");
  if (argc > 1) {
    tool_err(err, "whoami", "extra operand '%s'", argv[1]);
    return 1;
  }
  u = os_user_name(os_geteuid());
  fd_printf(out, "%s\n", u);
  free(u);
  return 0;
}


int t_id (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {{"user", 'u', 0}, {"group", 'g', 0}, {"groups", 'G', 0},
    {"name", 'n', 0}, {"real", 'r', 0}, {NULL, 0, 0}};
  Opts g;
  int c, what = 0, name = 0;
  long uid = os_geteuid(), gid;
  char *un, *gn;
  (void)in;
  opts_init(&g, "id", argc, argv, err);
  while ((c = opts_next(&g, "ugGnr", lo)) != 0) {
    switch (c) {
      case 'u': case 'g': case 'G': what = c; break;
      case 'n': name = 1; break;
      case 'r': uid = os_getuid(); break;
      case OPT_HELP: opts_free(&g); return tool_help(out, "id");
      default: opts_free(&g); return 1;
    }
  }
  gid = os_getgid();
  un = os_user_name(uid);
  gn = os_group_name(gid);
  if (what == 'u') {
    if (name) fd_printf(out, "%s\n", un);
    else fd_printf(out, "%ld\n", uid);
  }
  else if (what == 'g' || what == 'G') {
    if (name) fd_printf(out, "%s\n", gn);
    else fd_printf(out, "%ld\n", gid);
  }
  else fd_printf(out, "uid=%ld(%s) gid=%ld(%s) groups=%ld(%s)\n", uid, un, gid, gn, gid, gn);
  free(un);
  free(gn);
  opts_free(&g);
  return 0;
}

/* }================================================================== */


/*
** {==================================================================
** ps
** ===================================================================
*/

typedef struct PsCol {
  const char *key, *head;
  int width, left;
} PsCol;

static const PsCol ps_cols[] = {
  {"pid", "PID", 7, 0}, {"ppid", "PPID", 7, 0}, {"uid", "UID", 5, 0}, {"user", "USER", 10, 1},
  {"comm", "COMMAND", 0, 1}, {"ucomm", "COMMAND", 0, 1}, {"ucmd", "CMD", 0, 1},
  {"args", "COMMAND", 0, 1},
  {"cmd", "CMD", 0, 1}, {"command", "COMMAND", 0, 1}, {"time", "TIME", 8, 0},
  {"cputime", "TIME", 8, 0}, {"etime", "ELAPSED", 11, 0}, {"rss", "RSS", 8, 0},
  {"rsz", "RSZ", 8, 0}, {"vsz", "VSZ", 9, 0}, {"%cpu", "%CPU", 4, 0}, {"pcpu", "%CPU", 4, 0},
  {"%mem", "%MEM", 4, 0}, {"pmem", "%MEM", 4, 0}, {"stat", "STAT", 4, 1}, {"s", "S", 1, 1},
  {"state", "S", 1, 1}, {"tty", "TTY", 8, 1}, {"tname", "TTY", 8, 1}, {"start", "START", 5, 0},
  {"stime", "STIME", 5, 0}, {"lstart", "STARTED", 24, 1}, {"c", "C", 2, 0}, {"pgid", "PGID", 7, 0},
  {NULL, NULL, 0, 0}};


static void ps_field (Buf *b, const char *key, const OsProcInfo *p, unsigned long long memtotal,
                      time_t now) {
  char t[64];
  if (strcmp(key, "pid") == 0 || strcmp(key, "pgid") == 0) buf_printf(b, "%ld", p->pid);
  else if (strcmp(key, "ppid") == 0) buf_printf(b, "%ld", p->ppid);
  else if (strcmp(key, "uid") == 0) buf_printf(b, "%ld", p->uid);
  else if (strcmp(key, "user") == 0) {
    char *u = os_user_name(p->uid);
    buf_puts(b, u);
    free(u);
  }
  else if (strcmp(key, "comm") == 0 || strcmp(key, "ucomm") == 0 || strcmp(key, "ucmd") == 0)
    buf_puts(b, p->name);
  else if (strcmp(key, "args") == 0 || strcmp(key, "cmd") == 0 || strcmp(key, "command") == 0) buf_puts(b, p->cmd);
  else if (strcmp(key, "time") == 0 || strcmp(key, "cputime") == 0) {
    long s = (long)p->cpu;
    buf_printf(b, "%02ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60);
  }
  else if (strcmp(key, "etime") == 0) {
    long s = p->start ? (long)(now - p->start) : 0, d = s / 86400;
    if (d > 0) buf_printf(b, "%ld-%02ld:%02ld:%02ld", d, (s / 3600) % 24, (s / 60) % 60, s % 60);
    else if (s >= 3600) buf_printf(b, "%02ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60);
    else buf_printf(b, "%02ld:%02ld", s / 60, s % 60);
  }
  else if (strcmp(key, "rss") == 0 || strcmp(key, "rsz") == 0) buf_printf(b, "%lld", p->rss / 1024);
  else if (strcmp(key, "vsz") == 0) buf_printf(b, "%lld", p->vsz / 1024);
  else if (strcmp(key, "%cpu") == 0 || strcmp(key, "pcpu") == 0) {
    double el = p->start ? difftime(now, p->start) : 0;
    buf_printf(b, "%.1f", el > 0 ? p->cpu * 100.0 / el : 0.0);
  }
  else if (strcmp(key, "%mem") == 0 || strcmp(key, "pmem") == 0)
    buf_printf(b, "%.1f", memtotal ? (double)p->rss * 100.0 / (double)memtotal : 0.0);
  else if (strcmp(key, "stat") == 0 || strcmp(key, "s") == 0 || strcmp(key, "state") == 0)
    buf_putc(b, p->state ? p->state : '?');
  else if (strcmp(key, "tty") == 0 || strcmp(key, "tname") == 0) buf_puts(b, "?");
  else if (strcmp(key, "start") == 0 || strcmp(key, "stime") == 0) {
    struct tm *tm = p->start ? localtime(&p->start) : NULL;
    if (tm == NULL) buf_puts(b, "-");
    else {
      strftime(t, sizeof(t), (now - p->start) < 86400 ? "%H:%M" : "%b%d", tm);
      buf_puts(b, t);
    }
  }
  else if (strcmp(key, "lstart") == 0) {
    struct tm *tm = p->start ? localtime(&p->start) : NULL;
    if (tm) {
      strftime(t, sizeof(t), "%a %b %e %H:%M:%S %Y", tm);
      buf_puts(b, t);
    }
    else buf_puts(b, "-");
  }
  else if (strcmp(key, "c") == 0) buf_puts(b, "0");
  else buf_puts(b, "-");
}


static const PsCol *ps_col (const char *key) {
  const PsCol *c;
  for (c = ps_cols; c->key; c++)
    if (strcmp(c->key, key) == 0) return c;
  return NULL;
}


static int ps_cmp (const void *a, const void *b) {
  const OsProcInfo *x = (const OsProcInfo *)a, *y = (const OsProcInfo *)b;
  return x->pid < y->pid ? -1 : x->pid > y->pid;
}


int t_ps (int argc, char **argv, int in, int out, int err) {
  OsProcInfo *v;
  size_t n, i, k;
  int all = 0, full = 0, bsd_u = 0, bsd_x = 0, noheader = 0, wide = 0, status = 0;
  Vec fields, pids, users, names;
  Out o;
  unsigned long long memtotal = os_memtotal();
  time_t now = time(NULL);
  long me = os_getpid(), myuid = os_geteuid();
  char *sel;
  (void)in;
  vec_init(&fields);
  vec_init(&pids);
  vec_init(&users);
  vec_init(&names);
  for (k = 1; k < (size_t)argc; k++) {
    const char *a = argv[k];
    if (strcmp(a, "--help") == 0) return tool_help(out, "ps");
    if (strcmp(a, "--no-headers") == 0 || strcmp(a, "--no-heading") == 0) { noheader = 1; continue; }
    if (strncmp(a, "--sort", 6) == 0) { if (!strchr(a, '=')) k++; continue; }
    if (a[0] == '-') {
      const char *p;
      for (p = a + 1; *p; p++) {
        switch (*p) {
          case 'e': case 'A': all = 1; break;
          case 'f': full = 1; break;
          case 'l': full = 1; break;
          case 'w': wide = 1; break;
          case 'a': all = 1; break;
          case 'H': break;
          case 'p': case 'q': case 'u': case 'U': case 'C': case 'o': case 'O': case 'g': case 'G': {
            char opt = *p;
            const char *val = p[1] ? p + 1 : (k + 1 < (size_t)argc ? argv[++k] : NULL);
            Vec *dst = (opt == 'p' || opt == 'q') ? &pids : (opt == 'u' || opt == 'U') ? &users :
                       opt == 'C' ? &names : &fields;
            char *copy, *tok, *save;
            if (val == NULL) {
              tool_err(err, "ps", "option -%c requires an argument", opt);
              goto bad;
            }
            copy = xstrdup(val);
            for (tok = copy; tok; tok = save) {
              save = strpbrk(tok, ", ");
              if (save) *save++ = '\0';
              if (*tok) vec_push(dst, xstrdup(tok));
            }
            free(copy);
            p += strlen(p) - 1;
            break;
          }
          default:
            tool_err(err, "ps", "unsupported option (BSD syntax) -- '%c'", *p);
            goto bad;
        }
      }
    }
    else {	/* BSD style: aux, ax, x, u */
      const char *p;
      for (p = a; *p; p++) {
        if (*p == 'a') all = 1;
        else if (*p == 'u') bsd_u = 1;
        else if (*p == 'x') bsd_x = 1;
        else if (*p == 'w') wide = 1;
        else if (isdigit((unsigned char)*p)) {
          vec_push(&pids, xstrdup(p));
          break;
        }
        else if (*p == 'e' || *p == 'f' || *p == 'h') { if (*p == 'h') noheader = 1; }
        else {
          tool_err(err, "ps", "unsupported option (BSD syntax) -- '%c'", *p);
          goto bad;
        }
      }
    }
  }
  (void)wide;
  if (os_proclist(&v, &n) != 0) {
    tool_err(err, "ps", "cannot list the processes: %s", os_errmsg());
    goto bad;
  }
  qsort(v, n, sizeof(OsProcInfo), ps_cmp);
  sel = (char *)xmalloc(n + 1);
  for (i = 0; i < n; i++) {
    int s;
    if (pids.n || users.n || names.n) {
      s = 0;
      for (k = 0; k < pids.n; k++) if (atol(pids.v[k]) == v[i].pid) s = 1;
      for (k = 0; k < users.n; k++) {
        char *u = os_user_name(v[i].uid);
        if (strcmp(u, users.v[k]) == 0 || atol(users.v[k]) == v[i].uid) s = 1;
        free(u);
      }
      for (k = 0; k < names.n; k++) {
        const char *nm = v[i].name;
        size_t l = strlen(names.v[k]);
        if (strcmp(nm, names.v[k]) == 0 || (m_strnicmp(nm, names.v[k], l) == 0 && m_stricmp(nm + l, ".exe") == 0)) s = 1;
      }
    }
    else if (all) s = 1;
    else if (bsd_x) s = v[i].uid == myuid;
    else {	/* this shell and what it started */
      long p = v[i].pid;
      int depth;
      s = 0;
      for (depth = 0; depth < 64 && p > 0; depth++) {
        size_t j;
        if (p == me) {
          s = 1;
          break;
        }
        for (j = 0; j < n && v[j].pid != p; j++) {}
        if (j >= n || v[j].ppid == p) break;
        p = v[j].ppid;
      }
      if (v[i].pid == os_getppid() && 0) s = 1;
    }
    sel[i] = (char)s;
  }
  if (fields.n == 0) {	/* the usual layouts */
    const char *const *def;
    static const char *const d_min[] = {"pid", "tty", "time", "ucmd", NULL};
    static const char *const d_full[] = {"user", "pid", "ppid", "c", "stime", "tty", "time", "cmd", NULL};
    static const char *const d_u[] = {"user", "pid", "%cpu", "%mem", "vsz", "rss", "tty", "stat",
                                      "start", "time", "args", NULL};
    static const char *const d_x[] = {"pid", "tty", "stat", "time", "args", NULL};
    def = bsd_u ? d_u : full ? d_full : bsd_x ? d_x : d_min;
    for (k = 0; def[k]; k++) vec_push(&fields, xstrdup(def[k]));
  }
  out_init(&o, out);
  {
    size_t nf = fields.n, r;
    int *w = (int *)xmalloc(nf * sizeof(int));
    char ***cells = (char ***)xmalloc((n + 1) * sizeof(char **));
    size_t nr = 0;
    for (k = 0; k < nf; k++) {
      char *eq = strchr(fields.v[k], '=');
      const PsCol *c = ps_col(eq ? "" : fields.v[k]);
      if (eq) *eq = '\0';
      c = ps_col(fields.v[k]);
      if (c == NULL) {
        tool_err(err, "ps", "unknown user-defined format specifier \"%s\"", fields.v[k]);
        free(w);
        free(cells);
        os_proclist_free(v, n);
        free(sel);
        goto bad;
      }
      w[k] = (int)strlen(eq ? eq + 1 : (full && strcmp(c->key, "user") == 0) ? "UID" : c->head);
    }
    for (i = 0; i < n; i++) {
      if (!sel[i]) continue;
      cells[nr] = (char **)xmalloc(nf * sizeof(char *));
      for (k = 0; k < nf; k++) {
        Buf b;
        int l;
        buf_init(&b);
        ps_field(&b, fields.v[k], &v[i], memtotal, now);
        if (b.s == NULL) buf_putc(&b, '\0');
        cells[nr][k] = buf_take(&b);
        l = tool_utf8_cols(cells[nr][k], strlen(cells[nr][k]));
        if (k + 1 < nf && l > w[k]) w[k] = l;
      }
      nr++;
    }
    if (nr == 0) status = 1;
    if (!noheader) {
      for (k = 0; k < nf; k++) {
        const PsCol *c = ps_col(fields.v[k]);
        const char *h = (full && strcmp(c->key, "user") == 0) ? "UID" : c->head;
        if (k) out_putc(&o, ' ');
        if (k + 1 == nf) out_puts(&o, h);
        else if (c->left) out_printf(&o, "%-*s", w[k], h);
        else out_printf(&o, "%*s", w[k], h);
      }
      out_putc(&o, '\n');
    }
    for (r = 0; r < nr; r++) {
      for (k = 0; k < nf; k++) {
        const PsCol *c = ps_col(fields.v[k]);
        if (k) out_putc(&o, ' ');
        if (k + 1 == nf) out_puts(&o, cells[r][k]);
        else if (c->left) out_printf(&o, "%-*s", w[k], cells[r][k]);
        else out_printf(&o, "%*s", w[k], cells[r][k]);
        free(cells[r][k]);
      }
      out_putc(&o, '\n');
      free(cells[r]);
    }
    free(cells);
    free(w);
  }
  out_flush(&o);
  os_proclist_free(v, n);
  free(sel);
  vec_free(&fields);
  vec_free(&pids);
  vec_free(&users);
  vec_free(&names);
  return status;
bad:
  vec_free(&fields);
  vec_free(&pids);
  vec_free(&users);
  vec_free(&names);
  return 1;
}

/* }================================================================== */


/*
** {==================================================================
** watch
** ===================================================================
*/

int t_watch (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {{"interval", 'n', 1}, {"no-title", 't', 0},
    {"differences", 'd', 2}, {"errexit", 'e', 0}, {"chgexit", 'g', 0}, {"exec", 'x', 0},
    {"color", 'c', 0}, {"beep", 'b', 0}, {"precise", 'p', 0}, {NULL, 0, 0}};
  Opts g;
  double interval = 2.0;
  int c, notitle = 0, diff = 0, errexit = 0, chgexit = 0, status = 0;
  Buf cmd;
  char *prev = NULL;
  size_t i;
  int tty = os_is_tty(out);
  (void)in;
  opts_init(&g, "watch", argc, argv, err);
  g.no_permute = 1;
  while ((c = opts_next(&g, "n:td::egxcbp", lo)) != 0) {
    switch (c) {
      case 'n':
        interval = atof(g.arg);
        if (interval < 0.1) interval = 0.1;
        break;
      case 't': notitle = 1; break;
      case 'd': diff = 1; break;
      case 'e': errexit = 1; break;
      case 'g': chgexit = 1; break;
      case 'x': case 'c': case 'b': case 'p': break;
      case OPT_HELP: opts_free(&g); return tool_help(out, "watch");
      default: opts_free(&g); return 1;
    }
  }
  if (g.ops.n == 0) {
    fd_printf(err, "Usage:\n watch [options] command\n");
    opts_free(&g);
    return 1;
  }
  buf_init(&cmd);
  for (i = 0; i < g.ops.n; i++) {
    if (i) buf_putc(&cmd, ' ');
    buf_puts(&cmd, g.ops.v[i]);
  }
  if (tty) fd_puts(out, "\033[?1049h\033[?25l");	/* the other screen, no cursor */
  while (!tool_stop()) {
    size_t len;
    char *res, *wrapped = xstrcat3("{ ", cmd.s, "\n} 2>&1");
    int rows = tty ? os_term_rows() : 1000000, cols = tty ? os_term_cols() : 1000000, row = 0;
    Buf screen;
    const char *p;
    long long next = os_now_us() + (long long)(interval * 1e6);
    res = sh_capture(wrapped, &len);
    free(wrapped);
    status = sh_status;
    buf_init(&screen);
    if (tty) buf_puts(&screen, "\033[H\033[2J");
    if (!notitle) {
      char left[512], right[256], when[64];
      char *host = os_hostname();
      time_t t = time(NULL);
      struct tm *tm = localtime(&t);
      int pad;
      if (tm) strftime(when, sizeof(when), "%a %b %e %H:%M:%S %Y", tm);
      else when[0] = '\0';
      snprintf(left, sizeof(left), "Every %.1fs: %s", interval, cmd.s);
      snprintf(right, sizeof(right), "%s: %s", host, when);
      free(host);
      pad = cols - (int)strlen(left) - (int)strlen(right);
      if (tty && pad < 1) {	/* too narrow: the command is cut */
        int keep = cols - (int)strlen(right) - 1;
        if (keep < 0) keep = 0;
        left[keep < (int)sizeof(left) ? keep : (int)sizeof(left) - 1] = '\0';
        pad = 1;
      }
      buf_puts(&screen, left);
      if (tty) {
        int k;
        for (k = 0; k < pad; k++) buf_putc(&screen, ' ');
      }
      else buf_puts(&screen, "  ");
      buf_puts(&screen, right);
      buf_puts(&screen, "\n\n");
      row = 2;
    }
    /* the output, cut to the screen; -d: changed characters inverted */
    for (p = res; *p && row < rows; row++) {
      const char *e = strchr(p, '\n');
      size_t n = e ? (size_t)(e - p) : strlen(p), k;
      int col = 0;
      size_t off = (size_t)(p - res);
      for (k = 0; k < n && col < cols; k++) {
        int changed = diff && prev && (off + k >= strlen(prev) || prev[off + k] != p[k]);
        if (changed) buf_puts(&screen, "\033[7m");
        buf_putc(&screen, p[k]);
        if (changed) buf_puts(&screen, "\033[27m");
        if (((unsigned char)p[k] & 0xC0) != 0x80) col++;
      }
      if (row + 1 < rows || !tty) buf_putc(&screen, '\n');
      p = e ? e + 1 : p + n;
    }
    fd_puts(out, screen.s ? screen.s : "");
    buf_free(&screen);
    if (chgexit && prev && strcmp(prev, res) != 0) {
      free(prev);
      prev = res;
      break;
    }
    free(prev);
    prev = res;
    if (errexit && status != 0) {
      if (tty) fd_puts(out, "\033[?25h\033[?1049l");
      tool_err(err, "watch", "command exit with a non-zero status, press a key to exit");
      tty = 0;
      break;
    }
    while (!tool_stop() && os_now_us() < next) os_sleep_ms(50);
  }
  if (tty) fd_puts(out, "\033[?25h\033[?1049l");
  free(prev);
  buf_free(&cmd);
  opts_free(&g);
  return errexit && status ? 8 : 0;
}

/* }================================================================== */


/*
** {==================================================================
** cal
** ===================================================================
*/

static int is_leap (int y) {
  return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}


static int days_in (int m, int y) {
  static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return m == 2 && is_leap(y) ? 29 : d[m - 1];
}


/* 0 = Sunday */
static int weekday (int d, int m, int y) {
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y--;
  return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}


static const char *const month_names[] = {"January", "February", "March", "April", "May", "June",
  "July", "August", "September", "October", "November", "December"};


/* the 8 lines of one month, each 20 columns (plus color codes) */
static void cal_month (Vec *lines, int m, int y, int with_year, int monday, int today, int color) {
  char title[64], line[256];
  int first = weekday(1, m, y), days = days_in(m, y), d, col, k;
  size_t tl;
  int pad;
  if (with_year) snprintf(title, sizeof(title), "%s %d", month_names[m - 1], y);
  else snprintf(title, sizeof(title), "%s", month_names[m - 1]);
  tl = strlen(title);
  pad = (20 - (int)tl) / 2;
  snprintf(line, sizeof(line), "%*s%s%*s", pad, "", title, 20 - pad - (int)tl, "");
  vec_push(lines, xstrdup(line));
  vec_push(lines, xstrdup(monday ? "Mo Tu We Th Fr Sa Su" : "Su Mo Tu We Th Fr Sa"));
  if (monday) first = (first + 6) % 7;
  d = 1;
  for (k = 0; k < 6; k++) {
    Buf b;
    buf_init(&b);
    for (col = 0; col < 7; col++) {
      int cell = k * 7 + col;
      if (col) buf_putc(&b, ' ');
      if (cell < first || d > days) buf_puts(&b, "  ");
      else {
        if (color && d == today) buf_printf(&b, "\033[7m%2d\033[27m", d);
        else buf_printf(&b, "%2d", d);
        d++;
      }
    }
    vec_push(lines, buf_take(&b));
  }
}


int t_cal (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {{"monday", 'm', 0}, {"sunday", 's', 0}, {"three", '3', 0},
    {"year", 'y', 0}, {"twelve", 'Y', 0}, {"color", 1001, 2}, {NULL, 0, 0}};
  Opts g;
  Out o;
  int c, monday = 0, three = 0, year_view = 0, color = os_is_tty(out), m, y;
  time_t now = time(NULL);
  struct tm *tm = localtime(&now);
  int tm_m = tm ? tm->tm_mon + 1 : 1, tm_y = tm ? tm->tm_year + 1900 : 2000, tm_d = tm ? tm->tm_mday : 1;
  (void)in;
  opts_init(&g, "cal", argc, argv, err);
  while ((c = opts_next(&g, "ms3yY", lo)) != 0) {
    switch (c) {
      case 'm': monday = 1; break;
      case 's': monday = 0; break;
      case '3': three = 1; break;
      case 'y': year_view = 1; break;
      case 'Y': year_view = 1; break;
      case 1001:
        color = g.arg == NULL || strcmp(g.arg, "always") == 0 ? 1 : strcmp(g.arg, "never") == 0 ? 0 : color;
        break;
      case OPT_HELP: opts_free(&g); return tool_help(out, "cal");
      default: opts_free(&g); return 1;
    }
  }
  m = tm_m;
  y = tm_y;
  if (g.ops.n == 1) {
    y = atoi(g.ops.v[0]);
    year_view = 1;
  }
  else if (g.ops.n >= 2) {
    m = atoi(g.ops.v[0]);
    if (m < 1 || m > 12) {	/* a month name */
      int k;
      for (k = 0; k < 12; k++)
        if (m_strnicmp(g.ops.v[0], month_names[k], 3) == 0) m = k + 1;
    }
    y = atoi(g.ops.v[1]);
  }
  if (m < 1 || m > 12) {
    tool_err(err, "cal", "failed to parse month: %s", g.ops.v[0]);
    opts_free(&g);
    return 1;
  }
  if (y < 1 || y > 9999) {
    tool_err(err, "cal", "year %d not in range 1..9999", y);
    opts_free(&g);
    return 1;
  }
  out_init(&o, out);
  if (year_view || three) {
    int first_m = three ? m - 1 : 1, count = three ? 3 : 12, k, row;
    int first_y = y;
    if (three && first_m < 1) {
      first_m = 12;
      first_y = y - 1;
    }
    if (year_view && !three) {
      char t[16];
      snprintf(t, sizeof(t), "%d", y);
      out_printf(&o, "%*s%s\n", (64 - (int)strlen(t)) / 2, "", t);
    }
    for (row = 0; row < count / 3; row++) {
      Vec cols[3];
      int j;
      size_t l;
      for (j = 0; j < 3; j++) {
        int mm = first_m + row * 3 + j, yy = first_y;
        while (mm > 12) {
          mm -= 12;
          yy++;
        }
        vec_init(&cols[j]);
        cal_month(&cols[j], mm, yy, three, monday, (mm == tm_m && yy == tm_y) ? tm_d : 0, color);
      }
      for (l = 0; l < cols[0].n; l++) {
        Buf line;
        buf_init(&line);
        for (j = 0; j < 3; j++) {
          if (j) buf_puts(&line, "  ");
          buf_puts(&line, cols[j].v[l]);
        }
        /* no trailing spaces */
        while (line.len > 0 && line.s[line.len - 1] == ' ') line.len--;
        out_putn(&o, line.s, line.len);
        out_putc(&o, '\n');
        buf_free(&line);
      }
      for (j = 0; j < 3; j++) vec_free(&cols[j]);
      if (row + 1 < count / 3) out_putc(&o, '\n');
    }
    (void)k;
  }
  else {
    Vec lines;
    size_t l;
    vec_init(&lines);
    cal_month(&lines, m, y, 1, monday, (m == tm_m && y == tm_y) ? tm_d : 0, color);
    for (l = 0; l < lines.n; l++) {
      char *s = lines.v[l];
      size_t n = strlen(s);
      while (n > 0 && s[n - 1] == ' ') n--;
      out_putn(&o, s, n);
      out_putc(&o, '\n');
    }
    vec_free(&lines);
  }
  out_flush(&o);
  opts_free(&g);
  return 0;
}

/* }================================================================== */
