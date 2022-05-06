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
extern dpu_offload_status_t dpu_offload_parse_list_dpus(offloading_engine_t *engine, offloading_config_t *config_data, uint64_t *my_dpu_id);

int main(int argc, char **argv)
{
    if (argc != 4)
    {
        fprintf(stderr, "Please give in order:\n");
        fprintf(stderr, "\t- the path to the configuration file to parse,\n");
        fprintf(stderr, "\t- the list of DPUs you wish to simulate from the config file (e.g., \"jupiter-bf01.hpcadvisorycouncil.com,jupiter-bf02.hpcadvisorycouncil.com\"\n");
        fprintf(stderr, "\t- the DPU on which we want to simulate the parsing of the configuration (e.g., jupiter-bf01.hpcadvisorycouncil.com\n");
        return EXIT_FAILURE;
    }

    offloading_engine_t *engine;
    offload_engine_init(&engine);
    assert(engine);

    uint64_t my_dpu_id;
    offloading_config_t dpu_cfg;
    INIT_DPU_CONFIG_DATA(&dpu_cfg);
    dpu_cfg.list_dpus = argv[2];
    strcpy(dpu_cfg.local_dpu.hostname, argv[3]);
    dpu_offload_parse_list_dpus(engine, &dpu_cfg, &my_dpu_id);
    dpu_cfg.local_dpu.id = my_dpu_id;
    find_dpu_config_from_platform_configfile(argv[1], &dpu_cfg);
    fprintf(stderr, "%s: test succeeded\n", argv[0]);
    return EXIT_SUCCESS;
}