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
extern dpu_offload_status_t dpu_offload_parse_list_dpus(offloading_engine_t *engine, offloading_config_t *config_data);

int main(int argc, char **argv)
{
    if (argc != 4)
    {
        fprintf(stderr, "Please give in order:\n");
        fprintf(stderr, "\t- the path to the configuration file to parse,\n");
        fprintf(stderr, "\t- the list of DPUs you wish to simulate from the config file (e.g., \"jupiterbf001.hpcadvisorycouncil.com,jupiterbf002.hpcadvisorycouncil.com\"\n");
        fprintf(stderr, "\t- the DPU on which we want to simulate the parsing of the configuration (e.g., jupiterbf001.hpcadvisorycouncil.com\n");
        return EXIT_FAILURE;
    }

    offloading_engine_t *engine;
    offload_engine_init(&engine);
    assert(engine);

    offloading_config_t cfg;
    INIT_DPU_CONFIG_DATA(&cfg);
    cfg.list_dpus = argv[2];
    strcpy(cfg.local_service_proc.hostname, argv[3]);
    dpu_offload_parse_list_dpus(engine, &cfg);
    find_dpu_config_from_platform_configfile(argv[1], &cfg);
    fprintf(stdout, "Configuration: \n");
    fprintf(stdout, "\tNumber of DPUs: %ld\n", cfg.num_dpus);
    fprintf(stdout, "\tNumber of service process per DPU: %ld\n", cfg.num_service_procs_per_dpu);
    fprintf(stdout, "\tNumber of service process(es) to connect to: %ld\n", cfg.info_connecting_to.num_connect_to);
    fprintf(stdout, "\tNumber of service process(es) expected to connect to us: %ld\n", cfg.num_connecting_service_procs);
    fprintf(stdout, "\tPort for inter-service-process connection: %d\n", cfg.local_service_proc.inter_service_procs_conn_params.port);
    if (cfg.local_service_proc.inter_service_procs_conn_params.port == -1)
    {
        fprintf(stderr, "[ERROR] Invalid port\n");
        goto error_out;
    }
    fprintf(stdout, "\tPort for host connection: %d\n", cfg.local_service_proc.host_conn_params.port);
    if (cfg.local_service_proc.host_conn_params.port == -1)
    {
        fprintf(stderr, "[ERROR] Invalid port\n");
        goto error_out;
    }
    fprintf(stdout, "\tAddress: %s\n", cfg.local_service_proc.host_conn_params.addr_str);
    fprintf(stdout, "Connecting to %ld service processes\n", cfg.info_connecting_to.num_connect_to);
    connect_to_service_proc_t *remote_sp, *next_remote_sp;
    ucs_list_for_each_safe(remote_sp, next_remote_sp, &(cfg.info_connecting_to.sps_connect_to), item)
    {
        if (remote_sp->sp->init_params.conn_params == NULL)
        {
            fprintf(stderr, "[ERROR] connection parameters for %p are undefined\n", remote_sp->sp);
            goto error_out;
        }
        fprintf(stdout, "\tPort: %d\n", remote_sp->sp->init_params.conn_params->port);
        if (remote_sp->sp->init_params.conn_params->port == -1)
        {
            fprintf(stderr, "[ERROR] Invalid port\n");
            goto error_out;
        }
    }

    fprintf(stderr, "%s: test succeeded\n", argv[0]);
    return EXIT_SUCCESS;
error_out:
    fprintf(stderr, "%s: test failed\n", argv[0]);
    return EXIT_FAILURE;
}