//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

/*
 * This test is designed to be executed on a single DPU, with the list of DPUs (env var) set
 * and a configuration file with the associated environment variable set
 */

#include <stdlib.h>
#include <stdio.h>

#include "dpu_offload_service_daemon.h"

#define MY_TEST_NOTIF_ID (1000)
#define MY_TEST_MANY_NOTIFS_ID (1001)
#define NUM_NOTIFS (1000)

static bool self_notif_received = false;
static int self_notification_cb(struct dpu_offload_ev_sys *ev_sys, execution_context_t *econtext, am_header_t *hdr, size_t hdr_len, void *data, size_t data_len)
{
    self_notif_received = true;
}

static size_t count = 0;
static int self_many_notifs_cb(struct dpu_offload_ev_sys *ev_sys, execution_context_t *econtext, am_header_t *hdr, size_t hdr_len, void *data, size_t data_len)
{
    fprintf(stdout, "notifications #%ld received\n", count);
    count++;
}

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
    fprintf(stderr, "[INFO] msg successfully received\n");
}

static void send_cb(void *request, ucs_status_t status)
{
    fprintf(stderr, "[INFO] msg successfully sent\n");
}

int main(int argc, char **argv)
{
    /*
     * BOOTSTRAPPING: WE CREATE A CLIENT THAT CONNECT TO THE INITIATOR ON THE HOST
     * AND INITIALIZE THE OFFLOADING SERVICE.
     */
    fprintf(stderr, "Creating offload engine...\n");
    offloading_engine_t *engine = NULL;
    dpu_offload_status_t rc = offload_engine_init(&engine);
    if (rc || engine == NULL)
    {
        fprintf(stderr, "offload_engine_init() failed\n");
        goto error_out;
    }

    fprintf(stderr, "Registering callback for the many notifications test %d\n", MY_TEST_MANY_NOTIFS_ID);
    rc = engine_register_default_notification_handler(engine, MY_TEST_MANY_NOTIFS_ID, self_many_notifs_cb);
    if (rc)
    {
        fprintf(stderr, "[ERROR] engine_register_default_notification_handler() failed\n");
        return EXIT_FAILURE;
    }

    fprintf(stderr, "Registering callback for notifications of test completion %d\n", MY_TEST_NOTIF_ID);
    rc = engine_register_default_notification_handler(engine, MY_TEST_NOTIF_ID, self_notification_cb);
    if (rc)
    {
        fprintf(stderr, "[ERROR] engine_register_default_notification_handler() failed\n");
        return EXIT_FAILURE;
    }

    /*
     * GET THE CONFIGURATION.
     */
    fprintf(stderr, "Getting configuration...\n");
    offloading_config_t config_data;
    INIT_DPU_CONFIG_DATA(&config_data);
    config_data.offloading_engine = engine;
    int ret = get_dpu_config(engine, &config_data);
    if (ret)
    {
        fprintf(stderr, "[ERROR] get_config() failed\n");
        return EXIT_FAILURE;
    }
    engine->config = &config_data;

    /*
     * INITIATE CONNECTION BETWEEN DPUS.
     */
    fprintf(stderr, "Initiating connections between DPUs\n");
    rc = inter_dpus_connect_mgr(engine, &config_data);
    if (rc)
    {
        fprintf(stderr, "inter_dpus_connect_mgr() failed\n");
        return EXIT_FAILURE;
    }
    fprintf(stderr, "Connections between DPUs successfully initialized\n");

    /* UCX communication to self */
    if (engine->self_ep == NULL)
    {
        fprintf(stderr, "[ERROR] self EP is undefined\n");
        goto error_out;
    }

    int send_msg = 42;
    int recv_msg;
    int msg_tag = 42;
    ucp_tag_t msg_tag_mask = (ucp_tag_t)-1;
    fprintf(stdout, "[INFO] self EP: %p\n", engine->self_ep);

    fprintf(stdout, "Posting a nb recv from myself...\n");
    struct ucx_context *recv_req = ucp_tag_recv_nb(engine->ucp_worker, &recv_msg, sizeof(recv_msg), ucp_dt_make_contig(1), msg_tag, msg_tag_mask, recv_cb);
    if (UCS_PTR_IS_ERR(recv_req))
    {
        fprintf(stderr, "Recv failed\n");
        ucp_request_cancel(engine->ucp_worker, recv_req);
        ucp_request_free(recv_req);
        recv_req = NULL;
    }
    if (recv_req == NULL)
    {
        fprintf(stderr, "[WARN] recv completed but send was not posted\n");
    }

    fprintf(stdout, "Posting a non-blocking send to myself...\n");
    struct ucx_context *send_req = ucp_tag_send_nb(engine->self_ep, &send_msg, sizeof(send_msg), ucp_dt_make_contig(1), msg_tag, send_cb);
    if (UCS_PTR_IS_ERR(send_req))
    {
        ucp_request_cancel(engine->ucp_worker, send_req);
        ucp_request_free(send_req);
        send_req = NULL;
    }
    if (send_req)
        fprintf(stdout, "[INFO] send completed right away\n");

    fprintf(stdout, "Checking whether everything completed...\n");
    if (send_req != NULL)
    {
        while (!req_completed(send_req))
           offload_engine_progress(engine);
        ucp_request_free(send_req);
        send_req = NULL;
    }
    if (recv_req != NULL)
    {
        while (!req_completed(recv_req))
           offload_engine_progress(engine);
        ucp_request_free(recv_req);
        recv_req = NULL;
    }

    if (send_msg != recv_msg)
    {
        fprintf(stderr, "[ERROR] We received %d but were expecting %d\n", recv_msg, send_msg);
    }

    /* A bunch of notifications to self */
    size_t n;
    for (n = 0; n < NUM_NOTIFS; n++)
    {
        dpu_offload_event_t *self_ev;
        rc = event_get(engine->self_econtext->event_channels, NULL, &self_ev);
        assert(self_ev);
        assert(rc == DO_SUCCESS);

        rc = event_channel_emit(&self_ev, MY_TEST_MANY_NOTIFS_ID, engine->self_ep, 0, NULL);
        if (rc != EVENT_DONE && rc != EVENT_INPROGRESS)
        {
            fprintf(stderr, "[ERROR] event_channel_emit() failed\n");
            goto error_out;
        }
        if (rc == EVENT_INPROGRESS)
            ucs_list_add_tail(&(engine->self_econtext->ongoing_events), &(self_ev->item));
    }

    while (count != NUM_NOTIFS)
        offload_engine_progress(engine);

    count = 0;
    for (n = 0; n < NUM_NOTIFS; n++)
    {
        dpu_offload_event_t *self_ev;
        dpu_offload_event_info_t ev_info;
        ev_info.payload_size = 1024;
        rc = event_get(engine->self_econtext->event_channels, &ev_info, &self_ev);
        assert(self_ev);
        assert(rc == DO_SUCCESS);

        rc = event_channel_emit(&self_ev, MY_TEST_MANY_NOTIFS_ID, engine->self_ep, 0, NULL);
        if (rc != EVENT_DONE && rc != EVENT_INPROGRESS)
        {
            fprintf(stderr, "[ERROR] event_channel_emit() failed\n");
            goto error_out;
        }
        if (rc == EVENT_INPROGRESS)
            ucs_list_add_tail(&(engine->self_econtext->ongoing_events), &(self_ev->item));
    }

    /* Notification to self to terminate the test */
    dpu_offload_event_t *self_ev;
    rc = event_get(engine->self_econtext->event_channels, NULL, &self_ev);
    assert(self_ev);
    assert(rc == DO_SUCCESS);

    rc = event_channel_emit(&self_ev, MY_TEST_NOTIF_ID, engine->self_ep, 0, NULL);
    if (rc != EVENT_DONE && rc != EVENT_INPROGRESS)
    {
        fprintf(stderr, "[ERROR] event_channel_emit() failed\n");
        goto error_out;
    }
    if (rc == EVENT_INPROGRESS)
        ucs_list_add_tail(&(engine->self_econtext->ongoing_events), &(self_ev->item));

    while (self_notif_received == false)
        offload_engine_progress(engine);

    offload_engine_fini(&engine);

    fprintf(stdout, "Test succeeded\n");

    return EXIT_SUCCESS;
error_out:
    if (engine != NULL)
    {
        offload_engine_fini(&engine);
    }
    fprintf(stderr, "Test failed\n");
    return EXIT_FAILURE;
}