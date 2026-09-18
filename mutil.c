/*
** mutil.c - memory, strings, buffers
*/

#include "mmc.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


void *xmalloc (size_t n) {
  void *p = malloc(n ? n : 1);
  if (p == NULL) {
    os_write(2, "mmc: out of memory\n", 19);
    exit(2);
  }
  return p;
}


void *xrealloc (void *p, size_t n) {
  p = realloc(p, n ? n : 1);
  if (p == NULL) {
    os_write(2, "mmc: out of memory\n", 19);
    exit(2);
  }
  return p;
}


char *xstrndup (const char *s, size_t n) {
  char *d = (char *)xmalloc(n + 1);
  memcpy(d, s, n);
  d[n] = '\0';
  return d;
}


char *xstrdup (const char *s) {
  return xstrndup(s, strlen(s));
}


char *xstrcat3 (const char *a, const char *b, const char *c) {
  Buf r;
  buf_init(&r);
  buf_puts(&r, a);
  buf_puts(&r, b);
  buf_puts(&r, c);
  return buf_take(&r);
}


/*
** {==================================================================
** Buf
** ===================================================================
*/

void buf_init (Buf *b) {
  b->s = NULL;
  b->len = b->cap = 0;
}


void buf_putn (Buf *b, const char *s, size_t n) {
  if (b->len + n + 1 > b->cap) {
    size_t cap = b->cap ? b->cap * 2 : 64;
    while (cap < b->len + n + 1) cap *= 2;
    b->s = (char *)xrealloc(b->s, cap);
    b->cap = cap;
  }
  if (n) memcpy(b->s + b->len, s, n);
  b->len += n;
  b->s[b->len] = '\0';
}


void buf_putc (Buf *b, char c) {
  buf_putn(b, &c, 1);
}


void buf_puts (Buf *b, const char *s) {
  buf_putn(b, s, strlen(s));
}


/* hands the string to the caller (never NULL) and resets the buffer */
char *buf_take (Buf *b) {
  char *s = b->s ? b->s : xstrdup("");
  buf_init(b);
  return s;
}


void buf_free (Buf *b) {
  free(b->s);
  buf_init(b);
}

/* }================================================================== */


/*
** {==================================================================
** Vec
** ===================================================================
*/

void vec_init (Vec *v) {
  v->v = NULL;
  v->n = v->cap = 0;
}


void vec_push (Vec *v, char *s) {
  vec_insert(v, v->n, s);
}


void vec_insert (Vec *v, size_t at, char *s) {
  if (v->n + 2 > v->cap) {
    v->cap = v->cap ? v->cap * 2 : 8;
    v->v = (char **)xrealloc(v->v, v->cap * sizeof(char *));
  }
  memmove(v->v + at + 1, v->v + at, (v->n - at) * sizeof(char *));
  v->v[at] = s;
  v->n++;
  v->v[v->n] = NULL;
}


static int cmp_names (const void *a, const void *b) {
  const char *x = *(const char *const *)a;
  const char *y = *(const char *const *)b;
  int r = m_stricmp(x, y);
  return r ? r : strcmp(x, y);
}


void vec_sort (Vec *v) {
  if (v->n > 1) qsort(v->v, v->n, sizeof(char *), cmp_names);
}


void vec_free (Vec *v) {
  size_t i;
  for (i = 0; i < v->n; i++) free(v->v[i]);
  free(v->v);
  vec_init(v);
}

/* }================================================================== */


int fd_puts (int fd, const char *s) {
  return (int)os_write(fd, s, strlen(s));
}


int fd_printf (int fd, const char *fmt, ...) {
  char small[512];
  char *big;
  va_list ap;
  int n;
  va_start(ap, fmt);
  n = vsnprintf(small, sizeof(small), fmt, ap);
  va_end(ap);
  if (n < 0) return -1;
  if ((size_t)n < sizeof(small)) return (int)os_write(fd, small, (size_t)n);
  big = (char *)xmalloc((size_t)n + 1);
  va_start(ap, fmt);
  vsnprintf(big, (size_t)n + 1, fmt, ap);
  va_end(ap);
  n = (int)os_write(fd, big, (size_t)n);
  free(big);
  return n;
}


int m_stricmp (const char *a, const char *b) {
  for (;; a++, b++) {
    int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
    if (x != y) return x - y;
    if (x == 0) return 0;
  }
}


int m_strnicmp (const char *a, const char *b, size_t n) {
  for (; n > 0; a++, b++, n--) {
    int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
    if (x != y) return x - y;
    if (x == 0) return 0;
  }
  return 0;
}


/* file names and env names ignore case on Windows only */
int m_fncmp (const char *a, const char *b) {
#ifdef _WIN32
  return m_stricmp(a, b);
#else
  return strcmp(a, b);
#endif
}


int m_fnncmp (const char *a, const char *b, size_t n) {
#ifdef _WIN32
  return m_strnicmp(a, b, n);
#else
  return strncmp(a, b, n);
#endif
}


int m_envcmp (const char *a, const char *b) {
  return m_fncmp(a, b);
}


/* number of code points in the first 'nbytes' bytes of UTF-8 text */
size_t utf8_count (const char *s, size_t nbytes) {
  size_t i, n = 0;
  for (i = 0; i < nbytes; i++)
    if (((unsigned char)s[i] & 0xC0) != 0x80) n++;
  return n;
}


/* "%lld" is not portable to every C runtime; do it by hand */
char *ll_to_str (long long v, char *out) {
  char tmp[24];
  int i = 0, j = 0;
  unsigned long long u = (v < 0) ? 0ULL - (unsigned long long)v
                                 : (unsigned long long)v;
  do {
    tmp[i++] = (char)('0' + (int)(u % 10));
    u /= 10;
  } while (u != 0);
  if (v < 0) out[j++] = '-';
  while (i > 0) out[j++] = tmp[--i];
  out[j] = '\0';
  return out;
}


/* whole file as a NUL terminated string, or NULL */
char *read_file (const char *native, size_t *len) {
  Buf b;
  char chunk[4096];
  long n;
  int fd = os_open(native, OS_READ);
  if (fd < 0) return NULL;
  buf_init(&b);
  while ((n = os_read(fd, chunk, sizeof(chunk))) > 0) {
    buf_putn(&b, chunk, (size_t)n);
    if (b.len > ((size_t)64 << 20)) break;	/* be sane */
  }
  os_close(fd);
  if (len) *len = b.len;
  return buf_take(&b);
}


/* creates a directory and its parents; returns 0 if it exists afterwards */
int mkdir_p (const char *native) {
  char *p = xstrdup(native);
  size_t i;
  OsStat st;
  for (i = 1; p[i] != '\0'; i++) {
    if (!path_is_sep(p[i])) continue;
    if (p[i - 1] == ':' || path_is_sep(p[i - 1])) continue;	/* "D:\", "//" */
    p[i] = '\0';
    os_mkdir(p);
    p[i] = MMC_SEP;
  }
  os_mkdir(p);
  os_stat(p, &st);
  free(p);
  return (st.exists && st.is_dir) ? 0 : -1;
}
