/*
** ttheme.c - themes and configuration of mmc-term
**
** Built-in themes: the default dark one and a light one made from the
** mmc logo colors (navy #0B1220, blue #2F9BFF, green #22D36B), plus
** green, gruvbox (warm brown/orange) and red dark ones, then the 19 of
** the theme mockups (abyss .. mint). Ctrl+Shift+L goes through them in
** this order. The config file is "key=value" lines.
*/

#include "mterm.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* name, menu title, dark,
   bg, fg, cursor, cursor_text, sel_bg, accent1, accent2, ui, ui_text,
   color0 .. color15 */
static const Theme themes[] = {
  {"dark", "Default", 1,
   0x0B1220, 0xE6EDF7, 0x22D36B, 0x0B1220, 0x1F4E80, 0x2F9BFF, 0x22D36B,
   0x16213A, 0xE6EDF7,
   {0x1C2940, 0xFF5C7A, 0x22D36B, 0xFFD166, 0x2F9BFF, 0xC792EA, 0x3DDBD9,
    0xC9D4E5,
    0x5C6F8F, 0xFF8FA3, 0x5DF2A0, 0xFFE29A, 0x7CC4FF, 0xDDB6F2, 0x7FEDEA,
    0xFFFFFF}},
  {"green", "Green", 1,
   0x0C1A12, 0xD8F5E0, 0x39E27D, 0x0C1A12, 0x1D5C36, 0x39E27D, 0xB5E35F,
   0x132519, 0xD8F5E0,
   {0x1A2E21, 0xFF6B6B, 0x39E27D, 0xE8DC6A, 0x5FB3F5, 0xD393F0, 0x4FE0C0,
    0xC4DCCB,
    0x5F8269, 0xFF9A9A, 0x7DF5A8, 0xF5EC9A, 0x96CFFF, 0xE6B8F7, 0x8AF0DA,
    0xFFFFFF}},
  {"gruvbox", "Gruvbox", 1,
   0x282828, 0xEBDBB2, 0xFE8019, 0x282828, 0x504945, 0xFE8019, 0xFABD2F,
   0x32302F, 0xEBDBB2,
   {0x3C3836, 0xFB5A45, 0xB8BB26, 0xFABD2F, 0x83A598, 0xD3869B, 0x8EC07C,
    0xD5C4A1,
    0x928374, 0xFF7B6B, 0xD0D34A, 0xFFD467, 0xA3C4B8, 0xE8A6B8, 0xAAD69A,
    0xFBF1C7}},
  {"red", "Red", 1,
   0x1A0B0E, 0xF5E1E3, 0xFF4D5E, 0x1A0B0E, 0x6B1E2A, 0xFF4D5E, 0xFF9F43,
   0x251014, 0xF5E1E3,
   {0x2E1519, 0xFF4D5E, 0x5FD38D, 0xFFC857, 0x6FA8FF, 0xE58FE0, 0x5FD8D8,
    0xD9C2C5,
    0x86666E, 0xFF8591, 0x8FEBB1, 0xFFDE94, 0xA3C8FF, 0xF2B8EE, 0x94EDED,
    0xFFFFFF}},
  {"light", "Light", 0,
   0xF3FBF7, 0x0B1220, 0x1565C0, 0xFFFFFF, 0xB9DDF7, 0x2F9BFF, 0x22D36B,
   0xFFFFFF, 0x0B1220,
   {0x0B1220, 0xC2253D, 0x0B7A3B, 0x8A5D00, 0x1565C0, 0x7B3FA0, 0x006F6D,
    0x55657A,
    0x3A4A66, 0xD12D48, 0x0A8040, 0x946300, 0x1A6FD0, 0x8E49B8, 0x007B78,
    0x1B2535}},
  {"abyss", "Abyss", 1,
   0x06161F, 0xCFE8EF, 0x2EE6D6, 0x06161F, 0x15445A, 0x2EE6D6, 0x4FA3FF,
   0x0A212D, 0xCFE8EF,
   {0x12303D, 0xFF6E7F, 0x3FE0A0, 0xF2D06B, 0x4FA3FF, 0xB78CFF, 0x2EE6D6,
    0xBFD6DD,
    0x5A8494, 0xFF9AA6, 0x7AF2C2, 0xF8E39C, 0x8CC4FF, 0xD2B8FF, 0x7FF3EA,
    0xFFFFFF}},
  {"aurora", "Aurora", 1,
   0x0F1420, 0xDDE3F0, 0x7CF5B0, 0x0F1420, 0x2B3557, 0x7CF5B0, 0xB48CFF,
   0x151B2B, 0xDDE3F0,
   {0x1E2638, 0xF7768E, 0x7CF5B0, 0xF5D47A, 0x7AA8FF, 0xB48CFF, 0x6FE3E8,
    0xC3CADB,
    0x646F8A, 0xFF9DB0, 0xA6FFCF, 0xFFE5A3, 0xA3C2FF, 0xCFB5FF, 0x9CF0F3,
    0xFFFFFF}},
  {"ember", "Ember", 1,
   0x1A1411, 0xF0E2D4, 0xFF8A3D, 0x1A1411, 0x4A3326, 0xFF8A3D, 0xFFC24B,
   0x231B17, 0xF0E2D4,
   {0x2C221D, 0xF2594B, 0xA9C25A, 0xFFC24B, 0x6FA6D9, 0xD98BB5, 0x6CC4A8,
    0xD9C8B6,
    0x85705F, 0xFF8070, 0xC5DB7A, 0xFFD67F, 0x96C2EB, 0xEAAED0, 0x8FDBC2,
    0xFFF6EC}},
  {"sakura", "Sakura", 1,
   0x1B1320, 0xF3E4EE, 0xFF8CC6, 0x1B1320, 0x4A2A44, 0xFF8CC6, 0x9FD8FF,
   0x241A2B, 0xF3E4EE,
   {0x2E2133, 0xFF6B8B, 0x8EE3B0, 0xFFD58A, 0x8FB8FF, 0xFF8CC6, 0x7EE0E0,
    0xDCC9D6,
    0x806A84, 0xFF94AC, 0xB2F0CA, 0xFFE4B0, 0xB4CFFF, 0xFFB3DA, 0xA6EEEE,
    0xFFFFFF}},
  {"phosphor", "Phosphor", 1,
   0x050A06, 0x4BFF7A, 0x4BFF7A, 0x050A06, 0x0F3A1C, 0x4BFF7A, 0xB8FF5C,
   0x08120A, 0x4BFF7A,
   {0x103018, 0xFF6A55, 0x4BFF7A, 0xD6FF5C, 0x3FD8A8, 0x8CFFB0, 0x5CFFD6,
    0xA8E6B5,
    0x3A8A52, 0xFF8C7A, 0x8CFFA8, 0xE8FF8C, 0x72EBC4, 0xB4FFCC, 0x8CFFE6,
    0xD8FFE0}},
  {"amber", "Amber", 1,
   0x0E0904, 0xFFB547, 0xFFB547, 0x0E0904, 0x4A2E0A, 0xFFB547, 0xFFD98A,
   0x140D06, 0xFFB547,
   {0x24170A, 0xFF6A3D, 0xFFC65C, 0xFFE08A, 0xE89B3C, 0xFF9466, 0xF5CF7A,
    0xE8C79A,
    0x8A6430, 0xFF8C66, 0xFFD68A, 0xFFF0B8, 0xF2B566, 0xFFB08F, 0xFFE0A8,
    0xFFF5E0}},
  {"fjord", "Fjord", 1,
   0x222831, 0xDCE3EC, 0x8FD3E8, 0x222831, 0x3B4757, 0x86C4D8, 0xA8C98F,
   0x1B2028, 0xDCE3EC,
   {0x2E3642, 0xE07A82, 0xA8C98F, 0xE9CC8E, 0x86AEDA, 0xC49BC8, 0x86C4D8,
    0xD6DDE6,
    0x768194, 0xE8939A, 0xBCD9A6, 0xF2DCA8, 0xA3C2E6, 0xD6B3D9, 0xA3D6E5,
    0xF5F8FB}},
  {"neon", "Neon Drive", 1,
   0x1A1030, 0xF2E9FF, 0xFF4FD8, 0x1A1030, 0x47306E, 0xFF4FD8, 0x3DF2FF,
   0x22163D, 0xF2E9FF,
   {0x2A1C48, 0xFF4F7B, 0x52F2A6, 0xFFE66B, 0x6FA6FF, 0xFF4FD8, 0x3DF2FF,
    0xD9CCF0,
    0x85769F, 0xFF85A3, 0x8AFFC6, 0xFFF099, 0x9BC2FF, 0xFF8AE6, 0x8FF7FF,
    0xFFFFFF}},
  {"moss", "Moss", 1,
   0x141A14, 0xE0E6D6, 0xA8D672, 0x141A14, 0x34442E, 0xA8D672, 0xE0B35A,
   0x1A221A, 0xE0E6D6,
   {0x222B21, 0xE8705E, 0xA8D672, 0xE0B35A, 0x7AABCF, 0xC895C0, 0x72C2A0,
    0xC8CFBC,
    0x728068, 0xF08F7E, 0xC2EB94, 0xF0CB80, 0x98C4E2, 0xD6ADD0, 0x96DBBE,
    0xF6F8F0}},
  {"graphite", "Graphite", 1,
   0x1C1C1E, 0xE8E8EA, 0xFFFFFF, 0x1C1C1E, 0x3D3D42, 0xFFFFFF, 0x4FA3FF,
   0x252528, 0xE8E8EA,
   {0x2C2C30, 0xFF6961, 0x5FD38C, 0xFFD60A, 0x4FA3FF, 0xBF5AF2, 0x5AC8FA,
    0xD1D1D6,
    0x76767C, 0xFF8A80, 0x7DE3A6, 0xFFE066, 0x7DBBFF, 0xD68CFF, 0x8ADBFF,
    0xFFFFFF}},
  {"void", "Void", 1,
   0x000000, 0xFFFFFF, 0x00E5FF, 0x000000, 0x1F3A5C, 0x00E5FF, 0xFF2E88,
   0x0A0A0A, 0xFFFFFF,
   {0x1A1A1A, 0xFF3B5C, 0x2BFF88, 0xFFE14D, 0x3D9BFF, 0xD26BFF, 0x00E5FF,
    0xCCCCCC,
    0x737373, 0xFF6E86, 0x7AFFB5, 0xFFEC85, 0x7AB8FF, 0xE39CFF, 0x6BF0FF,
    0xFFFFFF}},
  {"mocha", "Mocha", 1,
   0x211B18, 0xEADBCF, 0xF2B48A, 0x211B18, 0x4A3A32, 0xF2B48A, 0xA6D3A0,
   0x2A2320, 0xEADBCF,
   {0x342B27, 0xEE8A8A, 0xA6D3A0, 0xF0CE8E, 0x8FB2E8, 0xD6A3D8, 0x8ED4CC,
    0xD8C7BA,
    0x85736A, 0xF5A8A8, 0xC0E6BB, 0xF7DFAE, 0xAECAF2, 0xE6BFE8, 0xADE6DF,
    0xFAF3EC}},
  {"dune", "Dune", 1,
   0x221A1F, 0xF2E3D5, 0xF29E6D, 0x221A1F, 0x523B3E, 0xF29E6D, 0xE8C170,
   0x2B2127, 0xF2E3D5,
   {0x33262D, 0xE8675E, 0xB9C46A, 0xE8C170, 0x85A6D6, 0xCB8CB0, 0x74C0B3,
    0xDCCBBD,
    0x8A7077, 0xF28A80, 0xD0DA8A, 0xF4D693, 0xA1BCE3, 0xDEA9C7, 0x98D6CB,
    0xFFF5EA}},
  {"volt", "Volt", 1,
   0x0D0E12, 0xEDEDF2, 0xFCEE0A, 0x0D0E12, 0x2E3340, 0xFCEE0A, 0x00F0FF,
   0x15171D, 0xEDEDF2,
   {0x1E2028, 0xFF3860, 0x3CF57E, 0xFCEE0A, 0x5C97FF, 0xE05CFF, 0x00F0FF,
    0xCED0D8,
    0x676B7A, 0xFF6E8A, 0x7DFFAA, 0xFFF566, 0x85B0FF, 0xEC94FF, 0x70F6FF,
    0xFFFFFF}},
  {"daylight", "Daylight", 0,
   0xFFFFFF, 0x1F2430, 0x2563EB, 0xFFFFFF, 0xCFE0FF, 0x2563EB, 0x16A34A,
   0xF1F3F7, 0x1F2430,
   {0x1F2430, 0xC8283E, 0x1B7F3A, 0x8F5B00, 0x1D5FD1, 0x8A3FB5, 0x0A7480,
    0x5B6475,
    0x6B7385, 0xD0384C, 0x1D843F, 0x9B6600, 0x2B6FE0, 0x9B4CC7, 0x0B7D88,
    0x2E3545}},
  {"paper", "Paper", 0,
   0xF7F3EA, 0x2B2620, 0xB4532A, 0xF7F3EA, 0xE6DAC2, 0xB4532A, 0x3F7D5A,
   0xEFE8DA, 0x2B2620,
   {0x2B2620, 0xB23A2E, 0x3C782C, 0x8A5F00, 0x2F5F9E, 0x8B3F7A, 0x2F7470,
    0x6E665A,
    0x776D5F, 0xBA473A, 0x3E7730, 0x8E640A, 0x3B6DB0, 0x9C4D8A, 0x367873,
    0x1F1B16}},
  {"lagoon", "Lagoon", 0,
   0xFBF6E9, 0x2E3B3F, 0x0E8A82, 0xFBF6E9, 0xD8EBE6, 0x0E8A82, 0xD9822B,
   0xF2EBD9, 0x2E3B3F,
   {0x2E3B3F, 0xC2413A, 0x4B7A11, 0x946600, 0x2A6CB8, 0xA0428C, 0x0D7A73,
    0x6B7478,
    0x676F72, 0xB84738, 0x4E7A16, 0x906600, 0x336FB7, 0xA94C95, 0x127C76,
    0x1E2A2E}},
  {"lavender", "Lavender", 0,
   0xF6F3FB, 0x2A2540, 0x7C4DDB, 0xFFFFFF, 0xE1D7F7, 0x7C4DDB, 0xD9468F,
   0xEEE8F8, 0x2A2540,
   {0x2A2540, 0xC7344F, 0x2C784C, 0x8F6200, 0x3D5FC9, 0x7C4DDB, 0x1B7586,
    0x6A6480,
    0x716B88, 0xBC3C54, 0x317C50, 0x926600, 0x4767CD, 0x8054D4, 0x1F7584,
    0x1E1A30}},
  {"mint", "Mint", 0,
   0xF2FAF6, 0x15302A, 0x0F9D6B, 0xFFFFFF, 0xC9EEDD, 0x0F9D6B, 0x2F7FD8,
   0xE4F3EC, 0x15302A,
   {0x15302A, 0xC33B45, 0x0C7A4E, 0x8A6400, 0x2565B8, 0x8B3FA8, 0x087A74,
    0x5E7069,
    0x62736B, 0xC0444C, 0x107F53, 0x8E6A00, 0x2D6EC2, 0x9B4FBA, 0x0C7D76,
    0x0E241F}}
};


#define NTHEMES	((int)(sizeof(themes) / sizeof(themes[0])))


/* the i-th built-in theme, NULL past the last one */
const Theme *theme_at (int i) {
  return (i >= 0 && i < NTHEMES) ? &themes[i] : NULL;
}


/* by name or title ("default" is "dark"); unknown names get the default */
const Theme *theme_find (const char *name) {
  int i;
  for (i = 0; i < NTHEMES; i++)
    if (m_stricmp(themes[i].name, name) == 0 ||
        m_stricmp(themes[i].title, name) == 0) return &themes[i];
  if (m_stricmp(name, "gruv") == 0) return theme_find("gruvbox");
  if (m_stricmp(name, "midnight") == 0) return &themes[0];	/* same colors */
  if (m_stricmp(name, "neondrive") == 0) return theme_find("neon");
  return &themes[0];
}


/* the theme after this one, round and round */
const Theme *theme_next (const char *name) {
  const Theme *t = theme_find(name);
  return &themes[(t - themes + 1) % NTHEMES];
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
  "theme=dark                # dark, green, gruvbox, red, light, abyss, aurora,\n"
  "                          # ember, sakura, phosphor, amber, fjord, neon, moss,\n"
  "                          # graphite, void, mocha, dune, volt, daylight, paper,\n"
  "                          # lagoon, lavender, mint (Ctrl+Shift+L switches)\n"
  "#font=JetBrains Mono      # JetBrains Mono (the Nerd Font, comes with mmc: has the\n"
  "                          # Powerline and Nerd icons), Cascadia Mono, Consolas ...\n"
  "#font_file=/usr/share/fonts/MyFont.ttf   # or any .ttf file\n"
  "font_size=9               # points, like git-bash (Ctrl + and - zoom)\n"
  "font_smoothing=cleartype  # cleartype, gray or stb (Windows; others use stb)\n"
  "ligatures=yes             # -> != === drawn as one sign, when the font has them\n"
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
  c->font_size = 9;
#ifdef _WIN32
  c->smoothing = SMOOTH_CLEARTYPE;
#else
  c->smoothing = SMOOTH_STB;
#endif
  c->cols = 100;
  c->rows = 30;
  c->padding = 4;
  c->scrollback = 10000;
  c->cursor_blink = 1;
  c->ligatures = 1;
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
  else if (strcmp(key, "font_smoothing") == 0)
    c->smoothing = (m_stricmp(v, "gray") == 0 || m_stricmp(v, "grey") == 0)
                     ? SMOOTH_GRAY
                   : (m_stricmp(v, "stb") == 0) ? SMOOTH_STB : SMOOTH_CLEARTYPE;
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
  else if (strcmp(key, "ligatures") == 0) c->ligatures = parse_bool(v);
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
