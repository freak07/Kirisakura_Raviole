/*
 * Copyright (c) 2019, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __GOOGLE_AOC_ENUM_H__
#define __GOOGLE_AOC_ENUM_H__

#define AOC_FE (0 << 31)
#define AOC_BE (1 << 31)

#define AOC_RX (0 << 30)
#define AOC_TX (1 << 30)

#define AOC_BITMASK (0xffff0000)

#define AOC_ID_TO_INDEX(x) (x & 0xFFFF)

enum {
	PORT_I2S_0_RX = 0,
	PORT_I2S_0_TX,
	PORT_I2S_1_RX,
	PORT_I2S_1_TX,
	PORT_I2S_2_RX,
	PORT_I2S_2_TX,
	PORT_TDM_0_RX,
	PORT_TDM_0_TX,
	PORT_TDM_1_RX,
	PORT_TDM_1_TX,
	PORT_MAX,
};

enum {
	IDX_EP1 = 0,
	IDX_EP2,
	IDX_EP3,
	IDX_EP4,
	IDX_EP5,
	IDX_EP6,
	IDX_EP7,
	IDX_EP8,
	IDX_EP_MAX,
};

#define I2S_0_RX	(AOC_BE|AOC_RX|PORT_I2S_0_RX)
#define I2S_0_TX	(AOC_BE|AOC_TX|PORT_I2S_0_TX)

#define I2S_1_RX	(AOC_BE|AOC_RX|PORT_I2S_1_RX)
#define I2S_1_TX	(AOC_BE|AOC_TX|PORT_I2S_1_TX)

#define I2S_2_RX	(AOC_BE|AOC_RX|PORT_I2S_2_RX)
#define I2S_2_TX	(AOC_BE|AOC_TX|PORT_I2S_2_TX)

#define TDM_0_RX	(AOC_BE|AOC_RX|PORT_TDM_0_RX)
#define TDM_0_TX	(AOC_BE|AOC_TX|PORT_TDM_0_TX)

#define TDM_1_RX	(AOC_BE|AOC_RX|PORT_TDM_1_RX)
#define TDM_1_TX	(AOC_BE|AOC_TX|PORT_TDM_1_TX)

#define IDX_EP1_RX	(AOC_FE|AOC_RX|IDX_EP1)
#define IDX_EP2_RX	(AOC_FE|AOC_RX|IDX_EP2)
#define IDX_EP3_RX	(AOC_FE|AOC_RX|IDX_EP3)
#define IDX_EP4_RX	(AOC_FE|AOC_RX|IDX_EP4)
#define IDX_EP5_RX	(AOC_FE|AOC_RX|IDX_EP5)
#define IDX_EP6_RX	(AOC_FE|AOC_RX|IDX_EP6)
#define IDX_EP7_RX	(AOC_FE|AOC_RX|IDX_EP7)
#define IDX_EP8_RX	(AOC_FE|AOC_RX|IDX_EP8)

#define IDX_EP1_TX	(AOC_FE|AOC_TX|IDX_EP1)
#define IDX_EP2_TX	(AOC_FE|AOC_TX|IDX_EP2)
#define IDX_EP3_TX	(AOC_FE|AOC_TX|IDX_EP3)
#define IDX_EP4_TX	(AOC_FE|AOC_TX|IDX_EP4)
#define IDX_EP5_TX	(AOC_FE|AOC_TX|IDX_EP5)
#define IDX_EP6_TX	(AOC_FE|AOC_TX|IDX_EP6)
#define IDX_EP7_TX	(AOC_FE|AOC_TX|IDX_EP7)

#endif /* __GOOGLE_AOC_ENUM_H__ */
