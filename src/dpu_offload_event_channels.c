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
#define DEFAULT_NUM_NOTIFICATION_CALLBACKS (8)

static ucs_status_t am_notification_msg_cb(void *arg, const void *header, size_t header_length,
                                           void *data, size_t length,
                                           const ucp_am_recv_param_t *param)
{
    assert(header_length == sizeof(am_header_t));
    fprintf(stderr, "Notification received, dispatching...\n");
    execution_context_t *econtext = (execution_context_t *)arg;
    CHECK_ERR_RETURN((header == NULL), UCS_ERR_NO_MESSAGE, "header is undefined");

    am_header_t *hdr = (am_header_t *)header;
    CHECK_ERR_RETURN((hdr == NULL), UCS_ERR_NO_MESSAGE, "header is NULL");
    CHECK_ERR_RETURN((hdr->type >= econtext->event_channels->num_notification_callbacks), UCS_ERR_NO_MESSAGE, "notification callback %" PRIu64 " is out of range", hdr->type);

    notification_callback_entry_t *entry = &(econtext->event_channels->notification_callbacks[hdr->type]);
    if (entry->set == false)
    {
        pending_notification_t *pending_notif;
        fprintf(stderr, "callback not available for %" PRIu64 "\n", hdr->type);
        DYN_LIST_GET(econtext->event_channels->free_pending_notifications, pending_notification_t, item, pending_notif);
        CHECK_ERR_RETURN((pending_notif == NULL), UCS_ERR_NO_MESSAGE, "unable to get pending notification object");

        pending_notif->type = hdr->type;
        pending_notif->client_id = hdr->id;
        pending_notif->data_size = length;
        pending_notif->header_size = header_length;
        pending_notif->arg = arg;
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
            memcpy(pending_notif->header, header, pending_notif->header_size);
        }
        else
        {
            pending_notif->header = NULL;
        }
        ucs_list_add_tail(&(econtext->event_channels->pending_notifications), &(pending_notif->item));
        return UCS_OK;
    }

    notification_cb cb = entry->cb;
    CHECK_ERR_RETURN((cb == NULL), UCS_ERR_NO_MESSAGE, "Callback is undefined");
    struct dpu_offload_ev_sys *ev_sys = EV_SYS(econtext);
    cb(ev_sys, econtext, hdr, header_length, data, length);
    return UCS_OK;
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
    size_t num_notifications_cbs = DEFAULT_NUM_NOTIFICATION_CALLBACKS;
    econtext->event_channels->notification_callbacks = malloc(num_notifications_cbs * sizeof(notification_callback_entry_t));
    CHECK_ERR_RETURN((econtext->event_channels->notification_callbacks == NULL), DO_ERROR, "Resource allocation failed");

    uint64_t i;
    for (i = 0; i < num_notifications_cbs; i++)
    {
        notification_callback_entry_t *entry = &(econtext->event_channels->notification_callbacks[i]);
        entry->set = false;
    }

    // Register the UCX AM handler used for all notifications
    ucp_am_handler_param_t ev_param;
    ev_param.field_mask = UCP_AM_HANDLER_PARAM_FIELD_ID |
                          UCP_AM_HANDLER_PARAM_FIELD_CB |
                          UCP_AM_HANDLER_PARAM_FIELD_ARG;
    ev_param.id = AM_EVENT_MSG_ID;
    ev_param.cb = am_notification_msg_cb;
    ev_param.arg = econtext;
    DBG("Registering AM callback for notifications (econtext=%p, worker=%p, ev_param=%p)", econtext, GET_WORKER(econtext), &ev_param);
    ucs_status_t status = ucp_worker_set_am_recv_handler(GET_WORKER(econtext), &ev_param);
    CHECK_ERR_RETURN((status != UCS_OK), DO_ERROR, "unable to set AM recv handler");
    return DO_SUCCESS;
}

dpu_offload_status_t event_channel_register(dpu_offload_ev_sys_t *ev_sys, uint64_t type, notification_cb cb)
{
    CHECK_ERR_RETURN((ev_sys == NULL), DO_ERROR, "undefined event system");
    CHECK_ERR_RETURN((ev_sys->num_notification_callbacks <= type), DO_ERROR, "type %" PRIu64 " is out of range", type);

    notification_callback_entry_t *entry = &(ev_sys->notification_callbacks[type]);
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
            cb((struct dpu_offload_ev_sys *)ev_sys, pending_notif->arg, pending_notif->header, pending_notif->header_size, pending_notif->data, pending_notif->data_size);
        }
    }

    return DO_SUCCESS;
}

dpu_offload_status_t event_channel_deregister(dpu_offload_ev_sys_t *ev_sys, uint64_t type)
{
    CHECK_ERR_RETURN((ev_sys == NULL), DO_ERROR, "undefined event system");
    CHECK_ERR_RETURN((ev_sys->num_notification_callbacks <= type), DO_ERROR, "type %" PRIu64 " is out of range", type);

    notification_callback_entry_t *entry = &(ev_sys->notification_callbacks[type]);
    CHECK_ERR_RETURN((entry->set == false), DO_ERROR, "type %" PRIu64 " is not registered", type);

    entry->set = false;
    return DO_SUCCESS;
}

static void notification_emit_cb(void *user_data, const char *type_str)
{
    am_req_t *ctx;
    if (user_data == NULL)
    {
        ERR_MSG("user_data passed to %s mustn't be NULL", type_str);
        return;
    }
    ctx = (am_req_t *)user_data;
    ctx->complete = 1;
}

dpu_offload_status_t event_channel_emit(dpu_offload_event_t *ev, uint64_t client_id, uint64_t type, ucp_ep_h dest_ep, void *ctx, void *payload, size_t payload_size)
{
    ucp_request_param_t params;
    DBG("Sending notification of type %" PRIu64 " (client_id=%" PRIu64 ")", type, client_id);
    ev->ctx.complete = 0;
    ev->ctx.hdr.type = type;
    ev->ctx.hdr.id = client_id;
    params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                          UCP_OP_ATTR_FIELD_DATATYPE |
                          UCP_OP_ATTR_FIELD_USER_DATA;
    params.datatype = ucp_dt_make_contig(1);
    params.user_data = &ctx;
    params.cb.send = (ucp_send_nbx_callback_t)notification_emit_cb;
    ev->req = ucp_am_send_nbx(dest_ep, AM_EVENT_MSG_ID, &(ev->ctx.hdr), sizeof(am_header_t), payload, payload_size, &params);
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

/********************/
/* DEFAULT HANDLERS */
/********************/

static dpu_offload_status_t op_completion_cb(struct dpu_offload_ev_sys *ev_sys, void *context, am_header_t *hdr, size_t hdr_size, void *data, size_t data_len)
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

static dpu_offload_status_t op_start_cb(struct dpu_offload_ev_sys *ev_sys, void *context, am_header_t *hdr, size_t hdr_size, void *data, size_t data_len)
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

static dpu_offload_status_t xgvmi_key_revoke_cb(struct dpu_offload_ev_sys *ev_sys, void *context, am_header_t *hdr, size_t hdr_size, void *data, size_t data_len)
{
    // todo
    return DO_ERROR;
}

static int xgvmi_key_recv_cb(struct dpu_offload_ev_sys *ev_sys, void *context, am_header_t *hdr, size_t hdr_size, void *data, size_t data_len)
{
    // todo
    return DO_ERROR;
}

static inline bool is_in_cache(cache_t *cache, int64_t gp_id, int64_t rank_id)
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
            if (entries[rank_id].peer.proc_info.group_id == INVALID_GROUP ||
                entries[rank_id].peer.proc_info.group_rank == INVALID_RANK)
            {
                return false;
            }
        }
    }
    return true;
}

static int peer_cache_entries_recv_cb(struct dpu_offload_ev_sys *ev_sys, void *context, am_header_t *hdr, size_t hdr_size, void *data, size_t data_len)
{
    assert(context);
    assert(data);

    execution_context_t *econtext = (execution_context_t *)context;
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
        bool in_cache;
        int64_t group_rank = entries[idx].peer.proc_info.group_rank;
        if (!is_in_cache(cache, group_id, group_rank))
        {
            // SET_PEER_CACHE_ENTRY(cache, entries[idx]);
        }

        cur_size += sizeof(peer_cache_entry_t);
        idx++;
    }

    return DO_SUCCESS;
}

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

    return DO_ERROR;
}