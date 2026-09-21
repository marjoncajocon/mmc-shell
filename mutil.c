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


void buf_printf (Buf *b, const char *fmt, ...) {
  char small[256];
  va_list ap;
  int n;
  va_start(ap, fmt);
  n = vsnprintf(small, sizeof(small), fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if ((size_t)n < sizeof(small)) {
    buf_putn(b, small, (size_t)n);
    return;
  }
  {
    char *big = (char *)xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    buf_putn(b, big, (size_t)n);
    free(big);
  }
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


void vec_copy (Vec *dst, char *const *src, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) vec_push(dst, xstrdup(src[i]));
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


/* bytes of the UTF-8 character at s (1 for a broken one) */
int utf8_len (const char *s) {
  unsigned char c = (unsigned char)*s;
  int n = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1, i;
  for (i = 1; i < n; i++)
    if (((unsigned char)s[i] & 0xC0) != 0x80) return 1;
  return n;
}


/*
** Columns a character takes on a terminal: 0 (it joins the one before:
** accents, joiners, variation selectors), 1, or 2 (CJK, emoji). The
** line editor and mmc-term both ask here, so they agree on the cursor.
*/
int uc_width (unsigned long cp) {
  static const unsigned long zero[][2] = {
    {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x05BF, 0x05BF},
    {0x05C1, 0x05C2}, {0x05C4, 0x05C5}, {0x05C7, 0x05C7}, {0x0610, 0x061A},
    {0x064B, 0x065F}, {0x0670, 0x0670}, {0x06D6, 0x06DC}, {0x06DF, 0x06E4},
    {0x06E7, 0x06E8}, {0x06EA, 0x06ED}, {0x0900, 0x0902}, {0x093A, 0x093A},
    {0x093C, 0x093C}, {0x0941, 0x0948}, {0x094D, 0x094D}, {0x0951, 0x0957},
    {0x0E31, 0x0E31}, {0x0E34, 0x0E3A}, {0x0E47, 0x0E4E}, {0x1AB0, 0x1AFF},
    {0x1DC0, 0x1DFF}, {0x200B, 0x200F}, {0x2028, 0x202E}, {0x2060, 0x2064},
    {0x20D0, 0x20FF}, {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F}, {0xFEFF, 0xFEFF},
    {0x1F3FB, 0x1F3FF}, {0xE0020, 0xE007F}, {0xE0100, 0xE01EF}
  };
  static const unsigned long wide[][2] = {
    {0x1100, 0x115F}, {0x231A, 0x231B}, {0x2329, 0x232A}, {0x23E9, 0x23EC},
    {0x23F0, 0x23F0}, {0x23F3, 0x23F3}, {0x25FD, 0x25FE}, {0x2614, 0x2615},
    {0x2648, 0x2653}, {0x267F, 0x267F}, {0x2693, 0x2693}, {0x26A1, 0x26A1},
    {0x26AA, 0x26AB}, {0x26BD, 0x26BE}, {0x26C4, 0x26C5}, {0x26CE, 0x26CE},
    {0x26D4, 0x26D4}, {0x26EA, 0x26EA}, {0x26F2, 0x26F3}, {0x26F5, 0x26F5},
    {0x26FA, 0x26FA}, {0x26FD, 0x26FD}, {0x2705, 0x2705}, {0x270A, 0x270B},
    {0x2728, 0x2728}, {0x274C, 0x274C}, {0x274E, 0x274E}, {0x2753, 0x2755},
    {0x2757, 0x2757}, {0x2795, 0x2797}, {0x27B0, 0x27B0}, {0x27BF, 0x27BF},
    {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55}, {0x2E80, 0x303E},
    {0x3041, 0x33FF}, {0x3400, 0x4DBF}, {0x4E00, 0x9FFF}, {0xA000, 0xA4CF},
    {0xA960, 0xA97F}, {0xAC00, 0xD7A3}, {0xF900, 0xFAFF}, {0xFE10, 0xFE19},
    {0xFE30, 0xFE6F}, {0xFF00, 0xFF60}, {0xFFE0, 0xFFE6}, {0x16FE0, 0x16FE4},
    {0x17000, 0x18CFF}, {0x1B000, 0x1B2FF}, {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF},
    {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A}, {0x1F200, 0x1F202}, {0x1F210, 0x1F23B},
    {0x1F240, 0x1F248}, {0x1F250, 0x1F251}, {0x1F260, 0x1F265}, {0x1F300, 0x1F64F},
    {0x1F680, 0x1F6FF}, {0x1F7E0, 0x1F7EB}, {0x1F90C, 0x1F9FF}, {0x1FA70, 0x1FAFF},
    {0x20000, 0x3FFFD}
  };
  size_t lo, hi;
  if (cp < 0x300) return 1;
  lo = 0;	/* both tables are sorted: binary search */
  hi = sizeof(zero) / sizeof(zero[0]);
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (cp < zero[mid][0]) hi = mid;
    else if (cp > zero[mid][1]) lo = mid + 1;
    else return 0;
  }
  lo = 0;
  hi = sizeof(wide) / sizeof(wide[0]);
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (cp < wide[mid][0]) hi = mid;
    else if (cp > wide[mid][1]) lo = mid + 1;
    else return 2;
  }
  return 1;
}


int is_name_n (const char *s, size_t n) {
  size_t i;
  if (n == 0 || !(isalpha((unsigned char)s[0]) || s[0] == '_')) return 0;
  for (i = 1; i < n; i++)
    if (!(isalnum((unsigned char)s[i]) || s[i] == '_')) return 0;
  return 1;
}


int is_name (const char *s) {
  return is_name_n(s, strlen(s));
}


/* the whole string is a decimal number (spaces around allowed) */
int str_to_ll (const char *s, long long *out) {
  unsigned long long v = 0;
  int neg = 0, any = 0;
  while (*s == ' ' || *s == '\t' || *s == '\n') s++;
  if (*s == '+' || *s == '-') neg = (*s++ == '-');
  for (; *s >= '0' && *s <= '9'; s++, any = 1) v = v * 10 + (unsigned long long)(*s - '0');
  while (*s == ' ' || *s == '\t' || *s == '\n') s++;
  if (!any || *s != '\0') return -1;
  *out = neg ? (long long)(0ULL - v) : (long long)v;
  return 0;
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


/* CR LF -> LF: scripts saved on Windows run like the others */
void crlf_to_lf (char *s, size_t *len) {
  size_t i, j = 0, n = len ? *len : strlen(s);
  for (i = 0; i < n; i++) {
    if (s[i] == '\r' && i + 1 < n && s[i + 1] == '\n') continue;
    s[j++] = s[i];
  }
  s[j] = '\0';
  if (len) *len = j;
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
