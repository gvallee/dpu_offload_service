//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <stdlib.h>
#include <stdio.h>

#include "dpu_offload_service_daemon.h"

extern dpu_offload_status_t find_config_from_platform_configfile(char *filepath, char *hostname, offloading_config_t *data);
extern dpu_offload_status_t find_dpu_config_from_platform_configfile(char *filepath, offloading_config_t *config_data);

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "Please give the path to the configuration file to parse\n");
        return EXIT_FAILURE;
    }

    offloading_config_t dpu_cfg;
    find_dpu_config_from_platform_configfile(argv[1], &dpu_cfg);

    offloading_config_t host_config;
    find_config_from_platform_configfile(argv[1], "toto", &host_config);

    return EXIT_SUCCESS;
}