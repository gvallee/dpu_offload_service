//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <stdlib.h>
#include <unistd.h>

#include "dpu_offload_service_daemon.h"
#include "test_cache_common.h"

/*
 * This test is meant to be used in conjunction with the daemons/job_persistent/job_persistent_dpu_daemon
 * daemon running on DPUs. It is assmued that a configuration file is being used through the OFFLOAD_CONFIG_FILE_PATH
 * environment variable.
 * 
 * The intent of the test is the following:
 * - start the daemon on the DPU(s) (see documentation)
 * - start 2 clients, which the associated DPU daemons will track
 * - both clients request the endpoint for the other client
 * - cache entries should end up being exchanged
 * - the clients should ultimately get the endpoints.
 * Note that it should not matter how many DPU daemons are used (one or more) and
 * whether the ranks are running on the same host.
 * 
 * Please provide the rank on the command line to start the test. Each process must have a unique rank.
 *      ./cache_job_client <RANK>
 */

int main(int argc, char **argv)
{
    /* Get the rank for the arguments */
    if (argc != 2)
    {
        fprintf(stderr, "the test requires exactly two arguments, please update your command:\n");
        fprintf(stderr, "\t%s <RANK>\n", argv[0]);
        return EXIT_FAILURE;
    }
    int my_rank = atoi(argv[1]);

    /* Get the configuration */
    dpu_config_t config_data;
    dpu_offload_status_t rc = get_host_config(&config_data);
    if (rc != DO_SUCCESS)
    {
        fprintf(stderr, "get_host_config() failed\n");
        return EXIT_FAILURE;
    }

    /* Initialize everything we need for the test */
    offloading_engine_t *offload_engine;
    rc = offload_engine_init(&offload_engine);
    if (rc || offload_engine == NULL)
    {
        fprintf(stderr, "offload_engine_init() failed\n");
        goto error_out;
    }

    dpu_config_data_t *dpu_config;
    dpu_config = config_data.dpus_config.base;
    fprintf(stderr, "INFO: connecting to DPU %s:%d\n", dpu_config[0].version_1.addr, dpu_config[0].version_1.rank_port);

    rank_info_t my_rank_info = {
        .group_id = 0,
        .group_rank = my_rank,
    };
    init_params_t init_params = {
        .proc_info = &my_rank_info,
    };
    execution_context_t *client = client_init(offload_engine, &init_params);
    if (client == NULL)
    {
        fprintf(stderr, "client handle is undefined\n");
        return EXIT_FAILURE;
    }

    int64_t target = 1;
    if (my_rank == 1)
        target = 0;

    dpu_offload_event_t *ev;
    uint64_t shadow_dpu_id;
    rc = get_dpu_id_by_group_rank(client, 0, target, 0, &shadow_dpu_id, &ev);
    if (rc != DO_SUCCESS)
    {
        fprintf(stderr, "get_dpu_id_by_host_rank() failed\n");
        goto error_out;
    }

    if (ev != NULL)
    {
        while (!event_completed(client->event_channels, ev))
            client->progress(client);
        
        rc = event_return(client->event_channels, &ev);
        if (rc != DO_SUCCESS)
        {
            fprintf(stderr, "event_return() failed\n");
            goto error_out;
        }

        rc = get_dpu_id_by_group_rank(client, 0, target, 0, &shadow_dpu_id, &ev);
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

    ucp_ep_h target_dpu_ep = get_dpu_ep_by_id(client, shadow_dpu_id);
    if (target_dpu_ep == NULL)
    {
        fprintf(stderr, "shadow DPU endpoint is undefined\n");
        goto error_out;
    }
    
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