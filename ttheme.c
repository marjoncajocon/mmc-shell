/*
** ttheme.c - themes and configuration of mmc-term
**
** Two built-in themes made from the mmc logo colors: navy #0B1220,
** blue #2F9BFF, green #22D36B. The config file is "key=value" lines.
*/

#include "mterm.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static const Theme themes[] = {
  {"dark", 1,
   0x0B1220, 0xE6EDF7, 0x22D36B, 0x0B1220, 0x1F4E80, 0x2F9BFF, 0x22D36B,
   0x16213A, 0xE6EDF7,
   {0x1C2940, 0xFF5C7A, 0x22D36B, 0xFFD166, 0x2F9BFF, 0xC792EA, 0x3DDBD9,
    0xC9D4E5,
    0x5C6F8F, 0xFF8FA3, 0x5DF2A0, 0xFFE29A, 0x7CC4FF, 0xDDB6F2, 0x7FEDEA,
    0xFFFFFF}},
  {"light", 0,
   0xF3FBF7, 0x0B1220, 0x1565C0, 0xFFFFFF, 0xB9DDF7, 0x2F9BFF, 0x22D36B,
   0xFFFFFF, 0x0B1220,
   {0x0B1220, 0xC2253D, 0x0B7A3B, 0x8A5D00, 0x1565C0, 0x7B3FA0, 0x006F6D,
    0x55657A,
    0x3A4A66, 0xD12D48, 0x0A8040, 0x946300, 0x1A6FD0, 0x8E49B8, 0x007B78,
    0x1B2535}}
};


const Theme *theme_find (const char *name) {
  size_t i;
  for (i = 0; i < sizeof(themes) / sizeof(themes[0]); i++)
    if (m_stricmp(themes[i].name, name) == 0) return &themes[i];
  return &themes[0];
}


/* the theme with the color overrides of the config file on top */
void theme_apply (Theme *out, const Theme *base, const Config *c) {
  int i;
  *out = *base;
  if (c->has_bg) out->bg = c->bg;
  if (c->has_fg) out->fg = c->fg;
  if (c->has_cursor) out->cursor = c->cursor_color;
  for (i = 0; i < 16; i++)
    if (c->has_pal[i]) out->pal[i] = c->pal[i];
}


/* cell color -> 0xRRGGBB */
uint32_t theme_color (const Theme *t, uint32_t col, int is_fg) {
  static const unsigned char ramp[6] = {0, 95, 135, 175, 215, 255};
  unsigned idx = col & 0xFF;
  switch (COL_TAG(col)) {
    case 1:
      if (idx < 16) return t->pal[idx];
      if (idx < 232) {	/* 6x6x6 color cube */
        idx -= 16;
        return ((uint32_t)ramp[idx / 36] << 16) |
               ((uint32_t)ramp[(idx / 6) % 6] << 8) | ramp[idx % 6];
      }
      idx = 8 + (idx - 232) * 10;	/* gray ramp */
      return (idx << 16) | (idx << 8) | idx;
    case 2: return col & 0xFFFFFF;
    default: return is_fg ? t->fg : t->bg;
  }
}


static double channel (uint32_t v) {
  double c = (double)(v & 0xFF) / 255.0;
  return (c <= 0.03928) ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}


/* WCAG contrast ratio, 1 .. 21 */
double theme_contrast (uint32_t a, uint32_t b) {
  double la = 0.2126 * channel(a >> 16) + 0.7152 * channel(a >> 8) +
              0.0722 * channel(a);
  double lb = 0.2126 * channel(b >> 16) + 0.7152 * channel(b >> 8) +
              0.0722 * channel(b);
  return (la > lb) ? (la + 0.05) / (lb + 0.05) : (lb + 0.05) / (la + 0.05);
}


/*
** {==================================================================
** Configuration file
** ===================================================================
*/

static const char *const default_config =
  "# mmcterm.conf - settings of the mmc-term window\n"
  "# Remove the '#' in front of a line to change it.\n"
  "\n"
  "theme=dark                # dark or light (Ctrl+Shift+T switches)\n"
  "#font=Hack                # Hack (comes with mmc), Cascadia Mono, Consolas ...\n"
  "#font_file=/usr/share/fonts/MyFont.ttf   # or any .ttf file\n"
  "font_size=11\n"
  "cols=100\n"
  "rows=30\n"
  "padding=4                 # space around the text, in pixels\n"
  "titlebar=custom           # custom: our own header; native: the system one\n"
  "scrollback=10000\n"
  "bold=yes                  # no: bold text in the normal weight\n"
  "cursor=block              # block, underline or bar\n"
  "cursor_blink=yes\n"
  "opacity=100               # 30 .. 100\n"
  "copy_on_select=yes\n"
  "#shell=mmc                # program to start\n"
  "\n"
  "# colors, as #RRGGBB (they override the theme)\n"
  "#bg=#0B1220\n"
  "#fg=#E6EDF7\n"
  "#cursor_color=#22D36B\n"
  "#color4=#2F9BFF           # color0 .. color15\n";


void config_defaults (Config *c) {
  memset(c, 0, sizeof(*c));
  strcpy(c->theme, "dark");
  c->font_size = 11;
  c->cols = 100;
  c->rows = 30;
  c->padding = 4;
  c->scrollback = 10000;
  c->cursor_blink = 1;
  c->opacity = 100;
  c->copy_on_select = 1;
}


static int parse_bool (const char *v) {
  return m_stricmp(v, "yes") == 0 || m_stricmp(v, "true") == 0 ||
         m_stricmp(v, "on") == 0 || strcmp(v, "1") == 0;
}


static int parse_color (const char *v, uint32_t *out) {
  char *end;
  unsigned long n;
  if (*v == '#') v++;
  if (strlen(v) != 6) return 0;
  n = strtoul(v, &end, 16);
  if (*end != '\0') return 0;
  *out = (uint32_t)n;
  return 1;
}


static int clamp (int v, int lo, int hi) {
  return v < lo ? lo : v > hi ? hi : v;
}


static void copy_text (char *dst, size_t size, const char *src) {
  strncpy(dst, src, size - 1);
  dst[size - 1] = '\0';
}


static void config_set (Config *c, const char *key, const char *v) {
  if (strcmp(key, "theme") == 0) copy_text(c->theme, sizeof(c->theme), v);
  else if (strcmp(key, "font") == 0) copy_text(c->font, sizeof(c->font), v);
  else if (strcmp(key, "font_file") == 0)
    copy_text(c->font_file, sizeof(c->font_file), v);
  else if (strcmp(key, "shell") == 0) copy_text(c->shell, sizeof(c->shell), v);
  else if (strcmp(key, "font_size") == 0) c->font_size = clamp(atoi(v), 6, 72);
  else if (strcmp(key, "cols") == 0) c->cols = clamp(atoi(v), 20, 500);
  else if (strcmp(key, "rows") == 0) c->rows = clamp(atoi(v), 5, 200);
  else if (strcmp(key, "padding") == 0) c->padding = clamp(atoi(v), 0, 64);
  else if (strcmp(key, "scrollback") == 0)
    c->scrollback = clamp(atoi(v), 0, 1000000);
  else if (strcmp(key, "opacity") == 0) c->opacity = clamp(atoi(v), 30, 100);
  else if (strcmp(key, "bold") == 0) c->no_bold = !parse_bool(v);
  else if (strcmp(key, "titlebar") == 0)
    c->native_titlebar = (m_stricmp(v, "native") == 0);
  else if (strcmp(key, "cursor_blink") == 0) c->cursor_blink = parse_bool(v);
  else if (strcmp(key, "copy_on_select") == 0)
    c->copy_on_select = parse_bool(v);
  else if (strcmp(key, "cursor") == 0)
    c->cursor = (m_stricmp(v, "underline") == 0) ? 1 :
                (m_stricmp(v, "bar") == 0) ? 2 : 0;
  else if (strcmp(key, "bg") == 0) c->has_bg = parse_color(v, &c->bg);
  else if (strcmp(key, "fg") == 0) c->has_fg = parse_color(v, &c->fg);
  else if (strcmp(key, "cursor_color") == 0)
    c->has_cursor = parse_color(v, &c->cursor_color);
  else if (strncmp(key, "color", 5) == 0) {
    int i = atoi(key + 5);
    if (i >= 0 && i < 16 && key[5] != '\0')
      c->has_pal[i] = parse_color(v, &c->pal[i]);
  }
}


/* "  key = value   # comment" -> key, value; returns 0 for other lines */
static int split_line (char *line, char **key, char **value) {
  char *eq, *end, *hash;
  while (*line == ' ' || *line == '\t') line++;
  if (*line == '#' || *line == '\0') return 0;
  if ((eq = strchr(line, '=')) == NULL) return 0;
  *eq = '\0';
  for (end = eq; end > line && (end[-1] == ' ' || end[-1] == '\t'); ) *--end = '\0';
  *key = line;
  *value = eq + 1;
  while (**value == ' ' || **value == '\t') (*value)++;
  /* a '#' after a blank starts a comment; "#RRGGBB" at the start does not */
  for (hash = *value + 1; *hash; hash++)
    if (*hash == '#' && (hash[-1] == ' ' || hash[-1] == '\t')) break;
  if (*hash == '#') *hash = '\0';
  for (end = *value + strlen(*value);
       end > *value && strchr(" \t\r", end[-1]) != NULL; )
    *--end = '\0';
  return 1;
}


void config_load (Config *c, const char *native) {
  char *text = read_file(native, NULL);
  char *line;
  if (text == NULL) return;
  for (line = text; *line != '\0'; ) {
    char *end = line + strcspn(line, "\n");
    int last = (*end == '\0');
    char *key, *value;
    *end = '\0';
    if (split_line(line, &key, &value)) config_set(c, key, value);
    if (last) break;
    line = end + 1;
  }
  free(text);
}


void config_save_default (const char *native) {
  OsStat st;
  int fd;
  if (os_stat(native, &st) == 0) return;
  if ((fd = os_open(native, OS_WRITE)) < 0) return;
  fd_puts(fd, default_config);
  os_close(fd);
}


/* changes one setting in the file, keeping everything else as it is */
void config_set_key (const char *native, const char *key, const char *value) {
  char *text = read_file(native, NULL);
  Buf out;
  size_t klen = strlen(key);
  int done = 0, fd;
  char *line;
  buf_init(&out);
  for (line = text ? text : ""; *line != '\0'; ) {
    size_t n = strcspn(line, "\n");
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (!done && strncmp(p, key, klen) == 0 &&
        (p[klen] == '=' || p[klen] == ' ' || p[klen] == '\t')) {
      buf_puts(&out, key);
      buf_putc(&out, '=');
      buf_puts(&out, value);
      done = 1;
    }
    else buf_putn(&out, line, n);
    buf_putc(&out, '\n');
    if (line[n] == '\0') break;
    line += n + 1;
  }
  if (!done) {
    buf_puts(&out, key);
    buf_putc(&out, '=');
    buf_puts(&out, value);
    buf_putc(&out, '\n');
  }
  if ((fd = os_open(native, OS_WRITE)) >= 0) {
    os_write(fd, out.s ? out.s : "", out.len);
    os_close(fd);
  }
  buf_free(&out);
  free(text);
}

/* }================================================================== */
