/*
** tcocoa.c - macOS (Cocoa) backend of mmc-term
**
** Cocoa is an Objective-C API, but every Objective-C call is a plain C
** call to objc_msgSend, so this file stays C. The runtime, AppKit and
** CoreGraphics are loaded with dlopen: no SDK is needed to build it,
** and zig can cross compile it from Windows or Linux.
**
** STATUS: UNTESTED. It compiles; it has never been run on a Mac.
*/

#include "mterm.h"

#if defined(__APPLE__)

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


typedef void *id;
typedef void *SEL;
typedef void *Class;
typedef signed char BOOL;
typedef struct NSPoint { double x, y; } NSPoint;
typedef struct NSSize { double w, h; } NSSize;
typedef struct NSRect { NSPoint origin; NSSize size; } NSRect;
typedef void (*AnyFn) (void);

/* objc_msgSend has no fixed signature: cast it to the shape of each call */
typedef id (*Msg) (id, SEL);
typedef id (*MsgId) (id, SEL, id);
typedef id (*MsgIdId) (id, SEL, id, id);
typedef id (*MsgStr) (id, SEL, const char *);
typedef id (*MsgLong) (id, SEL, long);
typedef id (*MsgBool) (id, SEL, BOOL);
typedef id (*MsgDouble) (id, SEL, double);
typedef id (*MsgRect) (id, SEL, NSRect);
typedef id (*MsgSize) (id, SEL, NSSize);
typedef id (*MsgWin) (id, SEL, NSRect, unsigned long, unsigned long, BOOL);
typedef id (*MsgColor) (id, SEL, double, double, double, double);
typedef id (*MsgTimer) (id, SEL, double, id, SEL, id, BOOL);
typedef double (*MsgRetDouble) (id, SEL);
typedef unsigned long (*MsgRetULong) (id, SEL);
typedef unsigned short (*MsgRetUShort) (id, SEL);
typedef const char *(*MsgRetStr) (id, SEL);
typedef NSPoint (*MsgRetPoint) (id, SEL);
typedef NSPoint (*MsgPointConv) (id, SEL, NSPoint, id);
typedef void (*MsgStretRect) (NSRect *, id, SEL);
typedef NSRect (*MsgRetRect) (id, SEL);

static struct {
  AnyFn objc_msgSend, objc_msgSend_stret;
  Class (*objc_getClass) (const char *);
  SEL (*sel_registerName) (const char *);
  Class (*objc_allocateClassPair) (Class, const char *, size_t);
  BOOL (*class_addMethod) (Class, SEL, AnyFn, const char *);
  void (*objc_registerClassPair) (Class);
  void *(*CGColorSpaceCreateDeviceRGB) (void);
  void *(*CGDataProviderCreateWithData) (void *, const void *, size_t, void *);
  void *(*CGImageCreate) (size_t, size_t, size_t, size_t, size_t, void *,
                          uint32_t, void *, const double *, int, int);
  void (*CGContextDrawImage) (void *, NSRect, void *);
  void (*CGContextTranslateCTM) (void *, double, double);
  void (*CGContextScaleCTM) (void *, double, double);
  void (*CGContextSetInterpolationQuality) (void *, int);
  void (*CGImageRelease) (void *);
  void (*CGDataProviderRelease) (void *);
  void (*CGColorSpaceRelease) (void *);
} R;

static id app = NULL, window = NULL, view = NULL, delegate = NULL;
static double backing = 1.0;
static int need_redraw = 1, is_fullscreen = 0;
static double wheel_rest = 0.0;
static const Frame *last_frame = NULL;


#define SELN(name)	R.sel_registerName(name)
#define CLS(name)	((id)R.objc_getClass(name))


static int load_runtime (void) {
  void *objc, *appkit, *cg;
  int missing = 0;
  if (R.objc_msgSend != NULL) return 0;
  objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_LAZY | RTLD_GLOBAL);
  appkit = dlopen("/System/Library/Frameworks/AppKit.framework/AppKit",
                  RTLD_LAZY | RTLD_GLOBAL);
  cg = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics",
              RTLD_LAZY | RTLD_GLOBAL);
  if (objc == NULL || appkit == NULL || cg == NULL) return -1;
#define LOAD(lib, name) do { void *p = dlsym(lib, #name); \
    if (p == NULL) missing++; memcpy(&R.name, &p, sizeof(p)); } while (0)
  LOAD(objc, objc_msgSend); LOAD(objc, objc_getClass);
  LOAD(objc, sel_registerName); LOAD(objc, objc_allocateClassPair);
  LOAD(objc, class_addMethod); LOAD(objc, objc_registerClassPair);
  LOAD(cg, CGColorSpaceCreateDeviceRGB); LOAD(cg, CGDataProviderCreateWithData);
  LOAD(cg, CGImageCreate); LOAD(cg, CGContextDrawImage);
  LOAD(cg, CGContextTranslateCTM); LOAD(cg, CGContextScaleCTM);
  LOAD(cg, CGContextSetInterpolationQuality); LOAD(cg, CGImageRelease);
  LOAD(cg, CGDataProviderRelease); LOAD(cg, CGColorSpaceRelease);
#if defined(__x86_64__)
  LOAD(objc, objc_msgSend_stret);	/* big structs come back this way on Intel */
#endif
#undef LOAD
  return missing ? -1 : 0;
}


static id msg (id o, const char *sel) {
  return ((Msg)R.objc_msgSend)(o, SELN(sel));
}


static id msg_id (id o, const char *sel, id a) {
  return ((MsgId)R.objc_msgSend)(o, SELN(sel), a);
}


static id msg_long (id o, const char *sel, long a) {
  return ((MsgLong)R.objc_msgSend)(o, SELN(sel), a);
}


static id msg_bool (id o, const char *sel, int a) {
  return ((MsgBool)R.objc_msgSend)(o, SELN(sel), (BOOL)(a != 0));
}


static id nsstring (const char *utf8) {
  return ((MsgStr)R.objc_msgSend)(CLS("NSString"), SELN("stringWithUTF8String:"), utf8);
}


static NSRect rect_of (id o, const char *sel) {
#if defined(__x86_64__)
  NSRect r;
  ((MsgStretRect)R.objc_msgSend_stret)(&r, o, SELN(sel));
  return r;
#else
  return ((MsgRetRect)R.objc_msgSend)(o, SELN(sel));
#endif
}


static NSRect make_rect (double x, double y, double w, double h) {
  NSRect r;
  r.origin.x = x; r.origin.y = y; r.size.w = w; r.size.h = h;
  return r;
}


/*
** {==================================================================
** The view and the delegate: Objective-C classes made at run time
** ===================================================================
*/

static void view_draw (id self, SEL cmd, NSRect dirty) {
  const Frame *f = app_render();
  NSRect b = rect_of(self, "bounds");
  id nsctx = msg(CLS("NSGraphicsContext"), "currentContext");
  void *ctx = nsctx ? (void *)msg(nsctx, "CGContext") : NULL;
  void *space, *provider, *image;
  (void)cmd; (void)dirty;
  if (f != NULL) last_frame = f;
  if (ctx == NULL || last_frame == NULL || last_frame->px == NULL) return;
  space = R.CGColorSpaceCreateDeviceRGB();
  provider = R.CGDataProviderCreateWithData(NULL, last_frame->px,
               (size_t)last_frame->w * (size_t)last_frame->h * 4, NULL);
  /* 0x00RRGGBB little endian: "skip first alpha" + 32 bit little = 6 | 8192 */
  image = R.CGImageCreate((size_t)last_frame->w, (size_t)last_frame->h, 8, 32,
                          (size_t)last_frame->w * 4, space, 6u | 8192u, provider,
                          NULL, 0, 0);
  if (image != NULL) {
    /* the view is flipped (y grows down) but images draw bottom up */
    R.CGContextSetInterpolationQuality(ctx, 1);
    R.CGContextTranslateCTM(ctx, 0.0, b.size.h);
    R.CGContextScaleCTM(ctx, 1.0, -1.0);
    R.CGContextDrawImage(ctx, make_rect(0, 0, (double)last_frame->w / backing,
                                        (double)last_frame->h / backing), image);
    R.CGImageRelease(image);
  }
  R.CGDataProviderRelease(provider);
  R.CGColorSpaceRelease(space);
}


static BOOL view_yes (id self, SEL cmd) {
  (void)self; (void)cmd;
  return 1;
}


static int mods_of (id event) {
  unsigned long f = ((MsgRetULong)R.objc_msgSend)(event, SELN("modifierFlags"));
  return ((f & (1ul << 17)) ? TM_SHIFT : 0) | ((f & (1ul << 18)) ? TM_CTRL : 0) |
         ((f & (1ul << 19)) ? TM_ALT : 0);
}


static int special_key (unsigned code) {
  static const struct { unsigned code; int key; } map[] = {
    {126, TK_UP}, {125, TK_DOWN}, {123, TK_LEFT}, {124, TK_RIGHT},
    {115, TK_HOME}, {119, TK_END}, {116, TK_PGUP}, {121, TK_PGDN},
    {114, TK_INSERT}, {117, TK_DELETE}, {36, TK_ENTER}, {76, TK_ENTER},
    {48, TK_TAB}, {51, TK_BACKSPACE}, {53, TK_ESCAPE},
    {122, TK_F1}, {120, TK_F1 + 1}, {99, TK_F1 + 2}, {118, TK_F1 + 3},
    {96, TK_F1 + 4}, {97, TK_F1 + 5}, {98, TK_F1 + 6}, {100, TK_F1 + 7},
    {101, TK_F1 + 8}, {109, TK_F1 + 9}, {103, TK_F1 + 10}, {111, TK_F1 + 11}
  };
  size_t i;
  for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
    if (map[i].code == code) return map[i].key;
  return TK_NONE;
}


static void view_key_down (id self, SEL cmd, id event) {
  unsigned long flags = ((MsgRetULong)R.objc_msgSend)(event, SELN("modifierFlags"));
  unsigned code = ((MsgRetUShort)R.objc_msgSend)(event, SELN("keyCode"));
  int mods = mods_of(event), key = special_key(code);
  id chars;
  const char *text;
  (void)self; (void)cmd;
  if (flags & (1ul << 20)) {	/* Command: the Mac way to say Ctrl+Shift */
    id plain = msg(event, "charactersIgnoringModifiers");
    const char *p = plain ? ((MsgRetStr)R.objc_msgSend)(plain, SELN("UTF8String")) : NULL;
    if (p == NULL || p[0] == '\0') return;
    if (p[0] == 'q' || p[0] == 'Q') win_close();
    else if (strchr("=+-0", p[0]) != NULL) app_on_key(TK_CHAR, TM_CTRL, (uint32_t)p[0]);
    else app_on_key(TK_CHAR, TM_CTRL | TM_SHIFT, (uint32_t)p[0]);
    return;
  }
  if (key != TK_NONE) {
    app_on_key(key, mods, 0);
    return;
  }
  chars = msg(event, "characters");
  text = chars ? ((MsgRetStr)R.objc_msgSend)(chars, SELN("UTF8String")) : NULL;
  if (text != NULL && text[0] != '\0') app_on_text(text, 0);
}


static void mouse_event (id self, id event, int type, int button) {
  NSPoint w = ((MsgRetPoint)R.objc_msgSend)(event, SELN("locationInWindow"));
  NSPoint p = ((MsgPointConv)R.objc_msgSend)(self, SELN("convertPoint:fromView:"), w, NULL);
  app_on_mouse(type, button, (int)(p.x * backing), (int)(p.y * backing),
               mods_of(event), 0);
}


static void view_mouse_down (id s, SEL c, id e) { (void)c; mouse_event(s, e, TMS_DOWN, 1); }
static void view_mouse_up (id s, SEL c, id e) { (void)c; mouse_event(s, e, TMS_UP, 1); }
static void view_mouse_move (id s, SEL c, id e) { (void)c; mouse_event(s, e, TMS_MOVE, 0); }
static void view_right_down (id s, SEL c, id e) { (void)c; mouse_event(s, e, TMS_DOWN, 3); }
static void view_other_down (id s, SEL c, id e) { (void)c; mouse_event(s, e, TMS_DOWN, 2); }


static void view_scroll (id self, SEL cmd, id event) {
  double dy = ((MsgRetDouble)R.objc_msgSend)(event, SELN("scrollingDeltaY"));
  BOOL precise = ((BOOL (*)(id, SEL))R.objc_msgSend)(event, SELN("hasPreciseScrollingDeltas"));
  int lines;
  (void)cmd;
  wheel_rest += precise ? dy / 12.0 : dy * 3.0;	/* trackpads send pixels */
  lines = (int)wheel_rest;
  if (lines != 0) {
    NSPoint w = ((MsgRetPoint)R.objc_msgSend)(event, SELN("locationInWindow"));
    NSPoint p = ((MsgPointConv)R.objc_msgSend)(self, SELN("convertPoint:fromView:"), w, NULL);
    wheel_rest -= (double)lines;
    app_on_mouse(TMS_WHEEL, 0, (int)(p.x * backing), (int)(p.y * backing),
                 mods_of(event), lines);
  }
}


static void report_size (void) {
  NSRect b = rect_of(view, "bounds");
  app_on_resize((int)(b.size.w * backing + 0.5), (int)(b.size.h * backing + 0.5));
}


static void del_resized (id self, SEL cmd, id note) {
  (void)self; (void)cmd; (void)note;
  report_size();
}


static void del_key (id self, SEL cmd, id note) {
  (void)self; (void)cmd; (void)note;
  app_on_focus(1);
}


static void del_unkey (id self, SEL cmd, id note) {
  (void)self; (void)cmd; (void)note;
  app_on_focus(0);
}


static BOOL del_should_close (id self, SEL cmd, id sender) {
  (void)self; (void)cmd; (void)sender;
  win_close();
  return 1;
}


static void del_tick (id self, SEL cmd, id timer) {
  struct timespec ts;
  (void)self; (void)cmd; (void)timer;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  app_on_tick((unsigned)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000));
  app_on_wake();	/* the pty is polled: reading never blocks */
  if (need_redraw && view != NULL) {
    need_redraw = 0;
    msg_bool(view, "setNeedsDisplay:", 1);
  }
}


static void add (Class c, const char *sel, AnyFn fn, const char *types) {
  R.class_addMethod(c, SELN(sel), fn, types);
}


static void make_classes (void) {
  const char *ev = "v@:@";
  Class v = R.objc_allocateClassPair((Class)CLS("NSView"), "MmcTermView", 0);
  Class d = R.objc_allocateClassPair((Class)CLS("NSObject"), "MmcTermDelegate", 0);
  add(v, "drawRect:", (AnyFn)view_draw, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
  add(v, "isFlipped", (AnyFn)view_yes, "c@:");
  add(v, "acceptsFirstResponder", (AnyFn)view_yes, "c@:");
  add(v, "keyDown:", (AnyFn)view_key_down, ev);
  add(v, "mouseDown:", (AnyFn)view_mouse_down, ev);
  add(v, "mouseUp:", (AnyFn)view_mouse_up, ev);
  add(v, "mouseDragged:", (AnyFn)view_mouse_move, ev);
  add(v, "mouseMoved:", (AnyFn)view_mouse_move, ev);
  add(v, "rightMouseDown:", (AnyFn)view_right_down, ev);
  add(v, "otherMouseDown:", (AnyFn)view_other_down, ev);
  add(v, "scrollWheel:", (AnyFn)view_scroll, ev);
  R.objc_registerClassPair(v);
  add(d, "windowDidResize:", (AnyFn)del_resized, ev);
  add(d, "windowDidBecomeKey:", (AnyFn)del_key, ev);
  add(d, "windowDidResignKey:", (AnyFn)del_unkey, ev);
  add(d, "windowShouldClose:", (AnyFn)del_should_close, "c@:@");
  add(d, "tick:", (AnyFn)del_tick, ev);
  R.objc_registerClassPair(d);
}

/* }================================================================== */


static int start_app (void) {
  if (app != NULL) return 0;
  if (load_runtime() != 0) {
    fprintf(stderr, TERM_NAME ": cannot load the Cocoa frameworks\n");
    return -1;
  }
  msg(msg(CLS("NSAutoreleasePool"), "alloc"), "init");	/* lives as long as we do */
  app = msg(CLS("NSApplication"), "sharedApplication");
  msg_long(app, "setActivationPolicy:", 0);	/* a normal app with a Dock icon */
  backing = ((MsgRetDouble)R.objc_msgSend)(msg(CLS("NSScreen"), "mainScreen"),
                                           SELN("backingScaleFactor"));
  if (backing < 1.0) backing = 1.0;
  return 0;
}


/* a point is 1/72 inch on the Mac and the core counts in 1/96: 0.75 */
float win_scale (void) {
  start_app();
  return (float)(backing * 0.75);
}


unsigned win_ticks (void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (unsigned)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}


int win_create (int w, int h, const char *title) {
  NSRect r;
  if (start_app() != 0) return -1;
  make_classes();
  r = make_rect(0, 0, (double)w / backing, (double)h / backing);
  window = ((MsgWin)R.objc_msgSend)(msg(CLS("NSWindow"), "alloc"),
             SELN("initWithContentRect:styleMask:backing:defer:"), r,
             15ul /* titled, closable, miniaturizable, resizable */, 2ul, 0);
  if (window == NULL) return -1;
  view = ((MsgRect)R.objc_msgSend)(msg(CLS("MmcTermView"), "alloc"),
                                   SELN("initWithFrame:"), r);
  delegate = msg(msg(CLS("MmcTermDelegate"), "alloc"), "init");
  msg_id(window, "setContentView:", view);
  msg_id(window, "makeFirstResponder:", view);
  msg_id(window, "setDelegate:", delegate);
  msg_bool(window, "setAcceptsMouseMovedEvents:", 1);
  msg_bool(window, "setReleasedWhenClosed:", 0);
  msg_long(window, "setCollectionBehavior:", 1L << 7);	/* may go full screen */
  msg(window, "center");
  win_set_title(title);
  return 0;
}


int win_run (void) {
  if (window == NULL) return 1;
  ((MsgTimer)R.objc_msgSend)(CLS("NSTimer"),
      SELN("scheduledTimerWithTimeInterval:target:selector:userInfo:repeats:"),
      1.0 / 60.0, delegate, SELN("tick:"), NULL, 1);
  msg_id(window, "makeKeyAndOrderFront:", NULL);
  msg_bool(app, "activateIgnoringOtherApps:", 1);
  report_size();
  msg(app, "run");	/* returns only through win_close */
  return app_exit_code();
}


void win_close (void) {
  pty_close();
  exit(app_exit_code());
}


void win_wake (void) { }	/* the timer looks at the pty 60 times a second */
void win_redraw (void) { need_redraw = 1; }


void win_set_title (const char *utf8) {
  if (window != NULL) msg_id(window, "setTitle:", nsstring(utf8));
}


void win_set_size (int w, int h) {
  NSSize s;
  if (window == NULL || is_fullscreen) return;
  s.w = (double)w / backing;
  s.h = (double)h / backing;
  ((MsgSize)R.objc_msgSend)(window, SELN("setContentSize:"), s);
}


void win_set_fullscreen (int on) {
  if (window == NULL || on == is_fullscreen) return;
  is_fullscreen = on;
  msg_id(window, "toggleFullScreen:", NULL);
}


/* title bar in the theme color, with light or dark buttons and text */
void win_set_chrome (uint32_t bg, uint32_t border, uint32_t text, int dark) {
  id color, look;
  (void)border; (void)text;
  if (window == NULL) return;
  color = ((MsgColor)R.objc_msgSend)(CLS("NSColor"),
            SELN("colorWithSRGBRed:green:blue:alpha:"),
            (double)((bg >> 16) & 0xFF) / 255.0, (double)((bg >> 8) & 0xFF) / 255.0,
            (double)(bg & 0xFF) / 255.0, 1.0);
  msg_id(window, "setBackgroundColor:", color);
  msg_bool(window, "setTitlebarAppearsTransparent:", 1);
  look = msg_id(CLS("NSAppearance"), "appearanceNamed:",
                nsstring(dark ? "NSAppearanceNameDarkAqua" : "NSAppearanceNameAqua"));
  if (look != NULL) msg_id(window, "setAppearance:", look);
}


void win_set_opacity (int percent) {
  if (window != NULL)
    ((MsgDouble)R.objc_msgSend)(window, SELN("setAlphaValue:"),
                                (double)(percent > 100 ? 100 : percent) / 100.0);
}


void win_set_clipboard (const char *utf8) {
  id pb = msg(CLS("NSPasteboard"), "generalPasteboard");
  msg(pb, "clearContents");
  ((MsgIdId)R.objc_msgSend)(pb, SELN("setString:forType:"), nsstring(utf8),
                            nsstring("public.utf8-plain-text"));
}


void win_request_paste (void) {
  id pb = msg(CLS("NSPasteboard"), "generalPasteboard");
  id s = msg_id(pb, "stringForType:", nsstring("public.utf8-plain-text"));
  const char *utf8 = s ? ((MsgRetStr)R.objc_msgSend)(s, SELN("UTF8String")) : NULL;
  if (utf8 != NULL) app_on_paste(utf8);
}


void win_flash (void) {
  if (app != NULL) msg_long(app, "requestUserAttention:", 10);
}


void win_message (const char *title, const char *text) {
  id alert;
  if (start_app() != 0) {
    fprintf(stderr, "%s: %s\n", title, text);
    return;
  }
  alert = msg(msg(CLS("NSAlert"), "alloc"), "init");
  msg_id(alert, "setMessageText:", nsstring(title));
  msg_id(alert, "setInformativeText:", nsstring(text));
  msg(alert, "runModal");
}


/* the window manager draws the title bar here */
int win_custom_chrome (int want) { (void)want; return 0; }
void win_minimize (void) { }
void win_toggle_maximize (void) { }
int win_is_maximized (void) { return 0; }

#else

typedef int tcocoa_is_empty_here;	/* ISO C wants something in every file */

#endif
