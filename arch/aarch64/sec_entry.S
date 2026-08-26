/*
 * sec_entry.S - entry point for secondaries woken via PSCI_CPU_ON.
 *
 * PSCI hands us x0 = context id (we pass the cpu index) and runs us
 * with the MMU off on a fresh pc. All we must do before C code is
 * install this cpu's kernel stack; everything else (MMU, vectors,
 * GIC cpu interface, timer) happens in secondary_start().
 */

.text
.align  2

.global secondary_entry
secondary_entry:
    ldr     x1, =sec_stacks
    ldr     x2, [x1, x0, lsl #3]
    mov     sp, x2
    bl      secondary_start
.Lsec_park:
    wfe
    b       .Lsec_park
