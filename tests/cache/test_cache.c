//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <stdlib.h>
#include <stdint.h>

#include "dpu_offload_service_daemon.h"
#include "test_cache_common.h"
#include "dpu_offload_event_channels.h"

/*
 * To run the test, simply execute: $ ./test_cache
 */

enum
{
    CACHE_POPULATION_GROUP_CACHE_ID = 42,
    DUMMY_CACHE_ENTRY_EXCHANGE_GROUP_UID,
};

#define NUM_FAKE_DPU_PER_HOST (1)
#define NUM_FAKE_SP_PER_DPU (2)
#define NUM_FAKE_HOSTS (2)
#define NUM_FAKE_RANKS_PER_SP (8)
#define FIRST_FAKE_HOST_UID (1234)
#define NUM_FAKE_SPS (NUM_FAKE_HOSTS * NUM_FAKE_DPU_PER_HOST * NUM_FAKE_SP_PER_DPU)
#define NUM_FAKE_CACHE_ENTRIES (NUM_FAKE_SPS * NUM_FAKE_RANKS_PER_SP)

extern dpu_offload_status_t register_default_notifications(dpu_offload_ev_sys_t *);

// Based on 2 virtual hosts
static host_uid_t get_host_uid(size_t i)
{
    host_uid_t host_id = FIRST_FAKE_HOST_UID;
    if (i >= NUM_FAKE_CACHE_ENTRIES / 2)
        host_id += 1;
    return host_id;
}

// Based on 2 virtual hosts
static size_t get_host_idx(size_t i)
{
    if (i < NUM_FAKE_CACHE_ENTRIES / 2)
        return 0;
    return 1;
}

// Create a dummy engine configuration
static int create_dummy_config(offloading_engine_t *engine)
{
    size_t i;
    engine->config = malloc(sizeof(offloading_config_t));
    assert(engine->config);
    INIT_DPU_CONFIG_DATA(engine->config);
    for (i = 0; i < NUM_FAKE_HOSTS; i++)
    {
        int ret;
        host_info_t *host_info = NULL;
        khiter_t host_key;
        host_uid_t host_uid;

        host_info = DYN_ARRAY_GET_ELT(&(engine->config->hosts_config), i, host_info_t);
        assert(host_info);
        host_info->idx = i;
        host_info->hostname = strdup("dummy");
        host_uid = FIRST_FAKE_HOST_UID + i;
        host_info->uid = host_uid;
        host_key = kh_put(host_info_hash_t,
                          engine->config->host_lookup_table,
                          host_uid,
                          &ret);
        kh_value(engine->config->host_lookup_table, host_key) = host_info;
        engine->config->num_hosts++;
    }
    return 0;
}

/**
 * Function that generates a bunch of dummy cache entries that we
 * sent to self. It should properly populate the cache and let us
 * make sure all the cache internal are working as expected.
 */
dpu_offload_status_t
simulate_cache_entry_exchange(offloading_engine_t *engine)
{
    size_t i;
    peer_cache_entry_t entries[NUM_FAKE_CACHE_ENTRIES];
    group_cache_t *gp_cache = NULL;
    size_t num_hashed_sps = 0;
    dpu_offload_status_t rc;

    assert(engine);

    // the self execution context does not register the default event
    // handlers so we explicitly do so.
    rc = register_default_notifications(engine->self_econtext->event_channels);
    if (rc != DO_SUCCESS)
    {
        fprintf(stderr, "ERROR: register_default_notifications() failed\n");
        return DO_ERROR;
    }

    // Create the dummy SPs
    for (i = 0; i < NUM_FAKE_SPS; i++)
    {
        remote_service_proc_info_t *ptr = NULL;
        ptr = DYN_ARRAY_GET_ELT(&(engine->service_procs),
                                i,
                                remote_service_proc_info_t);
        ptr->offload_engine = engine;
        ptr->idx = i;
        ptr->service_proc.global_id = i;
        ptr->service_proc.local_id = i % NUM_FAKE_SP_PER_DPU;
    }
    engine->num_service_procs = 4;
    fprintf(stdout, "Number of fake SPs that are now setup: %ld\n", engine->num_service_procs);

    // Create the dummy cache entries
    fprintf(stdout, "Creating entries for %d fake ranks:\n", NUM_FAKE_CACHE_ENTRIES);
    for (i = 0; i < NUM_FAKE_CACHE_ENTRIES; i++)
    {
        host_uid_t host_uid = get_host_uid(i);
        size_t host_idx = get_host_idx(i);
        size_t num_host_sps = NUM_FAKE_DPU_PER_HOST * NUM_FAKE_SP_PER_DPU;
        entries[i].set = true;
        entries[i].peer.proc_info.group_uid = DUMMY_CACHE_ENTRY_EXCHANGE_GROUP_UID;
        entries[i].peer.proc_info.group_rank = i;
        entries[i].peer.proc_info.group_size = NUM_FAKE_CACHE_ENTRIES;
        entries[i].peer.proc_info.n_local_ranks = NUM_FAKE_RANKS_PER_SP;
        entries[i].peer.proc_info.local_rank = NUM_FAKE_RANKS_PER_SP;
        entries[i].peer.proc_info.host_info = host_uid;
        entries[i].peer.host_info = host_uid;
        entries[i].peer.addr_len = 8;
        strcpy(entries[i].peer.addr, "deadbeef");
        entries[i].client_id = 0;
        entries[i].ep = NULL;
        entries[i].num_shadow_service_procs = 1;
        entries[i].shadow_service_procs[0] = (host_idx * num_host_sps) + i % num_host_sps;
        fprintf(stdout, "\trank %ld is assigned to SP %ld\n", i, entries[i].shadow_service_procs[0]);
        entries[i].events_initialized = false;
    }

    for (i = 0; i < NUM_FAKE_CACHE_ENTRIES; i++)
    {
        dpu_offload_event_t *ev = NULL;
        rc = event_get(engine->self_econtext->event_channels, NULL, &ev);
        if (rc)
        {
            fprintf(stderr, "ERROR: event_get() failed\n");
            return DO_ERROR;
        }

        rc = event_channel_emit_with_payload(&ev,
                                             AM_PEER_CACHE_ENTRIES_MSG_ID,
                                             engine->self_ep,
                                             0, // dest_id does not matter since we send to ourselves
                                             NULL,
                                             entries,
                                             NUM_FAKE_CACHE_ENTRIES * sizeof(peer_cache_entry_t));
        if (rc)
        {
            fprintf(stderr, "ERROR: event_channel_emit_with_payload() failed\n");
            return DO_ERROR;
        }
    }

    gp_cache = GET_GROUP_CACHE(&(engine->procs_cache), DUMMY_CACHE_ENTRY_EXCHANGE_GROUP_UID);
    num_hashed_sps = kh_size(gp_cache->sps_hash);
    if (num_hashed_sps != NUM_FAKE_SPS)
    {
        fprintf(stderr, "ERROR: number of SPs in hash is %ld instead of %d\n",
                num_hashed_sps,
                NUM_FAKE_SPS);
        return DO_ERROR;
    }

    // For the rest of the test, we simulate being on the first host, first SP
    engine->on_dpu = true;
    engine->config->local_service_proc.info.global_id = 0;
    engine->config->local_service_proc.host_uid = FIRST_FAKE_HOST_UID;

    // Force the creation of the lookup tables, the context of this test does not
    // provide all the requirements for an automatic creation.
    rc = populate_group_cache_lookup_table(engine, gp_cache);
    if (rc != DO_SUCCESS)
    {
        fprintf(stderr, "ERROR: populate_group_cache_lookup_table() failed\n");
        return DO_ERROR;
    }

    // Check we have the correct number of ranks per SP when using the SP hash
    // First we do so by iterating over the global SP identifers since we know them
    for (i = 0; i < NUM_FAKE_SPS; i++)
    {
        sp_cache_data_t *sp_data = NULL;
        sp_data = GET_GROUP_SP_HASH_ENTRY(gp_cache, i);
        if (sp_data == NULL)
        {
            fprintf(stderr, "ERROR: unable to get data for SP #%ld\n", i);
            return DO_ERROR;
        }
        if (sp_data->n_ranks != NUM_FAKE_RANKS_PER_SP)
        {
            fprintf(stderr, "ERROR: the number of ranks associated to SP #%ld is reported as %ld instead of %d\n",
                    i, sp_data->n_ranks, NUM_FAKE_RANKS_PER_SP);
            return DO_ERROR;
        }
    }

    // Second, check the content of the contiguous ordered array of SPs involed in the group
    for (i = 0; i < gp_cache->n_sps; i++)
    {
        remote_service_proc_info_t **sp_info = NULL;
        sp_info = DYN_ARRAY_GET_ELT(&(gp_cache->sps),
                                    i,
                                    remote_service_proc_info_t *);
        if ((*sp_info)->idx != i)
        {
            fprintf(stderr, "ERROR: the index of SP %ld is reported as %ld\n",
                    i, (*sp_info)->idx);
            return DO_ERROR;
        }
        if ((*sp_info)->service_proc.global_id != i)
        {
            fprintf(stderr, "ERROR: the global SP ID for %ld is %ld instead of %ld\n",
                    i, (*sp_info)->service_proc.global_id, i);
            return DO_ERROR;
        }
    }
    // Display some information
    fprintf(stdout, "Number of SP(s) involved in the group: %ld\n", gp_cache->n_sps);
    for (i = 0; i < gp_cache->n_sps; i++)
    {
        remote_service_proc_info_t **sp_info = NULL;
        sp_info = DYN_ARRAY_GET_ELT(&(gp_cache->sps),
                                    i,
                                    remote_service_proc_info_t *);
        fprintf(stdout, "\tSP %" PRIu64 " is involved in the group\n", (*sp_info)->service_proc.global_id);
    }

    // Then we check the hosts involved in the group
    if (gp_cache->n_hosts != NUM_FAKE_HOSTS)
    {
        fprintf(stderr, "ERROR: the number of host in the group is reported as %ld instead of %d\n",
                gp_cache->n_hosts, NUM_FAKE_HOSTS);
        return DO_ERROR;
    }
    for (i = 0; i < gp_cache->n_hosts; i++)
    {
        if (!GROUP_CACHE_BITSET_TEST(gp_cache->hosts_bitset, i))
        {
            fprintf(stderr, "ERROR: bit %ld in hosts_bitset is not properly set\n", i);
            return DO_ERROR;
        }
    }
    // Display some information about the hosts
    fprintf(stdout, "\nNumber of host(s) involved in the group: %ld\n", gp_cache->n_hosts);
    for (i = 0; i < gp_cache->n_hosts; i++)
    {
        host_info_t **host_info = NULL;
        host_info = DYN_ARRAY_GET_ELT(&(gp_cache->hosts),
                                      i,
                                      host_info_t *);
        fprintf(stdout, "\t%s (index: %ld)\n", (*host_info)->hostname, (*host_info)->idx);
    }

    return DO_SUCCESS;
}

dpu_offload_status_t test_topo_api(offloading_engine_t *engine)
{
    group_uid_t gpuid = DUMMY_CACHE_ENTRY_EXCHANGE_GROUP_UID;
    uint64_t sp_id, target_sp_gp_guid = 0, sp_gp_lid, target_local_host_sp_id = 0;
    uint64_t target_gp_gp_lid = 0, global_group_sp_id;
    int64_t target_rank = 0;
    size_t i, host_idx, num_sps, num_ranks, target_host_idx = 0, rank_idx, num_hosts;
    dyn_array_t *sps = NULL, *hosts = NULL, *ranks = NULL;
    dpu_offload_status_t rc;
    size_t expected_number_of_sps_per_host = NUM_FAKE_DPU_PER_HOST * NUM_FAKE_SP_PER_DPU;
    size_t expected_number_of_ranks_per_host = NUM_FAKE_DPU_PER_HOST * NUM_FAKE_SP_PER_DPU * NUM_FAKE_RANKS_PER_SP;

    assert(engine);
    fprintf(stdout, "Testing the topo API...\n");
    rc = get_global_sp_id_by_group(engine, gpuid, &sp_id);
    if (rc)
    {
        fprintf(stderr, "ERROR: get_global_sp_id_by_group() failed\n");
        return DO_ERROR;
    }
    fprintf(stdout, "-> Global SP ID is %" PRIu64 "\n", sp_id);
    if (sp_id != 0)
    {
        fprintf(stderr, "SP ID is %" PRIu64 " instead of 0\n", sp_id);
        return DO_ERROR;
    }

    rc = get_local_sp_id_by_group(engine, gpuid, target_sp_gp_guid, &sp_gp_lid);
    if (rc)
    {
        fprintf(stderr, "ERROR: get_local_sp_id_by_group() failed\n");
        return DO_ERROR;
    }
    fprintf(stdout, "-> SP group local ID: %" PRIu64 "\n", sp_gp_lid);
    if (sp_gp_lid != 0)
    {
        fprintf(stderr, "ERROR: SP group LID is invalid\n");
        return DO_ERROR;
    }

    rc = get_host_idx_by_group(engine, gpuid, &host_idx);
    if (rc)
    {
        fprintf(stderr, "ERROR: get_host_idx_by_group() failed\n");
        return DO_ERROR;
    }
    fprintf(stdout, "-> Host index: %ld\n", host_idx);
    if (host_idx != 0)
    {
        fprintf(stderr, "ERROR: invalid host index\n");
        return DO_ERROR;
    }

    rc = get_num_sps_by_group_host_idx(engine, gpuid, host_idx, &num_sps);
    if (rc)
    {
        fprintf(stderr, "ERROR: get_num_sps_by_group_host_idx() failed\n");
        return DO_ERROR;
    }
    fprintf(stderr, "-> Number of involved SP on the first host: %ld\n", num_sps);
    if (num_sps != expected_number_of_sps_per_host)
    {
        fprintf(stderr, "ERROR: number of SPs reported as %ld instead of %ld\n",
                num_sps, expected_number_of_sps_per_host);
        return DO_ERROR;
    }

    rc = get_num_ranks_for_group_sp(engine, gpuid, target_sp_gp_guid, &num_ranks);
    if (rc)
    {
        fprintf(stderr, "ERROR: get_num_ranks_for_group_sp() failed\n");
        return DO_ERROR;
    }
    fprintf(stdout, "-> Number of ranks associated to SP %ld: %ld\n", target_sp_gp_guid, num_ranks);
    if (num_ranks != NUM_FAKE_RANKS_PER_SP)
    {
        fprintf(stderr, "ERROR: number of ranks for SP %ld is reported %ld instead of %d\n",
                target_gp_gp_lid, num_ranks, NUM_FAKE_RANKS_PER_SP);
        return DO_ERROR;
    }

    rc = get_num_ranks_for_group_host_local_sp(engine, gpuid, target_host_idx, target_local_host_sp_id, &num_ranks);
    if (rc)
    {
        fprintf(stderr, "ERROR: get_num_ranks_for_group_host_local_sp() failed\n");
        return DO_ERROR;
    }
    fprintf(stdout, "-> Number of ranks for host at index %ld local SP %ld: %ld\n",
            target_host_idx, target_local_host_sp_id, num_ranks);
    if (num_ranks != NUM_FAKE_RANKS_PER_SP)
    {
        fprintf(stderr, "ERROR: number of ranks for host at index %ld and local SP %ld is %ld instead of %d\n",
                target_host_idx, target_local_host_sp_id, num_ranks, NUM_FAKE_RANKS_PER_SP);
        return DO_ERROR;
    }

    rc = get_num_ranks_for_group_host_idx(engine, gpuid, host_idx, &num_ranks);
    if (rc)
    {
        fprintf(stderr, "ERROR: get_num_ranks_for_group_host_idx() failed\n");
        return DO_ERROR;
    }
    fprintf(stdout, "-> Number of ranks associated with host at index %ld: %ld\n",
            host_idx, num_ranks);
    if (num_ranks != expected_number_of_ranks_per_host)
    {
        fprintf(stderr, "ERROR: number of ranks associated with host at index %ld is %ld instead of %ld\n",
                host_idx, num_ranks, expected_number_of_ranks_per_host);
        return DO_ERROR;
    }

    rc = get_rank_idx_by_group_host_idx(engine, gpuid, host_idx, target_rank, &rank_idx);
    if (rc)
    {
        fprintf(stderr, "ERROR: get_rank_idx_by_group_host_idx() failed\n");
        return DO_ERROR;
    }
    fprintf(stdout, "-> Index of rank %" PRId64 " on host index %ld: %ld\n", target_rank, host_idx, rank_idx);
    if (rank_idx != 0)
    {
        fprintf(stderr, "ERROR: report index of rank %" PRId64 " is %ld instead of 0\n", target_rank, rank_idx);
        return DO_ERROR;
    }

    rc = get_all_sps_by_group_host_idx(engine, gpuid, host_idx, &sps, &num_sps);
    if (rc)
    {
        fprintf(stderr, "ERROR: get_all_sps_by_group_host_idx() failed\n");
        return DO_ERROR;
    }
    fprintf(stdout, "-> Number of SPs on host at index %ld: %ld\n", host_idx, num_sps);
    if (num_sps != expected_number_of_sps_per_host)
    {
        fprintf(stderr, "ERROR: number of SPs for host at index %ld is reported as %ld instead of %ld\n",
                host_idx, num_sps, expected_number_of_sps_per_host);
        return DO_ERROR;
    }
    fprintf(stdout, "-> SP(s) data:\n");
    for (i = 0; i < num_sps; i++)
    {
        sp_cache_data_t **ptr = NULL;
        ptr = DYN_ARRAY_GET_ELT(sps, i, sp_cache_data_t *);
        assert(ptr);
        fprintf(stdout, "\tGID: %" PRIu64 "; Group UID: 0x%x; Host UID: 0x%lx; LID: %" PRIu64 "; number of ranks: %ld\n",
                (*ptr)->gid, (*ptr)->gp_uid, (*ptr)->host_uid, (*ptr)->lid, (*ptr)->n_ranks);
    }

    rc = get_all_hosts_by_group(engine, gpuid, &hosts, &num_hosts);
    if (rc)
    {
        fprintf(stderr, "ERROR: get_all_hosts_by_group() failed\n");
        return DO_ERROR;
    }
    fprintf(stdout, "Number of hosts in group: %ld\n", num_hosts);
    if (num_hosts != NUM_FAKE_HOSTS)
    {
        fprintf(stderr, "ERROR: Number of hosts in the group is reported as %ld instead of %d\n",
                num_hosts, NUM_FAKE_HOSTS);
        return 0;
    }
    fprintf(stdout, "-> Host(s) data:\n");
    for (i = 0; i < num_hosts; i++)
    {
        host_info_t **host_data = NULL;
        host_data = DYN_ARRAY_GET_ELT(hosts, i, host_info_t *);
        assert(host_data);
        fprintf(stdout, "\tHostname: %s; index: %ld; UID: 0x%lx\n",
                (*host_data)->hostname, (*host_data)->idx, (*host_data)->uid);
    }

    rc = get_all_ranks_by_group_sp_gid(engine, gpuid, target_sp_gp_guid, &ranks, &num_ranks);
    if (rc)
    {
        fprintf(stderr, "ERROR: get_all_ranks_by_group_sp_gid() failed\n");
        return DO_ERROR;
    }
    fprintf(stdout, "-> Number of ranks associated to SP with group UID %" PRIu64 ": %ld\n",
            target_sp_gp_guid, num_ranks);
    if (num_ranks != NUM_FAKE_RANKS_PER_SP)
    {
        fprintf(stderr, "ERROR: number of ranks is reported as %ld instead of %d\n",
                num_ranks, NUM_FAKE_RANKS_PER_SP);
        return DO_ERROR;
    }
    fprintf(stdout, "-> Rank(s) data:\n");
    for (i = 0; i < num_ranks; i++)
    {
        peer_cache_entry_t **ptr = NULL;
        ptr = DYN_ARRAY_GET_ELT(ranks, i, peer_cache_entry_t *);
        assert(ptr);
        assert(*ptr);
        fprintf(stderr, "Rank %" PRId64 ": group UID=0x%x; host UID: 0x%lx\n",
                (*ptr)->peer.proc_info.group_rank, (*ptr)->peer.proc_info.group_uid, (*ptr)->peer.host_info);
    }

    rc = get_all_ranks_by_group_sp_lid(engine, gpuid, host_idx, target_gp_gp_lid, &ranks, &num_ranks);
    if (rc)
    {
        fprintf(stderr, "ERROR: get_all_ranks_by_group_sp_lid() failed\n");
        return DO_ERROR;
    }

    rc = get_nth_sp_by_group_host_idx(engine, gpuid, host_idx, 0, &global_group_sp_id);
    if (rc)
    {
        fprintf(stderr, "ERROR: get_nth_sp_by_group_host_idx() failed\n");
        return DO_ERROR;
    }

    return DO_SUCCESS;
}

int main(int argc, char **argv)
{
    /* Initialize everything we need for the test */
    offloading_engine_t *offload_engine;
    dpu_offload_status_t rc = offload_engine_init(&offload_engine);
    if (rc || offload_engine == NULL)
    {
        fprintf(stderr, "offload_engine_init() failed\n");
        goto error_out;
    }

    rc = create_dummy_config(offload_engine);
    if (rc)
    {
        fprintf(stderr, "ERROR: create_dummy_config() failed");
        goto error_out;
    }

    fprintf(stdout, "Populating cache...\n");
    POPULATE_CACHE(offload_engine, CACHE_POPULATION_GROUP_CACHE_ID);

    group_id_t target_group = {
        .lead = 41,
        .id = CACHE_POPULATION_GROUP_CACHE_ID,
    };
    display_group_cache(&(offload_engine->procs_cache), target_group);

    fprintf(stdout, "Checking cache...\n");
    CHECK_CACHE(offload_engine, CACHE_POPULATION_GROUP_CACHE_ID);

    fprintf(stdout, "Simulating cache entry exchanges between SPs...\n");
    rc = simulate_cache_entry_exchange(offload_engine);
    if (rc)
    {
        fprintf(stderr, "ERROR: simulate_cache_entry_exchange() failed\n");
        goto error_out;
    }

    rc = test_topo_api(offload_engine);
    if (rc)
    {
        fprintf(stderr, "ERROR: test_topo_api() failed\n");
        goto error_out;
    }

    free(offload_engine->config);
    offload_engine->config = NULL;
    offload_engine_fini(&offload_engine);

    fprintf(stdout, "%s: test successful\n", argv[0]);
    return EXIT_SUCCESS;

error_out:
    if (offload_engine->config)
    {
        free(offload_engine->config);
        offload_engine->config = NULL;
    }
    offload_engine_fini(&offload_engine);
    fprintf(stderr, "%s: test failed\n", argv[0]);
    return EXIT_FAILURE;
}