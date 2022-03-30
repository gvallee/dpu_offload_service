//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include "client_common.h"

int main(int argc, char **argv)
{
    // We initialize UCX and pass the worker in.
    ucp_worker_h ucp_worker;
    ucp_params_t ucp_params;
    ucp_context_h ucp_context;
    ucs_status_t status;
    ucp_config_t *config;
    int ret = 0;
    
    memset(&ucp_params, 0, sizeof(ucp_params));
    ucp_params.field_mask = UCP_PARAM_FIELD_FEATURES;
    ucp_params.features = UCP_FEATURE_TAG | UCP_FEATURE_AM;
    status = ucp_config_read(NULL, NULL, &config);
    if (status != UCS_OK)
    {
        fprintf(stderr, "ucp_config_read() failed\n");
        return EXIT_FAILURE;
    }
    status = ucp_init(&ucp_params, config, &ucp_context);
    if (status != UCS_OK)
    {
        fprintf(stderr, "ucp_init() failed\n");
        return EXIT_FAILURE;
    }
    ucp_config_print(config, stdout, NULL, UCS_CONFIG_PRINT_CONFIG);
    ucp_config_release(config);
    ret = init_worker(ucp_context, &ucp_worker);
    if (ret != 0)
    {
        fprintf(stderr, "init_worker() failed\n");
        return EXIT_FAILURE;
    }
    if (ucp_worker == NULL)
    {
        fprintf(stderr, "undefined worker\n");
        return EXIT_FAILURE;
    }

    return run_client_test(ucp_worker, ucp_context);
}