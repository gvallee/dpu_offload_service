//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

/*
 * The test fake_mpi_rank aims at testing the connection of multiple
 * ranks on a *single node*. The group/rank information is provided
 * via command line arguments. The test is not meant to be used with
 * mpirun, just start processes with the appropriate arguments.
 * The test is designed to be executed with the job_persistent_dpu_daemon.
 * Use more than 1 fake rank.
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

#include "dpu_offload_service_daemon.h"

// fake_mpi_rank <dpu_ip> <dpu_port> <lead_rank> <group_id> <group_size> <rank>
int main(int argc, char **argv)
{
    int rank;
    int lead_rank;
    int group_id;
    int group_size;
    char *dpu_addr;
    int dpu_port;
    dpu_offload_status_t rc;
    offloading_engine_t *engine = NULL;
    execution_context_t *client = NULL;
    init_params_t client_init_params;
    conn_params_t client_conn_params;
    rank_info_t client_rank_info;
    group_id_t group;

    if (argc != 7)
    {
        fprintf(stderr,
                "[ERROR] the test requires 6 arguments: %s <dpu_ip> <dpu_port> <lead_rank> <group_id> <group_size> <rank>\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    dpu_addr = argv[1];
    dpu_port = atoi(argv[2]);
    lead_rank = atoi(argv[3]);
    group_id = atoi(argv[4]);
    group_size = atoi(argv[5]);
    rank = atoi(argv[6]);

    group.id = group_id;
    group.lead = lead_rank;

    // Start the client for offloading
    rc = offload_engine_init(&engine);
    if (rc)
    {
        fprintf(stderr, "[ERROR] offload_engine_init() failed\n");
        goto error_out;
    }
    if (engine == NULL)
    {
        fprintf(stderr, "[ERRO] undefined engine\n");
        goto error_out;
    }

    RESET_INIT_PARAMS(&client_init_params);
    RESET_CONN_PARAMS(&client_conn_params);
    client_init_params.conn_params = &client_conn_params;
    client_init_params.proc_info = &client_rank_info;
    client_init_params.proc_info->group_id.lead = lead_rank;
    client_init_params.proc_info->group_id.id = group_id;
    client_init_params.proc_info->group_rank = rank;
    client_init_params.proc_info->group_size = group_size;
    client_init_params.proc_info->n_local_ranks = group_size;
    client_init_params.conn_params->addr_str = dpu_addr;
    client_init_params.conn_params->port_str = argv[2];
    client_init_params.conn_params->port = dpu_port;
    client = client_init(engine, &client_init_params);
    if (client == NULL)
    {
        fprintf(stderr, "[ERROR] client is undefined\n");
        goto error_out;
    }
    engine->client = client;

    // When only one rank is used, make sure the code ensures full initialization
    // otherwise, the cache is full right away and client terminates too quickly
    do
    {
        client->progress(client);
    } while (client->client->bootstrapping.phase != BOOTSTRAP_DONE);

    // Wait for full initialization to complete, i.e., until the group cache is fully populated
    while (!group_cache_populated(engine, group))
        offload_engine_progress(engine);
    fprintf(stdout, "Rank %d: connection to DPU succeeded\n", rank);

    // Finalizing
    client_fini(&client);
    offload_engine_fini(&engine);

    fprintf(stdout, "%s: test succeeded\n", argv[0]);
    return EXIT_SUCCESS;
error_out:
    if (client)
        client_fini(&client);
    if (engine)
        offload_engine_fini(&engine);
    fprintf(stderr, "%s: test failed\n", argv[0]);
    return EXIT_FAILURE;
}