#define _POSIX_C_SOURCE 200809L

//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

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
     * INITIATE CONNECTION BETWEEN DPUS.
     */
    fprintf(stderr, "Getting configuration...\n");
    char *list_dpus = getenv(LIST_DPUS_ENVVAR);
    if (list_dpus == NULL)
    {
        fprintf(stderr, "Unable to get list of DPUs via %s environmnent variable\n", LIST_DPUS_ENVVAR);
        return EXIT_FAILURE;
    }

    init_params_t init_params;
    conn_params_t server_params;
    init_params.worker = NULL;
    init_params.proc_info = NULL;
    char *port_str = getenv(INTER_DPU_PORT_ENVVAR);
    server_params.addr_str = getenv(INTER_DPU_ADDR_ENVVAR);
    if (server_params.addr_str == NULL)
    {
        fprintf(stderr, "%s is not set, please set it\n", INTER_DPU_ADDR_ENVVAR);
        return EXIT_FAILURE;
    }
    server_params.port = DEFAULT_INTER_DPU_CONNECT_PORT;
    init_params.conn_params = &server_params;
    if (port_str)
        server_params.port = atoi(port_str);

    fprintf(stderr, "Getting hostname...\n");
    char hostname[1024];
    hostname[1023] = '\0';
    gethostname(hostname, 1023);
    fprintf(stderr, "hostname=%s\n", hostname);

    fprintf(stderr, "Initiating connections between DPUs\n");
    rc = inter_dpus_connect_mgr(offload_engine, list_dpus, hostname, &init_params);
    if (rc)
    {
        fprintf(stderr, "inter_dpus_connect_mgr() failed\n");
        return EXIT_FAILURE;
    }

    /*
     * CREATE A SERVER SO THAT PROCESSES RUNNING ON THE HOST CAN CONNECT.
     */
    fprintf(stderr, "Creating server for processes on the DPU\n");
    // We let the system figure out the configuration to use to let ranks connect
    execution_context_t *service_server = server_init(offload_engine, NULL);
    if (service_server == NULL)
    {
        fprintf(stderr, "service_server is undefined\n");
        return EXIT_FAILURE;
    }
    fprintf(stderr, "server for application processes to connect has been successfully created\n");

    /*
     * PROGRESS UNTIL ALL PROCESSES ON THE HOST SEND A TERMINATION MESSAGE
     */
    fprintf(stderr, "%s: progressing...\n", argv[0]);
    while (!EXECUTION_CONTEXT_DONE(service_server))
    {
        service_server->progress(service_server);
    }

    fprintf(stderr, "%s: server done, finalizing...\n", argv[0]);

    offload_engine_fini(&offload_engine);
    fprintf(stderr, "client all done, exiting successfully\n");

    return EXIT_SUCCESS;
}