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

#define DEFAULT_MAX_GROUPS (64)
#define DEFAULT_NUM_RANKS_IN_GROUP (2048)

/* GROUPS_CACHE_INIT initializes the cache that holds information about all the groups */
#define GROUPS_CACHE_INIT(_cache)                                                                   \
    do                                                                                              \
    {                                                                                               \
        /* an array of pointers, each entry index is the group ID and the pointer */                \
        /* gives access to the list of ranks in the group */                                        \
        dyn_array_t *_ptr = &(_cache->data);                                                        \
        DYN_ARRAY_ALLOC(_ptr, DEFAULT_MAX_GROUPS, uint8_t); /* uint8_t for the size of a pointer */ \
    } while (0)

/* GROUP_CACHE_INIT initializes the cache for a given group */
#define GROUP_CACHE_INIT(_cache, _gp_id)                                     \
    do                                                                       \
    {                                                                        \
        void *_gp_cache = _cache[_gp_id];                                    \
        DYN_ARRAY_ALLOC((dyn_array_t *)_gp_cache, 2048, peer_cache_entry_t); \
    } while (0)

#endif // DPU_OFFLOAD_MEM_MGT_H__