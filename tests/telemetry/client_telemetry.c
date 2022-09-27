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
#include "telemetry.h"

static int loadavg_notif_cb(struct dpu_offload_ev_sys *ev_sys, execution_context_t *econtext, am_header_t *hdr, size_t hdr_len, void *data, size_t data_len)
{
    cmd_output_t *cmd_output = NULL;
    size_t n;

    assert(data);
    cmd_output = (cmd_output_t *)data;

    // Display the output
    fprintf(stdout, "New command output:\n");
    for (n = 0; n < cmd_output->num; n++)
    {
        fprintf(stdout, "\t%s", cmd_output->output[n]);
    }
    fprintf(stdout, "\n");
    return DO_SUCCESS;
}

int main (int argc, char **argv)
{
    offloading_engine_t *offload_engine = NULL;
    execution_context_t *client = NULL;
    dpu_offload_status_t rc;

    rc = offload_engine_init(&offload_engine);
    assert(rc == DO_SUCCESS);
    assert(offload_engine);

    // Register notification callbacks for telemetry data
    rc = engine_register_default_notification_handler(offload_engine,
                                                      LOAD_AVG_TELEMETRY_NOTIF,
                                                      loadavg_notif_cb,
                                                      NULL);
    assert(rc == DO_SUCCESS);

    // Initiate the connection to the target DPU
    client = client_init(offload_engine, NULL);
    assert(client);

    // Wait until we are connected to server
    do
    {
        client->progress(client);
    } while (client->client->bootstrapping.phase != BOOTSTRAP_DONE);

    fprintf(stderr, "Client initialization all done\n");


    client_fini(&client);
    offload_engine_fini(&offload_engine);

    return EXIT_SUCCESS;
}