/*
** mterm.h - mmc-term, the terminal window of mmc
**
** One shared core draws the whole terminal into a pixel buffer:
**   tgrid.c  screen model        tvt.c    escape sequence parser
**   ttheme.c themes and config   tfont.c  fonts (stb_truetype)
**   tdraw.c  software renderer   tapp.c   keys, selection, menu ...
**   tpty.c   pseudo terminal (ConPTY on Windows, pty elsewhere)
** and one small backend per system shows it and forwards input:
**   twin32.c Windows             tx11.c   Linux (X11)
**   tcocoa.c macOS
*/

#ifndef mterm_h
#define mterm_h

#include "mmc.h"

#define TERM_NAME	"mmc-term"


/*
** {==================================================================
** tgrid.c - screen model
** ===================================================================
*/

/* cell colors: tag in the high byte */
#define COL_DEFAULT	0x00000000u
#define COL_IDX(i)	(0x01000000u | (uint32_t)(i))	/* palette 0..255 */
#define COL_RGB(rgb)	(0x02000000u | ((uint32_t)(rgb) & 0xFFFFFFu))
#define COL_TAG(c)	((c) >> 24)

#define A_BOLD		0x0001
#define A_DIM		0x0002
#define A_ITALIC	0x0004
#define A_UNDER		0x0008
#define A_REVERSE	0x0010
#define A_STRIKE	0x0020
#define A_HIDDEN	0x0040
#define A_WIDE		0x0080	/* first half of a double width character */
#define A_WCONT		0x0100	/* second half: draw nothing */
#define A_BLINK		0x0200
#define A_OVER		0x0400	/* overline */
#define A_ULSTYLE	0x3800	/* with A_UNDER: 0 single, 1 double, 2 curly,
				   3 dotted, 4 dashed (times 0x0800) */
#define UL_SHIFT	11

/* a cell's ch with this bit is a character with marks joined to it (an
** accent, an emoji sequence): the rest is its place in Grid.clu */
#define CH_CLUSTER	0x80000000u
/* a piece of an image: bits 16..29 the image's id, 0..15 which piece */
#define CH_IMAGE	0x40000000u

typedef struct Cell {
  uint32_t ch;	/* code point, 0 = empty */
  uint32_t fg, bg;
  uint16_t attr;
  uint16_t link;	/* OSC 8 hyperlink: Grid.links[link - 1], 0 = none */
  uint32_t ul;	/* underline color (SGR 58), COL_DEFAULT: the text color */
} Cell;

typedef struct Line {
  Cell *c;
  int n;	/* cells allocated */
  int wrapped;	/* continues on the next line */
  int dirty;
} Line;

typedef struct GridImage {
  int id;
  uint32_t *px;	/* 0xAARRGGBB, w * h */
  int w, h;
  int cw, ch;	/* the cell size it was placed with, in pixels */
  int tw;	/* pieces across */
} GridImage;

typedef struct Grid {
  int cols, rows;
  Line *screen;	/* the active screen, 'rows' lines */
  Line *other;	/* the inactive one (alternate or primary) */
  int alt;	/* is the alternate screen active? */
  Line *sb;	/* scrollback ring */
  int sb_cap, sb_len, sb_head;
  int view;	/* lines scrolled back; 0 = live */
  int cx, cy, wrap_next;
  Cell pen;
  int top, bot;	/* scroll region, inclusive */
  int saved_cx[2], saved_cy[2];
  Cell saved_pen[2];
  unsigned char *tabs;
  /* modes */
  int autowrap, cursor_on, app_cursor, bracketed, focus_events, insert;
  int mouse;	/* 0 off, or the mode: 9, 1000, 1002, 1003 */
  int mouse_sgr;	/* 1006: the report is text, not bytes */
  int cursor_shape;	/* DECSCUSR 0..6 */
  int sync;	/* 2026: the program is drawing, show the screen once it is done */
  int origin;	/* DECOM (?6): rows count from the top of the scroll region */
  int kitty[2][8], kitty_n[2];	/* kitty keyboard flags, a stack per screen */
  int cell_w, cell_h;	/* pixels of a cell, for images (the window says) */
  GridImage *images;
  int nimages, capimages, img_next;
  size_t img_bytes;
  int all_dirty;
  int join_next;	/* after a zero width joiner: the next character joins too */
  uint32_t *clu;	/* clusters: a count, then the code points; see CH_CLUSTER */
  int nclu, capclu;
  int *clu_hash, clu_hcap;	/* offsets into clu, to find one again */
  char **links;	/* hyperlink targets, kept for the life of the grid */
  int nlinks;
} Grid;

Grid *grid_new (int cols, int rows, int scrollback);
void grid_free (Grid *g);
void grid_reset (Grid *g);
void grid_resize (Grid *g, int cols, int rows);
int grid_wcwidth (uint32_t ch);
int grid_kitty (const Grid *g);
void grid_put_image (Grid *g, uint32_t *px, int w, int h);	/* at the cursor; takes px */
const GridImage *grid_image (const Grid *g, uint32_t ch, int *tile);	/* NULL: gone */	/* the kitty keyboard flags now: 1 disambiguate, 8 all keys */
int grid_cps (const Grid *g, const Cell *c, const uint32_t **cps);	/* code points */
uint32_t grid_base (const Grid *g, const Cell *c);	/* the first one */
int grid_link_add (Grid *g, const char *uri);	/* its number, 0 when full */
const char *grid_link (const Grid *g, int link);
void grid_putc (Grid *g, uint32_t ch);
void grid_cr (Grid *g);
void grid_lf (Grid *g);
void grid_ri (Grid *g);
void grid_bs (Grid *g);
void grid_tab (Grid *g, int n);
void grid_move (Grid *g, int x, int y);
void grid_erase_display (Grid *g, int mode);
void grid_erase_line (Grid *g, int mode);
void grid_erase_chars (Grid *g, int n);
void grid_insert_chars (Grid *g, int n);
void grid_delete_chars (Grid *g, int n);
void grid_insert_lines (Grid *g, int n);
void grid_delete_lines (Grid *g, int n);
void grid_scroll_up (Grid *g, int n);
void grid_scroll_down (Grid *g, int n);
void grid_set_region (Grid *g, int top, int bot);
void grid_save_cursor (Grid *g);
void grid_restore_cursor (Grid *g);
void grid_set_alt (Grid *g, int on, int clear);
void grid_set_view (Grid *g, int view);
const Line *grid_line (const Grid *g, int y);	/* y: -sb_len .. rows-1 */
const Line *grid_view_line (const Grid *g, int row);	/* what row shows */
char *grid_text (const Grid *g, int x0, int y0, int x1, int y1);

/* }================================================================== */


/*
** {==================================================================
** tvt.c - escape sequence parser
** ===================================================================
*/

#define VT_MAX_PARAMS	24

typedef struct Vt {
  Grid *g;
  int state;
  int params[VT_MAX_PARAMS];
  unsigned char sub[VT_MAX_PARAMS];	/* param was introduced by ':' */
  int nparams, has_digit;
  char priv, inter;
  Buf osc;
  Buf dcs;	/* a DCS string (sixel) being read */
  int in_dcs;
  uint32_t u8cp;
  int u8need;
  uint32_t last;	/* for REP */
  int charset[2], gl;	/* G0/G1 is line drawing?, which one is active */
  void *ud;
  void (*reply) (void *ud, const char *s, size_t n);
  void (*title) (void *ud, const char *utf8);
  void (*bell) (void *ud);
  void (*on_osc) (void *ud, int code, const char *text);	/* 4, 7, 10..12, 52 */
} Vt;

void vt_init (Vt *vt, Grid *g);
void vt_free (Vt *vt);
void vt_feed (Vt *vt, const char *buf, size_t n);

/* }================================================================== */


/*
** {==================================================================
** ttheme.c - themes and configuration
** ===================================================================
*/

typedef struct Theme {
  const char *name;	/* in mmcterm.conf and --theme */
  const char *title;	/* in the menu */
  int dark;
  uint32_t bg, fg, cursor, cursor_text, sel_bg, accent1, accent2, ui, ui_text;
  uint32_t pal[16];
} Theme;

typedef struct Config {
  char theme[32];
  char font[128];
  char font_file[512];
  char shell[512];
  int font_size, cols, rows, padding, scrollback;
  int cursor;	/* 0 block, 1 underline, 2 bar */
  int cursor_blink, opacity, copy_on_select;
  int no_bold;	/* 1: bold text is drawn in the normal weight */
  int native_titlebar;	/* 1: let the system draw the title bar */
  int smoothing;	/* SMOOTH_*: how glyphs are rasterized */
  int ligatures;	/* 1: the font's ligatures (calt, liga) are used */
  int has_bg, has_fg, has_cursor;
  uint32_t bg, fg, cursor_color;
  int has_pal[16];
  uint32_t pal[16];
} Config;

void config_defaults (Config *c);
void config_load (Config *c, const char *native);
void config_save_default (const char *native);
void config_set_key (const char *native, const char *key, const char *value);
const Theme *theme_at (int i);
const Theme *theme_find (const char *name);
const Theme *theme_next (const char *name);
void theme_apply (Theme *out, const Theme *base, const Config *c);
uint32_t theme_color (const Theme *t, uint32_t col, int is_fg);
double theme_contrast (uint32_t a, uint32_t b);

/* }================================================================== */


/*
** {==================================================================
** tfont.c - fonts
** ===================================================================
*/

/*
** font_smoothing in mmcterm.conf. ClearType and gray use the system
** rasterizer (Windows: GDI, hinted like every other Windows program, and
** what git-bash's mintty shows); stb is the built-in stb_truetype one,
** the only one on Linux and macOS.
*/
#define SMOOTH_CLEARTYPE	0
#define SMOOTH_GRAY		1
#define SMOOTH_STB		2

typedef struct Glyph {
  int w, h, xoff, yoff;	/* yoff from the baseline, up is negative */
  int lcd;	/* 0: stb_truetype coverage, 1: R, G, B coverage per pixel
		   (ClearType), 2: gray from the system rasterizer, 3: a
		   color picture (emoji), 0xAARRGGBB per pixel */
  unsigned char *bm;	/* w*h coverage (w*h*3 when lcd is 1, w*h*4 when
			   it is 3), NULL if empty */
} Glyph;

void font_add_dir (const char *native);	/* fonts carried with mmc */
int font_init (const Config *c);
void font_set_px (float px);
int font_cell_w (void);
int font_cell_h (void);
int font_ascent (void);
const char *font_name (void);
/* 'dark': dark text on a light background (the system rasterizer
** tunes its coverage for the colors it draws with) */
const Glyph *font_glyph (uint32_t cp, int bold, int italic, int dark);
/* an emoji of several code points (a skin tone, a ZWJ sequence) in color,
** 'cells' wide; NULL when there is no color font for it */
const Glyph *font_emoji (const uint32_t *cps, int n, int cells);
/* ligatures: does the font of this style have any (and are they on)? */
int font_has_ligatures (int bold, int italic);
/* the glyphs of a run of n cells after the font's ligature rules: out[k]
** is drawn in cell cells[k]; plain[] gets the glyphs before them. Returns
** how many glyphs, or -1 when the font lacks one of the characters */
int font_shape (const uint32_t *cps, int n, int bold, int italic, uint16_t *plain,
                uint16_t *out, int *cells, int cap);
const Glyph *font_glyph_id (int gid, int bold, int italic, int dark);

/* }================================================================== */


/*
** {==================================================================
** tshape.c - the OpenType tables stb_truetype does not read
** ===================================================================
*/

#define OT_TEXT	0	/* rlig rclt calt liga clig: the ligatures of code */
#define OT_SEQ	1	/* ccmp rlig liga clig calt: emoji sequences */

typedef struct OtFace {
  const unsigned char *d;
  size_t n;	/* bytes in d */
  uint32_t gsub, colr, cpal, cblc, cbdt, sbix;	/* where the tables are, 0: none */
  int nglyphs;
  int nlk[2];	/* lookups for OT_TEXT and OT_SEQ, in the order they run */
  uint16_t *lk[2];
  unsigned char *first[2];	/* per lookup a bit per glyph it may start at,
				   then all of them together */
} OtFace;

int ot_open (OtFace *o, const unsigned char *data, size_t n, int fontstart);
void ot_close (OtFace *o);
int ot_can_shape (const OtFace *o, int which);
/* substitutes g[0..n) in place (cl[] follows: the cell each came from);
** returns the new count, at most cap */
int ot_shape (const OtFace *o, int which, uint16_t *g, int *cl, int n, int cap);
/* COLR: the number of layers of a glyph (0: none), the first in *first */
int ot_color_layers (const OtFace *o, int gid, int *first);
void ot_layer (const OtFace *o, int index, int *gid, int *palette_index);
uint32_t ot_palette (const OtFace *o, int index, uint32_t fg);
/* CBDT or sbix: the picture of a glyph as 0xAARRGGBB, from the strike
** nearest above want_ppem; NULL when there is none */
uint32_t *ot_bitmap (const OtFace *o, int gid, int want_ppem, int *w, int *h);
uint32_t *png_decode (const unsigned char *p, size_t n, int *w, int *h);

/* }================================================================== */


/*
** {==================================================================
** tdraw.c - software renderer
** ===================================================================
*/

typedef struct Frame {
  uint32_t *px;	/* 0x00RRGGBB, top row first */
  int w, h;
  int dy, dh;	/* the rows of pixels the last drawing changed */
} Frame;

#define MENU_MAX	32
#define TAB_MAX	32
#define PANE_MAX	8	/* panes in one tab */

typedef struct Menu {
  int open, x, y, w, h, hot, n;
  const char *label[MENU_MAX];	/* NULL: separator */
  const char *hint[MENU_MAX];
  int id[MENU_MAX];
  int row, colw;	/* set by menu_layout: an entry's height, a column's width */
  int ix[MENU_MAX], iy[MENU_MAX];	/* each entry from the top left; -1: not shown */
} Menu;

typedef struct Scene {	/* everything the renderer needs to know */
  const Grid *g;
  const Theme *t;
  int head;	/* height of our own title bar, 0 = the system draws one */
  int pad, strip;	/* padding and gradient strip height in pixels */
  const char *title;
  int bar;	/* height of the tab bar below it, 0 = one tab, no bar */
  int ntabs, cur_tab;
  int hot_tab, hot_close;	/* under the mouse: a tab (ntabs: the + button),
				   and whether on its close box */
  const char *tab_title[TAB_MAX];
  unsigned char tab_news[TAB_MAX];	/* output came while it was hidden */
  const char *edit;	/* the tab on screen is being renamed: the text so far */
  int hot_button;	/* header button under the mouse: 0 hide, 1 zoom, 2 close */
  int maximized;
  int cw, ch, ascent;
  int focused, blink_on, cursor_style, no_bold;
  int text_blink_on;	/* blinking text (SGR 5) is shown now */
  int has_sel, sx0, sy0, sx1, sy1;	/* selection, y in grid_line() terms */
  int has_hot, hx0, hy0, hx1, hy1;	/* the link under the mouse, the same way */
  const char *find;	/* the search box, or NULL */
  int ox, oy;	/* where g starts in pixels; 0: the usual place */
  int npanes;	/* 2 and more: the tab is split, g is the focused pane */
  struct {
    const Grid *g;
    const Theme *t;
    int x, y, w, h;	/* in pixels */
    int focused;
  } pane[PANE_MAX];
  int ndivs;	/* the lines between panes: x, y, w, h */
  int div[PANE_MAX][4];
  int bar_alpha;	/* scrollbar 0..255 */
  const char *pill;	/* size hint while resizing, or NULL */
  const Menu *menu;
} Scene;

void frame_resize (Frame *f, int w, int h);
void frame_free (Frame *f);
int frame_save_bmp (const Frame *f, const char *native);
void draw_scene (Frame *f, const Scene *s);
/* only the terminal rows whose lines are dirty (the rest of the scene is
** as it was drawn last): returns 0 when nothing had to change */
int draw_scene_rows (Frame *f, const Scene *s);
void draw_scrollbar_rect (const Frame *f, const Scene *s, int *x, int *y,
                          int *w, int *h);
void draw_button_rect (const Frame *f, const Scene *s, int i, int *x, int *y,
                       int *w, int *h);
void draw_tab_rect (const Frame *f, const Scene *s, int i, int *x, int *y,
                    int *w, int *h);	/* i == ntabs: the + button */
void draw_tab_close_rect (const Frame *f, const Scene *s, int i, int *x,
                          int *y, int *w, int *h);
int draw_text_width (const char *utf8);
/* sizes the menu (w, h) and places its entries so it is at most max_h
** tall: tighter rows first, then more columns */
void menu_layout (Menu *m, int cell_w, int cell_h, int max_h);

/* }================================================================== */


/*
** {==================================================================
** tpty.c - pseudo terminal
** ===================================================================
*/

typedef struct Pty Pty;	/* one per tab */

Pty *pty_spawn (const char *exe, char **argv, int cols, int rows);
void pty_write (Pty *p, const char *s, size_t n);
void pty_resize (Pty *p, int cols, int rows);
long pty_read (Pty *p, char *buf, size_t n);	/* 0: nothing now, -1: closed */
int pty_fd (Pty *p);	/* POSIX: for poll(); -1 on Windows */
int pty_exited (Pty *p, int *code);
void pty_close (Pty *p);	/* ends the program and frees p */

/* }================================================================== */


/*
** {==================================================================
** tapp.c - behaviour (called by the backend)
** ===================================================================
*/

enum {
  TK_NONE, TK_UP, TK_DOWN, TK_LEFT, TK_RIGHT, TK_HOME, TK_END, TK_PGUP,
  TK_PGDN, TK_INSERT, TK_DELETE, TK_ENTER, TK_TAB, TK_BACKSPACE, TK_ESCAPE,
  TK_CHAR,	/* a letter/digit/sign used as a shortcut; see 'cp' */
  TK_F1	/* TK_F1 + n */
};

#define TM_SHIFT	1
#define TM_ALT		2
#define TM_CTRL		4

enum { TMS_DOWN, TMS_UP, TMS_MOVE, TMS_WHEEL, TMS_LEAVE };

typedef struct AppArgs {
  const char *theme;	/* --theme */
  int hold;	/* --hold */
  char **cmd;	/* -e prog args..., or NULL */
  const char *render_test;	/* --render-test file.bmp */
  int font_size;	/* --font-size N, 0 = from the config */
} AppArgs;

int app_init (const AppArgs *args, const char *argv0);
int app_start (void);	/* after the window exists: spawn the shell */
void app_initial_size (int *w, int *h);
void app_on_resize (int w, int h);
void app_on_dpi (float scale);
int app_on_key (int key, int mods, uint32_t cp);	/* 1 if consumed */
void app_on_text (const char *utf8, int mods);
void app_on_mouse (int type, int button, int x, int y, int mods, int arg);
void app_on_focus (int on);
void app_on_paste (const char *utf8);
void app_on_wake (void);	/* pty has data, or the child ended */
void app_on_tick (unsigned now_ms);
void app_snap_size (int *w, int *h);	/* whole cells while resizing */
void app_resizing (int on);
const Frame *app_render (void);	/* NULL if nothing changed */
int app_render_test (const char *native);
int app_exit_code (void);
int app_fds (int *fds, int max);	/* POSIX: the pty of every tab, for poll() */
void app_close_all (void);	/* the window goes: end every tab */

/* what is at this point of the window? (for our own title bar) */
enum { HIT_CLIENT, HIT_CAPTION, HIT_BUTTON, HIT_UI };	/* UI: menu, scrollbar */
int app_hit_test (int x, int y);

/* }================================================================== */


/*
** {==================================================================
** the backend: twin32.c, tx11.c or tcocoa.c
** ===================================================================
*/

int win_create (int w, int h, const char *title);
int win_run (void);	/* returns the exit code */
void win_close (void);
void win_wake (void);	/* thread safe: calls app_on_wake on the UI thread */
void win_redraw (void);
void win_set_title (const char *utf8);
void win_set_size (int w, int h);	/* client area */
void win_set_fullscreen (int on);
void win_set_chrome (uint32_t bg, uint32_t border, uint32_t text, int dark);
void win_set_opacity (int percent);
void win_set_clipboard (const char *utf8);
void win_request_paste (void);	/* answers through app_on_paste */
void win_flash (void);
unsigned win_ticks (void);
float win_scale (void);	/* 1.0 = 96 dpi */
void win_message (const char *title, const char *text);
void win_open_url (const char *utf8);	/* in the browser (http, https, mailto) */
int win_custom_chrome (int want);	/* before win_create; 1: we draw the title bar */
void win_minimize (void);
void win_toggle_maximize (void);
int win_is_maximized (void);

/* }================================================================== */

#endif
