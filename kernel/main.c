/*
 * main.c - phase 4 bring-up sequence.
 */

#include <stdint.h>

#include "el.h"
#include "exceptions.h"
#include "gic.h"
#include "input.h"
#include "irq.h"
#include "battery.h"
#include "audio.h"
#include "watchdog.h"
#include "modem.h"
#include "net.h"
#include "virtio.h"
#include "pm.h"
#include "psci.h"
#include "lib.h"
#include "mm/kheap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "panic.h"
#include "platform.h"
#include "proc.h"
#include "psci.h"
#include "smp.h"
#include "task.h"
#include "tasklet.h"
#include "time.h"
#include "uart.h"

extern uint8_t vectors_begin[];
extern uint8_t _start[];

void mem_selftest(void);
void irq_time_selftest(void);
void sched_selftest(void);
void sched_demo_start(void);
void phase6_init(const struct platform_info *plat);
void phase7_init(const struct platform_info *plat);
void phase8_init(const struct platform_info *plat);
void phase9_init(const struct platform_info *plat);
void phase10_init(const struct platform_info *plat);
void phase11_init(const struct platform_info *plat);
void phase12_init(const struct platform_info *plat);
void phase13_init(const struct platform_info *plat);
void phase14_init(const struct platform_info *plat);
void phase15_init(const struct platform_info *plat);
void phase16_init(const struct platform_info *plat);

static void housekeeping_task(void *arg)
{
    unsigned long mark = 0;
    uint64_t ms;
    (void)arg;

    for (;;) {
        tasklet_drain();                /* bottom halves here       */

        /*
         * phase 16: the software watchdog heartbeat -- as long as
         * this loop runs, the deadline keeps moving
         */
        watchdog_kick();

        /* phase 9: autorepeat engine runs off this cadence         */
        input_tick_repeats();

        /*
         * phase 10: display suspend policy + 1 Hz battery sampling
         * and warn transitions ride the same housekeeping loop --
         * zero dedicated tasks for the whole PM subsystem.
         */
        ms = time_uptime_ms();
        pm_display_tick(ms);
        battery_poll_tick(ms);

        /*
         * phase 11: stack timers (TCP retransmit/backoff, ARP retry,
         * DHCP re-arm) plus virtio-net RX re-arm -- the receive
         * slots only return to the ring when poll() runs.
         */
        net_timers_tick(ms);
        virtio_net_poll();

        /*
         * phase 12: AT engine drain + timeout/retry enforcement.
         */
        modem_tick(ms);

        /*
         * phase 13: ringer synthesis + backend drain accounting.
         */
        audio_tick(ms);

        /*
         * phase 14: dead-and-parked THREAD slots get their stacks
         * freed here (kernel task context; only settled slots are
         * touched) and a freed thread can unblock an init reap.
         */
        proc_threads_reclaim();

        /* one uptime line per second */
        if ((long)(jiffies_read() - mark) >= TIME_HZ) {
            mark += TIME_HZ;
            ms = time_uptime_ms();
            kprintf("time: %llu.%03llu s uptime\n",
                    (unsigned long long)(ms / 1000u),
                    (unsigned long long)(ms % 1000u));
        }
        msleep(2);
    }
}

extern const struct platform_info *smp_plat;

#define BANNER "[OK] mobile_phone_os phase 16"

/* phase 16 (item 88): boot-time measurement -- stamped right
 * after time_init, reported just before the banner                 */
static uint64_t boot_t0_ms;

/*
 * Phase 5 milestone demo: spawn the built-in static "hello" ELF at
 * EL0, then reap it and report the exit code the kernel received.
 * Runs as its own task so waitpid() can block like any syscall.
 */
static void process_demo_task(void *arg)
{
    int pid, code, rc;

    (void)arg;

    pid = proc_spawn("hello", (const char *const []){ "hello", NULL },
                     NULL);
    if (pid < 0) {
        kprintf("[demo] hello spawn failed (%d)\n", pid);
        task_exit();
    }

    rc = proc_kernel_wait(pid, &code, 10000u);
    if (rc == pid)
        kprintf("[demo] hello exited with code %d\n", code);
    else
        kprintf("[demo] waitpid(%d) failed (%d)\n", pid, rc);

    task_exit();
}

void kmain(uint64_t boot_el, uint64_t dtb_ptr)
{
    struct platform_info plat;
    struct pmm_stats ps;
    struct kheap_stats ks;
    struct irq_stats is;
    struct tasklet_stats ts;

    (void)dtb_ptr;

    uart_init();
    kprintf("\nmobile_phone_os kernel\n");

    el_drop_to_el1();

    /* phase 10: PSCI layer snapshot for reset/off/suspend paths    */
    psci_init(&plat);
    kprintf("bringup: running at EL%llu (boot EL%llu)\n",
            (unsigned long long)el_current(),
            (unsigned long long)boot_el);

    vectors_init();
    kprintf("bringup: vectors installed at %p\n", vectors_begin);

    platform_self(&plat);
    kprintf("platform: model \"%s\"\n", plat.model);
    kprintf("platform: RAM %llu MiB @ 0x%llx\n",
            (unsigned long long)(plat.ram_size >> 20),
            (unsigned long long)plat.ram_base);
    if (plat.has_uart)
        kprintf("platform: console UART @ 0x%llx (rx INTID %u)\n",
                (unsigned long long)plat.uart_base, plat.uart_irq);
    if (plat.has_gic)
        kprintf("platform: GICv%d dist 0x%llx cpu 0x%llx (timer INTID %u)\n",
                plat.gic_version,
                (unsigned long long)plat.gicd_base,
                (unsigned long long)plat.gicc_base,
                plat.timer_irq);
    if (plat.has_boot_args)
        kprintf("platform: bootargs \"%s\"\n", plat.boot_args);

    if ((uintptr_t)_start != 0x40000000UL)
        panic("kernel not loaded at its link address");

    vmm_init(&plat);
    kprintf("bringup: memory management up (TTBR0/TTBR1 + caches on)\n");

    mem_selftest();

    pmm_stats_get(&ps);
    kheap_stats_get(&ks);
    kprintf("mm: %llu/%llu frames free, heap %llu/%llu allocs\n",
            (unsigned long long)ps.free_frames,
            (unsigned long long)ps.total_frames,
            (unsigned long long)ks.allocs,
            (unsigned long long)ks.frees);

    gic_init(&plat);
    kprintf("irq: GICv%d online\n", plat.gic_version);

    /*
     * phase 16 (item 85): re-randomize the stack-smashing guard
     * from the architected counter. panic.c ships a fixed value so
     * early boot is deterministic; once the counter runs, every
     * boot gets a different guard.
     */
    __stack_chk_guard ^= time_counter_value();
    __stack_chk_guard |= 1ull;

    time_init(&plat);
    boot_t0_ms = time_uptime_ms();
    kprintf("time: %u Hz system counter, %u Hz tick (INTID %u)\n",
            time_counter_hz(), TIME_HZ, plat.timer_irq);

    /*
     * phase 10: wake-source table (item 54). The port keeps the GIC
     * armed across WFI, so these SPIs/PPIs ARE the wake events; real
     * deeper-state boards extend this table with per-depth masks.
     */
    pm_wake_source_note(plat.timer_irq, "timer");
    pm_wake_source_note(plat.uart_irq, "uart-rx");

    uart_rx_irq_init(plat.uart_irq);
    kprintf("uart: interrupt-driven echo armed\n");

    irq_time_selftest();

    irq_stats_get(&is);
    tasklet_stats_get(&ts);
    kprintf("selftest: irq (sgi, chain, timer) ok "
            "(raised %llu handled %llu, tasklets ran %llu)\n",
            (unsigned long long)is.raised,
            (unsigned long long)is.handled,
            (unsigned long long)ts.ran);

    /*
     * Phase 4: scheduler + secondaries. cpu1 is released out of the
     * firmware PSCI parking pen (it brings up its own MMU/GIC/timer,
     * then runs its scheduler loop), after which the strict
     * cross-cpu ping-pong selftest runs before the milestone demo
     * threads take the stage.
     */
    sched_init();
    smp_plat = &plat;
    smp_init();
    kprintf("smp: %u cpu%s released via PSCI\n", NR_CPUS,
            NR_CPUS == 1 ? "" : "s");
    if (plat.has_psci)
        kprintf("platform: PSCI %s conduit, CPU_ON 0x%x\n",
                plat.psci_hvc ? "hvc" : "smc", plat.psci_cpu_on_fn);
    sched_selftest();

    /*
     * Phase 6: driver framework + core drivers. FDT enumeration and
     * probe lifecycle, tty console takeover, virtio-blk/net, gpiolib,
     * block + partition layer; the blocking selftest runs as the
     * "drvtest" task once the schedulers start (see docs/PHASE_6.md).
     */
    phase6_init(&plat);

    /*
     * Phase 7: filesystems. Registers ramfs/devfs/vfat/ext2, mounts
     * the in-memory root and /dev, and spawns "fstest" (which also
     * negotiates the disk layout, mounts vfat + ext2 partitions and
     * proves file persistence across reboots -- see docs/PHASE_7.md).
     */
    phase7_init(&plat);

    /*
     * Phase 8: IPC, sync & the POSIX-ish surface. Registers the
     * pipe/shm/mqueue/socket pools, then spawns "ipctest" (mutex,
     * detector, semaphore, pipe, shm and unix-socket battery) and
     * the "ipcdemo" starter, whose built-in EL0 binary forks two
     * processes talking through a pipe AND shared memory -- the
     * milestone proof (see docs/PHASE_8.md).
     */
    phase8_init(&plat);

    /*
     * Phase 9: graphics & input. Registers the display backend(s)
     * and the /dev/event0 stream, then spawns "gfxtest" which arms
     * the virtio-gpu canvas, draws+verifies the UI test pattern,
     * pushes the calibration events and execs the evreader process
     * (milestone proof -- see docs/PHASE_9.md).
     */
    phase9_init(&plat);

    /*
     * Phase 10: power management & battery. PSCI system layer came
     * online at boot (platform snapshot), the idle governor took
     * over the scheduler's WFI branch, wake sources are noted, and
     * the battery registry has either a real PMIC gauge or the QEMU
     * mock. pmtest exercises the pure policy paths, a live display
     * blank/wake cycle and the PSCI conduit -- see docs/PHASE_10.md.
     */
    phase10_init(&plat);

    /*
     * Phase 11: networking. Registers loopback + virtio-net netifs,
     * arms the rx bridge, and spawns "nettest": DHCP via SLIRP,
     * loopback ping, full TCP loopback echo through the stack, then
     * the netcli process repeating the session through the EL0
     * socket syscalls (see docs/PHASE_11.md).
     */
    phase11_init(&plat);

    /*
     * Phase 12: telephony. modem_subsys_init selects the transport
     * (scripted mock on QEMU, real "modem" chardev on boards) and
     * modtest drives the milestone: status queries, outbound +
     * inbound calls through the state machine, SMS send/receive
     * with PDU round-trip into the /sms store, and the rmnet0 data
     * netif (see docs/PHASE_12.md).
     */
    phase12_init(&plat);

    /*
     * Phase 13: audio. Registers the null backend (QEMU fallback)
     * and the I2S scaffold, arms the phase-12 call-routing seam
     * (ACTIVE -> modem PCM hooks, INCOMING -> ringtone), then
     * modtest-style "audiotest" verifies mixer math, WAV parse+
     * play via the VFS, capture, call PCM both directions and the
     * ringer (see docs/PHASE_13.md).
     */
    phase13_init(&plat);

    /*
     * Phase 14: userspace foundation. crash records armed (RAM ring
     * + lazy /var/crash/records), the libc and its battery binary
     * exist as built-ins, and "init" runs as PID 1: it lays the /var
     * scaffolding, starts batteryd/udevd/timed and the interactive
     * shell, then reaps orphans and respawns critical daemons --
     * while "usertest" drives the libc, crash-record and respawn
     * batteries against it (see docs/PHASE_14.md).
     */
    phase14_init(&plat);

    /*
     * Phase 15: UI framework & phone apps. The compositor + app
     * binaries are built-ins init spawns; modemd (the telephony
     * broker) was started by phase12_init; this arms the "uitest15"
     * battery, which waits for the compositor and then drives the
     * milestone end to end: protocol round trip, SMS notification
     * banner, PIN unlock, launcher -> dialer, incoming call event
     * (see docs/PHASE_15.md).
     */
    phase15_init(&plat);

    /*
     * Phase 16: hardening, packaging & release polish. The W^X and
     * ASLR groundwork is live (proc.c/syscall.c/panic.c), the
     * software watchdog is armed, and "reltest" drives the release
     * battery: W^X probes, permission denials, kmsg persistence,
     * the A/B slot manager with rollback, and the perf metrics
     * (see docs/PHASE_16.md).
     */
    watchdog_init(&plat);
    phase16_init(&plat);

    kprintf("[perf] boot %llu ms\n",
            (unsigned long long)(time_uptime_ms() - boot_t0_ms));

    kprintf("%s\n", BANNER);

    sched_demo_start();                     /* ping vs pong forever-ish */
    task_create("housekeep", housekeeping_task, NULL, 50);

    /*
     * Phase 5: per-process address spaces. Both cpus take their
     * TCR.A1/CPACR config (boot cpu here, secondaries inside
     * secondary_start), then the demo task runs the built-in hello
     * ELF at EL0 and reports its exit code back.
     */
    proc_subsys_init();
    task_create("procdemo", process_demo_task, NULL, 10);

    /*
     * Boot context retires here: cpu0 (and each secondary, from its
     * own secondary_start) runs a per-cpu scheduler loop that owns
     * all dispatch decisions from this point on.
     */
    kprintf("boot: entering per-cpu schedulers\n");
    sched_run(0);
}
