#ifndef UACCESS_H
#define UACCESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mm/types.h"

/*
 * Copy-in/copy-out safety (plan item 31).
 *
 * Every user pointer handed to the kernel is validated page by page
 * against the CALLING process's page tables before a single byte is
 * moved: the walk must reach a user-accessible (AP[2]=1) leaf with
 * the required permission, and the whole range must sit inside the
 * user VA window. Copies then run through the identity alias of the
 * translated frames, so a hostile or buggy pointer can never fault
 * into kernel context nor touch kernel-only mappings.
 *
 * Routines return 0 on success or -EFAULT; partial work is not
 * attempted (validation precedes all copying).
 */
long uacc_copy_in(void *kdst, paddr_t root, uint64_t usrc, size_t len);
long uacc_copy_out(paddr_t root, uint64_t udst, const void *ksrc,
                   size_t len);

/* validate + strlen-style scan, returns length excluding NUL or -EFAULT */
long uacc_strnlen_user(paddr_t root, uint64_t usrc, size_t max);

/* current process convenience wrappers (panic if not in a process) */
struct proc;
long uacc_copy_in_cur(void *kdst, const void *usrc, size_t len);
long uacc_copy_out_cur(void *udst, const void *ksrc, size_t len);
long uacc_strnlen_user_cur(const void *usrc, size_t max);

#endif /* UACCESS_H */
