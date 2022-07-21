#define _POSIX_C_SOURCE 200809L

//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>

#include "dpu_offload_service_daemon.h"
#include "dpu_offload_envvars.h"

// This test mimics a very basic service process code that is meant to
// be used with valgrind to find any memory leak/issues. It creates 
// the engine and parses the configuration file.
int main(int argc, char **argv)
{
    offloading_engine_t *offload_engine;
    dpu_offload_status_t rc = offload_engine_init(&offload_engine);
    if (rc || offload_engine == NULL)
    {
        fprintf(stderr, "offload_engine_init() failed\n");
        return EXIT_FAILURE;
    }

    offloading_config_t config_data;
    INIT_DPU_CONFIG_DATA(&config_data);
    config_data.offloading_engine = offload_engine;
    int ret = get_dpu_config(offload_engine, &config_data);
    if (ret)
    {
        fprintf(stderr, "get_config() failed\n");
        return EXIT_FAILURE;
    }

    offload_engine_fini(&offload_engine);
    fprintf(stderr, "client all done, exiting successfully\n");

    return EXIT_SUCCESS;
}