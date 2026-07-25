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

/* 1080p60 UYVY on 2 lanes needs ~1.2 Gbps/lane headroom (was 972 for 1080p30). */
#define P4KVM_MIPI_LANE_MBPS 1200
