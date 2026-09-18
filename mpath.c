/*
** mpath.c - Linux style paths
**
** On Windows mmc shows and accepts paths the way git-bash does:
**   /            the MMC root (the folder that holds mmc.exe)
**   /home/me     <root>\home\me
**   /d/env/zig   D:\env\zig
**   /dev/null    NUL
** Programs started by mmc are native Windows programs, so path-looking
** arguments and environment values are converted back when spawning.
** On Linux and macOS paths are already what they should be.
*/

#include "mmc.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>


static char *g_root = NULL;	/* native, without trailing separator */


void path_set_root (const char *native) {
  size_t n;
  free(g_root);
  g_root = xstrdup(native);
  n = strlen(g_root);
  while (n > 0 && path_is_sep(g_root[n - 1])) g_root[--n] = '\0';
}


const char *path_root (void) {
  return g_root ? g_root : "";
}


int path_is_sep (int c) {
#ifdef _WIN32
  return c == '/' || c == '\\';
#else
  return c == '/';
#endif
}


char *path_join (const char *a, const char *b) {
  size_t n = strlen(a);
  if (n == 0) return xstrdup(b);
  if (path_is_sep(a[n - 1])) return xstrcat3(a, b, "");
  return xstrcat3(a, MMC_SEPS, b);
}


const char *path_basename (const char *native) {
  const char *p = native + strlen(native);
  while (p > native && !path_is_sep(p[-1])) p--;
  return p;
}


/* parent directory; the parent of a top directory is itself */
char *path_dirname (const char *native) {
  size_t n = strlen(native);
  while (n > 1 && path_is_sep(native[n - 1])) n--;	/* trailing seps */
  while (n > 0 && !path_is_sep(native[n - 1])) n--;	/* last component */
  if (n == 0) return xstrdup(".");
  while (n > 1 && path_is_sep(native[n - 1])) n--;
#ifdef _WIN32
  if (n == 2 && native[1] == ':') n = 3;	/* keep "D:\" */
#endif
  return xstrndup(native, n);
}


/* removes repeated entries of a path list, keeping the first one */
static void list_push_unique (Vec *v, char *s) {
  size_t i, n = strlen(s);
  while (n > 1 && s[n - 1] == '/') s[--n] = '\0';
  for (i = 0; i < v->n; i++) {
    if (m_fncmp(v->v[i], s) == 0) {
      free(s);
      return;
    }
  }
  vec_push(v, s);
}


static char *list_join (const Vec *v, char sep) {
  Buf b;
  size_t i;
  buf_init(&b);
  for (i = 0; i < v->n; i++) {
    if (i > 0) buf_putc(&b, sep);
    buf_puts(&b, v->v[i]);
  }
  return buf_take(&b);
}


#ifdef _WIN32

/*
** {==================================================================
** Windows
** ===================================================================
*/

static char *flip (const char *p, char from, char to) {
  char *r = xstrdup(p);
  char *q;
  for (q = r; *q; q++)
    if (*q == from) *q = to;
  return r;
}


/*
** git-bash's /tmp is the Windows temp folder (%TEMP%), and the programs
** that come with it (mktemp ...) print /tmp paths: mmc's /tmp is the same
** folder, or those paths would not be found.
*/
static const char *win_temp (void) {
  static char *t = NULL;
  if (t == NULL) {
    size_t n;
    t = os_getenv("TEMP");
    if (t == NULL) t = os_getenv("TMP");
    if (t == NULL) t = xstrdup("");
    n = strlen(t);
    while (n > 3 && path_is_sep(t[n - 1])) t[--n] = '\0';
  }
  return t;
}


static int is_tmp_path (const char *p) {
  return strncmp(p, "/tmp", 4) == 0 && (p[4] == '\0' || p[4] == '/') && win_temp()[0];
}


char *path_to_native (const char *p) {
  char *r, *rest;
  if (strcmp(p, "/dev/null") == 0) return xstrdup("NUL");
  /* "//x" without a share is "/x": what "$dir/x" gives when dir is "/" */
  if (p[0] == '/' && p[1] == '/' && p[2] != '/' && p[2] != '\0' && strchr(p + 2, '/') == NULL)
    return path_to_native(p + 1);
  if (p[0] != '/' || p[1] == '/')	/* relative, "C:/x" or "//server/share" */
    return flip(p, '/', '\\');
  if (is_tmp_path(p)) {
    rest = flip(p + 4, '/', '\\');
    r = xstrcat3(win_temp(), rest, "");
    free(rest);
    return r;
  }
  if (isalpha((unsigned char)p[1]) && (p[2] == '/' || p[2] == '\0')) {
    char drive[3];
    drive[0] = (char)toupper((unsigned char)p[1]);
    drive[1] = ':';
    drive[2] = '\0';
    rest = flip(p + 2, '/', '\\');
    r = xstrcat3(drive, rest[0] ? rest : "\\", "");
    free(rest);
    return r;
  }
  rest = flip(p, '/', '\\');
  r = xstrcat3(path_root(), rest, "");
  free(rest);
  return r;
}


char *path_to_drive (const char *native) {
  char *r = flip(native, '\\', '/');
  if (isalpha((unsigned char)r[0]) && r[1] == ':') {
    r[1] = (char)tolower((unsigned char)r[0]);
    r[0] = '/';
  }
  return r;
}


char *path_to_display (const char *native) {
  const char *root = path_root();
  size_t n = strlen(root);
  char *r;
  if (n > 0 && m_strnicmp(native, root, n) == 0 &&
      (native[n] == '\0' || path_is_sep(native[n]))) {
    const char *rest = native + n;
    while (path_is_sep(rest[0]) && path_is_sep(rest[1])) rest++;
    r = flip(rest[0] ? rest : "/", '\\', '/');
    if (strcmp(r, "/dev/null") != 0) return r;
    free(r);
  }
  {	/* %TEMP% is /tmp */
    const char *t = win_temp();
    size_t tn = strlen(t);
    if (tn > 0 && m_strnicmp(native, t, tn) == 0 &&
        (native[tn] == '\0' || path_is_sep(native[tn]))) {
      char *tail = flip(native + tn, '\\', '/');
      r = xstrcat3("/tmp", tail, "");
      free(tail);
      return r;
    }
  }
  return path_to_drive(native);
}


/*
** Does this argument look like a Linux style path? Be careful: plenty
** of Windows programs take switches such as "/c" or "/all".
*/
static int looks_posix (const char *a) {
  char *name, *full;
  OsStat st;
  size_t n;
  if (a[0] != '/') return 0;
  if (a[1] == '\0') return 1;	/* "/" is the root */
  if (a[1] == '/') return 0;
  if (isalpha((unsigned char)a[1]) && a[2] == '/') return 1;	/* /d/... */
  if (isalpha((unsigned char)a[1]) && a[2] == '\0') return 0;	/* /c */
  if (strcmp(a, "/dev/null") == 0) return 1;
  if (is_tmp_path(a)) return 1;
  n = strcspn(a + 1, "/");	/* first component must exist in the root */
  name = xstrndup(a + 1, n);
  full = path_join(path_root(), name);
  os_stat(full, &st);
  free(name);
  free(full);
  return st.exists;
}


char *path_arg_to_native (const char *arg) {
  const char *eq;
  if (looks_posix(arg)) return path_to_native(arg);
  if (arg[0] == '/' && arg[1] == '/' && strchr(arg + 2, '/') == NULL)
    return xstrdup(arg + 1);	/* "//c" escapes to "/c", like git-bash */
  if (arg[0] == '-' && (eq = strchr(arg, '=')) != NULL && looks_posix(eq + 1)) {
    char *head = xstrndup(arg, (size_t)(eq + 1 - arg));
    char *tail = path_to_native(eq + 1);
    char *r = xstrcat3(head, tail, "");
    free(head);
    free(tail);
    return r;
  }
  return xstrdup(arg);
}


/*
** Splits "a:b;c". A colon is a drive colon (not a separator) when it
** follows a single letter and is followed by a slash.
*/
void path_list_split (const char *val, Vec *out) {
  const char *start = val, *p;
  for (p = val;; p++) {
    int sep = (*p == ';' || *p == '\0');
    if (*p == ':') {
      int drive = (p - start == 1) && isalpha((unsigned char)start[0]) &&
                  path_is_sep(p[1]);
      sep = !drive;
    }
    if (!sep) continue;
    if (p > start) vec_push(out, xstrndup(start, (size_t)(p - start)));
    if (*p == '\0') break;
    start = p + 1;
  }
}


char *path_env_to_native (const char *val) {
  Vec parts, conv;
  size_t i;
  int changed = 0;
  char *r;
  if (val[0] != '/') return xstrdup(val);
  vec_init(&parts);
  vec_init(&conv);
  path_list_split(val, &parts);
  for (i = 0; i < parts.n; i++) {
    const char *e = parts.v[i];
    if (looks_posix(e)) {
      vec_push(&conv, path_to_native(e));
      changed = 1;
    }
    else vec_push(&conv, xstrdup(e));
  }
  r = changed ? list_join(&conv, ';') : xstrdup(val);
  vec_free(&parts);
  vec_free(&conv);
  return r;
}


char *path_list_normalize (const char *val) {
  Vec parts, uniq;
  size_t i;
  char *r;
  vec_init(&parts);
  vec_init(&uniq);
  path_list_split(val, &parts);
  for (i = 0; i < parts.n; i++) {
    const char *e = parts.v[i];
    list_push_unique(&uniq, (e[0] == '/') ? xstrdup(e) : path_to_display(e));
  }
  r = list_join(&uniq, ':');
  vec_free(&parts);
  vec_free(&uniq);
  return r;
}

/* }================================================================== */

#else

/*
** {==================================================================
** Linux, macOS: nothing to translate
** ===================================================================
*/

char *path_to_native (const char *p) { return xstrdup(p); }
char *path_to_display (const char *native) { return xstrdup(native); }
char *path_to_drive (const char *native) { return xstrdup(native); }
char *path_arg_to_native (const char *arg) { return xstrdup(arg); }
char *path_env_to_native (const char *val) { return xstrdup(val); }


void path_list_split (const char *val, Vec *out) {
  const char *start = val, *p;
  for (p = val;; p++) {
    if (*p != ':' && *p != '\0') continue;
    if (p > start) vec_push(out, xstrndup(start, (size_t)(p - start)));
    if (*p == '\0') break;
    start = p + 1;
  }
}


char *path_list_normalize (const char *val) {
  Vec parts, uniq;
  size_t i;
  char *r;
  vec_init(&parts);
  vec_init(&uniq);
  path_list_split(val, &parts);
  for (i = 0; i < parts.n; i++)
    list_push_unique(&uniq, xstrdup(parts.v[i]));
  r = list_join(&uniq, ':');
  vec_free(&parts);
  vec_free(&uniq);
  return r;
}

/* }================================================================== */

#endif
