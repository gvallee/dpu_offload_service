//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <stdlib.h>
#include <stdio.h>

#include "dpu_offload_envvars.h"
#include "dpu_offload_service_daemon.h"

extern dpu_offload_status_t find_config_from_platform_configfile(char *filepath, char *hostname, offloading_config_t *data);
extern dpu_offload_status_t find_dpu_config_from_platform_configfile(char *filepath, offloading_config_t *config_data);
extern dpu_offload_status_t dpu_offload_parse_list_dpus(offloading_engine_t *engine, offloading_config_t *config_data);

int main(int argc, char **argv)
{
    dpu_offload_status_t rc;
    size_t i;
    offloading_engine_t *engine = NULL;
    offloading_config_t cfg;
    uint64_t host_hash_key;
    host_info_t *host_hash_value = NULL;
    size_t sp_gid;

    if (argc != 4)
    {
        fprintf(stderr, "Please give in order:\n");
        fprintf(stderr, "\t- the path to the configuration file to parse,\n");
        fprintf(stderr, "\t- the list of DPUs you wish to simulate from the config file (e.g., \"jupiterbf001.hpcadvisorycouncil.com,jupiterbf002.hpcadvisorycouncil.com\"\n");
        fprintf(stderr, "\t- the DPU on which we want to simulate the parsing of the configuration (e.g., jupiterbf001.hpcadvisorycouncil.com\n");
        fprintf(stderr, "The environment variable %s is also expected to be properly set\n", DPU_OFFLOAD_SERVICE_PROCESSES_PER_DPU_ENVVAR);
        return EXIT_FAILURE;
    }

    if (getenv(DPU_OFFLOAD_SERVICE_PROCESSES_PER_DPU_ENVVAR) == NULL)
    {
        fprintf(stderr, "ERROR: %s is not set, please properly set it\n", DPU_OFFLOAD_SERVICE_PROCESSES_PER_DPU_ENVVAR);
        return EXIT_FAILURE;
    }

    rc = offload_engine_init(&engine);
    if (rc != DO_SUCCESS)
    {
        fprintf(stderr, "[ERROR] offload_engine_init() failed\n");
        goto error_out;
    }
    assert(engine);

    INIT_DPU_CONFIG_DATA(&cfg);
    cfg.list_dpus = argv[2];
    strcpy(cfg.local_service_proc.hostname, argv[3]);

    // Manually set a few things since we test a fairly low-level API
    cfg.num_service_procs_per_dpu_str = getenv(DPU_OFFLOAD_SERVICE_PROCESSES_PER_DPU_ENVVAR);
    cfg.offloading_engine = engine;

    rc = dpu_offload_parse_list_dpus(engine, &cfg);
    if (rc != DO_SUCCESS)
    {
        fprintf(stderr, "[ERROR] dpu_offload_parse_list_dpus() failed\n");
        goto error_out;
    }
    if (cfg.num_service_procs == 0)
    {
        fprintf(stderr, "no SP identified\n");
        goto error_out;
    }
    if (cfg.num_dpus == 0)
    {
        fprintf(stderr, "No DPU identified\n");
        goto error_out;
    }

    // Check a few things after the first step
    for (sp_gid = 0; sp_gid < cfg.num_service_procs; sp_gid++)
    {
        remote_service_proc_info_t *sp = DYN_ARRAY_GET_ELT(GET_ENGINE_LIST_SERVICE_PROCS(engine),
                                                           sp_gid,
                                                           remote_service_proc_info_t);
        if (sp == NULL)
        {
            fprintf(stderr, "undefined SP\n");
            goto error_out;
        }
        if (sp->init_params.conn_params == NULL)
        {
            fprintf(stderr, "undefined conn_params\n");
            goto error_out;
        }
    }

    rc = find_dpu_config_from_platform_configfile(argv[1], &cfg);
    if (rc != DO_SUCCESS)
    {
        fprintf(stderr, "[ERROR] find_dpu_config_from_platform_configfile() failed\n");
        goto error_out;
    }

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

    fprintf(stdout, "\nHost(s) information:\n");
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
        fprintf(stderr, "\t\tHost UID: 0x%lx, %s, index: %ld\n", host_hash_key, host_hash_value->hostname, host_hash_value->idx);
    })
    fprintf(stdout, "\n");

    fprintf(stdout, "%s: test succeeded\n", argv[0]);
    return EXIT_SUCCESS;
error_out:
    fprintf(stderr, "%s: test failed\n", argv[0]);
    return EXIT_FAILURE;
}