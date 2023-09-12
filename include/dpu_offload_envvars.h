//
// Copyright (c) 2021, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#ifndef DPU_OFFLOAD_ENVVARS_H
#define DPU_OFFLOAD_ENVVARS_H

// Legacy environment variable to specify the path to the configuration file
#define OFFLOAD_CONFIG_FILE_PATH_ENVVAR "OFFLOAD_CONFIG_FILE_PATH"
// Equivalent of OFFLOAD_CONFIG_FILE_PATH_ENVVAR but with the full DPU_OFFLOAD prefix
#define DPU_OFFLOAD_CONFIG_FILE_PATH_ENVVAR "DPU_OFFLOAD_CONFIG_FILE_PATH"

#define BLUEFIELD_NODE_NAME_ENVVAR "BLUEFIELD_NODE_NAME"

#define SERVER_PORT_ENVVAR "DPU_OFFLOAD_SERVER_PORT"
#define SERVER_IP_ADDR_ENVVAR "DPU_OFFLOAD_SERVER_ADDR"

#define INTER_DPU_ADDR_ENVVAR "INTER_DPU_CONN_ADDR"
#define INTER_DPU_PORT_ENVVAR "INTER_DPU_CONN_PORT"

/**
 * @brief Environment variable defining the list of DPUs to be used
 * for offloading. When used, it should be the exact same list
 * for all DPUs to be used.
 */
#define LIST_DPUS_ENVVAR "DPU_OFFLOAD_LIST_DPUS"

/**
 * @brief Environment variable defining the global ID of the service process.
 * It should be only in the context of a service process and will be used,
 * in conjunction with the list of DPUs to determine the configuration.
 * If not defined, it is assumed that a single service process is running
 * on the DPU.
 */
#define DPU_OFFLOAD_SERVICE_PROCESS_GLOBAL_ID_ENVVAR "DPU_OFFLOAD_SERVICE_PROCESS_GLOBAL_ID"

/**
 * @brief Environment variable defining the local ID of the service process.
 * If not defined, it is assumed that a single service process is running
 * on the DPU.
 */
#define DPU_OFFLOAD_SERVICE_PROCESS_LOCAL_ID_ENVVAR "DPU_OFFLOAD_SERVICE_PROCESS_LOCAL_ID"

/**
 * @brief Environment variable defining the number of service processes per DPU. At the
 * moment, it is assumed that the same number of service processes are running on all DPUs.
 */
#define DPU_OFFLOAD_SERVICE_PROCESSES_PER_DPU_ENVVAR "DPU_OFFLOAD_SERVICE_PROCESSES_PER_DPU"

/**
 * @brief Environment variable defining whether the endpoint cache used by the topology feature is
 * persistent.
 */
#define MIMOSA_PERSISTENT_CACHE "MIMOSA_PERSISTENT_CACHE"

#define DPU_OFFLOAD_DBG_VERBOSE "DPU_OFFLOAD_DBG_VERBOSE"

#endif // DPU_OFFLOAD_ENVVARS_H