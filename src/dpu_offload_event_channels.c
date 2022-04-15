#define _POSIX_C_SOURCE 200809L

//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <inttypes.h>
#include <string.h>
#include <errno.h>

#include <ucp/api/ucp.h>

#include "dpu_offload_types.h"
#include "dpu_offload_mem_mgt.h"
#include "dpu_offload_event_channels.h"
#include "dpu_offload_debug.h"
#include "dpu_offload_comms.h"

#define DEFAULT_NUM_EVTS (32)
#define DEFAULT_NUM_NOTIFICATION_CALLBACKS (5000)
#define DEFAULT_MAX_PENDING_EMITS (10)

#if USE_AM_IMPLEM
/**
 * @brief Note that the function assumes the execution context is not locked before it is invoked.
 * 
 * @param request 
 * @param status 
 * @param length 
 * @param user_data 
 */
static void am_rdv_recv_cb(void *request, ucs_status_t status, size_t length, void *user_data)
{
    pending_am_rdv_recv_t *recv_info = (pending_am_rdv_recv_t *)user_data;
    int rc = handle_notif_msg(recv_info->econtext, recv_info->hdr, recv_info->hdr_len, recv_info->user_data, recv_info->payload_size);
    ucp_request_free(request);
    ECONTEXT_LOCK(recv_info->econtext);
    ucs_list_del(&(recv_info->item));
    DYN_LIST_RETURN(recv_info->econtext->free_pending_rdv_recv, recv_info, item);
    ECONTEXT_UNLOCK(recv_info->econtext);
}

static ucs_status_t am_notification_recv_rdv_msg(execution_context_t *econtext, am_header_t *hdr, size_t hdr_len, size_t payload_size, void *desc)
{
    ucp_request_param_t am_rndv_recv_request_params;
    pending_am_rdv_recv_t *pending_recv;
    ECONTEXT_LOCK(econtext);
    DYN_LIST_GET(econtext->free_pending_rdv_recv, pending_am_rdv_recv_t, item, pending_recv);
    ECONTEXT_UNLOCK(econtext);
    RESET_PENDING_RDV_RECV(pending_recv);
    assert(pending_recv);
    DBG("RDV message to be received for type %ld", hdr->type);
    // Make sure we have space for the payload, note that we do not know the data size to
    // be received in advance but we do our best to avoid mallocs
    if (pending_recv->buff_size == 0)
    {
        pending_recv->user_data = MALLOC(payload_size);
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
                                               UCP_OP_ATTR_FIELD_MEMORY_TYPE |
                                               UCP_OP_ATTR_FLAG_NO_IMM_CMPL;
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
        /* request not yet completed */
        DBG("ucp_am_recv_data_nbx() is INPROGRESS");
        pending_recv->req = status;
    }
    else
    {
        DBG("ucp_am_recv_data_nbx() completed right away");
        assert(NULL == status);
        pending_recv->req = NULL;
        handle_notif_msg(econtext, hdr, hdr_len, pending_recv->user_data, payload_size);
        ucp_request_free(am_rndv_recv_request_params.request);
    }
    return UCS_OK;
}

static ucs_status_t am_notification_msg_cb(void *arg, const void *header, size_t header_length,
                                           void *data, size_t length,
                                           const ucp_am_recv_param_t *param)
{
    assert(header != NULL);
    assert(header_length == sizeof(am_header_t));
    execution_context_t *econtext = (execution_context_t *)arg;
    am_header_t *hdr = (am_header_t *)header;
    assert(hdr != NULL);
    assert(hdr->type < econtext->event_channels->notification_callbacks.num_elts);

    if (param->recv_attr & UCP_AM_RECV_ATTR_FLAG_RNDV)
    {
        // RDV message
        am_notification_recv_rdv_msg(econtext, hdr, header_length, length, data);
        return UCS_INPROGRESS;
    }

    DBG("Notification of type %" PRIu64 " received via eager message, dispatching...", hdr->type);
    return handle_notif_msg(econtext, hdr, header_length, data, length);
}
#endif // USE_AM_IMPLEM

void init_event(void *ev_ptr)
{
    assert(ev_ptr);
    dpu_offload_event_t *ev = (dpu_offload_event_t *)ev_ptr;
    RESET_EVENT(ev);
    ev->sub_events_initialized = false;
}

dpu_offload_status_t ev_channels_init(dpu_offload_ev_sys_t **ev_channels)
{
    dpu_offload_ev_sys_t *event_channels = MALLOC(sizeof(dpu_offload_ev_sys_t));
    CHECK_ERR_RETURN((event_channels == NULL), DO_ERROR, "Resource allocation failed");
    size_t num_evts = DEFAULT_NUM_EVTS;
    size_t num_free_pending_notifications = DEFAULT_NUM_NOTIFICATION_CALLBACKS;
    DYN_LIST_ALLOC_WITH_INIT_CALLBACK(event_channels->free_evs, num_evts, dpu_offload_event_t, item, init_event);
    assert(event_channels->free_evs);
    DYN_LIST_ALLOC(event_channels->free_pending_notifications, num_free_pending_notifications, pending_notification_t, item);
    assert(event_channels->free_pending_notifications);
    ucs_list_head_init(&(event_channels->pending_notifications));
    ucs_list_head_init(&(event_channels->pending_emits));
    event_channels->max_pending_emits = DEFAULT_MAX_PENDING_EMITS;
    event_channels->num_used_evs = 0;
    event_channels->num_pending_sends = 0;
    DYN_ARRAY_ALLOC(&(event_channels->notification_callbacks), DEFAULT_NUM_NOTIFICATION_CALLBACKS, notification_callback_entry_t);
    CHECK_ERR_RETURN((event_channels->notification_callbacks.base == NULL), DO_ERROR, "Resource allocation failed");
    int ret = pthread_mutex_init(&(event_channels->mutex), NULL);
    CHECK_ERR_RETURN((ret), DO_ERROR, "pthread_mutex_init() failed: %s", strerror(errno));
#if !USE_AM_IMPLEM
    event_channels->notif_recv.initialized = false;
#endif
    *ev_channels = event_channels;
    return DO_SUCCESS;
}

dpu_offload_status_t event_channels_init(execution_context_t *econtext)
{
    CHECK_ERR_RETURN((econtext == NULL), DO_ERROR, "Undefined execution context");
    CHECK_ERR_RETURN((GET_WORKER(econtext) == NULL), DO_ERROR, "Undefined worker");

    dpu_offload_status_t rc = ev_channels_init(&(econtext->event_channels));
    CHECK_ERR_RETURN((rc), DO_ERROR, "ev_channels_init() failed");

#if USE_AM_IMPLEM
    // Register the UCX AM handler
    ucp_am_handler_param_t am_param;
    am_param.field_mask = UCP_AM_HANDLER_PARAM_FIELD_ID |
                          UCP_AM_HANDLER_PARAM_FIELD_CB |
                          UCP_AM_HANDLER_PARAM_FIELD_ARG |
                          UCP_AM_FLAG_WHOLE_MSG;
    am_param.id = AM_EVENT_MSG_ID;
    // For all exchange, we receive the header first and from there post a receive for the either eager or RDV message.
    am_param.cb = am_notification_msg_cb;
    am_param.arg = econtext;
    DBG("Registering AM eager callback for notifications (type=%d, econtext=%p, worker=%p, ev_param=%p)",
        AM_EVENT_MSG_ID,
        econtext,
        GET_WORKER(econtext),
        &am_param);
    ucs_status_t status = ucp_worker_set_am_recv_handler(GET_WORKER(econtext),
                                                         &am_param);
    CHECK_ERR_RETURN((status != UCS_OK), DO_ERROR, "unable to set AM eager recv handler");
#else
    // Create a separate thread to receive notifications
    /*
    pthread_t event_tid;
    int ret = pthread_create(&event_tid, NULL, &recv_event_thread, econtext);
    CHECK_ERR_RETURN((ret), DO_ERROR, "unable to start connection thread");
    */
#endif

    return DO_SUCCESS;
}

/**
 * @brief Note: the function assumes the event system is locked before it is invoked
 *
 * @param ev_sys
 * @param type
 * @param cb
 * @return dpu_offload_status_t
 */
dpu_offload_status_t event_channel_register(dpu_offload_ev_sys_t *ev_sys, uint64_t type, notification_cb cb)
{
    CHECK_ERR_RETURN((cb == NULL), DO_ERROR, "Undefined callback");
    CHECK_ERR_RETURN((ev_sys == NULL), DO_ERROR, "undefined event system");
    notification_callback_entry_t *entry = DYN_ARRAY_GET_ELT(&(ev_sys->notification_callbacks), type, notification_callback_entry_t);
    CHECK_ERR_RETURN((entry == NULL), DO_ERROR, "unable to get callback %ld", type);
    CHECK_ERR_RETURN((entry->set == true), DO_ERROR, "type %" PRIu64 " is already set", type);

    entry->cb = cb;
    entry->set = true;
    entry->ev_sys = (struct dpu_offload_ev_sys *)ev_sys;
    DBG("Callback for notification of type %"PRIu64" is now registered on event system %p", type, ev_sys);

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

dpu_offload_status_t engine_register_default_notification_handler(offloading_engine_t *engine, uint64_t type, notification_cb cb)
{
    CHECK_ERR_RETURN((cb == NULL), DO_ERROR, "Undefined callback");
    CHECK_ERR_RETURN((engine == NULL), DO_ERROR, "Undefine engine");

    notification_callback_entry_t *list_callbacks = (notification_callback_entry_t *)engine->default_notifications->notification_callbacks.base;
    ENGINE_LOCK(engine);
    notification_callback_entry_t *entry = DYN_ARRAY_GET_ELT(&(engine->default_notifications->notification_callbacks), type, notification_callback_entry_t);
    ENGINE_UNLOCK(engine);
    CHECK_ERR_RETURN((entry == NULL), DO_ERROR, "unable to get callback %ld", type);
    CHECK_ERR_RETURN((entry->set == true), DO_ERROR, "type %" PRIu64 " is already set", type);

    entry->cb = cb;
    entry->set = true;
    entry->ev_sys = (struct dpu_offload_ev_sys *)engine->default_notifications;
    engine->num_default_notifications++;
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

static size_t num_completed_emit = 0;
static void notification_emit_cb(void *user_data, const char *type_str)
{
    dpu_offload_event_t *ev;
    am_req_t *ctx;
    num_completed_emit++;
    DBG("New completion, %ld emit have now completed", num_completed_emit);
    if (user_data == NULL)
    {
        ERR_MSG("user_data passed to %s mustn't be NULL", type_str);
        return;
    }
    ev = (dpu_offload_event_t *)user_data;
    if (ev == NULL)
        return;
    DBG("ev=%p ctx=%p id=%" PRIu64, ev, &(ev->ctx), ev->ctx.hdr.id);
    ev->ctx.complete = 1;
    assert(ev->event_system);
    execution_context_t *econtext = ev->event_system->econtext;
    DBG("Associated econtext: %p", econtext);
    assert(econtext);
    DBG("evt %p now completed, %ld events left on the ongoing list", ev, ucs_list_length(&(econtext->ongoing_events)));
}

static void notif_hdr_send_cb(void *request, ucs_status_t status, void *user_data)
{
    am_req_t *ctx; // todo: rename am_req_t to notif_req_t
    if (user_data == NULL)
    {
        ERR_MSG("user data is undefined, unable to send event header");
        return;
    }
    ctx = (am_req_t *)user_data;
    ctx->complete = 1;
}

uint64_t num_ev_sent = 0;

#if USE_AM_IMPLEM
int am_send_event_msg(dpu_offload_event_t **event)
{
    ucp_request_param_t params;
    params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                          UCP_OP_ATTR_FIELD_DATATYPE |
                          UCP_OP_ATTR_FIELD_USER_DATA;
    params.datatype = ucp_dt_make_contig(1);
    params.user_data = *event;
    params.cb.send = (ucp_send_nbx_callback_t)notification_emit_cb;
    (*event)->req = ucp_am_send_nbx((*event)->dest_ep,
                                    AM_EVENT_MSG_ID,
                                    &((*event)->ctx.hdr),
                                    sizeof(am_header_t),
                                    (*event)->payload,
                                    (*event)->payload_size,
                                    &params);
    DBG("Event %p %ld successfully emitted", (*event), num_ev_sent);
    num_ev_sent++;
    if ((*event)->req == NULL)
    {
        // Immediate completion, the callback is *not* invoked
        DBG("ucp_am_send_nbx() completed right away");
        dpu_offload_status_t rc = event_return(event);
        CHECK_ERR_RETURN((rc), DO_ERROR, "event_return() failed");
        return EVENT_DONE;
    }

    if (UCS_PTR_IS_ERR((*event)->req))
        return UCS_PTR_STATUS((*event)->req);

    DBG("ucp_am_send_nbx() did not completed right away");
    SYS_EVENT_LOCK((*event)->event_system);
    (*event)->event_system->num_pending_sends++;
    SYS_EVENT_UNLOCK((*event)->event_system);
    (*event)->was_pending = true;

    return EVENT_INPROGRESS;
}
#else
// FIXME: do not block here, blocking only for initial debugging
int tag_send_event_msg(dpu_offload_event_t **event)
{
    struct ucx_context *addr_request = NULL;
    ucp_request_param_t send_param;
    send_param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                              UCP_OP_ATTR_FIELD_DATATYPE |
                              UCP_OP_ATTR_FIELD_USER_DATA;
    send_param.cb.send = notif_hdr_send_cb;
    send_param.datatype = ucp_dt_make_contig(1);
    send_param.user_data = (void *)(*event);

    assert((*event)->event_system);
    assert((*event)->event_system->econtext);
    execution_context_t *econtext = (*event)->event_system->econtext;
    uint64_t myid = ECONTEXT_ID(econtext);

    /* 1. Send the hdr */
    ucp_tag_t hdr_ucp_tag = MAKE_SEND_TAG(AM_EVENT_MSG_HDR_ID, myid, 0, (*event)->scope_id, 0);
    struct ucx_context *hdr_request = NULL;
    assert((*event)->dest_ep);
    (*event)->hdr_request = ucp_tag_send_nbx((*event)->dest_ep, &((*event)->ctx.hdr), sizeof(am_header_t), hdr_ucp_tag, &send_param);
    if (UCS_PTR_IS_ERR((*event)->hdr_request))
    {
        ucs_status_t send_status = UCS_PTR_STATUS((*event)->hdr_request);
        ERR_MSG("ucp_tag_send_nbx() failed: %s", ucs_status_string(send_status));
        return send_status;
    }
    DBG("event %p send posted (hdr) - scope_id: %d, id: %"PRIu64, (*event), (*event)->scope_id, myid);

    /* 2. Send the payload */
    if ((*event)->ctx.hdr.payload_size > 0)
    {
        ucp_tag_t payload_ucp_tag = MAKE_SEND_TAG(AM_EVENT_MSG_ID, myid, 0, (*event)->scope_id, 0);
        struct ucx_context *payload_request = NULL;
        (*event)->payload_request = ucp_tag_send_nbx((*event)->dest_ep, (*event)->payload, (*event)->ctx.hdr.payload_size, payload_ucp_tag, &send_param);
        if (UCS_PTR_IS_ERR((*event)->payload_request))
        {
            ucs_status_t send_status = UCS_PTR_STATUS((*event)->payload_request);
            ERR_MSG("ucp_tag_send_nbx() failed: %s", ucs_status_string(send_status));
            return send_status;
        }
        DBG("event %p send posted (payload) - scope_id: %d", (*event), (*event)->scope_id);
    }

    if ((*event)->hdr_request == NULL && (*event)->payload_request == NULL)
    {
        DBG("event %p send immediately completed", (*event));
        (*event)->ctx.complete = 1;
        (*event)->was_pending = false;
        dpu_offload_status_t rc = event_return(event);
        CHECK_ERR_RETURN((rc), DO_ERROR, "event_return() failed");
        return EVENT_DONE;
    }
    return EVENT_INPROGRESS;
}
#endif // USE_AM_IMPLEM

int event_channel_emit_with_payload(dpu_offload_event_t **event, uint64_t my_id, uint64_t type, ucp_ep_h dest_ep, void *ctx, void *payload, size_t payload_size)
{
    assert(event);
    assert(dest_ep);
    assert((*event));
    DBG("Sending notification of type %" PRIu64 " (my_id=%" PRIu64 ")", type, my_id);
    (*event)->ctx.complete = 0;
    (*event)->ctx.hdr.type = type;
    (*event)->ctx.hdr.id = my_id;
    (*event)->ctx.hdr.payload_size = payload_size;
    (*event)->user_context = ctx;
    (*event)->payload = payload;
    (*event)->dest_ep = dest_ep;

    if ((*event)->event_system->num_pending_sends >= (*event)->event_system->max_pending_emits)
    {
        DBG("Queuing event for later emission");
        assert((*event)->manage_payload_buf == false);
        SYS_EVENT_LOCK((*event)->event_system);
        ucs_list_add_tail(&((*event)->event_system->pending_emits), &((*event)->item));
        SYS_EVENT_UNLOCK((*event)->event_system);
        return EVENT_INPROGRESS;
    }

#if USE_AM_IMPLEM
    return am_send_event_msg(event);
#else
    return tag_send_event_msg(event);
#endif // USE_AM_IMPLEM
}

int event_channel_emit(dpu_offload_event_t **event, uint64_t my_id, uint64_t type, ucp_ep_h dest_ep, void *ctx)
{
    dpu_offload_event_t *ev = *event;
    DBG("Sending notification of type %" PRIu64 " (client_id=%" PRIu64 ")", type, my_id);
    ev->ctx.complete = 0;
    ev->ctx.hdr.type = type;
    ev->ctx.hdr.id = my_id;
    ev->user_context = ctx;
    ev->dest_ep = dest_ep;

    SYS_EVENT_LOCK(ev->event_system);
    if (ev->event_system->num_pending_sends >= ev->event_system->max_pending_emits)
    {
        DBG("Queuing event for later emission");
        assert(ev->manage_payload_buf == false);
        ucs_list_add_tail(&(ev->event_system->pending_emits), &(ev->item));
        SYS_EVENT_UNLOCK(ev->event_system);
        return EVENT_INPROGRESS;
    }
    SYS_EVENT_UNLOCK(ev->event_system);
#if USE_AM_IMPLEM
    return am_send_event_msg(event);
#else
    return tag_send_event_msg(event);
#endif // USE_AM_IMPLEM
}

dpu_offload_status_t event_channels_fini(dpu_offload_ev_sys_t **ev_sys)
{
    SYS_EVENT_LOCK(*ev_sys);
    if ((*ev_sys)->num_used_evs > 0)
    {
        WARN_MSG("[WARN] %ld events objects have not been returned", (*ev_sys)->num_used_evs);
    }

#if USE_AM_IMPLEM
    // Deregister the UCX AM handler
    ucp_am_handler_param_t am_param;
    am_param.field_mask = UCP_AM_HANDLER_PARAM_FIELD_ID |
                          UCP_AM_HANDLER_PARAM_FIELD_CB;
    am_param.id = AM_EVENT_MSG_ID;
    // For all exchange, we receive the header first and from there post a receive for the either eager or RDV message.
    am_param.cb = NULL;
    /* FIXME: not working, create a crash, no idea why */
    // ucs_status_t status = ucp_worker_set_am_recv_handler(GET_WORKER((*ev_sys)->econtext),
    //                                                      &am_param);
    // CHECK_ERR_RETURN((status != UCS_OK), DO_ERROR, "unable to reset UCX AM recv handler");
#endif // USE_AM_IMPLEM

    DYN_LIST_FREE((*ev_sys)->free_evs, dpu_offload_event_t, item);
    DYN_LIST_FREE((*ev_sys)->free_pending_notifications, pending_notification_t, item);
    SYS_EVENT_UNLOCK(*ev_sys);
    free(*ev_sys);
    *ev_sys = NULL;
}

dpu_offload_status_t event_get(dpu_offload_ev_sys_t *ev_sys, dpu_offload_event_info_t *info, dpu_offload_event_t **ev)
{
    dpu_offload_event_t *_ev;
    DBG("Getting event...");
    CHECK_ERR_RETURN((ev_sys == NULL), DO_ERROR, "Undefine event system");
    SYS_EVENT_LOCK(ev_sys);
    DYN_LIST_GET(ev_sys->free_evs, dpu_offload_event_t, item, _ev);
    SYS_EVENT_UNLOCK(ev_sys);
    if (_ev != NULL)
    {
        SYS_EVENT_LOCK(ev_sys);
        ev_sys->num_used_evs++;
        SYS_EVENT_UNLOCK(ev_sys);
        RESET_EVENT(_ev);
        CHECK_EVENT(_ev);
        _ev->event_system = ev_sys;
        _ev->scope_id = _ev->event_system->econtext->scope_id;

        if (info == NULL || info->payload_size == 0)
            goto out;

        // If we get here, it means that we need to manage a payload buffer for that event
        _ev->manage_payload_buf = true;
        _ev->ctx.hdr.payload_size = info->payload_size;
        _ev->payload = MALLOC(info->payload_size); // No advanced memory management at the moment, just malloc
        assert(_ev->payload);
    }

out:
    DBG("Got event %p from list %p", _ev, ev_sys->free_evs);
    *ev = _ev;
    return DO_SUCCESS;
}

static dpu_offload_status_t do_event_return(dpu_offload_event_t *ev)
{
    assert(ev);
    if (ev->req)
    {
        WARN_MSG("returning event %p but it is still in progress", ev);
        return EVENT_INPROGRESS;
    }

    assert(ev->event_system);
    if (ev->was_pending)
    {
        SYS_EVENT_LOCK(ev->event_system);
        ev->event_system->num_pending_sends--;
        SYS_EVENT_UNLOCK(ev->event_system);
    }

    // If the event has a payload buffer that the library is managing, free that buffer
    if (ev->manage_payload_buf && ev->payload != NULL)
    {
        free(ev->payload);
        ev->payload = NULL;
        ev->ctx.hdr.payload_size = 0;
    }

    SYS_EVENT_LOCK(ev->event_system);
    ev->event_system->num_used_evs--;
    DYN_LIST_RETURN((ev->event_system->free_evs), ev, item);
    SYS_EVENT_UNLOCK(ev->event_system);
    DBG("event %p successfully returned", ev); 
    return EVENT_DONE;
}

dpu_offload_status_t event_return(dpu_offload_event_t **ev)
{
    assert(ev);
    assert(*ev);
    if (ev == NULL || (*ev) == NULL)
        return DO_SUCCESS;
    int ret = do_event_return(*ev);
    if (ret == EVENT_INPROGRESS)
    {
        return DO_ERROR;
    }
    *ev = NULL; // so it cannot be used any longer
    return DO_SUCCESS;
}

/**
 * @brief event_completed is a helper function to check whether an event is completed
 * The function is aware of sub-events. If all the sub-events are completed and the
 * request of the event is NULL, the event is reported as completed. The caller is
 * responsible for returning the event when reported as completed.
 *
 * @param ev Event to check for completion.
 * @return true when the event is completed
 * @return false when the event is still in progress
 */
bool event_completed(dpu_offload_event_t *ev)
{
    if (ev->ctx.complete)
        return true;

    assert(ev->event_system);

    // Update the list of sub-event by removing the completed ones
    if (ev->sub_events_initialized)
    {
        dpu_offload_event_t *subevt, *next;
        ucs_list_for_each_safe(subevt, next, &(ev->sub_events), item)
        {
            if (subevt->ctx.complete)
            {
                ucs_list_del(&(subevt->item));
                DBG("returning sub event %p of main event %p", subevt, ev);
                dpu_offload_status_t rc = do_event_return(subevt);
                CHECK_ERR_RETURN((rc), DO_ERROR, "event_return() failed");
            }
        }

        if (ucs_list_is_empty(&(ev->sub_events)) && ev->req == NULL)
        {
            DBG("All sub-events completed");
            ev->ctx.complete = true;
            return true;
        }
    }
    else
    {
#if !USE_AM_IMPLEM
        if (ev->hdr_request == NULL && ev->payload_request == NULL)
        {
            DBG("Event %p is completed", ev);
            ev->ctx.complete = true;
        }
#endif
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
    ucs_list_for_each_safe(cur_op, next_op, &(econtext->active_ops), item)
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
    RESET_OP_DESC(op_desc);
    ucs_list_add_tail(&(econtext->active_ops), &(op_desc->item));
    // Call the init function of the operation
    op_cfg->op_init();
    return DO_SUCCESS;
}

static dpu_offload_status_t xgvmi_key_revoke_cb(struct dpu_offload_ev_sys *ev_sys, execution_context_t *context, am_header_t *hdr, size_t hdr_size, void *data, size_t data_len)
{
    // todo
    return DO_ERROR;
}

static dpu_offload_status_t xgvmi_key_recv_cb(struct dpu_offload_ev_sys *ev_sys, execution_context_t *context, am_header_t *hdr, size_t hdr_size, void *data, size_t data_len)
{
    // todo
    return DO_ERROR;
}

/************************************/
/* Endpoint cache related functions */
/************************************/

extern dpu_offload_status_t send_group_cache(execution_context_t *econtext, ucp_ep_h dest, int64_t gp_id, dpu_offload_event_t *metaev);
extern int send_cache_entry_request(execution_context_t *econtext, ucp_ep_h ep, rank_info_t *requested_peer, dpu_offload_event_t **ev);

bool is_in_cache(cache_t *cache, int64_t gp_id, int64_t rank_id)
{
    peer_cache_entry_t *entry = GET_GROUP_RANK_CACHE_ENTRY(cache, gp_id, rank_id);
    if (entry == NULL)
        return false;
    return (entry->set);
}

static dpu_offload_status_t peer_cache_entries_request_recv_cb(struct dpu_offload_ev_sys *ev_sys, execution_context_t *econtext, am_header_t *hdr, size_t hdr_size, void *data, size_t data_len)
{
    assert(econtext);
    assert(data);
    rank_info_t *rank_info = (rank_info_t *)data;

    DBG("Cache entry requested received for gp/rank %" PRIu64 "/%" PRIu64, rank_info->group_id, rank_info->group_rank);

    if (is_in_cache(&(econtext->engine->procs_cache), rank_info->group_id, rank_info->group_rank))
    {
        // We send the cache back to the sender
        dpu_offload_status_t rc;
        ucp_ep_h dest;
        group_cache_t *gp_caches;
        dpu_offload_event_t *send_cache_ev;
        peer_info_t *peer_info = DYN_ARRAY_GET_ELT(&(econtext->server->connected_clients.clients), hdr->id, peer_info_t);
        assert(peer_info);
        event_get(econtext->event_channels, NULL, &send_cache_ev);
        assert(econtext->type == CONTEXT_SERVER);
        dest = peer_info->ep;
        gp_caches = (group_cache_t *)econtext->engine->procs_cache.data.base;
        DBG("Sending group cache to DPU #%" PRIu64 ": event=%p", hdr->id, send_cache_ev);
        rc = send_group_cache(econtext, dest, rank_info->group_id, send_cache_ev);
        CHECK_ERR_RETURN((rc), DO_ERROR, "send_group_cache() failed");

        // Add the event to the list of pending events so it completed implicitely
        if (send_cache_ev != NULL && !event_completed(send_cache_ev))
            ucs_list_add_tail(&(econtext->ongoing_events), &(send_cache_ev->item));

        return DO_SUCCESS;
    }

    // If the entry is not in the cache we forward the request
    // and also trigger the send of our cache for the target group
    // Fixme: forward should happen only when the request is coming from a local rank, not a DPU
    if (econtext->engine->on_dpu)
    {
        size_t i;
        DBG("Entry not in the cache, forwarding the request to other DPUs");
        for (i = 0; i < econtext->engine->num_dpus; i++)
        {
            dpu_offload_status_t rc;
            dpu_offload_event_t *req_fwd_ev;
            remote_dpu_info_t **list_dpus = (remote_dpu_info_t **)econtext->engine->dpus.base;
            rc = send_cache_entry_request(econtext, list_dpus[i]->ep, rank_info, &req_fwd_ev);
            CHECK_ERR_RETURN((rc), DO_ERROR, "send_group_cache() failed");

            // Add the event to the list of pending events so it completed implicitely
            if (req_fwd_ev != NULL && !event_completed(req_fwd_ev))
                ucs_list_add_tail(&(econtext->ongoing_events), &(req_fwd_ev->item));

            return DO_SUCCESS;
        }
    }

    return DO_ERROR;
}

static dpu_offload_status_t peer_cache_entries_recv_cb(struct dpu_offload_ev_sys *ev_sys, execution_context_t *econtext, am_header_t *hdr, size_t hdr_size, void *data, size_t data_len)
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
        DBG("Received a cache entry for rank:%ld, group:%ld from DPU %" PRId64 " (msg size=%ld, peer addr len=%ld)",
            group_rank, group_id, hdr->id, data_len, entries[idx].peer.addr_len);

        if (!is_in_cache(cache, group_id, group_rank))
        {
            peer_cache_entry_t *cache_entry = SET_PEER_CACHE_ENTRY(cache, &(entries[idx]));
            group_cache_t *gp_caches = (group_cache_t *)cache->data.base;
            // If any event is associated to the cache entry, handle them
            if (cache_entry->events_initialized)
            {

                while (!ucs_list_is_empty(&(cache_entry->events)))
                {
                    dpu_offload_event_t *e = ucs_list_extract_head(&(cache_entry->events), dpu_offload_event_t, item);
                    e->ctx.complete = 1;
                }
            }
        }
        cur_size += sizeof(peer_cache_entry_t);
        idx++;
    }
    DBG("Reception completed");

    return DO_SUCCESS;
}

/**
 * @brief add_group_rank_recv_cb is invoked on the DPU when receiving a notification from a rank running on the local host
 * that a new group/rank had been created
 *
 * @param ev_sys Associated event/notification system
 * @param econtext Associated execution contexxt
 * @param hdr Header of the received notification
 * @param hdr_size Size of the header
 * @param data Payload associated to the notification
 * @param data_len Size of the payload
 * @return dpu_offload_status_t
 */
static dpu_offload_status_t add_group_rank_recv_cb(struct dpu_offload_ev_sys *ev_sys, execution_context_t *econtext, am_header_t *hdr, size_t hdr_size, void *data, size_t data_len)
{
    assert(econtext);
    assert(data);

    offloading_engine_t *engine = (offloading_engine_t *)econtext->engine;
    rank_info_t *rank_info = (rank_info_t *)data;

    if (!is_in_cache(&(econtext->engine->procs_cache), rank_info->group_id, rank_info->group_rank))
    {
        SET_GROUP_RANK_CACHE_ENTRY(econtext, rank_info->group_id, rank_info->group_rank);
    }

    return DO_SUCCESS;
}

/*************************************/
/* Registration of all the callbacks */
/*************************************/

dpu_offload_status_t register_default_notifications(dpu_offload_ev_sys_t *ev_sys)
{
    CHECK_ERR_RETURN((ev_sys == NULL), DO_ERROR, "Undefined event channels");
    SYS_EVENT_LOCK(ev_sys);

    int rc = event_channel_register(ev_sys, AM_OP_START_MSG_ID, op_start_cb);
    CHECK_ERR_GOTO(rc, error_out, "cannot register handler to start operations");

    rc = event_channel_register(ev_sys, AM_OP_COMPLETION_MSG_ID, op_completion_cb);
    CHECK_ERR_GOTO(rc, error_out, "cannot register handler for operation completion");

    rc = event_channel_register(ev_sys, AM_XGVMI_ADD_MSG_ID, xgvmi_key_recv_cb);
    CHECK_ERR_GOTO(rc, error_out, "cannot register handler for receiving XGVMI keys");

    rc = event_channel_register(ev_sys, AM_XGVMI_DEL_MSG_ID, xgvmi_key_revoke_cb);
    CHECK_ERR_GOTO(rc, error_out, "cannot register handler for revoke XGVMI keys");

    rc = event_channel_register(ev_sys, AM_PEER_CACHE_ENTRIES_MSG_ID, peer_cache_entries_recv_cb);
    CHECK_ERR_GOTO(rc, error_out, "cannot register handler for receiving peer cache entries");

    rc = event_channel_register(ev_sys, AM_PEER_CACHE_ENTRIES_REQUEST_MSG_ID, peer_cache_entries_request_recv_cb);
    CHECK_ERR_GOTO(rc, error_out, "cannot register handler for receiving peer cache requests");

    rc = event_channel_register(ev_sys, AM_ADD_GP_RANK_MSG_ID, add_group_rank_recv_cb);
    CHECK_ERR_GOTO(rc, error_out, "cannot register handler for receiving requests to add a group/rank");
    SYS_EVENT_UNLOCK(ev_sys);

    return DO_SUCCESS;
error_out:
    SYS_EVENT_UNLOCK(ev_sys);
    return DO_ERROR;
}