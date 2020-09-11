/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020 Google LLC
 */

#include <linux/ion.h>

struct ion_heap *ion_physical_heap_create(phys_addr_t base, size_t size,
					  size_t align, const char *name,
					  struct device *dev);
