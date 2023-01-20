//
// Copyright (c) 2022-2023, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <limits.h>
#include <inttypes.h>

#include "dpu_offload_types.h"
#include "dpu_offload_debug.h"

typedef char group_cache_bitset_t;

// Create a bitset mask
#define GROUP_CACHE_BITSET_MASK(_bit) (1 << ((_bit) % CHAR_BIT))

// Return the slot of a given bit
#define GROUP_CACHE_BITSET_SLOT(_bit) ((_bit) / CHAR_BIT)

// Set a given bit in a bitset
#define GROUP_CACHE_BITSET_SET(_bitset, _bit) ((_bistset)[GROUP_CACHE_BITSET_SLOT(_bit)] |= GROUP_CACHE_BITSET_MASK(_bit))

// Clear a given bit in a bitset
#define GROUP_CACHE_BITSET_CLEAR(_bitset, _bit) ((_bitset)[GROUP_CACHE_BITSET_SLOT(_bit)] &= ~GROUP_CACHE_BITSET_MASK(_bit))

// Test if a bit in the bitset is set
#define GROUP_CACHE_BITSET_TEST(_bitset, _bitset_idx) ((_bitset)[GROUP_CACHE_BITSET_SLOT(_bitset_idx)] & GROUP_CACHE_BITSET_MASK(_bitset_idx))

// Return the number of slots required to implement a bitset of a given size
#define GROUP_CACHE_BITSET_NSLOTS(_bitset_size) ((_bitset_size + CHAR_BIT - 1) / CHAR_BIT)

// Create a given bitset based on a size
#define GROUP_CACHE_BITSET_CREATE(_bitset_ptr, _size)                                        \
    do                                                                                       \
    {                                                                                        \
        _bitset_ptr = calloc(GROUP_CACHE_BITSET_NSLOTS(size), sizeof(group_cache_bitset_t)); \
    } while (0)

// Destroy a given bitset that was previously created with GROUP_CACHE_BITSET_CREATE
#define GROUP_CACHE_BITSET_DESTROY(_bitset_ptr) \
    do                                          \
    {                                           \
        if ((_bitset_ptr) != NULL)              \
        {                                       \
            free(_bitset_ptr);                  \
            _bitset_ptr = NULL;                 \
        }                                       \
    } while (0)

bool group_cache_populated(offloading_engine_t *engine, group_uid_t gp_uid)
{
    group_cache_t *gp_cache = GET_GROUP_CACHE(&(engine->procs_cache), gp_uid);
    assert(gp_cache);
    if (gp_cache->global_revoked == 0 && gp_cache->group_size == gp_cache->num_local_entries)
    {
        DBG("Group cache for group 0x%x fully populated. num_local_entries = %" PRIu64 " group_size = %" PRIu64,
            gp_uid, gp_cache->num_local_entries, gp_cache->group_size);
        return true;
    }
    return false;
}

dpu_offload_status_t
get_global_sp_id_by_group(offloading_engine_t *engine,
                          group_uid_t gp_uid,
                          uint64_t *sp_id)
{
    return DO_SUCCESS;
}

dpu_offload_status_t
get_local_sp_id_by_group(offloading_engine_t *engine,
                         group_uid_t gp_uid,
                         uint64_t sp_gp_giuid,
                         uint64_t *sp_gp_lid)
{
    return DO_SUCCESS;
}

dpu_offload_status_t
get_ordered_host_id_by_group(offloading_engine_t *engine,
                             group_uid_t group_uid,
                             host_info_t *host_id)
{
    return DO_SUCCESS;
}

dpu_offload_status_t
get_num_sps_by_group_host(offloading_engine_t *engine,
                          group_uid_t group_uid,
                          host_info_t host_id,
                          size_t *num_sps)
{
    return DO_SUCCESS;
}

dpu_offload_status_t
get_num_ranks_for_group_sp(offloading_engine_t *engine,
                           group_uid_t group_uid,
                           uint64_t sp_gp_gid,
                           size_t *num_ranks)
{
    return DO_SUCCESS;
}

dpu_offload_status_t
get_num_ranks_for_group_host(offloading_engine_t *engine,
                             group_uid_t group_uid,
                             host_info_t host_id,
                             size_t *num_ranks)
{
    return DO_SUCCESS;
}

dpu_offload_status_t
get_n_for_rank_by_group_host(offloading_engine_t *engine,
                             group_uid_t group_uid,
                             host_info_t host_id,
                             int64_t rank,
                             int64_t *idx)
{
    return DO_SUCCESS;
}

dpu_offload_status_t
get_all_sps_by_group_host(offloading_engine_t *engine,
                          group_uid_t group_uid,
                          host_info_t host_id,
                          dyn_array_t *sps,
                          size_t *num_sps)
{
    return DO_SUCCESS;
}

dpu_offload_status_t
get_all_hosts_by_group(offloading_engine_t *engine,
                       group_uid_t group_uid,
                       dyn_array_t *hosts,
                       size_t *num_hosts)
{
    return DO_SUCCESS;
}

dpu_offload_status_t
get_all_ranks_by_group_sp(offloading_engine_t *engine,
                          group_uid_t group_uid,
                          uint64_t remote_global_group_sp_id,
                          dyn_array_t *ranks,
                          size_t *num_ranks)
{
    return DO_SUCCESS;
}

dpu_offload_status_t
get_nth_sp_by_group_host(offloading_engine_t *engine,
                         group_uid_t group_uid,
                         host_info_t host_id,
                         uint64_t *global_group_sp_id)
{
    return DO_SUCCESS;
}

dpu_offload_status_t
populate_group_cache_lookup_table(offloading_engine_t *engine,
                                  group_cache_t *gp_cache)
{
    assert(gp_cache);
    assert(group_cache_populated(engine, gp_cache->group_uid));
    // TODO

    return DO_SUCCESS;
}
