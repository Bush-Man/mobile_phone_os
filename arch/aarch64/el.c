/*
 * el.c - detect the boot exception level and fall back to EL1.
 */

#include <stdint.h>

#include "el.h"

#define SCR_NS  (1u << 0)       /* EL3: lower EL is Non-Secure          */
#define SCR_RW  (1u << 10)      /* EL3: lower EL execution state is 64  */

#define HCR_RW  (1u << 31)      /* EL2: EL1 execution state is 64       */

void el_drop_to_el1(void)
{
    switch (el_current()) {
    case 1:
        return;

    case 2:
        /* route counters/timers to EL1, zero virtual offset,
         * declare EL1 as AArch64, then drop */
        __asm__ volatile("msr cntvoff_el2, xzr");
        __asm__ volatile("msr cnthctl_el2, %0" :: "r"((3ull << 10)));
        __asm__ volatile("msr hcr_el2, %0" :: "r"(HCR_RW));
        el_enter_el1_from_el2((uint64_t)&&done);
        break;

    case 3:
        /* mark lower EL non-secure + AArch64, then drop */
        __asm__ volatile("msr scr_el3, %0" :: "r"((uint64_t)(SCR_NS | SCR_RW)));
        el_enter_el1_from_el3((uint64_t)&&done);
        break;

    default:
        for (;;)
            ;
    }

done:
    __asm__ volatile("isb");
}
