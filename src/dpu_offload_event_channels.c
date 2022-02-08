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

#define DEFAULT_NUM_EVTS (32)
#define DEFAULT_NUM_NOTIFICATION_CALLBACKS (8)

static ucs_status_t am_notification_msg_cb(void *arg, const void *header, size_t header_length,
                                           void *data, size_t length,
                                           const ucp_am_recv_param_t *param)
{
    assert(header_length == sizeof(am_header_t));
    fprintf(stderr, "Notification received, dispatching...\n");
    execution_context_t *econtext = (execution_context_t *)arg;
    if (header == NULL)
    {
        fprintf(stderr, "header is undefined\n");
        return UCS_ERR_NO_MESSAGE;
    }
    am_header_t *hdr = (am_header_t *)header;
    assert(hdr);
    if (hdr->type >= econtext->event_channels->num_notification_callbacks)
    {
        fprintf(stderr, "notification callback %" PRIu64 " is out of range\n", hdr->type);
        return UCS_ERR_NO_MESSAGE;
    }
    notification_callback_entry_t *entry = &(econtext->event_channels->notification_callbacks[hdr->type]);
    if (entry->set == false)
    {
        pending_notification_t *pending_notif;
        fprintf(stderr, "callback not available for %" PRIu64 "\n", hdr->type);
        DYN_LIST_GET(econtext->event_channels->free_pending_notifications, pending_notification_t, item, pending_notif);
        if (pending_notif == NULL)
        {
            fprintf(stderr, "unable to get pending notification object\n");
            return UCS_ERR_NO_MESSAGE;
        }
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
    if (cb == NULL)
    {
        fprintf(stderr, "Callback is undefined\n");
        return UCS_ERR_NO_MESSAGE;
    }
    struct dpu_offload_ev_sys *ev_sys = EV_SYS(econtext);
    cb(ev_sys, econtext, data);
    return UCS_OK;
}

int event_channels_init(dpu_offload_ev_sys_t **e, execution_context_t *econtext)
{
    if (econtext == NULL)
    {
        fprintf(stderr, "Undefined execution context\n");
        return -1;
    }

    dpu_offload_ev_sys_t *new_ev_sys = malloc(sizeof(dpu_offload_ev_sys_t));
    if (new_ev_sys == NULL)
    {
        fprintf(stderr, "Resource allocation failed\n");
        return -1;
    }
    size_t num_evts = DEFAULT_NUM_EVTS;
    size_t num_free_pending_notifications = DEFAULT_NUM_NOTIFICATION_CALLBACKS;
    DYN_LIST_ALLOC(new_ev_sys->free_evs, num_evts, dpu_offload_event_t, item);
    DYN_LIST_ALLOC(new_ev_sys->free_pending_notifications, num_free_pending_notifications, pending_notification_t, item);
    ucs_list_head_init(&(new_ev_sys->pending_notifications));
    new_ev_sys->num_used_evs = 0;
    size_t num_notifications_cbs = DEFAULT_NUM_NOTIFICATION_CALLBACKS;
    new_ev_sys->notification_callbacks = malloc(num_notifications_cbs * sizeof(notification_callback_entry_t));
    if (new_ev_sys->notification_callbacks == NULL)
    {
        fprintf(stderr, "Resource allocation failed\n");
        return -1;
    }
    uint64_t i;
    for (i = 0; i < num_notifications_cbs; i++)
    {
        notification_callback_entry_t *entry = &(new_ev_sys->notification_callbacks[i]);
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
    fprintf(stderr, "Registering AM callback for notifications\n");
    ucs_status_t status = ucp_worker_set_am_recv_handler(GET_WORKER(econtext), &ev_param);
    if (status != UCS_OK)
    {
        return -1;
    }

    *e = new_ev_sys;
    return 0;
}

int event_channel_register(dpu_offload_ev_sys_t *ev_sys, uint64_t type, notification_cb cb)
{
    if (ev_sys == NULL)
    {
        fprintf(stderr, "undefined event system\n");
        return -1;
    }

    if (ev_sys->num_notification_callbacks <= type)
    {
        fprintf(stderr, "type %" PRIu64 " is out of range\n", type);
        return -1;
    }

    notification_callback_entry_t *entry = &(ev_sys->notification_callbacks[type]);
    if (entry->set == true)
    {
        fprintf(stderr, "type %" PRIu64 " is already set\n", type);
        return -1;
    }

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
            cb((struct dpu_offload_ev_sys *)ev_sys, pending_notif->arg, pending_notif->data);
        }
    }

    return 0;
}

int event_channel_deregister(dpu_offload_ev_sys_t *ev_sys, uint64_t type)
{
    if (ev_sys == NULL)
    {
        fprintf(stderr, "undefined event system\n");
        return -1;
    }

    if (ev_sys->num_notification_callbacks <= type)
    {
        fprintf(stderr, "type %" PRIu64 " is out of range\n", type);
        return -1;
    }

    notification_callback_entry_t *entry = &(ev_sys->notification_callbacks[type]);
    if (entry->set == false)
    {
        fprintf(stderr, "type %" PRIu64 " is not registered\n", type);
        return -1;
    }

    entry->set = false;

    return 0;
}

static void notification_emit_cb(void *user_data, const char *type_str)
{
    am_req_t *ctx;
    if (user_data == NULL)
    {
        fprintf(stderr, "user_data passed to %s mustn't be NULL\n", type_str);
        return;
    }
    ctx = (am_req_t *)user_data;
    ctx->complete = 1;
}

int event_channel_emit(dpu_offload_event_t *ev, uint64_t client_id, uint64_t type, ucp_ep_h dest_ep, void *ctx, void *payload, size_t payload_size)
{
    ucp_request_param_t params;
    fprintf(stderr, "Sending notification of type %" PRIu64 " (client_id=%" PRIu64 ")\n", type, client_id);
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

int event_channels_fini(dpu_offload_ev_sys_t **ev_sys)
{
    if ((*ev_sys)->num_used_evs > 0)
    {
        fprintf(stderr, "[WARN] %ld events objects have not been returned\n", (*ev_sys)->num_used_evs);
    }

    DYN_LIST_FREE((*ev_sys)->free_evs, dpu_offload_event_t, item);
    DYN_LIST_FREE((*ev_sys)->free_pending_notifications, pending_notification_t, item);
    free(*ev_sys);
    *ev_sys = NULL;
}

int event_get(dpu_offload_ev_sys_t *ev_sys, dpu_offload_event_t **ev)
{
    dpu_offload_event_t *_ev;
    DYN_LIST_GET(ev_sys->free_evs, dpu_offload_event_t, item, _ev);
    if (_ev != NULL)
        ev_sys->num_used_evs++;
    *ev = _ev;
    return 0;
}

int event_return(dpu_offload_ev_sys_t *ev_sys, dpu_offload_event_t **ev)
{
    if (!(*ev)->ctx.complete)
        return EVENT_INPROGRESS;

    DYN_LIST_RETURN(ev_sys->free_evs, (*ev), item);
    ev_sys->num_used_evs--;
    *ev = NULL; // so it cannot be used any longer
    return 0;
}

static int op_completion_cb(struct dpu_offload_ev_sys *ev_sys, void *context, void *data)
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

    if (op == NULL)
    {
        fprintf(stderr, "%s(): unable to look up operation\n", __func__);
        return -1;
    }

    op->completed = true;
    if (op->op_definition->op_complete != NULL)
        op->op_definition->op_complete();

    return 0;
}

static int op_start_cb(struct dpu_offload_ev_sys *ev_sys, void *context, void *data)
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

    if (op_cfg == NULL)
    {
        fprintf(stderr, "%s(): unable to find a matching registered function\n", __func__);
        return -1;
    }

    // Instantiate the operation
    op_desc_t *op_desc;
    DYN_LIST_GET(econtext->engine->free_op_descs, op_desc_t, item, op_desc);
    if (op_desc == NULL)
    {
        fprintf(stderr, "%s(): unable to get a free operation descriptor\n", __func__);
        return -1;
    }

    op_desc->id = _data[0];
    op_desc->completed = false;
    op_desc->op_definition = op_cfg;

    ucs_list_add_tail(ACTIVE_OPS(econtext), &(op_desc->item));

    // Call the init function of the operation
    op_cfg->op_init();

    return 0;
}

int register_default_notifications(dpu_offload_ev_sys_t *ev_sys)
{
    int rc = event_channel_register(ev_sys, AM_OP_START_MSG_ID, op_start_cb);
    if (rc)
    {
        fprintf(stderr, "event_channel_register() failed, cannot register handler to start operations\n");
        return -1;
    }

    rc = event_channel_register(ev_sys, AM_OP_COMPLETION_MSG_ID, op_completion_cb);
    if (rc)
    {
        fprintf(stderr, "event_channel_register() failed, cannot register handler for operation completion\n");
        return -1;
    }
    return 0;
}