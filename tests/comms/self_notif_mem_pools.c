//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

/*
 * This test is designed to be executed on a single DPU, with the list of DPUs (env var) set
 * and a configuration file with the associated environment variable set.
 * Ex:
 *  $ DPU_OFFLOAD_LIST_DPUS="heliosbf010" OFFLOAD_CONFIG_FILE_PATH=/path/to/config/file.cfg ./self_notif_mem_pools
 */

#include <stdlib.h>
#include <stdio.h>

#include "dpu_offload_service_daemon.h"
#include "dynamic_structs.h"

#define MY_TEST_NOTIF_ID (1000)
#define NUM_NOTIFS (10)

typedef struct my_struct
{
    int i;
    double x;
} my_struct_t;

typedef struct my_struct_container
{
    ucs_list_link_t super;
    my_struct_t data;
} my_struct_container_t;

void *mem_pool_buf_get(void *p, void *args)
{
    my_struct_container_t *desc;
    dyn_list_t *pool = (dyn_list_t *)p;
    DYN_LIST_GET(pool, my_struct_container_t, super, desc);
    assert(desc);
    return (void *)(&(desc->data));
}

void mem_pool_buf_return(void *p, void *buf)
{
    dyn_list_t *pool = (dyn_list_t *)p;
    // Find the beginning of the structure so we can return the element to the list
    my_struct_container_t *desc =
        (my_struct_container_t *)((ptrdiff_t)buf - sizeof(ucs_list_link_t));
    DYN_LIST_RETURN(pool, desc, super);
}

static bool self_notif_received = false;
uint64_t count = 0;
static int self_notification_cb(struct dpu_offload_ev_sys *ev_sys, execution_context_t *econtext, am_header_t *hdr, size_t hdr_len, void *data, size_t data_len)
{
    self_notif_received = true;
    my_struct_t *my_data = (my_struct_t*)data;
    fprintf(stdout, "Received %d and %ld\n", my_data->i, my_data->x);
    count++;
    return 0;
}

int main(int argc, char **argv)
{
    size_t n;
    offloading_config_t config_data;
    notification_info_t reg_info;
    dpu_offload_event_info_t ev_info;
    offloading_engine_t *engine = NULL;
    dyn_list_t *mem_pool;
    dpu_offload_status_t rc = offload_engine_init(&engine);
    if (rc || engine == NULL)
    {
        fprintf(stderr, "offload_engine_init() failed\n");
        goto error_out;
    }

    // Allocate a memory pool of 32 elements.
    // If the registration succeeds, the buffer used to store the payload is
    // retrieved from the dynamic list upon reception and returned to the free
    // dynamic list after the execution of the notification handler
    DYN_LIST_ALLOC(mem_pool, 32, my_struct_container_t, super);
    RESET_NOTIF_INFO(&reg_info);
    reg_info.mem_pool = mem_pool;
    reg_info.get_buf = mem_pool_buf_get;
    reg_info.return_buf = mem_pool_buf_return;
    reg_info.element_size = sizeof(my_struct_t);
    rc = engine_register_default_notification_handler(engine,
                                                      MY_TEST_NOTIF_ID,
                                                      self_notification_cb,
                                                      &reg_info);
    if (rc)
    {
        fprintf(stderr, "[ERROR] engine_register_default_notification_handler() failed\n");
        return EXIT_FAILURE;
    }

    INIT_DPU_CONFIG_DATA(&config_data);
    config_data.offloading_engine = engine;
    int ret = get_dpu_config(engine, &config_data);
    if (ret)
    {
        fprintf(stderr, "[ERROR] get_config() failed\n");
        return EXIT_FAILURE;
    }
    engine->config = &config_data;

    rc = inter_dpus_connect_mgr(engine, &config_data);
    if (rc)
    {
        fprintf(stderr, "inter_dpus_connect_mgr() failed\n");
        return EXIT_FAILURE;
    }
    fprintf(stderr, "Connections between DPUs successfully initialized\n");

    RESET_NOTIF_INFO(&(ev_info.pool));
    ev_info.pool.mem_pool = mem_pool;
    ev_info.pool.get_buf = mem_pool_buf_get;
    ev_info.pool.return_buf = mem_pool_buf_return;
    ev_info.pool.element_size = sizeof(my_struct_t);
    /* A bunch of notifications to self */
    for (n = 0; n < NUM_NOTIFS; n++)
    {
        dpu_offload_event_t *self_ev;
        my_struct_t *ptr;
        // By using the info objects specifying the memory pool and its associated get/return
        // function, a buffer from the pool is implicitly assigned to the event when we get it.
        // The buffer is implicitly returned to the pool when the infrastructure returns the
        // event upon completion
        rc = event_get(engine->self_econtext->event_channels, &ev_info, &self_ev);
        assert(self_ev);
        assert(rc == DO_SUCCESS);
        ptr = (my_struct_t*)self_ev->payload;
        ptr->i = 42;
        ptr->x = 1.0;

        rc = event_channel_emit(&self_ev, MY_TEST_NOTIF_ID, engine->self_ep, 0, NULL);
        if (rc != EVENT_DONE && rc != EVENT_INPROGRESS)
        {
            fprintf(stderr, "[ERROR] event_channel_emit() failed\n");
            goto error_out;
        }
        if (rc == EVENT_INPROGRESS)
        {
            fprintf(stderr, "[ERROR] a notification for self is reported as INPROGRESS\n");
            goto error_out;
        }
    }

    while (count != NUM_NOTIFS)
        offload_engine_progress(engine);

    offload_engine_fini(&engine);
    DYN_LIST_FREE(mem_pool, my_struct_container_t, super);

    fprintf(stdout, "Test succeeded\n");

    return EXIT_SUCCESS;
error_out:

    if (engine != NULL)
    {
        offload_engine_fini(&engine);
    }
    DYN_LIST_FREE(mem_pool, my_struct_container_t, super);
    fprintf(stderr, "Test failed\n");
    return EXIT_FAILURE;
}
