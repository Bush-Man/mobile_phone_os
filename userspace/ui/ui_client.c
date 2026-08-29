/*
 * ui_client.c - compositor client library (phase 15, plan item 79).
 *
 * The app-facing half of the UI protocol: connect to /var/run/ui,
 * register, open a window (the compositor creates the shm object
 * and answers with its id + screen position; the client maps it
 * with shmat), flush dirty rectangles and pump events. Every
 * exchange is fixed 32-byte ui_msg records.
 */

#include "ui.h"

static int ui_xfer(struct ui_client *c, struct ui_msg *m)
{
    char *p = (char *)m;
    size_t left = UI_MSG_SIZE;

    while (left) {
        i64 r = read(c->fd, p, left);

        if (r <= 0)
            return -1;
        p += r;
        left -= (size_t)r;
    }
    return 0;
}

static int ui_put(struct ui_client *c, const struct ui_msg *m)
{
    return write(c->fd, m, UI_MSG_SIZE) == (i64)UI_MSG_SIZE ? 0 : -1;
}

int ui_send(struct ui_client *c, u32 type, i32 a, i32 b, i32 c2,
            i32 d, const char *text)
{
    struct ui_msg m;

    memset(&m, 0, sizeof(m));
    m.type = type;
    m.win = c->win;
    m.a = a;
    m.b = b;
    m.c = c2;
    m.d = d;
    if (text)
        strncpy(m.text, text, sizeof(m.text) - 1);
    return ui_put(c, &m);
}

int ui_connect(struct ui_client *c, const char *name, u32 w, u32 h)
{
    struct ui_msg m;

    memset(c, 0, sizeof(*c));
    c->fd = usock_connect("/var/run/ui");
    if (c->fd < 0)
        return -1;

    if (ui_send(c, UI_HELLO, (i32)w, (i32)h, 0, 0, name) ||
        ui_xfer(c, &m) || m.type != UI_WELCOME)
        goto fail;

    if (ui_send(c, UI_OPEN, (i32)w, (i32)h, 0, 0, NULL) ||
        ui_xfer(c, &m) || m.type != UI_OPENED)
        goto fail;

    c->win = m.win;
    c->shm_id = m.a;
    c->x = m.b;
    c->y = m.c;

    {
        u64 npages = ((u64)w * h * 4u + 4095u) / 4096u;
        long va = (long)(uintptr_t)shmat(c->shm_id);

        if (!va || va == -1)
            goto fail;
        c->surf.px = (u32 *)(uintptr_t)va;
        c->surf.w = w;
        c->surf.h = h;
        (void)npages;
    }

    return 0;

fail:
    close(c->fd);
    c->fd = -1;
    return -1;
}

int ui_flush(struct ui_client *c, u32 x, u32 y, u32 w, u32 h)
{
    if (x >= c->surf.w || y >= c->surf.h)
        return -1;
    if (w > c->surf.w - x)
        w = c->surf.w - x;
    if (h > c->surf.h - y)
        h = c->surf.h - y;
    return ui_send(c, UI_FLUSH, (i32)x, (i32)y, (i32)w, (i32)h, NULL);
}

int ui_flush_all(struct ui_client *c)
{
    return ui_flush(c, 0, 0, c->surf.w, c->surf.h);
}

int ui_show(struct ui_client *c)
{
    return ui_send(c, UI_SHOW, 0, 0, 0, 0, NULL);
}

int ui_hide(struct ui_client *c)
{
    return ui_send(c, UI_HIDE, 0, 0, 0, 0, NULL);
}

int ui_notify(struct ui_client *c, const char *text, u32 kind)
{
    return ui_send(c, UI_NOTIFY, (i32)kind, 0, 0, 0, text);
}

int ui_next(struct ui_client *c, struct ui_msg *m)
{
    return ui_xfer(c, m) == 0 ? (int)m->type : -1;
}
