/*
** cdiff.c - diff: compare files line by line, or folders
**
**   diff [-u|-U N|-c|-C N|-q|-s] [-r] [-N] [-i] [-w] [-b] [-B] [-a]
**        [--strip-trailing-cr] [--label x] [--color[=when]] a b
** The differences are found with Myers' O(ND) algorithm, in linear space
** (the "middle snake" split), on lines turned into numbers first.
*/

#include "mmc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


typedef struct DLine {
  const char *s;
  size_t len;
} DLine;

typedef struct DFile {
  const char *name;	/* as shown */
  char *data;
  size_t len;
  DLine *lines;
  size_t n;
  int no_nl;	/* the last line has no newline */
  int *id;	/* each line's class: equal lines, equal numbers */
  char *changed;
  OsStat st;
  int binary;
} DFile;

enum { FMT_NORMAL, FMT_UNIFIED, FMT_CONTEXT, FMT_BRIEF };

typedef struct Diff {
  int fmt, ctx, icase, iws, ispace, iblank, strip_cr, text, recursive, newfile;
  int report_same, color, status;
  const char *label[2];
  Buf cmdline;	/* "diff -ru" for the headers of -r */
  Out *o;
  int in, err;
} Diff;


/*
** {==================================================================
** Reading and numbering the lines
** ===================================================================
*/

static int dfile_read (Diff *d, DFile *f, const char *name, const char *native) {
  memset(f, 0, sizeof(*f));
  f->name = name;
  if (strcmp(name, "-") == 0) {
    Buf b;
    char chunk[65536];
    long n;
    buf_init(&b);
    while ((n = os_read(d->in, chunk, sizeof(chunk))) > 0) buf_putn(&b, chunk, (size_t)n);
    f->len = b.len;
    f->data = buf_take(&b);
    f->st.mtime = time(NULL);
  }
  else {
    int fd;
    Buf b;
    char chunk[65536];
    long n;
    if (native == NULL || os_stat(native, &f->st) != 0 || (fd = os_open(native, OS_READ)) < 0) {
      tool_err(d->err, "diff", "%s: %s", name, os_errmsg());
      return -1;
    }
    buf_init(&b);
    while ((n = os_read(fd, chunk, sizeof(chunk))) > 0) buf_putn(&b, chunk, (size_t)n);
    os_close(fd);
    f->len = b.len;
    f->data = buf_take(&b);
  }
  if (!d->text && memchr(f->data, '\0', f->len < 8192 ? f->len : 8192) != NULL) f->binary = 1;
  {	/* the lines */
    size_t pos = 0, cap = 0;
    while (pos < f->len) {
      char *e = (char *)memchr(f->data + pos, '\n', f->len - pos);
      size_t l = e ? (size_t)(e - (f->data + pos)) : f->len - pos;
      if (f->n == cap) {
        cap = cap ? cap * 2 : 256;
        f->lines = (DLine *)xrealloc(f->lines, cap * sizeof(DLine));
      }
      f->lines[f->n].s = f->data + pos;
      f->lines[f->n].len = l;
      f->n++;
      if (!e) f->no_nl = 1;
      pos += l + 1;
    }
  }
  return 0;
}


static void dfile_free (DFile *f) {
  free(f->data);
  free(f->lines);
  free(f->id);
  free(f->changed);
}


/* a line as compared: -i -w -b --strip-trailing-cr applied */
static void norm (Diff *d, const DLine *l, Buf *b) {
  size_t i, n = l->len;
  int in_space = 0;
  b->len = 0;
  if (d->strip_cr && n > 0 && l->s[n - 1] == '\r') n--;
  for (i = 0; i < n; i++) {
    int c = (unsigned char)l->s[i];
    if (d->iws && (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v')) continue;
    if (d->ispace && (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v')) {
      in_space = 1;
      continue;
    }
    if (in_space) {
      buf_putc(b, ' ');
      in_space = 0;
    }
    buf_putc(b, (char)(d->icase ? tolower(c) : c));
  }
  if (b->s == NULL) buf_putc(b, '\0'), b->len = 0;
}


typedef struct HEnt {
  char *key;
  size_t len;
  unsigned h;
  int id;
} HEnt;


static unsigned hash_str (const char *s, size_t n) {
  unsigned h = 2166136261u;
  size_t i;
  for (i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 16777619u;
  return h;
}


static void number_lines (Diff *d, DFile *a, DFile *b) {
  size_t cap = 1024, used = 0, i;
  HEnt *tab;
  int next = 0, k;
  Buf nb;
  DFile *fs[2];
  while (cap < (a->n + b->n) * 2) cap *= 2;
  tab = (HEnt *)xmalloc(cap * sizeof(HEnt));
  for (i = 0; i < cap; i++) tab[i].key = NULL;
  buf_init(&nb);
  fs[0] = a;
  fs[1] = b;
  for (k = 0; k < 2; k++) {
    DFile *f = fs[k];
    f->id = (int *)xmalloc((f->n + 1) * sizeof(int));
    f->changed = (char *)xmalloc(f->n + 1);
    memset(f->changed, 0, f->n + 1);
    for (i = 0; i < f->n; i++) {
      unsigned h;
      size_t at;
      norm(d, &f->lines[i], &nb);
      h = hash_str(nb.s, nb.len);
      at = h & (cap - 1);
      while (tab[at].key != NULL && !(tab[at].h == h && tab[at].len == nb.len &&
                                      memcmp(tab[at].key, nb.s, nb.len) == 0))
        at = (at + 1) & (cap - 1);
      if (tab[at].key == NULL) {
        tab[at].key = xstrndup(nb.s, nb.len);
        tab[at].len = nb.len;
        tab[at].h = h;
        tab[at].id = next++;
        used++;
      }
      f->id[i] = tab[at].id;
    }
  }
  for (i = 0; i < cap; i++) free(tab[i].key);
  free(tab);
  buf_free(&nb);
  (void)used;
}

/* }================================================================== */


/*
** {==================================================================
** Myers, linear space
** ===================================================================
*/

typedef struct Cmp {
  const int *a, *b;
  char *da, *db;	/* deleted from a, inserted from b */
  int *vf, *vb;
  long off;
} Cmp;


/* the middle snake of a[alo..ahi) b[blo..bhi): *x0,*y0 .. *x1,*y1 */
static void middle (Cmp *c, long alo, long ahi, long blo, long bhi,
                    long *x0, long *y0, long *x1, long *y1) {
  long n = ahi - alo, m = bhi - blo, delta = n - m, dmax = (n + m + 1) / 2, dd, k;
  int odd = (int)(delta & 1);
  int *vf = c->vf + c->off, *vb = c->vb + c->off;
  vf[1] = 0;
  vb[1] = 0;
  for (dd = 0; dd <= dmax; dd++) {
    for (k = -dd; k <= dd; k += 2) {	/* forward */
      long x, y, sx, sy;
      if (k == -dd || (k != dd && vf[k - 1] < vf[k + 1])) x = vf[k + 1];
      else x = vf[k - 1] + 1;
      y = x - k;
      sx = x;
      sy = y;
      while (x < n && y < m && c->a[alo + x] == c->b[blo + y]) {
        x++;
        y++;
      }
      vf[k] = (int)x;
      if (odd && k >= delta - (dd - 1) && k <= delta + (dd - 1) && vf[k] + vb[delta - k] >= n) {
        *x0 = alo + sx;
        *y0 = blo + sy;
        *x1 = alo + x;
        *y1 = blo + y;
        return;
      }
    }
    for (k = -dd; k <= dd; k += 2) {	/* backward, from the ends */
      long x, y, sx, sy;
      if (k == -dd || (k != dd && vb[k - 1] < vb[k + 1])) x = vb[k + 1];
      else x = vb[k - 1] + 1;
      y = x - k;
      sx = x;
      sy = y;
      while (x < n && y < m && c->a[ahi - 1 - x] == c->b[bhi - 1 - y]) {
        x++;
        y++;
      }
      vb[k] = (int)x;
      if (!odd && delta - k >= -dd && delta - k <= dd && vb[k] + vf[delta - k] >= n) {
        *x0 = ahi - x;
        *y0 = bhi - y;
        *x1 = ahi - sx;
        *y1 = bhi - sy;
        return;
      }
    }
  }
  *x0 = *x1 = alo;	/* not reached */
  *y0 = *y1 = blo;
}


static void compare (Cmp *c, long alo, long ahi, long blo, long bhi) {
  long x0, y0, x1, y1, i;
  while (alo < ahi && blo < bhi && c->a[alo] == c->b[blo]) {
    alo++;
    blo++;
  }
  while (alo < ahi && blo < bhi && c->a[ahi - 1] == c->b[bhi - 1]) {
    ahi--;
    bhi--;
  }
  if (alo == ahi) {
    for (i = blo; i < bhi; i++) c->db[i] = 1;
    return;
  }
  if (blo == bhi) {
    for (i = alo; i < ahi; i++) c->da[i] = 1;
    return;
  }
  middle(c, alo, ahi, blo, bhi, &x0, &y0, &x1, &y1);
  if ((x0 == alo && y0 == blo && x1 == ahi && y1 == bhi) || (x1 - x0 == 0 && x0 == alo && y0 == blo &&
                                                             x1 == alo && y1 == blo)) {
    /* no progress: everything changed (cannot happen, but be safe) */
    for (i = alo; i < ahi; i++) c->da[i] = 1;
    for (i = blo; i < bhi; i++) c->db[i] = 1;
    return;
  }
  compare(c, alo, x0, blo, y0);
  compare(c, x1, ahi, y1, bhi);
}


/* GNU diff slides a run of changes down when the lines match, so it
** ends at the last equal line (like "shift_boundaries") */
static void shift_runs (char *chg, const int *id, long n, const char *other_chg, long other_n) {
  long i = 0;
  (void)other_chg;
  (void)other_n;
  while (i < n) {
    long start, end;
    if (!chg[i]) {
      i++;
      continue;
    }
    start = i;
    while (i < n && chg[i]) i++;
    end = i;
    /* slide down while the line after the run equals the first of the run */
    while (end < n && !chg[end] && id[start] == id[end]) {
      chg[start] = 0;
      chg[end] = 1;
      start++;
      end++;
      while (end < n && chg[end]) end++;	/* joined the next run */
    }
    i = end;
  }
}

/* }================================================================== */


/*
** {==================================================================
** Output
** ===================================================================
*/

typedef struct Hunk {	/* a[a0..a1) replaced by b[b0..b1) */
  long a0, a1, b0, b1;
} Hunk;


static void collect (DFile *a, DFile *b, Hunk **out, size_t *nh) {
  long i = 0, j = 0;
  size_t cap = 0;
  *out = NULL;
  *nh = 0;
  while (i < (long)a->n || j < (long)b->n) {
    Hunk h;
    if (i < (long)a->n && j < (long)b->n && !a->changed[i] && !b->changed[j]) {
      i++;
      j++;
      continue;
    }
    h.a0 = i;
    h.b0 = j;
    while (i < (long)a->n && a->changed[i]) i++;
    while (j < (long)b->n && b->changed[j]) j++;
    h.a1 = i;
    h.b1 = j;
    if (h.a0 == h.a1 && h.b0 == h.b1) {	/* should not be: step on */
      if (i < (long)a->n) i++;
      if (j < (long)b->n) j++;
      continue;
    }
    if (*nh == cap) {
      cap = cap ? cap * 2 : 16;
      *out = (Hunk *)xrealloc(*out, cap * sizeof(Hunk));
    }
    (*out)[(*nh)++] = h;
  }
}


static int blank_only (DFile *f, long lo, long hi) {
  long i;
  for (i = lo; i < hi; i++) {
    size_t k;
    for (k = 0; k < f->lines[i].len; k++)
      if (!isspace((unsigned char)f->lines[i].s[k])) return 0;
  }
  return 1;
}


static void put_line (Diff *d, const char *mark, DFile *f, long i, const char *color) {
  if (color && d->color) out_puts(d->o, color);
  out_puts(d->o, mark);
  out_putn(d->o, f->lines[i].s, f->lines[i].len);
  if (color && d->color) out_puts(d->o, "\033[m");
  out_putc(d->o, '\n');
  if (i == (long)f->n - 1 && f->no_nl) out_puts(d->o, "\\ No newline at end of file\n");
}


static void range_normal (Out *o, long lo, long hi) {	/* 1-based lines lo+1..hi */
  if (hi - lo <= 1) out_printf(o, "%ld", hi - lo == 1 ? lo + 1 : lo);
  else out_printf(o, "%ld,%ld", lo + 1, hi);
}


static void header_time (Out *o, const DFile *f, const char *label) {
  char when[64];
  struct tm *tm = localtime(&f->st.mtime);
  if (label) {
    out_puts(o, label);
    return;
  }
  if (tm) strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S.000000000 %z", tm);
  else when[0] = '\0';
  out_printf(o, "%s\t%s", f->name, when);
}


static void out_normal (Diff *d, DFile *a, DFile *b, Hunk *h, size_t nh) {
  size_t k;
  for (k = 0; k < nh; k++) {
    Hunk *x = &h[k];
    long i;
    char op = (x->a0 < x->a1 && x->b0 < x->b1) ? 'c' : x->a0 < x->a1 ? 'd' : 'a';
    if (op == 'a') out_printf(d->o, "%ld", x->a0);
    else range_normal(d->o, x->a0, x->a1);
    out_putc(d->o, op);
    if (op == 'd') out_printf(d->o, "%ld", x->b0);
    else range_normal(d->o, x->b0, x->b1);
    out_putc(d->o, '\n');
    for (i = x->a0; i < x->a1; i++) put_line(d, "< ", a, i, "\033[31m");
    if (op == 'c') out_puts(d->o, "---\n");
    for (i = x->b0; i < x->b1; i++) put_line(d, "> ", b, i, "\033[32m");
  }
}


static void range_unified (Out *o, long lo, long hi) {
  long n = hi - lo;
  if (n == 1) out_printf(o, "%ld", lo + 1);
  else if (n == 0) out_printf(o, "%ld,0", lo);
  else out_printf(o, "%ld,%ld", lo + 1, n);
}


static void out_unified (Diff *d, DFile *a, DFile *b, Hunk *h, size_t nh) {
  size_t k = 0;
  if (d->color) out_puts(d->o, "\033[1m");
  out_puts(d->o, "--- ");
  header_time(d->o, a, d->label[0]);
  out_puts(d->o, "\n+++ ");
  header_time(d->o, b, d->label[1]);
  out_putc(d->o, '\n');
  if (d->color) out_puts(d->o, "\033[m");
  while (k < nh) {
    size_t e = k;
    long a0, a1, b0, b1, i, j;
    /* hunks closer than 2 * context lines join */
    while (e + 1 < nh && h[e + 1].a0 - h[e].a1 <= 2L * d->ctx) e++;
    a0 = h[k].a0 - d->ctx;
    b0 = h[k].b0 - d->ctx;
    if (a0 < 0) a0 = 0;
    if (b0 < 0) b0 = 0;
    if (h[k].a0 - a0 != h[k].b0 - b0) {	/* the same number of context lines */
      long c = h[k].a0 - a0 < h[k].b0 - b0 ? h[k].a0 - a0 : h[k].b0 - b0;
      a0 = h[k].a0 - c;
      b0 = h[k].b0 - c;
    }
    a1 = h[e].a1 + d->ctx;
    b1 = h[e].b1 + d->ctx;
    if (a1 > (long)a->n) a1 = (long)a->n;
    if (b1 > (long)b->n) b1 = (long)b->n;
    if (a1 - h[e].a1 != b1 - h[e].b1) {
      long c = a1 - h[e].a1 < b1 - h[e].b1 ? a1 - h[e].a1 : b1 - h[e].b1;
      a1 = h[e].a1 + c;
      b1 = h[e].b1 + c;
    }
    if (d->color) out_puts(d->o, "\033[36m");
    out_puts(d->o, "@@ -");
    range_unified(d->o, a0, a1);
    out_puts(d->o, " +");
    range_unified(d->o, b0, b1);
    out_puts(d->o, " @@");
    if (d->color) out_puts(d->o, "\033[m");
    out_putc(d->o, '\n');
    i = a0;
    j = b0;
    for (; k <= e; k++) {
      while (i < h[k].a0) {
        put_line(d, " ", a, i, NULL);
        i++;
        j++;
      }
      for (; i < h[k].a1; i++) put_line(d, "-", a, i, "\033[31m");
      for (; j < h[k].b1; j++) put_line(d, "+", b, j, "\033[32m");
    }
    while (i < a1) {
      put_line(d, " ", a, i, NULL);
      i++;
      j++;
    }
  }
}


static void range_context (Out *o, long lo, long hi) {
  if (hi - lo == 1) out_printf(o, "%ld", lo + 1);
  else if (hi == lo) out_printf(o, "%ld", lo);
  else out_printf(o, "%ld,%ld", lo + 1, hi);
}


static void out_context (Diff *d, DFile *a, DFile *b, Hunk *h, size_t nh) {
  size_t k = 0;
  out_puts(d->o, "*** ");
  header_time(d->o, a, d->label[0]);
  out_puts(d->o, "\n--- ");
  header_time(d->o, b, d->label[1]);
  out_putc(d->o, '\n');
  while (k < nh) {
    size_t e = k, q;
    long a0, a1, b0, b1, i;
    int adel = 0, bins = 0;
    while (e + 1 < nh && h[e + 1].a0 - h[e].a1 <= 2L * d->ctx) e++;
    a0 = h[k].a0 - d->ctx;
    b0 = h[k].b0 - d->ctx;
    if (a0 < 0) a0 = 0;
    if (b0 < 0) b0 = 0;
    a1 = h[e].a1 + d->ctx;
    b1 = h[e].b1 + d->ctx;
    if (a1 > (long)a->n) a1 = (long)a->n;
    if (b1 > (long)b->n) b1 = (long)b->n;
    for (q = k; q <= e; q++) {
      if (h[q].a1 > h[q].a0) adel = 1;
      if (h[q].b1 > h[q].b0) bins = 1;
    }
    out_puts(d->o, "***************\n*** ");
    range_context(d->o, a0, a1);
    out_puts(d->o, " ****\n");
    if (adel) {
      i = a0;
      for (q = k; q <= e; q++) {
        const char *mark = (h[q].b1 > h[q].b0) ? "! " : "- ";
        for (; i < h[q].a0; i++) put_line(d, "  ", a, i, NULL);
        for (; i < h[q].a1; i++) put_line(d, mark, a, i, "\033[31m");
      }
      for (; i < a1; i++) put_line(d, "  ", a, i, NULL);
    }
    out_puts(d->o, "--- ");
    range_context(d->o, b0, b1);
    out_puts(d->o, " ----\n");
    if (bins) {
      i = b0;
      for (q = k; q <= e; q++) {
        const char *mark = (h[q].a1 > h[q].a0) ? "! " : "+ ";
        for (; i < h[q].b0; i++) put_line(d, "  ", b, i, NULL);
        for (; i < h[q].b1; i++) put_line(d, mark, b, i, "\033[32m");
      }
      for (; i < b1; i++) put_line(d, "  ", b, i, NULL);
    }
    k = e + 1;
  }
}

/* }================================================================== */


/*
** {==================================================================
** Files and folders
** ===================================================================
*/

/* 0 same, 1 different, 2 trouble */
static int diff_files (Diff *d, const char *an, const char *anat, const char *bn, const char *bnat,
                       int a_missing, int b_missing) {
  DFile a, b;
  Hunk *h;
  size_t nh;
  int r;
  if (a_missing) {
    memset(&a, 0, sizeof(a));
    a.name = an;
  }
  else if (dfile_read(d, &a, an, anat) != 0) return 2;
  if (b_missing) {
    memset(&b, 0, sizeof(b));
    b.name = bn;
  }
  else if (dfile_read(d, &b, bn, bnat) != 0) {
    dfile_free(&a);
    return 2;
  }
  if (a.len == b.len && (a.len == 0 || memcmp(a.data, b.data, a.len) == 0) && !a_missing == !b_missing) {
    if (d->report_same) out_printf(d->o, "Files %s and %s are identical\n", an, bn);
    dfile_free(&a);
    dfile_free(&b);
    return 0;
  }
  if ((a.binary || b.binary) && !d->text) {
    out_printf(d->o, "Binary files %s and %s differ\n", an, bn);
    dfile_free(&a);
    dfile_free(&b);
    return 1;
  }
  number_lines(d, &a, &b);
  {
    Cmp c;
    long sz = (long)(a.n + b.n) + 3;
    c.a = a.id;
    c.b = b.id;
    c.da = a.changed;
    c.db = b.changed;
    c.vf = (int *)xmalloc((size_t)(2 * sz + 1) * sizeof(int));
    c.vb = (int *)xmalloc((size_t)(2 * sz + 1) * sizeof(int));
    c.off = sz;
    compare(&c, 0, (long)a.n, 0, (long)b.n);
    free(c.vf);
    free(c.vb);
  }
  shift_runs(a.changed, a.id, (long)a.n, b.changed, (long)b.n);
  shift_runs(b.changed, b.id, (long)b.n, a.changed, (long)a.n);
  /* the last lines differ only in their newline */
  if (a.n > 0 && b.n > 0 && a.no_nl != b.no_nl && !a.changed[a.n - 1] && !b.changed[b.n - 1]) {
    a.changed[a.n - 1] = 1;
    b.changed[b.n - 1] = 1;
  }
  collect(&a, &b, &h, &nh);
  if (d->iblank) {	/* -B: changes of blank lines only do not count */
    size_t i, j = 0;
    for (i = 0; i < nh; i++)
      if (!(blank_only(&a, h[i].a0, h[i].a1) && blank_only(&b, h[i].b0, h[i].b1))) h[j++] = h[i];
    nh = j;
  }
  r = nh > 0 ? 1 : 0;
  if (nh > 0) {
    if (d->fmt == FMT_BRIEF) out_printf(d->o, "Files %s and %s differ\n", an, bn);
    else {
      if (d->cmdline.len > 0) out_printf(d->o, "%s %s %s\n", d->cmdline.s, an, bn);
      if (d->fmt == FMT_UNIFIED) out_unified(d, &a, &b, h, nh);
      else if (d->fmt == FMT_CONTEXT) out_context(d, &a, &b, h, nh);
      else out_normal(d, &a, &b, h, nh);
    }
  }
  else if (d->report_same) out_printf(d->o, "Files %s and %s are identical\n", an, bn);
  free(h);
  dfile_free(&a);
  dfile_free(&b);
  return r;
}


static int diff_paths (Diff *d, const char *an, const char *bn, int top);


static int diff_dirs (Diff *d, const char *an, const char *anat, const char *bn, const char *bnat) {
  Vec la, lb, all;
  size_t i, j;
  int r = 0;
  vec_init(&la);
  vec_init(&lb);
  vec_init(&all);
  os_listdir(anat, &la);
  os_listdir(bnat, &lb);
  for (i = 0; i < la.n; i++) vec_push(&all, xstrdup(la.v[i]));
  for (i = 0; i < lb.n; i++) {
    for (j = 0; j < la.n; j++)
      if (strcmp(la.v[j], lb.v[i]) == 0) break;
    if (j == la.n) vec_push(&all, xstrdup(lb.v[i]));
  }
  vec_sort(&all);
  for (i = 0; i < all.n && !tool_stop(); i++) {
    const char *name = all.v[i];
    int in_a = 0, in_b = 0, rr;
    char *ca, *cb;
    for (j = 0; j < la.n; j++) if (strcmp(la.v[j], name) == 0) in_a = 1;
    for (j = 0; j < lb.n; j++) if (strcmp(lb.v[j], name) == 0) in_b = 1;
    ca = tool_join(an, name);
    cb = tool_join(bn, name);
    if (!in_a || !in_b) {
      if (d->newfile) {
        char *na = path_to_native(ca), *nb = path_to_native(cb);
        OsStat st;
        if (os_stat(in_a ? na : nb, &st) == 0 && st.is_dir && d->recursive) {
          /* a whole folder on one side: every file in it, against nothing */
          rr = diff_paths(d, ca, cb, 0);
        }
        else rr = diff_files(d, ca, na, cb, nb, !in_a, !in_b);
        free(na);
        free(nb);
      }
      else {
        out_printf(d->o, "Only in %s: %s\n", in_a ? an : bn, name);
        rr = 1;
      }
    }
    else rr = diff_paths(d, ca, cb, 0);
    if (rr > r) r = rr;
    free(ca);
    free(cb);
  }
  vec_free(&la);
  vec_free(&lb);
  vec_free(&all);
  return r;
}


static int diff_paths (Diff *d, const char *an, const char *bn, int top) {
  char *anat = strcmp(an, "-") == 0 ? NULL : path_to_native(an);
  char *bnat = strcmp(bn, "-") == 0 ? NULL : path_to_native(bn);
  OsStat sa, sb;
  int ha = anat == NULL || os_stat(anat, &sa) == 0, hb = bnat == NULL || os_stat(bnat, &sb) == 0;
  int r;
  if (anat == NULL) memset(&sa, 0, sizeof(sa));
  if (bnat == NULL) memset(&sb, 0, sizeof(sb));
  if (!ha || !hb) {
    if (d->newfile && (ha || hb) && !(ha ? sa.is_dir : sb.is_dir)) {
      r = diff_files(d, an, anat, bn, bnat, !ha, !hb);
    }
    else if (d->newfile && (ha || hb)) {	/* a folder against nothing */
      r = diff_dirs(d, an, ha ? anat : bnat, bn, ha ? anat : bnat);
    }
    else {
      tool_err(d->err, "diff", "%s: No such file or directory", !ha ? an : bn);
      r = 2;
    }
  }
  else if (sa.is_dir && sb.is_dir) {
    if (!top && !d->recursive) {
      out_printf(d->o, "Common subdirectories: %s and %s\n", an, bn);
      r = 0;
    }
    else r = diff_dirs(d, an, anat, bn, bnat);
  }
  else if (sa.is_dir != sb.is_dir && !top) {
    out_printf(d->o, "File %s is a %s while file %s is a %s\n", an, sa.is_dir ? "directory" : "regular file",
               bn, sb.is_dir ? "directory" : "regular file");
    r = 1;
  }
  else if (sa.is_dir || sb.is_dir) {	/* diff dir file: dir/file */
    const char *file = sa.is_dir ? bn : an;
    char *inside = tool_join(sa.is_dir ? an : bn, tool_base(file));
    r = sa.is_dir ? diff_paths(d, inside, bn, 0) : diff_paths(d, an, inside, 0);
    free(inside);
  }
  else r = diff_files(d, an, anat, bn, bnat, 0, 0);
  free(anat);
  free(bnat);
  return r;
}


int t_diff (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {
    {"unified", 'U', 2}, {"context", 'C', 2}, {"brief", 'q', 0}, {"report-identical-files", 's', 0},
    {"recursive", 'r', 0}, {"new-file", 'N', 0}, {"ignore-case", 'i', 0},
    {"ignore-all-space", 'w', 0}, {"ignore-space-change", 'b', 0}, {"ignore-blank-lines", 'B', 0},
    {"text", 'a', 0}, {"strip-trailing-cr", 1001, 0}, {"label", 1002, 1}, {"color", 1003, 2},
    {"normal", 1004, 0}, {"minimal", 'd', 0}, {"expand-tabs", 't', 0}, {NULL, 0, 0}};
  Diff d;
  Opts g;
  Out o;
  int c, r, nlabels = 0, i;
  memset(&d, 0, sizeof(d));
  d.ctx = 3;
  d.in = in;
  d.err = err;
  buf_init(&d.cmdline);
  opts_init(&g, "diff", argc, argv, err);
  while ((c = opts_next(&g, "uU:cC:qsrNiwbBadtpE", lo)) != 0) {
    switch (c) {
      case 'u': d.fmt = FMT_UNIFIED; break;
      case 'U': d.fmt = FMT_UNIFIED; if (g.arg) d.ctx = atoi(g.arg); break;
      case 'c': d.fmt = FMT_CONTEXT; break;
      case 'C': d.fmt = FMT_CONTEXT; if (g.arg) d.ctx = atoi(g.arg); break;
      case 'q': d.fmt = FMT_BRIEF; break;
      case 's': d.report_same = 1; break;
      case 'r': d.recursive = 1; break;
      case 'N': d.newfile = 1; break;
      case 'i': d.icase = 1; break;
      case 'w': d.iws = 1; break;
      case 'b': d.ispace = 1; break;
      case 'B': d.iblank = 1; break;
      case 'a': d.text = 1; break;
      case 1001: d.strip_cr = 1; break;
      case 1002: if (nlabels < 2) d.label[nlabels++] = g.arg; break;
      case 1003:
        d.color = g.arg == NULL || strcmp(g.arg, "always") == 0 ? 1 :
                  strcmp(g.arg, "auto") == 0 ? os_is_tty(out) : 0;
        break;
      case 1004: d.fmt = FMT_NORMAL; break;
      case 'd': case 't': case 'p': case 'E': break;
      case OPT_HELP: opts_free(&g); buf_free(&d.cmdline); return tool_help(out, "diff");
      default: opts_free(&g); buf_free(&d.cmdline); return 2;
    }
  }
  if (g.ops.n != 2) {
    if (g.ops.n < 2) tool_err(err, "diff", g.ops.n == 0 ? "missing operand" : "missing operand after '%s'",
                              g.ops.n ? g.ops.v[0] : "");
    else tool_err(err, "diff", "extra operand '%s'", g.ops.v[2]);
    fd_printf(err, "diff: Try 'diff --help' for more information.\n");
    opts_free(&g);
    buf_free(&d.cmdline);
    return 2;
  }
  if (d.recursive || 1) {	/* the "diff -ru" line that -r puts before each file */
    buf_puts(&d.cmdline, "diff");
    for (i = 1; i < argc; i++) {
      if (argv[i][0] != '-' || strcmp(argv[i], "-") == 0) continue;
      buf_putc(&d.cmdline, ' ');
      buf_puts(&d.cmdline, argv[i]);
    }
  }
  out_init(&o, out);
  d.o = &o;
  {
    char *na = path_to_native(g.ops.v[0]), *nb = path_to_native(g.ops.v[1]);
    OsStat sa, sb;
    int dirs = os_stat(na, &sa) == 0 && os_stat(nb, &sb) == 0 && sa.is_dir && sb.is_dir;
    free(na);
    free(nb);
    if (!dirs) {	/* two files: no header line */
      d.cmdline.len = 0;
    }
  }
  r = diff_paths(&d, g.ops.v[0], g.ops.v[1], 1);
  out_flush(&o);
  opts_free(&g);
  buf_free(&d.cmdline);
  return r;
}

/* }================================================================== */
