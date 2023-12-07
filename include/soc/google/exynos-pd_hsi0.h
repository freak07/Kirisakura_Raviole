/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2023 Google LLC.
 */

#ifndef __EXYNOS_PD_HSI0_H
#define __EXYNOS_PD_HSI0_H

#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>

struct exynos_pd_hsi0_data {
	struct device	*dev;
	struct regulator *vdd_hsi;
	struct regulator *vdd30;
	struct regulator *vdd18;
	struct regulator *vdd085;
};

#endif  /* __EXYNOS_PD_HSI0_H */
