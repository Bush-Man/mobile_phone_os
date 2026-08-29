#ifndef USABI_H
#define USABI_H

#include <stdint.h>

/*
 * usabi - fixed-layout structs that cross the kernel/user boundary
 * for the phase-14 "report" syscalls (psinfo, mountinfo, netinfo,
 * battinfo, devlist). They are filled by the kernel and copied out
 * through uaccess, so the layout must stay identical to the mirror
 * in userspace/libc/include/sysinfo.h -- keep the two in sync and
 * never reorder fields (all offsets are natural, no packing needed).
 *
 * IP addresses are host-order u32 (net.h convention): the octet for
 * printing a.b.c.d is the LOW byte first.
 */

/* layout anchors mirrored from proc.h (PROC_NAME_MAX) and vfs.h
 * (VFS_PATH_MAX) -- usabi.h must stay kernel-header-free so the
 * libc can mirror it, hence local copies of the sizes             */
#define PROC_ABI_NAME_MAX   16
#define VFS_ABI_PATH_MAX    160

/* SYS_psinfo: one entry per process (kernel threads never appear) */
struct psinfo_entry {
    uint32_t pid;
    uint32_t ppid;              /* 0 = none (boot-spawned or reaped) */
    uint32_t flags;             /* bit0: alive, bit1: zombie         */
    char     name[PROC_ABI_NAME_MAX];   /* NUL-terminated             */
};

#define PSINFO_ALIVE    (1u << 0)
#define PSINFO_ZOMBIE   (1u << 1)

/* SYS_mountinfo: one entry per active mount */
struct mountinfo_entry {
    char fstype[16];            /* NUL-terminated                    */
    char path[VFS_ABI_PATH_MAX];    /* mountpoint                    */
};

/* SYS_netinfo: one entry per registered netif */
struct netif_info {
    char    name[12];           /* NUL-terminated                    */
    uint8_t hwaddr[6];
    uint8_t up;                 /* 0/1                               */
    uint8_t is_loopback;
    uint8_t pad;
    uint32_t ip, netmask, gw;   /* host-order u32                    */
    uint32_t mtu;
};

/* SYS_battinfo: battery snapshot + provider provenance */
struct batt_info {
    uint8_t  present;
    uint8_t  is_mock;           /* provider is the QEMU mock         */
    uint8_t  percent;
    uint8_t  pad;
    uint16_t voltage_mv;
    int16_t  current_ma;        /* >0 charging                       */
    int16_t  temp_deci_c;
};

/* SYS_devlist: one entry per enumerated device */
struct dev_info {
    char    name[24];           /* unit name "pl011@9000000"         */
    char    drv[24];            /* bound driver name, "" if unbound  */
    uint8_t state;              /* enum dev_state                    */
    uint8_t pad[3];
};

/* dev_state mirror (values must match enum dev_state in device.h) */
#define DEVABI_UNBOUND  0
#define DEVABI_BOUND    1
#define DEVABI_FAILED   2

#endif /* USABI_H */
