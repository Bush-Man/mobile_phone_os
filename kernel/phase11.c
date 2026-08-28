/*
 * phase11.c - networking bring-up (phase 11 entry).
 *
 * Boot-context work: register the loopback netif, bridge the
 * virtio-net NIC into the netif registry (when present), arm the rx
 * handler, then spawn "nettest" which runs the deterministic
 * battery -- DHCP against SLIRP, ARP gateway resolution, loopback
 * ping, full TCP loopback echo through the whole stack, and the
 * userspace netcli process proving the EL0 socket syscalls.
 */

#include <stdint.h>

#include "lib.h"
#include "net.h"
#include "platform.h"
#include "task.h"
#include "virtio.h"

void net_selftest_task(void *arg);      /* kernel/selftest_net.c     */

static int eth_link_out(struct netif *nif, const uint8_t *dest_hw,
                        uint16_t ethertype, const void *buf,
                        unsigned len)
{
    uint8_t frame[ETH_HDR_LEN + ETH_MTU];

    if (len > ETH_MTU)
        return -1;
    memcpy(&frame[0], dest_hw, ETH_HWADDR_LEN);
    memcpy(&frame[6], nif->hwaddr, ETH_HWADDR_LEN);
    frame[12] = (uint8_t)(ethertype >> 8);
    frame[13] = (uint8_t)ethertype;
    memcpy(&frame[ETH_HDR_LEN], buf, len);
    return virtio_net_send(frame, len + ETH_HDR_LEN);
}

static struct netif eth0;
extern int lo_netif_register(void);

static void eth_rx_bridge(const void *frame, unsigned len, void *arg)
{
    netif_input(frame, len, arg);
}

void phase11_init(const struct platform_info *plat)
{
    static bool done;

    (void)plat;
    if (done)
        return;
    done = true;

    lo_netif_register();

    if (virtio_net_present()) {
        memset(&eth0, 0, sizeof(eth0));
        eth0.name = "eth0";
        {
            const uint8_t *mac = virtio_net_mac();

            for (unsigned i = 0; i < 6; i++)
                eth0.hwaddr[i] = mac[i];
        }
        eth0.mtu      = ETH_MTU;
        eth0.up       = true;
        eth0.link_out = eth_link_out;
        netif_register(&eth0);
        netif_set_default(&eth0);

        virtio_net_set_rx_handler(eth_rx_bridge, &eth0);
        kprintf("net: eth0 registered (%u ifaces)\n",
                netif_count());
    } else {
        kprintf("net: no NIC attached -- loopback only\n");
    }

    task_create("nettest", net_selftest_task, NULL, 48);
}
