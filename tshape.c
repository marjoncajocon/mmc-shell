/*
** tshape.c - the OpenType tables of mmc-term that stb_truetype does not read
**
**   GSUB        glyph substitution: the ligatures of programming fonts
**               (calt, liga) and emoji made of several code points
**               (ccmp: a skin tone, a ZWJ sequence)
**   COLR/CPAL   color emoji as layers of outlines, each in a color of the
**               palette (version 0 records: Segoe UI Emoji on Windows)
**   CBDT/CBLC   color emoji as PNG pictures (Noto Color Emoji on Linux)
**   sbix        the same, the Apple way (Apple Color Emoji on macOS)
**   PNG         a small decoder for those pictures (zlib inflate, filters)
**
** Every read is checked against the end of the file: a broken font gives
** no ligatures or no colors, never a crash.
*/

#include "mterm.h"

#include <stdlib.h>
#include <string.h>


/*
** {==================================================================
** Reading the font
** ===================================================================
*/

static unsigned rd8 (const OtFace *o, uint32_t at) {
  return at < o->n ? o->d[at] : 0;
}

static unsigned rd16 (const OtFace *o, uint32_t at) {
  return (at + 2 <= o->n && at + 2 > at) ? (unsigned)((o->d[at] << 8) | o->d[at + 1]) : 0;
}

static uint32_t rd32 (const OtFace *o, uint32_t at) {
  return (at + 4 <= o->n && at + 4 > at) ?
         ((uint32_t)o->d[at] << 24) | ((uint32_t)o->d[at + 1] << 16) |
         ((uint32_t)o->d[at + 2] << 8) | o->d[at + 3] : 0;
}


static uint32_t find_table (const OtFace *o, uint32_t start, const char *tag) {
  unsigned i, n = rd16(o, start + 4);
  for (i = 0; i < n; i++) {
    uint32_t rec = start + 12 + 16 * i;
    if (rec + 16 > o->n) break;
    if (memcmp(o->d + rec, tag, 4) == 0) {
      uint32_t off = rd32(o, rec + 8);
      return off < o->n ? off : 0;
    }
  }
  return 0;
}


/* the index of gid in a Coverage table, or -1 */
static int coverage (const OtFace *o, uint32_t at, unsigned gid) {
  unsigned fmt = rd16(o, at), n = rd16(o, at + 2), lo = 0, hi = n;
  if (fmt == 1) {
    while (lo < hi) {
      unsigned mid = (lo + hi) / 2, g = rd16(o, at + 4 + 2 * mid);
      if (g == gid) return (int)mid;
      if (g < gid) lo = mid + 1;
      else hi = mid;
    }
  }
  else if (fmt == 2) {
    while (lo < hi) {
      unsigned mid = (lo + hi) / 2;
      uint32_t r = at + 4 + 6 * mid;
      if (gid < rd16(o, r)) hi = mid;
      else if (gid > rd16(o, r + 2)) lo = mid + 1;
      else return (int)(rd16(o, r + 4) + gid - rd16(o, r));
    }
  }
  return -1;
}


/* the class of gid in a ClassDef table (0 when it has none) */
static unsigned class_of (const OtFace *o, uint32_t at, unsigned gid) {
  unsigned fmt = rd16(o, at);
  if (fmt == 1) {
    unsigned first = rd16(o, at + 2), n = rd16(o, at + 4);
    return (gid >= first && gid < first + n) ? rd16(o, at + 6 + 2 * (gid - first)) : 0;
  }
  if (fmt == 2) {
    unsigned lo = 0, hi = rd16(o, at + 2);
    while (lo < hi) {
      unsigned mid = (lo + hi) / 2;
      uint32_t r = at + 4 + 6 * mid;
      if (gid < rd16(o, r)) hi = mid;
      else if (gid > rd16(o, r + 2)) lo = mid + 1;
      else return rd16(o, r + 4);
    }
  }
  return 0;
}

/* }================================================================== */


/*
** {==================================================================
** GSUB: which lookups, and where each can start
** ===================================================================
*/

static const char *const feature_tags[2] = {
  "rlig" "rclt" "calt" "liga" "clig",	/* OT_TEXT: the ligatures of code */
  "ccmp" "rlig" "liga" "clig" "calt"	/* OT_SEQ: emoji sequences */
};


static int wanted (int which, const unsigned char *tag) {
  const char *t;
  for (t = feature_tags[which]; *t; t += 4)
    if (memcmp(t, tag, 4) == 0) return 1;
  return 0;
}


static uint32_t lookup_table (const OtFace *o, unsigned index) {
  uint32_t list = o->gsub + rd16(o, o->gsub + 8);
  if (index >= rd16(o, list)) return 0;
  return list + rd16(o, list + 2 + 2 * index);
}


/* the real type and place of a subtable (type 7 points elsewhere) */
static unsigned subtable (const OtFace *o, uint32_t lk, unsigned k, uint32_t *st) {
  unsigned type = rd16(o, lk);
  *st = lk + rd16(o, lk + 6 + 2 * k);
  if (type == 7) {
    type = rd16(o, *st + 2);
    *st += rd32(o, *st + 4);
  }
  return type;
}


/* the Coverage every subtable has for the glyph it starts at */
static uint32_t first_coverage (const OtFace *o, unsigned type, uint32_t st) {
  unsigned fmt = rd16(o, st);
  if (type >= 1 && type <= 4) return st + rd16(o, st + 2);
  if (type == 5) return fmt == 3 ? st + rd16(o, st + 6) : st + rd16(o, st + 2);
  if (type == 6) {
    if (fmt != 3) return st + rd16(o, st + 2);
    return st + rd16(o, st + 6 + 2 * rd16(o, st + 2));	/* past the backtrack */
  }
  return 0;
}


static void mark_coverage (const OtFace *o, uint32_t at, unsigned char *bits) {
  unsigned fmt = rd16(o, at), n = rd16(o, at + 2), i, g;
  for (i = 0; i < n; i++) {
    if (fmt == 1) {
      g = rd16(o, at + 4 + 2 * i);
      if ((int)g < o->nglyphs) bits[g >> 3] |= (unsigned char)(1u << (g & 7));
    }
    else if (fmt == 2) {
      unsigned a = rd16(o, at + 4 + 6 * i), b = rd16(o, at + 6 + 6 * i);
      for (g = a; g <= b && (int)g < o->nglyphs; g++)
        bits[g >> 3] |= (unsigned char)(1u << (g & 7));
    }
  }
}


static int cmp_u16 (const void *a, const void *b) {
  return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b;
}


/* the lookups of the wanted features, in the order they are applied */
static void pick_lookups (OtFace *o, int which) {
  uint32_t scripts = o->gsub + rd16(o, o->gsub + 4);
  uint32_t features = o->gsub + rd16(o, o->gsub + 6);
  uint32_t script = 0, lang;
  unsigned i, k, n = rd16(o, scripts), nf, row;
  uint16_t *lk = NULL;
  int nlk = 0, cap = 0;
  for (i = 0; i < n; i++) {	/* DFLT, else latn, else the first */
    uint32_t rec = scripts + 2 + 6 * i;
    if (rec + 6 > o->n) break;
    if (memcmp(o->d + rec, "DFLT", 4) == 0 ||
        (memcmp(o->d + rec, "latn", 4) == 0 && script == 0) || (i == 0 && script == 0))
      script = scripts + rd16(o, rec + 4);
  }
  if (script == 0) return;
  lang = rd16(o, script) ? script + rd16(o, script) :
         rd16(o, script + 2) ? script + rd16(o, script + 8) : 0;
  if (lang == 0) return;
  nf = rd16(o, lang + 4);
  for (i = 0; i <= nf; i++) {	/* the required feature, then the others */
    unsigned fi = (i == 0) ? rd16(o, lang + 2) : rd16(o, lang + 6 + 2 * (i - 1));
    uint32_t rec = features + 2 + 6 * fi, ft;
    if (fi == 0xFFFF || fi >= rd16(o, features) || rec + 6 > o->n) continue;
    if (i > 0 && !wanted(which, o->d + rec)) continue;
    ft = features + rd16(o, rec + 4);
    for (k = 0; k < rd16(o, ft + 2); k++) {
      if (nlk == cap) {
        cap = cap ? cap * 2 : 64;
        lk = (uint16_t *)xrealloc(lk, (size_t)cap * sizeof(uint16_t));
      }
      lk[nlk++] = (uint16_t)rd16(o, ft + 4 + 2 * k);
    }
  }
  if (nlk == 0) return;
  qsort(lk, (size_t)nlk, sizeof(uint16_t), cmp_u16);
  for (i = 1, k = 1; i < (unsigned)nlk; i++)	/* each once */
    if (lk[i] != lk[k - 1]) lk[k++] = lk[i];
  nlk = (int)k;
  row = (unsigned)(o->nglyphs + 7) / 8;
  o->first[which] = (unsigned char *)xmalloc((size_t)nlk * row + row);
  memset(o->first[which], 0, (size_t)nlk * row + row);
  for (i = 0; i < (unsigned)nlk; i++) {	/* where each lookup can start */
    uint32_t t = lookup_table(o, lk[i]), st;
    unsigned char *bits = o->first[which] + (size_t)i * row;
    for (k = 0; t != 0 && k < rd16(o, t + 4); k++) {
      unsigned type = subtable(o, t, k, &st);
      uint32_t cov = first_coverage(o, type, st);
      if (cov != 0 && type != 8) mark_coverage(o, cov, bits);
    }
    for (k = 0; k < row; k++) o->first[which][(size_t)nlk * row + k] |= bits[k];
  }
  o->lk[which] = lk;
  o->nlk[which] = nlk;
}

/* }================================================================== */


/*
** {==================================================================
** GSUB: applying the lookups
** ===================================================================
*/

typedef struct GBuf {
  uint16_t *g;
  int *cl;	/* the cell (the first input glyph) each glyph came from */
  int n, cap;
} GBuf;

/* what a rule compares the glyphs with */
#define BY_GLYPH	0
#define BY_CLASS	1
#define BY_COVERAGE	2

typedef struct Seq {
  int by;
  uint32_t base;	/* the ClassDef (BY_CLASS), the subtable (BY_COVERAGE) */
  uint32_t arr;	/* the values, 16 bits each */
  int n;
} Seq;


static int apply_lookup (const OtFace *o, unsigned index, GBuf *b, int i, int depth);


static int match_one (const OtFace *o, const Seq *s, int k, unsigned gid) {
  unsigned v = rd16(o, s->arr + 2 * (uint32_t)k);
  if (s->by == BY_GLYPH) return v == gid;
  if (s->by == BY_CLASS) return class_of(o, s->base, gid) == v;
  return coverage(o, s->base + v, gid) >= 0;
}


static void buf_remove (GBuf *b, int at, int count) {
  memmove(b->g + at, b->g + at + count, (size_t)(b->n - at - count) * sizeof(uint16_t));
  memmove(b->cl + at, b->cl + at + count, (size_t)(b->n - at - count) * sizeof(int));
  b->n -= count;
}


/*
** A context rule at i: 'back' before it (nearest first), 'in' from it on
** ('in' starts to compare at 'from': the first glyph may be checked
** already), 'ahead' after the input. Then the nested lookups run on
** the input. Returns where to go on, or -1.
*/
static int apply_rule (const OtFace *o, GBuf *b, int i, int depth, const Seq *back,
                       const Seq *in, int from, const Seq *ahead, uint32_t recs,
                       unsigned nrecs) {
  int k, end;
  unsigned r;
  if (i + in->n > b->n || i < back->n) return -1;
  if (ahead->n > 0 && i + in->n + ahead->n > b->n) return -1;
  for (k = 0; k < back->n; k++)
    if (!match_one(o, back, k, b->g[i - 1 - k])) return -1;
  for (k = from; k < in->n; k++)
    if (!match_one(o, in, k, b->g[i + k])) return -1;
  for (k = 0; k < ahead->n; k++)
    if (!match_one(o, ahead, k, b->g[i + in->n + k])) return -1;
  end = i + in->n;
  if (depth >= 8) return end;	/* nested too deep: a broken font */
  for (r = 0; r < nrecs; r++) {
    int at = i + (int)rd16(o, recs + 4 * r), n0 = b->n;
    if (at >= end) continue;
    apply_lookup(o, rd16(o, recs + 4 * r + 2), b, at, depth + 1);
    end += b->n - n0;	/* a ligature made the input shorter */
  }
  return end > i ? end : i + 1;
}


/* type 5 (context) and 6 (chained context), formats 1, 2 and 3 */
static int apply_context (const OtFace *o, unsigned type, uint32_t st, GBuf *b, int i,
                          int depth) {
  unsigned fmt = rd16(o, st), gid = b->g[i], k, nsets;
  Seq back = {BY_GLYPH, 0, 0, 0}, in = {BY_GLYPH, 0, 0, 0}, ahead = {BY_GLYPH, 0, 0, 0};
  uint32_t set, at;
  int c, r;
  if (fmt == 3) {
    in.by = back.by = ahead.by = BY_COVERAGE;
    in.base = back.base = ahead.base = st;
    if (type == 5) {
      in.n = (int)rd16(o, st + 2);
      in.arr = st + 6;
      return apply_rule(o, b, i, depth, &back, &in, 0, &ahead, in.arr + 2 * (uint32_t)in.n,
                        rd16(o, st + 4));
    }
    back.n = (int)rd16(o, st + 2);
    back.arr = st + 4;
    at = back.arr + 2 * (uint32_t)back.n;
    in.n = (int)rd16(o, at);
    in.arr = at + 2;
    at = in.arr + 2 * (uint32_t)in.n;
    ahead.n = (int)rd16(o, at);
    ahead.arr = at + 2;
    at = ahead.arr + 2 * (uint32_t)ahead.n;
    return apply_rule(o, b, i, depth, &back, &in, 0, &ahead, at + 2, rd16(o, at));
  }
  if ((c = coverage(o, st + rd16(o, st + 2), gid)) < 0) return -1;
  if (fmt == 1) {
    nsets = rd16(o, st + 4);
    if ((unsigned)c >= nsets || rd16(o, st + 6 + 2 * (unsigned)c) == 0) return -1;
    set = st + rd16(o, st + 6 + 2 * (unsigned)c);
  }
  else if (fmt == 2) {
    unsigned cls;
    uint32_t cd_in = st + rd16(o, st + (type == 5 ? 4 : 6));
    in.by = back.by = ahead.by = BY_CLASS;
    in.base = cd_in;
    if (type == 6) {
      back.base = st + rd16(o, st + 4);
      ahead.base = st + rd16(o, st + 8);
    }
    cls = class_of(o, cd_in, gid);
    nsets = rd16(o, st + (type == 5 ? 6 : 10));
    at = st + (type == 5 ? 8 : 12) + 2 * cls;
    if (cls >= nsets || rd16(o, at) == 0) return -1;
    set = st + rd16(o, at);
  }
  else return -1;
  for (k = 0; k < rd16(o, set); k++) {	/* the first rule that fits */
    uint32_t rule = set + rd16(o, set + 2 + 2 * k);
    if (type == 5) {
      in.n = (int)rd16(o, rule);
      in.arr = rule + 4 - 2;	/* index 0 is the glyph at i, not stored */
      back.n = ahead.n = 0;
      r = apply_rule(o, b, i, depth, &back, &in, 1, &ahead,
                     rule + 4 + 2 * (uint32_t)(in.n > 0 ? in.n - 1 : 0), rd16(o, rule + 2));
    }
    else {
      back.n = (int)rd16(o, rule);
      back.arr = rule + 2;
      at = back.arr + 2 * (uint32_t)back.n;
      in.n = (int)rd16(o, at);
      in.arr = at;	/* +2 - 2: index 0 not stored */
      at += 2 + 2 * (uint32_t)(in.n > 0 ? in.n - 1 : 0);
      ahead.n = (int)rd16(o, at);
      ahead.arr = at + 2;
      at = ahead.arr + 2 * (uint32_t)ahead.n;
      r = apply_rule(o, b, i, depth, &back, &in, 1, &ahead, at + 2, rd16(o, at));
    }
    if (r >= 0) return r;
  }
  return -1;
}


/* one subtable at glyph i: where to go on, or -1 when it does not apply */
static int apply_subtable (const OtFace *o, unsigned type, uint32_t st, GBuf *b, int i,
                           int depth) {
  unsigned fmt = rd16(o, st), gid = b->g[i], k, n;
  int c;
  if (type == 5 || type == 6) return apply_context(o, type, st, b, i, depth);
  if (type < 1 || type > 4) return -1;	/* 8, reverse chaining: Arabic, not us */
  if ((c = coverage(o, st + rd16(o, st + 2), gid)) < 0) return -1;
  switch (type) {
    case 1:	/* single */
      if (fmt == 1) b->g[i] = (uint16_t)(gid + rd16(o, st + 4));
      else if (fmt == 2 && (unsigned)c < rd16(o, st + 4))
        b->g[i] = (uint16_t)rd16(o, st + 6 + 2 * (unsigned)c);
      else return -1;
      return i + 1;
    case 2: {	/* multiple: one glyph becomes several */
      uint32_t seq;
      if ((unsigned)c >= rd16(o, st + 4)) return -1;
      seq = st + rd16(o, st + 6 + 2 * (unsigned)c);
      n = rd16(o, seq);
      if (n == 0 || b->n - 1 + (int)n > b->cap) return -1;
      memmove(b->g + i + n, b->g + i + 1, (size_t)(b->n - i - 1) * sizeof(uint16_t));
      memmove(b->cl + i + n, b->cl + i + 1, (size_t)(b->n - i - 1) * sizeof(int));
      for (k = 0; k < n; k++) {
        b->g[i + (int)k] = (uint16_t)rd16(o, seq + 2 + 2 * k);
        b->cl[i + (int)k] = b->cl[i];
      }
      b->n += (int)n - 1;
      return i + (int)n;
    }
    case 3: {	/* alternate: the first one */
      uint32_t set;
      if ((unsigned)c >= rd16(o, st + 4)) return -1;
      set = st + rd16(o, st + 6 + 2 * (unsigned)c);
      if (rd16(o, set) == 0) return -1;
      b->g[i] = (uint16_t)rd16(o, set + 2);
      return i + 1;
    }
    default: {	/* 4, ligature: several glyphs become one */
      uint32_t set;
      if ((unsigned)c >= rd16(o, st + 4)) return -1;
      set = st + rd16(o, st + 6 + 2 * (unsigned)c);
      for (k = 0; k < rd16(o, set); k++) {
        uint32_t lig = set + rd16(o, set + 2 + 2 * k);
        unsigned comps = rd16(o, lig + 2), j;
        if (comps == 0 || i + (int)comps > b->n) continue;
        for (j = 1; j < comps; j++)
          if (b->g[i + (int)j] != rd16(o, lig + 4 + 2 * (j - 1))) break;
        if (j < comps) continue;
        b->g[i] = (uint16_t)rd16(o, lig);
        buf_remove(b, i + 1, (int)comps - 1);
        return i + 1;
      }
      return -1;
    }
  }
}


static int apply_lookup (const OtFace *o, unsigned index, GBuf *b, int i, int depth) {
  uint32_t lk = lookup_table(o, index), st;
  unsigned k;
  if (lk == 0 || i >= b->n) return -1;
  for (k = 0; k < rd16(o, lk + 4); k++) {
    unsigned type = subtable(o, lk, k, &st);
    int r = apply_subtable(o, type, st, b, i, depth);
    if (r >= 0) return r;
  }
  return -1;
}


int ot_shape (const OtFace *o, int which, uint16_t *g, int *cl, int n, int cap) {
  GBuf b;
  size_t row = (size_t)(o->nglyphs + 7) / 8;
  const unsigned char *any;
  int k, i;
  if (o->nlk[which] == 0) return n;
  any = o->first[which] + (size_t)o->nlk[which] * row;
  for (i = 0; i < n; i++)	/* nothing can start here: done */
    if (g[i] < o->nglyphs && (any[g[i] >> 3] & (1u << (g[i] & 7)))) break;
  if (i == n) return n;
  b.g = g;
  b.cl = cl;
  b.n = n;
  b.cap = cap;
  for (k = 0; k < o->nlk[which]; k++) {
    const unsigned char *first = o->first[which] + (size_t)k * row;
    for (i = 0; i < b.n;) {
      int r;
      unsigned gid = b.g[i];
      if ((int)gid >= o->nglyphs || !(first[gid >> 3] & (1u << (gid & 7)))) {
        i++;
        continue;
      }
      r = apply_lookup(o, o->lk[which][k], &b, i, 0);
      i = (r > i) ? r : i + 1;
    }
  }
  return b.n;
}


int ot_can_shape (const OtFace *o, int which) {
  return o->nlk[which] > 0;
}

/* }================================================================== */


/*
** {==================================================================
** COLR / CPAL: layers in colors
** ===================================================================
*/

int ot_color_layers (const OtFace *o, int gid, int *first) {
  unsigned lo = 0, hi;
  uint32_t base;
  if (o->colr == 0 || o->cpal == 0) return 0;
  hi = rd16(o, o->colr + 2);
  base = o->colr + rd32(o, o->colr + 4);
  while (lo < hi) {
    unsigned mid = (lo + hi) / 2, g = rd16(o, base + 6 * mid);
    if (g == (unsigned)gid) {
      *first = (int)rd16(o, base + 6 * mid + 2);
      return (int)rd16(o, base + 6 * mid + 4);
    }
    if (g < (unsigned)gid) lo = mid + 1;
    else hi = mid;
  }
  return 0;
}


void ot_layer (const OtFace *o, int index, int *gid, int *palette_index) {
  uint32_t at = o->colr + rd32(o, o->colr + 8) + 4 * (uint32_t)index;
  *gid = (int)rd16(o, at);
  *palette_index = (int)rd16(o, at + 2);
}


/* 0xAARRGGBB from palette 0; 0xFFFF is the text color */
uint32_t ot_palette (const OtFace *o, int index, uint32_t fg) {
  uint32_t rec;
  if (index == 0xFFFF || index >= (int)rd16(o, o->cpal + 2)) return 0xFF000000u | fg;
  rec = o->cpal + rd32(o, o->cpal + 8) + 4 * (rd16(o, o->cpal + 12) + (uint32_t)index);
  return (rd8(o, rec + 3) << 24) | (rd8(o, rec + 2) << 16) | (rd8(o, rec + 1) << 8) |
         rd8(o, rec);
}

/* }================================================================== */


/*
** {==================================================================
** PNG: zlib inflate, then the filters of each row
** ===================================================================
*/

typedef struct Huff {
  uint16_t count[16];
  uint16_t sym[320];
} Huff;

typedef struct Inflate {
  const unsigned char *p;
  size_t n, at;
  uint32_t bits;
  int nbits, err;
  unsigned char *out;
  size_t len, cap;
} Inflate;


static unsigned bits (Inflate *z, int need) {
  unsigned v;
  while (z->nbits < need) {
    if (z->at >= z->n) {
      z->err = 1;
      return 0;
    }
    z->bits |= (uint32_t)z->p[z->at++] << z->nbits;
    z->nbits += 8;
  }
  v = z->bits & ((1u << need) - 1);
  z->bits >>= need;
  z->nbits -= need;
  return v;
}


static int huff_build (Huff *h, const unsigned char *len, int n) {
  uint16_t offs[16];
  int i;
  memset(h->count, 0, sizeof(h->count));
  for (i = 0; i < n; i++) h->count[len[i]]++;
  h->count[0] = 0;
  offs[1] = 0;
  for (i = 1; i < 15; i++) offs[i + 1] = (uint16_t)(offs[i] + h->count[i]);
  for (i = 0; i < n; i++)
    if (len[i] != 0) h->sym[offs[len[i]]++] = (uint16_t)i;
  return 0;
}


static int huff_decode (Inflate *z, const Huff *h) {
  int code = 0, first = 0, index = 0, len;
  for (len = 1; len < 16; len++) {
    int count = h->count[len];
    code |= (int)bits(z, 1);
    if (z->err) return -1;
    if (code - first < count) return h->sym[index + code - first];
    index += count;
    first = (first + count) << 1;
    code <<= 1;
  }
  z->err = 1;
  return -1;
}


static void put (Inflate *z, unsigned char c) {
  if (z->len >= z->cap) {
    z->err = 1;
    return;
  }
  z->out[z->len++] = c;
}


static const uint16_t len_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23,
  27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const unsigned char len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2,
  2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dist_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97,
  129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289,
  16385, 24577};
static const unsigned char dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5,
  6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};


static void inflate_block (Inflate *z, const Huff *lit, const Huff *dist) {
  for (;;) {
    int s = huff_decode(z, lit);
    if (z->err || s == 256) return;
    if (s < 256) put(z, (unsigned char)s);
    else {
      int len, d;
      size_t k;
      s -= 257;
      if (s >= 29) {
        z->err = 1;
        return;
      }
      len = len_base[s] + (int)bits(z, len_extra[s]);
      if ((d = huff_decode(z, dist)) < 0 || d >= 30) {
        z->err = 1;
        return;
      }
      d = dist_base[d] + (int)bits(z, dist_extra[d]);
      if ((size_t)d > z->len) {
        z->err = 1;
        return;
      }
      for (k = 0; k < (size_t)len && !z->err; k++) put(z, z->out[z->len - (size_t)d]);
    }
    if (z->err) return;
  }
}


static int inflate_run (Inflate *z) {
  static const unsigned char order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12,
                                          3, 13, 2, 14, 1, 15};
  Huff lit, dist;
  unsigned char lens[320];
  int last;
  z->at += 2;	/* the zlib header */
  do {
    int type;
    last = (int)bits(z, 1);
    type = (int)bits(z, 2);
    if (z->err) return 0;
    if (type == 0) {	/* stored */
      unsigned n;
      z->bits = 0;
      z->nbits = 0;
      if (z->at + 4 > z->n) return 0;
      n = z->p[z->at] | (unsigned)(z->p[z->at + 1] << 8);
      z->at += 4;
      if (z->at + n > z->n) return 0;
      while (n-- > 0) put(z, z->p[z->at++]);
    }
    else if (type == 1) {	/* the fixed codes */
      int i;
      for (i = 0; i < 144; i++) lens[i] = 8;
      for (; i < 256; i++) lens[i] = 9;
      for (; i < 280; i++) lens[i] = 7;
      for (; i < 288; i++) lens[i] = 8;
      huff_build(&lit, lens, 288);
      for (i = 0; i < 30; i++) lens[i] = 5;
      huff_build(&dist, lens, 30);
      inflate_block(z, &lit, &dist);
    }
    else if (type == 2) {	/* codes sent along */
      int nlit = (int)bits(z, 5) + 257, ndist = (int)bits(z, 5) + 1;
      int nclen = (int)bits(z, 4) + 4, i = 0;
      Huff cl;
      memset(lens, 0, sizeof(lens));
      for (i = 0; i < nclen; i++) lens[order[i]] = (unsigned char)bits(z, 3);
      huff_build(&cl, lens, 19);
      memset(lens, 0, sizeof(lens));
      for (i = 0; i < nlit + ndist && !z->err;) {
        int s = huff_decode(z, &cl), rep = 0;
        unsigned char v = 0;
        if (s < 0) return 0;
        if (s < 16) {
          lens[i++] = (unsigned char)s;
          continue;
        }
        if (s == 16) {
          if (i == 0) return 0;
          v = lens[i - 1];
          rep = 3 + (int)bits(z, 2);
        }
        else if (s == 17) rep = 3 + (int)bits(z, 3);
        else rep = 11 + (int)bits(z, 7);
        if (i + rep > nlit + ndist) return 0;
        while (rep-- > 0) lens[i++] = v;
      }
      huff_build(&lit, lens, nlit);
      huff_build(&dist, lens + nlit, ndist);
      inflate_block(z, &lit, &dist);
    }
    else return 0;
    if (z->err) return 0;
  } while (!last);
  return 1;
}


static int paeth (int a, int b, int c) {
  int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p;
  int pc = p > c ? p - c : c - p;
  return (pa <= pb && pa <= pc) ? a : (pb <= pc) ? b : c;
}


uint32_t *png_decode (const unsigned char *p, size_t n, int *pw, int *ph) {
  static const unsigned char sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  unsigned char *idat = NULL, pal[256 * 4];
  size_t nidat = 0, at = 8, stride, x;
  uint32_t w = 0, h = 0, y, *img = NULL;
  int depth = 0, ctype = 0, chans, bpp, ok = 1, trns = 0, npal = 0;
  uint16_t tkey[3] = {0, 0, 0};
  Inflate z;
  if (n < 8 || memcmp(p, sig, 8) != 0) return NULL;
  memset(pal, 255, sizeof(pal));
  while (at + 12 <= n) {
    uint32_t len = ((uint32_t)p[at] << 24) | ((uint32_t)p[at + 1] << 16) |
                   ((uint32_t)p[at + 2] << 8) | p[at + 3];
    const unsigned char *type = p + at + 4, *data = p + at + 8;
    if (len > n - at - 12) break;
    if (memcmp(type, "IHDR", 4) == 0 && len >= 13) {
      w = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3];
      h = ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) | ((uint32_t)data[6] << 8) | data[7];
      depth = data[8];
      ctype = data[9];
      if (data[12] != 0) ok = 0;	/* interlaced: not for glyphs */
    }
    else if (memcmp(type, "PLTE", 4) == 0) {
      uint32_t i;
      for (i = 0; i < len / 3 && i < 256; i++) {
        pal[i * 4] = data[i * 3];
        pal[i * 4 + 1] = data[i * 3 + 1];
        pal[i * 4 + 2] = data[i * 3 + 2];
      }
      npal = (int)(len / 3);
    }
    else if (memcmp(type, "tRNS", 4) == 0) {
      uint32_t i;
      trns = 1;
      if (ctype == 3)
        for (i = 0; i < len && i < 256; i++) pal[i * 4 + 3] = data[i];
      else
        for (i = 0; i < 3 && i * 2 + 1 < len; i++)
          tkey[i] = (uint16_t)((data[i * 2] << 8) | data[i * 2 + 1]);
    }
    else if (memcmp(type, "IDAT", 4) == 0) {
      idat = (unsigned char *)xrealloc(idat, nidat + len + 1);
      memcpy(idat + nidat, data, len);
      nidat += len;
    }
    else if (memcmp(type, "IEND", 4) == 0) break;
    at += 12 + len;
  }
  chans = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : ctype == 6 ? 4 : 0;
  if (!ok || idat == NULL || w == 0 || h == 0 || w > 4096 || h > 4096 || chans == 0 ||
      (depth != 8 && !(ctype == 3 && (depth == 1 || depth == 2 || depth == 4))) ||
      (ctype == 3 && npal == 0)) {
    free(idat);
    return NULL;
  }
  bpp = (chans * depth + 7) / 8;
  stride = ((size_t)w * (size_t)chans * (size_t)depth + 7) / 8;
  memset(&z, 0, sizeof(z));
  z.p = idat;
  z.n = nidat;
  z.cap = (stride + 1) * h;
  z.out = (unsigned char *)xmalloc(z.cap);
  if (!inflate_run(&z) || z.len != z.cap) {
    free(idat);
    free(z.out);
    return NULL;
  }
  free(idat);
  for (y = 0; y < h; y++) {	/* undo the filter of each row */
    unsigned char *row = z.out + y * (stride + 1) + 1;
    const unsigned char *up = y > 0 ? row - (stride + 1) : NULL;
    int f = row[-1];
    for (x = 0; x < stride; x++) {
      int a = x >= (size_t)bpp ? row[x - (size_t)bpp] : 0, b = up ? up[x] : 0;
      int c = (up && x >= (size_t)bpp) ? up[x - (size_t)bpp] : 0;
      switch (f) {
        case 1: row[x] = (unsigned char)(row[x] + a); break;
        case 2: row[x] = (unsigned char)(row[x] + b); break;
        case 3: row[x] = (unsigned char)(row[x] + (a + b) / 2); break;
        case 4: row[x] = (unsigned char)(row[x] + paeth(a, b, c)); break;
        default: break;
      }
    }
  }
  img = (uint32_t *)xmalloc((size_t)w * h * sizeof(uint32_t));
  for (y = 0; y < h; y++) {
    const unsigned char *row = z.out + y * (stride + 1) + 1;
    for (x = 0; x < w; x++) {
      unsigned r, g, b, a = 255;
      if (ctype == 3) {
        unsigned i = (depth == 8) ? row[x] :
                     (row[x * (size_t)depth / 8] >> (8 - depth - (int)(x * (size_t)depth % 8))) &
                     ((1u << depth) - 1);
        r = pal[i * 4];
        g = pal[i * 4 + 1];
        b = pal[i * 4 + 2];
        a = pal[i * 4 + 3];
      }
      else {
        const unsigned char *px = row + x * (size_t)chans;
        r = px[0];
        g = chans >= 3 ? px[1] : r;
        b = chans >= 3 ? px[2] : r;
        if (chans == 2) a = px[1];
        else if (chans == 4) a = px[3];
        else if (trns && r == tkey[0] && (chans == 1 || (g == tkey[1] && b == tkey[2]))) a = 0;
      }
      img[y * w + x] = ((uint32_t)a << 24) | (r << 16) | (g << 8) | b;
    }
  }
  free(z.out);
  *pw = (int)w;
  *ph = (int)h;
  return img;
}

/* }================================================================== */


/*
** {==================================================================
** CBDT / CBLC and sbix: emoji as pictures
** ===================================================================
*/

/* the PNG of a glyph in a CBDT strike, or NULL */
static const unsigned char *cbdt_png (const OtFace *o, uint32_t strike, int gid,
                                      uint32_t *len) {
  uint32_t arr = o->cblc + rd32(o, strike), nsub = rd32(o, strike + 8), i;
  for (i = 0; i < nsub && i < 4096; i++) {
    uint32_t rec = arr + 8 * i, sub, img_off = 0, img_end = 0;
    unsigned first = rd16(o, rec), last = rd16(o, rec + 2), ifmt, dfmt;
    uint32_t k = (uint32_t)gid - first, data;
    if ((unsigned)gid < first || (unsigned)gid > last) continue;
    sub = arr + rd32(o, rec + 4);
    ifmt = rd16(o, sub);
    dfmt = rd16(o, sub + 2);
    data = o->cbdt + rd32(o, sub + 4);
    switch (ifmt) {
      case 1:
        img_off = rd32(o, sub + 8 + 4 * k);
        img_end = rd32(o, sub + 12 + 4 * k);
        break;
      case 2: {
        uint32_t size = rd32(o, sub + 8);
        img_off = size * k;
        img_end = img_off + size;
        break;
      }
      case 3:
        img_off = rd16(o, sub + 8 + 2 * k);
        img_end = rd16(o, sub + 10 + 2 * k);
        break;
      case 4: {
        uint32_t j, ng = rd32(o, sub + 8);
        for (j = 0; j < ng && j < 65536; j++)
          if (rd16(o, sub + 12 + 4 * j) == (unsigned)gid) {
            img_off = rd16(o, sub + 14 + 4 * j);
            img_end = rd16(o, sub + 18 + 4 * j);
            break;
          }
        if (j >= ng) return NULL;
        break;
      }
      case 5: {
        uint32_t j, size = rd32(o, sub + 8), ng = rd32(o, sub + 20);
        for (j = 0; j < ng && j < 65536; j++)
          if (rd16(o, sub + 24 + 2 * j) == (unsigned)gid) break;
        if (j >= ng) return NULL;
        img_off = size * j;
        img_end = img_off + size;
        break;
      }
      default: return NULL;
    }
    if (img_end <= img_off) return NULL;
    data += img_off;
    if (dfmt == 17) data += 5;	/* small metrics */
    else if (dfmt == 18) data += 8;	/* big metrics */
    else if (dfmt != 19) return NULL;
    *len = rd32(o, data);
    if (data + 4 > o->n || *len > o->n - data - 4) return NULL;
    return o->d + data + 4;
  }
  return NULL;
}


uint32_t *ot_bitmap (const OtFace *o, int gid, int want_ppem, int *w, int *h) {
  const unsigned char *png = NULL;
  uint32_t len = 0, i, best = 0;
  int best_ppem = 0;
  if (o->cblc != 0 && o->cbdt != 0) {	/* the strike nearest above the size */
    uint32_t n = rd32(o, o->cblc + 4);
    for (i = 0; i < n && i < 64; i++) {
      uint32_t s = o->cblc + 8 + 48 * i;
      int ppem = (int)rd8(o, s + 45);
      if ((unsigned)gid < rd16(o, s + 40) || (unsigned)gid > rd16(o, s + 42)) continue;
      if (best == 0 || (best_ppem < want_ppem ? ppem > best_ppem :
                        ppem >= want_ppem && ppem < best_ppem)) {
        best = s;
        best_ppem = ppem;
      }
    }
    if (best != 0) png = cbdt_png(o, best, gid, &len);
  }
  else if (o->sbix != 0) {
    uint32_t n = rd32(o, o->sbix + 4), glyph, end;
    int hops;
    for (i = 0; i < n && i < 64; i++) {
      uint32_t s = o->sbix + rd32(o, o->sbix + 8 + 4 * i);
      int ppem = (int)rd16(o, s);
      if (best == 0 || (best_ppem < want_ppem ? ppem > best_ppem :
                        ppem >= want_ppem && ppem < best_ppem)) {
        best = s;
        best_ppem = ppem;
      }
    }
    for (hops = 0; best != 0 && hops < 4; hops++) {	/* 'dupe' points to another glyph */
      if (gid < 0 || gid >= o->nglyphs) return NULL;
      glyph = best + rd32(o, best + 4 + 4 * (uint32_t)gid);
      end = best + rd32(o, best + 8 + 4 * (uint32_t)gid);
      if (end <= glyph + 8 || end > o->n) return NULL;
      if (memcmp(o->d + glyph + 4, "dupe", 4) == 0) {
        gid = (int)rd16(o, glyph + 8);
        continue;
      }
      if (memcmp(o->d + glyph + 4, "png ", 4) != 0) return NULL;
      png = o->d + glyph + 8;
      len = end - glyph - 8;
      break;
    }
  }
  return png != NULL ? png_decode(png, len, w, h) : NULL;
}

/* }================================================================== */


int ot_open (OtFace *o, const unsigned char *data, size_t n, int fontstart) {
  uint32_t maxp;
  memset(o, 0, sizeof(*o));
  o->d = data;
  o->n = n;
  if (data == NULL || (size_t)fontstart + 12 > n) return 0;
  maxp = find_table(o, (uint32_t)fontstart, "maxp");
  o->nglyphs = maxp ? (int)rd16(o, maxp + 4) : 0;
  o->gsub = find_table(o, (uint32_t)fontstart, "GSUB");
  o->colr = find_table(o, (uint32_t)fontstart, "COLR");
  o->cpal = find_table(o, (uint32_t)fontstart, "CPAL");
  o->cblc = find_table(o, (uint32_t)fontstart, "CBLC");
  o->cbdt = find_table(o, (uint32_t)fontstart, "CBDT");
  o->sbix = find_table(o, (uint32_t)fontstart, "sbix");
  if (o->gsub != 0 && o->nglyphs > 0 && rd16(o, o->gsub) == 1) {
    pick_lookups(o, OT_TEXT);
    pick_lookups(o, OT_SEQ);
  }
  return 1;
}


void ot_close (OtFace *o) {
  int i;
  for (i = 0; i < 2; i++) {
    free(o->lk[i]);
    free(o->first[i]);
  }
  memset(o, 0, sizeof(*o));
}
