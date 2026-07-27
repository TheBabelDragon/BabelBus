/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * When the CSI I2C path is broken, every TC358743 register often returns the
 * *same* fill byte (seen: 0x5F, 0x7F, 0x97, 0xFF) and multi-byte AVI reads fail
 * with ESP_ERR_INVALID_RESPONSE. That is not HDMI status.
 *
 * Rule: if several unrelated regs all equal the same non-zero byte => garbage.
 * 0x00 alone can be valid (HDMI unplugged).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

static inline bool tc358743_byte_is_bus_garbage(uint8_t v)
{
    /* Known fills + any value where low nibble is 0xF and high bits look stuck */
    if (v == 0x00u) {
        return false; /* valid idle */
    }
    if (v == 0x5fu || v == 0x7fu || v == 0x97u || v == 0xffu) {
        return true;
    }
    /* 0bxxx11111 patterns often seen when bus floats partially */
    if ((v & 0x1fu) == 0x1fu && v != 0x1fu) {
        return true;
    }
    return false;
}

/** True when three unrelated reads are identical and non-zero (stuck fill). */
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
    return lo == hi && lo != 0;
}
