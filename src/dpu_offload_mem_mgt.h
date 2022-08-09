//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <assert.h>
#include <stddef.h>

#include "dpu_offload_types.h"

#ifndef DPU_OFFLOAD_MEM_MGT_H__
#define DPU_OFFLOAD_MEM_MGT_H__

#define MAX_GROUPS (128)

#include "dynamic_structs.h"

#define DEFAULT_NUM_GROUPS (32)
#define DEFAULT_NUM_RANKS_IN_GROUP (2048)

#if NDEBUG
#define DPU_OFFLOAD_MALLOC(_size) ({ \
    void *_ptr = malloc((_size));    \
    _ptr;                            \
})
#else
#define DPU_OFFLOAD_MALLOC(_size) ({ \
    void *_ptr = malloc((_size));    \
    if (_ptr != NULL)                \
    {                                \
        memset(_ptr, 0xef, _size);   \
    }                                \
    _ptr;                            \
})
#endif // NDEBUG

/* GROUPS_CACHE_INIT initializes the cache that holds information about all the groups */
#define GROUPS_CACHE_INIT(_cache)                                                            \
    do                                                                                       \
    {                                                                                        \
        (_cache)->data = kh_init(group_hash_t);                                              \
        DYN_ARRAY_ALLOC(&((_cache)->keys), DEFAULT_NUM_GROUPS, int64_t);                     \
        DYN_LIST_ALLOC((_cache)->group_cache_pool, DEFAULT_NUM_GROUPS, group_cache_t, item); \
        (_cache)->size = 0;                                                                  \
        (_cache)->keys_in_use = 0;                                                           \
    } while (0)

#define GROUPS_CACHE_FINI(_cache)                                       \
    do                                                                  \
    {                                                                   \
        kh_destroy(group_hash_t, (_cache)->data);                       \
        DYN_ARRAY_FREE(&((_cache)->keys));                              \
        DYN_LIST_FREE((_cache)->group_cache_pool, group_cache_t, item); \
        (_cache)->group_cache_pool = NULL;                              \
        (_cache)->size = 0;                                             \
        (_cache)->keys_in_use = 0;                                      \
    } while (0)

/* GROUP_CACHE_INIT initializes the cache for a given group */
#define GROUP_CACHE_INIT(_cache, _gp_id)                                     \
    do                                                                       \
    {                                                                        \
        void *_gp_cache = _cache[_gp_id];                                    \
        DYN_ARRAY_ALLOC((dyn_array_t *)_gp_cache, 2048, peer_cache_entry_t); \
    } while (0)

#endif // DPU_OFFLOAD_MEM_MGT_H__
