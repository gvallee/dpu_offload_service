//
// Copyright (c) 2022-2023, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <limits.h>
#include <inttypes.h>

#include "dpu_offload_types.h"
#include "dpu_offload_debug.h"

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
get_host_idx_by_group(offloading_engine_t *engine,
                      group_uid_t group_uid,
                      size_t *host_idx)
{
    return DO_SUCCESS;
}

dpu_offload_status_t
get_num_sps_by_group_host(offloading_engine_t *engine,
                          group_uid_t group_uid,
                          size_t host_idx,
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
get_num_ranks_for_group_host_local_sp(offloading_engine_t *engine,
                                      group_uid_t group_uid,
                                      size_t host_idx,
                                      uint64_t local_host_sp_id,
                                      size_t *num_ranks)
{
    return DO_SUCCESS;
}

dpu_offload_status_t
get_num_ranks_for_group_host_idx(offloading_engine_t *engine,
                                 group_uid_t group_uid,
                                 size_t host_idx,
                                 size_t *num_ranks)
{
    return DO_SUCCESS;
}

dpu_offload_status_t
get_rank_idx_by_group_host_idx(offloading_engine_t *engine,
                               group_uid_t group_uid,
                               size_t host_idx,
                               int64_t rank,
                               int64_t *idx)
{
    return DO_SUCCESS;
}

dpu_offload_status_t
get_all_sps_by_group_host(offloading_engine_t *engine,
                          group_uid_t group_uid,
                          size_t host_idx,
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
                          uint64_t sp_group_gid,
                          dyn_array_t *ranks,
                          size_t *num_ranks)
{
    return DO_SUCCESS;
}

dpu_offload_status_t
get_nth_sp_by_group_host_idx(offloading_engine_t *engine,
                             group_uid_t group_uid,
                             size_t host_idx,
                             uint64_t *global_group_sp_id)
{
    return DO_SUCCESS;
}

dpu_offload_status_t
populate_group_cache_lookup_table(offloading_engine_t *engine,
                                  group_cache_t *gp_cache)
{
    size_t i, idx = 0;

    assert(gp_cache);
    assert(group_cache_populated(engine, gp_cache->group_uid));

    INFO_MSG("Creating contiguous list of SPs involved in the group");

    // Get the total number of SPs that are involved
    INFO_MSG("-> %ld SPs", gp_cache->n_sps);
    if (gp_cache->sp_array_initialized == false)
    {
        gp_cache->sps = malloc(gp_cache->n_sps * sizeof(remote_service_proc_info_t*));
        assert(gp_cache->sps);
        gp_cache->sp_array_initialized = true;
    }

    i = 0;
    while (i < gp_cache->n_sps)
    {
        if (GROUP_CACHE_BITSET_TEST(gp_cache->sps_bitset, idx))
        {
            remote_service_proc_info_t *sp_data = NULL;
            sp_data = DYN_ARRAY_GET_ELT(&(engine->service_procs),
                                        idx,
                                        remote_service_proc_info_t);
            assert(sp_data);
            INFO_MSG("\tSP %ld is involved (idx: %ld)", idx, sp_data->idx);
            gp_cache->sps[i] = sp_data;
            i++;
        }
        idx++;
    }

    return DO_SUCCESS;
}
