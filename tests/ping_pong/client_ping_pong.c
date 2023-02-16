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

int pongs_received = 0;

static int pong_cb(struct dpu_offload_ev_sys *ev_sys, execution_context_t *econtext,
                   am_header_t *hdr, size_t hdr_len, void *data, size_t data_len)
{
    int pong_id = (*((int*) data));
    fprintf(stdout, "Pong %d received by host!\n", pong_id);
    pongs_received++;
    return DO_SUCCESS;
}

int main (int argc, char **argv)
{
    offloading_engine_t *offload_engine = NULL;
    execution_context_t *client = NULL;
    dpu_offload_status_t rc;

    // Initialize the offload engine
    rc = offload_engine_init(&offload_engine);
    assert(rc == DO_SUCCESS);
    assert(offload_engine);
    
    // Register pong notification callback
    rc = engine_register_default_notification_handler(offload_engine,
                                                      PONG_NOTIF_ID,
                                                      pong_cb,
                                                      NULL);
    assert(rc == DO_SUCCESS);
    
    // Initialize the client context and add to engine
    client = client_init(offload_engine, NULL);
    assert(client);
    ADD_CLIENT_TO_ENGINE(client, offload_engine);

    // Wait until we are connected to server
    do
    {
        client->progress(client);
    } while (GET_ECONTEXT_BOOTSTRAPING_PHASE(client) != BOOTSTRAP_DONE);

    // Emit ping events to server
    int payload;
    dpu_offload_event_t *event;
    for(payload = 1; payload <= PING_ITERATIONS; payload++)
    {
        rc = event_get(client->event_channels, NULL, &event);
        assert(rc == DO_SUCCESS);
        rc = event_channel_emit_with_payload(&event,
                                             PING_NOTIF_ID,
                                             GET_SERVER_EP(client),
                                             0,
                                             NULL,
                                             (void *) &payload,
                                             sizeof(payload));
        
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
                client->progress(client);
            }
        }
    }
    
    // Wait until we receive a pong for each ping
    while(pongs_received != PING_ITERATIONS)
    {
        client->progress(client);
    }
    
    // Finalize the offload engine
    offload_engine_fini(&offload_engine);

    return EXIT_SUCCESS;
}
