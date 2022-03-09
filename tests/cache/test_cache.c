//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <stdlib.h>

#include "dpu_offload_service_daemon.h"

#define NUM_CACHE_ENTRIES (DEFAULT_NUM_PEERS * 2)

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

    /* Create local cache entries */
    size_t i;
    cache_t *cache = &(offload_engine->procs_cache);
    for (i = 0; i < NUM_CACHE_ENTRIES; i++)
    {
        peer_cache_entry_t *new_entry;
        DYN_LIST_GET(offload_engine->free_peer_cache_entries, peer_cache_entry_t, item, new_entry);
        if (new_entry == NULL)
        {
            fprintf(stderr, "Unable to get cache entry #%ld\n", i);
            goto error_out;
        }

        new_entry->peer.proc_info.group_rank = i;
        new_entry->peer.proc_info.group_id = 42; // 42 because we can (avoid initialization to zero to be assumed all set)
        SET_PEER_CACHE_ENTRY(cache, new_entry);
    }

    if (cache->size != 1)
    {
        fprintf(stderr, "EP cache size is %ld instead of 1\n", cache->size);
        goto error_out;
    }

    group_cache_t *gp_caches = (group_cache_t *)offload_engine->procs_cache.data.base;
    group_cache_t *gp42 = &(gp_caches[42]);
    if (gp42->initialized == false)
    {
        fprintf(stderr, "target group is not marked as initialized");
        goto error_out;
    }

    peer_cache_entry_t *cache_entries = (peer_cache_entry_t *)gp42->ranks.base;
    for (i = 0; i < NUM_CACHE_ENTRIES; i++)
    {
        if (cache_entries[i].peer.proc_info.group_rank != i)
        {
            fprintf(stderr, "cache entry as rank %ld instead of %ld\n", cache_entries[i].peer.proc_info.group_rank, i);
            goto error_out;
        }

        if (cache_entries[i].peer.proc_info.group_id != 42)
        {
            fprintf(stderr, "cache entry as rank %ld instead of 42\n", cache_entries[i].peer.proc_info.group_id);
            goto error_out;
        }
    }

    offload_engine_fini(&offload_engine);
    fprintf(stdout, "%s: test successful\n", argv[0]);
    return EXIT_SUCCESS;

error_out:
    offload_engine_fini(&offload_engine);
    fprintf(stderr, "%s: test failed\n", argv[0]);
    return EXIT_FAILURE;
}