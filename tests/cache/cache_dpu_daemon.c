#define _POSIX_C_SOURCE 200809L

//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

/*
 * This test is meant to act as a daemon on two DPUs.
 *
 * The intent of the test is the following:
 * - start the daemon on two DPU(s) (see documentation)
 * - the second DPU will create a fake cache entry for a local rank
 * - the first DPU will request it
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>

#include "dpu_offload_service_daemon.h"
#include "dpu_offload_envvars.h"

int main(int argc, char **argv)
{
    /*
     * BOOTSTRAPPING: WE CREATE A CLIENT THAT CONNECT TO THE INITIATOR ON THE HOST
     * AND INITIALIZE THE OFFLOADING SERVICE.
     */
    fprintf(stderr, "Creating offload engine...\n");
    offloading_engine_t *offload_engine;
    dpu_offload_status_t rc = offload_engine_init(&offload_engine);
    if (rc || offload_engine == NULL)
    {
        fprintf(stderr, "offload_engine_init() failed\n");
        return EXIT_FAILURE;
    }

    /*
     * GET THE CONFIGURATION.
     */
    fprintf(stderr, "Getting configuration...\n");
    dpu_config_t config_data;
    INIT_DPU_CONFIG_DATA(&config_data);
    config_data.offloading_engine = offload_engine;
    int ret = get_dpu_config(offload_engine, &config_data);
    if (ret)
    {
        fprintf(stderr, "get_config() failed\n");
        return EXIT_FAILURE;
    }

    /*
     * INITIATE CONNECTION BETWEEN DPUS.
     */
    fprintf(stderr, "Initiating connections between DPUs\n");
    rc = inter_dpus_connect_mgr(offload_engine, &config_data);
    if (rc)
    {
        fprintf(stderr, "inter_dpus_connect_mgr() failed\n");
        return EXIT_FAILURE;
    }
    fprintf(stderr, "Connections between DPUs successfully initialized\n");

    fprintf(stderr, "I am DPU #%ld, starting the test\n", config_data.local_dpu.id);

    if (config_data.local_dpu.id == 1)
    {
        // Create a fake entry in the cache
        peer_cache_entry_t *new_entry;
        DYN_LIST_GET(offload_engine->free_peer_cache_entries, peer_cache_entry_t, item, new_entry);
        if (new_entry == NULL)
        {
            fprintf(stderr, "Unable to get cache entry\n");
            goto error_out;
        }

        // 42 is used to avoid lucky initialization effects that would hide a bug
        new_entry->peer.proc_info.group_rank = 42;
        new_entry->peer.proc_info.group_id = 42;
        new_entry->set = true;
        SET_PEER_CACHE_ENTRY(&(offload_engine->procs_cache), new_entry);

        fprintf(stderr, "Cache entry successfully created\n");
    }
    else
    {
        dpu_offload_event_t *ev;
        uint64_t shadow_dpu_id;
        execution_context_t *econtext = XXX;

        fprintf(stderr, "Looking up endpoint\n");
        rc = get_dpu_id_by_group_rank(offload_engine, 42, 42, 0, &shadow_dpu_id, &ev);
        if (rc != DO_SUCCESS)
        {
            fprintf(stderr, "get_dpu_id_by_host_rank() failed\n");
            goto error_out;
        }

        if (ev != NULL)
        {
            fprintf(stderr, "Waiting for look up to complete\n");
            while (!event_completed(econtext->event_channels, ev))
                econtext->progress(econtext);

            fprintf(stderr, "Look up completed\n");
            rc = event_return(econtext->event_channels, &ev);
            if (rc != DO_SUCCESS)
            {
                fprintf(stderr, "event_return() failed\n");
                goto error_out;
            }

            rc = get_dpu_id_by_group_rank(offload_engine, 42, 42, 0, &shadow_dpu_id, &ev);
            if (rc != DO_SUCCESS)
            {
                fprintf(stderr, "get_dpu_id_by_host_rank() failed\n");
                goto error_out;
            }
            if (ev != NULL)
            {
                fprintf(stderr, "cache entry still not available\n");
                goto error_out;
            }
        }

        ucp_ep_h target_dpu_ep = get_dpu_ep_by_id(offload_engine, shadow_dpu_id);
        if (target_dpu_ep == NULL)
        {
            fprintf(stderr, "shadow DPU endpoint is undefined\n");
            goto error_out;
        }
    }

    offload_engine_fini(&offload_engine);
    fprintf(stderr, "client all done, exiting successfully\n");

    return EXIT_SUCCESS;
error_out:
    offload_engine_fini(&offload_engine);
    fprintf(stderr, "%s: test failed\n", argv[0]);
    return EXIT_FAILURE;
}