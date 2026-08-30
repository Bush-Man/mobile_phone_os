/*
 * netcli.c - phase-11 milestone proof at EL0.
 *
 * Uses the raw socket syscalls (SYS_socket/connect/bind/listen/
 * accept/send/recv) to run a complete TCP session against the
 * loopback interface: server socket on 127.0.0.1:7307, client
 * connect, bidirectional payload, graceful FIN. Loopback delivery
 * is synchronous inside the transmit path, so a single-threaded
 * program can drive both ends deterministically.
 */

typedef unsigned long u64;
typedef long          i64;
typedef unsigned int  u32;
typedef unsigned short u16;
typedef unsigned char u8;

#define SYS_exit       1
#define SYS_write      5
#define SYS_getpid     7
#define SYS_socket    34
#define SYS_connect   35
#define SYS_bind      36
#define SYS_listen    37
#define SYS_accept    38
#define SYS_send      39
#define SYS_recv      40

#define AF_INET  2u
#define SOCK_STREAM 1u

struct sa_in {
    u16 family;
    u16 port;
    u32 addr;
    u8  zero[8];
} __attribute__((packed));

static i64 sc4(i64 n, i64 a, i64 b, i64 c, i64 d)
{
    register i64 x8 __asm__("x8") = n;
    register i64 x0 __asm__("x0") = a;
    register i64 x1 __asm__("x1") = b;
    register i64 x2 __asm__("x2") = c;
    register i64 x3 __asm__("x3") = d;

    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x8), "r"(x0), "r"(x1), "r"(x2), "r"(x3)
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
    sc4(SYS_write, 1, (i64)s, (i64)slen(s), 0);
}

static void nl(void)
{
    char c = 10;

    sc4(SYS_write, 1, (i64)&c, 1, 0);
}

static void sockaddr_fill(struct sa_in *sa, u32 ip_be, u16 port_be)
{
    sa->family = AF_INET;
    sa->port   = port_be;
    sa->addr   = ip_be;
    for (int i = 0; i < 8; i++)
        sa->zero[i] = 0;
}

void _start(unsigned long argc, char **argv)
{
    struct sa_in loop;
    int srv, conn, cli;
    static const char payload[] = "netcli-tcp-loopback";
    char buf[64];
    i64 r;

    (void)argc;
    (void)argv;

    out("netcli: pid ");
    {
        char b[24];
        i64 pid = sc4(SYS_getpid, 0, 0, 0, 0);
        int i = 23;

        b[i] = 0;
        do {
            b[--i] = (char)('0' + pid % 10);
            pid /= 10;
        } while (pid);
        out(&b[i]);
    }
    nl();

    /*
     * The kernel stores IPv4 as (a | b<<8 | c<<16 | d<<24), i.e. the
     * dotted quad in ascending byte order, and sockaddr_parse takes
     * sa.addr verbatim. Ports it does byte-swap, so those stay BE.
     * 127.0.0.1 -> 0x0100007f; port 7307 -> 0x8b1c swapped.
     */
    sockaddr_fill(&loop, 0x0100007fu, 0x8b1cu);

    /* the listener must own :7307 -- connect() targets that port,
     * and tcp_bind hands out an ephemeral one for port 0            */
    srv = (int)sc4(SYS_socket, SOCK_STREAM, 0, 0, 0);
    if (srv < 0) {
        out("netcli: socket failed");
        nl();
        sc4(SYS_exit, 95, 0, 0, 0);
        for (;;)
            ;
    }
    if (sc4(SYS_bind, srv, (i64)&loop, 16, 0)) {
        out("netcli: bind failed");
        nl();
        sc4(SYS_exit, 95, 0, 0, 0);
        for (;;)
            ;
    }
    if (sc4(SYS_listen, srv, 4, 0, 0)) {
        out("netcli: listen failed");
        nl();
        sc4(SYS_exit, 95, 0, 0, 0);
        for (;;)
            ;
    }

    cli = (int)sc4(SYS_socket, SOCK_STREAM, 0, 0, 0);
    if (cli < 0) {
        out("netcli: client socket failed");
        nl();
        sc4(SYS_exit, 95, 0, 0, 0);
        for (;;)
            ;
    }
    if (sc4(SYS_connect, cli, (i64)&loop, 16, 0)) {
        out("netcli: connect failed");
        nl();
        sc4(SYS_exit, 95, 0, 0, 0);
        for (;;)
            ;
    }

    conn = (int)sc4(SYS_accept, srv, 0, 0, 0);
    if (conn < 0) {
        out("netcli: accept failed");
        nl();
        sc4(SYS_exit, 95, 0, 0, 0);
        for (;;)
            ;
    }

    r = sc4(SYS_send, cli, (i64)(long)payload, (i64)slen(payload), 0);
    if (r != (i64)slen(payload)) {
        out("netcli: send failed");
        nl();
        sc4(SYS_exit, 92, 0, 0, 0);
        for (;;)
            ;
    }

    {
        int ok = 1;

        for (unsigned i = 0; i < sizeof(payload); i++)
            buf[i] = 0;
        r = sc4(SYS_recv, conn, (i64)(long)buf,
                (i64)sizeof(buf), 0);
        if (r != (i64)slen(payload))
            ok = 0;
        for (unsigned i = 0; i < sizeof(payload) && ok; i++)
            if (buf[i] != payload[i])
                ok = 0;

        if (ok) {
            out("netcli: STREAM ok (payload echoed)");
            nl();
        } else {
            out("netcli: STREAM FAILED");
            nl();
            sc4(SYS_exit, 91, 0, 0, 0);
            for (;;)
                ;
        }
    }

    out("netcli: exiting 21");
    nl();
    sc4(SYS_exit, 21, 0, 0, 0);
    for (;;)
        ;
}
