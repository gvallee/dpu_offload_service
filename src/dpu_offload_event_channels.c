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
    } while (0)

#define DISPLAY_ONGOING_EVENTS_INFO(_e)                       \
    do                                                        \
    {                                                         \
        size_t _i;                                            \
        for (_i = 0; _i < (_e)->num_servers; _i++)            \
        {                                                     \
            DISPLAY_ECONTEXT_ONGOING_EVTS((_e)->servers[_i]); \
        }                                                     \
                                                              \
        if ((_e)->client)                                     \
        {                                                     \
            DISPLAY_ECONTEXT_ONGOING_EVTS((_e)->client);      \
        }                                                     \
    } while (0)

/*
 * Queue a message to add a group, typically because the group is being revoked.
 * Adding a group always requires for a revoke to complete so until it happens,
 * because everything is asynchronous, we queue and differ the group addition.
 * The library handles the queued group addition upon final deletion of the group
 * and therefore guarantee group consistency.
 */
#define QUEUE_PENDING_GROUP_ADD_MSG(_gp_cache, _client_id, _data, _data_len)        \
    do                                                                              \
    {                                                                               \
        pending_group_add_t *_pending_group_add = NULL;                             \
                                                                                    \
        /* Make the message persistent, we may deal a bunch of rank_info objects */ \
        DYN_LIST_GET((_gp_cache)->engine->pool_pending_recv_group_add,              \
                     pending_group_add_t,                                           \
                     item,                                                          \
                     _pending_group_add);                                           \
        assert(_pending_group_add);                                                 \
        RESET_PENDING_RECV_GROUP_ADD(_pending_group_add);                           \
        _pending_group_add->client_id = _client_id;                                 \
        _pending_group_add->data_len = data_len;                                    \
        _pending_group_add->data = malloc(data_len);                                \
        _pending_group_add->group_cache = (_gp_cache);                              \
        assert(_pending_group_add->data);                                           \
        memcpy(_pending_group_add->data, data, data_len);                           \
                                                                                    \
        /* Queue the pending msg */                                                 \
        DBG("Queuing pending add group msg (%p)", _pending_group_add);              \
        ucs_list_add_tail(&((_gp_cache)->persistent.pending_group_add_msgs),        \
                          &(_pending_group_add->item));                             \
    } while (0)

extern dpu_offload_status_t unpack_data_sps(offloading_engine_t *engine, void *data);

#if USE_AM_IMPLEM
dpu_offload_status_t get_associated_econtext(offloading_engine_t *engine, am_header_t *hdr, execution_context_t **econtext_out)
{
    execution_context_t *econtext = NULL;
    assert(hdr);
    assert(hdr->sender_type == CONTEXT_CLIENT || hdr->sender_type == CONTEXT_SERVER);

    if (hdr->scope_id == SCOPE_HOST_DPU)
    {
        // Message exchanged between a host and a service process
        if (engine->on_dpu)
        {
            econtext = engine->servers[engine->num_servers - 1];
        }
        else
        {
            econtext = engine->client;
        }
    }
    else
    {
        assert(engine->on_dpu);
        // Message exchanged between service processes
        switch (hdr->sender_type)
        {
        case CONTEXT_SERVER:
        {
            // The sender was a server
            econtext = CLIENT_SERVER_LOOKUP(engine, hdr->client_id, hdr->server_id);
            break;
        }
        case CONTEXT_CLIENT:
        {
            // The sender was a client
            assert(hdr->server_id == engine->config->local_service_proc.info.global_id);
            if (hdr->scope_id == SCOPE_INTER_SERVICE_PROCS)
            {
                assert(engine->servers[0] != NULL);
                econtext = engine->servers[0];
            }
            else
            {
                econtext = get_server_servicing_host(engine);
            }
            break;
        }
        default:
        {
            ERR_MSG("invalid sender type: %d", hdr->sender_type);
            goto error_out;
        }
        }
    }

    *econtext_out = econtext;
    return DO_SUCCESS;
error_out:
    *econtext_out = NULL;
    return DO_ERROR;
}

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
    assert(status == UCS_OK);
    pending_am_rdv_recv_t *recv_info = (pending_am_rdv_recv_t *)user_data;
    assert(recv_info);
    assert(recv_info->engine);
    // Find associated execution context
    execution_context_t *econtext = NULL;
    dpu_offload_status_t ret = get_associated_econtext(recv_info->engine, recv_info->hdr, &econtext);
    if (ret != DO_SUCCESS)
    {
        ERR_MSG("get_associated_econtext() failed");
        return;
    }
    assert(econtext);
    int rc = handle_notif_msg(econtext, recv_info->hdr, recv_info->hdr_len, recv_info->user_data, recv_info->payload_size);
    if (rc != UCS_OK)
    {
        ERR_MSG("handle_notif_msg() failed");
        return;
    }
    if (length > 0)
    {
        // If a payload was involved, we may need to return it to its memory pool or the buddy buffer system, or free it
        if (recv_info->pool.mem_pool != NULL && recv_info->pool.return_buf != NULL)
        {
            recv_info->pool.return_buf(recv_info->pool.mem_pool, recv_info->user_data);
            RESET_NOTIF_INFO(&(recv_info->pool));
        }
        else
        {
            if (recv_info->engine->settings.buddy_buffer_system_enabled)
            {
                SMART_BUFF_RETURN(&(recv_info->engine->smart_buffer_sys),
                                  recv_info->payload_size,
                                  recv_info->smart_chunk);
                recv_info->smart_chunk = NULL;
            }
            else
            {
                free(recv_info->user_data);
            }
            recv_info->user_data = NULL;
        }
    }
    if (request != NULL)
    {
        ucp_request_release(request);
    }
    ENGINE_LOCK(recv_info->engine);
    DYN_LIST_RETURN(recv_info->engine->free_pending_rdv_recv, recv_info, item);
    ENGINE_UNLOCK(recv_info->engine);
}

static ucs_status_t am_notification_recv_rdv_msg(offloading_engine_t *engine, am_header_t *hdr, size_t hdr_len, size_t payload_size, void *desc)
{
    ucp_request_param_t am_rndv_recv_request_params = {0};
    pending_am_rdv_recv_t *pending_recv;
    notification_callback_entry_t *entry;
    ENGINE_LOCK(econtext);
    if (engine->free_pending_rdv_recv == NULL && hdr->type == AM_PEER_CACHE_ENTRIES_MSG_ID)
    {
        // We received a cache entry notification but we are in the finalization phase, i.e., the list
        // of free pending RDV receive objects are not available.
        // It can happen when the application is using many communicators of different sizes:
        // some processes may receive cache entries so late that termination has been already triggered,
        // usually when the process is not involved in the communicator.
        // In such a case, we just safely drop the message.
        ENGINE_UNLOCK(econtext);
        return DO_SUCCESS;
    }
    DYN_LIST_GET(engine->free_pending_rdv_recv, pending_am_rdv_recv_t, item, pending_recv);
    ENGINE_UNLOCK(econtext);
    RESET_PENDING_RDV_RECV(pending_recv);
    assert(pending_recv);
    if (pending_recv->user_data != NULL)
        WARN_MSG("Warning payload buffer was not previously freed");

    // Find execution context associated with the message
    execution_context_t *econtext = NULL;
    dpu_offload_status_t rc = get_associated_econtext(engine, hdr, &econtext);
    if (rc != DO_SUCCESS)
    {
        ERR_MSG("get_associated_econtext() failed");
        return UCS_ERR_NO_MESSAGE;
    }
    assert(econtext);

    DBG("RDV message to be received for type %ld", hdr->type);
    entry = DYN_ARRAY_GET_ELT(&(econtext->event_channels->notification_callbacks), hdr->type, notification_callback_entry_t);
    assert(entry);

    if (entry->info.mem_pool)
    {
        void *buf_from_pool = get_notif_buf(econtext->event_channels, hdr->type);
        assert(buf_from_pool);
        pending_recv->user_data = buf_from_pool;
        pending_recv->buff_size = payload_size;
        COPY_NOTIF_INFO(&(entry->info), &(pending_recv->pool));
        am_rndv_recv_request_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                                                   UCP_OP_ATTR_FIELD_USER_DATA |
                                                   UCP_OP_ATTR_FIELD_DATATYPE |
                                                   UCP_OP_ATTR_FIELD_MEMORY_TYPE |
                                                   UCP_AM_RECV_ATTR_FLAG_DATA;
    }
    else
    {

        if (econtext->engine->settings.buddy_buffer_system_enabled)
        {
            pending_recv->smart_chunk = SMART_BUFF_GET(&(econtext->engine->smart_buffer_sys), payload_size);
            pending_recv->user_data = pending_recv->smart_chunk->base;
        }
        else
        {
            pending_recv->user_data = DPU_OFFLOAD_MALLOC(payload_size);
        }
        pending_recv->buff_size = payload_size;
        am_rndv_recv_request_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                                                   UCP_OP_ATTR_FIELD_USER_DATA |
                                                   UCP_OP_ATTR_FIELD_DATATYPE |
                                                   UCP_OP_ATTR_FIELD_MEMORY_TYPE |
                                                   UCP_AM_RECV_ATTR_FLAG_DATA;
    }
    assert(pending_recv->user_data);
    pending_recv->hdr = hdr;
    pending_recv->hdr_len = hdr_len;
    pending_recv->engine = econtext->engine;
    pending_recv->payload_size = payload_size;

    am_rndv_recv_request_params.cb.recv_am = &am_rdv_recv_cb;
    am_rndv_recv_request_params.datatype = ucp_dt_make_contig(1);
    am_rndv_recv_request_params.user_data = pending_recv;
    am_rndv_recv_request_params.memory_type = UCS_MEMORY_TYPE_HOST;

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
    if (arg == NULL || header == NULL)
    {
        DBG("arguments or header is NULL, skipping...");
        return UCS_OK;
    }
    assert(header != NULL);
    assert(header_length == sizeof(am_header_t));
    offloading_engine_t *engine = (offloading_engine_t *)arg;
    am_header_t *hdr = (am_header_t *)header;

    // We always try to receive the RDV message, even if the associated econtext is not
    // there any more so we can keep UCX happy and avoid resource leaks.
    if (param->recv_attr & UCP_AM_RECV_ATTR_FLAG_RNDV)
    {
        // RDV message
        am_notification_recv_rdv_msg(engine, hdr, header_length, length, data);
        return UCS_INPROGRESS;
    }

    // Find execution context associated with the message
    execution_context_t *econtext = NULL;
    dpu_offload_status_t rc = get_associated_econtext(engine, hdr, &econtext);
    if (rc != DO_SUCCESS)
    {
        ERR_MSG("get_associated_econtext() failed");
        return UCS_ERR_NO_MESSAGE;
    }

    assert(hdr);
    if (econtext == NULL)
    {
        if (hdr->type == AM_REVOKE_GP_RANK_MSG_ID || hdr->type == AM_REVOKE_GP_SP_MSG_ID)
        {
            // The execution context is NULL (fully terminated) and we just
            // received a group revoke message. It is not something that is
            // unexpected: when the world group is revoked during finalization,
            // the client may get terminated before we get the ACK (this notification).
            // We can safely drop the notification
            DBG("Safely dropping the group revoke notification since execution context is terminated");
            return UCS_OK;
        }
        if (hdr->type == AM_PEER_CACHE_ENTRIES_MSG_ID)
        {
            // The execution context is NULL (fully terminated) and we just received
            // a cache entry. It is nnot something that is unexpected: the creation of
            // group is now totally asynchronous, meaning group can be created and cache
            // entries sent around as the application is running. Meanwhile, the
            // application is free to terminate, group revoke being a local operation.
            // In such a case, we may receive a cache entry after the econtext terminated.
            DBG("Safely dropping the cache entry notification since execution context is terminated");
            return UCS_OK;
        }
    }
    assert(econtext);
    if (econtext->event_channels == NULL)
    {
        DBG("notification system not initialized (may be finalized), skipping...");
        return UCS_OK;
    }
    assert(hdr->type < econtext->event_channels->notification_callbacks.capacity);

    void *ptr = data;
    DBG("Notification of type %" PRIu64 " received via eager message (data size: %ld), dispatching...", hdr->type, length);
    if (length == 0)
    {
        // UCX AM layer has no problem setting the length to 0 but giving a data pointer that is not NULL
        ptr = NULL;
    }
    return handle_notif_msg(econtext, hdr, header_length, ptr, length);
}
#endif // USE_AM_IMPLEM

void *get_notif_buf(dpu_offload_ev_sys_t *ev_sys, uint64_t type)
{
    notification_callback_entry_t *entry;
    entry = DYN_ARRAY_GET_ELT(&(ev_sys->notification_callbacks), type, notification_callback_entry_t);
    if (entry == NULL || entry->info.get_buf == NULL)
        return NULL;
    assert(entry->info.mem_pool);
    return (entry->info.get_buf(entry->info.mem_pool, entry->info.get_buf_args));
}

notification_callback_entry_t *get_notif_callback_entry(dpu_offload_ev_sys_t *ev_sys, uint64_t type)
{
    notification_callback_entry_t *entry;
    entry = DYN_ARRAY_GET_ELT(&(ev_sys->notification_callbacks), type, notification_callback_entry_t);
    return entry;
}

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
#if OFFLOADING_MT_ENABLE
    event_channels->mutex = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
#endif
    *ev_channels = event_channels;
    return DO_SUCCESS;
}

dpu_offload_status_t event_channels_init(execution_context_t *econtext)
{
    CHECK_ERR_RETURN((econtext == NULL), DO_ERROR, "Undefined execution context");

    dpu_offload_status_t rc = ev_channels_init(&(econtext->event_channels));
    CHECK_ERR_RETURN((rc), DO_ERROR, "ev_channels_init() failed");

#if USE_AM_IMPLEM
    if (econtext->scope_id == SCOPE_SELF)
    {
        // For self, the notification system never relies on the UCX AM layer;
        // the handler is instead invoked while in the emit code path.
        return DO_SUCCESS;
    }

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
    am_param.arg = econtext->engine;
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

dpu_offload_status_t event_channel_update(dpu_offload_ev_sys_t *ev_sys, uint64_t type, notification_info_t *info)
{
    CHECK_ERR_RETURN((ev_sys == NULL), DO_ERROR, "undefined event system");
    CHECK_ERR_RETURN((info == NULL), DO_ERROR, "undefined info object");
    notification_callback_entry_t *entry = DYN_ARRAY_GET_ELT(&(ev_sys->notification_callbacks), type, notification_callback_entry_t);
    CHECK_ERR_RETURN((entry == NULL), DO_ERROR, "unable to get callback %ld", type);
    CHECK_ERR_RETURN((entry->set == false), DO_ERROR, "type %" PRIu64 " is not already set, unable to update", type);
    COPY_NOTIF_INFO(info, &(entry->info));
    return DO_SUCCESS;
}

dpu_offload_status_t event_channel_register(dpu_offload_ev_sys_t *ev_sys, uint64_t type, notification_cb cb, notification_info_t *info)
{
    CHECK_ERR_RETURN((cb == NULL), DO_ERROR, "Undefined callback");
    CHECK_ERR_RETURN((ev_sys == NULL), DO_ERROR, "undefined event system");
    notification_callback_entry_t *entry = DYN_ARRAY_GET_ELT(&(ev_sys->notification_callbacks), type, notification_callback_entry_t);
    CHECK_ERR_RETURN((entry == NULL), DO_ERROR, "unable to get callback %ld", type);
    if (entry->set == true)
    {
        DBG("Handler already registered, successfully return");
        return DO_SUCCESS;
    }
    RESET_NOTIF_CB_ENTRY(entry);
    entry->cb = cb;
    entry->set = true;
    entry->ev_sys = (struct dpu_offload_ev_sys *)ev_sys;
    if (info)
    {
        COPY_NOTIF_INFO(info, &(entry->info));
    }
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

dpu_offload_status_t mimosa_internal_event_register(offloading_engine_t *engine, uint64_t type, notification_cb cb)
{
    return event_channel_register(engine->self_econtext->event_channels, type, cb, NULL);
}

dpu_offload_status_t mimosa_internal_event_deregister(offloading_engine_t *engine, uint64_t type)
{
    return event_channel_deregister(engine->self_econtext->event_channels, type);
}

dpu_offload_status_t engine_update_default_notification_handler(offloading_engine_t *engine, uint64_t type, notification_info_t *info)
{
    if (info == NULL)
        return DO_SUCCESS;
    CHECK_ERR_RETURN((engine == NULL), DO_ERROR, "Undefine engine");
    ENGINE_LOCK(engine);
    notification_callback_entry_t *entry = DYN_ARRAY_GET_ELT(&(engine->default_notifications->notification_callbacks), type, notification_callback_entry_t);
    ENGINE_UNLOCK(engine);
    CHECK_ERR_RETURN((entry == NULL), DO_ERROR, "unable to get callback %ld", type);
    CHECK_ERR_RETURN((entry->set == false), DO_ERROR, "type %" PRIu64 " is not already set, unable to update", type);
    COPY_NOTIF_INFO(info, &(entry->info));
    return DO_SUCCESS;
}

dpu_offload_status_t engine_register_default_notification_handler(offloading_engine_t *engine, uint64_t type, notification_cb cb, notification_info_t *info)
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
    dpu_offload_status_t rc = event_channel_register(engine->self_econtext->event_channels, type, cb, info);
    CHECK_ERR_RETURN((rc), DO_ERROR, "event_channel_register() failed");

    entry->cb = cb;
    entry->set = true;
    entry->ev_sys = (struct dpu_offload_ev_sys *)engine->default_notifications;
    if (info)
    {
        if (info->mem_pool || info->get_buf || info->return_buf)
        {
            assert(info->mem_pool);
            assert(info->get_buf);
            // return_buf can be NULL when the calling library explicitly manages
            // life cycle of the buffer, especially when it is returned
            COPY_NOTIF_INFO(info, &(entry->info));
        }
    }
    engine->num_default_notifications++;
    return DO_SUCCESS;
}

dpu_offload_status_t event_channel_deregister(dpu_offload_ev_sys_t *ev_sys, uint64_t type)
{
    CHECK_ERR_RETURN((ev_sys == NULL), DO_ERROR, "undefined event system");
    CHECK_ERR_RETURN((ev_sys->notification_callbacks.capacity <= type), DO_ERROR, "type %" PRIu64 " is out of range", type);

    notification_callback_entry_t *list_callbacks = (notification_callback_entry_t *)ev_sys->notification_callbacks.base;
    notification_callback_entry_t *entry = &(list_callbacks[type]);
    CHECK_ERR_RETURN((entry->set == false), DO_ERROR, "type %" PRIu64 " is not registered", type);

    entry->set = false;
    return DO_SUCCESS;
}

static size_t num_completed_emit = 0;
#if USE_AM_IMPLEM
static void notification_emit_cb(void *request, ucs_status_t status, void *user_data)
{
    dpu_offload_event_t *ev;
    num_completed_emit++;
    DBG("New completion, %ld emit have now completed", num_completed_emit);
    if (user_data == NULL)
    {
        ERR_MSG("undefined event");
        return;
    }
    ev = (dpu_offload_event_t *)user_data;
    if (ev == NULL)
        return;
    DBG("ev=%p ctx=%p id=%" PRIu64, ev, &(ev->ctx), EVENT_HDR_SEQ_NUM(ev));
    COMPLETE_EVENT(ev);
    ucp_request_free(ev->req);
    ev->req = NULL;
    assert(ev->event_system);
    execution_context_t *econtext = ev->event_system->econtext;
    DBG("Associated econtext: %p", econtext);
    assert(econtext);
    DBG("evt %p now completed, %ld events left on the ongoing list", ev, ucs_list_length(&(econtext->ongoing_events)));
}
#else
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
    DBG("ev=%p ctx=%p id=%" PRIu64, ev, &(ev->ctx), EVENT_HDR_SEQ_NUM(ev));
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
#endif // !USE_AM_IMPLEM

#define PREP_EVENT_FOR_EMIT(__ev)                                         \
    do                                                                    \
    {                                                                     \
        execution_context_t *__econtext = (__ev)->event_system->econtext; \
        uint64_t __cid, __sid;                                            \
        switch ((__ev)->event_system->econtext->type)                     \
        {                                                                 \
        case CONTEXT_CLIENT:                                              \
            __cid = __econtext->client->id;                               \
            __sid = __econtext->client->server_id;                        \
            break;                                                        \
        case CONTEXT_SERVER:                                              \
            __sid = __econtext->server->id;                               \
            __cid = (__ev)->dest.id;                                      \
            break;                                                        \
        case CONTEXT_SELF:                                                \
            __cid = 0;                                                    \
            __sid = 0;                                                    \
            break;                                                        \
        default:                                                          \
            return DO_ERROR;                                              \
        }                                                                 \
        (__ev)->client_id = __cid;                                        \
        (__ev)->server_id = __sid;                                        \
        EVENT_HDR_SEQ_NUM(__ev) = (__ev)->seq_num;                        \
        EVENT_HDR_CLIENT_ID(__ev) = (__ev)->client_id;                    \
        EVENT_HDR_SERVER_ID(__ev) = (__ev)->server_id;                    \
    } while (0)

#if USE_AM_IMPLEM
uint64_t num_ev_sent = 0;
int do_am_send_event_msg(dpu_offload_event_t *event)
{
    ucp_request_param_t params;
    params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                          UCP_OP_ATTR_FIELD_DATATYPE |
                          UCP_OP_ATTR_FIELD_USER_DATA;
    params.datatype = ucp_dt_make_contig(1);
    params.user_data = event;
    params.cb.send = (ucp_send_nbx_callback_t)notification_emit_cb;
    assert(event->dest.ep);
    event->req = ucp_am_send_nbx(event->dest.ep,
                                 AM_EVENT_MSG_ID,
                                 &(event->ctx.hdr),
                                 sizeof(am_header_t),
                                 event->payload,
                                 EVENT_HDR_PAYLOAD_SIZE(event),
                                 &params);
    DBG("Event %p %" PRIu64 " successfully emitted", (event), EVENT_HDR_SEQ_NUM(event));
    num_ev_sent++;
    if (UCS_PTR_IS_ERR(event->req))
    {
        ERR_MSG("ucp_am_send_nbx() failed");
        return UCS_PTR_STATUS(event->req);
    }

    if (event->req == NULL)
    {
        event->ctx.complete = true;
        return EVENT_DONE;
    }

    DBG("ucp_am_send_nbx() did not completed right away (ev %p %" PRIu64 ")", event, EVENT_HDR_SEQ_NUM(event));
    SYS_EVENT_LOCK(event->event_system);
    event->event_system->posted_sends++;
    SYS_EVENT_UNLOCK(event->event_system);
    event->was_posted = true;
    return EVENT_INPROGRESS;
}

int am_send_event_msg(dpu_offload_event_t **event)
{
    int rc;
    PREP_EVENT_FOR_EMIT(*event);
    (*event)->ctx.hdr.scope_id = (*event)->scope_id;
    rc = do_am_send_event_msg(*event);
    if ((rc == EVENT_DONE || rc == EVENT_INPROGRESS) && (*event)->req == NULL)
    {
        // No error
        if ((*event)->explicit_return == false)
        {
            // Immediate completion, the callback is *not* invoked
            DBG("ucp_am_send_nbx() completed right away");
            dpu_offload_status_t rc = event_return(event);
            CHECK_ERR_RETURN((rc), DO_ERROR, "event_return() failed");
            return EVENT_DONE;
        }
        else
        {
            COMPLETE_EVENT(*event);
            return EVENT_INPROGRESS;
        }
    }

    return rc;
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
        DBG("Sending notification header - ev: %" PRIu64 ", type: %" PRIu64 ", econtext: %p, scope_id: %d, client_id: %" PRIu64 ", server_id: %" PRIu64,
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
    execution_context_t *econtext = (*event)->event_system->econtext;
    assert((*event)->dest.ep);
    assert((*event)->event_system);
    assert((*event)->event_system->econtext);
    PREP_EVENT_FOR_EMIT(*event);
    if (econtext->type == CONTEXT_CLIENT && econtext->scope_id == SCOPE_HOST_DPU && econtext->rank.n_local_ranks > 0 && econtext->rank.n_local_ranks != UINT64_MAX)
    {
        assert((*event)->client_id < econtext->rank.n_local_ranks);
    }

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
    assert((*event));
    // This function can only be used when the user is managing the payload
    assert((*event)->manage_payload_buf == false);

    // Try to progress the sends before adding another one; except for subevent to avoid recurring calls
    // to progress_econtext_sends() which could complete the meta event too early
    if (!(*event)->is_subevent)
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

    assert(dest_ep);
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
    DBG("Sending event %p of type %" PRIu64 " (payload size: %ld)", *event, type, EVENT_HDR_PAYLOAD_SIZE(*event));

    // Try to progress the sends before adding another one; except for subevent to avoid recurring calls
    // to progress_econtext_sends() which could complete the meta-event too early
    if (!(*event)->is_subevent)
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
        WARN_MSG("%ld events objects have not been returned (event system %p; econtext: %p)",
                 (*ev_sys)->num_used_evs, (*ev_sys), (*ev_sys)->econtext);
        DISPLAY_ECONTEXT_ONGOING_EVTS((*ev_sys)->econtext);
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
#if USE_AM_IMPLEM
        _ev->ctx.hdr.sender_type = ev_sys->econtext->type;
#endif

        if (info != NULL)
        {
            _ev->explicit_return = info->explicit_return;
        }

        if (info == NULL || (info->pool.mem_pool == NULL && info->payload_size == 0))
        {
            // The library does not need to do anything for the payload buffer
            goto out;
        }

        // If we get here, it means that we need to manage a payload buffer for that event
        _ev->manage_payload_buf = true;
        EVENT_HDR_PAYLOAD_SIZE(_ev) = info->payload_size;
        if (info != NULL && info->pool.mem_pool != NULL)
        {
            assert(info->pool.get_buf);
            void *payload_buf_from_pool = info->pool.get_buf(info->pool.mem_pool,
                                                             info->pool.get_buf_args);
            assert(payload_buf_from_pool);
            _ev->payload = payload_buf_from_pool;
            _ev->info.mem_pool = info->pool.mem_pool;
            _ev->info.get_buf = info->pool.get_buf;
            _ev->info.return_buf = info->pool.return_buf;
            EVENT_HDR_PAYLOAD_SIZE(_ev) = info->pool.element_size;
        }
        else
        {
            _ev->payload = DPU_OFFLOAD_MALLOC(info->payload_size);
        }
        assert(_ev->payload);
    }

out:
#if USE_AM_IMPLEM
    DBG("Got event #%" PRIu64 " (%p) from list %p (payload_size: %ld)",
        _ev->seq_num, _ev, ev_sys->free_evs, EVENT_HDR_PAYLOAD_SIZE(_ev));
#else
    DBG("Got event #%" PRIu64 " (%p) from list %p (scope_id: %d, payload_size: %ld)",
        _ev->seq_num, _ev, ev_sys->free_evs, _ev->scope_id, EVENT_HDR_PAYLOAD_SIZE(_ev));
#endif
    *ev = _ev;
    return DO_SUCCESS;
}

static dpu_offload_status_t do_event_return(dpu_offload_event_t *ev)
{
    assert(ev);
    assert(ev->event_system);
#if !NDEBUG && !USE_AM_IMPLEM
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

    // Note that the type can be equal to UINT64_MAX since it is perfectly okay
    // to get a new event and return it without using it.
    assert(EVENT_HDR_TYPE(ev) > 0);

#if !NDEBUG
    if (EVENT_HDR_TYPE(ev) == META_EVENT_TYPE)
    {
        assert(!ev->sub_events_initialized || ucs_list_is_empty(&(ev->sub_events)));
    }
#endif

    // If the event has a payload buffer that the library is managing, free that buffer
    if (ev->manage_payload_buf && ev->payload != NULL)
    {
        if (ev->info.mem_pool == NULL)
        {
            // Buffer is managed by the library
            free(ev->payload);
        }
        else
        {
            // If a memory pool is specified by the return_buf() function pointer
            // is NULL, it means the calling library will return the buffer to
            // the pool, there is nothing to do.
            if (ev->info.return_buf != NULL)
            {
                // Return the buffer to the pool from calling library
                assert(ev->info.mem_pool);
                ev->info.return_buf(ev->info.mem_pool, ev->payload);
            }
        }
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
    {
        goto event_completed;
    }
#else
    if (EVENT_HDR_TYPE(ev) != META_EVENT_TYPE && ev->ctx.hdr_completed == true && ev->ctx.payload_completed == true)
        goto event_completed;
#endif

    assert(ev->event_system);
    // Update the list of sub-event by removing the completed ones
    if (EVENT_HDR_TYPE(ev) == META_EVENT_TYPE)
    {
        if (ev->sub_events_initialized)
        {
            dpu_offload_event_t *subevt = NULL, *next = NULL;
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
                    DBG("returning sub event %" PRIu64 " %p of meta event %p", subevt->seq_num, subevt, ev);
                    dpu_offload_status_t rc = do_event_return(subevt);
                    CHECK_ERR_RETURN((rc), DO_ERROR, "event_return() failed");
                }
            }
        }

        if (!ev->sub_events_initialized || ucs_list_is_empty(&(ev->sub_events)))
        {
            DBG("All sub-events completed, metaev %p %ld completes", ev, ev->seq_num);
            goto event_completed;
        }
    }
    else
    {
#if USE_AM_IMPLEM
        if (ev->ctx.complete)
#else
        if (ev->ctx.hdr_completed == true && ev->ctx.payload_completed == true)
#endif
            goto event_completed;
    }
    return false;

event_completed:
    DBG("Event %p (#%ld) is completed", ev, EVENT_HDR_SEQ_NUM(ev));
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

    DBG("Cache entry requested received for gp/rank 0x%x/%" PRIu64, rank_info->group_uid, rank_info->group_rank);

    if (is_in_cache(&(econtext->engine->procs_cache), rank_info->group_uid, rank_info->group_rank, rank_info->group_size))
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
        rc = send_group_cache(econtext, peer_info->ep, peer_info->id, rank_info->group_uid, send_cache_ev);
        CHECK_ERR_RETURN((rc), DO_ERROR, "send_group_cache() failed");
        if (!event_completed(send_cache_ev))
            QUEUE_EVENT(send_cache_ev);
        else
            event_return(&send_cache_ev);
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
 * @brief A little cleverness is required to know if a group is ready to add new cache entries or not.
 * For a sub-communicator, a group is not ready when we receive cache entries while the cache has been revoked.
 * Practically, it happens in two different situations, the group's sequence number, i.e., its version,
 * does not match the one in the cache entry that we just received. In that case, the received cache entries
 * are queued. When the group is successfully globally revoked, we process pending received cache entries.
 * This also means that the first communicator is a special case since it is never revoked, while we may
 * receive cache entries from other SPs. In such a case, the group's sequence number is equal to zero (not
 * yet initialized) and of course does not match the one from the received message.
 */
static dpu_offload_status_t is_group_ready(group_cache_t *gp_cache, uint64_t recvd_seq_num, bool *group_ready)
{
    assert(gp_cache);
    assert(group_ready);

    *group_ready = true;
    if (gp_cache->persistent.num > 0)
    {
        /* Sub-communicator */

        // It is absolutely possible to receive cache entries for a group that has been locally revoked.
        // It is also not possible to handle cache entries if the group's sequence number does not match,
        // meaning we are late compared to other SPs which already handle a newer instantiation of a group.
        if (gp_cache->persistent.num == recvd_seq_num + 1 && gp_cache->num_local_entries == 0)
        {
            // First time we know about this version of the group, we can deal with the entries.
            // handle_peer_cache_entries_recv will update the group's sequence number, a.k.a., version.
            *group_ready = false;
        }
        else if (gp_cache->persistent.num != recvd_seq_num)
        {
            // Different sequence number than the current one for the group.
            *group_ready = false;
        }
    }
#if !NDEBUG
    else
    {
        if (gp_cache->persistent.num != recvd_seq_num)
        {
            // If this is the first communicator, if the seq number mismatched, the local cache MUST be empty
            assert(gp_cache->n_local_ranks_populated == 0);
        }
    }
#endif
    return DO_SUCCESS;
}

/**
 * @brief receive handler for cache entry notifications. Note that a single notification
 * can hold multiple cache entries. An SP also sends all the cache entries it holds at
 * the time it initiates the send, i.e., the "local" ranks, as well as all the entries
 * it may have received
 *
 * @param ev_sys Associated event channels/system
 * @param econtext Associated execution context
 * @param hdr Header of the notification
 * @param hdr_size Size of the header
 * @param data Notifaction's payload, i.e., the cache entries
 * @param data_len Total size of the notification's payload
 * @return dpu_offload_status_t
 */
static dpu_offload_status_t peer_cache_entries_recv_cb(struct dpu_offload_ev_sys *ev_sys,
                                                       execution_context_t *econtext,
                                                       am_header_t *hdr,
                                                       size_t hdr_size,
                                                       void *data,
                                                       size_t data_len)
{
    offloading_engine_t *engine = NULL;
    peer_cache_entry_t *entries = NULL;
    int64_t group_size;
    uint64_t sp_global_id = UINT64_MAX;
    int group_uid;
    dpu_offload_status_t rc;
    group_cache_t *gp_cache = NULL;
    bool group_ready = true;

    assert(econtext);
    assert(data);
    engine = (offloading_engine_t *)econtext->engine;
    assert(engine);
    sp_global_id = LOCAL_ID_TO_GLOBAL(econtext, hdr->id);
    entries = (peer_cache_entry_t *)data;
    group_uid = entries[0].peer.proc_info.group_uid;
    group_size = entries[0].peer.proc_info.group_size;
    gp_cache = GET_GROUP_CACHE_INTERNAL(&(econtext->engine->procs_cache), group_uid, group_size);
    assert(gp_cache);
#if !NDEBUG
    {
        DBG("Receive cache entry for group 0x%x (seq num: %ld) from SP %" PRIu64 ", ev: %" PRIu64,
            group_uid, entries[0].peer.proc_info.group_seq_num, sp_global_id, hdr->event_id);
    }
#endif

    assert(entries[0].peer.proc_info.group_seq_num >= gp_cache->persistent.num);

    rc = is_group_ready(gp_cache, entries[0].peer.proc_info.group_seq_num, &group_ready);
    CHECK_ERR_RETURN((rc != DO_SUCCESS), DO_ERROR, "is_group_ready() failed");


    if (gp_cache->revokes.global > 0 || !group_ready)
    {
        // We are in the situation where we are still waiting for revoke messages from other
        // SPs or waiting to complete the final revoked with local ranks and already receiving
        // cache entries from other SPs that are ahead are already in the process of repopulating
        // a group with the same UID.
        // We therefore queue the cache entry; the queued cache entry will be handled once
        // the group is fully revoked.
        pending_recv_cache_entry_t *pending_recv = NULL;

        DBG("Queuing cache entry for group 0x%x (seq num: %ld) being revoked; local revokes: %ld out of %ld ranks (msg from %" PRIu64 ")",
            gp_cache->group_uid, gp_cache->persistent.num, gp_cache->revokes.local, gp_cache->sp_ranks, sp_global_id);
        DYN_LIST_GET(engine->pool_pending_recv_cache_entries,
                     pending_recv_cache_entry_t,
                     item,
                     pending_recv);
        assert(pending_recv);
        RESET_PENDING_RECV_CACHE_ENTRY(pending_recv);
        pending_recv->gp_uid = group_uid;
        pending_recv->econtext = econtext;
        pending_recv->sp_gid = sp_global_id;
        // Make the payload persistent
        pending_recv->payload = malloc(data_len);
        assert(pending_recv->payload);
        memcpy(pending_recv->payload, data, data_len);
        pending_recv->payload_size = data_len;
        ucs_list_add_tail(&(gp_cache->persistent.pending_recv_cache_entries), &(pending_recv->item));
        return DO_SUCCESS;
    }

    rc = handle_peer_cache_entries_recv(econtext, sp_global_id, data, data_len);
    CHECK_ERR_RETURN((rc != DO_SUCCESS), DO_ERROR, "handle_peer_cache_entries_recv() failed");
    return DO_SUCCESS;
}

static bool rank_is_on_sp(int64_t world_rank, offloading_engine_t *engine)
{
    group_cache_t *c = GET_GROUP_CACHE(&(engine->procs_cache), engine->procs_cache.world_group);
    peer_cache_entry_t *cache_entry = DYN_ARRAY_GET_ELT(&(c->ranks), world_rank, peer_cache_entry_t);
    assert(cache_entry);
    if (cache_entry->shadow_service_procs[0] == engine->config->local_service_proc.info.global_id)
    {
        DBG("World rank %ld is on SP %" PRIu64 "\n", world_rank, cache_entry->shadow_service_procs[0]);
        return true;
    }
    return false;
}

void revoke_send_to_host_cb(void *context)
{
    group_cache_t *gp_cache = NULL;
    dpu_offload_status_t rc;
    offloading_engine_t *engine = NULL;

    assert(context);
    gp_cache = (group_cache_t*) context;
    DBG("group cache revoke for 0x%x (seq num: %ld) sent to ranks (size: %ld)",
        gp_cache->group_uid, gp_cache->persistent.num, gp_cache->group_size);
    assert(gp_cache->engine);
    engine = gp_cache->engine; // We cache the engine handle before resetting the group cache since the group's revoke will set it to NULL
    gp_cache->persistent.revoke_sent_to_host = gp_cache->persistent.revoke_send_to_host_posted;
    // A revoke cannot complete if the cache was not sent to local ranks first
    assert(gp_cache->persistent.sent_to_host == gp_cache->persistent.num);
    rc = revoke_group_cache(gp_cache->engine, gp_cache->group_uid);
    if (rc != DO_SUCCESS)
        ERR_MSG("revoke_group_cache() failed (rc: %d)", rc);
    // We make sure that we handle the pending group add for the group that was just revoked.
    // First, re-set the engine to make sure handle_pending_group_cache_add_msgs can be properly executed;
    // we know because of our context that the same engine is used.
    gp_cache->engine = engine;
    rc = handle_pending_group_cache_add_msgs(gp_cache);
    if (rc != DO_SUCCESS)
        ERR_MSG("handle_pending_group_cache_add_msgs() failed (rc: %d)", rc);
}

dpu_offload_status_t send_revoke_group_to_ranks(offloading_engine_t *engine, group_uid_t gp_uid, uint64_t num_ranks)
{
    dpu_offload_status_t rc;
    size_t n = 0, idx = 0;
    execution_context_t *host_server = get_server_servicing_host(engine);
    group_cache_t *gp_cache = NULL;
    dpu_offload_event_t *metaev = NULL;

    assert(host_server);
    assert(num_ranks);
    gp_cache = GET_GROUP_CACHE(&(engine->procs_cache), gp_uid);
    assert(gp_cache);
    assert(gp_cache->persistent.revoke_send_to_host_posted == gp_cache->persistent.num - 1);

    rc = event_get(host_server->event_channels, NULL, &metaev);
    CHECK_ERR_RETURN((rc), DO_ERROR, "event_get() failed");
    assert(metaev);
    EVENT_HDR_TYPE(metaev) = META_EVENT_TYPE;
    metaev->ctx.completion_cb = revoke_send_to_host_cb;
    metaev->ctx.completion_cb_ctx = gp_cache;

    DBG("Sending final revoke to %ld local ranks (group UID: 0x%x, size: %ld, group seq num: %ld, ev: %p, cb: %p)",
        host_server->server->connected_clients.num_connected_clients,
        gp_uid,
        gp_cache->group_size,
        gp_cache->persistent.num,
        metaev,
        metaev->ctx.completion_cb);

    while (n < host_server->server->connected_clients.num_connected_clients)
    {
        peer_info_t *c = NULL;
        c = DYN_ARRAY_GET_ELT(&(host_server->server->connected_clients.clients),
                              idx, peer_info_t);
        if (c == NULL)
        {
            idx++;
            continue;
        }
        assert(c->rank_data.group_uid != INT_MAX);

        rc = send_revoke_group_rank_request_through_list_ranks(host_server, c->ep, c->id, gp_uid, num_ranks, metaev);
        CHECK_ERR_RETURN((rc), DO_ERROR, "send_revoke_group_rank_request_through_list_ranks() failed");
        n++;
        idx++;
    }
    gp_cache->persistent.revoke_send_to_host_posted = gp_cache->persistent.num;
    DBG("Final revoke messages for group 0x%x to ranks posted (seq num: %ld, meta-ev: %p, cb: %p)",
        gp_uid, gp_cache->persistent.num, metaev, metaev->ctx.completion_cb);

    if (!event_completed(metaev))
    {
        QUEUE_EVENT(metaev);
    }
    else
    {
        event_return(&metaev);
    }

    return DO_SUCCESS;
}

static dpu_offload_status_t do_add_group_rank_recv_cb(offloading_engine_t *engine, uint64_t client_id, void *data, size_t data_len)
{
    size_t cur_size = 0;
    rank_info_t *rank_info = NULL;
    
    assert(data);
    assert(engine);
    rank_info = (rank_info_t *)data;
    assert(rank_info->group_seq_num);

    execution_context_t *service_server = get_server_servicing_host(engine);
    assert(service_server);

    // we check on the first element, all the data is supposed to be ranks from the same group
    if (rank_info->group_uid == INT_MAX ||
        rank_info->group_rank == INVALID_RANK)
    {
        // The data we received really does not include any usable group data
        return DO_SUCCESS;
    }

    DBG("Recv'd data about group 0x%x, rank: %" PRId64 ", group size: %" PRId64 ", group seq num: %ld",
        rank_info->group_uid,
        rank_info->group_rank,
        rank_info->group_size,
        rank_info->group_seq_num);
    group_cache_t *gp_cache = GET_GROUP_CACHE_INTERNAL(&(engine->procs_cache),
                                                       rank_info->group_uid,
                                                       rank_info->group_size);
    assert(gp_cache);
    assert(rank_info->group_seq_num >= gp_cache->persistent.num);

    if (gp_cache->revokes.global > 0)
    {
        DBG("Queuing group add msg (UID: 0x%x, local seq num: %ld, add seq num: %ld)",
            gp_cache->group_uid, gp_cache->persistent.num, rank_info->group_seq_num);
        QUEUE_PENDING_GROUP_ADD_MSG(gp_cache, client_id, data, data_len);
        return DO_SUCCESS;
    }

    if (!is_in_cache(&(engine->procs_cache), rank_info->group_uid, rank_info->group_rank, rank_info->group_size))
    {
        peer_cache_entry_t *cache_entry = NULL;
        execution_context_t *host_server = NULL;
        peer_info_t *client_info = NULL;
        dpu_offload_status_t rc;

        if (gp_cache->num_local_entries == 0)
        {
            // First time a rank notifies us about a new group
            gp_cache->persistent.num++;
            DBG("Switched to seq num: %ld for group 0x%x", gp_cache->persistent.num, gp_cache->group_uid);
        }

        host_server = get_server_servicing_host(engine);
        assert(host_server);
        client_info = DYN_ARRAY_GET_ELT(&(host_server->server->connected_clients.clients),
                                        client_id,
                                        peer_info_t);
        assert(client_info);
        cache_entry = GET_GROUP_RANK_CACHE_ENTRY(&(engine->procs_cache),
                                                 rank_info->group_uid,
                                                 rank_info->group_rank,
                                                 rank_info->group_size);
        assert(cache_entry);
        cache_entry->peer.addr_len = client_info->peer_addr_len;
        memcpy(cache_entry->peer.addr,
               client_info->peer_addr,
               client_info->peer_addr_len);
        COPY_RANK_INFO(rank_info, &(cache_entry->peer.proc_info));
        cache_entry->num_shadow_service_procs = 1;
        cache_entry->shadow_service_procs[0] = engine->config->local_service_proc.info.global_id;
        cache_entry->client_id = client_id;
        cache_entry->peer.host_info = rank_info->host_info;
        cache_entry->set = true;

        if (gp_cache->group_uid == 0)
            gp_cache->group_uid = rank_info->group_uid;
        if (gp_cache->n_local_ranks <= 0 && rank_info->n_local_ranks >= 0)
        {
            gp_cache->n_local_ranks = rank_info->n_local_ranks;
        }
        gp_cache->n_local_ranks_populated++;
        gp_cache->num_local_entries++;
        DBG("group 0x%x (seq num: %ld) now has %ld entries, %ld local ranks, %ld being populated",
            gp_cache->group_uid,
            gp_cache->persistent.num,
            gp_cache->num_local_entries,
            gp_cache->n_local_ranks,
            gp_cache->n_local_ranks_populated);

        // Unpack the mapping of all the ranks
        int64_t rank = 0;
        size_t ranks_attached_to_sp = 0;
        int64_t *ptr = (int64_t *)((ptrdiff_t)data + sizeof(rank_info_t));
        cur_size = sizeof(rank_info_t);
        while (cur_size < data_len)
        {
            if (gp_cache->sp_ranks == 0)
            {
                DBG("Rank %" PRId64 " is rank %" PRId64 " on comm world\n", rank, *ptr);
                if (rank_is_on_sp(*ptr, engine))
                    ranks_attached_to_sp++;
            }
            rank++;
            cur_size += sizeof(int64_t);
            ptr = (int64_t *)((ptrdiff_t)ptr + sizeof(int64_t));
        }

        if (gp_cache->sp_ranks == 0)
        {
            gp_cache->sp_ranks = ranks_attached_to_sp;
            DBG("%ld/%ld ranks of group 0x%x are expected on SP #%" PRIu64,
                gp_cache->sp_ranks,
                gp_cache->group_size,
                rank_info->group_uid,
                engine->config->local_service_proc.info.global_id);
        }

        // Update the topology
        rc = update_topology_data(engine,
                                  gp_cache,
                                  rank_info->group_rank,
                                  engine->config->local_service_proc.info.global_id,
                                  engine->config->local_service_proc.host_uid);
        CHECK_ERR_RETURN((rc), DO_ERROR, "update_topology_data() failed");
    }

    HANDLE_PENDING_CACHE_ENTRIES(gp_cache);
    GROUP_CACHE_EXCHANGE(engine, rank_info->group_uid, gp_cache->sp_ranks);
    return DO_SUCCESS;
}

dpu_offload_status_t handle_pending_group_cache_add_msgs(group_cache_t *group_cache)
{
    dpu_offload_status_t rc;
    pending_group_add_t *pending_group_add, *next_pending;
    assert(group_cache);
    assert(group_cache->engine);

    if (ucs_list_is_empty(&(group_cache->persistent.pending_group_add_msgs)))
    {
        return DO_SUCCESS;
    }

    ucs_list_for_each_safe(pending_group_add, next_pending, &(group_cache->persistent.pending_group_add_msgs), item)
    {
        DBG("Handling pending group add %p for group cache 0x%x (seq num: %ld)",
            pending_group_add, group_cache->group_uid, group_cache->persistent.num);
        ucs_list_del(&(pending_group_add->item));
        rc = do_add_group_rank_recv_cb(group_cache->engine,
                                       pending_group_add->client_id,
                                       pending_group_add->data,
                                       pending_group_add->data_len);
        CHECK_ERR_RETURN((rc != DO_SUCCESS), DO_ERROR, "do_add_group_rank_recv_cb() failed");
        free(pending_group_add->data);
        pending_group_add->data = NULL;
        pending_group_add->data_len = 0;
        DYN_LIST_RETURN(group_cache->engine->pool_pending_recv_group_add, pending_group_add, item);
    }

    return DO_SUCCESS;
}

// Note: function invoked when we receive revokes from other SPs or on the host when receiving the final
// revoke message from the SP.
static dpu_offload_status_t handle_revoke_group_rank_through_list_ranks(execution_context_t *econtext,
                                                                        group_revoke_msg_from_sp_t *revoke_msg)
{
    dpu_offload_status_t rc;
    group_cache_t *gp_cache = NULL;
    gp_cache = GET_GROUP_CACHE_INTERNAL(&(econtext->engine->procs_cache), revoke_msg->gp_uid, revoke_msg->group_size);
    assert(gp_cache);
    assert(gp_cache->revokes.global <= gp_cache->group_size);
#if !NDEBUG
    if (gp_cache->group_size == 0)
    {
        if (econtext->engine->on_dpu)
        {
            // It is possible that the group was not known if there is no rank from the group associated to the
            // SP. We check that everything is consistent in that context.
            assert(gp_cache->sp_ranks == 0);
            assert(revoke_msg->group_size);
        }
        else
        {
            // On the host, the group size can be equal to 0 only if there is no rank locally
            if (gp_cache->group_size == 0)
            {
                assert(gp_cache->n_local_ranks == 0);
            }
        }
        gp_cache->group_size = revoke_msg->group_size;
    }
#endif // NDEBUG
    assert(revoke_msg->gp_seq_num == gp_cache->persistent.num);
    if (econtext->engine->on_dpu)
    {
        DBG("Revoke msg received from another SP (seq num: %ld), local data: n_local_ranks=%ld n_local_ranks_populated=%ld group_size=%ld",
            revoke_msg->gp_seq_num, gp_cache->n_local_ranks, gp_cache->n_local_ranks_populated, gp_cache->group_size);

        // If the group cache is not fully populated yet, we delay the revoke.
        // The delay allows us to make sure we can keep a consistent cache even when
        // the same group UID is re-used.
        // Similarily, if all the local ranks did not yet revoke the group, the revoke
        // received from other SPs is delayed.
        if (!gp_cache->initialized ||
            gp_cache->group_size != gp_cache->num_local_entries || /* Is the cache fully populated? Do not use group_cache_populated to ignore if group locally revoked */
            gp_cache->sp_ranks != gp_cache->revokes.local) /* Did all local ranks revoke the group? */
        {
            group_revoke_msg_from_sp_t *pending_msg = NULL;

            // Make the message persistent
            DBG("Queuing revoke msg from another SP (group seq num: %ld, n_local_ranks: %ld, local_revoked: %ld, group ID: 0x%x, ranks for SP: %ld)",
                gp_cache->persistent.num, gp_cache->n_local_ranks, gp_cache->revokes.local, revoke_msg->gp_uid, gp_cache->sp_ranks);
            assert(gp_cache->persistent.num == revoke_msg->gp_seq_num);
            DYN_LIST_GET(econtext->engine->pool_group_revoke_msgs_from_sps,
                         group_revoke_msg_from_sp_t,
                         item,
                         pending_msg);
            pending_msg->num_ranks = revoke_msg->num_ranks;
            pending_msg->gp_uid = revoke_msg->gp_uid;
            pending_msg->rank_start = revoke_msg->rank_start;
            pending_msg->group_size = revoke_msg->group_size;
            pending_msg->gp_seq_num = revoke_msg->gp_seq_num;
            memcpy(pending_msg->ranks, revoke_msg->ranks, revoke_msg->num_ranks * sizeof(int));
            // Queue the new pending message
            ucs_list_add_tail(&(gp_cache->persistent.pending_group_revoke_msgs_from_sps), &(pending_msg->item));
        }
        else
        {
            size_t new_revokes = 0;
            size_t rank;
            DBG("Handling the revoke message from another SP (UID: 0x%x, seq num: %ld, num_ranks: %ld, rank_start: %ld, local revokes: %ld, global revokes: %ld)",
                gp_cache->group_uid,
                revoke_msg->gp_seq_num,
                revoke_msg->num_ranks,
                revoke_msg->rank_start,
                gp_cache->revokes.local,
                gp_cache->revokes.global);
            if (gp_cache->group_size == 0)
            {
                // First time we hear about the group and because of the lazy initialization,
                // we need to initialize a few more things based on the revoke message's data.
                gp_cache->group_size = revoke_msg->group_size;
                assert(gp_cache->revokes.ranks == NULL);
                GROUP_CACHE_BITSET_CREATE(gp_cache->revokes.ranks, gp_cache->group_size);
            }
            assert(gp_cache->revokes.ranks);
            assert(gp_cache->revokes.global <= gp_cache->group_size);

            // Go through the list of ranks from the message that revoked the group
            for (rank = 0; rank < revoke_msg->num_ranks; rank++)
            {
                assert(revoke_msg->rank_start + rank <= gp_cache->group_size);
                if (revoke_msg->ranks[rank] == 1 && !GROUP_CACHE_BITSET_TEST(gp_cache->revokes.ranks, revoke_msg->rank_start + rank))
                {
                    DBG("Marking rank %ld as revoked (group seq num: %ld)",
                        revoke_msg->rank_start + rank, revoke_msg->gp_seq_num);
                    GROUP_CACHE_BITSET_SET(gp_cache->revokes.ranks, revoke_msg->rank_start + rank);
                    new_revokes++;
                }
            }

            gp_cache->revokes.global += new_revokes;
            assert(gp_cache->revokes.global <= gp_cache->group_size);
            // If we get new revokes and all the revokes are there, we sent the final revoke to the local ranks
            if (new_revokes > 0 &&
                gp_cache->revokes.global == gp_cache->group_size &&
                gp_cache->persistent.revoke_send_to_host_posted < gp_cache->persistent.num)
            {
                group_uid_t gpuid = revoke_msg->gp_uid;
                size_t group_size = gp_cache->group_size; // Get the number of ranks involved in the revoke before resetting the group cache
                assert(group_size);
                DBG("Sending revoke message to ranks for group 0x%x (size=%ld)", gp_cache->group_uid, group_size);
                rc = send_revoke_group_to_ranks(econtext->engine, gpuid, group_size);
                CHECK_ERR_RETURN((rc != DO_SUCCESS), DO_ERROR, "send_revoke_group_to_ranks() failed");
            }
        }
    }
    else
    {
        // On the host
        pending_send_group_add_t *pending_send = NULL, *next_pending = NULL;

        DBG("Received final revoke for group 0x%x (size: %ld, seq num: %ld)",
            revoke_msg->gp_uid, revoke_msg->group_size, revoke_msg->gp_seq_num);
        assert(revoke_msg->group_size);
        assert(revoke_msg->gp_seq_num == gp_cache->persistent.num);

        // We got a revoke message from our service process so the group is fully revoked, we can reset it
        rc = revoke_group_cache(econtext->engine, revoke_msg->gp_uid);
        CHECK_ERR_RETURN((rc != DO_SUCCESS), DO_ERROR, "revoke_group_cache() failed");

        // If some group add message are pending and corresponding to the group that was just revoked, we send them now
        if (!ucs_list_is_empty(&(gp_cache->persistent.pending_send_group_add_msgs)))
        {
            ucs_list_for_each_safe(pending_send, next_pending, &(gp_cache->persistent.pending_send_group_add_msgs), item)
            {
                rank_info_t *rank_info = (rank_info_t *)pending_send->ev->payload;
                assert(rank_info);
                if (revoke_msg->gp_uid == rank_info->group_uid)
                {
                    ucs_list_del(&(pending_send->item));
                    rc = do_send_add_group_rank_request(pending_send->econtext,
                                                        pending_send->dest_ep,
                                                        pending_send->dest_id,
                                                        pending_send->ev);
                    CHECK_ERR_RETURN((rc != DO_SUCCESS), DO_ERROR, "do_send_add_group_rank_request() failed");
                    DYN_LIST_RETURN(econtext->engine->pool_pending_send_group_add, pending_send, item);

                    // WARNING!!! only one group add per group/communicator at a time.
                    break;
                }
            }
        }
    }

    return DO_SUCCESS;
}

// Note: function invoked when a rank on a host sends a revoke message to its SP(s)
static dpu_offload_status_t handle_revoke_group_rank_through_rank_info(execution_context_t *econtext, group_revoke_msg_from_rank_t *revoke_msg)
{
    dpu_offload_status_t rc;
    group_cache_t *gp_cache = NULL;

    // rank info objects are only used right now from the host to a service process
    assert(econtext->engine->on_dpu);
    if (revoke_msg->rank_info.group_uid == INT_MAX)
    {
        // The data we received really does not include any usable group data
        return DO_SUCCESS;
    }

    // Revoke message received from the local host
    assert(is_in_cache(&(econtext->engine->procs_cache), revoke_msg->rank_info.group_uid, revoke_msg->rank_info.group_rank, revoke_msg->rank_info.group_size));
    gp_cache = GET_GROUP_CACHE_INTERNAL(&(econtext->engine->procs_cache),
                                        revoke_msg->rank_info.group_uid,
                                        revoke_msg->rank_info.group_size);
    assert(gp_cache);
    assert(gp_cache->initialized);
    assert(gp_cache->n_local_ranks > 0);
    assert(gp_cache->n_local_ranks_populated > 0);
    assert(gp_cache->revokes.global <= gp_cache->group_size);
    assert(gp_cache->revokes.local <= gp_cache->n_local_ranks);
    // A rank cannot revoke a group twice
    assert(gp_cache->revokes.ranks);
    assert(!GROUP_CACHE_BITSET_TEST(gp_cache->revokes.ranks, revoke_msg->rank_info.group_rank));
    assert(gp_cache->revokes.global <= gp_cache->group_size);

    if (gp_cache->group_size != gp_cache->num_local_entries)
    {
        // Group cache is not fully populated yet, queuing the revoke request
        group_revoke_msg_from_rank_t *desc = NULL;
        DBG("Cache not fully populated (group UID: 0x%x, group size: %ld), queuing revoke request",
            gp_cache->group_uid, gp_cache->group_size);
        DYN_LIST_GET(econtext->engine->pool_group_revoke_msgs_from_ranks, group_revoke_msg_from_rank_t, item, desc);
        assert(desc);
        COPY_RANK_INFO(&(revoke_msg->rank_info), &(desc->rank_info));
        ucs_list_add_tail(&(gp_cache->persistent.pending_group_revoke_msgs_from_ranks), &(desc->item));
    }
    else
    {
        // Mark the rank has one that revoked the group
        assert(revoke_msg->rank_info.group_seq_num == gp_cache->persistent.num);
        DBG("Marking rank %ld as revoked (group 0x%x, group seq num: %ld, bitset: %p, local: %ld, global: %ld)",
            revoke_msg->rank_info.group_rank,
            revoke_msg->rank_info.group_uid,
            revoke_msg->rank_info.group_seq_num,
            gp_cache->revokes.ranks,
            gp_cache->revokes.local,
            gp_cache->revokes.global);
        GROUP_CACHE_BITSET_SET(gp_cache->revokes.ranks, revoke_msg->rank_info.group_rank);
        assert(gp_cache->revokes.global <= gp_cache->group_size);
        gp_cache->revokes.local++;
        gp_cache->revokes.global++;
        assert(gp_cache->revokes.global <= gp_cache->group_size);
        DBG("Revoke recv'd from rank %ld for group 0x%x (size: %ld) from local ranks, we now have %ld local and %ld global revokes, expecting %ld local revokes",
            revoke_msg->rank_info.group_rank, revoke_msg->rank_info.group_uid, gp_cache->group_size, gp_cache->revokes.local, gp_cache->revokes.global, gp_cache->sp_ranks);

        assert(gp_cache->engine);
        assert(gp_cache->sp_ranks > 0);
        assert(gp_cache->sp_ranks <= gp_cache->group_size);
        if (gp_cache->revokes.local == gp_cache->sp_ranks)
        {
            // All the local ranks attached to this SP revoked this group, we now notify other SPs that the group
            // has been deleted here.
            size_t dummy_new_revokes = 0;
            DBG("Broadcasting group revoke (group UID: 0x%x, gp_cache: %p, seq num: %ld)",
                gp_cache->group_uid, gp_cache, gp_cache->persistent.num);
            rc = broadcast_group_cache_revoke(econtext->engine, gp_cache);
            CHECK_ERR_RETURN((rc != DO_SUCCESS), DO_ERROR, "broadcast_group_cache_revoke() failed");
            assert(gp_cache->revokes.global <= gp_cache->group_size);
            assert(gp_cache->revokes.ranks);
            HANDLE_PENDING_GROUP_REVOKE_MSGS_FROM_SPS(gp_cache, dummy_new_revokes);
            assert(gp_cache->revokes.global <= gp_cache->group_size);
        }
        if (gp_cache->revokes.global == gp_cache->group_size &&
            gp_cache->persistent.revoke_send_to_host_posted < gp_cache->persistent.num)
        {
            DBG("Sending final revoke to ranks: %ld", gp_cache->group_size);
            assert(gp_cache->group_size);
            rc = send_revoke_group_to_ranks(econtext->engine, revoke_msg->rank_info.group_uid, gp_cache->group_size);
            CHECK_ERR_RETURN((rc != DO_SUCCESS), DO_ERROR, "send_revoke_group_to_ranks() failed");
        }
    }

    assert(gp_cache->revokes.global <= gp_cache->group_size);
    return DO_SUCCESS;
}

/**
 * @brief revoke_group_from_rank_recv_cb is invoked on the DPU when receiving a notification from a rank running on the local host
 * that a group/rank had been revoked.
 *
 * @param ev_sys Associated event/notification system
 * @param econtext Associated execution contexxt
 * @param hdr Header of the received notification
 * @param hdr_size Size of the header
 * @param data Payload associated to the notification
 * @param data_len Size of the payload
 * @return dpu_offload_status_t
 */
static dpu_offload_status_t revoke_group_from_rank_recv_cb(struct dpu_offload_ev_sys *ev_sys, execution_context_t *econtext, am_header_t *hdr, size_t hdr_size, void *data, size_t data_len)
{
    group_revoke_msg_from_rank_t *revoke_msg = NULL;
    assert(econtext);
    assert(econtext->engine);
    assert(data);
    assert(data_len == sizeof(group_revoke_msg_from_rank_t));
    revoke_msg = (group_revoke_msg_from_rank_t *)data;
    assert(econtext->engine->on_dpu);
    assert(hdr->scope_id == SCOPE_HOST_DPU);
    return handle_revoke_group_rank_through_rank_info(econtext, revoke_msg);
}

/**
 * @brief revoke_group_from_sp_recv_cb is invoked either on the host or DPU when receiving a notification from a SP
 * to revoke a gorup.
 *
 * @param ev_sys Associated event/notification system
 * @param econtext Associated execution contexxt
 * @param hdr Header of the received notification
 * @param hdr_size Size of the header
 * @param data Payload associated to the notification
 * @param data_len Size of the payload
 * @return dpu_offload_status_t
 */
static dpu_offload_status_t revoke_group_from_sp_recv_cb(struct dpu_offload_ev_sys *ev_sys, execution_context_t *econtext, am_header_t *hdr, size_t hdr_size, void *data, size_t data_len)
{
    group_revoke_msg_from_sp_t *revoke_msg = NULL;
    assert(econtext);
    assert(econtext->engine);
    assert(data);
    assert(data_len == sizeof(group_revoke_msg_from_sp_t));
    revoke_msg = (group_revoke_msg_from_sp_t *)data;
    if (econtext->engine->on_dpu)
        assert(hdr->scope_id == SCOPE_INTER_SERVICE_PROCS);
    return handle_revoke_group_rank_through_list_ranks(econtext, revoke_msg);
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
    assert(econtext->engine);
    assert(data);
    assert(ev_sys->econtext->engine->on_dpu);
#if !NDEBUG
    {
        rank_info_t *rank_info = (rank_info_t *)data;
        DBG("Just received a group add message from local rank for group 0x%x (seq num: %ld)",
            rank_info->group_uid, rank_info->group_seq_num);
    }
#endif
    return do_add_group_rank_recv_cb(econtext->engine, hdr->client_id, data, data_len);
}

static dpu_offload_status_t sp_data_recv_cb(struct dpu_offload_ev_sys *ev_sys, execution_context_t *econtext, am_header_t *hdr, size_t hdr_size, void *data, size_t data_len)
{
    dpu_offload_status_t rc;

    assert(econtext);
    assert(econtext->engine);
    assert(data_len > 0);
    assert(data);
    assert(!ev_sys->econtext->engine->on_dpu);

    // If we already have the data, do nothing.
    if (econtext->engine->host.total_num_sps != SIZE_MAX)
        return DO_SUCCESS;

    assert(econtext->engine->host_dpu_data_initialized == true);
    assert(econtext->engine->buf_data_sps == NULL);
    econtext->engine->buf_data_sps = DPU_OFFLOAD_MALLOC(data_len);
    assert(econtext->engine->buf_data_sps);
    memcpy(econtext->engine->buf_data_sps, data, data_len);
    rc = unpack_data_sps(econtext->engine, econtext->engine->buf_data_sps);
    CHECK_ERR_RETURN((rc), DO_ERROR, "unpack_data_sps() failed");

#if !NDEBUG
    if (!econtext->engine->on_dpu)
    {
        assert(econtext->engine->host.total_num_sps > 0);
        assert(econtext->engine->host.total_num_sps != UINT64_MAX);
    }
#endif

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
    switch (econtext->type)
    {
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

    int rc = event_channel_register(ev_sys, AM_TERM_MSG_ID, term_msg_cb, NULL);
    CHECK_ERR_GOTO(rc, error_out, "cannot register handler to the term notification");

    rc = event_channel_register(ev_sys, AM_OP_START_MSG_ID, op_start_cb, NULL);
    CHECK_ERR_GOTO(rc, error_out, "cannot register handler to start operations");

    rc = event_channel_register(ev_sys, AM_OP_COMPLETION_MSG_ID, op_completion_cb, NULL);
    CHECK_ERR_GOTO(rc, error_out, "cannot register handler for operation completion");

    rc = event_channel_register(ev_sys, AM_XGVMI_ADD_MSG_ID, xgvmi_key_recv_cb, NULL);
    CHECK_ERR_GOTO(rc, error_out, "cannot register handler for receiving XGVMI keys");

    rc = event_channel_register(ev_sys, AM_XGVMI_DEL_MSG_ID, xgvmi_key_revoke_cb, NULL);
    CHECK_ERR_GOTO(rc, error_out, "cannot register handler for revoke XGVMI keys");

    rc = event_channel_register(ev_sys, AM_PEER_CACHE_ENTRIES_MSG_ID, peer_cache_entries_recv_cb, NULL);
    CHECK_ERR_GOTO(rc, error_out, "cannot register handler for receiving peer cache entries");

    rc = event_channel_register(ev_sys, AM_PEER_CACHE_ENTRIES_REQUEST_MSG_ID, peer_cache_entries_request_recv_cb, NULL);
    CHECK_ERR_GOTO(rc, error_out, "cannot register handler for receiving peer cache requests");

    rc = event_channel_register(ev_sys, AM_ADD_GP_RANK_MSG_ID, add_group_rank_recv_cb, NULL);
    CHECK_ERR_GOTO(rc, error_out, "cannot register handler for receiving requests to add a group/rank");

    rc = event_channel_register(ev_sys, AM_REVOKE_GP_RANK_MSG_ID, revoke_group_from_rank_recv_cb, NULL);
    CHECK_ERR_GOTO(rc, error_out, "cannot register handler for receiving requests to revoke a group from ranks");

    rc = event_channel_register(ev_sys, AM_REVOKE_GP_SP_MSG_ID, revoke_group_from_sp_recv_cb, NULL);
    CHECK_ERR_GOTO(rc, error_out, "cannot register handler for receiving requests to revoke a group from SPs");

    rc = event_channel_register(ev_sys, AM_SP_DATA_MSG_ID, sp_data_recv_cb, NULL);
    CHECK_ERR_GOTO(rc, error_out, "cannot register handler for receiving requests to get SPs data");

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
