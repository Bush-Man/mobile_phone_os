/*
 * sysinfo.h - userspace mirror of the kernel's include/usabi.h.
 *
 * Fixed-layout records the kernel fills for the phase-14 report
 * syscalls (psinfo, mountinfo, netinfo, battinfo, devlist). Keep in
 * sync with include/usabi.h: same fields, same order, natural
 * alignment, never repack.
 */

#ifndef SYSINFO_H
#define SYSINFO_H

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef short          i16;

#define PROC_ABI_NAME_MAX   16
#define VFS_ABI_PATH_MAX    160

/* SYS_psinfo: one entry per process (kernel threads never appear) */
struct psinfo_entry {
    u32 pid;
    u32 ppid;                   /* 0 = none (boot-spawned or reaped) */
    u32 flags;                  /* bit0: alive, bit1: zombie         */
    char name[PROC_ABI_NAME_MAX];
};

#define PSINFO_ALIVE   1u
#define PSINFO_ZOMBIE  2u

/* SYS_mountinfo: one entry per active mount */
struct mountinfo_entry {
    char fstype[16];
    char path[VFS_ABI_PATH_MAX];
};

/* SYS_netinfo: one entry per registered netif */
struct netif_info {
    char    name[12];
    u8      hwaddr[6];
    u8      up;                 /* 0/1                               */
    u8      is_loopback;
    u8      pad;
    u32     ip, netmask, gw;    /* host-order u32; low byte = a in
                                 * a.b.c.d                           */
    u32     mtu;
};

/* SYS_battinfo: battery snapshot + provider provenance */
struct batt_info {
    u8  present;
    u8  is_mock;
    u8  percent;
    u8  pad;
    u16 voltage_mv;
    i16 current_ma;             /* >0 charging                       */
    i16 temp_deci_c;
};

/* SYS_devlist: one entry per enumerated device */
struct dev_info {
    char name[24];              /* unit name "pl011@9000000"         */
    char drv[24];               /* bound driver name, "" if unbound  */
    u8   state;                 /* 0 nodrv, 1 bound, 2 failed        */
    u8   pad[3];
};

#endif /* SYSINFO_H */
