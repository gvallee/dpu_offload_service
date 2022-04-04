//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#ifndef _COMMON_TEST_PARAMS_H_
#define _COMMON_TEST_PARAMS_H_

#include <unistd.h>

#define NUM_TEST_EVTS (100000)
#define PINGPONG_NOTIF_ID (5000)

#define GET_DEST_EP(_econtext) ({                 \
    ucp_ep_h _dest_ep;                            \
    if (_econtext->type == CONTEXT_SERVER)        \
    {                                             \
        _dest_ep = GET_CLIENT_EP(_econtext, 0UL); \
    }                                             \
    else                                          \
    {                                             \
        _dest_ep = GET_SERVER_EP(_econtext);      \
    }                                             \
    _dest_ep;                                     \
})

#define REGISTER_NOTIF_CALLBACKS(_econtext)                                                                                  \
    do                                                                                                                       \
    {                                                                                                                        \
        fprintf(stderr, "Registering callback for notifications of type %d\n", AM_TEST_MSG_ID);                              \
        dpu_offload_status_t _rc = event_channel_register(_econtext->event_channels, AM_TEST_MSG_ID, basic_notification_cb); \
        if (_rc)                                                                                                             \
        {                                                                                                                    \
            fprintf(stderr, "event_channel_register() failed\n");                                                            \
            return EXIT_FAILURE;                                                                                             \
        }                                                                                                                    \
                                                                                                                             \
        /* Register a custom callback type, i.e., an ID that is not predefined and it is used for the */                     \
        /* ping-pong notification test. */                                                                                   \
        fprintf(stderr, "Registering callback for notifications of custom type %d\n", PINGPONG_NOTIF_ID);                    \
        _rc = event_channel_register(_econtext->event_channels, PINGPONG_NOTIF_ID, pingpong_notification_cb);                \
        if (_rc)                                                                                                             \
        {                                                                                                                    \
            fprintf(stderr, "event_channel_register() failed\n");                                                            \
            return EXIT_FAILURE;                                                                                             \
        }                                                                                                                    \
    } while (0)

#define EMIT_MANY_EVTS_AND_USE_ONGOING_LIST(_econtext)                                                                    \
    do                                                                                                                    \
    {                                                                                                                     \
        dpu_offload_status_t _rc;                                                                                         \
        dpu_offload_event_t **evts = (dpu_offload_event_t **)calloc(NUM_TEST_EVTS + 1, sizeof(dpu_offload_event_t *));    \
        if (evts == NULL)                                                                                                 \
        {                                                                                                                 \
            fprintf(stderr, "unable to allocate events\n");                                                               \
            return EXIT_FAILURE;                                                                                          \
        }                                                                                                                 \
                                                                                                                          \
        uint64_t *data = malloc((NUM_TEST_EVTS + 1) * sizeof(uint64_t));                                                  \
        if (data == NULL)                                                                                                 \
        {                                                                                                                 \
            fprintf(stderr, "unable to allocate data buffer\n");                                                          \
            return EXIT_FAILURE;                                                                                          \
        }                                                                                                                 \
        size_t i;                                                                                                         \
        for (i = 0; i <= NUM_TEST_EVTS; i++)                                                                              \
        {                                                                                                                 \
            data[i] = i;                                                                                                  \
        }                                                                                                                 \
                                                                                                                          \
        for (i = 0; i <= NUM_TEST_EVTS; i++)                                                                              \
        {                                                                                                                 \
            _rc = event_get(_econtext->event_channels, NULL, &(evts[i]));                                                 \
            if (_rc)                                                                                                      \
            {                                                                                                             \
                fprintf(stderr, "event_get() failed\n");                                                                  \
                return EXIT_FAILURE;                                                                                      \
            }                                                                                                             \
                                                                                                                          \
            _rc = event_channel_emit_with_payload(&(evts[i]),                                                             \
                                                  ECONTEXT_ID(_econtext),                                                 \
                                                  AM_TEST_MSG_ID,                                                         \
                                                  GET_DEST_EP(_econtext),                                                 \
                                                  NULL,                                                                   \
                                                  &(data[i]),                                                             \
                                                  sizeof(uint64_t));                                                      \
            if (_rc != EVENT_DONE && _rc != EVENT_INPROGRESS)                                                             \
            {                                                                                                             \
                fprintf(stderr, "event_channel_emit_with_payload() failed\n");                                            \
                return EXIT_FAILURE;                                                                                      \
            }                                                                                                             \
            if (_rc == EVENT_INPROGRESS)                                                                                  \
                ucs_list_add_tail(&(_econtext->ongoing_events), &(evts[i]->item));                                        \
            fprintf(stderr, "Ev #%ld = %p\n", i, evts[i]);                                                                \
        }                                                                                                                 \
                                                                                                                          \
        while (!ucs_list_is_empty(&(_econtext->ongoing_events)) != 0)                                                     \
        {                                                                                                                 \
            _econtext->progress(_econtext);                                                                               \
            fprintf(stderr, "%ld events are still on the ongoing list\n", ucs_list_length(&(_econtext->ongoing_events))); \
        }                                                                                                                 \
                                                                                                                          \
        free(data);                                                                                                       \
        data = NULL;                                                                                                      \
        free(evts);                                                                                                       \
        evts = NULL;                                                                                                      \
        fprintf(stderr, "EMIT_MANY_EVTS_AND_USE_ONGOING_LIST done\n");                                                    \
    } while (0)

#define EMIT_MANY_EVS_WITH_EXPLICIT_MGT(_econtext)                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        fprintf(stderr, "Sending %d notifications...\n", NUM_TEST_EVTS);                                               \
        size_t i;                                                                                                      \
        dpu_offload_event_t **evts = (dpu_offload_event_t **)calloc(NUM_TEST_EVTS + 1, sizeof(dpu_offload_event_t *)); \
        if (evts == NULL)                                                                                              \
        {                                                                                                              \
            fprintf(stderr, "unable to allocate events\n");                                                            \
            return EXIT_FAILURE;                                                                                       \
        }                                                                                                              \
        uint64_t *data = malloc((NUM_TEST_EVTS + 1) * sizeof(uint64_t));                                               \
        if (data == NULL)                                                                                              \
        {                                                                                                              \
            fprintf(stderr, "unable to allocate data buffer\n");                                                       \
            return EXIT_FAILURE;                                                                                       \
        }                                                                                                              \
        for (i = 0; i <= NUM_TEST_EVTS; i++)                                                                           \
        {                                                                                                              \
            data[i] = i;                                                                                               \
        }                                                                                                              \
                                                                                                                       \
        for (i = 0; i <= NUM_TEST_EVTS; i++)                                                                           \
        {                                                                                                              \
            rc = event_get(_econtext->event_channels, NULL, &(evts[i]));                                               \
            if (rc)                                                                                                    \
            {                                                                                                          \
                fprintf(stderr, "event_get() failed\n");                                                               \
                return EXIT_FAILURE;                                                                                   \
            }                                                                                                          \
                                                                                                                       \
            rc = event_channel_emit_with_payload(&(evts[i]),                                                           \
                                                 _econtext->client->id,                                                \
                                                 AM_TEST_MSG_ID,                                                       \
                                                 GET_DEST_EP(_econtext),                                               \
                                                 NULL,                                                                 \
                                                 &(data[i]),                                                           \
                                                 sizeof(uint64_t));                                                    \
            if (rc != EVENT_DONE && rc != EVENT_INPROGRESS)                                                            \
            {                                                                                                          \
                fprintf(stderr, "event_channel_emit_with_payload() failed\n");                                         \
                return EXIT_FAILURE;                                                                                   \
            }                                                                                                          \
            if (rc == EVENT_DONE)                                                                                      \
                fprintf(stderr, "event_channel_emit_with_payload() completed immediately\n");                          \
            else                                                                                                       \
                fprintf(stderr, "event_channel_emit_with_payload() is in progress\n");                                 \
            fprintf(stderr, "Ev #%ld = %p\n", i, evts[i]);                                                             \
        }                                                                                                              \
                                                                                                                       \
        /* All the events have been emitted, now waiting for them to complete */                                       \
        for (i = 0; i <= NUM_TEST_EVTS; i++)                                                                           \
        {                                                                                                              \
            while (evts[i] != NULL && !evts[i]->ctx.complete)                                                          \
            {                                                                                                          \
                fprintf(stderr, "Waiting for event #%ld (%p) to complete\n", i, evts[i]);                              \
                _econtext->progress(_econtext);                                                                        \
            }                                                                                                          \
            fprintf(stderr, "Evt #%ld completed\n", i);                                                                \
        }                                                                                                              \
                                                                                                                       \
        /* All events completed, we can safely return them */                                                          \
        for (i = 0; i <= NUM_TEST_EVTS; i++)                                                                           \
        {                                                                                                              \
            if (evts[i] == NULL)                                                                                       \
                continue;                                                                                              \
            rc = event_return(&(evts[i]));                                                                             \
            if (rc)                                                                                                    \
            {                                                                                                          \
                fprintf(stderr, "event_return() failed\n");                                                            \
                return EXIT_FAILURE;                                                                                   \
            }                                                                                                          \
        }                                                                                                              \
                                                                                                                       \
        free(evts);                                                                                                    \
        evts = NULL;                                                                                                   \
        free(data);                                                                                                    \
        data = NULL;                                                                                                   \
    } while (0)

#define WAIT_FOR_ALL_EVENTS(_econtext)      \
    do                                      \
    {                                       \
        while (!second_notification_recvd)  \
        {                                   \
            _econtext->progress(_econtext); \
        }                                   \
    } while (0)

static bool first_notification_recvd = false;
static bool second_notification_recvd = false;
static int basic_notification_cb(struct dpu_offload_ev_sys *ev_sys, execution_context_t *econtext, am_header_t *hdr, size_t hdr_len, void *data, size_t data_len)
{
    assert(data);
    int *msg = (int *)data;
    fprintf(stderr, "Notification successfully received. Msg = %d\n", *msg);
    if (*msg == NUM_TEST_EVTS)
    {
        if (!first_notification_recvd)
            first_notification_recvd = true;
        else if (!second_notification_recvd)
            second_notification_recvd = true;
    }
    return 0;
}

static bool ping_pong_done = false;
static uint64_t expected_value = 0;
static bool expected_value_set = false;
static int pingpong_notification_cb(struct dpu_offload_ev_sys *ev_sys, execution_context_t *econtext, am_header_t *hdr, size_t hdr_len, void *data, size_t data_len)
{
    assert(data);
    assert(econtext);
    uint64_t *msg = (uint64_t *)data;
    uint64_t val = *msg;
    fprintf(stderr, "Ping-pong notification successfully received. Msg = %" PRIu64 "\n", val);

    if (expected_value_set == false)
    {
        if (econtext->type == CONTEXT_CLIENT)
            expected_value = 1;
        expected_value_set = true;
    }

    if (val != expected_value)
    {
        fprintf(stderr, "Received %" PRIu64 " instead of %" PRIu64 "\n", val, expected_value);
        _exit(1); // We are in a handler, hard crash when facing an error
    }

    if (econtext->type == CONTEXT_SERVER)
        val++;

    if (val == NUM_TEST_EVTS)
    {
        if (!ping_pong_done)
            ping_pong_done = true;
    }

    if (val < NUM_TEST_EVTS || (econtext->type == CONTEXT_SERVER && val == NUM_TEST_EVTS))
    {
        // Send it back where it is coming from
        dpu_offload_event_t *cur_evt;
        dpu_offload_event_info_t evt_info;
        evt_info.payload_size = sizeof(uint64_t);
        fprintf(stderr, "Getting event to send notification back\n");
        dpu_offload_status_t _rc = event_get(econtext->event_channels, &evt_info, &cur_evt);
        if (_rc)
        {
            fprintf(stderr, "event_get() failed\n");
            _exit(1); // We are in a handler, hard crash when facing an error
        }
        uint64_t *new_val = (uint64_t *)cur_evt->payload;
        *new_val = val;

        fprintf(stderr, "Sending msg back with value %" PRIu64 "\n", val);
        _rc = event_channel_emit(&cur_evt,
                                 ECONTEXT_ID(econtext),
                                 PINGPONG_NOTIF_ID,
                                 GET_DEST_EP(econtext),
                                 econtext);
        if (_rc != EVENT_DONE && _rc != EVENT_INPROGRESS)
        {
            fprintf(stderr, "event_channel_emit() failed\n");
            _exit(1); // We are in a handler, hard crash when facing an error
        }
        if (_rc == EVENT_INPROGRESS)
            ucs_list_add_tail(&(econtext->ongoing_events), &(cur_evt->item));
        if (_rc == EVENT_DONE)
            fprintf(stderr, "event_channel_emit() completed right away ev=%p\n", cur_evt);

        expected_value++;
    }

    return 0;
}

// The client initiate the ping-pong; only the server increases the value. The test stop with the threshold value is reached
static bool pingpong_test_initiated = false;
#define INITIATE_PING_PONG_TEST(_econtext)                                                        \
    do                                                                                            \
    {                                                                                             \
        fprintf(stderr, "Starting INITIATE_PING_PONG_TEST...\n");                                 \
        if (_econtext->type == CONTEXT_CLIENT && !pingpong_test_initiated)                        \
        {                                                                                         \
            dpu_offload_event_t *cur_evt;                                                         \
            dpu_offload_event_info_t evt_info;                                                    \
            evt_info.payload_size = sizeof(uint64_t);                                             \
            fprintf(stderr, "Getting event...\n");                                                \
            dpu_offload_status_t _rc = event_get(_econtext->event_channels, &evt_info, &cur_evt); \
            if (_rc)                                                                              \
            {                                                                                     \
                fprintf(stderr, "event_get() failed\n");                                          \
                return EXIT_FAILURE;                                                              \
            }                                                                                     \
            fprintf(stderr, "Ev = %p, setting payload...\n", cur_evt);                            \
            uint64_t *val_to_send = (uint64_t *)cur_evt->payload;                                 \
            *val_to_send = 0;                                                                     \
            fprintf(stderr, "sending initial ping...\n");                                         \
            _rc = event_channel_emit(&cur_evt,                                                    \
                                     ECONTEXT_ID(_econtext),                                      \
                                     PINGPONG_NOTIF_ID,                                           \
                                     GET_DEST_EP(_econtext),                                      \
                                     _econtext);                                                  \
            if (_rc != EVENT_DONE && _rc != EVENT_INPROGRESS)                                     \
            {                                                                                     \
                fprintf(stderr, "event_channel_emit() failed\n");                                 \
                return EXIT_FAILURE;                                                              \
            }                                                                                     \
            if (_rc == EVENT_INPROGRESS)                                                          \
            {                                                                                     \
                fprintf(stderr, "Adding event to ongoing events list...\n");                      \
                ucs_list_add_tail(&(_econtext->ongoing_events), &(cur_evt->item));                \
            }                                                                                     \
            pingpong_test_initiated = true;                                                       \
        }                                                                                         \
        /* waiting for everything to go through */                                                \
        while (!ping_pong_done)                                                                   \
        {                                                                                         \
            _econtext->progress(_econtext);                                                       \
        }                                                                                         \
    } while (0)

#endif // _COMMON_TEST_PARAMS_H_