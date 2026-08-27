/*
 * evreader.c - phase-9 milestone program.
 *
 * Opens /dev/event0 at EL0 and streams the twelve calibration events
 * the kernel pushed (kernel/selftest_gfx.c holds the mirrored table).
 * The plan milestone -- "touch coordinates stream into an input
 * reader process" -- is proven when every record matches its expected
 * triple; the reader then exits with a distinctive code.
 *
 * Freestanding static ELF against the raw syscall ABI; same style as
 * userspace/hello.c (-mgeneral-regs-only, number in x8, -negative errno).
 */

typedef unsigned long u64;
typedef long          i64;
typedef unsigned int  u32;
typedef unsigned short u16;

#define SYS_exit       1
#define SYS_read       6
#define SYS_write      5
#define SYS_getpid     7
#define SYS_open      13

#define EV_SYN   0u
#define EV_KEY   1u
#define EV_ABS   3u
#define SYN_REPORT 0u

#define ABS_X     0u
#define ABS_Y     1u
#define BTN_TOUCH 330u
#define KEY_VOLUMEUP 115u

/* wire record exactly as the kernel defines it                    */
struct wire_ev {
    u32 ms;
    u16 type;
    u16 code;
    int  value;
} __attribute__((packed));

static const struct {
    u16 type;
    u16 code;
    int value;
    const char *tag;
} expect[12] = {
    { EV_KEY, BTN_TOUCH,   1, "TOUCH-DOWN" },
    { EV_ABS, ABS_X,     400, "MOVE-X-400" },
    { EV_ABS, ABS_Y,     300, "MOVE-Y-300" },
    { EV_SYN, SYN_REPORT,  0, "SYNC" },
    { EV_KEY, BTN_TOUCH,   0, "TOUCH-UP" },
    { EV_SYN, SYN_REPORT,  0, "SYNC" },
    { EV_ABS, ABS_X,     200, "MOVE-X-200" },
    { EV_SYN, SYN_REPORT,  0, "SYNC" },
    { EV_ABS, ABS_Y,     100, "MOVE-Y-100" },
    { EV_SYN, SYN_REPORT,  0, "SYNC" },
    { EV_KEY, KEY_VOLUMEUP,1, "VOLUP-DOWN" },
    { EV_SYN, SYN_REPORT,  0, "SYNC" },
};

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
    char c = 10;                        /* newline literal            */

    sc3(SYS_write, 1, (i64)&c, 1);
}

void _start(unsigned long argc, char **argv)
{
    struct wire_ev ev;
    int fd, failures = 0;

    (void)argc;
    (void)argv;

    out("evreader: pid ");
    out_dec(sc3(SYS_getpid, 0, 0, 0));
    out(" opening /dev/event0");
    nl();

    fd = (int)sc3(SYS_open, (i64)(long)"/dev/event0", 0, 0);
    if (fd < 0) {
        out("evreader: open failed ");
        out_dec(fd);
        nl();
        sc3(SYS_exit, 96, 0, 0);
        for (;;)
            ;
    }

    for (unsigned i = 0; i < sizeof(expect) / sizeof(expect[0]); i++) {
        i64 r = sc3(SYS_read, fd,
                    (i64)(long)(void *)&ev, (i64)sizeof(ev));

        if (r != (i64)sizeof(ev)) {
            out("evreader: short read at evt ");
            out_dec((i64)i);
            nl();
            sc3(SYS_exit, 92, 0, 0);
            for (;;)
                ;
        }

        {
            int ok = ev.type == expect[i].type &&
                     ev.code == expect[i].code &&
                     ev.value == expect[i].value;

            if (!ok)
                failures++;

            out("evreader: evt ");
            out_dec((i64)i);
            out(" type=");
            out_dec((i64)ev.type);
            out(" code=");
            out_dec((i64)ev.code);
            out(" value=");
            out_dec((i64)ev.value);
            out(ok ? " ok (" : " BAD (");
            out(expect[i].tag);
            out(")");
            nl();
        }
    }

    if (!failures) {
        out("evreader: STREAM ok (12/12 records match)");
        nl();
        out("evreader: exiting 21");
        nl();
        sc3(SYS_exit, 21, 0, 0);
    } else {
        out("evreader: STREAM FAILED (");
        out_dec(failures);
        out(" mismatches)");
        nl();
        sc3(SYS_exit, 91, 0, 0);
    }
    for (;;)
        ;
}

