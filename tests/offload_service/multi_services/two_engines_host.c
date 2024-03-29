//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

#include <ucp/api/ucp.h>

#include "multi_engines_utils.h"

bool client1_done = false;
bool client2_done = false;

int client1_notif_cb(struct dpu_offload_ev_sys *ev_sys, execution_context_t *context, am_header_t *hdr, size_t hdr_size, void *data, size_t data_size)
{
    int *val;
    assert(data_size == sizeof(int));
    val = (int*)data;
    assert(*val == MSG_CLIENT_ENGINE1);
    client1_done = true;
    return 0;
}

int client2_notif_cb(struct dpu_offload_ev_sys *ev_sys, execution_context_t *context, am_header_t *hdr, size_t hdr_size, void *data, size_t data_size)
{
    int *val;
    assert(data_size == sizeof(int));
    val = (int*)data;
    assert(*val == MSG_CLIENT_ENGINE2);
    client2_done = true;
    return 0;
}

int main(int argc, char **argv)
{
    ucp_worker_h service1_worker = NULL, service2_worker = NULL;
    ucp_params_t ucp_params;
    ucp_context_h ucp_context;
    ucs_status_t status;
    ucp_config_t *config = NULL;
    offloading_engine_t *engine1 = NULL, *engine2 = NULL;
    execution_context_t *client1 = NULL, *client2 = NULL;
    offloading_config_t service1_cfg, service2_cfg;
    dpu_config_data_t *service1_dpu_cfg = NULL, *service2_dpu_cfg;
    int *service1_port = NULL, *service2_port = NULL;
    init_params_t service1_init_params, service2_init_params;
    conn_params_t service1_conn_params, service2_conn_params;
    dpu_offload_event_t *service1_ev = NULL, *service2_ev = NULL;
    dpu_offload_status_t rc;
    int ret, service1_msg = MSG_CLIENT_ENGINE1, service2_msg = MSG_CLIENT_ENGINE2;

    memset(&ucp_params, 0, sizeof(ucp_params));
    ucp_params.field_mask = UCP_PARAM_FIELD_FEATURES;
    ucp_params.features = UCP_FEATURE_TAG | UCP_FEATURE_AM;
    status = ucp_config_read(NULL, NULL, &config);
    assert(status == UCS_OK);
    status = ucp_init(&ucp_params, config, &ucp_context);
    assert(status == UCS_OK);
    ret = init_worker(ucp_context, &service1_worker);
    assert(ret == 0);
    ret = init_worker(ucp_context, &service2_worker);
    assert(ret == 0);

    INIT_DPU_CONFIG_DATA(&service1_cfg);
    INIT_DPU_CONFIG_DATA(&service2_cfg);
    service1_cfg.config_file = getenv("MY_CFG_FILE_SERVICE1");
    assert(service1_cfg.config_file);
    service2_cfg.config_file = getenv("MY_CFG_FILE_SERVICE2");
    assert(service2_cfg.config_file);
    get_host_config(&service1_cfg);
    get_host_config(&service2_cfg);

    rc = offload_engine_init(&engine1);
    assert(rc == DO_SUCCESS);
    assert(engine1);

    rc = offload_engine_init(&engine2);
    assert(rc == DO_SUCCESS);
    assert(engine2);

    rc = engine_register_default_notification_handler(engine1, NOTIF_TEST_DONE, client1_notif_cb, NULL);
    assert(rc == DO_SUCCESS);
    rc = engine_register_default_notification_handler(engine2, NOTIF_TEST_DONE, client2_notif_cb, NULL);
    assert(rc == DO_SUCCESS);

    engine1->config = &service1_cfg;
    service1_cfg.offloading_engine = engine1;
    engine2->config = &service2_cfg;
    service2_cfg.offloading_engine = engine2;

    service1_dpu_cfg = DYN_ARRAY_GET_ELT(&(service1_cfg.dpus_config), 0, dpu_config_data_t);
    assert(service1_dpu_cfg);
    service1_port = DYN_ARRAY_GET_ELT(&(service1_dpu_cfg->version_1.host_ports), 0, int);
    assert(service1_port);

    service2_dpu_cfg = DYN_ARRAY_GET_ELT(&(service2_cfg.dpus_config), 0, dpu_config_data_t);
    assert(service2_dpu_cfg);
    service2_port = DYN_ARRAY_GET_ELT(&(service2_dpu_cfg->version_1.host_ports), 0, int);
    assert(service2_port);

    // Initiate the connection to the target services
    RESET_INIT_PARAMS(&service1_init_params);
    RESET_CONN_PARAMS(&service1_conn_params);
    service1_init_params.conn_params = &service1_conn_params;
    service1_conn_params.addr_str = service1_dpu_cfg->version_1.addr;
    service1_conn_params.port = *service1_port;
    service1_init_params.ucp_context = ucp_context;
    service1_init_params.worker = service1_worker;
    fprintf(stdout, "Connecting to service #1 - addr: %s, port: %d\n", service1_conn_params.addr_str, service1_conn_params.port);
    client1 = client_init(engine1, &service1_init_params);
    assert(client1);
    ADD_CLIENT_TO_ENGINE(client1, engine1);

    RESET_INIT_PARAMS(&service2_init_params);
    RESET_CONN_PARAMS(&service2_conn_params);
    service2_init_params.conn_params = &service2_conn_params;
    service2_conn_params.addr_str = service2_dpu_cfg->version_1.addr;
    service2_conn_params.port = *service2_port;
    service2_init_params.ucp_context = ucp_context;
    service2_init_params.worker = service2_worker;
    fprintf(stdout, "Connecting to service #2 - addr: %s, port: %d\n", service2_conn_params.addr_str, service2_conn_params.port);
    client2 = client_init(engine2, &service2_init_params);
    assert(client2);
    ADD_CLIENT_TO_ENGINE(client2, engine2);

    while (GET_ECONTEXT_BOOTSTRAPING_PHASE(client1) != BOOTSTRAP_DONE && GET_ECONTEXT_BOOTSTRAPING_PHASE(client2) != BOOTSTRAP_DONE)
    {
        lib_progress(client1);
        lib_progress(client2);
        if (GET_ECONTEXT_BOOTSTRAPING_PHASE(client1) == BOOTSTRAP_DONE)
            fprintf(stdout, "Connected to service 1\n");
        if (GET_ECONTEXT_BOOTSTRAPING_PHASE(client2) == BOOTSTRAP_DONE)
            fprintf(stdout, "Connected to service 2\n");
    }
    fprintf(stdout, "-> Now connected to the services\n");

    // Send messages with the same ID to the two services
    rc = event_get(client1->event_channels, NULL, &service1_ev);
    assert(rc == DO_SUCCESS);
    assert(service1_ev);
    rc = event_channel_emit_with_payload(&service1_ev,
                                         NOTIF_TEST_ID,
                                         GET_SERVER_EP(client1),
                                         client1->client->server_id,
                                         NULL,
                                         &service1_msg,
                                         sizeof(int));
    assert(rc == DO_SUCCESS);

    rc = event_get(client2->event_channels, NULL, &service2_ev);
    assert(rc == DO_SUCCESS);
    assert(service2_ev);
    rc = event_channel_emit_with_payload(&service2_ev,
                                         NOTIF_TEST_ID,
                                         GET_SERVER_EP(client2),
                                         client2->client->server_id,
                                         NULL,
                                         &service2_msg,
                                         sizeof(int));
    assert(rc == DO_SUCCESS);

    while (!client1_done && !client2_done)
    {
        offload_engine_progress(engine1);
        offload_engine_progress(engine2);
    }

    // Clients have been assigned to the engines, they will be implicitly terminated
    offload_engine_fini(&engine1);
    offload_engine_fini(&engine2);

    return EXIT_SUCCESS;
}