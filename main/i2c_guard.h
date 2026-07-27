/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Stuck I2C fill patterns (CSI ribbon / dead bus):
 *   0x7F, 0x97, 0xFF — often on every register; multi-byte AVI fails.
 * 0x00 is a valid unplugged SYS_STATUS — never treat as garbage.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

static inline bool tc358743_byte_is_bus_garbage(uint8_t v)
{
    return v == 0x7fu || v == 0x97u || v == 0xffu;
}

static inline bool tc358743_word_is_bus_garbage(uint16_t v)
{
    uint8_t lo = (uint8_t)(v & 0xffu);
    uint8_t hi = (uint8_t)(v >> 8);
    return tc358743_byte_is_bus_garbage(lo) && lo == hi;
}

static inline bool tc358743_triple_looks_poisoned(uint8_t a, uint8_t b, uint8_t c)
{
    return tc358743_byte_is_bus_garbage(a) && a == b && b == c;
}
