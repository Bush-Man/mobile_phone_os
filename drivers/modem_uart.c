/*
 * modem_uart.c - real-board transport (phase 12): wraps a chardev
 * named "modem" (a second UART wired to the cellular module) into
 * the at_transport interface. QEMU instantiates none, so this file
 * contributes nothing there; drivers/modem_mock.c supplies the
 * scripted transport instead.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "chardev.h"
#include "modem.h"

static struct char_dev *modem_cd;

static int tp_write(void *priv, const void *buf, unsigned len)
{
    struct char_dev *cd = priv;

    if (!cd || !cd->write)
        return -1;
    return cd->write(cd, buf, len);
}

static int tp_read(void *priv, void *buf, unsigned len)
{
    struct char_dev *cd = priv;

    if (!cd || !cd->read)
        return 0;
    return cd->read(cd, buf, len);
}

/* returns a transport when a "modem" chardev exists, else NULL   */
bool modem_uart_transport(struct at_transport *out)
{
    struct char_dev *cd = char_dev_find("modem");

    if (!cd)
        return false;
    modem_cd  = cd;
    out->write = tp_write;
    out->read  = tp_read;
    out->priv  = cd;
    return true;
}
