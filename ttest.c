/*
** ttest.c - headless tests for the core of mmc-term
**
**   ttest                   run the checks
**   ttest replay FILE       feed a captured terminal stream, print the screen
**   ttest render OUT.bmp [light|dark] [FONT|-] [POINTS] [cleartype|gray|stb]
**                                       draw a sample screen to an image
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


/* one row of the screen as it is, not joined with the next */
static void check_row (T *t, int y, const char *what, const char *want) {
  char *got = grid_text(t->g, 0, y, t->g->cols - 1, y);
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
            "\033[2;5R\033[0n\033[?62;4;22c");
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

  /* reflow: a narrower or wider window wraps the text again */
  t_open(&t, 10, 6, 100);
  feed(&t, "0123456789abcdef\r\nxy");
  grid_resize(t.g, 20, 6);
  check_row(&t, 0, "wider: the wrapped line is one again", "0123456789abcdef");
  check_row(&t, 1, "wider: next line", "xy");
  check_int("wider: not wrapped", t.g->screen[0].wrapped, 0);
  check_int("wider: cursor x", t.g->cx, 2);
  check_int("wider: cursor y", t.g->cy, 1);
  grid_resize(t.g, 5, 6);
  check_row(&t, 0, "narrower: row 1", "01234");
  check_row(&t, 2, "narrower: row 3", "abcde");
  check_row(&t, 3, "narrower: row 4", "f");
  check_row(&t, 4, "narrower: the short line", "xy");
  check_int("narrower: cursor y", t.g->cy, 4);
  check_int("narrower: cursor x", t.g->cx, 2);
  t_close(&t);

  t_open(&t, 10, 4, 100);
  feed(&t, "0123456789");	/* the cursor waits at the end of a full row */
  grid_resize(t.g, 5, 4);
  check_int("full row: cursor stays at its end", t.g->cx * 10 + t.g->cy, 41);
  feed(&t, "X");
  check_row(&t, 2, "full row: the next character goes below", "X");
  t_close(&t);

  t_open(&t, 10, 4, 100);
  feed(&t, "abcdefgh\xe4\xb8\xad\xe6\x96\x87");	/* two wide ones */
  grid_resize(t.g, 9, 4);
  check_row(&t, 0, "wide: no half character at the edge", "abcdefgh");
  check_row(&t, 1, "wide: both on the next row", "\xe4\xb8\xad\xe6\x96\x87");
  t_close(&t);

  t_open(&t, 10, 2, 100);
  feed(&t, "aaaaaaaaaaaaaaa\r\nb\r\nc");
  grid_resize(t.g, 20, 2);
  check_int("scrollback reflowed", t.g->sb_len, 1);
  {
    char *s = grid_text(t.g, 0, -1, 19, -1);
    check_str("scrollback: one line again", s, "aaaaaaaaaaaaaaa");
    free(s);
  }
  check_screen(&t, "scrollback: screen", "b\nc");
  feed(&t, "\033[?1049h\033[2J\033[Hfull screen app");
  grid_resize(t.g, 5, 2);
  feed(&t, "\033[?1049l");
  check_row(&t, 1, "under an app: the shell screen is reflowed too", "c");
  check_int("under an app: cursor comes back", t.g->cy, 1);
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
  const Theme *t;
  int k, i;
  for (k = 0; (t = theme_at(k)) != NULL; k++) {
    char what[64];
    check_int("fg contrast >= 7", theme_contrast(t->fg, t->bg) >= 7.0, 1);
    check_int("selected text contrast >= 4.5", theme_contrast(t->fg, t->sel_bg) >= 4.5, 1);
    check_int("menu text contrast >= 7", theme_contrast(t->ui_text, t->ui) >= 7.0, 1);
    for (i = 0; i < 16; i++) {
      double c = theme_contrast(t->pal[i], t->bg);
      /* "black" on dark and "white" on light are background-like on purpose */
      int skip = t->dark ? (i == 0) : (i == 15 || i == 7);
      double need = (t->dark && i == 8) ? 3.0 : 4.5;
      if (!t->dark && i == 7) { skip = 0; need = 3.0; }
      if (!t->dark && i == 15) skip = 0;
      if (skip) continue;
      sprintf(what, "%s color%d contrast %.2f >= %.1f", t->name, i, c, need);
      check_int(what, c >= need, 1);
    }
  }
  check_str("default is dark", theme_find("default")->name, "dark");
  check_str("unknown theme is dark", theme_find("nope")->name, "dark");
  check_str("gruv is gruvbox", theme_find("gruv")->name, "gruvbox");
  check_str("dark -> green", theme_next("dark")->name, "green");
  check_str("light -> dark (round)", theme_next("light")->name, "dark");
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
  "marks: cafe\xCC\x81 nai\xCC\x88ve A\xCC\x8A n\xCC\x83  "
  "\033]8;;https://example.com/docs\033\\a hyperlink\033]8;;\033\\  "
  "\033]8;;https://example.com/two\033\\hovered link\033]8;;\033\\\r\n"
  "\033[4:2mdouble\033[0m \033[4:3;58:2::255:80:80mcurly red\033[0m \033[4:4mdotted\033[0m "
  "\033[4:5mdashed\033[0m \033[53moverline\033[0m \033[5mblink\033[0m\r\n"
  "sixel: \033Pq#1;2;13;61;100#2;2;13;83;42#3;2;95;30;30"
  "#1!40~-#1!40~-#2!40~-#3!40~\033\\\r\n"
  "\033[32m$\033[0m git log --oneline\r\n"
  "\033[33m7e7950d\033[0m add the terminal window\r\n"
  "\033[33m2f9bff1\033[0m the quick brown fox jumps over the lazy dog 0123456789\r\n"
  "\033[32m$\033[0m echo selected text here";


static int render (const char *out, const char *theme_name, const char *font,
                   int size, const char *smooth) {
  Config c;
  Theme th;
  Grid *g;
  Vt vt;
  Frame f;
  Scene s;
  Menu m;
  int i;
  config_defaults(&c);
  if (font != NULL && strcmp(font, "-") != 0)
    strncpy(c.font, font, sizeof(c.font) - 1);
  if (size > 0) c.font_size = size;
  if (smooth != NULL)
    c.smoothing = strcmp(smooth, "stb") == 0 ? SMOOTH_STB :
                  strcmp(smooth, "gray") == 0 ? SMOOTH_GRAY : SMOOTH_CLEARTYPE;
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
  s.focused = s.blink_on = s.text_blink_on = 1;
  s.has_sel = 1;
  s.sy0 = s.sy1 = g->cy;
  s.sx0 = 7;
  s.sx1 = 19;
  s.bar_alpha = 200;
  s.find = "find: fox\xe2\x96\x8f  1/1";
  {	/* the mouse is on the second link */
    int y, x;
    for (y = 0; y < g->rows; y++)
      for (x = 0; x < g->cols; x++)
        if (g->screen[y].c[x].link == 2) {
          if (!s.has_hot) {
            s.has_hot = 1;
            s.hx0 = x;
            s.hy0 = y;
          }
          s.hx1 = x;
          s.hy1 = y;
        }
  }
  m.open = 1;
  m.n = 6;
  m.label[0] = "Copy";        m.hint[0] = "Ctrl+Shift+C";
  m.label[1] = "Paste";       m.hint[1] = "Ctrl+Shift+V";
  m.label[2] = NULL;
  m.label[3] = "Green theme"; m.hint[3] = "Ctrl+Shift+L";
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


/* accents and emoji sequences stay in their cell; hyperlinks; mode queries */
static void test_clusters_links (void) {
  T t;
  const uint32_t *cps;
  int n;
  char *s;

  t_open(&t, 20, 3, 10);
  feed(&t, "e\xcc\x81x");	/* e + U+0301, then x */
  n = grid_cps(t.g, &t.g->screen[0].c[0], &cps);
  check_int("accent joins its letter", n, 2);
  check_int("the letter first", (long)grid_base(t.g, &t.g->screen[0].c[0]), 'e');
  check_int("the next letter is in the next cell", (long)t.g->screen[0].c[1].ch, 'x');
  check_int("cursor after 2 cells", t.g->cx, 2);
  s = grid_text(t.g, 0, 0, 19, 0);
  check_str("copying keeps the accent", s, "e\xcc\x81x");
  free(s);
  feed(&t, "\r\na\xcc\x81");	/* the same cluster again: stored once */
  check_int("same cluster, same place", (long)(t.g->screen[1].c[0].ch == t.g->screen[0].c[0].ch), 0);
  feed(&t, "\r\ne\xcc\x81");
  check_int("same cluster, same place", (long)(t.g->screen[2].c[0].ch == t.g->screen[0].c[0].ch), 1);
  t_close(&t);

  t_open(&t, 20, 3, 10);
  /* man + ZWJ + laptop: one emoji, two cells */
  feed(&t, "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x92\xbb!");
  check_int("ZWJ sequence: one wide cell", t.g->screen[0].c[0].attr & A_WIDE ? 1 : 0, 1);
  check_int("ZWJ sequence: 3 code points", grid_cps(t.g, &t.g->screen[0].c[0], &cps), 3);
  check_int("ZWJ sequence: ! right after", (long)t.g->screen[0].c[2].ch, '!');
  /* thumbs up + skin tone */
  feed(&t, "\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd.");
  check_int("skin tone joins", grid_cps(t.g, &t.g->screen[0].c[3], &cps), 2);
  check_int("skin tone: . after", (long)t.g->screen[0].c[5].ch, '.');
  feed(&t, "\xe2\x80\x8d\r\nz");	/* a joiner, then a new line: nothing to join */
  check_int("a joiner does not reach over a new line", (long)t.g->screen[1].c[0].ch, 'z');
  grid_resize(t.g, 3, 3);	/* reflow keeps the clusters */
  s = grid_text(t.g, 0, 0, 2, 0);
  check_str("reflow keeps an emoji sequence", s, "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x92\xbb!");
  free(s);
  t_close(&t);

  t_open(&t, 30, 3, 10);
  feed(&t, "see \033]8;;https://example.com/a\033\\here\033]8;;\033\\ now");
  check_int("OSC 8: plain text before", t.g->screen[0].c[3].link, 0);
  check_int("OSC 8: linked text", t.g->screen[0].c[4].link != 0, 1);
  check_str("OSC 8: the address", grid_link(t.g, t.g->screen[0].c[7].link), "https://example.com/a");
  check_int("OSC 8: ends", t.g->screen[0].c[9].link, 0);
  feed(&t, "\033]8;id=1;https://example.com/a\007x\033]8;;\007");
  check_int("OSC 8: the same address again is not stored twice", t.g->nlinks, 1);
  t_close(&t);

  t_open(&t, 20, 3, 10);
  feed(&t, "\033[?2004h\033[?2004$p\033[?1049$p\033[?4242$p\033[4$p");
  check_str("DECRQM replies", replies.s ? replies.s : "",
            "\033[?2004;1$y\033[?1049;2$y\033[?4242;0$y\033[4;2$y");
  t_close(&t);

  t_open(&t, 20, 3, 10);
  feed(&t, "\033[?2026h");
  check_int("2026: held", t.g->sync, 1);
  feed(&t, "\033[?2026$p");
  check_str("2026 is known", replies.s ? replies.s : "", "\033[?2026;1$y");
  feed(&t, "\033[?2026l");
  check_int("2026: let go", t.g->sync, 0);
  t_close(&t);

  t_open(&t, 20, 3, 10);
  feed(&t, "\033[?25l\033[?7l\033[1;31mA\033[5;10r\033[!p");
  check_int("DECSTR: cursor shown", t.g->cursor_on, 1);
  check_int("DECSTR: autowrap", t.g->autowrap, 1);
  check_int("DECSTR: plain pen", t.g->pen.attr, 0);
  check_int("DECSTR: whole screen scrolls", t.g->bot, 2);
  check_str("DECSTR: the text stays", t.g->screen[0].c[0].ch == 'A' ? "A" : "?", "A");
  feed(&t, "\033[>q");
  check_int("XTVERSION answers", replies.s != NULL && strstr(replies.s, "\033P>|" TERM_NAME) != NULL, 1);
  t_close(&t);

  t_open(&t, 20, 6, 10);	/* sixel: an image on the cells it covers */
  {
    const GridImage *im;
    int tile;
    t.g->cell_w = 10;
    t.g->cell_h = 20;
    feed(&t, "ab\033Pq#1;2;100;0;0#1!15~-#2;2;0;0;100!15~-!15~-!15~\033\\z");
    im = grid_image(t.g, t.g->screen[0].c[2].ch, &tile);
    check_int("sixel: an image cell", (t.g->screen[0].c[2].ch & CH_IMAGE) != 0, 1);
    check_int("sixel: its size", im != NULL ? im->w * 100 + im->h : -1, 15 * 100 + 24);
    check_int("sixel: 2 cells across, 2 rows down", (t.g->screen[1].c[3].ch & CH_IMAGE) != 0, 1);
    check_int("sixel: the first band is red", im != NULL ? (long)im->px[0] : 0, (long)0xFFFF0000u);
    check_int("sixel: the second band is blue", im != NULL ? (long)im->px[15 * 6] : 0, (long)0xFF0000FFu);
    check_int("sixel: the cursor goes below it", t.g->cy * 100 + t.g->cx, 203);
    check_row(&t, 2, "sixel: text after it", "  z");
    feed(&t, "\033[1;1H\033[2K");
    check_int("sixel: erased like text", (long)t.g->screen[0].c[2].ch, 0);
    feed(&t, "\033[?2;1;0S");
    check_str("XTSMGRAPHICS: the most pixels", replies.s ? replies.s : "", "\033[?2;0;200;120S");
  }
  t_close(&t);

  t_open(&t, 20, 3, 10);	/* the kitty keyboard protocol: a stack of flags per screen */
  feed(&t, "\033[?u");
  check_str("kitty: none at first", replies.s ? replies.s : "", "\033[?0u");
  feed(&t, "\033[>1u\033[>9u");
  check_int("kitty: pushed", grid_kitty(t.g), 9);
  feed(&t, "\033[<u");
  check_int("kitty: popped", grid_kitty(t.g), 1);
  feed(&t, "\033[=8;2u");
  check_int("kitty: added", grid_kitty(t.g), 9);
  feed(&t, "\033[=1;3u");
  check_int("kitty: taken away", grid_kitty(t.g), 8);
  feed(&t, "\033[?1049h");
  check_int("kitty: the other screen has its own", grid_kitty(t.g), 0);
  feed(&t, "\033[?1049l\033[<5u");
  check_int("kitty: popping more than there is", grid_kitty(t.g), 0);
  feed(&t, "\033[1;1Hab\033[s\033[1;1H\033[u");	/* CSI u without a prefix still restores */
  check_int("CSI u is still restore cursor", t.g->cx, 2);
  t_close(&t);

  t_open(&t, 20, 8, 10);	/* DECOM: rows counted inside the scroll region */
  feed(&t, "\033[3;5r\033[?6h");
  check_int("DECOM: home is the region's top", t.g->cy, 2);
  feed(&t, "\033[2;4H");
  check_int("DECOM: CUP inside the region", t.g->cy * 100 + t.g->cx, 303);
  feed(&t, "\033[9;1H");
  check_int("DECOM: CUP stops at the region's bottom", t.g->cy, 4);
  feed(&t, "\033[3;1H\033[6n");
  check_str("DECOM: the cursor report counts from the region", replies.s ? replies.s : "", "\033[3;1R");
  feed(&t, "\033[?6l");
  check_int("DECOM off: home is the screen's top", t.g->cy, 0);
  t_close(&t);

  t_open(&t, 20, 3, 10);	/* underline styles and colors, blink, overline */
  feed(&t, "\033[4:3;58;5;196mA\033[21mB\033[24;5;53mC\033[0mD\033[4mE\033[4:0mF");
  check_int("curly", (t.g->screen[0].c[0].attr & A_ULSTYLE) >> UL_SHIFT, 2);
  check_int("underline color", (long)t.g->screen[0].c[0].ul, (long)COL_IDX(196));
  check_int("21 is double", (t.g->screen[0].c[1].attr & A_ULSTYLE) >> UL_SHIFT, 1);
  check_int("24 ends it, 5 blinks, 53 overlines",
            t.g->screen[0].c[2].attr & (A_UNDER | A_BLINK | A_OVER), A_BLINK | A_OVER);
  check_int("0 resets the underline color", (long)t.g->screen[0].c[3].ul, (long)COL_DEFAULT);
  check_int("4 is single", t.g->screen[0].c[4].attr & (A_UNDER | A_ULSTYLE), A_UNDER);
  check_int("4:0 is none", t.g->screen[0].c[5].attr & A_UNDER, 0);
  t_close(&t);
}


/* drawing only the changed rows gives the same picture as drawing it all */
static void test_partial (void) {
  Config c;
  Theme th;
  T t;
  Frame a, b;
  Scene s;
  int r, same = 1;
  size_t i;
  config_defaults(&c);
  if (font_init(&c) != 0) return;	/* no font here: nothing to compare */
  font_set_px((float)c.font_size * 96.0f / 72.0f);
  theme_apply(&th, theme_find("dark"), &c);
  t_open(&t, 40, 8, 100);
  feed(&t, "\033[1;32mgreen bold\033[0m plain\r\n\033[3mitalic lean\033[0m\r\n"
           "\033[44m blue bg \033[0m and \033[4:3mcurly\033[0m\r\n$ ");
  memset(&s, 0, sizeof(s));
  s.g = t.g;
  s.t = &th;
  s.pad = 4;
  s.strip = 3;
  s.cw = font_cell_w();
  s.ch = font_cell_h();
  s.ascent = font_ascent();
  s.focused = s.blink_on = s.text_blink_on = 1;
  memset(&a, 0, sizeof(a));
  memset(&b, 0, sizeof(b));
  frame_resize(&a, 2 * s.pad + 40 * s.cw, s.strip + 2 * s.pad + 8 * s.ch);
  frame_resize(&b, a.w, a.h);
  draw_scene(&a, &s);
  t.g->all_dirty = 0;
  for (r = 0; r < t.g->rows; r++) t.g->screen[r].dirty = 0;
  feed(&t, "ls\r\nfile.txt \033[1;34mdir/\033[0m\r\n$ ");	/* typing, output, a new prompt */
  check_int("partial: something to draw", draw_scene_rows(&a, &s), 1);
  draw_scene(&b, &s);
  for (i = 0; i < (size_t)a.w * (size_t)a.h; i++)
    if (a.px[i] != b.px[i]) same = 0;
  check_int("partial: the same picture as a full one", same, 1);
  t.g->all_dirty = 0;
  for (r = 0; r < t.g->rows; r++) t.g->screen[r].dirty = 0;
  check_int("partial: nothing changed, nothing drawn", draw_scene_rows(&a, &s), 0);
  /* the window without the focus: a hollow cursor, after the shell's usual dance */
  s.focused = 0;
  draw_scene(&a, &s);
  t.g->all_dirty = 0;
  for (r = 0; r < t.g->rows; r++) t.g->screen[r].dirty = 0;
  feed(&t, "\033[?25la\033[?25h");
  draw_scene_rows(&a, &s);
  t.g->all_dirty = 0;
  for (r = 0; r < t.g->rows; r++) t.g->screen[r].dirty = 0;
  feed(&t, "\033[?25l\033[1Db\033[?25h");
  draw_scene_rows(&a, &s);
  draw_scene(&b, &s);
  same = 1;
  for (i = 0; i < (size_t)a.w * (size_t)a.h; i++)
    if (a.px[i] != b.px[i]) same = 0;
  check_int("partial: unfocused cursor the same", same, 1);
  frame_free(&a);
  frame_free(&b);
  t_close(&t);
}


/* ttest bench: how long a whole frame takes, and one with a changed line */
static int bench (void) {
  Config c;
  Theme th;
  Grid *g;
  Vt vt;
  Frame f;
  Scene s;
  int i, y;
  long long t0, t1, t2;
  config_defaults(&c);
  if (font_init(&c) != 0) return 1;
  font_set_px((float)c.font_size * 96.0f / 72.0f);
  theme_apply(&th, theme_find("dark"), &c);
  g = grid_new(120, 40, 1000);
  vt_init(&vt, g);
  for (y = 0; y < 40; y++) {
    char line[200];
    sprintf(line, "\033[3%dm%03d the quick brown fox jumps over the lazy dog "
            "\033[1mbold\033[0m 0123456789 abcdefghijklmnopqrstuvwxyz ABCDEFG%s",
            y % 8, y, y < 39 ? "\r\n" : "");
    vt_feed(&vt, line, strlen(line));
  }
  memset(&s, 0, sizeof(s));
  s.g = g;
  s.t = &th;
  s.pad = 4;
  s.strip = 3;
  s.cw = font_cell_w();
  s.ch = font_cell_h();
  s.ascent = font_ascent();
  s.focused = s.blink_on = s.text_blink_on = 1;
  memset(&f, 0, sizeof(f));
  frame_resize(&f, 2 * s.pad + g->cols * s.cw, s.strip + 2 * s.pad + g->rows * s.ch);
  t0 = os_now_us();
  for (i = 0; i < 200; i++) draw_scene(&f, &s);
  t1 = os_now_us();
  for (i = 0; i < 200; i++) {	/* the shell echoes one key: one line changes */
    int r;
    vt_feed(&vt, "x", 1);
    draw_scene_rows(&f, &s);
    g->all_dirty = 0;
    for (r = 0; r < g->rows; r++) g->screen[r].dirty = 0;
  }
  t2 = os_now_us();
  printf("full frame: %.2f ms, one changed line: %.3f ms (%dx%d cells, %dx%d px)\n",
         (double)(t1 - t0) / 200.0 / 1000.0, (double)(t2 - t1) / 200.0 / 1000.0,
         g->cols, g->rows, f.w, f.h);
  return 0;
}


int main (int argc, char **argv) {
  if (argc >= 2 && strcmp(argv[1], "bench") == 0) return bench();
  if (argc >= 3 && strcmp(argv[1], "replay") == 0) return replay(argv[2]);
  if (argc >= 3 && strcmp(argv[1], "render") == 0)
    return render(argv[2], argc > 3 ? argv[3] : "dark", argc > 4 ? argv[4] : NULL,
                  argc > 5 ? atoi(argv[5]) : 0, argc > 6 ? argv[6] : NULL);
  test_core();
  test_modes();
  test_contrast();
  test_powerline();
  test_clusters_links();
  test_partial();
  printf("%d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
