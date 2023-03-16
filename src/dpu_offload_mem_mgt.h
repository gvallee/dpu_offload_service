//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <assert.h>
#include <stddef.h>

#include "dpu_offload_types.h"

#ifndef DPU_OFFLOAD_MEM_MGT_H__
#define DPU_OFFLOAD_MEM_MGT_H__

#define MAX_GROUPS (128)

#include "dynamic_structs.h"

#define DEFAULT_NUM_GROUPS (32)
#define DEFAULT_NUM_RANKS_IN_GROUP (2048)

#if NDEBUG
#define DPU_OFFLOAD_MALLOC(_size) ({ \
    void *_ptr = malloc((_size));    \
    _ptr;                            \
})
#else
#define DPU_OFFLOAD_MALLOC(_size) ({ \
    void *_ptr = malloc((_size));    \
    if (_ptr != NULL)                \
    {                                \
        memset(_ptr, 0xef, _size);   \
    }                                \
    _ptr;                            \
})
#endif // NDEBUG

#endif // DPU_OFFLOAD_MEM_MGT_H__
