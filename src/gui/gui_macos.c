/*
 * gui_macos.c — macOS AppKit GUI (CMP-413)
 *
 * Native windowed UI for nanocode on macOS.  Pure C — no .m files.
 * All Cocoa objects are created via the Objective-C runtime API:
 *   objc_getClass(), sel_registerName(), objc_msgSend(), etc.
 *
 * Architecture:
 *   - Main thread is owned by NSApplication (via gui_run → [NSApp run]).
 *   - An NSTimer fires every 16 ms, calling loop_step() to dispatch kqueue
 *     events (provider SSE, etc.) from within the AppKit run loop.
 *   - gui_post_output() is called from loop_step() callbacks → same thread.
 *   - Input: NSTextField + "Send" NSButton; delegate's submitAction: triggers
 *     the registered gui_input_cb.
 *
 * Compile guard: entire file is skipped on non-Apple platforms.
 */

#ifdef __APPLE__

/* Suppress variadic objc_msgSend warnings — intentional typed casts below. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wstrict-prototypes"

#include <objc/objc.h>
#include <objc/runtime.h>
#include <objc/message.h>

#pragma clang diagnostic pop

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../include/gui.h"
#include "../core/loop.h"
#include "../util/arena.h"

/* -------------------------------------------------------------------------
 * CoreGraphics / AppKit types needed without importing framework headers.
 * Using the runtime API only requires the types, not the full headers.
 * ---------------------------------------------------------------------- */

typedef double              CGFloat;
typedef struct { CGFloat x, y; }           CGPoint;
typedef struct { CGFloat width, height; }  CGSize;
typedef struct { CGPoint origin; CGSize size; } CGRect;
typedef CGRect  NSRect;
typedef unsigned long       NSUInteger;
typedef signed long         NSInteger;
/* BOOL, YES, NO are defined in <objc/objc.h> — do not redefine. */

/* NSWindow style mask bits */
#define NSWindowStyleMaskTitled          (1UL << 0)
#define NSWindowStyleMaskClosable        (1UL << 1)
#define NSWindowStyleMaskMiniaturizable  (1UL << 2)
#define NSWindowStyleMaskResizable       (1UL << 3)

/* NSBackingStoreType */
#define NSBackingStoreBuffered  2

/* NSString encoding */
#define NSUTF8StringEncoding  4

/* NSApplicationActivationPolicy */
#define NSApplicationActivationPolicyRegular  0

/* NSAutoresizingMask bits */
#define NSViewMinXMargin    1
#define NSViewWidthSizable  2
#define NSViewMinYMargin    8
#define NSViewHeightSizable 16
#define NSViewMaxYMargin    32

/* NSRange — used for scrollRangeToVisible: */
typedef struct { NSUInteger location; NSUInteger length; } NSRange;

/* -------------------------------------------------------------------------
 * Typed send helpers.
 *
 * On arm64 macOS all objc_msgSend variants collapse to one; on x86_64 the
 * stret variant is needed for struct *returns* > 16 bytes.  We never need
 * to receive an NSRect return value in Phase 1, so one helper suffices.
 *
 * Each helper casts objc_msgSend to the exact function type for the call
 * so argument registers are populated correctly.
 * ---------------------------------------------------------------------- */

/* Send a message returning id, with zero extra arguments. */
static inline id
msg0(id obj, SEL sel)
{
    return ((id (*)(id, SEL))objc_msgSend)(obj, sel);
}

/* Send a message returning void, with zero extra arguments. */
static inline void
msg0v(id obj, SEL sel)
{
    ((void (*)(id, SEL))objc_msgSend)(obj, sel);
}

/* Send with one id argument, returning void. */
static inline void
msg1idv(id obj, SEL sel, id a)
{
    ((void (*)(id, SEL, id))objc_msgSend)(obj, sel, a);
}

/* Send with one BOOL argument, returning void. */
static inline void
msg1bv(id obj, SEL sel, BOOL a)
{
    ((void (*)(id, SEL, BOOL))objc_msgSend)(obj, sel, a);
}

/* Send with one NSUInteger argument, returning void. */
static inline void
msg1uiv(id obj, SEL sel, NSUInteger a)
{
    ((void (*)(id, SEL, NSUInteger))objc_msgSend)(obj, sel, a);
}

/* Send with one SEL argument, returning void. */
static inline void
msg1selv(id obj, SEL sel, SEL a)
{
    ((void (*)(id, SEL, SEL))objc_msgSend)(obj, sel, a);
}

/* Send with one NSInteger argument, returning void. */
static inline void
msg1longv(id obj, SEL sel, NSInteger a)
{
    ((void (*)(id, SEL, NSInteger))objc_msgSend)(obj, sel, a);
}

/* Send with one CGFloat argument, returning id. */
static inline id
msg1fid(id obj, SEL sel, CGFloat a)
{
    return ((id (*)(id, SEL, CGFloat))objc_msgSend)(obj, sel, a);
}

/* Send with NSRange argument, returning void.
 * NSRange = {NSUInteger, NSUInteger} = 16 bytes on 64-bit.
 * Fits in two general-purpose registers on both arm64 and x86_64,
 * so regular objc_msgSend handles it correctly on both ABIs. */
static inline void
msg1rangev(id obj, SEL sel, NSRange r)
{
    ((void (*)(id, SEL, NSRange))objc_msgSend)(obj, sel, r);
}

/* NSString from a C string literal. */
static id
nsstr(const char *s)
{
    return ((id (*)(id, SEL, const char *))objc_msgSend)(
        (id)objc_getClass("NSString"),
        sel_registerName("stringWithUTF8String:"),
        s);
}

/* NSString from bytes + length (for non-NUL-terminated buffers). */
static id
nsstr_bytes(const char *data, size_t len)
{
    return ((id (*)(id, SEL, const void *, NSUInteger, NSUInteger))objc_msgSend)(
        (id)objc_getClass("NSString"),
        sel_registerName("stringWithBytes:length:encoding:"),
        (const void *)data, (NSUInteger)len, (NSUInteger)NSUTF8StringEncoding);
}

/* -------------------------------------------------------------------------
 * initWithFrame: typed sender (NSRect argument, id return).
 * NSRect (32 bytes on 64-bit) is an argument here, not a return value —
 * the ABI passes it on the stack / in fp registers safely.
 * ---------------------------------------------------------------------- */
typedef id (*initWithFrame_t)(id, SEL, NSRect);

static id
alloc_init_frame(const char *class_name, NSRect frame)
{
    id alloc = msg0((id)objc_getClass(class_name),
                    sel_registerName("alloc"));
    return ((initWithFrame_t)objc_msgSend)(alloc,
                                           sel_registerName("initWithFrame:"),
                                           frame);
}

/* -------------------------------------------------------------------------
 * GuiWindow struct (arena-allocated)
 * ---------------------------------------------------------------------- */
struct GuiWindow {
    Loop          *loop;
    Arena         *arena;
    id             app;
    id             window;
    id             text_view;
    id             text_field;
    id             text_storage;
    gui_input_cb   on_input;
    void          *input_ctx;
    char           model[64];
    int            in_tokens;
    int            out_tokens;
};

/* Module-level pointer for use in ObjC delegate callbacks. */
static GuiWindow *g_gui_win = NULL;

/* -------------------------------------------------------------------------
 * Delegate callbacks (registered as IMP on the NanocodeDelegate class)
 * ---------------------------------------------------------------------- */

/* timerFired: — drive kqueue loop every 16 ms from NSApp runloop. */
static void
cb_timer_fired(id self, SEL cmd, id timer)
{
    (void)self; (void)cmd; (void)timer;
    if (g_gui_win && g_gui_win->loop)
        loop_step(g_gui_win->loop, 0);
}

/* windowWillClose: — user closed the window; stop loop and terminate app. */
static void
cb_window_will_close(id self, SEL cmd, id notification)
{
    (void)self; (void)cmd; (void)notification;
    if (g_gui_win && g_gui_win->loop)
        loop_stop(g_gui_win->loop);
    if (g_gui_win && g_gui_win->app)
        msg1idv(g_gui_win->app, sel_registerName("terminate:"), nil);
}

/* submitAction: — Enter in text field or Send button click. */
static void
cb_submit_action(id self, SEL cmd, id sender)
{
    (void)self; (void)cmd; (void)sender;
    GuiWindow *win = g_gui_win;
    if (!win) return;

    id str_val = msg0(win->text_field, sel_registerName("stringValue"));
    const char *text = ((const char *(*)(id, SEL))objc_msgSend)(
        str_val, sel_registerName("UTF8String"));
    if (!text || text[0] == '\0') return;

    size_t len = strlen(text);
    if (win->on_input)
        win->on_input(text, len, win->input_ctx);

    /* Clear the input field. */
    msg1idv(win->text_field, sel_registerName("setStringValue:"), nsstr(""));
}

/* -------------------------------------------------------------------------
 * gui_init
 * ---------------------------------------------------------------------- */
GuiWindow *
gui_init(Loop *loop, Arena *arena)
{
    GuiWindow *win = (GuiWindow *)arena_alloc(arena, sizeof(GuiWindow));
    if (!win) return NULL;
    memset(win, 0, sizeof(*win));
    win->loop  = loop;
    win->arena = arena;
    g_gui_win  = win;

    /* ------------------------------------------------------------------
     * Build delegate class at runtime.
     * ------------------------------------------------------------------ */
    Class DelCls = objc_allocateClassPair(
        objc_getClass("NSObject"), "NanocodeDelegate", 0);
    if (!DelCls) return NULL;

    class_addMethod(DelCls, sel_registerName("timerFired:"),
                    (IMP)cb_timer_fired,       "v@:@");
    class_addMethod(DelCls, sel_registerName("windowWillClose:"),
                    (IMP)cb_window_will_close, "v@:@");
    class_addMethod(DelCls, sel_registerName("submitAction:"),
                    (IMP)cb_submit_action,     "v@:@");
    objc_registerClassPair(DelCls);

    id delegate = msg0(msg0((id)DelCls, sel_registerName("alloc")),
                       sel_registerName("init"));

    /* ------------------------------------------------------------------
     * NSApplication
     * ------------------------------------------------------------------ */
    id app = msg0((id)objc_getClass("NSApplication"),
                  sel_registerName("sharedApplication"));
    win->app = app;
    msg1longv(app, sel_registerName("setActivationPolicy:"),
              (NSInteger)NSApplicationActivationPolicyRegular);
    msg1idv(app, sel_registerName("setDelegate:"), delegate);

    /* ------------------------------------------------------------------
     * NSWindow
     * ------------------------------------------------------------------ */
    NSRect win_frame = {{100.0, 100.0}, {900.0, 650.0}};
    NSUInteger style = NSWindowStyleMaskTitled    |
                       NSWindowStyleMaskClosable  |
                       NSWindowStyleMaskMiniaturizable |
                       NSWindowStyleMaskResizable;

    id wobj;
    {
        typedef id (*initcr_t)(id, SEL, NSRect, NSUInteger, NSUInteger, BOOL);
        id alloc_w = msg0((id)objc_getClass("NSWindow"),
                          sel_registerName("alloc"));
        wobj = ((initcr_t)objc_msgSend)(
            alloc_w,
            sel_registerName("initWithContentRect:styleMask:backing:defer:"),
            win_frame, style, (NSUInteger)NSBackingStoreBuffered, NO);
    }
    win->window = wobj;
    msg1idv(wobj, sel_registerName("setTitle:"), nsstr("nanocode"));
    msg1idv(wobj, sel_registerName("setDelegate:"), delegate);

    id content_view = msg0(wobj, sel_registerName("contentView"));

    /* ------------------------------------------------------------------
     * Monospace font (13 pt)
     * ------------------------------------------------------------------ */
    id font = msg1fid((id)objc_getClass("NSFont"),
                      sel_registerName("userFixedPitchFontOfSize:"),
                      13.0);

    /* ------------------------------------------------------------------
     * NSScrollView + NSTextView (upper 600 px)
     * ------------------------------------------------------------------ */
    NSRect scroll_frame = {{0.0, 50.0}, {900.0, 600.0}};
    id scroll = alloc_init_frame("NSScrollView", scroll_frame);
    msg1bv(scroll, sel_registerName("setHasVerticalScroller:"),   YES);
    msg1bv(scroll, sel_registerName("setAutohidesScrollers:"),    YES);
    msg1bv(scroll, sel_registerName("setHasHorizontalScroller:"), NO);
    /* Resize with window: grow in both dimensions, bottom margin fixed at 50 */
    msg1uiv(scroll, sel_registerName("setAutoresizingMask:"),
            (NSUInteger)(NSViewWidthSizable | NSViewHeightSizable));

    NSRect tv_frame = {{0.0, 0.0}, {900.0, 600.0}};
    id text_view = alloc_init_frame("NSTextView", tv_frame);
    msg1bv(text_view, sel_registerName("setEditable:"),   NO);
    msg1bv(text_view, sel_registerName("setSelectable:"), YES);
    msg1bv(text_view, sel_registerName("setRichText:"),   NO);
    msg1idv(text_view, sel_registerName("setFont:"), font);
    msg1uiv(text_view, sel_registerName("setAutoresizingMask:"),
            (NSUInteger)(NSViewWidthSizable | NSViewHeightSizable));

    id text_storage = msg0(text_view, sel_registerName("textStorage"));
    win->text_view    = text_view;
    win->text_storage = text_storage;

    msg1idv(scroll, sel_registerName("setDocumentView:"), text_view);
    msg1idv(content_view, sel_registerName("addSubview:"), scroll);

    /* ------------------------------------------------------------------
     * NSTextField input (bottom 50 px, width = 770)
     * ------------------------------------------------------------------ */
    NSRect tf_frame = {{0.0, 3.0}, {770.0, 44.0}};
    id text_field = alloc_init_frame("NSTextField", tf_frame);
    msg1idv(text_field, sel_registerName("setFont:"), font);
    msg1idv(text_field, sel_registerName("setTarget:"), delegate);
    msg1selv(text_field, sel_registerName("setAction:"),
             sel_registerName("submitAction:"));
    /* Resize width with window; stay at bottom */
    msg1uiv(text_field, sel_registerName("setAutoresizingMask:"),
            (NSUInteger)(NSViewWidthSizable | NSViewMaxYMargin));
    win->text_field = text_field;
    msg1idv(content_view, sel_registerName("addSubview:"), text_field);

    /* ------------------------------------------------------------------
     * NSButton "Send" (bottom right, 130 × 50)
     * ------------------------------------------------------------------ */
    NSRect btn_frame = {{770.0, 3.0}, {130.0, 44.0}};
    id button = alloc_init_frame("NSButton", btn_frame);
    msg1idv(button, sel_registerName("setTitle:"), nsstr("Send"));
    msg1idv(button, sel_registerName("setTarget:"), delegate);
    msg1selv(button, sel_registerName("setAction:"),
             sel_registerName("submitAction:"));
    /* Stay at right edge, bottom */
    msg1uiv(button, sel_registerName("setAutoresizingMask:"),
            (NSUInteger)(NSViewMinXMargin | NSViewMaxYMargin));
    msg1idv(content_view, sel_registerName("addSubview:"), button);

    /* ------------------------------------------------------------------
     * NSTimer — 16 ms repeat → drives loop_step()
     * ------------------------------------------------------------------ */
    {
        typedef id (*sched_t)(id, SEL, double, id, SEL, id, BOOL);
        ((sched_t)objc_msgSend)(
            (id)objc_getClass("NSTimer"),
            sel_registerName("scheduledTimerWithTimeInterval:"
                             "target:selector:userInfo:repeats:"),
            0.016, delegate,
            sel_registerName("timerFired:"),
            nil, YES);
    }

    return win;
}

/* -------------------------------------------------------------------------
 * gui_run — hand control to NSApp; blocks until window is closed.
 * ---------------------------------------------------------------------- */
void
gui_run(GuiWindow *win)
{
    if (!win) return;
    msg1idv(win->window, sel_registerName("makeKeyAndOrderFront:"), nil);
    msg1bv(win->app, sel_registerName("activateIgnoringOtherApps:"), YES);
    msg0v(win->app, sel_registerName("run"));
}

/* -------------------------------------------------------------------------
 * gui_post_output — append text tokens to the NSTextView.
 * ---------------------------------------------------------------------- */
void
gui_post_output(GuiWindow *win, const char *text, size_t len)
{
    if (!win || !text || len == 0) return;

    id str = nsstr_bytes(text, len);
    if (!str) return;

    /* Empty attributes dictionary (plain text). */
    id attrs = msg0((id)objc_getClass("NSDictionary"),
                    sel_registerName("dictionary"));

    id astr = msg0((id)objc_getClass("NSAttributedString"),
                   sel_registerName("alloc"));
    astr = ((id (*)(id, SEL, id, id))objc_msgSend)(
        astr,
        sel_registerName("initWithString:attributes:"),
        str, attrs);

    id ts = win->text_storage;
    msg0v(ts, sel_registerName("beginEditing"));
    msg1idv(ts, sel_registerName("appendAttributedString:"), astr);
    msg0v(ts, sel_registerName("endEditing"));

    /* Release the attributed string (retain count was 1 from alloc/init). */
    msg0v(astr, sel_registerName("release"));

    /* Scroll to end. */
    NSUInteger ts_len = ((NSUInteger (*)(id, SEL))objc_msgSend)(
        ts, sel_registerName("length"));
    NSRange end = {ts_len, 0};
    msg1rangev(win->text_view, sel_registerName("scrollRangeToVisible:"), end);
}

/* -------------------------------------------------------------------------
 * gui_set_input_callback
 * ---------------------------------------------------------------------- */
void
gui_set_input_callback(GuiWindow *win, gui_input_cb cb, void *ctx)
{
    if (!win) return;
    win->on_input   = cb;
    win->input_ctx  = ctx;
}

/* -------------------------------------------------------------------------
 * gui_update_title
 * ---------------------------------------------------------------------- */
void
gui_update_title(GuiWindow *win, const char *model,
                 int in_tokens, int out_tokens)
{
    if (!win) return;
    char buf[256];
    snprintf(buf, sizeof(buf), "nanocode — %s  [in:%d out:%d]",
             model ? model : "?", in_tokens, out_tokens);
    msg1idv(win->window, sel_registerName("setTitle:"), nsstr(buf));
}

/* -------------------------------------------------------------------------
 * gui_destroy
 * ---------------------------------------------------------------------- */
void
gui_destroy(GuiWindow *win)
{
    (void)win;
    g_gui_win = NULL;
}

#endif /* __APPLE__ */
