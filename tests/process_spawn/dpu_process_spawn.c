//
// Copyright (c) 2021, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include "host_dpu_offload_service.h"

int main (int argc, char **argv)
{
    int rc;
    offload_config_t cfg;

    // host_oflload_init() gets all the details about offloading, for instance relying
    // on environment variables to get the list of DPUs and know if we actually need to
    // start any service. It will detect if a BF is available.
    fprintf(stderr, "Running host_offload_init()...\n");
    rc = host_offload_init(&cfg);
    if (rc != DPU_OFFLOAD_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    // dpu_offload_service_start() will virtually start the service on the DPUs. It is
    // smart enough to know which host must start the service on which DPUs.
    fprintf(stderr, "Running host_offload_service_start()...\n");
    rc = dpu_offload_service_start(&cfg);
    if (rc != DPU_OFFLOAD_SUCCESS)
    {
        return EXIT_FAILURE;
    }

#if 0
    // dpu_offload_service_check() ensures that the service is up, that we can connect to
    // it and perform a basic handcheck
    fprintf(stderr, "Running host_offload_service_check()...\n");
    rc = dpu_offload_service_check(&cfg);
    if (rc != DPU_OFFLOAD_SUCCESS)
    {
        return EXIT_FAILURE;
    }
#endif

#if 0
    fprintf(stderr, "Running host_offload_service_end()...\n");
    rc = dpu_offload_service_end(&cfg);
    if (rc != DPU_OFFLOAD_SUCCESS)
    {
        return EXIT_FAILURE;
    }
#endif

#if 0
    // dpu_offload_service_check() should check here, all DPU services should be done
    fprintf(stderr, "Running host_offload_service_check(), should be finalized and disconnected...\n");
    rc = dpu_offload_service_check(&cfg);
    if (rc != DPU_OFFLOAD_SUCCESS)
    {
        fprintf(stderr, "ERROR: DPU service(s) did not start properly\n");
        return EXIT_FAILURE;
    }
#endif

    fprintf(stdout, "Test succeeded\n");

    return EXIT_SUCCESS;
}
