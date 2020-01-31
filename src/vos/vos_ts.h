/**
 * (C) Copyright 2020 Intel Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * GOVERNMENT LICENSE RIGHTS-OPEN SOURCE SOFTWARE
 * The Government's rights to use, modify, reproduce, release, perform, display,
 * or disclose this software are subject to the terms of the Apache License as
 * provided in Contract No. B609815.
 * Any reproduction of computer software, computer software documentation, or
 * portions thereof marked with this legend must also reproduce the markings.
 */
/**
 * Record timestamp table
 * vos/vos_ts.h
 *
 * Author: Jeff Olivier <jeffrey.v.olivier@intel.com>
 */

#ifndef __VOS_TS__
#define __VOS_TS__

#include <vos_tls.h>

struct vos_ts_entry {
	/** Uniquely identifies the record */
	void		*te_record_ptr;
	/** Next most recently used */
	uint32_t	 te_next_idx;
	/** Previous most recently used */
	uint32_t	 te_prev_idx;
	/** empty child index */
	uint32_t	 te_child_idx;
	/** Type of entry */
	uint32_t	 te_type;
	/** Low read time */
	daos_epoch_t	 te_ts_rl;
	/** High read time */
	daos_epoch_t	 te_ts_rh;
	/** Write time */
	daos_epoch_t	 te_ts_w;
	/** uuid's of transactions.  These can potentially be changed
	 *  to 16 bits and save some space here.  But for now, stick
	 *  with the full id.
	 */
	/** Low read tx */
	uuid_t		 te_tx_rl;
	/** high read tx */
	uuid_t		 te_tx_rh;
	/** write tx */
	uuid_t		 te_tx_w;
};

/** Table will be per xstream */
#define VOS_TS_BITS	23
#define VOS_TS_SIZE	(1 << VOS_TS_BITS)
#define VOS_TS_MASK	(VOS_TS_SIZE - 1)

/** Timestamp types */
#define D_FOREACH_TS_TYPE(ACTION)			\
	ACTION(VOS_TS_TYPE_CONT,	"container")	\
	ACTION(VOS_TS_TYPE_OBJ,		"object")	\
	ACTION(VOS_TS_TYPE_DKEY,	"dkey")		\
	ACTION(VOS_TS_TYPE_AKEY,	"akey")		\

#define DEFINE_TS_TYPE(type, desc)	type,

enum {
	D_FOREACH_TS_TYPE(DEFINE_TS_TYPE)
	/** Number of timestamp types */
	VOS_TS_TYPE_COUNT,
};

struct vos_ts_info {
	/** Least recently accessed index */
	uint32_t		ti_lru;
	/** Most recently accessed index */
	uint32_t		ti_mru;
	/** Type read low timestamp */
	daos_epoch_t		ti_ts_rl;
	/** Type read high timestamp */
	daos_epoch_t		ti_ts_rh;
	/** Type write timestamp */
	daos_epoch_t		ti_ts_w;
};

struct vos_ts_table {
	/** Timestamp table pointers for a type */
	struct vos_ts_info	tt_type_info[VOS_TS_TYPE_COUNT + 1];
	/** The table entries */
	struct vos_ts_entry	tt_table[VOS_TS_SIZE];
};

/** Internal API: Evict the LRU, move it to MRU, update global time stamps, and
 *  return the index
 */
void
vos_ts_evict_lru(struct vos_ts_table *ts_table, struct vos_ts_entry **entryp,
		 uint32_t *idx, uint32_t type);

/** Internal API: Evict selected entry from the cache, update global
 *  timestamps
 */
void
vos_ts_evict_entry(struct vos_ts_table *ts_table, struct vos_ts_entry *entry,
		   uint32_t idx, uint32_t type);

/** Internal API: Remove an entry from the lru list */
static inline void
remove_ts_entry(struct vos_ts_entry *entries, struct vos_ts_entry *entry)
{
	struct vos_ts_entry	*prev = &entries[entry->te_prev_idx];
	struct vos_ts_entry	*next = &entries[entry->te_next_idx];

	prev->te_next_idx = entry->te_next_idx;
	next->te_prev_idx = entry->te_prev_idx;
}

/** Internal API: Insert an entry in the lru list */
static inline void
insert_ts_entry(struct vos_ts_entry *entries, struct vos_ts_entry *entry,
		uint32_t idx, uint32_t prev_idx, uint32_t next_idx)
{
	struct vos_ts_entry	*prev;
	struct vos_ts_entry	*next;

	prev = &entries[prev_idx];
	next = &entries[next_idx];
	next->te_prev_idx = idx;
	prev->te_next_idx = idx;
	entry->te_prev_idx = prev_idx;
	entry->te_next_idx = next_idx;
}

/** Internal API: Make the entry the mru */
static inline void
move_lru(struct vos_ts_table *ts_table, struct vos_ts_entry *entry,
	 uint32_t idx, uint32_t type)
{
	struct vos_ts_info	*info = &ts_table->tt_type_info[type];

	if (info->ti_mru == idx) {
		/** Already the mru */
		return;
	}

	if (info->ti_lru == idx)
		info->ti_lru = entry->te_next_idx;

	/** First remove */
	remove_ts_entry(&ts_table->tt_table[0], entry);

	/** Now add */
	insert_ts_entry(&ts_table->tt_table[0], entry, idx, info->ti_mru,
			info->ti_lru);

	info->ti_mru = idx;
}

/** Lookup an entry in the thread local timestamp cache.  If the entry at
 *  specified index matches, it is returned.  Otherwise, the LRU is evicted,
 *  \p idx is updated, and the entry is initialized with global timestamps
 *  for the type of object.
 *
 * \param	idx[in,out]	Address of the entry index.
 * \param	type[in]	Type of the object
 *
 * \return the timestamp entry referenced by the \p idx
 */
static inline struct vos_ts_entry *
vos_ts_lookup(uint32_t *idx, uint32_t type)
{
	struct vos_ts_table	*ts_table = vos_ts_table_get();
	struct vos_ts_entry	*entry;
	uint32_t		 tindex = *idx & VOS_TS_MASK;

	entry = &ts_table->tt_table[tindex];
	if (entry->te_record_ptr == idx) {
		move_lru(ts_table, entry, tindex, type);
		return entry;
	}

	vos_ts_evict_lru(ts_table, &entry, idx, type);

	return entry;

}

/** For negative entries, each entry has an index to an entry representing
 *  child trees.
 *
 *  \param	entry	The entry with a missing subtree entry
 *
 *  \return	The entry for negative lookups on the subtree
 */
static inline struct vos_ts_entry *
vos_ts_lookup_child(struct vos_ts_entry *entry)
{
	return vos_ts_lookup(&entry->te_child_idx, entry->te_type + 1);
}

/** If an entry is still in the thread local timestamp cache, evict it and
 *  update global timestamps for the type.  Move the evicted entry to the LRU
 *  and mark it as already evicted.
 *
 * \param	idx[in]		Address of the entry index.
 * \param	type[in]	Type of the object
 */
static inline void
vos_ts_evict(uint32_t *idx, uint32_t type)
{
	struct vos_ts_table	*ts_table = vos_ts_table_get();
	struct vos_ts_entry	*entry;
	uint32_t		 tindex = *idx & VOS_TS_MASK;

	entry = &ts_table->tt_table[tindex];
	if (entry->te_record_ptr != idx)
		return;

	vos_ts_evict_entry(ts_table, entry, *idx, type);
}

/** Allocate thread local timestamp cache.   Set the initial global times
 *
 * \param	ts_table[in,out]	Thread local table pointer
 * \param	start_hlc[in]		Timestamp for initialization
 *
 * \return	-DER_NOMEM	Not enough memory available
 *		0		Success
 */
int
vos_ts_table_alloc(struct vos_ts_table **ts_table, daos_epoch_t start_hlc);


/** Free the thread local timestamp cache and reset pointer to NULL
 *
 * \param	ts_table[in,out]	Thread local table pointer
 */
void
vos_ts_table_free(struct vos_ts_table **ts_table);

/** Update the low read timestamp, if applicable
 *
 *  \param	entry[in,out]	Entry to update
 *  \param	epoch[in]	Update epoch
 */
static inline void
vos_ts_update_read_low(struct vos_ts_entry *entry, daos_epoch_t epoch)
{
	if (entry == NULL)
		return;
	entry->te_ts_rl = MAX(entry->te_ts_rl, epoch);
}

/** Update the high read timestamp, if applicable
 *
 *  \param	entry[in,out]	Entry to update
 *  \param	epoch[in]	Update epoch
 */
static inline void
vos_ts_update_read_high(struct vos_ts_entry *entry, daos_epoch_t epoch)
{
	if (entry == NULL)
		return;
	entry->te_ts_rh = MAX(entry->te_ts_rh, epoch);
}

/** Update the write timestamp, if applicable
 *
 *  \param	entry[in,out]	Entry to update
 *  \param	epoch[in]	Update epoch
 */
static inline void
vos_ts_update_write(struct vos_ts_entry *entry, daos_epoch_t epoch,
		    const uuid_t xid)
{
	if (entry == NULL)
		return;

	if (entry->te_ts_w >= epoch)
		return;

	entry->te_ts_w = epoch;
	uuid_copy(entry->te_tx_w, xid);
}

#endif /* __VOS_TS__ */
