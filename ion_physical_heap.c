// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2011,2020 Google LLC
 */
#include <linux/spinlock.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/genalloc.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include <asm/cacheflush.h>

#include "ion_physical_heap.h"
#include "ion_exynos.h"
#include "ion_debug.h"

#define ION_PHYSICAL_ALLOCATE_FAIL -1

struct ion_physical_heap {
	struct ion_heap heap;
	struct gen_pool *pool;
	struct device *parent;
	phys_addr_t base;
	size_t size;
	size_t alloc_align;
};

static int _clear_pages(struct page **pages, int num, pgprot_t pgprot)
{
	void *addr = vmap(pages, num, VM_MAP, pgprot);

	if (!addr)
		return -ENOMEM;
	memset(addr, 0, PAGE_SIZE * num);
	vunmap(addr);

	return 0;
}

static int _sglist_zero(struct scatterlist *sgl, unsigned int nents,
			pgprot_t pgprot)
{
	int p = 0;
	int ret = 0;
	struct sg_page_iter piter;
	struct page *pages[32];

	for_each_sg_page(sgl, &piter, nents, 0) {
		pages[p++] = sg_page_iter_page(&piter);
		if (p == ARRAY_SIZE(pages)) {
			ret = _clear_pages(pages, p, pgprot);
			if (ret)
				return ret;
			p = 0;
		}
	}
	if (p)
		ret = _clear_pages(pages, p, pgprot);

	return ret;
}

static int _buffer_zero(struct ion_buffer *buffer)
{
	struct sg_table *table = buffer->sg_table;
	pgprot_t pgprot = PAGE_KERNEL;

	return _sglist_zero(table->sgl, table->orig_nents, pgprot);
}

static int _pages_zero(struct page *page, size_t size, pgprot_t pgprot)
{
	struct scatterlist sg;

	sg_init_table(&sg, 1);
	sg_set_page(&sg, page, size, 0);
	return _sglist_zero(&sg, 1, pgprot);
}

static phys_addr_t ion_physical_allocate(struct ion_physical_heap *heap,
					 unsigned long size)
{
	unsigned long offset = gen_pool_alloc(heap->pool, size);

	if (!offset)
		return ION_PHYSICAL_ALLOCATE_FAIL;

	return offset;
}

static void ion_physical_free(struct ion_physical_heap *carveout_heap,
			      phys_addr_t addr, unsigned long size)
{
	if (addr == ION_PHYSICAL_ALLOCATE_FAIL)
		return;
	gen_pool_free(carveout_heap->pool, addr,
		      ALIGN(size, carveout_heap->alloc_align));
}

static int ion_physical_heap_allocate(struct ion_heap *heap,
				      struct ion_buffer *buffer,
				      unsigned long size, unsigned long flags)
{
	struct ion_physical_heap *carveout_heap =
		container_of(heap, struct ion_physical_heap, heap);
	struct sg_table *table;
	unsigned long aligned_size = ALIGN(size, carveout_heap->alloc_align);
	phys_addr_t paddr;
	int ret;

	table = kmalloc(sizeof(*table), GFP_KERNEL);
	if (!table)
		return -ENOMEM;
	ret = sg_alloc_table(table, 1, GFP_KERNEL);
	if (ret) {
		perrfn("failed to allocate scatterlist (err %d)", ret);
		goto err_free;
	}

	paddr = ion_physical_allocate(carveout_heap, aligned_size);
	if (paddr == ION_PHYSICAL_ALLOCATE_FAIL) {
		perrfn("failed to allocate from %s(id %d), size %lu",
		       heap->name, heap->id, size);
		ret = -ENOMEM;
		goto err_free_table;
	}

	sg_set_page(table->sgl, pfn_to_page(PFN_DOWN(paddr)), size, 0);
	buffer->sg_table = table;

	return 0;

err_free_table:
	sg_free_table(table);
err_free:
	kfree(table);
	return ret;
}

static void ion_physical_heap_free(struct ion_buffer *buffer)
{
	struct ion_physical_heap *carveout_heap =
		container_of(buffer->heap, struct ion_physical_heap, heap);
	struct device *dev = carveout_heap->parent;
	struct sg_table *table = buffer->sg_table;
	struct page *page = sg_page(table->sgl);
	phys_addr_t paddr = PFN_PHYS(page_to_pfn(page));
	unsigned long size = buffer->size;

	if (dev && dev->dma_ops && dev->dma_ops->unmap_sg)
		dev->dma_ops->unmap_sg(dev, buffer->sg_table->sgl,
				       buffer->sg_table->orig_nents, DMA_FROM_DEVICE,
				       0);
	_buffer_zero(buffer);
	ion_physical_free(carveout_heap, paddr, size);

	sg_free_table(table);
	kfree(table);
}

static int carveout_heap_map_user(struct ion_heap *heap,
				  struct ion_buffer *buffer,
				  struct vm_area_struct *vma)
{
	struct ion_physical_heap *carveout_heap =
		container_of(heap, struct ion_physical_heap, heap);
	struct device *dev = carveout_heap->parent;

	if (dev && dev->dma_ops && dev->dma_ops->map_sg)
		dev->dma_ops->map_sg(dev, buffer->sg_table->sgl,
				     buffer->sg_table->orig_nents, DMA_FROM_DEVICE,
				     0);

	return ion_heap_map_user(heap, buffer, vma);
}

static void carveout_heap_query(struct ion_heap *heap,
				struct ion_heap_data *data)
{
	struct ion_physical_heap *carveout_heap =
		container_of(heap, struct ion_physical_heap, heap);

	data->size = (__u32)carveout_heap->size;
}

static struct ion_heap_ops carveout_heap_ops = {
	.allocate = ion_physical_heap_allocate,
	.free = ion_physical_heap_free,
	.map_user = carveout_heap_map_user,
	.map_kernel = ion_heap_map_kernel,
	.unmap_kernel = ion_heap_unmap_kernel,
	.query_heap = carveout_heap_query,
};

static int ion_physical_heap_debug_show(struct ion_heap *heap,
					struct seq_file *s, void *unused)
{
	return 0;
}

struct ion_heap *ion_physical_heap_create(struct ion_platform_heap *heap_data,
					  struct device *dev)
{
	struct ion_physical_heap *carveout_heap;
	int ret;

	struct page *page;
	size_t size;

	page = pfn_to_page(PFN_DOWN(heap_data->base));
	size = heap_data->size;

	ret = _pages_zero(page, size, PAGE_KERNEL);
	if (ret)
		return ERR_PTR(ret);

	carveout_heap = kzalloc(sizeof(*carveout_heap), GFP_KERNEL);
	if (!carveout_heap)
		return ERR_PTR(-ENOMEM);

	carveout_heap->pool =
		gen_pool_create(get_order(heap_data->align) + PAGE_SHIFT, -1);
	if (!carveout_heap->pool) {
		kfree(carveout_heap);
		return ERR_PTR(-ENOMEM);
	}
	carveout_heap->base = heap_data->base;
	gen_pool_add(carveout_heap->pool, carveout_heap->base, heap_data->size,
		     -1);
	carveout_heap->heap.ops = &carveout_heap_ops;
	carveout_heap->heap.type = ION_HEAP_TYPE_CARVEOUT;
	carveout_heap->heap.name =
		kstrndup(heap_data->name, MAX_HEAP_NAME - 1, GFP_KERNEL);
	if (!carveout_heap->heap.name) {
		gen_pool_destroy(carveout_heap->pool);
		kfree(carveout_heap);
		return ERR_PTR(-ENOMEM);
	}
	carveout_heap->size = heap_data->size;
	carveout_heap->alloc_align = heap_data->align;
	carveout_heap->heap.debug_show = ion_physical_heap_debug_show;
	carveout_heap->parent = dev;

	return &carveout_heap->heap;
}
