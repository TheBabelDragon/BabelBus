/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * I2C stuck-fill: *several unrelated registers* return the identical byte
 * (e.g. SYS=VI1=VOUT2=0x5F). A single SYS_STATUS value like 0xDF can be a
 * real bit pattern (TMDS+SYNC+AVMUTE etc.) — do NOT treat that alone as garbage.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/** Only obvious mono-fills sometimes seen when the bus is dead. */
static inline bool tc358743_byte_is_obvious_fill(uint8_t v)
{
    return v == 0x5fu || v == 0x7fu || v == 0x97u || v == 0xffu;
}

/**
 * True only when three unrelated regs match and are non-zero.
 * Call with SYS_STATUS, VI_STATUS1, VOUT_SET2 (or similar).
 */
static inline bool tc358743_triple_looks_poisoned(uint8_t a, uint8_t b, uint8_t c)
{
    if (a == 0 && b == 0 && c == 0) {
        return false;
    }
    return (a == b && b == c);
}

static inline bool tc358743_word_is_bus_garbage(uint16_t v)
{
    uint8_t lo = (uint8_t)(v & 0xffu);
    uint8_t hi = (uint8_t)(v >> 8);
    return lo == hi && tc358743_byte_is_obvious_fill(lo);
}
