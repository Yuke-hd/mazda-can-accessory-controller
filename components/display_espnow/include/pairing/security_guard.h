#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
/* phase: 0 = handshake, 1 = responder authenticated/key, 2 = initiator key response. */
static inline bool pairing_security_guard(const void *data, size_t size,
                                          size_t maximum, unsigned phase)
{
    if (!data || size < 3 || size > maximum) return false;
    const uint8_t *p = (const uint8_t *)data;
    if ((size_t)p[1] != size - 2) return false;
    return (phase == 0 && p[0] == 2) ||
           (phase == 1 && p[0] == 3 && p[1] == 32) ||
           (phase == 2 && p[0] == 4 && p[1] == 32);
}
static inline bool pairing_outer_security_guard(size_t size, size_t iv, size_t tag, size_t maximum)
{
    return size <= maximum && iv <= maximum && tag <= maximum - iv && size > iv + tag;
}
static inline uint32_t pairing_remaining_ticks(uint32_t start, uint32_t now, uint32_t budget, uint32_t cap)
{
    const uint32_t elapsed=now-start;
    if (elapsed>=budget) return 0;
    const uint32_t remaining=budget-elapsed;
    return remaining<cap ? remaining : cap;
}
