/*
** czip.c - zip and unzip (the Info-ZIP commands, their common options)
**
**   zip [-rqj0-9] [-x pattern...] archive[.zip] path...
**   unzip [-lotnqjp] [-d dir] archive[.zip] [member...] [-x pattern...]
** Methods: stored and deflated. Names are UTF-8 (flag bit 11). Unix
** permissions travel in the external attributes, as Info-ZIP does.
*/

#include "mmc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static unsigned get16 (const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static unsigned long get32 (const unsigned char *p) {
  return (unsigned long)p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}
static void put16 (unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32 (unsigned char *p, unsigned long v) {
  p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}


typedef struct ZEnt {
  char *name;
  unsigned method, flags, mtime, mdate, made_by, internal;
  unsigned long crc, csize, usize, ext, offset;
  int keep;	/* zip: an old entry that stays */
} ZEnt;


static void dos_time (time_t t, unsigned *dtime, unsigned *ddate) {
  struct tm *tm = localtime(&t);
  if (tm == NULL || tm->tm_year < 80) {
    *dtime = 0;
    *ddate = (1 << 5) | 1;
    return;
  }
  *dtime = (unsigned)((tm->tm_hour << 11) | (tm->tm_min << 5) | (tm->tm_sec / 2));
  *ddate = (unsigned)(((tm->tm_year - 80) << 9) | ((tm->tm_mon + 1) << 5) | tm->tm_mday);
}


static time_t unix_time (unsigned dtime, unsigned ddate) {
  struct tm tm;
  memset(&tm, 0, sizeof(tm));
  tm.tm_year = (int)((ddate >> 9) & 0x7F) + 80;
  tm.tm_mon = (int)((ddate >> 5) & 0x0F) - 1;
  tm.tm_mday = (int)(ddate & 0x1F);
  tm.tm_hour = (int)((dtime >> 11) & 0x1F);
  tm.tm_min = (int)((dtime >> 5) & 0x3F);
  tm.tm_sec = (int)((dtime & 0x1F) * 2);
  tm.tm_isdst = -1;
  return mktime(&tm);
}


/* the entries of an archive, from its central directory; -1: not a zip */
static int read_central (int fd, ZEnt **out, size_t *n, int err, const char *name) {
  long long size = os_seek(fd, 0, 2), start, cd_off, cd_size;
  unsigned char *tail;
  long got, k, found = -1;
  unsigned count, i;
  unsigned char *cd;
  size_t pos = 0;
  *out = NULL;
  *n = 0;
  if (size < 22) goto notzip;
  start = size > 65557 ? size - 65557 : 0;
  tail = (unsigned char *)xmalloc((size_t)(size - start));
  if (os_seek(fd, start, 0) < 0) {
    free(tail);
    goto notzip;
  }
  for (got = 0; got < size - start;) {
    long r = os_read(fd, tail + got, (size_t)(size - start - got));
    if (r <= 0) break;
    got += r;
  }
  for (k = got - 22; k >= 0; k--)
    if (tail[k] == 'P' && tail[k + 1] == 'K' && tail[k + 2] == 5 && tail[k + 3] == 6) {
      found = k;
      break;
    }
  if (found < 0) {
    free(tail);
    goto notzip;
  }
  count = get16(tail + found + 10);
  cd_size = (long long)get32(tail + found + 12);
  cd_off = (long long)get32(tail + found + 16);
  free(tail);
  if (cd_off + cd_size > size) goto notzip;
  cd = (unsigned char *)xmalloc((size_t)cd_size + 1);
  os_seek(fd, cd_off, 0);
  for (got = 0; got < cd_size;) {
    long r = os_read(fd, cd + got, (size_t)(cd_size - got));
    if (r <= 0) break;
    got += r;
  }
  *out = (ZEnt *)xmalloc((count + 1) * sizeof(ZEnt));
  for (i = 0; i < count; i++) {
    ZEnt *e = &(*out)[*n];
    unsigned nl, xl, cl;
    if (pos + 46 > (size_t)cd_size || get32(cd + pos) != 0x02014b50UL) break;
    memset(e, 0, sizeof(*e));
    e->made_by = get16(cd + pos + 4);
    e->flags = get16(cd + pos + 8);
    e->method = get16(cd + pos + 10);
    e->mtime = get16(cd + pos + 12);
    e->mdate = get16(cd + pos + 14);
    e->crc = get32(cd + pos + 16);
    e->csize = get32(cd + pos + 20);
    e->usize = get32(cd + pos + 24);
    nl = get16(cd + pos + 28);
    xl = get16(cd + pos + 30);
    cl = get16(cd + pos + 32);
    e->internal = get16(cd + pos + 36);
    e->ext = get32(cd + pos + 38);
    e->offset = get32(cd + pos + 42);
    if (pos + 46 + nl > (size_t)cd_size) break;
    e->name = xstrndup((const char *)cd + pos + 46, nl);
    {	/* backslashes from Windows zip programs */
      char *p;
      for (p = e->name; *p; p++)
        if (*p == '\\') *p = '/';
    }
    (*n)++;
    pos += 46 + nl + xl + cl;
  }
  free(cd);
  return 0;
notzip:
  if (err >= 0) {
    fd_printf(err, "  End-of-central-directory signature not found.  Either this file is not\n"
                   "  a zipfile, or it constitutes one disk of a multi-part archive.  In the\n"
                   "  latter case the central directory and zipfile comment will be found on\n"
                   "  the last disk(s) of this archive.\n");
    tool_err(err, "unzip", "cannot find zipfile directory in one of %s or\n        %s.zip, and cannot find %s.ZIP, period.",
             name, name, name);
  }
  return -1;
}


static void zent_free (ZEnt *v, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) free(v[i].name);
  free(v);
}


/*
** {==================================================================
** zip
** ===================================================================
*/

typedef struct Zip {
  int fd;
  unsigned long long pos;	/* where the next byte goes */
  ZEnt *ents;
  size_t n, cap;
  int recursive, quiet, junk, level, status, move;
  Vec exclude;
  Out *o;
  int err;
  ZEnt *old;
  size_t nold;
  int oldfd;
} Zip;


static int zip_sink (void *zv, const unsigned char *p, size_t n) {
  Zip *z = (Zip *)zv;
  if (os_write(z->fd, p, n) < 0) return -1;
  z->pos += n;
  return 0;
}


static ZEnt *zip_new_ent (Zip *z) {
  if (z->n == z->cap) {
    z->cap = z->cap ? z->cap * 2 : 64;
    z->ents = (ZEnt *)xrealloc(z->ents, z->cap * sizeof(ZEnt));
  }
  memset(&z->ents[z->n], 0, sizeof(ZEnt));
  return &z->ents[z->n++];
}


static void local_header (unsigned char *h, const ZEnt *e) {
  put32(h, 0x04034b50UL);
  put16(h + 4, e->method == 8 ? 20 : 10);
  put16(h + 6, e->flags);
  put16(h + 8, e->method);
  put16(h + 10, e->mtime);
  put16(h + 12, e->mdate);
  put32(h + 14, e->crc);
  put32(h + 18, e->csize);
  put32(h + 22, e->usize);
  put16(h + 26, (unsigned)strlen(e->name));
  put16(h + 28, 0);
}


static int non_ascii (const char *s) {
  for (; *s; s++)
    if ((unsigned char)*s >= 0x80) return 1;
  return 0;
}


/* an old entry the new archive keeps: copied as it is */
static void zip_copy_old (Zip *z, ZEnt *old) {
  unsigned char lh[30];
  unsigned long long skip;
  ZEnt *e;
  unsigned char buf[65536];
  unsigned long left;
  if (os_seek(z->oldfd, (long long)old->offset, 0) < 0 || os_read(z->oldfd, lh, 30) != 30) return;
  skip = get16(lh + 26) + get16(lh + 28);
  os_seek(z->oldfd, (long long)old->offset + 30 + (long long)skip, 0);
  e = zip_new_ent(z);
  *e = *old;
  e->name = xstrdup(old->name);
  e->flags &= ~8u;	/* sizes are in the header now */
  e->offset = (unsigned long)z->pos;
  local_header(lh, e);
  zip_sink(z, lh, 30);
  zip_sink(z, (const unsigned char *)e->name, strlen(e->name));
  for (left = old->csize; left > 0;) {
    long r = os_read(z->oldfd, buf, left > sizeof(buf) ? sizeof(buf) : left);
    if (r <= 0) break;
    zip_sink(z, buf, (size_t)r);
    left -= (unsigned long)r;
  }
}


static int zip_excluded (Zip *z, const char *name) {
  size_t i;
  for (i = 0; i < z->exclude.n; i++)
    if (pat_match(z->exclude.v[i], name, 0)) return 1;
  return 0;
}


static void zip_add (Zip *z, const char *disp, const char *native) {
  OsStat st;
  const char *name = disp;
  char *ename;
  ZEnt *e;
  unsigned char lh[30];
  size_t k;
  int updating = 0;
  if (tool_stop()) return;
  while (name[0] == '.' && name[1] == '/') name += 2;
  while (*name == '/') name++;
  if (os_stat(native, &st) != 0) {
    out_flush(z->o);
    fd_printf(z->err, "\tzip warning: name not matched: %s\n", disp);
    z->status = 12;
    return;
  }
  if (z->junk && st.is_dir) {
    if (!z->recursive) return;
  }
  ename = st.is_dir ? xstrcat3(name, name[0] && name[strlen(name) - 1] == '/' ? "" : "/", "")
                    : xstrdup(z->junk ? tool_base(name) : name);
  if (zip_excluded(z, ename) || zip_excluded(z, disp) || strcmp(ename, "/") == 0 || ename[0] == '\0') {
    free(ename);
    goto recurse;
  }
  if (z->junk && st.is_dir) {
    free(ename);
    goto recurse;
  }
  for (k = 0; k < z->nold; k++)	/* replacing an old entry */
    if (z->old[k].keep && strcmp(z->old[k].name, ename) == 0) {
      z->old[k].keep = 0;
      updating = 1;
    }
  for (k = 0; k < z->n; k++)	/* the same name twice */
    if (strcmp(z->ents[k].name, ename) == 0) {
      free(ename);
      goto recurse;
    }
  e = zip_new_ent(z);
  e->name = ename;
  e->offset = (unsigned long)z->pos;
  e->flags = non_ascii(ename) ? 0x0800 : 0;
  e->made_by = (3 << 8) | 30;
  e->ext = ((unsigned long)(st.mode | (st.is_dir ? 040000 : 0100000)) << 16) | (st.is_dir ? 0x10 : 0);
  dos_time(st.mtime, &e->mtime, &e->mdate);
  if (st.is_dir) {
    e->method = 0;
    local_header(lh, e);
    zip_sink(z, lh, 30);
    zip_sink(z, (const unsigned char *)e->name, strlen(e->name));
    if (!z->quiet) out_printf(z->o, "  %s: %s (stored 0%%)\n", updating ? "updating" : "adding", e->name);
  }
  else {
    int fd = os_open(native, OS_READ);
    unsigned char buf[65536];
    Buf keep;
    long n;
    unsigned long long datapos;
    Deflate *d;
    if (fd < 0) {
      out_flush(z->o);
      fd_printf(z->err, "\tzip warning: could not open for reading: %s\n", disp);
      z->n--;
      free(ename);
      z->status = 18;
      return;
    }
    e->method = z->level == 0 ? 0 : 8;
    local_header(lh, e);
    zip_sink(z, lh, 30);
    zip_sink(z, (const unsigned char *)e->name, strlen(e->name));
    datapos = z->pos;
    buf_init(&keep);
    d = e->method == 8 ? deflate_new(z->level, zip_sink, z) : NULL;
    while ((n = os_read(fd, buf, sizeof(buf))) > 0) {
      e->crc = crc32_update(e->crc, buf, (size_t)n);
      e->usize += (unsigned long)n;
      if (e->usize <= (1UL << 20)) buf_putn(&keep, (const char *)buf, (size_t)n);
      if (d) deflate_write(d, buf, (size_t)n);
      else zip_sink(z, buf, (size_t)n);
    }
    os_close(fd);
    if (d) deflate_end(d);
    e->csize = (unsigned long)(z->pos - datapos);
    if (e->method == 8 && e->csize >= e->usize && e->usize <= (1UL << 20)) {	/* stored is smaller */
      os_seek(z->fd, (long long)datapos, 0);
      z->pos = datapos;
      e->method = 0;
      if (keep.len) zip_sink(z, (const unsigned char *)keep.s, keep.len);
      e->csize = e->usize;
    }
    buf_free(&keep);
    /* the sizes, now known, into the local header */
    {
      unsigned long long end = z->pos;
      local_header(lh, e);
      os_seek(z->fd, (long long)e->offset, 0);
      os_write(z->fd, lh, 30);
      os_seek(z->fd, (long long)end, 0);
    }
    if (!z->quiet) {
      if (e->method == 8)
        out_printf(z->o, "  %s: %s (deflated %d%%)\n", updating ? "updating" : "adding", e->name,
                   e->usize ? (int)(100 - (e->csize * 100 + e->usize / 2) / e->usize) : 0);
      else out_printf(z->o, "  %s: %s (stored 0%%)\n", updating ? "updating" : "adding", e->name);
    }
  }
recurse:
  if (st.is_dir && z->recursive) {
    Vec names;
    size_t i;
    vec_init(&names);
    os_listdir(native, &names);
    vec_sort(&names);
    for (i = 0; i < names.n; i++) {
      char *cd = tool_join(disp, names.v[i]), *cn = path_join(native, names.v[i]);
      zip_add(z, cd, cn);
      free(cd);
      free(cn);
    }
    vec_free(&names);
  }
}


static void zip_central (Zip *z) {
  unsigned long long cd_start = z->pos;
  size_t i;
  unsigned char h[46], eocd[22];
  for (i = 0; i < z->n; i++) {
    ZEnt *e = &z->ents[i];
    put32(h, 0x02014b50UL);
    put16(h + 4, e->made_by ? e->made_by : ((3 << 8) | 30));
    put16(h + 6, e->method == 8 ? 20 : 10);
    put16(h + 8, e->flags);
    put16(h + 10, e->method);
    put16(h + 12, e->mtime);
    put16(h + 14, e->mdate);
    put32(h + 16, e->crc);
    put32(h + 20, e->csize);
    put32(h + 24, e->usize);
    put16(h + 28, (unsigned)strlen(e->name));
    put16(h + 30, 0);
    put16(h + 32, 0);
    put16(h + 34, 0);
    put16(h + 36, e->internal);
    put32(h + 38, e->ext);
    put32(h + 42, e->offset);
    zip_sink(z, h, 46);
    zip_sink(z, (const unsigned char *)e->name, strlen(e->name));
  }
  put32(eocd, 0x06054b50UL);
  put16(eocd + 4, 0);
  put16(eocd + 6, 0);
  put16(eocd + 8, (unsigned)z->n);
  put16(eocd + 10, (unsigned)z->n);
  put32(eocd + 12, (unsigned long)(z->pos - cd_start));
  put32(eocd + 16, (unsigned long)cd_start);
  put16(eocd + 20, 0);
  zip_sink(z, eocd, 22);
}


int t_zip (int argc, char **argv, int in, int out, int err) {
  Zip z;
  Out o;
  int i, k;
  char *archive = NULL, *anative, *tmp = NULL;
  Vec paths;
  char num[24];
  (void)in;
  memset(&z, 0, sizeof(z));
  z.level = 6;
  z.err = err;
  z.oldfd = -1;
  vec_init(&z.exclude);
  vec_init(&paths);
  for (i = 1; i < argc; i++) {	/* zip's options can be anywhere; -x takes the rest */
    const char *a = argv[i];
    if (strcmp(a, "-x") == 0 || strcmp(a, "--exclude") == 0) {
      for (i++; i < argc && argv[i][0] != '-'; i++) vec_push(&z.exclude, glob_mark(argv[i]));
      i--;
      continue;
    }
    if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
      vec_free(&z.exclude);
      vec_free(&paths);
      return tool_help(out, "zip");
    }
    if (a[0] == '-' && a[1] && a[1] != '-') {
      const char *p;
      for (p = a + 1; *p; p++) {
        if (*p == 'r') z.recursive = 1;
        else if (*p == 'q') z.quiet = 1;
        else if (*p == 'j') z.junk = 1;
        else if (*p == 'm') z.move = 1;
        else if (*p >= '0' && *p <= '9') z.level = *p - '0';
        else if (*p == 'v' || *p == 'y' || *p == 'X' || *p == 'D' || *p == 'o') continue;
        else {
          fd_printf(err, "zip error: Invalid command arguments (short option '%c' not supported)\n", *p);
          vec_free(&z.exclude);
          vec_free(&paths);
          return 16;
        }
      }
      continue;
    }
    if (archive == NULL) archive = argv[i];
    else vec_push(&paths, xstrdup(argv[i]));
  }
  if (archive == NULL) {
    fd_printf(err, "zip error: Nothing to do! (try: zip -r archive.zip folder)\n");
    vec_free(&z.exclude);
    vec_free(&paths);
    return 16;
  }
  {	/* "zip out dir": out.zip, unless "out" is there */
    char *native = path_to_native(archive);
    OsStat st;
    if (os_stat(native, &st) != 0 && strchr(tool_base(archive), '.') == NULL) archive = xstrcat3(archive, ".zip", "");
    else archive = xstrdup(archive);
    free(native);
  }
  if (paths.n == 0) {
    fd_printf(err, "zip error: Nothing to do! (%s)\n", archive);
    free(archive);
    vec_free(&z.exclude);
    vec_free(&paths);
    return 12;
  }
  anative = path_to_native(archive);
  {	/* an existing archive: its entries stay, unless replaced */
    OsStat st;
    if (os_stat(anative, &st) == 0) {
      z.oldfd = os_open(anative, OS_READ);
      if (z.oldfd >= 0 && read_central(z.oldfd, &z.old, &z.nold, -1, archive) != 0) {
        fd_printf(err, "zip error: Zip file structure invalid (%s)\n", archive);
        os_close(z.oldfd);
        free(archive);
        free(anative);
        vec_free(&z.exclude);
        vec_free(&paths);
        return 3;
      }
      for (k = 0; k < (int)z.nold; k++) z.old[k].keep = 1;
    }
  }
  /* written to a temporary file next to it, then renamed */
  for (k = 0;; k++) {
    char *dir = path_dirname(anative), base[48];
    snprintf(base, sizeof(base), "zi%s.tmp", ll_to_str((long long)os_getpid() * 100 + k, num));
    tmp = path_join(dir, base);
    free(dir);
    z.fd = os_open(tmp, OS_EXCL);
    if (z.fd >= 0 || k > 50) break;
    free(tmp);
  }
  if (z.fd < 0) {
    fd_printf(err, "zip error: Could not create output file (%s)\n", archive);
    free(tmp);
    free(archive);
    free(anative);
    if (z.oldfd >= 0) os_close(z.oldfd);
    zent_free(z.old, z.nold);
    vec_free(&z.exclude);
    vec_free(&paths);
    return 15;
  }
  out_init(&o, out);
  z.o = &o;
  /* the new entries first decide which old ones go; old ones are copied first */
  {
    Vec dummy;
    size_t q;
    vec_init(&dummy);
    (void)dummy;
    for (q = 0; q < paths.n; q++) {	/* mark replaced names */
      char *pn = path_to_native(paths.v[q]);
      OsStat st;
      const char *nm = paths.v[q];
      while (nm[0] == '.' && nm[1] == '/') nm += 2;
      if (os_stat(pn, &st) == 0 && !st.is_dir) {
        for (k = 0; k < (int)z.nold; k++)
          if (strcmp(z.old[k].name, z.junk ? tool_base(nm) : nm) == 0) z.old[k].keep = 2;
      }
      free(pn);
    }
    for (k = 0; k < (int)z.nold; k++) {
      if (z.old[k].keep == 1) zip_copy_old(&z, &z.old[k]);
      if (z.old[k].keep == 2) z.old[k].keep = 1;	/* zip_add sees it and says "updating" */
    }
    /* old ones already copied must not be copied again: mark them as done */
    for (k = 0; k < (int)z.nold; k++) {
      size_t j;
      for (j = 0; j < z.n; j++)
        if (strcmp(z.ents[j].name, z.old[k].name) == 0) z.old[k].keep = 0;
    }
  }
  for (k = 0; k < (int)paths.n && !tool_stop(); k++) {
    char *native = path_to_native(paths.v[k]);
    zip_add(&z, paths.v[k], native);
    free(native);
  }
  zip_central(&z);
  out_flush(&o);
  os_close(z.fd);
  if (z.oldfd >= 0) os_close(z.oldfd);
  if (tool_stop() || z.n == 0) {
    os_unlink(tmp);
    if (z.n == 0 && !tool_stop()) fd_printf(err, "zip error: Nothing to do! (%s)\n", archive);
    z.status = z.n == 0 ? 12 : 130;
  }
  else if (os_rename(tmp, anative) != 0) {
    fd_printf(err, "zip error: Temporary file failure (%s)\n", os_errmsg());
    os_unlink(tmp);
    z.status = 10;
  }
  else if (z.move) {	/* -m: the originals go */
    for (k = (int)paths.n - 1; k >= 0; k--) {
      char *native = path_to_native(paths.v[k]);
      OsStat st;
      if (os_stat(native, &st) == 0 && !st.is_dir) os_unlink(native);
      free(native);
    }
  }
  free(tmp);
  free(archive);
  free(anative);
  zent_free(z.ents, z.n);
  zent_free(z.old, z.nold);
  vec_free(&z.exclude);
  vec_free(&paths);
  return z.status;
}

/* }================================================================== */


/*
** {==================================================================
** unzip
** ===================================================================
*/

typedef struct LimSrc {	/* reads at most 'left' bytes of a descriptor */
  int fd;
  unsigned long left;
} LimSrc;


static long lim_source (void *ctx, unsigned char *p, size_t n) {
  LimSrc *s = (LimSrc *)ctx;
  long r;
  if (s->left == 0) return 0;
  if (n > s->left) n = s->left;
  r = os_read(s->fd, p, n);
  if (r > 0) s->left -= (unsigned long)r;
  return r;
}


typedef struct CrcOut {
  int fd;
  unsigned long crc;
  unsigned long size;
  int failed;
} CrcOut;


static int crc_out (void *ctx, const unsigned char *p, size_t n) {
  CrcOut *c = (CrcOut *)ctx;
  c->crc = crc32_update(c->crc, p, n);
  c->size += (unsigned long)n;
  if (c->fd >= 0 && os_write(c->fd, p, n) < 0) {
    c->failed = 1;
    return -1;
  }
  return 0;
}


/* the data of one entry into fd (-1: only check it); 0 ok */
static int unzip_data (int zfd, const ZEnt *e, int fd, unsigned long *crc) {
  unsigned char lh[30];
  LimSrc ls;
  CrcOut co;
  if (os_seek(zfd, (long long)e->offset, 0) < 0 || os_read(zfd, lh, 30) != 30 || get32(lh) != 0x04034b50UL)
    return -1;
  os_seek(zfd, (long long)e->offset + 30 + get16(lh + 26) + get16(lh + 28), 0);
  ls.fd = zfd;
  ls.left = e->csize;
  co.fd = fd;
  co.crc = 0;
  co.size = 0;
  co.failed = 0;
  if (e->method == 0) {
    unsigned char buf[65536];
    long r;
    while ((r = lim_source(&ls, buf, sizeof(buf))) > 0)
      if (crc_out(&co, buf, (size_t)r) != 0) break;
  }
  else if (e->method == 8) {
    ISrc *src = (ISrc *)xmalloc(sizeof(ISrc));
    int r;
    isrc_init(src, lim_source, &ls);
    r = inflate_stream(src, crc_out, &co);
    free(src);
    if (r != 0) return -1;
  }
  else return -2;
  *crc = co.crc;
  return co.failed ? -1 : 0;
}


/* the path to write an entry to, made safe; NULL: skip */
static char *unzip_path (const char *name, int junk, const char *dir, int err, int *warned) {
  const char *p = name;
  Buf b;
  int bad = 0;
  while (*p == '/') p++;
  if (junk) p = tool_base(p);
  buf_init(&b);
  while (*p) {	/* drop ".." parts */
    const char *e = strchr(p, '/');
    size_t n = e ? (size_t)(e - p) : strlen(p);
    if (n == 2 && p[0] == '.' && p[1] == '.') bad = 1;
    else if (!(n == 1 && p[0] == '.') && n > 0) {
      if (b.len) buf_putc(&b, '/');
      buf_putn(&b, p, n);
    }
    if (!e) break;
    p = e + 1;
  }
  if (bad && !*warned) {
    fd_printf(err, "warning:  skipped \"../\" path component(s) in %s\n", name);
    *warned = 1;
  }
  if (b.len == 0) {
    buf_free(&b);
    return NULL;
  }
  {
    char *rel = buf_take(&b), *full = dir ? tool_join(dir, rel) : xstrdup(rel);
    free(rel);
    return full;
  }
}


int t_unzip (int argc, char **argv, int in, int out, int err) {
  const char *archive = NULL, *dir = NULL;
  int list = 0, test = 0, overwrite = 0, never = 0, quiet = 0, junk = 0, pipe_ = 0, verbose_list = 0;
  int i, zfd, status = 0, answer_all = 0, answer_none = 0;
  Vec members, excludes;
  ZEnt *ents;
  size_t n, k;
  Out o;
  char *anat;
  vec_init(&members);
  vec_init(&excludes);
  for (i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "-x") == 0) {
      for (i++; i < argc && argv[i][0] != '-'; i++) vec_push(&excludes, glob_mark(argv[i]));
      i--;
      continue;
    }
    if (strcmp(a, "-d") == 0) {
      if (i + 1 < argc) dir = argv[++i];
      continue;
    }
    if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
      vec_free(&members);
      vec_free(&excludes);
      return tool_help(out, "unzip");
    }
    if (a[0] == '-' && a[1]) {
      const char *p;
      for (p = a + 1; *p; p++) {
        switch (*p) {
          case 'l': list = 1; break;
          case 'v': list = 1; verbose_list = 1; break;
          case 't': test = 1; break;
          case 'o': overwrite = 1; never = 0; break;
          case 'n': never = 1; overwrite = 0; break;
          case 'q': quiet++; break;
          case 'j': junk = 1; break;
          case 'p': pipe_ = 1; quiet = 2; break;
          case 'c': pipe_ = 1; break;
          case 'd': if (p[1]) { dir = p + 1; p += strlen(p) - 1; } else if (i + 1 < argc) dir = argv[++i]; break;
          case 'a': case 'b': case 'C': case 'L': case 'X': case 'u': case 'f': case 'D': case 'U': break;
          default:
            fd_printf(err, "unzip:  unrecognized option -%c\n", *p);
            vec_free(&members);
            vec_free(&excludes);
            return 10;
        }
      }
      continue;
    }
    if (archive == NULL) archive = a;
    else vec_push(&members, glob_mark(a));
  }
  (void)verbose_list;
  if (archive == NULL) {
    fd_printf(out, "UnZip (mmc) - usage: unzip [-lotnqjp] [-d dir] file[.zip] [list] [-x xlist]\n");
    vec_free(&members);
    vec_free(&excludes);
    return 0;
  }
  anat = path_to_native(archive);
  zfd = os_open(anat, OS_READ);
  if (zfd < 0) {	/* x -> x.zip */
    char *z = xstrcat3(anat, ".zip", "");
    zfd = os_open(z, OS_READ);
    free(z);
  }
  free(anat);
  if (zfd < 0) {
    tool_err(err, "unzip", "cannot find or open %s, %s.zip or %s.ZIP.", archive, archive, archive);
    vec_free(&members);
    vec_free(&excludes);
    return 9;
  }
  if (read_central(zfd, &ents, &n, err, archive) != 0) {
    os_close(zfd);
    vec_free(&members);
    vec_free(&excludes);
    return 9;
  }
  out_init(&o, out);
  if (quiet < 2 && !pipe_) out_printf(&o, "Archive:  %s\n", archive);
  if (list) {
    unsigned long long total = 0;
    size_t files = 0;
    out_puts(&o, "  Length      Date    Time    Name\n---------  ---------- -----   ----\n");
    for (k = 0; k < n; k++) {
      ZEnt *e = &ents[k];
      out_printf(&o, "%9lu  %02u-%02u-%04u %02u:%02u   %s\n", e->usize, (e->mdate >> 5) & 15, e->mdate & 31,
                 ((e->mdate >> 9) & 127) + 1980, (e->mtime >> 11) & 31, (e->mtime >> 5) & 63, e->name);
      total += e->usize;
      files++;
    }
    out_printf(&o, "---------                     -------\n%9llu                     %lu file%s\n", total,
               (unsigned long)files, files == 1 ? "" : "s");
    out_flush(&o);
    zent_free(ents, n);
    os_close(zfd);
    vec_free(&members);
    vec_free(&excludes);
    return 0;
  }
  if (dir && !pipe_ && !test) {
    char *dn = path_to_native(dir);
    OsStat st;
    if (os_stat(dn, &st) != 0) {
      if (quiet < 1) out_printf(&o, "   creating: %s/\n", dir);
      mkdir_p(dn);
    }
    free(dn);
  }
  {
    int warned = 0, found_any = members.n == 0;
    for (k = 0; k < n && !tool_stop(); k++) {
      ZEnt *e = &ents[k];
      int is_dir = e->name[0] && e->name[strlen(e->name) - 1] == '/';
      char *dest, *native;
      unsigned long crc = 0;
      int r;
      size_t m;
      if (members.n > 0) {
        int want = 0;
        for (m = 0; m < members.n; m++)
          if (pat_match(members.v[m], e->name, 0)) want = 1;
        if (!want) continue;
        found_any = 1;
      }
      {
        int skip = 0;
        for (m = 0; m < excludes.n; m++)
          if (pat_match(excludes.v[m], e->name, 0)) skip = 1;
        if (skip) continue;
      }
      if (e->flags & 1) {
        tool_err(err, "unzip", "%s: encrypted entries are not supported -- skipping", e->name);
        status = 1;
        continue;
      }
      if (test) {
        r = unzip_data(zfd, e, -1, &crc);
        if (r != 0 || crc != e->crc) {
          out_printf(&o, "    testing: %-22s  bad CRC %08lx  (should be %08lx)\n", e->name, crc, e->crc);
          status = 2;
        }
        else if (quiet < 1) out_printf(&o, "    testing: %-22s  OK\n", e->name);
        continue;
      }
      if (pipe_) {
        if (is_dir) continue;
        out_flush(&o);
        r = unzip_data(zfd, e, out, &crc);
        if (r == -2) {
          tool_err(err, "unzip", "%s: unsupported compression method %u", e->name, e->method);
          status = 1;
        }
        continue;
      }
      if (is_dir && junk) continue;
      dest = unzip_path(e->name, junk, dir, err, &warned);
      if (dest == NULL) continue;
      native = path_to_native(dest);
      if (is_dir) {
        OsStat st;
        if (os_stat(native, &st) != 0) {
          if (quiet < 1) out_printf(&o, "   creating: %s\n", dest);
          mkdir_p(native);
        }
      }
      else {
        OsStat st;
        char *parent = path_dirname(native);
        int fd, skip = 0;
        mkdir_p(parent);
        free(parent);
        if (os_lstat(native, &st) == 0) {
          if (never || answer_none) skip = 1;
          else if (!overwrite && !answer_all) {
            char ans[16];
            long got = 0;
            out_flush(&o);
            fd_printf(out, "replace %s? [y]es, [n]o, [A]ll, [N]one, [r]ename: ", dest);
            while (got < (long)sizeof(ans) - 1) {
              char ch;
              if (os_read(in, &ch, 1) != 1) {
                if (got == 0) {
                  fd_printf(out, "(EOF or read error, treating as \"[N]one\" ...)\n");
                  ans[got++] = 'N';
                }
                break;
              }
              if (ch == '\n') break;
              ans[got++] = ch;
            }
            ans[got] = '\0';
            if (ans[0] == 'A') answer_all = 1;
            else if (ans[0] == 'N') answer_none = skip = 1;
            else if (ans[0] != 'y') skip = 1;
          }
        }
        if (skip) {
          free(dest);
          free(native);
          continue;
        }
        if (os_lstat(native, &st) == 0 && !(st.mode & 0200)) os_chmod(native, 0666);
        fd = os_open(native, OS_WRITE);
        if (fd < 0) {
          tool_err(err, "unzip", "cannot create %s: %s", dest, os_errmsg());
          status = 1;
          free(dest);
          free(native);
          continue;
        }
        if (quiet < 1) out_printf(&o, "%s: %s\n", e->method == 0 ? " extracting" : "  inflating", dest);
        r = unzip_data(zfd, e, fd, &crc);
        os_close(fd);
        if (r == -2) {
          tool_err(err, "unzip", "%s: unsupported compression method %u", e->name, e->method);
          os_unlink(native);
          status = 1;
        }
        else if (r != 0 || crc != e->crc) {
          out_flush(&o);
          fd_printf(err, "%s:  bad CRC %08lx  (should be %08lx)\n", dest, crc, e->crc);
          status = 2;
        }
        else {
          time_t t = unix_time(e->mtime, e->mdate);
          os_utime(native, t, t);
          if ((e->made_by >> 8) == 3 && (e->ext >> 16) != 0) os_chmod(native, (unsigned)(e->ext >> 16) & 07777);
        }
      }
      free(dest);
      free(native);
    }
    /* folders get their times after their files were written */
    if (!pipe_ && !test) {
      for (k = 0; k < n; k++) {
        ZEnt *e = &ents[k];
        size_t l = strlen(e->name);
        if (l && e->name[l - 1] == '/' && !junk) {
          char *dest = unzip_path(e->name, 0, dir, err, &warned), *native;
          if (!dest) continue;
          native = path_to_native(dest);
          {
            time_t t = unix_time(e->mtime, e->mdate);
            os_utime(native, t, t);
          }
          free(dest);
          free(native);
        }
      }
    }
    if (test && status == 0 && quiet < 2) out_printf(&o, "No errors detected in compressed data of %s.\n", archive);
    if (!found_any) {
      out_flush(&o);
      fd_printf(err, "caution: filename not matched:  %s\n", members.v[0]);
      status = 11;
    }
  }
  out_flush(&o);
  zent_free(ents, n);
  os_close(zfd);
  vec_free(&members);
  vec_free(&excludes);
  return tool_stop() ? 130 : status;
}

/* }================================================================== */
