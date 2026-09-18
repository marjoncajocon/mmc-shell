/*
** tfont.c - fonts of mmc-term
**
** Finds a monospace font file on the system, rasterizes glyphs with
** stb_truetype and caches them. Missing glyphs come from fallback
** fonts; bold and italic are synthesized when the font has no file
** for them. The same code runs on every system, so text looks the
** same everywhere.
*/

#include "mterm.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif


typedef struct Face {
  unsigned char *data;	/* the font file; shared by faces of one .ttc */
  stbtt_fontinfo info;
  float scale;
  int ok;
} Face;

typedef struct Known {
  const char *name, *regular, *bold, *italic;
  int ttc_bold, ttc_italic;	/* face index inside a .ttc, 0 = none */
} Known;

static const Known known[] = {
  {"Cascadia Mono", "CascadiaMono.ttf", NULL, NULL, 0, 0},
  {"Cascadia Code", "CascadiaCode.ttf", NULL, NULL, 0, 0},
  {"Consolas", "consola.ttf", "consolab.ttf", "consolai.ttf", 0, 0},
  {"JetBrains Mono", "JetBrainsMono-Regular.ttf", "JetBrainsMono-Bold.ttf",
   "JetBrainsMono-Italic.ttf", 0, 0},
  {"DejaVu Sans Mono", "DejaVuSansMono.ttf", "DejaVuSansMono-Bold.ttf",
   "DejaVuSansMono-Oblique.ttf", 0, 0},
  {"Liberation Mono", "LiberationMono-Regular.ttf", "LiberationMono-Bold.ttf",
   "LiberationMono-Italic.ttf", 0, 0},
  {"Noto Sans Mono", "NotoSansMono-Regular.ttf", "NotoSansMono-Bold.ttf",
   NULL, 0, 0},
  {"Ubuntu Mono", "UbuntuMono-R.ttf", "UbuntuMono-B.ttf", "UbuntuMono-RI.ttf",
   0, 0},
  {"Menlo", "Menlo.ttc", NULL, NULL, 1, 2},
  {"SF Mono", "SFNSMono.ttf", NULL, "SFNSMonoItalic.ttf", 0, 0},
  {"Monaco", "Monaco.ttf", NULL, NULL, 0, 0},
  {"Courier New", "cour.ttf", "courbd.ttf", "couri.ttf", 0, 0},
  {"Lucida Console", "lucon.ttf", NULL, NULL, 0, 0},
  {NULL, NULL, NULL, NULL, 0, 0}
};

/* looked at in this order when the config names no font */
static const char *const preferred[] = {
#if defined(_WIN32)
  "Cascadia Mono", "Cascadia Code", "Consolas", "Lucida Console",
#elif defined(__APPLE__)
  "Menlo", "SF Mono", "Monaco",
#else
  "DejaVu Sans Mono", "Liberation Mono", "Noto Sans Mono", "Ubuntu Mono",
#endif
  "JetBrains Mono", "Courier New", NULL
};

/* fonts asked for glyphs the main font does not have */
static const char *const fallback_files[] = {
#if defined(_WIN32)
  "seguisym.ttf", "segoeui.ttf", "msgothic.ttc", "malgun.ttf", "msyh.ttc",
  "seguiemj.ttf",
#elif defined(__APPLE__)
  "Apple Symbols.ttf", "PingFang.ttc", "Hiragino Sans GB.ttc",
  "Arial Unicode.ttf",
#else
  "DejaVuSans.ttf", "NotoSansSymbols2-Regular.ttf", "NotoSansCJK-Regular.ttc",
  "unifont.ttf",
#endif
  NULL
};

#define MAX_FALLBACK	8

static Vec font_files;	/* every font file found on the system */
static int files_listed = 0;
static char *extra_dir = NULL;
static Face f_regular, f_bold, f_italic;
static Face f_fallback[MAX_FALLBACK];
static int fallback_state[MAX_FALLBACK];	/* 0 not tried, 1 loaded, -1 none */
static char name_buf[160];
static float cur_px = 15.0f;
static int cell_w = 8, cell_h = 16, ascent = 12;


/*
** {==================================================================
** Finding font files
** ===================================================================
*/

static int has_font_ext (const char *name) {
  size_t n = strlen(name);
  return n > 4 && (m_stricmp(name + n - 4, ".ttf") == 0 ||
                   m_stricmp(name + n - 4, ".ttc") == 0 ||
                   m_stricmp(name + n - 4, ".otf") == 0);
}


static void list_dir (const char *dir, int depth) {
  Vec names;
  size_t i;
  vec_init(&names);
  if (os_listdir(dir, &names) == 0) {
    for (i = 0; i < names.n; i++) {
      char *full = path_join(dir, names.v[i]);
      OsStat st;
      if (has_font_ext(names.v[i])) vec_push(&font_files, full);
      else {
        if (depth < 4 && os_stat(full, &st) == 0 && st.is_dir)
          list_dir(full, depth + 1);
        free(full);
      }
    }
  }
  vec_free(&names);
}


static void list_env_dir (const char *var, const char *sub) {
  char *base = os_getenv(var);
  if (base != NULL) {
    char *dir = path_join(base, sub);
    list_dir(dir, 0);
    free(dir);
    free(base);
  }
}


static void list_fonts (void) {
  if (files_listed) return;
  files_listed = 1;
  vec_init(&font_files);
  if (extra_dir) list_dir(extra_dir, 0);	/* fonts carried with mmc win */
#if defined(_WIN32)
  list_env_dir("LOCALAPPDATA", "Microsoft\\Windows\\Fonts");
  list_env_dir("WINDIR", "Fonts");
#elif defined(__APPLE__)
  list_env_dir("HOME", "Library/Fonts");
  list_dir("/Library/Fonts", 0);
  list_dir("/System/Library/Fonts", 0);
#else
  list_env_dir("HOME", ".local/share/fonts");
  list_env_dir("HOME", ".fonts");
  list_dir("/usr/local/share/fonts", 0);
  list_dir("/usr/share/fonts", 0);
#endif
}


static const char *find_file (const char *basename) {
  size_t i;
  if (basename == NULL) return NULL;
  list_fonts();
  for (i = 0; i < font_files.n; i++)
    if (m_stricmp(path_basename(font_files.v[i]), basename) == 0)
      return font_files.v[i];
  return NULL;
}


void font_add_dir (const char *native) {
  free(extra_dir);
  extra_dir = xstrdup(native);
}

/* }================================================================== */


static int face_open (Face *f, unsigned char *data, int index) {
  int off = stbtt_GetFontOffsetForIndex(data, index);
  memset(f, 0, sizeof(*f));
  if (off < 0 || !stbtt_InitFont(&f->info, data, off)) return 0;
  f->data = data;
  f->ok = 1;
  return 1;
}


static int face_load (Face *f, const char *file, int index) {
  size_t len = 0;
  unsigned char *data;
  memset(f, 0, sizeof(*f));
  if (file == NULL) return 0;
  data = (unsigned char *)read_file(file, &len);
  if (data == NULL || len < 64) {
    free(data);
    return 0;
  }
  if (!face_open(f, data, index)) {
    free(data);
    return 0;
  }
  return 1;
}


static int load_known (const Known *k) {
  const char *file = find_file(k->regular);
  if (!face_load(&f_regular, file, 0)) return 0;
  if (k->ttc_bold) face_open(&f_bold, f_regular.data, k->ttc_bold);
  else face_load(&f_bold, find_file(k->bold), 0);
  if (k->ttc_italic) face_open(&f_italic, f_regular.data, k->ttc_italic);
  else face_load(&f_italic, find_file(k->italic), 0);
  strncpy(name_buf, k->name, sizeof(name_buf) - 1);
  return 1;
}


static const Known *known_by_name (const char *name) {
  const Known *k;
  for (k = known; k->name; k++)
    if (m_stricmp(k->name, name) == 0) return k;
  return NULL;
}


int font_init (const Config *c) {
  const Known *k;
  size_t i;
  name_buf[0] = '\0';
  if (c->font_file[0] != '\0') {	/* an explicit file wins */
    char *native = path_to_native(c->font_file);
    int ok = face_load(&f_regular, native, 0);
    free(native);
    if (ok) strncpy(name_buf, path_basename(c->font_file), sizeof(name_buf) - 1);
  }
  if (!f_regular.ok && c->font[0] != '\0') {
    if ((k = known_by_name(c->font)) != NULL) load_known(k);
    else {	/* maybe it is a file name, with or without ".ttf" */
      char *guess = xstrcat3(c->font, ".ttf", "");
      const char *file = find_file(c->font);
      if (file == NULL) file = find_file(guess);
      if (face_load(&f_regular, file, 0))
        strncpy(name_buf, c->font, sizeof(name_buf) - 1);
      free(guess);
    }
  }
  for (i = 0; !f_regular.ok && preferred[i] != NULL; i++)
    load_known(known_by_name(preferred[i]));
  if (!f_regular.ok) {	/* last resort: anything that says "mono" */
    list_fonts();
    for (i = 0; i < font_files.n && !f_regular.ok; i++) {
      const char *base = path_basename(font_files.v[i]);
      char low[64];
      size_t j;
      for (j = 0; j < sizeof(low) - 1 && base[j]; j++)
        low[j] = (char)((base[j] >= 'A' && base[j] <= 'Z') ? base[j] + 32 : base[j]);
      low[j] = '\0';
      if (strstr(low, "mono") != NULL && strstr(low, "bold") == NULL &&
          strstr(low, "italic") == NULL && strstr(low, "oblique") == NULL &&
          face_load(&f_regular, font_files.v[i], 0))
        strncpy(name_buf, base, sizeof(name_buf) - 1);
    }
  }
  if (!f_regular.ok) return -1;
  font_set_px(cur_px);
  return 0;
}


const char *font_name (void) { return name_buf; }
int font_cell_w (void) { return cell_w; }
int font_cell_h (void) { return cell_h; }
int font_ascent (void) { return ascent; }


/*
** {==================================================================
** Glyph cache (open addressing, keyed by code point + style)
** ===================================================================
*/

typedef struct Slot {
  uint32_t key;	/* 0 = empty */
  Glyph g;
} Slot;

static Slot *slots = NULL;
static size_t nslots = 0, nused = 0;


static void cache_clear (void) {
  size_t i;
  for (i = 0; i < nslots; i++) free(slots[i].g.bm);
  free(slots);
  slots = NULL;
  nslots = nused = 0;
}


static Slot *cache_find (uint32_t key) {
  size_t i;
  if (nslots == 0) return NULL;
  for (i = (key * 2654435761u) & (nslots - 1); ; i = (i + 1) & (nslots - 1))
    if (slots[i].key == key || slots[i].key == 0) return &slots[i];
}


static Slot *cache_insert (uint32_t key) {
  Slot *s;
  if ((nused + 1) * 10 >= nslots * 7) {	/* grow at 70% */
    Slot *old = slots;
    size_t oldn = nslots, i;
    nslots = nslots ? nslots * 2 : 512;
    slots = (Slot *)xmalloc(nslots * sizeof(Slot));
    memset(slots, 0, nslots * sizeof(Slot));
    for (i = 0; i < oldn; i++)
      if (old[i].key != 0) *cache_find(old[i].key) = old[i];
    free(old);
  }
  s = cache_find(key);
  s->key = key;
  nused++;
  return s;
}

/* }================================================================== */


void font_set_px (float px) {
  int asc, desc, gap, adv, lsb, i;
  float s, height;
  if (px < 6.0f) px = 6.0f;
  cur_px = px;
  cache_clear();
  if (!f_regular.ok) return;
  s = stbtt_ScaleForMappingEmToPixels(&f_regular.info, px);
  f_regular.scale = s;
  if (f_bold.ok) f_bold.scale = stbtt_ScaleForMappingEmToPixels(&f_bold.info, px);
  if (f_italic.ok)
    f_italic.scale = stbtt_ScaleForMappingEmToPixels(&f_italic.info, px);
  for (i = 0; i < MAX_FALLBACK; i++)
    if (f_fallback[i].ok)
      f_fallback[i].scale = stbtt_ScaleForMappingEmToPixels(&f_fallback[i].info, px);
  stbtt_GetFontVMetrics(&f_regular.info, &asc, &desc, &gap);
  stbtt_GetCodepointHMetrics(&f_regular.info, 'M', &adv, &lsb);
  height = (float)(asc - desc + gap) * s;
  cell_h = (int)ceil(height * 1.08f);	/* a little air between lines */
  cell_w = (int)floor((float)adv * s + 0.5f);
  if (cell_w < 1) cell_w = 1;
  ascent = (int)floor((float)asc * s + ((float)cell_h - height) * 0.5f + 0.5f);
}


static Face *fallback_for (uint32_t cp, int *glyph) {
  int i;
  for (i = 0; i < MAX_FALLBACK && fallback_files[i] != NULL; i++) {
    Face *f = &f_fallback[i];
    if (fallback_state[i] == 0) {	/* loaded the first time they are needed */
      fallback_state[i] = face_load(f, find_file(fallback_files[i]), 0) ? 1 : -1;
      if (f->ok) f->scale = stbtt_ScaleForMappingEmToPixels(&f->info, cur_px);
    }
    if (f->ok && (*glyph = stbtt_FindGlyphIndex(&f->info, (int)cp)) != 0)
      return f;
  }
  return NULL;
}


/* one pixel fatter to the right */
static void embolden (Glyph *g) {
  int w = g->w + 1, x, y;
  unsigned char *out = (unsigned char *)xmalloc((size_t)w * (size_t)g->h);
  for (y = 0; y < g->h; y++) {
    const unsigned char *in = g->bm + (size_t)y * (size_t)g->w;
    for (x = 0; x < w; x++) {
      int a = (x < g->w) ? in[x] : 0;
      int b = (x > 0) ? in[x - 1] : 0;
      out[(size_t)y * (size_t)w + (size_t)x] = (unsigned char)(a > b ? a : b);
    }
  }
  free(g->bm);
  g->bm = out;
  g->w = w;
}


/* leans the glyph to the right around the baseline */
static void slant (Glyph *g) {
  const float k = 0.2f;
  float lo = k * (float)(-(g->yoff + g->h - 1));	/* bottom row */
  float hi = k * (float)(-g->yoff);	/* top row */
  int base = (int)floor(lo);
  int w = g->w + (int)ceil(hi - (float)base) + 1, x, y;
  unsigned char *out = (unsigned char *)xmalloc((size_t)w * (size_t)g->h);
  memset(out, 0, (size_t)w * (size_t)g->h);
  for (y = 0; y < g->h; y++) {
    float shift = k * (float)(-(g->yoff + y)) - (float)base;
    int is = (int)shift;
    float f = shift - (float)is;
    const unsigned char *in = g->bm + (size_t)y * (size_t)g->w;
    unsigned char *row = out + (size_t)y * (size_t)w;
    for (x = 0; x < g->w; x++) {
      int a = row[x + is] + (int)((float)in[x] * (1.0f - f));
      int b = row[x + is + 1] + (int)((float)in[x] * f);
      row[x + is] = (unsigned char)(a > 255 ? 255 : a);
      row[x + is + 1] = (unsigned char)(b > 255 ? 255 : b);
    }
  }
  free(g->bm);
  g->bm = out;
  g->w = w;
  g->xoff += base;
}


const Glyph *font_glyph (uint32_t cp, int bold, int italic) {
  uint32_t key = (cp & 0x1FFFFF) | (bold ? 1u << 24 : 0) |
                 (italic ? 1u << 25 : 0) | (1u << 31);
  Slot *s = cache_find(key);
  Face *f = &f_regular;
  int glyph, fake_bold = bold, fake_italic = italic;
  int x0, y0, x1, y1, adv, lsb, span;
  Glyph *g;
  if (s != NULL && s->key == key) return &s->g;
  s = cache_insert(key);
  g = &s->g;
  memset(g, 0, sizeof(*g));
  if (!f_regular.ok) return g;
  if (bold && f_bold.ok && stbtt_FindGlyphIndex(&f_bold.info, (int)cp) != 0) {
    f = &f_bold;
    fake_bold = 0;
  }
  else if (italic && f_italic.ok &&
           stbtt_FindGlyphIndex(&f_italic.info, (int)cp) != 0) {
    f = &f_italic;
    fake_italic = 0;
  }
  glyph = stbtt_FindGlyphIndex(&f->info, (int)cp);
  if (glyph == 0) {
    Face *fb = fallback_for(cp, &glyph);
    if (fb != NULL) f = fb;
    else {	/* nobody has it: show the replacement character */
      f = &f_regular;
      glyph = stbtt_FindGlyphIndex(&f->info, 0xFFFD);
      if (glyph == 0) glyph = stbtt_FindGlyphIndex(&f->info, '?');
    }
  }
  stbtt_GetGlyphBitmapBox(&f->info, glyph, f->scale, f->scale, &x0, &y0, &x1, &y1);
  g->w = x1 - x0;
  g->h = y1 - y0;
  g->xoff = x0;
  g->yoff = y0;
  if (g->w <= 0 || g->h <= 0) {
    g->w = g->h = 0;
    return g;
  }
  g->bm = (unsigned char *)xmalloc((size_t)g->w * (size_t)g->h);
  stbtt_MakeGlyphBitmap(&f->info, g->bm, g->w, g->h, g->w, f->scale, f->scale,
                        glyph);
  if (f != &f_regular && f != &f_bold && f != &f_italic) {
    /* a fallback font is not monospace: center it in its cell(s) */
    stbtt_GetGlyphHMetrics(&f->info, glyph, &adv, &lsb);
    span = cell_w * (grid_wcwidth(cp) == 2 ? 2 : 1);
    g->xoff += (span - (int)((float)adv * f->scale)) / 2;
  }
  if (fake_bold) embolden(g);
  if (fake_italic) slant(g);
  return g;
}
