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

/**
 * @brief Note: the function assumes that:
 * - the execution context is not locked before the function is invoked
 * - the event system is not locked beffore the function is invoked
 * @return int
 */
static int handle_notif_msg(execution_context_t *econtext, notif_reception_t *recv_info)
{
    assert(econtext);
    assert(econtext->event_channels);
    if (recv_info->header.payload_size > 0 && recv_info->buffer == NULL)
    {
        ERR_MSG("the payload is %" PRIu64 " but the buffer is NULL", recv_info->header.payload_size);
        return UCS_ERR_NO_MESSAGE;
    }
    SYS_EVENT_LOCK(econtext->event_channels);
    notification_callback_entry_t *entry = DYN_ARRAY_GET_ELT(&(econtext->event_channels->notification_callbacks),
                                                             recv_info->header.type,
                                                             notification_callback_entry_t);
    DBG("Notification of type %" PRIu64 " received from %" PRIu64 " (econtext: %p), dispatching...",
        recv_info->header.type,
        recv_info->header.id,
        econtext);
    if (entry->set == false)
    {
        pending_notification_t *pending_notif;
        DBG("callback not available for %" PRIu64 " on event system %p (econtext: %p)",
            recv_info->header.type, econtext->event_channels, econtext);
        DYN_LIST_GET(econtext->event_channels->free_pending_notifications, pending_notification_t, item, pending_notif);
        CHECK_ERR_RETURN((pending_notif == NULL), UCS_ERR_NO_MESSAGE, "unable to get pending notification object");
        RESET_PENDING_NOTIF(pending_notif);
        pending_notif->type = recv_info->header.type;
        pending_notif->src_id = recv_info->header.id;
        pending_notif->data_size = recv_info->header.payload_size;
        pending_notif->header_size = sizeof(recv_info->header);
        pending_notif->econtext = econtext;
        if (pending_notif->data_size > 0)
        {
            pending_notif->data = MALLOC(pending_notif->data_size);
            CHECK_ERR_RETURN((pending_notif->data == NULL), DO_ERROR, "unable to allocate pending notification's data");
            memcpy(pending_notif->data, recv_info->buffer, pending_notif->data_size);
        }
        else
        {
            pending_notif->data = NULL;
        }
        if (pending_notif->header_size > 0)
        {
            pending_notif->header = MALLOC(pending_notif->header_size);
            CHECK_ERR_RETURN((pending_notif->header == NULL), DO_ERROR, "unable to allocate pending notification's header");
            memcpy(pending_notif->header, &(recv_info->header), pending_notif->header_size);
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
    cb(ev_sys, econtext, &(recv_info->header), sizeof(recv_info->header), recv_info->buffer, recv_info->header.payload_size);
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
    assert(status == UCS_OK);
    notif_req_t *ctx = (notif_req_t *)user_data;
    assert(ctx);
    ctx->completed = 1;
    DBG("Payload received (ctx: %p)", ctx);
}

/**
 * @brief Note that the function assumes the execution context is not locked before it is invoked.
 *
 * @param ctx
 * @param econtext
 * @param peer_id
 */
static void post_payload_notif_recv(execution_context_t *econtext, notif_reception_t *recv_info)
{
    assert(recv_info);
    assert(econtext);
    DBG("Notification header received, payload size = %ld, type = %ld, econtext = %p-%p, ctx = %p",
        recv_info->header.payload_size,
        recv_info->header.type,
        recv_info->econtext,
        econtext,
        recv_info);
    assert(econtext == recv_info->econtext);
    assert(recv_info->hdr_ctx.status.completed == true); // The header should be completed

    if (recv_info->header.payload_size > 0)
    {
        DBG("Posting recv for notif payload of size %ld (client_id: %" PRIu64 ", server_id: %" PRIu64 ")",
            recv_info->header.payload_size, recv_info->client_id, recv_info->server_id);
        ucp_worker_h worker = GET_WORKER(econtext);
        recv_info->buffer = MALLOC(recv_info->header.payload_size);
        assert(recv_info->buffer);
        assert(worker);
        // Post the receive for the payload
        DBG("Tag: %d; scope_id: %u", AM_EVENT_MSG_ID, econtext->scope_id);
        recv_info->payload_ctx.req = ucp_tag_recv_nbx(worker,
                                                recv_info->buffer,
                                                recv_info->header.payload_size,
                                                recv_info->payload_ctx.ucp_tag,
                                                recv_info->payload_ctx.ucp_tag_mask,
                                                &(recv_info->payload_ctx.recv_params));
        if (recv_info->payload_ctx.req == NULL)
        {
            // Recv completed immediately, the callback is not invoked
            DBG("Recv of the payload completed right away");
            recv_info->payload_ctx.status.completed = true;
        }

        if (UCS_PTR_IS_ERR(recv_info->payload_ctx.req))
        {
            ucs_status_t recv_status = UCS_PTR_STATUS(recv_info->payload_ctx.req);
            ERR_MSG("ucp_tag_recv_nbx() failed: %s", ucs_status_string(recv_status));
        }
    }
    else
    {
        recv_info->payload_ctx.status.completed = true;
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
static void post_hdr_notif_recv(execution_context_t *econtext, notif_reception_t *recv_info)
{
#if !NDEBUG
    if (econtext->engine->on_dpu && econtext->scope_id == SCOPE_INTER_DPU)
    {
        if (recv_info->client_id >= econtext->engine->num_dpus)
        {
            ERR_MSG("requested client ID is invalid: %" PRIu64, recv_info->client_id);
        }
        assert(recv_info->client_id < econtext->engine->num_dpus);
    }
#endif

    ucp_worker_h worker = GET_WORKER(econtext);
    DBG("-------------> Posting recv for notif header (econtext: %p, scope_id: %d, worker: %p, client_id: %" PRIu64 ", server_id: %" PRIu64 ", size: %ld)",
        econtext, econtext->scope_id, worker, recv_info->client_id, recv_info->server_id, sizeof(am_header_t));
    struct ucx_context *req = ucp_tag_recv_nbx(worker,
                                               &(recv_info->header),
                                               sizeof(am_header_t),
                                               recv_info->hdr_ctx.ucp_tag,
                                               recv_info->hdr_ctx.ucp_tag_mask,
                                               &(recv_info->hdr_ctx.recv_params));
    if (UCS_PTR_IS_ERR(req))
    {
        ucs_status_t recv_status = UCS_PTR_STATUS(req);
        ERR_MSG("ucp_tag_recv_nbx() failed: %s", ucs_status_string(recv_status));
    }
    if (req == NULL)
    {
        assert(recv_info->client_id == recv_info->header.client_id);
        assert(recv_info->server_id == recv_info->header.server_id);

        // Receive completed immediately, callback is not called
        DBG("Recv of notification header completed right away, notif type: %ld", recv_info->header.type);
        recv_info->hdr_ctx.status.completed = true;
        recv_info->hdr_ctx.req = NULL;
    }
    else
    {
        recv_info->hdr_ctx.req = req;
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
    notif_req_t *ctx = (notif_req_t *)user_data;
    assert(status == UCS_OK);
    assert(tag_info->length == sizeof(am_header_t));
    DBG("Header received (ctx: %p)", ctx);
    ctx->completed = 1;
    // post_recv_for_notif_payload(ctx, (execution_context_t *)ctx->econtext, ctx->hdr.id);
}

#endif // DPU_OFFLOAD_COMMS_H_
