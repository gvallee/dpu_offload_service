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
#include "dpu_offload_event_channels.h"
#include "dpu_offload_service_daemon.h"

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
        RESET_REMOTE_DPU_INFO(new_conn_to);                                                                 \
        new_conn_to->idx = _idx;                                                                            \
        remote_dpu_info_t **_list_dpus = (remote_dpu_info_t **)_econtext->dpus.base;                        \
        _list_dpus[_idx] = new_conn_to;                                                                     \
        conn_params_t *new_conn_params;                                                                     \
        DYN_LIST_GET(_cfg->offloading_engine->pool_conn_params, conn_params_t, item, new_conn_params);      \
        assert(new_conn_params);                                                                            \
        RESET_CONN_PARAMS(new_conn_params);                                                                 \
        new_conn_to->hostname = _dpu_hostname;                                                              \
        new_conn_to->init_params.conn_params = new_conn_params;                                             \
        /* all connection parameters are not available at this point, we only have the list of hostnames */ \
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
        dpu_config_data_t *dpu_config = DYN_ARRAY_GET_ELT(&(config_data->dpus_config), config_data->num_dpus, dpu_config_data_t);
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
            assert(new_remote_dpu);
            RESET_REMOTE_DPU_INFO(new_remote_dpu);
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
            RESET_REMOTE_DPU_INFO(new_remote_dpu);
            DYN_LIST_GET(engine->pool_conn_params, conn_params_t, item, new_conn_params); // fixme: correctly return object
            assert(new_conn_params);
            RESET_CONN_PARAMS(new_conn_params);
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
    assert(connected_peer_data->econtext->engine);
    offloading_engine_t *engine = connected_peer_data->econtext->engine;
    ENGINE_LOCK(engine);
    if (connected_peer_data->econtext->engine->default_econtext == NULL)
        connected_peer_data->econtext->engine->default_econtext = connected_peer_data->econtext;
    ENGINE_UNLOCK(engine);
}

static void sync_group_caches(execution_context_t *econtext)
{
    size_t n = 0;
    size_t n_gp_cache_handled = 0;
    offloading_engine_t *engine;
    dpu_offload_status_t rc;

    assert(econtext);
    engine = econtext->engine;
    assert(engine);

    while (n < engine->procs_cache.size && n_gp_cache_handled < engine->procs_cache.size)
    {
        group_cache_t *gp_cache = GET_GROUP_CACHE(&(engine->procs_cache), n);
        if (gp_cache->initialized == false)
        {
            n++;
            continue;
        }

        if (gp_cache->n_local_ranks >= 0)
        {
            // We know how many local ranks to expected, we exchange the cache only when they are all connected
            if (gp_cache->n_local_ranks_populated == gp_cache->n_local_ranks)
            {
                rc = broadcast_group_cache(engine, n);
                if (rc != DO_SUCCESS)
                {
                    ERR_MSG("broadcast_group_cache() failed");
                    return;
                }
            }
        }
        else
        {
            // If we do not know how many local ranks to expect, we broadcast the group cache to all other DPUs
            rc = broadcast_group_cache(engine, n);
            if (rc != DO_SUCCESS)
            {
                ERR_MSG("broadcast_group_cache() failed");
                return;
            }
        }
        n_gp_cache_handled++;
        n++;
    }
}

/**
 * @brief Callback invoked when a connection to a remote server DPU completes
 *
 * @param data
 */
void connected_to_server_dpu(void *data)
{
    assert(data);
    bool can_exchange_cache = false;
    connected_peer_data_t *connected_peer = (connected_peer_data_t *)data;
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
    if (connected_peer->econtext->engine->num_connected_dpus + 1 == connected_peer->econtext->engine->num_dpus)
        can_exchange_cache = true;
    connected_peer->econtext->engine->num_connected_dpus++;
    DBG("we now have %ld connections with other DPUs", connected_peer->econtext->engine->num_connected_dpus);

    // If we now have all the connections with the other DPUs, we exchange our cache
    if (can_exchange_cache)
        sync_group_caches(connected_peer->econtext);
}

dpu_offload_status_t connect_to_remote_dpu(remote_dpu_info_t *remote_dpu_info)
{
    CHECK_ERR_RETURN((remote_dpu_info == NULL), DO_ERROR, "Remote DPU info is NULL");
    offloading_engine_t *offload_engine = remote_dpu_info->offload_engine;
    CHECK_ERR_RETURN((offload_engine == NULL), DO_ERROR, "undefined offload_engine");

    DBG("connecting to DPU server %s at %s:%d",
        remote_dpu_info->hostname,
        remote_dpu_info->init_params.conn_params->addr_str,
        remote_dpu_info->init_params.conn_params->port);
    // Inter-DPU connection, no group/rank
    remote_dpu_info->init_params.proc_info = &invalid_group_rank;
    remote_dpu_info->init_params.connected_cb = connected_to_server_dpu;
    remote_dpu_info->init_params.scope_id = SCOPE_INTER_DPU;
    execution_context_t *client = client_init(offload_engine, &(remote_dpu_info->init_params));
    CHECK_ERR_RETURN((client == NULL), DO_ERROR, "Unable to connect to %s\n", remote_dpu_info->init_params.conn_params->addr_str);
    ENGINE_LOCK(offload_engine);
    offload_engine->inter_dpus_clients[offload_engine->num_inter_dpus_clients].client_econtext = client;
    offload_engine->inter_dpus_clients[offload_engine->num_inter_dpus_clients].remote_dpu_info = remote_dpu_info;
    offload_engine->num_inter_dpus_clients++;
    ENGINE_UNLOCK(offload_engine);
    return DO_SUCCESS;
}

static dpu_offload_status_t
connect_to_dpus(offloading_engine_t *offload_engine, dpu_inter_connect_info_t *info_connect_to, init_params_t *init_params)
{
    // Create a connection thread for all the required connection
    remote_dpu_info_t *conn_info, *conn_info_next;
    ucs_list_for_each_safe(conn_info, conn_info_next, &(info_connect_to->connect_to), item)
    {
        // Initiate the connection to the remote DPU. This is a non-blocking operation,
        // meaning there is no guarantee the connection will be fully established when
        // the function returns
        dpu_offload_status_t rc = connect_to_remote_dpu(conn_info);
        CHECK_ERR_RETURN((rc), DO_ERROR, "unable to start connection thread");
    }
    return DO_SUCCESS;
}

/**
 * @brief Callback invoked when a client DPU finalizes its connection to us.
 *
 * @param data DPU data
 */
void client_dpu_connected(void *data)
{
    connected_peer_data_t *connected_peer;
    remote_dpu_info_t **list_dpus;
    dpu_offload_status_t rc;
    dpu_offload_event_t *exchange_ev;
    bool can_exchange_cache = false;

    assert(data);
    connected_peer = (connected_peer_data_t *)data;
    DBG("New client DPU (DPU #%" PRIu64 ") is now connected", connected_peer->peer_id);

    // Set the default econtext if necessary, the function will figure out what to do
    set_default_econtext(connected_peer);

    // Update data in the list of DPUs
    assert(connected_peer->econtext);
    assert(connected_peer->econtext->engine);
    list_dpus = LIST_DPUS_FROM_ENGINE(connected_peer->econtext->engine);
    assert(list_dpus);
    list_dpus[connected_peer->peer_id]->peer_addr = connected_peer->peer_addr;
    list_dpus[connected_peer->peer_id]->econtext = connected_peer->econtext;
    ENGINE_LOCK(connected_peer->econtext->engine);
    if (connected_peer->econtext->engine->num_connected_dpus + 1 == connected_peer->econtext->engine->num_dpus)
        can_exchange_cache = true;
    connected_peer->econtext->engine->num_connected_dpus++;
    DBG("we now have %ld connections with other DPUs", connected_peer->econtext->engine->num_connected_dpus);
    ENGINE_UNLOCK(connected_peer->econtext->engine);

    // If we now have all the connections with the other DPUs, we exchange our cache
    if (can_exchange_cache)
        sync_group_caches(connected_peer->econtext);
}

dpu_offload_status_t inter_dpus_connect_mgr(offloading_engine_t *engine, offloading_config_t *cfg)
{
    CHECK_ERR_RETURN((engine == NULL), DO_ERROR, "undefined engine");
    CHECK_ERR_RETURN((cfg == NULL), DO_ERROR, "undefined configuration");
    engine->on_dpu = true;
    DBG("Connection manager: expecting %ld inbound connections and %ld outbound connections",
        cfg->num_connecting_dpus, cfg->info_connecting_to.num_connect_to);

    /* Init UCP if necessary */
    engine->ucp_context = INIT_UCX();
    DBG("UCX successfully initialized, context: %p", engine->ucp_context);

    // Create a self worker
    INIT_WORKER(engine->ucp_context, &(engine->ucp_worker));

    /* self EP */
    static ucp_address_t *local_addr;
    size_t local_addr_len;
    ucp_worker_get_address(engine->ucp_worker, &local_addr, &local_addr_len);

    ucp_ep_params_t ep_params;
    ep_params.field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS;
    /*
                           | UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE
                           | UCP_EP_PARAM_FIELD_ERR_HANDLER
                           | UCP_EP_PARAM_FIELD_USER_DATA;
    */
    ep_params.address = local_addr;
    // ep_params.err_mode = err_handling_opt.ucp_err_mode;
    // ep_params.err_handler.cb = err_cb;
    // ep_params.err_handler.arg = NULL;
    // ep_params.user_data = &(client->server_ep_status);
    ucp_ep_h self_ep;
    ucs_status_t status = ucp_ep_create(engine->ucp_worker, &ep_params, &self_ep);
    CHECK_ERR_RETURN((status != UCS_OK), DO_ERROR, "ucp_ep_create() failed");
    engine->self_ep = self_ep; // fixme: correctly free

    remote_dpu_info_t **list_dpus = LIST_DPUS_FROM_ENGINE(engine);
    list_dpus[cfg->local_dpu.id]->ep = engine->self_ep;

    if (cfg->num_connecting_dpus > 0)
    {
        // Some DPUs will be connecting to us so we start a new server.
        DBG("Starting server to let other DPUs connect to us (init_params=%p, conn_params=%p)...",
            &(cfg->local_dpu.interdpu_init_params),
            &(cfg->local_dpu.interdpu_init_params.conn_params));
        cfg->local_dpu.interdpu_init_params.connected_cb = client_dpu_connected;
        cfg->local_dpu.interdpu_init_params.scope_id = SCOPE_INTER_DPU;
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
    CHECK_ERR_RETURN((offload_engine == NULL), DO_ERROR, "undefined offloading engine");
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

    offload_engine->config = (struct offloading_config *)config_data;

    return DO_SUCCESS;
}