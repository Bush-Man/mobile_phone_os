/*
 * ui_widgets.c - immediate-mode widgets + themes (phase 15,
 * plan items 80 and 84).
 *
 * Apps keep their own state and redraw the whole surface when it
 * changes; these helpers do the drawing and the hit-testing. The
 * on-screen keyboard is a four-row component (digits, two letter
 * rows, punctuation) whose tap handler delivers one character per
 * press -- 8 for backspace.
 */

#include "ui.h"

const struct ui_theme ui_theme_dark = {
    .bg       = 0xff101418u,
    .fg       = 0xffe8e8e8u,
    .accent   = 0xff2f81f7u,
    .panel    = 0xff1c2128u,
    .btn      = 0xff2d333bu,
    .btn_text = 0xffffffffu,
    .border   = 0xff3d444du,
    .ok       = 0xff3fb950u,
    .warn     = 0xfff0883eu,
};

bool ui_hit(const struct ui_view *v, i32 x, i32 y)
{
    return v && x >= v->x && x < v->x + v->w &&
           y >= v->y && y < v->y + v->h;
}

void ui_label(struct ui_surface *s, i32 x, i32 y, const char *txt,
              u32 fg)
{
    ui_text(s, (u32)x, (u32)y, txt, fg, 0);
}

void ui_button(struct ui_surface *s, const struct ui_view *v,
               const char *txt, bool pressed)
{
    u32 face = pressed ? ui_theme_dark.accent : ui_theme_dark.btn;
    u32 x = (u32)v->x, y = (u32)v->y;

    ui_fill(s, x, y, (u32)v->w, (u32)v->h, face);
    ui_rect(s, x, y, (u32)v->w, (u32)v->h, ui_theme_dark.border);

    if (txt) {
        u32 tw = ui_text_w(txt);
        u32 tx = x + ((u32)v->w > tw ? ((u32)v->w - tw) / 2u : 0);
        u32 ty = y + ((u32)v->h > 8u ? ((u32)v->h - 8u) / 2u : 0);

        ui_text(s, tx, ty, txt, ui_theme_dark.btn_text, face);
    }
}

void ui_list(struct ui_surface *s, const struct ui_view *v,
             char *const *items, unsigned n, unsigned sel)
{
    u32 rowh = 16u;
    u32 maxrows = (u32)v->h / rowh;

    ui_fill(s, (u32)v->x, (u32)v->y, (u32)v->w, (u32)v->h,
            ui_theme_dark.panel);
    ui_rect(s, (u32)v->x, (u32)v->y, (u32)v->w, (u32)v->h,
            ui_theme_dark.border);

    for (unsigned i = 0; i < n && i < maxrows; i++) {
        u32 ry = (u32)v->y + i * rowh;
        bool is_sel = (i == sel);

        if (is_sel)
            ui_fill(s, (u32)v->x + 1u, ry, (u32)v->w - 2u, rowh,
                    ui_theme_dark.accent);
        if (items[i])
            ui_text(s, (u32)v->x + 4u, ry + 4u, items[i],
                    is_sel ? 0xffffffffu : ui_theme_dark.fg,
                    is_sel ? ui_theme_dark.accent
                           : ui_theme_dark.panel);
    }
}

/* ---- on-screen keyboard ------------------------------------------------ */

#define KBD_ROWS 4
#define KBD_COLS 10

static const char kbd_keys[KBD_ROWS][KBD_COLS + 1] = {
    "1234567890",
    "QWERTYUIOP",
    "ASDFGHJKL-",
    "ZXCVBNM.,\x08",                /* \x08 = backspace        */
};

void ui_kbd_init(struct ui_kbd *k, i32 x, i32 y, i32 w, i32 h)
{
    k->v.x = x;
    k->v.y = y;
    k->v.w = w;
    k->v.h = h;
    k->v.id = 0;
    k->shift = false;
    k->last = 0;
}

void ui_kbd_draw(struct ui_surface *s, struct ui_kbd *k)
{
    u32 kw = (u32)k->v.w / KBD_COLS;
    u32 kh = (u32)k->v.h / KBD_ROWS;

    ui_fill(s, (u32)k->v.x, (u32)k->v.y,
            (u32)k->v.w, (u32)k->v.h, ui_theme_dark.panel);

    for (u32 r = 0; r < KBD_ROWS; r++) {
        for (u32 c2 = 0; c2 < KBD_COLS; c2++) {
            char ch = kbd_keys[r][c2];
            char lab[2] = { ch == '\x08' ? '<' : ch, 0 };
            struct ui_view key = {
                (i32)(k->v.x + c2 * (i32)kw + 1),
                (i32)(k->v.y + r * (i32)kh + 1),
                (i32)kw - 2, (i32)kh - 2, 0, false,
            };

            ui_button(s, &key, lab, false);
        }
    }
}

bool ui_kbd_tap(struct ui_kbd *k, i32 x, i32 y, char *out)
{
    u32 kw = (u32)k->v.w / KBD_COLS;
    u32 kh = (u32)k->v.h / KBD_ROWS;
    i32 relx, rely;

    if (!ui_hit(&k->v, x, y))
        return false;

    relx = x - k->v.x;
    rely = y - k->v.y;
    if (relx < 0 || rely < 0)
        return false;

    {
        u32 r = (u32)rely / kh;
        u32 c2 = (u32)relx / kw;

        if (r >= KBD_ROWS || c2 >= KBD_COLS)
            return false;
        *out = kbd_keys[r][c2];
        k->last = *out;
        return true;
    }
}
