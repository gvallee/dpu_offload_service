//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <stdlib.h>
#include <unistd.h>

#include "dpu_offload_service_daemon.h"
#include "dpu_offload_group_cache.h"
#include "test_cache_common.h"

/*
 * This test is meant to be used in conjunction with the daemons/job_persistent/job_persistent_dpu_daemon
 * daemon running on DPUs. It is assmued that a configuration file is being used through the OFFLOAD_CONFIG_FILE_PATH
 * environment variable.
 *
 * The intent of the test is the following:
 * - start the daemon on the DPU(s) (see documentation)
 * - start at least one client, which the associated DPU daemons will track
 * - both clients request the endpoint for the other client
 * - cache entries should end up being exchanged
 * - the clients should ultimately get the endpoints.
 * Note that it should not matter how many DPU daemons are used (one or more) and
 * whether the ranks are running on the same host.
 *
 * Please provide the rank on the command line to start the test. Each process must have a unique rank.
 *      ./cache_job_client <TOTAL NUMBER OF RANKS> <TOTAL NUMBER OF RANKS ON HOST> <RANK>
 */

enum {
    BIN_NAME_ARG = 0,
    TOTAL_NUM_RANKS_ARG,
    TOTAL_NUM_LOCAL_RANKS_ARG,
    MY_RANK_ARG,
};

const int num_args = 3;

int main(int argc, char **argv)
{
    offloading_engine_t *offload_engine = NULL;
    execution_context_t *client = NULL;
    uint64_t group_size = 0, local_ranks = 0;
    int my_rank;

    /* Get the rank for the arguments */
    if (argc != num_args + 1)
    {
        fprintf(stderr, "the test requires exactly two arguments, please update your command:\n");
        fprintf(stderr, "\t%s <TOTAL NUMBER OF RANKS> <RANK>\n", argv[0]);
        return EXIT_FAILURE;
    }

    group_size = atoi(argv[TOTAL_NUM_RANKS_ARG]);
    local_ranks = atoi(argv[TOTAL_NUM_LOCAL_RANKS_ARG]);
    my_rank = atoi(argv[MY_RANK_ARG]);

    fprintf(stdout, "Starting test with the following data:\n");
    fprintf(stdout, "\tMy rank: %d\n", my_rank);
    fprintf(stdout, "\tGroup size: %" PRId64 "\n", group_size);
    fprintf(stdout, "\tLocal ranks: %" PRId64 "\n", local_ranks);

    /* Get the configuration */
    offloading_config_t config_data;
    INIT_DPU_CONFIG_DATA(&config_data);
    dpu_offload_status_t rc = get_host_config(&config_data);
    if (rc != DO_SUCCESS)
    {
        fprintf(stderr, "get_host_config() failed\n");
        return EXIT_FAILURE;
    }
    assert(config_data.num_dpus > 0);
    assert(config_data.num_service_procs_per_dpu > 0);

    /* Initialize everything we need for the test */
    rc = offload_engine_init(&offload_engine);
    if (rc || offload_engine == NULL)
    {
        fprintf(stderr, "offload_engine_init() failed\n");
        goto error_out;
    }
    config_data.offloading_engine = offload_engine;
    offload_engine->config = &config_data;

    dpu_config_data_t *dpu_config;
    dpu_config = config_data.dpus_config.base;
    int *connect_port = DYN_ARRAY_GET_ELT(&(dpu_config[0].version_1.host_ports), 0, int);
    fprintf(stderr, "INFO: connecting to DPU %s:%d\n", dpu_config[0].version_1.addr, *connect_port);

    group_uid_t guid = 42;
    rank_info_t my_rank_info = {
        .group_uid = guid,
        .group_rank = my_rank,
        .group_size = group_size,
        .n_local_ranks = local_ranks,
    };
    init_params_t init_params;
    conn_params_t conn_params;
    RESET_INIT_PARAMS(&init_params);
    RESET_CONN_PARAMS(&conn_params);
    init_params.conn_params = &conn_params;
    init_params.proc_info = &my_rank_info;

    rc = get_local_service_proc_connect_info(&config_data, &my_rank_info, &init_params);
    if (rc)
    {
        fprintf(stderr, "ERROR: get_local_service_proc_connect_info() failed\n");
        goto error_out;
    }
    assert(init_params.num_sps > 0);

    client = client_init(offload_engine, &init_params);
    if (client == NULL)
    {
        fprintf(stderr, "client handle is undefined\n");
        return EXIT_FAILURE;
    }
    ADD_CLIENT_TO_ENGINE(client, offload_engine);

    // Wait for the connection to the DPU to complete
    // Avoid timing issues when a single rank is being used
    while (GET_ECONTEXT_BOOTSTRAPING_PHASE(client) != BOOTSTRAP_DONE)
    {
        offload_engine_progress(offload_engine);
    }

    // Wait for the cache to be populated
    while (!group_cache_populated(offload_engine, guid))
    {
        offload_engine_progress(offload_engine);
    }

    assert(client->type == CONTEXT_CLIENT);

    fprintf(stdout, "\n***********\n");
    fprintf(stdout, "Cache data:\n");
    display_group_cache(&(offload_engine->procs_cache), guid);
    fprintf(stdout, "***********\n");

    int64_t target = (my_rank + 1) % group_size;

    dpu_offload_event_t *ev;
    int64_t shadow_dpu_id;
    rc = get_sp_id_by_group_rank(offload_engine, guid, target, 0, &shadow_dpu_id, &ev);
    if (rc != DO_SUCCESS)
    {
        fprintf(stderr, "get_dpu_id_by_host_rank() failed\n");
        goto error_out;
    }

    if (ev != NULL)
    {
        while (!event_completed(ev))
            client->progress(client);

        rc = event_return(&ev);
        if (rc != DO_SUCCESS)
        {
            fprintf(stderr, "event_return() failed\n");
            goto error_out;
        }

        rc = get_sp_id_by_group_rank(offload_engine, guid, target, 0, &shadow_dpu_id, &ev);
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

    ucp_ep_h target_dpu_ep;
    execution_context_t *comm_econtext;
    uint64_t notif_dest_id;
    rc = get_sp_ep_by_id(offload_engine, shadow_dpu_id, &target_dpu_ep, &comm_econtext, &notif_dest_id);
    if (rc)
    {
        fprintf(stderr, "get_dpu_ep_by_id() failed\n");
        goto error_out;
    }
    if (target_dpu_ep == NULL)
    {
        fprintf(stderr, "shadow DPU endpoint is undefined\n");
        goto error_out;
    }
    if (comm_econtext == NULL)
    {
        fprintf(stderr, "econtext is undefined\n");
        goto error_out;
    }

    // client finalized during offload_engine_fini()
    offload_engine_fini(&offload_engine);
    fprintf(stdout, "%s: test successful\n", argv[0]);
    return EXIT_SUCCESS;

error_out:
    // client finalized during offload_engine_fini()
    if (offload_engine != NULL)
        offload_engine_fini(&offload_engine);
    fprintf(stderr, "%s: test failed\n", argv[0]);
    return EXIT_FAILURE;
}
