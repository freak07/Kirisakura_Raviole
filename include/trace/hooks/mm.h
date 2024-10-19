/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM mm

#define TRACE_INCLUDE_PATH trace/hooks

#if !defined(_TRACE_HOOK_MM_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_HOOK_MM_H

#include <linux/types.h>
#include <trace/hooks/vendor_hooks.h>

#ifdef __GENKSYMS__
#include <linux/mm.h>
#include <linux/oom.h>
#endif

struct oom_control;
struct cma;
struct acr_info;
struct compact_control;
struct slabinfo;
struct cgroup_subsys_state;
struct mem_cgroup;
struct cma;
struct acr_info;
struct vm_unmapped_area_info;

DECLARE_RESTRICTED_HOOK(android_rvh_set_skip_swapcache_flags,
			TP_PROTO(gfp_t *flags),
			TP_ARGS(flags), 1);
DECLARE_RESTRICTED_HOOK(android_rvh_set_gfp_zone_flags,
			TP_PROTO(gfp_t *flags),
			TP_ARGS(flags), 1);
DECLARE_RESTRICTED_HOOK(android_rvh_set_readahead_gfp_mask,
			TP_PROTO(gfp_t *flags),
			TP_ARGS(flags), 1);
DECLARE_HOOK(android_vh_cma_alloc_start,
	TP_PROTO(s64 *ts),
	TP_ARGS(ts));
DECLARE_HOOK(android_vh_cma_alloc_finish,
	TP_PROTO(struct cma *cma, struct page *page, unsigned long count,
		 unsigned int align, gfp_t gfp_mask, s64 ts),
	TP_ARGS(cma, page, count, align, gfp_mask, ts));
DECLARE_HOOK(android_vh_cma_alloc_busy_info,
	TP_PROTO(struct acr_info *info),
	TP_ARGS(info));
DECLARE_HOOK(android_vh_mm_compaction_begin,
	TP_PROTO(struct compact_control *cc, long *vendor_ret),
	TP_ARGS(cc, vendor_ret));
DECLARE_HOOK(android_vh_mm_compaction_end,
	TP_PROTO(struct compact_control *cc, long vendor_ret),
	TP_ARGS(cc, vendor_ret));
DECLARE_HOOK(android_vh_rmqueue,
	TP_PROTO(struct zone *preferred_zone, struct zone *zone,
		unsigned int order, gfp_t gfp_flags,
		unsigned int alloc_flags, int migratetype),
	TP_ARGS(preferred_zone, zone, order,
		gfp_flags, alloc_flags, migratetype));
DECLARE_HOOK(android_vh_pagevec_drain,
	TP_PROTO(struct page *page, bool *ret),
	TP_ARGS(page, ret));
DECLARE_HOOK(android_vh_pagecache_get_page,
	TP_PROTO(struct address_space *mapping, pgoff_t index,
		int fgp_flags, gfp_t gfp_mask, struct page *page),
	TP_ARGS(mapping, index, fgp_flags, gfp_mask, page));
DECLARE_HOOK(android_vh_filemap_fault_get_page,
	TP_PROTO(struct vm_fault *vmf, struct page **page, bool *retry),
	TP_ARGS(vmf, page, retry));
DECLARE_HOOK(android_vh_filemap_fault_cache_page,
	TP_PROTO(struct vm_fault *vmf, struct page *page),
	TP_ARGS(vmf, page));
DECLARE_HOOK(android_vh_meminfo_proc_show,
	TP_PROTO(struct seq_file *m),
	TP_ARGS(m));
DECLARE_HOOK(android_vh_exit_mm,
	TP_PROTO(struct mm_struct *mm),
	TP_ARGS(mm));
DECLARE_HOOK(android_vh_get_from_fragment_pool,
	TP_PROTO(struct mm_struct *mm, struct vm_unmapped_area_info *info,
		unsigned long *addr),
	TP_ARGS(mm, info, addr));
DECLARE_HOOK(android_vh_exclude_reserved_zone,
	TP_PROTO(struct mm_struct *mm, struct vm_unmapped_area_info *info),
	TP_ARGS(mm, info));
DECLARE_HOOK(android_vh_include_reserved_zone,
	TP_PROTO(struct mm_struct *mm, struct vm_unmapped_area_info *info,
		unsigned long *addr),
	TP_ARGS(mm, info, addr));
DECLARE_HOOK(android_vh_show_mem,
	TP_PROTO(unsigned int filter, nodemask_t *nodemask),
	TP_ARGS(filter, nodemask));
DECLARE_HOOK(android_vh_alloc_pages_slowpath,
	TP_PROTO(gfp_t gfp_mask, unsigned int order, unsigned long delta),
	TP_ARGS(gfp_mask, order, delta));
DECLARE_HOOK(android_vh_cma_alloc_adjust,
	TP_PROTO(struct zone *zone, bool *is_cma_alloc),
	TP_ARGS(zone, is_cma_alloc));
DECLARE_HOOK(android_vh_do_madvise_blk_plug,
	TP_PROTO(int behavior, bool *do_plug),
	TP_ARGS(behavior, do_plug));
DECLARE_HOOK(android_vh_shrink_inactive_list_blk_plug,
	TP_PROTO(bool *do_plug),
	TP_ARGS(do_plug));
DECLARE_HOOK(android_vh_shrink_lruvec_blk_plug,
	TP_PROTO(bool *do_plug),
	TP_ARGS(do_plug));
DECLARE_HOOK(android_vh_reclaim_pages_plug,
	TP_PROTO(bool *do_plug),
	TP_ARGS(do_plug));
DECLARE_HOOK(android_vh_zap_pte_range_tlb_start,
	TP_PROTO(void *ret),
	TP_ARGS(ret));
DECLARE_HOOK(android_vh_zap_pte_range_tlb_force_flush,
	TP_PROTO(struct page *page, bool *flush),
	TP_ARGS(page, flush));
DECLARE_HOOK(android_vh_zap_pte_range_tlb_end,
	TP_PROTO(void *ret),
	TP_ARGS(ret));
DECLARE_HOOK(android_vh_skip_lru_disable,
	TP_PROTO(bool *skip),
	TP_ARGS(skip));
DECLARE_HOOK(android_vh_print_slabinfo_header,
	TP_PROTO(struct seq_file *m),
	TP_ARGS(m));
DECLARE_HOOK(android_vh_cache_show,
	TP_PROTO(struct seq_file *m, struct slabinfo *sinfo, struct kmem_cache *s),
	TP_ARGS(m, sinfo, s));
struct dirty_throttle_control;
DECLARE_HOOK(android_vh_mm_dirty_limits,
	TP_PROTO(struct dirty_throttle_control *const gdtc, bool strictlimit,
		unsigned long dirty, unsigned long bg_thresh,
		unsigned long nr_reclaimable, unsigned long pages_dirtied),
	TP_ARGS(gdtc, strictlimit, dirty, bg_thresh,
		nr_reclaimable, pages_dirtied));
DECLARE_HOOK(android_vh_oom_check_panic,
	TP_PROTO(struct oom_control *oc, int *ret),
	TP_ARGS(oc, ret));
DECLARE_HOOK(android_vh_save_vmalloc_stack,
	TP_PROTO(unsigned long flags, struct vm_struct *vm),
	TP_ARGS(flags, vm));
DECLARE_HOOK(android_vh_show_stack_hash,
	TP_PROTO(struct seq_file *m, struct vm_struct *v),
	TP_ARGS(m, v));
DECLARE_HOOK(android_vh_save_track_hash,
	TP_PROTO(bool alloc, unsigned long p),
	TP_ARGS(alloc, p));
DECLARE_HOOK(android_vh_mem_cgroup_alloc,
	TP_PROTO(struct mem_cgroup *memcg),
	TP_ARGS(memcg));
DECLARE_HOOK(android_vh_mem_cgroup_free,
	TP_PROTO(struct mem_cgroup *memcg),
	TP_ARGS(memcg));
DECLARE_HOOK(android_vh_mem_cgroup_id_remove,
	TP_PROTO(struct mem_cgroup *memcg),
	TP_ARGS(memcg));
DECLARE_HOOK(android_vh_mem_cgroup_css_online,
	TP_PROTO(struct cgroup_subsys_state *css, struct mem_cgroup *memcg),
	TP_ARGS(css, memcg));
DECLARE_HOOK(android_vh_mem_cgroup_css_offline,
	TP_PROTO(struct cgroup_subsys_state *css, struct mem_cgroup *memcg),
	TP_ARGS(css, memcg));
DECLARE_HOOK(android_vh_alloc_pages_reclaim_bypass,
	TP_PROTO(gfp_t gfp_mask, int order, int alloc_flags,
	int migratetype, struct page **page),
	TP_ARGS(gfp_mask, order, alloc_flags, migratetype, page));
DECLARE_HOOK(android_vh_alloc_pages_failure_bypass,
	TP_PROTO(gfp_t gfp_mask, int order, int alloc_flags,
	int migratetype, struct page **page),
	TP_ARGS(gfp_mask, order, alloc_flags, migratetype, page));
DECLARE_HOOK(android_vh_ptep_clear_flush_young,
	TP_PROTO(bool *skip),
	TP_ARGS(skip));
DECLARE_HOOK(android_vh_do_swap_page_spf,
	TP_PROTO(bool *allow_swap_spf),
	TP_ARGS(allow_swap_spf));
/* macro versions of hooks are no longer required */

DECLARE_HOOK(android_vh_use_cma_first_check,
	TP_PROTO(bool *use_cma_first_check),
	TP_ARGS(use_cma_first_check));
#endif /* _TRACE_HOOK_MM_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
