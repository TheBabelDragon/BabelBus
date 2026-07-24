/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * I2S RX from TC358743-class HDMI→CSI adapters (GODIYMODULES / BliKVM style).
 * Typical flying-lead labels:
 *   SCK  → BCLK
 *   WFS  → WS / LRCK
 *   SD   → DIN (serial data from bridge)
 *   GND  → GND
 *   OSCK → leave open (optional MCLK)
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** Start I2S RX in slave mode (bridge is I2S master). No-op if disabled in Kconfig. */
esp_err_t i2s_audio_init(void);

/** True when the RX pipeline is running. */
bool i2s_audio_ready(void);

/**
 * Read up to @p max_bytes of PCM into @p dst.
 * Format: 16-bit stereo, little-endian, ~48 kHz (typical TC358743 I2S).
 * Returns bytes actually read (0 if none available / not ready).
 */
size_t i2s_audio_read(void *dst, size_t max_bytes, uint32_t timeout_ms);
