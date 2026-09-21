/*
** tdraw.c - software renderer of mmc-term
**
** Paints the whole terminal (cells, cursor, selection, scrollbar,
** gradient strip, popup menu) into a Frame of 0x00RRGGBB pixels. The
** backends only have to show that picture, so every system looks the
** same. Box drawing and block characters are drawn as geometry, which
** makes lines join perfectly whatever the font is.
*/

#include "mterm.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


void frame_resize (Frame *f, int w, int h) {
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  if (w == f->w && h == f->h && f->px != NULL) return;
  f->px = (uint32_t *)xrealloc(f->px, (size_t)w * (size_t)h * sizeof(uint32_t));
  f->w = w;
  f->h = h;
}


void frame_free (Frame *f) {
  free(f->px);
  f->px = NULL;
  f->w = f->h = 0;
}


/* 24 bit .bmp, for tests and screenshots */
int frame_save_bmp (const Frame *f, const char *native) {
  int rowsize = (f->w * 3 + 3) & ~3;
  unsigned long size = 54ul + (unsigned long)rowsize * (unsigned long)f->h;
  unsigned char head[54];
  unsigned char *row;
  int fd, x, y;
  memset(head, 0, sizeof(head));
  head[0] = 'B'; head[1] = 'M';
  head[2] = (unsigned char)size; head[3] = (unsigned char)(size >> 8);
  head[4] = (unsigned char)(size >> 16); head[5] = (unsigned char)(size >> 24);
  head[10] = 54; head[14] = 40;
  head[18] = (unsigned char)f->w; head[19] = (unsigned char)(f->w >> 8);
  head[22] = (unsigned char)f->h; head[23] = (unsigned char)(f->h >> 8);
  head[26] = 1; head[28] = 24;
  if ((fd = os_open(native, OS_WRITE)) < 0) return -1;
  os_write(fd, head, sizeof(head));
  row = (unsigned char *)xmalloc((size_t)rowsize);
  memset(row, 0, (size_t)rowsize);
  for (y = f->h - 1; y >= 0; y--) {	/* bottom row first */
    const uint32_t *src = f->px + (size_t)y * (size_t)f->w;
    for (x = 0; x < f->w; x++) {
      row[x * 3] = (unsigned char)src[x];
      row[x * 3 + 1] = (unsigned char)(src[x] >> 8);
      row[x * 3 + 2] = (unsigned char)(src[x] >> 16);
    }
    os_write(fd, row, (size_t)rowsize);
  }
  free(row);
  os_close(fd);
  return 0;
}


/*
** {==================================================================
** Pixels
** ===================================================================
*/

static uint32_t mix (uint32_t bg, uint32_t fg, int a) {
  uint32_t r, g, b;
  if (a <= 0) return bg;
  if (a >= 255) return fg;
  r = (((bg >> 16) & 0xFF) * (uint32_t)(255 - a) + ((fg >> 16) & 0xFF) * (uint32_t)a) / 255;
  g = (((bg >> 8) & 0xFF) * (uint32_t)(255 - a) + ((fg >> 8) & 0xFF) * (uint32_t)a) / 255;
  b = ((bg & 0xFF) * (uint32_t)(255 - a) + (fg & 0xFF) * (uint32_t)a) / 255;
  return (r << 16) | (g << 8) | b;
}


static void fill (Frame *f, int x, int y, int w, int h, uint32_t color) {
  int i, j;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > f->w) w = f->w - x;
  if (y + h > f->h) h = f->h - y;
  for (j = 0; j < h; j++) {
    uint32_t *p = f->px + (size_t)(y + j) * (size_t)f->w + (size_t)x;
    for (i = 0; i < w; i++) p[i] = color;
  }
}


static void fill_alpha (Frame *f, int x, int y, int w, int h, uint32_t color,
                        int a) {
  int i, j;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > f->w) w = f->w - x;
  if (y + h > f->h) h = f->h - y;
  for (j = 0; j < h; j++) {
    uint32_t *p = f->px + (size_t)(y + j) * (size_t)f->w + (size_t)x;
    for (i = 0; i < w; i++) p[i] = mix(p[i], color, a);
  }
}


/* rectangle with round corners, anti-aliased */
static void fill_round (Frame *f, int x, int y, int w, int h, int r,
                        uint32_t color, int alpha) {
  int i, j;
  if (r * 2 > h) r = h / 2;
  if (r * 2 > w) r = w / 2;
  for (j = 0; j < h; j++) {
    for (i = 0; i < w; i++) {
      int px = x + i, py = y + j, a = alpha;
      int cx = (i < r) ? r : (i >= w - r) ? w - r - 1 : i;
      int cy = (j < r) ? r : (j >= h - r) ? h - r - 1 : j;
      if (px < 0 || py < 0 || px >= f->w || py >= f->h) continue;
      if (cx != i && cy != j) {	/* inside a corner square */
        double d = sqrt((double)((i - cx) * (i - cx) + (j - cy) * (j - cy)));
        double cover = (double)r + 0.5 - d;
        if (cover <= 0.0) continue;
        if (cover < 1.0) a = (int)((double)alpha * cover);
      }
      f->px[(size_t)py * (size_t)f->w + (size_t)px] =
        mix(f->px[(size_t)py * (size_t)f->w + (size_t)px], color, a);
    }
  }
}


/* text is thin when light on dark and fat when dark on light: even it out */
static unsigned char lut_light[256], lut_dark[256];
static int lut_ready = 0;


static void make_luts (void) {
  int i;
  for (i = 0; i < 256; i++) {
    lut_light[i] = (unsigned char)(pow((double)i / 255.0, 0.78) * 255.0 + 0.5);
    lut_dark[i] = (unsigned char)(pow((double)i / 255.0, 1.10) * 255.0 + 0.5);
  }
  lut_ready = 1;
}


static int brightness (uint32_t c) {
  return (int)(((c >> 16) & 0xFF) * 3 + ((c >> 8) & 0xFF) * 6 + (c & 0xFF));
}


/* is this dark text on a light background? (the font asks, see tfont.c) */
static int dark_text (uint32_t fg, uint32_t bg) {
  return brightness(fg) < brightness(bg);
}


/* ClearType: every color channel has its own coverage */
static uint32_t mix3 (uint32_t bg, uint32_t fg, int ar, int ag, int ab) {
  uint32_t r = (((bg >> 16) & 0xFF) * (uint32_t)(255 - ar) + ((fg >> 16) & 0xFF) * (uint32_t)ar) / 255;
  uint32_t g = (((bg >> 8) & 0xFF) * (uint32_t)(255 - ag) + ((fg >> 8) & 0xFF) * (uint32_t)ag) / 255;
  uint32_t b = ((bg & 0xFF) * (uint32_t)(255 - ab) + (fg & 0xFF) * (uint32_t)ab) / 255;
  return (r << 16) | (g << 8) | b;
}


static void blit_glyph (Frame *f, const Glyph *g, int x, int y, uint32_t fg,
                        uint32_t bg, int clip_x0, int clip_x1) {
  const unsigned char *lut;
  int i, j;
  if (g == NULL || g->bm == NULL) return;
  if (!lut_ready) make_luts();
  lut = (brightness(fg) > brightness(bg)) ? lut_light : lut_dark;
  if (clip_x0 < 0) clip_x0 = 0;
  if (clip_x1 > f->w) clip_x1 = f->w;
  for (j = 0; j < g->h; j++) {
    int py = y + g->yoff + j;
    if (py < 0 || py >= f->h) continue;
    for (i = 0; i < g->w; i++) {
      int px = x + g->xoff + i;
      size_t at = (size_t)j * (size_t)g->w + (size_t)i;
      uint32_t *p;
      if (px < clip_x0 || px >= clip_x1) continue;
      p = &f->px[(size_t)py * (size_t)f->w + (size_t)px];
      if (g->lcd == 1) {	/* ClearType: already tuned by the system */
        const unsigned char *c = g->bm + at * 3;
        if ((c[0] | c[1] | c[2]) != 0) *p = mix3(*p, fg, c[0], c[1], c[2]);
      }
      else if (g->bm[at] != 0)	/* the system's gray is tuned too */
        *p = mix(*p, fg, g->lcd == 2 ? g->bm[at] : lut[g->bm[at]]);
    }
  }
}


static uint32_t next_cp (const char **s) {
  const unsigned char *p = (const unsigned char *)*s;
  uint32_t cp = *p++;
  int extra = (cp >= 0xF0) ? 3 : (cp >= 0xE0) ? 2 : (cp >= 0xC0) ? 1 : 0;
  if (extra) cp &= (0x3Fu >> extra);
  while (extra-- > 0 && (*p & 0xC0) == 0x80) cp = (cp << 6) | (*p++ & 0x3F);
  *s = (const char *)p;
  return cp;
}


/* width of UI text in pixels */
int draw_text_width (const char *utf8) {
  int n = 0;
  while (*utf8) n += grid_wcwidth(next_cp(&utf8));
  return n * font_cell_w();
}


static void draw_text (Frame *f, int x, int y, const char *utf8, uint32_t fg,
                       uint32_t bg, int bold) {
  while (*utf8) {
    uint32_t cp = next_cp(&utf8);
    blit_glyph(f, font_glyph(cp, bold, 0, dark_text(fg, bg)), x, y + font_ascent(),
               fg, bg, 0, f->w);
    x += font_cell_w() * grid_wcwidth(cp);
  }
}

/* }================================================================== */


/*
** {==================================================================
** Box drawing (U+2500..257F) and block elements (U+2580..259F)
** ===================================================================
*/

/* arms "left right up down": 0 none, 1 light, 2 heavy, 3 double */
static const char *const box_arms[128] = {
  "1100", "2200", "0011", "0022", "1100", "2200", "0011", "0022",
  "1100", "2200", "0011", "0022", "0101", "0201", "0102", "0202",
  "1001", "2001", "1002", "2002", "0110", "0210", "0120", "0220",
  "1010", "2010", "1020", "2020", "0111", "0211", "0121", "0112",
  "0122", "0221", "0212", "0222", "1011", "2011", "1021", "1012",
  "1022", "2021", "2012", "2022", "1101", "2101", "1201", "2201",
  "1102", "2102", "1202", "2202", "1110", "2110", "1210", "2210",
  "1120", "2120", "1220", "2220", "1111", "2111", "1211", "2211",
  "1121", "1112", "1122", "2121", "1221", "2112", "1212", "2221",
  "2212", "2122", "1222", "2222", "1100", "2200", "0011", "0022",
  "3300", "0033", "0301", "0103", "0303", "3001", "1003", "3003",
  "0310", "0130", "0330", "3010", "1030", "3030", "0311", "0133",
  "0333", "3011", "1033", "3033", "3301", "1103", "3303", "3310",
  "1130", "3330", "3311", "1133", "3333", "0101", "1001", "1010",
  "0110", "----", "----", "----", "1000", "0010", "0100", "0001",
  "2000", "0020", "0200", "0002", "1200", "0012", "2100", "0021"
};


static int draw_box (Frame *f, uint32_t cp, int x, int y, int w, int h,
                     uint32_t fg) {
  const char *arms = box_arms[cp - 0x2500];
  int a[4], i;
  int t1 = (w + 4) / 8, t2, d;
  int cx = x + w / 2, cy = y + h / 2;
  int dbl_h, dbl_v, active;
  if (arms[0] == '-') return 0;	/* diagonals: leave them to the font */
  if (t1 < 1) t1 = 1;
  t2 = t1 * 2 + (t1 == 1 ? 1 : 0);
  d = t1 + 1;
  for (i = 0; i < 4; i++) a[i] = arms[i] - '0';
  dbl_h = (a[0] == 3 || a[1] == 3);
  dbl_v = (a[2] == 3 || a[3] == 3);
  active = dbl_h && dbl_v;	/* a real double corner or junction */
  for (i = 0; i < 4; i++) {
    int t = (a[i] == 2) ? t2 : t1;
    int o = t / 2;
    /* single arms start at the double line they touch, not in the middle */
    int gap = (a[i] != 3 && ((i < 2) ? dbl_v : dbl_h)) ? d : 0;
    if (a[i] == 0) continue;
    if (a[i] != 3) {
      if (i == 0) fill(f, x, cy - o, cx - x - gap + (gap ? 0 : t - o), t, fg);
      else if (i == 1) fill(f, cx - o + gap, cy - o, x + w - (cx - o + gap), t, fg);
      else if (i == 2) fill(f, cx - o, y, t, cy - y - gap + (gap ? 0 : t - o), fg);
      else fill(f, cx - o, cy - o + gap, t, y + h - (cy - o + gap), fg);
    }
    else {	/* two parallel lines */
      int e = active ? d : -(t1 - t1 / 2);	/* stop at the box, or pass the middle */
      int k;
      for (k = -1; k <= 1; k += 2) {
        int off = k * d - t1 / 2;
        if (i == 0) fill(f, x, cy + off, cx - e - x, t1, fg);
        else if (i == 1) fill(f, cx + e, cy + off, x + w - (cx + e), t1, fg);
        else if (i == 2) fill(f, cx + off, y, t1, cy - e - y, fg);
        else fill(f, cx + off, cy + e, t1, y + h - (cy + e), fg);
      }
    }
  }
  if (active) {	/* close the sides of the middle box that have no arm */
    int lo = -d - t1 / 2, len = 2 * d + t1;
    if (a[0] != 3) fill(f, cx + lo, cy + lo, t1, len, fg);
    if (a[1] != 3) fill(f, cx + d - t1 / 2, cy + lo, t1, len, fg);
    if (a[2] != 3) fill(f, cx + lo, cy + lo, len, t1, fg);
    if (a[3] != 3) fill(f, cx + lo, cy + d - t1 / 2, len, t1, fg);
  }
  return 1;
}


static int draw_block (Frame *f, uint32_t cp, int x, int y, int w, int h,
                       uint32_t fg) {
  static const unsigned char quads[] = {4, 8, 1, 13, 9, 7, 11, 2, 6, 14};
  int hw = w / 2, hh = h / 2;
  if (cp == 0x2580) fill(f, x, y, w, hh, fg);
  else if (cp >= 0x2581 && cp <= 0x2588) {
    int n = (int)(cp - 0x2580), part = (h * n + 4) / 8;
    fill(f, x, y + h - part, w, part, fg);
  }
  else if (cp >= 0x2589 && cp <= 0x258F) {
    int n = (int)(0x2590 - cp);
    fill(f, x, y, (w * n + 4) / 8, h, fg);
  }
  else if (cp == 0x2590) fill(f, x + hw, y, w - hw, h, fg);
  else if (cp >= 0x2591 && cp <= 0x2593)
    fill_alpha(f, x, y, w, h, fg, 64 * (int)(cp - 0x2590));
  else if (cp == 0x2594) fill(f, x, y, w, (h + 4) / 8, fg);
  else if (cp == 0x2595) fill(f, x + w - (w + 4) / 8, y, (w + 4) / 8, h, fg);
  else if (cp >= 0x2596 && cp <= 0x259F) {
    int q = quads[cp - 0x2596];
    if (q & 1) fill(f, x, y, hw, hh, fg);
    if (q & 2) fill(f, x + hw, y, w - hw, hh, fg);
    if (q & 4) fill(f, x, y + hh, hw, h - hh, fg);
    if (q & 8) fill(f, x + hw, y + hh, w - hw, h - hh, fg);
  }
  else return 0;
  return 1;
}

/* }================================================================== */


/*
** {==================================================================
** Powerline separators (U+E0B0..E0BF) as geometry: they fill their cell
** exactly, so the colored segments of a prompt join without seams at
** every size and zoom. 4 x 4 samples per pixel give smooth edges.
** ===================================================================
*/

static double seg_dist (double px, double py, double ax, double ay,
                        double bx, double by) {
  double dx = bx - ax, dy = by - ay;
  double t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy);
  if (t < 0.0) t = 0.0;
  else if (t > 1.0) t = 1.0;
  dx = ax + t * dx - px;
  dy = ay + t * dy - py;
  return sqrt(dx * dx + dy * dy);
}


/* inside the half ellipse centered on (cx, m), radii rx, ry? */
static int in_ellipse (double x, double y, double cx, double m, double rx,
                       double ry) {
  double a, b;
  if (rx <= 0.0 || ry <= 0.0) return 0;
  a = (x - cx) / rx;
  b = (y - m) / ry;
  return a * a + b * b <= 1.0;
}


/* is the point (x, y) of a w x h cell inside the shape? t: line width */
static int pl_inside (uint32_t cp, double x, double y, double w, double h,
                      double t) {
  double m = h / 2.0, u = x / w, v = y / h, r = t / 2.0;
  switch (cp) {
    case 0xE0B0: return u <= 1.0 - fabs(2.0 * v - 1.0);	/* solid right arrow */
    case 0xE0B2: return u >= fabs(2.0 * v - 1.0);	/* solid left arrow */
    case 0xE0B1:	/* thin right arrow */
      return seg_dist(x, y, 0, 0, w, m) <= r || seg_dist(x, y, w, m, 0, h) <= r;
    case 0xE0B3:	/* thin left arrow */
      return seg_dist(x, y, w, 0, 0, m) <= r || seg_dist(x, y, 0, m, w, h) <= r;
    case 0xE0B4: return in_ellipse(x, y, 0, m, w, m);	/* solid right round */
    case 0xE0B6: return in_ellipse(x, y, w, m, w, m);	/* solid left round */
    case 0xE0B5:	/* thin right round */
      return in_ellipse(x, y, 0, m, w, m) && !in_ellipse(x, y, 0, m, w - t, m - t);
    case 0xE0B7:	/* thin left round */
      return in_ellipse(x, y, w, m, w, m) && !in_ellipse(x, y, w, m, w - t, m - t);
    case 0xE0B8: return u <= v;	/* lower left triangle */
    case 0xE0BA: return u >= 1.0 - v;	/* lower right triangle */
    case 0xE0BC: return u <= 1.0 - v;	/* upper left triangle */
    case 0xE0BE: return u >= v;	/* upper right triangle */
    case 0xE0B9: case 0xE0BF: return seg_dist(x, y, 0, 0, w, h) <= r;	/* \ */
    case 0xE0BB: case 0xE0BD: return seg_dist(x, y, w, 0, 0, h) <= r;	/* / */
  }
  return 0;
}


static int draw_powerline (Frame *f, uint32_t cp, int x, int y, int w, int h,
                           uint32_t fg) {
  double t = (double)h / 14.0;
  int i, j, si, sj;
  if (t < 1.0) t = 1.0;
  for (j = 0; j < h; j++) {
    if (y + j < 0 || y + j >= f->h) continue;
    for (i = 0; i < w; i++) {
      int n = 0;
      uint32_t *p;
      if (x + i < 0 || x + i >= f->w) continue;
      for (sj = 0; sj < 4; sj++)
        for (si = 0; si < 4; si++)
          n += pl_inside(cp, (double)i + (si + 0.5) / 4.0, (double)j + (sj + 0.5) / 4.0,
                         (double)w, (double)h, t);
      if (n == 0) continue;
      p = &f->px[(size_t)(y + j) * (size_t)f->w + (size_t)(x + i)];
      *p = mix(*p, fg, n * 255 / 16);
    }
  }
  return 1;
}

/* }================================================================== */


/*
** {==================================================================
** The terminal
** ===================================================================
*/

/* on the link or address the mouse points at? */
static int in_hot (const Scene *s, int x, int y) {
  if (!s->has_hot) return 0;
  if (y < s->hy0 || y > s->hy1) return 0;
  if (y == s->hy0 && x < s->hx0) return 0;
  if (y == s->hy1 && x > s->hx1) return 0;
  return 1;
}


static int in_selection (const Scene *s, int x, int y) {
  if (!s->has_sel) return 0;
  if (y < s->sy0 || y > s->sy1) return 0;
  if (y == s->sy0 && x < s->sx0) return 0;
  if (y == s->sy1 && x > s->sx1) return 0;
  return 1;
}


/* marks drawn over the character: accents, not joiners or emoji parts */
static int is_overlay (uint32_t cp) {
  return grid_wcwidth(cp) == 0 && cp != 0x200D && !(cp >= 0xFE00 && cp <= 0xFE0F) &&
         !(cp >= 0x1F3FB && cp <= 0x1F3FF) && !(cp >= 0xE0020 && cp <= 0xE007F);
}


/* a piece of an image, scaled from the cell size it was placed with */
static void draw_image_tile (Frame *f, const Scene *s, const Cell *c, int px, int py) {
  int tile, x, y, tx, ty;
  const GridImage *im = grid_image(s->g, c->ch, &tile);
  if (im == NULL) return;	/* dropped: too many images */
  tx = (tile % im->tw) * im->cw;
  ty = (tile / im->tw) * im->ch;
  for (y = 0; y < s->ch; y++) {
    int sy = ty + y * im->ch / s->ch, dy = py + y;
    if (sy >= im->h) break;
    if (dy < 0 || dy >= f->h) continue;
    for (x = 0; x < s->cw; x++) {
      int sx = tx + x * im->cw / s->cw, dx = px + x;
      uint32_t p, *d;
      unsigned a;
      if (sx >= im->w || dx >= f->w) break;
      if (dx < 0) continue;
      p = im->px[(size_t)sy * (size_t)im->w + (size_t)sx];
      a = p >> 24;
      if (a == 0) continue;	/* see-through */
      d = &f->px[(size_t)dy * (size_t)f->w + (size_t)dx];
      *d = (a == 255) ? (p & 0xFFFFFF) : mix(*d, p & 0xFFFFFF, (int)a);
    }
  }
}


/* the underline in its style (SGR 4:n), in its color (SGR 58) */
static void draw_underline (Frame *f, const Scene *s, const Cell *c, int px, int py,
                            int w, int line, uint32_t color) {
  int y = py + s->ascent + line + 1, x;
  switch ((c->attr & A_ULSTYLE) >> UL_SHIFT) {
    case 1:	/* double */
      fill(f, px, y, w, line, color);
      fill(f, px, y + 2 * line, w, line, color);
      break;
    case 2: {	/* curly: a wave that goes on from cell to cell */
      int amp = line + 1, period = s->cw > 4 ? s->cw : 4;
      for (x = 0; x < w; x++) {
        double t = (double)((px + x) % period) / (double)period;
        int dy = (int)((double)amp * (1.0 - cos(t * 6.283185307179586)) / 2.0 + 0.5);
        fill(f, px + x, y - 1 + dy, 1, line, color);
      }
      break;
    }
    case 3:	/* dotted */
      for (x = 0; x < w; x++)
        if (((px + x) / line) % 2 == 0) fill(f, px + x, y, 1, line, color);
      break;
    case 4:	/* dashed */
      for (x = 0; x < w; x++)
        if (((px + x) / (3 * line)) % 3 != 2) fill(f, px + x, y, 1, line, color);
      break;
    default: fill(f, px, y, w, line, color); break;
  }
}


/* the text of a cell; a glyph may reach out to [clip0, clip1) - italics
** and the colored ClearType edges need that. link: 1 a hyperlink is
** there (dotted line), 2 the mouse is on it (full line) */
static void draw_cell_fg (Frame *f, const Scene *s, const Cell *c, int px,
                          int py, uint32_t fg, uint32_t bg, int clip0,
                          int clip1, int link) {
  int w = s->cw * ((c->attr & A_WIDE) ? 2 : 1);
  int line = (s->ch + 8) / 16;
  uint32_t ch = grid_base(s->g, c);
  int bold = (c->attr & A_BOLD) && !s->no_bold, dark = dark_text(fg, bg);
  if ((c->ch & CH_IMAGE) && !(c->ch & CH_CLUSTER)) {
    draw_image_tile(f, s, c, px, py);
    return;
  }
  if (line < 1) line = 1;
  if (ch > ' ' && !(c->attr & A_HIDDEN) && (!(c->attr & A_BLINK) || s->text_blink_on)) {
    int drawn = 0;
    if (ch >= 0x2500 && ch <= 0x257F)
      drawn = draw_box(f, ch, px, py, w, s->ch, fg);
    else if (ch >= 0x2580 && ch <= 0x259F)
      drawn = draw_block(f, ch, px, py, w, s->ch, fg);
    else if (ch >= 0xE0B0 && ch <= 0xE0BF)
      drawn = draw_powerline(f, ch, px, py, w, s->ch, fg);
    if (!drawn)
      blit_glyph(f, font_glyph(ch, bold, c->attr & A_ITALIC, dark),
                 px, py + s->ascent, fg, bg, clip0, clip1);
    if (c->ch & CH_CLUSTER) {	/* the marks: centered over the character */
      const uint32_t *cps;
      int n = grid_cps(s->g, c, &cps), k;
      for (k = 1; k < n; k++) {
        const Glyph *m;
        if (!is_overlay(cps[k])) continue;
        m = font_glyph(cps[k], bold, c->attr & A_ITALIC, dark);
        if (m != NULL && m->bm != NULL)	/* fonts place marks differently */
          blit_glyph(f, m, px + w / 2 - m->xoff - m->w / 2, py + s->ascent, fg, bg,
                     clip0, clip1);
      }
    }
  }
  if (link == 2 && !(c->attr & A_UNDER))
    fill(f, px, py + s->ascent + line + 1, w, line, fg);
  else if (link == 1 && !(c->attr & A_UNDER)) {	/* dotted */
    int x;
    for (x = 0; x < w; x += 2) fill(f, px + x, py + s->ascent + line + 1, 1, line, mix(bg, fg, 160));
  }
  if (c->attr & A_UNDER)
    draw_underline(f, s, c, px, py, w, line,
                   c->ul != COL_DEFAULT ? theme_color(s->t, c->ul, 1) : fg);
  if (c->attr & A_STRIKE) fill(f, px, py + (s->ascent * 2) / 3, w, line, fg);
  if (c->attr & A_OVER) fill(f, px, py + 1, w, line, fg);
}


static void draw_cell (Frame *f, const Scene *s, const Cell *c, int px, int py,
                       uint32_t fg, uint32_t bg) {
  int w = s->cw * ((c->attr & A_WIDE) ? 2 : 1);
  fill(f, px, py, w, s->ch, bg);
  draw_cell_fg(f, s, c, px, py, fg, bg, px, px + w, 0);
}


static void cell_colors (const Scene *s, const Cell *c, int selected,
                         uint32_t *fg, uint32_t *bg) {
  *fg = theme_color(s->t, c->fg, 1);
  *bg = theme_color(s->t, c->bg, 0);
  if (c->attr & A_REVERSE) {
    uint32_t tmp = *fg;
    *fg = *bg;
    *bg = tmp;
  }
  if (c->attr & A_DIM) *fg = mix(*bg, *fg, 150);
  if (selected) {
    *bg = s->t->sel_bg;
    if (theme_contrast(*fg, *bg) < 2.5) *fg = s->t->fg;	/* keep it readable */
  }
}


/* where the grid of the scene starts */
static int grid_x (const Scene *s) {
  return s->ox > 0 ? s->ox : s->pad;
}


static int grid_y (const Scene *s) {
  return s->oy > 0 ? s->oy : s->head + s->bar + s->strip + s->pad;
}


static void draw_cursor (Frame *f, const Scene *s) {
  const Grid *g = s->g;
  const Line *l;
  Cell c;
  int shape = g->cursor_shape, style = s->cursor_style;
  int px, py, w;
  if (!g->cursor_on || g->view != 0) return;
  if (g->cy + g->view >= g->rows) return;
  if (shape >= 1) style = (shape <= 2) ? 0 : (shape <= 4) ? 1 : 2;
  l = &g->screen[g->cy];
  c = l->c[g->cx];
  if ((c.attr & A_WCONT) && g->cx > 0) c = l->c[g->cx - 1];
  px = grid_x(s) + ((c.attr & A_WIDE) && (l->c[g->cx].attr & A_WCONT)
                     ? g->cx - 1 : g->cx) * s->cw;
  py = grid_y(s) + g->cy * s->ch;
  w = s->cw * ((c.attr & A_WIDE) ? 2 : 1);
  if (!s->focused) {	/* hollow box */
    fill(f, px, py, w, 1, s->t->cursor);
    fill(f, px, py + s->ch - 1, w, 1, s->t->cursor);
    fill(f, px, py, 1, s->ch, s->t->cursor);
    fill(f, px + w - 1, py, 1, s->ch, s->t->cursor);
    return;
  }
  if (!s->blink_on) return;
  if (style == 1) fill(f, px, py + s->ch - 2 - s->ch / 12, w, 2 + s->ch / 12, s->t->cursor);
  else if (style == 2) fill(f, px, py, 2 + s->cw / 8, s->ch, s->t->cursor);
  else {
    c.attr &= (uint16_t)~(A_REVERSE | A_UNDER | A_STRIKE);
    draw_cell(f, s, &c, px, py, s->t->cursor_text, s->t->cursor);
  }
}


/* the thumb of the scrollbar, in pixels */
void draw_scrollbar_rect (const Frame *f, const Scene *s, int *x, int *y,
                          int *w, int *h) {
  const Grid *g = s->g;
  int track_y = s->head + s->bar + s->strip + 4, track_h = f->h - track_y - 4;
  int total = g->rows + g->sb_len;
  int th = (int)((long)track_h * g->rows / (total > 0 ? total : 1));
  if (th < 28) th = 28;
  if (th > track_h) th = track_h;
  *w = 6;
  *x = f->w - *w - 3;
  *h = th;
  *y = track_y + (g->sb_len > 0
         ? (int)((long)(track_h - th) * (g->sb_len - g->view) / g->sb_len) : 0);
}


static void draw_menu (Frame *f, const Scene *s) {
  const Menu *m = s->menu;
  const Theme *t = s->t;
  int i, y = m->y + 6, row = s->ch + 10;
  fill_round(f, m->x + 3, m->y + 5, m->w, m->h, 10, 0x000000, 70);	/* shadow */
  fill_round(f, m->x - 1, m->y - 1, m->w + 2, m->h + 2, 11, t->accent1, 150);
  fill_round(f, m->x, m->y, m->w, m->h, 10, t->ui, 255);
  for (i = 0; i < m->n; i++) {
    if (m->label[i] == NULL) {	/* separator */
      fill_alpha(f, m->x + 12, y + 4, m->w - 24, 1, t->ui_text, 50);
      y += 9;
      continue;
    }
    if (i == m->hot) fill_round(f, m->x + 5, y, m->w - 10, row, 7, t->accent1, 70);
    draw_text(f, m->x + 16, y + 5, m->label[i], t->ui_text, t->ui, 0);
    if (m->hint[i] != NULL)
      draw_text(f, m->x + m->w - 16 - draw_text_width(m->hint[i]), y + 5,
                m->hint[i], mix(t->ui, t->ui_text, 120), t->ui, 0);
    y += row;
  }
}


/*
** {==================================================================
** Our own title bar: the logo, the title, and hide / zoom / close
** ===================================================================
*/

/* button i (0 hide, 1 zoom, 2 close), counted from the right edge */
void draw_button_rect (const Frame *f, const Scene *s, int i, int *x, int *y,
                       int *w, int *h) {
  int d = s->head * 14 / 30, gap = s->head * 24 / 30;
  *w = *h = d;
  *y = (s->head - d) / 2;
  *x = f->w - s->head * 14 / 30 - d - (2 - i) * gap;
}


/* a line as a row of small squares: good enough for tiny icons */
static void stroke (Frame *f, int x0, int y0, int x1, int y1, int t, uint32_t c) {
  int dx = x1 - x0, dy = y1 - y0, i;
  int n = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx)
                                                    : (dy < 0 ? -dy : dy);
  if (n == 0) n = 1;
  for (i = 0; i <= n; i++)
    fill(f, x0 + dx * i / n - t / 2, y0 + dy * i / n - t / 2, t, t, c);
}


/* the logo: a diamond outline from blue to green, an M, a green cursor */
static void draw_logo (Frame *f, const Theme *t, int cx, int cy, int r) {
  int x, y, th = r / 4 + 1, k = r / 3, line = r / 7 + 1;
  for (y = -r; y <= r; y++) {
    for (x = -r; x <= r; x++) {
      int d = (x < 0 ? -x : x) + (y < 0 ? -y : y);
      int px = cx + x, py = cy + y;
      if (d > r || px < 0 || py < 0 || px >= f->w || py >= f->h) continue;
      f->px[(size_t)py * (size_t)f->w + (size_t)px] =
        (d > r - th) ? mix(t->accent1, t->accent2, (x + r) * 255 / (2 * r))
                     : 0x0B1220;
    }
  }
  stroke(f, cx - k, cy + k / 2, cx - k, cy - k, line, 0xFFFFFF);
  stroke(f, cx - k, cy - k, cx, cy, line, 0xFFFFFF);
  stroke(f, cx, cy, cx + k, cy - k, line, 0xFFFFFF);
  stroke(f, cx + k, cy - k, cx + k, cy + k / 2, line, 0xFFFFFF);
  fill(f, cx - k / 2, cy + k + line, k + 1, line, t->accent2);
}


static void draw_header (Frame *f, const Scene *s) {
  /* amber, not pale yellow: the white sign has to show on it */
  static const uint32_t colors[3] = {0xF29F2C, 0x22D36B, 0xFF5C7A};
  const Theme *t = s->t;
  uint32_t text = s->focused ? t->ui_text : mix(t->ui, t->ui_text, 120);
  int i, bx, by, bw, bh, tx, room;
  const char *p = s->title ? s->title : "";
  fill(f, 0, 0, f->w, s->head, t->ui);
  draw_logo(f, t, s->head * 18 / 30, s->head / 2, s->head * 10 / 30);
  draw_button_rect(f, s, 0, &bx, &by, &bw, &bh);
  tx = s->head * 36 / 30;
  room = (bx - s->head / 2 - tx) / s->cw;	/* cells left for the title */
  while (*p != '\0' && room > 0) {
    uint32_t cp = next_cp(&p);
    int wide = grid_wcwidth(cp);
    if (wide > room) break;
    blit_glyph(f, font_glyph(cp, 0, 0, dark_text(text, t->ui)), tx,
               (s->head - s->ch) / 2 + s->ascent, text, t->ui, 0, f->w);
    tx += wide * s->cw;
    room -= wide;
  }
  for (i = 0; i < 3; i++) {
    uint32_t c = s->focused ? colors[i] : mix(t->ui, t->ui_text, 70);
    int m, line = bw / 8 + 1;
    draw_button_rect(f, s, i, &bx, &by, &bw, &bh);
    if (i == s->hot_button) {
      c = colors[i];
      fill_round(f, bx - 3, by - 3, bw + 6, bh + 6, (bw + 6) / 2, c, 70);	/* glow */
    }
    fill_round(f, bx, by, bw, bh, bw / 2, c, 255);
    m = (bw * 30 + 50) / 100;	/* the white sign, with air around it */
    line = (bw - 2 * m + 3) / 6;	/* thin strokes keep small shapes clear */
    if (line < 1) line = 1;
    if (i == 0) fill(f, bx + m, by + bh / 2 - line / 2, bw - 2 * m, line, 0xFFFFFF);
    else if (i == 1) {
      int q = s->maximized ? m + 1 : m;
      fill(f, bx + q, by + q, bw - 2 * q, line, 0xFFFFFF);
      fill(f, bx + q, by + bh - q - line, bw - 2 * q, line, 0xFFFFFF);
      fill(f, bx + q, by + q, line, bh - 2 * q, 0xFFFFFF);
      fill(f, bx + bw - q - line, by + q, line, bh - 2 * q, 0xFFFFFF);
    }
    else {
      stroke(f, bx + m, by + m, bx + bw - m - 1, by + bh - m - 1, line, 0xFFFFFF);
      stroke(f, bx + bw - m - 1, by + m, bx + m, by + bh - m - 1, line, 0xFFFFFF);
    }
  }
}

/* }================================================================== */


/*
** {==================================================================
** The tab bar: one tab per shell, and a + button; only there with 2+
** ===================================================================
*/

/* tab i, or the + button when i == ntabs; tabs share the width equally */
void draw_tab_rect (const Frame *f, const Scene *s, int i, int *x, int *y,
                    int *w, int *h) {
  int side = s->bar / 3, gap = s->bar / 8 + 1, top = s->bar / 5;
  int plus = s->bar - top - 6;
  int room = f->w - 2 * side - plus - gap;
  int tw = room / (s->ntabs > 0 ? s->ntabs : 1);
  if (tw > 26 * s->cw) tw = 26 * s->cw;
  *y = s->head + top;
  *h = s->bar - top;
  if (i < s->ntabs) {
    *x = side + i * tw;
    *w = tw - gap;
  }
  else {
    *x = side + s->ntabs * tw;
    *y += (*h - plus) / 2;
    *w = *h = plus;
  }
}


/* the x at the right end of tab i */
void draw_tab_close_rect (const Frame *f, const Scene *s, int i, int *x,
                          int *y, int *w, int *h) {
  int tx, ty, tw, th, c;
  draw_tab_rect(f, s, i, &tx, &ty, &tw, &th);
  c = th * 9 / 20;
  *x = tx + tw - (th - c) / 2 - c;
  *y = ty + (th - c) / 2;
  *w = *h = c;
}


/* as much of the text as fits in 'room' pixels, ending in ... if cut */
static void draw_text_fit (Frame *f, int x, int y, const char *utf8, int room,
                           uint32_t fg, uint32_t bg) {
  const char *p = utf8;
  if (draw_text_width(utf8) <= room) {
    draw_text(f, x, y, utf8, fg, bg, 0);
    return;
  }
  room -= font_cell_w();	/* keep a cell for the ellipsis */
  while (*p != '\0') {
    uint32_t cp = next_cp(&p);
    int w = font_cell_w() * grid_wcwidth(cp);
    if (w > room) break;
    blit_glyph(f, font_glyph(cp, 0, 0, dark_text(fg, bg)), x, y + font_ascent(),
               fg, bg, 0, f->w);
    x += w;
    room -= w;
  }
  if (room >= 0)
    blit_glyph(f, font_glyph(0x2026, 0, 0, dark_text(fg, bg)), x,
               y + font_ascent(), fg, bg, 0, f->w);
}


static void draw_tabs (Frame *f, const Scene *s) {
  const Theme *t = s->t;
  uint32_t dim = mix(t->ui, t->ui_text, s->focused ? 150 : 110);
  int i, x, y, w, h;
  fill(f, 0, s->head, f->w, s->bar, t->ui);
  for (i = 0; i < s->ntabs; i++) {
    int active = (i == s->cur_tab), hot = (i == s->hot_tab);
    int r, cx, cy, cw, chh, line;
    uint32_t bg = active ? t->bg : t->ui;
    uint32_t fg = active ? t->fg : hot ? t->ui_text : dim;
    draw_tab_rect(f, s, i, &x, &y, &w, &h);
    if (w < 4) continue;
    r = h / 4;
    /* the active tab runs down into the terminal: the same color joins them */
    if (active) fill_round(f, x, y, w, h + r, r, t->bg, 255);
    else if (hot) fill_round(f, x, y, w, h - 2, r, t->accent1, 40);
    draw_tab_close_rect(f, s, i, &cx, &cy, &cw, &chh);
    if (active && s->edit != NULL) {	/* being renamed: the end of the text, a cursor */
      const char *p = s->edit;
      int room = cx - x - 2 * s->cw - 4, ty = y + (h - s->ch) / 2;
      fill_round(f, x + 3, y + 3, w - 6, h - 6, r, t->accent1, 60);
      fill_round(f, x + 4, y + 4, w - 8, h - 8, r, t->bg, 255);
      while (*p != '\0' && draw_text_width(p) > room) next_cp(&p);
      draw_text(f, x + s->cw, ty, p, t->fg, t->bg, 0);
      if (s->blink_on) fill(f, x + s->cw + draw_text_width(p), ty, 2, s->ch, t->accent1);
    }
    else if (w > 5 * s->cw)
      draw_text_fit(f, x + s->cw, y + (h - s->ch) / 2,
                    s->tab_title[i] ? s->tab_title[i] : "", cx - x - s->cw - 4, fg, bg);
    if (active || hot) {	/* the x */
      int m = cw / 4;
      line = cw / 9 + 1;
      if (hot && s->hot_close)
        fill_round(f, cx - 2, cy - 2, cw + 4, chh + 4, (cw + 4) / 2, t->accent1, 80);
      stroke(f, cx + m, cy + m, cx + cw - m - 1, cy + chh - m - 1, line, fg);
      stroke(f, cx + cw - m - 1, cy + m, cx + m, cy + chh - m - 1, line, fg);
    }
    else if (s->tab_news[i]) {	/* a dot: something happened in there */
      int d = cw / 2;
      fill_round(f, cx + (cw - d) / 2, cy + (chh - d) / 2, d, d, d / 2, t->accent2, 255);
    }
  }
  draw_tab_rect(f, s, s->ntabs, &x, &y, &w, &h);
  if (s->hot_tab == s->ntabs) fill_round(f, x, y, w, h, w / 2, t->accent1, 60);
  {
    int line = w / 10 + 1, len = w / 2;
    uint32_t c = s->hot_tab == s->ntabs ? t->ui_text : dim;
    fill(f, x + (w - len) / 2, y + (h - line) / 2, len, line, c);
    fill(f, x + (w - line) / 2, y + (h - len) / 2, line, len, c);
  }
}

/* }================================================================== */


/* is the line on this row of the view new since the last drawing? */
static int row_dirty (const Grid *g, int row) {
  const Line *l;
  if (row < 0 || row >= g->rows) return 0;
  l = grid_view_line(g, row);
  return l != NULL && l->dirty;
}


/*
** The rows of s->g, and its cursor. partial: only rows whose line is
** dirty, and the rows next to them (a glyph may reach into those), are
** painted again, over a fresh background from x0 to x1; *y0 and *y1 grow
** to hold the pixels that changed.
*/
static void draw_terminal (Frame *f, const Scene *s, int partial, int x0, int x1,
                           int *y0, int *y1) {
  const Grid *g = s->g;
  int row, x, gx = grid_x(s), cursor_row = g->cy + g->view;
  for (row = 0; row < g->rows; row++) {
    const Line *l = grid_view_line(g, row);
    int y = row - g->view;	/* in grid_line() terms, for the selection */
    int py = grid_y(s) + row * s->ch;
    if (partial) {
      if (!row_dirty(g, row) && !row_dirty(g, row - 1) && !row_dirty(g, row + 1)) continue;
      fill(f, x0, py, x1 - x0, s->ch, s->t->bg);
      if (py < *y0) *y0 = py;
      if (py + s->ch > *y1) *y1 = py + s->ch;
    }
    if (l == NULL) continue;
    /* backgrounds first, then the text: a glyph may lean into the next
    ** cell (italics, ClearType edges) without being painted over */
    for (x = 0; x < g->cols && x < l->n; x++) {
      const Cell *c = &l->c[x];
      uint32_t fg, bg;
      int sel = in_selection(s, x, y);
      if (c->attr & A_WCONT) continue;
      if (c->bg == COL_DEFAULT && !sel && !(c->attr & A_REVERSE)) continue;
      cell_colors(s, c, sel, &fg, &bg);
      fill(f, gx + x * s->cw, py, s->cw * ((c->attr & A_WIDE) ? 2 : 1), s->ch, bg);
    }
    for (x = 0; x < g->cols && x < l->n; x++) {
      const Cell *c = &l->c[x];
      uint32_t fg, bg;
      int sel = in_selection(s, x, y), px = gx + x * s->cw;
      if (c->attr & A_WCONT) continue;
      if (c->ch <= ' ' && !(c->attr & (A_UNDER | A_STRIKE | A_OVER)) && c->link == 0) continue;
      cell_colors(s, c, sel, &fg, &bg);
      draw_cell_fg(f, s, c, px, py, fg, bg, px - s->cw / 2,
                   px + s->cw * ((c->attr & A_WIDE) ? 2 : 1) + s->cw / 2,
                   in_hot(s, x, y) ? 2 : c->link != 0 ? 1 : 0);
    }
  }
  if (!partial || row_dirty(g, cursor_row) || row_dirty(g, cursor_row - 1) ||
      row_dirty(g, cursor_row + 1))
    draw_cursor(f, s);
}


int draw_scene_rows (Frame *f, const Scene *s) {
  int y0 = f->h, y1 = 0, i;
  if (s->npanes < 2) draw_terminal(f, s, 1, 0, f->w, &y0, &y1);
  else {
    for (i = 0; i < s->npanes; i++) {
      Scene ps = *s;
      ps.g = s->pane[i].g;
      ps.t = s->pane[i].t;
      ps.ox = s->pane[i].x;
      ps.oy = s->pane[i].y;
      if (!s->pane[i].focused) {
        ps.has_sel = ps.has_hot = 0;
        ps.focused = 0;
      }
      draw_terminal(f, &ps, 1, ps.ox, ps.ox + s->pane[i].w, &y0, &y1);
    }
  }
  f->dy = y0 < y1 ? y0 : 0;
  f->dh = y0 < y1 ? y1 - y0 : 0;
  return f->dh > 0;
}


void draw_scene (Frame *f, const Scene *s) {
  const Grid *g = s->g;
  const Theme *t = s->t;
  int x, i;
  fill(f, 0, 0, f->w, f->h, t->bg);
  if (s->head > 0) draw_header(f, s);
  if (s->bar > 0) draw_tabs(f, s);
  for (x = 0; x < f->w; x++) {	/* the logo gradient: blue to green */
    uint32_t c = mix(t->accent1, t->accent2, f->w > 1 ? x * 255 / (f->w - 1) : 0);
    fill(f, x, s->head + s->bar, 1, s->strip, c);
  }
  f->dy = 0;
  f->dh = f->h;
  if (s->npanes < 2) draw_terminal(f, s, 0, 0, 0, NULL, NULL);
  else {
    for (i = 0; i < s->npanes; i++) {	/* each with its own colors; only one has the focus */
      Scene ps = *s;
      ps.g = s->pane[i].g;
      ps.t = s->pane[i].t;
      ps.ox = s->pane[i].x;
      ps.oy = s->pane[i].y;
      if (!s->pane[i].focused) {
        ps.has_sel = ps.has_hot = 0;
        ps.focused = 0;	/* a hollow cursor */
      }
      if (ps.t->bg != t->bg) fill(f, ps.ox, ps.oy, s->pane[i].w, s->pane[i].h, ps.t->bg);
      draw_terminal(f, &ps, 0, 0, 0, NULL, NULL);
    }
    for (i = 0; i < s->ndivs; i++) {	/* a thin line in the middle of the gap */
      const int *d = s->div[i];
      uint32_t c = mix(t->bg, t->fg, 70);
      if (d[2] >= d[3]) fill(f, d[0], d[1] + d[3] / 2, d[2], 1, c);
      else fill(f, d[0] + d[2] / 2, d[1], 1, d[3], c);
    }
  }
  if (s->bar_alpha > 0 && g->sb_len > 0 && !g->alt) {
    int bx, by, bw, bh;
    draw_scrollbar_rect(f, s, &bx, &by, &bw, &bh);
    fill_round(f, bx, by, bw, bh, 3, t->accent1, s->bar_alpha);
  }
  if (s->pill != NULL) {
    int tw = draw_text_width(s->pill), w = tw + 36, h = s->ch + 20;
    int px = (f->w - w) / 2, py = (f->h - h) / 2;
    fill_round(f, px - 1, py - 1, w + 2, h + 2, h / 2 + 1, t->accent1, 200);
    fill_round(f, px, py, w, h, h / 2, t->ui, 255);
    draw_text(f, px + 18, py + 10, s->pill, t->ui_text, t->ui, 0);
  }
  if (s->find != NULL) {	/* the search box, top right */
    int tw = draw_text_width(s->find), w = tw + 28, h = s->ch + 14;
    int px = f->w - w - s->pad - 18, py = s->head + s->bar + s->strip + 8;
    if (px < 4) px = 4;
    fill_round(f, px - 1, py - 1, w + 2, h + 2, 8, t->accent1, 220);
    fill_round(f, px, py, w, h, 7, t->ui, 255);
    draw_text(f, px + 14, py + 7, s->find, t->ui_text, t->ui, 0);
  }
  if (s->menu != NULL && s->menu->open) draw_menu(f, s);
}

/* }================================================================== */
