//
// Copyright (c) 2021, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <stdbool.h>
#include <pthread.h>

#ifndef DPU_OFFLOAD_TYPES_H
#define DPU_OFFLOAD_TYPES_H

#if defined(c_plusplus) || defined(__cplusplus)
extern "C" {
#endif

typedef enum dpu_offload_state {
    DPU_OFFLOAD_STATE_UNKNOWN = 0,
    DPU_OFFLOAD_STATE_INITIALIZED,
    DPU_OFFLOAD_STATE_FINALIZED,
} dpu_offload_state_t;

typedef struct pmix_infrastructure {
    int dvm_argc;
    char **dvm_argv;
    int run_argc;
    char **run_argv;
    pid_t dvm_pid;
    pid_t service_pid;
    bool dvm_started;
    bool service_started;
} pmix_infrastructure_t;

typedef struct offload_config {
    dpu_offload_state_t state;
    char *offload_config_file_path;
    char *associated_bluefield;
    bool with_pmix;

    union {
        pmix_infrastructure_t pmix;
    } infra;
} offload_config_t;

#if defined(c_plusplus) || defined(__cplusplus)
}
#endif

#endif // DPU_OFFLOAD_TYPES_H