/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#define P4KVM_MIPI_LDO_CHAN_ID 3
#define P4KVM_MIPI_LDO_VOLTAGE_MV 2500
#define P4KVM_TC358743_I2C_SDA_GPIO 7
#define P4KVM_TC358743_I2C_SCL_GPIO 8

#define P4KVM_TC358743_REFCLK_HZ 27000000u

#define P4KVM_CSI_H_RES 1920u
#define P4KVM_CSI_V_RES 1080u

/*
 * 1080p30 RGB888 ≈ 1.5 Gbps → 972 Mbps/lane x2 is enough.
 * 1080p60 RGB888 ≈ 3.0 Gbps → need ~1500 Mbps/lane x2.
 * EDID is now 1080p60-preferred; match the CSI link budget.
 */
#define P4KVM_MIPI_LANE_MBPS 1500
