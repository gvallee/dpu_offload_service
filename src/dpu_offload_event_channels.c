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

#define DISPLAY_ECONTEXT_ONGOING_EVTS(_ec)                          \
    do                                                              \
    {                                                               \
        INFO_MSG("econtext %p: %ld ongoing events",                 \
                 (_ec), ucs_list_length(&((_ec)->ongoing_events))); \
    } while(0)

#define DISPLAY_ONGOING_EVENTS_INFO(_e)                         \
    do                                                          \
    {                                                           \
        size_t _i;                                              \
        for (_i = 0; _i < (_e)->num_servers; _i++)              \
        {                                                       \
            DISPLAY_ECONTEXT_ONGOING_EVTS((_e)->servers[_i]);   \
        }                                                       \
                                                                \
        if ((_e)->client)                                       \
        {                                                       \
            DISPLAY_ECONTEXT_ONGOING_EVTS((_e)->client);        \
        }                                                       \
    } while(0)

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
    ucp_request_param_t am_rndv_recv_request_params = {0};
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
        pending_recv->user_data = DPU_OFFLOAD_MALLOC(payload_size);
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
    dpu_offload_ev_sys_t *event_channels = DPU_OFFLOAD_MALLOC(sizeof(dpu_offload_ev_sys_t));
    CHECK_ERR_RETURN((event_channels == NULL), DO_ERROR, "Resource allocation failed");
    RESET_EV_SYS(event_channels);
    size_t num_evts = DEFAULT_NUM_EVTS;
    size_t num_free_pending_notifications = DEFAULT_NUM_NOTIFICATION_CALLBACKS;
    DYN_LIST_ALLOC_WITH_INIT_CALLBACK(event_channels->free_evs, num_evts, dpu_offload_event_t, item, init_event);
    assert(event_channels->free_evs);
    DYN_LIST_ALLOC(event_channels->free_pending_notifications, num_free_pending_notifications, pending_notification_t, item);
    assert(event_channels->free_pending_notifications);
    ucs_list_head_init(&(event_channels->pending_notifications));
    DYN_ARRAY_ALLOC(&(event_channels->notification_callbacks), DEFAULT_NUM_NOTIFICATION_CALLBACKS, notification_callback_entry_t);
    CHECK_ERR_RETURN((event_channels->notification_callbacks.base == NULL), DO_ERROR, "Resource allocation failed");
    event_channels->mutex = (pthread_mutex_t)PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP;
    *ev_channels = event_channels;
    return DO_SUCCESS;
}

dpu_offload_status_t event_channels_init(execution_context_t *econtext)
{
    CHECK_ERR_RETURN((econtext == NULL), DO_ERROR, "Undefined execution context");

    dpu_offload_status_t rc = ev_channels_init(&(econtext->event_channels));
    CHECK_ERR_RETURN((rc), DO_ERROR, "ev_channels_init() failed");

#if USE_AM_IMPLEM
    // Register the UCX AM handler
    CHECK_ERR_RETURN((GET_WORKER(econtext) == NULL), DO_ERROR, "Undefined worker");
    ucp_am_handler_param_t am_param = {0};
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
    DBG("Callback for notification of type %" PRIu64 " is now registered on event system %p (econtext: %p)",
        type, ev_sys, ev_sys->econtext);

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

    ENGINE_LOCK(engine);
    notification_callback_entry_t *entry = DYN_ARRAY_GET_ELT(&(engine->default_notifications->notification_callbacks), type, notification_callback_entry_t);
    ENGINE_UNLOCK(engine);
    CHECK_ERR_RETURN((entry == NULL), DO_ERROR, "unable to get callback %ld", type);
    CHECK_ERR_RETURN((entry->set == true), DO_ERROR, "type %" PRIu64 " is already set", type);

    // Make sure the self_econtext gets the notification handler registered as well
    assert(engine->self_econtext);
    dpu_offload_status_t rc = event_channel_register(engine->self_econtext->event_channels, type, cb);
    CHECK_ERR_RETURN((rc), DO_ERROR, "event_channel_register() failed");

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
    DBG("ev=%p ctx=%p id=%" PRIu64, ev, &(ev->ctx), EVENT_HDR_ID(ev));
    COMPLETE_EVENT(ev);
    assert(ev->event_system);
    execution_context_t *econtext = ev->event_system->econtext;
    DBG("Associated econtext: %p", econtext);
    assert(econtext);
    DBG("evt %p now completed, %ld events left on the ongoing list", ev, ucs_list_length(&(econtext->ongoing_events)));
}

static void notif_hdr_send_cb(void *request, ucs_status_t status, void *user_data)
{
    dpu_offload_event_t *ev = (dpu_offload_event_t *)user_data;
    assert(status == UCS_OK);
    if (request != NULL && UCS_PTR_IS_ERR(request))
    {
        ucs_status_t send_status = UCS_PTR_STATUS(request);
        ERR_MSG("send failed: %s", ucs_status_string(send_status));
        abort();
    }
    DBG("HDR for event #%ld successfully sent", ev->seq_num);
    ev->ctx.hdr_completed = true;
}

static void notif_payload_send_cb(void *request, ucs_status_t status, void *user_data)
{
    dpu_offload_event_t *ev = (dpu_offload_event_t *)user_data;
    assert(status == UCS_OK);
    if (request != NULL && UCS_PTR_IS_ERR(request))
    {
        ucs_status_t send_status = UCS_PTR_STATUS(request);
        ERR_MSG("send failed: %s", ucs_status_string(send_status));
        abort();
    }
    ev->ctx.payload_completed = true;
    DBG("Payload for event #%ld (%p) successfully sent (hdr completed: %d)", ev->seq_num, ev, ev->ctx.hdr_completed);
}

#if USE_AM_IMPLEM
uint64_t num_ev_sent = 0;
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
int do_tag_send_event_msg(dpu_offload_event_t *event)
{
    int rc = EVENT_INPROGRESS;
    assert(event->dest.ep);
    if (event->ctx.hdr_completed && event->ctx.payload_completed)
    {
        DBG("Event %p already completed, not sending", event);
        return EVENT_DONE;
    }

    if (send_moderation_on() && !CAN_POST(event->event_system))
    {
        // We reached the maximum number of posted sends that are waiting for completion,
        // we queue the send for later posting
        DBG("Delaying send of %p (#%ld), already %ld events are waiting for completion (client_id: %ld, server_id: %ld)",
            event, event->seq_num, event->event_system->posted_sends, EVENT_HDR_CLIENT_ID(event), EVENT_HDR_SERVER_ID(event));
        return EVENT_INPROGRESS;
    }

    /* 1. Send the hdr, we can be in the middle of a send, meaning the HDR went
       through but the payload */
    if (!(event->ctx.hdr_completed) && event->hdr_request == NULL)
    {
        ucp_request_param_t hdr_send_param = {0};
        ucp_tag_t hdr_ucp_tag = MAKE_SEND_TAG(AM_EVENT_MSG_HDR_ID,
                                              event->client_id,
                                              event->server_id,
                                              event->scope_id,
                                              0);
        hdr_send_param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                                      UCP_OP_ATTR_FIELD_DATATYPE |
                                      UCP_OP_ATTR_FIELD_USER_DATA;
        hdr_send_param.cb.send = notif_hdr_send_cb;
        hdr_send_param.datatype = ucp_dt_make_contig(1);
        hdr_send_param.user_data = (void *)event;
        DBG("Sending notification header - ev: %" PRIu64 ", type: %" PRIu64 ", econtext: %p, scope_id: %d, client_id: %" PRIu64", server_id: %" PRIu64,
            event->seq_num, EVENT_HDR_TYPE(event), event->event_system->econtext, event->scope_id, event->client_id, event->server_id);
        event->hdr_request = NULL;
        event->payload_request = NULL;
        event->hdr_request = ucp_tag_send_nbx(event->dest.ep, EVENT_HDR(event), sizeof(am_header_t), hdr_ucp_tag, &hdr_send_param);
        if (UCS_PTR_IS_ERR(event->hdr_request))
        {
            ucs_status_t send_status = UCS_PTR_STATUS(event->hdr_request);
            ERR_MSG("ucp_tag_send_nbx() failed: %s", ucs_status_string(send_status));
            return send_status;
        }
        if (event->hdr_request == NULL)
        {
            event->ctx.hdr_completed = true;
        }
        else
        {
            assert(event->was_posted == false);
            event->was_posted = true;
            event->event_system->posted_sends++;
        }
        DBG("event %p (%ld) send posted (hdr) - scope_id: %d, req: %p",
            event, event->seq_num, event->scope_id, event->hdr_request);
    }

    /* 2. Send the payload */
    if (EVENT_HDR_PAYLOAD_SIZE(event) > 0)
    {
        if (!(event->ctx.payload_completed) && event->payload_request == NULL)
        {
            DBG("Sending payload - tag: %d, scope_id: %d, size: %ld, client_id: %" PRIu64 ", server_id: %" PRIu64,
                AM_EVENT_MSG_ID,
                event->scope_id, 
                EVENT_HDR_PAYLOAD_SIZE(event),
                event->client_id,
                event->server_id);
            ucp_tag_t payload_ucp_tag = MAKE_SEND_TAG(AM_EVENT_MSG_ID,
                                                      event->client_id,
                                                      event->server_id,
                                                      event->scope_id,
                                                      0);
            struct ucx_context *payload_request = NULL;
            ucp_request_param_t payload_send_param = {0};
            payload_send_param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                                              UCP_OP_ATTR_FIELD_DATATYPE |
                                              UCP_OP_ATTR_FIELD_USER_DATA;
            payload_send_param.cb.send = notif_payload_send_cb;
            payload_send_param.datatype = ucp_dt_make_contig(1);
            payload_send_param.user_data = (void *)event;
            payload_request = ucp_tag_send_nbx(event->dest.ep,
                                               event->payload,
                                               EVENT_HDR_PAYLOAD_SIZE(event),
                                               payload_ucp_tag,
                                               &payload_send_param);
            if (UCS_PTR_IS_ERR(payload_request))
            {
                ucs_status_t send_status = UCS_PTR_STATUS(payload_request);
                ERR_MSG("ucp_tag_send_nbx() failed: %s", ucs_status_string(send_status));
                DISPLAY_ONGOING_EVENTS_INFO(event->event_system->econtext->engine);
                abort();
                return send_status;
            }
            if (payload_request != NULL)
            {
                // The send did not complete right away
                event->payload_request = payload_request;
                if (event->was_posted == false)
                {
                    event->was_posted = true;
                    event->event_system->posted_sends++;
                }
            }
            else
            {
                // The send completed right away
                event->ctx.payload_completed = true;
            }

            DBG("event %p (%ld) send posted (payload) - scope_id: %d, req: %p, complete: %d",
                event, event->seq_num, event->scope_id, payload_request, event->ctx.payload_completed);
        }
    }
    else
    {
        // no payload to send
        assert(event->payload_request == NULL);
        event->ctx.payload_completed = true;
    }

    if (event->ctx.hdr_completed == true && event->ctx.payload_completed == true)
    {
        DBG("ev %p completed right away", event);
        rc = EVENT_DONE;
    }

    return rc;
}

int tag_send_event_msg(dpu_offload_event_t **event)
{
    int rc;
    assert((*event)->dest.ep);
    assert((*event)->event_system);
    assert((*event)->event_system->econtext);
    execution_context_t *econtext = (*event)->event_system->econtext;
    uint64_t client_id, server_id;
    switch ((*event)->event_system->econtext->type)
    {
    case CONTEXT_CLIENT:
        client_id = econtext->client->id;
        server_id = econtext->client->server_id;
        break;
    case CONTEXT_SERVER:
        server_id = econtext->server->id;
        client_id = (*event)->dest.id;
        break;
    case CONTEXT_SELF:
        client_id = 0;
        server_id = 0;
        break;
    default:
        return DO_ERROR;
    }

#if !NDEBUG
    (*event)->client_id = client_id;
    (*event)->server_id = server_id;
    EVENT_HDR_SEQ_NUM(*event) = (*event)->seq_num;
    EVENT_HDR_CLIENT_ID(*event) = (*event)->client_id;
    EVENT_HDR_SERVER_ID(*event) = (*event)->server_id;
    if (econtext->type == CONTEXT_CLIENT && econtext->scope_id == SCOPE_HOST_DPU && econtext->rank.n_local_ranks > 0 && econtext->rank.n_local_ranks != UINT64_MAX)
    {
        assert(client_id < econtext->rank.n_local_ranks);
    }
#endif

    rc = do_tag_send_event_msg(*event);
    if (rc == EVENT_DONE && !((*event)->explicit_return))
    {
        COMPLETE_EVENT(*event);
        DBG("event %p send immediately completed", (*event));
        dpu_offload_status_t rc = event_return(event);
        CHECK_ERR_RETURN((rc), DO_ERROR, "event_return() failed");
        return EVENT_DONE;
    }
    return EVENT_INPROGRESS;
}
#endif // USE_AM_IMPLEM

int event_channel_emit_with_payload(dpu_offload_event_t **event, uint64_t type, ucp_ep_h dest_ep, uint64_t dest_id, void *ctx, void *payload, size_t payload_size)
{
    int rc = EVENT_INPROGRESS;
    assert(event);
    assert(dest_ep);
    assert((*event));
    // This function can only be used when the user is managing the payload
    assert((*event)->manage_payload_buf == false);

    // Try to progress the sends before adding another one
    progress_econtext_sends((*event)->event_system->econtext);

    DBG("Sending event %p of type %" PRIu64, *event, type);
#if USE_AM_IMPLEM
    (*event)->ctx.complete = false;
#else
    (*event)->ctx.hdr_completed = false;
    (*event)->ctx.payload_completed = false;
#endif
    EVENT_HDR_TYPE(*event) = type;
    EVENT_HDR_ID(*event) = ECONTEXT_ID((*event)->event_system->econtext);
    EVENT_HDR_PAYLOAD_SIZE(*event) = payload_size;
    (*event)->user_context = ctx;
    (*event)->payload = payload;
    (*event)->dest.ep = dest_ep;
    (*event)->dest.id = dest_id;

    if ((*event)->event_system->econtext->type == CONTEXT_SELF)
    {
        // Do not go through the comm layer (e.g., UCX), just deliver it internally
        int ret = handle_notif_msg((*event)->event_system->econtext,
                                   EVENT_HDR(*event),
                                   sizeof(am_header_t),
                                   (*event)->payload,
                                   EVENT_HDR_PAYLOAD_SIZE(*event));
        if (ret != EVENT_DONE)
        {
            ERR_MSG("local delivery of event did not complete");
#if !NDEBUG
            abort();
#endif
            return DO_ERROR;
        }

        dpu_offload_status_t return_rc = event_return(event);
        if (return_rc != DO_SUCCESS)
        {
            ERR_MSG("event_return() failed");
#if !NDEBUG
            abort();
#endif
            return DO_ERROR;
        }
        return ret;
    }

#if USE_AM_IMPLEM
    rc = am_send_event_msg(event);
#else
    rc = tag_send_event_msg(event);
#endif // USE_AM_IMPLEM

    if (rc == EVENT_INPROGRESS)
    {
        QUEUE_EVENT(*event);
    }

    return rc;
}

int event_channel_emit(dpu_offload_event_t **event, uint64_t type, ucp_ep_h dest_ep, uint64_t dest_id, void *ctx)
{
    int rc = EVENT_INPROGRESS;
    DBG("Sending event %p of type %" PRIu64, *event, type);

    // Try to progress the sends before adding another one
    progress_econtext_sends((*event)->event_system->econtext);
#if USE_AM_IMPLEM
    (*event)->ctx.complete = false;
#else
    (*event)->ctx.hdr_completed = false;
    (*event)->ctx.payload_completed = false;
#endif
    EVENT_HDR_TYPE(*event) = type;
    EVENT_HDR_ID(*event) = ECONTEXT_ID((*event)->event_system->econtext);
    (*event)->user_context = ctx;
    (*event)->dest.ep = dest_ep;
    (*event)->dest.id = dest_id;

    if ((*event)->event_system->econtext->type == CONTEXT_SELF)
    {
        // Do not go through the comm layer (e.g., UCX), just deliver it internally
        int ret = handle_notif_msg((*event)->event_system->econtext,
                                   EVENT_HDR(*event),
                                   sizeof(am_header_t),
                                   (*event)->payload,
                                   EVENT_HDR_PAYLOAD_SIZE(*event));
        if (ret != EVENT_DONE)
        {
            ERR_MSG("local delivery of event did not complete");
#if !NDEBUG
            abort();
#endif
            return DO_ERROR;
        }

        dpu_offload_status_t return_rc = event_return(event);
        if (return_rc != DO_SUCCESS)
        {
            ERR_MSG("event_return() failed");
#if !NDEBUG
            abort();
#endif
            return DO_ERROR;
        }

        return ret;
    }

#if USE_AM_IMPLEM
    rc = am_send_event_msg(event);
#else
    rc = tag_send_event_msg(event);
#endif // USE_AM_IMPLEM
    if (rc == EVENT_INPROGRESS)
    {
        QUEUE_EVENT(*event);
    }
    return rc;
}

void event_channels_fini(dpu_offload_ev_sys_t **ev_sys)
{
    if (ev_sys == NULL || *ev_sys == NULL)
        return;

    if ((*ev_sys)->num_used_evs > 0)
    {
        WARN_MSG("%ld events objects have not been returned", (*ev_sys)->num_used_evs);
    }

#if USE_AM_IMPLEM
    // Deregister the UCX AM handler
    ucp_am_handler_param_t am_param = {0};
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

    assert((*ev_sys)->free_evs);
    assert((*ev_sys)->free_pending_notifications);
    DYN_LIST_FREE((*ev_sys)->free_evs, dpu_offload_event_t, item);
    DYN_LIST_FREE((*ev_sys)->free_pending_notifications, pending_notification_t, item);
    DYN_ARRAY_FREE(&((*ev_sys)->notification_callbacks));
    free(*ev_sys);
    *ev_sys = NULL;
}

// todo: once stable, remove the seq num, we do not really need it other than for
// debugging purposes.
uint64_t seq_num = 0;
dpu_offload_status_t event_get(dpu_offload_ev_sys_t *ev_sys, dpu_offload_event_info_t *info, dpu_offload_event_t **ev)
{
    dpu_offload_event_t *_ev;
    DBG("Getting event (econtext: %p, econtext scope_id: %d)...",
        ev_sys->econtext, ev_sys->econtext->scope_id);
    CHECK_ERR_RETURN((ev_sys == NULL), DO_ERROR, "Undefine event system");
    SYS_EVENT_LOCK(ev_sys);
    DYN_LIST_GET(ev_sys->free_evs, dpu_offload_event_t, item, _ev);
    _ev->seq_num = seq_num;
    seq_num++;
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

        if (info != NULL)
        {
            _ev->explicit_return = info->explicit_return;
        }

        if (info == NULL || info->payload_size == 0)
            goto out;

        // If we get here, it means that we need to manage a payload buffer for that event
        _ev->manage_payload_buf = true;
        EVENT_HDR_PAYLOAD_SIZE(_ev) = info->payload_size;
        _ev->payload = DPU_OFFLOAD_MALLOC(info->payload_size); // No advanced memory management at the moment, just malloc
        assert(_ev->payload);
    }

out:
    DBG("Got event #%" PRIu64 " (%p) from list %p (scope_id: %d)", _ev->seq_num, _ev, ev_sys->free_evs, _ev->scope_id);
    *ev = _ev;
    return DO_SUCCESS;
}

static dpu_offload_status_t do_event_return(dpu_offload_event_t *ev)
{
    assert(ev);
    assert(ev->event_system);
#if !NDEBUG
    if (!ev->explicit_return)
    {
        assert(ev->was_posted == false);
    }
#endif
    if (ev->req)
    {
        WARN_MSG("returning event %p but it is still in progress", ev);
        return EVENT_INPROGRESS;
    }

    assert(EVENT_HDR_TYPE(ev) > 0);
    assert(EVENT_HDR_TYPE(ev) != UINT64_MAX);

    // If the event has a payload buffer that the library is managing, free that buffer
    if (ev->manage_payload_buf && ev->payload != NULL)
    {
        free(ev->payload);
        ev->payload = NULL;
        EVENT_HDR_PAYLOAD_SIZE(ev) = 0;
    }

#if !USE_AM_IMPLEM
    if (ev->hdr_request != NULL)
    {
        assert(ev->ctx.hdr_completed == true);
        ucp_request_free(ev->hdr_request);
        ev->hdr_request = NULL;
    }
    if (ev->payload_request != NULL)
    {
        assert(ev->ctx.payload_completed == true);
        ucp_request_free(ev->payload_request);
        ev->payload_request = NULL;
    }
#endif

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
    if ((*ev)->is_subevent && (*ev)->is_ongoing_event)
    {
        ERR_MSG("event %p is a subevent and on the ongoing list, which is prohibited", *ev);
        return DO_ERROR;
    }
    if ((*ev)->is_ongoing_event)
    {
        ERR_MSG("event %p (#%ld, type: %ld) is still on the ongoing list, which is prohibited",
                *ev, (*ev)->seq_num, EVENT_HDR_TYPE(*ev));
        return DO_ERROR;
    }
    assert(!((*ev)->is_ongoing_event));
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

bool event_completed(dpu_offload_event_t *ev)
{
#if USE_AM_IMPLEM
    if (EVENT_HDR_TYPE(ev) != META_EVENT_TYPE && ev->ctx.complete)
        goto event_completed;
#else
    if (EVENT_HDR_TYPE(ev) != META_EVENT_TYPE && ev->ctx.hdr_completed == true && ev->ctx.payload_completed == true)
        goto event_completed;
#endif

    assert(ev->event_system);
    // Update the list of sub-event by removing the completed ones
    if (EVENT_HDR_TYPE(ev) == META_EVENT_TYPE)
    {
        dpu_offload_event_t *subevt, *next;
        ucs_list_for_each_safe(subevt, next, &(ev->sub_events), item)
        {
            assert(subevt->is_subevent);
#if USE_AM_IMPLEM
            if (subevt->ctx.complete)
#else
            if (subevt->ctx.hdr_completed && subevt->ctx.payload_completed)
#endif
            {
                ucs_list_del(&(subevt->item));
                subevt->is_subevent = false;
                DBG("returning sub event %" PRIu64 " %p of main event %p", subevt->seq_num, subevt, ev);
                dpu_offload_status_t rc = do_event_return(subevt);
                CHECK_ERR_RETURN((rc), DO_ERROR, "event_return() failed");
            }
        }

        if (ucs_list_is_empty(&(ev->sub_events)))
        {
            DBG("All sub-events completed, metaev %p %ld completes", ev, ev->seq_num);
            goto event_completed;
        }
    }
    else
    {
#if !USE_AM_IMPLEM
        if (ev->ctx.hdr_completed == true && ev->ctx.payload_completed == true)
            goto event_completed;
#endif
    }
    return false;

event_completed:
    DBG("Event %p (#%ld) is completed", ev, ev->seq_num);
    COMPLETE_EVENT(ev);
    return true;
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

extern int send_cache_entry_request(execution_context_t *econtext, ucp_ep_h ep, rank_info_t *requested_peer, dpu_offload_event_t **ev);

static dpu_offload_status_t peer_cache_entries_request_recv_cb(struct dpu_offload_ev_sys *ev_sys, execution_context_t *econtext, am_header_t *hdr, size_t hdr_size, void *data, size_t data_len)
{
    assert(econtext);
    assert(data);
    rank_info_t *rank_info = (rank_info_t *)data;

    DBG("Cache entry requested received for gp/rank %" PRIu64 "/%" PRIu64, rank_info->group_id, rank_info->group_rank);

    if (is_in_cache(&(econtext->engine->procs_cache), rank_info->group_id, rank_info->group_rank, rank_info->group_size))
    {
        // We send the cache back to the sender
        dpu_offload_status_t rc;
        dpu_offload_event_t *send_cache_ev;
        peer_info_t *peer_info = DYN_ARRAY_GET_ELT(&(econtext->server->connected_clients.clients), hdr->id, peer_info_t);
        assert(peer_info);
        event_get(econtext->event_channels, NULL, &send_cache_ev);
        assert(send_cache_ev);
        EVENT_HDR_TYPE(send_cache_ev) = META_EVENT_TYPE;
        assert(econtext->type == CONTEXT_SERVER);
        DBG("Sending group cache to DPU #%" PRIu64 ": event=%p", hdr->id, send_cache_ev);
        rc = send_group_cache(econtext, peer_info->ep, peer_info->id, rank_info->group_id, send_cache_ev);
        CHECK_ERR_RETURN((rc), DO_ERROR, "send_group_cache() failed");
        QUEUE_EVENT(send_cache_ev);
        return DO_SUCCESS;
    }

    // If the entry is not in the cache we forward the request
    // and also trigger the send of our cache for the target group
    if (econtext->engine->on_dpu && econtext->scope_id == SCOPE_HOST_DPU)
    {
        size_t i;
        DBG("Entry not in the cache, forwarding the request to other DPUs");
        for (i = 0; i < econtext->engine->num_dpus; i++)
        {
            dpu_offload_status_t rc;
            dpu_offload_event_t *req_fwd_ev;
            remote_service_proc_info_t **list_sps = (remote_service_proc_info_t **)econtext->engine->service_procs.base;
            rc = send_cache_entry_request(econtext, list_sps[i]->ep, rank_info, &req_fwd_ev);
            CHECK_ERR_RETURN((rc), DO_ERROR, "send_cache_entry_request() failed");
            return DO_SUCCESS;
        }
    }

    return DO_ERROR;
}

/**
 * @brief receive handler for cache entry notifications. Note that a single notification
 * can hold multiple cache entries.
 * 
 * @param ev_sys Associated event channels/system
 * @param econtext Associated execution context
 * @param hdr Header of the notification
 * @param hdr_size Size of the header
 * @param data Notifaction's payload, i.e., the cache entries
 * @param data_len Total size of the notification's payload
 * @return dpu_offload_status_t 
 */
static dpu_offload_status_t peer_cache_entries_recv_cb(struct dpu_offload_ev_sys *ev_sys, execution_context_t *econtext, am_header_t *hdr, size_t hdr_size, void *data, size_t data_len)
{
    assert(econtext);
    assert(data);
    offloading_engine_t *engine = (offloading_engine_t *)econtext->engine;
    peer_cache_entry_t *entries = (peer_cache_entry_t *)data;
    size_t cur_size = 0;
    size_t idx = 0;
    int64_t group_id = INVALID_GROUP;
    int64_t group_rank, group_size;
    uint64_t dpu_global_id = LOCAL_ID_TO_GLOBAL(econtext, hdr->id);
    cache_t *cache = &(engine->procs_cache);
    size_t n_added = 0;
    // Make sure we know the group ID of what we receive otherwise we do not know what to do
    if (group_id == INVALID_GROUP)
        group_id = entries[idx].peer.proc_info.group_id;
    group_size = entries[idx].peer.proc_info.group_size;
    group_cache_t *gp_cache = GET_GROUP_CACHE(&(econtext->engine->procs_cache), group_id);
    while (cur_size < data_len)
    {
        // Now that we know for sure we have the group ID, we can move the received data into the local cache
        group_rank = entries[idx].peer.proc_info.group_rank;
        assert(entries[idx].peer.proc_info.group_id == group_id);
        assert(entries[idx].peer.proc_info.group_size == group_size);
        DBG("Received a cache entry for rank:%ld, group:%ld, group size:%ld, number of local rank: %ld from DPU %" PRId64 " (msg size=%ld, peer addr len=%ld)",
            group_rank, group_id, group_size, entries[idx].peer.proc_info.n_local_ranks, dpu_global_id, data_len, entries[idx].peer.addr_len);
        if (!is_in_cache(cache, group_id, group_rank, group_size))
        {
            size_t n;
            n_added++;
            gp_cache->num_local_entries++;
            peer_cache_entry_t *cache_entry;
            cache_entry = GET_GROUP_RANK_CACHE_ENTRY(cache, group_id, group_rank, group_size);
            cache_entry->set = true;
            COPY_PEER_DATA(&(entries[idx].peer), &(cache_entry->peer));
            assert(entries[idx].num_shadow_service_procs > 0);
            // append the shadow DPU data to the data already local available (if any)
            for (n = 0; n < entries[idx].num_shadow_service_procs; n++)
            {
                cache_entry->shadow_service_procs[cache_entry->num_shadow_service_procs + n] = entries[idx].shadow_service_procs[n];
            }
            cache_entry->num_shadow_service_procs += entries[idx].num_shadow_service_procs;

            // If any event is associated to the cache entry, handle them
            if (cache_entry->events_initialized)
            {

                while (!ucs_list_is_empty(&(cache_entry->events)))
                {
                    dpu_offload_event_t *e = ucs_list_extract_head(&(cache_entry->events), dpu_offload_event_t, item);
                    COMPLETE_EVENT(e);
                }
            }
        }
        else
        {
            DBG("Entry already in cache - gp: %ld, rank: %ld", group_id, group_rank);
        }
        cur_size += sizeof(peer_cache_entry_t);
        idx++;
    }

    // Once we handled all the cache entries we received, we check whether the cache is full and if so, send it to the local ranks
    DBG("The cache for group %ld now has %ld entries (group size: %ld)", group_id, gp_cache->num_local_entries, gp_cache->group_size);
    if (econtext->engine->on_dpu && n_added > 0)
    {
        // If all the ranks are on the local hosts, the case is handled in the callback that deals with the
        // final step of the connecting with the ranks.
        bool all_ranks_are_local = false;
        if (econtext->engine->config->num_service_procs_per_dpu == 1 && gp_cache->group_size == gp_cache->n_local_ranks)
            all_ranks_are_local = true;
        if (gp_cache->group_size > 0 && gp_cache->num_local_entries == gp_cache->group_size && !all_ranks_are_local)
        {
            execution_context_t *server = get_server_servicing_host(engine);
            assert(server->scope_id == SCOPE_HOST_DPU);
            dpu_offload_status_t rc = send_gp_cache_to_host(server, group_id);
            CHECK_ERR_RETURN((rc), DO_ERROR, "send_gp_cache_to_host() failed");
        }
        else
        {
            DBG("Cache is still missing some data. group_size: %ld, num_local_entries: %ld",
                gp_cache->group_size, gp_cache->num_local_entries);
        }
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

    rank_info_t *rank_info = (rank_info_t *)data;

    if (!is_in_cache(&(econtext->engine->procs_cache), rank_info->group_id, rank_info->group_rank, rank_info->group_size))
    {
        peer_cache_entry_t *cache_entry;
        cache_entry = GET_GROUP_RANK_CACHE_ENTRY(&(econtext->engine->procs_cache),
                                                 rank_info->group_id,
                                                 rank_info->group_rank,
                                                 rank_info->group_size);
        assert(cache_entry);
        COPY_RANK_INFO(rank_info, &(cache_entry->peer.proc_info));
        cache_entry->set = true;
    }

    return DO_SUCCESS;
}

static dpu_offload_status_t term_msg_cb(struct dpu_offload_ev_sys *ev_sys, execution_context_t *econtext, am_header_t *hdr, size_t hdr_size, void *data, size_t data_len)
{
    assert(econtext);
    // Termination messages never have a payload
    assert(data == NULL);
    assert(data_len == 0);
    assert(hdr);

    DBG("Recv'd a termination message from %" PRIu64, hdr->id);
    switch(econtext->type) {
        case CONTEXT_SERVER:
        {
            peer_info_t *client;
            client = DYN_ARRAY_GET_ELT(&(econtext->server->connected_clients.clients), hdr->id, peer_info_t);
            client->bootstrapping.phase = DISCONNECTED;
            econtext->server->connected_clients.num_connected_clients--;
            DBG("Remaining number of connected clients: %ld, ongoing connections: %ld",
                econtext->server->connected_clients.num_connected_clients,
                econtext->server->connected_clients.num_ongoing_connections);
            if (econtext->server->connected_clients.num_connected_clients == 0 && econtext->server->connected_clients.num_ongoing_connections == 0)
            {
                DBG("server is now done");
                econtext->server->done = true;
            }
            break;
        }
        case CONTEXT_CLIENT:
        {
            // The server sent us a termination message, we immediatly switch our state to done
            econtext->client->done = true;
            break;
        }
        default:
        {
            ERR_MSG("Recv'd a termination message on execution context of type %d", econtext->type);
            return DO_ERROR;
        }
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

    int rc = event_channel_register(ev_sys, AM_TERM_MSG_ID, term_msg_cb);
    CHECK_ERR_GOTO(rc, error_out, "cannot register handler to the term notification");
    
    rc = event_channel_register(ev_sys, AM_OP_START_MSG_ID, op_start_cb);
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

/****************************************/
/* Termination message util function(s) */
/****************************************/

dpu_offload_status_t send_term_msg(execution_context_t *ctx, dest_client_t *dest_info)
{
    dpu_offload_event_t *ev;
    dpu_offload_status_t rc;

    CHECK_ERR_RETURN((ctx == NULL), DO_ERROR, "undefined execution context");
    CHECK_ERR_RETURN((dest_info == NULL), DO_ERROR, "undefined destination");

    rc = event_get(ctx->event_channels, NULL, &ev);
    CHECK_ERR_RETURN((rc), DO_ERROR, "event_get() failed");
    assert(ev);
    ev->explicit_return = true;

    rc = event_channel_emit(&ev, AM_TERM_MSG_ID, dest_info->ep, dest_info->id, NULL);
    // We explicitly manage the event so the return code must be EVENT_INPROGRESS
    CHECK_ERR_RETURN((rc != EVENT_INPROGRESS), DO_ERROR, "event_channel_emit() failed");

    ctx->term.ev = ev;
    return DO_SUCCESS;
}
