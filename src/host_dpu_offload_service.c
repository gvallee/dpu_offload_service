//
// Copyright (c) 2021, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <signal.h>

#include "dpu_offload_common.h"
#include "dpu_offload_types.h"
#include "dpu_offload_envvars.h"

static inline void
check_config(offload_config_t *cfg)
{
    char hostname[64];
    hostname[63] = '\0';
    gethostname(hostname, 63);
    fprintf(stdout, "* Checking configuration (%s)...\n", hostname);
    if (cfg->associated_bluefield != NULL)
        fprintf(stdout, "-> Associated BlueField card: %s\n", cfg->associated_bluefield);

    fprintf(stdout, "\n");
}

dpu_offload_status_t host_offload_dpu_discover(offload_config_t *cfg)
{
    cfg->associated_bluefield = getenv(BLUEFIELD_NODE_NAME_ENVVAR);
    cfg->offload_config_file_path = getenv(OFFLOAD_CONFIG_FILE_PATH_ENVVAR);
    return DPU_OFFLOAD_SUCCESS;
}

dpu_offload_status_t host_offload_init(offload_config_t *cfg)
{
    int rc;

    rc = host_offload_dpu_discover(cfg);
    if (rc != DPU_OFFLOAD_SUCCESS) {
        return rc;
    }

    cfg->with_pmix = true;
    cfg->state = DPU_OFFLOAD_STATE_INITIALIZED;
    if (cfg->with_pmix == true)
    {
        cfg->infra.pmix.dvm_pid = -1;
        cfg->infra.pmix.service_pid = -1;
        cfg->infra.pmix.dvm_started = false;
        cfg->infra.pmix.service_started = false;
        cfg->infra.pmix.dvm_argc = 0;
        cfg->infra.pmix.dvm_argv = NULL;
        cfg->infra.pmix.run_argc = 0;
        cfg->infra.pmix.run_argv = NULL;
    }

    return DPU_OFFLOAD_SUCCESS;
}

static void dvm_start(offload_config_t *cfg)
{
    int rc;
    cfg->infra.pmix.dvm_argc = 5;
    cfg->infra.pmix.dvm_argv = (char**) calloc(cfg->infra.pmix.dvm_argc + 1, sizeof(char*));
    cfg->infra.pmix.dvm_argv[0] = strdup("/global/scratch/users/geoffroy/projects/pmix/x86/install/prrte/bin/prte");
    cfg->infra.pmix.dvm_argv[1] = strdup("--host");
    cfg->infra.pmix.dvm_argv[2] = strdup(cfg->associated_bluefield);
    cfg->infra.pmix.dvm_argv[3] = strdup("--prefix");
    cfg->infra.pmix.dvm_argv[4] = strdup("/global/scratch/users/geoffroy/projects/pmix/arm/install/prrte");
    cfg->infra.pmix.dvm_argv[cfg->infra.pmix.dvm_argc] = NULL;

    fprintf(stderr, "%s %d\n", __func__, __LINE__);
    cfg->infra.pmix.dvm_pid = fork();
    if (cfg->infra.pmix.dvm_pid == 0)
    {
        execv("/global/scratch/users/geoffroy/projects/pmix/x86/install/prrte/bin/prte", cfg->infra.pmix.dvm_argv);
    }

    fprintf(stdout, "[INFO] BlueField is now reachable for service bootstrapping (pid=%d)...\n", cfg->infra.pmix.dvm_pid);

}

static void wait_from_dvm_complete_bootstrap(offload_config_t *cfg)
{
    sleep(3);
}

static void prun_start(offload_config_t *cfg)
{
    int rc;
    if (cfg->infra.pmix.dvm_pid <= 0)
    {
        fprintf(stderr, "ERROR: invalid DVM pid (%d)\n", cfg->infra.pmix.dvm_pid);
    }
    char pid_str[8];
    snprintf(pid_str, 7, "%d", cfg->infra.pmix.dvm_pid);

    cfg->infra.pmix.run_argc = 6;
    cfg->infra.pmix.run_argv = calloc(cfg->infra.pmix.run_argc + 1, sizeof(char*));
    cfg->infra.pmix.run_argv[0] = strdup("/global/scratch/users/geoffroy/projects/pmix/x86/install/prrte/bin/prun");
    cfg->infra.pmix.run_argv[1] = strdup("--host");
    cfg->infra.pmix.run_argv[2] = strdup(cfg->associated_bluefield);
    //cfg->infra.pmix.run_argv[3] = strdup("--pid");
    //cfg->infra.pmix.run_argv[4] = strdup(pid_str);
    cfg->infra.pmix.run_argv[3] = strdup("-np");
    cfg->infra.pmix.run_argv[4] = strdup("1");
    cfg->infra.pmix.run_argv[5] = strdup("/usr/bin/hostname");
    cfg->infra.pmix.run_argv[cfg->infra.pmix.run_argc] = NULL;

    wait_from_dvm_complete_bootstrap(cfg);
    cfg->infra.pmix.service_pid = fork();
    if (cfg->infra.pmix.service_pid == 0)
    {
        execv("/global/scratch/users/geoffroy/projects/pmix/x86/install/prrte/bin/prun", cfg->infra.pmix.dvm_argv);
    }
    fprintf(stdout, "[INFO] Offloading service is now running on the BlueField...\n");
}

static inline dpu_offload_status_t
bootstrap_pmix_infrastructure(offload_config_t *cfg)
{
    int rc;
    
    // Sanity checks
    if (cfg->associated_bluefield == NULL) {
        fprintf(stderr, "ERROR: no associated BlueField DPU detected\n");
        return DPU_OFFLOAD_ERROR;
    }

    // Start DVM 
    dvm_start(cfg);

    // Start the service
    prun_start(cfg);

    sleep(10);

    return DPU_OFFLOAD_SUCCESS;
}

static inline dpu_offload_status_t
finalize_pmix_infrastructure(offload_config_t *cfg)
{
    int rc;
    if (cfg->infra.pmix.service_started == true)
    {
        if (cfg->infra.pmix.service_pid > 0)
        {
            kill(cfg->infra.pmix.service_pid, SIGKILL);
        }    
    }

    fprintf(stderr, "%d\n", __LINE__);
    if (cfg->infra.pmix.dvm_started == true)
    {
        if (cfg->infra.pmix.dvm_pid > 0)
        {
            kill(cfg->infra.pmix.dvm_pid, SIGKILL);
        }
        
    }

    if (cfg->infra.pmix.run_argv != NULL)
    {
        int i;
        for (i = 0; i < cfg->infra.pmix.run_argc - 1; i++)
        {
            if (cfg->infra.pmix.run_argv[i] != NULL)
            {
                free(cfg->infra.pmix.run_argv[i]);
                cfg->infra.pmix.run_argv[i] = NULL;
            }
        }
        free(cfg->infra.pmix.run_argv);
        cfg->infra.pmix.run_argv = NULL;
    }
    cfg->infra.pmix.run_argc = 0;

    if (cfg->infra.pmix.dvm_argv != NULL)
    {
        int i;
        for (i = 0; i < cfg->infra.pmix.dvm_argc - 1; i++)
        {
            if (cfg->infra.pmix.dvm_argv[i] != NULL)
            {
                free(cfg->infra.pmix.dvm_argv[i]);
                cfg->infra.pmix.dvm_argv[i] = NULL;
            }
        }
        free(cfg->infra.pmix.dvm_argv);
        cfg->infra.pmix.dvm_argv = NULL;
    }
    cfg->infra.pmix.dvm_argc = 0;

    fprintf(stderr, "%d\n", __LINE__);
    return DPU_OFFLOAD_SUCCESS;
}

dpu_offload_status_t dpu_offload_service_start(offload_config_t *cfg)
{
    int rc;
    if (cfg->with_pmix) {
        rc = bootstrap_pmix_infrastructure(cfg);
        if (rc != DPU_OFFLOAD_SUCCESS)
            return rc;
    } else {
        fprintf(stderr, "ERROR: non-PMIx configurations are not currently supported\n");
        return DPU_OFFLOAD_ERROR;
    }
    return DPU_OFFLOAD_SUCCESS;
}

dpu_offload_status_t dpu_offload_service_check(offload_config_t *cfg)
{
    check_config(cfg);
    return DPU_OFFLOAD_SUCCESS;
}

dpu_offload_status_t dpu_offload_service_end(offload_config_t *cfg)
{
    PMIx_Finalize (NULL, 0);
    cfg->state = DPU_OFFLOAD_STATE_FINALIZED;
    if (cfg->with_pmix == true)
    {
        finalize_pmix_infrastructure(cfg);
    }
    return DPU_OFFLOAD_SUCCESS;
}