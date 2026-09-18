/*
** mmc.c - MMC shell (Marjon Mangindo Cajocon)
**
** A small portable shell in the spirit of git-bash: the folder that
** holds the executable becomes "/", with home/, usr/, etc/ and tmp/
** inside, so a whole development environment can live on any drive.
*/

#include "mmc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static const char *const default_profile =
  "# /etc/profile - settings for everyone who uses this MMC folder.\n"
  "# Read once when mmc starts. Syntax is a small bash-like subset:\n"
  "#   export NAME=value    alias name='value'    source file    # comment\n"
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


static void setup_env (void) {
  Buf path;
  char level[16];
  char *user = os_username();
  char *host = os_hostname();
  char *old = os_getenv("PATH");
  char *lvl = os_getenv("MMC_LEVEL");
  char *exedir = path_dirname(g_exe);
  char *rootp, *home, *shell, *tmp;
#ifdef _WIN32
  char *term = os_getenv("TERM");
  char *drive = path_to_drive(path_root());
  rootp = xstrdup("");	/* the root is simply "/" */
  os_setenv("MMC_ROOT", drive);
  if (drive[0] == '/' && drive[1] != '\0') {
    drive[2] = '\0';
    os_setenv("MMC_DRIVE", drive);
  }
  if (term == NULL) os_setenv("TERM", "xterm-256color");
  free(term);
  free(drive);
#else
  rootp = xstrdup(path_root());
  os_setenv("MMC_ROOT", rootp);
#endif
  home = xstrcat3(rootp, "/home/", user);
  g_home = path_to_native(home);
  tmp = path_to_display(exedir);
  shell = path_to_display(g_exe);
  buf_init(&path);
  if (lvl != NULL) {	/* nested mmc: PATH is set up already, keep its order */
    buf_puts(&path, old ? old : "");
    buf_puts(&path, ":");
  }
  buf_puts(&path, rootp);
  buf_puts(&path, "/usr/bin:");
  buf_puts(&path, home);
  buf_puts(&path, "/bin:");
  buf_puts(&path, tmp);	/* before the system: Windows has its own mmc.exe */
  buf_puts(&path, ":");
  buf_puts(&path, old ? old : "");
  sh_setvar("PATH", path.s);
  os_setenv("HOME", home);
  os_setenv("USER", user);
  os_setenv("HOSTNAME", host);
  os_setenv("SHELL", shell);
  sprintf(level, "%d", (lvl ? atoi(lvl) : 0) + 1);
  os_setenv("MMC_LEVEL", level);
  buf_free(&path);
  free(user); free(host); free(old); free(lvl); free(exedir);
  free(rootp); free(home); free(shell); free(tmp);
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


static void source_config (int top_level) {
  char *etc = path_join(path_root(), "etc");
  char *profile = path_join(etc, "profile");
  char *rc = path_join(g_home, ".mmcrc");
  if (top_level) sh_source(profile, 0);	/* nested shells inherit it */
  if (top_level || sh_interactive) sh_source(rc, 0);
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

/* MMC_PROMPT=classic in /etc/profile or ~/.mmcrc gives the one line prompt */
static int prompt_is_classic (void) {
  char *style = os_getenv("MMC_PROMPT");
  int classic = style != NULL && strcmp(style, "classic") == 0;
  free(style);
  return classic;
}


static const char *prompt_last_line (void) {
  return prompt_is_classic() ? "\033[32m$\033[0m "
                             : LINE "└──╼ \033[0;33m$\033[0m ";
}


static void show_prompt_header (void) {
  char *cwd = os_getcwd();
  char *shown = path_to_display(cwd);
  char *home = os_getenv("HOME");
  char *user = os_getenv("USER");
  char *host = os_getenv("HOSTNAME");
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
  fd_printf(1, "\033]0;MMC:%s\007\n", b.s);
  if (prompt_is_classic()) {	/* one line, like git-bash */
    fd_printf(1, "\033[32m%s@%s \033[0;7;34m MMC \033[0m %s",
              user ? user : "", host ? host : "", b.s);
    if (branch) fd_printf(1, "\033[0;36m (%s)", branch);
  }
  else {	/* connector lines and brackets, like Parrot OS, in blue */
    fd_puts(1, LINE "┌─");
    if (sh_status != 0) fd_puts(1, "[\033[0;31m✗" LINE "]─");
    fd_printf(1, "[\033[0;92m%s\033[0;33m@\033[0;96m%s" LINE "]─[\033[0;94mMMC"
                 LINE "]─[\033[0;32m%s" LINE "]",
              user ? user : "", host ? host : "", b.s);
    if (branch) fd_printf(1, "─[\033[0;36m%s" LINE "]", branch);
  }
  fd_puts(1, "\033[0m\n");
  buf_free(&b);
  free(cwd); free(shown); free(home); free(user); free(host); free(branch);
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
  "███╗   ███╗ ███╗   ███╗  ██████╗",
  "████╗ ████║ ████╗ ████║ ██╔════╝",
  "██╔████╔██║ ██╔████╔██║ ██║     ",
  "██║╚██╔╝██║ ██║╚██╔╝██║ ██║     ",
  "██║ ╚═╝ ██║ ██║ ╚═╝ ██║ ╚██████╗",
  "╚═╝     ╚═╝ ╚═╝     ╚═╝  ╚═════╝",
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
  char *ct = os_getenv("COLORTERM");
  int yes = ct != NULL && (strstr(ct, "truecolor") || strstr(ct, "24bit"));
  free(ct);
  return yes;
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


static void repl (void) {
  if (sh_interactive) {
    char *hf = path_join(g_home, ".mmc_history");
    line_hist_load(hf);
    free(hf);
    show_banner();
  }
  while (!sh_exit) {
    char *line;
    if (sh_interactive) {
      os_reap();
      os_tty_fix();
      show_prompt_header();
    }
    line = line_read(sh_interactive ? prompt_last_line() : "");
    if (line == NULL) {
      if (sh_interactive) fd_puts(1, "exit\n");
      break;
    }
    if (sh_interactive) line_hist_add(line);
    if (strncmp(line, "\xEF\xBB\xBF", 3) == 0)	/* UTF-8 BOM from a pipe */
      memmove(line, line + 3, strlen(line + 3) + 1);
    sh_run_line(line);
    free(line);
  }
}


static int usage (int fd) {
  fd_printf(fd,
    "%s %s - portable shell by %s\n\n"
    "usage: %s [options] [script [args...]]\n"
    "  (nothing)      start the interactive shell in the current folder\n"
    "  -c 'command'   run one command line and exit\n"
    "  script         run the commands in a file\n"
    "  --root DIR     use DIR as the MMC folder instead of the program's\n"
    "  --version      print the version\n"
    "  --help         this text\n",
    MMC_NAME, MMC_VERSION, MMC_AUTHOR, MMC_NAME);
  return fd == 1 ? 0 : 2;
}


int main (int argc, char **argv) {
  const char *command = NULL, *root = NULL;
  char *lvl, *cwd, *exedir;
  int i, top_level;
  os_args(&argc, &argv);
  vec_init(&sh_args);
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--version") == 0) {
      fd_printf(1, "%s %s\n", MMC_NAME, MMC_VERSION);
      return 0;
    }
    else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
      return usage(1);
    else if (strcmp(argv[i], "--root") == 0 && i + 1 < argc) root = argv[++i];
    else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) command = argv[++i];
    else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "-i") == 0 ||
             strcmp(argv[i], "--login") == 0)
      ;	/* accepted for bash compatibility */
    else if (argv[i][0] == '-' && argv[i][1] != '\0') return usage(2);
    else break;
  }
  os_init();
  if (command != NULL) vec_push(&sh_args, xstrdup(MMC_NAME));
  for (; i < argc; i++) vec_push(&sh_args, xstrdup(argv[i]));
  lvl = os_getenv("MMC_LEVEL");
  top_level = (lvl == NULL);
  free(lvl);
  setup_root(argv[0], root);
  setup_env();
  setup_tree();
  sh_interactive = (command == NULL && sh_args.n == 0 && os_is_tty(0));
  source_config(top_level);
  if (command != NULL) sh_run_line(command);
  else if (sh_args.n > 0) {
    char *native = path_to_native(sh_args.v[0]);
    sh_status = sh_source(native, 1);
    free(native);
  }
  else {
    /* started from its own folder (double click): go home instead */
    cwd = os_getcwd();
    exedir = path_dirname(g_exe);
    if (sh_interactive && m_fncmp(cwd, exedir) == 0) os_chdir(g_home);
    free(cwd);
    free(exedir);
    repl();
  }
  os_shutdown();
  return sh_status & 0xFF;
}
