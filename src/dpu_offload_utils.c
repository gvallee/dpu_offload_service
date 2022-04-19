#define _POSIX_C_SOURCE 200809L

//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <string.h>
#include <stdio.h>
#include <stdio.h>
#include <assert.h>
#include <unistd.h>

#include "dpu_offload_types.h"
#include "dpu_offload_mem_mgt.h"
#include "dpu_offload_debug.h"
#include "dpu_offload_event_channels.h"
#include "dpu_offload_envvars.h"

#if !NDEBUG
char *my_hostname = NULL;
#endif // !NDEBUG

const char *config_file_version_token = "Format version:";

/*************************************/
/* FUNCTIONS RELATED TO GROUPS/RANKS */
/*************************************/

dpu_offload_status_t send_add_group_rank_request(execution_context_t *econtext, ucp_ep_h ep, int64_t group_id, int64_t rank, int64_t group_size, dpu_offload_event_t **e)
{
    dpu_offload_event_t *ev;
    dpu_offload_event_info_t ev_info;
    ev_info.payload_size = sizeof(rank_info_t);
    dpu_offload_status_t rc = event_get(econtext->event_channels, &ev_info, &ev);
    CHECK_ERR_RETURN((rc), DO_ERROR, "event_get() failed");

    DBG("Sending request to add group/rank");
    rank_info_t *rank_info = (rank_info_t *)ev->payload;
    rank_info->group_id = group_id;
    rank_info->group_rank = rank;
    rank_info->group_size = group_size;

    rc = event_channel_emit(&ev,
                            ECONTEXT_ID(econtext),
                            AM_ADD_GP_RANK_MSG_ID,
                            ep,
                            NULL);
    CHECK_ERR_RETURN((rc != EVENT_DONE && rc != EVENT_INPROGRESS), DO_ERROR, "event_channel_emit_with_payload() failed");
    *e = ev;
    return DO_SUCCESS;
}

/********************************************/
/* FUNCTIONS RELATED TO THE ENDPOINT CACHES */
/********************************************/

bool group_cache_populated(offloading_engine_t *engine, int64_t gp_id)
{
    group_cache_t *gp_cache = GET_GROUP_CACHE(&(engine->procs_cache), gp_id);
    if (gp_cache->group_size == gp_cache->num_local_entries)
        return true;
    return false;
}

dpu_offload_status_t send_cache_entry_request(execution_context_t *econtext, ucp_ep_h ep, rank_info_t *requested_peer, dpu_offload_event_t **ev)
{
    dpu_offload_event_t *cache_entry_request_ev;
    dpu_offload_status_t rc;
    rc = event_get(econtext->event_channels, NULL, &cache_entry_request_ev);
    CHECK_ERR_RETURN((rc), DO_ERROR, "event_get() failed");

    DBG("Sending cache entry request for rank:%ld/gp:%ld (econtext: %p, scope_id: %d)",
        requested_peer->group_rank,
        requested_peer->group_id,
        econtext,
        econtext->scope_id);
    rc = event_channel_emit_with_payload(&cache_entry_request_ev,
                                         ECONTEXT_ID(econtext),
                                         AM_PEER_CACHE_ENTRIES_REQUEST_MSG_ID,
                                         ep,
                                         NULL,
                                         requested_peer,
                                         sizeof(rank_info_t));
    CHECK_ERR_RETURN((rc != EVENT_DONE && rc != EVENT_INPROGRESS), DO_ERROR, "event_channel_emit_with_payload() failed");

    *ev = cache_entry_request_ev;
    return DO_SUCCESS;
}

dpu_offload_status_t send_cache_entry(execution_context_t *econtext, ucp_ep_h ep, peer_cache_entry_t *cache_entry, dpu_offload_event_t **ev)
{
    dpu_offload_event_t *send_cache_entry_ev;
    dpu_offload_status_t rc = event_get(econtext->event_channels, NULL, &send_cache_entry_ev);
    CHECK_ERR_RETURN((rc), DO_ERROR, "event_get() failed");

    DBG("Sending cache entry for rank:%" PRId64 "/gp:%" PRId64 " (msg size=%ld, notif type=%d)",
        cache_entry->peer.proc_info.group_rank,
        cache_entry->peer.proc_info.group_id,
        sizeof(peer_cache_entry_t),
        AM_PEER_CACHE_ENTRIES_MSG_ID);
    rc = event_channel_emit_with_payload(&send_cache_entry_ev,
                                         ECONTEXT_ID(econtext),
                                         AM_PEER_CACHE_ENTRIES_MSG_ID,
                                         ep,
                                         NULL,
                                         cache_entry,
                                         sizeof(peer_cache_entry_t));
    CHECK_ERR_RETURN((rc != EVENT_DONE && rc != EVENT_INPROGRESS), DO_ERROR, "event_channel_emit_with_payload() failed");
    // Never ever put the event on the ongoing list since it is also used to exchange the entire cache,
    // in which case the event is put on the list of sub-events and an event can only be on a single list at a time.
    // In other words, the caller is in charge of dealing with the event.

#if !NDEBUG
    if (rc == EVENT_DONE)
        DBG("event completed right away, cahce entry sent");
    if (rc == EVENT_INPROGRESS)
        DBG("cache entry send posted");
#endif

    *ev = send_cache_entry_ev;
    return DO_SUCCESS;
}

dpu_offload_status_t send_group_cache(execution_context_t *econtext, ucp_ep_h dest, int64_t gp_id, dpu_offload_event_t *metaev)
{
    size_t i;
    group_cache_t *gp_cache = (group_cache_t *)econtext->engine->procs_cache.data.base;
    if (!gp_cache[gp_id].initialized)
        return DO_SUCCESS;

    assert(gp_cache[gp_id].group_size > 0);
    for (i = 0; i < gp_cache[gp_id].ranks.num_elts; i++)
    {
        peer_cache_entry_t *cache_entry = GET_GROUP_RANK_CACHE_ENTRY(&(econtext->engine->procs_cache), gp_id, i, gp_cache[gp_id].group_size);
        if (cache_entry->set)
        {
            DBG("Sending cache entry for rank:%ld/gp:%ld (event: %p)", cache_entry->peer.proc_info.group_rank, cache_entry->peer.proc_info.group_id, metaev);
            dpu_offload_event_t *e;
            dpu_offload_status_t rc = send_cache_entry(econtext, dest, cache_entry, &e);
            CHECK_ERR_RETURN((rc), DO_ERROR, "send_cache_entry() failed");
            if (e != NULL)
            {
                // If the event did not complete right away, we add it as a sub-event to the meta-event so we can track everything
                if (!metaev->sub_events_initialized)
                {
                    ucs_list_head_init(&(metaev->sub_events));
                    metaev->sub_events_initialized = true;
                }
                DBG("Adding sub-event %p to main event %p", e, metaev);
                ucs_list_add_tail(&(metaev->sub_events), &(e->item));
            }
            else
            {
                DBG("Sending cache completed right away");
            }
        }
    }
    return DO_SUCCESS;
}

dpu_offload_status_t send_cache(execution_context_t *econtext, cache_t *cache, ucp_ep_h dest, dpu_offload_event_t *metaevt)
{
    // Note: it is all done using the notification channels so there is not
    // need to post receives. Simply send the data if anything needs to be sent
    dpu_offload_status_t rc;
    group_cache_t *groups_cache = (group_cache_t *)cache->data.base;
    size_t i;
    assert(metaevt);
    for (i = 0; i < cache->data.num_elts; i++)
    {
        if (groups_cache[i].initialized)
        {

            rc = send_group_cache(econtext, dest, i, metaevt);
            CHECK_ERR_RETURN((rc), DO_ERROR, "exchange_group_cache() failed\n");
        }
    }
    return DO_SUCCESS;
}

dpu_offload_status_t exchange_cache(execution_context_t *econtext, cache_t *cache, dpu_offload_event_t *meta_evt)
{
    offloading_engine_t *offload_engine;
    offloading_config_t *cfg;
    remote_dpu_info_t **list_dpus;
    dpu_offload_status_t rc;

    assert(econtext);
    offload_engine = econtext->engine;
    assert(offload_engine);
    cfg = (offloading_config_t *)offload_engine->config;
    assert(cfg);
    list_dpus = LIST_DPUS_FROM_ENGINE(offload_engine);
    assert(list_dpus);
    size_t i;
    for (i = 0; i < offload_engine->num_dpus; i++)
    {
        ucp_ep_h dest_ep = GET_REMOTE_DPU_EP(offload_engine, i);
        if (dest_ep)
        {
            rc = send_cache(econtext, &(offload_engine->procs_cache), dest_ep, meta_evt);
            CHECK_ERR_RETURN((rc), DO_ERROR, "send_cache() failed");
        }
    }
    return DO_SUCCESS;
}

bool all_dpus_connected(offloading_engine_t *engine)
{
    if (!engine->on_dpu)
        return false;

    return (engine->num_dpus == engine->num_connected_dpus);
}

dpu_offload_status_t broadcast_group_cache(offloading_engine_t *engine, int64_t group_id)
{
    size_t i;
    remote_dpu_info_t **list_dpus;
    group_cache_t *cache;
    assert(engine);
    assert(group_id >= 0);

    if (!engine->on_dpu)
        return DO_ERROR;

    if (!all_dpus_connected(engine))
    {
        // Not all the DPUs are connected, we cannot exchange the data yet
        WARN_MSG("Not all DPUs are connected, unable to broadcast group cache");
        return DO_ERROR;
    }

    cache = GET_GROUP_CACHE(&(engine->procs_cache), group_id);
    assert(cache);

    list_dpus = LIST_DPUS_FROM_ENGINE(engine);
    assert(list_dpus);
    for (i = 0; i < engine->num_dpus; i++)
    {
        dpu_offload_status_t rc;
        dpu_offload_event_t *ev;
        ucp_ep_h dest_ep;

        // Do not send to self
        offloading_config_t *cfg = (offloading_config_t*)engine->config;
        if (i == cfg->local_dpu.id)
            continue;

        assert(list_dpus[i]->econtext);
        event_get(list_dpus[i]->econtext->event_channels, NULL, &ev);
        assert(ev);
        dest_ep = GET_REMOTE_DPU_EP(engine, i);
        DBG("Sending group cache to DPU #%ld", i);
        rc = send_group_cache(list_dpus[i]->econtext, dest_ep, group_id, ev);
        CHECK_ERR_RETURN((rc), DO_ERROR, "exchange_cache() failed");
        // We do not want to explicitly deal with the event so we put it
        // on the list of ongoing events
        ucs_list_add_tail(&(list_dpus[i]->econtext->ongoing_events), &(ev->item));
    }
    return DO_SUCCESS;
}

bool is_in_cache(cache_t *cache, int64_t gp_id, int64_t rank_id, int64_t group_size)
{
    peer_cache_entry_t *entry = GET_GROUP_RANK_CACHE_ENTRY(cache, gp_id, rank_id, group_size);
    if (entry == NULL)
        return false;
    return (entry->set);
}

static dpu_offload_status_t do_get_cache_entry_by_group_rank(offloading_engine_t *engine, int64_t gp_id, int64_t rank, int64_t gp_size, int64_t dpu_idx, request_compl_cb_t cb, int64_t *dpu_id, dpu_offload_event_t **ev)
{
    if (ev != NULL && cb != NULL)
    {
        ERR_MSG("%s(): both the event and the callback are defined, impossible to understand the context", __func__);
        return DO_ERROR;
    }

    // If the event is defined, the dpu_id must also be defined, they go in pairs
    if (ev != NULL)
        assert(dpu_id);

    if (is_in_cache(&(engine->procs_cache), gp_id, rank, gp_size))
    {
        // The cache has the data
        dyn_array_t *gps_data = &(engine->procs_cache.data);
        dyn_array_t *gp_data = DYN_ARRAY_GET_ELT(gps_data, gp_id, dyn_array_t);
        peer_cache_entry_t *cache_entry = GET_GROUP_RANK_CACHE_ENTRY(&(engine->procs_cache), gp_id, rank, gp_size);
        DBG("%" PRId64 "/%" PRId64 " is in the cache, DPU ID = %" PRId64, rank, gp_id, cache_entry->shadow_dpus[dpu_idx]);
        if (ev != NULL)
        {
            *ev = NULL;
            *dpu_id = cache_entry->shadow_dpus[dpu_idx];
        }
        return DO_SUCCESS;
    }

    // The cache does not have the data. We sent a request to get the data.
    // The caller is in charge of calling the function after completion to actually get the data
    rank_info_t rank_data;
    rank_data.group_id = gp_id;
    rank_data.group_rank = rank;

    // Create the local event so we can know when the cache entry has been received
    dpu_offload_event_t *cache_entry_updated_ev;
    peer_cache_entry_t *cache_entry = GET_GROUP_RANK_CACHE_ENTRY(&(engine->procs_cache), gp_id, rank, gp_size);
    assert(engine->default_econtext);
    dpu_offload_status_t rc = event_get(engine->default_econtext->event_channels, NULL, &cache_entry_updated_ev);
    CHECK_ERR_RETURN((rc), DO_ERROR, "event_get() failed");
    if (!cache_entry->events_initialized)
    {
        ucs_list_head_init(&(cache_entry->events));
        cache_entry->events_initialized = true;
    }
    ucs_list_add_tail(&(cache_entry->events), &(cache_entry_updated_ev->item));
    DBG("Cache entry %p for gp/rank %" PRIu64 "/%" PRIu64 " now has %ld update events",
        cache_entry, gp_id, rank, ucs_list_length(&(cache_entry->events)));
    if (ev != NULL)
    {
        // If the calling function is expecting an event back, no need for anything other than
        // make sure we return the event
        *ev = cache_entry_updated_ev;
    }
    if (cb != NULL)
    {
        // If the calling function specified a callback, the event needs to be hidden from the
        // caller and the callback will be invoked upon completion. So we need to put the event
        // on the list of ongoing events. Make sure to never return the event to the caller to
        // prevent the case where the event could be put on two different lists (which leads
        // to memory corruptions).
        cache_entry_request_t *request_data;
        DYN_LIST_GET(engine->free_cache_entry_requests, cache_entry_request_t, item, request_data);
        assert(request_data);
        request_data->gp_id = gp_id;
        request_data->rank = rank;
        request_data->target_dpu_idx = dpu_idx;
        request_data->offload_engine = engine;
        cache_entry_updated_ev->ctx.completion_cb = cb;
        cache_entry_updated_ev->ctx.completion_cb_ctx = (void*)request_data;
        ucs_list_add_tail(&(engine->default_econtext->ongoing_events), &(cache_entry_updated_ev->item));
    }

    if (engine->on_dpu == true)
    {
        // If we are on a DPU, we need to send a request to all known DPUs
        // To track completion, we get an event from the execution context used for the
        // first DPU.
        size_t i;
        dpu_offload_status_t rc;
        dpu_offload_event_t *metaev = NULL;
        remote_dpu_info_t **list_dpus = LIST_DPUS_FROM_ENGINE(engine);
        execution_context_t *meta_econtext = NULL;

        for (i = 0; i < engine->num_dpus; i++)
        {
            if (list_dpus[i] != NULL && list_dpus[i]->ep != NULL && list_dpus[i]->init_params.conn_params != NULL)
            {
                execution_context_t *econtext = ECONTEXT_FOR_DPU_COMMUNICATION(engine, i);
                CHECK_ERR_RETURN((econtext == NULL), DO_ERROR, "unable to get execution context to communicate with DPU #0");
                DBG("Sending cache entry request to DPU #%ld (econtext: %p, scope_id: %d)",
                    i,
                    econtext,
                    econtext->scope_id);

                if (metaev == NULL)
                {
                    meta_econtext = econtext;
                    rc = event_get(meta_econtext->event_channels, NULL, &metaev);
                    CHECK_ERR_RETURN((rc), DO_ERROR, "get_event() failed");
                }

                ucp_ep_h dpu_ep = list_dpus[i]->ep;
                dpu_offload_event_t *subev;
                rc = send_cache_entry_request(econtext, dpu_ep, &rank_data, &subev);
                CHECK_ERR_RETURN((rc), DO_ERROR, "send_cache_entry_request() failed");
                DBG("Sub-event for sending cache to DPU %ld: %p", i, subev);
                if (subev != NULL)
                {
                    // If the event did not complete right away, we add it as a sub-event to the meta-event so we can track everything
                    if (metaev->sub_events_initialized == false)
                    {
                        ucs_list_head_init(&(metaev->sub_events));
                        metaev->sub_events_initialized = true;
                    }
                    ucs_list_add_tail(&(metaev->sub_events), &(subev->item));
                }
            }
        }
        if (metaev)
        {
            assert(meta_econtext);
            if (!event_completed(metaev))
                ucs_list_add_tail(&(meta_econtext->ongoing_events), &(metaev->item));
        }
    }
    else
    {
        // If we are on the host, we need to send a request to our first shadow DPU
        DBG("Sending request for cache entry...");
        execution_context_t *econtext = engine->client;
        return send_cache_entry_request(econtext, GET_SERVER_EP(econtext), &rank_data, ev);
    }
    return DO_SUCCESS;
}

dpu_offload_status_t get_cache_entry_by_group_rank(offloading_engine_t *engine, int64_t gp_id, int64_t rank, int64_t gp_size, int64_t dpu_idx, request_compl_cb_t cb)
{
    return do_get_cache_entry_by_group_rank(engine, gp_id, rank, gp_size, dpu_idx, cb, NULL, NULL);
}

dpu_offload_status_t get_dpu_id_by_group_rank(offloading_engine_t *engine, int64_t gp_id, int64_t rank, int64_t gp_size, int64_t dpu_idx, int64_t *dpu_id, dpu_offload_event_t **ev)
{
    return do_get_cache_entry_by_group_rank(engine, gp_id, rank, gp_size, dpu_idx, NULL, dpu_id, ev);
}

ucp_ep_h get_dpu_ep_by_id(offloading_engine_t *engine, uint64_t id)
{
    ucp_ep_h ep;
    remote_dpu_info_t **list_dpus = LIST_DPUS_FROM_ENGINE(engine);
    DBG("Looking up entry for DPU #%" PRIu64, id);
    if (list_dpus != NULL && list_dpus[id] == NULL)
        return NULL;
    ep = GET_REMOTE_DPU_EP(engine, id);
    return ep;
}

/******************************************/
/* FUNCTIONS RELATED TO THE CONFIGURATION */
/******************************************/

dpu_offload_status_t check_config_file_version(char *line, int *version)
{
    int idx = 0;

    // Skip heading spaces to find the first valid character
    while (line[idx] == ' ')
        idx++;

    // First valid character must be '#'
    CHECK_ERR_RETURN((line[idx] != '#'), DO_ERROR, "First line of config file does not start with #");
    idx++;

    // Then, first valid token must be 'Format version:'
    while (line[idx] == ' ')
        idx++;
    char *token = &(line[idx]);
    if (strncmp(token, config_file_version_token, strlen(config_file_version_token)) != 0)
    {
        ERR_MSG("First line does not include the version of the format (does not include %s)", config_file_version_token);
        return DO_ERROR;
    }
    idx += strlen(config_file_version_token);

    // Then we should have the version number
    while (line[idx] == ' ')
        idx++;
    token = &(line[idx]);
    *version = atoi(token);
    return DO_SUCCESS;
}

bool line_is_comment(char *line)
{
    int idx = 0;
    while (line[idx] == ' ')
        idx++;
    if (line[idx] == '#')
        return true;
    return false;
}

// <dpu_hostname:interdpu-port:rank-conn-port>
static inline bool parse_dpu_cfg(char *str, char **hostname, char **addr, int *interdpu_conn_port, int *host_conn_port)
{
    assert(hostname);
    assert(addr);
    assert(interdpu_conn_port);
    assert(host_conn_port);

    char *rest = str;
    char *token = strtok_r(rest, ":", &rest);
    assert(token);
    int step = 0;
    *hostname = strdup(token); // fixme: correctly free
    token = strtok_r(rest, ":", &rest);
    assert(token);
    while (token != NULL)
    {
        switch (step)
        {
        case 0:
            DBG("-> addr is %s", token);
            *addr = strdup(token); // fixme: correctly free
            step++;
            break;
        case 1:
            DBG("-> inter-DPU port is %s", token);
            *interdpu_conn_port = atoi(token);
            step++;
            break;
        case 2:
            DBG("-> port to connect with host is %s", token);
            *host_conn_port = atoi(token);
            return true;
        }
        token = strtok_r(rest, ":", &rest);
    }

    DBG("unable to parse entry, stopping at step %d", step);
    return false;
}

// <host name>,<dpu1_hostname:dpu_conn_addr:interdpu-port:rank-conn-port>,...
// bool parse_line_dpu_version_1(int format_version, char *dpu_hostname, char *line, offloading_config_t **local_dpu_config, dyn_array_t *dpus, size_t *num_dpus_connecting_from)
bool parse_line_dpu_version_1(offloading_config_t *data, char *line)
{
    int idx = 0;
    bool rc = false;
    uint64_t num_dpus = 0;
    char *rest_line = line;

    while (line[idx] == ' ')
        idx++;

    char *ptr = &(line[idx]);
    char *token = strtok_r(rest_line, ",", &rest_line);

    // The host's name does not really matter here, moving to the DPU(s) configuration
    token = strtok_r(rest_line, ",", &rest_line);
    assert(token);
    while (token != NULL)
    {
        bool target_dpu = false;
        size_t i, j;
        DBG("-> DPU data: %s", token);

        /* if the DPU is not part of the list of DPUs to use, we skip it */
        dpu_config_data_t *list_dpus_from_list = (dpu_config_data_t *)data->dpus_config.base;
        for (i = 0; i < data->num_dpus; i++)
        {
            // We do not expect users to be super strict in the way they create the list of DPUs
            size_t _strlen = strlen(token);
            if (_strlen > strlen(list_dpus_from_list[i].version_1.hostname))
                _strlen = strlen(list_dpus_from_list[i].version_1.hostname);
            if (strncmp(list_dpus_from_list[i].version_1.hostname, token, _strlen) == 0)
            {
                target_dpu = true;
                DBG("Found the configuration for %s", list_dpus_from_list[i].version_1.hostname);
                break;
            }
        }

        if (target_dpu)
        {
            int interdpu_conn_port, host_conn_port;
            bool parsing_okay = parse_dpu_cfg(token,
                                              &(list_dpus_from_list[i].version_1.hostname),
                                              &(list_dpus_from_list[i].version_1.addr),
                                              &interdpu_conn_port,
                                              &host_conn_port);
            CHECK_ERR_RETURN((parsing_okay == false), false, "unable to parse config file entry");
            DBG("-> DPU %s found (%s:%d:%d)", list_dpus_from_list[i].version_1.hostname, list_dpus_from_list[i].version_1.addr, interdpu_conn_port, host_conn_port);
            list_dpus_from_list[i].version_1.interdpu_port = interdpu_conn_port;
            list_dpus_from_list[i].version_1.rank_port = host_conn_port;

            /* Save the configuration details */
            remote_dpu_info_t **list_dpus = (remote_dpu_info_t **)data->offloading_engine->dpus.base;
            for (j = 0; j < data->offloading_engine->num_dpus; j++)
            {
                if (list_dpus[j] == NULL)
                    continue;

                if (strncmp(list_dpus_from_list[i].version_1.hostname, list_dpus[j]->hostname, strlen(list_dpus[j]->hostname)) == 0)
                {
                    DBG("Saving configuration details for DPU #%ld, %s (addr: %s)",
                        j,
                        list_dpus_from_list[i].version_1.hostname,
                        list_dpus_from_list[i].version_1.addr);

                    // We found the DPU in the engine's list
                    if (list_dpus[j]->init_params.conn_params != NULL)
                    {
                        list_dpus[j]->init_params.conn_params->addr_str = list_dpus_from_list[i].version_1.addr;
                        list_dpus[j]->init_params.conn_params->port = list_dpus_from_list[i].version_1.interdpu_port;
                    }
                }
            }

            if (strncmp(data->local_dpu.hostname, list_dpus_from_list[i].version_1.hostname, strlen(list_dpus_from_list[i].version_1.hostname)) == 0)
            {
                // This is the DPU's configuration we were looking for
                DBG("-> This is my configuration, my index is %d", idx);
                data->dpu_found = true;
                // At the moment, the unique ID from the list of DPUs is used as:
                // - reference,
                // - unique identifier when connecting to other DPUs or other DPUs connecting to us,
                // - unique identifier when handling connections from the ranks running on the local host.
                // In other terms, it is used to create the mapping between all DPUs and all ranks.
                data->local_dpu.interdpu_init_params.id_set = true;
                data->local_dpu.interdpu_init_params.id = data->local_dpu.id;
                data->local_dpu.host_init_params.id_set = true;
                data->local_dpu.host_init_params.id = data->local_dpu.id;
                data->local_dpu.config = &(list_dpus_from_list[i]);
                data->local_dpu.interdpu_conn_params.addr_str = data->local_dpu.config->version_1.addr;
                data->local_dpu.interdpu_conn_params.port = data->local_dpu.config->version_1.interdpu_port;
                data->local_dpu.interdpu_conn_params.port_str = NULL;
                data->local_dpu.host_conn_params.addr_str = data->local_dpu.config->version_1.addr;
                data->local_dpu.host_conn_params.port = data->local_dpu.config->version_1.rank_port;
                data->local_dpu.host_conn_params.port_str = NULL;

                list_dpus[data->local_dpu.id]->ep = data->offloading_engine->self_ep;
                // data->local_dpu.id is already set while parsing the list of DPUs to use for the job
                rc = true;
            }

            // Is it an outbound connection?
            remote_dpu_info_t *connect_to, *next_connect_to;
            ucs_list_for_each_safe(connect_to, next_connect_to, &(data->info_connecting_to.connect_to), item)
            {
                DBG("Check DPU %s that we need to connect to (with %s)", connect_to->hostname, list_dpus_from_list[i].version_1.hostname);
                if (strncmp(list_dpus_from_list[i].version_1.hostname, connect_to->hostname, strlen(connect_to->hostname)) == 0)
                {
                    DBG("Saving connection parameters to connect to %s (%p)", connect_to->hostname, connect_to);
                    conn_params_t *new_conn_params;
                    DYN_LIST_GET(data->offloading_engine->pool_conn_params, conn_params_t, item, new_conn_params); // fixme: properly return it
                    RESET_CONN_PARAMS(new_conn_params);
                    connect_to->init_params.conn_params = new_conn_params;
                    connect_to->init_params.conn_params->addr_str = list_dpus_from_list[i].version_1.addr;
                    connect_to->init_params.conn_params->port = list_dpus_from_list[i].version_1.interdpu_port;
                }
            }
        }
        else
        {
            DBG("%s is not to be used", token);
        }
        token = strtok_r(rest_line, ",", &rest_line);
    }
    return rc;
}

// <host name>,<dpu1_hostname:dpu_conn_addr:interdpu-port:rank-conn-port>,...
bool parse_line_version_1(char *target_hostname, offloading_config_t *data, char *line)
{
    int idx = 0;
    char *rest = line;

    while (line[idx] == ' ')
        idx++;

    char *ptr = &(line[idx]);
    char *token = strtok_r(rest, ",", &rest);
    DBG("Checking entry for %s", token);
    if (strncmp(token, target_hostname, strlen(token)) == 0)
    {
        // We found the hostname

        // Next tokens are the local DPUs' data
        // We get the DPUs configuration one-by-one.
        size_t dpu_idx = 0;
        token = strtok_r(rest, ",", &rest);
        while (token != NULL)
        {
            dpu_config_data_t *dpu_config;
            dpu_config = data->dpus_config.base;
            assert(dpu_config);
            CHECK_ERR_RETURN((dpu_config == NULL), DO_ERROR, "unable to allocate resources for DPUs' configuration");

            bool rc = parse_dpu_cfg(token,
                                    &(dpu_config[0].version_1.hostname),
                                    &(dpu_config[0].version_1.addr),
                                    &(dpu_config[0].version_1.interdpu_port),
                                    &(dpu_config[0].version_1.rank_port));
            CHECK_ERR_RETURN((rc == false), DO_ERROR, "parse_dpu_cfg() failed");
            data->num_dpus++;
            token = strtok_r(rest, ",", &rest);
        }
        DBG("%ld DPU(s) is/are specified for %s", data->num_dpus, target_hostname);
        return true;
    }
    return false;
}

/**
 * @brief parse_line parses a line of the configuration file looking for a specific host name. It shall not be used to seek the configuration of a DPU.
 *
 * @param target_hostname Host's name
 * @param line Line from the configuration file that is being parsed
 * @param data Configuration data
 * @return true when the line includes the host's configuration
 * @return false when the lines does not include the host's configuration
 */
bool parse_line(char *target_hostname, char *line, offloading_config_t *data)
{
    switch (data->format_version)
    {
    case 1:
        return parse_line_version_1(target_hostname, data, line);
    default:
        ERR_MSG("supported format (%s: version=%d)", line, data->format_version);
    }
    return false;
}

/**
 * @brief parse_line_for_dpu_cfg parses a line of the configuration file looking for a specific DPU. It shall not be used to seek the configuration of a host.
 *
 * @param data Data gathered while parsing the configuration file
 * @param line Line from the configuration file that is being parsed
 * @return true when the line includes the host's configuration
 * @return false when the lines does not include the host's configuration
 */
bool parse_line_for_dpu_cfg(offloading_config_t *data, char *line)
{
    switch (data->format_version)
    {
    case 1:
        return parse_line_dpu_version_1(data, line);
    default:
        ERR_MSG("supported format (%s: version=%d)", line, data->format_version);
    }
    return false;
}

/**
 * @brief find_dpu_config_from_platform_configfile extracts a DPU's configuration from a platform configuration file.
 * It shall not be used to extract the configuration of a host.
 *
 * @param filepath Path the configuration file
 * @param config_data Object where all the configuration details are stored
 * @return dpu_offload_status_t
 */
dpu_offload_status_t find_dpu_config_from_platform_configfile(char *filepath, offloading_config_t *config_data)
{
    size_t len = 0;
    ssize_t read;
    dpu_offload_status_t rc = DO_ERROR;
    bool first_line = true;
    bool found_self = false;

    // Read the entire file so we can go over the content quickly. Configure files are not expected to get huge
    FILE *file = fopen(filepath, "rb");
    fseek(file, 0, SEEK_END);
    len = ftell(file);
    fseek(file, 0, SEEK_SET); /* same as rewind(f); */

    char *content = MALLOC(len + 1);
    fread(content, len, 1, file);
    fclose(file);
    content[len] = '\0';
    char *rest_content = content;

    DBG("Configuration:\n%s", content);

    // We will get through the content line by line
    char *line = strtok_r(rest_content, "\n", &rest_content);

    // Get the format version from the first line
    rc = check_config_file_version(line, &(config_data->format_version));
    CHECK_ERR_GOTO((rc), error_out, "check_config_file_version() failed");
    CHECK_ERR_GOTO((config_data->format_version <= 0), error_out, "invalid version: %d", config_data->format_version);
    DBG("Configuration file based on format version %d", config_data->format_version);

    line = strtok_r(rest_content, "\n", &rest_content);
    while (line != NULL)
    {
        if (line_is_comment(line))
        {
            line = strtok_r(rest_content, "\n", &rest_content);
            continue;
        }

        DBG("Looking at %s", line);
        if (parse_line_for_dpu_cfg(config_data, line) == true)
            found_self = true;

        line = strtok_r(rest_content, "\n", &rest_content);
    }
    DBG("done parsing the configuration file");

    rc = DO_SUCCESS;

error_out:
    if (content)
        free(content);

    return rc;
}

/**
 * @brief find_config_from_platform_configfile extracts a host's configuration from a platform configuration file.
 * It shall not be used to extract the configuration of a DPU.
 *
 * @param filepath Path to the configuration file
 * @param hostname Name of the host to look up
 * @param data Configuration data of the host's local DPUs
 * @return dpu_offload_status_t
 */
dpu_offload_status_t find_config_from_platform_configfile(char *filepath, char *hostname, offloading_config_t *data)
{
    FILE *file = NULL;
    char *line = NULL;
    size_t len = 0;
    ssize_t read;
    dpu_offload_status_t rc = DO_ERROR;
    bool first_line = true;
    int version = 0;

    file = fopen(filepath, "r");

    while ((read = getline(&line, &len, file)) != -1)
    {
        if (first_line)
        {
            rc = check_config_file_version(line, &(data->format_version));
            CHECK_ERR_GOTO((rc), error_out, "check_config_file_version() failed");
            CHECK_ERR_GOTO((data->format_version <= 0), error_out, "invalid version: %d", data->format_version);
            DBG("Configuration file based on format version %d", data->format_version);
            first_line = false;
            continue;
        }

        if (line_is_comment(line))
            continue;

        if (parse_line(hostname, line, data))
        {
            // We found the configuration for the hostname
            break;
        }
    }

    rc = DO_SUCCESS;

error_out:
    fclose(file);
    if (line)
        free(line);

    return rc;
}

dpu_offload_status_t get_env_config(conn_params_t *params)
{
    char *server_port_envvar = getenv(SERVER_PORT_ENVVAR);
    char *server_addr = getenv(SERVER_IP_ADDR_ENVVAR);
    int port = -1;

    CHECK_ERR_RETURN((!server_addr), DO_ERROR,
                     "Invalid server address, please make sure the environment variable %s or %s is correctly set",
                     SERVER_IP_ADDR_ENVVAR, INTER_DPU_ADDR_ENVVAR);

    if (server_port_envvar)
    {
        port = (uint16_t)atoi(server_port_envvar);
    }

    CHECK_ERR_RETURN((port < 0), DO_ERROR, "Invalid server port (%s), please specify the environment variable %s",
                     server_port_envvar, SERVER_PORT_ENVVAR);

    params->addr_str = server_addr;
    params->port_str = server_port_envvar;
    params->port = port;

    return DO_SUCCESS;
}

dpu_offload_status_t get_host_config(offloading_config_t *config_data)
{
    dpu_offload_status_t rc;
    char hostname[1024];

    config_data->config_file = getenv(OFFLOAD_CONFIG_FILE_PATH_ENVVAR);
    hostname[1023] = '\0';
    gethostname(hostname, 1023);

    config_data->list_dpus = NULL;                                // Not used on host
    config_data->local_dpu.interdpu_init_params.worker = NULL;    // Not used on host
    config_data->local_dpu.interdpu_init_params.proc_info = NULL; // Not used on host
    config_data->local_dpu.host_init_params.worker = NULL;        // Not used on host
    config_data->local_dpu.host_init_params.proc_info = NULL;     // Not used on host

    /* First, we check whether we know about a configuration file. If so, we load all the configuration details from it */
    /* If there is no configuration file, we try to configuration from environment variables */
    if (config_data->config_file != NULL)
    {
        DBG("Looking for %s's configuration data from %s", hostname, config_data->config_file);
        rc = find_config_from_platform_configfile(config_data->config_file, hostname, config_data);
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

    return DO_SUCCESS;
}
