#define _POSIX_C_SOURCE 200809L

//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <stdlib.h>
#include <stdio.h>

#include "dpu_offload_service_daemon.h"

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
    int rc = offload_engine_init(&offload_engine);
    if (rc || offload_engine == NULL)
    {
        fprintf(stderr, "offload_engine_init() failed\n");
        return EXIT_FAILURE;
    }

    execution_context_t *client = client_init(offload_engine, NULL);
    if (client == NULL)
    {
        fprintf(stderr, "client handle is undefined\n");
        return EXIT_FAILURE;
    }

    /*
     * BOOTSTRAP COMPLETED: CREATE A SERVER TO LET ALL THE APPLICATION'S PROCESSES
     * OF THE JOB RUNNING ON THE HOST TO CONNECT TO US.
     */
    struct sockaddr_in bootstrap_conn_addr;
    socklen_t bootstrap_conn_addr_len = sizeof(bootstrap_conn_addr);
    memset(&bootstrap_conn_addr, 0, sizeof(bootstrap_conn_addr));
    getsockname(client->client->conn_data.oob.sock, (struct sockaddr *)&bootstrap_conn_addr, &bootstrap_conn_addr_len);
    char bootstrap_conn_ip[16];
    inet_ntop(AF_INET, &(bootstrap_conn_addr.sin_addr), bootstrap_conn_ip, sizeof(bootstrap_conn_ip));
    int bootstrap_conn_port;
    bootstrap_conn_port = ntohs(bootstrap_conn_addr.sin_port);

    init_params_t init_params;
    conn_params_t server_params;
    server_params.addr_str = strdup(bootstrap_conn_ip);
    server_params.port = bootstrap_conn_port+1;
    init_params.conn_params = &server_params;
    init_params.proc_info = NULL;
    execution_context_t *service_server = server_init(offload_engine, &init_params);

    while(!service_server->server->done)
    {
        ucp_worker_progress(service_server->server->ucp_worker);
    }

    free(server_params.addr_str);
    server_params.addr_str = NULL;

    client_fini(&client);
    offload_engine_fini(&offload_engine);
    fprintf(stderr, "client all done, exiting successfully\n");

    return EXIT_SUCCESS;
}