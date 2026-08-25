#ifndef MM_TYPES_H
#define MM_TYPES_H

#include <stdint.h>

typedef uint64_t paddr_t;           /* physical address  */
typedef uint64_t vaddr_t;           /* virtual address   */

#define PAGE_SHIFT  12
#define PAGE_SIZE   (1ul << PAGE_SHIFT)
#define PAGE_MASK   (~(PAGE_SIZE - 1))

#define KiB(x) ((uint64_t)(x) << 10)
#define MiB(x) ((uint64_t)(x) << 20)
#define GiB(x) ((uint64_t)(x) << 30)

#define ALIGN_UP(x, a)   (((x) + (a) - 1) & ~((a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
#define IS_ALIGNED(x, a) (((x) & ((a) - 1)) == 0)

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#endif /* MM_TYPES_H */
