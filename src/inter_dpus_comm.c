#define _POSIX_C_SOURCE 200809L

//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "dpu_offload_types.h"
#include "dpu_offload_debug.h"
#include "dpu_offload_envvars.h"

/**
 * @brief invalid_group_rank is used when there is a need to exchange
 * a group/rank but the current context does not have one. It is for
 * instance used during the inter-dpu connections.
 */
rank_info_t invalid_group_rank = {
    .group_id = INVALID_GROUP,
    .group_rank = INVALID_RANK,
};

extern execution_context_t *server_init(offloading_engine_t *, init_params_t *);
extern execution_context_t *client_init(offloading_engine_t *, init_params_t *);

#define SET_DPU_TO_CONNECT_TO(_econtext, _cfg, _dpu_hostname, _idx)                                         \
    do                                                                                                      \
    {                                                                                                       \
        remote_dpu_info_t *new_conn_to;                                                                     \
        DYN_LIST_GET(_cfg->info_connecting_to.pool_remote_dpu_info, remote_dpu_info_t, item, new_conn_to);  \
        assert(new_conn_to);                                                                                \
        new_conn_to->idx = _idx;                                                                            \
        remote_dpu_info_t **_list_dpus = (remote_dpu_info_t **)_econtext->dpus.base;                        \
        _list_dpus[_idx] = new_conn_to;                                                                     \
        conn_params_t *new_conn_params;                                                                     \
        DYN_LIST_GET(_cfg->offloading_engine->pool_conn_params, conn_params_t, item, new_conn_params);      \
        assert(new_conn_params);                                                                            \
        new_conn_to->hostname = _dpu_hostname;                                                              \
        new_conn_to->init_params.conn_params = new_conn_params;                                             \
        /* all connection parameters are not available at this point, we only have the list of hostnames */ \
        new_conn_to->init_params.conn_params->port_str = NULL;                                              \
        new_conn_to->init_params.conn_params->port = -1;                                                    \
        new_conn_to->offload_engine = _cfg->offloading_engine;                                              \
        ucs_list_add_tail(&(_cfg->info_connecting_to.connect_to), &(new_conn_to->item));                    \
        _cfg->info_connecting_to.num_connect_to++;                                                          \
    } while (0)

/**
 * @brief dpu_offload_parse_list_dpus parses the list of DPUs to know which ones the DPU needs to
 * connect to and which one will connect to it. All DPUs before the DPU's hostname will connect to
 * it, those after, the DPU will connect to them. If the hostname of the system is not in the list,
 * the list is assumed not applicable and DO_NOT_APPLICABLE is returned.
 * Note that the function ONLY gathers the list of the DPUs' hostname, the rest is extracted while
 * parsing the configuration file or other environment variables.
 *
 * @param[in] engine The offloading engine associated to the function call.
 * @param[in,out] config_data All the configuration details from which we get the list of DPUs and where the result is stored
 * @param[out] my_dpu_id Unique identifier assigned to the DPU, based on the index in the list.
 * @return dpu_offload_status_t
 */
static dpu_offload_status_t
dpu_offload_parse_list_dpus(offloading_engine_t *engine, offloading_config_t *config_data, uint64_t *my_dpu_id)
{
    size_t dpu_idx = 0;
    bool pre = true;
    bool list_connect_to_set = false;
    char *token;
    size_t n_connecting_from = 0;
    size_t n_connecting_to = 0;
    remote_dpu_info_t **dpu_info = (remote_dpu_info_t **)engine->dpus.base;

    token = strtok(config_data->list_dpus, ",");

    while (token != NULL)
    {
        dpu_config_data_t *dpu_config;
        DYN_ARRAY_GET_ELT(&(config_data->dpus_config), config_data->num_dpus, dpu_config_data_t, dpu_config);
        assert(dpu_config);
        dpu_config->version_1.hostname = strdup(token); // todo: correctly free
        config_data->num_dpus++;

        DBG("Checking hostname %s (i am %s)", token, config_data->local_dpu.hostname);
        if (strncmp(token, config_data->local_dpu.hostname, strlen(token)) == 0)
        {
            remote_dpu_info_t *new_remote_dpu;
            DBG("%s is me", token);
            pre = false;
            *my_dpu_id = dpu_idx;
            // We need an entry in the list of DPUs to support communications with self after a look up to a local rank.
            DYN_LIST_GET(config_data->info_connecting_to.pool_remote_dpu_info, remote_dpu_info_t, item, new_remote_dpu); // fixme: correctly return object
            new_remote_dpu->idx = dpu_idx;
            new_remote_dpu->hostname = config_data->local_dpu.hostname;
            new_remote_dpu->init_params.conn_params = NULL;
            dpu_info[dpu_idx] = new_remote_dpu; 
            dpu_idx++;
            token = strtok(NULL, ",");
            continue;
        }

        if (pre == true)
        {
            remote_dpu_info_t *new_remote_dpu;
            conn_params_t *new_conn_params;
            n_connecting_from++;
            DYN_LIST_GET(config_data->info_connecting_to.pool_remote_dpu_info, remote_dpu_info_t, item, new_remote_dpu); // fixme: correctly return object
            assert(new_remote_dpu);
            DYN_LIST_GET(engine->pool_conn_params, conn_params_t, item, new_conn_params); // fixme: correctly return object
            assert(new_conn_params);
            new_remote_dpu->idx = dpu_idx;
            new_remote_dpu->hostname = dpu_config->version_1.hostname;
            new_remote_dpu->init_params.conn_params = new_conn_params;
            // The address and port are not available at this point
            dpu_info[dpu_idx] = new_remote_dpu;
        }
        else
            SET_DPU_TO_CONNECT_TO(engine, config_data, dpu_config->version_1.hostname, dpu_idx);
        dpu_idx++;
        token = strtok(NULL, ",");
    }

    if (pre == true)
    {
        // The hostname of the current system was not in the list. The list is not applicable.
        DBG("I am not in the list of DPUs, not applicable to me");
        n_connecting_from = 0;
    }

    config_data->num_connecting_dpus = n_connecting_from;
    config_data->offloading_engine->num_dpus = dpu_idx;

    return DO_SUCCESS;
}

static int set_default_econtext(connected_peer_data_t *connected_peer_data)
{
    assert(connected_peer_data);
    assert(connected_peer_data->econtext);
    if (connected_peer_data->econtext->engine->default_econtext == NULL)
        connected_peer_data->econtext->engine->default_econtext = connected_peer_data->econtext;
}

void connected_to_server_dpu(void *data)
{
    assert(data);
    connected_peer_data_t *connected_peer = (connected_peer_data_t*)data;
    DBG("Successfully connected to server DPU at %s\n", connected_peer->peer_addr);
    set_default_econtext(connected_peer);

    /* Update data in the list of DPUs */
    assert(connected_peer->econtext);
    assert(connected_peer->econtext->engine);
    remote_dpu_info_t **list_dpus = LIST_DPUS_FROM_ECONTEXT(connected_peer->econtext);
    size_t i;
    for (i = 0; i < connected_peer->econtext->engine->num_dpus; i++)
    {
        if (list_dpus[i]->init_params.conn_params != NULL)
        {
            if (strncmp(connected_peer->peer_addr, list_dpus[i]->init_params.conn_params->addr_str, strlen(connected_peer->peer_addr)))
            {
                list_dpus[i]->econtext = connected_peer->econtext;
                break;
            }
        }
    }
}

static void *connect_thread(void *arg)
{
    remote_dpu_info_t *remote_dpu_info = (remote_dpu_info_t *)arg;
    if (remote_dpu_info == NULL)
    {
        ERR_MSG("Remote DPU info is NULL");
        pthread_exit(NULL);
    }
    offloading_engine_t *offload_engine = remote_dpu_info->offload_engine;
    if (offload_engine == NULL)
    {
        ERR_MSG("undefined offload_engine");
        pthread_exit(NULL);
    }

    DBG("connecting to DPU server %s at %s:%d",
        remote_dpu_info->hostname,
        remote_dpu_info->init_params.conn_params->addr_str,
        remote_dpu_info->init_params.conn_params->port);
    // Inter-DPU connection, no group/rank
    remote_dpu_info->init_params.proc_info = &invalid_group_rank;
    remote_dpu_info->init_params.connected_cb = connected_to_server_dpu;
    execution_context_t *client = client_init(offload_engine, &(remote_dpu_info->init_params));
    if (client == NULL)
    {
        ERR_MSG("Unable to connect to %s\n", remote_dpu_info->init_params.conn_params->addr_str);
        pthread_exit(NULL);
    }
    DBG("Connection successfully established");
    offload_engine->inter_dpus_clients[offload_engine->num_inter_dpus_clients] = client;
    remote_dpu_info_t **list_dpus = (remote_dpu_info_t **)offload_engine->dpus.base;
    list_dpus[remote_dpu_info->idx]->ep = client->client->server_ep;
    list_dpus[remote_dpu_info->idx]->econtext = client;
    list_dpus[remote_dpu_info->idx]->peer_addr = client->client->conn_data.oob.peer_addr;
    list_dpus[remote_dpu_info->idx]->ucp_worker = client->client->ucp_worker;
    assert(list_dpus[remote_dpu_info->idx]->peer_addr);
    DBG("-> DPU #%ld: addr=%s, port=%d, ep=%p, econtext=%p",
        remote_dpu_info->idx,
        list_dpus[remote_dpu_info->idx]->init_params.conn_params->addr_str,
        list_dpus[remote_dpu_info->idx]->init_params.conn_params->port,
        list_dpus[remote_dpu_info->idx]->ep,
        list_dpus[remote_dpu_info->idx]->econtext);

    ENGINE_LOCK(offload_engine);
    if (offload_engine->default_econtext == NULL)
        offload_engine->default_econtext = client;
    offload_engine->num_connected_dpus++;
    ENGINE_UNLOCK(offload_engine);
}

static dpu_offload_status_t
connect_to_dpus(offloading_engine_t *offload_engine, dpu_inter_connect_info_t *info_connect_to, init_params_t *init_params)
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

void client_dpu_connected(void *data)
{
    DBG("New client DPU is now connected");
    assert(data);
    connected_peer_data_t *connected_peer = (connected_peer_data_t*)data;
    set_default_econtext(connected_peer);
}

dpu_offload_status_t inter_dpus_connect_mgr(offloading_engine_t *engine, offloading_config_t *cfg)
{
    CHECK_ERR_RETURN((engine == NULL), DO_ERROR, "undefined engine");
    CHECK_ERR_RETURN((cfg == NULL), DO_ERROR, "undefined configuration");
    engine->on_dpu = true;
    DBG("Connection manager: expecting %ld inbound connections and %ld outbound connections", cfg->num_connecting_dpus, cfg->info_connecting_to.num_connect_to);

    if (cfg->num_connecting_dpus > 0)
    {
        // Some DPUs will be connecting to us so we start a new server.
        DBG("Starting server to let other DPUs connect to us (init_params=%p, conn_params=%p)...",
            &(cfg->local_dpu.interdpu_init_params),
            &(cfg->local_dpu.interdpu_init_params.conn_params));
        cfg->local_dpu.interdpu_init_params.connected_cb = client_dpu_connected;
        execution_context_t *server = server_init(cfg->offloading_engine, &(cfg->local_dpu.interdpu_init_params));
        CHECK_ERR_RETURN((server == NULL), DO_ERROR, "server_init() failed");
        CHECK_ERR_RETURN((cfg->offloading_engine->num_servers + 1 >= cfg->offloading_engine->num_max_servers),
                         DO_ERROR,
                         "max number of server (%ld) has been reached",
                         cfg->offloading_engine->num_max_servers);
        cfg->offloading_engine->servers[cfg->offloading_engine->num_servers] = server;
        cfg->offloading_engine->num_servers++;
        DBG("Server successfully started");
        // Nothing else to do in this context.
    }

    if (cfg->info_connecting_to.num_connect_to > 0)
    {
        // We need to connect to one or more other DPUs
        dpu_offload_status_t rc = connect_to_dpus(cfg->offloading_engine, &(cfg->info_connecting_to), &(cfg->local_dpu.interdpu_init_params));
        CHECK_ERR_RETURN((rc), DO_ERROR, "connect_to_dpus() failed");
    }

    return DO_SUCCESS;
}

dpu_offload_status_t get_dpu_config(offloading_engine_t *offload_engine, offloading_config_t *config_data)
{
    dpu_offload_status_t rc;
    uint64_t my_dpu_id;
    config_data->config_file = getenv(OFFLOAD_CONFIG_FILE_PATH_ENVVAR);

    config_data->local_dpu.hostname[1023] = '\0';
    gethostname(config_data->local_dpu.hostname, 1023);

    config_data->list_dpus = getenv(LIST_DPUS_ENVVAR);
    CHECK_ERR_RETURN((config_data->list_dpus == NULL),
                     DO_ERROR,
                     "Unable to get list of DPUs via %s environmnent variable\n",
                     LIST_DPUS_ENVVAR);
    rc = dpu_offload_parse_list_dpus(offload_engine, config_data, &my_dpu_id);
    CHECK_ERR_RETURN((rc == DO_ERROR), DO_ERROR, "dpu_offload_parse_list_dpus() failed");
    config_data->local_dpu.id = my_dpu_id;

    DBG("Number of DPUs to connect to: %ld; number of expected incoming connections: %ld; my unique ID: %" PRIu64 "\n",
        config_data->info_connecting_to.num_connect_to,
        config_data->num_connecting_dpus,
        config_data->local_dpu.id);

    config_data->local_dpu.interdpu_init_params.worker = NULL;
    config_data->local_dpu.interdpu_init_params.proc_info = NULL;
    config_data->local_dpu.host_init_params.worker = NULL;
    config_data->local_dpu.host_init_params.proc_info = NULL;

    /* First, we check whether we know about a configuration file. If so, we load all the configuration details from it */
    /* If there is no configuration file, we try to configuration from environment variables */
    if (config_data->config_file != NULL)
    {
        DBG("Looking for %s's configuration data from %s", config_data->local_dpu.hostname, config_data->config_file);
        rc = find_dpu_config_from_platform_configfile(config_data->config_file, config_data);
        CHECK_ERR_RETURN((rc), DO_ERROR, "find_dpu_config_from_platform_configfile() failed");
    }
    else
    {
        DBG("No configuration file");
        char *port_str = getenv(INTER_DPU_PORT_ENVVAR);
        config_data->local_dpu.interdpu_conn_params.addr_str = getenv(INTER_DPU_ADDR_ENVVAR);
        CHECK_ERR_RETURN((config_data->local_dpu.interdpu_conn_params.addr_str == NULL), DO_ERROR, "%s is not set, please set it\n", INTER_DPU_ADDR_ENVVAR);

        config_data->local_dpu.interdpu_conn_params.port = DEFAULT_INTER_DPU_CONNECT_PORT;
        if (port_str)
            config_data->local_dpu.interdpu_conn_params.port = atoi(port_str);
    }

    DBG("%ld DPU configuration(s) detected, connecting to %ld DPUs and expecting %ld inbound connections",
        config_data->num_dpus,
        config_data->info_connecting_to.num_connect_to,
        config_data->num_connecting_dpus);
    DBG("My configuration: addr: %s, inter-dpu port: %d, host port: %d",
        config_data->local_dpu.interdpu_conn_params.addr_str,
        config_data->local_dpu.interdpu_conn_params.port,
        config_data->local_dpu.host_conn_params.port);

    return DO_SUCCESS;
}