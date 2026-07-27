/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#define P4KVM_MIPI_LDO_CHAN_ID 3
#define P4KVM_MIPI_LDO_VOLTAGE_MV 2500

/* Waveshare ESP32-P4 CSI camera SCCB/I2C (same as DFRobot / Espressif examples). */
#define P4KVM_TC358743_I2C_SDA_GPIO 7
#define P4KVM_TC358743_I2C_SCL_GPIO 8

#define P4KVM_TC358743_REFCLK_HZ 27000000u

#define P4KVM_CSI_H_RES 1920u
#define P4KVM_CSI_V_RES 1080u

/*
 * CSI lane bit rate must match TC358743 PLL.
 * 720x480 UYVY ~331 Mbps total ≈ 166 Mbps/lane → 400 Mbps is safe headroom.
 * Bump to 1200 when the source is true 1080p.
 */
#define P4KVM_MIPI_LANE_MBPS 400
