/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Detect all-0x7F / failed I2C patterns so read-modify-write cannot poison
 * the TC358743 when the CSI ribbon / RESET path is bad.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

static inline bool tc358743_byte_is_bus_garbage(uint8_t v)
{
    return v == 0x7fu || v == 0xffu;
}

static inline bool tc358743_word_is_bus_garbage(uint16_t v)
{
    return v == 0x7fffu || v == 0xffffu || v == 0x7f7fu;
}

/** Three unrelated regs all 0x7F => bus dead or chip already poisoned. */
static inline bool tc358743_triple_looks_poisoned(uint8_t a, uint8_t b, uint8_t c)
{
    return tc358743_byte_is_bus_garbage(a) && tc358743_byte_is_bus_garbage(b) &&
           tc358743_byte_is_bus_garbage(c);
}
