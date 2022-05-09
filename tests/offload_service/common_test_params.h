//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#ifndef _COMMON_TEST_PARAMS_H_
#define _COMMON_TEST_PARAMS_H_

#include <unistd.h>

#define NUM_FLOOD_TEST_EVTS (1000)
#define NUM_TEST_EVTS (1000)
#define PINGPONG_NOTIF_ID (5000)
#define PAYLOAD_EXPLICIT_MGT_NOTIF_ID (5001)
#define USE_ONGOING_LIST_NOTIF_ID (5002)

static bool ping_pong_done = false;
static uint64_t expected_value = 0;
static bool expected_value_set = false;
static size_t current_data_size;
static uint64_t use_ongoing_list_notif_expected_value = NUM_FLOOD_TEST_EVTS + 10;
static uint64_t payload_explicit_mgt_notif_expected_value = 0;

#define INIT_DATA(_d, _value)                                           \
    do                                                                  \
    {                                                                   \
        size_t _i;                                                      \
        for (_i = 0; _i < (current_data_size / sizeof(uint64_t)); _i++) \
        {                                                               \
            (_d)[_i] = _value;                                          \
        }                                                               \
    } while (0)

#define CHECK_DATA(_d, _expected_value) ({                                                          \
    bool _data_okay = true;                                                                         \
    size_t _i;                                                                                      \
    for (_i = 0; _i < (current_data_size / sizeof(uint64_t)); _i++)                                 \
    {                                                                                               \
        if (_expected_value != (_d)[_i])                                                            \
        {                                                                                           \
            fprintf(stderr, "Element %ld is %" PRIu64 " instead of %" PRIu64 " (data size: %ld)\n", \
                    _i, (_d)[_i], _expected_value, current_data_size);                              \
            _data_okay = false;                                                                     \
            break;                                                                                  \
        }                                                                                           \
    }                                                                                               \
    _data_okay;                                                                                     \
})

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

#define REGISTER_NOTIF_CALLBACKS(_econtext)                                                                    \
    do                                                                                                         \
    {                                                                                                          \
        fprintf(stderr, "Registering callback for notifications of type %d\n", USE_ONGOING_LIST_NOTIF_ID);     \
        dpu_offload_status_t _rc = event_channel_register(_econtext->event_channels,                           \
                                                          USE_ONGOING_LIST_NOTIF_ID,                           \
                                                          use_ongoing_list_notification_cb);                   \
        if (_rc)                                                                                               \
        {                                                                                                      \
            fprintf(stderr, "event_channel_register() failed\n");                                              \
            return EXIT_FAILURE;                                                                               \
        }                                                                                                      \
                                                                                                               \
        fprintf(stderr, "Registering callback for notifications of type %d\n", PAYLOAD_EXPLICIT_MGT_NOTIF_ID); \
        _rc = event_channel_register(_econtext->event_channels,                                                \
                                     PAYLOAD_EXPLICIT_MGT_NOTIF_ID,                                            \
                                     payload_explicit_mgt_notification_cb);                                    \
        if (_rc)                                                                                               \
        {                                                                                                      \
            fprintf(stderr, "event_channel_register() failed\n");                                              \
            return EXIT_FAILURE;                                                                               \
        }                                                                                                      \
                                                                                                               \
        /* Register a custom callback type, i.e., an ID that is not predefined and it is used for the */       \
        /* ping-pong notification test. */                                                                     \
        fprintf(stderr, "Registering callback for notifications of custom type %d\n", PINGPONG_NOTIF_ID);      \
        _rc = event_channel_register(_econtext->event_channels, PINGPONG_NOTIF_ID, pingpong_notification_cb);  \
        if (_rc)                                                                                               \
        {                                                                                                      \
            fprintf(stderr, "event_channel_register() failed\n");                                              \
            return EXIT_FAILURE;                                                                               \
        }                                                                                                      \
    } while (0)

#define EMIT_MANY_EVTS_AND_USE_ONGOING_LIST(_econtext, _dest_id)                                                      \
    do                                                                                                                \
    {                                                                                                                 \
        dpu_offload_status_t _rc;                                                                                     \
        dpu_offload_event_t **evts = (dpu_offload_event_t **)calloc(NUM_FLOOD_TEST_EVTS + 1,                          \
                                                                    sizeof(dpu_offload_event_t *));                   \
        if (evts == NULL)                                                                                             \
        {                                                                                                             \
            fprintf(stderr, "unable to allocate events\n");                                                           \
            return EXIT_FAILURE;                                                                                      \
        }                                                                                                             \
                                                                                                                      \
        uint64_t *data = malloc((NUM_FLOOD_TEST_EVTS + 1) * current_data_size);                                       \
        if (data == NULL)                                                                                             \
        {                                                                                                             \
            fprintf(stderr, "unable to allocate data buffer\n");                                                      \
            return EXIT_FAILURE;                                                                                      \
        }                                                                                                             \
        size_t i;                                                                                                     \
        for (i = 0; i <= NUM_FLOOD_TEST_EVTS; i++)                                                                    \
        {                                                                                                             \
            uint64_t *ptr = (uint64_t *)((ptrdiff_t)data + (i * current_data_size));                                  \
            INIT_DATA(ptr, i + NUM_FLOOD_TEST_EVTS + 10);                                                             \
        }                                                                                                             \
                                                                                                                      \
        for (i = 0; i <= NUM_FLOOD_TEST_EVTS; i++)                                                                    \
        {                                                                                                             \
            _rc = event_get(_econtext->event_channels, NULL, &(evts[i]));                                             \
            if (_rc)                                                                                                  \
            {                                                                                                         \
                fprintf(stderr, "event_get() failed\n");                                                              \
                return EXIT_FAILURE;                                                                                  \
            }                                                                                                         \
                                                                                                                      \
            char *ptr = (char *)((ptrdiff_t)data + (i * current_data_size));                                          \
            _rc = event_channel_emit_with_payload(&(evts[i]),                                                         \
                                                  USE_ONGOING_LIST_NOTIF_ID,                                          \
                                                  GET_DEST_EP(_econtext),                                             \
                                                  _dest_id,                                                           \
                                                  NULL,                                                               \
                                                  ptr,                                                                \
                                                  current_data_size);                                                 \
            if (_rc != EVENT_DONE && _rc != EVENT_INPROGRESS)                                                         \
            {                                                                                                         \
                fprintf(stderr, "event_channel_emit_with_payload() failed\n");                                        \
                return EXIT_FAILURE;                                                                                  \
            }                                                                                                         \
            if (_rc == EVENT_INPROGRESS)                                                                              \
            {                                                                                                         \
                fprintf(stderr, "[INFO] add event %ld to the ongoing list\n", evts[i]->seq_num);                      \
                ucs_list_add_tail(&(_econtext->ongoing_events), &(evts[i]->item));                                    \
            }                                                                                                         \
            uint64_t *vals = (uint64_t *)ptr;                                                                         \
            fprintf(stderr, "Ev #%ld = %p, msg: %" PRIu64 " (EMIT_MANY_EVTS_AND_USE_ONGOING_LIST, data size: %ld)\n", \
                    i, evts[i], vals[0], current_data_size);                                                          \
            /* Progress once to let notifications progress */                                                         \
            _econtext->progress(_econtext);                                                                           \
        }                                                                                                             \
                                                                                                                      \
        while (!ucs_list_is_empty(&(_econtext->ongoing_events)) != 0)                                                 \
        {                                                                                                             \
            _econtext->progress(_econtext);                                                                           \
        }                                                                                                             \
                                                                                                                      \
        free(data);                                                                                                   \
        data = NULL;                                                                                                  \
        free(evts);                                                                                                   \
        evts = NULL;                                                                                                  \
        fprintf(stderr, "EMIT_MANY_EVTS_AND_USE_ONGOING_LIST done\n");                                                \
    } while (0)

#define EMIT_MANY_EVS_WITH_EXPLICIT_MGT(_econtext, _dest_id)                                        \
    do                                                                                              \
    {                                                                                               \
        fprintf(stderr, "Sending %d notifications...\n", NUM_FLOOD_TEST_EVTS);                      \
        size_t i;                                                                                   \
        dpu_offload_event_t **evts = (dpu_offload_event_t **)calloc(NUM_FLOOD_TEST_EVTS + 1,        \
                                                                    sizeof(dpu_offload_event_t *)); \
        if (evts == NULL)                                                                           \
        {                                                                                           \
            fprintf(stderr, "unable to allocate events\n");                                         \
            return EXIT_FAILURE;                                                                    \
        }                                                                                           \
        uint64_t *data = malloc((NUM_FLOOD_TEST_EVTS + 1) * current_data_size);                     \
        if (data == NULL)                                                                           \
        {                                                                                           \
            fprintf(stderr, "unable to allocate data buffer\n");                                    \
            return EXIT_FAILURE;                                                                    \
        }                                                                                           \
        for (i = 0; i <= NUM_FLOOD_TEST_EVTS; i++)                                                  \
        {                                                                                           \
            uint64_t *ptr = (uint64_t *)((ptrdiff_t)data + (i * current_data_size));                \
            INIT_DATA(ptr, i);                                                                      \
        }                                                                                           \
                                                                                                    \
        for (i = 0; i <= NUM_FLOOD_TEST_EVTS; i++)                                                  \
        {                                                                                           \
            rc = event_get(_econtext->event_channels, NULL, &(evts[i]));                            \
            if (rc)                                                                                 \
            {                                                                                       \
                fprintf(stderr, "event_get() failed\n");                                            \
                return EXIT_FAILURE;                                                                \
            }                                                                                       \
                                                                                                    \
            char *ptr = (char *)((ptrdiff_t)data + (i * current_data_size));                        \
            rc = event_channel_emit_with_payload(&(evts[i]),                                        \
                                                 PAYLOAD_EXPLICIT_MGT_NOTIF_ID,                     \
                                                 GET_DEST_EP(_econtext),                            \
                                                 _dest_id,                                          \
                                                 NULL,                                              \
                                                 ptr,                                               \
                                                 current_data_size);                                \
            if (rc != EVENT_DONE && rc != EVENT_INPROGRESS)                                         \
            {                                                                                       \
                fprintf(stderr, "event_channel_emit_with_payload() failed\n");                      \
                return EXIT_FAILURE;                                                                \
            }                                                                                       \
            if (rc == EVENT_DONE)                                                                   \
                fprintf(stderr, "event_channel_emit_with_payload() completed immediately\n");       \
            else                                                                                    \
                fprintf(stderr, "event_channel_emit_with_payload() is in progress\n");              \
            fprintf(stderr, "Ev #%ld = %p (EMIT_MANY_EVS_WITH_EXPLICIT_MGT)\n", i, evts[i]);        \
            /* Progress once to let notification progress */                                        \
            _econtext->progress(_econtext);                                                         \
        }                                                                                           \
                                                                                                    \
        /* All the events have been emitted, now waiting for them to complete */                    \
        for (i = 0; i <= NUM_FLOOD_TEST_EVTS; i++)                                                  \
        {                                                                                           \
            while (evts[i] != NULL && !event_completed(evts[i]))                                    \
            {                                                                                       \
                /*fprintf(stderr, "%s l.%d Waiting for event #%ld (%p) to complete\n", */           \
                /*        __FILE__, __LINE__, i, evts[i]);*/                                        \
                _econtext->progress(_econtext);                                                     \
            }                                                                                       \
            fprintf(stderr, "Evt #%ld completed\n", i);                                             \
        }                                                                                           \
                                                                                                    \
        /* All events completed, we can safely return them */                                       \
        for (i = 0; i <= NUM_FLOOD_TEST_EVTS; i++)                                                  \
        {                                                                                           \
            if (evts[i] == NULL)                                                                    \
                continue;                                                                           \
            rc = event_return(&(evts[i]));                                                          \
            if (rc)                                                                                 \
            {                                                                                       \
                fprintf(stderr, "event_return() failed\n");                                         \
                return EXIT_FAILURE;                                                                \
            }                                                                                       \
        }                                                                                           \
                                                                                                    \
        free(evts);                                                                                 \
        evts = NULL;                                                                                \
        free(data);                                                                                 \
        data = NULL;                                                                                \
    } while (0)

#define WAIT_FOR_ALL_EVENTS_WITH_EXPLICIT_MGT(_econtext) \
    do                                                   \
    {                                                    \
        while (!first_notification_recvd)                \
        {                                                \
            _econtext->progress(_econtext);              \
        }                                                \
    } while (0)

#define WAIT_FOR_ALL_EVENTS_WITH_ONGOING_LIST(_econtext) \
    do                                                   \
    {                                                    \
        while (!second_notification_recvd)               \
        {                                                \
            _econtext->progress(_econtext);              \
        }                                                \
    } while (0)

static bool first_notification_recvd = false;
static bool second_notification_recvd = false;
static int use_ongoing_list_notification_cb(struct dpu_offload_ev_sys *ev_sys, execution_context_t *econtext, am_header_t *hdr, size_t hdr_len, void *data, size_t data_len)
{
    assert(data);
    uint64_t *msg = (uint64_t *)data;
    fprintf(stderr, "%s: Notification successfully received. Msg = %" PRIu64 ", Expected = %" PRIu64 "\n",
            __func__, msg[0], use_ongoing_list_notif_expected_value);
    bool data_okay = CHECK_DATA(msg, use_ongoing_list_notif_expected_value);
    if (!data_okay)
        _exit(1); // We are in a handler, hard failure in case of error
    use_ongoing_list_notif_expected_value++;
    if (msg[0] == NUM_FLOOD_TEST_EVTS + 10 + NUM_FLOOD_TEST_EVTS)
        second_notification_recvd = true;

    return 0;
}

static int payload_explicit_mgt_notification_cb(struct dpu_offload_ev_sys *ev_sys, execution_context_t *econtext, am_header_t *hdr, size_t hdr_len, void *data, size_t data_len)
{
    assert(data);
    uint64_t *msg = (uint64_t *)data;
    fprintf(stderr, "%s: Notification successfully received. Msg = %" PRIu64 ", Expected = %" PRIu64 "\n",
            __func__, msg[0], payload_explicit_mgt_notif_expected_value);
    bool data_okay = CHECK_DATA(msg, payload_explicit_mgt_notif_expected_value);
    if (!data_okay)
        _exit(1); // We are in a handler, hard failure in case of error
    payload_explicit_mgt_notif_expected_value++;
    if (msg[0] == NUM_FLOOD_TEST_EVTS)
    {
        first_notification_recvd = true;
    }
    return 0;
}

static int pingpong_notification_cb(struct dpu_offload_ev_sys *ev_sys, execution_context_t *econtext, am_header_t *hdr, size_t hdr_len, void *data, size_t data_len)
{
    assert(data);
    assert(econtext);
    uint64_t *msg = (uint64_t *)data;
    uint64_t val = msg[0];
    fprintf(stderr, "Ping-pong notification successfully received. Expected value = %" PRIu64 ", received value = %" PRIu64 "\n",
            expected_value, val);

    if (data_len != current_data_size)
    {
        fprintf(stderr, "[ERROR] data len is %ld instead of the expected %ld\n",
                data_len, current_data_size);
        _exit(1); // We are in a handler, hard crash when facing an error
    }

    if (expected_value_set == false)
    {
        if (econtext->type == CONTEXT_CLIENT)
            expected_value = 1;
        expected_value_set = true;
    }

    bool data_okay = CHECK_DATA(msg, expected_value);
    if (!data_okay)
    {
        fprintf(stderr, "Received data is invalid\n");
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
        evt_info.payload_size = current_data_size;
        fprintf(stderr, "Getting event to send notification back\n");
        dpu_offload_status_t _rc = event_get(econtext->event_channels, &evt_info, &cur_evt);
        if (_rc)
        {
            fprintf(stderr, "event_get() failed\n");
            _exit(1); // We are in a handler, hard crash when facing an error
        }
        uint64_t *new_data = (uint64_t *)cur_evt->payload;
        INIT_DATA(new_data, val);

        fprintf(stderr, "Sending msg back with value %" PRIu64 "\n", val);
        _rc = event_channel_emit(&cur_evt,
                                 PINGPONG_NOTIF_ID,
                                 GET_DEST_EP(econtext),
                                 hdr->id,
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
#define INITIATE_PING_PONG_TEST(_econtext, _dest_id)                                              \
    do                                                                                            \
    {                                                                                             \
        fprintf(stderr, "Starting INITIATE_PING_PONG_TEST (data size: %ld)...\n",                 \
                current_data_size);                                                               \
        if (_econtext->type == CONTEXT_CLIENT && !pingpong_test_initiated)                        \
        {                                                                                         \
            dpu_offload_event_t *cur_evt;                                                         \
            dpu_offload_event_info_t evt_info;                                                    \
            evt_info.payload_size = current_data_size;                                            \
            fprintf(stderr, "Getting event...\n");                                                \
            dpu_offload_status_t _rc = event_get(_econtext->event_channels, &evt_info, &cur_evt); \
            if (_rc)                                                                              \
            {                                                                                     \
                fprintf(stderr, "event_get() failed\n");                                          \
                return EXIT_FAILURE;                                                              \
            }                                                                                     \
            fprintf(stderr, "Ev = %p, setting payload...\n", cur_evt);                            \
            uint64_t *data = (uint64_t *)cur_evt->payload;                                        \
            INIT_DATA(data, expected_value);                                                      \
            fprintf(stderr, "sending initial ping...\n");                                         \
            _rc = event_channel_emit(&cur_evt,                                                    \
                                     PINGPONG_NOTIF_ID,                                           \
                                     GET_DEST_EP(_econtext),                                      \
                                     (_dest_id),                                                  \
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
            expected_value++;                                                                     \
        }                                                                                         \
        /* waiting for everything to go through */                                                \
        while (!ping_pong_done)                                                                   \
        {                                                                                         \
            _econtext->progress(_econtext);                                                       \
        }                                                                                         \
    } while (0)

#endif // _COMMON_TEST_PARAMS_H_