//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <stdlib.h>
#include <stdio.h>

#include "dpu_offload_service_daemon.h"

/* This daemon simulate the execution of a single daemon on a single DPU.
   This is mainly used for testing purposes. */

// single <ip> <port>
int main(int argc, char **argv)
{
    ucp_params_t ucp_params;
    ucp_worker_params_t worker_params;
    ucs_status_t status;
    ucp_config_t *config;
    uint64_t my_dpu_id;
    offloading_config_t dpu_cfg;
    offloading_engine_t *engine = NULL;
    execution_context_t *service_server = NULL;
    init_params_t service_server_init_params;
    conn_params_t service_server_conn_params;

    if (argc != 3)
    {
        fprintf(stderr, "Please give in order:\n");
        fprintf(stderr, "\t- the IP address to use,\n");
        fprintf(stderr, "\t- the port to use.\n");
        return EXIT_FAILURE;
    }

    offload_engine_init(&engine);
    assert(engine);

    // Manually initialize some aspects of the DPU since it is not a fully functional one
    engine->on_dpu = true;
    engine->num_dpus = 1;

    /* NO INTER-DPUS CONNECTIONS */

    /*
     * CREATE A SERVER SO THAT PROCESSES RUNNING ON THE HOST CAN CONNECT.
     */
    fprintf(stderr, "Creating server for processes on the DPU\n");
    // We let the system figure out the configuration to use to let ranks connect
    RESET_INIT_PARAMS(&service_server_init_params);
    RESET_CONN_PARAMS(&service_server_conn_params);
    service_server_init_params.conn_params = &service_server_conn_params;
    service_server_init_params.conn_params->addr_str = argv[1];
    service_server_init_params.conn_params->port_str = argv[2];
    service_server_init_params.conn_params->port = atoi(argv[2]);
    SET_DEFAULT_DPU_HOST_SERVER_CALLBACKS(&service_server_init_params);
    service_server = server_init(engine, &(service_server_init_params));
    if (service_server == NULL)
    {
        fprintf(stderr, "service_server is undefined\n");
        return EXIT_FAILURE;
    }
    ADD_SERVER_TO_ENGINE(service_server, engine);
    fprintf(stderr, "server for application processes to connect has been successfully created\n");

    /*
     * PROGRESS UNTIL ALL PROCESSES ON THE HOST SEND A TERMINATION MESSAGE
     */
    fprintf(stderr, "%s: progressing...\n", argv[0]);
    while (!EXECUTION_CONTEXT_DONE(service_server))
    {
        lib_progress(service_server);
    }

    // Finalizing
    server_fini(&service_server);
    offload_engine_fini(&engine);

    return EXIT_SUCCESS;
error_out:
    if (service_server)
        server_fini(&service_server);
    if (engine)
        offload_engine_fini(&engine);
    return EXIT_FAILURE;
}