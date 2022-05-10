//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include "dpu_offload_service_daemon.h"

#ifndef DPU_OFFLOAD_COMMS_H_
#define DPU_OFFLOAD_COMMS_H_

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
    SYS_EVENT_LOCK(econtext->event_channels);
    notification_callback_entry_t *entry = DYN_ARRAY_GET_ELT(&(econtext->event_channels->notification_callbacks), hdr->type, notification_callback_entry_t);
    DBG("Notification of type %" PRIu64 " received (econtext: %p), dispatching...", hdr->type, econtext);
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
            pending_notif->data = MALLOC(pending_notif->data_size);
            CHECK_ERR_RETURN((pending_notif->data == NULL), DO_ERROR, "unable to allocate pending notification's data");
            memcpy(pending_notif->data, data, pending_notif->data_size);
        }
        else
        {
            pending_notif->data = NULL;
        }
        if (pending_notif->header_size > 0)
        {
            pending_notif->header = MALLOC(pending_notif->header_size);
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
    hdr_notif_req_t *ctx = (hdr_notif_req_t *)user_data;
    assert(ctx);
    assert(ctx->econtext);
    DBG("Notification payload received, ctx=%p econtext=%p type=%ld\n", ctx, ctx->econtext, ctx->hdr.type);
    ctx->payload_ctx.complete = true;

    // Invoke the associated callback
    handle_notif_msg(ctx->econtext, &(ctx->hdr), sizeof(am_header_t), ctx->payload_ctx.buffer, ctx->hdr.payload_size);
    if (ctx->hdr.payload_size)
    {
        free(ctx->payload_ctx.buffer);
        ctx->payload_ctx.buffer = NULL;
    }
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
}

/**
 * @brief Note that the function assumes the execution context is not locked before it is invoked.
 *
 * @param ctx
 * @param econtext
 * @param peer_id
 */
static void post_recv_for_notif_payload(hdr_notif_req_t *ctx, execution_context_t *econtext, uint64_t peer_id)
{
    assert(ctx);
    assert(econtext);
    DBG("Notification header received, payload size = %ld, type = %ld, econtext = %p-%p, ctx = %p",
        ctx->hdr.payload_size,
        ctx->hdr.type,
        ctx->econtext,
        econtext,
        ctx);
    assert(econtext == ctx->econtext);

    if (ctx->hdr.payload_size > 0 && ctx->payload_ctx.req != NULL)
    {
        WARN_MSG("We got a new header but still waiting for a previous notification payload");
        return;
    }

    if (ctx->hdr.payload_size > 0 && ctx->payload_ctx.req == NULL)
    {
        DBG("Posting recv for notif payload of size %ld for peer %ld (client_id: %" PRIu64 ", server_id: %" PRIu64 ")",
            ctx->hdr.payload_size, peer_id, ctx->client_id, ctx->server_id);
        ucp_worker_h worker;

        ucp_tag_t payload_ucp_tag, payload_ucp_tag_mask;
        ctx->payload_ctx.buffer = MALLOC(ctx->hdr.payload_size);
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
            // Recv completed immediately, the callback is not invoked
            DBG("Recv of the payload completed right away");
            ctx->payload_ctx.complete = true;
            handle_notif_msg(ctx->econtext, &(ctx->hdr), sizeof(am_header_t), ctx->payload_ctx.buffer, ctx->hdr.payload_size);
            if (ctx->hdr.payload_size)
            {
                free(ctx->payload_ctx.buffer);
                ctx->payload_ctx.buffer = NULL;
            }
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
        }

        if (UCS_PTR_IS_ERR(ctx->payload_ctx.req))
        {
            ucs_status_t recv_status = UCS_PTR_STATUS(ctx->payload_ctx.req);
            ERR_MSG("ucp_tag_recv_nbx() failed: %s", ucs_status_string(recv_status));
        }
    }
    else
    {
        handle_notif_msg(econtext, &(ctx->hdr), sizeof(am_header_t), NULL, 0);
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
    }
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
 */
static void post_new_notif_recv(ucp_worker_h worker, hdr_notif_req_t *ctx, execution_context_t *econtext, ucp_tag_t hdr_ucp_tag, ucp_tag_t hdr_ucp_tag_mask, ucp_request_param_t *hdr_recv_param)
{
    // Post a new receive only if we are not already in the middle of receiving a notification
    if (ctx->complete == true && ctx->payload_ctx.complete == true)
    {
        assert(worker);
        // Post the receive for the header
        ctx->complete = false;
        ctx->econtext = (struct execution_context *)econtext;
        DBG("-------------> Posting recv for notif header (econtext: %p, scope_id: %d, worker: %p, client_id: %" PRIu64 ", server_id: %" PRIu64 ")",
            econtext, econtext->scope_id, worker, ctx->client_id, ctx->server_id);
        ctx->req = ucp_tag_recv_nbx(worker, &(ctx->hdr), sizeof(am_header_t), hdr_ucp_tag, hdr_ucp_tag_mask, hdr_recv_param);
        if (ctx->req == NULL)
        {
            // Receive completed immediately, callback is not called
            DBG("Recv of notification header completed right away, notif type: %ld", ctx->hdr.type);
            ctx->complete = true;
            post_recv_for_notif_payload(ctx, (execution_context_t *)ctx->econtext, ctx->hdr.id);
        }
    }
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
    DBG("Notification header received from peer #%ld, type: %ld (client_id: %" PRIu64 ", server_id: %" PRIu64 ")",
        ctx->hdr.id, ctx->hdr.type, ctx->client_id, ctx->server_id);
    ctx->complete = true;
    post_recv_for_notif_payload(ctx, (execution_context_t *)ctx->econtext, ctx->hdr.id);
}

#endif // DPU_OFFLOAD_COMMS_H_
