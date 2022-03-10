//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <stdlib.h>
#include <stdio.h>

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

int main(int argc, char **argv)
{
    offloading_engine_t *offload_engine;
    dpu_offload_status_t rc = offload_engine_init(&offload_engine);
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

    /* ping-pong with the server */
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

    // NOTIFICATION TEST
    int i;
    dpu_offload_event_t **evts = (dpu_offload_event_t **)calloc(NUM_TEST_EVTS + 1, sizeof(dpu_offload_event_t*));
    if (evts == NULL)
    {
        fprintf(stderr, "unable to allocate events\n");
        return EXIT_FAILURE;
    }

    for (i = 0; i <= NUM_TEST_EVTS; i++)
    {
        dpu_offload_event_t *cur_evt;
        rc = event_get(client->event_channels, &cur_evt);
        if (rc)
        {
            fprintf(stderr, "event_get() failed\n");
            return EXIT_FAILURE;
        }
        evts[i] = cur_evt;

        int notif_data = i;
        rc = event_channel_emit(cur_evt, client->client->id, AM_TEST_MSG_ID, GET_SERVER_EP(client), NULL, &notif_data, sizeof(notif_data));
        if (rc)
        {
            fprintf(stderr, "event_channel_emit() failed\n");
            return EXIT_FAILURE;
        }
        fprintf(stderr, "Ev #%d = %p\n", i, cur_evt);
    }

    // All the events have been emitted, now waiting for them to complete
    for (i = 0; i <= NUM_TEST_EVTS; i++)
    {
        dpu_offload_event_t *cur_evt = evts[i];
        fprintf(stderr, "Waiting for event #%d (%p) to complete\n", i, cur_evt);
        while(!cur_evt->ctx.complete)
            client->progress(client);
    }

    // All events completed, we can safely return them
    for (i = 0; i <= NUM_TEST_EVTS; i++)
    {
        dpu_offload_event_t *cur_evt = evts[i];
        rc = event_return(client->event_channels, &cur_evt);
        if (rc)
        {
            fprintf(stderr, "event_return() failed\n");
            return EXIT_FAILURE;
        }
    }

    free(evts);

    fprintf(stderr, "ALL TESTS COMPLETED\n");

end_test:
    client_fini(&client);
    offload_engine_fini(&offload_engine);
    fprintf(stderr, "client all done, exiting successfully\n");

    return EXIT_SUCCESS;
}