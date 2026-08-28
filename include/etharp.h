#ifndef ETHARP_H
#define ETHARP_H

#include <stdint.h>

void ip4_pack(uint8_t *out, uint32_t ip);
uint32_t ip4_unpack(const uint8_t *in);

#endif /* ETHARP_H */
