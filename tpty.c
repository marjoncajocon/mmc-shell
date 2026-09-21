/*
** tpty.c - pseudo terminals of mmc-term
**
** Starts a shell "behind" the window: whatever the window writes is
** the keyboard of the shell, whatever the shell prints comes back as
** bytes for tvt.c. Every tab has its own Pty. Windows uses ConPTY
** (Windows 10 1809 or newer), Linux and macOS use a classic pty.
*/

#include "mterm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#ifdef _WIN32

/*
** {==================================================================
** Windows: ConPTY
** ===================================================================
*/

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE	0x00020016
#endif
#ifndef EXTENDED_STARTUPINFO_PRESENT
#define EXTENDED_STARTUPINFO_PRESENT	0x00080000
#endif

typedef HRESULT (WINAPI *CreatePcFn) (COORD, HANDLE, HANDLE, DWORD, void **);
typedef HRESULT (WINAPI *ResizePcFn) (void *, COORD);
typedef void (WINAPI *ClosePcFn) (void *);

/* the functions are looked up at run time so old Windows can say "no" */
typedef union FnPtr {
  FARPROC raw;
  CreatePcFn create;
  ResizePcFn resize;
  ClosePcFn close;
} FnPtr;

static FnPtr fn_create, fn_resize, fn_close;

/* the tab, the reader thread and the waiter thread each hold a
** reference: the last one to let go frees it */
struct Pty {
  void *pcon;
  HANDLE in_write, out_read, process;
  CRITICAL_SECTION lock;
  Buf queue;
  size_t queue_pos;
  volatile LONG closed, exited, refs;
  DWORD exit_code;
};


static void quote_arg (Buf *b, const char *a) {
  const char *p;
  if (a[0] != '\0' && strpbrk(a, " \t\"") == NULL) {
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


static wchar_t *widen (const char *s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  wchar_t *w = (wchar_t *)xmalloc((size_t)(n > 0 ? n : 1) * sizeof(wchar_t));
  w[0] = L'\0';
  MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
  return w;
}


static void pty_unref (Pty *p) {
  if (InterlockedDecrement(&p->refs) != 0) return;
  CloseHandle(p->out_read);
  CloseHandle(p->process);
  DeleteCriticalSection(&p->lock);
  buf_free(&p->queue);
  free(p);
}


static DWORD WINAPI reader_thread (LPVOID arg) {
  Pty *p = (Pty *)arg;
  char buf[16384];
  DWORD n;
  while (ReadFile(p->out_read, buf, sizeof(buf), &n, NULL) && n > 0) {
    size_t pending;
    EnterCriticalSection(&p->lock);
    buf_putn(&p->queue, buf, n);
    pending = p->queue.len - p->queue_pos;
    LeaveCriticalSection(&p->lock);
    win_wake();
    while (pending > ((size_t)4 << 20) && !p->closed) {	/* window is behind */
      Sleep(2);
      EnterCriticalSection(&p->lock);
      pending = p->queue.len - p->queue_pos;
      LeaveCriticalSection(&p->lock);
    }
  }
  InterlockedExchange(&p->closed, 1);
  win_wake();
  pty_unref(p);
  return 0;
}


static DWORD WINAPI waiter_thread (LPVOID arg) {
  Pty *p = (Pty *)arg;
  DWORD code = 0;
  WaitForSingleObject(p->process, INFINITE);
  GetExitCodeProcess(p->process, &code);
  p->exit_code = code;
  Sleep(60);	/* let the last output arrive */
  InterlockedExchange(&p->exited, 1);
  win_wake();
  pty_unref(p);
  return 0;
}


Pty *pty_spawn (const char *exe, char **argv, int cols, int rows) {
  HMODULE k32 = GetModuleHandleA("kernel32.dll");
  HANDLE in_read = NULL, in_write = NULL, out_read = NULL, out_write = NULL;
  HANDLE old_in, old_out, old_err;
  STARTUPINFOEXW si;
  PROCESS_INFORMATION pi;
  SIZE_T attr_size = 0;
  COORD size;
  Buf cl;
  wchar_t *wexe, *wcl;
  void *pcon = NULL;
  Pty *p;
  int i, ok;
  fn_create.raw = GetProcAddress(k32, "CreatePseudoConsole");
  fn_resize.raw = GetProcAddress(k32, "ResizePseudoConsole");
  fn_close.raw = GetProcAddress(k32, "ClosePseudoConsole");
  if (fn_create.raw == NULL || fn_resize.raw == NULL || fn_close.raw == NULL) {
    win_message(TERM_NAME, "This Windows has no pseudo console (ConPTY).\n"
                           "Windows 10 version 1809 or newer is needed.");
    return NULL;
  }
  if (!CreatePipe(&in_read, &in_write, NULL, 0)) return NULL;
  if (!CreatePipe(&out_read, &out_write, NULL, 0)) {
    CloseHandle(in_read);
    CloseHandle(in_write);
    return NULL;
  }
  size.X = (SHORT)cols;
  size.Y = (SHORT)rows;
  if (fn_create.create(size, in_read, out_write, 0, &pcon) != S_OK) {
    CloseHandle(in_read);
    CloseHandle(in_write);
    CloseHandle(out_read);
    CloseHandle(out_write);
    return NULL;
  }
  memset(&si, 0, sizeof(si));
  memset(&pi, 0, sizeof(pi));
  si.StartupInfo.cb = sizeof(si);
  InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
  si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)xmalloc(attr_size);
  InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attr_size);
  UpdateProcThreadAttribute(si.lpAttributeList, 0,
                            PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, pcon,
                            sizeof(pcon), NULL, NULL);
  buf_init(&cl);
  quote_arg(&cl, exe);
  for (i = 1; argv[i] != NULL; i++) {
    buf_putc(&cl, ' ');
    quote_arg(&cl, argv[i]);
  }
  wexe = widen(exe);
  wcl = widen(cl.s);
  /* our own std handles (if any) must not leak into the child: it has to
  ** take the ones of the pseudo console */
  old_in = GetStdHandle(STD_INPUT_HANDLE);
  old_out = GetStdHandle(STD_OUTPUT_HANDLE);
  old_err = GetStdHandle(STD_ERROR_HANDLE);
  SetStdHandle(STD_INPUT_HANDLE, NULL);
  SetStdHandle(STD_OUTPUT_HANDLE, NULL);
  SetStdHandle(STD_ERROR_HANDLE, NULL);
  ok = CreateProcessW(wexe, wcl, NULL, NULL, FALSE,
                      EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                      NULL, NULL, &si.StartupInfo, &pi) != 0;
  SetStdHandle(STD_INPUT_HANDLE, old_in);
  SetStdHandle(STD_OUTPUT_HANDLE, old_out);
  SetStdHandle(STD_ERROR_HANDLE, old_err);
  DeleteProcThreadAttributeList(si.lpAttributeList);
  free(si.lpAttributeList);
  free(wexe);
  free(wcl);
  buf_free(&cl);
  CloseHandle(in_read);
  CloseHandle(out_write);
  if (!ok) {
    fn_close.close(pcon);
    CloseHandle(in_write);
    CloseHandle(out_read);
    return NULL;
  }
  CloseHandle(pi.hThread);
  p = (Pty *)xmalloc(sizeof(Pty));
  memset(p, 0, sizeof(*p));
  p->pcon = pcon;
  p->in_write = in_write;
  p->out_read = out_read;
  p->process = pi.hProcess;
  p->refs = 3;
  InitializeCriticalSection(&p->lock);
  buf_init(&p->queue);
  CloseHandle(CreateThread(NULL, 0, reader_thread, p, 0, NULL));
  CloseHandle(CreateThread(NULL, 0, waiter_thread, p, 0, NULL));
  return p;
}


void pty_write (Pty *p, const char *s, size_t n) {
  while (n > 0 && p->in_write != NULL) {
    DWORD put = 0;
    if (!WriteFile(p->in_write, s, (DWORD)n, &put, NULL) || put == 0) return;
    s += put;
    n -= put;
  }
}


void pty_resize (Pty *p, int cols, int rows) {
  COORD size;
  if (p->pcon == NULL) return;
  size.X = (SHORT)cols;
  size.Y = (SHORT)rows;
  fn_resize.resize(p->pcon, size);
}


long pty_read (Pty *p, char *buf, size_t n) {
  size_t have;
  EnterCriticalSection(&p->lock);
  have = p->queue.len - p->queue_pos;
  if (have > n) have = n;
  if (have > 0) {
    memcpy(buf, p->queue.s + p->queue_pos, have);
    p->queue_pos += have;
    if (p->queue_pos == p->queue.len) {	/* all read: start over */
      p->queue.len = 0;
      p->queue_pos = 0;
    }
  }
  LeaveCriticalSection(&p->lock);
  if (have == 0 && p->closed) return -1;
  return (long)have;
}


int pty_fd (Pty *p) {
  (void)p;
  return -1;
}


int pty_exited (Pty *p, int *code) {
  if (!p->exited) return 0;
  if (code) *code = (int)p->exit_code;
  return 1;
}


void pty_close (Pty *p) {
  InterlockedExchange(&p->closed, 1);
  fn_close.close(p->pcon);	/* ends the shell if it is still there */
  p->pcon = NULL;
  CloseHandle(p->in_write);
  p->in_write = NULL;
  pty_unref(p);	/* the threads end on their own and let go as well */
}

/* }================================================================== */

#else

/*
** {==================================================================
** Linux, macOS: pty
** ===================================================================
*/

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

struct Pty {
  int master;
  pid_t child;
  int status, done;
};

/* shells of closed tabs that had not ended yet: waited for later */
#define ORPHAN_MAX	64
static pid_t orphans[ORPHAN_MAX];
static int norphans = 0;


static void reap_orphans (void) {
  int i = 0;
  while (i < norphans) {
    if (waitpid(orphans[i], NULL, WNOHANG) != 0) orphans[i] = orphans[--norphans];
    else i++;
  }
}


static void set_size (int fd, int cols, int rows) {
  struct winsize ws;
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = (unsigned short)cols;
  ws.ws_row = (unsigned short)rows;
  ioctl(fd, TIOCSWINSZ, &ws);
}


Pty *pty_spawn (const char *exe, char **argv, int cols, int rows) {
  const char *slave_name;
  pid_t child;
  Pty *p;
  int master = posix_openpt(O_RDWR | O_NOCTTY);
  if (master < 0) return NULL;
  if (grantpt(master) != 0 || unlockpt(master) != 0 ||
      (slave_name = ptsname(master)) == NULL) {
    close(master);
    return NULL;
  }
  set_size(master, cols, rows);
  child = fork();
  if (child < 0) {
    close(master);
    return NULL;
  }
  if (child == 0) {
    int slave;
    setsid();	/* new session: the pty becomes the controlling terminal */
    slave = open(slave_name, O_RDWR);
    if (slave < 0) _exit(126);
#ifdef TIOCSCTTY
    ioctl(slave, TIOCSCTTY, 0);
#endif
    dup2(slave, 0);
    dup2(slave, 1);
    dup2(slave, 2);
    if (slave > 2) close(slave);
    close(master);
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
    execv(exe, argv);
    _exit(127);
  }
  fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
  fcntl(master, F_SETFD, FD_CLOEXEC);
  p = (Pty *)xmalloc(sizeof(Pty));
  memset(p, 0, sizeof(*p));
  p->master = master;
  p->child = child;
  return p;
}


void pty_write (Pty *p, const char *s, size_t n) {
  while (n > 0 && p->master >= 0) {
    ssize_t r = write(p->master, s, n);
    if (r > 0) {
      s += r;
      n -= (size_t)r;
    }
    else if (r < 0 && (errno == EAGAIN || errno == EINTR)) {
      struct pollfd pf;	/* the shell is busy: wait until it takes more */
      pf.fd = p->master;
      pf.events = POLLOUT;
      pf.revents = 0;
      poll(&pf, 1, 100);
    }
    else return;
  }
}


void pty_resize (Pty *p, int cols, int rows) {
  if (p->master >= 0) set_size(p->master, cols, rows);
}


long pty_read (Pty *p, char *buf, size_t n) {
  ssize_t r;
  if (p->master < 0) return -1;
  r = read(p->master, buf, n);
  if (r > 0) return (long)r;
  if (r < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
  return -1;	/* 0 or EIO: the shell side is closed */
}


int pty_fd (Pty *p) {
  return p->master;
}


int pty_exited (Pty *p, int *code) {
  reap_orphans();
  if (!p->done && waitpid(p->child, &p->status, WNOHANG) == p->child) p->done = 1;
  if (!p->done) return 0;
  if (code)
    *code = WIFEXITED(p->status) ? WEXITSTATUS(p->status) : 128 + WTERMSIG(p->status);
  return 1;
}


void pty_close (Pty *p) {
  if (p->master >= 0) close(p->master);	/* the shell gets a hangup */
  if (!p->done && waitpid(p->child, NULL, WNOHANG) == 0) {
    kill(-p->child, SIGHUP);	/* its whole session: the jobs of the tab too */
    if (norphans < ORPHAN_MAX) orphans[norphans++] = p->child;
  }
  free(p);
}

/* }================================================================== */

#endif
