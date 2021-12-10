//
// Copyright (c) 2021, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#ifndef HOST_DPU_OFFLOAD_SERVICE_H
#define HOST_DPU_OFFLOAD_SERVICE_H

#include <stdio.h>
#include <pmix.h>
#include "dpu_offload_common.h"
#include "dpu_offload_types.h"

#if defined(c_plusplus) || defined(__cplusplus)
extern "C" {
#endif

DPU_OFFLOAD_EXPORT dpu_offload_status_t host_offload_init(offload_config_t *cfg);
DPU_OFFLOAD_EXPORT dpu_offload_status_t dpu_offload_service_start(offload_config_t *cfg);
DPU_OFFLOAD_EXPORT dpu_offload_status_t dpu_offload_service_check(offload_config_t *cfg);
DPU_OFFLOAD_EXPORT dpu_offload_status_t dpu_offload_service_end(offload_config_t *cfg);

#if defined(c_plusplus) || defined(__cplusplus)
}
#endif

#endif // HOST_DPU_OFFLOAD_SERVICE_H