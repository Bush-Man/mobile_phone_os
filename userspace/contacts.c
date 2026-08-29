/*
 * contacts.c - the address book (phase 15, plan item 82).
 *
 * A filesystem-backed DB: one line per entry, "name|number\n", in
 * /var/contacts. Loaded at start, rewritten on every ADD/DEL, so
 * entries survive app restarts (and reboots on a persisted root).
 * Editing fields uses the on-screen keyboard; a tap on a list row
 * selects the DEL target.
 */

#include "libc.h"
#include "ui.h"

#define DB_PATH   "/var/contacts"
#define MAX_CONTS 24

struct contact {
    char name[16];
    char num[16];
};

static struct ui_client ui;
static struct contact conts[MAX_CONTS];
static unsigned nconts;
static int sel = -1;
static char name_field[16], num_field[16];
static int field;                       /* 0 none, 1 name, 2 num  */

static void save_db(void)
{
    int fd = open(DB_PATH, O_CREAT | O_WRONLY | O_TRUNC);
    char line[40];

    if (fd < 0) {
        printf("[contacts] save failed (%d)\n", fd);
        return;
    }
    for (unsigned i = 0; i < nconts; i++) {
        int n = snprintf(line, sizeof(line), "%s|%s\n",
                         conts[i].name, conts[i].num);

        write(fd, line, (size_t)n);
    }
    close(fd);
}

static void load_db(void)
{
    int fd = open(DB_PATH, O_RDONLY);
    char buf[1024];
    i64 r;

    if (fd < 0)
        return;
    r = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (r <= 0)
        return;
    buf[r] = 0;

    char *p = buf;

    while (*p && nconts < MAX_CONTS) {
        char *nl = strchr(p, '\n');
        char *bar;

        if (nl)
            *nl = 0;
        bar = strchr(p, '|');
        if (bar) {
            *bar = 0;
            snprintf(conts[nconts].name,
                     sizeof(conts[nconts].name), "%s", p);
            snprintf(conts[nconts].num,
                     sizeof(conts[nconts].num), "%s", bar + 1);
            nconts++;
        }
        if (!nl)
            break;
        p = nl + 1;
    }
    printf("[contacts] loaded %u from %s\n", nconts, DB_PATH);
}

/* ---- drawing ------------------------------------------------------------ */

static void draw_field(i32 x, i32 y, i32 w, const char *txt,
                       bool focus)
{
    ui_fill(&ui.surf, (u32)x, (u32)y, (u32)w, 24,
            ui_theme_dark.panel);
    ui_rect(&ui.surf, (u32)x, (u32)y, (u32)w, 24,
            focus ? ui_theme_dark.accent : ui_theme_dark.border);
    ui_text(&ui.surf, (u32)x + 4, (u32)y + 8,
            txt[0] ? txt : "...",
            focus ? ui_theme_dark.accent : ui_theme_dark.fg,
            ui_theme_dark.panel);
}

static void draw(void)
{
    ui_fill(&ui.surf, 0, 0, ui.surf.w, ui.surf.h,
            ui_theme_dark.bg);
    ui_text(&ui.surf, 8, 0, "CONTACTS", ui_theme_dark.fg,
            ui_theme_dark.bg);

    /* list pane: tap a row to select the DEL target             */
    ui_fill(&ui.surf, 8, 12, 300, 190, ui_theme_dark.panel);
    ui_rect(&ui.surf, 8, 12, 300, 190, ui_theme_dark.border);
    for (unsigned i = 0; i < nconts && i < 11u; i++) {
        u32 y = 14u + i * 17u;
        char row[44];
        bool is_sel = (int)i == sel;

        if (is_sel)
            ui_fill(&ui.surf, 9, y, 298, 16,
                    ui_theme_dark.accent);
        snprintf(row, sizeof(row), "%s %s", conts[i].name,
                 conts[i].num);
        ui_text(&ui.surf, 12, y + 4, row,
                is_sel ? 0xffffffffu : ui_theme_dark.fg,
                is_sel ? ui_theme_dark.accent
                       : ui_theme_dark.panel);
    }

    draw_field(316, 12, 156, name_field, field == 1);
    draw_field(316, 40, 156, num_field, field == 2);
    ui_button(&ui.surf,
              &(struct ui_view){ 316, 68, 76, 26, 0, false },
              "ADD", false);
    ui_button(&ui.surf,
              &(struct ui_view){ 396, 68, 76, 26, 0, false },
              "DEL", false);

    {
        struct ui_kbd k;

        ui_kbd_init(&k, 0, 210, (i32)ui.surf.w, 140);
        ui_kbd_draw(&ui.surf, &k);
    }

    ui_flush_all(&ui);
}

/* ---- taps ------------------------------------------------------------------ */

static void kbd_char(char c)
{
    char *f = field == 1 ? name_field : num_field;
    size_t cap = field == 1 ? sizeof(name_field)
                            : sizeof(num_field);
    size_t n = strlen(f);

    if (!field)
        return;
    if (c == '\x08') {
        if (n)
            f[n - 1] = 0;
    } else if (n + 1 < cap) {
        f[n] = c;
        f[n + 1] = 0;
    }
}

static void tap(i32 x, i32 y)
{
    struct ui_kbd k;
    char ch;

    ui_kbd_init(&k, 0, 210, (i32)ui.surf.w, 140);
    if (ui_kbd_tap(&k, x, y, &ch)) {
        kbd_char(ch);
        return;
    }

    if (x >= 8 && x < 308 && y >= 12 && y < 202) {
        int row = (y - 14) / 17;

        sel = (row >= 0 && row < (int)nconts) ? row : -1;
        return;
    }

    if (y >= 12 && y < 36 && x >= 316 && x < 472)
        field = 1;
    else if (y >= 40 && y < 64 && x >= 316 && x < 472)
        field = 2;
    else if (y >= 68 && y < 94 && x >= 316 && x < 392) {
        if (name_field[0] && num_field[0] &&
            nconts < MAX_CONTS) {
            snprintf(conts[nconts].name,
                     sizeof(conts[nconts].name), "%s",
                     name_field);
            snprintf(conts[nconts].num,
                     sizeof(conts[nconts].num), "%s", num_field);
            nconts++;
            sel = (int)nconts - 1;
            save_db();
            printf("[contacts] added %s %s\n", name_field,
                   num_field);
            name_field[0] = 0;
            num_field[0] = 0;
        }
    } else if (y >= 68 && y < 94 && x >= 396 && x < 472) {
        if (sel >= 0 && (unsigned)sel < nconts) {
            printf("[contacts] deleted %s\n",
                   conts[sel].name);
            for (unsigned i = (unsigned)sel; i + 1 < nconts; i++)
                conts[i] = conts[i + 1];
            nconts--;
            sel = -1;
            save_db();
        }
    }
}

int main(int argc, char **argv, char **envp)
{
    i32 ex = -1, ey = -1;

    (void)argc;
    (void)argv;
    (void)envp;

    if (ui_connect(&ui, "contacts", UI_WIN_W, UI_WIN_H) < 0) {
        printf("[contacts] no compositor\n");
        return 1;
    }
    printf("[contacts] ready (window %u)\n", ui.win);
    load_db();

    draw();

    for (;;) {
        struct ui_msg m;
        int ty = ui_next(&ui, &m);

        if (ty < 0) {
            printf("[contacts] compositor gone\n");
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
