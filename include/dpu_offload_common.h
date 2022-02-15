//
// Copyright (c) 2021, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <pmix.h>
#include <ucp/api/ucp.h>

#ifndef DPU_OFFLOAD_COMMON_H
#define DPU_OFFLOAD_COMMON_H

/* Whether C compiler supports -fvisibility */
#define DPU_OFFLOAD_HAVE_VISIBILITY 1

#if DPU_OFFLOAD_HAVE_VISIBILITY == 1
#define DPU_OFFLOAD_EXPORT __attribute__((__visibility__("default")))
#else
#define DPU_OFFLOAD_EXPORT
#endif

#if defined(c_plusplus) || defined(__cplusplus)
extern "C"
{
#endif

    typedef pmix_info_t dpu_offload_info_t;

#define DPU_OFFLOAD_INFO_CREATE PMIX_INFO_CREATE
#define DPU_OFFLOAD_INFO_LOAD PMIX_INFO_LOAD
#define DPU_OFFLOAD_INFO_FREE PMIX_INFO_FREE

#define DPU_OFFLOAD_STRING PMIX_STRING

    typedef enum
    {
        DO_ERROR = UCS_ERR_NO_MESSAGE, // DPU offload generic error
        DO_SUCCESS = UCS_OK, // DPU offload ok
        DO_NOT_APPLICABLE = UCS_ERR_LAST - 1 // DPU offload not applicable feature
    } dpu_offload_status_t;

#define DPU_OFFLOAD_SUCCESS DO_SUCCESS
#define DPU_OFFLOAD_ERROR DO_ERROR

#if defined(c_plusplus) || defined(__cplusplus)
}
#endif

#endif /* DPU_OFFLOAD_COMMON_H */