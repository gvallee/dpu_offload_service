//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <stdlib.h>
#include <stdio.h>

#include <ucp/api/ucp.h>

#include "dpu_offload_service_daemon.h"

#include "../common_test_params.h"

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

static dpu_offload_status_t init_worker(ucp_context_h ucp_context, ucp_worker_h *ucp_worker)
{
    ucp_worker_params_t worker_params;
    ucs_status_t status;
    memset(&worker_params, 0, sizeof(worker_params));
    worker_params.field_mask = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
    worker_params.thread_mode = UCS_THREAD_MODE_MULTI;
    status = ucp_worker_create(ucp_context, &worker_params, ucp_worker);
    if (status != UCS_OK)
    {
        fprintf(stderr, "ucp_worker_create() failed\n");
        return DO_ERROR;
    }
    return DO_SUCCESS;
}

static inline int run_client_test(ucp_worker_h worker, ucp_context_h ucp_context)
{
    offloading_engine_t *offload_engine;
    init_params_t init_params;
    RESET_INIT_PARAMS(&init_params);
    dpu_offload_status_t rc;
    if (worker != NULL)
    {
        init_params.worker = worker;
        init_params.ucp_context = ucp_context;
        init_params.conn_params = NULL;
        init_params.proc_info = NULL;
        rc = offload_engine_init(&offload_engine);
    }
    else
        rc = offload_engine_init(&offload_engine);
     
    if (rc || offload_engine == NULL)
    {
        fprintf(stderr, "offload_engine_init() failed\n");
        return EXIT_FAILURE;
    }

    execution_context_t *client;
    if (worker != NULL)
    {
        client = client_init(offload_engine, &init_params);
    }
    else
    {
        client = client_init(offload_engine, NULL);
    }

    if (client == NULL)
    {
        fprintf(stderr, "client handle is undefined\n");
        return EXIT_FAILURE;
    }

    do
    {
        client->progress(client);
    } while (client->client->bootstrapping.phase != BOOTSTRAP_DONE);
    

    fprintf(stderr, "Client initialization all done\n");

    // REGISTER SOME EVENTS FOR TESTING
    REGISTER_NOTIF_CALLBACKS(client);

    /* ping-pong with the server using point-to-point tag based communications */
    int msg_tag = 42;
    ucp_tag_t msg_tag_mask = (ucp_tag_t)-1;
    int msg = 99;
    struct ucx_context *send_req = ucp_tag_send_nb(GET_SERVER_EP(client), &msg, sizeof(msg), ucp_dt_make_contig(1), msg_tag, send_cb);
    if (UCS_PTR_IS_ERR(send_req))
    {
        fprintf(stderr, "send failed\n");
        ucp_request_cancel(GET_WORKER(client), send_req);
        ucp_request_free(send_req);
        send_req = NULL;
    }
    if (send_req != NULL)
    {
        while (!req_completed(send_req))
            client->progress(client);
        ucp_request_free(send_req);
        send_req = NULL;
    }

    int response;
    struct ucx_context *recv_req = ucp_tag_recv_nb(GET_WORKER(client), &response, sizeof(response), ucp_dt_make_contig(1), msg_tag, msg_tag_mask, recv_cb);
    if (UCS_PTR_IS_ERR(recv_req))
    {
        fprintf(stderr, "Recv failed\n");
        ucp_request_cancel(GET_WORKER(client), recv_req);
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
        /* if it did not complete, we wait for it to complete */
        while (!req_completed(recv_req))
            client->progress(client);
        ucp_request_free(recv_req);
        recv_req = NULL;
    }

    if (response != msg + 1)
        fprintf(stderr, "Invalid result receives\n");
    else
        fprintf(stderr, "Successfully received the expected response from the server\n");

    // NOTIFICATIONS TEST
    fprintf(stderr, "STARTING TEST OF THE NOTIFICATION SYSTEM\n");

    /* First with emitting a bunch of events and manually managing all of them */
    EMIT_MANY_EVS_WITH_EXPLICIT_MGT(client);

    /* Similar test but using the ongoing events queue, i.e, with implicit return of the event objects */
    EMIT_MANY_EVTS_AND_USE_ONGOING_LIST(client);

    /* Then we become the receiving side for the same tests */
    WAIT_FOR_ALL_EVENTS_WITH_EXPLICIT_MGT(client);
    WAIT_FOR_ALL_EVENTS_WITH_ONGOING_LIST(client);

    /* Finally we do a notification-based ping-pong that we initiate */
    INITIATE_PING_PONG_TEST(client);

    fprintf(stderr, "ALL TESTS COMPLETED\n");

    client_fini(&client);
    offload_engine_fini(&offload_engine);
    fprintf(stderr, "client all done, exiting successfully\n");

    return EXIT_SUCCESS;
}
