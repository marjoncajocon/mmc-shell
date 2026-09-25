/*
** mos.c - operating system layer
**
** Everything that differs between Windows and Linux/macOS/Android lives
** here. All strings crossing this interface are UTF-8; all paths are
** native.
*/

#include "mmc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

volatile int os_interrupted = 0;
volatile int os_pending[65];
int os_pipe_exit = 0;


void os_proclist_free (OsProcInfo *v, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    free(v[i].name);
    free(v[i].cmd);
  }
  free(v);
}


#ifdef _WIN32

/*
** {==================================================================
** Windows
** ===================================================================
*/

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <fcntl.h>
#include <io.h>
#include <errno.h>
#include <psapi.h>
#include <sys/stat.h>

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING	0x0004
#endif
#ifndef ENABLE_VIRTUAL_TERMINAL_INPUT
#define ENABLE_VIRTUAL_TERMINAL_INPUT	0x0200
#endif

static HANDLE g_hin, g_hout;
static DWORD g_inmode, g_outmode;
static int g_have_in, g_have_out;
static UINT g_incp, g_outcp;
static int g_umask = 022;
static int g_int_mode = 0;	/* trap on INT: 0 default, 1 trap, 2 ignore */
static char *narrow (const wchar_t *w);
static int g_err = 0;	/* OS_E_... of the last failure */
static DWORD g_winerr = 0;


static int map_win_err (DWORD e) {
  switch (e) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_DRIVE:
    case ERROR_BAD_NETPATH: case ERROR_BAD_NET_NAME: case ERROR_INVALID_NAME:
    case ERROR_BAD_PATHNAME:
      return OS_E_NOENT;
    case ERROR_ACCESS_DENIED: case ERROR_WRITE_PROTECT: return OS_E_ACCES;
    case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS: return OS_E_EXIST;
    case ERROR_DIR_NOT_EMPTY: return OS_E_NOTEMPTY;
    case ERROR_DIRECTORY: return OS_E_NOTDIR;
    case ERROR_NOT_SAME_DEVICE: return OS_E_XDEV;
    case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION: case ERROR_BUSY:
      return OS_E_BUSY;
    case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: return OS_E_NOSPC;
    case ERROR_PRIVILEGE_NOT_HELD: return OS_E_PERM;
    case ERROR_INVALID_PARAMETER: return OS_E_INVAL;
    case ERROR_CANT_RESOLVE_FILENAME: return OS_E_LOOP;
  }
  return OS_E_OTHER;
}


/* the last Win32 call failed: remember why, return -1 */
static int fail (void) {
  g_winerr = GetLastError();
  g_err = map_win_err(g_winerr);
  return -1;
}


static int fail_errno (void) {
  g_winerr = 0;
  switch (errno) {
    case ENOENT: g_err = OS_E_NOENT; break;
    case EACCES: g_err = OS_E_ACCES; break;
    case EEXIST: g_err = OS_E_EXIST; break;
    case ENOTEMPTY: g_err = OS_E_NOTEMPTY; break;
    case ENOTDIR: g_err = OS_E_NOTDIR; break;
    case EISDIR: g_err = OS_E_ISDIR; break;
    case ENOSPC: g_err = OS_E_NOSPC; break;
    case EINVAL: g_err = OS_E_INVAL; break;
    default: g_err = OS_E_OTHER; break;
  }
  return -1;
}


int os_errcode (void) {
  return g_err;
}


const char *os_errmsg (void) {
  static char other[256];
  switch (g_err) {
    case OS_E_NOENT: return "No such file or directory";
    case OS_E_ACCES: return "Permission denied";
    case OS_E_EXIST: return "File exists";
    case OS_E_NOTEMPTY: return "Directory not empty";
    case OS_E_NOTDIR: return "Not a directory";
    case OS_E_ISDIR: return "Is a directory";
    case OS_E_XDEV: return "Invalid cross-device link";
    case OS_E_BUSY: return "Device or resource busy";
    case OS_E_NOSPC: return "No space left on device";
    case OS_E_PERM: return "Operation not permitted";
    case OS_E_INVAL: return "Invalid argument";
    case OS_E_LOOP: return "Too many levels of symbolic links";
  }
  if (g_winerr != 0) {	/* the text Windows has for it, on one line */
    wchar_t w[200];
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             NULL, g_winerr, 0, w, 200, NULL);
    if (n > 0) {
      char *t;
      while (n > 0 && (w[n - 1] == L'\r' || w[n - 1] == L'\n' || w[n - 1] == L'.')) n--;
      w[n] = 0;
      t = narrow(w);
      strncpy(other, t, sizeof(other) - 1);
      other[sizeof(other) - 1] = '\0';
      free(t);
      return other;
    }
  }
  return "Input/output error";
}


static wchar_t *widen (const char *s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  wchar_t *w;
  if (n <= 0) n = 1;
  w = (wchar_t *)xmalloc((size_t)n * sizeof(wchar_t));
  w[0] = L'\0';
  MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
  return w;
}


static char *narrow (const wchar_t *w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
  char *s;
  if (n <= 0) n = 1;
  s = (char *)xmalloc((size_t)n);
  s[0] = '\0';
  WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
  return s;
}


static BOOL WINAPI ctrl_handler (DWORD type) {
  /* Ctrl-C goes to the running child too; the shell survives and unwinds */
  if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
    if (g_int_mode != 2) {
      os_interrupted = (g_int_mode == 0);
      os_pending[2] = 1;
    }
    return TRUE;
  }
  return FALSE;
}


void os_init (void) {
  g_hin = GetStdHandle(STD_INPUT_HANDLE);
  g_hout = GetStdHandle(STD_OUTPUT_HANDLE);
  g_have_in = GetConsoleMode(g_hin, &g_inmode) != 0;
  g_have_out = GetConsoleMode(g_hout, &g_outmode) != 0;
  g_incp = GetConsoleCP();
  g_outcp = GetConsoleOutputCP();
  if (g_incp != 0) SetConsoleCP(CP_UTF8);
  if (g_outcp != 0) SetConsoleOutputCP(CP_UTF8);
  g_inmode |= ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT;
  g_inmode &= ~(DWORD)ENABLE_VIRTUAL_TERMINAL_INPUT;
  SetConsoleCtrlHandler(ctrl_handler, TRUE);
  os_tty_fix();
}


void os_shutdown (void) {
  if (g_have_in) SetConsoleMode(g_hin, g_inmode);
  if (g_have_out) SetConsoleMode(g_hout, g_outmode);
  if (g_incp != 0) SetConsoleCP(g_incp);
  if (g_outcp != 0) SetConsoleOutputCP(g_outcp);
}


/* programs may leave the console in any state; put it back */
void os_tty_fix (void) {
  if (g_have_in) SetConsoleMode(g_hin, g_inmode);
  if (g_have_out)
    SetConsoleMode(g_hout, g_outmode | ENABLE_PROCESSED_OUTPUT |
                           ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}


void os_args (int *argc, char ***argv) {
  int n = 0, i;
  wchar_t **w = CommandLineToArgvW(GetCommandLineW(), &n);
  char **v;
  if (w == NULL) return;
  v = (char **)xmalloc(((size_t)n + 1) * sizeof(char *));
  for (i = 0; i < n; i++) v[i] = narrow(w[i]);
  v[n] = NULL;
  LocalFree(w);
  *argc = n;
  *argv = v;
}


char *os_getenv (const char *name) {
  wchar_t *wn = widen(name);
  wchar_t *wv = NULL;
  char *r = NULL;
  DWORD n;
  SetLastError(0);
  n = GetEnvironmentVariableW(wn, NULL, 0);
  if (n == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND) goto done;
  wv = (wchar_t *)xmalloc(((size_t)n + 1) * sizeof(wchar_t));
  wv[0] = L'\0';
  GetEnvironmentVariableW(wn, wv, n + 1);
  r = narrow(wv);
 done:
  free(wn);
  free(wv);
  return r;
}


void os_setenv (const char *name, const char *value) {
  wchar_t *wn = widen(name);
  wchar_t *wv = value ? widen(value) : NULL;
  SetEnvironmentVariableW(wn, wv);
  free(wn);
  free(wv);
}


void os_env_list (Vec *out) {
  wchar_t *block = GetEnvironmentStringsW();
  wchar_t *p;
  if (block == NULL) return;
  for (p = block; *p; p += wcslen(p) + 1)
    if (*p != L'=') vec_push(out, narrow(p));	/* skip "=C:=C:\dir" */
  FreeEnvironmentStringsW(block);
}


char *os_getcwd (void) {
  DWORD n = GetCurrentDirectoryW(0, NULL);
  wchar_t *w = (wchar_t *)xmalloc(((size_t)n + 1) * sizeof(wchar_t));
  char *r;
  w[0] = L'\0';
  GetCurrentDirectoryW(n + 1, w);
  r = narrow(w);
  free(w);
  return r;
}


int os_chdir (const char *native) {
  wchar_t *w = widen(native);
  int ok = SetCurrentDirectoryW(w) != 0;
  if (!ok) fail();
  free(w);
  return ok ? 0 : -1;
}


static int has_ext (const char *path, const char *ext) {
  size_t n = strlen(path), e = strlen(ext);
  return n > e && m_stricmp(path + n - e, ext) == 0;
}


static time_t ft_time (FILETIME ft) {
  unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
  if (t < 116444736000000000ULL) return 0;
  return (time_t)(t / 10000000ULL - 11644473600ULL);
}


#define TAG_MOUNT_POINT	0xA0000003UL
#define TAG_SYMLINK	0xA000000CUL

/* a symbolic link or a junction; other reparse points (OneDrive files,
** app aliases ...) are just files and folders */
static int is_link_tag (const wchar_t *w) {
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(w, &fd);
  if (h == INVALID_HANDLE_VALUE) return 0;
  FindClose(h);
  return fd.dwReserved0 == TAG_SYMLINK || fd.dwReserved0 == TAG_MOUNT_POINT;
}


static void fill_common (OsStat *st, const char *native, DWORD attr, DWORD hi, DWORD lo,
                         FILETIME wt, FILETIME at, FILETIME ct) {
  st->exists = 1;
  st->is_dir = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
  st->size = st->is_dir ? 0 : (long long)(((unsigned long long)hi << 32) | lo);
  st->blocks = (st->size + 4095) / 4096 * 8;
  st->nlink = 1;
  st->mtime = ft_time(wt);
  st->atime = ft_time(at);
  st->ctime = ft_time(ct);
  st->mode = (attr & FILE_ATTRIBUTE_READONLY) ? 0444 : 0644;
  if (st->is_dir || has_ext(native, ".exe") || has_ext(native, ".com") ||
      has_ext(native, ".bat") || has_ext(native, ".cmd") || has_ext(native, ".sh"))
    st->mode |= 0111;
  st->uid = 1000;
  st->gid = 1000;
}


static int stat_common (const char *native, OsStat *st) {
  WIN32_FILE_ATTRIBUTE_DATA d;
  wchar_t *w = widen(native);
  int ok = GetFileAttributesExW(w, GetFileExInfoStandard, &d) != 0;
  memset(st, 0, sizeof(*st));
  if (!ok) {
    fail();
    free(w);
    if (m_stricmp(native, "NUL") == 0) {	/* /dev/null */
      st->exists = 1;
      st->is_chr = 1;
      st->mode = 0666;
      return 0;
    }
    return -1;
  }
  fill_common(st, native, d.dwFileAttributes, d.nFileSizeHigh, d.nFileSizeLow,
              d.ftLastWriteTime, d.ftLastAccessTime, d.ftCreationTime);
  st->is_link = (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 && is_link_tag(w);
  if (st->is_link) st->mode |= 0777;
  free(w);
  return 0;
}


int os_stat (const char *native, OsStat *st) {
  int r = stat_common(native, st);
  if (r == 0 && st->is_link) {	/* what the link points to */
    wchar_t *w = widen(native);
    HANDLE h = CreateFileW(w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    BY_HANDLE_FILE_INFORMATION fi;
    free(w);
    if (h == INVALID_HANDLE_VALUE) {	/* a dangling link */
      fail();
      memset(st, 0, sizeof(*st));
      return -1;
    }
    if (GetFileInformationByHandle(h, &fi)) {
      memset(st, 0, sizeof(*st));
      fill_common(st, native, fi.dwFileAttributes, fi.nFileSizeHigh, fi.nFileSizeLow,
                  fi.ftLastWriteTime, fi.ftLastAccessTime, fi.ftCreationTime);
      st->nlink = fi.nNumberOfLinks;
    }
    CloseHandle(h);
  }
  st->is_link = 0;
  return r;
}


int os_lstat (const char *native, OsStat *st) {
  return stat_common(native, st);
}


int os_is_exec (const char *native) {
  OsStat st;
  return os_stat(native, &st) == 0 && !st.is_dir;
}


/* a "#!" line makes a file executable, as in git-bash */
static int has_shebang (const char *native) {
  char head[2];
  int fd = os_open(native, OS_READ), ok = 0;
  if (fd < 0) return 0;
  ok = os_read(fd, head, 2) == 2 && head[0] == '#' && head[1] == '!';
  os_close(fd);
  return ok;
}


int os_access (const char *native, int what) {
  OsStat st;
  if (os_stat(native, &st) != 0) return 0;
  if (what == 'r') return 1;
  if (what == 'w') return (st.mode & 0200) != 0 || st.is_dir;
  if (st.is_dir || (st.mode & 0111)) return 1;
  return has_shebang(native);
}


int os_listdir (const char *native, Vec *out) {
  WIN32_FIND_DATAW fd;
  char *pat = path_join(native[0] ? native : ".", "*");
  wchar_t *w = widen(pat);
  HANDLE h = FindFirstFileW(w, &fd);
  free(pat);
  free(w);
  if (h == INVALID_HANDLE_VALUE) return fail();
  do {
    if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
      continue;
    vec_push(out, narrow(fd.cFileName));
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return 0;
}


int os_mkdir (const char *native) {
  wchar_t *w = widen(native);
  int ok = CreateDirectoryW(w, NULL) != 0;
  if (!ok) fail();
  free(w);
  return ok ? 0 : -1;
}


int os_unlink (const char *native) {
  wchar_t *w = widen(native);
  int ok = DeleteFileW(w) != 0;
  if (!ok) fail();
  free(w);
  return ok ? 0 : -1;
}


char *os_realpath (const char *native) {
  wchar_t *w = widen(native), buf[4096];
  DWORD n = GetFullPathNameW(w, 4096, buf, NULL);
  free(w);
  if (n == 0 || n >= 4096) return NULL;
  return narrow(buf);
}


int os_open (const char *native, int mode) {
  int flags = _O_BINARY | _O_NOINHERIT;
  wchar_t *w = widen(native);
  int fd;
  if (mode == OS_READ) flags |= _O_RDONLY;
  else if (mode == OS_WRITE) flags |= _O_WRONLY | _O_CREAT | _O_TRUNC;
  else if (mode == OS_RDWR) flags |= _O_RDWR | _O_CREAT;
  else if (mode == OS_EXCL) flags |= _O_WRONLY | _O_CREAT | _O_EXCL;
  else flags |= _O_WRONLY | _O_CREAT;
  fd = _wopen(w, flags, _S_IREAD | _S_IWRITE);
  if (fd < 0) {
    fail_errno();
    if (g_err == OS_E_ACCES) {	/* a folder, or really no permission? */
      DWORD a = GetFileAttributesW(w);
      if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) g_err = OS_E_ISDIR;
    }
  }
  free(w);
  /* we write with WriteFile, so do the "append" part ourselves */
  if (fd >= 0 && mode == OS_APPEND) _lseeki64(fd, 0, SEEK_END);
  return fd;
}


int os_pipe (int fds[2]) {
  return _pipe(fds, 65536, _O_BINARY | _O_NOINHERIT);
}


int os_dup (int fd) {
  HANDLE me = GetCurrentProcess();
  HANDLE h = (HANDLE)_get_osfhandle(fd);
  HANDLE copy;
  int r;
  if (h == INVALID_HANDLE_VALUE || h == (HANDLE)(intptr_t)-2) return -1;
  if (!DuplicateHandle(me, h, me, &copy, 0, FALSE, DUPLICATE_SAME_ACCESS))
    return -1;
  r = _open_osfhandle((intptr_t)copy, _O_BINARY | _O_NOINHERIT);
  if (r < 0) CloseHandle(copy);
  return r;
}


void os_close (int fd) {
  if (fd >= 0) _close(fd);
}


int os_fd_valid (int fd) {
  HANDLE h = (HANDLE)_get_osfhandle(fd);
  return h != INVALID_HANDLE_VALUE && h != (HANDLE)(intptr_t)-2;
}


long os_read (int fd, void *buf, size_t n) {
  HANDLE h = (HANDLE)_get_osfhandle(fd);
  DWORD got = 0;
  if (!ReadFile(h, buf, (DWORD)n, &got, NULL)) {
    DWORD e = GetLastError();
    return (e == ERROR_BROKEN_PIPE || e == ERROR_HANDLE_EOF) ? 0 : -1;
  }
  return (long)got;
}


long os_write (int fd, const void *buf, size_t n) {
  HANDLE h = (HANDLE)_get_osfhandle(fd);
  const char *p = (const char *)buf;
  size_t left = n;
  if (h == INVALID_HANDLE_VALUE) return -1;
  while (left > 0) {
    DWORD put = 0;
    if (!WriteFile(h, p, (DWORD)left, &put, NULL) || put == 0) {
      DWORD e = GetLastError();
      /* nobody reads the pipe any more: what SIGPIPE does elsewhere */
      if (os_pipe_exit && (e == ERROR_NO_DATA || e == ERROR_BROKEN_PIPE)) exit(128 + 13);
      return -1;
    }
    p += put;
    left -= put;
  }
  return (long)n;
}


int os_wait_readable (int fd, int ms) {
  HANDLE h;
  DWORD type;
  DWORD start = GetTickCount();
  if (fd < 0) {
    Sleep((DWORD)(ms > 0 ? ms : 0));
    return 0;
  }
  h = (HANDLE)_get_osfhandle(fd);
  type = GetFileType(h);
  if (type == FILE_TYPE_CHAR) {
    DWORD m;
    if (GetConsoleMode(h, &m)) return WaitForSingleObject(h, (DWORD)(ms < 0 ? INFINITE : ms)) == WAIT_OBJECT_0;
    return 1;
  }
  if (type != FILE_TYPE_PIPE) return 1;	/* files are always ready */
  for (;;) {
    DWORD avail = 0;
    if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL)) return 1;	/* EOF: ready */
    if (avail > 0) return 1;
    if (ms >= 0 && GetTickCount() - start >= (DWORD)ms) return 0;
    Sleep(10);
  }
}


int os_is_tty (int fd) {
  DWORD m;
  HANDLE h = (HANDLE)_get_osfhandle(fd);
  return GetConsoleMode(h, &m) != 0;
}


int os_tty_raw (int on) {
  DWORD m;
  if (!g_have_in) return -1;
  if (!on) return SetConsoleMode(g_hin, g_inmode) ? 0 : -1;
  m = g_inmode & ~(DWORD)(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT |
                          ENABLE_PROCESSED_INPUT | ENABLE_MOUSE_INPUT |
                          ENABLE_WINDOW_INPUT);
  m |= ENABLE_VIRTUAL_TERMINAL_INPUT;
  return SetConsoleMode(g_hin, m) ? 0 : -1;
}


/* next byte of UTF-8 typed at the console, -1 on end of input */
int os_tty_getbyte (void) {
  static unsigned char q[8];
  static int qn = 0, qi = 0;
  wchar_t wc[2];
  DWORD got = 0;
  int cnt = 1;
  if (qi < qn) return q[qi++];
  if (!ReadConsoleW(g_hin, &wc[0], 1, &got, NULL) || got == 0) return -1;
  if (wc[0] >= 0xD800 && wc[0] <= 0xDBFF) {	/* surrogate pair */
    if (ReadConsoleW(g_hin, &wc[1], 1, &got, NULL) && got == 1) cnt = 2;
  }
  qn = WideCharToMultiByte(CP_UTF8, 0, wc, cnt, (char *)q, (int)sizeof(q),
                           NULL, NULL);
  qi = 0;
  if (qn <= 0) {
    qn = 0;
    return '?';
  }
  if (q[0] == 26 && qn == 1) return -1;	/* Ctrl-Z in cooked mode */
  return q[qi++];
}


int os_term_cols (void) {
  CONSOLE_SCREEN_BUFFER_INFO info;
  if (!GetConsoleScreenBufferInfo(g_hout, &info)) return 80;
  return info.srWindow.Right - info.srWindow.Left + 1;
}


int os_term_rows (void) {
  CONSOLE_SCREEN_BUFFER_INFO info;
  if (!GetConsoleScreenBufferInfo(g_hout, &info)) return 24;
  return info.srWindow.Bottom - info.srWindow.Top + 1;
}


char *os_exe_path (const char *argv0) {
  wchar_t w[32768 / 8];
  DWORD n = GetModuleFileNameW(NULL, w, (DWORD)(sizeof(w) / sizeof(w[0])));
  if (n == 0 || n >= sizeof(w) / sizeof(w[0])) return xstrdup(argv0);
  return narrow(w);
}


char *os_hostname (void) {
  char *s = os_getenv("COMPUTERNAME");
  return s ? s : xstrdup("localhost");
}


char *os_username (void) {
  char *s = os_getenv("USERNAME");
  return (s && s[0]) ? s : xstrdup("user");
}


long os_getpid (void) {
  return (long)GetCurrentProcessId();
}


long os_getppid (void) {
  static long cached = -1;
  HANDLE snap;
  PROCESSENTRY32 pe;
  DWORD me = GetCurrentProcessId();
  if (cached >= 0) return cached;
  cached = 0;
  snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return 0;
  pe.dwSize = sizeof(pe);
  if (Process32First(snap, &pe)) {
    do {
      if (pe.th32ProcessID == me) {
        cached = (long)pe.th32ParentProcessID;
        break;
      }
    } while (Process32Next(snap, &pe));
  }
  CloseHandle(snap);
  return cached;
}


long os_getuid (void) { return 1000; }
long os_getgid (void) { return 1000; }
long os_geteuid (void) { return 1000; }


long long os_now_us (void) {
  FILETIME ft;
  unsigned long long t;
  GetSystemTimeAsFileTime(&ft);
  t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
  return (long long)(t / 10ULL) - 11644473600000000LL;
}


static double ft_seconds (FILETIME ft) {
  unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
  return (double)t / 1e7;
}


void os_times (double t[4]) {
  FILETIME c, e, k, u;
  t[0] = t[1] = t[2] = t[3] = 0.0;
  if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
    t[0] = ft_seconds(u);
    t[1] = ft_seconds(k);
  }
}


int os_umask (int mask) {
  int old = g_umask;
  if (mask >= 0) g_umask = mask & 0777;
  return old;
}


const char *os_type (void) { return "msys"; }


const char *os_machine (void) {
#if defined(__aarch64__) || defined(_M_ARM64)
  return "aarch64";
#else
  return "x86_64";
#endif
}


/* quoting rules of the Microsoft C runtime (and close enough for cmd) */
static void quote_arg (Buf *b, const char *a, int force, int batch) {
  const char *p;
  /* PortableGit's programs expand * ? [ { and ' in an unquoted argument
  ** themselves: those are quoted too, so they arrive as they are */
  const char *special = batch ? " \t\"&|<>^()%!;,=" : " \t\"*?[{'";
  if (!force && a[0] != '\0' && strpbrk(a, special) == NULL) {
    buf_puts(b, a);
    return;
  }
  buf_putc(b, '"');
  for (p = a;; p++) {
    size_t slashes = 0;
    while (*p == '\\') {
      slashes++;
      p++;
    }
    if (*p == '\0') {
      while (slashes-- > 0) buf_puts(b, "\\\\");
      break;
    }
    if (*p == '"') {
      while (slashes-- > 0) buf_puts(b, "\\\\");
      buf_puts(b, "\\\"");
    }
    else {
      while (slashes-- > 0) buf_putc(b, '\\');
      buf_putc(b, *p);
    }
  }
  buf_putc(b, '"');
}


/* environment for the child, with Linux style values made native */
static wchar_t *build_env_block (char **envp) {
  Vec env;
  size_t i, len = 0, cap = 0;
  wchar_t *out = NULL;
  vec_init(&env);
  if (envp == NULL) os_env_list(&env);
  else for (i = 0; envp[i] != NULL; i++) vec_push(&env, xstrdup(envp[i]));
  {	/* Windows wants the block sorted, case-insensitively */
    size_t a, b;
    for (a = 1; a < env.n; a++)
      for (b = a; b > 0 && m_stricmp(env.v[b - 1], env.v[b]) > 0; b--) {
        char *t = env.v[b];
        env.v[b] = env.v[b - 1];
        env.v[b - 1] = t;
      }
  }
  for (i = 0; i < env.n; i++) {
    char *entry = env.v[i], *made = NULL;
    char *eq = strchr(entry + 1, '=');
    wchar_t *w;
    size_t n;
    if (eq != NULL && eq[1] == '/') {
      char *conv = path_env_to_native(eq + 1);
      if (strcmp(conv, eq + 1) != 0) {
        *eq = '\0';
        made = xstrcat3(entry, "=", conv);
        *eq = '=';
        entry = made;
      }
      free(conv);
    }
    w = widen(entry);
    n = wcslen(w) + 1;
    if (len + n + 1 > cap) {
      cap = (len + n + 1) * 2;
      out = (wchar_t *)xrealloc(out, cap * sizeof(wchar_t));
    }
    memcpy(out + len, w, n * sizeof(wchar_t));
    len += n;
    free(w);
    free(made);
  }
  vec_free(&env);
  if (out == NULL) {	/* empty environment: two terminators */
    out = (wchar_t *)xmalloc(2 * sizeof(wchar_t));
    out[len++] = L'\0';
  }
  out[len] = L'\0';
  return out;
}


static HANDLE inheritable (int fd) {
  HANDLE me = GetCurrentProcess();
  HANDLE h, copy = NULL;
  if (fd < 0) return NULL;
  h = (HANDLE)_get_osfhandle(fd);
  if (h == INVALID_HANDLE_VALUE || h == (HANDLE)(intptr_t)-2) return NULL;
  if (!DuplicateHandle(me, h, me, &copy, 0, TRUE, DUPLICATE_SAME_ACCESS))
    return NULL;
  return copy;
}


int os_spawn (const char *exe, char **argv, char **envp, const int *fds,
              int nfds, OsProc *proc, long *pid) {
  STARTUPINFOW si;
  PROCESS_INFORMATION pi;
  Buf cl;
  char *app;
  wchar_t *wapp, *wcl, *wenv;
  int i, ok;
  int batch = has_ext(exe, ".cmd") || has_ext(exe, ".bat");
  buf_init(&cl);
  if (batch) {	/* batch files are run by the command interpreter */
    app = os_getenv("ComSpec");
    if (app == NULL) app = xstrdup("C:\\Windows\\System32\\cmd.exe");
    quote_arg(&cl, app, 0, 0);
    buf_puts(&cl, " /d /s /c \"");
    quote_arg(&cl, exe, 1, 1);
  }
  else {
    app = xstrdup(exe);
    quote_arg(&cl, exe, 0, 0);
  }
  for (i = 1; argv[i] != NULL; i++) {
    buf_putc(&cl, ' ');
    quote_arg(&cl, argv[i], 0, batch);
  }
  if (batch) buf_putc(&cl, '"');
  memset(&si, 0, sizeof(si));
  memset(&pi, 0, sizeof(pi));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = inheritable(nfds > 0 ? fds[0] : 0);
  si.hStdOutput = inheritable(nfds > 1 ? fds[1] : 1);
  si.hStdError = inheritable(nfds > 2 ? fds[2] : 2);
  wapp = widen(app);
  wcl = widen(cl.s);
  wenv = build_env_block(envp);
  ok = CreateProcessW(wapp, wcl, NULL, NULL, TRUE, CREATE_UNICODE_ENVIRONMENT,
                      wenv, NULL, &si, &pi) != 0;
  if (!ok)
    fd_printf(2, "mmc: %s: cannot execute (Windows error %lu)\n",
              argv[0], (unsigned long)GetLastError());
  if (si.hStdInput) CloseHandle(si.hStdInput);
  if (si.hStdOutput) CloseHandle(si.hStdOutput);
  if (si.hStdError) CloseHandle(si.hStdError);
  free(app);
  free(wapp);
  free(wcl);
  free(wenv);
  buf_free(&cl);
  if (!ok) return -1;
  CloseHandle(pi.hThread);
  *proc = (OsProc)pi.hProcess;
  *pid = (long)pi.dwProcessId;
  return 0;
}


int os_wait (OsProc proc) {
  HANDLE h = (HANDLE)proc;
  DWORD code = 1;
  WaitForSingleObject(h, INFINITE);
  GetExitCodeProcess(h, &code);
  CloseHandle(h);
  return (int)code;
}


int os_poll_proc (OsProc proc, int *status) {
  HANDLE h = (HANDLE)proc;
  DWORD code = 1;
  if (WaitForSingleObject(h, 0) != WAIT_OBJECT_0) return 0;
  GetExitCodeProcess(h, &code);
  CloseHandle(h);
  *status = (int)code;
  return 1;
}


void os_detach (OsProc proc) {
  CloseHandle((HANDLE)proc);
}


static int proc_pause (long pid, int stop);


int os_kill (long pid, int sig) {
  HANDLE h;
  int ok;
  if (sig == 19 || sig == 20 || sig == 21 || sig == 22) return proc_pause(pid, 1);	/* STOP TSTP TTIN TTOU */
  if (sig == 18) return proc_pause(pid, 0);	/* CONT */
  if (sig == 17 || sig == 23 || sig == 28) return 0;	/* CHLD URG WINCH: nothing to do */
  h = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
  if (h == NULL) return -1;
  if (sig == 0) {	/* only asks whether it exists */
    DWORD code = 0;
    ok = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return ok ? 0 : -1;
  }
  ok = TerminateProcess(h, (UINT)(128 + sig)) != 0;
  CloseHandle(h);
  return ok ? 0 : -1;
}


int os_exec (const char *exe, char **argv, char **envp) {
  (void)exe; (void)argv; (void)envp;
  return -1;
}

/*
** {==================================================================
** Job control. A console has no process groups and no Ctrl-Z for us
** (the key goes to the program as input), but a process can be stopped
** and let go on: kill -STOP / -CONT, fg and bg use that.
** ===================================================================
*/

typedef LONG (NTAPI *NtProcFn) (HANDLE);


/* stop (1) or let go on (0) every thread of a process */
static int proc_pause (long pid, int stop) {
  static NtProcFn suspend = NULL, resume = NULL;
  HANDLE h;
  LONG r;
  if (suspend == NULL) {
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    if (nt == NULL) return -1;
    suspend = (NtProcFn)(void (*)(void))GetProcAddress(nt, "NtSuspendProcess");
    resume = (NtProcFn)(void (*)(void))GetProcAddress(nt, "NtResumeProcess");
    if (suspend == NULL || resume == NULL) return -1;
  }
  h = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, (DWORD)pid);
  if (h == NULL) return -1;
  r = stop ? suspend(h) : resume(h);
  CloseHandle(h);
  return r >= 0 ? 0 : -1;
}


int os_job_control (int interactive) {
  (void)interactive;
  return 0;
}

int os_job_active (void) {
  return 0;
}

void os_job_pgid (long pgid) {
  (void)pgid;
}

void os_tty_give (long pgid) {
  (void)pgid;
}

int os_wait_fg (OsProc proc, int *stopped) {
  *stopped = 0;
  return os_wait(proc);
}

int os_suspend_self (void) {
  return -1;
}

/* }================================================================== */



int os_can_exec_replace (void) {
  return 0;
}


void os_catch_signal (int sig, int on) {
  if (sig == 2) g_int_mode = on;
}


typedef struct Tramp {
  OsThreadFn fn;
  void *arg;
} Tramp;

struct OsThread {
  HANDLE h;
};


static DWORD WINAPI thread_main (LPVOID p) {
  Tramp t = *(Tramp *)p;
  free(p);
  t.fn(t.arg);
  return 0;
}


OsThread *os_thread_start (OsThreadFn fn, void *arg) {
  Tramp *t = (Tramp *)xmalloc(sizeof(Tramp));
  OsThread *th = (OsThread *)xmalloc(sizeof(OsThread));
  t->fn = fn;
  t->arg = arg;
  th->h = CreateThread(NULL, 0, thread_main, t, 0, NULL);
  if (th->h == NULL) {
    free(t);
    free(th);
    return NULL;
  }
  return th;
}


void os_thread_join (OsThread *t) {
  if (t == NULL) return;
  WaitForSingleObject(t->h, INFINITE);
  CloseHandle(t->h);
  free(t);
}

/* }================================================================== */


/*
** {==================================================================
** Windows: for the tools (cp, rm, ls -l, df, ps ...)
** ===================================================================
*/

int os_rmdir (const char *native) {
  wchar_t *w = widen(native);
  int ok = RemoveDirectoryW(w) != 0;
  if (!ok) {
    fail();
    if (g_winerr == ERROR_DIRECTORY) g_err = OS_E_NOTDIR;
  }
  free(w);
  return ok ? 0 : -1;
}


int os_rename (const char *from, const char *to) {
  wchar_t *a = widen(from), *b = widen(to);
  int ok = MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING) != 0;
  if (!ok) fail();
  free(a);
  free(b);
  return ok ? 0 : -1;
}


int os_chmod (const char *native, unsigned mode) {
  wchar_t *w = widen(native);
  DWORD a = GetFileAttributesW(w);
  int ok = 1;
  if (a == INVALID_FILE_ATTRIBUTES) ok = 0;
  else if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
    DWORD na = (mode & 0200) ? (a & ~(DWORD)FILE_ATTRIBUTE_READONLY) : (a | FILE_ATTRIBUTE_READONLY);
    if (na == 0) na = FILE_ATTRIBUTE_NORMAL;
    if (na != a) ok = SetFileAttributesW(w, na) != 0;
  }
  if (!ok) fail();
  free(w);
  return ok ? 0 : -1;
}


static FILETIME to_ft (time_t t) {
  unsigned long long v = ((unsigned long long)t + 11644473600ULL) * 10000000ULL;
  FILETIME ft;
  ft.dwLowDateTime = (DWORD)v;
  ft.dwHighDateTime = (DWORD)(v >> 32);
  return ft;
}


int os_utime (const char *native, time_t atime, time_t mtime) {
  wchar_t *w = widen(native);
  HANDLE h = CreateFileW(w, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE |
                         FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  FILETIME a = to_ft(atime), m = to_ft(mtime);
  int ok;
  free(w);
  if (h == INVALID_HANDLE_VALUE) return fail();
  ok = SetFileTime(h, NULL, &a, &m) != 0;
  if (!ok) fail();
  CloseHandle(h);
  return ok ? 0 : -1;
}


int os_symlink (const char *target, const char *native, int is_dir) {
  wchar_t *t, *w = widen(native);
  char *tt = xstrdup(target), *p;
  int ok;
  for (p = tt; *p; p++)
    if (*p == '/') *p = '\\';
  t = widen(tt);
  /* 2: SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE (Developer Mode) */
  ok = CreateSymbolicLinkW(w, t, (is_dir ? 1 : 0) | 2) != 0;
  if (!ok && GetLastError() == ERROR_INVALID_PARAMETER)	/* older Windows 10 */
    ok = CreateSymbolicLinkW(w, t, is_dir ? 1 : 0) != 0;
  if (!ok) fail();
  free(tt);
  free(t);
  free(w);
  return ok ? 0 : -1;
}


int os_link (const char *from, const char *to) {
  wchar_t *a = widen(from), *b = widen(to);
  int ok = CreateHardLinkW(b, a, NULL) != 0;
  if (!ok) fail();
  free(a);
  free(b);
  return ok ? 0 : -1;
}


char *os_readlink (const char *native) {
  wchar_t *w = widen(native);
  HANDLE h = CreateFileW(w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                         NULL);
  unsigned char buf[16384];
  DWORD got = 0, tag;
  const unsigned char *pb;
  unsigned short sub_off, sub_len, pr_off, pr_len;
  wchar_t *name;
  size_t n;
  char *r, *p;
  free(w);
  if (h == INVALID_HANDLE_VALUE) {
    fail();
    return NULL;
  }
  if (!DeviceIoControl(h, 0x000900A8 /* FSCTL_GET_REPARSE_POINT */, NULL, 0, buf,
                       sizeof(buf), &got, NULL) || got < 16) {
    fail();
    CloseHandle(h);
    g_err = OS_E_INVAL;
    return NULL;
  }
  CloseHandle(h);
  memcpy(&tag, buf, 4);
  memcpy(&sub_off, buf + 8, 2);
  memcpy(&sub_len, buf + 10, 2);
  memcpy(&pr_off, buf + 12, 2);
  memcpy(&pr_len, buf + 14, 2);
  if (tag == TAG_SYMLINK) pb = buf + 20;
  else if (tag == TAG_MOUNT_POINT) pb = buf + 16;
  else {
    g_err = OS_E_INVAL;
    return NULL;
  }
  if (pr_len == 0) {	/* no print name: the substitute one, without \??\ */
    pr_off = sub_off;
    pr_len = sub_len;
  }
  if ((size_t)(pb - buf) + pr_off + pr_len > got) {
    g_err = OS_E_INVAL;
    return NULL;
  }
  n = pr_len / 2;
  name = (wchar_t *)xmalloc((n + 1) * sizeof(wchar_t));
  memcpy(name, pb + pr_off, n * sizeof(wchar_t));
  name[n] = 0;
  r = narrow(name);
  free(name);
  if (strncmp(r, "\\??\\", 4) == 0) memmove(r, r + 4, strlen(r + 4) + 1);
  if (tag == TAG_SYMLINK && strchr(r, ':') == NULL)	/* relative: Linux style */
    for (p = r; *p; p++)
      if (*p == '\\') *p = '/';
  return r;
}


static int file_id (const char *native, DWORD *vol, DWORD *hi, DWORD *lo) {
  wchar_t *w = widen(native);
  HANDLE h = CreateFileW(w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  BY_HANDLE_FILE_INFORMATION fi;
  int ok;
  free(w);
  if (h == INVALID_HANDLE_VALUE) return -1;
  ok = GetFileInformationByHandle(h, &fi) != 0;
  CloseHandle(h);
  if (!ok) return -1;
  *vol = fi.dwVolumeSerialNumber;
  *hi = fi.nFileIndexHigh;
  *lo = fi.nFileIndexLow;
  return 0;
}


int os_same_file (const char *a, const char *b) {
  DWORD v1, h1, l1, v2, h2, l2;
  if (file_id(a, &v1, &h1, &l1) != 0 || file_id(b, &v2, &h2, &l2) != 0) return 0;
  return v1 == v2 && h1 == h2 && l1 == l2;
}


long long os_seek (int fd, long long off, int whence) {
  long long r;
  if (GetFileType((HANDLE)_get_osfhandle(fd)) != FILE_TYPE_DISK) {	/* pipes "seek" too */
    g_err = OS_E_INVAL;
    g_winerr = 0;
    return -1;
  }
  r = _lseeki64(fd, off, whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END);
  if (r < 0) fail_errno();
  return r;
}


void os_sleep_ms (int ms) {
  Sleep((DWORD)(ms > 0 ? ms : 0));
}


int os_diskfree (const char *native, unsigned long long *total,
                 unsigned long long *avail, unsigned long long *free_) {
  ULARGE_INTEGER a, t, f;
  wchar_t *w = widen(native);
  int ok = GetDiskFreeSpaceExW(w, &a, &t, &f) != 0;
  free(w);
  if (!ok) return fail();
  *total = t.QuadPart;
  *avail = a.QuadPart;
  *free_ = f.QuadPart;
  return 0;
}


void os_mounts (Vec *out) {
  wchar_t drives[512], *d;
  DWORD n = GetLogicalDriveStringsW(511, drives);
  if (n == 0 || n > 511) return;
  for (d = drives; *d; d += wcslen(d) + 1) {
    UINT type = GetDriveTypeW(d);
    wchar_t fs[64];
    char *root, *fsn, *line;
    if (type == DRIVE_NO_ROOT_DIR || type == DRIVE_UNKNOWN) continue;
    fs[0] = 0;
    if (!GetVolumeInformationW(d, NULL, 0, NULL, NULL, NULL, fs, 64)) continue;
    root = narrow(d);
    fsn = narrow(fs);
    line = (char *)xmalloc(strlen(root) * 2 + strlen(fsn) + 4);
    sprintf(line, "%.2s\t%s\t%s", root, root, fsn);
    vec_push(out, line);
    free(root);
    free(fsn);
  }
}


void os_uname (OsUname *u) {
  typedef LONG (WINAPI *RtlGetVersionFn) (OSVERSIONINFOW *);
  OSVERSIONINFOW vi;
  RtlGetVersionFn fn = (RtlGetVersionFn)(void (*) (void))GetProcAddress(
                         GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
  memset(u, 0, sizeof(*u));
  memset(&vi, 0, sizeof(vi));
  vi.dwOSVersionInfoSize = sizeof(vi);
  if (fn == NULL || fn(&vi) != 0) {
    vi.dwMajorVersion = 10;
    vi.dwMinorVersion = 0;
  }
  /* git-bash says MINGW64_NT-10.0-26200: scripts look for MINGW* / MSYS* */
  snprintf(u->sysname, sizeof(u->sysname), "MINGW64_NT-%lu.%lu-%lu",
           (unsigned long)vi.dwMajorVersion, (unsigned long)vi.dwMinorVersion,
           (unsigned long)vi.dwBuildNumber);
  snprintf(u->release, sizeof(u->release), "%lu.%lu.%lu", (unsigned long)vi.dwMajorVersion,
           (unsigned long)vi.dwMinorVersion, (unsigned long)vi.dwBuildNumber);
  snprintf(u->version, sizeof(u->version), "Windows %s build %lu",
           vi.dwBuildNumber >= 22000 ? "11" : vi.dwMajorVersion >= 10 ? "10" : "NT",
           (unsigned long)vi.dwBuildNumber);
  snprintf(u->machine, sizeof(u->machine), "%s", os_machine());
  snprintf(u->os, sizeof(u->os), "Msys");
}


int os_proclist (OsProcInfo **out, size_t *count) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  PROCESSENTRY32W pe;
  OsProcInfo *v = NULL;
  size_t n = 0, cap = 0;
  *out = NULL;
  *count = 0;
  if (snap == INVALID_HANDLE_VALUE) return fail();
  pe.dwSize = sizeof(pe);
  if (Process32FirstW(snap, &pe)) {
    do {
      OsProcInfo *p;
      HANDLE h;
      if (n == cap) {
        cap = cap ? cap * 2 : 128;
        v = (OsProcInfo *)xrealloc(v, cap * sizeof(OsProcInfo));
      }
      p = &v[n++];
      memset(p, 0, sizeof(*p));
      p->pid = (long)pe.th32ProcessID;
      p->ppid = (long)pe.th32ParentProcessID;
      p->uid = 1000;
      p->state = 'S';
      p->name = narrow(pe.szExeFile);
      h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
      if (h != NULL) {
        FILETIME c, e, k, us;
        PROCESS_MEMORY_COUNTERS pm;
        wchar_t path[1024];
        DWORD len = 1024;
        if (GetProcessTimes(h, &c, &e, &k, &us)) {
          p->cpu = ft_seconds(k) + ft_seconds(us);
          p->start = ft_time(c);
        }
        memset(&pm, 0, sizeof(pm));
        pm.cb = sizeof(pm);
        if (GetProcessMemoryInfo(h, &pm, sizeof(pm))) {
          p->rss = (long long)pm.WorkingSetSize;
          p->vsz = (long long)pm.PagefileUsage;
        }
        if (QueryFullProcessImageNameW(h, 0, path, &len)) p->cmd = narrow(path);
        CloseHandle(h);
      }
      else p->uid = 0;	/* not ours to look at: the system's */
      if (p->cmd == NULL) p->cmd = xstrdup(p->name);
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
  *out = v;
  *count = n;
  return 0;
}


unsigned long long os_memtotal (void) {
  MEMORYSTATUSEX m;
  m.dwLength = sizeof(m);
  if (!GlobalMemoryStatusEx(&m)) return 0;
  return m.ullTotalPhys;
}


char *os_user_name (long uid) {
  char num[24];
  if (uid == 1000) return os_username();
  if (uid == 0) return xstrdup("SYSTEM");
  return xstrdup(ll_to_str(uid, num));
}


char *os_group_name (long gid) {
  char num[24];
  if (gid == 1000) return xstrdup("None");
  return xstrdup(ll_to_str(gid, num));
}


int os_is_system_program (const char *native) {
  char *root = os_getenv("SystemRoot");
  size_t n;
  int r;
  if (root == NULL || root[0] == '\0') {
    free(root);
    root = xstrdup("C:\\Windows");
  }
  n = strlen(root);
  r = m_strnicmp(native, root, n) == 0 && (native[n] == '\\' || native[n] == '/');
  free(root);
  return r;
}

/* }================================================================== */


#else

/*
** {==================================================================
** Linux, macOS, Android
** ===================================================================
*/

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <grp.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/mount.h>
#include <sys/sysctl.h>
#endif

/* job control, see below */
static int g_jobctl;
static pid_t g_orig_pgrp, g_shell_pgrp;
static long g_spawn_pgid;

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

extern char **environ;

static struct termios g_termios;
static int g_have_termios = 0;


static void on_signal (int sig) {
  if (sig > 0 && sig < 65) os_pending[sig] = 1;
  if (sig == SIGINT) os_interrupted = 1;
}


static void set_handler (int sig, void (*fn) (int)) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = fn;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;	/* no SA_RESTART: a read waiting on a pipe returns */
  sigaction(sig, &sa, NULL);
}


void os_init (void) {
  set_handler(SIGINT, on_signal);	/* Ctrl-C: the child dies, we unwind */
  signal(SIGQUIT, SIG_IGN);
  signal(SIGTSTP, SIG_IGN);
  signal(SIGTTOU, SIG_IGN);
  signal(SIGPIPE, SIG_IGN);
  g_have_termios = (tcgetattr(0, &g_termios) == 0);
}


void os_shutdown (void) {
  os_tty_fix();
  if (g_jobctl && g_orig_pgrp > 0) tcsetpgrp(0, g_orig_pgrp);	/* the terminal goes back */
}


void os_tty_fix (void) {
  if (g_have_termios) tcsetattr(0, TCSADRAIN, &g_termios);
}


void os_args (int *argc, char ***argv) {
  (void)argc;
  (void)argv;
}


char *os_getenv (const char *name) {
  const char *v = getenv(name);
  return v ? xstrdup(v) : NULL;
}


void os_setenv (const char *name, const char *value) {
  if (value) setenv(name, value, 1);
  else unsetenv(name);
}


void os_env_list (Vec *out) {
  char **e;
  for (e = environ; e && *e; e++) vec_push(out, xstrdup(*e));
}


char *os_getcwd (void) {
  size_t cap = 256;
  for (;;) {
    char *b = (char *)xmalloc(cap);
    if (getcwd(b, cap) != NULL) return b;
    free(b);
    if (errno != ERANGE) return xstrdup(".");
    cap *= 2;
  }
}


int os_chdir (const char *native) {
  return chdir(native);
}


static void fill_stat (OsStat *st, const struct stat *s) {
  st->exists = 1;
  st->is_dir = S_ISDIR(s->st_mode);
  st->is_link = S_ISLNK(s->st_mode);
  st->is_fifo = S_ISFIFO(s->st_mode);
  st->is_sock = S_ISSOCK(s->st_mode);
  st->is_chr = S_ISCHR(s->st_mode);
  st->is_blk = S_ISBLK(s->st_mode);
  st->mode = (unsigned)(s->st_mode & 07777);
  st->size = (long long)s->st_size;
  st->mtime = s->st_mtime;
  st->atime = s->st_atime;
  st->ctime = s->st_ctime;
  st->blocks = (long long)s->st_blocks;
  st->nlink = (unsigned long)s->st_nlink;
  st->dev = (unsigned long long)s->st_dev;
  st->ino = (unsigned long long)s->st_ino;
  st->uid = (long)s->st_uid;
  st->gid = (long)s->st_gid;
}


int os_stat (const char *native, OsStat *st) {
  struct stat s;
  memset(st, 0, sizeof(*st));
  if (stat(native, &s) != 0) return -1;
  fill_stat(st, &s);
  return 0;
}


int os_lstat (const char *native, OsStat *st) {
  struct stat s;
  memset(st, 0, sizeof(*st));
  if (lstat(native, &s) != 0) return -1;
  fill_stat(st, &s);
  return 0;
}


int os_is_exec (const char *native) {
  OsStat st;
  return os_stat(native, &st) == 0 && !st.is_dir && access(native, X_OK) == 0;
}


int os_access (const char *native, int what) {
  return access(native, what == 'r' ? R_OK : what == 'w' ? W_OK : X_OK) == 0;
}


int os_listdir (const char *native, Vec *out) {
  DIR *d = opendir(native[0] ? native : ".");
  struct dirent *e;
  if (d == NULL) return -1;
  while ((e = readdir(d)) != NULL) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    vec_push(out, xstrdup(e->d_name));
  }
  closedir(d);
  return 0;
}


int os_mkdir (const char *native) {
  return mkdir(native, 0777);
}


int os_unlink (const char *native) {
  return unlink(native);
}


char *os_realpath (const char *native) {
  return realpath(native, NULL);
}


int os_open (const char *native, int mode) {
  int flags = O_CLOEXEC;
  if (mode == OS_READ) flags |= O_RDONLY;
  else if (mode == OS_WRITE) flags |= O_WRONLY | O_CREAT | O_TRUNC;
  else if (mode == OS_RDWR) flags |= O_RDWR | O_CREAT;
  else if (mode == OS_EXCL) flags |= O_WRONLY | O_CREAT | O_EXCL;
  else flags |= O_WRONLY | O_CREAT | O_APPEND;
  return open(native, flags, 0666);
}


int os_pipe (int fds[2]) {
  if (pipe(fds) != 0) return -1;
  fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  fcntl(fds[1], F_SETFD, FD_CLOEXEC);
  return 0;
}


int os_dup (int fd) {
  int r = fcntl(fd, F_DUPFD_CLOEXEC, 3);
  return r;
}


void os_close (int fd) {
  if (fd >= 0) close(fd);
}


int os_fd_valid (int fd) {
  return fcntl(fd, F_GETFD) != -1;
}


long os_read (int fd, void *buf, size_t n) {
  for (;;) {
    ssize_t r = read(fd, buf, n);
    if (r < 0 && errno == EINTR) {
      if (os_interrupted) return -1;
      continue;
    }
    return (long)r;
  }
}


long os_write (int fd, const void *buf, size_t n) {
  const char *p = (const char *)buf;
  size_t left = n;
  while (left > 0) {
    ssize_t r = write(fd, p, left);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) return -1;
    p += r;
    left -= (size_t)r;
  }
  return (long)n;
}


int os_wait_readable (int fd, int ms) {
  struct pollfd p;
  int r;
  if (fd < 0) {
    if (ms > 0) usleep((useconds_t)ms * 1000);
    return 0;
  }
  p.fd = fd;
  p.events = POLLIN;
  p.revents = 0;
  do r = poll(&p, 1, ms); while (r < 0 && errno == EINTR && !os_interrupted);
  return r > 0 ? 1 : (r == 0 ? 0 : -1);
}


int os_is_tty (int fd) {
  return isatty(fd);
}


int os_tty_raw (int on) {
  struct termios t;
  if (!g_have_termios) return -1;
  if (!on) return tcsetattr(0, TCSADRAIN, &g_termios);
  t = g_termios;
  t.c_iflag &= ~(tcflag_t)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  t.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN | ISIG);
  t.c_cc[VMIN] = 1;
  t.c_cc[VTIME] = 0;
  return tcsetattr(0, TCSADRAIN, &t);
}


int os_tty_getbyte (void) {
  unsigned char c;
  return (os_read(0, &c, 1) == 1) ? c : -1;
}


int os_term_cols (void) {
#ifdef TIOCGWINSZ
  struct winsize ws;
  if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
#endif
  return 80;
}


int os_term_rows (void) {
#ifdef TIOCGWINSZ
  struct winsize ws;
  if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) return ws.ws_row;
#endif
  return 24;
}


char *os_exe_path (const char *argv0) {
  char buf[4096];
  char *r;
#if defined(__APPLE__)
  uint32_t size = (uint32_t)sizeof(buf);
  if (_NSGetExecutablePath(buf, &size) == 0 && (r = realpath(buf, NULL)) != NULL)
    return r;
#else
  ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n > 0) {
    buf[n] = '\0';
    return xstrdup(buf);
  }
#endif
  if (strchr(argv0, '/') != NULL && (r = realpath(argv0, NULL)) != NULL)
    return r;
  {	/* started by name: look it up in PATH */
    Vec dirs;
    size_t i;
    char *path = os_getenv("PATH");
    vec_init(&dirs);
    if (path) path_list_split(path, &dirs);
    free(path);
    for (i = 0; i < dirs.n; i++) {
      char *cand = path_join(dirs.v[i], argv0);
      if (os_is_exec(cand) && (r = realpath(cand, NULL)) != NULL) {
        free(cand);
        vec_free(&dirs);
        return r;
      }
      free(cand);
    }
    vec_free(&dirs);
  }
  return xstrdup(argv0);
}


char *os_hostname (void) {
  char buf[256];
  if (gethostname(buf, sizeof(buf)) != 0) return xstrdup("localhost");
  buf[sizeof(buf) - 1] = '\0';
  buf[strcspn(buf, ".")] = '\0';
  return xstrdup(buf);
}


char *os_username (void) {
  char *s = os_getenv("USER");
  if (s == NULL || s[0] == '\0') {
    free(s);
    s = os_getenv("LOGNAME");
  }
  return (s && s[0]) ? s : xstrdup("user");
}


long os_getpid (void) { return (long)getpid(); }
long os_getppid (void) { return (long)getppid(); }
long os_getuid (void) { return (long)getuid(); }
long os_getgid (void) { return (long)getgid(); }
long os_geteuid (void) { return (long)geteuid(); }


long long os_now_us (void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (long long)tv.tv_sec * 1000000LL + tv.tv_usec;
}


void os_times (double t[4]) {
  struct tms tm;
  double hz = (double)sysconf(_SC_CLK_TCK);
  if (hz <= 0) hz = 100;
  times(&tm);
  t[0] = (double)tm.tms_utime / hz;
  t[1] = (double)tm.tms_stime / hz;
  t[2] = (double)tm.tms_cutime / hz;
  t[3] = (double)tm.tms_cstime / hz;
}


int os_umask (int mask) {
  mode_t old;
  if (mask < 0) {
    old = umask(022);
    umask(old);
    return (int)old;
  }
  return (int)umask((mode_t)mask);
}


const char *os_type (void) {
#if defined(__ANDROID__)
  return "linux-android";
#elif defined(__APPLE__)
  return "darwin";
#elif defined(__linux__)
  return "linux-gnu";
#else
  return "unix";
#endif
}


const char *os_machine (void) {
#if defined(__aarch64__)
  return "aarch64";
#elif defined(__arm__)
  return "arm";
#elif defined(__x86_64__)
  return "x86_64";
#elif defined(__i386__)
  return "i686";
#else
  return "unknown";
#endif
}


int os_spawn (const char *exe, char **argv, char **envp, const int *fds,
              int nfds, OsProc *proc, long *pid) {
  pid_t p = fork();
  if (p < 0) {
    fd_printf(2, "mmc: %s: cannot fork: %s\n", argv[0], strerror(errno));
    return -1;
  }
  if (p == 0) {	/* child */
    int tmp[MMC_FDS], k;
    if (g_spawn_pgid >= 0) setpgid(0, (pid_t)g_spawn_pgid);
    signal(SIGTTIN, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
    if (nfds > MMC_FDS) nfds = MMC_FDS;
    /* move everything out of the way first, then into place */
    for (k = 0; k < nfds; k++) tmp[k] = (fds[k] >= 0) ? fcntl(fds[k], F_DUPFD, 100) : -1;
    for (k = 0; k < nfds; k++) {
      if (tmp[k] >= 0) dup2(tmp[k], k);
      else close(k);
    }
    for (k = 0; k < nfds; k++)
      if (tmp[k] >= 0) close(tmp[k]);
    execve(exe, argv, envp ? envp : environ);
    fd_printf(2, "mmc: %s: cannot execute: %s\n", argv[0], strerror(errno));
    _exit(errno == ENOENT ? 127 : 126);
  }
  if (g_spawn_pgid >= 0) setpgid(p, g_spawn_pgid > 0 ? (pid_t)g_spawn_pgid : p);
  *proc = (OsProc)p;
  *pid = (long)p;
  return 0;
}


static int decode (int st) {
  if (WIFEXITED(st)) return WEXITSTATUS(st);
  if (WIFSIGNALED(st)) {
    if (WTERMSIG(st) == SIGINT) os_write(2, "\n", 1);
    return 128 + WTERMSIG(st);
  }
  return 1;
}


int os_wait (OsProc proc) {
  int st = 0;
  for (;;) {
    pid_t r = waitpid((pid_t)proc, &st, WUNTRACED);
    if (r < 0 && errno == EINTR) continue;
    if (r < 0) return 1;
    if (WIFSTOPPED(st)) {	/* no job control: Ctrl-Z must not hang us */
      kill((pid_t)proc, SIGCONT);
      continue;
    }
    break;
  }
  return decode(st);
}


int os_poll_proc (OsProc proc, int *status) {
  int st = 0;
  pid_t r = waitpid((pid_t)proc, &st, WNOHANG | WUNTRACED);
  if (r <= 0) return r < 0 ? (*status = 127, 1) : 0;
  if (WIFSTOPPED(st)) {	/* a background job wanted the terminal, or kill -STOP */
    *status = 128 + WSTOPSIG(st);
    return 2;
  }
  *status = decode(st);
  return 1;
}


void os_detach (OsProc proc) {
  (void)proc;
}


/* the shell speaks Linux signal numbers (kill -l); macOS has others */
static int native_sig (int sig) {
  switch (sig) {
    case 7: return SIGBUS;
    case 10: return SIGUSR1;
    case 12: return SIGUSR2;
    case 17: return SIGCHLD;
    case 18: return SIGCONT;
    case 19: return SIGSTOP;
    case 20: return SIGTSTP;
    case 21: return SIGTTIN;
    case 22: return SIGTTOU;
    case 23: return SIGURG;
    case 28: return SIGWINCH;
    default: return sig;
  }
}


int os_kill (long pid, int sig) {
  return kill((pid_t)pid, native_sig(sig));
}

/*
** {==================================================================
** Job control: an interactive shell on a terminal has a process group
** of its own; every job gets one too, and the terminal while it runs in
** the foreground. Ctrl-Z stops it; fg and bg let it go on.
** ===================================================================
*/

static int g_jobctl = 0;
static pid_t g_orig_pgrp = -1, g_shell_pgrp = -1;
static long g_spawn_pgid = -1;	/* -1: children stay in our group */


int os_job_control (int interactive) {
  int tries;
  if (!interactive) {	/* a script stops on Ctrl-Z like any other program */
    signal(SIGTSTP, SIG_DFL);
    return 0;
  }
  if (!isatty(0)) return 0;
  for (tries = 0; tries < 20 && tcgetpgrp(0) != getpgrp(); tries++)
    kill(-getpgrp(), SIGTTIN);	/* started in the background: wait to be in front */
  if (tcgetpgrp(0) != getpgrp()) return 0;
  signal(SIGTTIN, SIG_IGN);
  g_orig_pgrp = getpgrp();
  if (getsid(0) != getpid()) setpgid(0, 0);	/* a group of our own */
  g_shell_pgrp = getpgrp();
  if (tcsetpgrp(0, g_shell_pgrp) != 0) return 0;
  g_jobctl = 1;
  return 1;
}


int os_job_active (void) {
  return g_jobctl;
}


void os_job_pgid (long pgid) {
  g_spawn_pgid = g_jobctl ? pgid : -1;
}


void os_tty_give (long pgid) {
  if (g_jobctl) tcsetpgrp(0, pgid > 0 ? (pid_t)pgid : g_shell_pgrp);
}


int os_wait_fg (OsProc proc, int *stopped) {
  int st = 0;
  *stopped = 0;
  for (;;) {
    pid_t r = waitpid((pid_t)proc, &st, WUNTRACED);
    if (r < 0 && errno == EINTR) continue;
    if (r < 0) return 1;
    if (WIFSTOPPED(st)) {
      if (g_jobctl) {	/* Ctrl-Z: it becomes a stopped job */
        *stopped = 1;
        return 128 + WSTOPSIG(st);
      }
      kill((pid_t)proc, SIGCONT);	/* no job control: it must not hang us */
      continue;
    }
    break;
  }
  return decode(st);
}


int os_suspend_self (void) {
  kill(getpid(), SIGSTOP);	/* whoever started us gets the terminal back */
  if (g_jobctl) tcsetpgrp(0, g_shell_pgrp);	/* and we take it again */
  return 0;
}

/* }================================================================== */



int os_exec (const char *exe, char **argv, char **envp) {
  os_tty_fix();
  signal(SIGINT, SIG_DFL);
  signal(SIGQUIT, SIG_DFL);
  signal(SIGTSTP, SIG_DFL);
  signal(SIGPIPE, SIG_DFL);
  execve(exe, argv, envp ? envp : environ);
  return -1;
}


int os_can_exec_replace (void) {
  return 1;
}


void os_catch_signal (int sig, int on) {
  if (sig <= 0 || sig >= 65 || sig == SIGKILL || sig == SIGSTOP) return;
  if (on == 1) set_handler(sig, on_signal);
  else if (on == 2) signal(sig, SIG_IGN);
  else if (sig == SIGINT) set_handler(sig, on_signal);	/* the shell keeps its own */
  else if (sig == SIGQUIT || sig == SIGTSTP || sig == SIGTTOU || sig == SIGPIPE)
    signal(sig, SIG_IGN);
  else signal(sig, SIG_DFL);
}


typedef struct Tramp {
  OsThreadFn fn;
  void *arg;
} Tramp;

struct OsThread {
  pthread_t t;
};


static void *thread_main (void *p) {
  Tramp t = *(Tramp *)p;
  free(p);
  t.fn(t.arg);
  return NULL;
}


OsThread *os_thread_start (OsThreadFn fn, void *arg) {
  Tramp *t = (Tramp *)xmalloc(sizeof(Tramp));
  OsThread *th = (OsThread *)xmalloc(sizeof(OsThread));
  t->fn = fn;
  t->arg = arg;
  if (pthread_create(&th->t, NULL, thread_main, t) != 0) {
    free(t);
    free(th);
    return NULL;
  }
  return th;
}


void os_thread_join (OsThread *t) {
  if (t == NULL) return;
  pthread_join(t->t, NULL);
  free(t);
}


/*
** {==================================================================
** Named pipes for <( ) and >( ): a FIFO in the temporary folder; a
** thread of ours copies between it and an ordinary pipe the other
** command has, so the data streams as it is made
** ===================================================================
*/

struct OsNPipe {
  char *path;
  int fd;	/* our end of the ordinary pipe */
  int to_reader;	/* 1: fd -> the program that opens it; 0: the other way */
  volatile int connected;
  int refs;
  pthread_mutex_t mu;
  OsThread *th;
};


static void npipe_unref (OsNPipe *np) {
  int left;
  pthread_mutex_lock(&np->mu);
  left = --np->refs;
  pthread_mutex_unlock(&np->mu);
  if (left > 0) return;
  unlink(np->path);
  free(np->path);
  pthread_mutex_destroy(&np->mu);
  free(np);
}


static void npipe_relay (void *arg) {
  OsNPipe *np = (OsNPipe *)arg;
  char buf[16384];
  int f = open(np->path, np->to_reader ? O_WRONLY : O_RDONLY);	/* waits for the program */
  np->connected = 1;
  if (f >= 0) {
    for (;;) {
      int from = np->to_reader ? np->fd : f, to = np->to_reader ? f : np->fd;
      ssize_t got = read(from, buf, sizeof(buf)), off = 0;
      if (got < 0 && errno == EINTR) continue;
      if (got <= 0) break;
      while (off < got) {
        ssize_t put = write(to, buf + off, (size_t)(got - off));
        if (put < 0 && errno == EINTR) continue;
        if (put <= 0) break;
        off += put;
      }
      if (off < got) break;	/* the other side went away (SIGPIPE is ignored) */
    }
    close(f);
  }
  close(np->fd);
  npipe_unref(np);
}


OsNPipe *os_npipe_new (const char *dir, int fd, int to_reader, char **path) {
  static int counter = 0;
  char name[64];
  OsNPipe *np = (OsNPipe *)xmalloc(sizeof(OsNPipe));
  memset(np, 0, sizeof(*np));
  sprintf(name, "mmc-ps-%ld-%d", (long)getpid(), ++counter);
  np->path = path_join(dir, name);
  unlink(np->path);
  if (mkfifo(np->path, 0600) != 0) {
    free(np->path);
    free(np);
    return NULL;
  }
  np->fd = fd;
  np->to_reader = to_reader;
  np->refs = 2;	/* the thread and the caller */
  pthread_mutex_init(&np->mu, NULL);
  np->th = os_thread_start(npipe_relay, np);
  if (np->th == NULL) {
    unlink(np->path);
    free(np->path);
    pthread_mutex_destroy(&np->mu);
    free(np);
    return NULL;
  }
  *path = xstrdup(np->path);
  return np;
}


/* the command is done. Nobody opened it: we do, so the thread ends. The
** data for a >( ) reader is all through when this returns. */
void os_npipe_end (OsNPipe *np) {
  int tries;
  for (tries = 0; !np->connected && tries < 200; tries++) {
    int f = open(np->path, (np->to_reader ? O_RDONLY : O_WRONLY) | O_NONBLOCK);
    if (f >= 0) {
      close(f);
      break;
    }
    usleep(5000);	/* the thread is not in its open() yet */
  }
  if (np->to_reader) {	/* a <( ) writer may go on for ever: let it */
    pthread_detach(np->th->t);
    free(np->th);
  }
  else os_thread_join(np->th);
  npipe_unref(np);
}

/* }================================================================== */


/*
** {==================================================================
** POSIX: for the tools (cp, rm, ls -l, df, ps ...)
** ===================================================================
*/

int os_errcode (void) {
  switch (errno) {
    case ENOENT: return OS_E_NOENT;
    case EACCES: return OS_E_ACCES;
    case EEXIST: return OS_E_EXIST;
    case ENOTEMPTY: return OS_E_NOTEMPTY;
    case ENOTDIR: return OS_E_NOTDIR;
    case EISDIR: return OS_E_ISDIR;
    case EXDEV: return OS_E_XDEV;
    case EBUSY: return OS_E_BUSY;
    case ENOSPC: return OS_E_NOSPC;
    case EPERM: return OS_E_PERM;
    case EINVAL: return OS_E_INVAL;
    case ELOOP: return OS_E_LOOP;
  }
  return OS_E_OTHER;
}


const char *os_errmsg (void) {
  return strerror(errno);
}


int os_rmdir (const char *native) { return rmdir(native); }
int os_rename (const char *from, const char *to) { return rename(from, to); }
int os_chmod (const char *native, unsigned mode) { return chmod(native, (mode_t)mode); }
int os_link (const char *from, const char *to) { return link(from, to); }


int os_utime (const char *native, time_t atime, time_t mtime) {
  struct timeval tv[2];
  tv[0].tv_sec = atime;
  tv[0].tv_usec = 0;
  tv[1].tv_sec = mtime;
  tv[1].tv_usec = 0;
  return utimes(native, tv);
}


int os_symlink (const char *target, const char *native, int is_dir) {
  (void)is_dir;
  return symlink(target, native);
}


char *os_readlink (const char *native) {
  size_t cap = 256;
  for (;;) {
    char *b = (char *)xmalloc(cap);
    ssize_t n = readlink(native, b, cap);
    if (n < 0) {
      int e = errno;
      free(b);
      errno = e;
      return NULL;
    }
    if ((size_t)n < cap) {
      b[n] = '\0';
      return b;
    }
    free(b);
    cap *= 2;
  }
}


int os_same_file (const char *a, const char *b) {
  struct stat x, y;
  if (stat(a, &x) != 0 || stat(b, &y) != 0) return 0;
  return x.st_dev == y.st_dev && x.st_ino == y.st_ino;
}


long long os_seek (int fd, long long off, int whence) {
  return (long long)lseek(fd, (off_t)off, whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END);
}


void os_sleep_ms (int ms) {
  struct timespec ts;
  if (ms <= 0) return;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  nanosleep(&ts, NULL);
}


int os_diskfree (const char *native, unsigned long long *total,
                 unsigned long long *avail, unsigned long long *free_) {
  struct statvfs v;
  unsigned long long bs;
  if (statvfs(native, &v) != 0) return -1;
  bs = v.f_frsize ? (unsigned long long)v.f_frsize : (unsigned long long)v.f_bsize;
  *total = (unsigned long long)v.f_blocks * bs;
  *avail = (unsigned long long)v.f_bavail * bs;
  *free_ = (unsigned long long)v.f_bfree * bs;
  return 0;
}


void os_mounts (Vec *out) {
#ifdef __APPLE__
  struct statfs *m;
  int i, n = getmntinfo(&m, MNT_NOWAIT);
  for (i = 0; i < n; i++) {
    Buf b;
    buf_init(&b);
    buf_printf(&b, "%s\t%s\t%s", m[i].f_mntfromname, m[i].f_mntonname, m[i].f_fstypename);
    vec_push(out, buf_take(&b));
  }
#else
  size_t len;
  char *text = read_file("/proc/mounts", &len), *line, *next;
  if (text == NULL) return;
  for (line = text; line && *line; line = next) {
    char dev[512], dir[512], type[64];
    next = strchr(line, '\n');
    if (next) *next++ = '\0';
    if (sscanf(line, "%511s %511s %63s", dev, dir, type) == 3) {
      Buf b;
      char *p, *q;
      for (p = q = dir; *p; p++) {	/* \040 is a space */
        if (p[0] == '\\' && p[1] >= '0' && p[1] <= '3' && p[2] && p[3]) {
          *q++ = (char)(((p[1] - '0') << 6) | ((p[2] - '0') << 3) | (p[3] - '0'));
          p += 3;
        }
        else *q++ = *p;
      }
      *q = '\0';
      buf_init(&b);
      buf_printf(&b, "%s\t%s\t%s", dev, dir, type);
      vec_push(out, buf_take(&b));
    }
  }
  free(text);
#endif
}


void os_uname (OsUname *u) {
  struct utsname n;
  memset(u, 0, sizeof(*u));
  if (uname(&n) == 0) {
    snprintf(u->sysname, sizeof(u->sysname), "%s", n.sysname);
    snprintf(u->release, sizeof(u->release), "%s", n.release);
    snprintf(u->version, sizeof(u->version), "%s", n.version);
    snprintf(u->machine, sizeof(u->machine), "%s", n.machine);
  }
#if defined(__ANDROID__)
  snprintf(u->os, sizeof(u->os), "Android");
#elif defined(__APPLE__)
  snprintf(u->os, sizeof(u->os), "Darwin");
#else
  snprintf(u->os, sizeof(u->os), "GNU/Linux");
#endif
}


#ifdef __APPLE__

int os_proclist (OsProcInfo **out, size_t *count) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0};
  size_t size = 0, n, i;
  struct kinfo_proc *kp;
  OsProcInfo *v;
  int argmax_mib[2] = {CTL_KERN, KERN_ARGMAX}, argmax = 0;
  size_t am = sizeof(argmax);
  *out = NULL;
  *count = 0;
  if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0) return -1;
  size += size / 8;
  kp = (struct kinfo_proc *)xmalloc(size);
  if (sysctl(mib, 4, kp, &size, NULL, 0) != 0) {
    free(kp);
    return -1;
  }
  sysctl(argmax_mib, 2, &argmax, &am, NULL, 0);
  n = size / sizeof(struct kinfo_proc);
  v = (OsProcInfo *)xmalloc((n ? n : 1) * sizeof(OsProcInfo));
  for (i = 0; i < n; i++) {
    OsProcInfo *p = &v[i];
    static const char states[] = "?IRSTZ";	/* SIDL SRUN SSLEEP SSTOP SZOMB */
    int st = kp[i].kp_proc.p_stat;
    memset(p, 0, sizeof(*p));
    p->pid = (long)kp[i].kp_proc.p_pid;
    p->ppid = (long)kp[i].kp_eproc.e_ppid;
    p->uid = (long)kp[i].kp_eproc.e_ucred.cr_uid;
    p->start = kp[i].kp_proc.p_starttime.tv_sec;
    p->state = (st >= 0 && st <= 5) ? states[st] : '?';
    p->name = xstrdup(kp[i].kp_proc.p_comm);
    if (argmax > 0) {	/* argc, the program path, then argv */
      int amib[3] = {CTL_KERN, KERN_PROCARGS2, 0};
      char *buf = (char *)xmalloc((size_t)argmax);
      size_t len = (size_t)argmax;
      amib[2] = (int)p->pid;
      if (sysctl(amib, 3, buf, &len, NULL, 0) == 0 && len > sizeof(int)) {
        int argc, k;
        char *q = buf + sizeof(int), *end = buf + len;
        Buf b;
        memcpy(&argc, buf, sizeof(int));
        q += strnlen(q, (size_t)(end - q));
        while (q < end && *q == '\0') q++;
        buf_init(&b);
        for (k = 0; k < argc && q < end; k++) {
          size_t l = strnlen(q, (size_t)(end - q));
          if (k) buf_putc(&b, ' ');
          buf_putn(&b, q, l);
          q += l + 1;
        }
        if (b.len > 0) p->cmd = buf_take(&b);
        else buf_free(&b);
      }
      free(buf);
    }
    if (p->cmd == NULL) p->cmd = xstrdup(p->name);
  }
  free(kp);
  *out = v;
  *count = n;
  return 0;
}


unsigned long long os_memtotal (void) {
  int mib[2] = {CTL_HW, HW_MEMSIZE};
  unsigned long long m = 0;
  size_t len = sizeof(m);
  if (sysctl(mib, 2, &m, &len, NULL, 0) != 0) return 0;
  return m;
}

#else

int os_proclist (OsProcInfo **out, size_t *count) {
  Vec names;
  size_t i, n = 0, len;
  OsProcInfo *v;
  long tick = sysconf(_SC_CLK_TCK), page = sysconf(_SC_PAGESIZE);
  time_t boot = 0;
  char *text;
  *out = NULL;
  *count = 0;
  if (tick <= 0) tick = 100;
  if ((text = read_file("/proc/stat", &len)) != NULL) {
    char *b = strstr(text, "\nbtime ");
    if (b) boot = (time_t)strtoll(b + 7, NULL, 10);
    free(text);
  }
  vec_init(&names);
  if (os_listdir("/proc", &names) != 0) {
    vec_free(&names);
    return -1;
  }
  v = (OsProcInfo *)xmalloc((names.n ? names.n : 1) * sizeof(OsProcInfo));
  for (i = 0; i < names.n; i++) {
    char path[64], *s, *rp;
    OsProcInfo *p;
    long long f[40];
    int k;
    if (strspn(names.v[i], "0123456789") != strlen(names.v[i])) continue;
    snprintf(path, sizeof(path), "/proc/%s/stat", names.v[i]);
    if ((s = read_file(path, &len)) == NULL) continue;
    rp = strrchr(s, ')');
    if (rp == NULL || strchr(s, '(') == NULL) {
      free(s);
      continue;
    }
    p = &v[n++];
    memset(p, 0, sizeof(*p));
    p->pid = atol(names.v[i]);
    *rp = '\0';
    p->name = xstrdup(strchr(s, '(') + 1);
    rp += 2;
    p->state = *rp ? *rp : '?';
    memset(f, 0, sizeof(f));
    /* fields from 4 on: ppid pgrp session tty tpgid flags minflt cminflt
    ** majflt cmajflt utime stime cutime cstime priority nice threads
    ** itreal starttime vsize rss */
    for (k = 4, rp += 1; k < 40 && *rp; k++) {
      f[k] = strtoll(rp, &rp, 10);
    }
    p->ppid = (long)f[4];
    p->cpu = (double)(f[14] + f[15]) / (double)tick;
    p->start = boot + (time_t)(f[22] / tick);
    p->vsz = f[23];
    p->rss = f[24] * page;
    free(s);
    snprintf(path, sizeof(path), "/proc/%s/status", names.v[i]);
    if ((s = read_file(path, &len)) != NULL) {
      char *u = strstr(s, "\nUid:");
      if (u) p->uid = strtol(u + 5, NULL, 10);
      free(s);
    }
    snprintf(path, sizeof(path), "/proc/%s/cmdline", names.v[i]);
    if ((s = read_file(path, &len)) != NULL && len > 0) {
      size_t j;
      while (len > 0 && s[len - 1] == '\0') len--;
      for (j = 0; j < len; j++)
        if (s[j] == '\0') s[j] = ' ';
      s[len] = '\0';
      p->cmd = s;
    }
    else {
      free(s);
      p->cmd = xstrcat3("[", p->name, "]");
    }
  }
  vec_free(&names);
  *out = v;
  *count = n;
  return 0;
}


unsigned long long os_memtotal (void) {
  long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGESIZE);
  if (pages <= 0 || page <= 0) return 0;
  return (unsigned long long)pages * (unsigned long long)page;
}

#endif


char *os_user_name (long uid) {
  struct passwd *pw = getpwuid((uid_t)uid);
  char num[24];
  if (pw && pw->pw_name) return xstrdup(pw->pw_name);
  return xstrdup(ll_to_str(uid, num));
}


char *os_group_name (long gid) {
  struct group *gr = getgrgid((gid_t)gid);
  char num[24];
  if (gr && gr->gr_name) return xstrdup(gr->gr_name);
  return xstrdup(ll_to_str(gid, num));
}


int os_is_system_program (const char *native) {
  (void)native;
  return 0;
}

/* }================================================================== */

/* }================================================================== */

#endif
