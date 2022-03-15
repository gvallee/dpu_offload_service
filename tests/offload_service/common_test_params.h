//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#ifndef _COMMON_TEST_PARAMS_H_
#define _COMMON_TEST_PARAMS_H_

#define NUM_TEST_EVTS (42)
#define PINGPONG_NOTIF_ID (5000)

#define GET_DEST_EP(_econtext) ({               \
    ucp_ep_h _dest_ep;                          \
    if (_econtext->type == CONTEXT_SERVER)      \
    {                                           \
        _dest_ep = GET_CLIENT_EP(_econtext, 0); \
    }                                           \
    else                                        \
    {                                           \
        _dest_ep = GET_SERVER_EP(_econtext);    \
    }                                           \
    _dest_ep;                                   \
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

#define EMIT_MANY_EVTS_AND_USE_ONGOING_LIST(_econtext)                                                                                                \
    do                                                                                                                                                \
    {                                                                                                                                                 \
        dpu_offload_status_t _rc;                                                                                                                     \
        dpu_offload_event_t **evts = (dpu_offload_event_t **)calloc(NUM_TEST_EVTS + 1, sizeof(dpu_offload_event_t *));                                \
        if (evts == NULL)                                                                                                                             \
        {                                                                                                                                             \
            fprintf(stderr, "unable to allocate events\n");                                                                                           \
            return EXIT_FAILURE;                                                                                                                      \
        }                                                                                                                                             \
                                                                                                                                                      \
        int i;                                                                                                                                        \
        for (i = 0; i <= NUM_TEST_EVTS; i++)                                                                                                          \
        {                                                                                                                                             \
            dpu_offload_event_t *cur_evt;                                                                                                             \
            _rc = event_get(_econtext->event_channels, &cur_evt);                                                                                     \
            if (_rc)                                                                                                                                  \
            {                                                                                                                                         \
                fprintf(stderr, "event_get() failed\n");                                                                                              \
                return EXIT_FAILURE;                                                                                                                  \
            }                                                                                                                                         \
            evts[i] = cur_evt;                                                                                                                        \
                                                                                                                                                      \
            int notif_data = i;                                                                                                                       \
            _rc = event_channel_emit(cur_evt, ECONTEXT_ID(_econtext), AM_TEST_MSG_ID, GET_DEST_EP(_econtext), NULL, &notif_data, sizeof(notif_data)); \
            if (_rc != EVENT_DONE && _rc != EVENT_INPROGRESS)                                                                                         \
            {                                                                                                                                         \
                fprintf(stderr, "event_channel_emit() failed\n");                                                                                     \
                return EXIT_FAILURE;                                                                                                                  \
            }                                                                                                                                         \
            ucs_list_add_tail(&(_econtext->ongoing_events), &(cur_evt->item));                                                                        \
            fprintf(stderr, "Ev #%d = %p\n", i, cur_evt);                                                                                             \
        }                                                                                                                                             \
                                                                                                                                                      \
        while (!ucs_list_is_empty(&(_econtext->ongoing_events)) != 0)                                                                                 \
            _econtext->progress(_econtext);                                                                                                           \
    } while (0)

#define EMIT_MANY_EVS_WITH_EXPLICIT_MGT(_econtext)                                                                                                  \
    do                                                                                                                                              \
    {                                                                                                                                               \
        int i;                                                                                                                                      \
        dpu_offload_event_t **evts = (dpu_offload_event_t **)calloc(NUM_TEST_EVTS + 1, sizeof(dpu_offload_event_t *));                              \
        if (evts == NULL)                                                                                                                           \
        {                                                                                                                                           \
            fprintf(stderr, "unable to allocate events\n");                                                                                         \
            return EXIT_FAILURE;                                                                                                                    \
        }                                                                                                                                           \
                                                                                                                                                    \
        for (i = 0; i <= NUM_TEST_EVTS; i++)                                                                                                        \
        {                                                                                                                                           \
            dpu_offload_event_t *cur_evt;                                                                                                           \
            rc = event_get(_econtext->event_channels, &cur_evt);                                                                                    \
            if (rc)                                                                                                                                 \
            {                                                                                                                                       \
                fprintf(stderr, "event_get() failed\n");                                                                                            \
                return EXIT_FAILURE;                                                                                                                \
            }                                                                                                                                       \
            evts[i] = cur_evt;                                                                                                                      \
                                                                                                                                                    \
            int notif_data = i;                                                                                                                     \
            rc = event_channel_emit(cur_evt, _econtext->client->id, AM_TEST_MSG_ID, GET_DEST_EP(_econtext), NULL, &notif_data, sizeof(notif_data)); \
            if (rc != EVENT_DONE && rc != EVENT_INPROGRESS)                                                                                         \
            {                                                                                                                                       \
                fprintf(stderr, "event_channel_emit() failed\n");                                                                                   \
                return EXIT_FAILURE;                                                                                                                \
            }                                                                                                                                       \
            fprintf(stderr, "Ev #%d = %p\n", i, cur_evt);                                                                                           \
        }                                                                                                                                           \
                                                                                                                                                    \
        /* All the events have been emitted, now waiting for them to complete */                                                                    \
        for (i = 0; i <= NUM_TEST_EVTS; i++)                                                                                                        \
        {                                                                                                                                           \
            dpu_offload_event_t *cur_evt = evts[i];                                                                                                 \
            fprintf(stderr, "Waiting for event #%d (%p) to complete\n", i, cur_evt);                                                                \
            while (!cur_evt->ctx.complete)                                                                                                          \
                _econtext->progress(_econtext);                                                                                                     \
        }                                                                                                                                           \
                                                                                                                                                    \
        /* All events completed, we can safely return them */                                                                                       \
        for (i = 0; i <= NUM_TEST_EVTS; i++)                                                                                                        \
        {                                                                                                                                           \
            dpu_offload_event_t *cur_evt = evts[i];                                                                                                 \
            rc = event_return(_econtext->event_channels, &cur_evt);                                                                                 \
            if (rc)                                                                                                                                 \
            {                                                                                                                                       \
                fprintf(stderr, "event_return() failed\n");                                                                                         \
                return EXIT_FAILURE;                                                                                                                \
            }                                                                                                                                       \
        }                                                                                                                                           \
                                                                                                                                                    \
        free(evts);                                                                                                                                 \
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
static int pingpong_notification_cb(struct dpu_offload_ev_sys *ev_sys, execution_context_t *econtext, am_header_t *hdr, size_t hdr_len, void *data, size_t data_len)
{
    assert(data);
    assert(econtext);
    int *msg = (int *)data;
    int val = *msg;
    fprintf(stderr, "Ping-pong notification successfully received. Msg = %d\n", val);

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
        fprintf(stderr, "Getting event to send notification back\n");
        dpu_offload_status_t _rc = event_get(econtext->event_channels, &cur_evt);
        if (_rc)
        {
            fprintf(stderr, "event_get() failed\n");
            return EXIT_FAILURE;
        }

        fprintf(stderr, "Sending msg back with value %d\n", val);
        _rc = event_channel_emit(cur_evt, ECONTEXT_ID(econtext), PINGPONG_NOTIF_ID, GET_DEST_EP(econtext), econtext, &val, sizeof(val));
        if (_rc != EVENT_DONE && _rc != EVENT_INPROGRESS)
        {
            fprintf(stderr, "event_channel_emit() failed\n");
            return EXIT_FAILURE;
        }
        ucs_list_add_tail(&(econtext->ongoing_events), &(cur_evt->item));
    }

    return 0;
}

// The client initiate the ping-pong; only the server increases the value. The test stop with the threshold value is reached
static bool pingpong_test_initiated = false;
#define INITIATE_PING_PONG_TEST(_econtext)                                                                                                                    \
    do                                                                                                                                                        \
    {                                                                                                                                                         \
        if (_econtext->type == CONTEXT_CLIENT && !pingpong_test_initiated)                                                                                    \
        {                                                                                                                                                     \
            dpu_offload_event_t *cur_evt;                                                                                                                     \
            dpu_offload_status_t _rc = event_get(_econtext->event_channels, &cur_evt);                                                                        \
            if (_rc)                                                                                                                                          \
            {                                                                                                                                                 \
                fprintf(stderr, "event_get() failed\n");                                                                                                      \
                return EXIT_FAILURE;                                                                                                                          \
            }                                                                                                                                                 \
            int notif_data = 0;                                                                                                                               \
            _rc = event_channel_emit(cur_evt, ECONTEXT_ID(_econtext), PINGPONG_NOTIF_ID, GET_DEST_EP(_econtext), _econtext, &notif_data, sizeof(notif_data)); \
            if (_rc != EVENT_DONE && _rc != EVENT_INPROGRESS)                                                                                                 \
            {                                                                                                                                                 \
                fprintf(stderr, "event_channel_emit() failed\n");                                                                                             \
                return EXIT_FAILURE;                                                                                                                          \
            }                                                                                                                                                 \
            ucs_list_add_tail(&(_econtext->ongoing_events), &(cur_evt->item));                                                                                \
            pingpong_test_initiated = true;                                                                                                                   \
        }                                                                                                                                                     \
        /* waiting for everything to go through */                                                                                                            \
        while (!ping_pong_done)                                                                                                                               \
            _econtext->progress(_econtext);                                                                                                                   \
    } while (0)

#endif // _COMMON_TEST_PARAMS_H_