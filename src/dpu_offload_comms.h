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
#define PREP_NOTIF_RECV(ctx, hdr_recv_param, hdr_ucp_tag, hdr_ucp_tag_mask, worker, peer_id) \
    do                                                                                       \
    {                                                                                        \
        /* Always have a recv posted so we are ready to get a header from the other side. */ \
        /* Remember we are using one thread per bootstrap client/server. */                  \
        /* Just to make sure the initial receive will post, we mark the two receives for */  \
        /* any notifications as completed */                                                 \
        ctx.complete = true;                                                                 \
        ctx.payload_ctx.complete = true;                                                     \
        ctx.recv_peer_id = peer_id;                                                          \
        hdr_recv_param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |                           \
                                      UCP_OP_ATTR_FIELD_DATATYPE |                           \
                                      UCP_OP_ATTR_FIELD_USER_DATA;                           \
        hdr_recv_param.datatype = ucp_dt_make_contig(1);                                     \
        hdr_recv_param.user_data = &ctx;                                                     \
        hdr_recv_param.cb.recv = notif_hdr_recv_handler;                                     \
        MAKE_RECV_TAG(hdr_ucp_tag, hdr_ucp_tag_mask, AM_EVENT_MSG_HDR_ID, peer_id, 0, 0, 0); \
        worker = GET_WORKER(econtext);                                                       \
        DBG("Reception of notification for peer %ld is now all set", peer_id);               \
    } while (0)

static int handle_notif_msg(execution_context_t *econtext, am_header_t *hdr, size_t header_length, void *data, size_t length)
{
    assert(econtext);
    assert(hdr);
    DBG("econtext = %p", econtext);
    assert(econtext->event_channels);
    SYS_EVENT_LOCK(econtext->event_channels);
    notification_callback_entry_t *list_callbacks = (notification_callback_entry_t *)econtext->event_channels->notification_callbacks.base;
    assert(list_callbacks);
    DBG("Notification of type %" PRIu64 " received, dispatching...", hdr->type);
    assert(hdr->type < econtext->event_channels->notification_callbacks.num_elts);
    if (list_callbacks[hdr->type].set == false)
    {
        pending_notification_t *pending_notif;
        DBG("callback not available for %" PRIu64, hdr->type);
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

    notification_cb cb = list_callbacks[hdr->type].cb;
    CHECK_ERR_GOTO((cb == NULL), error_out, "Callback is undefined");
    struct dpu_offload_ev_sys *ev_sys = EV_SYS(econtext);
    // Callbacks are responsible for handling any necessary locking
    // and can call any event API so we unlock before invoking it.
    SYS_EVENT_UNLOCK(econtext->event_channels);
    cb(ev_sys, econtext, hdr, header_length, data, length);
    return UCS_OK;
error_out:
    SYS_EVENT_UNLOCK(econtext->event_channels);
    return UCS_ERR_NO_MESSAGE;
}

static void notif_payload_recv_handler(void *request, ucs_status_t status, const ucp_tag_recv_info_t *tag_info, void *user_data)
{
    hdr_notif_req_t *ctx = (hdr_notif_req_t *)user_data;
    DBG("Notification payload received, ctx=%p econtext=%p type=%ld\n", ctx, ctx->econtext, ctx->hdr.type);
    ctx->payload_ctx.complete = 1;
    assert(ctx->econtext);

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

static void post_recv_for_notif_payload(hdr_notif_req_t *ctx, execution_context_t *econtext, uint64_t peer_id)
{
    assert(ctx);
    assert(econtext);
    ECONTEXT_LOCK(econtext);
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
        DBG("Posting recv for notif payload of size %ld for peer %ld", ctx->hdr.payload_size, peer_id);
        ucp_worker_h worker;

        ucp_tag_t payload_ucp_tag, payload_ucp_tag_mask;
        ucp_request_param_t payload_recv_param;
        ctx->payload_ctx.buffer = MALLOC(ctx->hdr.payload_size);
        assert(ctx->payload_ctx.buffer);
        worker = GET_WORKER(econtext);
        assert(worker);
        // Post the receive for the payload
        ctx->payload_ctx.complete = false;
        ctx->payload_ctx.recv_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                                                    UCP_OP_ATTR_FIELD_DATATYPE |
                                                    UCP_OP_ATTR_FIELD_USER_DATA;
        ctx->payload_ctx.recv_params.datatype = ucp_dt_make_contig(1);
        ctx->payload_ctx.recv_params.user_data = ctx;
        ctx->payload_ctx.recv_params.cb.recv = notif_payload_recv_handler;
        MAKE_RECV_TAG(payload_ucp_tag, payload_ucp_tag_mask, AM_EVENT_MSG_ID, peer_id, 0, 0, 0);
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
            ctx->payload_ctx.complete = 1;
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
    ECONTEXT_UNLOCK(econtext);
}

static void post_new_notif_recv(ucp_worker_h worker, hdr_notif_req_t *ctx, execution_context_t *econtext, ucp_tag_t hdr_ucp_tag, ucp_tag_t hdr_ucp_tag_mask, ucp_request_param_t *hdr_recv_param)
{
    // Post a new receive only if we are not already in the middle of receiving a notification
    if (ctx->complete == true && ctx->payload_ctx.complete == true)
    {
        // Post the receive for the header
        ctx->complete = false;
        ctx->econtext = (struct execution_context *)econtext;
        DBG("-------------> Posting recv for notif header");
        ctx->req = ucp_tag_recv_nbx(worker, &(ctx->hdr), sizeof(am_header_t), hdr_ucp_tag, hdr_ucp_tag_mask, hdr_recv_param);
        if (ctx->req == NULL)
        {
            // Receive completed immediately, callback is not called
            DBG("Recv of notification header completed right away, notif type: %ld", ctx->hdr.type);
            ctx->complete = true;
            ECONTEXT_UNLOCK(econtext);
            post_recv_for_notif_payload(ctx, (execution_context_t *)ctx->econtext, ctx->recv_peer_id);
        }
    }
}

static void notif_hdr_recv_handler(void *request, ucs_status_t status, const ucp_tag_recv_info_t *tag_info, void *user_data)
{
    hdr_notif_req_t *ctx = (hdr_notif_req_t *)user_data;
    DBG("Notification header received from peer #%ld, type: %ld\n", ctx->recv_peer_id, ctx->hdr.type);
    ctx->complete = true;
    post_recv_for_notif_payload(ctx, (execution_context_t *)ctx->econtext, ctx->recv_peer_id);
}

static ucs_status_t ucx_wait(ucp_worker_h ucp_worker, struct ucx_context *request,
                             const char *op_str, const char *data_str)
{
    ucs_status_t status;

    DBG("Waiting for request %p to complete", request);
    if (request == NULL)
    {
        // The request completed right away, nothing to do.
        return UCS_OK;
    }
    assert(ucp_worker);

    if (UCS_PTR_IS_ERR(request))
    {
        status = UCS_PTR_STATUS(request);
    }
    else if (UCS_PTR_IS_PTR(request))
    {
        while (!request->completed)
        {
            ucp_worker_progress(ucp_worker);
        }

        request->completed = 0;
        status = ucp_request_check_status(request);
        ucp_request_free(request);
    }
    else
    {
        status = UCS_OK;
    }

    if (status != UCS_OK)
    {
        ERR_MSG("unable to %s %s (%s)", op_str, data_str, ucs_status_string(status));
    }
    else
    {
        if (data_str != NULL)
            DBG("%s completed, data=%s", op_str, data_str);
        else
            DBG("%s completed", op_str);
    }

    return status;
}

static ucs_status_t request_wait(ucp_worker_h ucp_worker, void *request, am_req_t *ctx)
{
    ucs_status_t status;

    /* if operation was completed immediately */
    if (request == NULL)
    {
        return UCS_OK;
    }

    if (UCS_PTR_IS_ERR(request))
    {
        return UCS_PTR_STATUS(request);
    }

    while (ctx->complete == 0)
    {
        ucp_worker_progress(ucp_worker);
    }
    status = ucp_request_check_status(request);

    ucp_request_free(request);

    return status;
}

static dpu_offload_status_t request_finalize(ucp_worker_h ucp_worker, void *request, am_req_t *ctx)
{
    ucs_status_t status = request_wait(ucp_worker, request, ctx);
    CHECK_ERR_RETURN((status != UCS_OK), DO_ERROR, "request failed: %s", ucs_status_string(status));
    return DO_SUCCESS;
}

#endif // DPU_OFFLOAD_COMMS_H_