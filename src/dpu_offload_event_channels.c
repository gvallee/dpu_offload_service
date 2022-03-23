#define _POSIX_C_SOURCE 200809L

//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <inttypes.h>
#include <string.h>

#include <ucp/api/ucp.h>

#include "dpu_offload_types.h"
#include "dpu_offload_mem_mgt.h"
#include "dpu_offload_event_channels.h"
#include "dpu_offload_debug.h"

#define DEFAULT_NUM_EVTS (32)
#define DEFAULT_NUM_NOTIFICATION_CALLBACKS (5000)

static int handle_am_msg(execution_context_t *econtext, am_header_t *hdr, size_t header_length, void *data, size_t length)
{
    notification_callback_entry_t *list_callbacks = (notification_callback_entry_t *)econtext->event_channels->notification_callbacks.base;
    DBG("Notification of type %" PRIu64 " received via RDV message, dispatching...", hdr->type);
    if (list_callbacks[hdr->type].set == false)
    {
        pending_notification_t *pending_notif;
        DBG("callback not available for %" PRIu64, hdr->type);
        DYN_LIST_GET(econtext->event_channels->free_pending_notifications, pending_notification_t, item, pending_notif);
        CHECK_ERR_RETURN((pending_notif == NULL), UCS_ERR_NO_MESSAGE, "unable to get pending notification object");

        pending_notif->type = hdr->type;
        pending_notif->src_id = hdr->id;
        pending_notif->data_size = length;
        pending_notif->header_size = header_length;
        pending_notif->econtext = econtext;
        if (pending_notif->data_size > 0)
        {
            pending_notif->data = malloc(pending_notif->data_size);
            memcpy(pending_notif->data, data, pending_notif->data_size);
        }
        else
        {
            pending_notif->data = NULL;
        }
        if (pending_notif->header_size > 0)
        {
            pending_notif->header = malloc(pending_notif->header_size);
            memcpy(pending_notif->header, hdr, pending_notif->header_size);
        }
        else
        {
            pending_notif->header = NULL;
        }
        ucs_list_add_tail(&(econtext->event_channels->pending_notifications), &(pending_notif->item));
        return UCS_OK;
    }

    notification_cb cb = list_callbacks[hdr->type].cb;
    CHECK_ERR_RETURN((cb == NULL), UCS_ERR_NO_MESSAGE, "Callback is undefined");
    struct dpu_offload_ev_sys *ev_sys = EV_SYS(econtext);
    cb(ev_sys, econtext, hdr, header_length, data, length);
    return UCS_OK;
}

static void am_rdv_recv_cb(void *request, ucs_status_t status, size_t length, void *user_data)
{
    pending_am_rdv_recv_t *recv_info = (pending_am_rdv_recv_t *)user_data;
    int rc = handle_am_msg(recv_info->econtext, recv_info->hdr, recv_info->hdr_len, recv_info->user_data, recv_info->payload_size);
    ucp_request_free(request);
    ucs_list_del(&(recv_info->item));
    DYN_LIST_RETURN(recv_info->econtext->free_pending_rdv_recv, recv_info, item);
}

static ucs_status_t am_notification_recv_rdv_msg(execution_context_t *econtext, am_header_t *hdr, size_t hdr_len, size_t payload_size, void *desc)
{
    ucp_request_param_t am_rndv_recv_request_params;
    pending_am_rdv_recv_t *pending_recv;
    DYN_LIST_GET(econtext->free_pending_rdv_recv, pending_am_rdv_recv_t, item, pending_recv);
    assert(pending_recv);
    DBG("RDV message to be received for type %ld", hdr->type);
    // Make sure we have space for the payload, note that we do not know the data size to
    // be received in advance but we do our best to avoid mallocs
    if (pending_recv->buff_size == 0)
    {
        pending_recv->user_data = malloc(payload_size);
        pending_recv->buff_size = payload_size;
    }
    if (pending_recv->buff_size < payload_size)
    {
        pending_recv->user_data = realloc(pending_recv->user_data, payload_size);
        pending_recv->buff_size = payload_size;
    }
    assert(pending_recv->user_data);
    pending_recv->hdr = hdr;
    pending_recv->hdr_len = hdr_len;
    pending_recv->econtext = econtext;
    pending_recv->payload_size = payload_size;
    am_rndv_recv_request_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                                               UCP_OP_ATTR_FIELD_USER_DATA |
                                               UCP_OP_ATTR_FIELD_DATATYPE |
                                               UCP_OP_ATTR_FIELD_MEMORY_TYPE;
    am_rndv_recv_request_params.cb.recv_am = &am_rdv_recv_cb;
    am_rndv_recv_request_params.datatype = ucp_dt_make_contig(1);
    am_rndv_recv_request_params.user_data = pending_recv;
    am_rndv_recv_request_params.memory_type = UCS_MEMORY_TYPE_HOST;
    am_rndv_recv_request_params.request = NULL;

    ucs_status_ptr_t status;
    status = ucp_am_recv_data_nbx(GET_WORKER(econtext), desc, pending_recv->user_data, payload_size,
                                  &am_rndv_recv_request_params);
    if (UCS_PTR_IS_ERR(status))
    {
        /* non-recoverable error */
        ERR_MSG("ucp_am_recv_data_nbx() failed");
    }
    else if (UCS_PTR_IS_PTR(status))
    {
        /* request not yet completd */
        DBG("ucp_am_recv_data_nbx() is INPROGRESS");
        pending_recv->req = status;
    }
    else
    {
        DBG("ucp_am_recv_data_nbx() completed right away");
        assert(NULL == status);
        pending_recv->req = NULL;
        handle_am_msg(econtext, hdr, hdr_len, pending_recv->user_data, payload_size);
        ucp_request_free(am_rndv_recv_request_params.request);
    }
    return UCS_OK;
}

static ucs_status_t am_notification_msg_cb(void *arg, const void *header, size_t header_length,
                                           void *data, size_t length,
                                           const ucp_am_recv_param_t *param)
{
    CHECK_ERR_RETURN((header == NULL), UCS_ERR_NO_MESSAGE, "header is undefined");
    CHECK_ERR_RETURN((header_length != sizeof(am_header_t)), UCS_ERR_NO_MESSAGE, "header len is invalid");
    execution_context_t *econtext = (execution_context_t *)arg;
    am_header_t *hdr = (am_header_t *)header;
    CHECK_ERR_RETURN((hdr == NULL), UCS_ERR_NO_MESSAGE, "header is NULL");
    CHECK_ERR_RETURN((hdr->type >= econtext->event_channels->notification_callbacks.num_elts), UCS_ERR_NO_MESSAGE, "notification callback %" PRIu64 " is out of range", hdr->type);

    if (param->recv_attr & UCP_AM_RECV_ATTR_FLAG_RNDV)
    {
        // RDV message
        am_notification_recv_rdv_msg(econtext, hdr, header_length, length, data);
        return UCS_INPROGRESS;
    }

    DBG("Notification of type %" PRIu64 " received via eager message, dispatching...", hdr->type);
    return handle_am_msg(econtext, hdr, header_length, data, length);
}

dpu_offload_status_t event_channels_init(execution_context_t *econtext)
{
    CHECK_ERR_RETURN((econtext == NULL), DO_ERROR, "Undefined execution context");
    CHECK_ERR_RETURN((GET_WORKER(econtext) == NULL), DO_ERROR, "Undefined worker");

    econtext->event_channels = malloc(sizeof(dpu_offload_ev_sys_t));
    CHECK_ERR_RETURN((econtext->event_channels == NULL), DO_ERROR, "Resource allocation failed");

    size_t num_evts = DEFAULT_NUM_EVTS;
    size_t num_free_pending_notifications = DEFAULT_NUM_NOTIFICATION_CALLBACKS;
    DYN_LIST_ALLOC(econtext->event_channels->free_evs, num_evts, dpu_offload_event_t, item);
    DYN_LIST_ALLOC(econtext->event_channels->free_pending_notifications, num_free_pending_notifications, pending_notification_t, item);
    ucs_list_head_init(&(econtext->event_channels->pending_notifications));
    econtext->event_channels->num_used_evs = 0;
    DYN_ARRAY_ALLOC(&(econtext->event_channels->notification_callbacks), DEFAULT_NUM_NOTIFICATION_CALLBACKS, notification_callback_entry_t);
    CHECK_ERR_RETURN((econtext->event_channels->notification_callbacks.base == NULL), DO_ERROR, "Resource allocation failed");

    // Register the UCX AM handler
    ucp_am_handler_param_t ev_eager_param;
    ev_eager_param.field_mask = UCP_AM_HANDLER_PARAM_FIELD_ID |
                                UCP_AM_HANDLER_PARAM_FIELD_CB |
                                UCP_AM_HANDLER_PARAM_FIELD_ARG |
                                UCP_AM_FLAG_WHOLE_MSG;
    ev_eager_param.id = AM_EVENT_MSG_ID;
    // For all exchange, we receive the header first and from there post a receive for the either eager or RDV message.
    ev_eager_param.cb = am_notification_msg_cb;
    ev_eager_param.arg = econtext;
    DBG("Registering AM eager callback for notifications (type=%d, econtext=%p, worker=%p, ev_param=%p)",
        AM_EVENT_MSG_ID,
        econtext,
        GET_WORKER(econtext),
        &ev_eager_param);
    ucs_status_t status = ucp_worker_set_am_recv_handler(GET_WORKER(econtext),
                                                         &ev_eager_param);
    CHECK_ERR_RETURN((status != UCS_OK), DO_ERROR, "unable to set AM eager recv handler");
    return DO_SUCCESS;
}

dpu_offload_status_t event_channel_register(dpu_offload_ev_sys_t *ev_sys, uint64_t type, notification_cb cb)
{
    CHECK_ERR_RETURN((cb == NULL), DO_ERROR, "Undefined callback");
    CHECK_ERR_RETURN((ev_sys == NULL), DO_ERROR, "undefined event system");
    notification_callback_entry_t *list_callbacks = (notification_callback_entry_t *)ev_sys->notification_callbacks.base;
    notification_callback_entry_t *entry;
    DYN_ARRAY_GET_ELT(&(ev_sys->notification_callbacks), type, notification_callback_entry_t, entry);
    CHECK_ERR_RETURN((entry == NULL), DO_ERROR, "unable to get callback %ld", type);
    CHECK_ERR_RETURN((entry->set == true), DO_ERROR, "type %" PRIu64 " is already set", type);

    entry->cb = cb;
    entry->set = true;
    entry->ev_sys = (struct dpu_offload_ev_sys *)ev_sys;

    /* check for any pending notification that would match */
    pending_notification_t *pending_notif, *next_pending_notif;
    ucs_list_for_each_safe(pending_notif, next_pending_notif, (&(ev_sys->pending_notifications)), item)
    {
        if (pending_notif->type == type)
        {
            ucs_list_del(&(pending_notif->item));
            cb((struct dpu_offload_ev_sys *)ev_sys,
               pending_notif->econtext,
               pending_notif->header,
               pending_notif->header_size,
               pending_notif->data,
               pending_notif->data_size);
            if (pending_notif->data != NULL)
            {
                free(pending_notif->data);
                pending_notif->data = NULL;
            }
            if (pending_notif->header != NULL)
            {
                free(pending_notif->header);
                pending_notif->header = NULL;
            }
        }
    }

    return DO_SUCCESS;
}

dpu_offload_status_t event_channel_deregister(dpu_offload_ev_sys_t *ev_sys, uint64_t type)
{
    CHECK_ERR_RETURN((ev_sys == NULL), DO_ERROR, "undefined event system");
    CHECK_ERR_RETURN((ev_sys->notification_callbacks.num_elts <= type), DO_ERROR, "type %" PRIu64 " is out of range", type);

    notification_callback_entry_t *list_callbacks = (notification_callback_entry_t *)ev_sys->notification_callbacks.base;
    notification_callback_entry_t *entry = &(list_callbacks[type]);
    CHECK_ERR_RETURN((entry->set == false), DO_ERROR, "type %" PRIu64 " is not registered", type);

    entry->set = false;
    return DO_SUCCESS;
}

static void notification_emit_cb(void *user_data, const char *type_str)
{
    dpu_offload_event_t *ev;
    am_req_t *ctx;
    if (user_data == NULL)
    {
        ERR_MSG("user_data passed to %s mustn't be NULL", type_str);
        return;
    }
    ev = (dpu_offload_event_t *)user_data;
    if (ev == NULL)
        return;
    ev->ctx.complete = 1;
    DBG("evt %p now completed", ev);
}

int event_channel_emit(dpu_offload_event_t *ev, uint64_t my_id, uint64_t type, ucp_ep_h dest_ep, void *ctx, void *payload, size_t payload_size)
{
    ucp_request_param_t params;
    DBG("Sending notification of type %" PRIu64 " (client_id=%" PRIu64 ")", type, my_id);
    ev->ctx.complete = 0;
    ev->ctx.hdr.type = type;
    ev->ctx.hdr.id = my_id;
    ev->user_context = ctx;
    params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                          UCP_OP_ATTR_FIELD_DATATYPE |
                          UCP_OP_ATTR_FIELD_USER_DATA;
    params.datatype = ucp_dt_make_contig(1);
    params.user_data = &ev;
    params.cb.send = (ucp_send_nbx_callback_t)notification_emit_cb;
    ev->req = ucp_am_send_nbx(dest_ep, AM_EVENT_MSG_ID, &(ev->ctx.hdr), sizeof(am_header_t), payload, payload_size, &params);
    DBG("Event %p successfully emitted", ev);
    if (ev->req == NULL)
    {
        // Immediate completion, the callback is *not* invoked
        ev->ctx.complete = 1;
        return EVENT_DONE;
    }

    if (UCS_PTR_IS_ERR(ev->req))
        return UCS_PTR_STATUS(ev->req);

    return EVENT_INPROGRESS;
}

dpu_offload_status_t event_channels_fini(dpu_offload_ev_sys_t **ev_sys)
{
    if ((*ev_sys)->num_used_evs > 0)
    {
        WARN_MSG("[WARN] %ld events objects have not been returned", (*ev_sys)->num_used_evs);
    }

    DYN_LIST_FREE((*ev_sys)->free_evs, dpu_offload_event_t, item);
    DYN_LIST_FREE((*ev_sys)->free_pending_notifications, pending_notification_t, item);
    free(*ev_sys);
    *ev_sys = NULL;
}

dpu_offload_status_t event_get(dpu_offload_ev_sys_t *ev_sys, dpu_offload_event_t **ev)
{
    dpu_offload_event_t *_ev;
    DYN_LIST_GET(ev_sys->free_evs, dpu_offload_event_t, item, _ev);
    if (_ev != NULL)
    {
        ev_sys->num_used_evs++;
        _ev->ctx.complete = 0;
    }
    *ev = _ev;
    return DO_SUCCESS;
}

dpu_offload_status_t event_return(dpu_offload_ev_sys_t *ev_sys, dpu_offload_event_t **ev)
{
    if (!(*ev)->ctx.complete)
        return EVENT_INPROGRESS;

    DYN_LIST_RETURN(ev_sys->free_evs, (*ev), item);
    ev_sys->num_used_evs--;
    *ev = NULL; // so it cannot be used any longer
    return DO_SUCCESS;
}

/**
 * @brief event_completed is a helper function to check whether an event is completed
 * The function is aware of sub-events. If all the sub-events are completed and the
 * request of the event is NULL, the event is reported as completed.
 *
 * @param ev_sys Event system to use when events need to be returned
 * @param ev Event to check for completion.
 * @return true when the event is completed
 * @return false when the event is still in progress
 */
bool event_completed(dpu_offload_ev_sys_t *ev_sys, dpu_offload_event_t *ev)
{
    if (ev->ctx.complete)
        return true;

    // Update the list of sub-event by removing the completed ones
    if (ev->sub_events_initialized)
    {
        dpu_offload_event_t *subevt, *next;
        ucs_list_for_each_safe(subevt, next, &(ev->sub_events), item)
        {
            if (subevt->ctx.complete)
            {
                ucs_list_del(&(subevt->item));
                dpu_offload_status_t rc = event_return(ev_sys, &subevt);
                CHECK_ERR_RETURN((rc), DO_ERROR, "event_return() failed");
            }
        }
    }

    // If there is no more sub-event and the request of the event is complete, it is all done
    if (ev->sub_events_initialized && ucs_list_is_empty(&(ev->sub_events)) && ev->req == NULL)
    {
        ev->ctx.complete = true;
        return true;
    }

    return false;
}

/********************/
/* DEFAULT HANDLERS */
/********************/

static dpu_offload_status_t op_completion_cb(struct dpu_offload_ev_sys *ev_sys, execution_context_t *context, am_header_t *hdr, size_t hdr_size, void *data, size_t data_len)
{
    execution_context_t *econtext = (execution_context_t *)context;
    uint64_t *_data = (uint64_t *)data;

    // Find the operation and add it to the local list of active operations
    op_desc_t *cur_op, *next_op, *op = NULL;
    ucs_list_for_each_safe(cur_op, next_op, ACTIVE_OPS(econtext), item)
    {
        if (cur_op->id == _data[0])
        {
            op = cur_op;
            break;
        }
    }

    CHECK_ERR_RETURN((op == NULL), DO_ERROR, "unable to look up operation");

    op->completed = true;
    if (op->op_definition->op_complete != NULL)
        op->op_definition->op_complete();

    return DO_SUCCESS;
}

static dpu_offload_status_t op_start_cb(struct dpu_offload_ev_sys *ev_sys, execution_context_t *context, am_header_t *hdr, size_t hdr_size, void *data, size_t data_len)
{
    assert(context);
    assert(data);
    execution_context_t *econtext = (execution_context_t *)context;
    uint64_t *_data = (uint64_t *)data;
    uint64_t op_idx = _data[0];

    // Find the operation and add it to the local list of registered operations
    offload_op_t *op_cfg = NULL;
    int i;
    for (i = 0; i < econtext->engine->num_registered_ops; i++)
    {
        if (_data[0] == econtext->engine->registered_ops[i].alg_id)
        {
            op_cfg = &(econtext->engine->registered_ops[i]);
            break;
        }
    }

    CHECK_ERR_RETURN((op_cfg == NULL), DO_ERROR, "unable to find a matching registered function");

    // Instantiate the operation
    op_desc_t *op_desc;
    DYN_LIST_GET(econtext->engine->free_op_descs, op_desc_t, item, op_desc);
    CHECK_ERR_RETURN((op_desc == NULL), DO_ERROR, "unable to get a free operation descriptor");

    op_desc->id = _data[0];
    op_desc->completed = false;
    op_desc->op_definition = op_cfg;

    ucs_list_add_tail(ACTIVE_OPS(econtext), &(op_desc->item));

    // Call the init function of the operation
    op_cfg->op_init();

    return DO_SUCCESS;
}

static dpu_offload_status_t xgvmi_key_revoke_cb(struct dpu_offload_ev_sys *ev_sys, execution_context_t *context, am_header_t *hdr, size_t hdr_size, void *data, size_t data_len)
{
    // todo
    return DO_ERROR;
}

static int xgvmi_key_recv_cb(struct dpu_offload_ev_sys *ev_sys, execution_context_t *context, am_header_t *hdr, size_t hdr_size, void *data, size_t data_len)
{
    // todo
    return DO_ERROR;
}

/************************************/
/* Endpoint cache related functions */
/************************************/

extern dpu_offload_status_t send_group_cache(execution_context_t *econtext, ucp_ep_h dest, group_cache_t *gp_cache, dpu_offload_event_t *metaev);
extern int send_cache_entry_request(execution_context_t *econtext, ucp_ep_h ep, rank_info_t *requested_peer, dpu_offload_event_t **ev);

bool is_in_cache(cache_t *cache, int64_t gp_id, int64_t rank_id)
{
    dyn_array_t *gp_data, *gps_data = &(cache->data);
    DYN_ARRAY_GET_ELT(gps_data, gp_id, dyn_array_t, gp_data);
    if (gp_data == NULL)
    {
        return false;
    }
    else
    {
        if (gp_data->num_elts <= rank_id)
        {
            return false;
        }
        else
        {
            peer_cache_entry_t *entries;
            dyn_array_t *rank_array = (dyn_array_t *)gp_data->base;
            DYN_ARRAY_GET_ELT(rank_array, rank_id, peer_cache_entry_t, entries);
            if (entries[rank_id].set)
            {
                return false;
            }
        }
    }
    return true;
}

static int peer_cache_entries_request_recv_cb(struct dpu_offload_ev_sys *ev_sys, execution_context_t *econtext, am_header_t *hdr, size_t hdr_size, void *data, size_t data_len)
{
    assert(econtext);
    assert(data);
    rank_info_t *rank_info = (rank_info_t*)data;

    if (is_in_cache(&(econtext->engine->procs_cache), rank_info->group_id, rank_info->group_rank))
    {
        // We send the cache back to the sender
        dpu_offload_status_t rc;
        ucp_ep_h dest;
        group_cache_t *gp_caches;
        dpu_offload_event_t *send_cache_ev;
        event_get(econtext->event_channels, &send_cache_ev);
        assert(econtext->type == CONTEXT_SERVER);
        dest = econtext->server->connected_clients.clients[hdr->id].ep;
        gp_caches = (group_cache_t*) econtext->engine->procs_cache.data.base;
        rc = send_group_cache(econtext, dest, &gp_caches[rank_info->group_id], send_cache_ev);
        CHECK_ERR_RETURN((rc), DO_ERROR, "send_group_cache() failed");

        // Add the event to the list of pending events so it completed implicitely
        ucs_list_add_tail(&(econtext->ongoing_events), &(send_cache_ev->item));

        return DO_SUCCESS;
    }

    // If the entry is not in the cache we forward the request 
    // and also trigger the send of our cache for the target group
    if (econtext->engine->on_dpu)
    {
        size_t i;
        for (i = 0; i < econtext->engine->num_dpus; i++)
        {
            dpu_offload_status_t rc;
            dpu_offload_event_t *req_fwd_ev;
            remote_dpu_info_t **list_dpus = (remote_dpu_info_t **)econtext->engine->dpus.base;
            rc = send_cache_entry_request(econtext, list_dpus[i]->ep, rank_info, &req_fwd_ev);
            CHECK_ERR_RETURN((rc), DO_ERROR, "send_group_cache() failed");

            // Add the event to the list of pending events so it completed implicitely
            ucs_list_add_tail(&(econtext->ongoing_events), &(req_fwd_ev->item));

            return DO_SUCCESS;
        }
    }

    return DO_ERROR;
}

static int peer_cache_entries_recv_cb(struct dpu_offload_ev_sys *ev_sys, execution_context_t *econtext, am_header_t *hdr, size_t hdr_size, void *data, size_t data_len)
{
    assert(econtext);
    assert(data);

    offloading_engine_t *engine = (offloading_engine_t *)econtext->engine;
    peer_cache_entry_t *entries = (peer_cache_entry_t *)data;
    size_t cur_size = 0;
    size_t idx = 0;
    int64_t group_id = INVALID_GROUP;
    while (cur_size < data_len)
    {
        cache_t *cache = &(engine->procs_cache);
        // Make sure we know the group ID of what we receive otherwise we do not know what to do
        if (group_id == INVALID_GROUP)
            group_id = entries[idx].peer.proc_info.group_id;

        // Now that we know for sure we have the group ID, we can move the received data into the local cache
        int64_t group_rank = entries[idx].peer.proc_info.group_rank;
        DBG("Received a cache entry for rank:%ld, group:%ld (msg size=%ld)", group_rank, group_id, data_len);
        if (!is_in_cache(cache, group_id, group_rank))
        {
            SET_PEER_CACHE_ENTRY(cache, &(entries[idx]));
            group_cache_t *gp_caches = (group_cache_t *)cache->data.base;
        }

        cur_size += sizeof(peer_cache_entry_t);
        idx++;
    }
    DBG("Reception completed");

    return DO_SUCCESS;
}

/*************************************/
/* Registration of all the callbacks */
/*************************************/

dpu_offload_status_t register_default_notifications(dpu_offload_ev_sys_t *ev_sys)
{
    CHECK_ERR_RETURN((ev_sys == NULL), DO_ERROR, "Undefined event channels");

    int rc = event_channel_register(ev_sys, AM_OP_START_MSG_ID, op_start_cb);
    CHECK_ERR_RETURN(rc, DO_ERROR, "cannot register handler to start operations");

    rc = event_channel_register(ev_sys, AM_OP_COMPLETION_MSG_ID, op_completion_cb);
    CHECK_ERR_RETURN(rc, DO_ERROR, "cannot register handler for operation completion");

    rc = event_channel_register(ev_sys, AM_XGVMI_ADD_MSG_ID, xgvmi_key_recv_cb);
    CHECK_ERR_RETURN(rc, DO_ERROR, "cannot register handler for receiving XGVMI keys");

    rc = event_channel_register(ev_sys, AM_XGVMI_DEL_MSG_ID, xgvmi_key_revoke_cb);
    CHECK_ERR_RETURN(rc, DO_ERROR, "cannot register handler for revoke XGVMI keys");

    rc = event_channel_register(ev_sys, AM_PEER_CACHE_ENTRIES_MSG_ID, peer_cache_entries_recv_cb);
    CHECK_ERR_RETURN(rc, DO_ERROR, "cannot register handler for receiving peer cache entries");

    rc = event_channel_register(ev_sys, AM_PEER_CACHE_ENTRIES_REQUEST_MSG_ID, peer_cache_entries_request_recv_cb);
    CHECK_ERR_RETURN(rc, DO_ERROR, "cannot register handler for receiving peer cache requests");

    return DO_SUCCESS;
}