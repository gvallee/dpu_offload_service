//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include "dpu_offload_types.h"
#include "dpu_offload_debug.h"

extern execution_context_t* server_init(offloading_engine_t *, conn_params_t *);

typedef enum {
    CONNECT_STATUS_UNKNOWN = 0,
    CONNECT_STATUS_CONNECTED,
    CONNECT_STATUS_DISCONNECTED
} connect_status_t;

typedef struct remote_dpu_info
{
    ucs_list_link_t item;
    conn_params_t conn_params;
    connect_status_t conn_status;
    pthread_t connection_tid;
} remote_dpu_info_t;

typedef struct dpu_inter_connect_info
{
    dyn_list_t *pool_remote_dpu_info;
    ucs_list_link_t connect_to;
    size_t num_connect_to;
} dpu_inter_connect_info_t;

/**
 * @brief dpu_offload_parse_list_dpus parses the list of DPUs to know which ones the DPU needs to
 * connect to and which one will connect to it. All DPUs before the DPU's hostname will connect to
 * it, those after, the DPU will connect to them. If the hostname of the system is not in the list,
 * the list is assumed not applicable and DO_NOT_APPLICABLE is returned.
 *
 * @param dpu_hostname Hostname of the current DPU.
 * @param list Comma separated list that includes the DPU hostname.
 * @param num_connecting_to Number of DPUs we need to connect to.
 * @param num_connecting_from Numer of DPUs that will be connecting to us.
 * @param dpu_to_connect_to Initialized dynamic list that will be populated with the list of DPUs we need to connect to.
 * @return dpu_offload_status_t
 */
static dpu_offload_status_t
dpu_offload_parse_list_dpus(char *dpu_hostname,
                            char *list,
                            dpu_inter_connect_info_t *info_connecting_to,
                            size_t *num_connecting_from)
{
    bool pre = true;
    bool list_connect_to_set = false;
    char *token;
    size_t n_connecting_from = 0;
    size_t n_connecting_to = 0;

    token = strtok(list, ",");

    while (token != NULL)
    {
        if (strcmp(token, dpu_hostname) == 0)
        {
            pre = false;
            continue;
        }

        if (pre == true)
            n_connecting_from++;
        else
        {
            remote_dpu_info_t *new_conn_to;
            DYN_LIST_GET(info_connecting_to->pool_remote_dpu_info, remote_dpu_info_t, item, new_conn_to);
            DBG("Adding DPU %s to the list of DPUs to connect to", token);
            new_conn_to->conn_params.addr_str = token;
            ucs_list_add_tail(&(info_connecting_to->connect_to), &(new_conn_to->item));
            info_connecting_to->num_connect_to++;
        }
    }

    if (pre == false)
    {
        // The hostname of the current system was not in the list. The list is not applicable.
        n_connecting_from = 0;
    }

    *num_connecting_from = n_connecting_from;

    return DO_SUCCESS;
}

static void *connect_thread(void *arg)
{
    remote_dpu_info_t *remote_dpu_info = (remote_dpu_info_t*)arg;
    if (remote_dpu_info == NULL)
    {
        ERR_MSG("Remote DPU info is NULL");
        pthread_exit(NULL);
    }

}

static dpu_offload_status_t
connect_to_dpus(offloading_engine_t *offload_engine, dpu_inter_connect_info_t *info_connect_to)
{
    // Create a connection thread for all the required connection
    remote_dpu_info_t *conn_info, *conn_info_next;
    ucs_list_for_each_safe(conn_info, conn_info_next, &(info_connect_to->connect_to), item)
    {
        int rc = pthread_create(&conn_info->connection_tid, NULL, &connect_thread, conn_info);
        CHECK_ERR_RETURN((rc), DO_ERROR, "unable to start connection thread");
    }
    return DO_SUCCESS;
}

dpu_offload_status_t inter_dpus_connect_mgr(offloading_engine_t *offload_engine, char *list_dpus, char *dpu_hostname)
{
    size_t num_dpus_connecting_from;
    dpu_inter_connect_info_t info_connect_to;
    info_connect_to.num_connect_to = 0;
    ucs_list_head_init(&(info_connect_to.connect_to));
    DYN_LIST_ALLOC(info_connect_to.pool_remote_dpu_info, 32, remote_dpu_info_t, item);

    dpu_offload_status_t rc = dpu_offload_parse_list_dpus(dpu_hostname, list_dpus, &info_connect_to, &num_dpus_connecting_from);
    CHECK_ERR_RETURN((rc == DO_ERROR), DO_ERROR, "dpu_offload_parse_list_dpus() failed");

    if (rc == DO_NOT_APPLICABLE)
    {
        // This is not running on a applicable DPU (DPU not on a list or a host)
        return DO_SUCCESS;
    }

    if (num_dpus_connecting_from > 0)
    {
        // Some DPUs will be connecting to us so we start a new server.
        execution_context_t *server = server_init(offload_engine, NULL);
        CHECK_ERR_RETURN((server == NULL), DO_ERROR, "server_init() failed");
        CHECK_ERR_RETURN((offload_engine->num_servers+1 < offload_engine->num_max_servers), DO_ERROR, "max number of server has been reached");
        offload_engine->servers[offload_engine->num_servers] = server->server;
        offload_engine->num_servers++;
        // Nothing else to do in this context.
    }
    if (info_connect_to.num_connect_to > 0)
    {
        // We need to connect to one or more other DPUs
        rc = connect_to_dpus(offload_engine, &info_connect_to);
        CHECK_ERR_RETURN((rc), DO_ERROR, "connect_to_dpus() failed");
    }

    return DO_SUCCESS;
}