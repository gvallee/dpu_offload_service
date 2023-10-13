//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#ifndef DPU_OFFLOAD_UTILS_H
#define DPU_OFFLOAD_UTILS_H

#if defined(c_plusplus) || defined(__cplusplus)
#    define _EXTERN_C_BEGIN extern "C" {
#    define _EXTERN_C_END   }
#else
#    define _EXTERN_C_BEGIN
#    define _EXTERN_C_END
#endif

#define GET_ENGINE_LIST_SERVICE_PROCS(_engine) ({   \
    dyn_array_t *_list_sps = NULL;                  \
    assert((_engine)->on_dpu);                      \
    _list_sps = &(_engine->dpu.service_procs);      \
    _list_sps;                                      \
})

#endif // DPU_OFFLOAD_UTILS_H