/*
 * ui.h - the phase-15 UI contract: compositor protocol, layout
 * constants, immediate-mode widget helpers and the client library.
 *
 * Architecture (plan item 79): the compositor daemon owns /dev/fb0
 * and /dev/event0. App windows are shm objects (one per window);
 * an app draws into its surface with the ui_gfx helpers and asks
 * the compositor to composite it with UI_FLUSH(dirty rect). The
 * compositor forwards input records to the focused window and
 * shows status bar + notification banners itself. Everything
 * crosses the boundary as fixed 32-byte messages on the phase-8
 * unix transport (/var/run/ui), events echoed as UI_EVENT with the
 * kernel's input_event fields packed into a/b/c.
 */

#ifndef UI_H
#define UI_H

#include "libc.h"

typedef unsigned char  ui_u8;
typedef unsigned char  u8;
typedef signed   int   i32;
typedef unsigned int   u32;
typedef unsigned short u16;

/* ---- layout constants (shared by compositor + apps) --------------- */

#define UI_SCREEN_W     800u
#define UI_SCREEN_H     600u
#define UI_STATUS_H      24u
#define UI_TITLE_H       20u

/* app window geometry: compositor assigns this box on OPEN          */
#define UI_WIN_W        480u
#define UI_WIN_H        360u
#define UI_WIN_X        ((UI_SCREEN_W - UI_WIN_W) / 2u)
#define UI_WIN_Y        (UI_STATUS_H + 32u)

/* home-screen launcher grid (compositor-owned)                      */
#define UI_ICON_W       150u
#define UI_ICON_H        64u

/* ---- wire protocol ------------------------------------------------- */

struct ui_msg {
    u32 type;
    u32 win;                    /* window id                     */
    i32 a, b, c, d;             /* type-specific                 */
    char text[12];              /* type-specific, NUL-padded     */
} __attribute__((packed));      /* 32 bytes                      */

#define UI_MSG_SIZE     32u

enum ui_msg_type {
    UI_HELLO     = 1,           /* c->m: register, text = name   */
    UI_WELCOME   = 2,           /* m->c: a=screen w, b=screen h  */
    UI_OPEN      = 3,           /* c->m: a=w, b=h                */
    UI_OPENED    = 4,           /* m->c: a=shm id, b=x, c=y      */
    UI_CLOSE     = 5,           /* c->m: detach + forget         */
    UI_FLUSH     = 6,           /* c->m: dirty rect a,b,c,d      */
    UI_SHOW      = 7,           /* c->m: make visible + focus    */
    UI_HIDE      = 8,           /* c->m                          */
    UI_ACTIVATE  = 9,           /* m->c: b=1 show, 0 hide        */
    UI_EVENT     = 10,          /* m->c: a=type b=code c=value   */
    UI_FOCUS     = 11,          /* m->c: a=1 gained, 0 lost      */
    UI_NOTIFY    = 12,          /* c->m: banner, text + a=kind   */
    UI_CLOSED    = 13,          /* m->c: your window was dropped */
};

#define UI_NOTIFY_SMS    1u
#define UI_NOTIFY_CALL   2u
#define UI_NOTIFY_INFO   3u

/* ---- gfx (a surface is packed XRGB8888 rows) ----------------------- */

struct ui_surface {
    u32 *px;
    u32  w, h;
};

void ui_fill(struct ui_surface *s, u32 x, u32 y, u32 w, u32 h, u32 c);
void ui_rect(struct ui_surface *s, u32 x, u32 y, u32 w, u32 h, u32 c);
void ui_text(struct ui_surface *s, u32 x, u32 y, const char *str,
             u32 fg, u32 bg);
u32  ui_text_w(const char *str);

/* ---- theme (item 80) ------------------------------------------------ */

struct ui_theme {
    u32 bg, fg;                 /* window background / text       */
    u32 accent;                 /* highlights, focus ring         */
    u32 panel;                  /* bars, list background          */
    u32 btn, btn_text;          /* button face / label            */
    u32 border;                 /* outlines                       */
    u32 ok, warn;               /* semantic colors                */
};

extern const struct ui_theme ui_theme_dark;

/* ---- widgets (immediate-mode draw + hit test) ----------------------- */

struct ui_view {
    i32 x, y, w, h;
    u16 id;                     /* app-local control id           */
    bool pressed;
};

bool ui_hit(const struct ui_view *v, i32 x, i32 y);

void ui_label(struct ui_surface *s, i32 x, i32 y, const char *txt,
              u32 fg);
void ui_button(struct ui_surface *s, const struct ui_view *v,
               const char *txt, bool pressed);
void ui_list(struct ui_surface *s, const struct ui_view *v,
             char *const *items, unsigned n, unsigned sel);

/* ---- on-screen keyboard (item 84) ------------------------------------ */

struct ui_kbd {
    struct ui_view v;           /* whole keyboard box             */
    bool shift;                 /* unused today; caps layout only */
    char last;                  /* last key delivered, 0 = none   */
};

void ui_kbd_init(struct ui_kbd *k, i32 x, i32 y, i32 w, i32 h);
/* draws; on a tap inside, stores the char in k->last              */
void ui_kbd_draw(struct ui_surface *s, struct ui_kbd *k);
/* true when the tap (surface coords) hit a key; char in *out      */
bool ui_kbd_tap(struct ui_kbd *k, i32 x, i32 y, char *out);

/* ---- client library -------------------------------------------------- */

struct ui_client {
    int  fd;                    /* compositor socket              */
    u32  win;                   /* window id, 0 until OPENED      */
    int  shm_id;
    i32  x, y;                  /* window position on screen      */
    struct ui_surface surf;     /* app-facing surface             */
};

int  ui_connect(struct ui_client *c, const char *name, u32 w, u32 h);
int  ui_flush(struct ui_client *c, u32 x, u32 y, u32 w, u32 h);
int  ui_flush_all(struct ui_client *c);
int  ui_show(struct ui_client *c);
int  ui_hide(struct ui_client *c);
int  ui_notify(struct ui_client *c, const char *text, u32 kind);
/* blocks for one message; returns the type or <0 on error         */
int  ui_next(struct ui_client *c, struct ui_msg *m);
int  ui_send(struct ui_client *c, u32 type, i32 a, i32 b, i32 c2,
             i32 d, const char *text);

#endif /* UI_H */
