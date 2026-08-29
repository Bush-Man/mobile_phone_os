/*
 * calc.c - the calculator (phase 15, plan item 82).
 *
 * Immediate-execution arithmetic over 64-bit signed integers (the
 * libc/kernel are -mgeneral-regs-only: no FP anywhere). Digit keys
 * build the entry; operator keys fold the pending operation into
 * the accumulator, '=' shows the total, C resets. Division by
 * zero parks an error in the display until the next C.
 */

#include "libc.h"
#include "ui.h"

static struct ui_client ui;

static i64 acc;
static char op = '+';
static char cur[20];
static bool fresh;              /* cur holds a folded result    */
static bool err;

/* rows: 7 8 9 / | 4 5 6 * | 1 2 3 - | C 0 = +                     */
static const char calc_keys[] = "789/456*123-C0=+";

#define CK_X0 12
#define CK_Y0 60
#define CK_W  104
#define CK_H  56
#define CK_DX 112
#define CK_DY 64

/* tiny signed parser (libc strtoul is unsigned-only)             */
static i64 str_to_i64(const char *s)
{
    i64 v = 0;
    bool neg = *s == '-';

    if (neg)
        s++;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

static void fmt_i64(i64 v)
{
    snprintf(cur, sizeof(cur), "%lld", (long long)v);
}

static i64 apply(i64 a, char o, i64 b, bool *e)
{
    switch (o) {
    case '-': return a - b;
    case '*': return a * b;
    case '/':
        if (!b) {
            *e = true;
            return 0;
        }
        return a / b;
    default:  return a + b;
    }
}

static void press(char k)
{
    if (err) {
        if (k != 'C')
            return;
        err = false;
        acc = 0;
        op = '+';
        cur[0] = 0;
        fresh = false;
        return;
    }

    if (k >= '0' && k <= '9') {
        size_t n = strlen(cur);

        if (fresh) {
            cur[0] = 0;
            n = 0;
            fresh = false;
        }
        if (n + 1 < sizeof(cur)) {
            cur[n] = k;
            cur[n + 1] = 0;
        }
        return;
    }

    if (k == 'C') {
        acc = 0;
        op = '+';
        cur[0] = 0;
        fresh = false;
        return;
    }

    /* operator or '=': fold pending, show the accumulator         */
    {
        i64 v = cur[0] ? str_to_i64(cur) : acc;

        acc = apply(acc, op, v, &err);
        fmt_i64(acc);
        fresh = true;
        if (k == '=')
            op = '+';
        else
            op = k;
    }
}

static void draw(void)
{
    ui_fill(&ui.surf, 0, 0, ui.surf.w, ui.surf.h,
            ui_theme_dark.bg);

    ui_fill(&ui.surf, 8, 8, ui.surf.w - 16, 40,
            ui_theme_dark.panel);
    ui_rect(&ui.surf, 8, 8, ui.surf.w - 16, 40,
            ui_theme_dark.border);
    {
        const char *shown = err ? "DIV BY ZERO"
                                : (cur[0] ? cur : "0");

        ui_text(&ui.surf,
                ui.surf.w - 16u - ui_text_w(shown), 24, shown,
                err ? ui_theme_dark.warn : ui_theme_dark.fg,
                ui_theme_dark.panel);
    }

    for (u32 i = 0; i < 16; i++) {
        u32 r = i / 4u, c = i % 4u;
        char lab[2] = { calc_keys[i], 0 };
        bool is_op = (calc_keys[i] < '0' || calc_keys[i] > '9');

        ui_button(&ui.surf,
                  &(struct ui_view){
                      (i32)(CK_X0 + c * CK_DX),
                      (i32)(CK_Y0 + r * CK_DY),
                      (i32)CK_W, (i32)CK_H, 0, false },
                  lab, false);
        if (is_op && calc_keys[i] != 'C')
            ui_rect(&ui.surf, CK_X0 + c * CK_DX,
                    CK_Y0 + r * CK_DY, CK_W, CK_H,
                    ui_theme_dark.accent);
    }

    ui_flush_all(&ui);
}

static void tap(i32 x, i32 y)
{
    if (x < CK_X0 || y < CK_Y0)
        return;
    {
        u32 c = (u32)(x - CK_X0) / CK_DX;
        u32 r = (u32)(y - CK_Y0) / CK_DY;

        if (r < 4u && c < 4u) {
            i32 kx = CK_X0 + (i32)(c * CK_DX);
            i32 ky = CK_Y0 + (i32)(r * CK_DY);

            if (x < kx + (i32)CK_W && y < ky + (i32)CK_H)
                press(calc_keys[r * 4u + c]);
        }
    }
}

int main(int argc, char **argv, char **envp)
{
    i32 ex = -1, ey = -1;

    (void)argc;
    (void)argv;
    (void)envp;

    if (ui_connect(&ui, "calc", UI_WIN_W, UI_WIN_H) < 0) {
        printf("[calc] no compositor\n");
        return 1;
    }
    printf("[calc] ready (window %u)\n", ui.win);

    draw();

    for (;;) {
        struct ui_msg m;
        int ty = ui_next(&ui, &m);

        if (ty < 0) {
            printf("[calc] compositor gone\n");
            return 1;
        }
        if (ty == UI_EVENT) {
            if (m.a == (i32)UI_EV_ABS && m.b == (i32)UI_ABS_X)
                ex = m.c;
            else if (m.a == (i32)UI_EV_ABS &&
                     m.b == (i32)UI_ABS_Y)
                ey = m.c;
            else if (m.a == (i32)UI_EV_KEY &&
                     m.b == (i32)UI_BTN_TOUCH &&
                     m.c == 1 && ex >= 0 && ey >= 0)
                tap(ex, ey);
            draw();
        } else if (ty == UI_FOCUS && m.a == 1) {
            draw();
        } else if (ty == UI_CLOSED) {
            return 0;
        }
    }
}
