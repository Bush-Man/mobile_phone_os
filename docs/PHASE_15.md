# Phase 15 — UI Framework & Phone Apps (Implementation Log)

Milestone scope reached: a compositor daemon that owns fb0/event0
and composites shm-backed app windows into a private full-screen
stage, presenting dirty frames through the new fb0 blit ioctl
(item 79); an immediate-mode C widget toolkit with surface gfx
(8x8 font ported from the phase-9 kernel core), themes, buttons,
lists, tap hit-testing and an on-screen keyboard (items 80+84);
a PIN lock screen, home screen and launcher grid that fork/exec
the apps (item 81); six phone apps -- Dialer, Messages, Contacts
(fs-backed /var/contacts DB), Settings, Clock, Calculator (item
82) -- with a notification-banner framework and a status bar
carrying battery/signal/call-state/time (item 83); and the
telephony service broker "modemd", a kernel task serving a line
protocol on /var/run/modem so EL0 apps dial, answer, hang up and
send SMS while call/SMS events fan out to every connected client.
Milestone delivery in uitest15 + the compositor's own serial
proofs: protocol round trip (HELLO/OPEN/shm/SHOW/FOCUS/NOTIFY),
an injected DELIVER PDU decoded into "[ui] banner: SMS ...",
synthetic touches at the shared ui_layout.h numpad centers
unlocking PIN 1234 ("[ui] unlock ok"), the Dialer launcher tap
fork/execing the app ("[ui] launch dialer" + "[dialer] ready"),
and a RING URC surfacing as a call banner. Per the standing
coordination decision no make target ran; 24 units passed the
per-file -fsyntax-only sweeps.

## Order of files written

### Batch A — enablers + the UI contract (contract first)

1. **`include/ipc.h`** — shm limits raised for window surfaces:
   SHM_PAGE_MAX 16 -> 512 (one 800x600 XRGB8888 surface is 469
   pages), SHM_OBJS_MAX 8 -> 12 (root chrome + six apps + test
   client).
2. **`kernel/syscall.c`** — fb0 ioctl, per-device dispatch: the
   console keeps TTY_RAW/CANONICAL, fb0 gains FBIO_BLIT (copy a
   rect from CALLER memory into the framebuffer, destination
   page-chunked like fb0_write, every row uaccess-validated),
   FBIO_FILL and FBIO_INFO. This is the compositor's whole
   presentation path -- no user-mappable device memory needed.
3. **`kernel/modemd.c`** — the telephony broker: serves
   /var/run/modem over the phase-8 unix transport; line protocol
   PING/STATUS/DIAL/ANSWER/HANGUP/SMS/REG/SIGNAL answered with
   local line-builder helpers (no kernel snprintf); each accepted
   connection gets its own reader task, and modem call/SMS events
   fan out as "EV CALL <name>" / "EV SMS <from> <text>" to all
   live clients.
4. **`userspace/ui/ui.h`** — the single UI header: 32-byte
   fixed-layout ui_msg protocol (HELLO/WELCOME/OPEN/OPENED/FLUSH/
   SHOW/HIDE/ACTIVATE/EVENT/FOCUS/NOTIFY/CLOSED), layout
   constants, ui_surface, theme struct, widget + keyboard APIs,
   client API.
5. **`userspace/ui/ui_gfx.c`** — packed-XRGB8888 surface ops
   (fill/rect/text) with the kernel's 8x8 glyph table ported and
   extended (! ? , % < > * ( )); unknown chars keep the solid-box
   fallback.
6. **`userspace/ui/ui_widgets.c`** — dark theme, ui_hit, label,
   button, list, and the four-row on-screen keyboard (digits,
   QWERTY, home row, punctuation + backspace) whose tap handler
   delivers one char per press (item 84).
7. **`userspace/ui/ui_client.c`** — the app-side protocol
   library: connect/HELLO/WELCOME, OPEN -> OPENED(shm id, x, y)
   -> shmat, flush (dirty rect clamped), show/hide/notify, and
   the blocking 32-byte message pump. libc grew the wrappers this
   needs: shmget/shmat/shmdt, usock_serve/usock_connect and the
   raw ioctl().

Commits: "Raise shm limits for UI surfaces, add fb0 blit/fill/
info ioctl, modemd service broker (phase 15)"; "Add UI toolkit:
protocol header, surface gfx with ported font, widgets, keyboard,
client library (phase 15)"; "Add modemd service broker: unix-
socket telephony protocol with event fan-out (phase 15)".

### Batch B — compositor + apps + battery

8. **`include/ui_layout.h`** — the chrome-geometry CONTRACT:
   status/home bar heights, lockscreen numpad grid and home
   launcher grid as center-arithmetic macros. Pure u32
   arithmetic, no includes, so the kernel selftest and the
   compositor compile the SAME numbers -- the synthetic-tap
   milestone depends on it. Do not fork these constants.
9. **`userspace/compositor.c`** — the daemon (item 79). Four
   pthread-lite threads behind one ui_lock: input (blocks on
   /dev/event0, routes taps by mode: lockscreen numpad, launcher
   grid, focused-window forwarding with window-relative ABS
   coords), render (100 ms tick; recomposites chrome + windows
   and blits on need_redraw or the 1 Hz clock tick), server
   (accept loop, one reader thread per connection), modem (line
   client on /var/run/modem: EV SMS -> banner + unread badge, EV
   CALL -> status pill + banners, SIGNAL replies -> bars). The
   stage is 8 contiguous 256 KiB mmaps (2 MiB, 800x600 XRGB8888);
   the window table holds one shm object per app; KEY_POWER
   re-locks. Launcher taps fork/exec the app with the compositor's
   device/listener/client fds closed first -- an inherited
   duplicate modem fd would stall modemd's fan-out.
10. **`userspace/dialer.c`** — the phone app: keypad grid, number
    display, CALL/DEL/END, DIAL over the modem protocol; a reader
    thread owns the modem fd's inbound side so replies and EV CALL
    lines update the status strip without ever interleaving a
    request.
11. **`userspace/msgs.c`** — SMS/Messaging: loads the kernel
    /sms/msg<N> store, appends live EV SMS arrivals, compose via
    the on-screen keyboard, send via "SMS <num> <text>".
12. **`userspace/contacts.c`** — fs-backed DB: "name|number"
    lines in /var/contacts, rewritten on every ADD/DEL so entries
    survive restarts; keyboard editing, tap-to-select list.
13. **`userspace/settings.c`** — Settings rows (wifi/bt honest
    "no radio in QEMU" toggles, battery snapshot, display).
14. **`userspace/clock.c`** — Clock: big HH:MM:SS from SYS_gettime
    with an uptime fallback while the epoch is 0.
15. **`userspace/calc.c`** — Calculator: 4x4 key grid, display
    line, immediate binary evaluation with error state.
16. **`userspace/uitest.c`** — the protocol battery: full client
    chain against the compositor, test pattern drawn, exits 0
    only when SHOW -> FOCUS -> NOTIFY all answered; an 8 s
    watchdog thread exits 1 on a stall.
17. **`kernel/selftest_ui.c`** — the "uitest15" battery (EL1
    half): protocol spawn + reaped exit 0; DELIVER PDU injection
    (phase-12 layout, sender 5551234, text "PHASE15 SMS") with
    sms_seq() growth asserted; synthetic touches at the
    ui_layout.h numpad centers for PIN 1234 + OK; the Dialer
    launcher tap; a RING URC for the call banner.
18. **`kernel/phase15.c`** — phase entry: arms uitest15 (the
    compositor and apps are EL0 built-ins init spawns; modemd
    rides phase12_init).
19. **`userspace/init.c`** — spawns the compositor (deliberately
    NOT critical: on a GPU-less boot it exits gracefully instead
    of respawn-looping).
20. **`kernel/proc.c` + `include/proc.h`** — eight new builtin
    images (compositor, dialer, msgs, contacts, clock, calc,
    settings, uitest) and PROC_SHM_MAX 4 -> 8 (the compositor
    keeps one mapping per open window).
21. **`include/syscall.h` + `kernel/syscall.c`** — SYS_usock_accept
    55: the compositor's server thread accepts through a syscall
    (SYS_usock_serve hands back a listener fd; usock_accept
    blocks and installs the connection fd).
22. **`kernel/main.c`** — phase15_init() after phase14_init(),
    BANNER -> phase 15.
23. **`Makefile` + `arch/aarch64/builtin_imgs.S`** — UI_LIB_OBJS
    pattern rule (-Iinclude for ui_layout.h), the eight UI
    programs, incbin entries, `make test` phase 15.
24. **`tests/serial_harness.py`** — phase-15 mode: QEMU gets the
    virtio-gpu/tablet/keyboard devices even under -display none,
    deadline 30 s (the init -> compositor -> app chain takes a
    while), plus the milestone grep set ("[uitest] protocol ok",
    "[ui] unlock ok", "[ui] launch dialer", "[dialer] ready",
    "[ui] banner: SMS", "selftest: ui ok"); the phase argument is
    an int now, fixing the old lexicographic-string quirk.

Commits: "Add phase15 bring-up, uitest15 battery, SYS_usock_accept,
task budget for UI threads (phase 15)"; "Add compositor daemon,
phone apps and protocol test client (phase 15)"; "Embed UI
binaries, add toolkit build rules and phase-15 harness checks
(phase 15)".

### Batch C — integration fixes (the cross-session review)

25. **`drivers/modem.c`** — modem_set_call_handler/
    modem_set_sms_handler were SINGLE slots, and phase 15 needs
    two listeners: the phase-12 battery registers its observers
    from the modtest task (prio 56) while modemd (prio 40, so it
    runs FIRST) registers its fan-out -- whichever ran later won,
    a boot-order race that silently killed the UI event path.
    Handlers are now a 4-entry observer chain walked on delivery;
    the setter API is unchanged (the same fn+arg pair re-registers
    as a no-op, NULL unregisters), so phase-12 behavior is intact.
26. **`include/task.h` + `kernel/sync.c`** — MAX_TASKS 32 -> 64:
    the compositor alone runs four threads plus one reader per
    client connection and each app brings its own; the deadlock
    detector's visit budget follows (64 -> 128 = 2*MAX_TASKS).
    64 slots cost 1 MiB of static stacks.
27. **`kernel/phase15.c`** — missing <stddef.h> (NULL).
28. **`userspace/compositor.c`** — strtoul's end pointer is
    const char* per the libc ABI.
29. **`userspace/calc.c`** — key-table initializer kept its NUL
    (unterminated-string-init warning).

Commit: "Modem event handlers: observer chain so modtest and
modemd coexist (phase 15)" (batch C items 25-29 ride it and the
phase-15 bring-up commit).

## Milestone mapping

- "unlock -> dialer -> call" -> uitest15 pushes synthetic touches
  at the ui_layout.h numpad centers (PIN 1234 -> "[ui] unlock
  ok"), taps the Dialer launcher cell (compositor fork/execs the
  app -> "[ui] launch dialer" + "[dialer] ready (window N)"), and
  the injected RING URC fans out "EV CALL RING" to the dialer's
  connection and the compositor's, raising the CALL RING banner.
  The dialer itself drives DIAL/CONNECT/HANGUP over the same
  modemd protocol the mock answers, with the phase-12 call state
  machine and the phase-13 audio seam underneath.
- "receive SMS shows notification" -> the injected DELIVER PDU is
  decoded by the phase-12 layer, stored under /sms/msg<N>, fanned
  out by modemd as "EV SMS 5551234 PHASE15 SMS", rendered by the
  compositor as "[ui] banner: SMS 5551234: ..." with the Messages
  unread badge incrementing -- asserted from EL1 by sms_seq()
  growth and from the harness by the banner line.
- Items 80/83/84 ride along everywhere: the toolkit (themes,
  buttons, lists, keyboard) is what the apps draw with, the
  status bar carries signal/battery/call-state/time, and banners
  are the notification framework.

## Bugs found (and fixed) along the way

- **Modem handler single-slot race**: modtest and modemd both
  register event observers; boot priority order meant modemd won
  the slot first and modtest overwrote it, so no UI event would
  ever fan out (or vice versa, depending on priority churn).
  Converted to an observer chain -- the phase-15 SMS/call
  milestone is unreachable without this fix.
- **Task-table exhaustion**: 32 slots could not hold the compositor
  (4 threads + per-client readers), six apps with their own
  threads, modemd's per-connection readers and the kernel
  batteries. MAX_TASKS 64 with the deadlock budget doubled.
- **kernel/phase15.c NULL**: no <stddef.h>; caught by the sweep.
- **strtoul end-pointer type** in the compositor's SIG parser
  (char* vs the libc's const char**).
- **calc.c key-table NUL truncation warning**; harmless but fixed.

## Design decisions worth remembering

- **The compositor never touches device memory**: it draws into a
  private 2 MiB stage (8 contiguous mmaps -- the mmap window
  marches upward, so consecutive anon mmaps are contiguous) and
  presents through FBIO_BLIT/FBIO_FILL ioctls, with the kernel
  validating and page-chunking every row. Apps never see the fb
  at all; their shm surfaces are compositor USER memory during
  composition (plain memcpy), so "window surfaces via shared
  memory + IPC" needs exactly one new kernel capability: the
  fb0 blit.
- **One shared geometry header**: include/ui_layout.h is included
  by BOTH the compositor and the kernel selftest; the synthetic
  touch taps only land because the two sides compile the same
  numpad/icon centers. Treat it as ABI.
- **Apps are spawned on demand**: the compositor fork/execs a
  launcher target only when tapped (raising an existing window
  otherwise), and the child closes every inherited descriptor
  before exec -- compositor device/listener/client fds included.
- **modemd owns the modem's event handlers** (via the observer
  chain); apps and the compositor are just line-protocol clients.
  Every inbound side is owned by exactly one reader thread so
  replies and events never interleave mid-line.
- **Fixed 32-byte messages** everywhere: no dynamic allocation in
  the protocol, and the compositor's tables are static.

## Verification status

Per coordination decision no make/QEMU target ran this phase;
sweep over 24 units (per-file -fsyntax-only, Makefile flags,
-Wno-cast-function-type for the pre-existing table style):

```
CLEAN kernel/{phase15.c,selftest_ui.c,modemd.c,syscall.c,main.c,
              phase12.c,proc.c,sync.c,task.c}
CLEAN drivers/modem.c
CLEAN userspace/ui/{ui_gfx.c,ui_widgets.c,ui_client.c}
CLEAN userspace/{compositor,dialer,msgs,contacts,settings,
                 clock,calc,uitest,init}.c
CLEAN userspace/libc/{unistd.c,malloc.c}
```

When integration lands expect, in order:

```
make test        # cumulative criteria incl. banner "phase 15";
                 # QEMU now carries virtio-gpu/tablet/keyboard
serial adds:
  modemd: serving /var/run/modem
  compositor: starting (pid N) / [ui] compositor ready
  [ui] hello: uitest / [ui] window uitest: id 1 shm .. at ..
  [uitest] opened win 1 shm 0 at 160,56
  uitest15: ui protocol round trip                    (ok)
  [ui] banner: SMS 5551234: PHASE15 SMS
  uitest15: sms decoded (banner fanned)               (ok)
  [ui] tap 296,228 (mode 0) ... / [ui] unlock ok
  [ui] launch dialer / [dialer] ready (window 2)
  [ui] call event: RING / [ui] banner: CALL RING
  selftest: ui ok
```

Notes carried forward:

- The harness phase argument is an int now; the old string
  compares silently skipped the phase-4/5 checks for phases > 9.
- The compositor exits gracefully without fb0 (GPU-less boot);
  init deliberately does not respawn it.
- Wifi/BT in Settings are honest no-radio placeholders; real
  radios are the phase-11/16 hardware items.
- Text shaping is the 8x8 bitmap font only; subpixel/resampled
  text belongs to the phase-16 polish bucket.
- Full-screen banners, window dragging and multi-window tiling
  are natural next steps on the same protocol (UI_MOVE/UI_DRAG
  message types are unused today).
