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
    .group_uid = INT_MAX,
    .group_rank = INVALID_RANK,
};

extern execution_context_t *server_init(offloading_engine_t *, init_params_t *);
extern execution_context_t *client_init(offloading_engine_t *, init_params_t *);

// The function prepare the resources to track all the SPs running on a specific DPU.
static dpu_offload_status_t
add_local_sps_to_dpu_config(offloading_config_t *cfg, remote_dpu_info_t *dpu)
{
    // Handle used to access the unique object gathering all the data about an SP.
    remote_service_proc_info_t *sp = NULL;
    // Handle used to access the pointer used at the DPU level to track SPs running
    // on that DPU.
    uint64_t *local_dpu_sp;
    uint64_t sp_gid;
    size_t dpu_index, sp_index;

    assert(cfg);
    assert(cfg->offloading_engine);
    assert(dpu);
    assert(cfg->num_service_procs_per_dpu > 0);

    dpu_index = dpu->idx;

    for (sp_index = 0; sp_index < cfg->num_service_procs_per_dpu; sp_index++)
    {
        sp_gid = dpu->idx * cfg->num_service_procs_per_dpu + sp_index;
        /* Get the SP's object from the list at the engine level */
        sp = DYN_ARRAY_GET_ELT(GET_ENGINE_LIST_SERVICE_PROCS(cfg->offloading_engine),
                               sp_gid,
                               remote_service_proc_info_t);
        assert(sp);
        sp->idx = sp_gid;
        conn_params_t *sp_conn_params = NULL;
        DYN_LIST_GET(cfg->offloading_engine->pool_conn_params, conn_params_t, item, sp_conn_params);
        assert(sp_conn_params);
        RESET_CONN_PARAMS(sp_conn_params);
        sp->dpu = dpu;
        sp->init_params.conn_params = sp_conn_params;
        sp->service_proc.local_id = sp_index;
        sp->service_proc.global_id = sp_gid;
        sp->offload_engine = cfg->offloading_engine;
        /* Add a pointer to the SP at the DPU level */
        local_dpu_sp = DYN_ARRAY_GET_ELT(&(dpu->local_service_procs), sp_index, uint64_t);
        *local_dpu_sp = sp->service_proc.global_id;
    }

    return DO_SUCCESS;
}

#define SET_REMOTE_SP_TO_CONNECT_TO(_cfg, _sp_gid)                                              \
    do                                                                                          \
    {                                                                                           \
        /* Connection details are set while parsing the config file */                          \
        uint64_t *_sp_gid_connect_to = NULL;                                                    \
        _sp_gid_connect_to = DYN_ARRAY_GET_ELT(&((_cfg)->info_connecting_to.sps_connect_to),    \
                                               (_cfg)->info_connecting_to.num_connect_to,       \
                                               uint64_t);                                       \
        *_sp_gid_connect_to = _sp_gid;                                                          \
        (_cfg)->info_connecting_to.num_connect_to++;                                            \
    } while (0)

#define SET_SERVICE_PROC_TO_CONNECT_TO(_engine, _cfg, _remote_dpu)                              \
    do                                                                                          \
    {                                                                                           \
        size_t _s;                                                                              \
        for (_s = 0; _s < (_cfg)->num_service_procs_per_dpu; _s++)                              \
        {                                                                                       \
            uint64_t sp_global_id = _remote_dpu->idx * (_cfg)->num_service_procs_per_dpu + _s;  \
            SET_REMOTE_SP_TO_CONNECT_TO(_cfg, sp_global_id);                                    \
        }                                                                                       \
        (_cfg)->info_connecting_to.num_dpus++;                                                  \
    } while (0)

/**
 * @brief dpu_offload_parse_list_dpus parses the list of DPUs from the environment (not the
 * configuration file) to know how service processes are supposed to connect to each other.
 * From the list of DPUs and the number of service processes per DPU, we know the total number
 * of service processes and can order them based on the context of the configuration files.
 * All service processes before the current service process will connect to
 * it, those after will connect to them. If the hostname of the system is not in the list,
 * the list is assumed not applicable and DO_NOT_APPLICABLE is returned.
 * Note that the function gathers the list of the DPUs' hostname and prepare the structures
 * for all the service processes. The details are extracted while parsing the configuration file or
 * other environment variables.
 *
 * @param[in] engine The offloading engine associated to the function call.
 * @param[in,out] config_data All the configuration details from which we get the list of DPUs and where the result is stored
 * @return dpu_offload_status_t
 */
dpu_offload_status_t
dpu_offload_parse_list_dpus(offloading_engine_t *engine, offloading_config_t *config_data)
{
    bool pre = true;
    char *token;
    size_t n_connecting_from = 0;
    dpu_offload_status_t rc;

    assert(engine);
    assert(config_data);
    assert(config_data->local_service_proc.hostname);
    if (config_data->offloading_engine == NULL)
        config_data->offloading_engine = engine;

    // Once we get here, we know we have the basic data to set some of the internal
    // variable and instead of spreading the code throughout the code, we do it here.
    // So it is not strictly about parsing the list of DPUs that is passed in but
    // directly related to it.
    if (config_data->local_service_proc.info.global_id_str == NULL)
    {
        // The global ID will be set while parsing the list of DPUs, assuming one service process per DPU
        // At the moment, we assume all environment variables for the service processes are set or none
        config_data->local_service_proc.info.global_id = UINT64_MAX;
        config_data->local_service_proc.info.local_id = 0;
        config_data->num_service_procs_per_dpu = 1;
    }

    if (config_data->local_service_proc.info.global_id_str != NULL)
    {
        config_data->local_service_proc.info.global_id = strtoull(config_data->local_service_proc.info.global_id_str,
                                                                  NULL, 0);
        // If the global ID env var is set, the local ID one is mandatory
        assert(config_data->local_service_proc.info.local_id_str != NULL);
    }
    if (config_data->local_service_proc.info.local_id_str != NULL)
    {
        config_data->local_service_proc.info.local_id = strtoull(config_data->local_service_proc.info.local_id_str,
                                                                 NULL, 0);
        // If the local ID env var is set, the one for the number of SP per DPU is also mandatory
        assert(config_data->num_service_procs_per_dpu_str != NULL);
    }
    if (config_data->num_service_procs_per_dpu_str != NULL)
    {
        config_data->num_service_procs_per_dpu = strtoull(config_data->num_service_procs_per_dpu_str,
                                                          NULL, 0);
    }

    token = strtok(config_data->list_dpus, ",");

    // Iterate over all the DPUs
    while (token != NULL)
    {
        // Update the data in the list of DPUs' configurations
        dpu_config_data_t *dpu_config = DYN_ARRAY_GET_ELT(&(config_data->dpus_config), config_data->num_dpus, dpu_config_data_t);
        assert(dpu_config);
        dpu_config->version_1.hostname = strdup(token); // freed when calling offload_config_free()

        // Update the hostname in the engine's list of known DPUs. Will be used when parsing the configuration file
        remote_dpu_info_t *d_info = NULL;
        d_info = DYN_ARRAY_GET_ELT(&(engine->dpus), config_data->num_dpus, remote_dpu_info_t);
        assert(d_info);
        DYN_ARRAY_ALLOC(&(d_info->local_service_procs), 32, uint64_t);
        d_info->hostname = dpu_config->version_1.hostname;
        d_info->idx = config_data->num_dpus;
        d_info->config = dpu_config;

        DBG("** hostname of DPU #%ld: %s", config_data->num_dpus, dpu_config->version_1.hostname);
        rc = add_local_sps_to_dpu_config(config_data, d_info);
        CHECK_ERR_RETURN((rc != DO_SUCCESS), DO_ERROR, "add_local_sps_to_dpu_config() failed");

        DBG("Checking hostname %s (i am %s)", token, config_data->local_service_proc.hostname);
        if (strncmp(token, config_data->local_service_proc.hostname, strlen(token)) == 0)
        {
            DBG("%s is the current DPU", token);
            pre = false;
            config_data->local_service_proc.info.dpu = config_data->num_dpus;
            if (config_data->local_service_proc.info.global_id == UINT64_MAX)
            {
                // the global ID of the service proc was not set, we assume one service proc per DPU
                config_data->local_service_proc.info.global_id = config_data->num_dpus;
            }
            // We need an entry in the list of DPUs to support communications with self after a look up to a local rank.
            // Each DPU can have multiple service procs
            d_info->idx = config_data->num_dpus;
            d_info->hostname = config_data->local_service_proc.hostname;

            // Adjust the number of service processes to connect to and expected to connect to us based
            // on our local ID and the number of service processes per DPU
            size_t n_sp_connecting_to;
            n_connecting_from += config_data->local_service_proc.info.local_id;
            n_sp_connecting_to = config_data->num_service_procs_per_dpu -
                                 config_data->local_service_proc.info.local_id -
                                 1;

            // Update the list of SPs we need to connect to with the other SPs on the same DPU that require me to
            // connect to them
            if (n_sp_connecting_to > 0)
            {
                size_t local_sp;
                for (local_sp = 0; local_sp < n_sp_connecting_to; local_sp++)
                {
                    size_t global_sp_id = config_data->local_service_proc.info.global_id + local_sp + 1;
                    SET_REMOTE_SP_TO_CONNECT_TO(config_data, global_sp_id);
                }
            }

            token = strtok(NULL, ",");
            config_data->num_dpus++;
            continue;
        }

        if (pre == true)
        {
            // Service process that will connect to us.
            n_connecting_from += config_data->num_service_procs_per_dpu;
        }
        else
        {
            // Service proc we will connect to
            SET_SERVICE_PROC_TO_CONNECT_TO(engine, config_data, d_info);
        }

        token = strtok(NULL, ",");
        config_data->num_dpus++;
    }

    if (pre == true)
    {
        // The hostname of the current system was not in the list. The list is not applicable.
        DBG("I am not in the list of DPUs, not applicable to me");
        n_connecting_from = 0;
    }

    config_data->num_connecting_service_procs = n_connecting_from;
    config_data->offloading_engine->num_dpus = config_data->num_dpus;
    config_data->num_service_procs = config_data->offloading_engine->num_service_procs = config_data->offloading_engine->num_dpus * config_data->num_service_procs_per_dpu;
    return DO_SUCCESS;
}

static void set_default_econtext(connected_peer_data_t *connected_peer_data)
{
    assert(connected_peer_data);
    assert(connected_peer_data->econtext);
    assert(connected_peer_data->econtext->engine);
}

/**
 * @brief Callback invoked when a connection to a remote server running in the context of service process on a DPU completes.
 * This is executed in the context of clients on DPUs that are used to connect to servers on other DPUs.
 * In other words, client->connected_cb is set to this pointer.
 *
 * @param data Generic pointer to pass callback data
 */
void connected_to_server_dpu(void *data)
{
    assert(data);
    bool can_exchange_cache = false;
    connected_peer_data_t *connected_peer = (connected_peer_data_t *)data;

    DBG("Successfully connected to remote service process %" PRIu64 " running in the context server SP #%" PRIu64 " at %s (econtext: %p)",
        connected_peer->global_peer_id, connected_peer->peer_id, connected_peer->addr, connected_peer->econtext);
    set_default_econtext(connected_peer);

    assert(connected_peer->econtext);
    assert(connected_peer->econtext->engine);
    if (connected_peer->econtext->engine->num_connected_service_procs + 1 == connected_peer->econtext->engine->num_service_procs)
        can_exchange_cache = true;
    connected_peer->econtext->engine->num_connected_service_procs++;
    DBG("we now have %ld connections with other service processes",
        connected_peer->econtext->engine->num_connected_service_procs);

#if 0
    // If we now have all the connections with the other DPUs, we exchange our cache
    // Not needed for now, when ranks are all connected to the local DPU(s), the caches
    // are automatically exchanged using broadcast_group_cache()
    if (can_exchange_cache)
        sync_group_caches(connected_peer->econtext);
#endif
}

dpu_offload_status_t connect_to_remote_service_proc(remote_service_proc_info_t *remote_service_proc_info)
{
    CHECK_ERR_RETURN((remote_service_proc_info == NULL), DO_ERROR, "Remote DPU info is NULL");
    offloading_engine_t *offload_engine = remote_service_proc_info->offload_engine;
    CHECK_ERR_RETURN((offload_engine == NULL), DO_ERROR, "undefined offload_engine");

    assert(remote_service_proc_info->service_proc.global_id != UINT64_MAX);
    assert(remote_service_proc_info->init_params.conn_params->addr_str);
    assert(remote_service_proc_info->init_params.conn_params->port != -1);
    assert(remote_service_proc_info->dpu->hostname);
    assert(offload_engine->config->local_service_proc.info.global_id != UINT64_MAX);

    DBG("connecting to service proc %" PRIu64 " at %s:%d running on DPU server %s, my global ID %" PRIu64,
        remote_service_proc_info->service_proc.global_id,
        remote_service_proc_info->init_params.conn_params->addr_str,
        remote_service_proc_info->init_params.conn_params->port,
        remote_service_proc_info->dpu->hostname,
        offload_engine->config->local_service_proc.info.global_id);
    // Inter-DPU connection, no group, rank is the service process's global ID
    rank_info_t service_proc_info;
    RESET_RANK_INFO(&service_proc_info);
    service_proc_info.group_rank = offload_engine->config->local_service_proc.info.global_id;
    remote_service_proc_info->init_params.proc_info = &service_proc_info;
    remote_service_proc_info->init_params.connected_cb = connected_to_server_dpu;
    remote_service_proc_info->init_params.scope_id = SCOPE_INTER_SERVICE_PROCS;
    // We make sure that we use the inter-service-process port here because we do not know the context while
    // parsing the configuration file (host or DPU) and updating the value while parsing ends up beinng confusing
    remote_service_proc_info->init_params.conn_params->port = remote_service_proc_info->config->version_1.intersp_port;
    execution_context_t *client = client_init(offload_engine, &(remote_service_proc_info->init_params));
    CHECK_ERR_RETURN((client == NULL), DO_ERROR, "Unable to connect to %s\n", remote_service_proc_info->init_params.conn_params->addr_str);
    ENGINE_LOCK(offload_engine);
    offload_engine->inter_service_proc_clients[offload_engine->num_inter_service_proc_clients].client_econtext = client;
    offload_engine->inter_service_proc_clients[offload_engine->num_inter_service_proc_clients].remote_service_proc_info = remote_service_proc_info;
    offload_engine->num_inter_service_proc_clients++;
    assert(client->client->id != UINT64_MAX);
    assert(client->client->server_global_id != UINT64_MAX);
    CLIENT_SERVER_ADD(offload_engine,
                      client->client->id,
                      client->client->server_global_id,
                      client);
    ENGINE_UNLOCK(offload_engine);
    return DO_SUCCESS;
}

static dpu_offload_status_t
connect_to_service_procs(offloading_engine_t *offload_engine, service_procs_inter_connect_info_t *info_connect_to, init_params_t *init_params)
{
    size_t connect_to_idx;
    assert(offload_engine);
    assert(info_connect_to);
    assert(init_params);
    // Create a connection thread for all the required connection
    //connect_to_service_proc_t *conn_info, *conn_info_next;
    for (connect_to_idx = 0; connect_to_idx < info_connect_to->num_connect_to; connect_to_idx++)
    {
        // Initiate the connection to the remote service process running on a specific DPU.
        // This is a non-blocking operation, meaning there is no guarantee the connection
        // will not be fully established when the function returns
        uint64_t *target_sp_gid = NULL;
        dpu_offload_status_t rc;
        remote_service_proc_info_t *sp = NULL;

        target_sp_gid = DYN_ARRAY_GET_ELT(&(info_connect_to->sps_connect_to), connect_to_idx, uint64_t);
        assert(target_sp_gid);
        sp = DYN_ARRAY_GET_ELT(GET_ENGINE_LIST_SERVICE_PROCS(offload_engine), *target_sp_gid, remote_service_proc_info_t);
        assert(sp);
        rc = connect_to_remote_service_proc(sp);
        CHECK_ERR_RETURN((rc), DO_ERROR, "unable to start connection thread");
    }
    return DO_SUCCESS;
}

static uint64_t get_dpu_global_id_from_service_proc_id(offloading_engine_t *engine, uint64_t service_proc_global_id)
{
    remote_service_proc_info_t *sp = NULL;
    assert(engine->host_dpu_data_initialized == true);
    if (engine->num_service_procs <= service_proc_global_id)
        return UINT64_MAX;
    sp = DYN_ARRAY_GET_ELT(GET_ENGINE_LIST_SERVICE_PROCS(engine),
                           service_proc_global_id,
                           remote_service_proc_info_t);
    assert(sp);
    assert(sp->dpu);
    return (sp->dpu->idx);
}

/**
 * @brief Callback invoked when a client service process finalizes its connection to us.
 * This is set on server service processes running on DPUs that are used to let client service processes connect.
 * In other words, server->connected_cb is set to this pointer
 *
 * @param data DPU data
 */
void client_service_proc_connected(void *data)
{
    connected_peer_data_t *connected_peer;
    execution_context_t *econtext;
    uint64_t service_proc_global_id, dpu_global_id;
    peer_info_t *service_proc_info;
    remote_service_proc_info_t *sp;

    assert(data);
    connected_peer = (connected_peer_data_t *)data;
    econtext = connected_peer->econtext;
    assert(econtext);
    assert(econtext->type == CONTEXT_SERVER);
    assert(econtext->engine);
    assert(econtext->scope_id == SCOPE_INTER_SERVICE_PROCS);
    assert(econtext->engine);
    assert(econtext->engine->on_dpu);

    // Lookup the service proc's client data
    service_proc_info = DYN_ARRAY_GET_ELT(&(econtext->server->connected_clients.clients),
                                          connected_peer->rank_info.group_rank,
                                          peer_info_t);
    assert(service_proc_info);
    service_proc_global_id = connected_peer->rank_info.group_rank;
    assert(service_proc_global_id < econtext->engine->num_service_procs);
    dpu_global_id = get_dpu_global_id_from_service_proc_id(econtext->engine, service_proc_global_id);
    assert(dpu_global_id != UINT64_MAX);
    assert(dpu_global_id < econtext->engine->num_dpus);
    DBG("Service proc #%" PRIu64 " running on DPU #%" PRIu64 " (client %" PRIu64 ") is now fully connected",
        service_proc_global_id,
        dpu_global_id,
        connected_peer->peer_id);

    // Set the default econtext if necessary, the function will figure out what to do
    set_default_econtext(connected_peer);

    // Update data in the list of DPUs
    sp = DYN_ARRAY_GET_ELT(GET_ENGINE_LIST_SERVICE_PROCS(connected_peer->econtext->engine), service_proc_global_id, remote_service_proc_info_t);
    assert(sp);
    sp->addr = connected_peer->addr;
    sp->addr_len = connected_peer->addr_len;
    sp->econtext = connected_peer->econtext;
    sp->client_id = connected_peer->peer_id;
#if !NDEBUG
    {
        uint64_t my_sp_gid;
        my_sp_gid = econtext->engine->config->local_service_proc.info.global_id;
        // We can only be a server for SPs with a lower GID
        assert(service_proc_global_id < my_sp_gid);
    }
#endif

    // Increase the number of connected service proc
    connected_peer->econtext->engine->num_connected_service_procs++;
    DBG("we now have %ld connections with other service processes", connected_peer->econtext->engine->num_connected_service_procs);

    // Not needed for now, when ranks are all connected to the local DPU(s), the caches
    // are automatically exchanged using broadcast_group_cache()
#if 0
    {
        bool can_exchange_cache = false;
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
#endif
}

dpu_offload_status_t inter_dpus_connect_mgr(offloading_engine_t *engine, offloading_config_t *cfg)
{
    remote_service_proc_info_t *sp = NULL;
    ucp_ep_params_t ep_params;
    ucs_status_t status;
    CHECK_ERR_RETURN((engine == NULL), DO_ERROR, "undefined engine");
    CHECK_ERR_RETURN((cfg == NULL), DO_ERROR, "undefined configuration");

    engine->on_dpu = true;

    DBG("Connection manager: expecting %ld inbound connections and %ld outbound connections",
        cfg->num_connecting_service_procs, cfg->info_connecting_to.num_connect_to);

    /* Init UCP if necessary */
    if (engine->ucp_context == NULL)
    {
        engine->ucp_context = INIT_UCX();
        engine->ucp_context_allocated = true;
        DBG("UCX successfully initialized, context: %p", engine->ucp_context);
    }

    // Create a worker if necessary
    if (engine->ucp_worker == NULL)
    {
        INIT_WORKER(engine->ucp_context, &(engine->ucp_worker));
        engine->ucp_worker_allocated = true;
    }

    /* Self address*/
    sp = DYN_ARRAY_GET_ELT(GET_ENGINE_LIST_SERVICE_PROCS(cfg->offloading_engine),
                           cfg->local_service_proc.info.global_id,
                           remote_service_proc_info_t);
    assert(sp);
    status = ucp_worker_get_address(engine->ucp_worker,
                                    (ucp_address_t **)&(sp->addr),
                                    &sp->addr_len);
    CHECK_ERR_RETURN((status != UCS_OK), DO_ERROR, "ucp_worker_get_address() failed");


    /* Self EP */
    ep_params.field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS;
    ep_params.address = sp->addr;
    ucp_ep_h self_ep;
    status = ucp_ep_create(engine->ucp_worker, &ep_params, &self_ep);
    CHECK_ERR_RETURN((status != UCS_OK), DO_ERROR, "ucp_ep_create() failed");
    engine->self_ep = self_ep; // fixme: correctly free
    assert(cfg->local_service_proc.info.global_id != UINT64_MAX);
    sp->ep = engine->self_ep;

    assert(cfg->offloading_engine->num_servers == 0);

    if (cfg->num_connecting_service_procs > 0)
    {
        // Some service processes will be connecting to us so we start a new server.
        DBG("Starting server to let other service processes connect to us (init_params=%p, conn_params=%p)...",
            &(cfg->local_service_proc.inter_service_procs_init_params),
            &(cfg->local_service_proc.inter_service_procs_init_params.conn_params));
        cfg->local_service_proc.inter_service_procs_init_params.connected_cb = client_service_proc_connected;
        cfg->local_service_proc.inter_service_procs_init_params.scope_id = SCOPE_INTER_SERVICE_PROCS;
        execution_context_t *server = server_init(cfg->offloading_engine, &(cfg->local_service_proc.inter_service_procs_init_params));
        CHECK_ERR_RETURN((server == NULL), DO_ERROR, "server_init() failed");
        CHECK_ERR_RETURN((cfg->offloading_engine->num_servers + 1 >= cfg->offloading_engine->num_max_servers),
                         DO_ERROR,
                         "max number of server (%ld) has been reached",
                         cfg->offloading_engine->num_max_servers);
        // server_init() already adds the server to the list of servers and handle the associated counter
        DBG("Server successfully started (econtext: %p)", server);
        // Nothing else to do in this context.
    }

    if (cfg->info_connecting_to.num_connect_to > 0)
    {
        // We need to connect to one or more other service processes
        dpu_offload_status_t rc = connect_to_service_procs(cfg->offloading_engine, &(cfg->info_connecting_to), &(cfg->local_service_proc.inter_service_procs_init_params));
        CHECK_ERR_RETURN((rc), DO_ERROR, "connect_to_service_procs() failed");
    }

    return DO_SUCCESS;
}

dpu_offload_status_t get_dpu_config(offloading_engine_t *offload_engine, offloading_config_t *config_data)
{
    dpu_offload_status_t rc;
    CHECK_ERR_RETURN((offload_engine == NULL), DO_ERROR, "undefined offloading engine");

    // If a configuration file is not defined, we use the default one
    if (config_data->config_file == NULL)
    {
        config_data->config_file = getenv(OFFLOAD_CONFIG_FILE_PATH_ENVVAR);
        if (config_data->config_file == NULL)
            config_data->config_file = getenv(DPU_OFFLOAD_CONFIG_FILE_PATH_ENVVAR);
    }

    config_data->local_service_proc.hostname[1023] = '\0';
    gethostname(config_data->local_service_proc.hostname, 1023);

    config_data->list_dpus = getenv(LIST_DPUS_ENVVAR);
    CHECK_ERR_RETURN((config_data->list_dpus == NULL),
                     DO_ERROR,
                     "Unable to get list of DPUs via %s environmnent variable\n",
                     LIST_DPUS_ENVVAR);
    config_data->local_service_proc.info.local_id_str = getenv(DPU_OFFLOAD_SERVICE_PROCESS_LOCAL_ID_ENVVAR);
    config_data->num_service_procs_per_dpu_str = getenv(DPU_OFFLOAD_SERVICE_PROCESSES_PER_DPU_ENVVAR);
    config_data->local_service_proc.info.global_id_str = getenv(DPU_OFFLOAD_SERVICE_PROCESS_GLOBAL_ID_ENVVAR);

    rc = dpu_offload_parse_list_dpus(offload_engine, config_data);
    CHECK_ERR_RETURN((rc == DO_ERROR), DO_ERROR, "dpu_offload_parse_list_dpus() failed");

    if (config_data->num_service_procs_per_dpu == 1)
        config_data->num_service_procs = config_data->num_dpus;
    else
    {
        assert(config_data->num_service_procs_per_dpu > 0 && config_data->num_service_procs != UINT64_MAX);
        config_data->num_service_procs = config_data->num_dpus * config_data->num_service_procs_per_dpu;
    }

    DBG("Number of service processes to connect to: %ld; number of expected incoming connections: %ld; my unique ID: %" PRIu64 "\n",
        config_data->info_connecting_to.num_connect_to,
        config_data->num_connecting_service_procs,
        config_data->local_service_proc.info.global_id);

    config_data->local_service_proc.inter_service_procs_init_params.worker = NULL;
    config_data->local_service_proc.inter_service_procs_init_params.proc_info = NULL;
    config_data->local_service_proc.host_init_params.worker = NULL;
    config_data->local_service_proc.host_init_params.proc_info = NULL;

    /* First, we check whether we know about a configuration file. If so, we load all the configuration details from it */
    /* If there is no configuration file, we try to configuration from environment variables */
    if (config_data->config_file != NULL)
    {
        DBG("Looking for %s's configuration data from %s", config_data->local_service_proc.hostname, config_data->config_file);
        rc = find_dpu_config_from_platform_configfile(config_data->config_file, config_data);
        CHECK_ERR_RETURN((rc), DO_ERROR, "find_dpu_config_from_platform_configfile() failed");
    }
    else
    {
        DBG("No configuration file");
        char *port_str = getenv(INTER_DPU_PORT_ENVVAR);
        config_data->local_service_proc.inter_service_procs_conn_params.addr_str = getenv(INTER_DPU_ADDR_ENVVAR);
        CHECK_ERR_RETURN((config_data->local_service_proc.inter_service_procs_conn_params.addr_str == NULL), DO_ERROR, "%s is not set, please set it\n", INTER_DPU_ADDR_ENVVAR);

        config_data->local_service_proc.inter_service_procs_conn_params.port = DEFAULT_INTER_DPU_CONNECT_PORT;
        if (port_str)
            config_data->local_service_proc.inter_service_procs_conn_params.port = atoi(port_str);
    }

    DBG("%ld service process configuration(s) detected, connecting to %ld service processes and expecting %ld inbound connections",
        config_data->num_dpus,
        config_data->info_connecting_to.num_connect_to,
        config_data->num_connecting_service_procs);
    DBG("My configuration: id: %" PRIu64 ", addr: %s, inter-service-proc port: %d, host port: %d",
        config_data->local_service_proc.info.global_id,
        config_data->local_service_proc.inter_service_procs_conn_params.addr_str,
        config_data->local_service_proc.inter_service_procs_conn_params.port,
        config_data->local_service_proc.host_conn_params.port);

    offload_engine->config = (struct offloading_config *)config_data;

    return DO_SUCCESS;
}
