//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <stdlib.h>
#include <unistd.h>

#include "dpu_offload_service_daemon.h"
#include "test_cache_common.h"

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

    execution_context_t *client = client_init(offload_engine, NULL);
    if (client == NULL)
    {
        fprintf(stderr, "client handle is undefined\n");
        return EXIT_FAILURE;
    }

    ucp_ep_h remote_ep = client->client->server_ep;
    if (remote_ep == NULL)
    {
        fprintf(stderr, "undefined destination endpoint\n");
        goto error_out;
    }
    rc = exchange_cache(client, &(offload_engine->procs_cache), remote_ep);
    if (rc != DO_SUCCESS)
    {
        fprintf(stderr, "exchange_cache() failed\n");
        goto error_out;
    }

    /* Progress until the last element in the cache is set */
    fprintf(stderr, "Waiting for all the cache entries to arrive...\n");

    int retry = 0;
    bool test_done = false;
    while (!test_done)
    {
        client->progress(client);
        cache_t *cache = &(offload_engine->procs_cache);
        group_cache_t *gp_caches = (group_cache_t *)cache->data.base;
        if (gp_caches[42].initialized)
        {
            peer_cache_entry_t *list_ranks = (peer_cache_entry_t *)gp_caches[42].ranks.base;
            peer_data_t *target_peer = &(list_ranks[NUM_CACHE_ENTRIES - 1].peer);
            if (IS_A_VALID_PEER_DATA(target_peer))
                test_done = true;
        }
        sleep(1);
        retry++;
        if (retry == 5)
        {
            fprintf(stderr, "error: data still not received\n");
            break;
        }
    }

    /* Check we got all the expected data in the cache */
    CHECK_CACHE(offload_engine);

    client_fini(&client);
    offload_engine_fini(&offload_engine);
    fprintf(stdout, "%s: test successful\n", argv[0]);
    return EXIT_SUCCESS;

error_out:
    client_fini(&client);
    offload_engine_fini(&offload_engine);
    fprintf(stderr, "%s: test failed\n", argv[0]);
    return EXIT_FAILURE;
}