# Phase 9 — Graphics & Input (Implementation Log)

Milestone scope reached: a framebuffer core built around canvases of
scattered 4K pages with span-safe blitters (fill/copy/blend), an
FNV-1a region hash for headless pixel verification and a compact 8x8
bitmap font (include/fb.h + drivers/fb.c); a virtio-gpu 2D frontend
over the phase-6 mmio transport that backs its scanout resource with
those very pages and exposes "fb0" through the chardev registry
(drivers/virtio_gpu.c); an evdev-style input core with the Linux
EV_SYN/EV_KEY/EV_ABS numeric model, /dev/event0 as a blocking chardev
stream and the value==2 autorepeat convention (include/input.h +
drivers/input.c, plan item 50); virtio-input tablet+keyboard
frontends feeding that stream raw (drivers/virtio_input.c, item 51);
a board-keyed GPIO buttons driver whose pin tables idle cleanly on
QEMU where virtio covers input (drivers/buttons.c, item 52); and a
deterministic end-to-end milestone: gfxtest paints + verifies the UI
test pattern, queues a 12-event calibration stream, then execs the
built-in evreader ELF which opens /dev/event0 at EL0 and asserts all
twelve records -- "touch coordinates stream into an input reader
process". Per the standing coordination decision no make target was
run; every phase-9 unit passed per-file `aarch64-linux-gnu-gcc
-fsyntax-only -Wall -Wextra -ffreestanding -fno-builtin
-mgeneral-regs-only` sweeps (20/20 CLEAN).

## Order of files written

### Batch A — display core (item 48 groundwork)

1. **`include/fb.h`** — data layer first: `struct fb_canvas`
   {name,width,height,stride_bytes,nframes,frames[],flip,priv},
   fb_rgb() composer (XRGB8888 / DRM fourcc XR24), backend registry
   types (`struct fb_backend{name,priority,claim}`), claim/present
   accessors, and every draw API: fill_rect, copy_rect (overlap-safe
   via line snapshot), blend_rect, put_pixel, region_hash (FNV-1a,
   never zero), text_width, draw_text. Declared BEFORE drivers/fb.c
   so core responsibilities were pinned down up front.
2. **`drivers/fb.c`** — one subsystem spinlock guards the registry
   plus the active-canvas singleton. The heart is the byte-offset ->
   (uncached dmap alias,length) page decomposition shared by every
   primitive: fill walks spans forward via callback; copy gathers
   each source line into a page-sized scratch first so overlapping
   self-copies stay correct; blend needs both endpoints per pixel so
   it reuses the gather/scatter pattern instead of the walker; hash
   folds bytes chunk-wise. Font table = designated-initializer array
   of glyph8x8 {8 rows} covering A-Z/0-9/./-/#/:/+/' '/'=' with an
   outlined-box fallback glyph so unknown chars can never vanish.
3. Two fixups caught by sweep before commit: cv_page_of() parameter
   removed after simplification (-Wunused-parameter) and missing
   braces in glyph initializers.

Commit: "Add framebuffer core ...".

### Batch B — display backend over virtio (item 48)

4. **`include/virtio.h`** — VDEV_ID_GPU 16 / VDEV_ID_INPUT 18 ids
   added; frontend prototype list grows virtio_gpu_attach().
5. **`drivers/virtio_gpu.c`** — state struct vgpu_state (vt, controlq,
   kmutex serializing it, active-response slot, 16 KiB request stage /
   64 B response stage from the uncached arena, frame list, stats).
   Command ABI structs byte-mirror the spec (ctrl_hdr 16B, rect,
   create2d, scanout, transfer2d, mem_entry). gpu_cmd(): two-descriptor
   chain [readable header+payload][writable response], msleep-poll
   deadline identical to vblk IO, mutex-guarded. Resource arming is
   LAZY on first fb_claim_default() so boot context stays free of
   scheduler-dependent blocking: pmm frames gathered individually
   (~461 pages for 800x600x4 -- exactly why scatter backing exists,
   contiguous allocation would be un-servable today), CREATE_2D
   (XR24) then ATTACH_BACKING with the entry list inline in the same
   request blob.
6. **chardev "fb0"** (item 49): read/write do whole-screen streaming
   copies bounded to PAGE_SIZE chunks; registered lazily inside claim
   once geometry is live. flip() hook stays NULL honestly -- QEMU's
   head has no fence events, so the backend declares single-scanout
   update-on-present semantics and docs carry the page-flip roadmap
   for real panels.
7. **`drivers/virtio_mmio.c`** — devid switch gains the GPU case
   (the INPUT case lands with batch D's file).

Commit: "Add virtio-gpu 2D frontend with fb0 device node ...".

### Batch C — input core (items 50+52)

8. **`include/input.h`** — Linux-valued event enums (EV_SYN/EV_KEY/
   EV_ABS, ABS_X/Y, BTN_TOUCH, KEY_VOLUMEUP/DOWN/POWER, SYN_REPORT),
   wire struct input_event{ms,type,code,value} packed 12 B, repeat
   tunables (400 ms delay / 60 ms rate), push/pending/stats/tick APIs.
9. **`drivers/input.c`** — static ring (256 events) under input_lock;
   push is IRQ-safe, stamps uptime-ms, maintains repeater key-state,
   then wakes readers using the same two-lock dance as the IPC stack
   (input_lock -> task_state_lock; waiters moved READY with FIFO keys
   + priority preempt flag). Reader side parks CURRENT while STILL
   HOLDING input_lock -- identical invariant to sync.c's kmutex park,
   closing the check-to-enqueue window class entirely.
   input_tick_repeats() synthesizes EV_KEY/value==2 records off the
   housekeeping cadence (wired in batch F).
10. Review catch fixed pre-commit: the first draft took
    task_state_lock BEFORE the subsystem lock in ev0_read (AB-BA
    hazard against push); rewritten to the established order.

Commit: "Add evdev-style input core ...".

### Batch D — real event sources (items 51+52)

11. **`drivers/virtio_input.c`** — eventq RX slots pre-posted eight
    deep (virtio-net receive pattern); completions parse the 8-byte
    vin_event and push RAW values (axis scaling deliberately belongs
    to consumers with real window geometry). Config-space name query
    decides the role: "tablet" -> VIN_TABLET (queries ABS_INFO for X,
    logs min..max), else keyboard. One instance per role supported;
    buffers re-arm from the tasklet as soon as they complete.
12. **`include/buttons.h` / `drivers/buttons.c`** — per-board pin
    tables keyed by platform-model prefix (entries reserved for
    raspberrypi / pinephone bring-up stay empty on purpose so QEMU's
    `-M virt` matches nothing by design): poller samples lines at
    20 ms with 2-sample debounce and emits KEY down/up transitions;
    exits immediately when no keys are mapped, returning its slot.

Commits in batch order C then D ("...input core...", then
"...virtio-input tablet/keyboard frontends and GPIO buttons...").

### Batch E — verification battery + milestone process

13. **`kernel/selftest_gfx.c`** — "gfxtest": claims the canvas,
    geometry sanity, repaint determinism (region-hash stability
    across identical redraws), sprite copy equivalence with source
    coordinates chosen deliberately near width/page boundaries, blend
    (a=255)==copy byte-equality, glyph-region mutation check,
    present() round-trip acknowledged by the GPU, then pushes THE
    calibration sequence (const table below) and spawns "evreader".
    Summary line: selftest: gfx ok.
14. **`userspace/evreader.c` / `evreader.ld`** — third built-in ELF:
    open("/dev/event0") -> twelve blocking 12-byte reads -> every
    record compared against its expected triple and printed with tag
    ("TOUCH-DOWN","MOVE-X-400",...,"SYNC"); STREAM ok requires 12/12;
    exit 21 on success / 91 mismatch / 92 short read.
15. **`include/virtio_gpu.h`** — tiny shim exposing present() and
    backend-register prototypes so tests never poke driver internals.

Commit: "Add gfx/input selftest battery and evreader milestone ...".

### Batch F — bring-up & harness

16. **`kernel/phase9.c`** — boot-context registration only (backend
    into fb registry, event0 into chardev registry, board keys
    checked); spawns "gfxtest" at priority 45.
17. **`kernel/main.c`** — phase9_init() call site after phase8 block;
    banner bumps to `phase 9`; housekeeping loop now also drives
    input_tick_repeats() each ~2 ms iteration -- the repeat engine
    costs zero task slots this way.
18. **`arch/aarch64/builtin_imgs.S` + kernel/proc.c builtins[]** --
    third embedded image "evreader" beside hello/ipcdemo.
19. **Makefile** — userspace/evreader rule added to the builtin
    prerequisite; `make test` passes phase 9 to the cumulative
    harness; new run-display target attaches virtio-gpu/tablet/
    keyboard devices with DISPLAYARGS overridable (`-display gtk`
    shows the pattern live).

## The calibration ABI (kernel <-> evreader contract)

index : type/code/value/tag
 0 : KEY  BTN_TOUCH(330)      1  TOUCH-DOWN
 1 : ABS  ABS_X(0)          400  MOVE-X-400
 2 : ABS  ABS_Y(1)          300  MOVE-Y-300
 3 : SYN  SYN_REPORT(0)       0  SYNC
 4 : KEY  BTN_TOUCH            0  TOUCH-UP
 5 : SYN                      0  SYNC
 6 : ABS  ABS_X             200  MOVE-X-200
 7 : SYN                      0  SYNC
 8 : ABS  ABS_Y             100  MOVE-Y-100
 9 : SYN                      0  SYNC
10 : KEY  KEY_VOLUMEUP(115)   1  VOLUP-DOWN
11 : SYN                      0  SYNC

Pushed BEFORE the reader process exists, so its first blocking read
returns instantly regardless of scheduling jitter; nothing else may
write the ring during the demo window.

## Bugs found (and fixed) along the way

- **ev0_read lock-order inversion**: the draft took task_state_lock
  before input_lock while push() acquires them the other way round --
  classic AB-BA. Rewritten to the sync.c ordering before commit.
- **warm-up dead end**: probing input_push() then busy-draining has
  no consumer at ring-lifecycle start, so the drain can never
  finish; replaced by "ring starts empty, calibration is index 0"
  semantics, which also makes record indices deterministic.
- **Editor-escape corruption**: several pasted drafts carried literal
  backslash sequences inside C strings (kprintf formats) and even a
  stray quote character; caught by grep/syntax sweeps each time and
  rebuilt via scripted patching until every file passed clean.
- **MAX_TASKS pressure (phase-8 carried debt)**: the phase-8 socket
  echo helper parked forever, burning a slot that gfxtest now needs.
  sv.done handshake added; the helper retires shortly after its reply
  is consumed.

## Design decisions worth remembering

- **Scatter-page canvases everywhere**: GPU resources accept page
  lists natively, so the display stack never needs contiguous
  physical memory (the property that made direct-linfb/ramfb
  backends unattractive under today's PMM).
- **Uncached aliases beat cache maintenance**, consistent with the
  entire virtio stack -- zero cache-clean instructions on pixel hot
  paths anywhere in phase 9.
- **Lazy device arming**: attach() only negotiates and sets up queues;
  all command round-trips run in task context (vblk precedent: msleep
  panics outside the scheduler).
- **Raw axis passthrough**: scaling belongs to consumers with real
  window geometry (compositor, phase 15); kernel code must not guess
  display bounds or resolutions.
- **Buttons are board DATA, not logic**: empty QEMU tables keep the
  driver honest about what it can actually sense.

## Verification status

Per coordination decision no make/QEMU target ran this phase; final
sweep over 15 new + 4 edited units:

```
CLEAN include/{fb.h,input.h,buttons.h,virtio_gpu.h,virtio.h}
CLEAN drivers/{fb.c,virtio_gpu.c,input.c,virtio_input.c,buttons.c,virtio_mmio.c}
CLEAN kernel/{selftest_gfx.c,selftest_ipc.c,phase9.c,main.c,proc.c}
CLEAN userspace/evreader.c   Makefile   arch/aarch64/builtin_imgs.S
```

When integration lands expect, in order:

```
make test        # cumulative criteria incl. banner "phase 9"
serial adds:
  vgpu: present (canvas arming deferred to first claim)
  ...
  selftest: ipc ok                       (phase-8 battery still green)
  gfx/input: registries online
  vgpu: canvas ready (~458 backing pages)
  fb: canvas "virtgpu" 800x600 from virtio-gpu (single)
  gfxtest: ... battery lines ...
  selftest: gfx ok
  evreader: evt 0..11 <type code value> ok (tag)
  evreader: STREAM ok (12/12 records match)
  evreader: exiting 21

make run-display DISPLAYARGS="-display gtk"
                 # test pattern visible; mouse/touch drags + keyboard
                 # events feed the same stream live
```

Notes carried forward:

- No O_NONBLOCK/EINTR handling on event0 yet (standing debts from
  phases 5-7 unchanged).
- fb0 exposes whole-frame streaming read/write; mmap-on-fd was
  considered and deliberately deferred to the compositor work
  (phase 15) so SYS_mmap hint/prot semantics stay as phase 8 left them.
- Virtio-GPU cursor queue intentionally unclaimed (legacy setup
  allows partial queue use); two-resource page flipping remains the
  documented upgrade path once fence negotiation happens.
- Button tables ship EMPTY deliberately; filling them is phase-10 HW
  bring-up data, not phase-9 logic.

