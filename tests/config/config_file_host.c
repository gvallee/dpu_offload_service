//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <stdlib.h>
#include <stdio.h>

#include "dpu_offload_service_daemon.h"

extern dpu_offload_status_t find_config_from_platform_configfile(char *filepath, char *hostname, offloading_config_t *data);
extern dpu_offload_status_t find_config_from_platform_configfile(char *filepath, char *hostname, offloading_config_t *config_data);
extern dpu_offload_status_t dpu_offload_parse_list_dpus(offloading_engine_t *engine, offloading_config_t *config_data);

int main(int argc, char **argv)
{
    dpu_offload_status_t rc;
    if (argc != 4)
    {
        fprintf(stderr, "Please give in order:\n");
        fprintf(stderr, "\t- the path to the configuration file to parse,\n");
        fprintf(stderr, "\t- the host on which we want to simulate the parsing of the configuration (e.g., jupiter001.hpcadvisorycouncil.com\n");
        return EXIT_FAILURE;
    }

    offloading_engine_t *engine;
    rc = offload_engine_init(&engine);
    if (rc != DO_SUCCESS)
    {
        fprintf(stderr, "[ERROR] dpu_offload_parse_list_dpus() failed\n");
        goto error_out;
    }
    assert(engine);

    offloading_config_t cfg;
    INIT_DPU_CONFIG_DATA(&cfg);
    strcpy(cfg.local_service_proc.hostname, argv[2]);
    rc = find_config_from_platform_configfile(argv[1], argv[3], &cfg);
    if (rc != DO_SUCCESS)
    {
        fprintf(stderr, "[ERROR] find_config_from_platform_configfile() failed\n");
        goto error_out;
    }

    fprintf(stderr, "%s: test succeeded\n", argv[0]);
    return EXIT_SUCCESS;
error_out:
    fprintf(stderr, "%s: test failed\n", argv[0]);
    return EXIT_FAILURE;
}