/*
** tgrid.c - screen model of mmc-term
**
** A grid of cells with a cursor, a scroll region, an alternate screen
** and a ring of scrolled-off lines. Knows nothing about escape
** sequences (tvt.c) or pixels (tdraw.c).
*/

#include "mterm.h"

#include <stdlib.h>
#include <string.h>


static Cell blank_cell (const Grid *g) {
  Cell c;
  c.ch = 0;
  c.fg = COL_DEFAULT;
  c.bg = g ? g->pen.bg : COL_DEFAULT;	/* erased cells keep the background */
  c.attr = 0;
  c.link = 0;
  c.ul = COL_DEFAULT;
  return c;
}


static void line_fill (const Grid *g, Line *l, int from, int to) {
  Cell b = blank_cell(g);
  int i;
  for (i = from; i < to && i < l->n; i++) l->c[i] = b;
  l->dirty = 1;
}


static void line_init (const Grid *g, Line *l, int n) {
  l->c = (Cell *)xmalloc((size_t)n * sizeof(Cell));
  l->n = n;
  l->wrapped = 0;
  line_fill(g, l, 0, n);
}


static void line_set_cols (const Grid *g, Line *l, int n) {
  int old = l->n;
  l->c = (Cell *)xrealloc(l->c, (size_t)n * sizeof(Cell));
  l->n = n;
  if (n > old) line_fill(g, l, old, n);
  else if (n > 0 && (l->c[n - 1].attr & A_WIDE)) {	/* cut a wide char */
    l->c[n - 1].ch = 0;
    l->c[n - 1].attr &= (uint16_t)~A_WIDE;
  }
  l->dirty = 1;
}


static int line_is_blank (const Line *l) {
  int i;
  for (i = 0; i < l->n; i++)
    if (l->c[i].ch != 0 || l->c[i].bg != COL_DEFAULT) return 0;
  return 1;
}


static void set_tabs (Grid *g) {
  int i;
  g->tabs = (unsigned char *)xrealloc(g->tabs, (size_t)g->cols);
  for (i = 0; i < g->cols; i++) g->tabs[i] = (i % 8 == 0);
}


static Line *new_screen (const Grid *g, int cols, int rows) {
  Line *s = (Line *)xmalloc((size_t)rows * sizeof(Line));
  int y;
  for (y = 0; y < rows; y++) line_init(g, &s[y], cols);
  return s;
}


Grid *grid_new (int cols, int rows, int scrollback) {
  Grid *g = (Grid *)xmalloc(sizeof(Grid));
  memset(g, 0, sizeof(*g));
  g->cols = cols < 2 ? 2 : cols;
  g->rows = rows < 1 ? 1 : rows;
  g->cell_w = 10;
  g->cell_h = 20;
  g->sb_cap = scrollback < 0 ? 0 : scrollback;
  if (g->sb_cap > 0) {
    g->sb = (Line *)xmalloc((size_t)g->sb_cap * sizeof(Line));
    memset(g->sb, 0, (size_t)g->sb_cap * sizeof(Line));
  }
  g->screen = new_screen(g, g->cols, g->rows);
  g->other = new_screen(g, g->cols, g->rows);
  grid_reset(g);
  return g;
}


void grid_free (Grid *g) {
  int i;
  if (g == NULL) return;
  for (i = 0; i < g->rows; i++) {
    free(g->screen[i].c);
    free(g->other[i].c);
  }
  for (i = 0; i < g->sb_cap; i++) free(g->sb[i].c);
  free(g->screen);
  free(g->other);
  free(g->sb);
  free(g->tabs);
  free(g->clu);
  free(g->clu_hash);
  for (i = 0; i < g->nimages; i++) free(g->images[i].px);
  free(g->images);
  for (i = 0; i < g->nlinks; i++) free(g->links[i]);
  free(g->links);
  free(g);
}


/* RIS: back to the power-on state (the scrollback is kept) */
void grid_reset (Grid *g) {
  int y;
  if (g->alt) grid_set_alt(g, 0, 0);
  g->pen = blank_cell(NULL);
  for (y = 0; y < g->rows; y++) {
    line_fill(g, &g->screen[y], 0, g->cols);
    g->screen[y].wrapped = 0;
  }
  g->cx = g->cy = g->wrap_next = 0;
  g->top = 0;
  g->bot = g->rows - 1;
  g->autowrap = 1;
  g->cursor_on = 1;
  g->app_cursor = g->bracketed = g->focus_events = g->insert = 0;
  g->mouse = g->mouse_sgr = 0;
  g->cursor_shape = 0;
  g->sync = g->join_next = g->origin = 0;
  g->kitty_n[0] = g->kitty_n[1] = 0;
  g->view = 0;
  memset(g->saved_cx, 0, sizeof(g->saved_cx));
  memset(g->saved_cy, 0, sizeof(g->saved_cy));
  g->saved_pen[0] = g->saved_pen[1] = g->pen;
  set_tabs(g);
  g->all_dirty = 1;
}


/*
** {==================================================================
** Scrollback
** ===================================================================
*/

/* takes ownership of the cells of 'l' */
static void sb_push (Grid *g, Line *l) {
  Line *slot;
  if (g->sb_cap == 0) {
    free(l->c);
    return;
  }
  if (g->sb_len == g->sb_cap) {	/* full: the oldest line goes */
    free(g->sb[g->sb_head].c);
    g->sb_head = (g->sb_head + 1) % g->sb_cap;
    g->sb_len--;
  }
  slot = &g->sb[(g->sb_head + g->sb_len) % g->sb_cap];
  *slot = *l;
  g->sb_len++;
  if (g->view > 0 && g->view < g->sb_len) g->view++;	/* keep the view still */
}


const Line *grid_line (const Grid *g, int y) {
  if (y >= 0) return (y < g->rows) ? &g->screen[y] : NULL;
  if (-y > g->sb_len) return NULL;
  return &g->sb[(g->sb_head + g->sb_len + y) % g->sb_cap];
}


const Line *grid_view_line (const Grid *g, int row) {
  return grid_line(g, row - g->view);
}


void grid_set_view (Grid *g, int view) {
  if (view > g->sb_len) view = g->sb_len;
  if (view < 0 || g->alt) view = 0;
  if (view != g->view) {
    g->view = view;
    g->all_dirty = 1;
  }
}

/* }================================================================== */


/*
** {==================================================================
** Scrolling inside the region
** ===================================================================
*/

void grid_scroll_up (Grid *g, int n) {
  int span = g->bot - g->top + 1;
  if (n > span) n = span;
  for (; n > 0; n--) {
    Line first = g->screen[g->top];
    if (!g->alt && g->top == 0) {	/* leaves through the top: remember it */
      sb_push(g, &first);
      line_init(g, &first, g->cols);
    }
    else {
      line_fill(g, &first, 0, first.n);
      first.wrapped = 0;
    }
    memmove(&g->screen[g->top], &g->screen[g->top + 1],
            (size_t)(span - 1) * sizeof(Line));
    g->screen[g->bot] = first;
  }
  g->all_dirty = 1;
}


void grid_scroll_down (Grid *g, int n) {
  int span = g->bot - g->top + 1;
  if (n > span) n = span;
  for (; n > 0; n--) {
    Line last = g->screen[g->bot];
    line_fill(g, &last, 0, last.n);
    last.wrapped = 0;
    memmove(&g->screen[g->top + 1], &g->screen[g->top],
            (size_t)(span - 1) * sizeof(Line));
    g->screen[g->top] = last;
  }
  g->all_dirty = 1;
}


void grid_set_region (Grid *g, int top, int bot) {
  if (top < 0) top = 0;
  if (bot >= g->rows || bot < 0) bot = g->rows - 1;
  if (top >= bot) {
    top = 0;
    bot = g->rows - 1;
  }
  g->top = top;
  g->bot = bot;
  grid_move(g, 0, g->origin ? top : 0);	/* home, which is the region's top in origin mode */
}


/* IL / DL: like scrolling a region that starts at the cursor line */
void grid_insert_lines (Grid *g, int n) {
  int top = g->top;
  if (g->cy < g->top || g->cy > g->bot) return;
  g->top = g->cy;
  grid_scroll_down(g, n);
  g->top = top;
  g->cx = 0;
  g->wrap_next = 0;
}


void grid_delete_lines (Grid *g, int n) {
  int top = g->top, alt = g->alt;
  if (g->cy < g->top || g->cy > g->bot) return;
  g->top = g->cy;
  g->alt = 1;	/* deleted lines are not history */
  grid_scroll_up(g, n);
  g->alt = alt;
  g->top = top;
  g->cx = 0;
  g->wrap_next = 0;
}

/* }================================================================== */


/*
** {==================================================================
** Cursor
** ===================================================================
*/

void grid_move (Grid *g, int x, int y) {
  if (x < 0) x = 0;
  if (x >= g->cols) x = g->cols - 1;
  if (y < 0) y = 0;
  if (y >= g->rows) y = g->rows - 1;
  if (y != g->cy) g->screen[g->cy].dirty = 1;
  g->cx = x;
  g->cy = y;
  g->wrap_next = g->join_next = 0;
  g->screen[g->cy].dirty = 1;
}


void grid_cr (Grid *g) {
  grid_move(g, 0, g->cy);
}


void grid_lf (Grid *g) {
  g->wrap_next = 0;
  if (g->cy == g->bot) grid_scroll_up(g, 1);
  else if (g->cy < g->rows - 1) grid_move(g, g->cx, g->cy + 1);
}


void grid_ri (Grid *g) {
  g->wrap_next = 0;
  if (g->cy == g->top) grid_scroll_down(g, 1);
  else if (g->cy > 0) grid_move(g, g->cx, g->cy - 1);
}


void grid_bs (Grid *g) {
  if (g->cx > 0) grid_move(g, g->cx - 1, g->cy);
  else g->wrap_next = 0;
}


/* n > 0: forward n tab stops; n < 0: backward */
void grid_tab (Grid *g, int n) {
  int x = g->cx;
  for (; n > 0; n--) {
    do x++; while (x < g->cols - 1 && !g->tabs[x]);
    if (x >= g->cols - 1) {
      x = g->cols - 1;
      break;
    }
  }
  for (; n < 0; n++) {
    do x--; while (x > 0 && !g->tabs[x]);
    if (x <= 0) {
      x = 0;
      break;
    }
  }
  grid_move(g, x, g->cy);
}


void grid_save_cursor (Grid *g) {
  g->saved_cx[g->alt] = g->cx;
  g->saved_cy[g->alt] = g->cy;
  g->saved_pen[g->alt] = g->pen;
}


void grid_restore_cursor (Grid *g) {
  g->pen = g->saved_pen[g->alt];
  grid_move(g, g->saved_cx[g->alt], g->saved_cy[g->alt]);
}

/* }================================================================== */


/*
** {==================================================================
** Writing and erasing
** ===================================================================
*/

int grid_kitty (const Grid *g) {
  int n = g->kitty_n[g->alt];
  return n > 0 ? g->kitty[g->alt][n - 1] : 0;
}


int grid_wcwidth (uint32_t ch) {
  return uc_width(ch);
}


/*
** {==================================================================
** Clusters: a character and the marks joined to it (e + U+0301 is é,
** an emoji and a skin tone, emoji joined with U+200D) share one cell.
** Every different cluster is kept once, for as long as the grid lives.
** ===================================================================
*/

#define CLU_MAX_CPS	16
#define CLU_MAX_SIZE	(1 << 22)


static uint32_t clu_hash_of (const uint32_t *cp, int n) {
  uint32_t h = 2166136261u;
  int i;
  for (i = 0; i < n; i++) {
    h ^= cp[i];
    h *= 16777619u;
  }
  return h;
}


static void clu_rehash (Grid *g, int cap) {
  int off, *old = g->clu_hash, oldcap = g->clu_hcap, i;
  g->clu_hash = (int *)xmalloc((size_t)cap * sizeof(int));
  memset(g->clu_hash, 0, (size_t)cap * sizeof(int));
  g->clu_hcap = cap;
  for (i = 0; i < oldcap; i++) {
    uint32_t h;
    if (old[i] == 0) continue;
    off = old[i] - 1;
    h = clu_hash_of(g->clu + off + 1, (int)g->clu[off]) & (uint32_t)(cap - 1);
    while (g->clu_hash[h] != 0) h = (h + 1) & (uint32_t)(cap - 1);
    g->clu_hash[h] = old[i];
  }
  free(old);
}


/* the place of this cluster in g->clu, added if new; -1 when all is full */
static int clu_intern (Grid *g, const uint32_t *cp, int n) {
  uint32_t h;
  if (g->clu_hcap < 2 * (g->nclu + n + 1)) {	/* entries are 3 or more long */
    int cap = g->clu_hcap ? g->clu_hcap : 256;
    while (cap < 2 * (g->nclu + n + 1)) cap *= 2;
    clu_rehash(g, cap);
  }
  h = clu_hash_of(cp, n) & (uint32_t)(g->clu_hcap - 1);
  while (g->clu_hash[h] != 0) {
    int off = g->clu_hash[h] - 1;
    if ((int)g->clu[off] == n && memcmp(g->clu + off + 1, cp, (size_t)n * sizeof(uint32_t)) == 0)
      return off;
    h = (h + 1) & (uint32_t)(g->clu_hcap - 1);
  }
  if (g->nclu + n + 1 > CLU_MAX_SIZE) return -1;
  if (g->nclu + n + 1 > g->capclu) {
    while (g->nclu + n + 1 > g->capclu) g->capclu = g->capclu ? g->capclu * 2 : 256;
    g->clu = (uint32_t *)xrealloc(g->clu, (size_t)g->capclu * sizeof(uint32_t));
  }
  g->clu[g->nclu] = (uint32_t)n;
  memcpy(g->clu + g->nclu + 1, cp, (size_t)n * sizeof(uint32_t));
  g->clu_hash[h] = g->nclu + 1;
  g->nclu += n + 1;
  return g->nclu - n - 1;
}


int grid_cps (const Grid *g, const Cell *c, const uint32_t **cps) {
  static const uint32_t space = ' ';
  if ((c->ch & CH_IMAGE) && !(c->ch & CH_CLUSTER)) {	/* copied as a blank */
    *cps = &space;
    return 1;
  }
  if (c->ch & CH_CLUSTER) {
    uint32_t off = c->ch & ~CH_CLUSTER;
    *cps = g->clu + off + 1;
    return (int)g->clu[off];
  }
  *cps = &c->ch;
  return 1;
}


uint32_t grid_base (const Grid *g, const Cell *c) {
  if (c->ch & CH_CLUSTER) return g->clu[(c->ch & ~CH_CLUSTER) + 1];
  return (c->ch & CH_IMAGE) ? ' ' : c->ch;
}


/* a mark joins the character the cursor just wrote */
static void join_prev (Grid *g, uint32_t ch) {
  Line *l = &g->screen[g->cy];
  uint32_t buf[CLU_MAX_CPS];
  const uint32_t *cps;
  int x = g->wrap_next ? g->cx : g->cx - 1, n, off;
  if (x < 0 || x >= l->n) return;
  if ((l->c[x].attr & A_WCONT) && x > 0) x--;
  if (l->c[x].ch == 0) return;	/* nothing to join: the mark goes */
  n = grid_cps(g, &l->c[x], &cps);
  if (n >= CLU_MAX_CPS) return;
  memcpy(buf, cps, (size_t)n * sizeof(uint32_t));
  buf[n++] = ch;
  off = clu_intern(g, buf, n);
  if (off < 0) return;
  l->c[x].ch = CH_CLUSTER | (uint32_t)off;
  l->dirty = 1;
}

/* }================================================================== */


/*
** {==================================================================
** Hyperlinks (OSC 8)
** ===================================================================
*/

int grid_link_add (Grid *g, const char *uri) {
  int i;
  for (i = g->nlinks - 1; i >= 0 && i >= g->nlinks - 64; i--)	/* the same one again */
    if (strcmp(g->links[i], uri) == 0) return i + 1;
  if (g->nlinks >= 65535) return 0;
  if (g->nlinks == 0 || (g->nlinks >= 8 && (g->nlinks & (g->nlinks - 1)) == 0))
    g->links = (char **)xrealloc(g->links, (size_t)(g->nlinks ? g->nlinks * 2 : 8) * sizeof(char *));
  g->links[g->nlinks++] = xstrdup(uri);
  return g->nlinks;
}


const char *grid_link (const Grid *g, int link) {
  return (link >= 1 && link <= g->nlinks) ? g->links[link - 1] : NULL;
}

/* }================================================================== */


/* a cell is about to change: do not leave half a wide character behind */
static void unlink_wide (Grid *g, Line *l, int x) {
  if (x < 0 || x >= l->n) return;
  if ((l->c[x].attr & A_WCONT) && x > 0) {
    l->c[x - 1].ch = 0;
    l->c[x - 1].attr &= (uint16_t)~A_WIDE;
  }
  if ((l->c[x].attr & A_WIDE) && x + 1 < l->n) {
    l->c[x + 1].ch = 0;
    l->c[x + 1].attr &= (uint16_t)~A_WCONT;
  }
  (void)g;
}


void grid_putc (Grid *g, uint32_t ch) {
  Line *l;
  int w = grid_wcwidth(ch);
  if (g->join_next) {	/* after a joiner: one emoji made of several */
    g->join_next = 0;
    join_prev(g, ch);
    return;
  }
  if (w == 0) {	/* an accent, a skin tone, a joiner: onto the one before */
    join_prev(g, ch);
    if (ch == 0x200D) g->join_next = 1;
    return;
  }
  if (g->wrap_next && g->autowrap) {
    g->screen[g->cy].wrapped = 1;
    grid_cr(g);
    grid_lf(g);
  }
  if (w == 2 && g->cx == g->cols - 1) {	/* no room for both halves */
    if (!g->autowrap) return;
    g->screen[g->cy].wrapped = 1;
    grid_cr(g);
    grid_lf(g);
  }
  l = &g->screen[g->cy];
  if (g->insert) grid_insert_chars(g, w);
  unlink_wide(g, l, g->cx);
  if (w == 2) unlink_wide(g, l, g->cx + 1);
  l->c[g->cx] = g->pen;
  l->c[g->cx].ch = ch;
  l->c[g->cx].attr = (uint16_t)((g->pen.attr & ~(A_WIDE | A_WCONT)) |
                                (w == 2 ? A_WIDE : 0));
  if (w == 2) {
    l->c[g->cx + 1] = g->pen;
    l->c[g->cx + 1].ch = 0;
    l->c[g->cx + 1].attr = (uint16_t)((g->pen.attr & ~A_WIDE) | A_WCONT);
  }
  l->dirty = 1;
  g->cx += w;
  g->wrap_next = 0;
  if (g->cx >= g->cols) {
    g->cx = g->cols - 1;
    g->wrap_next = g->autowrap;
  }
}


/*
** {==================================================================
** Images (sixel): a picture sits on the cells it covers; each of them
** holds CH_IMAGE, the image's id and which piece of it is there. So it
** scrolls, reflows and is written over like text. Images are kept up to
** a total size; after that the oldest go and their cells show nothing.
** ===================================================================
*/

#define IMG_BYTES_MAX	((size_t)64 << 20)


static void image_drop_oldest (Grid *g) {
  free(g->images[0].px);
  g->img_bytes -= (size_t)g->images[0].w * (size_t)g->images[0].h * 4;
  memmove(g->images, g->images + 1, (size_t)(g->nimages - 1) * sizeof(GridImage));
  g->nimages--;
}


const GridImage *grid_image (const Grid *g, uint32_t ch, int *tile) {
  int id = (int)((ch >> 16) & 0x3FFF), i;
  *tile = (int)(ch & 0xFFFF);
  for (i = g->nimages - 1; i >= 0; i--)
    if (g->images[i].id == id) return &g->images[i];
  return NULL;
}


/* puts an image (0xAARRGGBB, takes px) at the cursor; the cursor goes below it */
void grid_put_image (Grid *g, uint32_t *px, int w, int h) {
  int cw = g->cell_w > 0 ? g->cell_w : 10, ch = g->cell_h > 0 ? g->cell_h : 20;
  int cols = (w + cw - 1) / cw, rows = (h + ch - 1) / ch, x0 = g->cx, r, c, id;
  GridImage *im;
  if (w <= 0 || h <= 0) {
    free(px);
    return;
  }
  if (cols > g->cols - x0) cols = g->cols - x0;
  if (cols < 1) cols = 1;
  if (rows * cols > 0xFFFF) rows = 0xFFFF / cols;	/* the piece number has 16 bits */
  id = g->img_next = (g->img_next % 0x3FFF) + 1;
  while (g->nimages > 0 && (g->img_bytes + (size_t)w * (size_t)h * 4 > IMG_BYTES_MAX ||
                            g->nimages >= 1024))
    image_drop_oldest(g);
  if (g->nimages == g->capimages) {
    g->capimages = g->capimages ? g->capimages * 2 : 16;
    g->images = (GridImage *)xrealloc(g->images, (size_t)g->capimages * sizeof(GridImage));
  }
  im = &g->images[g->nimages++];
  im->id = id;
  im->px = px;
  im->w = w;
  im->h = h;
  im->cw = cw;
  im->ch = ch;
  im->tw = cols;
  g->img_bytes += (size_t)w * (size_t)h * 4;
  g->wrap_next = 0;
  for (r = 0; r < rows; r++) {
    Line *l = &g->screen[g->cy];
    for (c = 0; c < cols && x0 + c < g->cols; c++) {
      Cell *cell = &l->c[x0 + c];
      unlink_wide(g, l, x0 + c);
      *cell = blank_cell(NULL);
      cell->ch = CH_IMAGE | ((uint32_t)id << 16) | (uint32_t)(r * cols + c);
    }
    l->dirty = 1;
    g->cx = x0;
    grid_lf(g);	/* scrolls at the bottom, like text does */
  }
  g->cx = x0;
}

/* }================================================================== */


void grid_erase_line (Grid *g, int mode) {
  Line *l = &g->screen[g->cy];
  int from = (mode == 0) ? g->cx : 0;
  int to = (mode == 1) ? g->cx + 1 : g->cols;
  unlink_wide(g, l, from);
  unlink_wide(g, l, to - 1);
  line_fill(g, l, from, to);
  if (mode != 1) l->wrapped = 0;
  g->wrap_next = 0;
}


void grid_erase_display (Grid *g, int mode) {
  int y;
  if (mode == 3) {	/* the scrollback too */
    for (y = 0; y < g->sb_cap; y++) {
      free(g->sb[y].c);
      g->sb[y].c = NULL;
    }
    g->sb_len = g->sb_head = g->view = 0;
    g->all_dirty = 1;
    return;
  }
  if (mode == 0 || mode == 1) grid_erase_line(g, mode);
  for (y = 0; y < g->rows; y++) {
    if ((mode == 0 && y <= g->cy) || (mode == 1 && y >= g->cy)) continue;
    line_fill(g, &g->screen[y], 0, g->cols);
    g->screen[y].wrapped = 0;
  }
  g->wrap_next = 0;
}


void grid_erase_chars (Grid *g, int n) {
  Line *l = &g->screen[g->cy];
  int to = g->cx + (n < 1 ? 1 : n);
  if (to > g->cols) to = g->cols;
  unlink_wide(g, l, g->cx);
  unlink_wide(g, l, to - 1);
  line_fill(g, l, g->cx, to);
  g->wrap_next = 0;
}


void grid_insert_chars (Grid *g, int n) {
  Line *l = &g->screen[g->cy];
  int room = g->cols - g->cx;
  if (n < 1) n = 1;
  if (n > room) n = room;
  unlink_wide(g, l, g->cx);
  memmove(&l->c[g->cx + n], &l->c[g->cx], (size_t)(room - n) * sizeof(Cell));
  line_fill(g, l, g->cx, g->cx + n);
  if (l->c[g->cols - 1].attr & A_WIDE) {	/* second half fell off */
    l->c[g->cols - 1].ch = 0;
    l->c[g->cols - 1].attr &= (uint16_t)~A_WIDE;
  }
  g->wrap_next = 0;
}


void grid_delete_chars (Grid *g, int n) {
  Line *l = &g->screen[g->cy];
  int room = g->cols - g->cx;
  if (n < 1) n = 1;
  if (n > room) n = room;
  unlink_wide(g, l, g->cx);
  unlink_wide(g, l, g->cx + n);
  memmove(&l->c[g->cx], &l->c[g->cx + n], (size_t)(room - n) * sizeof(Cell));
  line_fill(g, l, g->cols - n, g->cols);
  g->wrap_next = 0;
}

/* }================================================================== */


/* switches between the primary and the alternate screen */
void grid_set_alt (Grid *g, int on, int clear) {
  Line *tmp;
  int y;
  on = (on != 0);
  if (on != g->alt) {
    tmp = g->screen;
    g->screen = g->other;
    g->other = tmp;
    g->alt = on;
    g->view = 0;
  }
  if (on && clear) {
    for (y = 0; y < g->rows; y++) {
      line_fill(g, &g->screen[y], 0, g->cols);
      g->screen[y].wrapped = 0;
    }
  }
  g->wrap_next = 0;
  g->all_dirty = 1;
}


/*
** A new size for one screen, cut or padded as it is (the alternate
** screen: the program there repaints it). k = 0: the active screen.
*/
static void resize_plain (Grid *g, int k, int cols, int rows) {
  Line **scr = (k == 0) ? &g->screen : &g->other;
  int primary = (k == 0) ? !g->alt : g->alt;
  int n = g->rows, y;
  while (n > rows) {	/* too many lines */
    int has_cursor_room = (k != 0) || g->cy < n - 1;
    if (has_cursor_room && line_is_blank(&(*scr)[n - 1])) free((*scr)[n - 1].c);
    else {	/* drop from the top instead */
      Line first = (*scr)[0];
      int alt = g->alt;
      if (primary) {
        g->alt = 0;
        sb_push(g, &first);
        g->alt = alt;
      }
      else free(first.c);
      memmove(&(*scr)[0], &(*scr)[1], (size_t)(n - 1) * sizeof(Line));
      if (k == 0 && g->cy > 0) g->cy--;
    }
    n--;
  }
  *scr = (Line *)xrealloc(*scr, (size_t)rows * sizeof(Line));
  for (y = n; y < rows; y++) line_init(g, &(*scr)[y], cols);
  for (y = 0; y < n; y++) line_set_cols(g, &(*scr)[y], cols);
}


/*
** {==================================================================
** Reflow: when the width changes, the text of the primary screen and
** of the scrollback is wrapped again, like a paragraph in an editor
** ===================================================================
*/

typedef struct LLine {	/* a logical line: the rows the terminal wrapped, joined */
  Cell *c;
  int n, cap;
} LLine;


static void ll_add (LLine *ll, const Cell *c, int n) {
  if (ll->n + n > ll->cap) {
    while (ll->n + n > ll->cap) ll->cap = ll->cap ? ll->cap * 2 : 64;
    ll->c = (Cell *)xrealloc(ll->c, (size_t)ll->cap * sizeof(Cell));
  }
  memcpy(ll->c + ll->n, c, (size_t)n * sizeof(Cell));
  ll->n += n;
}


/* a place in a logical line, and where it lands after the new wrapping */
typedef struct Spot {
  int ll, off;
  int row, col, wrap;	/* wrap: the row is full, the next character goes on */
} Spot;


static void spot_at (Spot *sp, int i, int j, int x, int nout, int cols, int end) {
  if (sp->ll != i || sp->off != j) return;
  if (end && x >= cols) {	/* right after a full row */
    sp->row = nout - 1;
    sp->col = cols - 1;
    sp->wrap = 1;
  }
  else if (x >= cols) {	/* the character there starts the next row */
    sp->row = nout;
    sp->col = 0;
  }
  else {
    sp->row = nout - 1;
    sp->col = x;
  }
}


static void spot_past (Spot *sp, int i, int n, int x, int nout, int cols) {
  int col;
  if (sp->ll != i || sp->off <= n) return;	/* past the text: after blanks */
  col = (x >= cols ? 0 : x) + (sp->off - n);
  sp->row = nout - 1 + (x >= cols ? 1 : 0);
  sp->col = col >= cols ? cols - 1 : col;
}


static Line *out_room (Line *out, int *cap, int need) {
  if (need <= *cap) return out;
  *cap = need * 2;
  return (Line *)xrealloc(out, (size_t)*cap * sizeof(Line));
}


static void reflow (Grid *g, Line **scrp, int *cx, int *cy, int *wrap_next,
                    int cols, int rows) {
  Line *scr = *scrp, *out = NULL;
  LLine *lls = NULL;
  int nll = 0, capll = 0, nout = 0, capout = 0;
  int last, i, y, nphys, prev_wrapped = 0, top;
  Spot cur, first;
  memset(&cur, 0, sizeof(cur));
  memset(&first, 0, sizeof(first));
  cur.ll = first.ll = -1;
  /* the rows of the screen that count: down to the cursor, or text below it */
  for (last = g->rows - 1; last > *cy && line_is_blank(&scr[last]); last--)
    ;
  nphys = g->sb_len + last + 1;
  for (i = 0; i < nphys; i++) {
    int sb = (i < g->sb_len), sy = i - g->sb_len, n;
    Line *l = sb ? &g->sb[(g->sb_head + i) % g->sb_cap] : &scr[sy];
    LLine *ll;
    if (i == 0 || !prev_wrapped) {
      if (nll == capll) {
        capll = capll ? capll * 2 : 256;
        lls = (LLine *)xrealloc(lls, (size_t)capll * sizeof(LLine));
      }
      memset(&lls[nll++], 0, sizeof(LLine));
    }
    ll = &lls[nll - 1];
    n = l->n;
    if (l->wrapped) {	/* a wide character that did not fit left a hole */
      if (n > 0 && l->c[n - 1].ch == 0 && !(l->c[n - 1].attr & A_WCONT)) n--;
    }
    else while (n > 0 && l->c[n - 1].ch == 0 && !(l->c[n - 1].attr & A_WCONT)) n--;
    if (!sb && sy == 0) {
      first.ll = nll - 1;
      first.off = ll->n;
    }
    if (!sb && sy == *cy) {
      cur.ll = nll - 1;
      cur.off = ll->n + *cx + (*wrap_next ? 1 : 0);
    }
    if (n > 0) ll_add(ll, l->c, n);
    prev_wrapped = l->wrapped;
    free(l->c);
    l->c = NULL;
  }
  for (y = last + 1; y < g->rows; y++) free(scr[y].c);
  free(scr);
  g->sb_len = g->sb_head = 0;	/* every line of it was taken above */
  for (i = 0; i < nll; i++) {	/* now wrap them again */
    LLine *ll = &lls[i];
    int x = 0, j;
    Line *l;
    out = out_room(out, &capout, nout + ll->n / (cols - 1) + 2);
    line_init(NULL, &out[nout], cols);
    l = &out[nout++];
    for (j = 0; j <= ll->n; j++) {
      int w;
      spot_at(&cur, i, j, x, nout, cols, j == ll->n);
      spot_at(&first, i, j, x, nout, cols, j == ll->n);
      if (j == ll->n) break;
      if (ll->c[j].attr & A_WCONT) continue;	/* it comes with its first half */
      w = (ll->c[j].attr & A_WIDE) ? 2 : 1;
      if (x + w > cols) {	/* the row is full: on to the next */
        l->wrapped = 1;
        line_init(NULL, &out[nout], cols);
        l = &out[nout++];
        x = 0;
      }
      l->c[x] = ll->c[j];
      if (w == 2) {
        l->c[x + 1] = ll->c[j];
        l->c[x + 1].ch = 0;
        l->c[x + 1].attr = (uint16_t)((ll->c[j].attr & ~A_WIDE) | A_WCONT);
      }
      x += w;
    }
    spot_past(&cur, i, ll->n, x, nout, cols);
    spot_past(&first, i, ll->n, x, nout, cols);
    free(ll->c);
  }
  free(lls);
  while (cur.row >= nout) {	/* the cursor sits on a row of its own */
    out = out_room(out, &capout, nout + 1);
    line_init(NULL, &out[nout++], cols);
  }
  /* the screen starts where it did, as far as the cursor lets it */
  top = (first.ll >= 0) ? first.row : 0;
  if (nout - top > rows) top = nout - rows;
  if (top > cur.row) top = cur.row;
  if (cur.row - top >= rows) top = cur.row - rows + 1;
  if (top < 0) top = 0;
  for (i = 0; i < top; i++) sb_push(g, &out[i]);
  *scrp = (Line *)xmalloc((size_t)rows * sizeof(Line));
  for (y = 0; y < rows; y++) {
    if (top + y < nout) (*scrp)[y] = out[top + y];
    else line_init(NULL, &(*scrp)[y], cols);
  }
  for (i = top + rows; i < nout; i++) free(out[i].c);
  free(out);
  *cx = cur.col;
  *cy = cur.row - top;
  *wrap_next = cur.wrap;
}

/* }================================================================== */


/*
** A new size. When the width changes, the primary screen and the
** scrollback are wrapped again; the alternate screen is cut or padded.
*/
void grid_resize (Grid *g, int cols, int rows) {
  int k, wrap = 0;
  if (cols < 2) cols = 2;
  if (rows < 1) rows = 1;
  if (cols == g->cols && rows == g->rows) return;
  g->view = 0;
  if (cols != g->cols && g->alt) {
    resize_plain(g, 0, cols, rows);
    reflow(g, &g->other, &g->saved_cx[0], &g->saved_cy[0], &wrap, cols, rows);
    g->wrap_next = 0;
  }
  else if (cols != g->cols) {
    resize_plain(g, 1, cols, rows);
    reflow(g, &g->screen, &g->cx, &g->cy, &g->wrap_next, cols, rows);
  }
  else {
    resize_plain(g, 0, cols, rows);
    resize_plain(g, 1, cols, rows);
    g->wrap_next = 0;
  }
  g->cols = cols;
  g->rows = rows;
  g->top = 0;
  g->bot = rows - 1;
  if (g->cx >= cols) g->cx = cols - 1;
  if (g->cy >= rows) g->cy = rows - 1;
  for (k = 0; k < 2; k++) {
    if (g->saved_cx[k] >= cols) g->saved_cx[k] = cols - 1;
    if (g->saved_cy[k] >= rows) g->saved_cy[k] = rows - 1;
  }
  set_tabs(g);
  g->all_dirty = 1;
}


static void put_utf8 (Buf *b, uint32_t cp) {
  if (cp < 0x80) buf_putc(b, (char)cp);
  else if (cp < 0x800) {
    buf_putc(b, (char)(0xC0 | (cp >> 6)));
    buf_putc(b, (char)(0x80 | (cp & 0x3F)));
  }
  else if (cp < 0x10000) {
    buf_putc(b, (char)(0xE0 | (cp >> 12)));
    buf_putc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
    buf_putc(b, (char)(0x80 | (cp & 0x3F)));
  }
  else {
    buf_putc(b, (char)(0xF0 | (cp >> 18)));
    buf_putc(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
    buf_putc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
    buf_putc(b, (char)(0x80 | (cp & 0x3F)));
  }
}


/*
** Text between two positions (inclusive), y as in grid_line(). Lines
** that were wrapped by the terminal are joined again.
*/
char *grid_text (const Grid *g, int x0, int y0, int x1, int y1) {
  Buf b;
  int y;
  buf_init(&b);
  for (y = y0; y <= y1; y++) {
    const Line *l = grid_line(g, y);
    int from = (y == y0) ? x0 : 0;
    int to = (y == y1) ? x1 : g->cols - 1;
    int last, x;
    if (l == NULL) continue;
    if (to >= l->n) to = l->n - 1;
    for (last = to; last >= from && l->c[last].ch == 0; last--)
      ;	/* trailing blanks are not text */
    for (x = from; x <= last; x++) {
      const uint32_t *cps;
      int n, k;
      if (l->c[x].attr & A_WCONT) continue;
      n = grid_cps(g, &l->c[x], &cps);
      for (k = 0; k < n; k++) put_utf8(&b, cps[k] ? cps[k] : ' ');
    }
    if (y < y1 && !(l->wrapped && last == l->n - 1)) buf_putc(&b, '\n');
  }
  return buf_take(&b);
}
