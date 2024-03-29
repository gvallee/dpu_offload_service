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

#define BUFF_AT(_ptr, _offset)                        \
    ((void *)((ptrdiff_t)(_ptr) + (off_t)(_offset)))

#define GET_ENGINE_LIST_SERVICE_PROCS(_engine) ({   \
    dyn_array_t *_list_sps = NULL;                  \
    _list_sps = &(_engine->service_procs);          \
    _list_sps;                                      \
})

#endif // DPU_OFFLOAD_UTILS_H