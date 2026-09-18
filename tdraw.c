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


static void blit_glyph (Frame *f, const Glyph *g, int x, int y, uint32_t fg,
                        uint32_t bg, int clip_x0, int clip_x1) {
  const unsigned char *lut;
  int i, j;
  if (g == NULL || g->bm == NULL) return;
  if (!lut_ready) make_luts();
  lut = (brightness(fg) > brightness(bg)) ? lut_light : lut_dark;
  for (j = 0; j < g->h; j++) {
    int py = y + g->yoff + j;
    if (py < 0 || py >= f->h) continue;
    for (i = 0; i < g->w; i++) {
      int px = x + g->xoff + i;
      int a = g->bm[(size_t)j * (size_t)g->w + (size_t)i];
      uint32_t *p;
      if (a == 0 || px < clip_x0 || px >= clip_x1 || px < 0 || px >= f->w) continue;
      p = &f->px[(size_t)py * (size_t)f->w + (size_t)px];
      *p = mix(*p, fg, lut[a]);
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
    blit_glyph(f, font_glyph(cp, bold, 0), x, y + font_ascent(), fg, bg, 0, f->w);
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
** The terminal
** ===================================================================
*/

static int in_selection (const Scene *s, int x, int y) {
  if (!s->has_sel) return 0;
  if (y < s->sy0 || y > s->sy1) return 0;
  if (y == s->sy0 && x < s->sx0) return 0;
  if (y == s->sy1 && x > s->sx1) return 0;
  return 1;
}


static void draw_cell (Frame *f, const Scene *s, const Cell *c, int px, int py,
                       uint32_t fg, uint32_t bg) {
  int w = s->cw * ((c->attr & A_WIDE) ? 2 : 1);
  int line = (s->ch + 8) / 16;
  if (line < 1) line = 1;
  fill(f, px, py, w, s->ch, bg);
  if (c->ch > ' ' && !(c->attr & A_HIDDEN)) {
    int drawn = 0;
    if (c->ch >= 0x2500 && c->ch <= 0x257F)
      drawn = draw_box(f, c->ch, px, py, w, s->ch, fg);
    else if (c->ch >= 0x2580 && c->ch <= 0x259F)
      drawn = draw_block(f, c->ch, px, py, w, s->ch, fg);
    if (!drawn)
      blit_glyph(f, font_glyph(c->ch, (c->attr & A_BOLD) && !s->no_bold,
                                  c->attr & A_ITALIC),
                 px, py + s->ascent, fg, bg, px, px + w);
  }
  if (c->attr & A_UNDER) fill(f, px, py + s->ascent + line + 1, w, line, fg);
  if (c->attr & A_STRIKE) fill(f, px, py + (s->ascent * 2) / 3, w, line, fg);
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
  px = s->pad + ((c.attr & A_WIDE) && (l->c[g->cx].attr & A_WCONT)
                   ? g->cx - 1 : g->cx) * s->cw;
  py = s->head + s->strip + s->pad + g->cy * s->ch;
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
  int track_y = s->head + s->strip + 4, track_h = f->h - track_y - 4;
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
    blit_glyph(f, font_glyph(cp, 0, 0), tx, (s->head - s->ch) / 2 + s->ascent,
               text, t->ui, 0, f->w);
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


void draw_scene (Frame *f, const Scene *s) {
  const Grid *g = s->g;
  const Theme *t = s->t;
  int row, x;
  fill(f, 0, 0, f->w, f->h, t->bg);
  if (s->head > 0) draw_header(f, s);
  for (x = 0; x < f->w; x++) {	/* the logo gradient: blue to green */
    uint32_t c = mix(t->accent1, t->accent2, f->w > 1 ? x * 255 / (f->w - 1) : 0);
    fill(f, x, s->head, 1, s->strip, c);
  }
  for (row = 0; row < g->rows; row++) {
    const Line *l = grid_view_line(g, row);
    int y = row - g->view;	/* in grid_line() terms, for the selection */
    int py = s->head + s->strip + s->pad + row * s->ch;
    if (l == NULL) continue;
    for (x = 0; x < g->cols && x < l->n; x++) {
      const Cell *c = &l->c[x];
      uint32_t fg, bg;
      int sel = in_selection(s, x, y);
      if (c->attr & A_WCONT) continue;
      if (c->ch <= ' ' && c->bg == COL_DEFAULT && !sel &&
          !(c->attr & (A_REVERSE | A_UNDER | A_STRIKE)))
        continue;	/* plain background: already there */
      cell_colors(s, c, sel, &fg, &bg);
      draw_cell(f, s, c, s->pad + x * s->cw, py, fg, bg);
    }
  }
  draw_cursor(f, s);
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
  if (s->menu != NULL && s->menu->open) draw_menu(f, s);
}

/* }================================================================== */
