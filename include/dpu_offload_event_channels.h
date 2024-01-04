//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>

#include "dpu_offload_types.h"

#ifndef DPU_OFFLOAD_EVENT_CHANNELS_H_
#define DPU_OFFLOAD_EVENT_CHANNELS_H_

#define QUEUE_EVENT(__ev)                                                            \
    do                                                                               \
    {                                                                                \
        if ((__ev)->explicit_return)                                                 \
        {                                                                            \
            /* The event is flagged for manual management, do not queue */           \
        }                                                                            \
        else                                                                         \
        {                                                                            \
            if ((__ev)->is_subevent == false)                                        \
            {                                                                        \
                assert((__ev)->is_ongoing_event == false);                           \
                ucs_list_add_tail(&((__ev)->event_system->econtext->ongoing_events), \
                                  &((__ev)->item));                                  \
                (__ev)->is_ongoing_event = true;                                     \
            }                                                                        \
        }                                                                            \
    } while (0)

#define QUEUE_SUBEVENT(_metaev, _ev)                                  \
    do                                                                \
    {                                                                 \
        assert((_ev)->is_subevent == true);                           \
        assert((_ev)->is_ongoing_event == false);                     \
        if (!(_metaev)->sub_events_initialized)                       \
        {                                                             \
            ucs_list_head_init(&((_metaev)->sub_events));             \
            (_metaev)->sub_events_initialized = true;                 \
        }                                                             \
        ucs_list_add_tail(&((_metaev)->sub_events), &((_ev)->item));  \
    } while (0)

#if USE_AM_IMPLEM
#define COMPLETE_EVENT(__ev)                                          \
    do                                                                \
    {                                                                 \
        (__ev)->ctx.complete = true;                                  \
        if ((__ev)->ctx.completion_cb != NULL)                        \
        {                                                             \
            /* Reset the ev's callback before invoking it so we */    \
            /* can enforce a callback to be called only once and */   \
            /* prevent function re-entering issues during complex */  \
            /* completions */                                         \
            request_compl_cb_t _cb = (__ev)->ctx.completion_cb;       \
            (__ev)->ctx.completion_cb = NULL;                         \
            _cb((__ev)->ctx.completion_cb_ctx);                       \
            (__ev)->ctx.completion_cb_ctx = NULL;                     \
        }                                                             \
    } while (0)
#else
#define COMPLETE_EVENT(__ev)                                          \
    do                                                                \
    {                                                                 \
        (__ev)->ctx.hdr_completed = true;                             \
        (__ev)->ctx.payload_completed = true;                         \
        if ((__ev)->ctx.completion_cb != NULL)                        \
        {                                                             \
            (__ev)->ctx.completion_cb((__ev)->ctx.completion_cb_ctx); \
            (__ev)->ctx.completion_cb = NULL;                         \
        }                                                             \
    } while (0)
#endif

static inline bool send_moderation_on()
{
    return true;
}

#if USE_AM_IMPLEM
int do_am_send_event_msg(dpu_offload_event_t *event);
#else
int do_tag_send_event_msg(dpu_offload_event_t *event);
#endif

dpu_offload_status_t event_channels_init(execution_context_t *);
dpu_offload_status_t ev_channels_init(dpu_offload_ev_sys_t **ev_channels);
void event_channels_fini(dpu_offload_ev_sys_t **);

/**
 * @brief Register a handler in the context of an event system for a specific type of notifications.
 * Only one handler can be registered per notification type. If registration is performed multiple times,
 * only the first one is active. Updates are possible using the event_channel_update function.
 * It is safe to emit a new event from a handler.
 * Note: the function assumes the event system is locked before it is invoked
 *
 * @param ev_sys Event system to use to register the notification handler.
 * @param type Notification type.
 * @param cb Callback to invoke upon reception of the notification.
 * @param info Optional information related to the handling of the notification (can be set to NULL).
 * @return dpu_offload_status_t
 */
dpu_offload_status_t event_channel_register(dpu_offload_ev_sys_t *ev_sys, uint64_t type, notification_cb cb, notification_info_t *info);

dpu_offload_status_t event_channel_update(dpu_offload_ev_sys_t *ev_sys, uint64_t type, notification_info_t *info);

dpu_offload_status_t event_channel_deregister(dpu_offload_ev_sys_t *ev_sys, uint64_t type);

/**
 * @brief Register a handler for one of MIMOSA's internal event. As a reminder, an internal event
 * cannot be used in the context of emits. In other words, internal events are used to let the
 * caller be notified of specific internal MIMOSA's state change, e.g., a group being revoked.
 * Only one callback can be registered, meaning only the first registeration succeeds and any
 * other registeration will fail.
 *
 * @param[in] engine Associated engine.
 * @param[in] type Identifier of the internal MIMOSA event.
 * @param[in] cb Callback to register.
 * @return dpu_offload_status_t
 */
dpu_offload_status_t mimosa_internal_event_register(offloading_engine_t *engine, uint64_t type, notification_cb cb);

/**
 * @brief Deregister a handler for one of MIMOSA's internal event.
 *
 * @param engine Associated engine.
 * @param type Identifier of the internal MIMOSA event.
 * @return dpu_offload_status_t
 */
dpu_offload_status_t mimosa_internal_event_deregister(offloading_engine_t *engine, uint64_t type);

void *get_notif_buf(dpu_offload_ev_sys_t *ev_sys, uint64_t type);

notification_callback_entry_t *get_notif_callback_entry(dpu_offload_ev_sys_t *ev_sys, uint64_t type);

/**
 * @brief Registers a default handler at the engine level. After the registration, the handler will be
 * automatically added to all execution context that will be created. It is not applied to execution
 * contexts that were already previously created. The default handlers cannot be deregistered.
 *
 * @param engine Engine to which the default handler applies.
 * @param type Event type, i.e., identifier of the callback to invoke when the event is delivered at destination.
 * @param cb Callback to invoke upon reception of the notification.
 * @param info Optional information related to the handling of the notification (can be set to NULL).
 * @return dpu_offload_status_t
 */
dpu_offload_status_t engine_register_default_notification_handler(offloading_engine_t *engine, uint64_t type, notification_cb cb, notification_info_t *info);

dpu_offload_status_t engine_update_default_notification_handler(offloading_engine_t *engine, uint64_t type, notification_info_t *info);

/**
 * @brief event_channel_emit triggers the communication associated to a previously locally defined event.
 *
 * @param ev Event to be emitted. The object needs to be fully initialized prior the invokation of the function (see 'event_get()' and 'event_return()').
 * @param type Event type, i.e., identifier of the callback to invoke when the event is delivered at destination.
 * @param dest_id The unique identifier to be used to send the event. It is used to identify the source of the event.
 * @param dest_ep Endpoint of the target of the event.
 * @param ctx User-defined context to help identify the context of the event upon local completion.
 * @return result of UCS_PTR_STATUS in the context of errors,
 * @return EVENT_DONE if the emittion completed right away,
 * @return EVENT_INPROGRESS if emitting the event is still in progress (e.g., communication not completed).
 *
 * One can check on completion using ev->req.
 */
int event_channel_emit(dpu_offload_event_t **ev, uint64_t type, ucp_ep_h dest_ep, uint64_t dest_id, void *ctx);

/**
 * @brief event_channel_emit_with_payload triggers the communication associated to a previously
 * locally defined event, specifying a payload to be sent during the emit.
 * This function assumes the caller is responsible for the management of the payload buffer, which
 * must remain available until the event completes.
 *
 * @param ev Event to be emitted. The object needs to be fully initialized prior the invokation of the function (see 'event_get()' and 'event_return()').
 * @param type Event type, i.e., identifier of the callback to invoke when the event is delivered at destination.
 * @param dest_ep Endpoint of the target of the event.
 * @param dest_id The unique identifier to be used to send the event. It is used to identify the source of the event.
 * @param ctx User-defined context to help identify the context of the event upon local completion.
 * @param payload User-defined payload associated to the event.
 * @param payload_size Size of the user-defined payload.
 * @return result of UCS_PTR_STATUS in the context of errors during communications.
 * @return DO_ERROR in case of an error during the library's handling of the event.
 * @return EVENT_DONE if the emittion completed right away, the library returns the event.
 * @return EVENT_INPROGRESS if emitting the event is still in progress (e.g., communication not completed). One can check on completion using event_completed().
 *
 * Example: the user uses the ongoing events list so that the event is implicitly returned when completed, with an event payload managed outside of the offloading library.
 * @code{.unparsed}
 *      dpu_offload_event_t *my_ev;
 *      event_get(ev_sys, NULL, &my_ev);
 *      int rc = event_channel_emit_with_payload(my_ev, my_notification_type, dest_ep, dest_id, NULL, &my_global_static_object, object_size);
 *      if (rc != EVENT_DONE && rc != EVENT_INPROGRESS) {
 *          // Event is ongoing, placed on the ongoing event list. The event will be automatically removed from the list and returned during progress of the execution context upon completion.
 *          ucs_list_add_tail(&(econtext->ongoing_events), &(myev->item));
 *      } else {
 *          // Error case
 *          return -1;
 *      }
 * @endcode
 */
int event_channel_emit_with_payload(dpu_offload_event_t **ev, uint64_t type, ucp_ep_h dest_ep, uint64_t dest_id, void *ctx, void *payload, size_t payload_size);

/**
 * @brief Get an event from a pool of event. Using a pool of events prevents dynamic allocations.
 *
 * @param ev_sys The event system from which the event must come from.
 * @param info Information about the event. For instance, it is possible to ask the library to allocate/free the memory for a payload. See examples for details.
 * @param ev Returned event.
 * @return result of UCS_PTR_STATUS in the context of errors.
 * @return DO_ERROR in case of an error during the library's handling of the event.
 * @return EVENT_DONE if the emittion completed right away, the library returns the event.
 * @return EVENT_INPROGRESS if emitting the event is still in progress (e.g., communication not completed). One can check on completion using event_completed().
 *
 * Examples:
 *  - get an event that does not require a payload.
 * @code{.unparsed}
 *      event_get(ev_sys, NULL, &myev);
 * @endcode
 *  - get and emit an event with a payload that the caller entirely control.
 * @code{.unparsed}
 *      event_get(ev_sys, NULL, &myev);
 *      int rc = event_channel_emit_with_payload(myev, dest_ep, dest_id, NULL, my_payload, my_payload_size);
 *      if (rc == EVENT_DONE)
 *      {
 *          // Event completed right away
 *          free(my_payload);
 *      }
 *      else if (rc != EVENT_INPROGRESS)
 *          // Error
 *          return -1;
 *      }
 * @endcode
 * - get an event and let the library gives me a buffer for the payload; the library will free the payload when the event is returned.
 * @code{.unparsed}
 *      dpu_offload_event_info_t ev_info = {
 *          .payload_size = my_payload_size,
 *      };
 *      event_get(ev_sys, &ev_info, &myev);
 *      memcpy(myev->payload, my_data, sizeof(my_payload_size));
 *      int rc = event_channel_emit(myev, dest_ep, dest_id, NULL);
 *      if (rc != EVENT_DONE && rc != EVENT_INPROGRESS)
 *          // Error
 *          return -1;
 *      }
 * @endcode
 */
dpu_offload_status_t event_get(dpu_offload_ev_sys_t *ev_sys, dpu_offload_event_info_t *info, dpu_offload_event_t **ev);

/**
 * @brief Return an event once completed
 *
 * @param ev Pointer to the event to return. The event is set to NULL upon success so the event handle cannot be used anymore.
 * @return DO_SUCCESS
 * @return DO_ERROR
 */
dpu_offload_status_t event_return(dpu_offload_event_t **ev);

/**
 * @brief event_completed is a helper function to check whether an event is completed.
 * The function is aware of sub-events. If all the sub-events are completed and the
 * request of the event is NULL, the event is reported as completed. If the event has a
 * completion callback setup, the callback is invoked.
 * The caller is responsible for returning the event when reported as completed.
 *
 * @param ev Event to check for completion.
 * @return true when the event is completed
 * @return false when the event is still in progress
 */
bool event_completed(dpu_offload_event_t *ev);

dpu_offload_status_t send_term_msg(execution_context_t *ctx, dest_client_t *dest_info);

/* FIXME: those should not be there */
/**
 * @brief send_revoke_group_to_ranks sends a group revoke message that specifies how many ranks revoked the said group.
 * 
 * @param engine Offload engine associated to the revoke.
 * @param gp_uid Group UID associated to the revoke.
 * @param num_ranks Number of ranks that already revoked the group.
 * @return dpu_offload_status_t 
 */
dpu_offload_status_t send_revoke_group_to_ranks(offloading_engine_t *engine, group_uid_t gp_uid, uint64_t num_ranks);

dpu_offload_status_t handle_pending_group_cache_add_msgs(group_cache_t *group_cache);
/* END FIXME */

#endif // DPU_OFFLOAD_EVENT_CHANNELS_H_
