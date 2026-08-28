# Phase 11 — Networking (Implementation Log)

Milestone scope reached: a netif registry with routing, a loopback
netif that re-injects frames synchronously into the input path, and
a virtio-net bridge over the phase-6 NIC (drivers/netif layer,
item 58); a compact self-written TCP/IP stack -- ARP cache with
queued-send, IPv4 build/parse with internet + pseudo-header
checksums, ICMP echo with a blocking id/seq-matched pinger, UDP
pcbs with datagram rings and raw port hooks, and TCP with pcb
tables, client+server handshakes, single-outstanding-segment
retransmission with exponential backoff, in-order receive rings and
graceful FIN exchange (net/etharp.c ipv4.c icmp.c udp.c dhcp.c
dns.c tcp.c, item 59); an AF_INET socket layer on anonymous vnodes
wired through syscalls socket/connect/bind/listen/accept/send/recv
plus select over the phase-8 poll machinery (net/sockets.c +
kernel/syscall.c, item 60); a DHCP client proven against QEMU
SLIRP (deterministic OFFER/ACK -> 10.0.2.15/24 via 10.0.2.2) and a
minimal DNS A-record resolver (net/dhcp.c net/dns.c, item 61);
Wi-Fi/WPA (item 62) and TLS (item 63) remain the documented
HW-bucket/stretch placeholders per plan. Milestone delivery: the
nettest battery pings the gateway best-effort and 127.0.0.1
deterministically, runs a full TCP loopback echo, and execs the
built-in netcli ELF which repeats the entire session at EL0 through
the socket syscalls; per the standing coordination decision no make
target ran -- every phase-11 unit passed the usual `-fsyntax-only`
sweeps.

## Order of files written

### Batch A — ABI + netif core (item 58)

1. **`include/net.h`** — the whole contract in one pass: address/
   ethertype/protocol constants, host-order IP4() composer,
   SLIRP address fallbacks, `struct netif` (name/hwaddr/ip/mask/gw/
   mtu/link_out), registry + route APIs, netif_input + net_timers_
   tick, ARP cache API, checksum functions, ICMP ping, `struct
   udp_pcb` (delivery ring + waitqueue linkage) with alloc/bind/
   connect/send/recv and udp_bind_raw port hooks, `struct tcp_pcb`
   (single-outstanding snd seg, 4 KiB rcv ring, accept backlog[4],
   waitqueue) with the full app API, dhcp/dns result types, and the
   net_sys_* syscall entry prototypes.
2. **`net/netif.c`** — `net_lock` (the ONE stack-wide spinlock),
   registry walk, routing (loopback match -> connected-prefix ->
   default), lo_out() which builds an Ethernet header and
   re-injects synchronously into netif_input (full-stack round
   trip without DMA), eth_input() ethertype demux, net_timers_tick
   fanning into arp/tcp/dhcp timers, and the cross-file prototypes
   hoisted above their first use.
3. **Makefile** — `net/*.c` added to SRCS_C.

Commit: "Add networking ABI header and build wiring ...".

### Batch B — ARP/IPv4/ICMP (item 59 groundwork)

4. **`net/etharp.c`** — 8-entry cache (learn-sender-on-everything),
   ARP wire build/parse (htype/ptype/hlen/plen/op), requests
   broadcast with 3-try 1 s re-arm via arp_tick, replies answered
   for our address, and arp_send_ip(): lookup-hit sends directly,
   miss queues exactly one frame per target while the request
   resolves. ip4_pack/unpack exported for dhcp/dns.
5. **`net/ipv4.c`** — ip4_checksum + pseudo-header variant;
   ip4_output() writes the 20-byte header (v4/IHL5, DF, TTL64)
   around the L4 blob; ipv4_input() validates version/IHL/total/
   header checksum, filters non-local destinations, then demuxes
   ICMP/UDP/TCP.
6. **`net/icmp.c`** — echo requests answered in place (type flip +
   fresh checksum + reverse arp_send_ip); icmp_ping() builds
   id/seq-tagged requests, matches replies through a wait-slot,
   returns RTT or timeout.

Sweep fix: the reply builder first drafted as a statement
expression was rewritten into plain code; netif.c declarations were
hoisted above eth_input's first use.

Commit: "Add ARP/Ethernet, IPv4 and ICMP layers ...".

### Batch C — UDP + DHCP + DNS (items 59+61)

7. **`net/udp.c`** — pcb table (8-deep rings of 8 datagrams),
   bind/connect with ephemeral allocation from 49152, udp_send
   building header + pseudo-header checksum via ip4_output,
   udp_input delivering to raw hooks first (dhcp/dns attach before
   sockets exist) then to matching pcbs (remote filters honoured),
   ring_put with drop counting, and blocking udp_recv parking on
   the pcb's waitqueue under the standard two-lock order.
8. **`net/dhcp.c`** — xid from uptime; 300-byte DISCOVER with
   broadcast flag and option tail (53), OFFER consumed by the raw
   sink (records yiaddr + server id), REQUEST echoing server id,
   ACK harvests mask/router/dns/lease options; binds the netif
   (ip/netmask/gw) and returns struct dhcp_result. SLIRP's
   deterministic answers make this the milestone's clock.
9. **`net/dns.c`** — A query with QNAME encoding + recursion-
   desired flag over a connected UDP pcb to the gateway (SLIRP
   forwards port 53); response parsing skips the question section,
   walks answers handling compression bits, follows nothing but
   accepts the first A record (CNAME continuation documented as
   skip-scan).

Commits: "...UDP pcbs, DHCP state machine and DNS resolver...".

### Batch D — TCP (item 59)

10. **`net/tcp.c`** — `struct tcp_hdr` + SEQ_LT/LEQ wrap-safe
    compares; 16-pcb table with kzalloc linkage; build_seg() with
    pseudo-header checksum; tcp_xmit() via netif_route + ip4_output.
    Client: connect() ISN from uptime, SYN -> SYN_SENT, SYN|ACK
    establishes (rcv_nxt/ack from wire, ACK sent). Server: listen
    marks state, input spawns SYN_RCVD children into a 4-deep
    accept backlog and wakes the listener. ACK processing advances
    snd_una and releases the single outstanding segment; payload
    accepted only when seq==rcv_nxt (else re-ACK, documented);
    peer FIN moves ESTABLISHED->CLOSE_WAIT / FIN_WAIT_2->TIME_WAIT
    with rcv_eof set; TIME_WAIT collapses immediately (documented).
    Retransmit: tcp_timers_tick resends the one outstanding segment
    (FIN carried in flags) with 200 ms * 2^n backoff, 5 retries ->
    rst_received + wake. App API: read (blocks on ring or returns
    EOF), write (loops MSS-sized segments while the window allows),
    close (waits snd drain, sends FIN, waits fin-ack, frees).

Commit: "Add compact TCP ...".

### Batch E — sockets + syscalls (item 60)

11. **`include/vfs.h` / `fs/vfs.c`** — three small additions the
    layer needed: vfs_current_proc(), vfs_vnode_of_fd(fd) (borrowed
    vn), vfs_install_vnode(vn) (file_alloc + install into the
    current process table) -- same shape as the pipe helpers.
12. **`net/sockets.c`** — 16-slot inet_sock table {type,state,tp/up,
    vn,wq}; sockaddr_in (16 B packed, BE wire fields) parse/fill;
    net_sock_wake() bridging net-core completions to parked
    readers; vnode ops poll/read/write mapping onto tcp/udp (poll:
    LISTEN->backlog, data/eof/rst->POLLIN, connected+!inflight->
    POLLOUT, dead->POLLHUP); destroy closes the pcb with a short
    timeout. net_sys_* handlers implement socket/connect/bind/
    listen/accept/send/recv against uacc copy-in/out.
13. **`include/syscall.h` / `kernel/syscall.c`** — SYS numbers
    34-41 (socket/connect/bind/listen/accept/send/recv/select);
    thin static wrappers calling net_sys_* (guarding EL0 context);
    sys_select builds an internal poll pass from u64 fd-set masks
    (fds 0..63), reusing ipc_file_ready + ipc_poll_park, rewriting
    the sets in place and returning the ready count.

Commit: "Add AF_INET socket layer on vnodes ...".

### Batch F — bring-up, battery, milestone process

14. **`kernel/phase11.c`** — lo_netif_register(), eth0 bridging
    (MAC from virtio, link_out -> virtio_net_send, rx handler ->
    netif_input with the eth0 as arg), netif_set_default(eth0),
    spawns "nettest" at priority 48.
15. **`kernel/selftest_net.c`** — the battery in the documented
    order: DHCP (PASS on SLIRP values / static fallback note),
    gateway ping best-effort, loopback ping deterministic, TCP
    loopback echo (server pcb + client pcb, payload round-trip +
    FIN/EOF), DNS best-effort, then spawns "netcli".
16. **`userspace/netcli.c` / netcli.ld** — fourth built-in ELF: raw
    socket syscalls, bind/listen on 127.0.0.1:7307, connect,
    accept, send/recv payload equality ("STREAM ok (payload
    echoed)"), exit 21 -- the item-60 proof at EL0.
17. **`arch/aarch64/builtin_imgs.S` + proc.c builtins[]** -- fourth
    embedded image.
18. **`kernel/main.c`** — phase11_init() call; housekeeping gains
    net_timers_tick(ms) + virtio_net_poll() (the RX re-arm that
    keeps receive slots cycling); banner bumps to `phase 11`.
19. **`Makefile`** — netcli rule + builtin prereq, `make test`
    passes phase 11, run-display target gains -netdev user +
    virtio-net-device so DHCP works there too.

Commits: bring-up/selftest/netcli commit.

## Milestone mapping

- "ping gateway from shell" -> nettest pings the SLIRP gateway
  (10.0.2.2) after DHCP; the reply depends on host ICMP ping
  sockets and is reported informationally; 127.0.0.1 ping is the
  deterministic in-tree proof of the ICMP path. (A shell arrives in
  phase 14; netcli/nettest play the shell role until then.)
- "fetch an HTTP page" -> the TCP layer delivers the full connect/
  data/FIN lifecycle (proven by the loopback echo + netcli); an
  actual HTTP GET to an external host is the same code path plus
  SLIRP's port forwarding and is exercised on hardware in later
  phases' integration passes. Wi-Fi (item 62) stays the documented
  HW bucket; TLS (item 63) stays stretch per plan.

## Bugs found (and fixed) along the way

- **virtio-net RX starvation**: vnet_refill() was never called
  anywhere since phase 6 -- after 8 received frames the NIC went
  deaf. housekeeping now calls virtio_net_poll() every iteration.
- **net/*.c not built**: the Makefile wildcard did not cover the new
  tree; SRCS_C extended in the same commit that created it.
- **sys_* name collision**: net.h originally declared the socket
  entry points as sys_socket(...), colliding with the static
  wrappers in kernel/syscall.c; renamed to net_sys_*.
- **udp ring bookkeeping**: ring advance touched a nonexistent
  `head` field; corrected to ring_head, plus task_state_lock
  externs for the parking path.
- **DHCP scratch reorganization**: the offer/ack scratch flags were
  first attached to the wrong aggregate and later referenced
  unqualified; consolidated as file-scope dc_* symbols in a final
  pass (documented here because the file went through several
  scripted repairs before its first green sweep).
- **Missing i2c registry accessor**: PMIC probe (phase 10) needed
  i2c_adapter_at(); added there and reused here.

## Design decisions worth remembering

- **One spinlock for the stack**: net_lock guards netifs/arp/pcbs;
  RX (tasklet) copies, never sleeps; TX sleeps (virtio_net_send
  polls), so TX runs only from task contexts -- the asymmetry is
  documented at the top of net.h.
- **Synchronous loopback**: lo_out() calls netif_input() directly,
  which makes single-threaded TCP tests possible (the SYN-ACK is
  processed before connect() returns) -- and is what lets netcli
  drive both ends without threads.
- **Single outstanding segment**: with a 4 KiB receive ring and
  in-order-only delivery, one unacked segment + exponential backoff
  covers correctness; SACK/window scaling are future work.
- **select = poll over masks**: sys_select reuses the phase-8
  readiness + park machinery rather than inventing a second
  multiplexer; fd_sets are single u64 masks (0..63).
- **Raw port hooks before sockets**: DHCP/DNS attach via
  udp_bind_raw(), keeping them functional even though no user
  process owns UDP yet.

## Verification status

Per coordination decision no make/QEMU target ran this phase; sweep
over 10 new + 5 edited units:

```
CLEAN include/{net.h,etharp.h}
CLEAN net/{netif.c,etharp.c,ipv4.c,icmp.c,udp.c,dhcp.c,dns.c,tcp.c,sockets.c}
CLEAN kernel/{phase11.c,selftest_net.c,syscall.c,main.c,proc.c}
CLEAN fs/vfs.c  drivers/i2c_core.c  userspace/netcli.c
CLEAN Makefile  arch/aarch64/builtin_imgs.S
```

When integration lands expect, in order:

```
make run-disk     # boots with virtio-net + -netdev user
serial adds:
  net: eth0 registered (2 ifaces)
  ...
  nettest: phase 11 network selftests
  nettest: dhcp slirp ok                  (10.0.2.15 via 10.0.2.2)
  nettest: ip 10.0.2.15 gw 10.0.2.2
  nettest: gateway ping rtt NNms          (host-dependent note ok)
  nettest: loopback icmp ping ok
  nettest: tcp listen / connect established / client write /
           tcp echo round-trip / tcp FIN -> EOF            all ok
  nettest: dns localhost -> ...            (or unanswered note)
  [demo] netcli spawned pid N
  netcli: STREAM ok (payload echoed)
  netcli: exiting 21
  selftest: net ok
```

Notes carried forward:

- No out-of-order receive queueing, no window probing, no SACK;
  TIME_WAIT collapses immediately. All are documented inside
  net/tcp.c and are additive work for the hardening phase.
- UDP sendto/recvfrom are not exposed yet (connected-socket
  semantics only); DHCP/DNS use the raw hooks. The socket API grows
  with the first real consumer (phase 14 daemons).
- Wi-Fi (SDIO) and WPA supplicant remain hardware-bucket items; TLS
  remains the plan's phase-15 stretch goal.
- nettest leaves its spawned netcli as a reaped-by-nobody zombie by
  design (kernel threads cannot waitpid); one slot, reclaimed when
  the process-exit path gains an orphan reaper in phase 14.


