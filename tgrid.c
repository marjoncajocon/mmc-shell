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
  g->cursor_shape = 0;
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
  grid_move(g, 0, 0);
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
  g->wrap_next = 0;
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

/* double width and zero width characters (a compact wcwidth) */
int grid_wcwidth (uint32_t ch) {
  static const uint32_t wide[][2] = {
    {0x1100, 0x115F}, {0x2E80, 0x303E}, {0x3041, 0x33FF}, {0x3400, 0x4DBF},
    {0x4E00, 0x9FFF}, {0xA000, 0xA4CF}, {0xAC00, 0xD7A3}, {0xF900, 0xFAFF},
    {0xFE30, 0xFE4F}, {0xFF00, 0xFF60}, {0xFFE0, 0xFFE6}, {0x1F300, 0x1F64F},
    {0x1F680, 0x1F6FF}, {0x1F900, 0x1F9FF}, {0x20000, 0x3FFFD}
  };
  static const uint32_t zero[][2] = {
    {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x0610, 0x061A},
    {0x064B, 0x065F}, {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF}, {0x200B, 0x200F},
    {0x2028, 0x202E}, {0x2060, 0x2064}, {0x20D0, 0x20FF}, {0xFE00, 0xFE0F},
    {0xFE20, 0xFE2F}, {0xFEFF, 0xFEFF}, {0xE0100, 0xE01EF}
  };
  size_t i;
  if (ch < 0x300) return 1;
  for (i = 0; i < sizeof(zero) / sizeof(zero[0]); i++)
    if (ch >= zero[i][0] && ch <= zero[i][1]) return 0;
  for (i = 0; i < sizeof(wide) / sizeof(wide[0]); i++)
    if (ch >= wide[i][0] && ch <= wide[i][1]) return 2;
  return 1;
}


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
  if (w == 0) return;	/* combining marks are not composed (yet) */
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


/* new size; text is not re-wrapped (the program repaints its screen) */
void grid_resize (Grid *g, int cols, int rows) {
  int y, k;
  if (cols < 2) cols = 2;
  if (rows < 1) rows = 1;
  if (cols == g->cols && rows == g->rows) return;
  for (k = 0; k < 2; k++) {	/* k = 0: active screen, 1: the other one */
    Line **scr = (k == 0) ? &g->screen : &g->other;
    int primary = (k == 0) ? !g->alt : g->alt;
    int n = g->rows;
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
  g->wrap_next = 0;
  if (g->view > g->sb_len) g->view = g->sb_len;
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
      if (l->c[x].attr & A_WCONT) continue;
      put_utf8(&b, l->c[x].ch ? l->c[x].ch : ' ');
    }
    if (y < y1 && !(l->wrapped && last == l->n - 1)) buf_putc(&b, '\n');
  }
  return buf_take(&b);
}
