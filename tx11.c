/*
** tx11.c - Linux (X11) backend of mmc-term
**
** libX11 is loaded at run time with dlopen and the few Xlib types that
** are needed are declared here, so this file builds on any system
** without X11 headers (zig can cross compile it from Windows).
** Wayland desktops run it through XWayland.
**
** STATUS: compiles and links; not yet run on a real Linux desktop.
*/

#include "mterm.h"

#if !defined(_WIN32) && !defined(__APPLE__)

#include <dlfcn.h>
#include <limits.h>
#include <locale.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


/*
** {==================================================================
** The part of Xlib that is used (same layout as <X11/Xlib.h>)
** ===================================================================
*/

typedef struct XDisplay_ Display;
typedef struct XVisual_ Visual;
typedef struct XGC_ *GC;
typedef struct XIM_ *XIM;
typedef struct XIC_ *XIC;
typedef unsigned long XID, Window, Atom, Time, KeySym, Cursor;
typedef int Bool;

typedef struct XAnyEvent {
  int type; unsigned long serial; Bool send_event; Display *display;
  Window window;
} XAnyEvent;

typedef struct XKeyEvent {	/* XButtonEvent has the same layout */
  int type; unsigned long serial; Bool send_event; Display *display;
  Window window, root, subwindow; Time time;
  int x, y, x_root, y_root; unsigned int state, keycode; Bool same_screen;
} XKeyEvent;

typedef struct XMotionEvent {
  int type; unsigned long serial; Bool send_event; Display *display;
  Window window, root, subwindow; Time time;
  int x, y, x_root, y_root; unsigned int state; char is_hint; Bool same_screen;
} XMotionEvent;

typedef struct XConfigureEvent {
  int type; unsigned long serial; Bool send_event; Display *display;
  Window event, window; int x, y, width, height, border_width;
  Window above; Bool override_redirect;
} XConfigureEvent;

typedef struct XClientMessageEvent {
  int type; unsigned long serial; Bool send_event; Display *display;
  Window window; Atom message_type; int format;
  union { char b[20]; short s[10]; long l[5]; } data;
} XClientMessageEvent;

typedef struct XSelectionRequestEvent {
  int type; unsigned long serial; Bool send_event; Display *display;
  Window owner, requestor; Atom selection, target, property; Time time;
} XSelectionRequestEvent;

typedef struct XSelectionEvent {
  int type; unsigned long serial; Bool send_event; Display *display;
  Window requestor; Atom selection, target, property; Time time;
} XSelectionEvent;

typedef union XEvent {
  int type;
  XAnyEvent any;
  XKeyEvent key;
  XMotionEvent motion;
  XConfigureEvent configure;
  XClientMessageEvent client;
  XSelectionRequestEvent request;
  XSelectionEvent selection;
  long pad[24];
} XEvent;

typedef struct XImage {	/* only the first fields are touched */
  int width, height, xoffset, format;
  char *data;
} XImage;

typedef struct XSizeHints {
  long flags; int x, y, width, height, min_width, min_height, max_width,
  max_height, width_inc, height_inc;
  struct { int x, y; } min_aspect, max_aspect;
  int base_width, base_height, win_gravity;
} XSizeHints;

typedef struct XClassHint { char *res_name, *res_class; } XClassHint;

enum { KeyPress = 2, ButtonPress = 4, ButtonRelease = 5, MotionNotify = 6,
       LeaveNotify = 8, FocusIn = 9, FocusOut = 10, Expose = 12,
       ConfigureNotify = 22, SelectionClear = 29, SelectionRequest = 30,
       SelectionNotify = 31, ClientMessage = 33 };

#define EVENT_MASK	(1L | 1L << 2 | 1L << 3 | 1L << 5 | 1L << 6 | 1L << 13 | \
			 1L << 15 | 1L << 17 | 1L << 21)
#define ShiftMask	1u
#define ControlMask	4u
#define Mod1Mask	8u
#define Button1Mask	(1u << 8)
#define ZPixmap		2
#define XA_ATOM		4
#define XA_CARDINAL	6
#define XA_STRING	31
#define PropModeReplace	0
#define PMinSize	(1L << 4)
#define PResizeInc	(1L << 6)
#define PBaseSize	(1L << 8)
#define XIMPreeditNothing	0x0008L
#define XIMStatusNothing	0x0400L

static struct {
  Display *(*XOpenDisplay) (const char *);
  int (*XDefaultScreen) (Display *);
  Window (*XRootWindow) (Display *, int);
  Visual *(*XDefaultVisual) (Display *, int);
  int (*XDefaultDepth) (Display *, int);
  Window (*XCreateSimpleWindow) (Display *, Window, int, int, unsigned, unsigned,
                                 unsigned, unsigned long, unsigned long);
  int (*XSelectInput) (Display *, Window, long);
  int (*XMapWindow) (Display *, Window);
  int (*XStoreName) (Display *, Window, const char *);
  int (*XChangeProperty) (Display *, Window, Atom, Atom, int, int,
                          const unsigned char *, int);
  Atom (*XInternAtom) (Display *, const char *, Bool);
  int (*XSetWMProtocols) (Display *, Window, Atom *, int);
  GC (*XCreateGC) (Display *, Window, unsigned long, void *);
  XImage *(*XCreateImage) (Display *, Visual *, unsigned, int, int, char *,
                           unsigned, unsigned, int, int);
  int (*XPutImage) (Display *, Window, GC, XImage *, int, int, int, int,
                    unsigned, unsigned);
  int (*XFree) (void *);
  int (*XPending) (Display *);
  int (*XNextEvent) (Display *, XEvent *);
  int (*XFlush) (Display *);
  int (*XConnectionNumber) (Display *);
  int (*XLookupString) (XKeyEvent *, char *, int, KeySym *, void *);
  int (*Xutf8LookupString) (XIC, XKeyEvent *, char *, int, KeySym *, int *);
  XIM (*XOpenIM) (Display *, void *, char *, char *);
  XIC (*XCreateIC) (XIM, ...);
  void (*XSetICFocus) (XIC);
  void (*XUnsetICFocus) (XIC);
  Bool (*XFilterEvent) (XEvent *, Window);
  char *(*XSetLocaleModifiers) (const char *);
  int (*XSetSelectionOwner) (Display *, Atom, Window, Time);
  Window (*XGetSelectionOwner) (Display *, Atom);
  int (*XConvertSelection) (Display *, Atom, Atom, Atom, Window, Time);
  int (*XGetWindowProperty) (Display *, Window, Atom, long, long, Bool, Atom,
                             Atom *, int *, unsigned long *, unsigned long *,
                             unsigned char **);
  int (*XSendEvent) (Display *, Window, Bool, long, XEvent *);
  int (*XResizeWindow) (Display *, Window, unsigned, unsigned);
  void (*XSetWMNormalHints) (Display *, Window, XSizeHints *);
  int (*XSetClassHint) (Display *, Window, XClassHint *);
  char *(*XResourceManagerString) (Display *);
  Cursor (*XCreateFontCursor) (Display *, unsigned);
  int (*XDefineCursor) (Display *, Window, Cursor);
} X;

/* }================================================================== */


static Display *dpy = NULL;
static Window win = 0, root = 0;
static Visual *visual;
static int depth, screen;
static GC gc;
static XIM xim;
static XIC xic;
static XImage *image = NULL;
static Atom a_delete, a_protocols, a_clipboard, a_primary, a_utf8, a_targets,
            a_text, a_paste, a_net_name, a_state, a_fullscreen, a_attention,
            a_opacity;
static char *clip_text = NULL;
static int quit = 0, woken = 0, need_redraw = 1;
static int win_w, win_h;
static float dpi_scale = 1.0f;


static int load_x11 (void) {
  static const char *const libs[] = {"libX11.so.6", "libX11.so", NULL};
  void *lib = NULL;
  int i, missing = 0;
  for (i = 0; libs[i] != NULL && lib == NULL; i++)
    lib = dlopen(libs[i], RTLD_LAZY | RTLD_GLOBAL);
  if (lib == NULL) return -1;
#define LOAD(name) do { void *p = dlsym(lib, #name); \
    if (p == NULL) missing++; memcpy(&X.name, &p, sizeof(p)); } while (0)
  LOAD(XOpenDisplay); LOAD(XDefaultScreen); LOAD(XRootWindow);
  LOAD(XDefaultVisual); LOAD(XDefaultDepth); LOAD(XCreateSimpleWindow);
  LOAD(XSelectInput); LOAD(XMapWindow); LOAD(XStoreName);
  LOAD(XChangeProperty); LOAD(XInternAtom); LOAD(XSetWMProtocols);
  LOAD(XCreateGC); LOAD(XCreateImage); LOAD(XPutImage); LOAD(XFree);
  LOAD(XPending); LOAD(XNextEvent); LOAD(XFlush); LOAD(XConnectionNumber);
  LOAD(XLookupString); LOAD(Xutf8LookupString); LOAD(XOpenIM);
  LOAD(XCreateIC); LOAD(XSetICFocus); LOAD(XUnsetICFocus); LOAD(XFilterEvent);
  LOAD(XSetLocaleModifiers); LOAD(XSetSelectionOwner);
  LOAD(XGetSelectionOwner); LOAD(XConvertSelection); LOAD(XGetWindowProperty);
  LOAD(XSendEvent); LOAD(XResizeWindow); LOAD(XSetWMNormalHints);
  LOAD(XSetClassHint); LOAD(XResourceManagerString); LOAD(XCreateFontCursor);
  LOAD(XDefineCursor);
#undef LOAD
  return missing ? -1 : 0;
}


static int open_display (void) {
  const char *res, *p;
  if (dpy != NULL) return 0;
  if (load_x11() != 0) {
    fprintf(stderr, TERM_NAME ": cannot load libX11 (is X11 installed?)\n");
    return -1;
  }
  setlocale(LC_CTYPE, "");
  X.XSetLocaleModifiers("");
  if ((dpy = X.XOpenDisplay(NULL)) == NULL) {
    fprintf(stderr, TERM_NAME ": cannot open the X display ($DISPLAY)\n");
    return -1;
  }
  screen = X.XDefaultScreen(dpy);
  root = X.XRootWindow(dpy, screen);
  visual = X.XDefaultVisual(dpy, screen);
  depth = X.XDefaultDepth(dpy, screen);
  res = X.XResourceManagerString(dpy);	/* "Xft.dpi:\t144" */
  if (res != NULL && (p = strstr(res, "Xft.dpi:")) != NULL) {
    double dpi = atof(p + 8);
    if (dpi >= 48.0 && dpi <= 960.0) dpi_scale = (float)(dpi / 96.0);
  }
  return 0;
}


float win_scale (void) {
  open_display();
  return dpi_scale;
}


unsigned win_ticks (void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (unsigned)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}


static void set_hints (void) {
  XSizeHints h;
  int bw = 0, bh = 0;
  app_snap_size(&bw, &bh);	/* the smallest grid that still makes sense */
  memset(&h, 0, sizeof(h));
  h.flags = PMinSize;
  h.min_width = bw;
  h.min_height = bh;
  X.XSetWMNormalHints(dpy, win, &h);
}


int win_create (int w, int h, const char *title) {
  XClassHint cls;
  char name[] = TERM_NAME, klass[] = "Mmc-term";
  if (open_display() != 0) return -1;
  if (depth != 24 && depth != 32) {
    fprintf(stderr, TERM_NAME ": needs a 24 or 32 bit display (this is %d)\n", depth);
    return -1;
  }
  win = X.XCreateSimpleWindow(dpy, root, 0, 0, (unsigned)w, (unsigned)h, 0, 0, 0);
  win_w = w;
  win_h = h;
  X.XSelectInput(dpy, win, EVENT_MASK);
  gc = X.XCreateGC(dpy, win, 0, NULL);
  a_protocols = X.XInternAtom(dpy, "WM_PROTOCOLS", 0);
  a_delete = X.XInternAtom(dpy, "WM_DELETE_WINDOW", 0);
  a_clipboard = X.XInternAtom(dpy, "CLIPBOARD", 0);
  a_primary = 1;	/* XA_PRIMARY */
  a_utf8 = X.XInternAtom(dpy, "UTF8_STRING", 0);
  a_targets = X.XInternAtom(dpy, "TARGETS", 0);
  a_text = X.XInternAtom(dpy, "TEXT", 0);
  a_paste = X.XInternAtom(dpy, "MMC_TERM_PASTE", 0);
  a_net_name = X.XInternAtom(dpy, "_NET_WM_NAME", 0);
  a_state = X.XInternAtom(dpy, "_NET_WM_STATE", 0);
  a_fullscreen = X.XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", 0);
  a_attention = X.XInternAtom(dpy, "_NET_WM_STATE_DEMANDS_ATTENTION", 0);
  a_opacity = X.XInternAtom(dpy, "_NET_WM_WINDOW_OPACITY", 0);
  X.XSetWMProtocols(dpy, win, &a_delete, 1);
  cls.res_name = name;
  cls.res_class = klass;
  X.XSetClassHint(dpy, win, &cls);
  X.XDefineCursor(dpy, win, X.XCreateFontCursor(dpy, 152));	/* XC_xterm */
  set_hints();
  win_set_title(title);
  if ((xim = X.XOpenIM(dpy, NULL, NULL, NULL)) != NULL)
    xic = X.XCreateIC(xim, "inputStyle", XIMPreeditNothing | XIMStatusNothing,
                      "clientWindow", win, "focusWindow", win, (char *)NULL);
  return 0;
}


/*
** {==================================================================
** Events
** ===================================================================
*/

static int mods_of (unsigned state) {
  return ((state & ShiftMask) ? TM_SHIFT : 0) |
         ((state & ControlMask) ? TM_CTRL : 0) |
         ((state & Mod1Mask) ? TM_ALT : 0);
}


static int special_key (KeySym k) {
  switch (k) {
    case 0xff52: case 0xff97: return TK_UP;
    case 0xff54: case 0xff99: return TK_DOWN;
    case 0xff51: case 0xff96: return TK_LEFT;
    case 0xff53: case 0xff98: return TK_RIGHT;
    case 0xff50: case 0xff95: return TK_HOME;
    case 0xff57: case 0xff9c: return TK_END;
    case 0xff55: case 0xff9a: return TK_PGUP;
    case 0xff56: case 0xff9b: return TK_PGDN;
    case 0xff63: case 0xff9e: return TK_INSERT;
    case 0xffff: case 0xff9f: return TK_DELETE;
    case 0xff0d: case 0xff8d: return TK_ENTER;
    case 0xff09: case 0xfe20: return TK_TAB;
    case 0xff08: return TK_BACKSPACE;
    case 0xff1b: return TK_ESCAPE;
    default:
      if (k >= 0xffbe && k <= 0xffc9) return TK_F1 + (int)(k - 0xffbe);
      return TK_NONE;
  }
}


static void on_key (XKeyEvent *e) {
  char text[64];
  KeySym sym = 0, plain = 0;
  int mods = mods_of(e->state), key, n, status = 0;
  if (xic != NULL) n = X.Xutf8LookupString(xic, e, text, (int)sizeof(text) - 1, &sym, &status);
  else n = X.XLookupString(e, text, (int)sizeof(text) - 1, &sym, NULL);
  if (n < 0) n = 0;
  text[n] = '\0';
  if ((key = special_key(sym)) != TK_NONE) {
    if (sym == 0xfe20) mods |= TM_SHIFT;	/* ISO_Left_Tab is Shift+Tab */
    app_on_key(key, mods, 0);
    return;
  }
  if (mods & TM_CTRL) {	/* a shortcut such as Ctrl+Shift+C or Ctrl+= ? */
    e->state &= ~(ControlMask | ShiftMask);
    X.XLookupString(e, NULL, 0, &plain, NULL);
    if (plain == 0xffab) plain = '=';	/* keypad + */
    if (plain == 0xffad) plain = '-';	/* keypad - */
    if (plain >= 0x20 && plain < 0x7f && app_on_key(TK_CHAR, mods, (uint32_t)plain))
      return;
  }
  if (n > 0) app_on_text(text, mods & TM_ALT);
}


static void send_selection (const XSelectionRequestEvent *r) {
  XEvent reply;
  const char *text = clip_text ? clip_text : "";
  memset(&reply, 0, sizeof(reply));
  reply.selection.type = SelectionNotify;
  reply.selection.display = r->display;
  reply.selection.requestor = r->requestor;
  reply.selection.selection = r->selection;
  reply.selection.target = r->target;
  reply.selection.time = r->time;
  reply.selection.property = r->property ? r->property : r->target;
  if (r->target == a_targets) {
    Atom list[4];
    list[0] = a_targets; list[1] = a_utf8; list[2] = XA_STRING; list[3] = a_text;
    X.XChangeProperty(dpy, r->requestor, reply.selection.property, XA_ATOM, 32,
                      PropModeReplace, (const unsigned char *)list, 4);
  }
  else if (r->target == a_utf8 || r->target == XA_STRING || r->target == a_text)
    X.XChangeProperty(dpy, r->requestor, reply.selection.property,
                      r->target == a_text ? a_utf8 : r->target, 8,
                      PropModeReplace, (const unsigned char *)text,
                      (int)strlen(text));
  else reply.selection.property = 0;	/* cannot convert to that */
  X.XSendEvent(dpy, r->requestor, 0, 0, &reply);
}


static void receive_paste (const XSelectionEvent *s) {
  Atom type = 0;
  int format = 0;
  unsigned long items = 0, after = 0;
  unsigned char *data = NULL;
  if (s->property == 0) return;
  X.XGetWindowProperty(dpy, win, s->property, 0, LONG_MAX / 4, 1, 0, &type,
                       &format, &items, &after, &data);
  if (data != NULL) {
    if (format == 8) app_on_paste((const char *)data);
    X.XFree(data);
  }
}


static void on_event (XEvent *e) {
  switch (e->type) {
    case KeyPress: on_key(&e->key); break;
    case ButtonPress: {
      unsigned b = e->key.keycode;	/* "button" sits where keycode is */
      int mods = mods_of(e->key.state);
      if (b == 4 || b == 5)
        app_on_mouse(TMS_WHEEL, 0, e->key.x, e->key.y, mods, b == 4 ? 3 : -3);
      else if (b >= 1 && b <= 3)
        app_on_mouse(TMS_DOWN, (int)b, e->key.x, e->key.y, mods, 0);
      break;
    }
    case ButtonRelease:
      if (e->key.keycode >= 1 && e->key.keycode <= 3)
        app_on_mouse(TMS_UP, (int)e->key.keycode, e->key.x, e->key.y,
                     mods_of(e->key.state), 0);
      break;
    case MotionNotify:
      app_on_mouse(TMS_MOVE, 0, e->motion.x, e->motion.y,
                   mods_of(e->motion.state), 0);
      break;
    case LeaveNotify: app_on_mouse(TMS_LEAVE, 0, -1, -1, 0, 0); break;
    case FocusIn:
      if (xic) X.XSetICFocus(xic);
      app_on_focus(1);
      break;
    case FocusOut:
      if (xic) X.XUnsetICFocus(xic);
      app_on_focus(0);
      break;
    case Expose: need_redraw = 1; break;
    case ConfigureNotify:
      if (e->configure.width != win_w || e->configure.height != win_h) {
        win_w = e->configure.width;
        win_h = e->configure.height;
        app_on_resize(win_w, win_h);
      }
      break;
    case ClientMessage:
      if (e->client.message_type == a_protocols &&
          (Atom)e->client.data.l[0] == a_delete)
        quit = 1;
      break;
    case SelectionRequest: send_selection(&e->request); break;
    case SelectionNotify: receive_paste(&e->selection); break;
    case SelectionClear: break;	/* somebody else owns the clipboard now */
    default: break;
  }
}

/* }================================================================== */


static void present (void) {
  static const Frame *last = NULL;
  const Frame *f = app_render();
  if (f != NULL) last = f;
  if (last == NULL || last->px == NULL) return;
  if (image == NULL || image->data != (char *)last->px ||
      image->width != last->w || image->height != last->h) {
    if (image != NULL) {
      image->data = NULL;	/* the pixels belong to the core */
      X.XFree(image);
    }
    image = X.XCreateImage(dpy, visual, (unsigned)depth, ZPixmap, 0,
                           (char *)last->px, (unsigned)last->w,
                           (unsigned)last->h, 32, 0);
  }
  if (image != NULL)
    X.XPutImage(dpy, win, gc, image, 0, 0, 0, 0, (unsigned)last->w,
                (unsigned)last->h);
  X.XFlush(dpy);
}


int win_run (void) {
  XEvent e;
  if (dpy == NULL) return 1;
  X.XMapWindow(dpy, win);
  app_on_resize(win_w, win_h);
  while (!quit) {
    struct pollfd fds[2];
    int n = 1;
    while (X.XPending(dpy) > 0) {
      X.XNextEvent(dpy, &e);
      if (X.XFilterEvent(&e, 0)) continue;	/* the input method took it */
      on_event(&e);
    }
    if (quit) break;
    if (need_redraw) {
      need_redraw = 0;
      present();
    }
    fds[0].fd = X.XConnectionNumber(dpy);
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    if (pty_fd() >= 0) {
      fds[1].fd = pty_fd();
      fds[1].events = POLLIN;
      fds[1].revents = 0;
      n = 2;
    }
    if (woken) woken = 0;
    else poll(fds, (nfds_t)n, 30);
    app_on_tick(win_ticks());
    if (n == 2 && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) app_on_wake();
  }
  pty_close();
  return app_exit_code();
}


void win_close (void) { quit = 1; }
void win_wake (void) { woken = 1; }
void win_redraw (void) { need_redraw = 1; }


void win_set_title (const char *utf8) {
  if (dpy == NULL || win == 0) return;
  X.XStoreName(dpy, win, utf8);
  X.XChangeProperty(dpy, win, a_net_name, a_utf8, 8, PropModeReplace,
                    (const unsigned char *)utf8, (int)strlen(utf8));
}


void win_set_size (int w, int h) {
  if (dpy != NULL && win != 0) X.XResizeWindow(dpy, win, (unsigned)w, (unsigned)h);
}


/* asks the window manager to add or remove a _NET_WM_STATE */
static void net_state (Atom state, int add) {
  XEvent e;
  if (dpy == NULL || win == 0) return;
  memset(&e, 0, sizeof(e));
  e.client.type = ClientMessage;
  e.client.window = win;
  e.client.message_type = a_state;
  e.client.format = 32;
  e.client.data.l[0] = add ? 1 : 0;
  e.client.data.l[1] = (long)state;
  e.client.data.l[3] = 1;	/* from a normal application */
  X.XSendEvent(dpy, root, 0, (1L << 20) | (1L << 19), &e);	/* redirect | notify */
}


void win_set_fullscreen (int on) { net_state(a_fullscreen, on); }
void win_flash (void) { net_state(a_attention, 1); }


void win_set_chrome (uint32_t bg, uint32_t border, uint32_t text, int dark) {
  (void)bg; (void)border; (void)text; (void)dark;	/* the WM draws the frame */
}


void win_set_opacity (int percent) {
  unsigned long v;
  if (dpy == NULL || win == 0 || percent >= 100) return;
  v = (unsigned long)(0xFFFFFFFFul / 100ul * (unsigned long)percent);
  X.XChangeProperty(dpy, win, a_opacity, XA_CARDINAL, 32, PropModeReplace,
                    (const unsigned char *)&v, 1);
}


void win_set_clipboard (const char *utf8) {
  if (dpy == NULL || win == 0) return;
  free(clip_text);
  clip_text = xstrdup(utf8);
  X.XSetSelectionOwner(dpy, a_clipboard, win, 0);
  X.XSetSelectionOwner(dpy, a_primary, win, 0);
}


void win_request_paste (void) {
  Atom from;
  if (dpy == NULL || win == 0) return;
  from = X.XGetSelectionOwner(dpy, a_clipboard) ? a_clipboard : a_primary;
  if (X.XGetSelectionOwner(dpy, from) == win) {	/* it is our own text */
    if (clip_text) app_on_paste(clip_text);
    return;
  }
  X.XConvertSelection(dpy, from, a_utf8, a_paste, win, 0);
}


void win_message (const char *title, const char *text) {
  fprintf(stderr, "%s: %s\n", title, text);
}


/* the window manager draws the title bar here */
int win_custom_chrome (int want) { (void)want; return 0; }
void win_minimize (void) { }
void win_toggle_maximize (void) { }
int win_is_maximized (void) { return 0; }

#else

typedef int tx11_is_empty_here;	/* ISO C wants something in every file */

#endif
