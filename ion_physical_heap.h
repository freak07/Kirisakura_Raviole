/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020 Google LLC
 */

#include <linux/ion.h>

struct ion_heap *ion_physical_heap_create(phys_addr_t base, size_t size,
					  size_t align, const char *name);

typedef void(ion_physical_heap_allocate_callback)(struct ion_buffer *buffer,
						  void *ctx);
typedef void(ion_physical_heap_free_callback)(struct ion_buffer *buffer,
					      void *ctx);

void ion_physical_heap_set_allocate_callback(
	struct ion_heap *heap, ion_physical_heap_allocate_callback cb,
	void *ctx);

void ion_physical_heap_set_free_callback(struct ion_heap *heap,
					 ion_physical_heap_free_callback cb,
					 void *ctx);
