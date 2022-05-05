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

// Set to 1 to use the AM implementaton; 0 to use tag send/recv implementation
#define USE_AM_IMPLEM (0)

#if USE_AM_IMPLEM
#define COMPLETE_EVENT(__ev)                                          \
    do                                                                \
    {                                                                 \
        (__ev)->ctx.complete = true;                                  \
        (__ev)->payload_ctx.complete = true;                          \
        if ((__ev)->ctx.completion_cb != NULL)                        \
        {                                                             \
            (__ev)->ctx.completion_cb((__ev)->ctx.completion_cb_ctx); \
            (__ev)->ctx.completion_cb = NULL;                         \
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

dpu_offload_status_t event_channels_init(execution_context_t *);
dpu_offload_status_t ev_channels_init(dpu_offload_ev_sys_t **ev_channels);
dpu_offload_status_t event_channels_fini(dpu_offload_ev_sys_t **);

dpu_offload_status_t event_channel_register(dpu_offload_ev_sys_t *ev_sys, uint64_t type, notification_cb cb);
dpu_offload_status_t event_channel_deregister(dpu_offload_ev_sys_t *ev_sys, uint64_t type);

/**
 * @brief Registers a default handler at the engine level. After the registration, the handler will be
 * automatically added to all execution context that will be created. It is not applied to execution
 * contexts that were already previously created. The default handlers cannot be deregistered.
 *
 * @param engine Engine to which the default handler applies.
 * @param type Event type, i.e., identifier of the callback to invoke when the event is delivered at destination.
 * @param cb Callback to invoke upon reception of the notification.
 * @return dpu_offload_status_t
 */
dpu_offload_status_t engine_register_default_notification_handler(offloading_engine_t *engine, uint64_t type, notification_cb cb);

/**
 * @brief event_channel_emit triggers the communication associated to a previously locally defined event.
 *
 * @param ev Event to be emitted. The object needs to be fully initialized prior the invokation of the function (see 'event_get()' and 'event_return()').
 * @param my_id The unique identifier to be used to send the event. It is used to identify the source of the event.
 * @param type Event type, i.e., identifier of the callback to invoke when the event is delivered at destination.
 * @param dest_ep Endpoint of the target of the event.
 * @param ctx User-defined context to help identify the context of the event upon local completion.
 * @return result of UCS_PTR_STATUS in the context of errors, EVENT_DONE if the emittion completed right away, EVENT_INPROGRESS if emitting the event is still in progress (e.g., communication not completed). One can check on completion using ev->req.
 */
int event_channel_emit(dpu_offload_event_t **ev, uint64_t my_id, uint64_t type, ucp_ep_h dest_ep, void *ctx);

/**
 * @brief event_channel_emit_with_payload triggers the communication associated to a previously
 * locally defined event, specifying a payload to be sent during the emit.
 * This function assumes the caller is responsible for the management of the payload buffer, which
 * must remain available until the event completes.
 *
 * @param ev Event to be emitted. The object needs to be fully initialized prior the invokation of the function (see 'event_get()' and 'event_return()').
 * @param my_id The unique identifier to be used to send the event. It is used to identify the source of the event.
 * @param type Event type, i.e., identifier of the callback to invoke when the event is delivered at destination.
 * @param dest_ep Endpoint of the target of the event.
 * @param ctx User-defined context to help identify the context of the event upon local completion.
 * @param payload User-defined payload associated to the event.
 * @param payload_size Size of the user-defined payload.
 * @return result of UCS_PTR_STATUS in the context of errors during communications.
 * @return DO_ERROR in case of an error during the library's handling of the event.
 * @return EVENT_DONE if the emittion completed right away, the library returns the event.
 * @return EVENT_INPROGRESS if emitting the event is still in progress (e.g., communication not completed). One can check on completion using event_completed().
 *
 * Example: the user uses the ongoing events list so that the event is implicitly returned when completed, with an event payload managed outside of the offloading library.
 *      dpu_offload_event_t *my_ev;
 *      event_get(ev_sys, NULL, &my_ev);
 *      int rc = event_channel_emit_with_payload(my_ev, my_id, my_notification_type, dest_ep, NULL, &my_global_static_object, object_size);
 *      if (rc == EVENT_DONE)
 *      {
 *          // Event completed right away. Nothing else to do.
 *      } else if (rc == EVENT_INPROGRESS) {
 *          // Event is ongoing, placed on the ongoing event list. The event will be automatically removed from the list and returned during progress of the execution context upon completion.
 *          ucs_list_add_tail(&(econtext->ongoing_events), &(myev->item));
 *      } else {
 *          // Error case
 *          return -1;
 *      }
 */
int event_channel_emit_with_payload(dpu_offload_event_t **ev, uint64_t my_id, uint64_t type, ucp_ep_h dest_ep, void *ctx, void *payload, size_t payload_size);

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
 *      event_get(ev_sys, NULL, &myev);
 *  - get and emit an event with a payload that the caller entirely control.
 *      event_get(ev_sys, NULL, &myev);
 *      int rc = event_channel_emit_with_payload(myev, econtext_id, dest_ep, NULL, my_payload, my_payload_size);
 *      if (rc == EVENT_DONE)
 *      {
 *          // Event completed right away
 *          free(my_payload);
 *      } else if (rc == EVENT_INPROGRESS)
 *          // Nothing to do, event is in progress. The user will check for completion using event_completed()
 *      } else {
 *          // Error
 *          return -1;
 *      }
 * - get an event and let the library gives me a buffer for the payload; the library will free the payload when the event is returned.
 *      dpu_offload_event_info_t ev_info = {
 *          .payload_size = my_payload_size,
 *      };
 *      event_get(ev_sys, &ev_info, &myev);
 *      memcpy(myev->payload, my_data, sizeof(my_payload_size));
 *      int rc = event_channel_emit(myev, econtext_id, dest_ep, NULL);
 *      if (rc == EVENT_DONE)
 *      {
 *          // Event completed right away. Nothing else to do.
 *      } else if (rc == EVENT_INPROGRESS)
 *          // Nothing to do, event is in progress
 *          ucs_list_add_tail(&(econtext->ongoing_events), &(myev->item));
 *      } else {
 *          // Error
 *          return -1;
 *      }
 */
dpu_offload_status_t event_get(dpu_offload_ev_sys_t *ev_sys, dpu_offload_event_info_t *info, dpu_offload_event_t **ev);
dpu_offload_status_t event_return(dpu_offload_event_t **ev);

/**
 * @brief event_completed checks whether an event is completed or not. If the event is completed,
 * it is also implicitly returned.
 *
 * @param ev Event to check
 * @return true
 * @return false
 */
bool event_completed(dpu_offload_event_t *ev);

#endif // DPU_OFFLOAD_EVENT_CHANNELS_H_
