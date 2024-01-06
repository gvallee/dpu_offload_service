#define _POSIX_C_SOURCE 200809L

//
// Copyright (c) 2022-2024, NVIDIA CORPORATION. All rights reserved.
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
#include "../../src/dpu_offload_debug.h"

uint64_t group_size = 2;

offloading_config_t config_data; // TODO: find a way to avoid having a global variable when we want to access the config from a handler

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

static int send_test_successful_message(execution_context_t *econtext, uint64_t dest_id)
{
    dpu_offload_event_t *evt;
    dpu_offload_status_t rc = event_get(econtext->event_channels, NULL, &evt);
    if (rc)
    {
        fprintf(stderr, "[ERROR] event_get() failed\n");
        return -1;
    }

    rc = event_channel_emit_with_payload(&evt, TEST_COMPLETED_NOTIF_ID, GET_DEST_EP(econtext), dest_id, econtext, NULL, 0);
    if (rc != EVENT_DONE && rc != EVENT_INPROGRESS)
    {
        fprintf(stderr, "[ERROR] event_channel_emit_with_payload() failed\n");
        return -1;
    }
    if (rc == EVENT_INPROGRESS)
    {
        while (!ucs_list_is_empty(&(econtext->ongoing_events)))
        {
            econtext->progress(econtext);
        }
    }

    return 0;
}

static bool endpoint_success = false;
/* Called on service process #0 */
void cache_entry_cb(void *data)
{
    assert(data);
    cache_entry_request_t *cache_entry_req = (cache_entry_request_t *)data;
    fprintf(stderr, "Cache entry for group 0x%x and rank %" PRId64 " is now available\n",
            cache_entry_req->gp_uid,
            cache_entry_req->rank);
    assert(cache_entry_req->offload_engine);
    offloading_engine_t *engine = (offloading_engine_t *)cache_entry_req->offload_engine;
    ucp_ep_h target_sp_ep;
    execution_context_t *econtext_comm;
    uint64_t notif_dest_id;
    dpu_offload_status_t rc = get_sp_ep_by_id(engine, cache_entry_req->target_sp_idx, &target_sp_ep, &econtext_comm, &notif_dest_id);
    if (rc)
    {
        fprintf(stderr, "l.%d - [ERROR] get_sp_ep_by_id() failed\n", __LINE__);
        return;
    }
    if (target_sp_ep == NULL)
    {
        fprintf(stderr, "l.%d - [ERROR] shadow DPU endpoint is undefined\n", __LINE__);
        return;
    }
    if (econtext_comm == NULL)
    {
        fprintf(stderr, "l.%d - [ERROR] econtext is undefined\n", __LINE__);
        return;
    }
    fprintf(stderr, "l.%d - Successfully retrieved endpoint (%p)\n", __LINE__, target_sp_ep);
    fprintf(stderr, "-> lookup succeeded (l.%d)\n", __LINE__);
    dpu_offload_event_t *end_test_cb_ev;
    remote_service_proc_info_t *sp0 = DYN_ARRAY_GET_ELT(GET_ENGINE_LIST_SERVICE_PROCS(engine), 0, remote_service_proc_info_t);
    assert(sp0);
    remote_service_proc_info_t *sp1 = DYN_ARRAY_GET_ELT(GET_ENGINE_LIST_SERVICE_PROCS(engine), 1, remote_service_proc_info_t);
    assert(sp1);
    execution_context_t *target_sp_econtext = sp0->econtext;
    assert(target_sp_econtext);
    rc = event_get(target_sp_econtext->event_channels, NULL, &end_test_cb_ev);
    if (rc)
    {
        fprintf(stderr, "l.%d: [ERROR] event_get() failed\n", __LINE__);
        return;
    }
    rc = event_channel_emit(&end_test_cb_ev, END_TEST_FROM_CALLBACK, sp1->ep, 1, NULL);
    if (rc != EVENT_DONE && rc != EVENT_INPROGRESS)
    {
        fprintf(stderr, "l.%d: [ERROR] event_channel_emit() failed\n", __LINE__);
        return;
    }
    DYN_LIST_RETURN(engine->free_cache_entry_requests, cache_entry_req, item);
    endpoint_success = true;
}

/**
 * @brief Called on service process #0.
 *
 * @param offload_engine
 * @param gp_id
 * @param rank_id
 * @param expected_dpu_id
 * @return int
 */
static int do_lookup_from_callback(offloading_engine_t *offload_engine, group_uid_t gp_uid, int64_t rank_id, int64_t expected_dpu_id)
{
    dpu_offload_status_t rc = get_cache_entry_by_group_rank(offload_engine, gp_uid, rank_id, 0, cache_entry_cb);
    if (rc != DO_SUCCESS)
    {
        fprintf(stderr, "get_cache_entry_by_group_rank() failed");
        return -1;
    }
    return 0;
}

static int do_lookup(offloading_engine_t *offload_engine, group_uid_t gp_uid, int64_t rank_id, int64_t expected_dpu_id)
{
    dpu_offload_status_t rc;
    int64_t remote_sp_id;
    dpu_offload_event_t *ev;
    execution_context_t *econtext = ECONTEXT_FOR_SERVICE_PROC_COMMUNICATION(offload_engine, 1);
    if (econtext == NULL)
    {
        fprintf(stderr, "[ERROR] unable to find a valid execution context\n");
        goto error_out;
    }

    fprintf(stderr, "Looking up endpoint for group 0x%x  and rank %" PRId64 "\n", gp_uid, rank_id);
    rc = get_sp_id_by_group_rank(offload_engine, gp_uid, rank_id, 0, &remote_sp_id, &ev);
    if (rc != DO_SUCCESS)
    {
        fprintf(stderr, "[ERROR] first get_dpu_id_by_host_rank() failed\n");
        goto error_out;
    }
    fprintf(stderr, "l.%d - get_sp_id_by_group_rank() succeeded, ev=%p\n", __LINE__, ev);

    if (ev != NULL)
    {
        fprintf(stderr, "l.%d - Waiting for look up to complete (ev=%p)\n", __LINE__, ev);
        while (!event_completed(ev))
            lib_progress(econtext);

        fprintf(stderr, "Look up completed, returning event\n");
        rc = event_return(&ev);
        if (rc != DO_SUCCESS)
        {
            fprintf(stderr, "[ERROR] event_return() failed\n");
            goto error_out;
        }

        rc = get_sp_id_by_group_rank(offload_engine, gp_uid, rank_id, 0, &remote_sp_id, &ev);
        if (rc != DO_SUCCESS)
        {
            fprintf(stderr, "[ERROR] second get_dpu_id_by_host_rank() failed\n");
            goto error_out;
        }
        if (ev != NULL)
        {
            fprintf(stderr, "[ERROR] cache entry still not available\n");
            goto error_out;
        }
    }

    if (remote_sp_id != expected_dpu_id)
    {
        fprintf(stderr, "[ERROR] returned DPU is %" PRIu64 " instead of %" PRId64 "\n", remote_sp_id, expected_dpu_id);
        goto error_out;
    }
    fprintf(stderr, "Successfully got the remote DPU ID, getting the corresponding endpoint...\n");

    ucp_ep_h target_dpu_ep;
    execution_context_t *econtext_comm;
    uint64_t notif_dest_id;
    rc = get_sp_ep_by_id(offload_engine, remote_sp_id, &target_dpu_ep, &econtext_comm, &notif_dest_id);
    if (rc)
    {
        fprintf(stderr, "l.%d - [ERROR] get_sp_ep_by_id\n", __LINE__);
        goto error_out;
    }
    if (target_dpu_ep == NULL)
    {
        fprintf(stderr, "l.%d - [ERROR] shadow DPU endpoint is undefined\n", __LINE__);
        goto error_out;
    }
    if (econtext_comm == NULL)
    {
        fprintf(stderr, "l.%d - [ERROR] undefined execution context\n", __LINE__);
        goto error_out;
    }
    fprintf(stderr, "l.%d - Successfully retrieved endpoint (%p)\n", __LINE__, target_dpu_ep);
    return 0;
error_out:
    return -1;
}

static bool test_done = false;
static int test_complete_notification_cb(struct dpu_offload_ev_sys *ev_sys, execution_context_t *econtext, am_header_t *hdr, size_t hdr_len, void *data, size_t data_len)
{
    test_done = true;
    return 0;
}

static bool cb_test_done = false;
static int end_test_cb(struct dpu_offload_ev_sys *ev_sys, execution_context_t *econtext, am_header_t *hdr, size_t hdr_len, void *data, size_t data_len)
{
    cb_test_done = true;
    return 0;
}

/**
 * @brief Callback invoked upon reception of the notification to start the test from a callback. Invoked on DPU #0.
 *
 * @param ev_sys
 * @param econtext
 * @param hdr
 * @param hdr_len
 * @param data
 * @param data_len
 * @return int
 */
static int test_cb(struct dpu_offload_ev_sys *ev_sys, execution_context_t *econtext, am_header_t *hdr, size_t hdr_len, void *data, size_t data_len)
{
    // We received the init callback from the server, we look up the EP we are supposed
    // to find and we notify the server once the look up completes. All that from
    // handler.
    group_uid_t group;
    offloading_engine_t *engine = (offloading_engine_t *)econtext->engine;
    group = 42;
    fprintf(stderr, "-> Starting test from a callback...\n");
    int ret = do_lookup_from_callback(engine, group, 52, 1);
    assert(ret == 0);
    cb_test_done = true;
    return 0;
}

#define ADD_TO_CACHE(_rank, _gp_uid, _gp_sz, _engine, _config_data)                                   \
    do                                                                                                \
    {                                                                                                 \
        peer_cache_entry_t *_entry = NULL;                                                            \
        _entry = GET_GROUP_RANK_CACHE_ENTRY(&((_engine)->procs_cache), _gp_uid, (_rank), (_gp_sz));   \
        assert(_entry);                                                                               \
        _entry->peer.proc_info.group_rank = (_rank);                                                  \
        _entry->peer.proc_info.group_uid = (_gp_uid);                                                 \
        _entry->set = true;                                                                           \
        /* The shadow DPU is myself. */                                                               \
        _entry->num_shadow_service_procs = 1;                                                         \
        _entry->shadow_service_procs[0] = (_config_data).local_service_proc.info.global_id;           \
        _entry->peer.addr_len = 43;                                                                   \
        if (!is_in_cache(&((_engine)->procs_cache), _gp_uid, _rank, group_size))                      \
        {                                                                                             \
            fprintf(stderr, "[ERROR] Cache entry not reported as being in the cache\n");              \
            goto error_out;                                                                           \
        }                                                                                             \
    } while (0)

int main(int argc, char **argv)
{
    remote_service_proc_info_t *sp;
    fprintf(stderr, "Creating offload engine...\n");
    offloading_engine_t *offload_engine;
    dpu_offload_status_t rc = offload_engine_init(&offload_engine);
    if (rc || offload_engine == NULL)
    {
        fprintf(stderr, "[ERROR] offload_engine_init() failed\n");
        return EXIT_FAILURE;
    }

    /*
     * REGISTER THE REQUIRED NOTIFICATION CALLBACKS AT THE ENGINE LEVEL SO THAT ALL EXECUTION CONTEXTS
     * FOR INTER-DPU COMMUNICATIONS WILL AUTOMATICALLY HAVE IT.
     */
    fprintf(stderr, "Registering callback for notifications of test completion %d\n", TEST_COMPLETED_NOTIF_ID);
    rc = engine_register_default_notification_handler(offload_engine, TEST_COMPLETED_NOTIF_ID, test_complete_notification_cb, NULL);
    if (rc)
    {
        fprintf(stderr, "[ERROR] engine_register_default_notification_handler() failed\n");
        return EXIT_FAILURE;
    }

    fprintf(stderr, "Registering callback %d to start test in the context of a callback\n", START_TEST_FROM_CALLBACK);
    rc = engine_register_default_notification_handler(offload_engine, START_TEST_FROM_CALLBACK, test_cb, NULL);
    if (rc)
    {
        fprintf(stderr, "[ERROR] engine_register_default_notification_handler() failed\n");
        return EXIT_FAILURE;
    }

    fprintf(stderr, "Registering callback %d to notify the end of the test performed in the context of a callback\n", END_TEST_FROM_CALLBACK);
    rc = engine_register_default_notification_handler(offload_engine, END_TEST_FROM_CALLBACK, end_test_cb, NULL);
    if (rc)
    {
        fprintf(stderr, "[ERROR] engine_register_default_notification_handler() failed\n");
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
        fprintf(stderr, "[ERROR] get_config() failed\n");
        return EXIT_FAILURE;
    }
    fprintf(stderr, "Configuration loaded, I am service process #%ld\n", config_data.local_service_proc.info.global_id);

    /*
     * INITIATE CONNECTION BETWEEN DPUS.
     */
    fprintf(stderr, "Initiating connections between service processes\n");
    rc = inter_dpus_connect_mgr(offload_engine, &config_data);
    if (rc)
    {
        fprintf(stderr, "[ERROR] inter_dpus_connect_mgr() failed\n");
        return EXIT_FAILURE;
    }
    fprintf(stderr, "Connections between DPUs successfully initialized\n");

    fprintf(stderr, "I am service process #%ld, starting the test\n", config_data.local_service_proc.info.global_id);

    int64_t remote_sp_id;
    if (config_data.local_service_proc.info.global_id == 1)
    {
        /* Service Process #1 */
        group_uid_t group;
        remote_service_proc_info_t *sp0;
        sp = DYN_ARRAY_GET_ELT(GET_ENGINE_LIST_SERVICE_PROCS(offload_engine),
                               config_data.local_service_proc.info.global_id,
                               remote_service_proc_info_t);
        assert(sp);
        sp0 = DYN_ARRAY_GET_ELT(GET_ENGINE_LIST_SERVICE_PROCS(offload_engine),
                                0,
                                remote_service_proc_info_t);
        assert(sp0);

        group = 42;

        // Create a fake entry in the cache
        // 42 is used to avoid lucky initialization effects that would hide a bug
        ADD_TO_CACHE(42, group, group_size, offload_engine, config_data);

        fprintf(stderr, "Cache entry successfully created, waiting for the notification from DPU #0 that test completed\n");
        while (!test_done)
        {
            offload_engine_progress(offload_engine);
        }
        fprintf(stderr, "Got notification that the lookup from DPU #0 succeeded, moving on...\n");

        // Next test with local cache: look up the cache entry that is already in the cache
        dpu_offload_event_t *ev;
        rc = get_sp_id_by_group_rank(offload_engine, group, 42, 0, &remote_sp_id, &ev);
        if (rc != DO_SUCCESS)
        {
            fprintf(stderr, "[ERROR] first get_dpu_id_by_host_rank() failed\n");
            goto error_out;
        }

        if (ev != NULL)
        {
            fprintf(stderr, "[ERROR] get_sp_id_by_group_rank() did not complete right away but was expected to\n");
            goto error_out;
        }

        ucp_ep_h target_sp_ep;
        execution_context_t *econtext_comm;
        uint64_t notif_dest_id;
        rc = get_sp_ep_by_id(offload_engine, remote_sp_id, &target_sp_ep, &econtext_comm, &notif_dest_id);
        if (rc)
        {
            fprintf(stderr, "[ERROR] get_sp_ep_by_id() failed\n");
        }
        if (target_sp_ep == NULL)
        {
            fprintf(stderr, "[ERROR] undefined endpoint\n");
            goto error_out;
        }
        if (econtext_comm == NULL)
        {
            fprintf(stderr, "[ERROR] undefined execution context\n");
            goto error_out;
        }
        if (target_sp_ep != sp->ep)
        {
            fprintf(stderr, "[ERROR] invalid endpoint was returned (%p instead of %p)\n", target_sp_ep, sp->ep);
            goto error_out;
        }

        fprintf(stderr, "-> l.%d - Successfully got the entry in local cache\n", __LINE__);

        // Start the test that will trigger lookups and communications from callback
        fprintf(stderr, "-> l.%d - Sending notification to initiate the test in a callback...\n", __LINE__);
        ADD_TO_CACHE(52, group, group_size, offload_engine, config_data);
        dpu_offload_event_t *start_test_cb_ev;
        execution_context_t *sp0_econtext = sp0->econtext;
        assert(sp0_econtext);
        rc = event_get(sp0_econtext->event_channels, NULL, &start_test_cb_ev);
        if (rc)
        {
            fprintf(stderr, "l.%d: [ERROR] event_get() failed\n", __LINE__);
            goto error_out;
        }
        // Direct access to the endpoint, which will not trigger the creation of the endpoint
        // even if all the required data to do so is there, so I can check on things and not
        // only get fail/success details.
        if (sp0->ep == NULL)
        {
            if (sp0->addr == NULL)
            {
                fprintf(stderr, "[WARN] the peer's address is NULL\n");
            }
        }
        ucp_ep_h remote_sp_ep = GET_REMOTE_SERVICE_PROC_EP(offload_engine, 0ul);
        // After calling DPU_GET_REMOTE_DPU_EP, the endpoint should always be there since
        // in the worst case, we had all the data required to generate the endpoint.
        assert(sp0->ep);
        assert(remote_sp_ep);
        rc = event_channel_emit(&start_test_cb_ev, START_TEST_FROM_CALLBACK, sp0->ep, 0, NULL);
        if (rc != EVENT_DONE && rc != EVENT_INPROGRESS)
        {
            fprintf(stderr, "l.%d: [ERROR] event_channel_emit() failed (rc: %d - %s)\n", __LINE__, rc, ucs_status_string(rc));
            goto error_out;
        }

        while (!cb_test_done)
        {
            offload_engine_progress(offload_engine);
        }
    }
    else
    {
        /* Service Process #0 */
        group_uid_t group;
        group = 42;

        // We need to make sure we have the connection to service process #1
        remote_service_proc_info_t *sp1_config;
        sp1_config = DYN_ARRAY_GET_ELT(GET_ENGINE_LIST_SERVICE_PROCS(offload_engine), 1, remote_service_proc_info_t);
        fprintf(stderr, "Waiting to be connected to service process #1\n");
        do
        {
            ENGINE_LOCK(offload_engine);
            sp1_config = DYN_ARRAY_GET_ELT(GET_ENGINE_LIST_SERVICE_PROCS(offload_engine), 1, remote_service_proc_info_t);
            ENGINE_UNLOCK(offload_engine);
            offload_engine_progress(offload_engine);
        } while (sp1_config == NULL || sp1_config->econtext == NULL);
        fprintf(stderr, "Now connected to DPU #1 (econtext=%p)\n", sp1_config->econtext);

        int ret = do_lookup(offload_engine, group, 42, 1);
        if (ret != 0)
        {
            fprintf(stderr, "l.%d - [ERROR] lookup failed\n", __LINE__);
            goto error_out;
        }
        fprintf(stderr, "-> lookup succeeded (l.%d)\n", __LINE__);

        fprintf(stderr, "All done with first test, notify DPU #1...\n");
        execution_context_t *econtext = ECONTEXT_FOR_SERVICE_PROC_COMMUNICATION(offload_engine, 1);
        send_test_successful_message(econtext, 1);

        while (!endpoint_success)
            offload_engine_progress(offload_engine);

        fprintf(stderr, "All done with second test, notify DPU #1...\n");
        send_test_successful_message(econtext, 1);
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
