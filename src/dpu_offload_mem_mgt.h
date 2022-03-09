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

#define DEFAULT_NUM_GROUPS (64)
#define DEFAULT_NUM_RANKS_IN_GROUP (2048)

/* GROUPS_CACHE_INIT initializes the cache that holds information about all the groups */
#define GROUPS_CACHE_INIT(_cache)                                              \
    do                                                                         \
    {                                                                          \
        DYN_ARRAY_ALLOC(&((_cache)->data), DEFAULT_NUM_GROUPS, group_cache_t); \
        (_cache)->size = 0;                                                    \
    } while (0)

#define GROUPS_CACHE_FINI(_cache)                                                    \
    do                                                                               \
    {                                                                                \
        /* an array of pointers, each entry index is the group ID and the pointer */ \
        /* gives access to the list of ranks in the group */                         \
        /*dyn_array_t *_ptr = &(_cache->data);*/                                     \
        size_t _i;                                                                   \
        group_cache_t *_gp_caches = (group_cache_t *)(_cache)->data.base;            \
        for (_i = 0; _i < (_cache)->data.num_elts; _i++)                             \
        {                                                                            \
            if (_gp_caches[_i].initialized)                                          \
            {                                                                        \
                DYN_ARRAY_FREE(&(_gp_caches[_i].ranks));                             \
            }                                                                        \
        }                                                                            \
        DYN_ARRAY_FREE(&((_cache)->data));                                           \
        (_cache)->size = 0;                                                          \
    } while (0)

/* GROUP_CACHE_INIT initializes the cache for a given group */
#define GROUP_CACHE_INIT(_cache, _gp_id)                                     \
    do                                                                       \
    {                                                                        \
        void *_gp_cache = _cache[_gp_id];                                    \
        DYN_ARRAY_ALLOC((dyn_array_t *)_gp_cache, 2048, peer_cache_entry_t); \
    } while (0)

#endif // DPU_OFFLOAD_MEM_MGT_H__