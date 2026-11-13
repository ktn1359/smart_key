
#ifndef CLOUD_COUNTER_H
#define CLOUD_COUNTER_H

#include <stdint.h>

struct __attribute__((packed)) CloudCounterSlot
{
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t generation;
    uint64_t reservedUntil;
    uint32_t crc32;
};

#endif