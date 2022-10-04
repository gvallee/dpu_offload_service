//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

#include "multi_engines_utils.h"

int main(int argc, char **argv)
{
    ucp_worker_h service1_worker = NULL, service2_worker = NULL;
    ucp_params_t ucp_params;
    ucp_context_h ucp_context;
    ucs_status_t status;
    ucp_config_t *config = NULL;
    offloading_engine_t *engine1 = NULL, *engine2 = NULL;
    offloading_config_t engine1_cfg, engine2_cfg;
    execution_context_t *service1_server = NULL, *service2_server = NULL;
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

    INIT_DPU_CONFIG_DATA(&engine1_cfg);
    INIT_DPU_CONFIG_DATA(&engine2_cfg);
    engine1_cfg.config_file = getenv("MY_CFG_FILE_SERVICE1");
    engine2_cfg.config_file = getenv("MY_CFG_FILE_SERVICE2");

    rc = offload_engine_init(&engine1);
    assert(rc == DO_SUCCESS);
    assert(engine1);
    engine1->config = &engine1_cfg;
    engine1_cfg.offloading_engine = engine1;
    engine1->ucp_context = ucp_context;
    engine1->ucp_worker = service1_worker;

    rc = offload_engine_init(&engine2);
    assert(rc == DO_SUCCESS);
    assert(engine2);
    engine2->config = &engine2_cfg;
    engine2_cfg.offloading_engine = engine2;
    engine2->ucp_context = ucp_context;
    engine2->ucp_worker = service2_worker;

    rc = get_dpu_config(engine1, &engine1_cfg);
    assert(rc == DO_SUCCESS);
    rc = get_dpu_config(engine2, &engine2_cfg);
    assert(rc == DO_SUCCESS);

    rc = inter_dpus_connect_mgr(engine1, &engine1_cfg);
    assert(rc == DO_SUCCESS);
    rc = inter_dpus_connect_mgr(engine2, &engine2_cfg);
    assert(rc == DO_SUCCESS);

    engine1_cfg.local_service_proc.host_init_params.ucp_context = ucp_context;
    engine1_cfg.local_service_proc.host_init_params.worker = service1_worker;
    service1_server = server_init(engine1, &(engine1_cfg.local_service_proc.host_init_params));
    assert(service1_server);
    ADD_SERVER_TO_ENGINE(service1_server, engine1);

    engine2_cfg.local_service_proc.host_init_params.ucp_context = ucp_context;
    engine2_cfg.local_service_proc.host_init_params.worker = service2_worker;
    service2_server = server_init(engine2, &(engine2_cfg.local_service_proc.host_init_params));
    assert(service2_server);
    ADD_SERVER_TO_ENGINE(service2_server, engine2);

    while (!EXECUTION_CONTEXT_DONE(service1_server) || !EXECUTION_CONTEXT_DONE(service2_server))
    {
        if (!EXECUTION_CONTEXT_DONE(service1_server))
            lib_progress(service1_server);
        if (!EXECUTION_CONTEXT_DONE(service2_server))
            lib_progress(service2_server);
    }

    // Servers have been assigned to the engines, they will be implicitly terminated
    offload_engine_fini(&engine1);
    offload_engine_fini(&engine2);

    return EXIT_SUCCESS;
}