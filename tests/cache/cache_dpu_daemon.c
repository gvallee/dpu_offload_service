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

// from dpu_offload_event_channels.c
extern bool is_in_cache(cache_t *cache, int64_t gp_id, int64_t rank_id);

#define TEST_COMPLETED_NOTIF_ID (5000)

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

static int send_term_message(execution_context_t *econtext)
{
    dpu_offload_event_t *evt;
    dpu_offload_status_t rc = event_get(econtext->event_channels, NULL, &evt);
    if (rc)
    {
        fprintf(stderr, "event_get() failed\n");
        return -1;
    }

    rc = event_channel_emit_with_payload(evt, ECONTEXT_ID(econtext), TEST_COMPLETED_NOTIF_ID, GET_DEST_EP(econtext), econtext, NULL, 0);
    if (rc != EVENT_DONE && rc != EVENT_INPROGRESS)
    {
        fprintf(stderr, "event_channel_emit_with_payload() failed\n");
        return -1;
    }
    if (rc != EVENT_DONE)
    {
        ucs_list_add_tail(&(econtext->ongoing_events), &(evt->item));
        while (!ucs_list_is_empty(&(econtext->ongoing_events)))
        {
            econtext->progress(econtext);
        }
    }

    return 0;
}

static bool test_done = false;
static int test_complete_notification_cb(struct dpu_offload_ev_sys *ev_sys, execution_context_t *econtext, am_header_t *hdr, size_t hdr_len, void *data, size_t data_len)
{
    test_done = true;
}

int main(int argc, char **argv)
{
    fprintf(stderr, "Creating offload engine...\n");
    offloading_engine_t *offload_engine;
    dpu_offload_status_t rc = offload_engine_init(&offload_engine, NULL);
    if (rc || offload_engine == NULL)
    {
        fprintf(stderr, "offload_engine_init() failed\n");
        return EXIT_FAILURE;
    }

    /*
     * REGISTER A NOTIFICATION CALLBACK AT THE ENGINE LEVEL SO THAT ALL EXECUTION CONTEXTS
     * FOR INTER-DPU COMMUNICATIONS WILL AUTOMATICALLY HAVE IT.
     */
    fprintf(stderr, "Registering callback for notifications of test completion %d\n", TEST_COMPLETED_NOTIF_ID);
    rc = engine_register_default_notification_handler(offload_engine, TEST_COMPLETED_NOTIF_ID, test_complete_notification_cb);
    if (rc)
    {
        fprintf(stderr, "engine_register_default_notification_handler() failed\n");
        return EXIT_FAILURE;
    }

    /*
     * GET THE CONFIGURATION.
     */
    fprintf(stderr, "Getting configuration...\n");
    offloading_config_t config_data;
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
        peer_cache_entry_t *new_entry;
        DYN_LIST_GET(offload_engine->free_peer_cache_entries, peer_cache_entry_t, item, new_entry);
        if (new_entry == NULL)
        {
            fprintf(stderr, "Unable to get cache entry\n");
            goto error_out;
        }

        // 42 is used to avoid lucky initialization effects that would hide a bug
        new_entry->peer.proc_info.group_rank = 42;
        new_entry->peer.proc_info.group_id = 42;
        new_entry->set = true;
        // The shadow DPU is myself.
        new_entry->num_shadow_dpus = 1;
        new_entry->shadow_dpus[0] = config_data.local_dpu.id;
        new_entry->peer.addr_len = 43;
        SET_PEER_CACHE_ENTRY(&(offload_engine->procs_cache), new_entry);
        if (!is_in_cache(&(offload_engine->procs_cache), 42, 42))
        {
            fprintf(stderr, "Cache entry not reported as being in the cache\n");
            goto error_out;
        }

        fprintf(stderr, "Cache entry successfully created, waiting for the notification from DPU #0 that test completed\n");
        while (!test_done)
        {
            offload_engine_progress(offload_engine);
        }

        // Last test with local cache: look up the cache entry that is already in the cache
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
            fprintf(stderr, "invalid endpoint was returned (%p instead of %p)\n", target_dpu_ep, list_dpus[config_data.local_dpu.id]->conn_status);
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
        } while (dpu1_config == NULL || dpu1_config->econtext == NULL);
        fprintf(stderr, "Now connected to DPU #1 (econtext=%p)\n", dpu1_config->econtext);

        dpu_offload_event_t *ev;
        execution_context_t *econtext = ECONTEXT_FOR_DPU_COMMUNICATION(offload_engine, 1);
        if (econtext == NULL)
        {
            fprintf(stderr, "unable to find a valid execution context\n");
            goto error_out;
        }

        fprintf(stderr, "Looking up endpoint\n");
        rc = get_dpu_id_by_group_rank(offload_engine, 42, 42, 0, &remote_dpu_id, &ev);
        if (rc != DO_SUCCESS)
        {
            fprintf(stderr, "first get_dpu_id_by_host_rank() failed\n");
            goto error_out;
        }
        fprintf(stderr, "get_dpu_id_by_group_rank() succeeded, ev=%p\n", ev);

        if (ev != NULL)
        {
            fprintf(stderr, "Waiting for look up to complete (ev=%p)\n", ev);
            while (!event_completed(econtext->event_channels, ev))
                econtext->progress(econtext);

            fprintf(stderr, "Look up completed\n");
            rc = event_return(econtext->event_channels, &ev);
            if (rc != DO_SUCCESS)
            {
                fprintf(stderr, "event_return() failed\n");
                goto error_out;
            }

            rc = get_dpu_id_by_group_rank(offload_engine, 42, 42, 0, &remote_dpu_id, &ev);
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

        if (remote_dpu_id != 1)
        {
            fprintf(stderr, "returned DPU is %" PRIu64 " instead of 1\n", remote_dpu_id);
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
        fprintf(stderr, "All done, notify DPU #1...\n");
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