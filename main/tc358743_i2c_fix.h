/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Drop-in notes for tc358743.c I2C path. The real fixes are applied in
 * tc358743.c via absolute CONFCTL programming + readback.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/** After init, CONFCTL must not be a mono-fill word. */
static inline bool tc358743_confctl_is_fill(uint16_t c)
{
    uint8_t lo = (uint8_t)(c & 0xffu);
    uint8_t hi = (uint8_t)(c >> 8);
    if (lo == 0 && hi == 0) {
        return false;
    }
    return lo == hi;
}
