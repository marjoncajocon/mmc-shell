/*
** cfile.c - the file tools: ls cat mkdir rmdir rm cp mv touch ln
** readlink realpath
**
** Paths are shown the way they were typed ("src/" + "a" -> "src/a"),
** and turned into native ones for the OS calls. Messages are the GNU
** ones, so "cp -rfv src dst" looks the same on every system.
*/

#include "mmc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*
** {==================================================================
** Modes: drwxr-xr-x, and "755" / "u+x,go-w" for chmod and mkdir -m
** ===================================================================
*/

void mode_string (char out[11], const OsStat *st) {
  unsigned m = st->mode;
  out[0] = st->is_link ? 'l' : st->is_dir ? 'd' : st->is_fifo ? 'p' : st->is_sock ? 's' :
           st->is_chr ? 'c' : st->is_blk ? 'b' : '-';
  out[1] = (m & 0400) ? 'r' : '-';
  out[2] = (m & 0200) ? 'w' : '-';
  out[3] = (m & 04000) ? ((m & 0100) ? 's' : 'S') : ((m & 0100) ? 'x' : '-');
  out[4] = (m & 040) ? 'r' : '-';
  out[5] = (m & 020) ? 'w' : '-';
  out[6] = (m & 02000) ? ((m & 010) ? 's' : 'S') : ((m & 010) ? 'x' : '-');
  out[7] = (m & 04) ? 'r' : '-';
  out[8] = (m & 02) ? 'w' : '-';
  out[9] = (m & 01000) ? ((m & 01) ? 't' : 'T') : ((m & 01) ? 'x' : '-');
  out[10] = '\0';
}


int parse_mode (const char *spec, unsigned old, int is_dir, unsigned *out) {
  const char *p = spec;
  unsigned mode = old & 07777, umask_ = (unsigned)os_umask(-1);
  if (*p >= '0' && *p <= '7') {
    unsigned v = 0;
    for (; *p; p++) {
      if (*p < '0' || *p > '7') return -1;
      v = v * 8 + (unsigned)(*p - '0');
    }
    if (v > 07777) return -1;
    *out = v;
    return 0;
  }
  for (;;) {	/* [ugoa]*([-+=]([rwxXst]*|[ugo]))+  separated by ',' */
    unsigned who = 0;
    for (; *p && strchr("ugoa", *p); p++)
      who |= *p == 'u' ? 04700u : *p == 'g' ? 02070u : *p == 'o' ? 01007u : 07777u;
    if (*p != '+' && *p != '-' && *p != '=') return -1;
    while (*p == '+' || *p == '-' || *p == '=') {
      char op = *p++;
      unsigned bits = 0, mask = who ? who : 07777u & ~umask_;
      if (*p && strchr("ugo", *p)) {	/* copy from another class */
        unsigned src = *p == 'u' ? (mode >> 6) & 7 : *p == 'g' ? (mode >> 3) & 7 : mode & 7;
        bits = (src << 6) | (src << 3) | src;
        p++;
      }
      else {
        for (; *p && strchr("rwxXst", *p); p++) {
          if (*p == 'r') bits |= 0444;
          else if (*p == 'w') bits |= 0222;
          else if (*p == 'x') bits |= 0111;
          else if (*p == 'X') { if (is_dir || (mode & 0111)) bits |= 0111; }
          else if (*p == 's') bits |= 06000;
          else if (*p == 't') bits |= 01000;
        }
      }
      bits &= who ? who : (mask | 07000u);
      if (op == '+') mode |= bits;
      else if (op == '-') mode &= ~bits;
      else {
        unsigned clear = who ? who : 07777u;
        mode = (mode & ~clear) | bits;
      }
    }
    if (*p == '\0') break;
    if (*p != ',') return -1;
    p++;
  }
  *out = mode;
  return 0;
}

/* }================================================================== */


/*
** {==================================================================
** ls
** ===================================================================
*/

typedef struct LsEnt {
  char *name;	/* as shown */
  char *native;
  OsStat st;	/* lstat */
  OsStat tst;	/* what a link points to */
  int tok;	/* tst is valid */
  char *target;	/* link target */
} LsEnt;

typedef struct Ls {
  int all, almost, lng, human, si, reverse, sort, recursive, dirs_only;
  int classify, slash, format, numeric, no_owner, no_group, inode, blocks;
  int timekind, color, group_first, full_time, deref, ci_sort, kilo;
  int width, status;
  Vec ignore;
  Out *o;
  int err;
  char *colors;	/* LS_COLORS */
} Ls;

enum { SORT_NAME, SORT_TIME, SORT_SIZE, SORT_EXT, SORT_NONE, SORT_VERSION };
enum { FMT_ONE, FMT_COLS, FMT_ACROSS, FMT_COMMAS, FMT_LONG };
enum { TIME_M, TIME_C, TIME_A };


static int name_cmp_ci (const char *a, const char *b) {
  /* like a UTF-8 locale: letters and digits first, case after */
  const unsigned char *p = (const unsigned char *)a, *q = (const unsigned char *)b;
  int r;
  for (;;) {
    while (*p && !isalnum(*p) && *p < 0x80) p++;
    while (*q && !isalnum(*q) && *q < 0x80) q++;
    if (!*p || !*q) break;
    if (tolower(*p) != tolower(*q)) return tolower(*p) - tolower(*q);
    p++;
    q++;
  }
  if (*p || *q) return *p ? 1 : -1;
  if ((r = m_stricmp(a, b)) != 0) return r;
  return strcmp(b, a);	/* lower case first, like en_US */
}


static int locale_is_c (void) {
  const char *v = var_get("LC_ALL");
  if (v == NULL || !*v) v = var_get("LC_COLLATE");
  if (v == NULL || !*v) v = var_get("LANG");
  return v == NULL || !*v || strcmp(v, "C") == 0 || strcmp(v, "POSIX") == 0 ||
         strncmp(v, "C.", 2) == 0;
}


/* 1.10 after 1.9: digits compare as numbers */
static int version_cmp (const char *a, const char *b) {
  while (*a && *b) {
    if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
      const char *x = a, *y = b;
      size_t lx, ly;
      while (*x == '0') x++;
      while (*y == '0') y++;
      lx = strspn(x, "0123456789");
      ly = strspn(y, "0123456789");
      if (lx != ly) return lx < ly ? -1 : 1;
      if (memcmp(x, y, lx) != 0) return memcmp(x, y, lx);
      a = x + lx;
      b = y + ly;
    }
    else {
      if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
      a++;
      b++;
    }
  }
  return (unsigned char)*a - (unsigned char)*b;
}


static Ls *g_ls;	/* for the qsort comparator */

static int ls_name_cmp (const char *a, const char *b) {
  return g_ls->ci_sort ? name_cmp_ci(a, b) : strcmp(a, b);
}


static time_t ent_time (const Ls *ls, const LsEnt *e) {
  const OsStat *s = (ls->deref && e->tok) ? &e->tst : &e->st;
  return ls->timekind == TIME_C ? s->ctime : ls->timekind == TIME_A ? s->atime : s->mtime;
}


static int ent_is_dir (const LsEnt *e) {
  return e->st.is_dir || (e->st.is_link && e->tok && e->tst.is_dir);
}


static int ls_cmp (const void *pa, const void *pb) {
  const LsEnt *a = (const LsEnt *)pa, *b = (const LsEnt *)pb;
  Ls *ls = g_ls;
  int r = 0;
  if (ls->group_first) {
    int da = ent_is_dir(a), db = ent_is_dir(b);
    if (da != db) return da ? -1 : 1;
  }
  switch (ls->sort) {
    case SORT_TIME: {
      time_t ta = ent_time(ls, a), tb = ent_time(ls, b);
      r = ta > tb ? -1 : ta < tb ? 1 : 0;
      break;
    }
    case SORT_SIZE:
      r = a->st.size > b->st.size ? -1 : a->st.size < b->st.size ? 1 : 0;
      break;
    case SORT_EXT: {
      const char *ea = strrchr(a->name, '.'), *eb = strrchr(b->name, '.');
      r = ls_name_cmp(ea ? ea : "", eb ? eb : "");
      break;
    }
    case SORT_VERSION:
      r = version_cmp(a->name, b->name);
      break;
  }
  if (r == 0) r = ls_name_cmp(a->name, b->name);
  return ls->reverse ? -r : r;
}


static void ls_sort (Ls *ls, LsEnt *v, size_t n) {
  if (ls->sort == SORT_NONE || n < 2) {
    if (ls->reverse && ls->sort == SORT_NONE) {
      size_t i;
      for (i = 0; i < n / 2; i++) {
        LsEnt t = v[i];
        v[i] = v[n - 1 - i];
        v[n - 1 - i] = t;
      }
    }
    return;
  }
  g_ls = ls;
  qsort(v, n, sizeof(LsEnt), ls_cmp);
}


static void ent_fill (Ls *ls, LsEnt *e, const char *name, const char *native) {
  memset(e, 0, sizeof(*e));
  e->name = xstrdup(name);
  e->native = xstrdup(native);
  os_lstat(native, &e->st);
  if (e->st.is_link) {
    e->tok = os_stat(native, &e->tst) == 0;
    e->target = os_readlink(native);
    if (ls->deref && e->tok) {
      e->st = e->tst;
      e->st.is_link = 0;
    }
  }
  else {
    e->tst = e->st;
    e->tok = 1;
  }
}


static void ent_free (LsEnt *e) {
  free(e->name);
  free(e->native);
  free(e->target);
}


/* the LS_COLORS code for an entry, or NULL */
static const char *color_lookup (Ls *ls, const char *key, char *buf, size_t cap) {
  const char *p = ls->colors;
  size_t kl = strlen(key);
  if (p == NULL) return NULL;
  while (*p) {
    const char *end = strchr(p, ':'), *eq;
    size_t n = end ? (size_t)(end - p) : strlen(p);
    eq = (const char *)memchr(p, '=', n);
    if (eq && (size_t)(eq - p) == kl &&
        (key[0] == '*' ? m_strnicmp(p, key, kl) == 0 : strncmp(p, key, kl) == 0)) {
      size_t vl = n - kl - 1;
      if (vl >= cap) vl = cap - 1;
      memcpy(buf, eq + 1, vl);
      buf[vl] = '\0';
      return buf;
    }
    if (!end) break;
    p = end + 1;
  }
  return NULL;
}


static const char *ext_color (const char *name) {
  static const char *const arch[] = {"tar", "tgz", "zip", "gz", "7z", "rar", "xz", "bz2",
    "zst", "deb", "rpm", "jar", "tbz2", "txz", "lz", "lzma", "cab", "apk", NULL};
  static const char *const img[] = {"jpg", "jpeg", "png", "gif", "bmp", "svg", "webp", "ico",
    "tif", "tiff", "mp4", "mkv", "avi", "mov", "webm", "wmv", "flv", NULL};
  static const char *const audio[] = {"mp3", "flac", "wav", "ogg", "m4a", "aac", "opus", NULL};
  const char *dot = strrchr(name, '.');
  int i;
  if (dot == NULL || dot == name) return NULL;
  dot++;
  for (i = 0; arch[i]; i++) if (m_stricmp(dot, arch[i]) == 0) return "01;31";
  for (i = 0; img[i]; i++) if (m_stricmp(dot, img[i]) == 0) return "01;35";
  for (i = 0; audio[i]; i++) if (m_stricmp(dot, audio[i]) == 0) return "00;36";
  return NULL;
}


static const char *ent_color (Ls *ls, const LsEnt *e, char *buf, size_t cap) {
  const char *key, *c;
  const OsStat *s = &e->st;
  if (s->is_link) key = e->tok ? "ln" : "or";
  else if (s->is_dir) key = (s->mode & 01002) == 01002 ? "tw" : (s->mode & 02) ? "ow" : "di";
  else if (s->is_fifo) key = "pi";
  else if (s->is_sock) key = "so";
  else if (s->is_blk) key = "bd";
  else if (s->is_chr) key = "cd";
  else if (s->mode & 0111) key = "ex";
  else key = NULL;
  if (key && (c = color_lookup(ls, key, buf, cap)) != NULL) return c;
  if (key == NULL || strcmp(key, "ex") == 0) {	/* by extension */
    const char *dot = strrchr(e->name, '.');
    if (dot && dot[1]) {
      char k[64];
      snprintf(k, sizeof(k), "*%s", dot);
      if ((c = color_lookup(ls, k, buf, cap)) != NULL) return c;
    }
    if (key == NULL) return (c = ext_color(e->name)) != NULL ? c : NULL;
  }
  if (strcmp(key, "ln") == 0) return "01;36";
  if (strcmp(key, "or") == 0) return "40;31;01";
  if (strcmp(key, "di") == 0) return "01;34";
  if (strcmp(key, "tw") == 0) return "30;42";
  if (strcmp(key, "ow") == 0) return "34;42";
  if (strcmp(key, "pi") == 0) return "40;33";
  if (strcmp(key, "so") == 0) return "01;35";
  if (strcmp(key, "bd") == 0 || strcmp(key, "cd") == 0) return "40;33;01";
  if (strcmp(key, "ex") == 0) return "01;32";
  return NULL;
}


static char ent_indicator (Ls *ls, const LsEnt *e) {
  const OsStat *s = &e->st;
  if (ls->slash) return s->is_dir ? '/' : 0;
  if (!ls->classify) return 0;
  if (s->is_dir) return '/';
  if (s->is_link) return ls->format == FMT_LONG ? 0 : '@';
  if (s->is_fifo) return '|';
  if (s->is_sock) return '=';
  if (s->mode & 0111) return '*';
  return 0;
}


/* name with its color and indicator; returns the columns it takes */
static int put_name (Ls *ls, Buf *b, const LsEnt *e) {
  char cbuf[64], ind = ent_indicator(ls, e);
  const char *c = ls->color ? ent_color(ls, e, cbuf, sizeof(cbuf)) : NULL;
  int w = tool_utf8_cols(e->name, strlen(e->name));
  if (c) buf_printf(b, "\033[%sm", c);
  buf_puts(b, e->name);
  if (c) buf_puts(b, "\033[0m");
  if (ind) {
    buf_putc(b, ind);
    w++;
  }
  return w;
}


static void ls_time_str (Ls *ls, char *out, size_t cap, time_t t) {
  struct tm *tm = localtime(&t);
  time_t now = time(NULL);
  if (tm == NULL) {
    snprintf(out, cap, "?");
    return;
  }
  if (ls && ls->full_time) strftime(out, cap, "%Y-%m-%d %H:%M:%S.000000000 %z", tm);
  else if (t > now - 15778476 && t <= now + 60) strftime(out, cap, "%b %e %H:%M", tm);
  else strftime(out, cap, "%b %e  %Y", tm);
}


void ls_long_line (Out *o, const char *name, const char *native, const OsStat *st, int human) {
  char mode[11], when[64], size[32];
  char *user = os_user_name(st->uid), *group = os_group_name(st->gid);
  mode_string(mode, st);
  ls_time_str(NULL, when, sizeof(when), st->mtime);
  if (human) human_size(size, (unsigned long long)st->size, 0);
  else sprintf(size, "%lld", st->size);
  out_printf(o, "%9llu %6lld %s %3lu %-8s %-8s %8s %s %s", st->ino, (st->blocks + 1) / 2,
             mode, st->nlink, user, group, size, when, name);
  if (st->is_link) {
    char *t = os_readlink(native);
    if (t) out_printf(o, " -> %s", t);
    free(t);
  }
  out_putc(o, '\n');
  free(user);
  free(group);
}


static void ls_print_long (Ls *ls, LsEnt *v, size_t n, int total_line) {
  size_t i;
  int wl = 1, wu = 1, wg = 1, ws = 1, wi = 1, wb = 1;
  char **users = (char **)xmalloc((n + 1) * sizeof(char *));
  char **groups = (char **)xmalloc((n + 1) * sizeof(char *));
  char **sizes = (char **)xmalloc((n + 1) * sizeof(char *));
  char **blks = (char **)xmalloc((n + 1) * sizeof(char *));
  long long total = 0;
  char tmp[64];
  for (i = 0; i < n; i++) {
    const OsStat *s = &v[i].st;
    int l;
    total += s->blocks;
    users[i] = ls->numeric ? xstrdup(ll_to_str(s->uid, tmp)) : os_user_name(s->uid);
    groups[i] = ls->numeric ? xstrdup(ll_to_str(s->gid, tmp)) : os_group_name(s->gid);
    if (ls->human) human_size(tmp, (unsigned long long)s->size, ls->si);
    else sprintf(tmp, "%lld", s->size);
    sizes[i] = xstrdup(tmp);
    if (ls->human) human_size(tmp, (unsigned long long)s->blocks * 512, ls->si);
    else sprintf(tmp, "%lld", (s->blocks + 1) / 2);
    blks[i] = xstrdup(tmp);
    sprintf(tmp, "%lu", s->nlink);
    if ((l = (int)strlen(tmp)) > wl) wl = l;
    if ((l = (int)strlen(users[i])) > wu) wu = l;
    if ((l = (int)strlen(groups[i])) > wg) wg = l;
    if ((l = (int)strlen(sizes[i])) > ws) ws = l;
    if ((l = (int)strlen(blks[i])) > wb) wb = l;
    sprintf(tmp, "%llu", s->ino);
    if ((l = (int)strlen(tmp)) > wi) wi = l;
  }
  if (total_line) {
    if (ls->human) {
      human_size(tmp, (unsigned long long)total * 512, ls->si);
      out_printf(ls->o, "total %s\n", tmp);
    }
    else out_printf(ls->o, "total %lld\n", (total + 1) / 2);
  }
  for (i = 0; i < n && !ls->o->failed; i++) {
    LsEnt *e = &v[i];
    char mode[11], when[64];
    Buf b;
    buf_init(&b);
    mode_string(mode, &e->st);
    ls_time_str(ls, when, sizeof(when), ent_time(ls, e));
    if (ls->inode) buf_printf(&b, "%*llu ", wi, e->st.ino);
    if (ls->blocks) buf_printf(&b, "%*s ", wb, blks[i]);
    buf_printf(&b, "%s %*lu ", mode, wl, e->st.nlink);
    if (!ls->no_owner) buf_printf(&b, "%-*s ", wu, users[i]);
    if (!ls->no_group) buf_printf(&b, "%-*s ", wg, groups[i]);
    buf_printf(&b, "%*s %s ", ws, sizes[i], when);
    put_name(ls, &b, e);
    if (e->st.is_link && e->target) {
      buf_puts(&b, " -> ");
      if (ls->color && e->tok) {	/* the target gets its own color */
        LsEnt t;
        char cbuf[64];
        const char *c;
        memset(&t, 0, sizeof(t));
        t.name = e->target;
        t.st = e->tst;
        t.tst = e->tst;
        t.tok = 1;
        c = ent_color(ls, &t, cbuf, sizeof(cbuf));
        if (c) buf_printf(&b, "\033[%sm%s\033[0m", c, e->target);
        else buf_puts(&b, e->target);
      }
      else buf_puts(&b, e->target);
      if (ls->classify && e->tok) {
        if (e->tst.is_dir) buf_putc(&b, '/');
        else if (e->tst.mode & 0111) buf_putc(&b, '*');
      }
    }
    buf_putc(&b, '\n');
    out_putn(ls->o, b.s, b.len);
    buf_free(&b);
    free(users[i]);
    free(groups[i]);
    free(sizes[i]);
    free(blks[i]);
  }
  for (; i < n; i++) {
    free(users[i]);
    free(groups[i]);
    free(sizes[i]);
    free(blks[i]);
  }
  free(users);
  free(groups);
  free(sizes);
  free(blks);
}


static void ls_print_short (Ls *ls, LsEnt *v, size_t n) {
  Buf *cells = (Buf *)xmalloc((n + 1) * sizeof(Buf));
  int *w = (int *)xmalloc((n + 1) * sizeof(int));
  size_t i, cols = 1, rows = n, c, r;
  int *colw = NULL;
  char pre[64];
  if (n == 0) {
    free(cells);
    free(w);
    return;
  }
  for (i = 0; i < n; i++) {
    buf_init(&cells[i]);
    pre[0] = '\0';
    if (ls->inode) {
      sprintf(pre, "%llu ", v[i].st.ino);
      buf_puts(&cells[i], pre);
    }
    if (ls->blocks) {
      char t[32];
      if (ls->human) human_size(t, (unsigned long long)v[i].st.blocks * 512, ls->si);
      else sprintf(t, "%lld", (v[i].st.blocks + 1) / 2);
      buf_printf(&cells[i], "%s ", t);
      strcat(pre, t);
      strcat(pre, " ");
    }
    w[i] = (int)strlen(pre) + put_name(ls, &cells[i], &v[i]);
  }
  if (ls->format == FMT_COMMAS) {
    int col = 0;
    for (i = 0; i < n; i++) {
      int need = w[i] + (i + 1 < n ? 2 : 0);
      if (i > 0 && col + need > ls->width) {
        out_putc(ls->o, '\n');
        col = 0;
      }
      else if (i > 0) {
        out_putc(ls->o, ' ');
        col++;
      }
      out_putn(ls->o, cells[i].s, cells[i].len);
      if (i + 1 < n) out_putc(ls->o, ',');
      col += w[i] + 1;
    }
    out_putc(ls->o, '\n');
  }
  else {
    if (ls->format == FMT_COLS || ls->format == FMT_ACROSS) {
      /* the most columns that fit, each as wide as its widest name */
      size_t try_;
      colw = (int *)xmalloc((n + 1) * sizeof(int));
      for (try_ = n; try_ >= 1; try_--) {
        size_t rr = (n + try_ - 1) / try_, cc;
        int total = 0;
        if (try_ > 1 && (n + rr - 1) / rr != try_ && ls->format == FMT_COLS) continue;
        for (cc = 0; cc < try_; cc++) colw[cc] = 0;
        for (i = 0; i < n; i++) {
          size_t col = ls->format == FMT_COLS ? i / rr : i % try_;
          if (w[i] > colw[col]) colw[col] = w[i];
        }
        for (cc = 0; cc < try_; cc++) total += colw[cc] + (cc + 1 < try_ ? 2 : 0);
        if (total <= ls->width || try_ == 1) {
          cols = try_;
          rows = rr;
          break;
        }
      }
    }
    else {
      cols = 1;
      rows = n;
    }
    for (r = 0; r < rows && !ls->o->failed; r++) {
      for (c = 0; c < cols; c++) {
        size_t k = ls->format == FMT_ACROSS ? r * cols + c : c * rows + r;
        size_t nextk = ls->format == FMT_ACROSS ? k + 1 : (c + 1) * rows + r;
        int pad;
        if (k >= n) break;
        out_putn(ls->o, cells[k].s, cells[k].len);
        if (c + 1 < cols && nextk < n && (ls->format != FMT_ACROSS || c + 1 < cols)) {
          for (pad = w[k]; pad < colw[c] + 2; pad++) out_putc(ls->o, ' ');
        }
      }
      out_putc(ls->o, '\n');
    }
  }
  for (i = 0; i < n; i++) buf_free(&cells[i]);
  free(cells);
  free(w);
  free(colw);
}


static void ls_print (Ls *ls, LsEnt *v, size_t n, int total_line) {
  if (ls->format == FMT_LONG) ls_print_long(ls, v, n, total_line);
  else ls_print_short(ls, v, n);
}


static int ls_ignored (Ls *ls, const char *name) {
  size_t i;
  if (name[0] == '.') {
    if (!ls->all && !ls->almost) return 1;
    if (ls->almost && !ls->all && is_dot_or_dotdot(name)) return 1;
  }
  for (i = 0; i < ls->ignore.n; i++)
    if (pat_match(ls->ignore.v[i], name, 0)) return 1;
  return 0;
}


static void ls_dir (Ls *ls, const char *disp, const char *native, int header, int *first) {
  Vec names;
  LsEnt *v;
  size_t i, n = 0;
  if (tool_stop() || ls->o->failed) return;
  vec_init(&names);
  if (os_listdir(native, &names) != 0) {
    out_flush(ls->o);
    tool_err(ls->err, "ls", "cannot open directory '%s': %s", disp, os_errmsg());
    ls->status = ls->status > 1 ? ls->status : 1;
    vec_free(&names);
    return;
  }
  if (ls->all) {
    vec_push(&names, xstrdup("."));
    vec_push(&names, xstrdup(".."));
  }
  v = (LsEnt *)xmalloc((names.n + 1) * sizeof(LsEnt));
  for (i = 0; i < names.n; i++) {
    char *full;
    if (ls_ignored(ls, names.v[i])) continue;
    full = path_join(native, names.v[i]);
    ent_fill(ls, &v[n++], names.v[i], full);
    free(full);
  }
  ls_sort(ls, v, n);
  if (header) {
    if (!*first) out_putc(ls->o, '\n');
    out_printf(ls->o, "%s:\n", disp);
  }
  *first = 0;
  ls_print(ls, v, n, 1);
  if (ls->recursive) {
    for (i = 0; i < n; i++) {
      LsEnt *e = &v[i];
      if (e->st.is_dir && !e->st.is_link && !is_dot_or_dotdot(e->name)) {
        char *d = tool_join(disp, e->name);
        ls_dir(ls, d, e->native, 1, first);
        free(d);
      }
    }
  }
  for (i = 0; i < n; i++) ent_free(&v[i]);
  free(v);
  vec_free(&names);
}


int t_ls (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {
    {"all", 'a', 0}, {"almost-all", 'A', 0}, {"human-readable", 'h', 0}, {"si", 1001, 0},
    {"reverse", 'r', 0}, {"recursive", 'R', 0}, {"directory", 'd', 0}, {"classify", 'F', 2},
    {"color", 1002, 2}, {"colour", 1002, 2}, {"group-directories-first", 1003, 0},
    {"full-time", 1004, 0}, {"numeric-uid-gid", 'n', 0}, {"inode", 'i', 0}, {"size", 's', 0},
    {"dereference", 'L', 0}, {"ignore", 'I', 1}, {"hide", 1005, 1}, {"sort", 1006, 1},
    {"format", 1007, 1}, {"indicator-style", 1008, 1}, {"no-group", 'G', 0},
    {"literal", 'N', 0}, {"width", 'w', 1}, {"kibibytes", 'k', 0}, {"time", 1009, 1},
    {NULL, 0, 0}};
  Ls ls;
  Opts g;
  Out o;
  int c, first = 1, color_when = -1, forced_format = -1;
  size_t i, nfiles = 0;
  LsEnt *files, *dirs;
  size_t ndirs = 0;
  const char *cols;
  (void)in;
  memset(&ls, 0, sizeof(ls));
  vec_init(&ls.ignore);
  out_init(&o, out);
  ls.o = &o;
  ls.err = err;
  ls.sort = SORT_NAME;
  opts_init(&g, "ls", argc, argv, err);
  while ((c = opts_next(&g, "aAlhrtSXUvR1dFpCxmngoGisLcuI:Nw:kQ", lo)) != 0) {
    switch (c) {
      case 'a': ls.all = 1; break;
      case 'A': ls.almost = 1; break;
      case 'l': forced_format = FMT_LONG; break;
      case 'h': ls.human = 1; break;
      case 1001: ls.human = ls.si = 1; break;
      case 'r': ls.reverse = 1; break;
      case 't': ls.sort = SORT_TIME; break;
      case 'S': ls.sort = SORT_SIZE; break;
      case 'X': ls.sort = SORT_EXT; break;
      case 'U': ls.sort = SORT_NONE; break;
      case 'v': ls.sort = SORT_VERSION; break;
      case 'R': ls.recursive = 1; break;
      case '1': forced_format = FMT_ONE; break;
      case 'C': forced_format = FMT_COLS; break;
      case 'x': forced_format = FMT_ACROSS; break;
      case 'm': forced_format = FMT_COMMAS; break;
      case 'd': ls.dirs_only = 1; break;
      case 'F': ls.classify = 1; break;
      case 'p': ls.slash = 1; break;
      case 'n': ls.numeric = 1; forced_format = FMT_LONG; break;
      case 'g': ls.no_owner = 1; forced_format = FMT_LONG; break;
      case 'o': ls.no_group = 1; forced_format = FMT_LONG; break;
      case 'G': ls.no_group = 1; break;
      case 'i': ls.inode = 1; break;
      case 's': ls.blocks = 1; break;
      case 'k': ls.kilo = 1; break;
      case 'L': ls.deref = 1; break;
      case 'c': ls.timekind = TIME_C; break;
      case 'u': ls.timekind = TIME_A; break;
      case 'N': case 'Q': break;
      case 'I': case 1005: vec_push(&ls.ignore, glob_mark(g.arg)); break;
      case 'w': ls.width = atoi(g.arg); break;
      case 1002:
        if (g.arg == NULL || strcmp(g.arg, "always") == 0 || strcmp(g.arg, "yes") == 0 ||
            strcmp(g.arg, "force") == 0) color_when = 1;
        else if (strcmp(g.arg, "never") == 0 || strcmp(g.arg, "no") == 0 ||
                 strcmp(g.arg, "none") == 0) color_when = 0;
        else color_when = -1;
        break;
      case 1003: ls.group_first = 1; break;
      case 1004: ls.full_time = 1; forced_format = FMT_LONG; break;
      case 1006:
        ls.sort = strcmp(g.arg, "time") == 0 ? SORT_TIME : strcmp(g.arg, "size") == 0 ? SORT_SIZE :
                  strcmp(g.arg, "extension") == 0 ? SORT_EXT : strcmp(g.arg, "none") == 0 ? SORT_NONE :
                  strcmp(g.arg, "version") == 0 ? SORT_VERSION : SORT_NAME;
        break;
      case 1007:
        forced_format = strcmp(g.arg, "long") == 0 || strcmp(g.arg, "verbose") == 0 ? FMT_LONG :
                        strcmp(g.arg, "single-column") == 0 ? FMT_ONE :
                        strcmp(g.arg, "commas") == 0 ? FMT_COMMAS :
                        strcmp(g.arg, "across") == 0 || strcmp(g.arg, "horizontal") == 0 ? FMT_ACROSS : FMT_COLS;
        break;
      case 1008:
        ls.classify = strcmp(g.arg, "classify") == 0;
        ls.slash = strcmp(g.arg, "slash") == 0;
        break;
      case 1009:
        ls.timekind = (strcmp(g.arg, "ctime") == 0 || strcmp(g.arg, "status") == 0) ? TIME_C :
                      (strcmp(g.arg, "atime") == 0 || strcmp(g.arg, "access") == 0 ||
                       strcmp(g.arg, "use") == 0) ? TIME_A : TIME_M;
        break;
      case OPT_HELP:
        opts_free(&g);
        vec_free(&ls.ignore);
        return tool_help(out, "ls");
      default:
        opts_free(&g);
        vec_free(&ls.ignore);
        return 2;
    }
  }
  /* -t with -c or -u sorts by that time; -lc shows it */
  if (ls.timekind != TIME_M && forced_format != FMT_LONG && ls.sort == SORT_NAME)
    ls.sort = SORT_TIME;
  if (forced_format >= 0) ls.format = forced_format;
  else ls.format = os_is_tty(out) ? FMT_COLS : FMT_ONE;
  ls.color = color_when >= 0 ? color_when : os_is_tty(out);
  if (ls.color) {
    const char *lc = var_get("LS_COLORS");
    ls.colors = lc ? xstrdup(lc) : NULL;
  }
  if (ls.width <= 0) {
    cols = var_get("COLUMNS");
    ls.width = (cols && atoi(cols) > 0) ? atoi(cols) : os_is_tty(out) ? os_term_cols() : 80;
  }
  ls.ci_sort = !locale_is_c();
  if (g.ops.n == 0) vec_push(&g.ops, xstrdup("."));
  files = (LsEnt *)xmalloc((g.ops.n + 1) * sizeof(LsEnt));
  dirs = (LsEnt *)xmalloc((g.ops.n + 1) * sizeof(LsEnt));
  for (i = 0; i < g.ops.n; i++) {
    char *native = path_to_native(g.ops.v[i]);
    LsEnt e;
    OsStat probe;
    if (os_lstat(native, &probe) != 0) {
      out_flush(&o);
      tool_err(err, "ls", "cannot access '%s': %s", g.ops.v[i], os_errmsg());
      ls.status = 2;
      free(native);
      continue;
    }
    ent_fill(&ls, &e, g.ops.v[i], native);
    /* a link to a folder named on the command line: its contents (no -l) */
    if (!ls.dirs_only && (e.st.is_dir || (e.st.is_link && e.tok && e.tst.is_dir &&
                                         ls.format != FMT_LONG)))
      dirs[ndirs++] = e;
    else files[nfiles++] = e;
    free(native);
  }
  ls_sort(&ls, files, nfiles);
  ls_sort(&ls, dirs, ndirs);
  if (nfiles > 0) {
    ls_print(&ls, files, nfiles, 0);
    first = 0;
  }
  for (i = 0; i < ndirs; i++) {
    int header = ls.recursive || g.ops.n > 1;
    ls_dir(&ls, dirs[i].name, dirs[i].native, header, &first);
  }
  for (i = 0; i < nfiles; i++) ent_free(&files[i]);
  for (i = 0; i < ndirs; i++) ent_free(&dirs[i]);
  free(files);
  free(dirs);
  out_flush(&o);
  opts_free(&g);
  vec_free(&ls.ignore);
  free(ls.colors);
  return tool_stop() ? 130 : ls.status;
}

/* }================================================================== */


/*
** {==================================================================
** cat
** ===================================================================
*/

int t_cat (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {
    {"number", 'n', 0}, {"number-nonblank", 'b', 0}, {"squeeze-blank", 's', 0},
    {"show-nonprinting", 'v', 0}, {"show-ends", 'E', 0}, {"show-tabs", 'T', 0},
    {"show-all", 'A', 0}, {NULL, 0, 0}};
  Opts g;
  Out o;
  int c, number = 0, nonblank = 0, squeeze = 0, nonprint = 0, ends = 0, tabs = 0, status = 0;
  long long lineno = 0;
  int blank_run = 0;
  size_t i;
  opts_init(&g, "cat", argc, argv, err);
  while ((c = opts_next(&g, "nbsvETAetu", lo)) != 0) {
    switch (c) {
      case 'n': number = 1; break;
      case 'b': number = nonblank = 1; break;
      case 's': squeeze = 1; break;
      case 'v': nonprint = 1; break;
      case 'E': ends = 1; break;
      case 'T': tabs = 1; break;
      case 'A': nonprint = ends = tabs = 1; break;
      case 'e': nonprint = ends = 1; break;
      case 't': nonprint = tabs = 1; break;
      case 'u': break;
      case OPT_HELP: opts_free(&g); return tool_help(out, "cat");
      default: opts_free(&g); return 1;
    }
  }
  if (g.ops.n == 0) vec_push(&g.ops, xstrdup("-"));
  out_init(&o, out);
  for (i = 0; i < g.ops.n && !o.failed && !tool_stop(); i++) {
    In r;
    if (in_open(&r, g.ops.v[i], in) != 0) {
      char *native = path_to_native(g.ops.v[i]);
      OsStat st;
      out_flush(&o);
      if (os_stat(native, &st) == 0 && st.is_dir) tool_err(err, "cat", "%s: Is a directory", g.ops.v[i]);
      else tool_err(err, "cat", "%s: %s", g.ops.v[i], os_errmsg());
      free(native);
      status = 1;
      continue;
    }
    if (!number && !squeeze && !nonprint && !ends && !tabs) {	/* plain copy */
      char chunk[65536];
      long n;
      out_flush(&o);
      while (!o.failed && (n = in_read(&r, chunk, sizeof(chunk))) > 0) {
        if (os_write(out, chunk, (size_t)n) < 0) o.failed = 1;
        if (tool_stop()) break;
      }
    }
    else {
      char *line;
      size_t len;
      int had;
      while (!o.failed && in_line(&r, &line, &len, '\n', &had)) {
        size_t k;
        if (len == 0 && had) {
          if (squeeze && blank_run) continue;
          blank_run = 1;
        }
        else blank_run = 0;
        if (number && !(nonblank && len == 0)) out_printf(&o, "%6lld\t", ++lineno);
        for (k = 0; k < len; k++) {
          unsigned char ch = (unsigned char)line[k];
          if (ch == '\t') {
            if (tabs) out_puts(&o, "^I");
            else out_putc(&o, '\t');
          }
          else if (nonprint && (ch < 32 || ch >= 127)) {
            if (ch >= 128) {
              out_puts(&o, "M-");
              ch -= 128;
            }
            if (ch < 32) {
              out_putc(&o, '^');
              out_putc(&o, ch + 64);
            }
            else if (ch == 127) out_puts(&o, "^?");
            else out_putc(&o, ch);
          }
          else out_putc(&o, ch);
        }
        if (had) {
          if (ends) out_putc(&o, '$');
          out_putc(&o, '\n');
        }
      }
    }
    in_close(&r);
  }
  out_flush(&o);
  opts_free(&g);
  return tool_stop() ? 130 : status;
}

/* }================================================================== */


/*
** {==================================================================
** mkdir, rmdir
** ===================================================================
*/

int t_mkdir (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {{"parents", 'p', 0}, {"verbose", 'v', 0}, {"mode", 'm', 1},
                               {NULL, 0, 0}};
  Opts g;
  int c, parents = 0, verbose = 0, status = 0, have_mode = 0;
  unsigned mode = 0;
  size_t i;
  Out o;
  (void)in;
  opts_init(&g, "mkdir", argc, argv, err);
  while ((c = opts_next(&g, "pvm:", lo)) != 0) {
    if (c == 'p') parents = 1;
    else if (c == 'v') verbose = 1;
    else if (c == 'm') {
      if (parse_mode(g.arg, 0777 & ~(unsigned)os_umask(-1), 1, &mode) != 0) {
        tool_err(err, "mkdir", "invalid mode '%s'", g.arg);
        opts_free(&g);
        return 1;
      }
      have_mode = 1;
    }
    else if (c == OPT_HELP) { opts_free(&g); return tool_help(out, "mkdir"); }
    else { opts_free(&g); return 1; }
  }
  if (g.ops.n == 0) {
    tool_err(err, "mkdir", "missing operand");
    opts_free(&g);
    return 1;
  }
  out_init(&o, out);
  for (i = 0; i < g.ops.n; i++) {
    const char *arg = g.ops.v[i];
    char *native = path_to_native(arg);
    if (parents) {	/* each missing part, from the top */
      size_t k, n = strlen(arg);
      int failed = 0;
      for (k = 1; k <= n && !failed; k++) {
        if (k < n && arg[k] != '/') continue;
        if (k < n && k > 0 && arg[k - 1] == '/') continue;
        {
          char *part = xstrndup(arg, k), *pn = path_to_native(part);
          OsStat st;
          if (os_stat(pn, &st) != 0) {
            if (os_mkdir(pn) != 0 && !(os_stat(pn, &st) == 0 && st.is_dir)) {
              out_flush(&o);
              tool_err(err, "mkdir", "cannot create directory '%s': %s", part, os_errmsg());
              failed = 1;
            }
            else if (verbose) out_printf(&o, "mkdir: created directory '%s'\n", part);
          }
          else if (!st.is_dir) {
            out_flush(&o);
            tool_err(err, "mkdir", "cannot create directory '%s': %s", part,
                     k == n ? "File exists" : "Not a directory");
            failed = 1;
          }
          free(part);
          free(pn);
        }
      }
      if (failed) status = 1;
    }
    else if (os_mkdir(native) != 0) {
      out_flush(&o);
      tool_err(err, "mkdir", "cannot create directory '%s': %s", arg, os_errmsg());
      status = 1;
    }
    else if (verbose) out_printf(&o, "mkdir: created directory '%s'\n", arg);
    if (have_mode && status == 0) os_chmod(native, mode);
    free(native);
  }
  out_flush(&o);
  opts_free(&g);
  return status;
}


int t_rmdir (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {{"parents", 'p', 0}, {"verbose", 'v', 0},
                               {"ignore-fail-on-non-empty", 1001, 0}, {NULL, 0, 0}};
  Opts g;
  int c, parents = 0, verbose = 0, ignore = 0, status = 0;
  size_t i;
  Out o;
  (void)in;
  opts_init(&g, "rmdir", argc, argv, err);
  while ((c = opts_next(&g, "pv", lo)) != 0) {
    if (c == 'p') parents = 1;
    else if (c == 'v') verbose = 1;
    else if (c == 1001) ignore = 1;
    else if (c == OPT_HELP) { opts_free(&g); return tool_help(out, "rmdir"); }
    else { opts_free(&g); return 1; }
  }
  if (g.ops.n == 0) {
    tool_err(err, "rmdir", "missing operand");
    opts_free(&g);
    return 1;
  }
  out_init(&o, out);
  for (i = 0; i < g.ops.n; i++) {
    char *path = xstrdup(g.ops.v[i]);
    for (;;) {
      char *native = path_to_native(path);
      size_t n;
      int r;
      if (verbose) out_printf(&o, "rmdir: removing directory, '%s'\n", path);
      r = os_rmdir(native);
      free(native);
      if (r != 0) {
        int e = os_errcode();
        if (!(ignore && (e == OS_E_NOTEMPTY || e == OS_E_EXIST))) {
          out_flush(&o);
          tool_err(err, "rmdir", "failed to remove '%s': %s", path, os_errmsg());
          status = 1;
        }
        break;
      }
      if (!parents) break;
      n = strlen(path);	/* a/b/c -> a/b */
      while (n > 0 && path[n - 1] == '/') n--;
      while (n > 0 && path[n - 1] != '/') n--;
      while (n > 0 && path[n - 1] == '/') n--;
      if (n == 0) break;
      path[n] = '\0';
    }
    free(path);
  }
  out_flush(&o);
  opts_free(&g);
  return status;
}

/* }================================================================== */


/*
** {==================================================================
** rm
** ===================================================================
*/

typedef struct RmOpt {
  int force, interactive, recursive, dirs, verbose, preserve_root;
  int in, err, status;
  Out *o;
} RmOpt;


static int rm_unlink (const OsStat *st, const char *native) {
#ifdef _WIN32
  int r = (st->is_link && st->is_dir) ? os_rmdir(native) : os_unlink(native);
  if (r != 0 && os_errcode() == OS_E_ACCES && !st->is_link) {
    os_chmod(native, 0666);	/* read-only (git objects): Windows needs the bit off */
    r = os_unlink(native);
  }
  return r;
#else
  (void)st;
  return os_unlink(native);
#endif
}


static const char *file_kind (const OsStat *st) {
  if (st->is_link) return "symbolic link";
  if (st->is_dir) return "directory";
  if (st->is_fifo) return "fifo";
  if (st->is_sock) return "socket";
  if (st->size == 0) return "regular empty file";
  return "regular file";
}


static void rm_path (RmOpt *r, const char *disp, const char *native) {
  OsStat st;
  if (tool_stop()) return;
  if (os_lstat(native, &st) != 0) {
    if (!(r->force && os_errcode() == OS_E_NOENT)) {
      out_flush(r->o);
      tool_err(r->err, "rm", "cannot remove '%s': %s", disp, os_errmsg());
      r->status = 1;
    }
    return;
  }
  if (st.is_dir && !st.is_link) {
    Vec names;
    size_t i;
    if (!r->recursive) {
      if (r->dirs) {
        if (r->interactive && !tool_ask(r->in, r->err, "rm: remove directory '%s'? ", disp)) return;
        if (os_rmdir(native) == 0) {
          if (r->verbose) out_printf(r->o, "removed directory '%s'\n", disp);
          return;
        }
        out_flush(r->o);
        tool_err(r->err, "rm", "cannot remove '%s': %s", disp, os_errmsg());
      }
      else {
        out_flush(r->o);
        tool_err(r->err, "rm", "cannot remove '%s': Is a directory", disp);
      }
      r->status = 1;
      return;
    }
    vec_init(&names);
    if (os_listdir(native, &names) != 0) {
      out_flush(r->o);
      tool_err(r->err, "rm", "cannot remove '%s': %s", disp, os_errmsg());
      r->status = 1;
      vec_free(&names);
      return;
    }
    if (r->interactive && names.n > 0 &&
        !tool_ask(r->in, r->err, "rm: descend into directory '%s'? ", disp)) {
      vec_free(&names);
      return;
    }
    for (i = 0; i < names.n && !tool_stop(); i++) {
      char *cd = tool_join(disp, names.v[i]), *cn = path_join(native, names.v[i]);
      rm_path(r, cd, cn);
      free(cd);
      free(cn);
    }
    vec_free(&names);
    if (tool_stop()) return;
    if (r->interactive && !tool_ask(r->in, r->err, "rm: remove directory '%s'? ", disp)) return;
    if (os_rmdir(native) != 0) {
#ifdef _WIN32
      if (os_errcode() == OS_E_ACCES) {	/* a read-only folder */
        os_chmod(native, 0777);
        if (os_rmdir(native) == 0) {
          if (r->verbose) out_printf(r->o, "removed directory '%s'\n", disp);
          return;
        }
      }
#endif
      out_flush(r->o);
      tool_err(r->err, "rm", "cannot remove '%s': %s", disp, os_errmsg());
      r->status = 1;
      return;
    }
    if (r->verbose) out_printf(r->o, "removed directory '%s'\n", disp);
    return;
  }
  if (r->interactive) {
    if (!tool_ask(r->in, r->err, "rm: remove %s '%s'? ", file_kind(&st), disp)) return;
  }
  else if (!r->force && !st.is_link && !(st.mode & 0222) && os_is_tty(r->in)) {
    if (!tool_ask(r->in, r->err, "rm: remove write-protected %s '%s'? ", file_kind(&st), disp))
      return;
  }
  if (rm_unlink(&st, native) != 0) {
    out_flush(r->o);
    tool_err(r->err, "rm", "cannot remove '%s': %s", disp, os_errmsg());
    r->status = 1;
    return;
  }
  if (r->verbose) out_printf(r->o, "removed '%s'\n", disp);
}


/* "/" (the mmc folder), a drive, the real root: rm -r refuses them */
static int is_root_dir (const char *native) {
  char *real = os_realpath(native);
  const char *root = path_root();
  int r = 0;
  size_t n;
  if (real == NULL) return 0;
  n = strlen(real);
  while (n > 1 && path_is_sep(real[n - 1]) && !(n == 3 && real[1] == ':')) real[--n] = '\0';
  if (strcmp(real, "/") == 0) r = 1;
  else if (n <= 3 && n >= 2 && real[1] == ':') r = 1;	/* C: C:\ */
  else if (root[0] && m_fncmp(real, root) == 0) r = 1;
  free(real);
  return r;
}


int t_rm (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {
    {"force", 'f', 0}, {"interactive", 1001, 2}, {"recursive", 'r', 0}, {"dir", 'd', 0},
    {"verbose", 'v', 0}, {"no-preserve-root", 1002, 0}, {"preserve-root", 1003, 2},
    {"one-file-system", 1004, 0}, {NULL, 0, 0}};
  RmOpt r;
  Opts g;
  Out o;
  int c, once = 0;
  size_t i;
  memset(&r, 0, sizeof(r));
  r.preserve_root = 1;
  r.in = in;
  r.err = err;
  opts_init(&g, "rm", argc, argv, err);
  while ((c = opts_next(&g, "fiIrRdv", lo)) != 0) {
    switch (c) {
      case 'f': r.force = 1; r.interactive = 0; once = 0; break;
      case 'i': r.interactive = 1; r.force = 0; break;
      case 'I': once = 1; r.interactive = 0; r.force = 0; break;
      case 1001:
        if (g.arg == NULL || strcmp(g.arg, "always") == 0) r.interactive = 1;
        else if (strcmp(g.arg, "once") == 0) once = 1;
        else r.interactive = 0;
        break;
      case 'r': case 'R': r.recursive = 1; break;
      case 'd': r.dirs = 1; break;
      case 'v': r.verbose = 1; break;
      case 1002: r.preserve_root = 0; break;
      case 1003: case 1004: break;
      case OPT_HELP: opts_free(&g); return tool_help(out, "rm");
      default: opts_free(&g); return 1;
    }
  }
  if (g.ops.n == 0) {
    opts_free(&g);
    if (r.force) return 0;
    tool_err(err, "rm", "missing operand");
    fd_printf(err, "Try 'rm --help' for more information.\n");
    return 1;
  }
  if (once && (g.ops.n > 3 || r.recursive)) {
    int ok = r.recursive ?
      tool_ask(in, err, "rm: remove %d argument%s recursively? ", (int)g.ops.n, g.ops.n > 1 ? "s" : "") :
      tool_ask(in, err, "rm: remove %d arguments? ", (int)g.ops.n);
    if (!ok) {
      opts_free(&g);
      return 0;
    }
  }
  out_init(&o, out);
  r.o = &o;
  for (i = 0; i < g.ops.n && !tool_stop(); i++) {
    const char *arg = g.ops.v[i];
    const char *base = tool_base(arg);
    size_t bl = strlen(base);
    char *native;
    while (bl > 0 && (base[bl - 1] == '/' || base[bl - 1] == '\\')) bl--;
    if ((bl == 1 && base[0] == '.') || (bl == 2 && base[0] == '.' && base[1] == '.')) {
      out_flush(&o);
      tool_err(err, "rm", "refusing to remove '.' or '..' directory: skipping '%s'", arg);
      r.status = 1;
      continue;
    }
    native = path_to_native(arg);
    if (r.recursive && r.preserve_root && is_root_dir(native)) {
      out_flush(&o);
      tool_err(err, "rm", "it is dangerous to operate recursively on '%s'", arg);
      tool_err(err, "rm", "use --no-preserve-root to override this failsafe");
      r.status = 1;
      free(native);
      continue;
    }
    rm_path(&r, arg, native);
    free(native);
  }
  out_flush(&o);
  opts_free(&g);
  return tool_stop() ? 130 : r.status;
}

/* }================================================================== */


/*
** {==================================================================
** cp, mv
** ===================================================================
*/

typedef struct CpOpt {
  const char *tool;
  int recursive, force, interactive, noclobber, verbose, update;
  int keep_mode, keep_time, hard, soft, remove_dest, parents, attr_only;
  int deref;	/* 0: never (-P), 1: always (-L), 2: on the command line (-H) */
  int in, err, status, mv;
  Out *o;
} CpOpt;


/* dst is src itself or inside it? (cp -r a a/b) */
static int inside (const char *src_native, const char *dst_native) {
  char *s = os_realpath(src_native), *d, *dd;
  int r = 0;
  if (s == NULL) return 0;
  d = os_realpath(dst_native);
  if (d == NULL) {	/* dst does not exist yet: look at its folder */
    char *parent = path_dirname(dst_native);
    char *pr = os_realpath(parent);
    free(parent);
    if (pr == NULL) {
      free(s);
      return 0;
    }
    d = path_join(pr, path_basename(dst_native));
    free(pr);
  }
  dd = d;
  {
    size_t n = strlen(s);
    while (n > 1 && path_is_sep(s[n - 1])) s[--n] = '\0';
    if (m_fnncmp(dd, s, n) == 0 && (dd[n] == '\0' || path_is_sep(dd[n]))) r = 1;
  }
  free(s);
  free(d);
  return r;
}


static int copy_data (CpOpt *c, const char *sd, const char *sn, const char *dd, const char *dn,
                      const OsStat *sst, int *created) {
  int in = os_open(sn, OS_READ), outfd;
  long n;
  char *buf;
  OsStat dst;
  int existed = os_stat(dn, &dst) == 0;
  if (in < 0) {
    out_flush(c->o);
    tool_err(c->err, c->tool, "cannot open '%s' for reading: %s", sd, os_errmsg());
    return -1;
  }
  if (c->remove_dest && existed) {
    os_unlink(dn);
    existed = 0;
  }
  outfd = os_open(dn, OS_WRITE);
  if (outfd < 0 && existed && (c->force || c->mv)) {	/* read-only target: away with it */
    os_chmod(dn, 0666);
    os_unlink(dn);
    existed = 0;
    outfd = os_open(dn, OS_WRITE);
  }
  if (outfd < 0) {
    out_flush(c->o);
    tool_err(c->err, c->tool, "cannot create regular file '%s': %s", dd, os_errmsg());
    os_close(in);
    return -1;
  }
  *created = !existed;
  buf = (char *)xmalloc(1 << 18);
  while ((n = os_read(in, buf, 1 << 18)) > 0) {
    if (os_write(outfd, buf, (size_t)n) < 0) {
      out_flush(c->o);
      tool_err(c->err, c->tool, "error writing '%s': %s", dd, os_errmsg());
      free(buf);
      os_close(in);
      os_close(outfd);
      return -1;
    }
    if (tool_stop()) break;
  }
  free(buf);
  os_close(in);
  os_close(outfd);
  if (n < 0) {
    out_flush(c->o);
    tool_err(c->err, c->tool, "error reading '%s': %s", sd, os_errmsg());
    return -1;
  }
  (void)sst;
  return 0;
}


static void copy_attrs (CpOpt *c, const char *dn, const OsStat *sst, int created) {
  if (c->keep_mode) os_chmod(dn, sst->mode & 07777);
  else if (created) os_chmod(dn, sst->mode & 0777 & ~(unsigned)os_umask(-1));
  if (c->keep_time) os_utime(dn, sst->atime, sst->mtime);
}


static void cp_verbose (CpOpt *c, const char *sd, const char *dd) {
  if (!c->verbose) return;
  if (c->mv) out_printf(c->o, "copied '%s' -> '%s'\n", sd, dd);
  else out_printf(c->o, "'%s' -> '%s'\n", sd, dd);
}


static int cp_any (CpOpt *c, const char *sd, const char *sn, const char *dd, const char *dn,
                   int top) {
  OsStat sst, dst;
  int follow = c->deref == 1 || (c->deref == 2 && top), have_dst;
  if (tool_stop()) return -1;
  if ((follow ? os_stat(sn, &sst) : os_lstat(sn, &sst)) != 0) {
    out_flush(c->o);
    tool_err(c->err, c->tool, "cannot stat '%s': %s", sd, os_errmsg());
    return -1;
  }
  have_dst = os_lstat(dn, &dst) == 0;
  if (have_dst && !dst.is_link && os_same_file(sn, dn) && !(sst.is_link && !follow)) {
    out_flush(c->o);
    tool_err(c->err, c->tool, "'%s' and '%s' are the same file", sd, dd);
    return -1;
  }
  if (sst.is_dir && !sst.is_link) {
    Vec names;
    size_t i;
    int created = 0, bad = 0;
    if (!c->recursive) {
      out_flush(c->o);
      tool_err(c->err, c->tool, "-r not specified; omitting directory '%s'", sd);
      return -1;
    }
    if (top && inside(sn, dn)) {
      out_flush(c->o);
      tool_err(c->err, c->tool, "cannot copy a directory, '%s', into itself, '%s'", sd, dd);
      return -1;
    }
    if (have_dst && !(dst.is_dir || (dst.is_link && os_stat(dn, &dst) == 0 && dst.is_dir))) {
      out_flush(c->o);
      tool_err(c->err, c->tool, "cannot overwrite non-directory '%s' with directory '%s'", dd, sd);
      return -1;
    }
    if (!have_dst) {
      if (os_mkdir(dn) != 0) {
        out_flush(c->o);
        tool_err(c->err, c->tool, "cannot create directory '%s': %s", dd, os_errmsg());
        return -1;
      }
      created = 1;
      cp_verbose(c, sd, dd);
    }
    vec_init(&names);
    if (os_listdir(sn, &names) != 0) {
      out_flush(c->o);
      tool_err(c->err, c->tool, "cannot access '%s': %s", sd, os_errmsg());
      vec_free(&names);
      return -1;
    }
    for (i = 0; i < names.n && !tool_stop(); i++) {
      char *csd = tool_join(sd, names.v[i]), *csn = path_join(sn, names.v[i]);
      char *cdd = tool_join(dd, names.v[i]), *cdn = path_join(dn, names.v[i]);
      if (cp_any(c, csd, csn, cdd, cdn, 0) != 0) bad = 1;
      free(csd);
      free(csn);
      free(cdd);
      free(cdn);
    }
    vec_free(&names);
    copy_attrs(c, dn, &sst, created);
    return bad ? -1 : 0;
  }
  if (have_dst) {
    if (dst.is_dir && !dst.is_link) {
      out_flush(c->o);
      tool_err(c->err, c->tool, "cannot overwrite directory '%s' with non-directory", dd);
      return -1;
    }
    if (c->noclobber) return 0;
    if (c->update && dst.mtime >= sst.mtime) return 0;
    if (c->interactive && !tool_ask(c->in, c->err, "%s: overwrite '%s'? ", c->tool, dd)) return 0;
  }
  if (sst.is_link && !follow) {	/* the link itself */
    char *target = os_readlink(sn);
    if (target != NULL) {
      OsStat tst;
      int is_dir = os_stat(sn, &tst) == 0 && tst.is_dir;
      if (have_dst) {
        if (dst.is_link && dst.is_dir) os_rmdir(dn);
        else os_unlink(dn);
      }
      if (os_symlink(target, dn, is_dir) == 0) {
        free(target);
        cp_verbose(c, sd, dd);
        return 0;
      }
      free(target);
#ifndef _WIN32
      out_flush(c->o);
      tool_err(c->err, c->tool, "cannot create symbolic link '%s': %s", dd, os_errmsg());
      return -1;
#else
      /* no symlink rights (Developer Mode off): copy what it points to */
      if (os_stat(sn, &sst) != 0) return -1;
      if (sst.is_dir) {
        int saved = c->deref, r;
        c->deref = 1;
        r = cp_any(c, sd, sn, dd, dn, 0);
        c->deref = saved;
        return r;
      }
#endif
    }
  }
  if (c->hard || c->soft) {
    int r;
    if (have_dst) os_unlink(dn);
    r = c->hard ? os_link(sn, dn) : os_symlink(sd, dn, 0);
    if (r != 0) {
      out_flush(c->o);
      tool_err(c->err, c->tool, "cannot create %s link '%s': %s", c->hard ? "hard" : "symbolic",
               dd, os_errmsg());
      return -1;
    }
    cp_verbose(c, sd, dd);
    return 0;
  }
  {
    int created = 0;
    if (c->attr_only) {
      if (!have_dst) {
        int fd = os_open(dn, OS_WRITE);
        if (fd >= 0) os_close(fd);
        created = 1;
      }
    }
    else if (copy_data(c, sd, sn, dd, dn, &sst, &created) != 0) return -1;
    copy_attrs(c, dn, &sst, created);
  }
  cp_verbose(c, sd, dd);
  return 0;
}


/* where each source goes: dest itself, or dest/name when dest is a folder */
static int cp_targets (CpOpt *c, Opts *g, const char *tdir, int no_target_dir,
                       int (*one) (CpOpt *, const char *, const char *)) {
  size_t i, nsrc;
  const char *dest;
  OsStat st;
  char *dn;
  int is_dir, status = 0;
  if (tdir == NULL && g->ops.n < 2) {
    if (g->ops.n == 0) tool_err(c->err, c->tool, "missing file operand");
    else tool_err(c->err, c->tool, "missing destination file operand after '%s'", g->ops.v[0]);
    fd_printf(c->err, "Try '%s --help' for more information.\n", c->tool);
    return 1;
  }
  dest = tdir ? tdir : g->ops.v[g->ops.n - 1];
  nsrc = tdir ? g->ops.n : g->ops.n - 1;
  dn = path_to_native(dest);
  is_dir = os_stat(dn, &st) == 0 && st.is_dir;
  free(dn);
  if (no_target_dir) {
    if (nsrc > 1) {
      tool_err(c->err, c->tool, "extra operand '%s'", g->ops.v[2]);
      return 1;
    }
    is_dir = 0;
  }
  if (tdir && !is_dir) {
    tool_err(c->err, c->tool, "target directory '%s': %s", tdir,
             os_stat(tdir, &st) == 0 ? "Not a directory" : "No such file or directory");
    return 1;
  }
  if (!is_dir && nsrc > 1) {
    tool_err(c->err, c->tool, "target '%s' is not a directory", dest);
    return 1;
  }
  for (i = 0; i < nsrc && !tool_stop(); i++) {
    const char *src = g->ops.v[i];
    char *to;
    if (is_dir) {
      if (c->parents) {
        const char *s = src;
        while (*s == '/') s++;
        to = tool_join(dest, s);
      }
      else {
        char *b = xstrdup(tool_base(src));
        size_t n = strlen(b);
        while (n > 1 && (b[n - 1] == '/' || b[n - 1] == '\\')) b[--n] = '\0';
        to = tool_join(dest, b);
        free(b);
      }
    }
    else to = xstrdup(dest);
    if (c->parents && is_dir) {	/* make the folders of the path first */
      char *tn = path_to_native(to), *parent = path_dirname(tn);
      mkdir_p(parent);
      free(parent);
      free(tn);
    }
    if (one(c, src, to) != 0) status = 1;
    free(to);
  }
  return status;
}


static int cp_one (CpOpt *c, const char *src, const char *to) {
  char *sn = path_to_native(src), *tn = path_to_native(to);
  int r = cp_any(c, src, sn, to, tn, 1);
  free(sn);
  free(tn);
  return r;
}


int t_cp (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {
    {"recursive", 'r', 0}, {"archive", 'a', 0}, {"force", 'f', 0}, {"interactive", 'i', 0},
    {"no-clobber", 'n', 0}, {"verbose", 'v', 0}, {"preserve", 1001, 2}, {"update", 'u', 2},
    {"link", 'l', 0}, {"symbolic-link", 's', 0}, {"dereference", 'L', 0},
    {"no-dereference", 'P', 0}, {"target-directory", 't', 1}, {"no-target-directory", 'T', 0},
    {"parents", 1002, 0}, {"remove-destination", 1003, 0}, {"attributes-only", 1004, 0},
    {"no-preserve", 1005, 1}, {"one-file-system", 'x', 0}, {"sparse", 1006, 1},
    {"reflink", 1006, 2}, {"backup", 1006, 2}, {NULL, 0, 0}};
  CpOpt c;
  Opts g;
  Out o;
  int ch, no_target_dir = 0, deref_set = -1, status;
  const char *tdir = NULL;
  memset(&c, 0, sizeof(c));
  c.tool = "cp";
  c.in = in;
  c.err = err;
  opts_init(&g, "cp", argc, argv, err);
  while ((ch = opts_next(&g, "rRafinvpulsLPHdt:Tx", lo)) != 0) {
    switch (ch) {
      case 'r': case 'R': c.recursive = 1; break;
      case 'a': c.recursive = 1; c.keep_mode = c.keep_time = 1; deref_set = 0; break;
      case 'f': c.force = 1; c.interactive = 0; c.noclobber = 0; break;
      case 'i': c.interactive = 1; c.noclobber = 0; break;
      case 'n': c.noclobber = 1; c.interactive = 0; break;
      case 'v': c.verbose = 1; break;
      case 'p': c.keep_mode = c.keep_time = 1; break;
      case 1001:
        if (g.arg == NULL || strstr(g.arg, "mode") || strstr(g.arg, "all")) c.keep_mode = 1;
        if (g.arg == NULL || strstr(g.arg, "timestamps") || strstr(g.arg, "all")) c.keep_time = 1;
        break;
      case 1005:
        if (strstr(g.arg, "mode") || strstr(g.arg, "all")) c.keep_mode = 0;
        if (strstr(g.arg, "timestamps") || strstr(g.arg, "all")) c.keep_time = 0;
        break;
      case 'u': c.update = 1; break;
      case 'l': c.hard = 1; break;
      case 's': c.soft = 1; break;
      case 'L': deref_set = 1; break;
      case 'P': case 'd': deref_set = 0; break;
      case 'H': deref_set = 2; break;
      case 't': tdir = g.arg; break;
      case 'T': no_target_dir = 1; break;
      case 1002: c.parents = 1; break;
      case 1003: c.remove_dest = 1; break;
      case 1004: c.attr_only = 1; break;
      case 'x': case 1006: break;
      case OPT_HELP: opts_free(&g); return tool_help(out, "cp");
      default: opts_free(&g); return 1;
    }
  }
  /* without -R, links named on the command line are followed; with -R none are */
  c.deref = deref_set >= 0 ? deref_set : c.recursive ? 0 : 1;
  out_init(&o, out);
  c.o = &o;
  status = cp_targets(&c, &g, tdir, no_target_dir, cp_one);
  out_flush(&o);
  opts_free(&g);
  return tool_stop() ? 130 : status;
}


static int mv_one (CpOpt *c, const char *src, const char *to) {
  char *sn = path_to_native(src), *tn = path_to_native(to);
  OsStat sst, dst;
  int have_dst, r = 0;
  if (os_lstat(sn, &sst) != 0) {
    out_flush(c->o);
    tool_err(c->err, "mv", "cannot stat '%s': %s", src, os_errmsg());
    r = -1;
    goto done;
  }
  have_dst = os_lstat(tn, &dst) == 0;
  if (have_dst) {
    int same = os_same_file(sn, tn);
    /* on Windows "mv a.txt A.txt" names the same file: a rename all the same */
    if (same && strcmp(path_basename(sn), path_basename(tn)) != 0 &&
        m_fncmp(path_basename(sn), path_basename(tn)) == 0) same = 0, have_dst = 0;
    if (same) {
      out_flush(c->o);
      tool_err(c->err, "mv", "'%s' and '%s' are the same file", src, to);
      r = -1;
      goto done;
    }
  }
  if (have_dst) {
    if (sst.is_dir && !sst.is_link && !dst.is_dir) {
      out_flush(c->o);
      tool_err(c->err, "mv", "cannot overwrite non-directory '%s' with directory '%s'", to, src);
      r = -1;
      goto done;
    }
    if (!(sst.is_dir && !sst.is_link) && dst.is_dir && !dst.is_link) {
      out_flush(c->o);
      tool_err(c->err, "mv", "cannot overwrite directory '%s' with non-directory", to);
      r = -1;
      goto done;
    }
    if (c->noclobber) goto done;
    if (c->update && dst.mtime >= sst.mtime) goto done;
    if (c->interactive && !tool_ask(c->in, c->err, "mv: overwrite '%s'? ", to)) goto done;
  }
  if (sst.is_dir && !sst.is_link && inside(sn, tn)) {
    out_flush(c->o);
    tool_err(c->err, "mv", "cannot move '%s' to a subdirectory of itself, '%s'", src, to);
    r = -1;
    goto done;
  }
  if (os_rename(sn, tn) != 0) {
    int e = os_errcode();
    if (e == OS_E_XDEV) {	/* another disk: copy, then remove */
      CpOpt cc = *c;
      RmOpt rr;
      cc.recursive = 1;
      cc.keep_mode = cc.keep_time = 1;
      cc.deref = 0;
      cc.verbose = 0;
      cc.mv = 1;
      cc.force = 1;
      if (cp_any(&cc, src, sn, to, tn, 1) != 0) {
        r = -1;
        goto done;
      }
      memset(&rr, 0, sizeof(rr));
      rr.force = rr.recursive = 1;
      rr.in = c->in;
      rr.err = c->err;
      rr.o = c->o;
      rm_path(&rr, src, sn);
      if (rr.status) r = -1;
    }
    else {
      int retried = 0;
#ifdef _WIN32
      if (have_dst && dst.is_dir && !dst.is_link && sst.is_dir) {	/* replace an empty folder */
        if (os_rmdir(tn) == 0) retried = os_rename(sn, tn) == 0 ? 1 : -1;
        else {
          e = os_errcode();
          if (e == OS_E_NOTEMPTY) {
            out_flush(c->o);
            tool_err(c->err, "mv", "cannot move '%s' to '%s': Directory not empty", src, to);
            r = -1;
            goto done;
          }
        }
      }
      else if (have_dst && e == OS_E_ACCES && !(dst.mode & 0200)) {	/* read-only target */
        os_chmod(tn, 0666);
        retried = os_rename(sn, tn) == 0 ? 1 : -1;
      }
#endif
      if (retried != 1) {
        out_flush(c->o);
        tool_err(c->err, "mv", "cannot move '%s' to '%s': %s", src, to, os_errmsg());
        r = -1;
        goto done;
      }
    }
  }
  if (c->verbose) out_printf(c->o, "renamed '%s' -> '%s'\n", src, to);
done:
  free(sn);
  free(tn);
  return r;
}


int t_mv (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {
    {"force", 'f', 0}, {"interactive", 'i', 0}, {"no-clobber", 'n', 0}, {"verbose", 'v', 0},
    {"update", 'u', 2}, {"target-directory", 't', 1}, {"no-target-directory", 'T', 0},
    {"backup", 1001, 2}, {"strip-trailing-slashes", 1001, 0}, {NULL, 0, 0}};
  CpOpt c;
  Opts g;
  Out o;
  int ch, no_target_dir = 0, status;
  const char *tdir = NULL;
  memset(&c, 0, sizeof(c));
  c.tool = "mv";
  c.in = in;
  c.err = err;
  c.mv = 1;
  opts_init(&g, "mv", argc, argv, err);
  while ((ch = opts_next(&g, "finvut:Tb", lo)) != 0) {
    switch (ch) {
      case 'f': c.force = 1; c.interactive = c.noclobber = 0; break;
      case 'i': c.interactive = 1; c.noclobber = 0; break;
      case 'n': c.noclobber = 1; c.interactive = 0; break;
      case 'v': c.verbose = 1; break;
      case 'u': c.update = 1; break;
      case 't': tdir = g.arg; break;
      case 'T': no_target_dir = 1; break;
      case 'b': case 1001: break;
      case OPT_HELP: opts_free(&g); return tool_help(out, "mv");
      default: opts_free(&g); return 1;
    }
  }
  out_init(&o, out);
  c.o = &o;
  status = cp_targets(&c, &g, tdir, no_target_dir, mv_one);
  out_flush(&o);
  opts_free(&g);
  return tool_stop() ? 130 : status;
}

/* }================================================================== */


/*
** {==================================================================
** touch
** ===================================================================
*/

/* touch -d: "2024-01-31", "2024-01-31 13:45[:07]", "@1700000000", "now",
** "yesterday", "3 days ago", "1 hour ago" */
static int parse_date (const char *s, time_t *out) {
  struct tm tm;
  int y, mo, d, h = 0, mi = 0, sec = 0, n;
  time_t now = time(NULL);
  char unit[32];
  while (*s == ' ') s++;
  if (*s == '@') {
    char *end;
    long long v = strtoll(s + 1, &end, 10);
    if (*end) return -1;
    *out = (time_t)v;
    return 0;
  }
  if (strcmp(s, "now") == 0 || strcmp(s, "today") == 0) { *out = now; return 0; }
  if (strcmp(s, "yesterday") == 0) { *out = now - 86400; return 0; }
  if (strcmp(s, "tomorrow") == 0) { *out = now + 86400; return 0; }
  if (strstr(s, " ago") && sscanf(s, "%d %31s ago", &n, unit) == 2) {
    long long mul = 0;
    size_t ul = strlen(unit);
    if (ul > 1 && unit[ul - 1] == 's') unit[--ul] = '\0';
    if (strcmp(unit, "sec") == 0 || strcmp(unit, "second") == 0) mul = 1;
    else if (strcmp(unit, "min") == 0 || strcmp(unit, "minute") == 0) mul = 60;
    else if (strcmp(unit, "hour") == 0) mul = 3600;
    else if (strcmp(unit, "day") == 0) mul = 86400;
    else if (strcmp(unit, "week") == 0) mul = 604800;
    else if (strcmp(unit, "month") == 0) mul = 2592000;
    else if (strcmp(unit, "year") == 0) mul = 31536000;
    else return -1;
    *out = now - (time_t)(n * mul);
    return 0;
  }
  n = sscanf(s, "%d-%d-%d%*[ T]%d:%d:%d", &y, &mo, &d, &h, &mi, &sec);
  if (n < 3) return -1;
  memset(&tm, 0, sizeof(tm));
  tm.tm_year = y - 1900;
  tm.tm_mon = mo - 1;
  tm.tm_mday = d;
  tm.tm_hour = h;
  tm.tm_min = mi;
  tm.tm_sec = sec;
  tm.tm_isdst = -1;
  *out = mktime(&tm);
  return *out == (time_t)-1 ? -1 : 0;
}


/* touch -t [[CC]YY]MMDDhhmm[.ss] */
static int parse_stamp (const char *s, time_t *out) {
  struct tm tm;
  char digits[16];
  const char *dot = strchr(s, '.');
  size_t n = dot ? (size_t)(dot - s) : strlen(s), i;
  int v[6], k = 0, year;
  time_t now = time(NULL);
  struct tm *lt = localtime(&now);
  if (n != 8 && n != 10 && n != 12) return -1;
  if (strspn(s, "0123456789") != n) return -1;
  memcpy(digits, s, n);
  digits[n] = '\0';
  for (i = 0; i < n; i += 2) v[k++] = (digits[i] - '0') * 10 + (digits[i + 1] - '0');
  memset(&tm, 0, sizeof(tm));
  k = 0;
  if (n == 12) {
    year = v[0] * 100 + v[1];
    k = 2;
  }
  else if (n == 10) {
    year = v[0] < 69 ? 2000 + v[0] : 1900 + v[0];
    k = 1;
  }
  else year = lt ? lt->tm_year + 1900 : 2000;
  tm.tm_year = year - 1900;
  tm.tm_mon = v[k] - 1;
  tm.tm_mday = v[k + 1];
  tm.tm_hour = v[k + 2];
  tm.tm_min = v[k + 3];
  if (dot) tm.tm_sec = atoi(dot + 1);
  tm.tm_isdst = -1;
  *out = mktime(&tm);
  return *out == (time_t)-1 ? -1 : 0;
}


int t_touch (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {{"no-create", 'c', 0}, {"date", 'd', 1}, {"reference", 'r', 1},
                               {"time", 1001, 1}, {"no-dereference", 'h', 0}, {NULL, 0, 0}};
  Opts g;
  int c, only_a = 0, only_m = 0, no_create = 0, status = 0, have_time = 0;
  time_t at = 0, mt = 0;
  size_t i;
  (void)in;
  opts_init(&g, "touch", argc, argv, err);
  while ((c = opts_next(&g, "acmd:r:t:fh", lo)) != 0) {
    switch (c) {
      case 'a': only_a = 1; break;
      case 'm': only_m = 1; break;
      case 'c': no_create = 1; break;
      case 'f': case 'h': break;
      case 1001:
        if (strcmp(g.arg, "access") == 0 || strcmp(g.arg, "atime") == 0) only_a = 1;
        else only_m = 1;
        break;
      case 'd':
        if (parse_date(g.arg, &mt) != 0) {
          tool_err(err, "touch", "invalid date format '%s'", g.arg);
          opts_free(&g);
          return 1;
        }
        at = mt;
        have_time = 1;
        break;
      case 't':
        if (parse_stamp(g.arg, &mt) != 0) {
          tool_err(err, "touch", "invalid date format '%s'", g.arg);
          opts_free(&g);
          return 1;
        }
        at = mt;
        have_time = 1;
        break;
      case 'r': {
        char *native = path_to_native(g.arg);
        OsStat st;
        if (os_stat(native, &st) != 0) {
          tool_err(err, "touch", "failed to get attributes of '%s': %s", g.arg, os_errmsg());
          free(native);
          opts_free(&g);
          return 1;
        }
        free(native);
        at = st.atime;
        mt = st.mtime;
        have_time = 1;
        break;
      }
      case OPT_HELP: opts_free(&g); return tool_help(out, "touch");
      default: opts_free(&g); return 1;
    }
  }
  if (g.ops.n == 0) {
    tool_err(err, "touch", "missing file operand");
    opts_free(&g);
    return 1;
  }
  if (!have_time) at = mt = time(NULL);
  for (i = 0; i < g.ops.n; i++) {
    char *native = path_to_native(g.ops.v[i]);
    OsStat st;
    int exists = os_stat(native, &st) == 0;
    if (!exists) {
      int fd;
      if (no_create) {
        free(native);
        continue;
      }
      fd = os_open(native, OS_APPEND);
      if (fd < 0) {
        tool_err(err, "touch", "cannot touch '%s': %s", g.ops.v[i], os_errmsg());
        status = 1;
        free(native);
        continue;
      }
      os_close(fd);
      if (!have_time) {	/* just made: its times are now already */
        free(native);
        continue;
      }
      os_stat(native, &st);
    }
    if (os_utime(native, only_m && !only_a ? st.atime : at, only_a && !only_m ? st.mtime : mt) != 0) {
      tool_err(err, "touch", "setting times of '%s': %s", g.ops.v[i], os_errmsg());
      status = 1;
    }
    free(native);
  }
  opts_free(&g);
  return status;
}

/* }================================================================== */


/*
** {==================================================================
** ln
** ===================================================================
*/

int t_ln (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {
    {"symbolic", 's', 0}, {"force", 'f', 0}, {"no-dereference", 'n', 0}, {"verbose", 'v', 0},
    {"target-directory", 't', 1}, {"no-target-directory", 'T', 0}, {"interactive", 'i', 0},
    {"relative", 'r', 0}, {"logical", 'L', 0}, {"physical", 'P', 0}, {NULL, 0, 0}};
  Opts g;
  Out o;
  int c, sym = 0, force = 0, nodir = 0, verbose = 0, notarget = 0, ask = 0, status = 0;
  const char *tdir = NULL, *dest;
  size_t i, nsrc;
  int dest_is_dir = 0;
  opts_init(&g, "ln", argc, argv, err);
  while ((c = opts_next(&g, "sfnvt:TirLP", lo)) != 0) {
    switch (c) {
      case 's': sym = 1; break;
      case 'f': force = 1; ask = 0; break;
      case 'i': ask = 1; force = 0; break;
      case 'n': nodir = 1; break;
      case 'v': verbose = 1; break;
      case 't': tdir = g.arg; break;
      case 'T': notarget = 1; break;
      case 'r': case 'L': case 'P': break;
      case OPT_HELP: opts_free(&g); return tool_help(out, "ln");
      default: opts_free(&g); return 1;
    }
  }
  if (g.ops.n == 0) {
    tool_err(err, "ln", "missing file operand");
    opts_free(&g);
    return 1;
  }
  if (tdir) {
    dest = tdir;
    nsrc = g.ops.n;
  }
  else if (g.ops.n == 1) {
    dest = ".";
    nsrc = 1;
  }
  else {
    dest = g.ops.v[g.ops.n - 1];
    nsrc = g.ops.n - 1;
  }
  if (!notarget) {
    char *dn = path_to_native(dest);
    OsStat st, lst;
    dest_is_dir = os_stat(dn, &st) == 0 && st.is_dir;
    /* -n: a link to a folder is the target itself, not a folder to go in */
    if (dest_is_dir && nodir && os_lstat(dn, &lst) == 0 && lst.is_link) dest_is_dir = 0;
    free(dn);
  }
  if (nsrc > 1 && !dest_is_dir) {
    tool_err(err, "ln", "target '%s' is not a directory", dest);
    opts_free(&g);
    return 1;
  }
  out_init(&o, out);
  for (i = 0; i < nsrc; i++) {
    const char *target = g.ops.v[i];
    char *link, *ln_native, *tn;
    OsStat lst, tst;
    int r, is_dir = 0;
    if (dest_is_dir) {
      char *b = xstrdup(tool_base(target));
      size_t n = strlen(b);
      while (n > 1 && b[n - 1] == '/') b[--n] = '\0';
      link = tool_join(dest, b);
      free(b);
    }
    else link = xstrdup(dest);
    ln_native = path_to_native(link);
    if (os_lstat(ln_native, &lst) == 0) {
      if (ask && !tool_ask(in, err, "ln: replace '%s'? ", link)) goto next;
      if (!force && !ask) {
        out_flush(&o);
        tool_err(err, "ln", "failed to create %s link '%s': File exists", sym ? "symbolic" : "hard", link);
        status = 1;
        goto next;
      }
      if (lst.is_dir && !lst.is_link) {
        out_flush(&o);
        tool_err(err, "ln", "%s: cannot overwrite directory", link);
        status = 1;
        goto next;
      }
      if (lst.is_link && lst.is_dir) os_rmdir(ln_native);
      else os_unlink(ln_native);
    }
    if (sym) {
      /* a relative target is relative to the link's folder */
      if (target[0] == '/' || (target[0] && target[1] == ':')) tn = path_to_native(target);
      else {
        char *dir = path_dirname(ln_native);
        char *t = path_to_native(target);
        tn = path_join(dir, t);
        free(dir);
        free(t);
      }
      is_dir = os_stat(tn, &tst) == 0 && tst.is_dir;
      free(tn);
      {
#ifdef _WIN32
        char *t = (target[0] == '/') ? path_to_native(target) : xstrdup(target);
        r = os_symlink(t, ln_native, is_dir);
        free(t);
#else
        r = os_symlink(target, ln_native, is_dir);
#endif
      }
    }
    else {
      char *sn = path_to_native(target);
      r = os_link(sn, ln_native);
      free(sn);
    }
    if (r != 0) {
      out_flush(&o);
      if (sym) tool_err(err, "ln", "failed to create symbolic link '%s': %s", link, os_errmsg());
      else tool_err(err, "ln", "failed to create hard link '%s' => '%s': %s", link, target, os_errmsg());
#ifdef _WIN32
      if (sym && os_errcode() == OS_E_PERM)
        fd_printf(err, "ln: (Windows: turn on Developer Mode, or run as administrator)\n");
#endif
      status = 1;
    }
    else if (verbose) out_printf(&o, sym ? "'%s' -> '%s'\n" : "'%s' => '%s'\n", link, target);
  next:
    free(link);
    free(ln_native);
  }
  out_flush(&o);
  opts_free(&g);
  return status;
}

/* }================================================================== */


/*
** {==================================================================
** readlink, realpath
** ===================================================================
*/

/* the absolute path without . and .., links resolved where they exist;
** must: 0 anything, 1 all but the last part must exist, 2 all of it */
static char *canonical (const char *arg, int must) {
  char *native = path_to_native(arg), *r = os_realpath(native), *shown;
  OsStat st;
  if (r != NULL && must == 2 && os_stat(r, &st) != 0) {
    free(r);
    r = NULL;
  }
  if (r == NULL && must < 2) {	/* the last part is missing: its folder */
    char *dir = path_dirname(native), *rd = os_realpath(dir);
    if (rd != NULL && (must == 0 || os_stat(rd, &st) == 0)) {
      r = path_join(rd, path_basename(native));
    }
    else if (must == 0) {
      char *cwd = os_getcwd();
      r = path_join(cwd, native);
      free(cwd);
    }
    free(dir);
    free(rd);
  }
  free(native);
  if (r == NULL) return NULL;
#ifdef _WIN32
  shown = path_to_display(r);
  free(r);
  return shown;
#else
  (void)shown;
  return r;
#endif
}


int t_readlink (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {{"canonicalize", 'f', 0}, {"canonicalize-existing", 'e', 0},
    {"canonicalize-missing", 'm', 0}, {"no-newline", 'n', 0}, {"quiet", 'q', 0},
    {"silent", 's', 0}, {"verbose", 'v', 0}, {"zero", 'z', 0}, {NULL, 0, 0}};
  Opts g;
  Out o;
  int c, mode = -1, nonl = 0, verbose = 0, zero = 0, status = 0;
  size_t i;
  (void)in;
  opts_init(&g, "readlink", argc, argv, err);
  while ((c = opts_next(&g, "femnqsvz", lo)) != 0) {
    switch (c) {
      case 'f': mode = 1; break;
      case 'e': mode = 2; break;
      case 'm': mode = 0; break;
      case 'n': nonl = 1; break;
      case 'q': case 's': verbose = 0; break;
      case 'v': verbose = 1; break;
      case 'z': zero = 1; break;
      case OPT_HELP: opts_free(&g); return tool_help(out, "readlink");
      default: opts_free(&g); return 1;
    }
  }
  if (g.ops.n == 0) {
    tool_err(err, "readlink", "missing operand");
    opts_free(&g);
    return 1;
  }
  out_init(&o, out);
  for (i = 0; i < g.ops.n; i++) {
    char *r;
    if (mode >= 0) r = canonical(g.ops.v[i], mode);
    else {
      char *native = path_to_native(g.ops.v[i]);
      r = os_readlink(native);
      free(native);
    }
    if (r == NULL) {
      if (verbose) tool_err(err, "readlink", "%s: %s", g.ops.v[i], os_errmsg());
      status = 1;
      continue;
    }
    out_puts(&o, r);
    if (zero) out_putc(&o, '\0');
    else if (!nonl || g.ops.n > 1) out_putc(&o, '\n');
    free(r);
  }
  out_flush(&o);
  opts_free(&g);
  return status;
}


int t_realpath (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {{"canonicalize-existing", 'e', 0},
    {"canonicalize-missing", 'm', 0}, {"quiet", 'q', 0}, {"zero", 'z', 0},
    {"no-symlinks", 's', 0}, {"logical", 'L', 0}, {"physical", 'P', 0}, {NULL, 0, 0}};
  Opts g;
  Out o;
  int c, mode = 1, quiet = 0, zero = 0, status = 0;
  size_t i;
  (void)in;
  opts_init(&g, "realpath", argc, argv, err);
  while ((c = opts_next(&g, "emqzsLP", lo)) != 0) {
    switch (c) {
      case 'e': mode = 2; break;
      case 'm': mode = 0; break;
      case 'q': quiet = 1; break;
      case 'z': zero = 1; break;
      case 's': case 'L': case 'P': break;
      case OPT_HELP: opts_free(&g); return tool_help(out, "realpath");
      default: opts_free(&g); return 1;
    }
  }
  if (g.ops.n == 0) {
    tool_err(err, "realpath", "missing operand");
    opts_free(&g);
    return 1;
  }
  out_init(&o, out);
  for (i = 0; i < g.ops.n; i++) {
    char *r = canonical(g.ops.v[i], mode);
    if (r == NULL) {
      out_flush(&o);
      if (!quiet) tool_err(err, "realpath", "%s: No such file or directory", g.ops.v[i]);
      status = 1;
      continue;
    }
    out_puts(&o, r);
    out_putc(&o, zero ? '\0' : '\n');
    free(r);
  }
  out_flush(&o);
  opts_free(&g);
  return status;
}

/* }================================================================== */
