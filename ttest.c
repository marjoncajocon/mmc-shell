/*
** ttest.c - headless tests for the core of mmc-term
**
**   ttest                   run the checks
**   ttest replay FILE       feed a captured terminal stream, print the screen
**   ttest render OUT.bmp [light|dark]   draw a sample screen to an image
*/

#include "mterm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static int failures = 0, checks = 0;
static Buf replies;
static char last_title[256];


static void on_reply (void *ud, const char *s, size_t n) {
  (void)ud;
  buf_putn(&replies, s, n);
}


static void on_title (void *ud, const char *s) {
  (void)ud;
  strncpy(last_title, s, sizeof(last_title) - 1);
}


static void check_str (const char *what, const char *got, const char *want) {
  checks++;
  if (strcmp(got, want) != 0) {
    failures++;
    printf("FAIL %s\n  want: [%s]\n  got:  [%s]\n", what, want, got);
  }
}


static void check_int (const char *what, long got, long want) {
  checks++;
  if (got != want) {
    failures++;
    printf("FAIL %s: want %ld, got %ld\n", what, want, got);
  }
}


typedef struct T {
  Grid *g;
  Vt vt;
} T;


static void t_open (T *t, int cols, int rows, int sb) {
  t->g = grid_new(cols, rows, sb);
  vt_init(&t->vt, t->g);
  t->vt.reply = on_reply;
  t->vt.title = on_title;
  buf_free(&replies);
}


static void t_close (T *t) {
  vt_free(&t->vt);
  grid_free(t->g);
}


static void feed (T *t, const char *s) {
  vt_feed(&t->vt, s, strlen(s));
}


static void check_screen (T *t, const char *what, const char *want) {
  char *got = grid_text(t->g, 0, 0, t->g->cols - 1, t->g->rows - 1);
  size_t n = strlen(got);
  while (n > 0 && got[n - 1] == '\n') got[--n] = '\0';	/* blank tail */
  check_str(what, got, want);
  free(got);
}


static void test_core (void) {
  T t;

  t_open(&t, 20, 5, 100);
  feed(&t, "hello\r\nworld");
  check_screen(&t, "plain text", "hello\nworld");
  check_int("cursor x", t.g->cx, 5);
  check_int("cursor y", t.g->cy, 1);
  feed(&t, "\033[1;3HXY\033[K");
  check_screen(&t, "CUP + EL", "heXY\nworld");
  feed(&t, "\033[2;1H\033[2K\033[1;9H\033[1K");
  check_screen(&t, "EL 2 / EL 1", "");
  t_close(&t);

  t_open(&t, 10, 4, 100);
  feed(&t, "0123456789ab");
  check_screen(&t, "autowrap joins the line again", "0123456789ab");
  check_int("wrapped flag", t.g->screen[0].wrapped, 1);
  check_int("wrap cursor y", t.g->cy, 1);
  feed(&t, "\033[?7l\033[2;1H0123456789XYZ");
  check_int("no autowrap keeps the cursor", t.g->cx, 9);
  check_int("no autowrap overwrites the last cell", (long)t.g->screen[1].c[9].ch, 'Z');
  t_close(&t);

  t_open(&t, 10, 3, 100);
  feed(&t, "1\r\n2\r\n3\r\n4");
  check_screen(&t, "scrolling", "2\n3\n4");
  check_int("scrollback length", t.g->sb_len, 1);
  check_int("scrollback content", (long)grid_line(t.g, -1)->c[0].ch, '1');
  grid_set_view(t.g, 1);
  check_int("view line 0 is history", (long)grid_view_line(t.g, 0)->c[0].ch, '1');
  grid_set_view(t.g, 0);
  feed(&t, "\033[3J");
  check_int("ED 3 clears the scrollback", t.g->sb_len, 0);
  t_close(&t);

  t_open(&t, 10, 5, 100);
  feed(&t, "A\r\nB\r\nC\r\nD\r\nE\033[2;4r\033[4;1H\n");
  check_screen(&t, "scroll region", "A\nC\nD\n\nE");
  check_int("region scroll is not history", t.g->sb_len, 0);
  feed(&t, "\033[2;1H\033M");
  check_screen(&t, "reverse index in region", "A\n\nC\nD\nE");
  feed(&t, "\033[r\033[1;1H\033[2L");
  check_screen(&t, "insert lines", "\n\nA\n\nC");
  feed(&t, "\033[2M");
  check_screen(&t, "delete lines", "A\n\nC");
  t_close(&t);

  t_open(&t, 12, 2, 0);
  feed(&t, "abcdef\033[1;3H\033[2@");
  check_screen(&t, "ICH", "ab  cdef");
  feed(&t, "\033[3P");
  check_screen(&t, "DCH", "abdef");
  feed(&t, "\033[1;2H\033[2X");
  check_screen(&t, "ECH", "a  ef");
  feed(&t, "\033[1;1H\033[4hZZ\033[4l");
  check_screen(&t, "insert mode", "ZZa  ef");
  feed(&t, "\033[2;1Hx\033[3b");
  check_screen(&t, "REP", "ZZa  ef\nxxxx");
  feed(&t, "\r\033[K1\t2\b\bQ");
  check_screen(&t, "tab and backspace", "ZZa  ef\n1      Q2");
  t_close(&t);

  t_open(&t, 20, 3, 0);
  feed(&t, "\033[1;31;44mX\033[0mY");
  check_int("SGR bold", t.g->screen[0].c[0].attr & A_BOLD, A_BOLD);
  check_int("SGR fg", (long)t.g->screen[0].c[0].fg, (long)COL_IDX(1));
  check_int("SGR bg", (long)t.g->screen[0].c[0].bg, (long)COL_IDX(4));
  check_int("SGR reset", t.g->screen[0].c[1].attr, 0);
  feed(&t, "\033[38;2;1;2;3mA\033[48;5;200mB\033[38:2::9:8:7mC\033[38:2:4:5:6mD");
  check_int("truecolor", (long)t.g->screen[0].c[2].fg, (long)COL_RGB(0x010203));
  check_int("256 color bg", (long)t.g->screen[0].c[3].bg, (long)COL_IDX(200));
  check_int("colon truecolor", (long)t.g->screen[0].c[4].fg, (long)COL_RGB(0x090807));
  check_int("colon truecolor, short", (long)t.g->screen[0].c[5].fg, (long)COL_RGB(0x040506));
  feed(&t, "\033[0;4;3;7;9mE\033[24;23;27;29mF\033[91;102mG");
  check_int("under+italic+reverse+strike", t.g->screen[0].c[6].attr,
            A_UNDER | A_ITALIC | A_REVERSE | A_STRIKE);
  check_int("attributes off", t.g->screen[0].c[7].attr, 0);
  check_int("bright fg", (long)t.g->screen[0].c[8].fg, (long)COL_IDX(9));
  check_int("bright bg", (long)t.g->screen[0].c[8].bg, (long)COL_IDX(10));
  feed(&t, "\033[0m\033[2;1H\033[44m\033[K");
  check_int("erase keeps the background", (long)t.g->screen[1].c[15].bg, (long)COL_IDX(4));
  t_close(&t);

  t_open(&t, 20, 3, 50);
  feed(&t, "main\033[?1049h\033[2;2Halt");
  check_screen(&t, "alternate screen", "\n alt");
  feed(&t, "\033[?1049l");
  check_screen(&t, "back to the main screen", "main");
  check_int("cursor restored x", t.g->cx, 4);
  check_int("cursor restored y", t.g->cy, 0);
  feed(&t, "\0337\033[3;9H\0338!");
  check_screen(&t, "DECSC / DECRC", "main!");
  t_close(&t);

  t_open(&t, 20, 3, 0);
  feed(&t, "a\xE6\x97\xA5" "b\xC3\xA9");
  check_screen(&t, "UTF-8 and wide characters", "a\xE6\x97\xA5" "b\xC3\xA9");
  check_int("wide flag", t.g->screen[0].c[1].attr & A_WIDE, A_WIDE);
  check_int("continuation flag", t.g->screen[0].c[2].attr & A_WCONT, A_WCONT);
  check_int("cursor after wide", t.g->cx, 5);
  feed(&t, "\033[1;3HZ");
  check_screen(&t, "overwriting half a wide char clears it", "a Zb\xC3\xA9");
  feed(&t, "\033[2;1H\xFF\xC3(");
  check_screen(&t, "bad UTF-8 becomes U+FFFD", "a Zb\xC3\xA9\n\xEF\xBF\xBD\xEF\xBF\xBD(");
  feed(&t, "\033[3;1H\033(0lqk\033(Bx");
  check_int("line drawing l", (long)t.g->screen[2].c[0].ch, 0x250C);
  check_int("line drawing q", (long)t.g->screen[2].c[1].ch, 0x2500);
  check_int("charset back to ASCII", (long)t.g->screen[2].c[3].ch, 'x');
  t_close(&t);

  t_open(&t, 20, 3, 0);
  feed(&t, "\033[2;5H\033[6n\033[5n\033[c");
  check_str("DSR / DA replies", replies.s ? replies.s : "",
            "\033[2;5R\033[0n\033[?1;2c");
  feed(&t, "\033]0;title one\007\033]2;title two\033\\after");
  check_str("OSC title (ST)", last_title, "title two");
  check_screen(&t, "text after OSC", "\n    after");
  feed(&t, "\033[?25l\033[?2004h\033[?1h\033[5 q\033P junk \033\\");
  check_int("cursor hidden", t.g->cursor_on, 0);
  check_int("bracketed paste", t.g->bracketed, 1);
  check_int("application cursor keys", t.g->app_cursor, 1);
  check_int("cursor shape", t.g->cursor_shape, 5);
  feed(&t, "\033c");
  check_int("RIS shows the cursor again", t.g->cursor_on, 1);
  check_screen(&t, "RIS clears", "");
  t_close(&t);

  t_open(&t, 10, 4, 100);
  feed(&t, "1\r\n2\r\n3\r\n4");
  grid_resize(t.g, 10, 2);
  check_screen(&t, "fewer rows keep the cursor line", "3\n4");
  check_int("lines pushed to history", t.g->sb_len, 2);
  grid_resize(t.g, 4, 3);
  check_screen(&t, "fewer columns", "3\n4");
  check_int("cursor clamped", t.g->cx < 4, 1);
  grid_resize(t.g, 30, 6);
  feed(&t, "\033[6;25Hend");
  check_int("bigger grid is usable", (long)t.g->screen[5].c[26].ch, 'd');
  t_close(&t);

  check_int("width of a", grid_wcwidth('a'), 1);
  check_int("width of CJK", grid_wcwidth(0x4E2D), 2);
  check_int("width of emoji", grid_wcwidth(0x1F600), 2);
  check_int("width of combining", grid_wcwidth(0x0301), 0);
}


static int replay (const char *file) {
  T t;
  size_t n = 0;
  char *data = read_file(file, &n);
  char *text;
  int y;
  if (data == NULL) {
    printf("cannot read %s\n", file);
    return 1;
  }
  t_open(&t, 100, 30, 1000);
  vt_feed(&t.vt, data, n);
  printf("--- title: %s\n--- history: %d lines, cursor at %d,%d\n",
         last_title, t.g->sb_len, t.g->cx, t.g->cy);
  for (y = -t.g->sb_len; y < t.g->rows; y++) {
    text = grid_text(t.g, 0, y, t.g->cols - 1, y);
    if (y == 0) printf("--- screen:\n");
    printf("%s\n", text);
    free(text);
  }
  t_close(&t);
  free(data);
  return 0;
}


/* the mouse modes and the OSC sequences programs use to ask things */
static char osc_seen[256];

static void on_osc (void *ud, int code, const char *text) {
  (void)ud;
  sprintf(osc_seen, "%d|%.200s", code, text);
}


static void test_modes (void) {
  T t;
  t_open(&t, 20, 5, 10);
  t.vt.on_osc = on_osc;

  feed(&t, "\033[?1000h");
  check_int("mouse 1000 on", t.g->mouse, 1000);
  feed(&t, "\033[?1006h");
  check_int("mouse SGR on", t.g->mouse_sgr, 1);
  feed(&t, "\033[?1002h");
  check_int("mouse 1002 replaces 1000", t.g->mouse, 1002);
  feed(&t, "\033[?1002l");
  check_int("mouse off", t.g->mouse, 0);
  feed(&t, "\033[?1003h\033c");	/* RIS clears it again */
  check_int("reset clears the mouse mode", t.g->mouse, 0);
  check_int("reset clears SGR", t.g->mouse_sgr, 0);

  feed(&t, "\033]0;a title\007");
  check_str("OSC 0 sets the title", last_title, "a title");
  strcpy(last_title, "kept");
  feed(&t, "\033]1;icon\007");
  check_str("OSC 1 leaves the title alone", last_title, "kept");

  osc_seen[0] = '\0';
  feed(&t, "\033]11;?\033\\");
  check_str("OSC 11 asks for the background", osc_seen, "11|?");
  osc_seen[0] = '\0';
  feed(&t, "\033]52;c;bW1j\007");
  check_str("OSC 52 carries the clipboard", osc_seen, "52|c;bW1j");
  osc_seen[0] = '\0';
  feed(&t, "\033]7;file://pc/d/w/mmc\033\\");
  check_str("OSC 7 carries the folder", osc_seen, "7|file://pc/d/w/mmc");
  osc_seen[0] = '\0';
  feed(&t, "\033]4;12;?\007");
  check_str("OSC 4 asks for a palette color", osc_seen, "4|12;?");
  osc_seen[0] = '\0';
  feed(&t, "\033]99;whatever\007");
  check_str("an unknown OSC is ignored", osc_seen, "");
  t_close(&t);
}


/* every palette color must be readable on the theme background */
static void test_contrast (void) {
  static const char *const names[] = {"dark", "light"};
  int k, i;
  for (k = 0; k < 2; k++) {
    const Theme *t = theme_find(names[k]);
    char what[64];
    check_int("fg contrast >= 7", theme_contrast(t->fg, t->bg) >= 7.0, 1);
    for (i = 0; i < 16; i++) {
      double c = theme_contrast(t->pal[i], t->bg);
      /* "black" on dark and "white" on light are background-like on purpose */
      int skip = t->dark ? (i == 0) : (i == 15 || i == 7);
      double need = (t->dark && i == 8) ? 3.0 : 4.5;
      if (!t->dark && i == 7) { skip = 0; need = 3.0; }
      if (!t->dark && i == 15) skip = 0;
      if (skip) continue;
      sprintf(what, "%s color%d contrast %.2f >= %.1f", names[k], i, c, need);
      check_int(what, c >= need, 1);
    }
  }
}


/* Powerline separators are geometry: they must fill their cell edge to
** edge, or the colored segments of a prompt show seams */
static void test_powerline (void) {
  Config c;
  Theme th;
  Grid *g;
  Vt vt;
  Frame f;
  Scene s;
  const int cw = 9, ch = 18;
  uint32_t red, bg;
  int y, solid;
  config_defaults(&c);
  theme_apply(&th, theme_find("dark"), &c);
  g = grid_new(3, 1, 0);
  vt_init(&vt, g);
  {
    static const char pl[] = "\033[?25l\033[31m\xee\x82\xb0\xee\x82\xb2\xee\x82\xb8";
    vt_feed(&vt, pl, sizeof(pl) - 1);
  }
  memset(&s, 0, sizeof(s));
  s.g = g;
  s.t = &th;
  s.cw = cw;
  s.ch = ch;
  s.ascent = ch * 3 / 4;
  memset(&f, 0, sizeof(f));
  frame_resize(&f, 3 * cw, ch);
  draw_scene(&f, &s);
  red = theme_color(&th, COL_IDX(1), 1);
  bg = th.bg;
  /* U+E0B0, solid right arrow: left edge solid, tip on the right edge */
  for (solid = 1, y = 1; y < ch - 1; y++) solid &= (f.px[y * f.w] == red);
  check_int("powerline E0B0 left edge is solid", solid, 1);
  check_int("powerline E0B0 tip reaches the right edge",
            f.px[(ch / 2) * f.w + cw - 1] != bg, 1);
  check_int("powerline E0B0 corner stays background", f.px[cw - 1] == bg, 1);
  /* U+E0B2, solid left arrow: the mirror image */
  for (solid = 1, y = 1; y < ch - 1; y++) solid &= (f.px[y * f.w + 2 * cw - 1] == red);
  check_int("powerline E0B2 right edge is solid", solid, 1);
  check_int("powerline E0B2 tip reaches the left edge",
            f.px[(ch / 2) * f.w + cw] != bg, 1);
  /* U+E0B8, lower left triangle: the whole bottom row, not the top right */
  for (solid = 1, y = 2 * cw; y < 3 * cw - 1; y++)
    solid &= (f.px[(ch - 1) * f.w + y] == red);
  check_int("powerline E0B8 bottom row is solid", solid, 1);
  check_int("powerline E0B8 top right stays background", f.px[3 * cw - 1] == bg, 1);
  frame_free(&f);
  vt_free(&vt);
  grid_free(g);
}


static const char *const demo =
  "\033]0;MMC:~/w/mmc\007"
  "\033[32mmarjon@DESKTOP \033[0;44;97;1m MMC \033[0m \033[1m~/w/mmc\033[0;36m (main)\033[0m\r\n"
  "\033[32m$\033[0m ls -l\r\n"
  "drwxr-xr-x  \033[1;34mlogo/\033[0m   -rw-r--r--  mmc.c   -rwxr-xr-x  \033[32mmmc-term.exe\033[0m\r\n"
  "\r\n"
  " normal \033[1mbold\033[0m \033[3mitalic\033[0m \033[1;3mbold italic\033[0m "
  "\033[4munderline\033[0m \033[9mstrike\033[0m \033[2mdim\033[0m \033[7mreverse\033[0m\r\n"
  "\r\n"
  " \033[30m black \033[31m red \033[32m green \033[33m yellow \033[34m blue "
  "\033[35m magenta \033[36m cyan \033[37m white \033[0m\r\n"
  " \033[90m black \033[91m red \033[92m green \033[93m yellow \033[94m blue "
  "\033[95m magenta \033[96m cyan \033[97m white \033[0m\r\n"
  " \033[40m  \033[41m  \033[42m  \033[43m  \033[44m  \033[45m  \033[46m  \033[47m  "
  "\033[100m  \033[101m  \033[102m  \033[103m  \033[104m  \033[105m  \033[106m  \033[107m  \033[0m"
  "  \033[48;2;47;155;255m \033[48;2;44;169;217m \033[48;2;41;183;180m "
  "\033[48;2;38;197;143m \033[48;2;34;211;107m \033[0m truecolor\r\n"
  "\r\n"
  " \xE2\x94\x8C\xE2\x94\x80\xE2\x94\x80\xE2\x94\xAC\xE2\x94\x80\xE2\x94\x80\xE2\x94\x90  "
  "\xE2\x95\x94\xE2\x95\x90\xE2\x95\x90\xE2\x95\xA6\xE2\x95\x90\xE2\x95\x90\xE2\x95\x97  "
  "\xE2\x94\x8F\xE2\x94\x81\xE2\x94\x81\xE2\x94\xB3\xE2\x94\x81\xE2\x94\x81\xE2\x94\x93  "
  "\xE2\x96\x81\xE2\x96\x82\xE2\x96\x83\xE2\x96\x84\xE2\x96\x85\xE2\x96\x86\xE2\x96\x87\xE2\x96\x88 "
  "\xE2\x96\x91\xE2\x96\x92\xE2\x96\x93\xE2\x96\x88 \xE2\x96\x98\xE2\x96\x9D\xE2\x96\x96\xE2\x96\x97\xE2\x96\x9A\xE2\x96\x9E\r\n"
  " \xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80\xE2\x94\xBC\xE2\x94\x80\xE2\x94\x80\xE2\x94\xA4  "
  "\xE2\x95\xA0\xE2\x95\x90\xE2\x95\x90\xE2\x95\xAC\xE2\x95\x90\xE2\x95\x90\xE2\x95\xA3  "
  "\xE2\x94\xA3\xE2\x94\x81\xE2\x94\x81\xE2\x95\x8B\xE2\x94\x81\xE2\x94\x81\xE2\x94\xAB  "
  "\xE2\x95\x9E\xE2\x95\x90\xE2\x95\xA1 \xE2\x95\x9F\xE2\x94\x80\xE2\x95\xA2 \xE2\x95\xA4 \xE2\x95\xA7\r\n"
  " \xE2\x94\x94\xE2\x94\x80\xE2\x94\x80\xE2\x94\xB4\xE2\x94\x80\xE2\x94\x80\xE2\x94\x98  "
  "\xE2\x95\x9A\xE2\x95\x90\xE2\x95\x90\xE2\x95\xA9\xE2\x95\x90\xE2\x95\x90\xE2\x95\x9D  "
  "\xE2\x94\x97\xE2\x94\x81\xE2\x94\x81\xE2\x94\xBB\xE2\x94\x81\xE2\x94\x81\xE2\x94\x9B  "
  "caf\xC3\xA9 na\xC3\xAFve \xC3\xB1 \xE2\x86\x92 \xE2\x9C\x93 \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E \xED\x95\x9C\xEA\xB8\x80\r\n"
  "\r\n"
  "\033[32m$\033[0m git log --oneline\r\n"
  "\033[33m7e7950d\033[0m add the terminal window\r\n"
  "\033[33m2f9bff1\033[0m the quick brown fox jumps over the lazy dog 0123456789\r\n"
  "\033[32m$\033[0m echo selected text here";


static int render (const char *out, const char *theme_name, const char *font) {
  Config c;
  Theme th;
  Grid *g;
  Vt vt;
  Frame f;
  Scene s;
  Menu m;
  int i;
  config_defaults(&c);
  if (font != NULL) strncpy(c.font, font, sizeof(c.font) - 1);
  if (font_init(&c) != 0) {
    printf("no font found\n");
    return 1;
  }
  font_set_px((float)c.font_size * 96.0f / 72.0f);
  theme_apply(&th, theme_find(theme_name), &c);
  g = grid_new(84, 22, 1000);
  vt_init(&vt, g);
  for (i = 0; i < 40; i++) vt_feed(&vt, "history line\r\n", 14);
  vt_feed(&vt, "\033[2J\033[H", 7);
  vt_feed(&vt, demo, strlen(demo));
  memset(&s, 0, sizeof(s));
  memset(&m, 0, sizeof(m));
  s.g = g;
  s.t = &th;
  s.pad = 4;
  s.strip = 3;
  s.head = 30;	/* our own title bar, with the mouse over a button */
  s.title = "MMC:~/w/mmc";
  s.hot_button = (strcmp(theme_name, "light") == 0) ? 2 : 1;
  s.cw = font_cell_w();
  s.ch = font_cell_h();
  s.ascent = font_ascent();
  s.focused = s.blink_on = 1;
  s.has_sel = 1;
  s.sy0 = s.sy1 = g->cy;
  s.sx0 = 7;
  s.sx1 = 19;
  s.bar_alpha = 200;
  m.open = 1;
  m.n = 6;
  m.label[0] = "Copy";        m.hint[0] = "Ctrl+Shift+C";
  m.label[1] = "Paste";       m.hint[1] = "Ctrl+Shift+V";
  m.label[2] = NULL;
  m.label[3] = "Light theme"; m.hint[3] = "Ctrl+Shift+T";
  m.label[4] = "New window";  m.hint[4] = "Ctrl+Shift+N";
  m.label[5] = "About mmc-term";
  m.hot = 3;
  m.w = 30 * s.cw;
  m.h = 5 * (s.ch + 10) + 9 + 12;
  m.x = 50 * s.cw;
  m.y = 16 * s.ch - 40;
  s.menu = &m;
  memset(&f, 0, sizeof(f));
  frame_resize(&f, 2 * s.pad + g->cols * s.cw + 8,
               s.head + s.strip + 2 * s.pad + g->rows * s.ch);
  draw_scene(&f, &s);
  printf("font: %s, cell %dx%d, frame %dx%d\n", font_name(), s.cw, s.ch, f.w, f.h);
  return frame_save_bmp(&f, out);
}


int main (int argc, char **argv) {
  if (argc >= 3 && strcmp(argv[1], "replay") == 0) return replay(argv[2]);
  if (argc >= 3 && strcmp(argv[1], "render") == 0)
    return render(argv[2], argc > 3 ? argv[3] : "dark", argc > 4 ? argv[4] : NULL);
  test_core();
  test_modes();
  test_contrast();
  test_powerline();
  printf("%d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
