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

#include "dpu_offload_service_daemon.h"
#include "dpu_offload_event_channels.h"

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
    dpu_offload_status_t rc;
    int ret;

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

    engine1->config = &service1_cfg;
    engine2->config = &service2_cfg;

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

    RESET_INIT_PARAMS(&service2_init_params);
    RESET_CONN_PARAMS(&service2_conn_params);
    service2_init_params.conn_params = &service2_conn_params;
    service2_conn_params.addr_str = service2_dpu_cfg->version_1.addr;
    service2_conn_params.port = *service2_port;
    service1_init_params.ucp_context = ucp_context;
    service1_init_params.worker = service1_worker;
    fprintf(stdout, "Connecting to service #2 - addr: %s, port: %d\n", service2_conn_params.addr_str, service2_conn_params.port);
    client2 = client_init(engine2, &service2_init_params);
    assert(client2);

    client_fini(&client1);
    client_fini(&client2);
    offload_engine_fini(&engine1);
    offload_engine_fini(&engine2);

    return EXIT_SUCCESS;
}