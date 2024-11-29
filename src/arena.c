/* ----------------------------------------------------------------------------
Copyright (c) 2019-2024, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

/* ----------------------------------------------------------------------------
"Arenas" are fixed area's of OS memory from which we can allocate
large blocks (>= MI_ARENA_MIN_BLOCK_SIZE, 4MiB).
In contrast to the rest of mimalloc, the arenas are shared between
threads and need to be accessed using atomic operations.

Arenas are also used to for huge OS page (1GiB) reservations or for reserving
OS memory upfront which can be improve performance or is sometimes needed
on embedded devices. We can also employ this with WASI or `sbrk` systems
to reserve large arenas upfront and be able to reuse the memory more effectively.

The arena allocation needs to be thread safe and we use an atomic bitmap to allocate.
-----------------------------------------------------------------------------*/

#include "mimalloc.h"
#include "mimalloc/internal.h"
#include "bitmap.h"


/* -----------------------------------------------------------
  Arena allocation
----------------------------------------------------------- */

#define MI_ARENA_BIN_COUNT      (MI_BIN_COUNT)


// A memory arena descriptor
typedef struct mi_arena_s {
  mi_memid_t          memid;                // memid of the memory area
  mi_arena_id_t       id;                   // arena id; 0 for non-specific

  size_t              block_count;          // size of the area in arena blocks (of `MI_ARENA_BLOCK_SIZE`)
  int                 numa_node;            // associated NUMA node
  bool                exclusive;            // only allow allocations if specifically for this arena
  bool                is_large;             // memory area consists of large- or huge OS pages (always committed)
  mi_lock_t           abandoned_visit_lock; // lock is only used when abandoned segments are being visited
  _Atomic(mi_msecs_t) purge_expire;         // expiration time when blocks should be decommitted from `blocks_decommit`.

  mi_bitmap_t         blocks_free;          // is the block free?
  mi_bitmap_t         blocks_committed;     // is the block committed? (i.e. accessible)
  mi_bitmap_t         blocks_purge;         // can the block be purged? (block in purge => block in free)
  mi_bitmap_t         blocks_dirty;         // is the block potentially non-zero?
  mi_bitmap_t         blocks_abandoned[MI_BIN_COUNT];  // abandoned pages per size bin (a set bit means the start of the page)
                                            // the full queue contains abandoned full pages
} mi_arena_t;

#define MI_MAX_ARENAS         (1024)        // Limited for now (and takes up .bss)

// The available arenas
static mi_decl_cache_align _Atomic(mi_arena_t*) mi_arenas[MI_MAX_ARENAS];
static mi_decl_cache_align _Atomic(size_t)      mi_arena_count; // = 0


/* -----------------------------------------------------------
  Arena id's
  id = arena_index + 1
----------------------------------------------------------- */

size_t mi_arena_id_index(mi_arena_id_t id) {
  return (size_t)(id <= 0 ? MI_MAX_ARENAS : id - 1);
}

static mi_arena_id_t mi_arena_id_create(size_t arena_index) {
  mi_assert_internal(arena_index < MI_MAX_ARENAS);
  return (int)arena_index + 1;
}

mi_arena_id_t _mi_arena_id_none(void) {
  return 0;
}

static bool mi_arena_id_is_suitable(mi_arena_id_t arena_id, bool arena_is_exclusive, mi_arena_id_t req_arena_id) {
  return ((!arena_is_exclusive && req_arena_id == _mi_arena_id_none()) ||
          (arena_id == req_arena_id));
}

bool _mi_arena_memid_is_suitable(mi_memid_t memid, mi_arena_id_t request_arena_id) {
  if (memid.memkind == MI_MEM_ARENA) {
    return mi_arena_id_is_suitable(memid.mem.arena.id, memid.mem.arena.is_exclusive, request_arena_id);
  }
  else {
    return mi_arena_id_is_suitable(_mi_arena_id_none(), false, request_arena_id);
  }
}

size_t mi_arena_get_count(void) {
  return mi_atomic_load_relaxed(&mi_arena_count);
}

mi_arena_t* mi_arena_from_index(size_t idx) {
  mi_assert_internal(idx < mi_arena_get_count());
  return mi_atomic_load_ptr_acquire(mi_arena_t, &mi_arenas[idx]);
}



/* -----------------------------------------------------------
  Util
----------------------------------------------------------- */


// Size of an arena
static size_t mi_arena_size(mi_arena_t* arena) {
  return mi_size_of_blocks(arena->block_count);
}

static size_t mi_arena_info_blocks(void) {
  const size_t os_page_size = _mi_os_page_size();
  const size_t info_size    = _mi_align_up(sizeof(mi_arena_t), os_page_size) + os_page_size; // + guard page
  const size_t info_blocks  = mi_block_count_of_size(info_size);
  return info_blocks;
}


// Start of the arena memory area
static uint8_t* mi_arena_start(mi_arena_t* arena) {
  return ((uint8_t*)arena);
}

// Start of a block
void* mi_arena_block_start(mi_arena_t* arena, size_t block_index) {
  return (mi_arena_start(arena) + mi_size_of_blocks(block_index));
}

// Arena area
void* mi_arena_area(mi_arena_id_t arena_id, size_t* size) {
  if (size != NULL) *size = 0;
  const size_t arena_index = mi_arena_id_index(arena_id);
  if (arena_index >= MI_MAX_ARENAS) return NULL;
  mi_arena_t* arena = mi_atomic_load_ptr_acquire(mi_arena_t, &mi_arenas[arena_index]);
  if (arena == NULL) return NULL;
  if (size != NULL) { *size = mi_size_of_blocks(arena->block_count); }
  return mi_arena_start(arena);
}


// Create an arena memid
static mi_memid_t mi_memid_create_arena(mi_arena_id_t id, bool is_exclusive, size_t block_index) {
  mi_memid_t memid = _mi_memid_create(MI_MEM_ARENA);
  memid.mem.arena.id = id;
  memid.mem.arena.block_index = block_index;
  memid.mem.arena.is_exclusive = is_exclusive;
  return memid;
}

// returns if the arena is exclusive
bool mi_arena_memid_indices(mi_memid_t memid, size_t* arena_index, size_t* block_index) {
  mi_assert_internal(memid.memkind == MI_MEM_ARENA);
  *arena_index = mi_arena_id_index(memid.mem.arena.id);
  *block_index = memid.mem.arena.block_index;
  return memid.mem.arena.is_exclusive;
}



/* -----------------------------------------------------------
  Arena Allocation
----------------------------------------------------------- */

static mi_decl_noinline void* mi_arena_try_alloc_at(mi_arena_t* arena, size_t arena_index, size_t needed_bcount,
  bool commit, size_t tseq, mi_memid_t* memid, mi_os_tld_t* tld)
{
  MI_UNUSED(arena_index);
  mi_assert_internal(mi_arena_id_index(arena->id) == arena_index);

  size_t block_index;
  if (!mi_bitmap_try_find_and_clearN(&arena->blocks_free, tseq, needed_bcount, &block_index)) return NULL;

  // claimed it!
  void* p = mi_arena_block_start(arena, block_index);
  *memid = mi_memid_create_arena(arena->id, arena->exclusive, block_index);
  memid->is_pinned = arena->memid.is_pinned;

  // set the dirty bits
  if (arena->memid.initially_zero) {
    memid->initially_zero = mi_bitmap_xsetN(MI_BIT_SET, &arena->blocks_dirty, block_index, needed_bcount, NULL);
  }

  // set commit state
  if (commit) {
    // commit requested, but the range may not be committed as a whole: ensure it is committed now
    memid->initially_committed = true;

    bool all_already_committed;
    mi_bitmap_xsetN(MI_BIT_SET, &arena->blocks_committed, block_index, needed_bcount, &all_already_committed);
    if (!all_already_committed) {
      bool commit_zero = false;
      if (!_mi_os_commit(p, mi_size_of_blocks(needed_bcount), &commit_zero, tld->stats)) {
        memid->initially_committed = false;
      }
      else {
        if (commit_zero) { memid->initially_zero = true; }
      }
    }
  }
  else {
    // no need to commit, but check if already fully committed
    memid->initially_committed = mi_bitmap_is_xsetN(MI_BIT_SET, &arena->blocks_committed, block_index, needed_bcount);
  }

  return p;
}

// allocate in a speficic arena
static void* mi_arena_try_alloc_at_id(mi_arena_id_t arena_id, bool match_numa_node, int numa_node,
  size_t size, size_t alignment,
  bool commit, bool allow_large, mi_arena_id_t req_arena_id, size_t tseq, mi_memid_t* memid, mi_os_tld_t* tld)
{
  mi_assert(alignment <= MI_ARENA_BLOCK_ALIGN);
  if (alignment > MI_ARENA_BLOCK_ALIGN) return NULL;

  const size_t bcount = mi_block_count_of_size(size);
  const size_t arena_index = mi_arena_id_index(arena_id);
  mi_assert_internal(arena_index < mi_atomic_load_relaxed(&mi_arena_count));
  mi_assert_internal(size <= mi_size_of_blocks(bcount));

  // Check arena suitability
  mi_arena_t* arena = mi_arena_from_index(arena_index);
  if (arena == NULL) return NULL;
  if (!allow_large && arena->is_large) return NULL;
  if (!mi_arena_id_is_suitable(arena->id, arena->exclusive, req_arena_id)) return NULL;
  if (req_arena_id == _mi_arena_id_none()) { // in not specific, check numa affinity
    const bool numa_suitable = (numa_node < 0 || arena->numa_node < 0 || arena->numa_node == numa_node);
    if (match_numa_node) { if (!numa_suitable) return NULL; }
    else { if (numa_suitable) return NULL; }
  }

  // try to allocate
  void* p = mi_arena_try_alloc_at(arena, arena_index, bcount, commit, tseq, memid, tld);
  mi_assert_internal(p == NULL || _mi_is_aligned(p, alignment));
  return p;
}


// allocate from an arena with fallback to the OS
static mi_decl_noinline void* mi_arena_try_alloc(int numa_node, size_t size, size_t alignment,
  bool commit, bool allow_large,
  mi_arena_id_t req_arena_id, size_t tseq, mi_memid_t* memid, mi_os_tld_t* tld)
{
  mi_assert(alignment <= MI_ARENA_BLOCK_ALIGN);
  if (alignment > MI_ARENA_BLOCK_ALIGN) return NULL;

  const size_t max_arena = mi_atomic_load_relaxed(&mi_arena_count);
  if mi_likely(max_arena == 0) return NULL;

  if (req_arena_id != _mi_arena_id_none()) {
    // try a specific arena if requested
    if (mi_arena_id_index(req_arena_id) < max_arena) {
      void* p = mi_arena_try_alloc_at_id(req_arena_id, true, numa_node, size, alignment, commit, allow_large, req_arena_id, tseq, memid, tld);
      if (p != NULL) return p;
    }
  }
  else {
    // try numa affine allocation
    for (size_t i = 0; i < max_arena; i++) {
      void* p = mi_arena_try_alloc_at_id(mi_arena_id_create(i), true, numa_node, size, alignment, commit, allow_large, req_arena_id, tseq, memid, tld);
      if (p != NULL) return p;
    }

    // try from another numa node instead..
    if (numa_node >= 0) {  // if numa_node was < 0 (no specific affinity requested), all arena's have been tried already
      for (size_t i = 0; i < max_arena; i++) {
        void* p = mi_arena_try_alloc_at_id(mi_arena_id_create(i), false /* only proceed if not numa local */, numa_node, size, alignment, commit, allow_large, req_arena_id, tseq, memid, tld);
        if (p != NULL) return p;
      }
    }
  }
  return NULL;
}

// try to reserve a fresh arena space
static bool mi_arena_reserve(size_t req_size, bool allow_large, mi_arena_id_t req_arena_id, mi_arena_id_t* arena_id)
{
  if (_mi_preloading()) return false;  // use OS only while pre loading
  if (req_arena_id != _mi_arena_id_none()) return false;

  const size_t arena_count = mi_atomic_load_acquire(&mi_arena_count);
  if (arena_count > (MI_MAX_ARENAS - 4)) return false;

  // calc reserve
  size_t arena_reserve = mi_option_get_size(mi_option_arena_reserve);
  if (arena_reserve == 0) return false;

  if (!_mi_os_has_virtual_reserve()) {
    arena_reserve = arena_reserve/4;  // be conservative if virtual reserve is not supported (for WASM for example)
  }
  arena_reserve = _mi_align_up(arena_reserve, MI_ARENA_BLOCK_SIZE);

  if (arena_count >= 8 && arena_count <= 128) {
    // scale up the arena sizes exponentially every 8 entries (128 entries get to 589TiB)
    const size_t multiplier = (size_t)1 << _mi_clamp(arena_count/8, 0, 16);
    size_t reserve = 0;
    if (!mi_mul_overflow(multiplier, arena_reserve, &reserve)) {
      arena_reserve = reserve;
    }
  }

  // check arena bounds
  const size_t min_reserve = mi_size_of_blocks(mi_arena_info_blocks() + 1);
  const size_t max_reserve = MI_BITMAP_MAX_BITS * MI_ARENA_BLOCK_SIZE;
  if (arena_reserve < min_reserve) {
    arena_reserve = min_reserve;
  }
  else if (arena_reserve > max_reserve) {
    arena_reserve = max_reserve;
  }

  if (arena_reserve < req_size) return false;  // should be able to at least handle the current allocation size

  // commit eagerly?
  bool arena_commit = false;
  if (mi_option_get(mi_option_arena_eager_commit) == 2) { arena_commit = _mi_os_has_overcommit(); }
  else if (mi_option_get(mi_option_arena_eager_commit) == 1) { arena_commit = true; }

  return (mi_reserve_os_memory_ex(arena_reserve, arena_commit, allow_large, false /* exclusive? */, arena_id) == 0);
}


void* _mi_arena_alloc_aligned(size_t size, size_t alignment, size_t align_offset, bool commit, bool allow_large,
  mi_arena_id_t req_arena_id, mi_memid_t* memid, mi_os_tld_t* tld)
{
  mi_assert_internal(memid != NULL && tld != NULL);
  mi_assert_internal(size > 0);
  size_t tseq = _mi_thread_seq_id();
  *memid = _mi_memid_none();

  const int numa_node = _mi_os_numa_node(tld); // current numa node

  // try to allocate in an arena if the alignment is small enough and the object is not too small (as for heap meta data)
  if (!mi_option_is_enabled(mi_option_disallow_arena_alloc) || req_arena_id != _mi_arena_id_none()) {  // is arena allocation allowed?
    if (size >= MI_ARENA_MIN_OBJ_SIZE && size <= MI_ARENA_MAX_OBJ_SIZE && alignment <= MI_ARENA_BLOCK_ALIGN && align_offset == 0) {
      void* p = mi_arena_try_alloc(numa_node, size, alignment, commit, allow_large, req_arena_id, tseq, memid, tld);
      if (p != NULL) return p;

      // otherwise, try to first eagerly reserve a new arena
      if (req_arena_id == _mi_arena_id_none()) {
        mi_arena_id_t arena_id = 0;
        if (mi_arena_reserve(size, allow_large, req_arena_id, &arena_id)) {
          // and try allocate in there
          mi_assert_internal(req_arena_id == _mi_arena_id_none());
          p = mi_arena_try_alloc_at_id(arena_id, true, numa_node, size, alignment, commit, allow_large, req_arena_id, tseq, memid, tld);
          if (p != NULL) return p;
        }
      }
    }
  }

  // if we cannot use OS allocation, return NULL
  if (mi_option_is_enabled(mi_option_disallow_os_alloc) || req_arena_id != _mi_arena_id_none()) {
    errno = ENOMEM;
    return NULL;
  }

  // finally, fall back to the OS
  if (align_offset > 0) {
    return _mi_os_alloc_aligned_at_offset(size, alignment, align_offset, commit, allow_large, memid, tld->stats);
  }
  else {
    return _mi_os_alloc_aligned(size, alignment, commit, allow_large, memid, tld->stats);
  }
}

void* _mi_arena_alloc(size_t size, bool commit, bool allow_large, mi_arena_id_t req_arena_id, mi_memid_t* memid, mi_os_tld_t* tld)
{
  return _mi_arena_alloc_aligned(size, MI_ARENA_BLOCK_SIZE, 0, commit, allow_large, req_arena_id, memid, tld);
}


/* -----------------------------------------------------------
  Arena free
----------------------------------------------------------- */
static void mi_arena_schedule_purge(mi_arena_t* arena, size_t block_idx, size_t blocks, mi_stats_t* stats);
static void mi_arenas_try_purge(bool force, bool visit_all, mi_stats_t* stats);

void _mi_arena_free(void* p, size_t size, size_t committed_size, mi_memid_t memid, mi_stats_t* stats) {
  mi_assert_internal(size > 0 && stats != NULL);
  mi_assert_internal(committed_size <= size);
  if (p==NULL) return;
  if (size==0) return;
  const bool all_committed = (committed_size == size);

  // need to set all memory to undefined as some parts may still be marked as no_access (like padding etc.)
  mi_track_mem_undefined(p, size);

  if (mi_memkind_is_os(memid.memkind)) {
    // was a direct OS allocation, pass through
    if (!all_committed && committed_size > 0) {
      // if partially committed, adjust the committed stats (as `_mi_os_free` will increase decommit by the full size)
      _mi_stat_decrease(&_mi_stats_main.committed, committed_size);
    }
    _mi_os_free(p, size, memid, stats);
  }
  else if (memid.memkind == MI_MEM_ARENA) {
    // allocated in an arena
    size_t arena_idx;
    size_t block_idx;
    mi_arena_memid_indices(memid, &arena_idx, &block_idx);
    mi_assert_internal(arena_idx < MI_MAX_ARENAS);
    mi_arena_t* arena = mi_atomic_load_ptr_acquire(mi_arena_t, &mi_arenas[arena_idx]);
    mi_assert_internal(arena != NULL);
    const size_t blocks = mi_block_count_of_size(size);

    // checks
    if (arena == NULL) {
      _mi_error_message(EINVAL, "trying to free from an invalid arena: %p, size %zu, memid: 0x%zx\n", p, size, memid);
      return;
    }
    mi_assert_internal(block_idx < arena->block_count);
    mi_assert_internal(block_idx > mi_arena_info_blocks());
    if (block_idx <= mi_arena_info_blocks() || block_idx > arena->block_count) {
      _mi_error_message(EINVAL, "trying to free from an invalid arena block: %p, size %zu, memid: 0x%zx\n", p, size, memid);
      return;
    }

    // potentially decommit
    if (arena->memid.is_pinned || arena->memid.initially_committed) {
      mi_assert_internal(all_committed);
    }
    else {
      if (!all_committed) {
        // mark the entire range as no longer committed (so we recommit the full range when re-using)
        mi_bitmap_xsetN(MI_BIT_CLEAR, &arena->blocks_committed, blocks, block_idx, NULL);
        mi_track_mem_noaccess(p, size);
        if (committed_size > 0) {
          // if partially committed, adjust the committed stats (is it will be recommitted when re-using)
          // in the delayed purge, we now need to not count a decommit if the range is not marked as committed.
          _mi_stat_decrease(&_mi_stats_main.committed, committed_size);
        }
        // note: if not all committed, it may be that the purge will reset/decommit the entire range
        // that contains already decommitted parts. Since purge consistently uses reset or decommit that
        // works (as we should never reset decommitted parts).
      }
      // (delay) purge the entire range
      mi_arena_schedule_purge(arena, block_idx, blocks, stats);
    }

    // and make it available to others again
    bool all_inuse = mi_bitmap_xsetN(MI_BIT_SET, &arena->blocks_free, block_idx, blocks, NULL);
    if (!all_inuse) {
      _mi_error_message(EAGAIN, "trying to free an already freed arena block: %p, size %zu\n", p, size);
      return;
    };
  }
  else {
    // arena was none, external, or static; nothing to do
    mi_assert_internal(memid.memkind < MI_MEM_OS);
  }

  // purge expired decommits
  mi_arenas_try_purge(false, false, stats);
}

// destroy owned arenas; this is unsafe and should only be done using `mi_option_destroy_on_exit`
// for dynamic libraries that are unloaded and need to release all their allocated memory.
static void mi_arenas_unsafe_destroy(void) {
  const size_t max_arena = mi_atomic_load_relaxed(&mi_arena_count);
  size_t new_max_arena = 0;
  for (size_t i = 0; i < max_arena; i++) {
    mi_arena_t* arena = mi_atomic_load_ptr_acquire(mi_arena_t, &mi_arenas[i]);
    if (arena != NULL) {
      mi_lock_done(&arena->abandoned_visit_lock);
      if (mi_memkind_is_os(arena->memid.memkind)) {
        mi_atomic_store_ptr_release(mi_arena_t, &mi_arenas[i], NULL);
        _mi_os_free(mi_arena_start(arena), mi_arena_size(arena), arena->memid, &_mi_stats_main);
      }
    }
  }

  // try to lower the max arena.
  size_t expected = max_arena;
  mi_atomic_cas_strong_acq_rel(&mi_arena_count, &expected, new_max_arena);
}

// Purge the arenas; if `force_purge` is true, amenable parts are purged even if not yet expired
void _mi_arenas_collect(bool force_purge, mi_stats_t* stats) {
  mi_arenas_try_purge(force_purge, force_purge /* visit all? */, stats);
}

// destroy owned arenas; this is unsafe and should only be done using `mi_option_destroy_on_exit`
// for dynamic libraries that are unloaded and need to release all their allocated memory.
void _mi_arena_unsafe_destroy_all(mi_stats_t* stats) {
  mi_arenas_unsafe_destroy();
  _mi_arenas_collect(true /* force purge */, stats);  // purge non-owned arenas
}

// Is a pointer inside any of our arenas?
bool _mi_arena_contains(const void* p) {
  const size_t max_arena = mi_atomic_load_relaxed(&mi_arena_count);
  for (size_t i = 0; i < max_arena; i++) {
    mi_arena_t* arena = mi_atomic_load_ptr_relaxed(mi_arena_t, &mi_arenas[i]);
    if (arena != NULL && mi_arena_start(arena) <= (const uint8_t*)p && mi_arena_start(arena) + mi_size_of_blocks(arena->block_count) > (const uint8_t*)p) {
      return true;
    }
  }
  return false;
}


/* -----------------------------------------------------------
  Add an arena.
----------------------------------------------------------- */

static bool mi_arena_add(mi_arena_t* arena, mi_arena_id_t* arena_id, mi_stats_t* stats) {
  mi_assert_internal(arena != NULL);
  mi_assert_internal(arena->block_count > 0);
  if (arena_id != NULL) { *arena_id = -1; }

  size_t i = mi_atomic_increment_acq_rel(&mi_arena_count);
  if (i >= MI_MAX_ARENAS) {
    mi_atomic_decrement_acq_rel(&mi_arena_count);
    return false;
  }
  _mi_stat_counter_increase(&stats->arena_count,1);
  arena->id = mi_arena_id_create(i);
  mi_atomic_store_ptr_release(mi_arena_t,&mi_arenas[i], arena);
  if (arena_id != NULL) { *arena_id = arena->id; }
  return true;
}

static bool mi_manage_os_memory_ex2(void* start, size_t size, bool is_large, int numa_node, bool exclusive, mi_memid_t memid, mi_arena_id_t* arena_id) mi_attr_noexcept
{
  mi_assert(!is_large || memid.initially_committed && memid.is_pinned);
  mi_assert(_mi_is_aligned(start,MI_ARENA_BLOCK_SIZE));
  mi_assert(start!=NULL);
  if (start==NULL) return false;
  if (!_mi_is_aligned(start,MI_ARENA_BLOCK_SIZE)) {
    // todo: use alignment in memid to align to blocksize first?
    _mi_warning_message("cannot use OS memory since it is not aligned to %zu KiB (address %p)", MI_ARENA_BLOCK_SIZE/MI_KiB, start);
    return false;
  }

  if (arena_id != NULL) { *arena_id = _mi_arena_id_none(); }

  const size_t info_blocks = mi_arena_info_blocks();
  const size_t bcount      = size / MI_ARENA_BLOCK_SIZE;  // divide down
  if (bcount < info_blocks+1) {
    _mi_warning_message("cannot use OS memory since it is not large enough (size %zu KiB, minimum required is %zu KiB)", size/MI_KiB, mi_size_of_blocks(info_blocks+1)/MI_KiB);
    return false;
  }
  if (bcount > MI_BITMAP_MAX_BITS) {
    // todo: allow larger areas (either by splitting it up in arena's or having larger arena's)
    _mi_warning_message("cannot use OS memory since it is too large (size %zu MiB, maximum is %zu MiB)", size/MI_MiB, mi_size_of_blocks(MI_BITMAP_MAX_BITS)/MI_MiB);
    return false;
  }
  mi_arena_t* arena = (mi_arena_t*)start;

  // commit & zero if needed
  bool is_zero = memid.initially_zero;
  if (!memid.initially_committed) {
    _mi_os_commit(arena, mi_size_of_blocks(info_blocks), &is_zero, &_mi_stats_main);
  }
  if (!is_zero) {
    _mi_memzero(arena, mi_size_of_blocks(info_blocks));
  }

  // init
  arena->id           = _mi_arena_id_none();
  arena->memid        = memid;
  arena->exclusive    = exclusive;
  arena->block_count  = bcount;
  arena->numa_node    = numa_node; // TODO: or get the current numa node if -1? (now it allows anyone to allocate on -1)
  arena->is_large     = is_large;
  arena->purge_expire = 0;
  mi_lock_init(&arena->abandoned_visit_lock);

  // init bitmaps
  mi_bitmap_init(&arena->blocks_free,true);
  mi_bitmap_init(&arena->blocks_committed,true);
  mi_bitmap_init(&arena->blocks_dirty,true);
  mi_bitmap_init(&arena->blocks_purge,true);
  for( int i = 0; i < MI_ARENA_BIN_COUNT; i++) {
    mi_bitmap_init(&arena->blocks_abandoned[i],true);
  }

  // reserve our meta info (and reserve blocks outside the memory area)
  mi_bitmap_unsafe_xsetN(MI_BIT_SET, &arena->blocks_free, info_blocks /* start */, arena->block_count - info_blocks);
  if (memid.initially_committed) {
    mi_bitmap_unsafe_xsetN(MI_BIT_SET, &arena->blocks_committed, 0, arena->block_count);
  }
  else {
    mi_bitmap_xsetN(MI_BIT_SET, &arena->blocks_committed, 0, info_blocks, NULL);
  }
  mi_bitmap_xsetN(MI_BIT_SET, &arena->blocks_dirty, 0, info_blocks, NULL);

  return mi_arena_add(arena, arena_id, &_mi_stats_main);
}


bool mi_manage_os_memory_ex(void* start, size_t size, bool is_committed, bool is_large, bool is_zero, int numa_node, bool exclusive, mi_arena_id_t* arena_id) mi_attr_noexcept {
  mi_memid_t memid = _mi_memid_create(MI_MEM_EXTERNAL);
  memid.initially_committed = is_committed;
  memid.initially_zero = is_zero;
  memid.is_pinned = is_large;
  return mi_manage_os_memory_ex2(start, size, is_large, numa_node, exclusive, memid, arena_id);
}

// Reserve a range of regular OS memory
int mi_reserve_os_memory_ex(size_t size, bool commit, bool allow_large, bool exclusive, mi_arena_id_t* arena_id) mi_attr_noexcept {
  if (arena_id != NULL) *arena_id = _mi_arena_id_none();
  size = _mi_align_up(size, MI_ARENA_BLOCK_SIZE); // at least one block
  mi_memid_t memid;
  void* start = _mi_os_alloc_aligned(size, MI_ARENA_BLOCK_ALIGN, commit, allow_large, &memid, &_mi_stats_main);
  if (start == NULL) return ENOMEM;
  const bool is_large = memid.is_pinned; // todo: use separate is_large field?
  if (!mi_manage_os_memory_ex2(start, size, is_large, -1 /* numa node */, exclusive, memid, arena_id)) {
    _mi_os_free_ex(start, size, commit, memid, &_mi_stats_main);
    _mi_verbose_message("failed to reserve %zu KiB memory\n", _mi_divide_up(size, 1024));
    return ENOMEM;
  }
  _mi_verbose_message("reserved %zu KiB memory%s\n", _mi_divide_up(size, 1024), is_large ? " (in large os pages)" : "");
  return 0;
}


// Manage a range of regular OS memory
bool mi_manage_os_memory(void* start, size_t size, bool is_committed, bool is_large, bool is_zero, int numa_node) mi_attr_noexcept {
  return mi_manage_os_memory_ex(start, size, is_committed, is_large, is_zero, numa_node, false /* exclusive? */, NULL);
}

// Reserve a range of regular OS memory
int mi_reserve_os_memory(size_t size, bool commit, bool allow_large) mi_attr_noexcept {
  return mi_reserve_os_memory_ex(size, commit, allow_large, false, NULL);
}


/* -----------------------------------------------------------
  Debugging
----------------------------------------------------------- */
static size_t mi_debug_show_bfield(mi_bfield_t field, char* buf) {
  size_t bit_set_count = 0;
  for (int bit = 0; bit < MI_BFIELD_BITS; bit++) {
    bool is_set = ((((mi_bfield_t)1 << bit) & field) != 0);
    if (is_set) bit_set_count++;
    buf[bit] = (is_set ? 'x' : '.');
  }
  return bit_set_count;
}

static size_t mi_debug_show_bitmap(const char* prefix, const char* header, size_t block_count, mi_bitmap_t* bitmap) {
  _mi_verbose_message("%s%s:\n", prefix, header);
  size_t bit_count = 0;
  size_t bit_set_count = 0;
  for (int i = 0; i < MI_BFIELD_BITS && bit_count < block_count; i++) {
    char buf[MI_BITMAP_CHUNK_BITS + 1];
    mi_bitmap_chunk_t* chunk = &bitmap->chunks[i];
    for (int j = 0; j < MI_BITMAP_CHUNK_FIELDS; j++) {
      if (bit_count < block_count) {
        bit_set_count += mi_debug_show_bfield(chunk->bfields[j], buf + j*MI_BFIELD_BITS);
      }
      else {
        _mi_memset(buf + j*MI_BFIELD_BITS, ' ', MI_BFIELD_BITS);
      }
      bit_count += MI_BFIELD_BITS;
    }
    buf[MI_BITMAP_CHUNK_BITS] = 0;
    _mi_verbose_message("%s  %s\n", prefix, buf);
  }
  _mi_verbose_message("%s  total ('x'): %zu\n", prefix, bit_set_count);
  return bit_set_count;
}

void mi_debug_show_arenas(bool show_inuse, bool show_abandoned, bool show_purge) mi_attr_noexcept {
  MI_UNUSED(show_abandoned);
  size_t max_arenas = mi_atomic_load_relaxed(&mi_arena_count);
  size_t free_total = 0;
  size_t block_total = 0;
  //size_t abandoned_total = 0;
  size_t purge_total = 0;
  for (size_t i = 0; i < max_arenas; i++) {
    mi_arena_t* arena = mi_atomic_load_ptr_relaxed(mi_arena_t, &mi_arenas[i]);
    if (arena == NULL) break;
    block_total += arena->block_count;
    _mi_verbose_message("arena %zu: %zu blocks%s\n", i, arena->block_count, (arena->memid.is_pinned ? ", pinned" : ""));
    if (show_inuse) {
      free_total += mi_debug_show_bitmap("  ", "free blocks", arena->block_count, &arena->blocks_free);
    }
    mi_debug_show_bitmap("  ", "committed blocks", arena->block_count, &arena->blocks_committed);
    // todo: abandoned blocks
    if (show_purge) {
      purge_total += mi_debug_show_bitmap("  ", "purgeable blocks", arena->block_count, &arena->blocks_purge);
    }
  }
  if (show_inuse)     _mi_verbose_message("total inuse blocks    : %zu\n", block_total - free_total);
  // if (show_abandoned) _mi_verbose_message("total abandoned blocks: %zu\n", abandoned_total);
  if (show_purge)     _mi_verbose_message("total purgeable blocks: %zu\n", purge_total);
}


/* -----------------------------------------------------------
  Reserve a huge page arena.
----------------------------------------------------------- */
// reserve at a specific numa node
int mi_reserve_huge_os_pages_at_ex(size_t pages, int numa_node, size_t timeout_msecs, bool exclusive, mi_arena_id_t* arena_id) mi_attr_noexcept {
  if (arena_id != NULL) *arena_id = -1;
  if (pages==0) return 0;
  if (numa_node < -1) numa_node = -1;
  if (numa_node >= 0) numa_node = numa_node % _mi_os_numa_node_count();
  size_t hsize = 0;
  size_t pages_reserved = 0;
  mi_memid_t memid;
  void* p = _mi_os_alloc_huge_os_pages(pages, numa_node, timeout_msecs, &pages_reserved, &hsize, &memid);
  if (p==NULL || pages_reserved==0) {
    _mi_warning_message("failed to reserve %zu GiB huge pages\n", pages);
    return ENOMEM;
  }
  _mi_verbose_message("numa node %i: reserved %zu GiB huge pages (of the %zu GiB requested)\n", numa_node, pages_reserved, pages);

  if (!mi_manage_os_memory_ex2(p, hsize, true, numa_node, exclusive, memid, arena_id)) {
    _mi_os_free(p, hsize, memid, &_mi_stats_main);
    return ENOMEM;
  }
  return 0;
}

int mi_reserve_huge_os_pages_at(size_t pages, int numa_node, size_t timeout_msecs) mi_attr_noexcept {
  return mi_reserve_huge_os_pages_at_ex(pages, numa_node, timeout_msecs, false, NULL);
}

// reserve huge pages evenly among the given number of numa nodes (or use the available ones as detected)
int mi_reserve_huge_os_pages_interleave(size_t pages, size_t numa_nodes, size_t timeout_msecs) mi_attr_noexcept {
  if (pages == 0) return 0;

  // pages per numa node
  size_t numa_count = (numa_nodes > 0 ? numa_nodes : _mi_os_numa_node_count());
  if (numa_count <= 0) numa_count = 1;
  const size_t pages_per = pages / numa_count;
  const size_t pages_mod = pages % numa_count;
  const size_t timeout_per = (timeout_msecs==0 ? 0 : (timeout_msecs / numa_count) + 50);

  // reserve evenly among numa nodes
  for (size_t numa_node = 0; numa_node < numa_count && pages > 0; numa_node++) {
    size_t node_pages = pages_per;  // can be 0
    if (numa_node < pages_mod) node_pages++;
    int err = mi_reserve_huge_os_pages_at(node_pages, (int)numa_node, timeout_per);
    if (err) return err;
    if (pages < node_pages) {
      pages = 0;
    }
    else {
      pages -= node_pages;
    }
  }

  return 0;
}

int mi_reserve_huge_os_pages(size_t pages, double max_secs, size_t* pages_reserved) mi_attr_noexcept {
  MI_UNUSED(max_secs);
  _mi_warning_message("mi_reserve_huge_os_pages is deprecated: use mi_reserve_huge_os_pages_interleave/at instead\n");
  if (pages_reserved != NULL) *pages_reserved = 0;
  int err = mi_reserve_huge_os_pages_interleave(pages, 0, (size_t)(max_secs * 1000.0));
  if (err==0 && pages_reserved!=NULL) *pages_reserved = pages;
  return err;
}



/* -----------------------------------------------------------
  Abandoned pages
----------------------------------------------------------- */

void mi_arena_page_abandon(mi_page_t* page) {
  mi_assert_internal(mi_page_is_abandoned(page));
  if (mi_page_is_full(page)) {}
}



/* -----------------------------------------------------------
  Arena purge
----------------------------------------------------------- */

static long mi_arena_purge_delay(void) {
  // <0 = no purging allowed, 0=immediate purging, >0=milli-second delay
  return (mi_option_get(mi_option_purge_delay) * mi_option_get(mi_option_arena_purge_mult));
}

// reset or decommit in an arena and update the committed/decommit bitmaps
// assumes we own the area (i.e. blocks_free is claimed by us)
static void mi_arena_purge(mi_arena_t* arena, size_t block_idx, size_t blocks, mi_stats_t* stats) {
  mi_assert_internal(!arena->memid.is_pinned);
  const size_t size = mi_size_of_blocks(blocks);
  void* const p = mi_arena_block_start(arena, block_idx);
  bool needs_recommit;
  if (mi_bitmap_is_xsetN(MI_BIT_SET, &arena->blocks_committed, block_idx, blocks)) {
    // all blocks are committed, we can purge freely
    needs_recommit = _mi_os_purge(p, size, stats);
  }
  else {
    // some blocks are not committed -- this can happen when a partially committed block is freed
    // in `_mi_arena_free` and it is conservatively marked as uncommitted but still scheduled for a purge
    // we need to ensure we do not try to reset (as that may be invalid for uncommitted memory),
    // and also undo the decommit stats (as it was already adjusted)
    mi_assert_internal(mi_option_is_enabled(mi_option_purge_decommits));
    needs_recommit = _mi_os_purge_ex(p, size, false /* allow reset? */, stats);
    if (needs_recommit) { _mi_stat_increase(&_mi_stats_main.committed, size); }
  }

  // clear the purged blocks
  mi_bitmap_xsetN(MI_BIT_CLEAR, &arena->blocks_purge, blocks, block_idx, NULL);

  // update committed bitmap
  if (needs_recommit) {
    mi_bitmap_xsetN(MI_BIT_CLEAR, &arena->blocks_committed, blocks, block_idx, NULL);
  }
}


// Schedule a purge. This is usually delayed to avoid repeated decommit/commit calls.
// Note: assumes we (still) own the area as we may purge immediately
static void mi_arena_schedule_purge(mi_arena_t* arena, size_t block_idx, size_t blocks, mi_stats_t* stats) {
  const long delay = mi_arena_purge_delay();
  if (delay < 0) return;  // is purging allowed at all?

  if (_mi_preloading() || delay == 0) {
    // decommit directly
    mi_arena_purge(arena, block_idx, blocks, stats);
  }
  else {
    // schedule decommit
    _mi_error_message(EFAULT, "purging not yet implemented\n");
  }
}


static void mi_arenas_try_purge(bool force, bool visit_all, mi_stats_t* stats) {
  if (_mi_preloading() || mi_arena_purge_delay() <= 0) return;  // nothing will be scheduled

  const size_t max_arena = mi_atomic_load_acquire(&mi_arena_count);
  if (max_arena == 0) return;

  _mi_error_message(EFAULT, "purging not yet implemented\n");
  MI_UNUSED(stats);
  MI_UNUSED(visit_all);
  MI_UNUSED(force);
}


#if 0

#define MI_IN_ARENA_C
#include "arena-abandon.c"
#undef MI_IN_ARENA_C

/* -----------------------------------------------------------
  Arena id's
  id = arena_index + 1
----------------------------------------------------------- */

size_t mi_arena_id_index(mi_arena_id_t id) {
  return (size_t)(id <= 0 ? MI_MAX_ARENAS : id - 1);
}

static mi_arena_id_t mi_arena_id_create(size_t arena_index) {
  mi_assert_internal(arena_index < MI_MAX_ARENAS);
  return (int)arena_index + 1;
}

mi_arena_id_t _mi_arena_id_none(void) {
  return 0;
}

static bool mi_arena_id_is_suitable(mi_arena_id_t arena_id, bool arena_is_exclusive, mi_arena_id_t req_arena_id) {
  return ((!arena_is_exclusive && req_arena_id == _mi_arena_id_none()) ||
          (arena_id == req_arena_id));
}

bool _mi_arena_memid_is_suitable(mi_memid_t memid, mi_arena_id_t request_arena_id) {
  if (memid.memkind == MI_MEM_ARENA) {
    return mi_arena_id_is_suitable(memid.mem.arena.id, memid.mem.arena.is_exclusive, request_arena_id);
  }
  else {
    return mi_arena_id_is_suitable(_mi_arena_id_none(), false, request_arena_id);
  }
}

size_t mi_arena_get_count(void) {
  return mi_atomic_load_relaxed(&mi_arena_count);
}

mi_arena_t* mi_arena_from_index(size_t idx) {
  mi_assert_internal(idx < mi_arena_get_count());
  return mi_atomic_load_ptr_acquire(mi_arena_t, &mi_arenas[idx]);
}


/* -----------------------------------------------------------
  Arena allocations get a (currently) 16-bit memory id where the
  lower 8 bits are the arena id, and the upper bits the block index.
----------------------------------------------------------- */

static size_t mi_block_count_of_size(size_t size) {
  return _mi_divide_up(size, MI_ARENA_BLOCK_SIZE);
}

static size_t mi_size_of_blocks(size_t bcount) {
  return (bcount * MI_ARENA_BLOCK_SIZE);
}

static size_t mi_arena_size(mi_arena_t* arena) {
  return mi_size_of_blocks(arena->block_count);
}

static mi_memid_t mi_memid_create_arena(mi_arena_id_t id, bool is_exclusive, mi_bitmap_index_t bitmap_index) {
  mi_memid_t memid = _mi_memid_create(MI_MEM_ARENA);
  memid.mem.arena.id = id;
  memid.mem.arena.block_index = bitmap_index;
  memid.mem.arena.is_exclusive = is_exclusive;
  return memid;
}

bool mi_arena_memid_indices(mi_memid_t memid, size_t* arena_index, mi_bitmap_index_t* bitmap_index) {
  mi_assert_internal(memid.memkind == MI_MEM_ARENA);
  *arena_index = mi_arena_id_index(memid.mem.arena.id);
  *bitmap_index = memid.mem.arena.block_index;
  return memid.mem.arena.is_exclusive;
}



/* -----------------------------------------------------------
  Special static area for mimalloc internal structures
  to avoid OS calls (for example, for the arena metadata (~= 256b))
----------------------------------------------------------- */

#define MI_ARENA_STATIC_MAX  ((MI_INTPTR_SIZE/2)*MI_KiB)  // 4 KiB on 64-bit

static mi_decl_cache_align uint8_t mi_arena_static[MI_ARENA_STATIC_MAX];  // must be cache aligned, see issue #895
static mi_decl_cache_align _Atomic(size_t) mi_arena_static_top;

static void* mi_arena_static_zalloc(size_t size, size_t alignment, mi_memid_t* memid) {
  *memid = _mi_memid_none();
  if (size == 0 || size > MI_ARENA_STATIC_MAX) return NULL;
  const size_t toplow = mi_atomic_load_relaxed(&mi_arena_static_top);
  if ((toplow + size) > MI_ARENA_STATIC_MAX) return NULL;

  // try to claim space
  if (alignment < MI_MAX_ALIGN_SIZE) { alignment = MI_MAX_ALIGN_SIZE; }
  const size_t oversize = size + alignment - 1;
  if (toplow + oversize > MI_ARENA_STATIC_MAX) return NULL;
  const size_t oldtop = mi_atomic_add_acq_rel(&mi_arena_static_top, oversize);
  size_t top = oldtop + oversize;
  if (top > MI_ARENA_STATIC_MAX) {
    // try to roll back, ok if this fails
    mi_atomic_cas_strong_acq_rel(&mi_arena_static_top, &top, oldtop);
    return NULL;
  }

  // success
  *memid = _mi_memid_create(MI_MEM_STATIC);
  memid->initially_zero = true;
  const size_t start = _mi_align_up(oldtop, alignment);
  uint8_t* const p = &mi_arena_static[start];
  _mi_memzero_aligned(p, size);
  return p;
}

void* _mi_arena_meta_zalloc(size_t size, mi_memid_t* memid) {
  *memid = _mi_memid_none();

  // try static
  void* p = mi_arena_static_zalloc(size, MI_MAX_ALIGN_SIZE, memid);
  if (p != NULL) return p;

  // or fall back to the OS
  p = _mi_os_alloc(size, memid, &_mi_stats_main);
  if (p == NULL) return NULL;

  // zero the OS memory if needed
  if (!memid->initially_zero) {
    _mi_memzero_aligned(p, size);
    memid->initially_zero = true;
  }
  return p;
}

void _mi_arena_meta_free(void* p, mi_memid_t memid, size_t size) {
  if (mi_memkind_is_os(memid.memkind)) {
    _mi_os_free(p, size, memid, &_mi_stats_main);
  }
  else {
    mi_assert(memid.memkind == MI_MEM_STATIC);
  }
}

void* mi_arena_block_start(mi_arena_t* arena, mi_bitmap_index_t bindex) {
  return (arena->start + mi_size_of_blocks(mi_bitmap_index_bit(bindex)));
}


/* -----------------------------------------------------------
  Thread safe allocation in an arena
----------------------------------------------------------- */

// claim the `blocks_inuse` bits
static bool mi_arena_try_claim(mi_arena_t* arena, size_t blocks, size_t block_idx, mi_stats_t* stats)
{
  size_t idx = 0; // mi_atomic_load_relaxed(&arena->search_idx);  // start from last search; ok to be relaxed as the exact start does not matter
  if (_mi_bitmap_try_find_from_claim_across(arena->blocks_inuse, arena->field_count, idx, blocks, bitmap_idx, stats)) {
    mi_atomic_store_relaxed(&arena->search_idx, mi_bitmap_index_field(*bitmap_idx));  // start search from found location next time around
    return true;
  };
  return false;
}


/* -----------------------------------------------------------
  Arena Allocation
----------------------------------------------------------- */

static mi_decl_noinline void* mi_arena_try_alloc_at(mi_arena_t* arena, size_t arena_index, size_t needed_bcount,
                                                    bool commit, mi_memid_t* memid, mi_os_tld_t* tld)
{
  MI_UNUSED(arena_index);
  mi_assert_internal(mi_arena_id_index(arena->id) == arena_index);

  mi_bitmap_index_t bitmap_index;
  if (!mi_arena_try_claim(arena, needed_bcount, &bitmap_index, tld->stats)) return NULL;

  // claimed it!
  void* p = mi_arena_block_start(arena, bitmap_index);
  *memid = mi_memid_create_arena(arena->id, arena->exclusive, bitmap_index);
  memid->is_pinned = arena->memid.is_pinned;

  // none of the claimed blocks should be scheduled for a decommit
  if (arena->blocks_purge != NULL) {
    // this is thread safe as a potential purge only decommits parts that are not yet claimed as used (in `blocks_inuse`).
    _mi_bitmap_unclaim_across(arena->blocks_purge, arena->field_count, needed_bcount, bitmap_index);
  }

  // set the dirty bits (todo: no need for an atomic op here?)
  if (arena->memid.initially_zero && arena->blocks_dirty != NULL) {
    memid->initially_zero = _mi_bitmap_claim_across(arena->blocks_dirty, arena->field_count, needed_bcount, bitmap_index, NULL);
  }

  // set commit state
  if (arena->blocks_committed == NULL) {
    // always committed
    memid->initially_committed = true;
  }
  else if (commit) {
    // commit requested, but the range may not be committed as a whole: ensure it is committed now
    memid->initially_committed = true;
    bool any_uncommitted;
    _mi_bitmap_claim_across(arena->blocks_committed, arena->field_count, needed_bcount, bitmap_index, &any_uncommitted);
    if (any_uncommitted) {
      bool commit_zero = false;
      if (!_mi_os_commit(p, mi_size_of_blocks(needed_bcount), &commit_zero, tld->stats)) {
        memid->initially_committed = false;
      }
      else {
        if (commit_zero) { memid->initially_zero = true; }
      }
    }
  }
  else {
    // no need to commit, but check if already fully committed
    memid->initially_committed = _mi_bitmap_is_claimed_across(arena->blocks_committed, arena->field_count, needed_bcount, bitmap_index);
  }

  return p;
}

// allocate in a speficic arena
static void* mi_arena_try_alloc_at_id(mi_arena_id_t arena_id, bool match_numa_node, int numa_node, size_t size, size_t alignment,
                                       bool commit, bool allow_large, mi_arena_id_t req_arena_id, mi_memid_t* memid, mi_os_tld_t* tld )
{
  MI_UNUSED_RELEASE(alignment);
  mi_assert(alignment <= MI_SEGMENT_ALIGN);
  const size_t bcount = mi_block_count_of_size(size);
  const size_t arena_index = mi_arena_id_index(arena_id);
  mi_assert_internal(arena_index < mi_atomic_load_relaxed(&mi_arena_count));
  mi_assert_internal(size <= mi_size_of_blocks(bcount));

  // Check arena suitability
  mi_arena_t* arena = mi_arena_from_index(arena_index);
  if (arena == NULL) return NULL;
  if (!allow_large && arena->is_large) return NULL;
  if (!mi_arena_id_is_suitable(arena->id, arena->exclusive, req_arena_id)) return NULL;
  if (req_arena_id == _mi_arena_id_none()) { // in not specific, check numa affinity
    const bool numa_suitable = (numa_node < 0 || arena->numa_node < 0 || arena->numa_node == numa_node);
    if (match_numa_node) { if (!numa_suitable) return NULL; }
                    else { if (numa_suitable) return NULL; }
  }

  // try to allocate
  void* p = mi_arena_try_alloc_at(arena, arena_index, bcount, commit, memid, tld);
  mi_assert_internal(p == NULL || _mi_is_aligned(p, alignment));
  return p;
}


// allocate from an arena with fallback to the OS
static mi_decl_noinline void* mi_arena_try_alloc(int numa_node, size_t size, size_t alignment,
                                                  bool commit, bool allow_large,
                                                  mi_arena_id_t req_arena_id, mi_memid_t* memid, mi_os_tld_t* tld )
{
  MI_UNUSED(alignment);
  mi_assert_internal(alignment <= MI_SEGMENT_ALIGN);
  const size_t max_arena = mi_atomic_load_relaxed(&mi_arena_count);
  if mi_likely(max_arena == 0) return NULL;

  if (req_arena_id != _mi_arena_id_none()) {
    // try a specific arena if requested
    if (mi_arena_id_index(req_arena_id) < max_arena) {
      void* p = mi_arena_try_alloc_at_id(req_arena_id, true, numa_node, size, alignment, commit, allow_large, req_arena_id, memid, tld);
      if (p != NULL) return p;
    }
  }
  else {
    // try numa affine allocation
    for (size_t i = 0; i < max_arena; i++) {
      void* p = mi_arena_try_alloc_at_id(mi_arena_id_create(i), true, numa_node, size, alignment, commit, allow_large, req_arena_id, memid, tld);
      if (p != NULL) return p;
    }

    // try from another numa node instead..
    if (numa_node >= 0) {  // if numa_node was < 0 (no specific affinity requested), all arena's have been tried already
      for (size_t i = 0; i < max_arena; i++) {
        void* p = mi_arena_try_alloc_at_id(mi_arena_id_create(i), false /* only proceed if not numa local */, numa_node, size, alignment, commit, allow_large, req_arena_id, memid, tld);
        if (p != NULL) return p;
      }
    }
  }
  return NULL;
}

// try to reserve a fresh arena space
static bool mi_arena_reserve(size_t req_size, bool allow_large, mi_arena_id_t req_arena_id, mi_arena_id_t *arena_id)
{
  if (_mi_preloading()) return false;  // use OS only while pre loading
  if (req_arena_id != _mi_arena_id_none()) return false;

  const size_t arena_count = mi_atomic_load_acquire(&mi_arena_count);
  if (arena_count > (MI_MAX_ARENAS - 4)) return false;

  size_t arena_reserve = mi_option_get_size(mi_option_arena_reserve);
  if (arena_reserve == 0) return false;

  if (!_mi_os_has_virtual_reserve()) {
    arena_reserve = arena_reserve/4;  // be conservative if virtual reserve is not supported (for WASM for example)
  }
  arena_reserve = _mi_align_up(arena_reserve, MI_ARENA_BLOCK_SIZE);
  arena_reserve = _mi_align_up(arena_reserve, MI_SEGMENT_SIZE);
  if (arena_count >= 8 && arena_count <= 128) {
    // scale up the arena sizes exponentially every 8 entries (128 entries get to 589TiB)
    const size_t multiplier = (size_t)1 << _mi_clamp(arena_count/8, 0, 16 );
    size_t reserve = 0;
    if (!mi_mul_overflow(multiplier, arena_reserve, &reserve)) {
      arena_reserve = reserve;
    }
  }
  if (arena_reserve < req_size) return false;  // should be able to at least handle the current allocation size

  // commit eagerly?
  bool arena_commit = false;
  if (mi_option_get(mi_option_arena_eager_commit) == 2)      { arena_commit = _mi_os_has_overcommit(); }
  else if (mi_option_get(mi_option_arena_eager_commit) == 1) { arena_commit = true; }

  return (mi_reserve_os_memory_ex(arena_reserve, arena_commit, allow_large, false /* exclusive? */, arena_id) == 0);
}


void* _mi_arena_alloc_aligned(size_t size, size_t alignment, size_t align_offset, bool commit, bool allow_large,
                              mi_arena_id_t req_arena_id, mi_memid_t* memid, mi_os_tld_t* tld)
{
  mi_assert_internal(memid != NULL && tld != NULL);
  mi_assert_internal(size > 0);
  *memid = _mi_memid_none();

  const int numa_node = _mi_os_numa_node(tld); // current numa node

  // try to allocate in an arena if the alignment is small enough and the object is not too small (as for heap meta data)
  if (!mi_option_is_enabled(mi_option_disallow_arena_alloc) || req_arena_id != _mi_arena_id_none()) {  // is arena allocation allowed?
    if (size >= MI_ARENA_MIN_OBJ_SIZE && alignment <= MI_SEGMENT_ALIGN && align_offset == 0) {
      void* p = mi_arena_try_alloc(numa_node, size, alignment, commit, allow_large, req_arena_id, memid, tld);
      if (p != NULL) return p;

      // otherwise, try to first eagerly reserve a new arena
      if (req_arena_id == _mi_arena_id_none()) {
        mi_arena_id_t arena_id = 0;
        if (mi_arena_reserve(size, allow_large, req_arena_id, &arena_id)) {
          // and try allocate in there
          mi_assert_internal(req_arena_id == _mi_arena_id_none());
          p = mi_arena_try_alloc_at_id(arena_id, true, numa_node, size, alignment, commit, allow_large, req_arena_id, memid, tld);
          if (p != NULL) return p;
        }
      }
    }
  }

  // if we cannot use OS allocation, return NULL
  if (mi_option_is_enabled(mi_option_disallow_os_alloc) || req_arena_id != _mi_arena_id_none()) {
    errno = ENOMEM;
    return NULL;
  }

  // finally, fall back to the OS
  if (align_offset > 0) {
    return _mi_os_alloc_aligned_at_offset(size, alignment, align_offset, commit, allow_large, memid, tld->stats);
  }
  else {
    return _mi_os_alloc_aligned(size, alignment, commit, allow_large, memid, tld->stats);
  }
}

void* _mi_arena_alloc(size_t size, bool commit, bool allow_large, mi_arena_id_t req_arena_id, mi_memid_t* memid, mi_os_tld_t* tld)
{
  return _mi_arena_alloc_aligned(size, MI_ARENA_BLOCK_SIZE, 0, commit, allow_large, req_arena_id, memid, tld);
}


void* mi_arena_area(mi_arena_id_t arena_id, size_t* size) {
  if (size != NULL) *size = 0;
  size_t arena_index = mi_arena_id_index(arena_id);
  if (arena_index >= MI_MAX_ARENAS) return NULL;
  mi_arena_t* arena = mi_atomic_load_ptr_acquire(mi_arena_t, &mi_arenas[arena_index]);
  if (arena == NULL) return NULL;
  if (size != NULL) { *size = mi_size_of_blocks(arena->block_count); }
  return arena->start;
}


/* -----------------------------------------------------------
  Arena purge
----------------------------------------------------------- */

static long mi_arena_purge_delay(void) {
  // <0 = no purging allowed, 0=immediate purging, >0=milli-second delay
  return (mi_option_get(mi_option_purge_delay) * mi_option_get(mi_option_arena_purge_mult));
}

// reset or decommit in an arena and update the committed/decommit bitmaps
// assumes we own the area (i.e. blocks_in_use is claimed by us)
static void mi_arena_purge(mi_arena_t* arena, size_t bitmap_idx, size_t blocks, mi_stats_t* stats) {
  mi_assert_internal(arena->blocks_committed != NULL);
  mi_assert_internal(arena->blocks_purge != NULL);
  mi_assert_internal(!arena->memid.is_pinned);
  const size_t size = mi_size_of_blocks(blocks);
  void* const p = mi_arena_block_start(arena, bitmap_idx);
  bool needs_recommit;
  if (_mi_bitmap_is_claimed_across(arena->blocks_committed, arena->field_count, blocks, bitmap_idx)) {
    // all blocks are committed, we can purge freely
    needs_recommit = _mi_os_purge(p, size, stats);
  }
  else {
    // some blocks are not committed -- this can happen when a partially committed block is freed
    // in `_mi_arena_free` and it is conservatively marked as uncommitted but still scheduled for a purge
    // we need to ensure we do not try to reset (as that may be invalid for uncommitted memory),
    // and also undo the decommit stats (as it was already adjusted)
    mi_assert_internal(mi_option_is_enabled(mi_option_purge_decommits));
    needs_recommit = _mi_os_purge_ex(p, size, false /* allow reset? */, stats);
    if (needs_recommit) { _mi_stat_increase(&_mi_stats_main.committed, size); }
  }

  // clear the purged blocks
  _mi_bitmap_unclaim_across(arena->blocks_purge, arena->field_count, blocks, bitmap_idx);
  // update committed bitmap
  if (needs_recommit) {
    _mi_bitmap_unclaim_across(arena->blocks_committed, arena->field_count, blocks, bitmap_idx);
  }
}

// Schedule a purge. This is usually delayed to avoid repeated decommit/commit calls.
// Note: assumes we (still) own the area as we may purge immediately
static void mi_arena_schedule_purge(mi_arena_t* arena, size_t bitmap_idx, size_t blocks, mi_stats_t* stats) {
  mi_assert_internal(arena->blocks_purge != NULL);
  const long delay = mi_arena_purge_delay();
  if (delay < 0) return;  // is purging allowed at all?

  if (_mi_preloading() || delay == 0) {
    // decommit directly
    mi_arena_purge(arena, bitmap_idx, blocks, stats);
  }
  else {
    // schedule decommit
    mi_msecs_t expire = mi_atomic_loadi64_relaxed(&arena->purge_expire);
    if (expire != 0) {
      mi_atomic_addi64_acq_rel(&arena->purge_expire, (mi_msecs_t)(delay/10));  // add smallish extra delay
    }
    else {
      mi_atomic_storei64_release(&arena->purge_expire, _mi_clock_now() + delay);
    }
    _mi_bitmap_claim_across(arena->blocks_purge, arena->field_count, blocks, bitmap_idx, NULL);
  }
}

// purge a range of blocks
// return true if the full range was purged.
// assumes we own the area (i.e. blocks_in_use is claimed by us)
static bool mi_arena_purge_range(mi_arena_t* arena, size_t idx, size_t startseqx, size_t bitlen, size_t purge, mi_stats_t* stats) {
  const size_t endidx = startseqx + bitlen;
  size_t bitseqx = startseqx;
  bool all_purged = false;
  while (bitseqx < endidx) {
    // count consecutive ones in the purge mask
    size_t count = 0;
    while (bitseqx + count < endidx && (purge & ((size_t)1 << (bitseqx + count))) != 0) {
      count++;
    }
    if (count > 0) {
      // found range to be purged
      const mi_bitmap_index_t range_idx = mi_bitmap_index_create(idx, bitseqx);
      mi_arena_purge(arena, range_idx, count, stats);
      if (count == bitlen) {
        all_purged = true;
      }
    }
    bitseqx += (count+1); // +1 to skip the zero bit (or end)
  }
  return all_purged;
}

// returns true if anything was purged
static bool mi_arena_try_purge(mi_arena_t* arena, mi_msecs_t now, bool force, mi_stats_t* stats)
{
  if (arena->memid.is_pinned || arena->blocks_purge == NULL) return false;
  mi_msecs_t expire = mi_atomic_loadi64_relaxed(&arena->purge_expire);
  if (expire == 0) return false;
  if (!force && expire > now) return false;

  // reset expire (if not already set concurrently)
  mi_atomic_casi64_strong_acq_rel(&arena->purge_expire, &expire, (mi_msecs_t)0);

  // potential purges scheduled, walk through the bitmap
  bool any_purged = false;
  bool full_purge = true;
  for (size_t i = 0; i < arena->field_count; i++) {
    size_t purge = mi_atomic_load_relaxed(&arena->blocks_purge[i]);
    if (purge != 0) {
      size_t bitseqx = 0;
      while (bitseqx < MI_BITMAP_FIELD_BITS) {
        // find consecutive range of ones in the purge mask
        size_t bitlen = 0;
        while (bitseqx + bitlen < MI_BITMAP_FIELD_BITS && (purge & ((size_t)1 << (bitseqx + bitlen))) != 0) {
          bitlen++;
        }
        // temporarily claim the purge range as "in-use" to be thread-safe with allocation
        // try to claim the longest range of corresponding in_use bits
        const mi_bitmap_index_t bitmap_index = mi_bitmap_index_create(i, bitseqx);
        while( bitlen > 0 ) {
          if (_mi_bitmap_try_claim(arena->blocks_inuse, arena->field_count, bitlen, bitmap_index)) {
            break;
          }
          bitlen--;
        }
        // actual claimed bits at `in_use`
        if (bitlen > 0) {
          // read purge again now that we have the in_use bits
          purge = mi_atomic_load_acquire(&arena->blocks_purge[i]);
          if (!mi_arena_purge_range(arena, i, bitseqx, bitlen, purge, stats)) {
            full_purge = false;
          }
          any_purged = true;
          // release the claimed `in_use` bits again
          _mi_bitmap_unclaim(arena->blocks_inuse, arena->field_count, bitlen, bitmap_index);
        }
        bitseqx += (bitlen+1);  // +1 to skip the zero (or end)
      } // while bitseqx
    } // purge != 0
  }
  // if not fully purged, make sure to purge again in the future
  if (!full_purge) {
    const long delay = mi_arena_purge_delay();
    mi_msecs_t expected = 0;
    mi_atomic_casi64_strong_acq_rel(&arena->purge_expire,&expected,_mi_clock_now() + delay);
  }
  return any_purged;
}

static void mi_arenas_try_purge( bool force, bool visit_all, mi_stats_t* stats ) {
  if (_mi_preloading() || mi_arena_purge_delay() <= 0) return;  // nothing will be scheduled

  const size_t max_arena = mi_atomic_load_acquire(&mi_arena_count);
  if (max_arena == 0) return;

  // allow only one thread to purge at a time
  static mi_atomic_guard_t purge_guard;
  mi_atomic_guard(&purge_guard)
  {
    mi_msecs_t now = _mi_clock_now();
    size_t max_purge_count = (visit_all ? max_arena : 1);
    for (size_t i = 0; i < max_arena; i++) {
      mi_arena_t* arena = mi_atomic_load_ptr_acquire(mi_arena_t, &mi_arenas[i]);
      if (arena != NULL) {
        if (mi_arena_try_purge(arena, now, force, stats)) {
          if (max_purge_count <= 1) break;
          max_purge_count--;
        }
      }
    }
  }
}


/* -----------------------------------------------------------
  Arena free
----------------------------------------------------------- */

void _mi_arena_free(void* p, size_t size, size_t committed_size, mi_memid_t memid, mi_stats_t* stats) {
  mi_assert_internal(size > 0 && stats != NULL);
  mi_assert_internal(committed_size <= size);
  if (p==NULL) return;
  if (size==0) return;
  const bool all_committed = (committed_size == size);

  // need to set all memory to undefined as some parts may still be marked as no_access (like padding etc.)
  mi_track_mem_undefined(p,size);

  if (mi_memkind_is_os(memid.memkind)) {
    // was a direct OS allocation, pass through
    if (!all_committed && committed_size > 0) {
      // if partially committed, adjust the committed stats (as `_mi_os_free` will increase decommit by the full size)
      _mi_stat_decrease(&_mi_stats_main.committed, committed_size);
    }
    _mi_os_free(p, size, memid, stats);
  }
  else if (memid.memkind == MI_MEM_ARENA) {
    // allocated in an arena
    size_t arena_idx;
    size_t bitmap_idx;
    mi_arena_memid_indices(memid, &arena_idx, &bitmap_idx);
    mi_assert_internal(arena_idx < MI_MAX_ARENAS);
    mi_arena_t* arena = mi_atomic_load_ptr_acquire(mi_arena_t,&mi_arenas[arena_idx]);
    mi_assert_internal(arena != NULL);
    const size_t blocks = mi_block_count_of_size(size);

    // checks
    if (arena == NULL) {
      _mi_error_message(EINVAL, "trying to free from an invalid arena: %p, size %zu, memid: 0x%zx\n", p, size, memid);
      return;
    }
    mi_assert_internal(arena->field_count > mi_bitmap_index_field(bitmap_idx));
    if (arena->field_count <= mi_bitmap_index_field(bitmap_idx)) {
      _mi_error_message(EINVAL, "trying to free from an invalid arena block: %p, size %zu, memid: 0x%zx\n", p, size, memid);
      return;
    }

    // potentially decommit
    if (arena->memid.is_pinned || arena->blocks_committed == NULL) {
      mi_assert_internal(all_committed);
    }
    else {
      mi_assert_internal(arena->blocks_committed != NULL);
      mi_assert_internal(arena->blocks_purge != NULL);

      if (!all_committed) {
        // mark the entire range as no longer committed (so we recommit the full range when re-using)
        _mi_bitmap_unclaim_across(arena->blocks_committed, arena->field_count, blocks, bitmap_idx);
        mi_track_mem_noaccess(p,size);
        if (committed_size > 0) {
          // if partially committed, adjust the committed stats (is it will be recommitted when re-using)
          // in the delayed purge, we now need to not count a decommit if the range is not marked as committed.
          _mi_stat_decrease(&_mi_stats_main.committed, committed_size);
        }
        // note: if not all committed, it may be that the purge will reset/decommit the entire range
        // that contains already decommitted parts. Since purge consistently uses reset or decommit that
        // works (as we should never reset decommitted parts).
      }
      // (delay) purge the entire range
      mi_arena_schedule_purge(arena, bitmap_idx, blocks, stats);
    }

    // and make it available to others again
    bool all_inuse = _mi_bitmap_unclaim_across(arena->blocks_inuse, arena->field_count, blocks, bitmap_idx);
    if (!all_inuse) {
      _mi_error_message(EAGAIN, "trying to free an already freed arena block: %p, size %zu\n", p, size);
      return;
    };
  }
  else {
    // arena was none, external, or static; nothing to do
    mi_assert_internal(memid.memkind < MI_MEM_OS);
  }

  // purge expired decommits
  mi_arenas_try_purge(false, false, stats);
}

// destroy owned arenas; this is unsafe and should only be done using `mi_option_destroy_on_exit`
// for dynamic libraries that are unloaded and need to release all their allocated memory.
static void mi_arenas_unsafe_destroy(void) {
  const size_t max_arena = mi_atomic_load_relaxed(&mi_arena_count);
  size_t new_max_arena = 0;
  for (size_t i = 0; i < max_arena; i++) {
    mi_arena_t* arena = mi_atomic_load_ptr_acquire(mi_arena_t, &mi_arenas[i]);
    if (arena != NULL) {
      mi_lock_done(&arena->abandoned_visit_lock);
      if (arena->start != NULL && mi_memkind_is_os(arena->memid.memkind)) {
        mi_atomic_store_ptr_release(mi_arena_t, &mi_arenas[i], NULL);
        _mi_os_free(arena->start, mi_arena_size(arena), arena->memid, &_mi_stats_main);
      }
      else {
        new_max_arena = i;
      }
      _mi_arena_meta_free(arena, arena->meta_memid, arena->meta_size);
    }
  }

  // try to lower the max arena.
  size_t expected = max_arena;
  mi_atomic_cas_strong_acq_rel(&mi_arena_count, &expected, new_max_arena);
}

// Purge the arenas; if `force_purge` is true, amenable parts are purged even if not yet expired
void _mi_arenas_collect(bool force_purge, mi_stats_t* stats) {
  mi_arenas_try_purge(force_purge, force_purge /* visit all? */, stats);
}

// destroy owned arenas; this is unsafe and should only be done using `mi_option_destroy_on_exit`
// for dynamic libraries that are unloaded and need to release all their allocated memory.
void _mi_arena_unsafe_destroy_all(mi_stats_t* stats) {
  mi_arenas_unsafe_destroy();
  _mi_arenas_collect(true /* force purge */, stats);  // purge non-owned arenas
}

// Is a pointer inside any of our arenas?
bool _mi_arena_contains(const void* p) {
  const size_t max_arena = mi_atomic_load_relaxed(&mi_arena_count);
  for (size_t i = 0; i < max_arena; i++) {
    mi_arena_t* arena = mi_atomic_load_ptr_relaxed(mi_arena_t, &mi_arenas[i]);
    if (arena != NULL && arena->start <= (const uint8_t*)p && arena->start + mi_size_of_blocks(arena->block_count) > (const uint8_t*)p) {
      return true;
    }
  }
  return false;
}

/* -----------------------------------------------------------
  Add an arena.
----------------------------------------------------------- */

static bool mi_arena_add(mi_arena_t* arena, mi_arena_id_t* arena_id, mi_stats_t* stats) {
  mi_assert_internal(arena != NULL);
  mi_assert_internal((uintptr_t)mi_atomic_load_ptr_relaxed(uint8_t,&arena->start) % MI_SEGMENT_ALIGN == 0);
  mi_assert_internal(arena->block_count > 0);
  if (arena_id != NULL) { *arena_id = -1; }

  size_t i = mi_atomic_increment_acq_rel(&mi_arena_count);
  if (i >= MI_MAX_ARENAS) {
    mi_atomic_decrement_acq_rel(&mi_arena_count);
    return false;
  }
  _mi_stat_counter_increase(&stats->arena_count,1);
  arena->id = mi_arena_id_create(i);
  mi_atomic_store_ptr_release(mi_arena_t,&mi_arenas[i], arena);
  if (arena_id != NULL) { *arena_id = arena->id; }
  return true;
}

static bool mi_manage_os_memory_ex2(void* start, size_t size, bool is_large, int numa_node, bool exclusive, mi_memid_t memid, mi_arena_id_t* arena_id) mi_attr_noexcept
{
  if (arena_id != NULL) *arena_id = _mi_arena_id_none();
  if (size < MI_ARENA_BLOCK_SIZE) return false;

  if (is_large) {
    mi_assert_internal(memid.initially_committed && memid.is_pinned);
  }

  const size_t bcount = size / MI_ARENA_BLOCK_SIZE;
  const size_t fields = _mi_divide_up(bcount, MI_BITMAP_FIELD_BITS);
  const size_t bitmaps = (memid.is_pinned ? 3 : 5);
  const size_t asize  = sizeof(mi_arena_t) + (bitmaps*fields*sizeof(mi_bitmap_field_t));
  mi_memid_t meta_memid;
  mi_arena_t* arena   = (mi_arena_t*)_mi_arena_meta_zalloc(asize, &meta_memid);
  if (arena == NULL) return false;

  // already zero'd due to zalloc
  // _mi_memzero(arena, asize);
  arena->id = _mi_arena_id_none();
  arena->memid = memid;
  arena->exclusive = exclusive;
  arena->meta_size = asize;
  arena->meta_memid = meta_memid;
  arena->block_count = bcount;
  arena->field_count = fields;
  arena->start = (uint8_t*)start;
  arena->numa_node    = numa_node; // TODO: or get the current numa node if -1? (now it allows anyone to allocate on -1)
  arena->is_large     = is_large;
  arena->purge_expire = 0;
  arena->search_idx   = 0;
  mi_lock_init(&arena->abandoned_visit_lock);
  // consecutive bitmaps
  arena->blocks_dirty     = &arena->blocks_inuse[fields];     // just after inuse bitmap
  arena->blocks_abandoned = &arena->blocks_inuse[2 * fields]; // just after dirty bitmap
  arena->blocks_committed = (arena->memid.is_pinned ? NULL : &arena->blocks_inuse[3*fields]); // just after abandoned bitmap
  arena->blocks_purge     = (arena->memid.is_pinned ? NULL : &arena->blocks_inuse[4*fields]); // just after committed bitmap
  // initialize committed bitmap?
  if (arena->blocks_committed != NULL && arena->memid.initially_committed) {
    memset((void*)arena->blocks_committed, 0xFF, fields*sizeof(mi_bitmap_field_t)); // cast to void* to avoid atomic warning
  }

  // and claim leftover blocks if needed (so we never allocate there)
  ptrdiff_t post = (fields * MI_BITMAP_FIELD_BITS) - bcount;
  mi_assert_internal(post >= 0);
  if (post > 0) {
    // don't use leftover bits at the end
    mi_bitmap_index_t postseqx = mi_bitmap_index_create(fields - 1, MI_BITMAP_FIELD_BITS - post);
    _mi_bitmap_claim(arena->blocks_inuse, fields, post, postseqx, NULL);
  }
  return mi_arena_add(arena, arena_id, &_mi_stats_main);

}

bool mi_manage_os_memory_ex(void* start, size_t size, bool is_committed, bool is_large, bool is_zero, int numa_node, bool exclusive, mi_arena_id_t* arena_id) mi_attr_noexcept {
  mi_memid_t memid = _mi_memid_create(MI_MEM_EXTERNAL);
  memid.initially_committed = is_committed;
  memid.initially_zero = is_zero;
  memid.is_pinned = is_large;
  return mi_manage_os_memory_ex2(start,size,is_large,numa_node,exclusive,memid, arena_id);
}

// Reserve a range of regular OS memory
int mi_reserve_os_memory_ex(size_t size, bool commit, bool allow_large, bool exclusive, mi_arena_id_t* arena_id) mi_attr_noexcept {
  if (arena_id != NULL) *arena_id = _mi_arena_id_none();
  size = _mi_align_up(size, MI_ARENA_BLOCK_SIZE); // at least one block
  mi_memid_t memid;
  void* start = _mi_os_alloc_aligned(size, MI_SEGMENT_ALIGN, commit, allow_large, &memid, &_mi_stats_main);
  if (start == NULL) return ENOMEM;
  const bool is_large = memid.is_pinned; // todo: use separate is_large field?
  if (!mi_manage_os_memory_ex2(start, size, is_large, -1 /* numa node */, exclusive, memid, arena_id)) {
    _mi_os_free_ex(start, size, commit, memid, &_mi_stats_main);
    _mi_verbose_message("failed to reserve %zu KiB memory\n", _mi_divide_up(size, 1024));
    return ENOMEM;
  }
  _mi_verbose_message("reserved %zu KiB memory%s\n", _mi_divide_up(size, 1024), is_large ? " (in large os pages)" : "");
  return 0;
}


// Manage a range of regular OS memory
bool mi_manage_os_memory(void* start, size_t size, bool is_committed, bool is_large, bool is_zero, int numa_node) mi_attr_noexcept {
  return mi_manage_os_memory_ex(start, size, is_committed, is_large, is_zero, numa_node, false /* exclusive? */, NULL);
}

// Reserve a range of regular OS memory
int mi_reserve_os_memory(size_t size, bool commit, bool allow_large) mi_attr_noexcept {
  return mi_reserve_os_memory_ex(size, commit, allow_large, false, NULL);
}


/* -----------------------------------------------------------
  Debugging
----------------------------------------------------------- */

static size_t mi_debug_show_bitmap(const char* prefix, const char* header, size_t block_count, mi_bitmap_field_t* fields, size_t field_count ) {
  _mi_verbose_message("%s%s:\n", prefix, header);
  size_t bcount = 0;
  size_t inuse_count = 0;
  for (size_t i = 0; i < field_count; i++) {
    char buf[MI_BITMAP_FIELD_BITS + 1];
    uintptr_t field = mi_atomic_load_relaxed(&fields[i]);
    for (size_t bit = 0; bit < MI_BITMAP_FIELD_BITS; bit++, bcount++) {
      if (bcount < block_count) {
        bool inuse = ((((uintptr_t)1 << bit) & field) != 0);
        if (inuse) inuse_count++;
        buf[bit] = (inuse ? 'x' : '.');
      }
      else {
        buf[bit] = ' ';
      }
    }
    buf[MI_BITMAP_FIELD_BITS] = 0;
    _mi_verbose_message("%s  %s\n", prefix, buf);
  }
  _mi_verbose_message("%s  total ('x'): %zu\n", prefix, inuse_count);
  return inuse_count;
}

void mi_debug_show_arenas(bool show_inuse, bool show_abandoned, bool show_purge) mi_attr_noexcept {
  size_t max_arenas = mi_atomic_load_relaxed(&mi_arena_count);
  size_t inuse_total = 0;
  size_t abandoned_total = 0;
  size_t purge_total = 0;
  for (size_t i = 0; i < max_arenas; i++) {
    mi_arena_t* arena = mi_atomic_load_ptr_relaxed(mi_arena_t, &mi_arenas[i]);
    if (arena == NULL) break;
    _mi_verbose_message("arena %zu: %zu blocks of size %zuMiB (in %zu fields) %s\n", i, arena->block_count, MI_ARENA_BLOCK_SIZE / MI_MiB, arena->field_count, (arena->memid.is_pinned ? ", pinned" : ""));
    if (show_inuse) {
      inuse_total += mi_debug_show_bitmap("  ", "inuse blocks", arena->block_count, arena->blocks_inuse, arena->field_count);
    }
    if (arena->blocks_committed != NULL) {
      mi_debug_show_bitmap("  ", "committed blocks", arena->block_count, arena->blocks_committed, arena->field_count);
    }
    if (show_abandoned) {
      abandoned_total += mi_debug_show_bitmap("  ", "abandoned blocks", arena->block_count, arena->blocks_abandoned, arena->field_count);
    }
    if (show_purge && arena->blocks_purge != NULL) {
      purge_total += mi_debug_show_bitmap("  ", "purgeable blocks", arena->block_count, arena->blocks_purge, arena->field_count);
    }
  }
  if (show_inuse)     _mi_verbose_message("total inuse blocks    : %zu\n", inuse_total);
  if (show_abandoned) _mi_verbose_message("total abandoned blocks: %zu\n", abandoned_total);
  if (show_purge)     _mi_verbose_message("total purgeable blocks: %zu\n", purge_total);
}


/* -----------------------------------------------------------
  Reserve a huge page arena.
----------------------------------------------------------- */
// reserve at a specific numa node
int mi_reserve_huge_os_pages_at_ex(size_t pages, int numa_node, size_t timeout_msecs, bool exclusive, mi_arena_id_t* arena_id) mi_attr_noexcept {
  if (arena_id != NULL) *arena_id = -1;
  if (pages==0) return 0;
  if (numa_node < -1) numa_node = -1;
  if (numa_node >= 0) numa_node = numa_node % _mi_os_numa_node_count();
  size_t hsize = 0;
  size_t pages_reserved = 0;
  mi_memid_t memid;
  void* p = _mi_os_alloc_huge_os_pages(pages, numa_node, timeout_msecs, &pages_reserved, &hsize, &memid);
  if (p==NULL || pages_reserved==0) {
    _mi_warning_message("failed to reserve %zu GiB huge pages\n", pages);
    return ENOMEM;
  }
  _mi_verbose_message("numa node %i: reserved %zu GiB huge pages (of the %zu GiB requested)\n", numa_node, pages_reserved, pages);

  if (!mi_manage_os_memory_ex2(p, hsize, true, numa_node, exclusive, memid, arena_id)) {
    _mi_os_free(p, hsize, memid, &_mi_stats_main);
    return ENOMEM;
  }
  return 0;
}

int mi_reserve_huge_os_pages_at(size_t pages, int numa_node, size_t timeout_msecs) mi_attr_noexcept {
  return mi_reserve_huge_os_pages_at_ex(pages, numa_node, timeout_msecs, false, NULL);
}

// reserve huge pages evenly among the given number of numa nodes (or use the available ones as detected)
int mi_reserve_huge_os_pages_interleave(size_t pages, size_t numa_nodes, size_t timeout_msecs) mi_attr_noexcept {
  if (pages == 0) return 0;

  // pages per numa node
  size_t numa_count = (numa_nodes > 0 ? numa_nodes : _mi_os_numa_node_count());
  if (numa_count <= 0) numa_count = 1;
  const size_t pages_per = pages / numa_count;
  const size_t pages_mod = pages % numa_count;
  const size_t timeout_per = (timeout_msecs==0 ? 0 : (timeout_msecs / numa_count) + 50);

  // reserve evenly among numa nodes
  for (size_t numa_node = 0; numa_node < numa_count && pages > 0; numa_node++) {
    size_t node_pages = pages_per;  // can be 0
    if (numa_node < pages_mod) node_pages++;
    int err = mi_reserve_huge_os_pages_at(node_pages, (int)numa_node, timeout_per);
    if (err) return err;
    if (pages < node_pages) {
      pages = 0;
    }
    else {
      pages -= node_pages;
    }
  }

  return 0;
}

int mi_reserve_huge_os_pages(size_t pages, double max_secs, size_t* pages_reserved) mi_attr_noexcept {
  MI_UNUSED(max_secs);
  _mi_warning_message("mi_reserve_huge_os_pages is deprecated: use mi_reserve_huge_os_pages_interleave/at instead\n");
  if (pages_reserved != NULL) *pages_reserved = 0;
  int err = mi_reserve_huge_os_pages_interleave(pages, 0, (size_t)(max_secs * 1000.0));
  if (err==0 && pages_reserved!=NULL) *pages_reserved = pages;
  return err;
}


#endif