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
    offloading_engine_t *engine = NULL;
    offloading_config_t cfg;

    if (argc != 3)
    {
        fprintf(stderr, "Please give in order:\n");
        fprintf(stderr, "\t- the path to the configuration file to parse,\n");
        fprintf(stderr, "\t- the host on which we want to simulate the parsing of the configuration (e.g., jupiter001.hpcadvisorycouncil.com)\n");
        return EXIT_FAILURE;
    }

    rc = offload_engine_init(&engine);
    if (rc != DO_SUCCESS)
    {
        fprintf(stderr, "[ERROR] dpu_offload_parse_list_dpus() failed\n");
        goto error_out;
    }
    assert(engine);

    INIT_DPU_CONFIG_DATA(&cfg);
    strcpy(cfg.local_service_proc.hostname, argv[2]);
    rc = find_config_from_platform_configfile(argv[1], argv[2], &cfg);
    if (rc != DO_SUCCESS)
    {
        fprintf(stderr, "[ERROR] find_config_from_platform_configfile() failed\n");
        goto error_out;
    }

#if 1
    fprintf(stdout, "WARNING!!! We do not currently parse the entire configuration file "
            "so we do not have data about all the hosts that are involved\n");
#else
    {
        uint64_t host_hash_key;
        host_info_t *host_hash_value = NULL;

        fprintf(stdout, "Host(s) information:\n");
        fprintf(stdout, "\tNumber of hosts: %ld\n", cfg.num_hosts);
        fprintf(stdout, "\tList: ");
        for (i = 0; i < cfg.num_hosts; i++)
        {
            host_info_t *host_info = NULL;
            host_info = DYN_ARRAY_GET_ELT(&(cfg.hosts_config), i, host_info_t);
            assert(host_info);
            fprintf(stdout, "%s ", host_info->hostname);
        }
        fprintf(stdout, "\n\tLookup table content:\n");
        kh_foreach(cfg.host_lookup_table, host_hash_key, host_hash_value, {
            fprintf(stderr, "\tHost UID: 0x%lx, %s, index: %ld\n", host_hash_key, host_hash_value->hostname, host_hash_value->idx);
        }) fprintf(stdout, "\n");
    }
#endif

    // cfg.num_hosts needs to be set so cache lookups work. We should have at least
    // one host, the current one.
    assert(cfg.num_hosts > 0);

    fprintf(stderr, "%s: test succeeded\n", argv[0]);
    return EXIT_SUCCESS;
error_out:
    fprintf(stderr, "%s: test failed\n", argv[0]);
    return EXIT_FAILURE;
}