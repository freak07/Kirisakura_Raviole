/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020 Google LLC
 */

#include "ion.h"

struct ion_heap *ion_physical_heap_create(struct ion_platform_heap *heap_data,
                                          struct device *dev);
