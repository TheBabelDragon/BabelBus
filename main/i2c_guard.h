/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Detect stuck-fill I2C patterns so we never treat them as HDMI lock.
 * Observed on this hardware: 0x7F, 0x97, 0xFF across every TC358743 reg
 * while multi-byte reads return ESP_ERR_INVALID_RESPONSE.
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
    return v == 0x7fffu || v == 0xffffu || v == 0x7f7fu || v == 0x9797u;
}

static inline bool tc358743_triple_looks_poisoned(uint8_t a, uint8_t b, uint8_t c)
{
    return tc358743_byte_is_bus_garbage(a) && a == b && b == c;
}
