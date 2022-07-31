//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include "dpu_offload_service_daemon.h"

#ifndef DPU_OFFLOAD_COMMS_H_
#define DPU_OFFLOAD_COMMS_H_

#define MAX_POSTED_SENDS (8)

/* All the tag related code has been taken from UCC */

/* Reflects the definition in UCS - The i-th bit */
#define OFFLOAD_BIT(i) (1ul << (i))

#define OFFLOAD_MASK(i) (OFFLOAD_BIT(i) - 1)

/*
 * UCP tag structure:
 *
 *  01        | 01234567 01234567 |    234   |      567    | 01234567 01234567 01234567 | 01234567 01234567
 *            |                   |          |             |                            |
 *  RESERV(2) | message tag (16)  | SCOPE(3) | SCOPE_ID(3) |     source rank (24)       |    team id (16)
 */

#define OFFLOAD_RESERVED_BITS 2
#define OFFLOAD_SCOPE_BITS 3
#define OFFLOAD_SCOPE_ID_BITS 3
#define OFFLOAD_TAG_BITS 16
#define OFFLOAD_SENDER_BITS 24
#define OFFLOAD_ID_BITS 16

#define OFFLOAD_RESERVED_BITS_OFFSET                                 \
    (OFFLOAD_ID_BITS + OFFLOAD_SENDER_BITS + OFFLOAD_SCOPE_ID_BITS + \
     OFFLOAD_SCOPE_BITS + OFFLOAD_TAG_BITS)

#define OFFLOAD_TAG_BITS_OFFSET                                      \
    (OFFLOAD_ID_BITS + OFFLOAD_SENDER_BITS + OFFLOAD_SCOPE_ID_BITS + \
     OFFLOAD_SCOPE_BITS)

#define OFFLOAD_SCOPE_BITS_OFFSET \
    (OFFLOAD_ID_BITS + OFFLOAD_SENDER_BITS + OFFLOAD_SCOPE_ID_BITS)

#define OFFLOAD_SCOPE_ID_BITS_OFFSET (OFFLOAD_ID_BITS + OFFLOAD_SENDER_BITS)
#define OFFLOAD_SENDER_BITS_OFFSET (OFFLOAD_ID_BITS)
#define OFFLOAD_ID_BITS_OFFSET 0

#define OFFLOAD_MAX_TAG OFFLOAD_MASK(OFFLOAD_TAG_BITS)
#define OFFLOAD_RESERVED_TAGS 8
#define OFFLOAD_MAX_COLL_TAG (OFFLOAD_MAX_TAG - OFFLOAD_RESERVED_TAGS)
#define OFFLOAD_SERVICE_TAG (OFFLOAD_MAX_COLL_TAG + 1)
#define OFFLOAD_MAX_SENDER OFFLOAD_MASK(OFFLOAD_SENDER_BITS)
#define OFFLOAD_MAX_ID OFFLOAD_MASK(OFFLOAD_ID_BITS)

#define MAKE_TAG(_tag, _rank, _id, _scope_id, _scope)            \
    ((((uint64_t)(_tag)) << OFFLOAD_TAG_BITS_OFFSET) |           \
     (((uint64_t)(_rank)) << OFFLOAD_SENDER_BITS_OFFSET) |       \
     (((uint64_t)(_scope)) << OFFLOAD_SCOPE_BITS_OFFSET) |       \
     (((uint64_t)(_scope_id)) << OFFLOAD_SCOPE_ID_BITS_OFFSET) | \
     (((uint64_t)(_id)) << OFFLOAD_ID_BITS_OFFSET))

#define MAKE_SEND_TAG(_tag, _rank, _id, _scope_id, _scope) \
    MAKE_TAG(_tag, _rank, _id, _scope_id, _scope)

#define MAKE_RECV_TAG(_ucp_tag, _ucp_tag_mask, _tag, _src, _id,     \
                      _scope_id, _scope)                            \
    do                                                              \
    {                                                               \
        assert((_tag) <= OFFLOAD_MAX_TAG);                          \
        assert((_src) <= OFFLOAD_MAX_SENDER);                       \
        assert((_id) <= OFFLOAD_MAX_ID);                            \
        (_ucp_tag_mask) = (uint64_t)(-1);                           \
        (_ucp_tag) =                                                \
            MAKE_TAG((_tag), (_src), (_id), (_scope_id), (_scope)); \
    } while (0)

// PREP_NOTIF_RECV assumes the execution context is locked before being invoked
#define PREP_NOTIF_RECV(_ctx, _hdr_recv_param, _hdr_ucp_tag, _hdr_ucp_tag_mask, _worker, _client_id, _server_id, _scope_id)                \
    do                                                                                                                                     \
    {                                                                                                                                      \
        /* Always have a recv posted so we are ready to get a header from the other side. */                                               \
        /* Remember we are using one thread per bootstrap client/server. */                                                                \
        /* Just to make sure the initial receive will post, we mark the two receives for */                                                \
        /* any notifications as completed */                                                                                               \
        (_ctx).complete = true;                                                                                                            \
        (_ctx).payload_ctx.complete = true;                                                                                                \
        (_ctx).client_id = (_client_id);                                                                                                   \
        (_ctx).server_id = (_server_id);                                                                                                   \
        (_hdr_recv_param).op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |                                                                      \
                                         UCP_OP_ATTR_FIELD_DATATYPE |                                                                      \
                                         UCP_OP_ATTR_FIELD_USER_DATA;                                                                      \
        (_hdr_recv_param).datatype = ucp_dt_make_contig(1);                                                                                \
        (_hdr_recv_param).user_data = &(_ctx);                                                                                             \
        (_hdr_recv_param).cb.recv = notif_hdr_recv_handler;                                                                                \
        MAKE_RECV_TAG((_hdr_ucp_tag),                                                                                                      \
                      (_hdr_ucp_tag_mask),                                                                                                 \
                      AM_EVENT_MSG_HDR_ID,                                                                                                 \
                      (_client_id),                                                                                                        \
                      (_server_id),                                                                                                        \
                      (_scope_id),                                                                                                         \
                      0);                                                                                                                  \
        worker = GET_WORKER(econtext);                                                                                                     \
        DBG(" -> Reception of notification on econtext %p, client_id: %" PRIu64 ", server_id: %" PRIu64 " and scope_id %d is now all set", \
            econtext,                                                                                                                      \
            (_client_id), (_server_id),                                                                                                    \
            (_scope_id));                                                                                                                  \
    } while (0)

#define CAN_POST(_event_system) ({                                                   \
    bool _can_post = false;                                                          \
    if (MAX_POSTED_SENDS <= 0 || ((_event_system)->posted_sends < MAX_POSTED_SENDS)) \
        _can_post = true;                                                            \
    _can_post;                                                                       \
})

#define GROUP_CACHE_EXCHANGE(_engine, _gp_id, _n_local_ranks)                                                                \
    do                                                                                                                       \
    {                                                                                                                        \
        uint64_t n_connecting_ranks;                                                                                         \
        get_num_connecting_ranks((_engine)->config->num_service_procs_per_dpu,                                               \
                                 (_n_local_ranks),                                                                           \
                                 (_engine)->config->local_service_proc.info.local_id,                                        \
                                 &n_connecting_ranks);                                                                       \
        group_cache_t *__gp_cache = GET_GROUP_CACHE(&((_engine)->procs_cache), (_gp_id));                                    \
        if (__gp_cache->n_local_ranks > 0 && __gp_cache->n_local_ranks_populated == n_connecting_ranks)                      \
        {                                                                                                                    \
            DBG("We now have a connection with all the local ranks, we can broadcast the group cache at once");              \
            broadcast_group_cache((_engine), _gp_id);                                                                        \
        }                                                                                                                    \
        if (__gp_cache->n_local_ranks < 0)                                                                                   \
        {                                                                                                                    \
            DBG("We do not know how many ranks to locally expect for that group, broadcast the new information by default"); \
            broadcast_group_cache((_engine), (_gp_id));                                                                      \
        }                                                                                                                    \
    } while (0)

#if USE_AM_IMPLEM
static bool event_posted(dpu_offload_event_t *ev)
{
    if (ev->ctx.complete == false && (ev->was_posted == true || ev->req != NULL))
        return true;
    return false;
}

#define PROGRESS_EVENT_SEND(__ev)                                                                \
    do                                                                                           \
    {                                                                                            \
        /* if we can post more events and the event is not posted yet, try to send it. */        \
        if (!event_completed((__ev)) && CAN_POST((__ev)->event_system) && !event_posted((__ev))) \
        {                                                                                        \
            int rc;                                                                              \
            rc = do_am_send_event_msg((__ev));                                                   \
            if (rc != EVENT_DONE && rc != EVENT_INPROGRESS)                                      \
                ERR_MSG("do_am_send_event_msg() failed");                                        \
        }                                                                                        \
        /* Now check if it is completed */                                                       \
        if (event_completed((__ev)))                                                             \
        {                                                                                        \
            if ((__ev)->is_ongoing_event && (__ev)->is_subevent)                                 \
                ERR_MSG("sub-event %p %ld also on the ongoing list", (__ev), (__ev)->seq_num);   \
            ucs_list_del(&((__ev)->item));                                                       \
            if ((__ev)->is_ongoing_event)                                                        \
                (__ev)->is_ongoing_event = false;                                                \
            if ((__ev)->is_subevent)                                                             \
                (__ev)->is_subevent = false;                                                     \
            if ((__ev)->was_posted)                                                              \
            {                                                                                    \
                (__ev)->event_system->posted_sends--;                                            \
                (__ev)->was_posted = false;                                                      \
            }                                                                                    \
            event_return(&(__ev));                                                               \
        }                                                                                        \
    } while(0)
#else
#define PROGRESS_EVENT_SEND(__ev)                                                                \
    do                                                                                           \
    {                                                                                            \
        /* if we can post more events and the event is not posted yet, try to send it. */        \
        if (!event_completed((__ev)) && CAN_POST((__ev)->event_system) && !event_posted((__ev))) \
        {                                                                                        \
            int rc;                                                                              \
            rc = do_tag_send_event_msg((__ev));                                                  \
            if (rc != EVENT_DONE && rc != EVENT_INPROGRESS)                                      \
                ERR_MSG("do_tag_send_event_msg() failed");                                       \
        }                                                                                        \
        /* Now check if it is completed */                                                       \
        if (event_completed((__ev)))                                                             \
        {                                                                                        \
            if ((__ev)->is_ongoing_event && (__ev)->is_subevent)                                 \
                ERR_MSG("sub-event %p %ld also on the ongoing list", (__ev), (__ev)->seq_num);   \
            ucs_list_del(&((__ev)->item));                                                       \
            if ((__ev)->is_ongoing_event)                                                        \
                (__ev)->is_ongoing_event = false;                                                \
            if ((__ev)->is_subevent)                                                             \
                (__ev)->is_subevent = false;                                                     \
            if ((__ev)->was_posted)                                                              \
            {                                                                                    \
                (__ev)->event_system->posted_sends--;                                            \
                (__ev)->was_posted = false;                                                      \
            }                                                                                    \
            event_return(&(__ev));                                                               \
        }                                                                                        \
    } while(0)

static bool event_posted(dpu_offload_event_t *ev)
{
    if (ev->ctx.hdr_completed == false && ev->hdr_request != NULL && EVENT_HDR_PAYLOAD_SIZE(ev) == 0)
    {
        // The send for the header is posted (and not completed) and no payload needs to be sent
        return true;
    }
    
    if (ev->ctx.hdr_completed == true &&
        ev->hdr_request == NULL &&
        EVENT_HDR_PAYLOAD_SIZE(ev) > 0 &&
        ev->ctx.payload_completed == false &&
        ev->payload_request != NULL)
    {
        // The send for the header completed and the send for the payload was posted and not yet completed
        return true;
    }

    return false;
}
#endif // USE_AM_IMPLEM

static void progress_econtext_sends(execution_context_t *ctx)
{
    dpu_offload_event_t *ev, *next_ev;
#if USE_AM_IMPLEM
    if (ctx->scope_id == CONTEXT_SELF)
        return;
#endif
    ucs_list_for_each_safe(ev, next_ev, &(ctx->ongoing_events), item)
    {
        if (ev->is_ongoing_event == false)
        {
            ERR_MSG("Ev %p type %ld is not on ongoing list", ev, EVENT_HDR_TYPE(ev));
            assert(0);
        }
        assert(ev->is_ongoing_event == true);       
        if (EVENT_HDR_TYPE(ev) == META_EVENT_TYPE)
        {
            assert(ev->sub_events_initialized);
            // if the event is meta-event, we need to check the sub-events
            dpu_offload_event_t *subev, *next_subev;
            ucs_list_for_each_safe(subev, next_subev, (&(ev->sub_events)), item)
            {
                PROGRESS_EVENT_SEND(subev);
            }

            // Finally check if the meta-event is now completed
            if (event_completed(ev))
            {
                assert(ev->is_ongoing_event);
                ucs_list_del(&(ev->item));
                ev->is_ongoing_event = false;
                if (ev->was_posted)
                {
                    ev->event_system->posted_sends--;
                    ev->was_posted = false;
                }
                event_return(&ev);
            }
        }
        else
        {
            PROGRESS_EVENT_SEND(ev);
        }
    }
}

/**
 * @brief Note: the function assumes that:
 * - the execution context is not locked before the function is invoked
 * - the event system is not locked beffore the function is invoked
 *
 * @param econtext Execution context associated to the event
 * @param hdr Header of the notification that was received
 * @param header_length Length of the header
 * @param data Notification's payload. Can be NULL
 * @param length Length of the payload; must be zero when data is NULL.
 * @return int
 */
static int handle_notif_msg(execution_context_t *econtext, am_header_t *hdr, size_t header_length, void *data, size_t length)
{
    assert(econtext);
    assert(hdr);
    assert(econtext->event_channels);
    if (hdr->payload_size > 0 && data == NULL)
    {
        ERR_MSG("the payload is %" PRIu64 " but the buffer is NULL", hdr->payload_size);
        return UCS_ERR_NO_MESSAGE;
    }
    SYS_EVENT_LOCK(econtext->event_channels);
    notification_callback_entry_t *entry = DYN_ARRAY_GET_ELT(&(econtext->event_channels->notification_callbacks), hdr->type, notification_callback_entry_t);
    DBG("Notification of type %" PRIu64 " received from %" PRIu64 " (econtext: %p), dispatching...", hdr->type, hdr->id, econtext);
    if (entry->set == false)
    {
        pending_notification_t *pending_notif;
        DBG("callback not available for %" PRIu64 " on event system %p (econtext: %p)",
            hdr->type, econtext->event_channels, econtext);
        DYN_LIST_GET(econtext->event_channels->free_pending_notifications, pending_notification_t, item, pending_notif);
        CHECK_ERR_RETURN((pending_notif == NULL), UCS_ERR_NO_MESSAGE, "unable to get pending notification object");
        RESET_PENDING_NOTIF(pending_notif);
        pending_notif->type = hdr->type;
        pending_notif->src_id = hdr->id;
        pending_notif->data_size = length;
        pending_notif->header_size = header_length;
        pending_notif->econtext = econtext;
        if (pending_notif->data_size > 0)
        {
            pending_notif->data = DPU_OFFLOAD_MALLOC(pending_notif->data_size);
            CHECK_ERR_RETURN((pending_notif->data == NULL), DO_ERROR, "unable to allocate pending notification's data");
            memcpy(pending_notif->data, data, pending_notif->data_size);
        }
        else
        {
            pending_notif->data = NULL;
        }
        if (pending_notif->header_size > 0)
        {
            pending_notif->header = DPU_OFFLOAD_MALLOC(pending_notif->header_size);
            CHECK_ERR_RETURN((pending_notif->header == NULL), DO_ERROR, "unable to allocate pending notification's header");
            memcpy(pending_notif->header, hdr, pending_notif->header_size);
        }
        else
        {
            pending_notif->header = NULL;
        }
        ucs_list_add_tail(&(econtext->event_channels->pending_notifications), &(pending_notif->item));
        SYS_EVENT_UNLOCK(econtext->event_channels);
        return UCS_OK;
    }

    notification_cb cb = entry->cb;
    CHECK_ERR_GOTO((cb == NULL), error_out, "Callback is undefined");
    struct dpu_offload_ev_sys *ev_sys = EV_SYS(econtext);
    // Callbacks are responsible for handling any necessary locking
    // and can call any event API so we unlock before invoking it.
    SYS_EVENT_UNLOCK(econtext->event_channels);
    // We unlock the execution context before invoking the callback to limit
    // the constraints on what can be done in the callback.
    cb(ev_sys, econtext, hdr, header_length, data, length);
    return UCS_OK;
error_out:
    SYS_EVENT_UNLOCK(econtext->event_channels);
    return UCS_ERR_NO_MESSAGE;
}

#if !USE_AM_IMPLEM
/**
 * @brief Note that the function assumes the execution context is not locked before it is invoked.
 *
 * @param request
 * @param status
 * @param tag_info
 * @param user_data
 */
static void notif_payload_recv_handler(void *request, ucs_status_t status, const ucp_tag_recv_info_t *tag_info, void *user_data)
{
    int rc;
    hdr_notif_req_t *ctx;
    if (status == UCS_ERR_CANCELED)
    {
        // Callback was invoked during finalization when the notification recv is canceled. Do nothing.
        return;
    }
    assert(status == UCS_OK);
    ctx = (hdr_notif_req_t *)user_data;
    assert(ctx);
    assert(ctx->econtext);
    DBG("Notification payload received, ctx=%p econtext=%p type=%ld", ctx, ctx->econtext, ctx->hdr.type);
    assert(ctx->hdr.payload_size == tag_info->length);
    ctx->payload_ctx.complete = true;

    // Invoke the associated callback
    rc = handle_notif_msg(ctx->econtext, &(ctx->hdr), sizeof(am_header_t), ctx->payload_ctx.buffer, ctx->hdr.payload_size);
    if (rc != UCS_OK)
    {
        ERR_MSG("handle_notif_msg() failed");
        assert(0); // fail when in debug mode
        return;
    }

    if (ctx->req != NULL)
    {
        assert(ctx->complete == true);
        ucp_request_free(ctx->req);
        ctx->req = NULL;
    }
    if (ctx->payload_ctx.req != NULL)
    {
        assert(ctx->payload_ctx.complete == true);
        ucp_request_free(ctx->payload_ctx.req);
        ctx->payload_ctx.req = NULL;
    }
    if (ctx->payload_ctx.buffer != NULL)
    {
        if (ctx->payload_ctx.pool.mem_pool != NULL)
        {
            if (ctx->payload_ctx.pool.return_buf != NULL)
            {
                // Call the return function specified by the caller library.
                assert(ctx->payload_ctx.pool.return_buf);
                ctx->payload_ctx.pool.return_buf(ctx->payload_ctx.pool.mem_pool,
                                                 ctx->payload_ctx.buffer);
            }
            ctx->payload_ctx.pool.mem_pool = NULL;
        }
        else
        {
            // Return the buffer to the smart buffer system
            assert(ctx->payload_ctx.smart_buf);
            SMART_BUFF_RETURN(&(ctx->econtext->engine->smart_buffer_sys),
                              ctx->hdr.payload_size,
                              ctx->payload_ctx.smart_buf);
            ctx->payload_ctx.smart_buf = NULL;
        }
        ctx->payload_ctx.buffer = NULL;
    }
    ctx->payload_ctx.complete = true;
}

/**
 * @brief Note that the function assumes the execution context is not locked before it is invoked.
 *
 * @param ctx
 * @param econtext
 * @param peer_id
 * @return EVENT_DONE if the receive completed right away; EVENT_INPROGRESS otherwise
 */
static int post_recv_for_notif_payload(hdr_notif_req_t *ctx, execution_context_t *econtext, uint64_t peer_id)
{
    int rc = EVENT_INPROGRESS;
    assert(ctx);
    assert(econtext);
    DBG("Notification header received, payload size = %ld, type = %ld, econtext = %p-%p, ctx = %p",
        ctx->hdr.payload_size,
        ctx->hdr.type,
        ctx->econtext,
        econtext,
        ctx);
    assert(econtext == ctx->econtext);
    assert(ctx->complete == true); // The header should be completed

    if (ctx->hdr.payload_size > 0 && ctx->payload_ctx.req != NULL)
    {
        DBG("We got a new header but still waiting for a previous notification payload");
        return EVENT_INPROGRESS;
    }

    if (ctx->hdr.payload_size > 0)
    {
        ucp_tag_t payload_ucp_tag, payload_ucp_tag_mask;
        notification_callback_entry_t *entry;
        DBG("Posting recv for notif payload of size %ld for peer %ld (client_id: %" PRIu64 ", server_id: %" PRIu64 ")",
            ctx->hdr.payload_size, peer_id, ctx->client_id, ctx->server_id);
        ucp_worker_h worker;

        // If the notification type is already registered and is associated to a memory pool, we use a buffer from than pool
        entry = DYN_ARRAY_GET_ELT(&(econtext->event_channels->notification_callbacks), ctx->hdr.type, notification_callback_entry_t);
        assert(entry);
        if (entry->info.mem_pool)
        {
            void *buf_from_pool = get_notif_buf(econtext->event_channels, ctx->hdr.type);
            assert(buf_from_pool);
            assert(ctx->payload_ctx.pool.mem_pool == NULL);
            ctx->payload_ctx.buffer = buf_from_pool;
            COPY_NOTIF_INFO(&(entry->info), &(ctx->payload_ctx.pool));
        }
        else
        {
            // Get a buffer from the smart buffer system to avoid allocating memory
            assert(ctx->payload_ctx.smart_buf == NULL);
            ctx->payload_ctx.smart_buf = SMART_BUFF_GET(&(econtext->engine->smart_buffer_sys), ctx->hdr.payload_size);
            assert(ctx->payload_ctx.smart_buf);
            ctx->payload_ctx.buffer = ctx->payload_ctx.smart_buf->base;
        }
        assert(ctx->payload_ctx.buffer);
        worker = GET_WORKER(econtext);
        assert(worker);
        // Post the receive for the payload
        ctx->payload_ctx.complete = false;
        memset(&ctx->payload_ctx.recv_params, 0, sizeof(ctx->payload_ctx.recv_params));
        ctx->payload_ctx.recv_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                                                    UCP_OP_ATTR_FIELD_DATATYPE |
                                                    UCP_OP_ATTR_FIELD_USER_DATA;
        ctx->payload_ctx.recv_params.datatype = ucp_dt_make_contig(1);
        ctx->payload_ctx.recv_params.user_data = ctx;
        ctx->payload_ctx.recv_params.cb.recv = notif_payload_recv_handler;
        MAKE_RECV_TAG(payload_ucp_tag, payload_ucp_tag_mask, AM_EVENT_MSG_ID, ctx->client_id, ctx->server_id, econtext->scope_id, 0);
        DBG("Tag: %d; scope_id: %u", AM_EVENT_MSG_ID, econtext->scope_id);
        ctx->payload_ctx.req = ucp_tag_recv_nbx(worker,
                                                ctx->payload_ctx.buffer,
                                                ctx->hdr.payload_size,
                                                payload_ucp_tag,
                                                payload_ucp_tag_mask,
                                                &(ctx->payload_ctx.recv_params));
        if (ctx->payload_ctx.req == NULL)
        {
            int rc;
            // Recv completed immediately, the callback is not invoked
            DBG("Recv of the payload completed right away");
            ctx->payload_ctx.complete = true;
            rc = handle_notif_msg(ctx->econtext, &(ctx->hdr), sizeof(am_header_t), ctx->payload_ctx.buffer, ctx->hdr.payload_size);
            if (rc != UCS_OK)
            {
                ERR_MSG("handle_notif_msg() failed");
                return -1;
            }

            if (ctx->payload_ctx.buffer != NULL)
            {
                if (ctx->payload_ctx.pool.mem_pool == NULL)
                {
                    // Return the smart chunk to the smart buffer system.
                    assert(ctx->payload_ctx.smart_buf);
                    SMART_BUFF_RETURN(&(econtext->engine->smart_buffer_sys),
                                      ctx->hdr.payload_size,
                                      ctx->payload_ctx.smart_buf);
                    ctx->payload_ctx.smart_buf = NULL;
                    assert(ctx->payload_ctx.smart_buf == NULL);
                }
                if (ctx->payload_ctx.pool.mem_pool != NULL)
                {
                    if (ctx->payload_ctx.pool.return_buf != NULL)
                    {
                        // The calling library as its own pool of objects to handle notifications.
                        // Call the return function.
                        ctx->payload_ctx.pool.return_buf(ctx->payload_ctx.pool.mem_pool, ctx->payload_ctx.buffer);
                    }
                    ctx->payload_ctx.pool.mem_pool = NULL;
                }
                ctx->payload_ctx.buffer = NULL;
            }
#if !NDEBUG
            else
            {
                assert(ctx->hdr.payload_size == 0);
            }
#endif
            assert(ctx->payload_ctx.smart_buf == NULL);

            if (ctx->req != NULL)
            {
                assert(ctx->complete == true);
                ucp_request_free(ctx->req);
                ctx->req = NULL;
            }
            if (ctx->payload_ctx.req != NULL)
            {
                assert(ctx->payload_ctx.complete == true);
                ucp_request_free(ctx->payload_ctx.req);
                ctx->payload_ctx.req = NULL;
            }
            rc = EVENT_DONE;
            assert(ctx->payload_ctx.smart_buf == NULL);
        }
        else
            rc = EVENT_INPROGRESS;

        if (UCS_PTR_IS_ERR(ctx->payload_ctx.req))
        {
            ucs_status_t recv_status = UCS_PTR_STATUS(ctx->payload_ctx.req);
            ERR_MSG("ucp_tag_recv_nbx() failed: %s", ucs_status_string(recv_status));
            return -1;
        }
    }
    else
    {
        // No payload to be received
        ctx->payload_ctx.complete = true;
        int rc = handle_notif_msg(econtext, &(ctx->hdr), sizeof(am_header_t), NULL, 0);
        if (rc != UCS_OK)
        {
            ERR_MSG("handle_notif_msg() failed\n");
            return -1;
        }

        // Payload is freed when the event is returned
        if (ctx->req != NULL)
        {
            ucp_request_free(ctx->req);
            ctx->req = NULL;
        }
        if (ctx->payload_ctx.req != NULL)
        {
            ucp_request_free(ctx->payload_ctx.req);
            ctx->payload_ctx.req = NULL;
        }

        rc = EVENT_DONE;
    }
    return rc;
}

/**
 * @brief Note that the function assumes that the execution context is not locked before it is invoked.
 *
 * @param worker
 * @param ctx
 * @param econtext
 * @param hdr_ucp_tag
 * @param hdr_ucp_tag_mask
 * @param hdr_recv_param
 * @return EVENT_DONE if the receive completed right away; EVENT_INPROGRESS otherwise
 */
static int post_new_notif_recv(ucp_worker_h worker, hdr_notif_req_t *ctx, execution_context_t *econtext, ucp_tag_t hdr_ucp_tag, ucp_tag_t hdr_ucp_tag_mask, ucp_request_param_t *hdr_recv_param)
{
    int rc = EVENT_INPROGRESS;
#if !NDEBUG
    if (econtext->engine->on_dpu && econtext->scope_id == SCOPE_INTER_SERVICE_PROCS)
    {
        if (ctx->client_id >= econtext->engine->num_service_procs)
        {
            ERR_MSG("requested client ID is invalid: %" PRIu64, ctx->client_id);
        }
        assert(ctx->client_id < econtext->engine->num_service_procs);
    }
#endif

    // If the execution context is in the process of finalizing, do not post a new receive.
    // Only valid when the execution context is a client or a server.
    if ((econtext->type == CONTEXT_SERVER || econtext->type == CONTEXT_CLIENT) && EXECUTION_CONTEXT_DONE(econtext) == true)
        return EVENT_INPROGRESS;

    // Post a new receive only if we are not already in the middle of receiving a notification
    if (ctx->complete == true && ctx->payload_ctx.complete == true)
    {
        // Post the receive for the header
        ctx->complete = false;
        ctx->econtext = (struct execution_context *)econtext;
        DBG("-------------> Posting recv for notif header (econtext: %p, scope_id: %d, worker: %p, client_id: %" PRIu64 ", server_id: %" PRIu64 ", size: %ld)",
            econtext, econtext->scope_id, worker, ctx->client_id, ctx->server_id, sizeof(am_header_t));
        struct ucx_context *req = ucp_tag_recv_nbx(worker, &(ctx->hdr), sizeof(am_header_t), hdr_ucp_tag, hdr_ucp_tag_mask, hdr_recv_param);
        if (req == NULL)
        {
            assert(ctx->client_id == ctx->hdr.client_id);
            assert(ctx->server_id == ctx->hdr.server_id);

            // Receive completed immediately, callback is not called
            DBG("Recv of notification header completed right away, notif type: %ld", ctx->hdr.type);
            ctx->complete = true;
            rc = post_recv_for_notif_payload(ctx, (execution_context_t *)ctx->econtext, ctx->hdr.id);
        }
        else
        {
            ctx->req = req;
            rc = EVENT_INPROGRESS;
        }
    }
    return rc;
}

/**
 * @brief Note that the function assumes that the execution context is not locked before it is invoked.
 *
 * @param request
 * @param status
 * @param tag_info
 * @param user_data
 */
static void notif_hdr_recv_handler(void *request, ucs_status_t status, const ucp_tag_recv_info_t *tag_info, void *user_data)
{
    hdr_notif_req_t *ctx = (hdr_notif_req_t *)user_data;
    if (status == UCS_ERR_CANCELED)
    {
        // Callback was invoked during finalization when the notification recv is canceled. Do nothing.
        return;
    }
    assert(status == UCS_OK);
    assert(tag_info->length == sizeof(am_header_t));
#if !NDEBUG
    if (ctx->client_id != ctx->hdr.client_id)
    {
        ERR_MSG("!!!  expecting a message from client_id: %" PRIu64 " but received %" PRIu64 " econtext: %p scope_id: %d etype: %d ctx: %p hdr type: %" PRIu64 ", event ID: %" PRIu64,
                ctx->client_id, ctx->hdr.client_id, ctx->econtext, ctx->econtext->scope_id, ctx->econtext->type, ctx, ctx->hdr.type, ctx->hdr.event_id);
        abort();
    }

    if (ctx->server_id != ctx->hdr.server_id)
    {
        ERR_MSG("expecting a message from server_id: %" PRIu64 " but received %" PRIu64 " ctx: %p", ctx->server_id, ctx->hdr.server_id, ctx);
        if (!ctx->econtext->engine->on_dpu && ctx->econtext->type == CONTEXT_CLIENT)
        {
            DBG("My client ID is %" PRId64, ctx->econtext->rank.group_rank);
        }
        abort();
    }
#endif

    assert(ctx->client_id == ctx->hdr.client_id);
    assert(ctx->server_id == ctx->hdr.server_id);
    DBG("Notification header received from peer #%ld, type: %ld (client_id: %" PRIu64 ", server_id: %" PRIu64 ")",
        ctx->hdr.id, ctx->hdr.type, ctx->client_id, ctx->server_id);
    ctx->complete = true;
    post_recv_for_notif_payload(ctx, (execution_context_t *)ctx->econtext, ctx->hdr.id);
}
#endif // !USE_AM_IMPLEM

#endif // DPU_OFFLOAD_COMMS_H_
