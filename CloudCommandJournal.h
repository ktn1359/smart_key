#pragma once

struct __attribute__((packed)) CloudCommandJournal
{
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t generation;

    uint32_t appliedVersion[TOUCH_COUNT];
    uint32_t pendingVersion[TOUCH_COUNT];

    uint8_t pendingState[TOUCH_COUNT];
    uint8_t pendingFlags[TOUCH_COUNT];

    uint32_t crc32;
};