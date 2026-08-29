/*
 * builtin_imgs.S - user programs embedded in the kernel image.
 *
 * The full ELF FILE bytes are included verbatim (.incbin), so the
 * phase-5 loader parses genuine ELF64 headers/program headers --
 * the same path a filesystem-backed exec will use later; only the
 * source of the bytes changes.
 *
 * userspace/hello is built by the Makefile before this file is
 * assembled (explicit prerequisite).
 */

.section .rodata.builtin, "a"
.align 3

.global builtin_hello_start
builtin_hello_start:
    .incbin "userspace/hello"
.global builtin_hello_end
builtin_hello_end:

.align 3
.global builtin_ipcdemo_start
builtin_ipcdemo_start:
    .incbin "userspace/ipcdemo"
.global builtin_ipcdemo_end
builtin_ipcdemo_end:

.align 3
.global builtin_evreader_start
builtin_evreader_start:
    .incbin "userspace/evreader"
.global builtin_evreader_end
builtin_evreader_end:

.align 3
.global builtin_netcli_start
builtin_netcli_start:
    .incbin "userspace/netcli"
.global builtin_netcli_end
builtin_netcli_end:

/* phase 14: libc-linked programs (crt0 + userspace/libc)           */

.align 3
.global builtin_init_start
builtin_init_start:
    .incbin "userspace/init"
.global builtin_init_end
builtin_init_end:

.align 3
.global builtin_sh_start
builtin_sh_start:
    .incbin "userspace/sh"
.global builtin_sh_end
builtin_sh_end:

.align 3
.global builtin_batteryd_start
builtin_batteryd_start:
    .incbin "userspace/batteryd"
.global builtin_batteryd_end
builtin_batteryd_end:

.align 3
.global builtin_udevd_start
builtin_udevd_start:
    .incbin "userspace/udevd"
.global builtin_udevd_end
builtin_udevd_end:

.align 3
.global builtin_timed_start
builtin_timed_start:
    .incbin "userspace/timed"
.global builtin_timed_end
builtin_timed_end:

.align 3
.global builtin_libctest_start
builtin_libctest_start:
    .incbin "userspace/libctest"
.global builtin_libctest_end
builtin_libctest_end:

.align 3
.global builtin_crasher_start
builtin_crasher_start:
    .incbin "userspace/crasher"
.global builtin_crasher_end
builtin_crasher_end:

/* phase 15: compositor + phone apps (UI toolkit linked)            */

.align 3
.global builtin_compositor_start
builtin_compositor_start:
    .incbin "userspace/compositor"
.global builtin_compositor_end
builtin_compositor_end:

.align 3
.global builtin_dialer_start
builtin_dialer_start:
    .incbin "userspace/dialer"
.global builtin_dialer_end
builtin_dialer_end:

.align 3
.global builtin_msgs_start
builtin_msgs_start:
    .incbin "userspace/msgs"
.global builtin_msgs_end
builtin_msgs_end:

.align 3
.global builtin_contacts_start
builtin_contacts_start:
    .incbin "userspace/contacts"
.global builtin_contacts_end
builtin_contacts_end:

.align 3
.global builtin_clock_start
builtin_clock_start:
    .incbin "userspace/clock"
.global builtin_clock_end
builtin_clock_end:

.align 3
.global builtin_calc_start
builtin_calc_start:
    .incbin "userspace/calc"
.global builtin_calc_end
builtin_calc_end:

.align 3
.global builtin_settings_start
builtin_settings_start:
    .incbin "userspace/settings"
.global builtin_settings_end
builtin_settings_end:

.align 3
.global builtin_uitest_start
builtin_uitest_start:
    .incbin "userspace/uitest"
.global builtin_uitest_end
builtin_uitest_end:
