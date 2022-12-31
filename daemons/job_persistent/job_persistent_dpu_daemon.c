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

/** 
 * @brief This is an example of a service process that can be executed on DPUs
 * and let processes on the hosts connect. Please refer to the documentation 
 * [RUN.md](../../RUN.md) for details about how to start a daemon on DPUs.
 */

int main(int argc, char **argv)
{
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
    offloading_config_t config_data;
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

    /* Wait for the DPUs to connect to each other */
    while (!all_service_procs_connected(offload_engine))
        offload_engine_progress(offload_engine);

    /*
     * CREATE A SERVER SO THAT PROCESSES RUNNING ON THE HOST CAN CONNECT.
     */
    fprintf(stderr, "Creating server for processes on the DPU\n");
    // We let the system figure out the configuration to use to let ranks connect
    execution_context_t *service_server = server_init(offload_engine, &(config_data.local_service_proc.host_init_params));
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
        lib_progress(service_server);
    }

    fprintf(stderr, "%s: server done, finalizing...\n", argv[0]);

    offload_engine_fini(&offload_engine);
    fprintf(stderr, "client all done, exiting successfully\n");

    return EXIT_SUCCESS;
}