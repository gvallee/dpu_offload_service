//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "dpu_offload_service_daemon.h"
#include "dpu_offload_event_channels.h"
#include "ping_pong.h"

static int ping_cb(struct dpu_offload_ev_sys *ev_sys, execution_context_t *econtext,
                   am_header_t *hdr, size_t hdr_len, void *data, size_t data_len)
{
    int ping_id = (*((int*) data));
    fprintf(stdout, "Ping %d received by dpu!\n", ping_id);
    
    // Emit pong response event to client
    dpu_offload_status_t rc;
    dpu_offload_event_t *event;
    rc = event_get(econtext->event_channels, NULL, &event);
    assert(rc == DO_SUCCESS);
    
    rc = event_channel_emit_with_payload(&event,
                                         PONG_NOTIF_ID,
                                         GET_CLIENT_EP(econtext, 0),
                                         1,
                                         NULL,
                                         (void *) &ping_id,
                                         sizeof(ping_id));
        
    // Check for emit failure
    if (rc != EVENT_DONE && rc != EVENT_INPROGRESS)
    {
        fprintf(stderr, "[ERROR] event_channel_emit_with_payload() failed\n");
        exit(-1);
    }
        
    // If emit did not complete, wait for it to finish
    if (rc == EVENT_INPROGRESS)
    {
        while (!event_completed(event))
        {
            econtext->progress(econtext);
        }
    }
     
    return DO_SUCCESS;
}

int main (int argc, char **argv)
{
    offloading_engine_t *offload_engine = NULL;
    execution_context_t *server = NULL;
    offloading_config_t config_data;
    dpu_offload_status_t rc;

    // Initialize the offload engine
    rc = offload_engine_init(&offload_engine);
    assert(rc == DO_SUCCESS);
    assert(offload_engine);

    // Initialize dpu configuration
    INIT_DPU_CONFIG_DATA(&config_data);
    config_data.offloading_engine = offload_engine;
    rc = get_dpu_config(offload_engine, &config_data);
    assert(rc == DO_SUCCESS);

    // Initiate inter-dpu connections
    rc = inter_dpus_connect_mgr(offload_engine, &config_data);
    assert(rc == DO_SUCCESS);

    /* Wait for the DPUs to connect to each other */
    while (!all_service_procs_connected(offload_engine))
    {
        offload_engine_progress(offload_engine);
    }
    
    // Register ping notification callback
    rc = engine_register_default_notification_handler(offload_engine,
                                                      PING_NOTIF_ID,
                                                      ping_cb,
                                                      NULL);
    assert(rc == DO_SUCCESS);

    // Initiate the server context and add to engine
    // We let the system figure out the configuration to use to let ranks connect
    server = server_init(offload_engine, &(config_data.local_service_proc.host_init_params));
    assert(server);
    ADD_SERVER_TO_ENGINE(server, offload_engine);

    // Progress until all processes on the host send a termination message
    while (!EXECUTION_CONTEXT_DONE(server))
    {
        lib_progress(server);
    }
    
    // Finalize the offload engine
    offload_engine_fini(&offload_engine);

    return EXIT_SUCCESS;
}

