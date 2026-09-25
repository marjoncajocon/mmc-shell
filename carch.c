/*
** carch.c - compression and archives: deflate/inflate (RFC 1951), CRC-32,
** and the tools gzip gunzip zcat tar
**
** deflate: LZ77 over a 32K window with hash chains and lazy matching
** (like zlib's level 6), then fixed or dynamic Huffman blocks, whichever
** is smaller. inflate: all three block types, with a 9-bit lookup table
** for the common codes.
*/

#include "mmc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*
** {==================================================================
** CRC-32
** ===================================================================
*/

static unsigned long crc_table[256];
static int crc_ready = 0;

unsigned long crc32_update (unsigned long crc, const void *data, size_t n) {
  const unsigned char *p = (const unsigned char *)data;
  size_t i;
  if (!crc_ready) {
    unsigned long c;
    int k, j;
    for (k = 0; k < 256; k++) {
      c = (unsigned long)k;
      for (j = 0; j < 8; j++) c = (c & 1) ? 0xEDB88320UL ^ (c >> 1) : c >> 1;
      crc_table[k] = c;
    }
    crc_ready = 1;
  }
  crc = crc ^ 0xFFFFFFFFUL;
  for (i = 0; i < n; i++) crc = crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFUL;
}

/* }================================================================== */


/*
** {==================================================================
** Tables shared by both directions
** ===================================================================
*/

static const unsigned short len_base[29] = {
  3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115,
  131, 163, 195, 227, 258};
static const unsigned char len_extra[29] = {
  0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const unsigned short dist_base[30] = {
  1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537,
  2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const unsigned char dist_extra[30] = {
  0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
static const unsigned char cl_order[19] = {
  16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

/* }================================================================== */


/*
** {==================================================================
** deflate
** ===================================================================
*/

#define WSIZE	32768
#define WMASK	(WSIZE - 1)
#define HBITS	15
#define HSIZE	(1 << HBITS)
#define MINM	3
#define MAXM	258
#define LOOK	(MAXM + MINM + 1)
#define SYMMAX	16384

struct Deflate {
  unsigned char win[2 * WSIZE];
  long wend, pos;	/* data in win; the next position to code */
  int head[HSIZE];
  int prev[WSIZE];
  unsigned hash;
  /* lazy matching */
  int match_avail;
  long match_len, match_start, prev_len, prev_match;
  int chain, lazy, nice, good;
  /* the symbols of the block */
  unsigned short syms[SYMMAX], dists[SYMMAX];
  int nsyms;
  /* bits out */
  unsigned long long bitbuf;
  int bitcnt;
  unsigned char out[65536];
  size_t outn;
  SinkFn sink;
  void *ctx;
  int failed;
};


static void d_flush_out (Deflate *d) {
  if (d->outn > 0 && !d->failed && d->sink(d->ctx, d->out, d->outn) != 0) d->failed = 1;
  d->outn = 0;
}


static void put_bits (Deflate *d, unsigned v, int n) {
  d->bitbuf |= (unsigned long long)v << d->bitcnt;
  d->bitcnt += n;
  while (d->bitcnt >= 8) {
    d->out[d->outn++] = (unsigned char)d->bitbuf;
    d->bitbuf >>= 8;
    d->bitcnt -= 8;
    if (d->outn == sizeof(d->out)) d_flush_out(d);
  }
}


static void align_byte (Deflate *d) {
  if (d->bitcnt > 0) put_bits(d, 0, 8 - d->bitcnt);
}


static unsigned reverse_bits (unsigned code, int len) {
  unsigned r = 0;
  int i;
  for (i = 0; i < len; i++) {
    r = (r << 1) | (code & 1);
    code >>= 1;
  }
  return r;
}


/* Huffman code lengths for n symbols, at most maxbits long */
static void build_lengths (const unsigned *freq, int n, int maxbits, unsigned char *len) {
  int *order = (int *)xmalloc((size_t)n * sizeof(int)), used = 0, i, j;
  unsigned long long *w;
  int *parent;
  memset(len, 0, (size_t)n);
  for (i = 0; i < n; i++)
    if (freq[i] > 0) order[used++] = i;
  if (used == 0) {
    free(order);
    return;
  }
  if (used == 1) {
    len[order[0]] = 1;
    free(order);
    return;
  }
  /* sort by frequency (insertion sort is fine: n <= 288) */
  for (i = 1; i < used; i++) {
    int s = order[i];
    for (j = i; j > 0 && freq[order[j - 1]] > freq[s]; j--) order[j] = order[j - 1];
    order[j] = s;
  }
  /* two-queue Huffman: leaves 0..used-1, inner nodes used.. */
  w = (unsigned long long *)xmalloc((size_t)(2 * used) * sizeof(unsigned long long));
  parent = (int *)xmalloc((size_t)(2 * used) * sizeof(int));
  {
    int li = 0, ni = used, nn = used;
    for (i = 0; i < used; i++) w[i] = freq[order[i]];
    while (nn < 2 * used - 1) {
      int a, b;
      if (li < used && (ni >= nn || w[li] <= w[ni])) a = li++; else a = ni++;
      if (li < used && (ni >= nn || w[li] <= w[ni])) b = li++; else b = ni++;
      w[nn] = w[a] + w[b];
      parent[a] = parent[b] = nn;
      nn++;
    }
    parent[2 * used - 2] = -1;
    {
      int *depth = (int *)xmalloc((size_t)(2 * used) * sizeof(int));
      depth[2 * used - 2] = 0;
      for (i = 2 * used - 3; i >= 0; i--) depth[i] = depth[parent[i]] + 1;
      for (i = 0; i < used; i++) {
        int dd = depth[i];
        len[order[i]] = (unsigned char)(dd > maxbits ? maxbits : dd);
      }
      free(depth);
    }
  }
  /* too long codes were cut: lengthen others until the code fits (Kraft) */
  {
    long long kraft = 0, cap = 1LL << maxbits;
    for (i = 0; i < used; i++) kraft += 1LL << (maxbits - len[order[i]]);
    while (kraft > cap) {
      int best = -1;
      for (i = 0; i < used; i++) {	/* the least frequent code shorter than max */
        int s = order[i];
        if (len[s] < maxbits && (best < 0 || len[s] > len[best])) best = s;
      }
      if (best < 0) break;
      kraft -= 1LL << (maxbits - len[best] - 1);
      len[best]++;
    }
  }
  free(order);
  free(w);
  free(parent);
}


/* canonical codes, bit-reversed, from the lengths */
static void build_codes (const unsigned char *len, int n, unsigned *code) {
  int count[16] = {0}, next[16], i, c = 0;
  for (i = 0; i < n; i++) count[len[i]]++;
  count[0] = 0;
  for (i = 1; i < 16; i++) {
    c = (c + count[i - 1]) << 1;
    next[i] = c;
  }
  for (i = 0; i < n; i++)
    code[i] = len[i] ? reverse_bits((unsigned)next[len[i]]++, len[i]) : 0;
}


static int len_code (int len) {
  int k;
  for (k = 28; k > 0 && len_base[k] > len; k--) {}
  return k;
}


static int dist_code (int dist) {
  int k;
  for (k = 29; k > 0 && dist_base[k] > dist; k--) {}
  return k;
}


static void put_symbols (Deflate *d, const unsigned *lc, const unsigned char *ll, const unsigned *dc,
                         const unsigned char *dl) {
  int i;
  for (i = 0; i < d->nsyms; i++) {
    unsigned s = d->syms[i];
    if (d->dists[i] == 0) put_bits(d, lc[s], ll[s]);
    else {
      int len = (int)s, k = len_code(len), dist = d->dists[i], j = dist_code(dist);
      put_bits(d, lc[257 + k], ll[257 + k]);
      if (len_extra[k]) put_bits(d, (unsigned)(len - len_base[k]), len_extra[k]);
      put_bits(d, dc[j], dl[j]);
      if (dist_extra[j]) put_bits(d, (unsigned)(dist - dist_base[j]), dist_extra[j]);
    }
  }
  put_bits(d, lc[256], ll[256]);
}


static void flush_block (Deflate *d, int last) {
  unsigned lf[288], df[30], lc[288], dc[30];
  unsigned char ll[288], dl[30];
  unsigned char fll[288], fdl[30];
  unsigned flc[288], fdc[30];
  int i, hlit, hdist, hclen;
  long long dyn_bits = 0, fix_bits = 0;
  unsigned char all[320];
  int nall, rle[320], rle_extra[320], nrle = 0;
  unsigned cf[19], cc[19];
  unsigned char cl[19];
  memset(lf, 0, sizeof(lf));
  memset(df, 0, sizeof(df));
  for (i = 0; i < d->nsyms; i++) {
    if (d->dists[i] == 0) lf[d->syms[i]]++;
    else {
      lf[257 + len_code(d->syms[i])]++;
      df[dist_code(d->dists[i])]++;
    }
  }
  lf[256] = 1;
  build_lengths(lf, 286, 15, ll);
  build_lengths(df, 30, 15, dl);
  {	/* at least one distance code, and not a lonely one of length 0 */
    int any = 0;
    for (i = 0; i < 30; i++) if (dl[i]) any = 1;
    if (!any) dl[0] = 1;
  }
  build_codes(ll, 288, lc);
  build_codes(dl, 30, dc);
  for (hlit = 286; hlit > 257 && ll[hlit - 1] == 0; hlit--) {}
  for (hdist = 30; hdist > 1 && dl[hdist - 1] == 0; hdist--) {}
  /* the code lengths, run-length coded */
  nall = 0;
  for (i = 0; i < hlit; i++) all[nall++] = ll[i];
  for (i = 0; i < hdist; i++) all[nall++] = dl[i];
  memset(cf, 0, sizeof(cf));
  for (i = 0; i < nall;) {
    int v = all[i], run = 1;
    while (i + run < nall && all[i + run] == v) run++;
    if (v == 0 && run >= 3) {
      int r = run > 138 ? 138 : run;
      rle[nrle] = r >= 11 ? 18 : 17;
      rle_extra[nrle++] = r >= 11 ? r - 11 : r - 3;
      cf[r >= 11 ? 18 : 17]++;
      i += r;
    }
    else if (v != 0 && run >= 4) {
      int r = run - 1 > 6 ? 6 : run - 1;
      rle[nrle] = v;
      rle_extra[nrle++] = 0;
      cf[v]++;
      rle[nrle] = 16;
      rle_extra[nrle++] = r - 3;
      cf[16]++;
      i += r + 1;
    }
    else {
      rle[nrle] = v;
      rle_extra[nrle++] = 0;
      cf[v]++;
      i++;
    }
  }
  build_lengths(cf, 19, 7, cl);
  build_codes(cl, 19, cc);
  for (hclen = 19; hclen > 4 && cl[cl_order[hclen - 1]] == 0; hclen--) {}
  /* sizes of both kinds */
  for (i = 0; i < 288; i++) fll[i] = (unsigned char)(i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);
  for (i = 0; i < 30; i++) fdl[i] = 5;
  build_codes(fll, 288, flc);
  build_codes(fdl, 30, fdc);
  dyn_bits = 5 + 5 + 4 + 3 * hclen;
  for (i = 0; i < nrle; i++)
    dyn_bits += cl[rle[i]] + (rle[i] == 16 ? 2 : rle[i] == 17 ? 3 : rle[i] == 18 ? 7 : 0);
  for (i = 0; i < 286; i++) {
    dyn_bits += (long long)lf[i] * ll[i];
    fix_bits += (long long)lf[i] * fll[i];
    if (i >= 257) {
      dyn_bits += (long long)lf[i] * len_extra[i - 257];
      fix_bits += (long long)lf[i] * len_extra[i - 257];
    }
  }
  for (i = 0; i < 30; i++) {
    dyn_bits += (long long)df[i] * (dl[i] + dist_extra[i]);
    fix_bits += (long long)df[i] * (5 + dist_extra[i]);
  }
  if (fix_bits <= dyn_bits) {
    put_bits(d, last ? 1 : 0, 1);
    put_bits(d, 1, 2);
    put_symbols(d, flc, fll, fdc, fdl);
  }
  else {
    put_bits(d, last ? 1 : 0, 1);
    put_bits(d, 2, 2);
    put_bits(d, (unsigned)(hlit - 257), 5);
    put_bits(d, (unsigned)(hdist - 1), 5);
    put_bits(d, (unsigned)(hclen - 4), 4);
    for (i = 0; i < hclen; i++) put_bits(d, cl[cl_order[i]], 3);
    for (i = 0; i < nrle; i++) {
      put_bits(d, cc[rle[i]], cl[rle[i]]);
      if (rle[i] == 16) put_bits(d, (unsigned)rle_extra[i], 2);
      else if (rle[i] == 17) put_bits(d, (unsigned)rle_extra[i], 3);
      else if (rle[i] == 18) put_bits(d, (unsigned)rle_extra[i], 7);
    }
    put_symbols(d, lc, ll, dc, dl);
  }
  d->nsyms = 0;
}


static void emit_lit (Deflate *d, int c) {
  d->syms[d->nsyms] = (unsigned short)c;
  d->dists[d->nsyms++] = 0;
  if (d->nsyms == SYMMAX) flush_block(d, 0);
}


static void emit_match (Deflate *d, int len, int dist) {
  d->syms[d->nsyms] = (unsigned short)len;
  d->dists[d->nsyms++] = (unsigned short)dist;
  if (d->nsyms == SYMMAX) flush_block(d, 0);
}


#define HASH3(p)	((((unsigned)(p)[0] << 10) ^ ((unsigned)(p)[1] << 5) ^ (unsigned)(p)[2]) & (HSIZE - 1))

static int insert (Deflate *d, long pos) {
  unsigned h = HASH3(d->win + pos);
  int old = d->head[h];
  d->prev[pos & WMASK] = old;
  d->head[h] = (int)pos;
  return old;
}


static long longest (Deflate *d, int cur, long *start) {
  long best = d->prev_len, pos = d->pos, limit = pos > WSIZE - LOOK ? pos - (WSIZE - LOOK) : 0;
  int chain = d->chain;
  const unsigned char *scan = d->win + pos;
  long maxlen = d->wend - pos < MAXM ? d->wend - pos : MAXM;
  if (d->prev_len >= d->good) chain >>= 2;
  if (best < MINM - 1) best = MINM - 1;
  while (cur >= 0 && cur >= limit && chain-- > 0) {
    const unsigned char *m = d->win + cur;
    if (m[best] == scan[best] && m[0] == scan[0] && m[1] == scan[1]) {
      long l = 2;
      while (l < maxlen && m[l] == scan[l]) l++;
      if (l > best) {
        best = l;
        *start = cur;
        if (l >= d->nice || l >= maxlen) break;
      }
    }
    cur = d->prev[cur & WMASK];
  }
  return best;
}


/* codes what is in the window; all of it when finishing */
static void d_compress (Deflate *d, int finish) {
  while (d->pos < d->wend && (finish || d->wend - d->pos >= LOOK)) {
    int head = -1;
    if (d->wend - d->pos >= MINM) head = insert(d, d->pos);
    d->prev_len = d->match_len;
    d->prev_match = d->match_start;
    d->match_len = MINM - 1;
    if (head >= 0 && d->prev_len < d->lazy && d->pos - head <= WSIZE - LOOK) {
      long st = 0;
      d->match_len = longest(d, head, &st);
      if (d->match_len > d->prev_len) d->match_start = st;
      else d->match_len = MINM - 1;
      if (d->match_len == MINM && d->pos - d->match_start > 4096) d->match_len = MINM - 1;
      if (d->match_len > d->wend - d->pos) d->match_len = d->wend - d->pos;
    }
    if (d->prev_len >= MINM && d->match_len <= d->prev_len) {
      long end = d->pos - 1 + d->prev_len, p;
      emit_match(d, (int)d->prev_len, (int)(d->pos - 1 - d->prev_match));
      for (p = d->pos + 1; p < end; p++)
        if (d->wend - p >= MINM) insert(d, p);
      d->pos = end;
      d->match_avail = 0;
      d->match_len = MINM - 1;
    }
    else if (d->match_avail) {
      emit_lit(d, d->win[d->pos - 1]);
      d->pos++;
    }
    else {
      d->match_avail = 1;
      d->pos++;
    }
  }
  if (finish && d->match_avail) {
    emit_lit(d, d->win[d->pos - 1]);
    d->match_avail = 0;
  }
}


static void slide (Deflate *d) {
  int i;
  memmove(d->win, d->win + WSIZE, (size_t)(d->wend - WSIZE));
  d->wend -= WSIZE;
  d->pos -= WSIZE;
  d->match_start -= WSIZE;
  d->prev_match -= WSIZE;
  for (i = 0; i < HSIZE; i++) d->head[i] = d->head[i] >= WSIZE ? d->head[i] - WSIZE : -1;
  for (i = 0; i < WSIZE; i++) d->prev[i] = d->prev[i] >= WSIZE ? d->prev[i] - WSIZE : -1;
}


Deflate *deflate_new (int level, SinkFn sink, void *ctx) {
  Deflate *d = (Deflate *)xmalloc(sizeof(Deflate));
  int i;
  memset(d, 0, sizeof(*d));
  for (i = 0; i < HSIZE; i++) d->head[i] = -1;
  for (i = 0; i < WSIZE; i++) d->prev[i] = -1;
  d->match_len = d->prev_len = MINM - 1;
  d->sink = sink;
  d->ctx = ctx;
  if (level <= 3) { d->chain = 8; d->lazy = 4; d->nice = 32; d->good = 4; }
  else if (level <= 6) { d->chain = 128; d->lazy = 16; d->nice = 128; d->good = 8; }
  else { d->chain = 4096; d->lazy = 258; d->nice = 258; d->good = 32; }
  return d;
}


int deflate_write (Deflate *d, const void *data, size_t n) {
  const unsigned char *p = (const unsigned char *)data;
  while (n > 0 && !d->failed) {
    size_t room = (size_t)(2 * WSIZE - d->wend), k = n < room ? n : room;
    memcpy(d->win + d->wend, p, k);
    d->wend += (long)k;
    p += k;
    n -= k;
    d_compress(d, 0);
    if (d->wend == 2 * WSIZE && d->pos >= WSIZE + 0) slide(d);
    else if (d->wend == 2 * WSIZE) {
      d_compress(d, 0);
      if (d->pos >= WSIZE) slide(d);
    }
  }
  return d->failed ? -1 : 0;
}


int deflate_end (Deflate *d) {
  int r;
  d_compress(d, 1);
  flush_block(d, 1);
  align_byte(d);
  d_flush_out(d);
  r = d->failed ? -1 : 0;
  free(d);
  return r;
}

/* }================================================================== */


/*
** {==================================================================
** inflate
** ===================================================================
*/

typedef struct Huff {
  short count[16];
  short symbol[288];
  short fast[512];	/* 9 bits: symbol << 4 | length; -1: the long way */
} Huff;

typedef struct Inflate {
  ISrc *src;
  unsigned long long bitbuf;
  int bitcnt;
  unsigned char win[WSIZE];
  unsigned wpos;
  unsigned char out[65536];
  size_t outn;
  SinkFn sink;
  void *ctx;
  int err;
} Inflate;


long isrc_fill (ISrc *s) {
  long n;
  if (s->eof) return 0;
  if (s->pos > 16) {	/* keep a few bytes to give back */
    memmove(s->buf, s->buf + s->pos - 16, s->len - s->pos + 16);
    s->len -= s->pos - 16;
    s->pos = 16;
  }
  if (s->len >= sizeof(s->buf)) return 1;
  n = s->fn(s->ctx, s->buf + s->len, sizeof(s->buf) - s->len);
  if (n <= 0) {
    s->eof = 1;
    return 0;
  }
  s->len += (size_t)n;
  s->total += (unsigned long long)n;
  return n;
}


int isrc_byte (ISrc *s) {
  if (s->pos >= s->len && isrc_fill(s) <= 0) return -1;
  return s->buf[s->pos++];
}


/* as many whole bytes as fit in the bit buffer, straight from the source's buffer */
static void refill (Inflate *z) {
  ISrc *s = z->src;
  while (z->bitcnt <= 56) {
    if (s->pos >= s->len && isrc_fill(s) <= 0) return;
    z->bitbuf |= (unsigned long long)s->buf[s->pos++] << z->bitcnt;
    z->bitcnt += 8;
  }
}


static int need (Inflate *z, int n) {
  if (z->bitcnt < n) {
    refill(z);
    if (z->bitcnt < n) {
      z->err = 1;
      return -1;
    }
  }
  return 0;
}


static unsigned bits (Inflate *z, int n) {
  unsigned v;
  if (n == 0) return 0;
  if (need(z, n) != 0) return 0;
  v = (unsigned)(z->bitbuf & ((1ULL << n) - 1));
  z->bitbuf >>= n;
  z->bitcnt -= n;
  return v;
}


static void z_flush (Inflate *z) {
  if (z->outn > 0 && !z->err && z->sink(z->ctx, z->out, z->outn) != 0) z->err = 2;
  z->outn = 0;
}


static void z_put (Inflate *z, unsigned char c) {
  z->win[z->wpos++ & WMASK] = c;
  z->out[z->outn++] = c;
  if (z->outn == sizeof(z->out)) z_flush(z);
}


static int huff_build (Huff *h, const unsigned char *len, int n) {
  short offs[16];
  int i, left = 1;
  memset(h->count, 0, sizeof(h->count));
  for (i = 0; i < n; i++) h->count[len[i]]++;
  if (h->count[0] == n) return 0;
  for (i = 1; i < 16; i++) {
    left <<= 1;
    left -= h->count[i];
    if (left < 0) return -1;	/* too many codes */
  }
  offs[1] = 0;
  for (i = 1; i < 15; i++) offs[i + 1] = (short)(offs[i] + h->count[i]);
  for (i = 0; i < n; i++)
    if (len[i]) h->symbol[offs[len[i]]++] = (short)i;
  /* the fast table */
  for (i = 0; i < 512; i++) h->fast[i] = -1;
  {
    int code = 0, first = 0, index = 0, l;
    for (l = 1; l <= 9; l++) {
      int cnt = h->count[l], k;
      for (k = 0; k < cnt; k++) {
        int c = code + k, rev = (int)reverse_bits((unsigned)c, l), fill;
        for (fill = rev; fill < 512; fill += 1 << l) h->fast[fill] = (short)((h->symbol[index + k] << 4) | l);
      }
      index += cnt;
      first += cnt;
      code = (code + cnt) << 1;
    }
    (void)first;
  }
  return left;
}


static int decode (Inflate *z, const Huff *h) {
  int code = 0, first = 0, index = 0, l;
  if (z->bitcnt < 15) refill(z);	/* the stream may end sooner */
  if (z->bitcnt >= 9) {
    int f = h->fast[z->bitbuf & 511];
    if (f >= 0) {
      z->bitbuf >>= f & 15;
      z->bitcnt -= f & 15;
      return f >> 4;
    }
  }
  for (l = 1; l < 16; l++) {	/* the long way: bit by bit */
    int cnt;
    code |= (int)bits(z, 1);
    if (z->err) return -1;
    cnt = h->count[l];
    if (code - cnt < first) return h->symbol[index + (code - first)];
    index += cnt;
    first += cnt;
    first <<= 1;
    code <<= 1;
  }
  z->err = 1;
  return -1;
}


static int codes (Inflate *z, const Huff *lh, const Huff *dh) {
  for (;;) {
    int sym = decode(z, lh);
    if (sym < 0 || z->err) return -1;
    if (sym < 256) {
      z->win[z->wpos++ & WMASK] = (unsigned char)sym;
      z->out[z->outn++] = (unsigned char)sym;
      if (z->outn == sizeof(z->out)) z_flush(z);
    }
    else if (sym == 256) return 0;
    else {
      int len, dist, k;
      unsigned from;
      sym -= 257;
      if (sym >= 29) return -1;
      len = len_base[sym] + (int)bits(z, len_extra[sym]);
      k = decode(z, dh);
      if (k < 0 || k >= 30) return -1;
      dist = dist_base[k] + (int)bits(z, dist_extra[k]);
      if ((unsigned)dist > z->wpos) return -1;	/* before the start */
      from = z->wpos - (unsigned)dist;
      if (z->outn + (size_t)len > sizeof(z->out)) z_flush(z);
      while (len-- > 0) {
        unsigned char c = z->win[from++ & WMASK];
        z->win[z->wpos++ & WMASK] = c;
        z->out[z->outn++] = c;
      }
      if (z->outn == sizeof(z->out)) z_flush(z);
    }
    if (z->err) return -1;
  }
}


/* inflates one deflate stream from src into sink: 0 ok, -1 bad data, -2 sink */
int inflate_stream (ISrc *src, SinkFn sink, void *ctx) {
  Inflate *z = (Inflate *)xmalloc(sizeof(Inflate));
  int last, r = 0;
  Huff *lh = (Huff *)xmalloc(sizeof(Huff)), *dh = (Huff *)xmalloc(sizeof(Huff));
  memset(z, 0, sizeof(*z));
  z->src = src;
  z->sink = sink;
  z->ctx = ctx;
  do {
    int type;
    last = (int)bits(z, 1);
    type = (int)bits(z, 2);
    if (z->err) { r = -1; break; }
    if (type == 0) {	/* stored */
      unsigned len, nlen;
      z->bitbuf >>= z->bitcnt & 7;
      z->bitcnt -= z->bitcnt & 7;
      len = bits(z, 16);
      nlen = bits(z, 16);
      if (z->err || len != (~nlen & 0xFFFF)) { r = -1; break; }
      while (len-- > 0) {
        int c;
        if (z->bitcnt >= 8) {
          c = (int)(z->bitbuf & 0xFF);
          z->bitbuf >>= 8;
          z->bitcnt -= 8;
        }
        else c = isrc_byte(src);
        if (c < 0) { r = -1; break; }
        z_put(z, (unsigned char)c);
      }
      if (r) break;
    }
    else if (type == 1) {	/* fixed codes */
      unsigned char l[288];
      int i;
      for (i = 0; i < 144; i++) l[i] = 8;
      for (; i < 256; i++) l[i] = 9;
      for (; i < 280; i++) l[i] = 7;
      for (; i < 288; i++) l[i] = 8;
      huff_build(lh, l, 288);
      for (i = 0; i < 30; i++) l[i] = 5;
      huff_build(dh, l, 30);
      if (codes(z, lh, dh) != 0) { r = -1; break; }
    }
    else if (type == 2) {	/* dynamic */
      unsigned char l[320];
      int nlen = (int)bits(z, 5) + 257, ndist = (int)bits(z, 5) + 1, ncode = (int)bits(z, 4) + 4, i;
      Huff ch;
      if (z->err || nlen > 286 || ndist > 30) { r = -1; break; }
      memset(l, 0, 19);
      for (i = 0; i < ncode; i++) l[cl_order[i]] = (unsigned char)bits(z, 3);
      if (huff_build(&ch, l, 19) != 0) { r = -1; break; }
      for (i = 0; i < nlen + ndist;) {
        int sym = decode(z, &ch), rep, val = 0;
        if (sym < 0) { r = -1; break; }
        if (sym < 16) {
          l[i++] = (unsigned char)sym;
          continue;
        }
        if (sym == 16) {
          if (i == 0) { r = -1; break; }
          val = l[i - 1];
          rep = 3 + (int)bits(z, 2);
        }
        else if (sym == 17) rep = 3 + (int)bits(z, 3);
        else rep = 11 + (int)bits(z, 7);
        if (i + rep > nlen + ndist) { r = -1; break; }
        while (rep-- > 0) l[i++] = (unsigned char)val;
      }
      if (r) break;
      if (l[256] == 0) { r = -1; break; }
      {
        int e1 = huff_build(lh, l, nlen), e2 = huff_build(dh, l + nlen, ndist);
        if (e1 < 0 || (e1 > 0 && nlen - lh->count[0] != 1) || e2 < 0 ||
            (e2 > 0 && ndist - dh->count[0] != 1)) { r = -1; break; }
      }
      if (codes(z, lh, dh) != 0) { r = -1; break; }
    }
    else { r = -1; break; }
    if (z->err == 2) break;
  } while (!last);
  z_flush(z);
  if (z->err == 2) r = -2;
  /* whole bytes read ahead go back to the source */
  if (r == 0) {
    int back = z->bitcnt / 8;
    src->pos = src->pos >= (size_t)back ? src->pos - (size_t)back : 0;
  }
  free(lh);
  free(dh);
  free(z);
  return r;
}

/* }================================================================== */


/*
** {==================================================================
** Sources and sinks over descriptors
** ===================================================================
*/

long fd_source (void *ctx, unsigned char *p, size_t n) {
  return os_read(*(int *)ctx, p, n);
}


int fd_sink (void *ctx, const unsigned char *p, size_t n) {
  return os_write(*(int *)ctx, p, n) < 0 ? -1 : 0;
}


void isrc_init (ISrc *s, SourceFn fn, void *ctx) {
  s->fn = fn;
  s->ctx = ctx;
  s->pos = s->len = 0;
  s->eof = 0;
  s->total = 0;
}


/* gzip, as a sink: the header, deflate, CRC and size at the end */
typedef struct GzOut {
  Deflate *d;
  SinkFn sink;
  void *ctx;
  unsigned long crc;
  unsigned long long size;
} GzOut;


GzOut *gz_open (SinkFn sink, void *ctx, int level, const char *name, time_t mtime) {
  GzOut *g = (GzOut *)xmalloc(sizeof(GzOut));
  unsigned char h[10];
  h[0] = 0x1f;
  h[1] = 0x8b;
  h[2] = 8;
  h[3] = name ? 8 : 0;
  h[4] = (unsigned char)mtime;
  h[5] = (unsigned char)(mtime >> 8);
  h[6] = (unsigned char)(mtime >> 16);
  h[7] = (unsigned char)(mtime >> 24);
  h[8] = level >= 9 ? 2 : level <= 1 ? 4 : 0;
  h[9] = 3;	/* Unix */
  g->sink = sink;
  g->ctx = ctx;
  g->crc = 0;
  g->size = 0;
  sink(ctx, h, 10);
  if (name) sink(ctx, (const unsigned char *)name, strlen(name) + 1);
  g->d = deflate_new(level, sink, ctx);
  return g;
}


int gz_write (void *gv, const unsigned char *p, size_t n) {
  GzOut *g = (GzOut *)gv;
  g->crc = crc32_update(g->crc, p, n);
  g->size += n;
  return deflate_write(g->d, p, n);
}


int gz_close (GzOut *g) {
  unsigned char t[8];
  int r = deflate_end(g->d);
  unsigned long s = (unsigned long)(g->size & 0xFFFFFFFFULL);
  t[0] = (unsigned char)g->crc;
  t[1] = (unsigned char)(g->crc >> 8);
  t[2] = (unsigned char)(g->crc >> 16);
  t[3] = (unsigned char)(g->crc >> 24);
  t[4] = (unsigned char)s;
  t[5] = (unsigned char)(s >> 8);
  t[6] = (unsigned char)(s >> 16);
  t[7] = (unsigned char)(s >> 24);
  if (r == 0 && g->sink(g->ctx, t, 8) != 0) r = -1;
  free(g);
  return r;
}


/* gunzip: every member of src into sink; 0 ok, -1 not gzip, -2 bad data,
** -3 a write failed. first: the magic was checked already (2 bytes eaten) */
typedef struct CrcSink {
  SinkFn sink;
  void *ctx;
  unsigned long crc;
  unsigned long long size;
} CrcSink;


static int crc_sink (void *cv, const unsigned char *p, size_t n) {
  CrcSink *c = (CrcSink *)cv;
  c->crc = crc32_update(c->crc, p, n);
  c->size += n;
  return c->sink(c->ctx, p, n);
}


int gunzip_stream (ISrc *src, SinkFn sink, void *ctx) {
  int members = 0;
  for (;;) {
    int a = isrc_byte(src), b, flags, k, r;
    CrcSink cs;
    unsigned long crc = 0, size = 0;
    if (a < 0) return members > 0 ? 0 : -1;
    b = isrc_byte(src);
    if (a != 0x1f || b != 0x8b) {
      if (members > 0 && a == 0) return 0;	/* padding after the data */
      return members > 0 ? 0 : -1;
    }
    if (isrc_byte(src) != 8) return -2;
    flags = isrc_byte(src);
    for (k = 0; k < 6; k++) isrc_byte(src);
    if (flags & 4) {	/* extra */
      int lo = isrc_byte(src), hi = isrc_byte(src), n = lo | (hi << 8);
      while (n-- > 0) isrc_byte(src);
    }
    if (flags & 8) while ((a = isrc_byte(src)) > 0) {}
    if (flags & 16) while ((a = isrc_byte(src)) > 0) {}
    if (flags & 2) {
      isrc_byte(src);
      isrc_byte(src);
    }
    cs.sink = sink;
    cs.ctx = ctx;
    cs.crc = 0;
    cs.size = 0;
    r = inflate_stream(src, crc_sink, &cs);
    if (r == -2) return -3;
    if (r != 0) return -2;
    for (k = 0; k < 4; k++) crc |= (unsigned long)(isrc_byte(src) & 0xFF) << (8 * k);
    for (k = 0; k < 4; k++) size |= (unsigned long)(isrc_byte(src) & 0xFF) << (8 * k);
    if (crc != cs.crc || size != (unsigned long)(cs.size & 0xFFFFFFFFULL)) return -2;
    members++;
  }
}

/* }================================================================== */


/*
** {==================================================================
** gzip, gunzip, zcat
** ===================================================================
*/

static int gz_one (const char *name, int decompress, int to_stdout, int keep, int force,
                   int level, int verbose, const char *suffix, int in, int out, int err) {
  char *native = NULL, *target = NULL, *tn = NULL;
  int ifd, ofd, r = 0;
  OsStat st;
  memset(&st, 0, sizeof(st));
  if (strcmp(name, "-") == 0) {
    ifd = in;
    to_stdout = 1;
  }
  else {
    size_t nl = strlen(name), sl = strlen(suffix);
    native = path_to_native(name);
    if (os_stat(native, &st) != 0) {
      tool_err(err, "gzip", "%s: %s", name, os_errmsg());
      free(native);
      return 1;
    }
    if (st.is_dir) {
      tool_err(err, "gzip", "%s: is a directory -- ignored", name);
      free(native);
      return 2;
    }
    if (decompress) {
      if (nl > sl && strcmp(name + nl - sl, suffix) == 0) target = xstrndup(name, nl - sl);
      else if (nl > 4 && strcmp(name + nl - 4, ".tgz") == 0) target = xstrcat3(xstrndup(name, nl - 4), ".tar", "");
      else if (!to_stdout) {
        tool_err(err, "gzip", "%s: unknown suffix -- ignored", name);
        free(native);
        return 2;
      }
    }
    else {
      if (nl > sl && strcmp(name + nl - sl, suffix) == 0 && !to_stdout && !force) {
        tool_err(err, "gzip", "%s already has %s suffix -- unchanged", name, suffix);
        free(native);
        return 2;
      }
      target = xstrcat3(name, suffix, "");
    }
    ifd = os_open(native, OS_READ);
    if (ifd < 0) {
      tool_err(err, "gzip", "%s: %s", name, os_errmsg());
      free(native);
      free(target);
      return 1;
    }
  }
  if (to_stdout) ofd = out;
  else {
    OsStat ts;
    tn = path_to_native(target);
    if (os_stat(tn, &ts) == 0 && !force) {
      tool_err(err, "gzip", "%s already exists; not overwritten", target);
      os_close(ifd);
      free(native);
      free(target);
      free(tn);
      return 2;
    }
    ofd = os_open(tn, OS_WRITE);
    if (ofd < 0) {
      tool_err(err, "gzip", "%s: %s", target, os_errmsg());
      os_close(ifd);
      free(native);
      free(target);
      free(tn);
      return 1;
    }
  }
  if (decompress) {
    ISrc src;
    int g;
    isrc_init(&src, fd_source, &ifd);
    g = gunzip_stream(&src, fd_sink, &ofd);
    if (g == -1) {
      tool_err(err, "gzip", "%s: not in gzip format", name);
      r = 1;
    }
    else if (g == -2) {
      tool_err(err, "gzip", "%s: invalid compressed data--format violated", name);
      r = 1;
    }
    else if (g == -3) r = 1;
  }
  else {
    GzOut *gz = gz_open(fd_sink, &ofd, level, native ? tool_base(name) : NULL, native ? st.mtime : 0);
    unsigned char buf[65536];
    long n;
    unsigned long long total = 0;
    while ((n = os_read(ifd, buf, sizeof(buf))) > 0) {
      gz_write(gz, buf, (size_t)n);
      total += (unsigned long long)n;
      if (tool_stop()) break;
    }
    if (gz_close(gz) != 0) r = 1;
    if (verbose && native) {
      long long after = os_seek(ofd, 0, 1);
      fd_printf(err, "%s:\t%5.1f%% -- replaced with %s\n", name,
                total ? 100.0 - (double)after * 100.0 / (double)total : 0.0, target);
    }
  }
  if (ifd != in) os_close(ifd);
  if (ofd != out) {
    os_close(ofd);
    if (r == 0 && native) {
      os_utime(tn, st.atime, st.mtime);
      os_chmod(tn, st.mode);
    }
    if (r != 0) os_unlink(tn);
    else if (!keep && native) os_unlink(native);
  }
  free(native);
  free(target);
  free(tn);
  return r;
}


static int gzip_main (int argc, char **argv, int in, int out, int err, int mode) {
  static const LongOpt lo[] = {{"stdout", 'c', 0}, {"to-stdout", 'c', 0}, {"decompress", 'd', 0},
    {"uncompress", 'd', 0}, {"force", 'f', 0}, {"keep", 'k', 0}, {"quiet", 'q', 0},
    {"verbose", 'v', 0}, {"suffix", 'S', 1}, {"fast", '1', 0}, {"best", '9', 0},
    {"recursive", 'r', 0}, {"no-name", 'n', 0}, {"name", 'N', 0}, {"test", 't', 0}, {NULL, 0, 0}};
  Opts g;
  int c, decompress = mode != 0, to_stdout = mode == 'c', keep = 0, force = 0, level = 6,
      verbose = 0, status = 0;
  const char *suffix = ".gz";
  size_t i;
  opts_init(&g, mode == 'c' ? "zcat" : mode == 'd' ? "gunzip" : "gzip", argc, argv, err);
  while ((c = opts_next(&g, "cdfkqvS:123456789rnNt", lo)) != 0) {
    switch (c) {
      case 'c': to_stdout = 1; break;
      case 'd': decompress = 1; break;
      case 'f': force = 1; break;
      case 'k': keep = 1; break;
      case 'q': verbose = 0; break;
      case 'v': verbose = 1; break;
      case 'S': suffix = g.arg; break;
      case 't': decompress = 1; to_stdout = 1; break;
      case 'r': case 'n': case 'N': break;
      case OPT_HELP: opts_free(&g); return tool_help(out, mode == 'c' ? "zcat" : mode == 'd' ? "gunzip" : "gzip");
      default:
        if (c >= '1' && c <= '9') {
          level = c - '0';
          break;
        }
        opts_free(&g);
        return 1;
    }
  }
  if (g.ops.n == 0) {
    if (!decompress && os_is_tty(out) && !force) {
      tool_err(err, "gzip", "compressed data not written to a terminal. Use -f to force compression.");
      opts_free(&g);
      return 1;
    }
    vec_push(&g.ops, xstrdup("-"));
  }
  for (i = 0; i < g.ops.n && !tool_stop(); i++) {
    int r = gz_one(g.ops.v[i], decompress, to_stdout, keep, force, level, verbose, suffix, in, out, err);
    if (r > status) status = r;
  }
  opts_free(&g);
  return status;
}


int t_gzip (int argc, char **argv, int in, int out, int err) {
  return gzip_main(argc, argv, in, out, err, 0);
}


int t_gunzip (int argc, char **argv, int in, int out, int err) {
  return gzip_main(argc, argv, in, out, err, 'd');
}


int t_zcat (int argc, char **argv, int in, int out, int err) {
  return gzip_main(argc, argv, in, out, err, 'c');
}

/* }================================================================== */


/*
** {==================================================================
** tar
** ===================================================================
*/

typedef struct Tar {
  int create, extract, list, verbose, gz, to_stdout, keep_old, no_same_owner, strip;
  int status, absolute, overwrite_dir;
  const char *file;
  const char *dir;	/* -C */
  Vec exclude;
  Out *o;
  int in, out, err;
  /* writing */
  int fd;
  GzOut *gzo;
  unsigned long long written;
  /* reading */
  ISrc src;
  int gz_pipe[2];
} Tar;


static int tar_sink (void *tv, const unsigned char *p, size_t n) {
  Tar *t = (Tar *)tv;
  t->written += n;
  if (t->gzo) return gz_write(t->gzo, p, n);
  return os_write(t->fd, p, n) < 0 ? -1 : 0;
}


static void octal (char *dst, int width, unsigned long long v) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%0*llo", width - 1, v);
  if ((int)strlen(buf) > width - 1) {	/* too big: base-256, GNU style */
    int k;
    memset(dst, 0, (size_t)width);
    dst[0] = (char)0x80;
    for (k = width - 1; k > 0 && v; k--) {
      dst[k] = (char)(v & 0xFF);
      v >>= 8;
    }
    return;
  }
  memcpy(dst, buf, (size_t)width - 1);
  dst[width - 1] = '\0';
}


static void tar_header (Tar *t, const char *name, char type, const OsStat *st, unsigned long long size,
                        const char *link) {
  unsigned char h[512];
  unsigned sum = 0;
  int k;
  size_t nl = strlen(name);
  char *un, *gn;
  if (nl > 100 || (link && strlen(link) > 100)) {	/* GNU long name: a ././@LongLink entry first */
    const char *longs[2];
    char types[2];
    int j;
    longs[0] = nl > 100 ? name : NULL;
    types[0] = 'L';
    longs[1] = (link && strlen(link) > 100) ? link : NULL;
    types[1] = 'K';
    for (j = 0; j < 2; j++) {
      size_t ln, pad;
      unsigned char z[512];
      if (!longs[j]) continue;
      ln = strlen(longs[j]) + 1;
      memset(h, 0, 512);
      strcpy((char *)h, "././@LongLink");
      octal((char *)h + 100, 8, 0644);
      octal((char *)h + 108, 8, 0);
      octal((char *)h + 116, 8, 0);
      octal((char *)h + 124, 12, ln);
      octal((char *)h + 136, 12, 0);
      h[156] = (unsigned char)types[j];
      memcpy(h + 257, "ustar  ", 8);
      memset(h + 148, ' ', 8);
      for (sum = 0, k = 0; k < 512; k++) sum += h[k];
      snprintf((char *)h + 148, 8, "%06o", sum);
      h[155] = ' ';
      tar_sink(t, h, 512);
      tar_sink(t, (const unsigned char *)longs[j], ln);
      pad = (512 - ln % 512) % 512;
      memset(z, 0, sizeof(z));
      if (pad) tar_sink(t, z, pad);
    }
  }
  memset(h, 0, 512);
  memcpy(h, name, nl > 100 ? 100 : nl);
  octal((char *)h + 100, 8, st->mode & 07777);
  octal((char *)h + 108, 8, (unsigned long long)(st->uid > 0 ? st->uid : 0));
  octal((char *)h + 116, 8, (unsigned long long)(st->gid > 0 ? st->gid : 0));
  octal((char *)h + 124, 12, size);
  octal((char *)h + 136, 12, (unsigned long long)(st->mtime > 0 ? st->mtime : 0));
  h[156] = (unsigned char)type;
  if (link) memcpy(h + 157, link, strlen(link) > 100 ? 100 : strlen(link));
  memcpy(h + 257, "ustar  ", 8);	/* the GNU format, as GNU tar writes by default */
  un = os_user_name(st->uid);
  gn = os_group_name(st->gid);
  strncpy((char *)h + 265, un, 31);
  strncpy((char *)h + 297, gn, 31);
  free(un);
  free(gn);
  memset(h + 148, ' ', 8);
  for (sum = 0, k = 0; k < 512; k++) sum += h[k];
  snprintf((char *)h + 148, 8, "%06o", sum);
  h[155] = ' ';
  tar_sink(t, h, 512);
}


static int tar_excluded (Tar *t, const char *name) {
  size_t i;
  for (i = 0; i < t->exclude.n; i++)
    if (pat_match(t->exclude.v[i], name, 0) || pat_match(t->exclude.v[i], tool_base(name), 0)) return 1;
  return 0;
}


static void tar_add (Tar *t, const char *disp, const char *native) {
  OsStat st;
  char *name;
  const char *nm = disp;
  if (tool_stop()) return;
  if (tar_excluded(t, disp)) return;
  if (os_lstat(native, &st) != 0) {
    out_flush(t->o);
    tool_err(t->err, "tar", "%s: Cannot stat: %s", disp, os_errmsg());
    t->status = 2;
    return;
  }
  /* member names are relative: no leading / (unless -P) */
  if (!t->absolute) {
    int warned = 0;
    while (*nm == '/') {
      nm++;
      warned = 1;
    }
    if (warned) {
      static int once = 0;
      if (!once) tool_err(t->err, "tar", "Removing leading `/' from member names");
      once = 1;
    }
    if (*nm == '\0') nm = ".";
  }
  if (t->verbose) out_printf(t->o, "%s%s\n", nm, st.is_dir && !st.is_link ? "/" : "");
  if (st.is_link) {
    char *target = os_readlink(native);
    tar_header(t, nm, '2', &st, 0, target ? target : "");
    free(target);
    return;
  }
  if (st.is_dir) {
    Vec names;
    size_t i;
    name = xstrcat3(nm, nm[strlen(nm) - 1] == '/' ? "" : "/", "");
    tar_header(t, name, '5', &st, 0, NULL);
    free(name);
    vec_init(&names);
    os_listdir(native, &names);
    vec_sort(&names);
    for (i = 0; i < names.n; i++) {
      char *cd = tool_join(disp, names.v[i]), *cn = path_join(native, names.v[i]);
      tar_add(t, cd, cn);
      free(cd);
      free(cn);
    }
    vec_free(&names);
    return;
  }
  {
    int fd = os_open(native, OS_READ);
    unsigned char buf[65536];
    long n;
    unsigned long long done = 0;
    if (fd < 0) {
      out_flush(t->o);
      tool_err(t->err, "tar", "%s: Cannot open: %s", disp, os_errmsg());
      t->status = 2;
      return;
    }
    tar_header(t, nm, '0', &st, (unsigned long long)st.size, NULL);
    while (done < (unsigned long long)st.size && (n = os_read(fd, buf, sizeof(buf))) > 0) {
      if (done + (unsigned long long)n > (unsigned long long)st.size) n = (long)((unsigned long long)st.size - done);
      tar_sink(t, buf, (size_t)n);
      done += (unsigned long long)n;
    }
    os_close(fd);
    if (done < (unsigned long long)st.size) {	/* the file shrank: pad with zeros */
      memset(buf, 0, sizeof(buf));
      tool_err(t->err, "tar", "%s: File shrank by %llu bytes; padding with zeros", disp,
               (unsigned long long)st.size - done);
      while (done < (unsigned long long)st.size) {
        unsigned long long k = (unsigned long long)st.size - done;
        if (k > sizeof(buf)) k = sizeof(buf);
        tar_sink(t, buf, (size_t)k);
        done += k;
      }
      t->status = 1;
    }
    if (st.size % 512) {
      memset(buf, 0, 512);
      tar_sink(t, buf, (size_t)(512 - st.size % 512));
    }
  }
}


static unsigned long long get_octal (const unsigned char *p, int width) {
  unsigned long long v = 0;
  int k = 0;
  if (p[0] & 0x80) {	/* base-256 */
    v = p[0] & 0x7F;
    for (k = 1; k < width; k++) v = (v << 8) | p[k];
    return v;
  }
  while (k < width && (p[k] == ' ' || p[k] == 0)) k++;
  for (; k < width && p[k] >= '0' && p[k] <= '7'; k++) v = v * 8 + (unsigned long long)(p[k] - '0');
  return v;
}


static int tar_read (Tar *t, void *dst, size_t n) {
  unsigned char *p = (unsigned char *)dst;
  while (n > 0) {
    int c;
    if (t->src.pos < t->src.len) {
      size_t k = t->src.len - t->src.pos;
      if (k > n) k = n;
      memcpy(p, t->src.buf + t->src.pos, k);
      t->src.pos += k;
      p += k;
      n -= k;
      continue;
    }
    c = isrc_byte(&t->src);
    if (c < 0) return -1;
    *p++ = (unsigned char)c;
    n--;
  }
  return 0;
}


/* a member name made safe: no leading /, no ".." parts; NULL: skip it */
static char *safe_name (Tar *t, const char *name) {
  const char *p = name;
  int k;
  Buf b;
  if (!t->absolute) {
    while (*p == '/') p++;
  }
  /* --strip-components */
  for (k = 0; k < t->strip; k++) {
    const char *s = strchr(p, '/');
    if (s == NULL) return NULL;
    p = s + 1;
    while (*p == '/') p++;
  }
  if (*p == '\0') return NULL;
  buf_init(&b);
  buf_puts(&b, p);
  if (!t->absolute) {	/* a ".." part could climb out */
    const char *q = b.s;
    while (q && *q) {
      if (q[0] == '.' && q[1] == '.' && (q[2] == '/' || q[2] == '\0')) {
        tool_err(t->err, "tar", "%s: Member name contains '..'", name);
        buf_free(&b);
        t->status = 2;
        return NULL;
      }
      q = strchr(q, '/');
      if (q) q++;
    }
  }
  return buf_take(&b);
}


static void tar_list_line (Tar *t, const unsigned char *h, const char *name, unsigned long long size,
                           const char *link) {
  char type = (char)h[156];
  if (t->verbose) {
    OsStat st;
    char mode[11], when[32];
    time_t mt = (time_t)get_octal(h + 136, 12);
    struct tm *tm = localtime(&mt);
    char user[40], group[40];
    memset(&st, 0, sizeof(st));
    st.mode = (unsigned)get_octal(h + 100, 8);
    st.is_dir = type == '5';
    st.is_link = type == '2';
    mode_string(mode, &st);
    if (type == '1') mode[0] = 'h';
    snprintf(user, sizeof(user), "%.32s", h[265] ? (const char *)h + 265 : "");
    snprintf(group, sizeof(group), "%.32s", h[297] ? (const char *)h + 297 : "");
    if (!user[0]) snprintf(user, sizeof(user), "%llu", get_octal(h + 108, 8));
    if (!group[0]) snprintf(group, sizeof(group), "%llu", get_octal(h + 116, 8));
    if (tm) strftime(when, sizeof(when), "%Y-%m-%d %H:%M", tm);
    else strcpy(when, "?");
    out_printf(t->o, "%s %s/%s %*llu %s %s", mode, user, group,
               (int)(19 - strlen(user) - strlen(group)) > 0 ? (int)(19 - strlen(user) - strlen(group)) : 1,
               size, when, name);
    if (type == '2') out_printf(t->o, " -> %s", link);
    if (type == '1') out_printf(t->o, " link to %s", link);
    out_putc(t->o, '\n');
  }
  else out_printf(t->o, "%s\n", name);
}


/* reads the archive: lists or extracts */
static void tar_extract (Tar *t, Vec *members) {
  unsigned char h[512];
  char *longname = NULL, *longlink = NULL;
  int zeros = 0;
  unsigned long long pax_size = 0;
  int have_pax_size = 0;
  for (;;) {
    char name[512], link[512];
    unsigned long long size;
    char type;
    char *path;
    unsigned sum = 0, stored;
    int k, want = 1;
    if (tool_stop()) break;
    if (tar_read(t, h, 512) != 0) {
      if (zeros == 0) {
        tool_err(t->err, "tar", "Unexpected EOF in archive");
        t->status = 2;
      }
      break;
    }
    for (k = 0; k < 512 && h[k] == 0; k++) {}
    if (k == 512) {
      if (++zeros == 2) break;
      continue;
    }
    zeros = 0;
    stored = (unsigned)get_octal(h + 148, 8);
    for (k = 0; k < 512; k++) sum += (k >= 148 && k < 156) ? ' ' : h[k];
    if (sum != stored) {
      tool_err(t->err, "tar", "This does not look like a tar archive");
      t->status = 2;
      break;
    }
    type = (char)h[156];
    size = get_octal(h + 124, 12);
    if (have_pax_size) {
      size = pax_size;
      have_pax_size = 0;
    }
    if (type == 'L' || type == 'K' || type == 'x' || type == 'g') {	/* data for the next header */
      char *data = (char *)xmalloc((size_t)size + 1);
      size_t pad = (size_t)((512 - size % 512) % 512);
      unsigned char skip[512];
      if (tar_read(t, data, (size_t)size) != 0 || (pad && tar_read(t, skip, pad) != 0)) {
        free(data);
        tool_err(t->err, "tar", "Unexpected EOF in archive");
        t->status = 2;
        break;
      }
      data[size] = '\0';
      if (type == 'L') {
        free(longname);
        longname = data;
      }
      else if (type == 'K') {
        free(longlink);
        longlink = data;
      }
      else {	/* pax: "len key=value\n" records */
        char *p = data, *e = data + size;
        while (p < e) {
          char *sp = strchr(p, ' ');
          long len = atol(p);
          if (sp == NULL || len <= 0 || p + len > e) break;
          if (type == 'x') {
            char *kv = sp + 1, *eq = strchr(kv, '='), *end = p + len - 1;
            if (eq && eq < end) {
              *eq = '\0';
              *end = '\0';
              if (strcmp(kv, "path") == 0) {
                free(longname);
                longname = xstrdup(eq + 1);
              }
              else if (strcmp(kv, "linkpath") == 0) {
                free(longlink);
                longlink = xstrdup(eq + 1);
              }
              else if (strcmp(kv, "size") == 0) {
                pax_size = strtoull(eq + 1, NULL, 10);
                have_pax_size = 1;
              }
            }
          }
          p += len;
        }
        free(data);
      }
      continue;
    }
    if (longname) {
      snprintf(name, sizeof(name), "%s", longname);
      free(longname);
      longname = NULL;
    }
    else if (memcmp(h + 257, "ustar", 5) == 0 && h[345]) {	/* prefix/name */
      snprintf(name, sizeof(name), "%.155s/%.100s", (const char *)h + 345, (const char *)h);
    }
    else snprintf(name, sizeof(name), "%.100s", (const char *)h);
    if (longlink) {
      snprintf(link, sizeof(link), "%s", longlink);
      free(longlink);
      longlink = NULL;
    }
    else snprintf(link, sizeof(link), "%.100s", (const char *)h + 157);
    if (members && members->n > 0) {	/* only the ones asked for (or inside them) */
      size_t i;
      want = 0;
      for (i = 0; i < members->n; i++) {
        size_t ml = strlen(members->v[i]);
        while (ml > 1 && members->v[i][ml - 1] == '/') ml--;
        if (strncmp(name, members->v[i], ml) == 0 && (name[ml] == '\0' || name[ml] == '/')) want = 1;
        else if (pat_match(members->v[i], name, 0)) want = 1;
      }
    }
    if (want && tar_excluded(t, name)) want = 0;
    path = want ? safe_name(t, name) : NULL;
    if (path == NULL) want = 0;
    if (want && t->list) tar_list_line(t, h, name, size, link);
    else if (want && t->verbose && !t->to_stdout) out_printf(t->o, "%s\n", name);
    if (want && t->extract) {
      char *dest = t->dir ? tool_join(t->dir, path) : xstrdup(path);
      char *native = path_to_native(dest), *parent = path_dirname(native);
      unsigned mode = (unsigned)get_octal(h + 100, 8) & 07777;
      time_t mtime = (time_t)get_octal(h + 136, 12);
      if (!t->to_stdout) mkdir_p(parent);
      if (type == '5') {
        if (!t->to_stdout) {
          OsStat st;
          if (mkdir_p(native) != 0 && !(os_stat(native, &st) == 0 && st.is_dir)) {
            tool_err(t->err, "tar", "%s: Cannot mkdir: %s", dest, os_errmsg());
            t->status = 2;
          }
        }
      }
      else if (type == '2' && !t->to_stdout) {
        OsStat st;
        if (os_lstat(native, &st) == 0) os_unlink(native);
        if (os_symlink(link, native, 0) != 0) {
#ifdef _WIN32
          /* no symlink rights: copy what it points to, if it is there already */
          char *tgt = path_join(parent, link), *tn = path_to_native(tgt);
          OsStat ts;
          if (os_stat(tn, &ts) == 0 && !ts.is_dir) {
            char *data;
            size_t len;
            data = read_file(tn, &len);
            if (data) {
              int fd = os_open(native, OS_WRITE);
              if (fd >= 0) {
                os_write(fd, data, len);
                os_close(fd);
              }
              free(data);
            }
          }
          else {
            tool_err(t->err, "tar", "%s: Cannot create symlink to '%s': %s", dest, link, os_errmsg());
            t->status = 2;
          }
          free(tgt);
          free(tn);
#else
          tool_err(t->err, "tar", "%s: Cannot create symlink to '%s': %s", dest, link, os_errmsg());
          t->status = 2;
#endif
        }
      }
      else if (type == '1' && !t->to_stdout) {
        char *lp = safe_name(t, link), *ld = lp ? (t->dir ? tool_join(t->dir, lp) : xstrdup(lp)) : NULL;
        char *ln = ld ? path_to_native(ld) : NULL;
        OsStat st;
        if (os_lstat(native, &st) == 0) os_unlink(native);
        if (ln == NULL || os_link(ln, native) != 0) {
          tool_err(t->err, "tar", "%s: Cannot hard link to '%s': %s", dest, link, os_errmsg());
          t->status = 2;
        }
        free(lp);
        free(ld);
        free(ln);
      }
      else if (type == '0' || type == '\0' || type == '7') {
        int fd;
        unsigned long long left = size;
        unsigned char buf[65536];
        OsStat st;
        if (t->to_stdout) fd = t->out;
        else if (t->keep_old && os_lstat(native, &st) == 0) {
          tool_err(t->err, "tar", "%s: Cannot open: File exists", dest);
          t->status = 2;
          fd = -2;
        }
        else {
          if (os_lstat(native, &st) == 0 && (st.is_link || !(st.mode & 0200))) {
            os_chmod(native, 0666);
            os_unlink(native);
          }
          fd = os_open(native, OS_WRITE);
          if (fd < 0) {
            tool_err(t->err, "tar", "%s: Cannot open: %s", dest, os_errmsg());
            t->status = 2;
          }
        }
        while (left > 0) {
          size_t k = left > sizeof(buf) ? sizeof(buf) : (size_t)left;
          if (tar_read(t, buf, k) != 0) {
            tool_err(t->err, "tar", "Unexpected EOF in archive");
            t->status = 2;
            break;
          }
          if (fd >= 0) os_write(fd, buf, k);
          left -= k;
        }
        size = 0;	/* read already */
        if (fd >= 0 && fd != t->out) {
          os_close(fd);
          os_utime(native, mtime, mtime);
          os_chmod(native, mode);
        }
      }
      if (type == '5' && !t->to_stdout) os_utime(native, mtime, mtime);
      free(dest);
      free(native);
      free(parent);
    }
    free(path);
    {	/* the data we did not read */
      unsigned long long skip = size + (512 - size % 512) % 512;
      unsigned char buf[65536];
      if (size == 0 && (type == '0' || type == '\0' || type == '7') && want && t->extract) {
        /* the file data was read: only the padding is left */
        skip = 0;
      }
      if (want && t->extract && (type == '0' || type == '\0' || type == '7')) {
        unsigned long long orig = get_octal(h + 124, 12);
        skip = (512 - orig % 512) % 512;
      }
      while (skip > 0) {
        size_t k = skip > sizeof(buf) ? sizeof(buf) : (size_t)skip;
        if (tar_read(t, buf, k) != 0) {
          skip = 0;
          break;
        }
        skip -= k;
      }
    }
  }
  free(longname);
  free(longlink);
}


/* the byte source of the archive: the file (or stdin), gunzipped on the fly */
typedef struct GzPipe {
  ISrc raw;
  int wfd;
} GzPipe;


static void gunzip_thread (void *arg) {
  GzPipe *gp = (GzPipe *)arg;
  gunzip_stream(&gp->raw, fd_sink, &gp->wfd);
  os_close(gp->wfd);
}


int t_tar (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {
    {"create", 'c', 0}, {"extract", 'x', 0}, {"get", 'x', 0}, {"list", 't', 0}, {"file", 'f', 1},
    {"verbose", 'v', 0}, {"gzip", 'z', 0}, {"gunzip", 'z', 0}, {"directory", 'C', 1},
    {"strip-components", 1001, 1}, {"exclude", 1002, 1}, {"to-stdout", 'O', 0},
    {"keep-old-files", 'k', 0}, {"absolute-names", 'P', 0}, {"no-same-owner", 1003, 0},
    {"same-permissions", 'p', 0}, {"preserve-permissions", 'p', 0}, {"auto-compress", 'a', 0},
    {"bzip2", 'j', 0}, {"xz", 'J', 0}, {"zstd", 1004, 0}, {"overwrite", 1003, 0},
    {"no-same-permissions", 1003, 0}, {"numeric-owner", 1003, 0}, {"owner", 1005, 1},
    {"group", 1005, 1}, {"mtime", 1005, 1}, {"sort", 1005, 1}, {NULL, 0, 0}};
  Tar t;
  Opts g;
  Out o;
  int c, i, nargc = argc, autoz = 0;
  char **nargv = argv, *bundle = NULL;
  memset(&t, 0, sizeof(t));
  t.in = in;
  t.out = out;
  t.err = err;
  vec_init(&t.exclude);
  /* old style: "tar czf x.tgz dir": the first word is a bundle of letters */
  if (argc > 1 && argv[1][0] != '-' && strlen(argv[1]) > 0) {
    const char *p;
    int k = 2;
    Vec nv;
    vec_init(&nv);
    vec_push(&nv, xstrdup(argv[0]));
    for (p = argv[1]; *p; p++) {
      char opt[3];
      opt[0] = '-';
      opt[1] = *p;
      opt[2] = '\0';
      vec_push(&nv, xstrdup(opt));
      if ((*p == 'f' || *p == 'C') && k < argc) vec_push(&nv, xstrdup(argv[k++]));
    }
    for (; k < argc; k++) vec_push(&nv, xstrdup(argv[k]));
    nargc = (int)nv.n;
    nargv = nv.v;
    bundle = (char *)nv.v;	/* freed below */
  }
  opts_init(&g, "tar", nargc, nargv, err);
  while ((c = opts_next(&g, "cxtf:vzC:OkPpajJ", lo)) != 0) {
    switch (c) {
      case 'c': t.create = 1; break;
      case 'x': t.extract = 1; break;
      case 't': t.list = 1; break;
      case 'f': t.file = g.arg; break;
      case 'v': t.verbose++; break;
      case 'z': t.gz = 1; break;
      case 'a': autoz = 1; break;
      case 'C': t.dir = g.arg; break;
      case 'O': t.to_stdout = 1; break;
      case 'k': t.keep_old = 1; break;
      case 'P': t.absolute = 1; break;
      case 'p': case 1003: case 1005: break;
      case 1001: t.strip = atoi(g.arg); break;
      case 1002: vec_push(&t.exclude, glob_mark(g.arg)); break;
      case 'j': case 'J': case 1004:
        tool_err(err, "tar", "only gzip (-z) compression is built in; %s is not",
                 c == 'j' ? "bzip2" : c == 'J' ? "xz" : "zstd");
        goto bad;
      case OPT_HELP: opts_free(&g); vec_free(&t.exclude); return tool_help(out, "tar");
      default: goto bad;
    }
  }
  if (t.create + t.extract + t.list != 1) {
    tool_err(err, "tar", t.create + t.extract + t.list == 0 ?
             "You must specify one of the '-Acdtrux', '--delete' or '--test-label' options" :
             "You may not specify more than one '-Acdtrux', '--delete' or  '--test-label' option");
    fd_printf(err, "Try 'tar --help' or 'tar --usage' for more information.\n");
    goto bad;
  }
  if (t.file == NULL) t.file = "-";
  if (autoz || t.create) {
    size_t n = strlen(t.file);
    if (autoz && ((n > 3 && strcmp(t.file + n - 3, ".gz") == 0) || (n > 4 && strcmp(t.file + n - 4, ".tgz") == 0)))
      t.gz = 1;
  }
  out_init(&o, t.list || !t.to_stdout ? out : err);
  t.o = &o;
  if (t.create && t.verbose && strcmp(t.file, "-") == 0) out_init(&o, err);
  if (t.create) {
    unsigned char zero[10240];
    if (g.ops.n == 0) {
      tool_err(err, "tar", "Cowardly refusing to create an empty archive");
      fd_printf(err, "Try 'tar --help' or 'tar --usage' for more information.\n");
      goto bad;
    }
    if (strcmp(t.file, "-") == 0) t.fd = out;
    else {
      char *native = path_to_native(t.file);
      t.fd = os_open(native, OS_WRITE);
      free(native);
      if (t.fd < 0) {
        tool_err(err, "tar", "%s: Cannot open: %s", t.file, os_errmsg());
        goto bad;
      }
    }
    if (t.gz) t.gzo = gz_open(fd_sink, &t.fd, 6, NULL, 0);
    for (i = 0; i < (int)g.ops.n && !tool_stop(); i++) {
      char *base = NULL, *native;
      if (t.dir) {	/* -C dir: the names are inside it */
        base = tool_join(t.dir, g.ops.v[i]);
        native = path_to_native(base);
      }
      else native = path_to_native(g.ops.v[i]);
      tar_add(&t, g.ops.v[i], native);
      free(native);
      free(base);
    }
    /* two zero blocks, then up to a whole 10K record */
    memset(zero, 0, sizeof(zero));
    tar_sink(&t, zero, 1024);
    if (t.written % 10240) tar_sink(&t, zero, (size_t)(10240 - t.written % 10240));
    if (t.gzo && gz_close(t.gzo) != 0) t.status = 2;
    if (t.fd != out) os_close(t.fd);
  }
  else {
    int fd;
    int gzpipe = 0;
    GzPipe *gp = NULL;
    OsThread *th = NULL;
    unsigned char magic[2];
    if (strcmp(t.file, "-") == 0) fd = in;
    else {
      char *native = path_to_native(t.file);
      fd = os_open(native, OS_READ);
      free(native);
      if (fd < 0) {
        tool_err(err, "tar", "%s: Cannot open: %s", t.file, os_errmsg());
        tool_err(err, "tar", "Error is not recoverable: exiting now");
        goto bad;
      }
    }
    /* gzip or not: look at the first two bytes (GNU tar does too) */
    isrc_init(&t.src, fd_source, &fd);
    {
      int a = isrc_byte(&t.src), b = isrc_byte(&t.src);
      magic[0] = (unsigned char)a;
      magic[1] = (unsigned char)b;
      t.src.pos = 0;
      if (a == 0x1f && b == 0x8b) gzpipe = 1;
    }
    if (gzpipe) {	/* a thread gunzips into a pipe; we read the pipe */
      int p[2];
      if (os_pipe(p) != 0) {
        tool_err(err, "tar", "cannot make a pipe");
        goto bad;
      }
      gp = (GzPipe *)xmalloc(sizeof(GzPipe));
      gp->raw = t.src;
      gp->raw.ctx = &fd;
      gp->wfd = p[1];
      th = os_thread_start(gunzip_thread, gp);
      t.gz_pipe[0] = p[0];
      isrc_init(&t.src, fd_source, &t.gz_pipe[0]);
    }
    (void)magic;
    {
      Vec members;
      vec_init(&members);
      for (i = 0; i < (int)g.ops.n; i++) vec_push(&members, glob_mark(g.ops.v[i]));
      if (t.extract && t.dir) {
        char *dn = path_to_native(t.dir);
        OsStat st;
        if (os_stat(dn, &st) != 0 || !st.is_dir) {
          tool_err(err, "tar", "%s: Cannot open: No such file or directory", t.dir);
          tool_err(err, "tar", "Error is not recoverable: exiting now");
          free(dn);
          vec_free(&members);
          if (th) {
            os_close(t.gz_pipe[0]);
            os_thread_join(th);
            free(gp);
          }
          if (fd != in) os_close(fd);
          goto bad;
        }
        free(dn);
      }
      tar_extract(&t, &members);
      vec_free(&members);
    }
    if (th) {	/* drain, so the thread can end */
      char buf[65536];
      while (os_read(t.gz_pipe[0], buf, sizeof(buf)) > 0) {}
      os_close(t.gz_pipe[0]);
      os_thread_join(th);
      free(gp);
    }
    if (fd != in) os_close(fd);
  }
  out_flush(&o);
  opts_free(&g);
  vec_free(&t.exclude);
  if (bundle) {
    Vec nv;
    nv.v = (char **)bundle;
    nv.n = (size_t)nargc;
    nv.cap = (size_t)nargc;
    vec_free(&nv);
  }
  return tool_stop() ? 130 : t.status;
bad:
  opts_free(&g);
  vec_free(&t.exclude);
  if (bundle) {
    Vec nv;
    nv.v = (char **)bundle;
    nv.n = (size_t)nargc;
    nv.cap = (size_t)nargc;
    vec_free(&nv);
  }
  return 2;
}

/* }================================================================== */
