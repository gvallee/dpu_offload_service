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
    fprintf(stderr, "ping msg from client successfully received, sending pong...\n");
}

static void send_cb(void *request, ucs_status_t status)
{
    fprintf(stderr, "pong successfully sent\n");
}

int main(int argc, char **argv)
{
    dpu_offload_daemon_t *server;
    int rc = server_init(&server);
    if (rc)
    {
        fprintf(stderr, "init_server() failed\n");
        return EXIT_FAILURE;
    }
    if (server == NULL)
    {
        fprintf(stderr, "server handle is undefined\n");
        return EXIT_FAILURE;
    }

    ucp_worker_h worker;
    DAEMON_GET_WORKER(server, worker);
    ucp_ep_h client_ep;
    DAEMON_GET_PEER_EP(server, client_ep);

    int msg_tag = 42;
    ucp_tag_t msg_tag_mask = (ucp_tag_t)-1;
    int ping;
    struct ucx_context *recv_req = ucp_tag_recv_nb(worker, &ping, sizeof(ping), ucp_dt_make_contig(1), msg_tag, msg_tag_mask, recv_cb);
    if (UCS_PTR_IS_ERR(recv_req))
    {
        fprintf(stderr, "Recv failed\n");
        ucp_request_cancel(worker, recv_req);
        ucp_request_free(recv_req);
        recv_req = NULL;
    }
    /* Did the request complete right away? */
    ucp_tag_recv_info_t _info;
    ucs_status_t _status = ucp_tag_recv_request_test(recv_req, &_info);
    if (_status != UCS_INPROGRESS)
    {
        ucp_request_free(recv_req);
        recv_req = NULL;
    }
    if (recv_req != NULL)
    {
        while (!req_completed(recv_req))
            ucp_worker_progress(worker);
        ucp_request_free(recv_req);
        recv_req = NULL;
    }

    int msg = ping + 1;
    struct ucx_context *send_req = ucp_tag_send_nb(client_ep, &msg, sizeof(msg), ucp_dt_make_contig(1), msg_tag, send_cb);
    if (UCS_PTR_IS_ERR(send_req))
    {
        ucp_request_cancel(worker, send_req);
        ucp_request_free(send_req);
        send_req = NULL;
    }
    if (send_req != NULL)
    {
        while (!req_completed(send_req))
            ucp_worker_progress(worker);
        ucp_request_free(send_req);
        send_req = NULL;
    }

    fprintf(stderr, "Waiting for client to terminate...\n");

    while (!server->done)
    {
        ucp_worker_progress(worker);
    }

    server_fini(&server);

    fprintf(stderr, "server all done, exiting successfully\n");
    return EXIT_SUCCESS;
}