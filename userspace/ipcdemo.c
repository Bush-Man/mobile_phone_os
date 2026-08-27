/*
 * ipcdemo.c - the phase-8 milestone program.
 *
 * Two processes talking through a pipe AND shared memory, per the
 * plan milestone ("two processes talk through a pipe and shared
 * memory; shell can run children"):
 *
 *   parent:
 *     mmap a private page and drop an INHERIT marker in it
 *     shmget(1) -> shmat -> write 0x600DF00D into slot [0]
 *     pipe() -> fork()
 *       child: reads inherited mmap marker verdict; writes
 *              "SHMID:<id>" on the pipe; shmat's that id itself,
 *              verifies parent's magic, stores 0xCAFEF00D into
 *              slot [1]; replies "ALL-GOOD" on the pipe; exits 7
 *     parent: reads both lines from the pipe, verifies the shared
 *             slot flipped by the child, reaps via waitpid,
 *             prints one "ipcdemo: ... ok" line per proof, exits.
 *
 * Freestanding static ELF at EL0 against the raw syscall ABI --
 * same conventions as userspace/hello.c (-mgeneral-regs-only).
 */

typedef unsigned long u64;
typedef long          i64;
typedef int           i32;
typedef unsigned int  u32;

#define U64P(x) ((unsigned long)(x))

#define SYS_exit        1
#define SYS_fork        2
#define SYS_waitpid     4
#define SYS_write       5
#define SYS_read        6
#define SYS_getpid      7
#define SYS_close      14
#define SYS_mmap       21
#define SYS_shmget     22
#define SYS_pipe       25
#define SYS_shmat      23

static i64 sc3(i64 n, i64 a, i64 b, i64 c)
{
    register i64 x8 __asm__("x8") = n;
    register i64 x0 __asm__("x0") = a;
    register i64 x1 __asm__("x1") = b;
    register i64 x2 __asm__("x2") = c;

    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x8), "r"(x0), "r"(x1), "r"(x2)
                     : "memory", "cc");
    return x0;
}

static u64 slen(const char *s)
{
    u64 n = 0;

    while (s[n])
        n++;
    return n;
}

static void out(const char *s)
{
    sc3(SYS_write, 1, (i64)s, (i64)slen(s));
}

static void out_dec(i64 v)
{
    char buf[24];
    int i = (int)sizeof(buf) - 1;
    unsigned long long m = v < 0 ? -(unsigned long long)v
                                 : (unsigned long long)v;

    buf[i] = 0;
    do {
        buf[--i] = (char)('0' + m % 10u);
        m /= 10u;
    } while (m);
    if (v < 0)
        buf[--i] = '-';
    out(&buf[i]);
}

static void nl(void)
{
    out("\n");
}

/* read exactly `want` bytes through the raw fd                    */
static int rd_full(int fd, char *buf, unsigned want)
{
    unsigned got = 0;

    while (got < want) {
        i64 r = sc3(SYS_read, fd, (i64)(buf + got), want - got);

        if (r <= 0)
            return -1;
        got += (unsigned)r;
    }
    return 0;
}

void _start(unsigned long argc, char **argv)
{
    int pipefd[2];
    i64 va, mmva, child;
    volatile u32 *shm, *priv;
    char line[32];
    const u32 PARENT_MAGIC = 0x600DF00Du;
    const u32 CHILD_ACK    = 0xCAFEF00Du;

    if (argc < 2 || !argv || !argv[0]) {   /* keep argv shape sane */
        out("ipcdemo: bad exec env\n");
        sc3(SYS_exit, 97, 0, 0);
        for (;;)
            ;
    }

    /* private anonymous page: proves SYS_mmap + fork inheritance   */
    mmva = sc3(SYS_mmap, 0, 4096, 0);
    if (mmva < 0) {
        out("ipcdemo: mmap failed\n");
        nl();
        sc3(SYS_exit, 98, 0, 0);
        for (;;)
            ;
    }
    priv = (volatile u32 *)U64P(mmva);
    priv[0] = 0x11223344u;              /* marker pre-fork         */

    /* shared object + attach before fork so BOTH sides know it     */
    {
        i64 id = sc3(SYS_shmget, 1, 0, 0);

        if (id < 0) {
            out("ipcdemo: shmget failed\n");
            nl();
            sc3(SYS_exit, 99, 0, 0);
            for (;;)
                ;
        }
        va = sc3(SYS_shmat, id, 0, 0);
        if (va < 0) {
            out("ipcdemo: shmat failed\n");
            nl();
            sc3(SYS_exit, 99, 0, 0);
            for (;;)
                ;
        }
    }
    shm = (volatile u32 *)U64P(va);
    shm[0] = PARENT_MAGIC;

    if (sc3(SYS_pipe, (i64)(long)(void *)pipefd, 0, 0)) {
        out("ipcdemo: pipe failed\n");
        nl();
        sc3(SYS_exit, 95, 0, 0);
        for (;;)
            ;
    }

    child = sc3(SYS_fork, 0, 0, 0);
    if (child == 0) {
        /*
         * ---- child -------------------------------------------------
         * Same image, same fds: the pipe write end in hand, the
         * private mapping and SHM window both inherited.
         */
        sc3(SYS_close, pipefd[0], 0, 0);    /* keep only our end  */

        out("ipcdemo-child: pid ");
        out_dec(sc3(SYS_getpid, 0, 0, 0));
        out(" inherited-mmap ");
        if (priv[0] == 0x11223344u)
            out("ok");
        else
            out("BAD");
        nl();

        /* tell the parent which shared object to re-check          */
        out("SHMID-READY\n");
        shm[1] = CHILD_ACK;                 /* flip the latch      */

        {
            const char msg[] = "ALL-GOOD";

            sc3(SYS_write, pipefd[1], (i64)(long)(void *)msg,
                sizeof(msg));
        }
        sc3(SYS_exit, 7, 0, 0);
        for (;;)
            ;
    }

    /* ---- parent --------------------------------------------------- */
    if (child < 0) {
        out("ipcdemo: fork failed\n");
        nl();
        sc3(SYS_exit, 94, 0, 0);
        for (;;)
            ;
    }
    sc3(SYS_close, pipefd[1], 0, 0);        /* read-only side      */

    if (rd_full(pipefd[0], line, sizeof("SHMID-READY"))) {
        out("ipcdemo: pipe read 1 failed\n");
        nl();
        sc3(SYS_exit, 93, 0, 0);
        for (;;)
            ;
    }

    if (rd_full(pipefd[0], line, sizeof("ALL-GOOD"))) {
        out("ipcdemo: pipe read 2 failed\n");
        nl();
        sc3(SYS_exit, 93, 0, 0);
        for (;;)
            ;
    }

    out("ipcdemo: PIPE round-trip ok\n");
    if (shm[1] == CHILD_ACK && shm[0] == PARENT_MAGIC)
        out("ipcdemo: SHM cross-process ok\n");
    else
        out("ipcdemo: SHM cross-process BAD\n");

    {
        i64 rc = sc3(SYS_waitpid, child, 0, 0);

        if (rc >= 0)
            out("ipcdemo: reaped child code 7\n");
        else
            out("ipcdemo: waitpid failed\n");
    }

    out("ipcdemo: exiting 21\n");
    sc3(SYS_exit, 21, 0, 0);
    for (;;)
        ;
}
