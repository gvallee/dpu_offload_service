#define _POSIX_C_SOURCE 200809L

//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

/*
 * This test is meant to act as a daemon on two DPUs.
 *
 * The intent of the test is the following:
 * - start the daemon on two DPU(s) (see documentation)
 * - the second DPU will create a fake cache entry for a local rank
 * - the first DPU will request it
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>

#include "dpu_offload_service_daemon.h"
#include "dpu_offload_envvars.h"

offloading_config_t config_data; // TODO: find a way to avoid having a global variable when we want to access the config from a handler

// from dpu_offload_event_channels.c
extern bool is_in_cache(cache_t *cache, int64_t gp_id, int64_t rank_id);

#define TEST_COMPLETED_NOTIF_ID (5000)
#define START_TEST_FROM_CALLBACK (5001)
#define END_TEST_FROM_CALLBACK (5002)

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

static int send_term_message(execution_context_t *econtext)
{
    dpu_offload_event_t *evt;
    dpu_offload_status_t rc = event_get(econtext->event_channels, NULL, &evt);
    if (rc)
    {
        fprintf(stderr, "event_get() failed\n");
        return -1;
    }

    rc = event_channel_emit_with_payload(&evt, ECONTEXT_ID(econtext), TEST_COMPLETED_NOTIF_ID, GET_DEST_EP(econtext), econtext, NULL, 0);
    if (rc != EVENT_DONE && rc != EVENT_INPROGRESS)
    {
        fprintf(stderr, "event_channel_emit_with_payload() failed\n");
        return -1;
    }
    if (rc == EVENT_INPROGRESS)
    {
        ucs_list_add_tail(&(econtext->ongoing_events), &(evt->item));
        while (!ucs_list_is_empty(&(econtext->ongoing_events)))
        {
            econtext->progress(econtext);
        }
    }

    return 0;
}

static int do_lookup(offloading_engine_t *offload_engine, int64_t gp_id, int64_t rank_id, int64_t expected_dpu_id)
{
    dpu_offload_status_t rc;
    int64_t remote_dpu_id;
    dpu_offload_event_t *ev;
    execution_context_t *econtext = ECONTEXT_FOR_DPU_COMMUNICATION(offload_engine, 1);
    if (econtext == NULL)
    {
        fprintf(stderr, "unable to find a valid execution context\n");
        goto error_out;
    }

    fprintf(stderr, "Looking up endpoint\n");
    rc = get_dpu_id_by_group_rank(offload_engine, gp_id, rank_id, 0, &remote_dpu_id, &ev);
    if (rc != DO_SUCCESS)
    {
        fprintf(stderr, "first get_dpu_id_by_host_rank() failed\n");
        goto error_out;
    }
    fprintf(stderr, "l.%d - get_dpu_id_by_group_rank() succeeded, ev=%p\n", __LINE__, ev);

    if (ev != NULL)
    {
        fprintf(stderr, "l.%d - Waiting for look up to complete (ev=%p)\n", __LINE__, ev);
        while (!event_completed(ev))
            lib_progress(econtext);

        fprintf(stderr, "Look up completed, returning event\n");
        rc = event_return(&ev);
        if (rc != DO_SUCCESS)
        {
            fprintf(stderr, "event_return() failed\n");
            goto error_out;
        }

        rc = get_dpu_id_by_group_rank(offload_engine, gp_id, rank_id, 0, &remote_dpu_id, &ev);
        if (rc != DO_SUCCESS)
        {
            fprintf(stderr, "second get_dpu_id_by_host_rank() failed\n");
            goto error_out;
        }
        if (ev != NULL)
        {
            fprintf(stderr, "cache entry still not available\n");
            goto error_out;
        }
    }

    if (remote_dpu_id != expected_dpu_id)
    {
        fprintf(stderr, "returned DPU is %" PRIu64 " instead of %" PRId64 "\n", remote_dpu_id, expected_dpu_id);
        send_term_message(econtext);
        goto error_out;
    }
    fprintf(stderr, "Successfully got the remote DPU ID, getting the corresponding endpoint...\n");

    ucp_ep_h target_dpu_ep = get_dpu_ep_by_id(offload_engine, remote_dpu_id);
    if (target_dpu_ep == NULL)
    {
        fprintf(stderr, "shadow DPU endpoint is undefined\n");
        send_term_message(econtext);
        goto error_out;
    }
    fprintf(stderr, "Successfully retrieved endpoint (%p)\n", target_dpu_ep);
    return 0;
error_out:
    return -1;
}

static bool test_done = false;
static int test_complete_notification_cb(struct dpu_offload_ev_sys *ev_sys, execution_context_t *econtext, am_header_t *hdr, size_t hdr_len, void *data, size_t data_len)
{
    test_done = true;
}

static int test_cb(struct dpu_offload_ev_sys *ev_sys, execution_context_t *econtext, am_header_t *hdr, size_t hdr_len, void *data, size_t data_len)
{
    // We received the init callback from the server, we look up the EP we are supposed
    // to find and we notify the server once the look up completes. All that from
    // handler.
    offloading_engine_t *engine = (offloading_engine_t *)econtext->engine;
    remote_dpu_info_t **list_dpus = LIST_DPUS_FROM_ENGINE(engine);
    int ret = do_lookup(engine, 42, 52, 1);
    assert(ret == 0);
    dpu_offload_event_t *end_test_cb_ev;
    dpu_offload_status_t rc = event_get(ev_sys, NULL, &end_test_cb_ev);
    if (rc)
    {
        fprintf(stderr, "l.%d: event_get() failed\n", __LINE__);
        goto error_out;
    }
    rc = event_channel_emit(&end_test_cb_ev, config_data.local_dpu.id, END_TEST_FROM_CALLBACK, list_dpus[1]->ep, NULL);
    if (rc)
    {
        fprintf(stderr, "l.%d: event_channel_emit() failed\n", __LINE__);
        goto error_out;
    }
    return 0;
error_out:
    return -1;
}

#define ADD_TO_CACHE(_rank, _gp, _engine, _config_data)                                         \
    do                                                                                          \
    {                                                                                           \
        peer_cache_entry_t *_new_entry;                                                         \
        DYN_LIST_GET((_engine)->free_peer_cache_entries, peer_cache_entry_t, item, _new_entry); \
        if (_new_entry == NULL)                                                                 \
        {                                                                                       \
            fprintf(stderr, "Unable to get cache entry\n");                                     \
            goto error_out;                                                                     \
        }                                                                                       \
        _new_entry->peer.proc_info.group_rank = _rank;                                          \
        _new_entry->peer.proc_info.group_id = _gp;                                              \
        _new_entry->set = true;                                                                 \
        /* The shadow DPU is myself. */                                                         \
        _new_entry->num_shadow_dpus = 1;                                                        \
        _new_entry->shadow_dpus[0] = (_config_data).local_dpu.id;                               \
        _new_entry->peer.addr_len = 43;                                                         \
        SET_PEER_CACHE_ENTRY(&((_engine)->procs_cache), _new_entry);                            \
        if (!is_in_cache(&((_engine)->procs_cache), _gp, _rank))                                \
        {                                                                                       \
            fprintf(stderr, "Cache entry not reported as being in the cache\n");                \
            goto error_out;                                                                     \
        }                                                                                       \
    } while (0)

int main(int argc, char **argv)
{
    fprintf(stderr, "Creating offload engine...\n");
    offloading_engine_t *offload_engine;
    dpu_offload_status_t rc = offload_engine_init(&offload_engine);
    if (rc || offload_engine == NULL)
    {
        fprintf(stderr, "offload_engine_init() failed\n");
        return EXIT_FAILURE;
    }

    /*
     * REGISTER THE REQUIRED NOTIFICATION CALLBACKS AT THE ENGINE LEVEL SO THAT ALL EXECUTION CONTEXTS
     * FOR INTER-DPU COMMUNICATIONS WILL AUTOMATICALLY HAVE IT.
     */
    fprintf(stderr, "Registering callback for notifications of test completion %d\n", TEST_COMPLETED_NOTIF_ID);
    rc = engine_register_default_notification_handler(offload_engine, TEST_COMPLETED_NOTIF_ID, test_complete_notification_cb);
    if (rc)
    {
        fprintf(stderr, "engine_register_default_notification_handler() failed\n");
        return EXIT_FAILURE;
    }

    fprintf(stderr, "Registering callback %d to start test in the context of a callback\n", TEST_COMPLETED_NOTIF_ID);
    rc = engine_register_default_notification_handler(offload_engine, START_TEST_FROM_CALLBACK, test_cb);
    if (rc)
    {
        fprintf(stderr, "engine_register_default_notification_handler() failed\n");
        return EXIT_FAILURE;
    }

    /*
     * GET THE CONFIGURATION.
     */
    fprintf(stderr, "Getting configuration...\n");
    INIT_DPU_CONFIG_DATA(&config_data);
    config_data.offloading_engine = offload_engine;
    int ret = get_dpu_config(offload_engine, &config_data);
    if (ret)
    {
        fprintf(stderr, "get_config() failed\n");
        return EXIT_FAILURE;
    }
    fprintf(stderr, "Configuration loaded, I am DPU #%ld\n", config_data.local_dpu.id);

    /*
     * INITIATE CONNECTION BETWEEN DPUS.
     */
    fprintf(stderr, "Initiating connections between DPUs\n");
    rc = inter_dpus_connect_mgr(offload_engine, &config_data);
    if (rc)
    {
        fprintf(stderr, "inter_dpus_connect_mgr() failed\n");
        return EXIT_FAILURE;
    }
    fprintf(stderr, "Connections between DPUs successfully initialized\n");

    fprintf(stderr, "I am DPU #%ld, starting the test\n", config_data.local_dpu.id);

    remote_dpu_info_t **list_dpus = LIST_DPUS_FROM_ENGINE(offload_engine);
    uint64_t remote_dpu_id;
    if (config_data.local_dpu.id == 1)
    {
        /* DPU #1 */

        // Create a fake entry in the cache
        // 42 and 52 is used to avoid lucky initialization effects that would hide a bug
        ADD_TO_CACHE(42, 42, offload_engine, config_data);
        ADD_TO_CACHE(52, 42, offload_engine, config_data);

        fprintf(stderr, "Cache entry successfully created, waiting for the notification from DPU #0 that test completed\n");
        while (!test_done)
        {
            offload_engine_progress(offload_engine);
        }

        // Next test with local cache: look up the cache entry that is already in the cache
        dpu_offload_event_t *ev;
        rc = get_dpu_id_by_group_rank(offload_engine, 42, 42, 0, &remote_dpu_id, &ev);
        if (rc != DO_SUCCESS)
        {
            fprintf(stderr, "first get_dpu_id_by_host_rank() failed\n");
            goto error_out;
        }

        if (ev != NULL)
        {
            fprintf(stderr, "get_dpu_id_by_group_rank() did not complete right away but was expected to\n");
            goto error_out;
        }

        ucp_ep_h target_dpu_ep = get_dpu_ep_by_id(offload_engine, remote_dpu_id);
        if (target_dpu_ep == NULL)
        {
            fprintf(stderr, "get_dpu_ep_by_id() failed\n");
            goto error_out;
        }

        if (target_dpu_ep != list_dpus[config_data.local_dpu.id]->ep)
        {
            fprintf(stderr, "invalid endpoint was returned (%p instead of %p)\n", target_dpu_ep, list_dpus[config_data.local_dpu.id]->ep);
            goto error_out;
        }

        // Start the test that will trigger lookups and communications from callback
        dpu_offload_event_t *start_test_cb_ev;
        rc = event_get(offload_engine->default_econtext->event_channels, NULL, &start_test_cb_ev);
        if (rc)
        {
            fprintf(stderr, "l.%d: event_get() failed\n", __LINE__);
            goto error_out;
        }
        rc = event_channel_emit(&start_test_cb_ev, config_data.local_dpu.id, START_TEST_FROM_CALLBACK, list_dpus[0]->ep, NULL);
        if (rc)
        {
            fprintf(stderr, "l.%d: event_channel_emit() failed\n", __LINE__);
            goto error_out;
        }
    }
    else
    {
        /* DPU #0 */

        // We need to make sure we have the connection to DPU #1
        remote_dpu_info_t *dpu1_config;
        fprintf(stderr, "Waiting to be connected to DPU #1\n");
        do
        {
            ENGINE_LOCK(offload_engine);
            dpu1_config = list_dpus[1];
            ENGINE_UNLOCK(offload_engine);
            offload_engine_progress(offload_engine);
        } while (dpu1_config == NULL || dpu1_config->econtext == NULL);
        fprintf(stderr, "Now connected to DPU #1 (econtext=%p)\n", dpu1_config->econtext);

        int ret = do_lookup(offload_engine, 42, 42, 1);
        if (ret != 0)
        {
            fprintf(stderr, "lookup failed\n");
            goto error_out;
        }

        fprintf(stderr, "All done, notify DPU #1...\n");
        execution_context_t *econtext = ECONTEXT_FOR_DPU_COMMUNICATION(offload_engine, 1);
        send_term_message(econtext);
    }

    fprintf(stderr, "Finalizing...\n");
    offload_engine_fini(&offload_engine);
    fprintf(stderr, "client all done, exiting successfully\n");

    return EXIT_SUCCESS;
error_out:
    offload_engine_fini(&offload_engine);
    fprintf(stderr, "%s: test failed\n", argv[0]);
    return EXIT_FAILURE;
}