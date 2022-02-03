//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <ucp/api/ucp.h>
#include <inttypes.h>
#include <string.h>

#include "dpu_offload_types.h"
#include "dpu_offload_mem_mgt.h"
#include "dpu_offload_event_channels.h"

#define DEFAULT_NUM_EVTS (32)
#define DEFAULT_NUM_NOTIFICATION_CALLBACKS (8)

static ucs_status_t am_notification_msg_cb(void *arg, const void *header, size_t header_length,
                                           void *data, size_t length,
                                           const ucp_am_recv_param_t *param)
{
    fprintf(stderr, "Notification received, dispatching...\n");
    dpu_offload_daemon_t *d = (dpu_offload_daemon_t *)arg;
    if (header == NULL)
    {
        fprintf(stderr, "header is undefined\n");
        return UCS_ERR_NO_MESSAGE;
    }
    uint64_t *hdr = (uint64_t *)header;
    uint64_t idx = hdr[0];

    if (idx >= d->event_channels->num_notification_callbacks)
    {
        fprintf(stderr, "notification callback %" PRIu64 " is out of range\n", idx);
        return UCS_ERR_NO_MESSAGE;
    }
    notification_callback_entry_t *entry = &(d->event_channels->notification_callbacks[idx]);
    if (entry->set == false)
    {
        pending_notification_t *pending_notif;
        DYN_LIST_GET(d->event_channels->free_pending_notifications, pending_notification_t, item, pending_notif);
        if (pending_notif == NULL)
        {
            fprintf(stderr, "unable to get pending notification object\n");
            return UCS_ERR_NO_MESSAGE;
        }
        pending_notif->type = idx;
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
        ucs_list_add_tail(&(d->event_channels->pending_notifications), &(pending_notif->item));
    }
    notification_cb cb = entry->cb;
    if (cb == NULL)
    {
        fprintf(stderr, "Callback is undefined\n");
        return UCS_ERR_NO_MESSAGE;
    }
    cb(d, data);

    return UCS_OK;
}

int event_channels_init(dpu_offload_daemon_t *d)
{
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
    fprintf(stderr, "Initializing evt cbs...\n");
    uint64_t i;
    for (i = 0; i < num_notifications_cbs; i++)
    {
        notification_callback_entry_t *entry = &(new_ev_sys->notification_callbacks[i]);
        entry->set = false;
    }
    fprintf(stderr, "... done\n");

    ucp_am_handler_param_t ev_param;
    ev_param.field_mask = UCP_AM_HANDLER_PARAM_FIELD_ID |
                          UCP_AM_HANDLER_PARAM_FIELD_CB |
                          UCP_AM_HANDLER_PARAM_FIELD_ARG;
    ev_param.id = AM_EVENT_MSG_ID;
    ev_param.cb = am_notification_msg_cb;
    ev_param.arg = d;
    ucp_worker_h worker;
    fprintf(stderr, "Getting worker...\n");
    DAEMON_GET_WORKER(d, worker);
    assert(worker);
    fprintf(stderr, "Registering AM callback for notifications\n");
    ucs_status_t status = ucp_worker_set_am_recv_handler(worker, &ev_param);
    if (status != UCS_OK)
    {
        return -1;
    }

    d->event_channels = new_ev_sys;
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

    /* check for any pending notification that would match */
    pending_notification_t *pending_notif, *next_pending_notif;
    ucs_list_for_each_safe(pending_notif, next_pending_notif, (&(ev_sys->pending_notifications)), item)
    {
        if (pending_notif->type == type)
        {
            ucs_list_del(&(pending_notif->item));
            cb(pending_notif->arg, pending_notif->data);
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

int event_channel_emit(dpu_offload_event_t *ev, uint64_t type, ucp_ep_h dest_ep, void *ctx, void *payload, size_t payload_size)
{
    ucp_request_param_t params;
    ev->ctx.complete = 0;
    ev->ctx.hdr = type;
    params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                          UCP_OP_ATTR_FIELD_DATATYPE |
                          UCP_OP_ATTR_FIELD_USER_DATA;
    params.datatype = ucp_dt_make_contig(1);
    params.user_data = &ctx;
    params.cb.send = (ucp_send_nbx_callback_t)notification_emit_cb;
    ev->req = ucp_am_send_nbx(dest_ep, AM_EVENT_MSG_ID, &(ev->ctx.hdr), sizeof(uint64_t), payload, payload_size, &params);
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