/*
 * sh.c - the interactive shell (phase 14, plan item 76).
 *
 * Reads lines from fd 0 (the console tty's line discipline) and
 * dispatches builtins plus `run <prog>` (fork + execve over the
 * built-in image set). Coreutils-lite surface: ps, kill, ls, cat,
 * echo, mount, ifconfig -- each a thin consumer of one phase-14
 * report syscall, so the shell doubles as a live ABI demo.
 *
 * `exit` kills the shell; init respawns it (critical service), which
 * is itself part of the milestone proof.
 */

#include "libc.h"
#include "sysinfo.h"

#define LINE_MAX  256

static void print_ip(u32 ip, int with_slash, u32 mask)
{
    printf("%u.%u.%u.%u",
           ip & 0xff, (ip >> 8) & 0xff, (ip >> 16) & 0xff,
           (ip >> 24) & 0xff);
    if (with_slash) {
        int prefix = 0;

        while (mask & 0x80000000u) {
            prefix++;
            mask <<= 1;
        }
        printf("/%d", prefix);
    }
}

static int cmd_ps(void)
{
    struct psinfo_entry ents[32];
    int n = psinfo(ents, 32);

    if (n < 0) {
        printf("ps: psinfo failed (%d)\n", n);
        return 1;
    }
    printf("%-5s %-5s %-6s %s\n", "PID", "PPID", "STATE", "NAME");
    for (int i = 0; i < n; i++) {
        printf("%-5u %-5u %-6s %s\n", ents[i].pid, ents[i].ppid,
               ents[i].flags & PSINFO_ZOMBIE ? "zombie" : "run",
               ents[i].name);
    }
    return 0;
}

static int cmd_kill(int argc, char **argv)
{
    int pid, sig = SIGKILL;

    if (argc < 2) {
        printf("usage: kill <pid> [sig]\n");
        return 1;
    }
    if (argc >= 3)
        sig = (int)strtoul(argv[2], 0, 10);
    pid = (int)strtoul(argv[1], 0, 10);
    return kill(pid, sig) == 0 ? 0 : 1;
}

static int cmd_ls(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/";
    int fd = open(path, O_RDONLY);
    char buf[512];

    if (fd < 0) {
        printf("ls: cannot open %s (%d)\n", path, fd);
        return 1;
    }
    for (;;) {
        long r = _sys3(SYS_getdents, fd, (i64)buf, sizeof(buf));
        long off = 0;

        if (r <= 0) {
            if (r < 0)
                printf("ls: getdents failed (%ld)\n", r);
            break;
        }
        while (off < r) {
            u16 reclen;
            const char *name;

            memcpy(&reclen, buf + off, sizeof(reclen));
            name = buf + off + 3;       /* u16 len + u8 type       */
            printf("%s%s\n", name,
                   buf[off + 2] == 2 ? "/" : "");  /* dir type 2  */
            off += reclen;
        }
    }
    close(fd);
    return 0;
}

static int cmd_cat(int argc, char **argv)
{
    int fd;
    char buf[256];
    i64 r;

    if (argc < 2) {
        printf("usage: cat <file>\n");
        return 1;
    }
    fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        printf("cat: cannot open %s (%d)\n", argv[1], fd);
        return 1;
    }
    while ((r = read(fd, buf, sizeof(buf))) > 0)
        write(1, buf, (size_t)r);
    close(fd);
    return 0;
}

static int cmd_mount(void)
{
    struct mountinfo_entry ents[8];
    int n = mountinfo(ents, 8);

    if (n < 0) {
        printf("mount: mountinfo failed (%d)\n", n);
        return 1;
    }
    for (int i = 0; i < n; i++)
        printf("%-8s on %s\n", ents[i].fstype, ents[i].path);
    return 0;
}

static int cmd_ifconfig(void)
{
    struct netif_info ents[8];
    int n = netinfo(ents, 8);

    if (n < 0) {
        printf("ifconfig: netinfo failed (%d)\n", n);
        return 1;
    }
    for (int i = 0; i < n; i++) {
        printf("%s: hw %02x:%02x:%02x:%02x:%02x:%02x %s\n",
               ents[i].name,
               ents[i].hwaddr[0], ents[i].hwaddr[1],
               ents[i].hwaddr[2], ents[i].hwaddr[3],
               ents[i].hwaddr[4], ents[i].hwaddr[5],
               ents[i].up ? "UP" : "DOWN");
        printf("      inet ");
        print_ip(ents[i].ip, 1, ents[i].netmask);
        printf(" gw ");
        print_ip(ents[i].gw, 0, 0);
        printf(" mtu %u%s\n", ents[i].mtu,
               ents[i].is_loopback ? " (loopback)" : "");
    }
    return 0;
}

static int cmd_crashlog(void)
{
    int fd = open("/var/crash/records", O_RDONLY);
    char buf[256];
    i64 r;

    if (fd < 0) {
        printf("crashlog: no records yet\n");
        return 0;
    }
    while ((r = read(fd, buf, sizeof(buf))) > 0)
        write(1, buf, (size_t)r);
    close(fd);
    return 0;
}

static int cmd_run(int argc, char **argv)
{
    int pid;

    if (argc < 2) {
        printf("usage: run <prog>\n");
        return 1;
    }
    pid = fork();
    if (pid == 0) {
        if (execve(argv[1], argv + 1,
                   (char *const []){ 0 }) < 0) {
            printf("sh: execve(%s) failed (%d)\n", argv[1],
                   getpid());
            _exit(127);
        }
        for (;;)
            ;
    }
    {
        i64 r = waitpid(pid);

        if (r < 0)
            printf("sh: waitpid failed (%ld)\n", r);
        else
            printf("sh: pid %d exited with code %d\n",
                   (int)(r >> 8), (int)(r & 0xff));
    }
    return 0;
}

static int dispatch(int argc, char **argv)
{
    const char *c = argv[0];

    if (!strcmp(c, "help")) {
        printf("builtins: help ps kill ls cat echo mount ifconfig "
               "bat devs crashlog date uptime sleep run exit\n");
        return 0;
    }
    if (!strcmp(c, "ps"))
        return cmd_ps();
    if (!strcmp(c, "kill"))
        return cmd_kill(argc, argv);
    if (!strcmp(c, "ls"))
        return cmd_ls(argc, argv);
    if (!strcmp(c, "cat"))
        return cmd_cat(argc, argv);
    if (!strcmp(c, "echo")) {
        for (int i = 1; i < argc; i++)
            printf("%s%s", argv[i], i + 1 < argc ? " " : "");
        printf("\n");
        return 0;
    }
    if (!strcmp(c, "mount"))
        return cmd_mount();
    if (!strcmp(c, "ifconfig"))
        return cmd_ifconfig();
    if (!strcmp(c, "bat")) {
        struct batt_info bi;

        if (battinfo(&bi) || !bi.present) {
            printf("bat: no gauge\n");
            return 1;
        }
        printf("%u%% %dmV %s (%d mA)\n", bi.percent, bi.voltage_mv,
               bi.current_ma > 0 ? "chg" : "dis", bi.current_ma);
        return 0;
    }
    if (!strcmp(c, "devs")) {
        struct dev_info devs[32];
        int n = devlist(devs, 32);

        for (int i = 0; i < n; i++)
            printf("%-24s %-24s %u\n", devs[i].name, devs[i].drv,
                   devs[i].state);
        return 0;
    }
    if (!strcmp(c, "crashlog"))
        return cmd_crashlog();
    if (!strcmp(c, "date")) {
        printf("epoch %llu ns, uptime %llu ms\n",
               (unsigned long long)gettime_ns(),
               (unsigned long long)uptime_ms());
        return 0;
    }
    if (!strcmp(c, "uptime")) {
        printf("up %llu ms\n", (unsigned long long)uptime_ms());
        return 0;
    }
    if (!strcmp(c, "sleep")) {
        sleep_ms(argc > 1 ? strtoul(argv[1], 0, 10) : 1000);
        return 0;
    }
    if (!strcmp(c, "run"))
        return cmd_run(argc, argv);
    if (!strcmp(c, "exit")) {
        printf("sh: exiting (init will respawn me)\n");
        return -2;
    }
    if (!strcmp(c, "poweroff") || !strcmp(c, "reboot")) {
        printf("sh: %s wired to PSCI but refused in the demo "
               "build\n", c);
        return 0;
    }

    printf("sh: %s: not found (try `help` or `run <prog>`)\n", c);
    return 1;
}

int main(int argc, char **argv, char **envp)
{
    char line[LINE_MAX];

    (void)argc;
    (void)argv;
    (void)envp;

    printf("sh: mobile_phone_os shell (pid %d) -- `help` lists "
           "builtins\n", getpid());

    for (;;) {
        char *argvl[16];
        int argcnt = 0;
        i64 r = read(0, line, sizeof(line) - 1);
        char *p;
        size_t n;

        if (r <= 0)
            continue;
        line[r] = 0;

        n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;
        if (!n)
            continue;

        p = line;
        while (*p && argcnt < 15) {
            while (*p == ' ')
                p++;
            if (!*p)
                break;
            argvl[argcnt++] = p;
            while (*p && *p != ' ')
                p++;
            if (*p)
                *p++ = 0;
        }
        if (!argcnt)
            continue;
        argvl[argcnt] = 0;

        if (dispatch(argcnt, argvl) == -2)
            return 0;           /* init respawns us                  */
    }
    return 0;
}

