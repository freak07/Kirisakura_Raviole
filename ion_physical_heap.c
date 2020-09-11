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

#include <uapi/linux/ion.h>

#include "ion_physical_heap.h"

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
		pr_err("%s: failed to allocate scatterlist (err %d)", __func__,
		       ret);
		goto err_free;
	}

	paddr = ion_physical_allocate(carveout_heap, aligned_size);
	if (paddr == ION_PHYSICAL_ALLOCATE_FAIL) {
		pr_err("%s: failed to allocate from %s(id %d), size %lu",
		       __func__, heap->name, heap->id, size);
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
				       buffer->sg_table->orig_nents,
				       DMA_FROM_DEVICE, 0);
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
				     buffer->sg_table->orig_nents,
				     DMA_FROM_DEVICE, 0);

	return ion_heap_map_user(heap, buffer, vma);
}

static long ion_physical_get_pool_size(struct ion_heap *heap)
{
	struct ion_physical_heap *physical_heap =
		container_of(heap, struct ion_physical_heap, heap);

	return physical_heap->size / PAGE_SIZE;
}

static struct ion_heap_ops carveout_heap_ops = {
	.allocate = ion_physical_heap_allocate,
	.free = ion_physical_heap_free,
	.get_pool_size = ion_physical_get_pool_size,
};

static int ion_physical_heap_debug_show(struct ion_heap *heap,
					struct seq_file *s, void *unused)
{
	return 0;
}

struct ion_heap *ion_physical_heap_create(phys_addr_t base, size_t size,
					  size_t align, const char *name,
					  struct device *dev)
{
	struct ion_physical_heap *carveout_heap;
	int ret;

	struct page *page;

	page = pfn_to_page(PFN_DOWN(base));

	ret = _pages_zero(page, size, PAGE_KERNEL);
	if (ret)
		return ERR_PTR(ret);

	carveout_heap = kzalloc(sizeof(*carveout_heap), GFP_KERNEL);
	if (!carveout_heap)
		return ERR_PTR(-ENOMEM);

	carveout_heap->pool =
		gen_pool_create(get_order(align) + PAGE_SHIFT, -1);
	if (!carveout_heap->pool) {
		kfree(carveout_heap);
		return ERR_PTR(-ENOMEM);
	}
	carveout_heap->base = base;
	gen_pool_add(carveout_heap->pool, carveout_heap->base, size, -1);
	carveout_heap->heap.ops = &carveout_heap_ops;
	carveout_heap->heap.name =
		kstrndup(name, MAX_HEAP_NAME - 1, GFP_KERNEL);
	if (!carveout_heap->heap.name) {
		gen_pool_destroy(carveout_heap->pool);
		kfree(carveout_heap);
		return ERR_PTR(-ENOMEM);
	}
	carveout_heap->heap.type = ION_HEAP_TYPE_CUSTOM;
	carveout_heap->heap.owner = THIS_MODULE;

	carveout_heap->size = size;
	carveout_heap->alloc_align = align;
	carveout_heap->parent = dev;

	return &carveout_heap->heap;
}
