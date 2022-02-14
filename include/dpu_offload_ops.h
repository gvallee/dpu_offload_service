//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <inttypes.h>

#include "dpu_offload_types.h"

#ifndef DPU_OFFLOAD_OPS_H
#define DPU_OFFLOAD_OPS_H

/*
 * The registration of an operation only allows the offloading engine to know the definition
 * of an offloaded operation. It does not instanciate it. Once registered, it can be used to
 * start and execute many operations. An oepration is defined by three high-level functions that
 * can be used by developers: init, progress, complete.
 * To execute an operation, one must get a descriptor. For that, the identifier returned during
 * the registration must be provided. Once the descriptor is acquired, developers can customized
 * the descriptor with run-time parameters and submit it for execution.
 */

/**
 * @brief Register a new operation, i.e., implementation of an algorithm in the offload engine.
 *
 * @param engine Offload engine on the host or DPU where the operation needs to be registered.
 * @param op Description of the operation.
 * @return uint64_t
 */
uint64_t register_new_op(offloading_engine_t *engine, offload_op_t *op);

/**
 * @brief Get a descriptor for the execution of a new operation
 *
 * @param engine Associated offload engine.
 * @param id Unique identifier associated with the operation. For collective operation, it must be the same for all app processes.
 * @param op_id Registration identifier returned by register_new_op().
 * @param desc Returned descriptor. The operation is active only once submited.
 * @return dpu_offload_status_t
 */
dpu_offload_status_t op_desc_get(offloading_engine_t *engine, const uint64_t id, uint64_t op_id, op_desc_t **desc);

/**
 * @brief Start the execution of a operation descriptor.
 *
 * @param engine Associated offload engine.
 * @param desc Descriptor of the operation to execute.
 * @return dpu_offload_status_t
 */
dpu_offload_status_t op_desc_submit(offloading_engine_t *engine, op_desc_t *desc);

/**
 * @brief Return the descriptor of a completed operation. Returning a non-completed operation
 * is an error.
 *
 * @param engine Associated offload engine.
 * @param desc Descriptor of the operation to return. In case of error, the descriptor is in an undefined step; upon success the descriptor shall be NULL.
 * @return dpu_offload_status_t
 */
dpu_offload_status_t op_desc_return(offloading_engine_t *engine, op_desc_t **desc);

#endif // DPU_OFFLOAD_OPS_H