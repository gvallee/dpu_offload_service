#define _POSIX_C_SOURCE 200809L

//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <stdlib.h>
#include <stdio.h>

#include "dpu_offload_service_daemon.h"
#include "dpu_offload_envvars.h"

static inline bool req_completed(struct ucx_context *req)
{
    if (req == NULL)
        return true;

    ucs_status_t status = ucp_request_check_status(req);
    if (status == UCS_INPROGRESS)
        return false;
    return true;
}

static void recv_cb(void *request, ucs_status_t status, ucp_tag_recv_info_t *info)
{
    fprintf(stderr, "pong successfully received\n");
}

void send_cb(void *request, ucs_status_t status)
{
    fprintf(stderr, "ping msg from client successfully sent\n");
}

int main(int argc, char **argv)
{
    /*
     * BOOTSTRAPPING: WE CREATE A CLIENT THAT CONNECT TO THE INITIATOR ON THE HOST
     * AND INITIALIZE THE OFFLOADING SERVICE.
     */
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
    char *list_dpus = getenv(LIST_DPUS_ENVVAR);
    if (list_dpus == NULL)
    {
        fprintf(stderr, "Unable to get list of DPUs via %s environmnent variable\n", LIST_DPUS_ENVVAR);
        return EXIT_FAILURE;
    }

    char hostname[1024];
    hostname[1023] = '\0';
    gethostname(hostname, 1023);

    rc = inter_dpus_connect_mgr(offload_engine, list_dpus, hostname);
    if (rc)
    {
        fprintf(stderr, "inter_dpus_connect_mgr() failed\n");
        return EXIT_FAILURE;
    }

    /*
     * CREATE A SERVER SO THAT PROCESSES RUNNING ON THE HOST CAN CONNECT.
     */

    conn_params_t server_params;
    server_params.addr_str = NULL; // fixme
    server_params.port = 11111; // fixme
    execution_context_t *service_server = server_init(offload_engine, &server_params);

    /*
     * PROGRESS UNTIL ALL PROCESSES ON THE HOST SEND A TERMINATION MESSAGE
     */

    while (!service_server->server->done)
    {
        ucp_worker_progress(service_server->server->ucp_worker);
    }

    offload_engine_fini(&offload_engine);
    fprintf(stderr, "client all done, exiting successfully\n");

    return EXIT_SUCCESS;
}