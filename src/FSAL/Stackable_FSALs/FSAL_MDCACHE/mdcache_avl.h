/*
 * Copyright (C) 2010, Linux Box Corporation
 * All Rights Reserved
 *
 * Contributor: Matt Benjamin
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 * -------------
 */

/**
 * @addtogroup FSAL_MDCACHE
 * @{
 */

/**
 * @file mdcache_avl.h
 * @author Matt Benjamin
 * @brief Definitions supporting AVL dirent representation
 *
 */

/**
 * @page AVLOverview Overview
 *
 * Definitions supporting AVL dirent representation.  The current
 * design represents dirents as a single AVL tree ordered by a
 * collision-resistent hash function (currently, Murmur3, which
 * appears to be several times faster than lookup3 on x86_64
 * architecture).  Quadratic probing is used to emulate perfect
 * hashing.  Worst case behavior is challenging to reproduce.
 * Heuristic methods are used to detect worst-case scenarios and fall
 * back to tractable (e.g., lookup) algorthims.
 *
 */

#ifndef MDCACHE_AVL_H
#define MDCACHE_AVL_H

#include "config.h"
#include "log.h"
#include "mdcache_int.h"
#include "avltree.h"

static inline int avl_dirent_hk_cmpf(const struct avltree_node *lhs,
				     const struct avltree_node *rhs)
{
	mdcache_dir_entry_t *lk, *rk;

	lk = avltree_container_of(lhs, mdcache_dir_entry_t, node_hk);
	rk = avltree_container_of(rhs, mdcache_dir_entry_t, node_hk);

	if (lk->hk.k < rk->hk.k)
		return -1;

	if (lk->hk.k == rk->hk.k)
		return 0;

	return 1;
}

void avl_dirent_set_deleted(mdcache_entry_t *entry, mdcache_dir_entry_t *v);
void mdcache_avl_init(mdcache_entry_t *entry);
int mdcache_avl_qp_insert(mdcache_entry_t *entry, mdcache_dir_entry_t **dirent);

#define MDCACHE_FLAG_NONE        0x0000
#define MDCACHE_FLAG_NEXT_ACTIVE 0x0001
#define MDCACHE_FLAG_ONLY_ACTIVE 0x0002

mdcache_dir_entry_t *mdcache_avl_lookup_k(mdcache_entry_t *entry, uint64_t k,
					  uint32_t flags);
mdcache_dir_entry_t *mdcache_avl_qp_lookup_s(mdcache_entry_t *entry,
					     const char *name, int maxj);
void mdcache_avl_clean_tree(struct avltree *tree);

#endif				/* MDCACHE_AVL_H */

/** @} */
