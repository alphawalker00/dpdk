/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2014 Intel Corporation
 */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/queue.h>

#include <rte_memory.h>
#include <rte_eal.h>
#include <rte_eal_memconfig.h>
#include <rte_launch.h>
#include <rte_per_lcore.h>
#include <rte_lcore.h>
#include <rte_common.h>
#include <rte_string_fns.h>
#include <rte_spinlock.h>
#include <rte_memcpy.h>
#include <rte_atomic.h>
#include <rte_fbarray.h>

#include "eal_internal_cfg.h"
#include "eal_memalloc.h"
#include "malloc_elem.h"
#include "malloc_heap.h"

static unsigned
check_hugepage_sz(unsigned flags, uint64_t hugepage_sz)
{
	unsigned check_flag = 0;

	if (!(flags & ~RTE_MEMZONE_SIZE_HINT_ONLY))
		return 1;

	switch (hugepage_sz) {
	case RTE_PGSIZE_256K:
		check_flag = RTE_MEMZONE_256KB;
		break;
	case RTE_PGSIZE_2M:
		check_flag = RTE_MEMZONE_2MB;
		break;
	case RTE_PGSIZE_16M:
		check_flag = RTE_MEMZONE_16MB;
		break;
	case RTE_PGSIZE_256M:
		check_flag = RTE_MEMZONE_256MB;
		break;
	case RTE_PGSIZE_512M:
		check_flag = RTE_MEMZONE_512MB;
		break;
	case RTE_PGSIZE_1G:
		check_flag = RTE_MEMZONE_1GB;
		break;
	case RTE_PGSIZE_4G:
		check_flag = RTE_MEMZONE_4GB;
		break;
	case RTE_PGSIZE_16G:
		check_flag = RTE_MEMZONE_16GB;
	}

	return check_flag & flags;
}

/*
 * Expand the heap with a memory area.
 */
static struct malloc_elem *
malloc_heap_add_memory(struct malloc_heap *heap, struct rte_memseg_list *msl,
		void *start, size_t len)
{
	struct malloc_elem *elem = start;

	malloc_elem_init(elem, heap, msl, len);

	malloc_elem_insert(elem);

	elem = malloc_elem_join_adjacent_free(elem);

	malloc_elem_free_list_insert(elem);

	heap->total_size += len;

	return elem;
}

static int
malloc_add_seg(const struct rte_memseg_list *msl,
		const struct rte_memseg *ms, size_t len, void *arg __rte_unused)
{
	struct rte_mem_config *mcfg = rte_eal_get_configuration()->mem_config;
	struct rte_memseg_list *found_msl;
	struct malloc_heap *heap;
	int msl_idx;

	heap = &mcfg->malloc_heaps[msl->socket_id];

	/* msl is const, so find it */
	msl_idx = msl - mcfg->memsegs;
	found_msl = &mcfg->memsegs[msl_idx];

	if (msl_idx < 0 || msl_idx >= RTE_MAX_MEMSEG_LISTS)
		return -1;

	malloc_heap_add_memory(heap, found_msl, ms->addr, len);

	RTE_LOG(DEBUG, EAL, "Added %zuM to heap on socket %i\n", len >> 20,
			msl->socket_id);
	return 0;
}

/*
 * Iterates through the freelist for a heap to find a free element
 * which can store data of the required size and with the requested alignment.
 * If size is 0, find the biggest available elem.
 * Returns null on failure, or pointer to element on success.
 */
static struct malloc_elem *
find_suitable_element(struct malloc_heap *heap, size_t size,
		unsigned int flags, size_t align, size_t bound, bool contig)
{
	size_t idx;
	struct malloc_elem *elem, *alt_elem = NULL;

	for (idx = malloc_elem_free_list_index(size);
			idx < RTE_HEAP_NUM_FREELISTS; idx++) {
		for (elem = LIST_FIRST(&heap->free_head[idx]);
				!!elem; elem = LIST_NEXT(elem, free_list)) {
			if (malloc_elem_can_hold(elem, size, align, bound,
					contig)) {
				if (check_hugepage_sz(flags,
						elem->msl->page_sz))
					return elem;
				if (alt_elem == NULL)
					alt_elem = elem;
			}
		}
	}

	if ((alt_elem != NULL) && (flags & RTE_MEMZONE_SIZE_HINT_ONLY))
		return alt_elem;

	return NULL;
}

/*
 * Main function to allocate a block of memory from the heap.
 * It locks the free list, scans it, and adds a new memseg if the
 * scan fails. Once the new memseg is added, it re-scans and should return
 * the new element after releasing the lock.
 */
static void *
heap_alloc(struct malloc_heap *heap, const char *type __rte_unused, size_t size,
		unsigned int flags, size_t align, size_t bound, bool contig)
{
	struct malloc_elem *elem;

	size = RTE_CACHE_LINE_ROUNDUP(size);
	align = RTE_CACHE_LINE_ROUNDUP(align);

	elem = find_suitable_element(heap, size, flags, align, bound, contig);
	if (elem != NULL) {
		elem = malloc_elem_alloc(elem, size, align, bound, contig);

		/* increase heap's count of allocated elements */
		heap->alloc_count++;
	}

	return elem == NULL ? NULL : (void *)(&elem[1]);
}

static int
try_expand_heap(struct malloc_heap *heap, size_t pg_sz, size_t elt_size,
		int socket, unsigned int flags, size_t align, size_t bound,
		bool contig)
{
	size_t map_len;
	struct rte_memseg_list *msl;
	struct rte_memseg **ms;
	struct malloc_elem *elem;
	int n_segs, allocd_pages;
	void *ret, *map_addr;

	align = RTE_MAX(align, MALLOC_ELEM_HEADER_LEN);
	map_len = RTE_ALIGN_CEIL(align + elt_size + MALLOC_ELEM_TRAILER_LEN,
			pg_sz);

	n_segs = map_len / pg_sz;

	/* we can't know in advance how many pages we'll need, so malloc */
	ms = malloc(sizeof(*ms) * n_segs);

	allocd_pages = eal_memalloc_alloc_seg_bulk(ms, n_segs, pg_sz,
			socket, true);

	/* make sure we've allocated our pages... */
	if (allocd_pages < 0)
		goto free_ms;

	map_addr = ms[0]->addr;
	msl = rte_mem_virt2memseg_list(map_addr);

	/* check if we wanted contiguous memory but didn't get it */
	if (contig && !eal_memalloc_is_contig(msl, map_addr, map_len)) {
		RTE_LOG(DEBUG, EAL, "%s(): couldn't allocate physically contiguous space\n",
				__func__);
		goto free_pages;
	}

	/* add newly minted memsegs to malloc heap */
	elem = malloc_heap_add_memory(heap, msl, map_addr, map_len);

	/* try once more, as now we have allocated new memory */
	ret = find_suitable_element(heap, elt_size, flags, align, bound,
			contig);

	if (ret == NULL)
		goto free_elem;

	RTE_LOG(DEBUG, EAL, "Heap on socket %d was expanded by %zdMB\n",
		socket, map_len >> 20ULL);

	free(ms);

	return 0;

free_elem:
	malloc_elem_free_list_remove(elem);
	malloc_elem_hide_region(elem, map_addr, map_len);
	heap->total_size -= map_len;

free_pages:
	eal_memalloc_free_seg_bulk(ms, n_segs);
free_ms:
	free(ms);

	return -1;
}

static int
compare_pagesz(const void *a, const void *b)
{
	const struct rte_memseg_list * const*mpa = a;
	const struct rte_memseg_list * const*mpb = b;
	const struct rte_memseg_list *msla = *mpa;
	const struct rte_memseg_list *mslb = *mpb;
	uint64_t pg_sz_a = msla->page_sz;
	uint64_t pg_sz_b = mslb->page_sz;

	if (pg_sz_a < pg_sz_b)
		return -1;
	if (pg_sz_a > pg_sz_b)
		return 1;
	return 0;
}

static int
alloc_mem_on_socket(size_t size, int socket, unsigned int flags, size_t align,
		size_t bound, bool contig)
{
	struct rte_mem_config *mcfg = rte_eal_get_configuration()->mem_config;
	struct malloc_heap *heap = &mcfg->malloc_heaps[socket];
	struct rte_memseg_list *requested_msls[RTE_MAX_MEMSEG_LISTS];
	struct rte_memseg_list *other_msls[RTE_MAX_MEMSEG_LISTS];
	uint64_t requested_pg_sz[RTE_MAX_MEMSEG_LISTS];
	uint64_t other_pg_sz[RTE_MAX_MEMSEG_LISTS];
	uint64_t prev_pg_sz;
	int i, n_other_msls, n_other_pg_sz, n_requested_msls, n_requested_pg_sz;
	bool size_hint = (flags & RTE_MEMZONE_SIZE_HINT_ONLY) > 0;
	unsigned int size_flags = flags & ~RTE_MEMZONE_SIZE_HINT_ONLY;
	void *ret;

	memset(requested_msls, 0, sizeof(requested_msls));
	memset(other_msls, 0, sizeof(other_msls));
	memset(requested_pg_sz, 0, sizeof(requested_pg_sz));
	memset(other_pg_sz, 0, sizeof(other_pg_sz));

	/*
	 * go through memseg list and take note of all the page sizes available,
	 * and if any of them were specifically requested by the user.
	 */
	n_requested_msls = 0;
	n_other_msls = 0;
	for (i = 0; i < RTE_MAX_MEMSEG_LISTS; i++) {
		struct rte_memseg_list *msl = &mcfg->memsegs[i];

		if (msl->socket_id != socket)
			continue;

		if (msl->base_va == NULL)
			continue;

		/* if pages of specific size were requested */
		if (size_flags != 0 && check_hugepage_sz(size_flags,
				msl->page_sz))
			requested_msls[n_requested_msls++] = msl;
		else if (size_flags == 0 || size_hint)
			other_msls[n_other_msls++] = msl;
	}

	/* sort the lists, smallest first */
	qsort(requested_msls, n_requested_msls, sizeof(requested_msls[0]),
			compare_pagesz);
	qsort(other_msls, n_other_msls, sizeof(other_msls[0]),
			compare_pagesz);

	/* now, extract page sizes we are supposed to try */
	prev_pg_sz = 0;
	n_requested_pg_sz = 0;
	for (i = 0; i < n_requested_msls; i++) {
		uint64_t pg_sz = requested_msls[i]->page_sz;

		if (prev_pg_sz != pg_sz) {
			requested_pg_sz[n_requested_pg_sz++] = pg_sz;
			prev_pg_sz = pg_sz;
		}
	}
	prev_pg_sz = 0;
	n_other_pg_sz = 0;
	for (i = 0; i < n_other_msls; i++) {
		uint64_t pg_sz = other_msls[i]->page_sz;

		if (prev_pg_sz != pg_sz) {
			other_pg_sz[n_other_pg_sz++] = pg_sz;
			prev_pg_sz = pg_sz;
		}
	}

	/* finally, try allocating memory of specified page sizes, starting from
	 * the smallest sizes
	 */
	for (i = 0; i < n_requested_pg_sz; i++) {
		uint64_t pg_sz = requested_pg_sz[i];

		/*
		 * do not pass the size hint here, as user expects other page
		 * sizes first, before resorting to best effort allocation.
		 */
		if (!try_expand_heap(heap, pg_sz, size, socket, size_flags,
				align, bound, contig))
			return 0;
	}
	if (n_other_pg_sz == 0)
		return -1;

	/* now, check if we can reserve anything with size hint */
	ret = find_suitable_element(heap, size, flags, align, bound, contig);
	if (ret != NULL)
		return 0;

	/*
	 * we still couldn't reserve memory, so try expanding heap with other
	 * page sizes, if there are any
	 */
	for (i = 0; i < n_other_pg_sz; i++) {
		uint64_t pg_sz = other_pg_sz[i];

		if (!try_expand_heap(heap, pg_sz, size, socket, flags,
				align, bound, contig))
			return 0;
	}
	return -1;
}

/* this will try lower page sizes first */
static void *
heap_alloc_on_socket(const char *type, size_t size, int socket,
		unsigned int flags, size_t align, size_t bound, bool contig)
{
	struct rte_mem_config *mcfg = rte_eal_get_configuration()->mem_config;
	struct malloc_heap *heap = &mcfg->malloc_heaps[socket];
	unsigned int size_flags = flags & ~RTE_MEMZONE_SIZE_HINT_ONLY;
	void *ret;

	rte_spinlock_lock(&(heap->lock));

	align = align == 0 ? 1 : align;

	/* for legacy mode, try once and with all flags */
	if (internal_config.legacy_mem) {
		ret = heap_alloc(heap, type, size, flags, align, bound, contig);
		goto alloc_unlock;
	}

	/*
	 * we do not pass the size hint here, because even if allocation fails,
	 * we may still be able to allocate memory from appropriate page sizes,
	 * we just need to request more memory first.
	 */
	ret = heap_alloc(heap, type, size, size_flags, align, bound, contig);
	if (ret != NULL)
		goto alloc_unlock;

	if (!alloc_mem_on_socket(size, socket, flags, align, bound, contig)) {
		ret = heap_alloc(heap, type, size, flags, align, bound, contig);

		/* this should have succeeded */
		if (ret == NULL)
			RTE_LOG(ERR, EAL, "Error allocating from heap\n");
	}
alloc_unlock:
	rte_spinlock_unlock(&(heap->lock));
	return ret;
}

void *
malloc_heap_alloc(const char *type, size_t size, int socket_arg,
		unsigned int flags, size_t align, size_t bound, bool contig)
{
	int socket, i, cur_socket;
	void *ret;

	/* return NULL if size is 0 or alignment is not power-of-2 */
	if (size == 0 || (align && !rte_is_power_of_2(align)))
		return NULL;

	if (!rte_eal_has_hugepages())
		socket_arg = SOCKET_ID_ANY;

	if (socket_arg == SOCKET_ID_ANY)
		socket = malloc_get_numa_socket();
	else
		socket = socket_arg;

	/* Check socket parameter */
	if (socket >= RTE_MAX_NUMA_NODES)
		return NULL;

	ret = heap_alloc_on_socket(type, size, socket, flags, align, bound,
			contig);
	if (ret != NULL || socket_arg != SOCKET_ID_ANY)
		return ret;

	/* try other heaps */
	for (i = 0; i < (int) rte_socket_count(); i++) {
		cur_socket = rte_socket_id_by_idx(i);
		if (cur_socket == socket)
			continue;
		ret = heap_alloc_on_socket(type, size, cur_socket, flags,
				align, bound, contig);
		if (ret != NULL)
			return ret;
	}
	return NULL;
}

int
malloc_heap_free(struct malloc_elem *elem)
{
	struct malloc_heap *heap;
	void *start, *aligned_start, *end, *aligned_end;
	size_t len, aligned_len, page_sz;
	struct rte_memseg_list *msl;
	int n_segs, seg_idx, max_seg_idx, ret;

	if (!malloc_elem_cookies_ok(elem) || elem->state != ELEM_BUSY)
		return -1;

	/* elem may be merged with previous element, so keep heap address */
	heap = elem->heap;
	msl = elem->msl;
	page_sz = (size_t)msl->page_sz;

	rte_spinlock_lock(&(heap->lock));

	/* mark element as free */
	elem->state = ELEM_FREE;

	elem = malloc_elem_free(elem);

	/* anything after this is a bonus */
	ret = 0;

	/* ...of which we can't avail if we are in legacy mode */
	if (internal_config.legacy_mem)
		goto free_unlock;

	/* check if we can free any memory back to the system */
	if (elem->size < page_sz)
		goto free_unlock;

	/* probably, but let's make sure, as we may not be using up full page */
	start = elem;
	len = elem->size;
	aligned_start = RTE_PTR_ALIGN_CEIL(start, page_sz);
	end = RTE_PTR_ADD(elem, len);
	aligned_end = RTE_PTR_ALIGN_FLOOR(end, page_sz);

	aligned_len = RTE_PTR_DIFF(aligned_end, aligned_start);

	/* can't free anything */
	if (aligned_len < page_sz)
		goto free_unlock;

	malloc_elem_free_list_remove(elem);

	malloc_elem_hide_region(elem, (void *) aligned_start, aligned_len);

	/* we don't really care if we fail to deallocate memory */
	n_segs = aligned_len / page_sz;
	seg_idx = RTE_PTR_DIFF(aligned_start, msl->base_va) / page_sz;
	max_seg_idx = seg_idx + n_segs;

	for (; seg_idx < max_seg_idx; seg_idx++) {
		struct rte_memseg *ms;

		ms = rte_fbarray_get(&msl->memseg_arr, seg_idx);
		eal_memalloc_free_seg(ms);
	}
	heap->total_size -= aligned_len;

	RTE_LOG(DEBUG, EAL, "Heap on socket %d was shrunk by %zdMB\n",
		msl->socket_id, aligned_len >> 20ULL);
free_unlock:
	rte_spinlock_unlock(&(heap->lock));
	return ret;
}

int
malloc_heap_resize(struct malloc_elem *elem, size_t size)
{
	int ret;

	if (!malloc_elem_cookies_ok(elem) || elem->state != ELEM_BUSY)
		return -1;

	rte_spinlock_lock(&(elem->heap->lock));

	ret = malloc_elem_resize(elem, size);

	rte_spinlock_unlock(&(elem->heap->lock));

	return ret;
}

/*
 * Function to retrieve data for heap on given socket
 */
int
malloc_heap_get_stats(struct malloc_heap *heap,
		struct rte_malloc_socket_stats *socket_stats)
{
	size_t idx;
	struct malloc_elem *elem;

	rte_spinlock_lock(&heap->lock);

	/* Initialise variables for heap */
	socket_stats->free_count = 0;
	socket_stats->heap_freesz_bytes = 0;
	socket_stats->greatest_free_size = 0;

	/* Iterate through free list */
	for (idx = 0; idx < RTE_HEAP_NUM_FREELISTS; idx++) {
		for (elem = LIST_FIRST(&heap->free_head[idx]);
			!!elem; elem = LIST_NEXT(elem, free_list))
		{
			socket_stats->free_count++;
			socket_stats->heap_freesz_bytes += elem->size;
			if (elem->size > socket_stats->greatest_free_size)
				socket_stats->greatest_free_size = elem->size;
		}
	}
	/* Get stats on overall heap and allocated memory on this heap */
	socket_stats->heap_totalsz_bytes = heap->total_size;
	socket_stats->heap_allocsz_bytes = (socket_stats->heap_totalsz_bytes -
			socket_stats->heap_freesz_bytes);
	socket_stats->alloc_count = heap->alloc_count;

	rte_spinlock_unlock(&heap->lock);
	return 0;
}

/*
 * Function to retrieve data for heap on given socket
 */
void
malloc_heap_dump(struct malloc_heap *heap, FILE *f)
{
	struct malloc_elem *elem;

	rte_spinlock_lock(&heap->lock);

	fprintf(f, "Heap size: 0x%zx\n", heap->total_size);
	fprintf(f, "Heap alloc count: %u\n", heap->alloc_count);

	elem = heap->first;
	while (elem) {
		malloc_elem_dump(elem, f);
		elem = elem->next;
	}

	rte_spinlock_unlock(&heap->lock);
}

int
rte_eal_malloc_heap_init(void)
{
	struct rte_mem_config *mcfg = rte_eal_get_configuration()->mem_config;

	if (mcfg == NULL)
		return -1;

	/* secondary process does not need to initialize anything */
	if (rte_eal_process_type() != RTE_PROC_PRIMARY)
		return 0;

	/* add all IOVA-contiguous areas to the heap */
	return rte_memseg_contig_walk(malloc_add_seg, NULL);
}
