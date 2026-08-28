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
