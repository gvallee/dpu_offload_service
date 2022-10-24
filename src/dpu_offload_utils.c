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
debug_config_t dbg_cfg = {
    .my_hostname = NULL,
    .verbose = 1,
};
#endif // !NDEBUG

execution_context_t *get_server_servicing_host(offloading_engine_t *engine);

const char *config_file_version_token = "Format version:";

#define GROUP_SIZE_UNKNOWN (-1)

/*************************************/
/* FUNCTIONS RELATED TO GROUPS/RANKS */
/*************************************/

bool is_in_cache(cache_t *cache, group_uid_t gp_uid, int64_t rank_id, int64_t group_size)
{
    peer_cache_entry_t *entry = GET_GROUP_RANK_CACHE_ENTRY(cache, gp_uid, rank_id, group_size);
    if (entry == NULL)
        return false;
    return (entry->set);
}

dpu_offload_status_t do_send_add_group_rank_request(execution_context_t *econtext, ucp_ep_h ep, uint64_t dest_id, dpu_offload_event_t *ev)
{
    dpu_offload_status_t rc;
    // The rank info is always at the beginning of the payload that the caller prepared
    rank_info_t *rank_info = (rank_info_t *)ev->payload;
    // We add ourselves to the local EP cache as shadow service process
    if (!is_in_cache(&(econtext->engine->procs_cache), rank_info->group_uid, rank_info->group_rank, rank_info->group_size))
    {
        // Before sending the data, update the local cache
        peer_cache_entry_t *cache_entry = GET_GROUP_RANK_CACHE_ENTRY(&(econtext->engine->procs_cache),
                                                                     rank_info->group_uid,
                                                                     rank_info->group_rank,
                                                                     rank_info->group_size);
        group_cache_t *gp_cache = GET_GROUP_CACHE(&(econtext->engine->procs_cache), rank_info->group_uid);
        assert(cache_entry);
        assert(gp_cache);
        assert(econtext->engine->config != NULL);
        cache_entry->shadow_service_procs[cache_entry->num_shadow_service_procs] = econtext->engine->config->local_service_proc.info.global_id;
        cache_entry->peer.proc_info.group_uid = rank_info->group_uid;
        cache_entry->peer.proc_info.group_rank = rank_info->group_rank;
        cache_entry->peer.proc_info.group_size = rank_info->group_size;
        cache_entry->peer.proc_info.n_local_ranks = rank_info->n_local_ranks;
        cache_entry->num_shadow_service_procs++;
        cache_entry->set = true;
        gp_cache->num_local_entries++;
    }

    DBG("Sending request to add the group/rank");
    rc = event_channel_emit(&ev,
                            AM_ADD_GP_RANK_MSG_ID,
                            ep,
                            dest_id,
                            NULL);
    CHECK_ERR_RETURN((rc != EVENT_DONE && rc != EVENT_INPROGRESS), DO_ERROR, "event_channel_emit() failed");
    return DO_SUCCESS;
}

dpu_offload_status_t send_add_group_rank_request(execution_context_t *econtext, ucp_ep_h ep, uint64_t dest_id, dpu_offload_event_t *ev)
{
    group_cache_t *gp_cache = NULL;
    pending_send_group_add_t *pending_send = NULL;

    // The rank info is always at the beginning of the payload that the caller prepared
    // All the ranks' data is supposed for the same group
    rank_info_t *rank_info = (rank_info_t *)ev->payload;
    gp_cache = GET_GROUP_CACHE(&(econtext->engine->procs_cache), rank_info->group_uid);
    assert(gp_cache);
    if (gp_cache->global_revoked == 0)
    {
        return do_send_add_group_rank_request(econtext, ep, dest_id, ev);
    }

    // The group is in the process of being deleted, we cannot add this right away, otherwise the local cache would be in a inconsistent state
    DYN_LIST_GET(econtext->engine->pool_pending_send_group_add,
                 pending_send_group_add_t,
                 item,
                 pending_send);
    assert(pending_send);
    RESET_PENDING_SEND_GROUP_ADD(pending_send);
    pending_send->dest_ep = ep;
    pending_send->dest_id = dest_id;
    pending_send->econtext = econtext;
    pending_send->ev = ev;
    ucs_list_add_tail(&(econtext->engine->pending_send_group_add_msgs), &(pending_send->item));
    return DO_SUCCESS;
}

/**
 * @brief revoke_msg_return returns a revoke message to a pool. The function can be used for messages both on the host or DPUs.
 *
 * @param pool
 * @param buf
 */
static void revoke_msg_return(void *pool, void *buf)
{
    dyn_list_t *dlist = NULL;
    group_revoke_msg_obj_t *desc = NULL;
    assert(pool);
    dlist = (dyn_list_t *)pool;
    desc = (group_revoke_msg_obj_t *)((ptrdiff_t)buf - sizeof(ucs_list_link_t));
    DYN_LIST_RETURN(dlist, desc, item);
}

static void *revoke_msg_get(void *pool, void *args)
{
    group_revoke_msg_obj_t *elt = NULL;
    dyn_list_t *dlist = NULL;
    assert(pool);
    dlist = (dyn_list_t *)pool;
    DYN_LIST_GET(dlist, group_revoke_msg_obj_t, item, elt);
    assert(elt);
    return &(elt->msg);
}

dpu_offload_status_t send_revoke_group_rank_request_through_rank_info(execution_context_t *econtext,
                                                                      ucp_ep_h ep,
                                                                      uint64_t dest_id,
                                                                      rank_info_t *rank_info,
                                                                      dpu_offload_event_t *meta_ev)
{
    dpu_offload_status_t rc;
    dpu_offload_event_t *ev = NULL;
    group_revoke_msg_t *desc = NULL;
    dpu_offload_event_info_t ev_info;

    // Check the validity of the group
    if (rank_info->group_rank == INVALID_RANK)
    {
        ERR_MSG("Invalid group, unable to remove");
        return DO_ERROR;
    }
    assert(rank_info->group_uid != INT_MAX);

    RESET_EVENT_INFO(&ev_info);
    ev_info.pool.element_size = sizeof(group_revoke_msg_t); // Size of the payload, not the type used to get element
    ev_info.pool.get_buf = revoke_msg_get;
    ev_info.pool.return_buf = revoke_msg_return;
    ev_info.pool.mem_pool = econtext->engine->pool_group_revoke_msgs;

    rc = event_get(econtext->event_channels, &ev_info, &ev);
    CHECK_ERR_RETURN((rc != DO_SUCCESS), DO_ERROR, "event_get() failed");
    assert(ev);
    desc = (group_revoke_msg_t *)ev->payload;
    desc->type = GROUP_REVOKE_THROUGH_RANK_INFO;
    COPY_RANK_INFO(rank_info, &(desc->info));

    if (meta_ev != NULL)
        ev->is_subevent = true;

    DBG("Sending request to revoke the group/rank");
    rc = event_channel_emit(&ev,
                            AM_REVOKE_GP_RANK_MSG_ID,
                            ep,
                            dest_id,
                            NULL);
    CHECK_ERR_RETURN((rc != EVENT_DONE && rc != EVENT_INPROGRESS), DO_ERROR, "event_channel_emit() failed");

    if (meta_ev != NULL)
    {
        QUEUE_SUBEVENT(meta_ev, ev);
    }

    // If on the host, we locally mark the group as being revoked
    if (!econtext->engine->on_dpu)
    {
        group_cache_t *gp_cache = NULL;
        gp_cache = GET_GROUP_CACHE(&(econtext->engine->procs_cache), rank_info->group_uid);
        gp_cache->local_revoked++;
        gp_cache->global_revoked++;
    }

    return DO_SUCCESS;
}

dpu_offload_status_t send_revoke_group_rank_request_through_num_ranks(execution_context_t *econtext,
                                                                      ucp_ep_h ep,
                                                                      uint64_t dest_id,
                                                                      group_uid_t gp_uid,
                                                                      uint64_t num_ranks,
                                                                      dpu_offload_event_t *meta_ev)
{
    dpu_offload_status_t rc;
    dpu_offload_event_t *ev = NULL;
    group_revoke_msg_t *desc = NULL;
    dpu_offload_event_info_t ev_info;

    // Check the validity of the group
    if (gp_uid == INT_MAX)
    {
        ERR_MSG("Invalid group, unable to remove");
        return DO_ERROR;
    }

    RESET_EVENT_INFO(&ev_info);
    ev_info.pool.element_size = sizeof(group_revoke_msg_t);
    ev_info.pool.get_buf = revoke_msg_get;
    ev_info.pool.return_buf = revoke_msg_return;
    ev_info.pool.mem_pool = econtext->engine->pool_group_revoke_msgs;

    rc = event_get(econtext->event_channels, &ev_info, &ev);
    CHECK_ERR_RETURN((rc != DO_SUCCESS), DO_ERROR, "event_get() failed");
    assert(ev);
    desc = (group_revoke_msg_t *)ev->payload;
    desc->type = GROUP_REVOKE_THROUGH_NUM_RANKS;
    desc->num_ranks.gp_uid = gp_uid;
    desc->num_ranks.num = num_ranks;
    
    if (meta_ev != NULL)
        ev->is_subevent = true;

    DBG("Sending request to revoke the group/rank");
    rc = event_channel_emit(&ev,
                            AM_REVOKE_GP_RANK_MSG_ID,
                            ep,
                            dest_id,
                            NULL);
    CHECK_ERR_RETURN((rc != EVENT_DONE && rc != EVENT_INPROGRESS), DO_ERROR, "event_channel_emit() failed");

    if (meta_ev != NULL)
    {
        QUEUE_SUBEVENT(meta_ev, ev);
    }
    return DO_SUCCESS;
}

/********************************************/
/* FUNCTIONS RELATED TO THE ENDPOINT CACHES */
/********************************************/

bool group_cache_populated(offloading_engine_t *engine, group_uid_t gp_uid)
{
    group_cache_t *gp_cache = GET_GROUP_CACHE(&(engine->procs_cache), gp_uid);
    if (gp_cache->global_revoked == 0 && gp_cache->group_size == gp_cache->num_local_entries)
    {
        DBG("Group cache for group 0x%x fully populated. num_local_entries = %" PRIu64 " group_size = %" PRIu64,
            gp_uid, gp_cache->num_local_entries, gp_cache->group_size);
        return true;
    }
    return false;
}

// Translate the local SP ID received in the header of a notification to a global ID
uint64_t LOCAL_ID_TO_GLOBAL(execution_context_t *econtext, uint64_t local_id)
{
    uint64_t global_id = UINT64_MAX;
    switch (econtext->type)
    {
    case CONTEXT_SERVER:
    {
        peer_info_t *_c = DYN_ARRAY_GET_ELT(&(econtext->server->connected_clients.clients), local_id, peer_info_t);
        assert(_c);
        global_id = _c->rank_data.group_rank;
        break;
    }
    case CONTEXT_CLIENT:
    {
        if (econtext->engine->on_dpu)
            global_id = local_id;
        else
            global_id = econtext->client->server_global_id;
        break;
    }
    case CONTEXT_SELF:
    {
        if (econtext->engine->on_dpu)
            global_id = econtext->engine->config->local_service_proc.info.global_id;
        else
            global_id = 0; /* By default, for comm to self the ID is 0 */
        break;
    }
    default:
    {
        global_id = UINT64_MAX;
        break;
    }
    }
    return global_id;
}

dpu_offload_status_t do_send_cache_entry_request(execution_context_t *econtext, ucp_ep_h ep, uint64_t dest_id, rank_info_t *requested_peer, dpu_offload_event_t *ev)
{
    int rc;
    DBG("Sending cache entry request for rank:%ld/gp:0x%x (econtext: %p, scope_id: %d)",
        requested_peer->group_rank,
        requested_peer->group_uid,
        econtext,
        econtext->scope_id);
    rc = event_channel_emit_with_payload(&ev,
                                         AM_PEER_CACHE_ENTRIES_REQUEST_MSG_ID,
                                         ep,
                                         dest_id,
                                         NULL,
                                         requested_peer,
                                         sizeof(rank_info_t));
    CHECK_ERR_RETURN((rc != EVENT_DONE && rc != EVENT_INPROGRESS), DO_ERROR, "event_channel_emit_with_payload() failed");
    return DO_SUCCESS;
}

dpu_offload_status_t send_cache_entry_request(execution_context_t *econtext, ucp_ep_h ep, uint64_t dest_id, rank_info_t *requested_peer, dpu_offload_event_t **ev)
{
    dpu_offload_event_t *cache_entry_request_ev;
    dpu_offload_status_t rc;
    rc = event_get(econtext->event_channels, NULL, &cache_entry_request_ev);
    CHECK_ERR_GOTO((rc), error_out, "event_get() failed");

    rc = do_send_cache_entry_request(econtext, ep, dest_id, requested_peer, cache_entry_request_ev);
    CHECK_ERR_GOTO((rc), error_out, "do_send_cache_entry_request() failed");

    *ev = cache_entry_request_ev;
    return DO_SUCCESS;
error_out:
    *ev = NULL;
    return DO_ERROR;
}

dpu_offload_status_t do_send_cache_entry(execution_context_t *econtext, ucp_ep_h ep, uint64_t dest_id, peer_cache_entry_t *cache_entry, dpu_offload_event_t *ev)
{
    int rc;
    DBG("Sending cache entry for rank:%" PRId64 "/gp:0%x (msg size=%ld, notif type=%d)",
        cache_entry->peer.proc_info.group_rank,
        cache_entry->peer.proc_info.group_uid,
        sizeof(peer_cache_entry_t),
        AM_PEER_CACHE_ENTRIES_MSG_ID);
    rc = event_channel_emit_with_payload(&ev,
                                         AM_PEER_CACHE_ENTRIES_MSG_ID,
                                         ep,
                                         dest_id,
                                         NULL,
                                         cache_entry,
                                         sizeof(peer_cache_entry_t));
    CHECK_ERR_RETURN((rc != EVENT_DONE && rc != EVENT_INPROGRESS), DO_ERROR, "event_channel_emit_with_payload() failed");
    // Never ever put the event on the ongoing list since it is also used to exchange the entire cache,
    // in which case the event is put on the list of sub-events and an event can only be on a single list at a time.
    // In other words, the caller is in charge of dealing with the event.

#if !NDEBUG
    if (rc == EVENT_DONE)
    {
        DBG("event completed right away, cahce entry sent");
    }
    if (rc == EVENT_INPROGRESS)
    {
        DBG("cache entry send posted");
    }
#endif
    return DO_SUCCESS;
}

dpu_offload_status_t send_cache_entry(execution_context_t *econtext, ucp_ep_h ep, uint64_t dest_id, peer_cache_entry_t *cache_entry, dpu_offload_event_t **ev)
{
    dpu_offload_event_t *send_cache_entry_ev;
    dpu_offload_status_t rc = event_get(econtext->event_channels, NULL, &send_cache_entry_ev);
    CHECK_ERR_GOTO((rc), error_out, "event_get() failed");
    rc = do_send_cache_entry(econtext, ep, dest_id, cache_entry, send_cache_entry_ev);
    CHECK_ERR_GOTO((rc), error_out, "do_send_cache_entry() failed");
    *ev = send_cache_entry_ev;
    return DO_SUCCESS;
error_out:
    *ev = NULL;
    return DO_ERROR;
}

dpu_offload_status_t send_group_cache(execution_context_t *econtext, ucp_ep_h dest_ep, uint64_t dest_id, group_uid_t gp_uid, dpu_offload_event_t *metaev)
{
    size_t i;
    int rc;
    group_cache_t *gp_cache;
    assert(econtext);
    assert(econtext->engine);
    assert(metaev);
    assert(EVENT_HDR_TYPE(metaev) == META_EVENT_TYPE);
    gp_cache = GET_GROUP_CACHE(&(econtext->engine->procs_cache), gp_uid);
    assert(gp_cache);
    if (!gp_cache->initialized)
        return DO_SUCCESS;

    assert(gp_cache->group_size > 0);

    // The entire group is supposed to be ready, starting at rank 0
#if !NDEBUG
    for (i = 0; i < gp_cache->group_size; i++)
    {
        peer_cache_entry_t *cache_entry = GET_GROUP_RANK_CACHE_ENTRY(&(econtext->engine->procs_cache), gp_uid, i, gp_cache->group_size);
        assert(cache_entry->set == true);
    }
#endif

    dpu_offload_event_t *e;
    peer_cache_entry_t *first_entry = GET_GROUP_RANK_CACHE_ENTRY(&(econtext->engine->procs_cache), gp_uid, 0, gp_cache->group_size);
    rc = event_get(econtext->event_channels, NULL, &e);
    CHECK_ERR_RETURN((rc), DO_ERROR, "event_get() failed");
    e->is_subevent = true;
    DBG("Sending %ld cache entries to %ld, ev: %p (%ld), metaev: %ld\n",
        gp_cache->group_size, dest_id, e, e->seq_num, metaev->seq_num);
    rc = event_channel_emit_with_payload(&e, AM_PEER_CACHE_ENTRIES_MSG_ID, dest_ep, dest_id, NULL, first_entry, gp_cache->group_size * sizeof(peer_cache_entry_t));
    if (rc != EVENT_DONE && rc != EVENT_INPROGRESS)
    {
        ERR_MSG("event_channel_emit_with_payload() failed");
        return DO_ERROR;
    }
    if (e != NULL)
    {
        QUEUE_SUBEVENT(metaev, e);
    }
    else
    {
        WARN_MSG("Sending cache completed right away");
    }
    return DO_SUCCESS;
}

bool find_range_local_ranks(execution_context_t *econtext, group_uid_t gp_uid, int64_t gp_size, size_t start_idx, size_t total_count, size_t cur_count, size_t *range_start, size_t *num, size_t *cur_idx)
{
    size_t count = 0;
    size_t idx = start_idx;
    bool found_begining = false;
    while (found_begining == false || cur_count + count != total_count)
    {
        peer_cache_entry_t *cache_entry = GET_GROUP_RANK_CACHE_ENTRY(&(econtext->engine->procs_cache), gp_uid, idx, gp_size);
        if (cache_entry->set == false)
        {
            idx++;
            continue;
        }

        if (found_begining == false && cache_entry->shadow_service_procs[0] == econtext->engine->config->local_service_proc.info.global_id)
        {
            found_begining = true;
            *range_start = idx;
        }

        if (found_begining && idx > *range_start && INVALID_RANK != cache_entry->peer.proc_info.group_rank)
        {
            // We reached a element that is not valid. It happens with groups with a sparse set of local ranks
            break;
        }

        if (found_begining == true && cache_entry->shadow_service_procs[0] == econtext->engine->config->local_service_proc.info.global_id)
            count++;

        idx++;
    }
    *num = count;
    *cur_idx = idx;

    return true;
}

static dpu_offload_status_t send_local_revoke_rank_group_cache(execution_context_t *econtext,
                                                               ucp_ep_h dest_ep,
                                                               uint64_t dest_id,
                                                               group_uid_t gp_uid,
                                                               uint64_t n_ranks,
                                                               dpu_offload_event_t *metaev)
{
    int rc;
    group_cache_t *gp_cache = NULL;
    dpu_offload_event_t *e = NULL;
    group_revoke_msg_t *payload = NULL;
    dpu_offload_event_info_t ev_info;

    assert(econtext);
    assert(econtext->engine);
    assert(metaev);
    assert(EVENT_HDR_TYPE(metaev) == META_EVENT_TYPE);
    gp_cache = GET_GROUP_CACHE(&(econtext->engine->procs_cache), gp_uid);
    assert(gp_cache);
    if (!gp_cache->initialized)
        return DO_SUCCESS;

    assert(gp_cache->group_size > 0);

    // Get a revoke buffer for the message
    RESET_EVENT_INFO(&ev_info);
    ev_info.pool.element_size = sizeof(group_revoke_msg_t);
    ev_info.pool.get_buf = revoke_msg_get;
    ev_info.pool.return_buf = revoke_msg_return;
    ev_info.pool.mem_pool = econtext->engine->pool_group_revoke_msgs;

    // Send the notification and queue the sub-event
    rc = event_get(econtext->event_channels, &ev_info, &e);
    CHECK_ERR_RETURN((rc), DO_ERROR, "event_get() failed");
    payload = (group_revoke_msg_t *)e->payload;
    payload->type = GROUP_REVOKE_THROUGH_NUM_RANKS;
    payload->num_ranks.num = n_ranks;
    payload->num_ranks.gp_uid = gp_uid;
    e->is_subevent = true;

    rc = event_channel_emit(&e, AM_REVOKE_GP_RANK_MSG_ID, dest_ep, dest_id, NULL);
    if (rc != EVENT_DONE && rc != EVENT_INPROGRESS)
    {
        ERR_MSG("event_channel_emit_with_payload() failed");
        return DO_ERROR;
    }
    if (e != NULL)
    {
        QUEUE_SUBEVENT(metaev, e);
    }
    else
    {
        WARN_MSG("Sending cache completed right away");
    }

    return DO_SUCCESS;
}

static dpu_offload_status_t send_local_rank_group_cache(execution_context_t *econtext, ucp_ep_h dest_ep, uint64_t dest_id, group_uid_t gp_uid, dpu_offload_event_t *metaev)
{
    size_t count, idx;
    int rc;
    group_cache_t *gp_cache;
    assert(econtext);
    assert(econtext->engine);
    assert(metaev);
    assert(EVENT_HDR_TYPE(metaev) == META_EVENT_TYPE);
    gp_cache = GET_GROUP_CACHE(&(econtext->engine->procs_cache), gp_uid);
    assert(gp_cache);
    if (!gp_cache->initialized)
        return DO_SUCCESS;

    assert(gp_cache->group_size > 0);
#if AGGR_SEND_CACHE_ENTRIES
    size_t idx_start, idx_end;
    bool idx_start_set = false;
    bool idx_end_set = false;
#endif
    DBG("Sending group cache 0x%x for local ranks: %ld entries", gp_uid, gp_cache->n_local_ranks_populated);
    count = 0;
    idx = 0;
    while (count < gp_cache->n_local_ranks_populated)
    {
        size_t n_entries_to_send;
        size_t idx_start;
        find_range_local_ranks(econtext, gp_uid, gp_cache->group_size, idx, gp_cache->n_local_ranks_populated, count, &idx_start, &n_entries_to_send, &idx);
        dpu_offload_event_t *e;
        peer_cache_entry_t *first_entry = GET_GROUP_RANK_CACHE_ENTRY(&(econtext->engine->procs_cache), gp_uid, idx_start, gp_cache->group_size);
        rc = event_get(econtext->event_channels, NULL, &e);
        CHECK_ERR_RETURN((rc), DO_ERROR, "event_get() failed");
        e->is_subevent = true;
        DBG("Sending %ld cache entries to %ld from entry %ld, ev: %p (%ld), metaev: %ld (msg size: %ld)\n",
            n_entries_to_send, dest_id, idx_start, e, e->seq_num, metaev->seq_num, n_entries_to_send * sizeof(peer_cache_entry_t));
        rc = event_channel_emit_with_payload(&e, AM_PEER_CACHE_ENTRIES_MSG_ID, dest_ep, dest_id, NULL, first_entry, n_entries_to_send * sizeof(peer_cache_entry_t));
        if (rc != EVENT_DONE && rc != EVENT_INPROGRESS)
        {
            ERR_MSG("event_channel_emit_with_payload() failed");
            return DO_ERROR;
        }
        if (e != NULL)
        {
            QUEUE_SUBEVENT(metaev, e);
        }
        else
        {
            WARN_MSG("Sending cache completed right away");
        }
        count += n_entries_to_send;
    }

    return DO_SUCCESS;
}

dpu_offload_status_t send_gp_cache_to_host(execution_context_t *econtext, group_uid_t group_uid)
{
    assert(econtext->type == CONTEXT_SERVER);
    assert(econtext->scope_id == SCOPE_HOST_DPU);
    size_t n = 0, idx = 0;
    group_cache_t *gp_cache = GET_GROUP_CACHE(&(econtext->engine->procs_cache), group_uid);
    if (gp_cache->sent_to_host == false)
    {
        DBG("Cache is complete for group 0x%x, sending it to the local ranks (econtext: %p, number of connected clients: %ld, total: %ld)",
            group_uid,
            econtext,
            econtext->server->connected_clients.num_connected_clients,
            econtext->server->connected_clients.num_total_connected_clients);
        while (n < econtext->server->connected_clients.num_connected_clients)
        {
            dpu_offload_event_t *metaev;
            peer_info_t *c = DYN_ARRAY_GET_ELT(&(econtext->server->connected_clients.clients),
                                               idx, peer_info_t);
            if (c == NULL)
            {
                idx++;
                continue;
            }
            DBG("Send cache to client #%ld (id: %" PRIu64 ")", idx, c->id);
            dpu_offload_status_t rc = event_get(econtext->event_channels, NULL, &metaev);
            CHECK_ERR_RETURN((rc), DO_ERROR, "event_get() failed");
            assert(metaev);
            EVENT_HDR_TYPE(metaev) = META_EVENT_TYPE;
            rc = send_group_cache(econtext, c->ep, c->id, group_uid, metaev);
            CHECK_ERR_RETURN((rc), DO_ERROR, "send_group_cache() failed");
            QUEUE_EVENT(metaev);
            n++;
            idx++;
        }
        gp_cache->sent_to_host = true;
    }
    else
        DBG("cache aleady sent to host");
    return DO_SUCCESS;
}

dpu_offload_status_t send_cache(execution_context_t *econtext, cache_t *cache, ucp_ep_h dest_ep, uint64_t dest_id, dpu_offload_event_t *metaevt)
{
    // Note: it is all done using the notification channels so there is not
    // need to post receives. Simply send the data if anything needs to be sent
    dpu_offload_status_t rc;
    uint64_t key;
    group_cache_t *value;

    assert(metaevt);
    assert(EVENT_HDR_TYPE(metaevt) == META_EVENT_TYPE);

    kh_foreach(econtext->engine->procs_cache.data, key, value, {
        if (value != NULL)
        {
            rc = send_group_cache(econtext, dest_ep, dest_id, key, metaevt);
            CHECK_ERR_RETURN((rc), DO_ERROR, "exchange_group_cache() failed\n");
        }
    })

        return DO_SUCCESS;
}

bool all_service_procs_connected(offloading_engine_t *engine)
{
    if (!engine->on_dpu)
        return false;

    return (engine->num_service_procs == (engine->num_connected_service_procs + 1)); // Plus one because we do not connect to ourselves
}

dpu_offload_status_t broadcast_group_cache_revoke(offloading_engine_t *engine, group_uid_t group_uid, uint64_t n_ranks)
{
    size_t sp_gid;
    group_cache_t *cache;

    assert(engine);
    assert(group_uid != INT_MAX);
    assert(all_service_procs_connected(engine));
    assert(engine->on_dpu);

    if (engine->num_service_procs == 1)
    {
        // The configuration has a single service process, the current DPU, nothing to do
        return DO_SUCCESS;
    }

    cache = GET_GROUP_CACHE(&(engine->procs_cache), group_uid);
    assert(cache);
    for (sp_gid = 0; sp_gid < engine->num_service_procs; sp_gid++)
    {
        dpu_offload_status_t rc;
        // Meta-event to be used to track all that need to happen
        dpu_offload_event_t *ev;
        ucp_ep_h dest_ep;
        uint64_t dest_id = sp_gid;
        remote_service_proc_info_t *sp;
        sp = DYN_ARRAY_GET_ELT(&(engine->service_procs), sp_gid, remote_service_proc_info_t);
        assert(sp);

        // Do not send to self
        offloading_config_t *cfg = (offloading_config_t *)engine->config;
        if (sp_gid == cfg->local_service_proc.info.global_id)
            continue;

        assert(sp->econtext);
        event_get(sp->econtext->event_channels, NULL, &ev);
        assert(ev);
        EVENT_HDR_TYPE(ev) = META_EVENT_TYPE;
        dest_ep = GET_REMOTE_SERVICE_PROC_EP(engine, sp_gid);
        // If the econtext is a client to connect to a server, the dest_id is the index;
        // otherwise we need to find the client ID based on the index
        if (sp->econtext->type == CONTEXT_SERVER)
            dest_id = sp->client_id;
        DBG("Sending group cache revoke to service process #%ld (econtext: %p, scope_id: %d, dest_id: %ld, ep: %p)",
            sp_gid, sp->econtext, sp->econtext->scope_id, dest_id, dest_ep);
        rc = send_local_revoke_rank_group_cache(sp->econtext, dest_ep, dest_id, group_uid, n_ranks, ev);
        CHECK_ERR_RETURN((rc), DO_ERROR, "send_local_revoke_rank_group_cache() failed");
        QUEUE_EVENT(ev);
    }
    return DO_SUCCESS;
}

dpu_offload_status_t broadcast_group_cache(offloading_engine_t *engine, group_uid_t group_uid)
{
    size_t sp_gid;
    group_cache_t *cache;
    assert(engine);
    assert(group_uid != INT_MAX);

    // Check whether all the service processes are connected, if not, do not do anything
    if (!all_service_procs_connected(engine))
    {
        // Not all the DPUs are connected, we cannot exchange the data yet
        DBG("Not all service processes are connected, unable to broadcast group cache (num service processes: %ld, connected service processes: %ld)",
            engine->num_service_procs,
            engine->num_connected_service_procs);
        return DO_SUCCESS;
    }
    DBG("All service processes connected, starting broadcast of the cache entries for the local ranks");

    if (!engine->on_dpu)
    {
        ERR_MSG("Not on a DPU, not allowed to broadcast group cache");
        return DO_ERROR;
    }

    if (engine->num_service_procs == 1)
    {
        // The configuration has a single service process, the current DPU, nothing to do
        return DO_SUCCESS;
    }

    cache = GET_GROUP_CACHE(&(engine->procs_cache), group_uid);
    assert(cache);
    for (sp_gid = 0; sp_gid < engine->num_service_procs; sp_gid++)
    {
        dpu_offload_status_t rc;
        // Meta-event to be used to track all that need to happen
        dpu_offload_event_t *ev;
        ucp_ep_h dest_ep;
        uint64_t dest_id = sp_gid;
        remote_service_proc_info_t *sp;
        sp = DYN_ARRAY_GET_ELT(&(engine->service_procs), sp_gid, remote_service_proc_info_t);
        assert(sp);

        // Do not send to self
        offloading_config_t *cfg = (offloading_config_t *)engine->config;
        if (sp_gid == cfg->local_service_proc.info.global_id)
            continue;

        assert(sp->econtext);
        event_get(sp->econtext->event_channels, NULL, &ev);
        assert(ev);
        EVENT_HDR_TYPE(ev) = META_EVENT_TYPE;
        dest_ep = GET_REMOTE_SERVICE_PROC_EP(engine, sp_gid);
        // If the econtext is a client to connect to a server, the dest_id is the index, i.e., the global SP ID;
        // otherwise we need to find the client ID based on the index
        if (sp->econtext->type == CONTEXT_SERVER)
        {
            dest_id = sp->client_id;
        }
        DBG("Sending group cache to service process #%ld (econtext: %p, scope_id: %d, dest_id: %ld, ep: %p)",
            sp_gid, sp->econtext, sp->econtext->scope_id, dest_id, dest_ep);
        rc = send_local_rank_group_cache(sp->econtext, dest_ep, dest_id, group_uid, ev);
        CHECK_ERR_RETURN((rc), DO_ERROR, "send_local_rank_group_cache() failed");
        QUEUE_EVENT(ev);
    }

    // Timing for sending/receiving cache entries is obviously not always the same
    // so we check if the cache is full and needs to be sent to the local ranks
    // both when we receive cache entries and broadcast our cache. We may have received
    // all the remote data first
    if (cache->group_size == cache->num_local_entries)
    {
        pending_peer_cache_entry_t *pending_cache_entry_recv, *next_pending;
        execution_context_t *server = NULL;
        dpu_offload_status_t rc;
        bool found = false; // Did we find pending cache entries recv for the group or not?

        server = get_server_servicing_host(engine);
        assert(server);
        assert(server->scope_id == SCOPE_HOST_DPU);
        rc = send_gp_cache_to_host(server, group_uid);
        CHECK_ERR_RETURN((rc), DO_ERROR, "send_gp_cache_to_host() failed");

        // Now that we sent the cache entries to the other SPs, we check on the
        // pending receives of cache entries from other SPs.
        ucs_list_for_each_safe(pending_cache_entry_recv, next_pending, &(engine->pending_cache_entry_recv), item)
        {
            // Find the potential last cache entry, which at the top of the list (added to the head)
            if (pending_cache_entry_recv->gp_uid == group_uid)
            {
                found = true;
            }
        }
    }

    return DO_SUCCESS;
}

void display_group_cache(cache_t *cache, group_uid_t gp_uid)
{
    size_t i = 0;
    size_t idx = 0;
    group_cache_t *gp_cache = NULL;
    gp_cache = GET_GROUP_CACHE(cache, gp_uid);
    fprintf(stderr, "Content of cache for group 0x%x\n", gp_uid);
    fprintf(stderr, "-> group size: %ld\n", gp_cache->group_size);
    fprintf(stderr, "-> n_local_rank: %ld\n", gp_cache->n_local_ranks);
    fprintf(stderr, "-> n_local_ranks_populated: %ld\n", gp_cache->n_local_ranks_populated);
    fprintf(stderr, "-> num_local_entries: %ld\n", gp_cache->num_local_entries);
    fprintf(stderr, "-> sent_to_host: %d\n\n", gp_cache->sent_to_host);

    while (i < gp_cache->group_size)
    {
        peer_cache_entry_t *entry = GET_GROUPRANK_CACHE_ENTRY(cache, gp_uid, idx);
        if (entry->set)
        {
            fprintf(stderr, "Rank %" PRId64 "\n", entry->peer.proc_info.group_rank);
            assert(idx == entry->peer.proc_info.group_rank);
            i++;
        }
        idx++;
    }
}

static dpu_offload_status_t do_get_cache_entry_by_group_rank(offloading_engine_t *engine, group_uid_t gp_uid, int64_t rank, int64_t sp_idx, request_compl_cb_t cb, int64_t *sp_global_id, dpu_offload_event_t **ev)
{
    if (ev != NULL && cb != NULL)
    {
        ERR_MSG("%s(): both the event and the callback are defined, impossible to understand the context", __func__);
        return DO_ERROR;
    }

    // If the event is defined, the dpu_id must also be defined, they go in pairs
    if (ev != NULL)
        assert(sp_global_id);

    if (is_in_cache(&(engine->procs_cache), gp_uid, rank, -1))
    {
        // The cache has the data
        peer_cache_entry_t *cache_entry = GET_GROUP_RANK_CACHE_ENTRY(&(engine->procs_cache), gp_uid, rank, GROUP_SIZE_UNKNOWN);
        DBG("%" PRId64 " from group 0x%x is in the cache, service proc ID = %" PRId64, rank, gp_uid, cache_entry->shadow_service_procs[sp_idx]);
        if (ev != NULL)
        {
            *ev = NULL;
            *sp_global_id = cache_entry->shadow_service_procs[sp_idx];
        }
        return DO_SUCCESS;
    }

#if !NDEBUG
    // With the current implementation, it should always be in the cache
    WARN_MSG("rank %" PRId64 " from group 0x%x is not in the cache", rank, gp_uid);
    display_group_cache(&(engine->procs_cache), gp_uid);
    assert(0);
#endif

    // The cache does not have the data. We sent a request to get the data.
    // The caller is in charge of calling the function after completion to actually get the data
    rank_info_t rank_data;
    RESET_RANK_INFO(&rank_data);
    rank_data.group_uid = gp_uid;
    rank_data.group_rank = rank;

    // Create the local event so we can know when the cache entry has been received
    dpu_offload_event_t *cache_entry_updated_ev;
    peer_cache_entry_t *cache_entry = GET_GROUP_RANK_CACHE_ENTRY(&(engine->procs_cache), gp_uid, rank, GROUP_SIZE_UNKNOWN);
    dpu_offload_status_t rc = event_get(engine->self_econtext->event_channels, NULL, &cache_entry_updated_ev);
    CHECK_ERR_RETURN((rc), DO_ERROR, "event_get() failed");
    if (!cache_entry->events_initialized)
    {
        SIMPLE_LIST_INIT(&(cache_entry->events));
        cache_entry->events_initialized = true;
    }
    EVENT_HDR_TYPE(cache_entry_updated_ev) = META_EVENT_TYPE;
    SIMPLE_LIST_PREPEND(&(cache_entry->events), &(cache_entry_updated_ev->item));
    DBG("Cache entry %p for gp/rank 0x%x/%" PRIu64 " now has %ld update events",
        cache_entry, gp_uid, rank, SIMPLE_LIST_LENGTH(&(cache_entry->events)));
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
        request_data->gp_uid = gp_uid;
        request_data->rank = rank;
        request_data->target_sp_idx = sp_idx;
        request_data->offload_engine = engine;
        cache_entry_updated_ev->ctx.completion_cb = cb;
        cache_entry_updated_ev->ctx.completion_cb_ctx = (void *)request_data;
        assert(0); // FIXME: events cannot be on two lists
        // ucs_list_add_tail(&(engine->self_econtext->ongoing_events), &(cache_entry_updated_ev->item));
    }

    if (engine->on_dpu == true)
    {
        // If we are on a DPU, we need to send a request to all known DPUs
        // To track completion, we get an event from the execution context used for the
        // first DPU.
        size_t i;
        dpu_offload_status_t rc;
        dpu_offload_event_t *metaev = NULL;
        execution_context_t *meta_econtext = NULL;

        for (i = 0; i < engine->num_service_procs; i++)
        {
            remote_service_proc_info_t *sp;
            sp = DYN_ARRAY_GET_ELT(&(engine->service_procs), i, remote_service_proc_info_t);
            assert(sp);
            if (sp != NULL && sp->ep != NULL && sp->init_params.conn_params != NULL)
            {
                execution_context_t *econtext = ECONTEXT_FOR_SERVICE_PROC_COMMUNICATION(engine, i);
                CHECK_ERR_RETURN((econtext == NULL), DO_ERROR, "unable to get execution context to communicate with service process #%ld", i);
                uint64_t global_sp_id = LOCAL_ID_TO_GLOBAL(econtext, i);
                DBG("Sending cache entry request to service process #%ld (econtext: %p, scope_id: %d)",
                    global_sp_id,
                    econtext,
                    econtext->scope_id);

                if (metaev == NULL)
                {
                    meta_econtext = econtext;
                    rc = event_get(meta_econtext->event_channels, NULL, &metaev);
                    CHECK_ERR_RETURN((rc), DO_ERROR, "get_event() failed");
                    EVENT_HDR_TYPE(metaev) = META_EVENT_TYPE;
                }

                ucp_ep_h dpu_ep = sp->ep;
                dpu_offload_event_t *subev;
                rc = event_get(econtext->event_channels, NULL, &subev);
                CHECK_ERR_RETURN((rc), DO_ERROR, "event_get() failed");
                subev->is_subevent = true;
                rc = do_send_cache_entry_request(econtext, dpu_ep, i, &rank_data, subev);
                CHECK_ERR_RETURN((rc), DO_ERROR, "send_cache_entry_request() failed");
                DBG("Sub-event for sending cache to DPU %ld: %p", global_sp_id, subev);
                if (subev != NULL)
                {
                    // If the event did not complete right away, we add it as a sub-event to the meta-event so we can track everything
                    QUEUE_SUBEVENT(metaev, subev);
                }
            }
        }
        if (metaev)
        {
            assert(meta_econtext);
            if (!event_completed(metaev))
                QUEUE_EVENT(metaev);
        }
    }
    else
    {
        // If we are on the host, we need to send a request to our first shadow DPU
        DBG("Sending request for cache entry...");
        execution_context_t *econtext = engine->client;
        return send_cache_entry_request(econtext, GET_SERVER_EP(econtext), econtext->client->server_id, &rank_data, ev);
    }
    return DO_SUCCESS;
}

dpu_offload_status_t get_cache_entry_by_group_rank(offloading_engine_t *engine, group_uid_t gp_uid, int64_t rank, int64_t sp_idx, request_compl_cb_t cb)
{
    return do_get_cache_entry_by_group_rank(engine, gp_uid, rank, sp_idx, cb, NULL, NULL);
}

dpu_offload_status_t get_sp_id_by_group_rank(offloading_engine_t *engine, group_uid_t gp_uid, int64_t rank, int64_t sp_idx, int64_t *sp_id, dpu_offload_event_t **ev)
{
    return do_get_cache_entry_by_group_rank(engine, gp_uid, rank, sp_idx, NULL, sp_id, ev);
}

dpu_offload_status_t get_sp_ep_by_id(offloading_engine_t *engine, uint64_t sp_id, ucp_ep_h *sp_ep, execution_context_t **econtext_comm, uint64_t *comm_id)
{
    CHECK_ERR_RETURN((engine == NULL), DO_ERROR, "engine is undefined");
    CHECK_ERR_RETURN((sp_id >= engine->num_service_procs),
                     DO_ERROR,
                     "request service process #%ld but only %ld service processes are known",
                     sp_id, engine->num_service_procs);
    remote_service_proc_info_t *sp;
    sp = DYN_ARRAY_GET_ELT(&(engine->service_procs), sp_id, remote_service_proc_info_t);
    if (sp == NULL)
    {
        *sp_ep = NULL;
        *econtext_comm = NULL;
        DBG("Endpoint not available");
        // This is not an error, just that the data is not yet available.
        return DO_SUCCESS;
    }

    // If not, we find the appropriate client or server
    *sp_ep = GET_REMOTE_SERVICE_PROC_EP(engine, sp_id);
    *econtext_comm = GET_REMOTE_SERVICE_PROC_ECONTEXT(engine, sp_id);
    switch ((*econtext_comm)->type)
    {
    case CONTEXT_SERVER:
        // If the DPU is a local client, we cannot use the global service process's ID,
        // we have to look up the local ID that can be used to send notifications.
        *comm_id = sp->client_id;
        break;
    case CONTEXT_CLIENT:
        *comm_id = sp_id;
        break;
    case CONTEXT_SELF:
        *comm_id = 0;
        break;
    default:
        *comm_id = UINT64_MAX;
    }

    DBG("Details to communicate with service process #%" PRIu64 ": econtext=%p ep=%p comm_id=%" PRIu64, sp_id, *econtext_comm, *sp_ep, *comm_id);

#if !NDEBUG
    // Some checks in debug mode if the destination is really self
    if (sp_id == engine->config->local_service_proc.info.global_id)
    {
        assert(*econtext_comm == engine->self_econtext);
        assert(*comm_id == 0);
    }
#endif
    assert(*econtext_comm);
    assert(*sp_ep);
    assert(*comm_id != UINT64_MAX);
    return DO_SUCCESS;
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

// <dpu_hostname:interdpu-port1&interdpu-port2,...:host-conn-port1&host-conn-port2,...>
static inline bool parse_dpu_cfg(char *str, dpu_config_data_t *config_entry)
{
    assert(str);
    assert(config_entry);

    DYN_ARRAY_ALLOC(&(config_entry->version_1.interdpu_ports), 2, int);
    DYN_ARRAY_ALLOC(&(config_entry->version_1.host_ports), 2, int);

    char *rest = str;
    char *token = strtok_r(rest, ":", &rest);
    assert(token);
    int step = 0;
    int j;
    if (config_entry->version_1.hostname == NULL)
    {
        // freed when calling offload_config_free()
        config_entry->version_1.hostname = strdup(token);
    }
    token = strtok_r(rest, ":", &rest);
    assert(token);
    while (token != NULL)
    {
        switch (step)
        {
        case 0:
            DBG("-> addr is %s", token);
            config_entry->version_1.addr = strdup(token); // freed when calling offload_config_free()
            step++;
            token = strtok_r(rest, ":", &rest);
            break;
        case 1:
            j = 0;
            char *interdpu_ports_str = token;
            char *token_port = strtok_r(interdpu_ports_str, "&", &interdpu_ports_str);
            assert(token_port); // We must have at least one port
            while (token_port != NULL)
            {
                DBG("-> inter-DPU port #%d is %s", j, token_port);
                int *port = DYN_ARRAY_GET_ELT(&(config_entry->version_1.interdpu_ports), j, int);
                *port = atoi(token_port);
                j++;
                token_port = strtok_r(interdpu_ports_str, "&", &interdpu_ports_str);
            }
            token = strtok_r(rest, ":", &rest);
            assert(token);
            config_entry->version_1.num_interdpu_ports = j;
            step++;
            break;
        case 2:
            j = 0;
            char *host_ports_str = token;
            token_port = strtok_r(host_ports_str, "&", &host_ports_str);
            assert(token_port); // We must have at least one port
            while (token_port != NULL)
            {
                DBG("-> port to connect with host #%d is %s", j, token_port);
                int *port = DYN_ARRAY_GET_ELT(&(config_entry->version_1.host_ports), j, int);
                *port = atoi(token_port);
                j++;
                token_port = strtok_r(host_ports_str, "&", &host_ports_str);
            }
            config_entry->version_1.num_host_ports = j;
            return true;
        }
    }

    DBG("unable to parse entry, stopping at step %d", step);
    return false;
}

/**
 * @brief Main function to parse the content of the configuration file in the context of service processes.
 * The function relies on the placeholders created while parsing the list of DPUs to be used.
 * Format: <host name>,<dpu1_hostname:dpu_conn_addr:interdpu-port1&interdpu-port2,...:rank-conn-port1&rank-conn-port2,...>,...
 */
bool parse_line_dpu_version_1(offloading_config_t *data, char *line)
{
    int idx = 0;
    size_t dpu_idx = 0;
    size_t cur_global_sp_id = 0;
    bool rc = false;
    char *rest_line = line;
    dpu_config_data_t *target_entry = NULL;
    remote_dpu_info_t **list_dpus = NULL;

    assert(data);
    assert(data->offloading_engine);
    assert(line);
    list_dpus = LIST_DPUS_FROM_ENGINE(data->offloading_engine);
    assert(list_dpus);

    while (line[idx] == ' ')
        idx++;

    char *token = strtok_r(rest_line, ",", &rest_line);

    // The host's name does not really matter here, moving to the service process(es) configuration
    token = strtok_r(rest_line, ",", &rest_line);
    if (token == NULL)
        ERR_MSG("unable to parse: %s", line);
    assert(token);
    while (token != NULL)
    {
        bool target_dpu = false;
        size_t i;
        DBG("-> Service proc data: %s", token);

        /* if the DPU is not part of the list of DPUs to use, we skip it */
        for (i = 0; i < data->num_dpus; i++)
        {
            dpu_config_data_t *entry = DYN_ARRAY_GET_ELT(&(data->dpus_config), i, dpu_config_data_t);
            assert(entry);
            assert(entry->version_1.hostname);
            // We do not expect users to be super strict in the way they create the list of DPUs
            size_t _strlen = strlen(token);
            if (_strlen > strlen(entry->version_1.hostname))
                _strlen = strlen(entry->version_1.hostname);
            if (strncmp(entry->version_1.hostname, token, _strlen) == 0)
            {
                target_dpu = true;
                target_entry = entry;
                dpu_idx = i;
                DBG("Found the configuration for %s", entry->version_1.hostname);
                break;
            }
        }

        if (target_dpu)
        {
            size_t sp_idx = 0;
            size_t d;
            // Find the corresponding remote_dpu_info_t structure so we can populate it
            // while parsing the line.
            remote_dpu_info_t *cur_dpu = NULL;
            for (d = 0; d < data->num_dpus; d++)
            {
                remote_dpu_info_t *remote_dpu = list_dpus[d];
                size_t _strlen = strlen(remote_dpu->hostname);
                if (strlen(target_entry->version_1.hostname) < _strlen)
                    _strlen = strlen(target_entry->version_1.hostname);
                assert(remote_dpu);
                assert(remote_dpu->hostname);
                if (strncmp(target_entry->version_1.hostname, remote_dpu->hostname, _strlen) == 0)
                {
                    cur_dpu = remote_dpu;
                    break;
                }
            }
            if (cur_dpu == NULL)
            {
                ERR_MSG("Unable to find data for DPU %s", target_entry->version_1.hostname);
                return false;
            }
            assert(cur_dpu);

            bool parsing_okay = parse_dpu_cfg(token, target_entry);
            CHECK_ERR_RETURN((parsing_okay == false), false, "unable to parse config file entry");
            // We now have the configuration associated to the line we just parsed, checking a few things...
            assert(target_entry->version_1.addr);

            /*
             * Save the configuration details of each service process on that DPU.
             * This is for example where we figure out which port should be used to connect to
             * a given service process.
             */
            remote_service_proc_info_t *cur_sp, *next_sp;
            SIMPLE_LIST_FOR_EACH(cur_sp, next_sp, &(cur_dpu->remote_service_procs), item)
            {
                size_t sp_port_idx = cur_sp->idx % data->num_service_procs_per_dpu;
                assert(cur_sp->init_params.conn_params);
                cur_sp->init_params.conn_params->addr_str = target_entry->version_1.addr;
                int *port = DYN_ARRAY_GET_ELT(&(target_entry->version_1.interdpu_ports),
                                              sp_idx,
                                              int);
                cur_sp->init_params.conn_params->port = *port;
                service_proc_config_data_t *sp_config;
                sp_config = DYN_ARRAY_GET_ELT(&(data->sps_configs), cur_global_sp_id, service_proc_config_data_t);
                assert(sp_config);
                cur_sp->config = sp_config;
                sp_config->version_1.hostname = target_entry->version_1.hostname;
                int *intersp_port = DYN_ARRAY_GET_ELT(&(target_entry->version_1.interdpu_ports), sp_port_idx, int);
                int *host_port = DYN_ARRAY_GET_ELT(&(target_entry->version_1.host_ports), sp_idx, int);
                sp_config->version_1.host_port = *host_port;
                sp_config->version_1.intersp_port = *intersp_port;
                cur_sp->init_params.conn_params->port = *intersp_port;
                cur_sp->init_params.conn_params->addr_str = target_entry->version_1.addr;
                sp_idx++;
                cur_global_sp_id++;
            }

            if (strncmp(data->local_service_proc.hostname, target_entry->version_1.hostname, strlen(target_entry->version_1.hostname)) == 0)
            {
                remote_service_proc_info_t *sp = NULL;
                // This is the DPU's configuration we were looking for
                DBG("-> This is the current DPU");

                // We now know that the configuration is about the local DPU. Based on this, the nest
                // steps are:
                // 1. Find how many service processes are before/after us to update the list of service
                //    processes we need to connect to and the list of service processes we expect to
                //    connect to us.
                // 2. Update the data of the local service processes, e.g., ports and so on.
                // In debug mode, we make sure the local ID for the service proc is correct
                if (data->local_service_proc.info.global_id == UINT64_MAX)
                {
                    assert(data->local_service_proc.info.local_id == 0);
                    // This assumes the global ID was not set in the environment and therefore
                    // we assume a single service process per DPU and as a result the global ID
                    // of the service process is equal to the DPU global ID
                    data->local_service_proc.info.global_id = dpu_idx;
                }
                assert(data->local_service_proc.info.local_id != UINT64_MAX);
                assert(data->local_service_proc.info.global_id != UINT64_MAX);
                assert(data->num_service_procs_per_dpu != 0);
                assert(data->num_service_procs_per_dpu != UINT64_MAX);
                assert(SIMPLE_LIST_LENGTH(&(cur_dpu->remote_service_procs)) == data->num_service_procs_per_dpu);

                if (data->local_service_proc.info.global_id < data->num_service_procs_per_dpu * (dpu_idx + 1))
                    data->service_proc_found = true;
                data->local_service_proc.inter_service_procs_init_params.id_set = true;
                data->local_service_proc.inter_service_procs_init_params.id = data->local_service_proc.info.global_id;
                data->local_service_proc.host_init_params.id_set = true;
                data->local_service_proc.host_init_params.id = data->local_service_proc.info.global_id;
                int *intersp_port = DYN_ARRAY_GET_ELT(&(target_entry->version_1.interdpu_ports),
                                                      data->local_service_proc.info.local_id, int);
                int *host_port = DYN_ARRAY_GET_ELT(&(target_entry->version_1.host_ports),
                                                   data->local_service_proc.info.local_id, int);
                data->local_service_proc.inter_service_procs_conn_params.port = *intersp_port;
                data->local_service_proc.inter_service_procs_conn_params.addr_str = target_entry->version_1.addr;
                data->local_service_proc.host_conn_params.port = *host_port;
                data->local_service_proc.host_conn_params.addr_str = target_entry->version_1.addr;
                assert(data->local_service_proc.info.dpu <= data->offloading_engine->num_dpus);
                sp = DYN_ARRAY_GET_ELT(&(data->offloading_engine->service_procs), data->local_service_proc.info.global_id, remote_service_proc_info_t);
                assert(sp);
                sp->ep = data->offloading_engine->self_ep;
                // data->local_dpu.id is already set while parsing the list of DPUs to use for the job
                rc = true;
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

/**
 * @brief Main function to parse the configuration file in the context of the host.
 * Format: <host name>,<dpu1_hostname:dpu_conn_addr:interdpu-port:rank-conn-port>,...
 */
bool parse_line_version_1(char *target_hostname, offloading_config_t *data, char *line)
{
    int idx = 0;
    char *rest = line;
    assert(target_hostname);
    assert(data);
    assert(line);

    while (line[idx] == ' ')
        idx++;

    char *token = strtok_r(rest, ",", &rest);
    DBG("Checking entry for %s", token);
    if (strncmp(token, target_hostname, strlen(token)) == 0)
    {
        // We found the hostname

        // Next tokens are the local DPUs' data
        // We get the DPUs configuration one-by-one.
        token = strtok_r(rest, ",", &rest);
        while (token != NULL)
        {
            // We overwrite the configuration until we find the entry associated to the host
            dpu_config_data_t *dpu_config = DYN_ARRAY_GET_ELT(&(data->dpus_config), 0, dpu_config_data_t);
            assert(dpu_config);
            CHECK_ERR_RETURN((dpu_config == NULL), DO_ERROR, "unable to allocate resources for DPUs' configuration");

            bool rc = parse_dpu_cfg(token, dpu_config);
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
 * @brief parse_line parses a line of the configuration file looking for a specific host name.
 * It shall not be used to seek the configuration of a DPU.
 *
 * @param target_hostname Host's name
 * @param line Line from the configuration file that is being parsed
 * @param data Configuration data
 * @return true when the line includes the host's configuration
 * @return false when the lines does not include the host's configuration
 */
bool parse_line(char *target_hostname, char *line, offloading_config_t *data)
{
    assert(target_hostname);
    assert(line);
    assert(data);
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
 * @brief parse_line_for_dpu_cfg parses a line of the configuration file looking for a specific DPU.
 * It shall not be used to seek the configuration of a host.
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
    dpu_offload_status_t rc = DO_ERROR;
    bool found_self = false;

    assert(filepath);
    assert(config_data);

    // Read the entire file so we can go over the content quickly. Configure files are not expected to get huge
    FILE *file = fopen(filepath, "rb");
    if (file == NULL)
        ERR_MSG("unable to open %s", filepath);
    assert(file);
    fseek(file, 0, SEEK_END);
    len = ftell(file);
    fseek(file, 0, SEEK_SET); /* same as rewind(f); */

    char *content = DPU_OFFLOAD_MALLOC(len + 1);
    size_t nread = fread(content, len, 1, file);
    fclose(file);
    assert(nread == 1);
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

    assert(filepath);
    assert(hostname);
    assert(data);

    file = fopen(filepath, "r");
    if (file == NULL)
        ERR_MSG("unable to open %s", filepath);

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

    // The configuration is stored in the first element of dpus_configs
    dpu_config_data_t *dpu_config = DYN_ARRAY_GET_ELT(&(data->dpus_config), 0, dpu_config_data_t);
    assert(dpu_config);

    // At this point if we do not know how many service processes are on the DPU, we use the number
    // of ports for the host to connect to set it.
    if (data->num_service_procs_per_dpu == 0)
        data->num_service_procs_per_dpu = dpu_config->version_1.num_host_ports;

    // Same for the DPU id that we are associated with. We currently assume we have a single DPU
    if (data->local_service_proc.info.dpu == UINT64_MAX)
    {
        // When parsing the configuration file, the number of DPUs stops increasing when we find the local DPU
        // so the DPU ID is the number of DPUs - 1
        data->local_service_proc.info.dpu = data->num_dpus - 1;
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

    // If a configuration file is not set, we try to use the default one
    if (config_data->config_file == NULL)
    {
        config_data->config_file = getenv(OFFLOAD_CONFIG_FILE_PATH_ENVVAR);
    }
    hostname[1023] = '\0';
    gethostname(hostname, 1023);

    config_data->list_dpus = NULL;                                                         // Not used on host
    RESET_INIT_PARAMS(&(config_data->local_service_proc.inter_service_procs_init_params)); // Not used on host
    RESET_INIT_PARAMS(&(config_data->local_service_proc.host_init_params));                // Not used on host

    /* First, we check whether we know about a configuration file. If so, we load all the configuration details from it */
    /* If there is no configuration file, we try to configuration from environment variables */
    if (config_data->config_file != NULL)
    {
        DBG("Looking for %s's configuration data from %s", hostname, config_data->config_file);
        rc = find_config_from_platform_configfile(config_data->config_file, hostname, config_data);
        CHECK_ERR_RETURN((rc), DO_ERROR, "find_config_from_platform_configfile() failed");
        assert(config_data->num_service_procs_per_dpu > 0);
        assert(config_data->num_service_procs_per_dpu != UINT64_MAX);
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

    return DO_SUCCESS;
}

execution_context_t *get_server_servicing_host(offloading_engine_t *engine)
{
    size_t i;
    // We start at the end of the list because the server servicing the host
    // is traditionally the last one added
    // Fixme: avoid o(n) lookup
    for (i = engine->num_servers - 1; i >= 0; i--)
    {
        if (engine->servers[i] == NULL)
            continue;
        if (engine->servers[i]->scope_id == SCOPE_HOST_DPU)
            return (engine->servers[i]);
    }

    return NULL;
}

dpu_offload_status_t get_num_connecting_ranks(uint64_t num_service_procs_per_dpu, int64_t n_local_ranks, uint64_t sp_lid, uint64_t *n_ranks)
{
    uint64_t base = (uint64_t)n_local_ranks / num_service_procs_per_dpu;
    uint64_t rest = (uint64_t)n_local_ranks - (num_service_procs_per_dpu * base);
    if (sp_lid < rest)
    {
        *n_ranks = base + 1;
        return DO_SUCCESS;
    }
    *n_ranks = base;
    return DO_SUCCESS;
}

dpu_offload_status_t get_local_service_proc_connect_info(offloading_config_t *cfg, rank_info_t *rank_info, init_params_t *init_params)
{
    int64_t service_proc_local_id = 0;
    dpu_config_data_t *entry = NULL;
    int *host_port = NULL;
    assert(cfg);
    assert(rank_info);
    assert(init_params);
    assert(cfg->num_service_procs_per_dpu != UINT64_MAX);
    assert(cfg->local_service_proc.info.dpu != UINT64_MAX);
    assert(cfg->num_service_procs_per_dpu > 0);

    if (rank_info->local_rank != INVALID_RANK)
    {
        service_proc_local_id = rank_info->local_rank % cfg->num_service_procs_per_dpu;
    }

    entry = DYN_ARRAY_GET_ELT(&(cfg->dpus_config), cfg->local_service_proc.info.dpu, dpu_config_data_t);
    cfg->local_service_proc.info.local_id = service_proc_local_id;
    cfg->local_service_proc.info.global_id = cfg->local_service_proc.info.dpu * cfg->num_service_procs_per_dpu + service_proc_local_id;
    // The config of our DPU is always the first one in the list.
    dpu_config_data_t *dpu_config = DYN_ARRAY_GET_ELT(&(cfg->dpus_config), 0, dpu_config_data_t);
    assert(dpu_config);
    host_port = DYN_ARRAY_GET_ELT(&(dpu_config->version_1.host_ports), service_proc_local_id, int);
    init_params->conn_params->port = *host_port;
    assert(dpu_config->version_1.addr);
    init_params->conn_params->addr_str = dpu_config->version_1.addr;
    DBG("Service process connection info - port: %d, addr: %s, local_id: %" PRIu64 ", global_id: %" PRIu64,
        init_params->conn_params->port,
        init_params->conn_params->addr_str,
        cfg->local_service_proc.info.local_id,
        cfg->local_service_proc.info.global_id);
    return DO_SUCCESS;
}

void offload_config_free(offloading_config_t *cfg)
{
    size_t i;
    for (i = 0; i < cfg->num_dpus; i++)
    {
        dpu_config_data_t *dpu_config = DYN_ARRAY_GET_ELT(&(cfg->dpus_config), i, dpu_config_data_t);
        if (dpu_config != NULL)
        {
            if (dpu_config->version_1.hostname != NULL)
            {
                free(dpu_config->version_1.hostname);
                dpu_config->version_1.hostname = NULL;
            }

            if (dpu_config->version_1.addr != NULL)
            {
                free(dpu_config->version_1.addr);
                dpu_config->version_1.addr = NULL;
            }

            DYN_ARRAY_FREE(&(dpu_config->version_1.interdpu_ports));
            DYN_ARRAY_FREE(&(dpu_config->version_1.host_ports));
        }
    }

    DYN_LIST_FREE(cfg->info_connecting_to.pool_remote_sp_connect_to, connect_to_service_proc_t, item);

    DYN_ARRAY_FREE(&(cfg->dpus_config));
    DYN_ARRAY_FREE(&(cfg->sps_configs));
}

#if !NDEBUG
__attribute__((destructor)) void calledLast()
{
    if (dbg_cfg.my_hostname != NULL)
    {
        free(dbg_cfg.my_hostname);
        dbg_cfg.my_hostname = NULL;
    }
}
#endif
