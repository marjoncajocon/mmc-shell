/*
** mmc.c - MMC shell (Marjon Mangindo Cajocon)
**
** A small portable shell in the spirit of git-bash: the folder that
** holds the executable becomes "/", with home/, usr/, etc/ and tmp/
** inside, so a whole development environment can live on any drive.
*/

#include "mmc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static const char *const default_profile =
  "# /etc/profile - settings for everyone who uses this MMC folder.\n"
  "# Read once when mmc starts. The syntax is bash's:\n"
  "#   export NAME=value    alias name='value'    source file    # comment\n"
  "#   if/for/case, functions, $(command) ... all work here too\n"
  "#\n"
  "# Paths are Linux style (on Windows too):\n"
  "#   /             the MMC folder (where the mmc program is)\n"
  "#   /d/env/zig    D:\\env\\zig - drive letters work like in git-bash\n"
  "#   ~             your home: /home/<user>\n"
  "#   $MMC_DRIVE    the drive MMC is on, e.g. /d; use it so this file\n"
  "#                 keeps working when the drive letter changes\n"
  "#   $MMC_ROOT     the MMC folder as a full path\n"
  "#\n"
  "# Examples:\n"
  "# export PATH=\"$MMC_DRIVE/env/zig:$PATH\"\n"
  "# export PATH=\"$MMC_DRIVE/env/node/node-v22:$PATH\"\n"
  "# export EDITOR=nvim\n";

static const char *const default_rc =
  "# ~/.mmcrc - your own settings, read by every interactive mmc.\n"
  "alias ll='ls -l'\n"
  "alias la='ls -a'\n"
  "alias ..='cd ..'\n";


static char *g_exe;	/* native path of this program */
static char *g_home;	/* native path of the home directory */


const char *mmc_home (void) { return g_home; }
const char *mmc_exe (void) { return g_exe; }


static void write_default (const char *native, const char *text) {
  OsStat st;
  int fd;
  if (os_stat(native, &st) == 0) return;
  if ((fd = os_open(native, OS_WRITE)) < 0) return;
  fd_puts(fd, text);
  os_close(fd);
}


/* the root is where the program is, unless that is <root>[/usr]/bin */
static void setup_root (const char *argv0, const char *forced) {
  char *dir;
  g_exe = os_exe_path(argv0);
  if (forced != NULL) {
    path_set_root(forced);
    return;
  }
  dir = path_dirname(g_exe);
  if (m_fncmp(path_basename(dir), "bin") == 0) {
    char *up = path_dirname(dir);
    free(dir);
    dir = up;
    if (m_fncmp(path_basename(dir), "usr") == 0) {
      up = path_dirname(dir);
      free(dir);
      dir = up;
    }
  }
  path_set_root(dir);
  free(dir);
}


/* where temporary files go: $TMPDIR, /tmp (not Windows), <root>/tmp */
char *path_tmpdir (void) {
  const char *t = var_get("TMPDIR");
  OsStat st;
  if (t != NULL && *t) {
    char *native = path_to_native(t);
    if (os_stat(native, &st) == 0 && st.is_dir) return native;
    free(native);
  }
  {	/* /tmp: the system's (on Windows %TEMP%, like git-bash) */
    char *native = path_to_native("/tmp");
    if (os_stat(native, &st) == 0 && st.is_dir && os_access(native, 'w')) return native;
    free(native);
  }
  {
    char *tmp = path_join(path_root(), "tmp");
    mkdir_p(tmp);
    return tmp;
  }
}


/*
** The user of this mmc folder. mmc is carried from PC to PC, and the
** login name is different on each of them; the home folder must not
** be. So the name is kept in /etc/user: written on the first start
** (from the home folder that is already there, or else from the login
** name) and used everywhere after that. Edit the file to change it.
*/
static char *portable_user (void) {
  char *etc = path_join(path_root(), "etc");
  char *file = path_join(etc, "user");
  char *home = path_join(path_root(), "home");
  char *text = read_file(file, NULL);
  char *name = NULL;
  int fd;
  if (text != NULL) {	/* first line that is not a comment */
    char *line = text;
    while (*line != '\0' && name == NULL) {
      char *end = line + strcspn(line, "\r\n");
      char *next = (*end == '\0') ? end : end + 1;
      *end = '\0';
      while (*line == ' ' || *line == '\t') line++;
      while (end > line && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
      if (line[0] != '\0' && line[0] != '#' && strpbrk(line, "/\\:*?\"<>|") == NULL)
        name = xstrdup(line);
      line = next;
    }
    free(text);
  }
  if (name == NULL) {
    Vec dirs;
    size_t i, homes = 0;
    vec_init(&dirs);
    os_listdir(home, &dirs);
    for (i = 0; i < dirs.n; i++) {	/* exactly one home: that is the user */
      char *full = path_join(home, dirs.v[i]);
      OsStat st;
      if (os_stat(full, &st) == 0 && st.is_dir) {
        homes++;
        free(name);
        name = xstrdup(dirs.v[i]);
      }
      free(full);
    }
    if (homes != 1) {
      free(name);
      name = os_username();
    }
    vec_free(&dirs);
    mkdir_p(etc);
    if ((fd = os_open(file, OS_WRITE)) >= 0) {
      fd_printf(fd, "# the user of this mmc folder: home is /home/<this name> on every PC\n"
                    "%s\n", name);
      os_close(fd);
    }
  }
  free(etc);
  free(file);
  free(home);
  return name;
}


static void export_var (const char *name, const char *value) {
  var_set(name, value);
  var_set_flags(name, V_EXPORT, 0);
}


static void setup_env (void) {
  Buf path;
  char num[24];
  char *user = portable_user();
  char *host = os_hostname();
  const char *old = var_get("PATH");
  const char *lvl = var_get("MMC_LEVEL");
  char *oldcopy = old ? xstrdup(old) : NULL;
  char *exedir = path_dirname(g_exe);
  char *rootp, *home, *shell, *tmp;
  int level = lvl ? atoi(lvl) : 0;
#ifdef _WIN32
  const char *term = var_get("TERM");
  char *drive = path_to_drive(path_root());
  rootp = xstrdup("");	/* the root is simply "/" */
  export_var("MMC_ROOT", drive);
  if (drive[0] == '/' && drive[1] != '\0') {
    drive[2] = '\0';
    export_var("MMC_DRIVE", drive);
  }
  if (term == NULL) export_var("TERM", "xterm-256color");
  free(drive);
#else
  rootp = xstrdup(path_root());
  export_var("MMC_ROOT", rootp);
#endif
  home = xstrcat3(rootp, "/home/", user);
  g_home = path_to_native(home);
  tmp = path_to_display(exedir);
  shell = path_to_display(g_exe);
  buf_init(&path);
  if (lvl != NULL) {	/* nested mmc: PATH is set up already, keep its order */
    buf_puts(&path, oldcopy ? oldcopy : "");
    buf_puts(&path, ":");
  }
  buf_puts(&path, rootp);
  buf_puts(&path, "/usr/bin:");
  buf_puts(&path, home);
  buf_puts(&path, "/bin:");
  buf_puts(&path, tmp);	/* before the system: Windows has its own mmc.exe */
  buf_puts(&path, ":");
  buf_puts(&path, oldcopy ? oldcopy : "");
  export_var("PATH", path.s);
  export_var("HOME", home);
  export_var("USER", user);
  export_var("HOSTNAME", host);
  export_var("SHELL", shell);
  export_var("MMC_LEVEL", ll_to_str(level + 1, num));
  {
    const char *sl = var_get("SHLVL");
    export_var("SHLVL", ll_to_str((sl ? atoi(sl) : 0) + 1, num));
  }
  buf_free(&path);
  free(user); free(host); free(oldcopy); free(exedir);
  free(rootp); free(home); free(shell); free(tmp);
}


/* the variables bash sets for itself */
static void setup_shell_vars (void) {
  char num[24], *cwd, *shown;
  Buf mt;
  var_set("BASH_VERSION", MMC_BASH_COMPAT);
  var_make_array("BASH_VERSINFO", 0);
  var_aset("BASH_VERSINFO", 0, "5");
  var_aset("BASH_VERSINFO", 1, "2");
  var_aset("BASH_VERSINFO", 2, "0");
  var_aset("BASH_VERSINFO", 3, "1");
  var_aset("BASH_VERSINFO", 4, "release");
  var_set("MMC_VERSION", MMC_VERSION);
  {
    char *shown_exe = path_to_display(g_exe);
    var_set("BASH", shown_exe);
    free(shown_exe);
  }
  var_set("OSTYPE", os_type());
  var_set("HOSTTYPE", os_machine());
  buf_init(&mt);
  buf_puts(&mt, os_machine());
  buf_puts(&mt, strcmp(os_type(), "darwin") == 0 ? "-apple-" : "-pc-");
  buf_puts(&mt, os_type());
  var_set("MACHTYPE", mt.s);
  buf_free(&mt);
  var_set("PPID", ll_to_str(os_getppid(), num));
  var_set("UID", ll_to_str(os_getuid(), num));
  var_set("EUID", ll_to_str(os_geteuid(), num));
  var_set_flags("PPID", V_READONLY, 0);
  var_set_flags("UID", V_READONLY, 0);
  var_set_flags("EUID", V_READONLY, 0);
  if (var_get("IFS") == NULL) var_set("IFS", " \t\n");
  var_set("OPTIND", "1");
  var_set("BASH_SUBSHELL", "0");
  if (var_get("PS2") == NULL) var_set("PS2", "> ");
  if (var_get("PS4") == NULL) var_set("PS4", "+ ");
  if (var_get("PS1") == NULL) var_set("PS1", "\\s-\\v\\$ ");
  if (var_get("HISTSIZE") == NULL) var_set("HISTSIZE", "1000");
  cwd = os_getcwd();
  shown = path_to_display(cwd);
  export_var("PWD", shown);
  free(cwd);
  free(shown);
  var_unset("OLDPWD");
  var_set("MMC_ROOT_NATIVE", path_root());
}


/* first run: create home/<user>, usr/bin, etc/profile, tmp */
static void setup_tree (void) {
  const char *root = path_root();
  char *etc = path_join(root, "etc");
  char *usr = path_join(root, "usr");
  char *usrbin = path_join(usr, "bin");
  char *tmp = path_join(root, "tmp");
  char *profile = path_join(etc, "profile");
  char *rc = path_join(g_home, ".mmcrc");
  mkdir_p(g_home);
  mkdir_p(etc);
  mkdir_p(usrbin);
  mkdir_p(tmp);
  write_default(profile, default_profile);
  write_default(rc, default_rc);
  free(etc); free(usr); free(usrbin); free(tmp); free(profile); free(rc);
}


static void source_config (int top_level, int norc, int noprofile) {
  char *etc = path_join(path_root(), "etc");
  char *profile = path_join(etc, "profile");
  char *rc = path_join(g_home, ".mmcrc");
  if (top_level && !noprofile) sh_source(profile, 0, NULL, 0);	/* nested shells inherit it */
  if ((top_level || sh_interactive) && sh_interactive && !norc) sh_source(rc, 0, NULL, 0);
  else if (top_level && !norc && !noprofile) sh_source(rc, 0, NULL, 0);
  sh_status = 0;
  sh_exit = 0;
  free(etc); free(profile); free(rc);
}


/*
** {==================================================================
** Prompt:  user@host MMC ~/project (branch)
** ===================================================================
*/

/* current git branch, read straight from .git/HEAD */
static char *git_branch (const char *cwd) {
  char *dir = xstrdup(cwd);
  char *head = NULL;
  for (;;) {
    char *dotgit = path_join(dir, ".git");
    char *parent;
    OsStat st;
    if (os_stat(dotgit, &st) == 0) {
      if (st.is_dir) {
        char *f = path_join(dotgit, "HEAD");
        head = read_file(f, NULL);
        free(f);
      }
      else {	/* worktree or submodule: "gitdir: <path>" */
        char *text = read_file(dotgit, NULL);
        if (text != NULL && strncmp(text, "gitdir: ", 8) == 0) {
          char *f, *gitdir;
          text[strcspn(text, "\r\n")] = '\0';
          gitdir = (text[8] == '/' || strchr(text + 8, ':') != NULL)
                     ? xstrdup(text + 8) : path_join(dir, text + 8);
          f = path_join(gitdir, "HEAD");
          head = read_file(f, NULL);
          free(f);
          free(gitdir);
        }
        free(text);
      }
      free(dotgit);
      break;
    }
    free(dotgit);
    parent = path_dirname(dir);
    if (strcmp(parent, dir) == 0 || strcmp(parent, ".") == 0) {
      free(parent);
      break;
    }
    free(dir);
    dir = parent;
  }
  free(dir);
  if (head != NULL) {
    char *r;
    head[strcspn(head, "\r\n")] = '\0';
    if (strncmp(head, "ref: refs/heads/", 16) == 0) r = xstrdup(head + 16);
    else {
      head[strlen(head) > 7 ? 7 : strlen(head)] = '\0';
      r = xstrcat3(head, "...", "");
    }
    free(head);
    return r;
  }
  return NULL;
}


#define LINE	"\033[0;94m"	/* the connector lines: the blue of the logo */

/* MMC_PROMPT: parrot (default), classic, powerline, or ps1 (your $PS1) */
static const char *prompt_style (void) {
  const char *style = var_get("MMC_PROMPT");
  return style ? style : "parrot";
}


static char *prompt_last_line (void) {
  const char *st = prompt_style();
  if (strcmp(st, "ps1") == 0) {
    const char *ps1 = var_get("PS1");
    return expand_prompt(ps1 ? ps1 : "$ ");
  }
  if (strcmp(st, "classic") == 0) return xstrdup("\033[32m$\033[0m ");
  if (strcmp(st, "powerline") == 0)	/* a small arrow on the second line */
    return xstrdup("\033[0;32m\xef\x84\xa0 \033[0;94m\xee\x82\xb1\033[0m ");
  return xstrdup(LINE "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80\xe2\x95\xbc \033[0;33m$\033[0m ");
}


/* one segment of the powerline prompt: text on a color, arrow into the next */
static void pl_segment (Buf *b, const char *fg, int bg, int next_bg, const char *text) {
  buf_printf(b, "\033[%s;%dm %s ", fg, 40 + bg, text);
  if (next_bg >= 0) buf_printf(b, "\033[%d;%dm\xee\x82\xb0", 30 + bg, 40 + next_bg);
  else buf_printf(b, "\033[0;%dm\xee\x82\xb0\033[0m", 30 + bg);
}


static void show_prompt_header (void) {
  char *cwd = os_getcwd();
  char *shown = path_to_display(cwd);
  const char *home = var_get("HOME");
  const char *user = var_get("USER");
  const char *host = var_get("HOSTNAME");
  const char *st = prompt_style();
  char *branch = git_branch(cwd);
  size_t hn = home ? strlen(home) : 0;
  Buf b;
  buf_init(&b);
  if (hn > 1 && m_fnncmp(shown, home, hn) == 0 &&
      (shown[hn] == '\0' || shown[hn] == '/')) {
    buf_putc(&b, '~');
    buf_puts(&b, shown + hn);
  }
  else buf_puts(&b, shown);
  fd_printf(1, "\033]0;MMC:%s\007", b.s);
  /* OSC 7: the window learns where we are, so a new one opens here too */
  fd_printf(1, "\033]7;file://%s%s\033\\\n", host ? host : "", shown);
  if (strcmp(st, "ps1") == 0) {
    /* all of it is in PS1 */
  }
  else if (strcmp(st, "classic") == 0) {	/* one line, like git-bash */
    fd_printf(1, "\033[32m%s@%s \033[0;7;34m MMC \033[0m %s",
              user ? user : "", host ? host : "", b.s);
    if (branch) fd_printf(1, "\033[0;36m (%s)", branch);
    fd_puts(1, "\033[0m\n");
  }
  else if (strcmp(st, "powerline") == 0) {	/* segments and arrows */
    Buf p;
    char who[256];
    buf_init(&p);
    if (sh_status != 0) {
      char num[48];
      sprintf(num, "\xe2\x9c\x98 %d", sh_status);
      pl_segment(&p, "1;97", 1, 2, num);
    }
    sprintf(who, "%.100s@%.100s", user ? user : "", host ? host : "");
    pl_segment(&p, "30", 2, 4, who);
    if (branch) {
      char br[300];
      pl_segment(&p, "97", 4, 5, b.s);
      sprintf(br, "\xee\x82\xa0 %.200s", branch);
      pl_segment(&p, "30", 5, -1, br);
    }
    else pl_segment(&p, "97", 4, -1, b.s);
    buf_puts(&p, "\n");
    os_write(1, p.s, p.len);
    buf_free(&p);
  }
  else {	/* connector lines and brackets, like Parrot OS, in blue */
    fd_puts(1, LINE "\xe2\x94\x8c\xe2\x94\x80");
    if (sh_status != 0) fd_puts(1, "[\033[0;31m\xe2\x9c\x97" LINE "]\xe2\x94\x80");
    fd_printf(1, "[\033[0;92m%s\033[0;33m@\033[0;96m%s" LINE "]\xe2\x94\x80[\033[0;94mMMC"
                 LINE "]\xe2\x94\x80[\033[0;32m%s" LINE "]",
              user ? user : "", host ? host : "", b.s);
    if (branch) fd_printf(1, "\xe2\x94\x80[\033[0;36m%s" LINE "]", branch);
    fd_puts(1, "\033[0m\n");
  }
  buf_free(&b);
  free(cwd); free(shown); free(branch);
}

/* }================================================================== */


/*
** {==================================================================
** Welcome banner: MMC in big letters, green like an old monitor, and
** who made it. A file /etc/banner replaces the big letters (with a
** picture, for example); an empty /etc/banner means no banner at all.
** ===================================================================
*/

static const char *const banner_art[] = {
  "\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x95\x97   \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x95\x97 \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x95\x97   \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x95\x97  \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x95\x97",
  "\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x95\x97 \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x95\x91 \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x95\x97 \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x95\x91 \xe2\x96\x88\xe2\x96\x88\xe2\x95\x94\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x9d",
  "\xe2\x96\x88\xe2\x96\x88\xe2\x95\x94\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x95\x94\xe2\x96\x88\xe2\x96\x88\xe2\x95\x91 \xe2\x96\x88\xe2\x96\x88\xe2\x95\x94\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x95\x94\xe2\x96\x88\xe2\x96\x88\xe2\x95\x91 \xe2\x96\x88\xe2\x96\x88\xe2\x95\x91     ",
  "\xe2\x96\x88\xe2\x96\x88\xe2\x95\x91\xe2\x95\x9a\xe2\x96\x88\xe2\x96\x88\xe2\x95\x94\xe2\x95\x9d\xe2\x96\x88\xe2\x96\x88\xe2\x95\x91 \xe2\x96\x88\xe2\x96\x88\xe2\x95\x91\xe2\x95\x9a\xe2\x96\x88\xe2\x96\x88\xe2\x95\x94\xe2\x95\x9d\xe2\x96\x88\xe2\x96\x88\xe2\x95\x91 \xe2\x96\x88\xe2\x96\x88\xe2\x95\x91     ",
  "\xe2\x96\x88\xe2\x96\x88\xe2\x95\x91 \xe2\x95\x9a\xe2\x95\x90\xe2\x95\x9d \xe2\x96\x88\xe2\x96\x88\xe2\x95\x91 \xe2\x96\x88\xe2\x96\x88\xe2\x95\x91 \xe2\x95\x9a\xe2\x95\x90\xe2\x95\x9d \xe2\x96\x88\xe2\x96\x88\xe2\x95\x91 \xe2\x95\x9a\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x95\x97",
  "\xe2\x95\x9a\xe2\x95\x90\xe2\x95\x9d     \xe2\x95\x9a\xe2\x95\x90\xe2\x95\x9d \xe2\x95\x9a\xe2\x95\x90\xe2\x95\x9d     \xe2\x95\x9a\xe2\x95\x90\xe2\x95\x9d  \xe2\x95\x9a\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x9d",
  NULL
};

/* bright mint at the top, deep green at the bottom */
static const unsigned char banner_rgb[][3] = {
  {0xB6, 0xFF, 0xD6}, {0x7D, 0xF7, 0xB2}, {0x4C, 0xE8, 0x8E},
  {0x22, 0xD3, 0x6B}, {0x17, 0xA8, 0x54}, {0x0F, 0x7D, 0x3E}
};


/* can this terminal show 24 bit colors? */
static int has_truecolor (void) {
#ifdef _WIN32
  return 1;	/* the Windows 10+ console and Windows Terminal do */
#else
  const char *ct = var_get("COLORTERM");
  return ct != NULL && (strstr(ct, "truecolor") || strstr(ct, "24bit"));
#endif
}


static void banner_lines (Buf *b) {
  static const char *const mark = "  \033[32m[\033[0;92m+\033[0;32m]\033[0m ";
  static const char *const sep = " \033[32m::\033[0m ";
  buf_puts(b, mark);
  buf_puts(b, "\033[0;92m" MMC_NAME " " MMC_VERSION "\033[0m");
  buf_puts(b, sep);
  buf_puts(b, "\033[32mportable shell\033[0m\n");
  buf_puts(b, mark);
  buf_puts(b, "\033[32mdeveloped by\033[0m");
  buf_puts(b, sep);
  buf_puts(b, "\033[0;92m" MMC_AUTHOR "\033[0m\n");
  buf_puts(b, mark);
  buf_puts(b, "\033[32mtype \033[0;92mhelp\033[0;32m for help, "
              "\033[0;92mexit\033[0;32m to leave\033[0m\n");
}


static void show_banner (void) {
  char *etc = path_join(path_root(), "etc");
  char *file = path_join(etc, "banner");
  size_t len = 0;
  char *custom = read_file(file, &len);
  int truecolor = has_truecolor();
  int row;
  Buf b;
  free(etc);
  free(file);
  if (custom != NULL && len == 0) {	/* empty file: stay quiet */
    free(custom);
    return;
  }
  buf_init(&b);
  buf_putc(&b, '\n');
  if (custom != NULL) {	/* your own drawing */
    buf_putn(&b, custom, len);
    if (custom[len - 1] != '\n') buf_putc(&b, '\n');
    buf_puts(&b, "\033[0m");
    free(custom);
  }
  else {
    for (row = 0; banner_art[row] != NULL; row++) {
      char esc[40];
      if (truecolor)
        sprintf(esc, "\033[38;2;%d;%d;%dm", banner_rgb[row][0],
                banner_rgb[row][1], banner_rgb[row][2]);
      else strcpy(esc, row < 3 ? "\033[0;92m" : "\033[0;32m");
      buf_puts(&b, "  ");
      buf_puts(&b, esc);
      buf_puts(&b, banner_art[row]);
      buf_puts(&b, "\033[0m\n");
    }
  }
  buf_putc(&b, '\n');
  banner_lines(&b);
  os_write(1, b.s, b.len);
  buf_free(&b);
}

/* }================================================================== */


/*
** {==================================================================
** Reading commands: the prompt, continuation lines, history
** ===================================================================
*/

/*
** History expansion (set -H, on in an interactive shell):
** !! !n !-n !word !?word? and the words !^ !$ !* !!:n !!:n-m,
** the parts :h :t :r :e, :p, :s/old/new/ (:gs) and ^old^new.
*/

/* the words of a history line; quotes keep their blanks together */
static void hist_words (const char *s, Vec *out) {
  while (*s != '\0') {
    Buf w;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '\0') break;
    buf_init(&w);
    while (*s != '\0' && *s != ' ' && *s != '\t') {
      if (*s == '\'' || *s == '"') {
        char q = *s++;
        buf_putc(&w, q);
        while (*s != '\0' && *s != q) buf_putc(&w, *s++);
        if (*s != '\0') buf_putc(&w, *s++);
      }
      else buf_putc(&w, *s++);
    }
    vec_push(out, w.s ? buf_take(&w) : xstrdup(""));
  }
}


/* the newest history line that starts with (or, whole != 0, contains) 'what' */
static const char *hist_search (const char *what, int whole) {
  const Vec *h = line_hist();
  size_t i;
  for (i = h->n; i > 0; i--) {
    const char *s = h->v[i - 1];
    if (whole ? (strstr(s, what) != NULL) : (strncmp(s, what, strlen(what)) == 0))
      return s;
  }
  return NULL;
}


/* old -> new in s: once, or everywhere when 'all' */
static char *hist_sub (const char *s, const char *old, const char *new_, int all) {
  Buf b;
  size_t n = strlen(old);
  const char *p = s;
  buf_init(&b);
  if (n == 0) return xstrdup(s);
  while (*p != '\0') {
    if (strncmp(p, old, n) == 0) {
      buf_puts(&b, new_);
      p += n;
      if (!all) break;
    }
    else buf_putc(&b, *p++);
  }
  buf_puts(&b, p);
  return b.s ? buf_take(&b) : xstrdup("");
}


/* :h :t :r :e and :s/:gs, in place */
static void hist_modify (char **text, const char **pp, char **last_old, char **last_new) {
  const char *p = *pp;
  while (*p == ':') {
    char c = p[1];
    char *s = *text;
    int all = 0;
    if (c == 'g' && (p[2] == 's' || p[2] == '&')) {
      all = 1;
      p++;
      c = p[1];
    }
    if (c == 'h') {	/* the directory */
      char *slash = strrchr(s, '/');
      *text = slash ? xstrndup(s, (size_t)(slash - s)) : xstrdup(".");
      free(s);
      p += 2;
    }
    else if (c == 't') {	/* the file name */
      char *slash = strrchr(s, '/');
      *text = xstrdup(slash ? slash + 1 : s);
      free(s);
      p += 2;
    }
    else if (c == 'r') {	/* without the extension */
      char *dot = strrchr(s, '.');
      *text = dot ? xstrndup(s, (size_t)(dot - s)) : xstrdup(s);
      free(s);
      p += 2;
    }
    else if (c == 'e') {	/* only the extension */
      char *dot = strrchr(s, '.');
      *text = xstrdup(dot ? dot : "");
      free(s);
      p += 2;
    }
    else if (c == 's' || c == '&') {
      char *old = NULL, *new_ = NULL;
      if (c == '&') {	/* :& repeats the last substitution */
        old = *last_old ? xstrdup(*last_old) : NULL;
        new_ = *last_new ? xstrdup(*last_new) : xstrdup("");
        p += 2;
      }
      else {
        char sep = p[2];
        const char *q;
        if (sep == '\0') break;
        q = p + 3;
        {
          const char *e1 = strchr(q, sep);
          if (e1 == NULL) {
            old = xstrdup(q);
            new_ = xstrdup("");
            p = q + strlen(q);
          }
          else {
            const char *e2 = strchr(e1 + 1, sep);
            old = xstrndup(q, (size_t)(e1 - q));
            new_ = e2 ? xstrndup(e1 + 1, (size_t)(e2 - e1 - 1)) : xstrdup(e1 + 1);
            p = e2 ? e2 + 1 : e1 + 1 + strlen(e1 + 1);
          }
        }
        free(*last_old);
        free(*last_new);
        *last_old = xstrdup(old);
        *last_new = xstrdup(new_);
      }
      if (old != NULL) {
        *text = hist_sub(s, old, new_, all);
        free(s);
      }
      free(old);
      free(new_);
    }
    else break;
  }
  *pp = p;
}


/* one !... at *pp; NULL and *err when there is no such line */
static char *hist_one (const char **pp, int *err, int *print_only,
                       char **last_old, char **last_new) {
  const Vec *h = line_hist();
  const char *p = *pp + 1;	/* after the ! */
  const char *line = NULL;
  char *text;
  Vec words;
  size_t from = 0, to = 0;
  int have_words = 0;
  if (*p == '!') {
    line = h->n ? h->v[h->n - 1] : NULL;
    p++;
  }
  else if (*p == '?') {
    const char *e = strchr(p + 1, '?');
    char *what = e ? xstrndup(p + 1, (size_t)(e - p - 1)) : xstrdup(p + 1);
    line = hist_search(what, 1);
    free(what);
    p = e ? e + 1 : p + 1 + strlen(p + 1);
  }
  else if (*p == '-' || isdigit((unsigned char)*p)) {
    int neg = (*p == '-');
    long n;
    if (neg) p++;
    n = strtol(p, (char **)&p, 10);
    if (neg) line = ((size_t)n <= h->n && n > 0) ? h->v[h->n - (size_t)n] : NULL;
    else line = (n > 0 && (size_t)n <= h->n) ? h->v[n - 1] : NULL;
  }
  else if (*p == '$' || *p == '^' || *p == '*' || *p == ':') {
    line = h->n ? h->v[h->n - 1] : NULL;	/* !$ is short for !!:$ */
  }
  else {	/* !word: the newest line that starts with it */
    size_t n = strcspn(p, " \t\n:;&|<>()'\"$");
    char *what;
    if (n == 0) {
      *err = 0;	/* a lonely ! is just a ! */
      return NULL;
    }
    what = xstrndup(p, n);
    line = hist_search(what, 0);
    free(what);
    p += n;
  }
  if (line == NULL) {
    *err = 1;
    return NULL;
  }
  vec_init(&words);
  if (*p == ':' && (isdigit((unsigned char)p[1]) || p[1] == '^' || p[1] == '$' || p[1] == '*' || p[1] == '-')) p++;
  if (*p == '^' || *p == '$' || *p == '*' || isdigit((unsigned char)*p)) {
    hist_words(line, &words);
    have_words = 1;
    if (words.n == 0) from = to = 0;
    else if (*p == '^') {
      from = to = (words.n > 1) ? 1 : 0;
      p++;
    }
    else if (*p == '$') {
      from = to = words.n - 1;
      p++;
    }
    else if (*p == '*') {
      from = (words.n > 1) ? 1 : 0;
      to = words.n - 1;
      p++;
    }
    else {
      from = to = (size_t)strtol(p, (char **)&p, 10);
      if (*p == '-') {
        p++;
        if (*p == '$') {
          to = words.n ? words.n - 1 : 0;
          p++;
        }
        else if (isdigit((unsigned char)*p)) to = (size_t)strtol(p, (char **)&p, 10);
        else to = words.n ? words.n - 1 : 0;
      }
      else if (*p == '*') {
        to = words.n ? words.n - 1 : 0;
        p++;
      }
    }
  }
  if (have_words) {
    Buf b;
    size_t i;
    buf_init(&b);
    for (i = from; i <= to && i < words.n; i++) {
      if (i > from) buf_putc(&b, ' ');
      buf_puts(&b, words.v[i]);
    }
    text = b.s ? buf_take(&b) : xstrdup("");
  }
  else text = xstrdup(line);
  vec_free(&words);
  if (*p == ':' && p[1] == 'p') {
    *print_only = 1;
    p += 2;
  }
  hist_modify(&text, &p, last_old, last_new);
  *pp = p;
  return text;
}


/*
** Expands the ! references of an input line. Returns 1 when the line
** changed, 0 when there was nothing to do, and -1 when a reference
** points at a line that is not in the history (the line is not run).
*/
static int hist_expand (const char *line, char **out) {
  Buf b;
  const char *p = line;
  int changed = 0, sq = 0, print_only = 0;
  static char *last_old = NULL, *last_new = NULL;
  const Vec *h = line_hist();
  *out = NULL;
  if (line[0] == '^' && h->n > 0) {	/* ^old^new^: correct the last line */
    const char *e1 = strchr(line + 1, '^');
    char *old, *new_, *text;
    if (e1 == NULL) return 0;
    old = xstrndup(line + 1, (size_t)(e1 - line - 1));
    {
      const char *e2 = strchr(e1 + 1, '^');
      new_ = e2 ? xstrndup(e1 + 1, (size_t)(e2 - e1 - 1)) : xstrdup(e1 + 1);
    }
    text = hist_sub(h->v[h->n - 1], old, new_, 0);
    free(old);
    free(new_);
    *out = text;
    return 1;
  }
  buf_init(&b);
  while (*p != '\0') {
    if (*p == '\\' && p[1] == '!') {	/* \! is a plain ! */
      buf_putc(&b, '!');
      p += 2;
      changed = 1;
      continue;
    }
    if (*p == '\'' && !sq) sq = 1;
    else if (*p == '\'' && sq) sq = 0;
    if (*p == '!' && !sq && p[1] != '\0' && p[1] != ' ' && p[1] != '\t' &&
        p[1] != '=' && p[1] != '(' && p[1] != '\n') {
      int err = 0;
      const char *q = p;
      char *text = hist_one(&q, &err, &print_only, &last_old, &last_new);
      if (text != NULL) {
        buf_puts(&b, text);
        free(text);
        p = q;
        changed = 1;
        continue;
      }
      if (err) {
        char *what = xstrndup(p, strcspn(p, " \t\n;|&"));
        sh_error("%s: event not found", what);
        free(what);
        buf_free(&b);
        return -1;
      }
    }
    buf_putc(&b, *p++);
  }
  if (!changed) {
    buf_free(&b);
    return 0;
  }
  *out = b.s ? buf_take(&b) : xstrdup("");
  if (print_only) {	/* :p only shows the line */
    fd_printf(1, "%s\n", *out);
    line_hist_add(*out);
    free(*out);
    *out = xstrdup("");
  }
  return 1;
}


/* a multi-line command as one history line: "if x; then y; fi" */
static char *history_line (const char *cmd) {
  Buf b;
  const char *p = cmd;
  buf_init(&b);
  while (*p) {
    const char *nl = strchr(p, '\n');
    size_t n = nl ? (size_t)(nl - p) : strlen(p);
    buf_putn(&b, p, n);
    if (nl == NULL) break;
    p = nl + 1;
    if (*p == '\0') break;
    {	/* after "then", "do", "{", "|", "&&" ... a space; else "; " */
      size_t k = b.len;
      static const char *const open[] = {"then", "do", "else", "{", "(", "|", "&&", "||",
                                         "in", ";", "&", NULL};
      int soft = 0, j;
      while (k > 0 && (b.s[k - 1] == ' ' || b.s[k - 1] == '\t')) k--;
      for (j = 0; open[j] && !soft; j++) {
        size_t ol = strlen(open[j]);
        if (k >= ol && strncmp(b.s + k - ol, open[j], ol) == 0 &&
            (k == ol || b.s[k - ol - 1] == ' ' || !isalnum((unsigned char)b.s[k - ol - 1]) || ol == 1))
          soft = 1;
      }
      buf_puts(&b, (soft || k == 0) ? " " : "; ");
    }
  }
  return buf_take(&b);
}


/* $PROMPT_COMMAND, before every prompt: a string, or an array of them */
static void run_prompt_command (void) {
  size_t n = var_count("PROMPT_COMMAND"), i;
  int saved = sh_status;
  if (n == 0) {
    const char *one = var_get("PROMPT_COMMAND");
    if (one != NULL && one[0] != '\0') sh_run_string(one, "PROMPT_COMMAND", sh_lineno);
  }
  else {
    for (i = 0; i < n; i++) {
      const char *one = var_aget("PROMPT_COMMAND", i);
      if (one != NULL && one[0] != '\0') sh_run_string(one, "PROMPT_COMMAND", sh_lineno);
    }
  }
  sh_status = saved;	/* $? keeps the status of the command, not of the hook */
}


static void repl (void) {
  int line0 = 1, warned_jobs = 0;
  if (sh_interactive) {
    const char *set = var_get("HISTFILE");	/* the profile may point elsewhere */
    char *hf = set && *set ? path_to_native(set) : path_join(g_home, ".mmc_history");
    line_hist_load(hf);
    if (set == NULL || *set == '\0') {
      char *shown = path_to_display(hf);
      var_set("HISTFILE", shown);
      free(shown);
    }
    free(hf);
    opt_set("histexpand", 1);	/* !! and friends, like an interactive bash */
    show_banner();
  }
  while (!sh_exit) {
    Buf cmd;
    char *line;
    int first = 1, r, bad = 0;
    buf_init(&cmd);
    for (;;) {
      char *prompt;
      if (sh_interactive) {
        job_poll(1);
        os_tty_fix();
        os_interrupted = 0;
        if (first) {
          run_prompt_command();
          show_prompt_header();
        }
        prompt = first ? prompt_last_line() : expand_prompt(var_get("PS2") ? var_get("PS2") : "> ");
      }
      else prompt = xstrdup("");
      line = line_read(prompt);
      free(prompt);
      if (line == NULL) break;
      if (first && strncmp(line, "\xEF\xBB\xBF", 3) == 0)	/* UTF-8 BOM from a pipe */
        memmove(line, line + 3, strlen(line + 3) + 1);
      if (sh_interactive && opt_get("histexpand") &&
          (strchr(line, '!') != NULL || (first && line[0] == '^'))) {
        char *ex = NULL;
        int e = hist_expand(line, &ex);
        if (e < 0) {	/* !nothing: the line is dropped, like bash */
          free(line);
          bad = 1;
          break;
        }
        if (e > 0) {
          free(line);
          line = ex;
          fd_printf(1, "%s\n", line);	/* show what it became */
        }
      }
      if (!first) buf_putc(&cmd, '\n');
      buf_puts(&cmd, line);
      free(line);
      first = 0;
      if (cmd.len > 0 && cmd.s[cmd.len - 1] == '\\' &&
          (cmd.len < 2 || cmd.s[cmd.len - 2] != '\\')) {	/* a line continuation */
        continue;
      }
      r = parse_is_complete(cmd.s ? cmd.s : "");
      if (r != P_INCOMPLETE) break;
    }
    if (bad) {	/* a ! that pointed nowhere: forget the whole command */
      sh_status = 1;
      buf_free(&cmd);
      continue;
    }
    if (first && line == NULL) {	/* end of input */
      if (sh_interactive) fd_puts(1, "exit\n");
      buf_free(&cmd);
      break;
    }
    if (cmd.s == NULL) {
      buf_free(&cmd);
      if (line == NULL) break;
      continue;
    }
    if (sh_interactive && cmd.s[0] != '\0') {
      char *h = history_line(cmd.s);
      line_hist_add(h);
      free(h);
    }
    sh_run_string(cmd.s, sh_interactive ? NULL : "mmc", line0);
    {	/* line numbers keep counting over the whole input */
      const char *p;
      for (p = cmd.s; *p; p++)
        if (*p == '\n') line0++;
      line0++;
    }
    buf_free(&cmd);
    if (line == NULL) break;
    if (sh_interactive) {
      os_interrupted = 0;
      if (sh_exit && job_count() > 0) {
        if (opt_get("checkjobs") && !warned_jobs) {	/* like bash: ask once */
          fd_puts(2, "mmc: there are running jobs.\n");
          job_list(2, 0);
          warned_jobs = 1;
          sh_exit = 0;
        }
        else if (opt_get("huponexit")) job_hup_all();
      }
    }
  }
}

/* }================================================================== */


/*
** {==================================================================
** mmc --check: syntax and commands, without running anything
** ===================================================================
*/

typedef struct Check {
  const char *file;
  Vec funcs;	/* functions the script defines */
  Vec missing;	/* "line: name" already reported */
  int problems;
  int sources;	/* the script sources other files: they may define more */
} Check;


static int plain_word (const char *w) {
  return w[0] != '\0' && strpbrk(w, "$`'\"\\*?[{~=") == NULL;
}


static void check_node (Check *c, Node *n);


static void check_list (Check *c, Node **k, int n) {
  int i;
  for (i = 0; i < n; i++) check_node(c, k[i]);
}


static void collect_funcs (Check *c, Node *n) {
  int i;
  CaseItem *it;
  if (n == NULL) return;
  if (n->type == N_FUNC) vec_push(&c->funcs, xstrdup(n->str));
  if (n->type == N_SIMPLE && n->nwords > 0 && (strcmp(n->words[0], "source") == 0 ||
                                          strcmp(n->words[0], ".") == 0))
    c->sources = 1;
  collect_funcs(c, n->a);
  collect_funcs(c, n->b);
  collect_funcs(c, n->c);
  for (i = 0; i < n->nkids; i++) collect_funcs(c, n->kids[i]);
  for (it = n->items; it != NULL; it = it->next) collect_funcs(c, it->body);
}


static int known_command (Check *c, const char *name) {
  size_t i;
  char *exe;
  if (builtin_find(name, 0) || builtin_find(name, 1) || alias_get(name) ||
      parse_is_keyword(name) || func_find(name))
    return 1;
  for (i = 0; i < c->funcs.n; i++)
    if (strcmp(c->funcs.v[i], name) == 0) return 1;
  if ((exe = sh_find_command(name)) != NULL) {
    free(exe);
    return 1;
  }
  return 0;
}


static void check_node (Check *c, Node *n) {
  CaseItem *it;
  if (n == NULL) return;
  switch (n->type) {
    case N_SIMPLE:
      if (n->nwords > 0 && plain_word(n->words[0]) && !known_command(c, n->words[0])) {
        size_t i;
        int seen = 0;
        for (i = 0; i < c->missing.n; i++)
          if (strcmp(c->missing.v[i], n->words[0]) == 0) seen = 1;
        if (!seen) {
          if (c->sources)	/* it may come from a file the script reads */
            fd_printf(1, "%s:%d: note: '%s' is not a builtin, function or program here "
                         "(maybe defined in a sourced file)\n", c->file, n->line, n->words[0]);
          else {
            fd_printf(1, "%s:%d: '%s': command not found (not a builtin, function or in PATH)\n",
                      c->file, n->line, n->words[0]);
            c->problems++;
          }
          vec_push(&c->missing, xstrdup(n->words[0]));
        }
      }
      break;
    case N_COPROC:
      fd_printf(1, "%s:%d: coproc is not supported by mmc\n", c->file, n->line);
      c->problems++;
      break;
    default:
      break;
  }
  check_node(c, n->a);
  check_node(c, n->b);
  check_node(c, n->c);
  if (n->kids) check_list(c, n->kids, n->nkids);
  for (it = n->items; it != NULL; it = it->next) check_node(c, it->body);
}


static int check_file (const char *name) {
  char *native = path_to_native(name);
  size_t len = 0;
  char *text = read_file(native, &len);
  Parser *p;
  Check c;
  Vec progs;
  int r, lines = 0;
  size_t i;
  free(native);
  if (text == NULL) {
    fd_printf(2, "mmc: --check: %s: cannot read\n", name);
    return 1;
  }
  crlf_to_lf(text, &len);
  memset(&c, 0, sizeof(c));
  c.file = name;
  vec_init(&c.funcs);
  vec_init(&c.missing);
  vec_init(&progs);
  /* first pass: every function the file defines */
  p = parse_new(text, name, 1);
  parse_set_check(p, 1);
  {
    Node *n;
    while ((r = parse_next(p, &n)) == P_OK) collect_funcs(&c, n);
  }
  if (r == P_ERROR || r == P_INCOMPLETE) c.problems++;
  parse_free(p);
  /* second pass: the commands */
  if (c.problems == 0) {
    Node *n;
    p = parse_new(text, name, 1);
    parse_set_check(p, 1);
    while ((r = parse_next(p, &n)) == P_OK) check_node(&c, n);
    parse_free(p);
  }
  for (i = 0; i < len; i++)
    if (text[i] == '\n') lines++;
  if (c.problems == 0) fd_printf(1, "%s: OK (%d lines)\n", name, lines);
  else fd_printf(1, "%s: %d problem%s\n", name, c.problems, c.problems == 1 ? "" : "s");
  vec_free(&c.funcs);
  vec_free(&c.missing);
  vec_free(&progs);
  free(text);
  return c.problems ? 1 : 0;
}

/* }================================================================== */


static int usage (int fd) {
  fd_printf(fd,
    "%s %s - portable shell by %s (bash compatible)\n\n"
    "usage: %s [options] [script [args...]]\n"
    "  (nothing)        start the interactive shell in the current folder\n"
    "  -c 'command'     run one command line and exit ($0 $1 ... may follow)\n"
    "  -s               read commands from standard input\n"
    "  script           run the commands in a file\n"
    "  -e -u -x -o opt  set options, as with 'set' (-o pipefail ...)\n"
    "  -n               read the commands, run nothing (syntax check)\n"
    "  --check file...  check scripts: syntax, and commands that do not exist\n"
    "  --complete LINE  what Tab would offer for that command line\n"
    "  --norc           do not read ~/.mmcrc\n"
    "  --noprofile      do not read /etc/profile\n"
    "  --root DIR       use DIR as the MMC folder instead of the program's\n"
    "  --version        print the version\n"
    "  --help           this text\n",
    MMC_NAME, MMC_VERSION, MMC_AUTHOR, MMC_NAME);
  return fd == 1 ? 0 : 2;
}


int main (int argc, char **argv) {
  const char *command = NULL, *root = NULL, *stage_file = NULL;
  long stage_pid = 0;
  char *cwd, *exedir;
  int i, top_level, norc = 0, noprofile = 0, from_stdin = 0, check = 0, force_i = 0;
  const char *complete_line = NULL;
  int have_script = 0;
  Vec setopts;	/* -e -x -o pipefail ... applied after setup */
  os_args(&argc, &argv);
  vec_init(&sh_pos);
  vec_init(&setopts);
  sh_init_fds();
  for (i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--version") == 0) {
      fd_printf(1, "%s %s (bash %s compatible)\n", MMC_NAME, MMC_VERSION, MMC_BASH_COMPAT);
      return 0;
    }
    else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) return usage(1);
    else if (strcmp(a, "--root") == 0 && i + 1 < argc) root = argv[++i];
    else if (strcmp(a, "--stage") == 0 && i + 2 < argc) {
      stage_file = argv[++i];
      stage_pid = atol(argv[++i]);
    }
    else if (strcmp(a, "--check") == 0) {
      check = 1;
      i++;
      break;
    }
    else if (strcmp(a, "--complete") == 0 && i + 1 < argc) complete_line = argv[++i];
    else if (strcmp(a, "--norc") == 0) norc = 1;
    else if (strcmp(a, "--noprofile") == 0) noprofile = 1;
    else if (strcmp(a, "--login") == 0 || strcmp(a, "-l") == 0) sh_login = 1;
    else if (strcmp(a, "--posix") == 0) vec_push(&setopts, xstrdup("-oposix"));
    else if (strcmp(a, "--") == 0 || strcmp(a, "-") == 0) {
      i++;
      break;
    }
    else if (strcmp(a, "-c") == 0) {
      if (i + 1 >= argc) {
        fd_puts(2, "mmc: -c: option requires an argument\n");
        return 2;
      }
      command = argv[++i];
    }
    else if (strcmp(a, "-o") == 0 || strcmp(a, "+o") == 0) {
      if (i + 1 < argc) vec_push(&setopts, xstrcat3(a[0] == '-' ? "-o" : "+o", argv[++i], ""));
    }
    else if ((a[0] == '-' || a[0] == '+') && a[1] != '\0' && a[1] != '-') {
      const char *p;
      for (p = a + 1; *p; p++) {
        if (*p == 'c' && a[0] == '-') {	/* -ec 'cmd' */
          if (i + 1 >= argc) return usage(2);
          command = argv[++i];
        }
        else if (*p == 's') from_stdin = 1;
        else if (*p == 'i') force_i = 1;
        else if (*p == 'l') sh_login = 1;
        else {
          char opt[3];
          opt[0] = a[0];
          opt[1] = *p;
          opt[2] = '\0';
          vec_push(&setopts, xstrdup(opt));
        }
      }
    }
    else if (a[0] == '-' && a[1] == '-') {
      fd_printf(2, "mmc: %s: invalid option\n", a);
      return usage(2);
    }
    else break;
  }
  os_init();
  if (command != NULL) {	/* -c 'cmd' [name [args]] */
    vec_push(&sh_pos, xstrdup(i < argc ? argv[i++] : MMC_NAME));
    for (; i < argc; i++) vec_push(&sh_pos, xstrdup(argv[i]));
  }
  else if (check || stage_file) {
    vec_push(&sh_pos, xstrdup(MMC_NAME));
  }
  else if (from_stdin) {
    vec_push(&sh_pos, xstrdup(MMC_NAME));
    for (; i < argc; i++) vec_push(&sh_pos, xstrdup(argv[i]));
  }
  else {
    have_script = i < argc;
    for (; i < argc; i++) vec_push(&sh_pos, xstrdup(argv[i]));	/* script args */
  }
  var_init();
  top_level = var_get("MMC_LEVEL") == NULL;
  setup_root(argv[0], root);
  setup_env();
  setup_tree();
  sh_pid = os_getpid();
  setup_shell_vars();
  if (stage_file != NULL) {	/* a pipeline stage or & job of a parent mmc */
    int st = sh_stage_main(stage_file, stage_pid);
    os_shutdown();
    return st & 0xFF;
  }
  if (check) {
    int status = 0;
    char *etc = path_join(path_root(), "etc");
    char *profile = path_join(etc, "profile");
    sh_source(profile, 0, NULL, 0);	/* PATH from the profile, to find commands */
    free(etc);
    free(profile);
    if (i >= argc) {
      fd_puts(2, "mmc: --check: which files?\n");
      return 2;
    }
    for (; i < argc; i++)
      if (check_file(argv[i]) != 0) status = 1;
    return status;
  }
  /* --complete reads the config like an interactive shell: that is where
  ** the completion rules are set up */
  sh_interactive = force_i || complete_line != NULL ||
                   (command == NULL && !have_script && os_is_tty(0) && !from_stdin);
  if (sh_interactive) opt_set("monitor", 1);
  opt_set("expand_aliases", sh_interactive);	/* bash: scripts do not expand aliases */
  if (sh_login) opt_set("login_shell", 1);
  if (sh_pos.n == 0) vec_push(&sh_pos, xstrdup(MMC_NAME));
  source_config(top_level, norc, noprofile);
  {	/* -e -x -o pipefail ... from the command line */
    size_t k;
    for (k = 0; k < setopts.n; k++) {
      const char *o = setopts.v[k];
      if (o[1] == 'o') opt_set(o + 2, o[0] == '-');
      else opt_letter(o[1], o[0] == '-');
    }
    vec_free(&setopts);
  }
  if (complete_line != NULL) {	/* --complete 'git che': what Tab would offer */
    Vec words, cands;
    size_t cword = 0, k;
    unsigned opts = 0;
    const char *word;
    vec_init(&words);
    vec_init(&cands);
    line_words_at(complete_line, strlen(complete_line), &words, &cword);
    word = (cword < words.n) ? words.v[cword] : "";
    if (!comp_for_line(complete_line, strlen(complete_line), &words, cword, word,
                       &cands, &opts))
      fd_puts(2, "mmc: --complete: no completion rule for this command\n");
    vec_sort(&cands);
    for (k = 0; k < cands.n; k++) fd_printf(1, "%s\n", cands.v[k]);
    vec_free(&words);
    vec_free(&cands);
    return 0;
  }
  if (command != NULL) {
    sh_run_string(command, MMC_NAME, 1);
  }
  else if (have_script) {
    char *native = path_to_native(sh_pos.v[0]);
    OsStat st;
    if (os_stat(native, &st) != 0) {
      fd_printf(2, "mmc: %s: No such file or directory\n", sh_pos.v[0]);
      free(native);
      os_shutdown();
      return 127;
    }
    sh_status = sh_run_script(native);
    free(native);
  }
  else {
    /* started from its own folder (double click): go home instead */
    cwd = os_getcwd();
    exedir = path_dirname(g_exe);
    if (sh_interactive && m_fncmp(cwd, exedir) == 0) {
      char *shown;
      os_chdir(g_home);
      shown = path_to_display(g_home);
      var_set("PWD", shown);
      free(shown);
    }
    free(cwd);
    free(exedir);
    repl();
  }
  sh_exit_now(sh_status);
  os_shutdown();
  return sh_status & 0xFF;
}
