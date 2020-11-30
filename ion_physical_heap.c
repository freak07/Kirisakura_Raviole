// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2011,2020 Google LLC
 */
#include <linux/spinlock.h>
#include <linux/dma-map-ops.h>
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
	phys_addr_t base;
	size_t size;
	size_t alloc_align;

	ion_physical_heap_allocate_callback *allocate_cb;
	void *allocate_ctx;

	ion_physical_heap_free_callback *free_cb;
	void *free_ctx;
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

void ion_physical_heap_set_allocate_callback(
	struct ion_heap *heap, ion_physical_heap_allocate_callback cb,
	void *ctx)
{
	struct ion_physical_heap *physical_heap =
		container_of(heap, struct ion_physical_heap, heap);

	physical_heap->allocate_cb = cb;
	physical_heap->allocate_ctx = ctx;
}

void ion_physical_heap_set_free_callback(struct ion_heap *heap,
					 ion_physical_heap_free_callback cb,
					 void *ctx)
{
	struct ion_physical_heap *physical_heap =
		container_of(heap, struct ion_physical_heap, heap);

	physical_heap->free_cb = cb;
	physical_heap->free_ctx = ctx;
}

static phys_addr_t ion_physical_allocate(struct ion_physical_heap *heap,
					 unsigned long size)
{
	unsigned long offset = gen_pool_alloc(heap->pool, size);

	if (!offset)
		return ION_PHYSICAL_ALLOCATE_FAIL;

	return offset;
}

static void ion_physical_free(struct ion_physical_heap *physical_heap,
			      phys_addr_t addr, unsigned long size)
{
	if (addr == ION_PHYSICAL_ALLOCATE_FAIL)
		return;
	gen_pool_free(physical_heap->pool, addr,
		      ALIGN(size, physical_heap->alloc_align));
}

static int ion_physical_heap_allocate(struct ion_heap *heap,
				      struct ion_buffer *buffer,
				      unsigned long size, unsigned long flags)
{
	struct ion_physical_heap *physical_heap =
		container_of(heap, struct ion_physical_heap, heap);
	struct sg_table *table;
	unsigned long aligned_size = ALIGN(size, physical_heap->alloc_align);
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

	paddr = ion_physical_allocate(physical_heap, aligned_size);
	if (paddr == ION_PHYSICAL_ALLOCATE_FAIL) {
		pr_err("%s: failed to allocate from %s(id %d), size %lu",
		       __func__, heap->name, heap->id, size);
		ret = -ENOMEM;
		goto err_free_table;
	}

	sg_set_page(table->sgl, pfn_to_page(PFN_DOWN(paddr)), size, 0);
	buffer->sg_table = table;
	buffer->priv_virt = (void *)(uintptr_t)hash_long(paddr, 32);

	if (physical_heap->allocate_cb)
		physical_heap->allocate_cb(buffer, physical_heap->allocate_ctx);

	return 0;

err_free_table:
	sg_free_table(table);
err_free:
	kfree(table);
	return ret;
}

static void ion_physical_heap_free(struct ion_buffer *buffer)
{
	struct ion_physical_heap *physical_heap =
		container_of(buffer->heap, struct ion_physical_heap, heap);
	struct sg_table *table = buffer->sg_table;
	struct page *page = sg_page(table->sgl);
	phys_addr_t paddr = PFN_PHYS(page_to_pfn(page));
	unsigned long size = buffer->size;

	if (physical_heap->free_cb)
		physical_heap->free_cb(buffer, physical_heap->free_ctx);

	_buffer_zero(buffer);
	ion_physical_free(physical_heap, paddr, size);

	sg_free_table(table);
	kfree(table);
}

static long ion_physical_get_pool_size(struct ion_heap *heap)
{
	struct ion_physical_heap *physical_heap =
		container_of(heap, struct ion_physical_heap, heap);

	return physical_heap->size / PAGE_SIZE;
}

static struct ion_heap_ops physical_heap_ops = {
	.allocate = ion_physical_heap_allocate,
	.free = ion_physical_heap_free,
	.get_pool_size = ion_physical_get_pool_size,
};

struct ion_heap *ion_physical_heap_create(phys_addr_t base, size_t size,
					  size_t align, const char *name)
{
	struct ion_physical_heap *physical_heap;
	int ret;

	struct page *page;

	page = pfn_to_page(PFN_DOWN(base));

	ret = _pages_zero(page, size, PAGE_KERNEL);
	if (ret)
		return ERR_PTR(ret);

	physical_heap = kzalloc(sizeof(*physical_heap), GFP_KERNEL);
	if (!physical_heap)
		return ERR_PTR(-ENOMEM);

	physical_heap->pool =
		gen_pool_create(get_order(align) + PAGE_SHIFT, -1);
	if (!physical_heap->pool) {
		kfree(physical_heap);
		return ERR_PTR(-ENOMEM);
	}
	physical_heap->base = base;
	gen_pool_add(physical_heap->pool, physical_heap->base, size, -1);
	physical_heap->heap.ops = &physical_heap_ops;
	physical_heap->heap.name =
		kstrndup(name, MAX_HEAP_NAME - 1, GFP_KERNEL);
	if (!physical_heap->heap.name) {
		gen_pool_destroy(physical_heap->pool);
		kfree(physical_heap);
		return ERR_PTR(-ENOMEM);
	}
	physical_heap->heap.type = ION_HEAP_TYPE_CUSTOM;
	physical_heap->heap.owner = THIS_MODULE;

	physical_heap->size = size;
	physical_heap->alloc_align = align;

	return &physical_heap->heap;
}
