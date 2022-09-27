//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

#include "dpu_offload_service_daemon.h"
#include "dpu_offload_event_channels.h"
#include "telemetry.h"

bool client_done = false;

static int loadavg_notif_cb(struct dpu_offload_ev_sys *ev_sys, execution_context_t *econtext, am_header_t *hdr, size_t hdr_len, void *data, size_t data_len)
{
    char *cmd_output = NULL;
    assert(data);
    cmd_output = (char *)data;
    fprintf(stdout, "New command output:\n%s\n", cmd_output);
    return DO_SUCCESS;
}

void endHandler(int dummy)
{
    client_done = true;
}

int main (int argc, char **argv)
{
    offloading_engine_t *offload_engine = NULL;
    execution_context_t *client = NULL;
    dpu_offload_status_t rc;

    signal(SIGINT, endHandler);

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
    } while (!client_done);

    client_fini(&client);
    offload_engine_fini(&offload_engine);

    return EXIT_SUCCESS;
}