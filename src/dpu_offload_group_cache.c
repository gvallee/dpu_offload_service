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
    size_t i;
    group_cache_t *gp_cache = NULL;

    assert(engine);
    if (!engine->on_dpu)
        return DO_ERROR;

    gp_cache = GET_GROUP_CACHE(&(engine->procs_cache), gp_uid);
    assert(gp_cache);
    for (i = 0; i < gp_cache->n_sps; i++)
    {
        remote_service_proc_info_t **ptr = NULL;

        ptr = DYN_ARRAY_GET_ELT(&(gp_cache->sps),
                                i,
                                remote_service_proc_info_t *);
        assert(ptr);
        if ((*ptr)->service_proc.global_id == engine->config->local_service_proc.info.global_id)
        {
            *sp_id = engine->config->local_service_proc.info.global_id;
            return DO_SUCCESS;
        }
    }
    // The SP is not in the group, which is unexpected so an error
    return DO_ERROR;
}

dpu_offload_status_t
get_local_sp_id_by_group(offloading_engine_t *engine,
                         group_uid_t gp_uid,
                         uint64_t sp_gp_guid,
                         uint64_t *sp_gp_lid)
{
    remote_service_proc_info_t **ptr = NULL;
    sp_cache_data_t *sp_data = NULL;
    group_cache_t *gp_cache = NULL;

    assert(engine);
    gp_cache = GET_GROUP_CACHE(&(engine->procs_cache), gp_uid);
    assert(gp_cache);
    ptr = DYN_ARRAY_GET_ELT(&(gp_cache->sps), sp_gp_guid, remote_service_proc_info_t *);
    assert(ptr);
    sp_data = GET_GROUP_SP_HASH_ENTRY(gp_cache, (*ptr)->service_proc.global_id);
    assert(sp_data);
    *sp_gp_lid = sp_data->lid;
    return DO_SUCCESS;
}

dpu_offload_status_t
get_host_idx_by_group(offloading_engine_t *engine,
                      group_uid_t group_uid,
                      size_t *host_idx)
{
    group_cache_t *gp_cache = NULL;
    host_uid_t my_host_uid;
    size_t i;

    assert(engine);
    my_host_uid = engine->config->local_service_proc.host_uid;
    gp_cache = GET_GROUP_CACHE(&(engine->procs_cache), group_uid);
    assert(gp_cache);
    for (i = 0; i < gp_cache->n_hosts; i++)
    {
        host_info_t **ptr = NULL;
        ptr = DYN_ARRAY_GET_ELT(&(gp_cache->hosts),
                                i,
                                host_info_t *);
        assert(ptr);
        if ((*ptr)->uid == my_host_uid)
        {
            *host_idx = i;
            return DO_SUCCESS;
        }
    }
    // The host is not in the group, which is not expected so an error.
    return DO_ERROR;
}

dpu_offload_status_t
get_num_sps_by_group_host_idx(offloading_engine_t *engine,
                              group_uid_t group_uid,
                              size_t host_idx,
                              size_t *num_sps)
{
    host_info_t **ptr = NULL;
    host_cache_data_t *host_data = NULL;
    group_cache_t *gp_cache = NULL;

    assert(engine);
    gp_cache = GET_GROUP_CACHE(&(engine->procs_cache), group_uid);
    assert(gp_cache);
    ptr = DYN_ARRAY_GET_ELT(&(gp_cache->hosts),
                            host_idx,
                            host_info_t *);
    assert(ptr);
    host_data = GET_GROUP_HOST_HASH_ENTRY(gp_cache, (*ptr)->uid);
    assert(host_data);
    *num_sps = host_data->num_sps;
    return DO_SUCCESS;
}

dpu_offload_status_t
get_num_ranks_for_group_sp(offloading_engine_t *engine,
                           group_uid_t group_uid,
                           uint64_t sp_gp_gid,
                           size_t *num_ranks)
{
    remote_service_proc_info_t **sp_data = NULL;
    sp_cache_data_t *sp_info = NULL;
    group_cache_t *gp_cache = NULL;

    assert(engine);
    gp_cache = GET_GROUP_CACHE(&(engine->procs_cache), group_uid);
    assert(gp_cache);
    sp_data = DYN_ARRAY_GET_ELT(&(gp_cache->sps), sp_gp_gid, remote_service_proc_info_t *);
    assert(sp_data);
    sp_info = GET_GROUP_SP_HASH_ENTRY(gp_cache, (*sp_data)->service_proc.global_id);
    assert(sp_info);
    *num_ranks = sp_info->n_ranks;
    return DO_SUCCESS;
}

dpu_offload_status_t
get_num_ranks_for_group_host_local_sp(offloading_engine_t *engine,
                                      group_uid_t group_uid,
                                      size_t host_idx,
                                      uint64_t local_host_sp_id,
                                      size_t *num_ranks)
{
    group_cache_t *gp_cache = NULL;
    host_info_t **host_info_ptr = NULL;
    host_cache_data_t *host_data = NULL;
    sp_cache_data_t **sp_data_ptr = NULL;

    assert(engine);
    gp_cache = GET_GROUP_CACHE(&(engine->procs_cache), group_uid);
    assert(gp_cache);
    host_info_ptr = DYN_ARRAY_GET_ELT(&(gp_cache->hosts), host_idx, host_info_t *);
    assert(host_info_ptr);
    host_data = GET_GROUP_HOST_HASH_ENTRY(gp_cache, (*host_info_ptr)->uid);
    assert(host_data);
    if (local_host_sp_id >= host_data->num_sps)
    {
        // The requested local SP is beyond the number of SP associated to the host
        // and involved in the group. This is an error.
        return DO_ERROR;
    }
    sp_data_ptr = DYN_ARRAY_GET_ELT(&(host_data->sps), local_host_sp_id, sp_cache_data_t *);
    assert(sp_data_ptr);
    *num_ranks = (*sp_data_ptr)->n_ranks;
    return DO_SUCCESS;
}

dpu_offload_status_t
get_num_ranks_for_group_host_idx(offloading_engine_t *engine,
                                 group_uid_t group_uid,
                                 size_t host_idx,
                                 size_t *num_ranks)
{
    host_info_t **ptr = NULL;
    group_cache_t *gp_cache = NULL;
    host_cache_data_t *host_data = NULL;

    assert(engine);
    gp_cache = GET_GROUP_CACHE(&(engine->procs_cache), group_uid);
    assert(gp_cache);
    ptr = DYN_ARRAY_GET_ELT(&(gp_cache->hosts),
                            host_idx,
                            host_info_t *);
    assert(ptr);
    host_data = GET_GROUP_HOST_HASH_ENTRY(gp_cache, (*ptr)->uid);
    assert(host_data);
    *num_ranks = host_data->num_ranks;
    return DO_SUCCESS;
}

dpu_offload_status_t
get_rank_idx_by_group_host_idx(offloading_engine_t *engine,
                               group_uid_t group_uid,
                               size_t host_idx,
                               int64_t rank,
                               int64_t *idx)
{
    group_cache_t *gp_cache = NULL;
    host_info_t **host_info_ptr = NULL;
    host_cache_data_t *host_data = NULL;
    size_t i, rank_index = 0;

    assert(engine);
    gp_cache = GET_GROUP_CACHE(&(engine->procs_cache), group_uid);
    assert(gp_cache);
    host_info_ptr = DYN_ARRAY_GET_ELT(&(gp_cache->hosts), host_idx, host_info_t *);
    assert(host_info_ptr);
    host_data = GET_GROUP_HOST_HASH_ENTRY(gp_cache, (*host_info_ptr)->uid);
    assert(host_data);
    if (!GROUP_CACHE_BITSET_TEST(host_data->ranks_bitset, rank))
    {
        // The rank is not involved in the group and running on that host, error
        return DO_ERROR;
    }

    // From there we know the rank is on the host and involved in the rank, we just need
    // to find its index
    for (i = 0; i < host_data->num_ranks; i++)
    {
        if (i == rank)
            break;

        if (GROUP_CACHE_BITSET_TEST(host_data->ranks_bitset, i))
            rank_index++;
    }
    *idx = rank_index;
    return DO_SUCCESS;
}

dpu_offload_status_t
get_rank_idx_by_group_sp_id(offloading_engine_t *engine,
                            group_uid_t group_uid,
                            uint64_t sp_gp_gid,
                            int64_t rank,
                            size_t *rank_idx)
{
    return DO_SUCCESS;
}

dpu_offload_status_t
get_all_sps_by_group_host_idx(offloading_engine_t *engine,
                              group_uid_t group_uid,
                              size_t host_idx,
                              dyn_array_t **sps,
                              size_t *num_sps)
{
    group_cache_t *gp_cache = NULL;
    host_info_t **host_info_ptr = NULL;
    host_cache_data_t *host_data = NULL;

    assert(engine);
    gp_cache = GET_GROUP_CACHE(&(engine->procs_cache), group_uid);
    assert(gp_cache);
    host_info_ptr = DYN_ARRAY_GET_ELT(&(gp_cache->hosts), host_idx, host_info_t *);
    assert(host_info_ptr);
    host_data = GET_GROUP_HOST_HASH_ENTRY(gp_cache, (*host_info_ptr)->uid);
    assert(host_data);
    *sps = &(host_data->sps);
    *num_sps = host_data->num_sps;
    return DO_SUCCESS;
}

dpu_offload_status_t
get_all_hosts_by_group(offloading_engine_t *engine,
                       group_uid_t group_uid,
                       dyn_array_t **hosts,
                       size_t *num_hosts)
{
    group_cache_t *gp_cache = NULL;

    assert(engine);
    gp_cache = GET_GROUP_CACHE(&(engine->procs_cache), group_uid);
    assert(gp_cache);
    *hosts = &(gp_cache->hosts);
    *num_hosts = gp_cache->n_hosts;
    return DO_SUCCESS;
}

dpu_offload_status_t
get_all_ranks_by_group_sp_gid(offloading_engine_t *engine,
                              group_uid_t group_uid,
                              uint64_t sp_group_gid,
                              dyn_array_t **ranks,
                              size_t *num_ranks)
{
    group_cache_t *gp_cache = NULL;
    remote_service_proc_info_t **ptr = NULL;
    sp_cache_data_t *sp_data = NULL;

    assert(engine);
    gp_cache = GET_GROUP_CACHE(&(engine->procs_cache), group_uid);
    assert(gp_cache);
    ptr = DYN_ARRAY_GET_ELT(&(gp_cache->sps),
                            sp_group_gid,
                            remote_service_proc_info_t *);
    assert(ptr);
    sp_data = GET_GROUP_SP_HASH_ENTRY(gp_cache, (*ptr)->service_proc.global_id);
    assert(sp_data);
    *ranks = &(sp_data->ranks);
    *num_ranks = sp_data->n_ranks;
    return DO_SUCCESS;
}

dpu_offload_status_t
get_all_ranks_by_group_sp_lid(offloading_engine_t *engine,
                              group_uid_t group_uid,
                              size_t host_idx,
                              uint64_t sp_group_lid,
                              dyn_array_t **ranks,
                              size_t *num_ranks)
{
    group_cache_t *gp_cache = NULL;
    host_info_t **host_info_ptr = NULL;
    host_cache_data_t *host_data = NULL;
    sp_cache_data_t **sp_data_ptr = NULL;

    assert(engine);
    gp_cache = GET_GROUP_CACHE(&(engine->procs_cache), group_uid);
    assert(gp_cache);

    // Get the host data
    host_info_ptr = DYN_ARRAY_GET_ELT(&(gp_cache->hosts), host_idx, host_info_t *);
    assert(host_info_ptr);
    host_data = GET_GROUP_HOST_HASH_ENTRY(gp_cache, (*host_info_ptr)->uid);
    assert(host_data);

    // Get the SP's data
    sp_data_ptr = DYN_ARRAY_GET_ELT(&(host_data->sps), sp_group_lid, sp_cache_data_t *);
    assert(sp_data_ptr);
    *ranks = &((*sp_data_ptr)->ranks);
    *num_ranks = (*sp_data_ptr)->n_ranks;
    return DO_SUCCESS;
}

dpu_offload_status_t
get_nth_sp_by_group_host_idx(offloading_engine_t *engine,
                             group_uid_t group_uid,
                             size_t host_idx,
                             size_t n,
                             uint64_t *global_group_sp_id)
{
    group_cache_t *gp_cache = NULL;
    host_info_t **host_ptr = NULL;
    host_cache_data_t *host_data = NULL;
    sp_cache_data_t **sp_data_ptr = NULL;

    assert(engine);
    gp_cache = GET_GROUP_CACHE(&(engine->procs_cache), group_uid);
    assert(gp_cache);

    // Lookup the host's data
    host_ptr = DYN_ARRAY_GET_ELT(&(gp_cache->hosts),
                                 host_idx,
                                 host_info_t *);
    assert(host_ptr);
    host_data = GET_GROUP_HOST_HASH_ENTRY(gp_cache, (*host_ptr)->uid);
    if (n >= host_data->num_sps)
        return DO_ERROR;
    
    // Lookup the SP's data
    sp_data_ptr = DYN_ARRAY_GET_ELT(&(host_data->sps), n, sp_cache_data_t *);
    assert(sp_data_ptr);
    *global_group_sp_id = (*sp_data_ptr)->gid;
    return DO_SUCCESS;
}

static void
populate_sp_ranks(offloading_engine_t *engine, group_cache_t *gp_cache, sp_cache_data_t *sp_data)
{
    size_t i = 0, idx = 0;
    DYN_ARRAY_ALLOC(&(sp_data->ranks),
                    gp_cache->group_size,
                    peer_cache_entry_t *);
    assert(sp_data->n_ranks);
    while (idx < sp_data->n_ranks)
    {
        if (GROUP_CACHE_BITSET_TEST(sp_data->ranks_bitset, i))
        {
            peer_cache_entry_t *rank_info = NULL;
            peer_cache_entry_t **ptr = NULL;
            rank_info = GET_GROUP_RANK_CACHE_ENTRY(&(engine->procs_cache),
                                                   gp_cache->group_uid,
                                                   i,
                                                   gp_cache->group_size);
            assert(rank_info);
            ptr = DYN_ARRAY_GET_ELT(&(sp_data->ranks), idx, peer_cache_entry_t *);
            assert(ptr);
            (*ptr) = rank_info;
            idx++;
        }
        i++;
    }
    assert(idx == sp_data->n_ranks);
}

static void
populate_host_sps(group_cache_t *gp_cache, host_cache_data_t *host_data)
{
    size_t i = 0, idx = 0;
    DYN_ARRAY_ALLOC(&(host_data->sps),
                    gp_cache->group_size,
                    sp_cache_data_t *);
    while (idx < host_data->num_sps)
    {
        if (GROUP_CACHE_BITSET_TEST(host_data->sps_bitset, i))
        {
            sp_cache_data_t **ptr = NULL, *sp_info = NULL;
            sp_info = GET_GROUP_SP_HASH_ENTRY(gp_cache, i);
            assert(sp_info);
            ptr = DYN_ARRAY_GET_ELT(&(host_data->sps), idx, sp_cache_data_t *);
            assert(ptr);
            sp_info->lid = idx;
            (*ptr) = sp_info;
            idx++;
        }
        i++;
    }
    assert(idx == host_data->num_sps);
}

dpu_offload_status_t
populate_group_cache_lookup_table(offloading_engine_t *engine,
                                  group_cache_t *gp_cache)
{
    size_t i, idx = 0;

    assert(gp_cache);
    assert(group_cache_populated(engine, gp_cache->group_uid));

    DBG("Creating the contiguous and ordered list of SPs involved in the group");
    assert(gp_cache->n_sps);
    if (gp_cache->sp_array_initialized == false)
    {
        DYN_ARRAY_ALLOC(&(gp_cache->sps),
                        gp_cache->n_sps,
                        remote_service_proc_info_t *);
        gp_cache->sp_array_initialized = true;
    }
    i = 0;
    while (i < gp_cache->n_sps)
    {
        if (GROUP_CACHE_BITSET_TEST(gp_cache->sps_bitset, idx))
        {
            remote_service_proc_info_t *sp_data = NULL, **ptr = NULL;
            sp_data = DYN_ARRAY_GET_ELT(&(engine->service_procs),
                                        idx,
                                        remote_service_proc_info_t);
            assert(sp_data);
            ptr = DYN_ARRAY_GET_ELT(&(gp_cache->sps),
                                    i,
                                    remote_service_proc_info_t *);
            *ptr = sp_data;
            i++;
        }
        idx++;
    }

    DBG("Creating the contiguous and ordered list of ranks associated with each SP");
    assert(kh_size(gp_cache->sps_hash) == gp_cache->n_sps);
    if (kh_size(gp_cache->sps_hash) != 0)
    {
        uint64_t sp_key;
        sp_cache_data_t *sp_value = NULL;
        kh_foreach(gp_cache->sps_hash, sp_key, sp_value, {
            populate_sp_ranks(engine, gp_cache, sp_value);
        })
    }

    DBG("Creating the contiguous and ordered list of hosts involved in the group");
    if (gp_cache->host_array_initialized == false)
    {
        DYN_ARRAY_ALLOC(&(gp_cache->hosts),
                        gp_cache->n_hosts,
                        host_info_t *);
        gp_cache->host_array_initialized = true;
    }
    i = 0;
    idx = 0;
    while (i < gp_cache->n_hosts)
    {
        if (GROUP_CACHE_BITSET_TEST(gp_cache->hosts_bitset, idx))
        {
            host_info_t *info = NULL, **ptr = NULL;
            info = DYN_ARRAY_GET_ELT(&(engine->config->hosts_config),
                                     idx,
                                     host_info_t);
            assert(info);
            ptr = DYN_ARRAY_GET_ELT(&(gp_cache->hosts),
                                    i,
                                    host_info_t *);
            *ptr = info;
            i++;
        }
        idx++;
    }

    DBG("Handling data of SPs in the context of hosts");
    if (kh_size(gp_cache->hosts_hash) != 0)
    {
        uint64_t host_key;
        host_cache_data_t *host_value = NULL;
        kh_foreach(gp_cache->hosts_hash, host_key, host_value, {
            populate_host_sps(gp_cache, host_value);
        })
    }

    return DO_SUCCESS;
}

dpu_offload_status_t
update_topology_data(offloading_engine_t *engine, group_cache_t *gp_cache, int64_t group_rank, uint64_t sp_gid, host_uid_t host_uid)
{

    sp_cache_data_t *sp_data = NULL;
    host_cache_data_t *host_data = NULL;

    assert(engine);

    // SPs have a unique ID, are all known, as well as the host associated to them.
    // So when we receive a cache entry, we look the SP of the cache entry and update
    // a SP lookup table so we can track which SPs are involved in the group.
    // As mentioned, knowing which SPs are involed in the group also allows us to track
    // which hosts are involved in the group. Note that it is difficult to directly
    // track which hosts are involved because host are represented via a hash and
    // it is therefore difficult to keep an ordered list of hosts that is consistent
    // everywhere.

    // Check if the SP is already in the group SP hash; if not, it means it is first
    // time we learn about that SP in the group so we increment the number of SPs
    // involved in the group
    sp_data = GET_GROUP_SP_HASH_ENTRY(gp_cache, sp_gid);
    if (sp_data == NULL)
    {
        // SP is not in the hash, we start by updating some bookkeeping variables
        DBG("group cache does not have SP %" PRIu64 ", adding SP to hash for the group (0x%x)",
            sp_gid, gp_cache->group_uid);
        gp_cache->n_sps++;
        // Add the SP to the hash using the global SP id as key
        DYN_LIST_GET(engine->free_sp_cache_hash_obj,
                     sp_cache_data_t,
                     item,
                     sp_data);
        RESET_SP_CACHE_DATA(sp_data);
        GROUP_CACHE_BITSET_CREATE(sp_data->ranks_bitset, gp_cache->group_size);
        sp_data->gid = sp_gid;
        sp_data->n_ranks = 1;
        sp_data->gp_uid = gp_cache->group_uid;
        sp_data->host_uid = host_uid;
        ADD_GROUP_SP_HASH_ENTRY(gp_cache, sp_data);
        GROUP_CACHE_BITSET_SET(gp_cache->sps_bitset, sp_gid);
    }
    else
    {
        // The SP is already in the hash
        sp_data->n_ranks++;
        DBG("cache entry has SP %" PRIu64 ", updating SP hash for the group (0x%x), # of ranks = %ld",
            sp_gid, gp_cache->group_uid, sp_data->n_ranks);
    }
    // Make the rank as associated to the SP
    GROUP_CACHE_BITSET_SET(sp_data->ranks_bitset, group_rank);

    // Same idea for the host
    host_data = GET_GROUP_HOST_HASH_ENTRY(gp_cache, host_uid);
    if (host_data == NULL)
    {
        // The host is not in the hash yet
        host_info_t *host_info = NULL;
        DBG("group cache does not have host 0x%lx, adding host to hash for the group (0x%x)",
            host_uid, gp_cache->group_uid);
        gp_cache->n_hosts++;
        // Add the SP to the hash using the global SP id as key
        assert(engine->free_host_cache_hash_obj);
        DYN_LIST_GET(engine->free_host_cache_hash_obj,
                     host_cache_data_t,
                    item,
                    host_data);
        assert(host_data);
        RESET_HOST_CACHE_DATA(host_data);
        host_data->uid = host_uid;
        host_data->num_ranks = 1;
        host_data->num_sps = 1;
        GROUP_CACHE_BITSET_CREATE(host_data->sps_bitset, gp_cache->group_size);
        GROUP_CACHE_BITSET_SET(host_data->sps_bitset, sp_gid);
        GROUP_CACHE_BITSET_CREATE(host_data->ranks_bitset, gp_cache->group_size);
        ADD_GROUP_HOST_HASH_ENTRY(gp_cache, host_data);
        host_info = LOOKUP_HOST_CONFIG(engine, host_uid);
        assert(host_info);
        host_data->config_idx = host_info->idx;
        assert(gp_cache->hosts_bitset);
        GROUP_CACHE_BITSET_SET(gp_cache->hosts_bitset,
                               host_info->idx);
    }
    else
    {
        // The host is already in the hash
        host_data->num_ranks++;
        if (!GROUP_CACHE_BITSET_TEST(host_data->sps_bitset, sp_gid))
        {
            // The SP is not known yet as being involved in the group
            host_data->num_sps++;
            GROUP_CACHE_BITSET_SET(host_data->sps_bitset, sp_gid);
        }
    }
    // Mark the rank as being part of the group and running on the host
    GROUP_CACHE_BITSET_SET(host_data->ranks_bitset, group_rank);

    return DO_SUCCESS;
}
