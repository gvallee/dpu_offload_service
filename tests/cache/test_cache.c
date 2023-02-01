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

enum {
    CACHE_POPULATION_GROUP_CACHE_ID=42,
    DUMMY_CACHE_ENTRY_EXCHANGE_GROUP_UID,
};

#define NUM_FAKE_DPU_PER_HOST (1)
#define NUM_FAKE_SP_PER_DPU (2)
#define NUM_FAKE_HOSTS (2)
#define NUM_FAKE_RANKS_PER_SP (8)
#define NUM_FAKE_SPS (NUM_FAKE_HOSTS * NUM_FAKE_DPU_PER_HOST * NUM_FAKE_SP_PER_DPU)
#define NUM_FAKE_CACHE_ENTRIES (NUM_FAKE_SPS * NUM_FAKE_RANKS_PER_SP)

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

    assert(engine);

    // Create the dummy cache entries
    for (i = 0; i < NUM_FAKE_CACHE_ENTRIES; i++)
    {
        host_info_t host_id = 1234;
        if (i >= NUM_FAKE_CACHE_ENTRIES / 2)
            host_id++;
        entries[i].set = true;
        entries[i].peer.proc_info.group_uid = DUMMY_CACHE_ENTRY_EXCHANGE_GROUP_UID;
        entries[i].peer.proc_info.group_rank = i;
        entries[i].peer.proc_info.group_size = NUM_FAKE_CACHE_ENTRIES;
        entries[i].peer.proc_info.n_local_ranks = NUM_FAKE_RANKS_PER_SP;
        entries[i].peer.proc_info.local_rank = NUM_FAKE_RANKS_PER_SP;
        entries[i].peer.proc_info.host_info = host_id;
        entries[i].peer.host_info = host_id;
        entries[i].peer.addr_len = 8;
        strcpy(entries[i].peer.addr, "deadbeef");
        entries[i].client_id = 0;
        entries[i].ep = NULL;
        entries[i].num_shadow_service_procs = 1;
        entries[i].shadow_service_procs[0] = i % NUM_FAKE_SPS;
        entries[i].events_initialized = false;
    }

    for (i = 0; i < NUM_FAKE_CACHE_ENTRIES; i++)
    {
        dpu_offload_event_t *ev = NULL;
        dpu_offload_status_t rc;
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

    offload_engine_fini(&offload_engine);

    fprintf(stdout, "%s: test successful\n", argv[0]);
    return EXIT_SUCCESS;

error_out:
    offload_engine_fini(&offload_engine);
    fprintf(stderr, "%s: test failed\n", argv[0]);
    return EXIT_FAILURE;
}