//
// Copyright (c) 2023, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include "dpu_offload_types.h"

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
                         uint64_t *sp_id)
{
    return DO_SUCCESS;
}

dpu_offload_status_t
get_ordered_host_id_by_group(offloading_engine_t *engine,
                             group_uid_t group_id,
                             host_info_t *host_id)
{
    return DO_SUCCESS;
}

dpu_offload_status_t
get_num_sps_by_group_host(offloading_engine_t *engine,
                          group_uid_t group_id,
                          size_t *num_sps)
{
    return DO_SUCCESS;
}

dpu_offload_status_t
get_num_ranks_for_group_sp(offloading_engine_t *engine,
                           group_uid_t group_id,
                           size_t *num_ranks)
{
    return DO_SUCCESS;
}

dpu_offload_status_t
get_num_ranks_for_group_host(offloading_engine_t *engine,
                             group_uid_t group_id,
                             host_info_t my_host_id,
                             size_t *num_ranks)
{
    return DO_SUCCESS;
}

dpu_offload_status_t
get_n_for_rank_by_group_host(offloading_engine_t *engine,
                             group_uid_t group_id,
                             host_info_t my_host_id,
                             int64_t *idx)
{
    return DO_SUCCESS;
}

dpu_offload_status_t
get_all_sps_by_group_host(offloading_engine_t *engine,
                          group_uid_t group_id,
                          host_info_t host_id,
                          dyn_array_t *sps,
                          size_t *num_sps)
{
    return DO_SUCCESS;
}

dpu_offload_status_t
get_all_hosts_by_group(offloading_engine_t *engine,
                       group_uid_t group_id,
                       dyn_array_t *hosts,
                       size_t *num_hosts)
{
    return DO_SUCCESS;
}

dpu_offload_status_t
get_all_ranks_by_group_sp(offloading_engine_t *engine,
                          group_uid_t group_id,
                          uint64_t remote_global_group_sp_id,
                          dyn_array_t *ranks,
                          size_t *num_ranks)
{
    return DO_SUCCESS;
}

dpu_offload_status_t
get_nth_sp_by_group_host(offloading_engine_t *engine,
                         group_uid_t group_id,
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


