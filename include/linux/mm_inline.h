/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LINUX_MM_INLINE_H
#define LINUX_MM_INLINE_H

#include <linux/huge_mm.h>
#include <linux/swap.h>

/**
 * page_is_file_cache - should the page be on a file LRU or anon LRU?
 * @page: the page to test
 *
 * Returns 1 if @page is page cache page backed by a regular filesystem,
 * or 0 if @page is anonymous, tmpfs or otherwise ram or swap backed.
 * Used by functions that manipulate the LRU lists, to sort a page
 * onto the right LRU list.
 *
 * We would like to get this info without a page flag, but the state
 * needs to survive until the page is last deleted from the LRU, which
 * could be as far down as __page_cache_release.
 */
static inline int page_is_file_cache(struct page *page)
{
	return !PageSwapBacked(page);
}

static __always_inline void __update_lru_size(struct lruvec *lruvec,
				enum lru_list lru, enum zone_type zid,
				int nr_pages)
{
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);

	__mod_node_page_state(pgdat, NR_LRU_BASE + lru, nr_pages);
	__mod_zone_page_state(&pgdat->node_zones[zid],
				NR_ZONE_LRU_BASE + lru, nr_pages);
}

static __always_inline void update_lru_size(struct lruvec *lruvec,
				enum lru_list lru, enum zone_type zid,
				int nr_pages)
{
	__update_lru_size(lruvec, lru, zid, nr_pages);
#ifdef CONFIG_MEMCG
	mem_cgroup_update_lru_size(lruvec, lru, zid, nr_pages);
#endif
}

#ifdef CONFIG_LRU_GEN
/*
 * PitchKernel MGLRU Phase 2: generation-list add/del.
 *
 * Design (Option A / Option 1, see earlier project discussion and the
 * comment on struct lru_gen_struct in mmzone.h): when CONFIG_LRU_GEN is
 * compiled in, every evictable page is added directly to its
 * generation's bucket from the moment it first joins a lruvec, never to
 * the classic lists[] array at all. This is what makes the classic and
 * MGLRU paths safe to coexist in the same binary -- a lruvec's pages
 * live on exactly one list scheme, decided once at compile/build-config
 * time, not migrated between schemes at runtime. The alternative (lazy
 * migration of already-classic-listed pages) was considered and
 * rejected: it would need to intercept or coordinate with every one of
 * this kernel's ~8 add_page_to_lru_list call sites across 5 files
 * (mlock.c, vmscan.c, compaction.c, memcontrol.c, swap.c) rather than
 * just the add/del choke point itself, for no benefit on this project
 * (CONFIG_LRU_GEN is a per-build, not per-runtime-toggle, choice here).
 *
 * New pages start in the current max_seq generation (freshly faulted
 * pages are, definitionally, just-accessed) with page_lru_gen()
 * uninitialized (0, "never tracked") until this first add sets it --
 * so unlike the aging path, there is no "old generation" to remove
 * from here; every add is necessarily this page's first.
 *
 * Locking: callers of add/del_page_to/from_lru_list already hold
 * lruvec_pgdat(lruvec)->lru_lock (that has always been true on this
 * kernel, classic path included -- see e.g. mm/vmscan.c's
 * isolate_lru_pages()/putback_inactive_pages() callers). This function
 * relies on that existing contract rather than taking the lock itself.
 */
static __always_inline bool mglru_add_page(struct page *page,
				struct lruvec *lruvec, enum lru_list lru)
{
	int type;
	unsigned long gen;

	if (lru == LRU_UNEVICTABLE)
		return false;

	type = is_file_lru(lru) ? LRU_GEN_FILE : LRU_GEN_ANON;
	gen = lruvec->lrugen.max_seq;

	/*
	 * Defensive fallback, not the primary mechanism: lruvec_init()
	 * (mm/mmzone.c) now seeds max_seq=1 directly, which is the real
	 * fix for the bug this guard was originally covering (see that
	 * function's comment for the full story -- a mismatch here
	 * silently made MGLRU reclaim never run at all). This check
	 * stays as a harmless safety net in case some future lruvec
	 * allocation path is added that doesn't go through lruvec_init,
	 * not because it's expected to fire in normal operation.
	 */
	if (gen == 0)
		gen = 1;

	set_page_lru_gen(page, gen);
	list_add(&page->lru, &lruvec->lrugen.lists[gen % MAX_NR_GENS][type]);
	lruvec->lrugen.nr_pages[gen % MAX_NR_GENS][type]++;

	return true;
}

static __always_inline bool mglru_del_page(struct page *page,
				struct lruvec *lruvec, enum lru_list lru)
{
	int type;
	unsigned long gen;

	if (lru == LRU_UNEVICTABLE)
		return false;

	gen = page_lru_gen(page);
	if (gen == 0) {
		/*
		 * Page was never added via mglru_add_page (e.g. it predates
		 * CONFIG_LRU_GEN being enabled on a running system, or a
		 * bug elsewhere left it untracked). Nothing to remove from
		 * lrugen lists. This should not happen in a build where
		 * LRU_GEN has been on since boot, but is not fatal if it
		 * does -- the page just was never counted in nr_pages[][]
		 * and isn't on any lrugen.lists[] bucket to unlink from.
		 */
		return false;
	}

	type = is_file_lru(lru) ? LRU_GEN_FILE : LRU_GEN_ANON;
	list_del(&page->lru);
	lruvec->lrugen.nr_pages[gen % MAX_NR_GENS][type]--;
	set_page_lru_gen(page, 0);

	return true;
}
#endif /* CONFIG_LRU_GEN */

static __always_inline void add_page_to_lru_list(struct page *page,
				struct lruvec *lruvec, enum lru_list lru)
{
	update_lru_size(lruvec, lru, page_zonenum(page), hpage_nr_pages(page));
#ifdef CONFIG_LRU_GEN
	if (mglru_add_page(page, lruvec, lru))
		return;
#endif
	list_add(&page->lru, &lruvec->lists[lru]);
}

static __always_inline void add_page_to_lru_list_tail(struct page *page,
				struct lruvec *lruvec, enum lru_list lru)
{
	update_lru_size(lruvec, lru, page_zonenum(page), hpage_nr_pages(page));
#ifdef CONFIG_LRU_GEN
	/*
	 * MGLRU has no head/tail distinction within a generation bucket
	 * the way the classic list does (that distinction encodes
	 * reclaim priority ordering that generations replace with the
	 * min_seq/max_seq scheme instead) -- mglru_add_page's plain
	 * list_add is the correct behavior for both callers.
	 */
	if (mglru_add_page(page, lruvec, lru))
		return;
#endif
	list_add_tail(&page->lru, &lruvec->lists[lru]);
}

static __always_inline void del_page_from_lru_list(struct page *page,
				struct lruvec *lruvec, enum lru_list lru)
{
#ifdef CONFIG_LRU_GEN
	if (mglru_del_page(page, lruvec, lru)) {
		update_lru_size(lruvec, lru, page_zonenum(page), -hpage_nr_pages(page));
		return;
	}
#endif
	list_del(&page->lru);
	update_lru_size(lruvec, lru, page_zonenum(page), -hpage_nr_pages(page));
}

/**
 * page_lru_base_type - which LRU list type should a page be on?
 * @page: the page to test
 *
 * Used for LRU list index arithmetic.
 *
 * Returns the base LRU type - file or anon - @page should be on.
 */
static inline enum lru_list page_lru_base_type(struct page *page)
{
	if (page_is_file_cache(page))
		return LRU_INACTIVE_FILE;
	return LRU_INACTIVE_ANON;
}

/**
 * page_off_lru - which LRU list was page on? clearing its lru flags.
 * @page: the page to test
 *
 * Returns the LRU list a page was on, as an index into the array of LRU
 * lists; and clears its Unevictable or Active flags, ready for freeing.
 */
static __always_inline enum lru_list page_off_lru(struct page *page)
{
	enum lru_list lru;

	if (PageUnevictable(page)) {
		__ClearPageUnevictable(page);
		lru = LRU_UNEVICTABLE;
	} else {
		lru = page_lru_base_type(page);
		if (PageActive(page)) {
			__ClearPageActive(page);
			lru += LRU_ACTIVE;
		}
	}
	return lru;
}

/**
 * page_lru - which LRU list should a page be on?
 * @page: the page to test
 *
 * Returns the LRU list a page should be on, as an index
 * into the array of LRU lists.
 */
static __always_inline enum lru_list page_lru(struct page *page)
{
	enum lru_list lru;

	if (PageUnevictable(page))
		lru = LRU_UNEVICTABLE;
	else {
		lru = page_lru_base_type(page);
		if (PageActive(page))
			lru += LRU_ACTIVE;
	}
	return lru;
}

#define lru_to_page(head) (list_entry((head)->prev, struct page, lru))

#endif
